# ConfigStore - instruction.md

# Objective

Develop a reusable, production-quality ConfigStore for ESP32.

ConfigStore is the single owner of the `hacfg` NVS namespace. It persists everything the generic plug-and-play firmware learns at provisioning time — WiFi credentials and MQTT credentials — so no per-board value is ever compiled in.

No other module should open the `hacfg` namespace directly.

---

# Deliverables

Create only these files.

```text
Config/
└── ConfigStore/
    ├── ConfigStore.h
    ├── ConfigStore.cpp
    ├── README.md
    └── instruction.md
```

No captive portal.

No MQTT client.

No WiFi connection logic.

Only storage.

---

# Design Principles

Follow:

- Single Responsibility Principle
- Encapsulation
- Modern C++

ConfigStore owns everything related to persisted device configuration. It never connects to WiFi, never talks MQTT, never serves HTTP.

---

# Configuration Model

Two independent credential sets, cleared independently:

- **WiFi** — `ssid`, `pass`. Written by the captive portal. Cleared alone (`clearWifi()`) when saved WiFi never connects, so the board can reopen setup without losing MQTT provisioning.
- **MQTT** — `host`, `port`, `user`, `pass`, plus the `deviceId` the credentials were issued for. Written by the sketch's cloud provisioning step. Cleared alone (`clearMqtt()`) when the stored device id no longer matches the board's own id (credentials belong to different hardware).

`clear()` removes both sets for a full factory reset. Device identity itself (derived from `ESP.getEfuseMac()`) is never stored — it is always recomputed, so factory reset cannot desync it.

---

# Public API

```cpp
void begin();

bool hasWifi() const;
void getWifiSsid(char* out, size_t len) const;
void getWifiPass(char* out, size_t len) const;
void saveWifi(const char* ssid, const char* pass);
void clearWifi();

bool hasMqtt() const;
void getMqttHost(char* out, size_t len) const;
uint16_t getMqttPort() const;
void getMqttUser(char* out, size_t len) const;
void getMqttPass(char* out, size_t len) const;
void saveMqtt(const char* host, uint16_t port, const char* user, const char* pass, const char* deviceId);
void getMqttDeviceId(char* out, size_t len) const;
void clearMqtt();

void clear();
```

All string getters take a caller-owned `char*` buffer + length — never return `String`, never allocate.

---

# Validation

- Getters must always null-terminate the output buffer, even when the stored value is empty or missing.
- Getters must never overrun the caller-supplied buffer (`strncpy` + explicit `out[len-1] = '\0'`).
- `hasWifi()` / `hasMqtt()` must reflect actual NVS content — never cached in RAM — since credentials can be cleared by other code paths (mismatch check, factory reset) between calls.

---

# GPIO Ownership

Not applicable — ConfigStore touches no GPIO.

---

# Memory

Avoid:

- `new`
- `delete`
- returning `String` from any public getter

Prefer fixed-size caller-owned buffers. `Preferences::getString()` internally returns a `String` — acceptable because it is immediately copied into the caller's buffer and discarded, never stored as a member.

---

# Non-Blocking

`Preferences` NVS access is synchronous flash I/O — acceptable here because ConfigStore is only touched during setup/provisioning, never from a hot `loop()` path.

---

# Logging

ConfigStore must not use `Serial` / `Serial.println()`. Future `LoggerManager` will handle logging.

---

# Deviation From Framework Standard

ConfigStore does not implement `loop()`, `enable()`, `disable()`, `isEnabled()`, or a `Result` enum, unlike the standard framework lifecycle in `AGENT_INSTRUCTIONS.md`.

**Why:** it is a passive NVS-backed store, not a manager with ongoing runtime behavior. There is no periodic work for `loop()` to drive, and every operation either always succeeds (flash write) or has an unambiguous empty-string/false fallback that a `Result` enum would not improve — callers already have `hasWifi()`/`hasMqtt()` as the success check.

---

# Future Compatibility

The architecture should support future values (e.g. timezone, per-device feature flags) without changing existing getters/setters — add new key/getter/setter pairs, never repurpose an existing NVS key.

---

# Final Goal

Every other module treats device configuration as opaque key/value pairs behind `ConfigStore` — nothing outside this module ever opens the `hacfg` NVS namespace directly.
