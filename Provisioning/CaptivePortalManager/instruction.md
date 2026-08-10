# CaptivePortalManager - instruction.md

# Objective

Develop a reusable, production-quality CaptivePortalManager for ESP32.

CaptivePortalManager is the single owner of setup-mode SoftAP + DNS + HTTP for first-boot WiFi provisioning. No other module should start a SoftAP, run a DNS server, or serve HTTP for the purpose of WiFi setup.

---

# Deliverables

Create only these files.

```text
Provisioning/
└── CaptivePortalManager/
    ├── CaptivePortalManager.h
    ├── CaptivePortalManager.cpp
    ├── README.md
    └── instruction.md
```

No MQTT provisioning.

No cloud `/api/provision` call.

No relay/input/status-LED logic.

Only WiFi setup.

---

# Design Principles

Follow:

- Single Responsibility Principle
- Encapsulation
- Modern C++
- Dependency injection (`ConfigStore` passed into the constructor, never a global)

CaptivePortalManager owns everything related to the first-boot WiFi setup experience: SoftAP, DNS catch-all, HTTP portal page, saving the result.

---

# Behavior

- `begin(apSuffix)` starts `WIFI_AP_STA` mode, SoftAP named `HA-SETUP-<apSuffix>`, catch-all DNS (`*` → `192.168.4.1`), and the HTTP server. Sets the active flag.
- `loop()` services DNS + HTTP every call. Must no-op immediately (single `if (!m_active) return;`) when not started — safe to call unconditionally from the sketch's `loop()` regardless of provisioning state.
- `GET /` scans nearby networks synchronously and renders a page with a `<datalist>`-backed SSID input (pick from scan, or type manually for hidden/out-of-range networks) and a password field.
- `POST /save` validates the SSID is non-empty, calls `ConfigStore::saveWifi()`, sends a confirmation page, then reboots (`ESP.restart()`) after a short `delay()` to flush the HTTP response.
- Unknown paths get a 302 redirect to `/` — required for the OS to detect a captive portal and auto-open the setup page.

---

# Public API

```cpp
explicit CaptivePortalManager(ConfigStore& store);

void begin(const char* apSuffix);
void loop();
bool isActive() const;
```

`ConfigStore&` is stored by reference — CaptivePortalManager never owns or constructs its own ConfigStore.

---

# GPIO Ownership

Not applicable — CaptivePortalManager touches no GPIO. It owns only WiFi radio mode, DNS server, and HTTP server — no other module should touch these for setup purposes.

---

# Validation

- The served page MUST declare `<meta charset='utf-8'>` and the form MUST set `accept-charset='utf-8'`. Without them the phone guesses a legacy encoding and any SSID character it cannot represent is submitted back as an HTML numeric reference — a network named with an emoji arrived as the literal text `&#128225;`, was stored verbatim, and could never match the real network.
- SSIDs interpolated into the scan `<datalist>` MUST be HTML-escaped (`htmlEscape()`); a quote or ampersand in a neighbour's SSID would otherwise break out of the attribute.
- Reject an empty SSID in `handleSave()` — respond 400 with a message the installer can act on, do not save or restart.
- `loop()` must check `m_active` before touching `m_dns`/`m_server` — calling into an unstarted `WebServer`/`DNSServer` is undefined behavior.

---

# Non-Blocking

`loop()` itself must never block. The one exception is the short `delay(800)` in `handleSave()` immediately before `ESP.restart()` — acceptable because it only runs once, at the very end of the setup flow, to let the HTTP response flush before the reboot tears down the network stack.

`WiFi.scanNetworks()` in `handleRoot()` is synchronous and blocks for ~2-4s — acceptable because it only runs in setup mode, serving one installer at a time, not during normal operation.

---

# Memory

Avoid `new` / `delete`. `String` is used for the rendered HTML page and scanned SSIDs — acceptable here (deviation from the general no-`String` rule) because this code runs only during human-paced setup, not in the 24/7 hot loop, and `WebServer`/`DNSServer` APIs are `String`-based by design.

---

# Logging

CaptivePortalManager must not use `Serial` / `Serial.println()`. Future `LoggerManager` will handle logging.

---

# Deviation From Framework Standard

No `enable()` / `disable()` / `isEnabled()`, no `Result` enum return values.

**Why:** the portal is a one-shot setup flow started once per provisioning cycle, never toggled during normal operation — there is no runtime "disable the portal" case. The only consumer of its results is the installer's phone browser, not other firmware code, so failures are rendered as HTML rather than returned as a `Result` enum.

---

# Future Compatibility

The architecture should support future portal fields (e.g. device name, timezone) without changing `begin()`/`loop()`/`isActive()` — add new form fields + a new `ConfigStore` setter, keep the public API unchanged.

---

# Final Goal

Every board ships one generic binary. On first boot with no WiFi configured, the installer experience is: connect to `HA-SETUP-<id>`, pick or type WiFi, done — CaptivePortalManager is the sole owner of that experience.
