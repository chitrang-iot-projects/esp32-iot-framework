# CaptivePortalManager

First-boot WiFi setup, "like a new router".

When the board has no WiFi configured it starts its own access point (`HA-SETUP-<deviceId>`) and serves a captive portal at `192.168.4.1`. The installer connects with a phone, the OS auto-opens the page, they pick their home WiFi (scanned datalist, or type manually) and enter the password. Credentials are saved to `ConfigStore` and the board reboots to connect.

Only WiFi is collected here — MQTT credentials are fetched automatically from the platform after the board is online (the sketch's cloud provisioning step, after this portal hands off). Keeps the installer experience to just "pick WiFi".

Depends on `ConfigStore` (constructor injection) and the ESP32 core libraries `WiFi` + `DNSServer` + `WebServer`.

---

# Installation

Copy the module into your project.

```text
Provisioning/
└── CaptivePortalManager/
    ├── CaptivePortalManager.h
    ├── CaptivePortalManager.cpp
    ├── README.md
    └── instruction.md
```

Include the header.

```cpp
#include "Provisioning/CaptivePortalManager/CaptivePortalManager.h"
```

---

# Create Object

`ConfigStore` is injected — the portal writes into it, never owns it.

```cpp
ConfigStore config;
CaptivePortalManager portal(config);
```

---

# Setup

Only start the portal when there's no WiFi configured yet.

```cpp
void setup()
{
    config.begin();

    if (!config.hasWifi())
    {
        portal.begin(deviceId);   // SSID becomes HA-SETUP-<deviceId>
    }
}
```

---

# Loop

Call every loop iteration. No-ops immediately when the portal isn't active — safe to call unconditionally.

```cpp
void loop()
{
    portal.loop();
}
```

---

# Check Active

```cpp
if (portal.isActive())
{
    // still in setup mode, waiting for the installer
}
```

---

# Public API

```cpp
explicit CaptivePortalManager(ConfigStore& store);

void begin(const char* apSuffix);
void loop();
bool isActive() const;
```

---

# What It Does

- Opens an open (no-password) SoftAP named `HA-SETUP-<apSuffix>` in `WIFI_AP_STA` mode (STA stays up so the installer's chosen network can be scanned/listed).
- Starts a catch-all DNS server (`*` → `192.168.4.1`) so any hostname the phone probes resolves to the portal — this is what makes the OS auto-open the setup page.
- Serves `/` with a scanned-network `<datalist>` plus a free-text SSID field (manual entry for hidden/out-of-range networks) and a password field.
- On `POST /save`, saves the entered SSID/password via `ConfigStore::saveWifi()`, responds with a confirmation page, then `ESP.restart()`s after a short delay to let the response flush.
- Any unknown request gets a 302 redirect back to `/` — the mechanism that triggers the phone's captive-portal detection popup.

---

# Deviation From Framework Standard

CaptivePortalManager does not implement `enable()` / `disable()` / `isEnabled()`, and its operations return `void`/`bool` rather than a `Result` enum.

**Why:** the portal is a one-shot setup-mode flow, not a manager toggled on/off during normal operation — it is either not started, or active until the device reboots (there is no "disable the portal" case in the field). Errors from the save flow (missing SSID) are shown directly in the served HTML to the installer rather than returned to firmware caller code, since the only "caller" reading the result is a human with a phone browser, not another module.

---

# Best Practices

- Only call `begin()` when `ConfigStore::hasWifi()` is false — starting it while already connected drops the STA connection.
- Never call `digitalWrite()`/`pinMode()` from application code to "help" the portal — it owns no GPIO of its own.
- Keep the portal's SoftAP open (no password) — the setup network is short-lived and scoped to the installer's own premises.
- Don't assume the portal survives across `ESP.restart()` — it is recreated fresh from `setup()` on next boot.
