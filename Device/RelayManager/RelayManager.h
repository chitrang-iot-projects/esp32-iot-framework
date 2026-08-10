#pragma once

// RelayManager — the single owner of relay GPIO on an ESP32 device.
//
// No other module may call pinMode(), digitalWrite(), or digitalRead() on a
// relay pin.  Higher layers (MQTT, Switch, Scheduler, Scene) interact
// exclusively through this class:
//
//   relay.configureRelay(RelayChannel::Relay1, 23, RelayActiveState::ActiveLow, "Kitchen Light");
//   relay.begin();
//   relay.turnOn(RelayChannel::Relay1);
//
// Each relay is configured individually before begin().  This keeps GPIO
// numbers out of the public API: the application names its relays once in
// setup() and then operates on logical channels everywhere else.
//
// The class maintains an authoritative in-RAM copy of every relay's state
// and writes to hardware only when that state actually changes.  Fully
// non-blocking: no delay(), no while-loops, no Serial.

#include <Arduino.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// RelayChannel
//
// Type-safe relay identifier.  Values equal their zero-based array indices
// (Relay1 = 0, Relay2 = 1, …) so channelToIndex() is a plain cast with no
// arithmetic.  Application code always uses these names; GPIO numbers and
// array indices stay hidden inside RelayManager.
// ---------------------------------------------------------------------------
enum class RelayChannel : uint8_t
{
    Relay1  =  0,
    Relay2  =  1,
    Relay3  =  2,
    Relay4  =  3,
    Relay5  =  4,
    Relay6  =  5,
    Relay7  =  6,
    Relay8  =  7,
    Relay9  =  8,
    Relay10 =  9,
    Relay11 = 10,
    Relay12 = 11,
    Relay13 = 12,
    Relay14 = 13,
    Relay15 = 14,
    Relay16 = 15
};

// ---------------------------------------------------------------------------
// RelayState
// ---------------------------------------------------------------------------
enum class RelayState : uint8_t
{
    Off,
    On
};

// ---------------------------------------------------------------------------
// RelayActiveState
//
// Per-relay electrical wiring:
//   ActiveHigh — driving the GPIO HIGH energises the relay coil (On = HIGH).
//   ActiveLow  — driving the GPIO LOW  energises the relay coil (On = LOW).
//
// Each relay has its own active state so boards with mixed wiring on the same
// device are supported without any application-side inversion.
// ---------------------------------------------------------------------------
enum class RelayActiveState : uint8_t
{
    ActiveHigh,
    ActiveLow
};

// ---------------------------------------------------------------------------
// RelayResult
//
// Returned by every mutating operation.  Callers may ignore it for fire-and-
// forget use, or inspect it for precise feedback.
// ---------------------------------------------------------------------------
enum class RelayResult : uint8_t
{
    Success,             // state changed and written to hardware
    InvalidChannel,      // channel index >= MAX_RELAYS
    AlreadyOn,           // no-op: relay was already On  (hardware untouched)
    AlreadyOff,          // no-op: relay was already Off (hardware untouched)
    AlreadyConfigured,   // configureRelay: this channel is already set up
    DuplicatePin,        // configureRelay: GPIO pin is already used by another channel
    InvalidPin,          // configureRelay: GPIO number out of valid range
    NotConfigured,       // control call on a channel that has not been configured
    Disabled,            // module is disabled; command rejected, hardware untouched
    ConfigurationClosed  // configureRelay called after begin() — too late
};

// ---------------------------------------------------------------------------
// RelayManager
// ---------------------------------------------------------------------------

class RelayManager
{
public:

    // Maximum number of relay channels supported at compile time.
    // Matches RelayChannel::Relay1 .. Relay16.
    static constexpr uint8_t MAX_RELAYS = 16;

    // Maximum length (including null terminator) for a relay description string.
    static constexpr uint8_t MAX_DESCRIPTION_LEN = 32;

    // Sentinel returned by getRelayPin() for an unconfigured channel.
    // Callers may compare against this value.
    static constexpr uint8_t INVALID_GPIO_PIN = 0xFF;

    // -----------------------------------------------------------------------
    // State-change callback
    //
    // Called synchronously after a relay's state actually changes.
    // Raw function pointer (no std::function): zero heap allocation, safe for
    // 24/7 embedded use.
    //   channel  - which relay changed (type-safe)
    //   newState - the state it transitioned to
    //   context  - opaque user pointer registered with onStateChanged()
    // -----------------------------------------------------------------------
    using StateChangedCallback =
        void (*)(RelayChannel channel, RelayState newState, void* context);

    // -----------------------------------------------------------------------
    // Construction
    //
    // Default constructor only.  All relay-specific configuration is done
    // through configureRelay() before begin() is called.
    // -----------------------------------------------------------------------
    RelayManager();

    // -----------------------------------------------------------------------
    // Configuration  (must be called before begin())
    //
    // Registers one relay channel.  Each call is validated:
    //   - channel must be a valid RelayChannel (< MAX_RELAYS)
    //   - channel must not already be configured
    //   - gpioPin must be within the valid ESP32 GPIO range
    //   - gpioPin must not already be used by another configured channel
    //   - begin() must not have been called yet (ConfigurationClosed)
    //
    // description is stored internally (truncated to MAX_DESCRIPTION_LEN - 1)
    // for future diagnostics, web UI, and cloud integration.  It is not used
    // in Phase 1 beyond storage.
    //
    // Returns Success on acceptance, or the appropriate RelayResult on error.
    // -----------------------------------------------------------------------
    RelayResult configureRelay(RelayChannel    channel,
                               uint8_t         gpioPin,
                               RelayActiveState activeState,
                               const char*     description);

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Call once from setup(), after all configureRelay() calls.
    // Configures every registered pin as OUTPUT and drives it to the OFF level
    // so relays do not glitch on at boot.  Seals configuration: subsequent
    // configureRelay() calls return ConfigurationClosed.
    void begin();

    // Call from every loop() iteration.  A no-op placeholder in Phase 1;
    // kept so the public contract is stable when future features (timed pulses,
    // auto-off timers, persistence flush) need per-tick work.
    void loop();

    // -----------------------------------------------------------------------
    // Enable / disable (module-level)
    //
    // disable() does NOT change physical relay outputs; it rejects mutating
    // commands (they return Disabled) until enable() is called again.  The
    // last commanded state is held across a disable/enable cycle.
    // Query methods always work regardless of enabled state.
    // -----------------------------------------------------------------------

    void enable();
    void disable();
    bool isEnabled() const;

    // -----------------------------------------------------------------------
    // Single-channel control
    //
    // Validation order inside setState() — all single-channel methods call it:
    //   1. channel < MAX_RELAYS?          → InvalidChannel
    //   2. channel is configured?         → NotConfigured
    //   3. module is enabled?             → Disabled
    //   4. state is different?            → AlreadyOn / AlreadyOff
    //   5. write hardware, notify, return → Success
    // -----------------------------------------------------------------------

    RelayResult turnOn(RelayChannel channel);
    RelayResult turnOff(RelayChannel channel);
    RelayResult toggle(RelayChannel channel);
    RelayResult setState(RelayChannel channel, RelayState state);

    // -----------------------------------------------------------------------
    // Single-channel query  (const; always safe, even when disabled)
    //
    // An invalid or unconfigured channel returns the safe default: Off / false.
    // -----------------------------------------------------------------------

    RelayState  getState(RelayChannel channel) const;
    bool        isOn(RelayChannel channel) const;
    bool        isOff(RelayChannel channel) const;

    // -----------------------------------------------------------------------
    // Bulk control
    //
    // Iterates all slots 0..MAX_RELAYS-1.  Unconfigured slots are skipped.
    // Configured slots are driven through setState() / toggle(), so:
    //   - per-channel callbacks fire individually
    //   - "write only on change" rule holds per channel
    //   - calls are rejected when disabled
    // -----------------------------------------------------------------------

    void turnAllOn();
    void turnAllOff();
    void toggleAll();

    // -----------------------------------------------------------------------
    // Information queries  (const; always safe)
    // -----------------------------------------------------------------------

    // Number of relays that have been successfully configured.
    uint8_t     getRelayCount() const;

    // True if the channel has been registered via configureRelay().
    bool        isConfigured(RelayChannel channel) const;

    // GPIO pin assigned to the channel, or INVALID_GPIO_PIN if not configured.
    uint8_t     getRelayPin(RelayChannel channel) const;

    // Human-readable description, or "" if not configured.
    const char* getRelayDescription(RelayChannel channel) const;

    // -----------------------------------------------------------------------
    // State-change callback
    //
    // Pass nullptr as callback to unregister.
    // context is optional opaque user data forwarded to the callback.
    // -----------------------------------------------------------------------
    void onStateChanged(StateChangedCallback callback, void* context = nullptr);

private:

    // -----------------------------------------------------------------------
    // Compile-time validation limit for configureRelay().
    // ESP32 classic has GPIO 0-39.  Raise for ESP32-S2 (0-45) or S3 (0-48).
    // -----------------------------------------------------------------------
    static constexpr uint8_t ESP32_MAX_GPIO_PIN = 39;

    // -----------------------------------------------------------------------
    // Per-relay storage
    //
    // One RelayEntry exists for every possible RelayChannel slot (MAX_RELAYS).
    // Only entries where configured == true are live; the rest are ignored.
    // -----------------------------------------------------------------------
    struct RelayEntry
    {
        uint8_t          pin;                            // GPIO number
        RelayActiveState activeState;                    // wiring polarity
        RelayState       state;                          // current logical state
        bool             configured;                     // true after configureRelay()
        bool             enabled;                        // per-relay enable (future use)
        char             description[MAX_DESCRIPTION_LEN]; // null-terminated label
    };

    RelayEntry m_relays[MAX_RELAYS];   // indexed by channelToIndex()
    uint8_t    m_configuredCount;      // count of configured (live) relays
    bool       m_enabled;              // module-level enable gate
    bool       m_began;               // true after begin(); seals configuration

    StateChangedCallback m_callback;
    void*                m_callbackContext;

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    // Convert a RelayChannel to its zero-based array index.
    // The enum values equal their indices by design — this is a plain cast.
    static uint8_t channelToIndex(RelayChannel channel);

    // True when channel is a valid index (< MAX_RELAYS).
    // Does NOT check whether it has been configured.
    bool isValidChannel(RelayChannel channel) const;

    // True if gpioPin is already assigned to any configured channel.
    bool isPinAlreadyUsed(uint8_t gpioPin) const;

    // Translate a logical RelayState to HIGH or LOW for the given wiring.
    static uint8_t physicalLevelFor(RelayActiveState activeState, RelayState state);

    // Write state to the physical GPIO pin at internal index.
    // Callers must ensure: m_began == true, index < MAX_RELAYS, configured.
    void writePin(uint8_t index, RelayState state);

    // Fire the state-changed callback if one is registered.
    void notifyStateChanged(RelayChannel channel, RelayState newState);
};
