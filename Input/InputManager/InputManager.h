#pragma once

// InputManager — the single owner of all digital input GPIO on an ESP32 device.
//
// No other module may call pinMode() or digitalRead() on an input pin.
// Higher layers (Application, MQTT, Switch, Scene) react to events:
//
//   input.configureInput(InputChannel::Input1, 13, InputType::TTP223,
//                        InputMode::PullDown, InputActiveState::ActiveHigh,
//                        "Kitchen Touch");
//   input.begin();
//   input.onPressed(onInputPressed);
//
// Each input is configured individually before begin().  This keeps GPIO
// numbers out of the application's event-handling logic: the application
// names its inputs once in setup() and reacts to logical events everywhere
// else.
//
// Debouncing runs entirely inside loop() via millis() — no delay(), no ISR.

#include <Arduino.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// InputChannel
//
// Type-safe input identifier.  Values equal their zero-based array indices
// (Input1 = 0, Input2 = 1, …) so channelToIndex() is a plain cast.
// Application code always uses channel names; GPIO numbers and indices stay
// hidden inside InputManager.
// ---------------------------------------------------------------------------
enum class InputChannel : uint8_t
{
    Input1  =  0,
    Input2  =  1,
    Input3  =  2,
    Input4  =  3,
    Input5  =  4,
    Input6  =  5,
    Input7  =  6,
    Input8  =  7,
    Input9  =  8,
    Input10 =  9,
    Input11 = 10,
    Input12 = 11,
    Input13 = 12,
    Input14 = 13,
    Input15 = 14,
    Input16 = 15
};

// ---------------------------------------------------------------------------
// InputType
//
// Stored per-input for future type-specific behaviour (e.g. different default
// debounce windows, PIR hold detection, ToggleSwitch state tracking).
// Phase 1 stores the type but does not differentiate behaviour by it.
// ---------------------------------------------------------------------------
enum class InputType : uint8_t
{
    PushButton,
    ToggleSwitch,
    TTP223,
    ReedSwitch,
    MagneticSensor,
    PIR,
    FloatSwitch,
    Custom
};

// ---------------------------------------------------------------------------
// InputMode
//
// Controls the GPIO pull resistor configuration applied in begin().
//   PullUp   → INPUT_PULLUP    (pin pulled to VCC; ActiveLow buttons)
//   PullDown → INPUT_PULLDOWN  (pin pulled to GND; ESP32-specific, ActiveHigh sensors)
//   Floating → INPUT           (no pull; requires external biasing)
// ---------------------------------------------------------------------------
enum class InputMode : uint8_t
{
    PullUp,
    PullDown,
    Floating
};

// ---------------------------------------------------------------------------
// InputActiveState
//
// Per-input electrical polarity:
//   ActiveHigh — GPIO HIGH means the input is active (Pressed).
//   ActiveLow  — GPIO LOW  means the input is active (Pressed).
//
// Per-input configuration allows mixed polarity on the same device without
// any application-side inversion.
// ---------------------------------------------------------------------------
enum class InputActiveState : uint8_t
{
    ActiveHigh,
    ActiveLow
};

// ---------------------------------------------------------------------------
// InputState
// ---------------------------------------------------------------------------
enum class InputState : uint8_t
{
    Released = 0,  // default on zero-init; GPIO idle
    Pressed  = 1
};

// ---------------------------------------------------------------------------
// InputEvent
//
// Used as a tag for future unified event callbacks and event queue entries.
// Phase 1 raises only Pressed and Released.
// ---------------------------------------------------------------------------
enum class InputEvent : uint8_t
{
    Pressed,
    Released,
    Click,
    DoubleClick,
    LongPress,
    VeryLongPress,
    Hold
};

// ---------------------------------------------------------------------------
// InputResult
// ---------------------------------------------------------------------------
enum class InputResult : uint8_t
{
    Success,             // operation accepted
    InvalidChannel,      // channel index >= MAX_INPUTS
    AlreadyConfigured,   // configureInput: channel already registered
    DuplicatePin,        // configureInput: GPIO pin used by another channel
    InvalidPin,          // configureInput: GPIO number out of valid range
    NotConfigured,       // query/control on a channel not yet configured
    Disabled,            // module is disabled
    ConfigurationClosed  // configureInput called after begin()
};

// ---------------------------------------------------------------------------
// InputManager
// ---------------------------------------------------------------------------

class InputManager
{
public:

    // Maximum number of input channels supported at compile time.
    static constexpr uint8_t  MAX_INPUTS          = 16;

    // Maximum length (including null terminator) for an input description.
    static constexpr uint8_t  MAX_DESCRIPTION_LEN = 32;

    // Sentinel returned by getInputPin() for an unconfigured channel.
    static constexpr uint8_t  INVALID_GPIO_PIN    = 0xFF;

    // Minimum time (ms) a raw GPIO state must be stable before it is accepted
    // as a confirmed state change.  Eliminates contact bounce on mechanical
    // switches; safe for solid-state sensors (TTP223, PIR) which have clean
    // output transitions.
    static constexpr uint32_t DEBOUNCE_MS         = 20;

    // -----------------------------------------------------------------------
    // Callback type
    //
    // Called from normal application context (inside loop()), never from an
    // ISR.  Raw function pointer — no std::function, no heap allocation.
    //   channel — which input raised the event
    //   context — opaque user pointer registered alongside the callback
    // -----------------------------------------------------------------------
    using InputEventCallback = void (*)(InputChannel channel, void* context);

    // -----------------------------------------------------------------------
    // Construction
    //
    // Default constructor only.  GPIO configuration happens entirely through
    // configureInput() calls before begin().
    // -----------------------------------------------------------------------
    InputManager();

    // -----------------------------------------------------------------------
    // Configuration  (must be called before begin())
    //
    // Registers one input channel.  Validates:
    //   - channel < MAX_INPUTS                 → InvalidChannel
    //   - channel not already configured        → AlreadyConfigured
    //   - gpioPin <= ESP32_MAX_GPIO_PIN         → InvalidPin
    //   - gpioPin not used by another channel   → DuplicatePin
    //   - begin() not yet called               → ConfigurationClosed
    //
    // description is stored internally (truncated to MAX_DESCRIPTION_LEN - 1)
    // for future diagnostics, web UI, and cloud integration.
    // -----------------------------------------------------------------------
    InputResult configureInput(InputChannel     channel,
                               uint8_t          gpioPin,
                               InputType        type,
                               InputMode        mode,
                               InputActiveState activeState,
                               const char*      description);

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Call once from setup() after all configureInput() calls.
    // Sets GPIO mode for every configured pin, reads the initial hardware
    // state, and seeds the debounce engine — prevents spurious callbacks
    // on the very first loop() tick.
    // Seals configuration: subsequent configureInput() calls return
    // ConfigurationClosed.
    void begin();

    // Call from every loop() iteration.
    // Reads all configured inputs, runs the debounce state machine, and fires
    // callbacks when confirmed state changes are detected.
    void loop();

    // -----------------------------------------------------------------------
    // Enable / disable (module-level)
    //
    // disable() stops loop() from processing inputs and stops callbacks from
    // firing.  It does NOT change GPIO pin modes.  enable() resumes normal
    // operation.  Query methods (isPressed, getState) always reflect the last
    // confirmed hardware state regardless of enabled state.
    // -----------------------------------------------------------------------

    void enable();
    void disable();
    bool isEnabled() const;

    // -----------------------------------------------------------------------
    // Information queries  (const; always safe)
    // -----------------------------------------------------------------------

    // True if the channel has been registered via configureInput().
    bool        isConfigured(InputChannel channel) const;

    // Number of channels successfully configured.
    uint8_t     getInputCount() const;

    // GPIO pin assigned to the channel, or INVALID_GPIO_PIN if not configured.
    uint8_t     getInputPin(InputChannel channel) const;

    // Human-readable description, or "" if not configured.
    const char* getDescription(InputChannel channel) const;

    // -----------------------------------------------------------------------
    // Input state queries  (const; always safe)
    //
    // Returns the last confirmed debounced state.  An invalid or unconfigured
    // channel returns the safe default: Released / false.
    // -----------------------------------------------------------------------

    InputState  getState(InputChannel channel) const;
    bool        isPressed(InputChannel channel) const;
    bool        isReleased(InputChannel channel) const;

    // -----------------------------------------------------------------------
    // Callbacks
    //
    // Each event type has its own registration method so callers can provide
    // a different handler (and optional context pointer) per event type.
    // Pass nullptr as callback to unregister.
    //
    // Phase 1 fires: onPressed, onReleased.
    // Phase 2+ fires: onClick, onDoubleClick, onLongPress, onVeryLongPress,
    //                 onHold (stored now; activation requires no API change).
    // -----------------------------------------------------------------------

    void onPressed      (InputEventCallback callback, void* context = nullptr);
    void onReleased     (InputEventCallback callback, void* context = nullptr);
    void onClick        (InputEventCallback callback, void* context = nullptr);
    void onDoubleClick  (InputEventCallback callback, void* context = nullptr);
    void onLongPress    (InputEventCallback callback, void* context = nullptr);
    void onVeryLongPress(InputEventCallback callback, void* context = nullptr);
    void onHold         (InputEventCallback callback, void* context = nullptr);

private:

    // ESP32 classic supports GPIO 0-39.
    // Raise for ESP32-S2 (0-45) or ESP32-S3 (0-48) variants.
    static constexpr uint8_t ESP32_MAX_GPIO_PIN = 39;

    // -----------------------------------------------------------------------
    // Per-input storage
    //
    // All MAX_INPUTS slots are pre-allocated; only configured == true entries
    // participate in polling and event dispatch.
    //
    // Fields needed by future phases are stored from day one so Phase 2+
    // features (Click, LongPress, Hold) can be activated without touching
    // the internal layout or the public API.
    // -----------------------------------------------------------------------
    struct InputEntry
    {
        uint8_t          pin;
        InputType        type;
        InputMode        mode;
        InputActiveState activeState;

        InputState       state;           // current confirmed debounced state
        InputState       previousState;   // state before last accepted change
        InputState       pendingState;    // raw reading held for debounce

        uint32_t         debounceStartMs; // when pendingState first diverged
        uint32_t         lastChangeMs;    // when debounced state last changed

        bool             configured;
        bool             enabled;         // per-input enable (future use)

        char             description[MAX_DESCRIPTION_LEN];
    };

    // -----------------------------------------------------------------------
    // Callback pair: function pointer + opaque context.
    // Grouped into a struct so each event type is self-contained.
    // -----------------------------------------------------------------------
    struct CallbackEntry
    {
        InputEventCallback fn;
        void*              context;
    };

    InputEntry    m_inputs[MAX_INPUTS];  // indexed by channelToIndex()
    uint8_t       m_configuredCount;
    bool          m_enabled;
    bool          m_began;

    CallbackEntry m_pressedCb;
    CallbackEntry m_releasedCb;
    CallbackEntry m_clickCb;
    CallbackEntry m_doubleClickCb;
    CallbackEntry m_longPressCb;
    CallbackEntry m_veryLongPressCb;
    CallbackEntry m_holdCb;

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    // Convert a channel enum to its zero-based array index.
    static uint8_t channelToIndex(InputChannel channel);

    // True when the channel value is within the valid index range.
    // Does NOT check whether it has been configured.
    bool isValidChannel(InputChannel channel) const;

    // True if gpioPin is already assigned to any configured channel.
    bool isPinAlreadyUsed(uint8_t gpioPin) const;

    // Map InputMode to the Arduino pinMode constant.
    static int arduinoPinMode(InputMode mode);

    // Read the raw GPIO level and translate it to InputState using the
    // channel's per-input active state (polarity inversion).
    InputState readRawState(uint8_t index) const;

    // Run one debounce tick for a single configured input entry.
    void tickInput(uint8_t index, uint32_t nowMs);

    // Invoke a callback if it is registered.
    static void fireCallback(const CallbackEntry& cb, InputChannel channel);

    // Store a callback+context pair for one event type.
    static void setCallback(CallbackEntry& entry,
                            InputEventCallback callback,
                            void* context);
};
