# July_24072026 — Generic plug-and-play firmware

**One binary for every board.** No per-board values compiled in. A board sets
itself up on first boot like a new WiFi router.

## Flash once (per board, factory step)
1. Arduino IDE 2.x, ESP32 core installed.
2. Libraries: **PubSubClient** (Nick O'Leary), **ArduinoJson** (bblanchon).
3. Copy `secrets.h.example` → `secrets.h`, fill the two shared values
   (`SECRET_PROVISION_KEY`, `SECRET_API_BASE`) — same on every board.
4. Flash. Done — this firmware never needs reflashing for config.

## First boot (installer / customer, no computer)
1. Board has no WiFi → starts AP **`HA-SETUP-<deviceId>`** (open network).
2. Phone connects → captive-portal page opens at `192.168.4.1`.
3. Pick home WiFi + password → **Connect**.
4. Board reboots, joins WiFi, calls `POST /api/provision` with its Hardware ID
   + the shared key → platform returns unique MQTT credentials (stored in NVS).
5. Board connects to MQTT → online. Customer adds it in the app by Hardware ID.

## Factory reset
Hold **BOOT (GPIO0)** ~5 s → clears WiFi + MQTT config → reboots into setup.

## Status LED
- Warning (needs setup / WiFi lost) · Busy (connecting) · Success (online) ·
  Error (factory reset / MQTT error).

## Behaviour notes
- Physical touch switches work **always** — before provisioning and while
  offline. Relay state persists in NVS across power cuts.
- `deviceId` = `esp32-<mac>` (12 hex from the chip MAC). Unique, printed on the
  board's QR/label; entered in the app to claim.
- MQTT topic/credential contract: see
  `home-automation-platform/ai-documents/MQTT_CONTRACT.md`.

## Modules used
`Config/ConfigStore` (NVS config) · `Provisioning/CaptivePortalManager`
(AP + captive portal) · `Cloud/MqttManager` · `Device/RelayManager` ·
`Device/StatusLedManager` · `Input/InputManager` · `Storage/PreferencesManager`.

> Not compile-tested in this environment (no board attached). Flash one unit
> and verify the AP → WiFi → provision → online handshake.
