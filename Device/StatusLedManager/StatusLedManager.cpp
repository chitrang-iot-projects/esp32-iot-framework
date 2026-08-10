#include "StatusLedManager.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StatusLedManager::StatusLedManager(uint8_t gpioPin, neoPixelType pixelType)
    : _strip(NUM_LEDS, gpioPin, pixelType)
    , _enabled(true)
    , _brightness(DEFAULT_BRIGHTNESS)
    , _color(LedColor::Off)
    , _currentRgb{0, 0, 0}
    , _animation(LedAnimation::None)
    , _animationSpeed(BLINK_DEFAULT_MS)
    , _currentState(StatusLedState::Off)
    , _previousState(StatusLedState::Off)
    , _hasOverride(false)
    , _animState{}
    , _lastAnimationTick(0)
    , _hardwareDirty(true)
{
    resetAnimationState();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void StatusLedManager::begin()
{
    _strip.begin();
    _strip.clear();
    _strip.show();

    _lastAnimationTick = millis();
    _hardwareDirty     = false;
}

void StatusLedManager::loop()
{
    if (!_enabled)
    {
        // Push the off state to hardware once, then go idle.
        if (_hardwareDirty)
        {
            writePixelOff();
            _hardwareDirty = false;
        }
        return;
    }

    uint32_t nowMs = millis();
    tickAnimation(nowMs);

    if (_hardwareDirty)
    {
        applyToHardware();
    }
}

// ---------------------------------------------------------------------------
// Power control
// ---------------------------------------------------------------------------

void StatusLedManager::on()
{
    setEnabled(true);
}

void StatusLedManager::off()
{
    setEnabled(false);
}

void StatusLedManager::setEnabled(bool enabled)
{
    if (_enabled == enabled)
    {
        return;
    }

    _enabled       = enabled;
    _hardwareDirty = true;

    if (enabled)
    {
        // Reset the animation timer so the animation doesn't jump forward
        // to compensate for the time spent disabled.
        _lastAnimationTick = millis();
    }
}

bool StatusLedManager::isEnabled() const
{
    return _enabled;
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

void StatusLedManager::setBrightness(uint8_t brightness)
{
    _brightness    = brightness;
    _hardwareDirty = true;
}

uint8_t StatusLedManager::getBrightness() const
{
    return _brightness;
}

// ---------------------------------------------------------------------------
// Color
// ---------------------------------------------------------------------------

void StatusLedManager::setColor(LedColor color)
{
    _color      = color;
    _currentRgb = resolveNamedColor(color);
    _hardwareDirty = true;
}

void StatusLedManager::setColor(uint8_t r, uint8_t g, uint8_t b)
{
    _color      = LedColor::Custom;
    _currentRgb = {r, g, b};
    _hardwareDirty = true;
}

LedColor StatusLedManager::getColor() const
{
    return _color;
}

// ---------------------------------------------------------------------------
// Animation control
// ---------------------------------------------------------------------------

void StatusLedManager::setAnimation(LedAnimation animation)
{
    // Each animation type has a sensible default speed.
    // setAnimationSpeed() can override this after the fact.
    uint16_t defaultSpeedMs;

    switch (animation)
    {
        case LedAnimation::Blink:      defaultSpeedMs = BLINK_DEFAULT_MS;   break;
        case LedAnimation::SlowBlink:  defaultSpeedMs = SLOW_BLINK_MS;      break;
        case LedAnimation::FastBlink:  defaultSpeedMs = FAST_BLINK_MS;      break;
        case LedAnimation::Breathe:    defaultSpeedMs = BREATHE_DEFAULT_MS; break;
        case LedAnimation::Pulse:      defaultSpeedMs = PULSE_DEFAULT_MS;   break;
        case LedAnimation::Rainbow:    defaultSpeedMs = RAINBOW_STEP_MS;    break;
        default:                       defaultSpeedMs = 0;                   break;
    }

    enterAnimation(animation, defaultSpeedMs);
}

LedAnimation StatusLedManager::getAnimation() const
{
    return _animation;
}

void StatusLedManager::setAnimationSpeed(uint16_t ms)
{
    // Speed change takes effect on the next animation tick; no state reset needed.
    _animationSpeed = ms;
}

void StatusLedManager::stopAnimation()
{
    enterAnimation(LedAnimation::None, 0);
}

// ---- Convenience wrappers --------------------------------------------------

void StatusLedManager::breathe()
{
    setAnimation(LedAnimation::Breathe);
}

void StatusLedManager::pulse()
{
    // One-shot breathe: runs one full cycle (0 -> 255 -> 0) then stops.
    enterAnimation(LedAnimation::Pulse, PULSE_DEFAULT_MS);
    _animState.breathe.isPulse = true;
}

void StatusLedManager::blink()
{
    setAnimation(LedAnimation::Blink);
}

void StatusLedManager::blink(uint16_t intervalMs)
{
    enterAnimation(LedAnimation::Blink, intervalMs);
}

void StatusLedManager::flash(uint8_t count)
{
    enterAnimation(LedAnimation::Flash, 0);
    _animState.flash.remainingFlashes = count;
    _animState.flash.isOnPhase        = true;
}

void StatusLedManager::rainbow()
{
    setAnimation(LedAnimation::Rainbow);
}

// ---------------------------------------------------------------------------
// Device-state abstraction
// ---------------------------------------------------------------------------

void StatusLedManager::setState(StatusLedState state)
{
    if (_hasOverride)
    {
        // An override is active. Update the saved state so clearOverride()
        // will restore to this new state rather than the old one.
        _previousState = state;
        return;
    }

    _previousState = _currentState;
    _currentState  = state;
    applyStateMapping(state);
}

StatusLedState StatusLedManager::getState() const
{
    return _currentState;
}

void StatusLedManager::overrideState(StatusLedState state)
{
    if (!_hasOverride)
    {
        // Save the current state so clearOverride() can restore it.
        _previousState = _currentState;
        _hasOverride   = true;
    }

    _currentState = state;
    applyStateMapping(state);
}

void StatusLedManager::clearOverride()
{
    if (!_hasOverride)
    {
        return;
    }

    _hasOverride  = false;
    _currentState = _previousState;
    applyStateMapping(_currentState);
}

// ---------------------------------------------------------------------------
// Private: state machine helpers
// ---------------------------------------------------------------------------

void StatusLedManager::applyStateMapping(StatusLedState state)
{
    // Each state has a canonical color + animation.  The Custom state is an
    // explicit escape hatch: the application takes full manual control.
    switch (state)
    {
        case StatusLedState::Off:
            setColor(LedColor::Off);
            enterAnimation(LedAnimation::None, 0);
            break;

        case StatusLedState::Booting:
            setColor(LedColor::Blue);
            enterAnimation(LedAnimation::Breathe, BREATHE_DEFAULT_MS);
            break;

        case StatusLedState::Idle:
            setColor(LedColor::Off);
            enterAnimation(LedAnimation::None, 0);
            break;

        case StatusLedState::Success:
            setColor(LedColor::Green);
            enterAnimation(LedAnimation::Solid, 0);
            break;

        case StatusLedState::Busy:
            setColor(LedColor::Blue);
            enterAnimation(LedAnimation::Breathe, BREATHE_DEFAULT_MS);
            break;

        case StatusLedState::Warning:
            setColor(LedColor::Orange);
            enterAnimation(LedAnimation::Blink, BLINK_DEFAULT_MS);
            break;

        case StatusLedState::Error:
            setColor(LedColor::Red);
            enterAnimation(LedAnimation::FastBlink, FAST_BLINK_MS);
            break;

        case StatusLedState::Pairing:
            setColor(LedColor::Yellow);
            enterAnimation(LedAnimation::Blink, BLINK_DEFAULT_MS);
            break;

        case StatusLedState::Updating:
            setColor(LedColor::Purple);
            enterAnimation(LedAnimation::Breathe, BREATHE_DEFAULT_MS);
            break;

        case StatusLedState::FactoryReset:
            setColor(LedColor::White);
            enterAnimation(LedAnimation::FastBlink, FAST_BLINK_MS);
            break;

        case StatusLedState::Custom:
            // Application controls color and animation; nothing to override here.
            break;
    }
}

void StatusLedManager::enterAnimation(LedAnimation animation, uint16_t speedMs)
{
    _animation      = animation;
    _animationSpeed = speedMs;
    _lastAnimationTick = millis();
    resetAnimationState();
    _hardwareDirty  = true;
}

void StatusLedManager::resetAnimationState()
{
    _animState.blink   = {true};           // start on-phase
    _animState.breathe = {0, true, false}; // level=0, rising, not a pulse
    _animState.flash   = {0, true};        // no flashes remaining, start on-phase
    _animState.rainbow = {0};              // start at hue=0
}

// ---------------------------------------------------------------------------
// Private: animation engine
// ---------------------------------------------------------------------------

void StatusLedManager::tickAnimation(uint32_t nowMs)
{
    switch (_animation)
    {
        case LedAnimation::None:
            // Static off state; nothing to tick.
            break;

        case LedAnimation::Solid:
            // Static color; set dirty on first entry (handled by enterAnimation).
            break;

        case LedAnimation::Blink:
            tickBlink(nowMs, _animationSpeed / 2);
            break;

        case LedAnimation::SlowBlink:
            // SlowBlink bakes SLOW_BLINK_MS into _animationSpeed at entry.
            tickBlink(nowMs, _animationSpeed / 2);
            break;

        case LedAnimation::FastBlink:
            // FastBlink bakes FAST_BLINK_MS into _animationSpeed at entry.
            tickBlink(nowMs, _animationSpeed / 2);
            break;

        case LedAnimation::Breathe:
        case LedAnimation::Pulse:
            tickBreathe(nowMs);
            break;

        case LedAnimation::Flash:
            tickFlash(nowMs);
            break;

        case LedAnimation::Rainbow:
            tickRainbow(nowMs);
            break;
    }
}

void StatusLedManager::tickBlink(uint32_t nowMs, uint16_t halfPeriodMs)
{
    if (halfPeriodMs == 0)
    {
        halfPeriodMs = 1;
    }

    if (nowMs - _lastAnimationTick < halfPeriodMs)
    {
        return;
    }

    _lastAnimationTick    += halfPeriodMs; // += avoids drift accumulation
    _animState.blink.isOnPhase = !_animState.blink.isOnPhase;
    _hardwareDirty         = true;
}

void StatusLedManager::tickBreathe(uint32_t nowMs)
{
    // BREATHE_STEPS steps per full cycle (rise + fall).
    // stepIntervalMs = full cycle period / BREATHE_STEPS.
    uint16_t stepIntervalMs = (_animationSpeed > BREATHE_STEPS)
                              ? static_cast<uint16_t>(_animationSpeed / BREATHE_STEPS)
                              : 1;

    if (nowMs - _lastAnimationTick < stepIntervalMs)
    {
        return;
    }

    BreatheState& breathe = _animState.breathe;

    // Catch up all steps that elapsed — prevents the animation from appearing
    // frozen when loop() was delayed by a blocking call (WiFi init, TLS
    // token fetch, etc.).  Without this, setup() alone causes a multi-step
    // deficit that keeps the breathe level stuck near 0 (invisible).
    while (nowMs - _lastAnimationTick >= stepIntervalMs)
    {
        _lastAnimationTick += stepIntervalMs;

        if (breathe.rising)
        {
            if (breathe.level < 255)
            {
                breathe.level++;
            }
            else
            {
                breathe.rising = false;
            }
        }
        else
        {
            if (breathe.level > 0)
            {
                breathe.level--;
            }
            else
            {
                // Reached darkness on the falling side.
                if (breathe.isPulse)
                {
                    // Pulse complete: go dark and stop.
                    _animation     = LedAnimation::None;
                    _hardwareDirty = true;
                    return;
                }
                else
                {
                    // Continuous breathe: start the next rise.
                    breathe.rising = true;
                }
            }
        }
    }

    _hardwareDirty = true;
}

void StatusLedManager::tickFlash(uint32_t nowMs)
{
    uint16_t intervalMs = _animState.flash.isOnPhase ? FLASH_ON_MS : FLASH_OFF_MS;

    if (nowMs - _lastAnimationTick < intervalMs)
    {
        return;
    }

    _lastAnimationTick += intervalMs;

    FlashState& flash = _animState.flash;

    if (flash.isOnPhase)
    {
        flash.isOnPhase = false;
    }
    else
    {
        // End of an off-phase: one complete burst (on + off) is done.
        if (flash.remainingFlashes > 0)
        {
            flash.remainingFlashes--;
        }

        if (flash.remainingFlashes == 0)
        {
            _animation = LedAnimation::None;
        }
        else
        {
            flash.isOnPhase = true;
        }
    }

    _hardwareDirty = true;
}

void StatusLedManager::tickRainbow(uint32_t nowMs)
{
    // _animationSpeed is re-used as the per-step interval for rainbow.
    uint16_t stepIntervalMs = (_animationSpeed > 0) ? _animationSpeed : RAINBOW_STEP_MS;

    if (nowMs - _lastAnimationTick < stepIntervalMs)
    {
        return;
    }

    _lastAnimationTick    += stepIntervalMs;
    _animState.rainbow.hue++;   // uint8_t wraps 255 -> 0 automatically
    _hardwareDirty         = true;
}

// ---------------------------------------------------------------------------
// Private: hardware output
// Only writePixelColor() and writePixelOff() may call _strip methods.
// Extend these two functions when adding multi-LED support.
// ---------------------------------------------------------------------------

void StatusLedManager::applyToHardware()
{
    _hardwareDirty = false;

    if (!_enabled)
    {
        writePixelOff();
        return;
    }

    switch (_animation)
    {
        case LedAnimation::None:
            writePixelOff();
            break;

        case LedAnimation::Solid:
            writePixelColor(_currentRgb.r, _currentRgb.g, _currentRgb.b);
            break;

        case LedAnimation::Blink:
        case LedAnimation::SlowBlink:
        case LedAnimation::FastBlink:
            if (_animState.blink.isOnPhase)
                writePixelColor(_currentRgb.r, _currentRgb.g, _currentRgb.b);
            else
                writePixelOff();
            break;

        case LedAnimation::Breathe:
        case LedAnimation::Pulse:
        {
            // Scale the base color by the breathe level, then apply master brightness.
            uint8_t level = _animState.breathe.level;
            uint8_t r     = scaleBrightness(_currentRgb.r, level);
            uint8_t g     = scaleBrightness(_currentRgb.g, level);
            uint8_t b     = scaleBrightness(_currentRgb.b, level);
            writePixelColor(r, g, b);
            break;
        }

        case LedAnimation::Flash:
            if (_animState.flash.isOnPhase)
                writePixelColor(_currentRgb.r, _currentRgb.g, _currentRgb.b);
            else
                writePixelOff();
            break;

        case LedAnimation::Rainbow:
        {
            RgbColor rgb = hueToRgb(_animState.rainbow.hue);
            writePixelColor(rgb.r, rgb.g, rgb.b);
            break;
        }
    }
}

void StatusLedManager::writePixelColor(uint8_t r, uint8_t g, uint8_t b)
{
    // Apply master brightness as a final scaling step.
    uint8_t scaledR = scaleBrightness(r, _brightness);
    uint8_t scaledG = scaleBrightness(g, _brightness);
    uint8_t scaledB = scaleBrightness(b, _brightness);

    _strip.setPixelColor(0, _strip.Color(scaledR, scaledG, scaledB));
    _strip.show();
}

void StatusLedManager::writePixelOff()
{
    _strip.setPixelColor(0, 0);
    _strip.show();
}

// ---------------------------------------------------------------------------
// Private: pure utility functions
// ---------------------------------------------------------------------------

StatusLedManager::RgbColor StatusLedManager::resolveNamedColor(LedColor color)
{
    switch (color)
    {
        case LedColor::Red:     return {255,   0,   0};
        case LedColor::Green:   return {  0, 255,   0};
        case LedColor::Blue:    return {  0,   0, 255};
        case LedColor::White:   return {255, 255, 255};
        case LedColor::Yellow:  return {255, 200,   0};
        case LedColor::Orange:  return {255,  80,   0};
        case LedColor::Purple:  return {150,   0, 200};
        case LedColor::Cyan:    return {  0, 255, 220};
        case LedColor::Pink:    return {255,  20,  80};
        case LedColor::Off:
        case LedColor::Custom:
        default:                return {  0,   0,   0};
    }
}

StatusLedManager::RgbColor StatusLedManager::hueToRgb(uint8_t hue)
{
    // Classic 3-segment hue wheel: R->G->B->R across 0-255.
    // Identical to the Adafruit Wheel() function used in their examples.
    uint8_t h = 255 - hue;

    if (h < 85)
    {
        return {static_cast<uint8_t>(255 - h * 3),
                0,
                static_cast<uint8_t>(h * 3)};
    }

    if (h < 170)
    {
        h -= 85;
        return {0,
                static_cast<uint8_t>(h * 3),
                static_cast<uint8_t>(255 - h * 3)};
    }

    h -= 170;
    return {static_cast<uint8_t>(h * 3),
            static_cast<uint8_t>(255 - h * 3),
            0};
}

uint8_t StatusLedManager::scaleBrightness(uint8_t value, uint8_t scale)
{
    // 16-bit intermediate prevents overflow; result stays in [0, value].
    return static_cast<uint8_t>((static_cast<uint16_t>(value) * scale) / 255);
}
