# ConfigStore

Persistent device configuration in NVS.

Holds everything the board learns at provisioning time so it survives reboots and power cuts: home WiFi credentials (entered in the captive portal) and MQTT credentials (fetched from the platform's `/api/provision`).

Generic firmware — no per-board values are compiled in. Every board runs the same binary and fills this store during setup.

---

# Installation

Copy the module into your project.

```text
Config/
└── ConfigStore/
    ├── ConfigStore.h
    ├── ConfigStore.cpp
    ├── README.md
    └── instruction.md
```

Include the header.

```cpp
#include "Config/ConfigStore/ConfigStore.h"
```

---

# Create Object

```cpp
ConfigStore config;
```

---

# Setup

```cpp
void setup()
{
    config.begin();
}
```

`begin()` only opens the NVS namespace (`hacfg`) to validate access — it does not load values into RAM. Every getter reads NVS directly.

---

# WiFi Credentials

Set by the captive portal (`CaptivePortalManager::handleSave()`).

```cpp
if (config.hasWifi())
{
    char ssid[64];
    char pass[64];
    config.getWifiSsid(ssid, sizeof(ssid));
    config.getWifiPass(pass, sizeof(pass));
}

config.saveWifi("MyHomeWiFi", "secretpass");
```

`clearWifi()` removes only the WiFi entry (keeps MQTT credentials) — used when saved WiFi never connects, so the board reopens the captive portal without losing its cloud provisioning.

```cpp
config.clearWifi();
```

---

# MQTT Credentials

Set by the sketch's cloud provisioning step, after the board reaches the platform's `/api/provision` endpoint.

```cpp
if (config.hasMqtt())
{
    char host[64];
    char user[64];
    char pass[64];
    config.getMqttHost(host, sizeof(host));
    uint16_t port = config.getMqttPort();
    config.getMqttUser(user, sizeof(user));
    config.getMqttPass(pass, sizeof(pass));
}

config.saveMqtt("mqtt.example.com", 8883, "device-user", "device-pass", deviceId);
```

MQTT credentials are stored together with the `deviceId` they were issued for. Always check the stored id against the board's own id before using saved credentials:

```cpp
char storedId[64];
config.getMqttDeviceId(storedId, sizeof(storedId));

if (strcmp(storedId, myDeviceId) != 0)
{
    config.clearMqtt();   // credentials belong to a different device — discard
}
```

```cpp
config.clearMqtt();
```

---

# Factory Reset

Clears WiFi + MQTT config. Device identity is derived from the ESP32's eFuse MAC at runtime and is never stored, so it survives a factory reset.

```cpp
config.clear();
```

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

---

# Deviation From Framework Standard

ConfigStore does not implement `loop()`, `enable()`, `disable()`, `isEnabled()`, or a `Result` enum.

**Why:** it is a passive NVS-backed storage/DAO, not a runtime manager with ongoing behavior — every call is a synchronous read/write to flash with no state machine to drive from `loop()`, and failure has nowhere useful to propagate to inside a getter that fills a caller-owned buffer. Getters silently return an empty string when a key is absent; callers must check `hasWifi()`/`hasMqtt()` first rather than inspect a result code.

---

# Best Practices

- Call `config.begin()` once in `setup()` before any other call.
- Buffers passed to getters should be at least 64 bytes — values longer than the buffer are truncated and always null-terminated.
- Always validate `getMqttDeviceId()` against the board's own id before trusting stored MQTT credentials.
- Use `clearWifi()` for "wrong WiFi, reopen setup" — use `clearMqtt()` only when the device id no longer matches — use `clear()` only for a full factory reset.
