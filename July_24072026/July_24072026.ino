/**
 * =============================================================================
 *  July_24072026.ino  —  GENERIC firmware (plug-and-play provisioning)
 * =============================================================================
 *  ONE binary for EVERY board. Nothing board-specific is compiled in — no
 *  WiFi, no MQTT credentials, no device id. A board configures itself on first
 *  boot like a new WiFi router:
 *
 *    1. No config  → starts AP  HA-SETUP-<id>  + captive portal (192.168.4.1)
 *    2. Installer picks home WiFi + password on their phone
 *    3. Board reboots, connects to WiFi
 *    4. Board calls POST /api/provision (hardwareId + shared provisioning key)
 *       → platform returns unique MQTT credentials → saved in NVS
 *    5. Board connects to MQTT → online → customer claims it in the app
 *
 *    Factory reset: hold the BOOT button (GPIO0) ~5 s → clears WiFi + MQTT
 *    config → reboots into setup mode. No reflashing ever needed.
 *
 *  Physical touch switches work at all times — even before provisioning and
 *  while offline (relay state persists in NVS).
 *
 *  The ONLY compiled-in values (identical on every board) live in secrets.h:
 *    SECRET_PROVISION_KEY  — shared key gating /api/provision
 *    SECRET_API_BASE       — platform API base URL
 *
 *  Hardware: B805-class ESP32 switch board (4 relays + 4 TTP223). Relay/switch
 *  GPIOs are the fixed board wiring; relay count is fixed per board model.
 *
 *  BUILD (Arduino IDE 2.x): module .cpp files included directly. Libraries:
 *    PubSubClient (Nick O'Leary), ArduinoJson (bblanchon).
 * =============================================================================
 */

#include "../Config/ConfigStore/ConfigStore.h"
#include "../Provisioning/CaptivePortalManager/CaptivePortalManager.h"
#include "../Device/RelayManager/RelayManager.h"
#include "../Device/StatusLedManager/StatusLedManager.h"
#include "../Input/InputManager/InputManager.h"
#include "../Storage/PreferencesManager/PreferencesManager.h"
#include "../Cloud/MqttManager/MqttManager.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Module implementations (single-translation-unit build).
#include "../Config/ConfigStore/ConfigStore.cpp"
#include "../Provisioning/CaptivePortalManager/CaptivePortalManager.cpp"
#include "../Device/RelayManager/RelayManager.cpp"
#include "../Device/StatusLedManager/StatusLedManager.cpp"
#include "../Input/InputManager/InputManager.cpp"
#include "../Storage/PreferencesManager/PreferencesManager.cpp"
#include "../Cloud/MqttManager/MqttManager.cpp"

#include "secrets.h"

static constexpr const char* PROVISION_KEY = SECRET_PROVISION_KEY;
static constexpr const char* API_BASE      = SECRET_API_BASE;   // e.g. https://...onrender.com
static constexpr const char* FIRMWARE_VERSION = "3.0.0-provision";
static constexpr const char* DEVICE_TYPE      = "controller";

// ---- fixed board wiring (B805 4-relay) ------------------------------------
static constexpr uint8_t STATUS_LED_PIN = 4;
static constexpr uint8_t LED_BRIGHTNESS = 50;
static constexpr uint8_t RELAY_COUNT    = 4;
static constexpr uint8_t RELAY_PIN[4]   = { 25, 33, 32, 27 };   // active-low
static constexpr uint8_t SWITCH_PIN[4]  = { 19, 18, 17, 16 };   // TTP223
static constexpr const char* RELAY_DESC[4] =
    { "Main Light", "Light", "Inner Entrance", "Entrance Light" };

static constexpr uint8_t  FACTORY_BUTTON_PIN = 0;       // BOOT button
static constexpr uint32_t FACTORY_HOLD_MS    = 5000UL;
static constexpr uint32_t HEARTBEAT_MS       = 300000UL; // 5 min
static constexpr uint32_t PROVISION_RETRY_MS = 15000UL;

// ---- module instances -----------------------------------------------------
ConfigStore          config;
CaptivePortalManager portal(config);
PreferencesManager   prefs;
RelayManager         relays;
StatusLedManager     led(STATUS_LED_PIN);
InputManager         inputs;
MqttManager          mqtt;

// ---- runtime state ---------------------------------------------------------
enum class Mode : uint8_t { Provisioning, Normal };
static Mode     g_mode = Mode::Normal;
static char     g_deviceId[24] = {};
static bool     g_wifiWasConnected = false;
static bool     g_mqttStarted = false;
static uint32_t g_lastProvisionAttempt = 0;
static uint32_t g_lastHeartbeat = 0;
static uint32_t g_factoryHoldStart = 0;

namespace PrefKey {
    constexpr const char* RelayState[4] =
        { "relay.1.state", "relay.2.state", "relay.3.state", "relay.4.state" };
}

// ---- forward decls ---------------------------------------------------------
void computeDeviceId();
void restoreRelaysFromNvs();
void onRelayStateChanged(RelayChannel ch, RelayState st, void* ctx);
void onInputPressed(InputChannel ch, void* ctx);
void onMqttCommand(uint8_t channel, bool on, void* ctx);
void onMqttEvent(MqttEvent e, MqttResult r, const char* topic, void* ctx);
bool cloudProvision();
void startMqttFromConfig();
void publishAllRelays();
void publishStatus();
void checkFactoryReset();

// ===========================================================================
// setup
// ===========================================================================
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] generic firmware July_24072026");

    pinMode(FACTORY_BUTTON_PIN, INPUT_PULLUP);
    prefs.begin();
    config.begin();
    computeDeviceId();
    Serial.printf("[BOOT] device id: %s\n", g_deviceId);

    // Relays + switches come up first so the room works regardless of network.
    led.begin();
    led.setBrightness(LED_BRIGHTNESS);
    led.setState(StatusLedState::Booting);

    relays.configureRelay(RelayChannel::Relay1, RELAY_PIN[0], RelayActiveState::ActiveLow, RELAY_DESC[0]);
    relays.configureRelay(RelayChannel::Relay2, RELAY_PIN[1], RelayActiveState::ActiveLow, RELAY_DESC[1]);
    relays.configureRelay(RelayChannel::Relay3, RELAY_PIN[2], RelayActiveState::ActiveLow, RELAY_DESC[2]);
    relays.configureRelay(RelayChannel::Relay4, RELAY_PIN[3], RelayActiveState::ActiveLow, RELAY_DESC[3]);
    relays.onStateChanged(onRelayStateChanged);
    relays.begin();
    restoreRelaysFromNvs();

    inputs.configureInput(InputChannel::Input1, SWITCH_PIN[0], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW1");
    inputs.configureInput(InputChannel::Input2, SWITCH_PIN[1], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW2");
    inputs.configureInput(InputChannel::Input3, SWITCH_PIN[2], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW3");
    inputs.configureInput(InputChannel::Input4, SWITCH_PIN[3], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW4");
    inputs.onPressed(onInputPressed);
    inputs.begin();

    WiFi.setSleep(false);   // ESP32 GPIO errata — avoid phantom touch edges

    if (!config.hasWifi())
    {
        g_mode = Mode::Provisioning;
        portal.begin(g_deviceId);
        led.setState(StatusLedState::Warning);   // "needs setup"
        Serial.printf("[SETUP] no WiFi config — AP 'HA-SETUP-%s' up at 192.168.4.1\n", g_deviceId);
    }
    else
    {
        g_mode = Mode::Normal;
        char ssid[CFG_MAX], pass[CFG_MAX];
        config.getWifiSsid(ssid, sizeof(ssid));
        config.getWifiPass(pass, sizeof(pass));
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);
        led.setState(StatusLedState::Busy);
        Serial.printf("[SETUP] connecting to WiFi '%s'...\n", ssid);
    }

    // MQTT wiring (used only in Normal mode once creds exist).
    mqtt.onEvent(onMqttEvent);
    mqtt.onCommand(onMqttCommand);
}

// ===========================================================================
// loop
// ===========================================================================
void loop()
{
    prefs.loop();
    inputs.loop();
    led.loop();
    relays.loop();
    checkFactoryReset();

    if (g_mode == Mode::Provisioning)
    {
        portal.loop();
        return;
    }

    // ---- Normal mode ----
    const bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected && !g_wifiWasConnected)
    {
        g_wifiWasConnected = true;
        Serial.printf("[WIFI] connected, ip=%s\n", WiFi.localIP().toString().c_str());
        led.setState(StatusLedState::Busy);
    }
    else if (!connected && g_wifiWasConnected)
    {
        g_wifiWasConnected = false;
        led.setState(StatusLedState::Warning);
    }

    if (connected)
    {
        // Fetch MQTT creds once, then start MQTT.
        if (!config.hasMqtt())
        {
            const uint32_t now = millis();
            if (g_lastProvisionAttempt == 0 || (now - g_lastProvisionAttempt) > PROVISION_RETRY_MS)
            {
                g_lastProvisionAttempt = now;
                if (cloudProvision()) startMqttFromConfig();
            }
        }
        else if (!g_mqttStarted)
        {
            startMqttFromConfig();
        }
    }

    if (g_mqttStarted)
    {
        mqtt.loop();
        const uint32_t now = millis();
        if (mqtt.isConnected() && (g_lastHeartbeat == 0 || now - g_lastHeartbeat > HEARTBEAT_MS))
        {
            g_lastHeartbeat = now;
            publishStatus();
        }
    }
}

// ===========================================================================
// helpers
// ===========================================================================
void computeDeviceId()
{
    uint64_t mac = ESP.getEfuseMac();
    // 12 hex chars from the 48-bit MAC → stable, unique, printed on the label.
    snprintf(g_deviceId, sizeof(g_deviceId), "esp32-%04x%08x",
             (uint16_t)(mac >> 32), (uint32_t)mac);
}

void restoreRelaysFromNvs()
{
    if (!prefs.isReady()) return;
    for (uint8_t i = 0; i < RELAY_COUNT; i++)
    {
        bool on = false;
        prefs.load(PrefKey::RelayState[i], on, false);
        if (on) relays.setState(static_cast<RelayChannel>(i), RelayState::On);
    }
}

void onRelayStateChanged(RelayChannel ch, RelayState st, void* /*ctx*/)
{
    const uint8_t idx = static_cast<uint8_t>(ch);
    if (idx < RELAY_COUNT) prefs.save(PrefKey::RelayState[idx], st == RelayState::On);
}

void onInputPressed(InputChannel ch, void* /*ctx*/)
{
    const uint8_t idx = static_cast<uint8_t>(ch);
    const auto relayCh = static_cast<RelayChannel>(idx);
    relays.toggle(relayCh);
    const bool on = relays.isOn(relayCh);
    Serial.printf("[INPUT] %s -> %s\n", RELAY_DESC[idx], on ? "ON" : "OFF");
    if (g_mqttStarted) mqtt.publishRelayState(idx + 1, on);   // queued if offline
}

void onMqttCommand(uint8_t channel, bool on, void* /*ctx*/)
{
    if (channel < 1 || channel > RELAY_COUNT) return;
    const auto relayCh = static_cast<RelayChannel>(channel - 1);
    relays.setState(relayCh, on ? RelayState::On : RelayState::Off);
    mqtt.publishRelayState(channel, relays.isOn(relayCh));
}

void onMqttEvent(MqttEvent e, MqttResult /*r*/, const char* /*topic*/, void* /*ctx*/)
{
    switch (e)
    {
        case MqttEvent::Connected:
            led.setState(StatusLedState::Success);
            publishAllRelays();
            publishStatus();
            break;
        case MqttEvent::Disconnected: led.setState(StatusLedState::Warning); break;
        case MqttEvent::Error:        led.setState(StatusLedState::Error);   break;
        default: break;
    }
}

bool cloudProvision()
{
    Serial.println("[PROVISION] requesting MQTT credentials...");
    WiFiClientSecure client;
    client.setInsecure();               // V1: skip cert pinning; platform is public CA

    HTTPClient http;
    String url = String(API_BASE) + "/api/provision";
    if (!http.begin(client, url)) { Serial.println("[PROVISION] begin failed"); return false; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Provision-Key", PROVISION_KEY);

    char body[192];
    snprintf(body, sizeof(body),
             "{\"hardwareId\":\"%s\",\"firmwareVersion\":\"%s\",\"deviceType\":\"%s\",\"relayCount\":%u}",
             g_deviceId, FIRMWARE_VERSION, DEVICE_TYPE, RELAY_COUNT);

    const int code = http.POST((uint8_t*)body, strlen(body));
    if (code != 200)
    {
        Serial.printf("[PROVISION] HTTP %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload)) { Serial.println("[PROVISION] bad JSON"); return false; }

    const char* host = doc["mqttHost"] | "";
    const uint16_t port = doc["mqttPort"] | 8883;
    const char* user = doc["mqttUsername"] | "";
    const char* pass = doc["mqttPassword"] | "";
    if (!*host || !*user) { Serial.println("[PROVISION] missing fields"); return false; }

    config.saveMqtt(host, port, user, pass);
    Serial.println("[PROVISION] credentials stored");
    return true;
}

void startMqttFromConfig()
{
    if (g_mqttStarted) return;
    char host[CFG_MAX], user[CFG_MAX], pass[CFG_MAX];
    config.getMqttHost(host, sizeof(host));
    config.getMqttUser(user, sizeof(user));
    config.getMqttPass(pass, sizeof(pass));
    mqtt.configure(host, config.getMqttPort(), user, pass, g_deviceId);
    mqtt.begin();
    g_mqttStarted = true;
    Serial.println("[MQTT] started");
}

void publishAllRelays()
{
    for (uint8_t i = 0; i < RELAY_COUNT; i++)
        mqtt.publishRelayState(i + 1, relays.isOn(static_cast<RelayChannel>(i)));
}

void publishStatus()
{
    mqtt.publishStatus(FIRMWARE_VERSION, WiFi.RSSI(), ESP.getFreeHeap(), 0,
                       (uint32_t)(millis() / 1000UL));
}

// Hold BOOT button FACTORY_HOLD_MS → wipe config → reboot into setup.
void checkFactoryReset()
{
    if (digitalRead(FACTORY_BUTTON_PIN) == LOW)   // pressed (active-low)
    {
        if (g_factoryHoldStart == 0) g_factoryHoldStart = millis();
        else if (millis() - g_factoryHoldStart > FACTORY_HOLD_MS)
        {
            Serial.println("[FACTORY] reset — clearing config, rebooting");
            led.setState(StatusLedState::Error);
            config.clear();
            delay(300);
            ESP.restart();
        }
    }
    else
    {
        g_factoryHoldStart = 0;
    }
}
