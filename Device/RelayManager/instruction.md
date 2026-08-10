# RelayManager - instruction.md

# Objective

Develop a reusable, production-quality RelayManager for ESP32.

RelayManager is the single owner of all relay GPIO pins.

No other module in the framework should directly call:

- pinMode()
- digitalWrite()
- digitalRead()

All relay operations must go through RelayManager.

---

# Deliverables

Create only these files.

```text
Device/
└── RelayManager/
    ├── RelayManager.h
    ├── RelayManager.cpp
    ├── README.md
    └── instruction.md
```

No examples.

No WiFi.

No MQTT.

No Switch logic.

Only RelayManager.

---

# Design Principles

Follow:

- Single Responsibility Principle
- Open / Closed Principle
- Clean Architecture
- Modern C++
- Encapsulation

RelayManager owns everything related to relay hardware.

---

# Relay Capacity

Support a configurable compile-time maximum.

Example

```cpp
MAX_RELAYS
```

Default

```text
16
```

The application may configure any number of relays from

```text
1 ... MAX_RELAYS
```

without modifying the library.

---

# Configuration

RelayManager should not receive relay configuration in its constructor.

Instead, the application creates the manager first.

Example

```cpp
RelayManager relay;
```

Before calling begin(), each relay is configured individually.

Provide a public API similar to

```cpp
configureRelay(
    RelayChannel channel,
    uint8_t gpioPin,
    const char* description
);
```

Parameters

- RelayChannel
- GPIO Pin
- Human-readable description

The description should be stored internally for future diagnostics, web UI, cloud integration, and logging.

Phase 1 does not use the description.

---

# Configuration Rules

- configureRelay() must be called before begin().
- Duplicate relay channels must be rejected.
- Duplicate GPIO pins must be rejected.
- Invalid GPIO numbers must be rejected.
- Relay count must never exceed MAX_RELAYS.
- Relays that are not configured remain unavailable.
- begin() initializes only configured relays.

---

# Example Usage

```cpp
RelayManager relay;

void setup()
{
    relay.configureRelay(
        RelayChannel::Relay1,
        23,
        "Kitchen Light"
    );

    relay.configureRelay(
        RelayChannel::Relay2,
        5,
        "Bedroom Light"
    );

    relay.configureRelay(
        RelayChannel::Relay3,
        18,
        "Exhaust Fan"
    );

    relay.begin();
}
```

Changing ESP32 boards should only require changing GPIO numbers inside configureRelay().

The RelayManager library itself should never require modification.

---

# Enumerations

Use enum class wherever possible.

Minimum required enums.

## RelayChannel

Relay1

Relay2

...

Relay16

---

## RelayState

Off

On

---

## RelayActiveState

ActiveHigh

ActiveLow

---

## RelayResult

Success

InvalidChannel

AlreadyOn

AlreadyOff

AlreadyConfigured

DuplicatePin

InvalidPin

NotConfigured

Disabled

ConfigurationClosed

---

# Public API

The public API should include functionality equivalent to:

Lifecycle

- begin()
- loop()
- enable()
- disable()
- isEnabled()

Configuration

- configureRelay(...)

Single Relay

- turnOn(...)
- turnOff(...)
- toggle(...)
- setState(...)
- getState(...)
- isOn(...)
- isOff(...)

Bulk Operations

- turnAllOn()
- turnAllOff()
- toggleAll()

Information

- getRelayCount()
- isConfigured(...)
- getRelayPin(...)
- getRelayDescription(...)

Callbacks

- onStateChanged(...)

---

# Internal Storage

RelayManager should internally maintain information for every configured relay.

Each relay should contain

- Relay Channel
- GPIO Pin
- Description
- Relay State
- Configured Flag
- Enabled Flag

Application code must never access these members directly.

---

# GPIO Ownership

Only RelayManager may access relay GPIO pins.

No other module should call

- pinMode()
- digitalWrite()
- digitalRead()

for relay outputs.

---

# Active Logic

Support

Active HIGH

Active LOW

The application selects the active logic.

The application must never manually invert GPIO values.

---

# Validation

Validate every public operation.

Examples

- Invalid relay channel
- Relay not configured
- Duplicate GPIO
- Duplicate relay channel
- Module disabled
- Already ON
- Already OFF

Return RelayResult instead of silently failing.

---

# Performance

Avoid unnecessary GPIO writes.

If the relay is already ON

do not write the GPIO again.

Maintain an internal relay state table.

Do not continuously read GPIO pins.

---

# Non-Blocking

Never use

- delay()
- while()

loop() should execute quickly.

---

# Logging

RelayManager must not use

- Serial
- Serial.println()

Future LoggerManager will handle logging.

---

# Memory

Avoid

- new
- delete
- String

Prefer fixed-size arrays.

Suitable for 24/7 embedded systems.

---

# Future Compatibility

The architecture should support future features without changing the public API.

Examples

- Relay Groups
- Relay Scenes
- Scheduler
- Timers
- Auto-Off
- Persistent State
- Power-On Restore
- Cloud Synchronization
- Web Dashboard
- Statistics
- Usage Counter
- Per-Relay Inversion

---

# Coding Standards

Use

- enum class
- const correctness
- descriptive naming
- helper functions
- private members
- short methods
- no duplicated code

Readable code is preferred.

---

# Framework Standard

RelayManager follows the common lifecycle used by every framework module.

```cpp
begin();

loop();

enable();

disable();

isEnabled();
```

Future modules should expose the same lifecycle.

---

# Final Goal

The application should never know anything about GPIO operations.

Instead, it configures the hardware once.

```cpp
relay.configureRelay(
    RelayChannel::Relay1,
    23,
    "Kitchen Light"
);
```

After that, the application only uses logical relay operations.

```cpp
relay.turnOn(RelayChannel::Relay1);

relay.turnOff(RelayChannel::Relay2);

relay.toggle(RelayChannel::Relay3);
```

RelayManager becomes the single source of truth for relay configuration, relay state, and relay hardware across all ESP32 projects.
