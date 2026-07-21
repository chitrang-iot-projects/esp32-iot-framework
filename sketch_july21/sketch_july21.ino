/**
 * =============================================================================
 *  sketch_july21.ino  —  MQTT edition (home-automation-platform)
 * =============================================================================
 *  Successor of sketch_july04. Same hardware, same pins, same user-visible
 *  behaviour — but the cloud transport is now MQTT (EMQX Cloud broker) instead
 *  of Firebase RTDB, to talk to the new home-automation-platform.
 *
 *  Modules used:
 *    Events/EventBus            — pub/sub backbone
 *    Logging/LoggerManager      — central logging
 *    Scheduling/Scheduler       — boot-complete + periodic status heartbeat
 *    Storage/PreferencesManager — relay state persistence + boot counter (NVS)
 *    Core/DeviceStateManager    — runtime state database
 *    Network/IoTWiFiManager     — WiFi lifecycle
 *    Cloud/MqttManager          — MQTT connect/subscribe/publish (NEW)
 *    Device/RelayManager        — 4 active-low relay channels
 *    Device/StatusLedManager    — WS2812B status LED
 *    Input/InputManager         — 4 TTP223 capacitive touch switches
 *
 *  DROPPED vs sketch_july04:
 *    Cloud/FirebaseManager and Synchronization/SyncManager — MqttManager now
 *    owns all cloud I/O (relay state + device status telemetry).
 *
 *  Hardware : B805 Main Hall — ESP32
 *  Device ID: set in secrets.h (SECRET_DEVICE_ID); must equal the board's
 *             hardware_id registered in the admin portal.
 *
 * -----------------------------------------------------------------------------
 *  CLOUD CONTRACT (see home-automation-platform/ai-documents/MQTT_CONTRACT.md)
 *    subscribe : ha/<id>/relay/<n>/set   "1"|"0"   (command from platform)
 *    publish   : ha/<id>/relay/<n>       "1"|"0"   retained (reported state)
 *    publish   : ha/<id>/status          JSON      retained (telemetry + LWT)
 *
 *  Unlike the Firebase build there is no self-echo problem: the device
 *  subscribes only to the command topic (…/set), never to its own state topic,
 *  so a physical toggle and a cloud command never fight over an echo. Commands
 *  are applied directly; physical toggles publish the resulting state.
 * -----------------------------------------------------------------------------
 *  BUILD NOTE (Arduino IDE 2.x): module .cpp files are included directly
 *  (single-translation-unit trick). Install libraries: PubSubClient (Nick
 *  O'Leary) and ArduinoJson (bblanchon).
 *
 *  Credentials live in sketch_july21/secrets.h (gitignored).
 *  Copy secrets.h.example → secrets.h and fill in values before flashing.
 * =============================================================================
 */

// ---------------------------------------------------------------------------
// Module headers
// ---------------------------------------------------------------------------
#include "../Events/EventBus/EventBus.h"
#include "../Logging/LoggerManager/LoggerManager.h"
#include "../Scheduling/Scheduler/Scheduler.h"
#include "../Storage/PreferencesManager/PreferencesManager.h"
#include "../Network/IoTWiFiManager/IoTWiFiManager.h"
#include "../Core/DeviceStateManager/DeviceStateManager.h"
#include "../Device/RelayManager/RelayManager.h"
#include "../Device/StatusLedManager/StatusLedManager.h"
#include "../Input/InputManager/InputManager.h"
#include "../Cloud/MqttManager/MqttManager.h"

#include <WiFi.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Module implementations (single-translation-unit build)
// ---------------------------------------------------------------------------
#include "../Events/EventBus/EventBus.cpp"
#include "../Logging/LoggerManager/LoggerManager.cpp"
#include "../Scheduling/Scheduler/Scheduler.cpp"
#include "../Storage/PreferencesManager/PreferencesManager.cpp"
#include "../Network/IoTWiFiManager/IoTWiFiManager.cpp"
#include "../Device/RelayManager/RelayManager.cpp"
#include "../Device/StatusLedManager/StatusLedManager.cpp"
#include "../Input/InputManager/InputManager.cpp"
#include "../Core/DeviceStateManager/DeviceStateManager.cpp"
#include "../Cloud/MqttManager/MqttManager.cpp"

#include "secrets.h"

// ---------------------------------------------------------------------------
// Credentials (from secrets.h)
// ---------------------------------------------------------------------------
static constexpr const char* WIFI_SSID     = SECRET_WIFI_SSID;
static constexpr const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
static constexpr const char* DEVICE_ID     = SECRET_DEVICE_ID;
static constexpr const char* MQTT_HOST     = SECRET_MQTT_HOST;
static constexpr uint16_t    MQTT_PORT     = SECRET_MQTT_PORT;
static constexpr const char* MQTT_USER     = SECRET_MQTT_USER;
static constexpr const char* MQTT_PASS     = SECRET_MQTT_PASS;

// Optional TLS root CA. Leave SECRET_MQTT_CA empty to skip validation
// (convenient for first bring-up; pin the CA for production).
#ifndef SECRET_MQTT_CA
#define SECRET_MQTT_CA ""
#endif
static constexpr const char* MQTT_CA = SECRET_MQTT_CA;

static constexpr const char* FIRMWARE_VERSION = "2.0.0-mqtt";

// ---------------------------------------------------------------------------
// GPIO — B805 Main Hall (identical to sketch_july04)
// ---------------------------------------------------------------------------
static constexpr uint8_t STATUS_LED_PIN = 4;
static constexpr uint8_t LED_BRIGHTNESS = 50;
static constexpr uint8_t RELAY_PIN[4]   = { 25, 33, 32, 27 };   // active-low
static constexpr uint8_t SWITCH_PIN[4]  = { 19, 18, 17, 16 };   // TTP223

static constexpr const char* RELAY_DESC[4] =
{
    "Main Light", "Light", "Inner Entrance", "Entrance Light",
};

// ---------------------------------------------------------------------------
// Scheduler intervals
// ---------------------------------------------------------------------------
static constexpr uint32_t BOOT_COMPLETE_DELAY_MS = 2000UL;
static constexpr uint32_t HEARTBEAT_INTERVAL_MS  = 300000UL;    // 5 minutes

// ---------------------------------------------------------------------------
// Module instances
// ---------------------------------------------------------------------------
EventBus           eventBus;
LoggerManager      logger;
Scheduler          scheduler;
PreferencesManager prefs;
DeviceStateManager deviceState;
IoTWiFiManager     wifiMgr(WIFI_SSID, WIFI_PASSWORD);
MqttManager        mqtt;
RelayManager       relays;
StatusLedManager   led(STATUS_LED_PIN);
InputManager       inputs;

// ---------------------------------------------------------------------------
// Persistent storage keys
// ---------------------------------------------------------------------------
namespace PreferenceKey
{
    constexpr const char* RelayState[4] =
        { "relay.1.state", "relay.2.state", "relay.3.state", "relay.4.state" };
    constexpr const char* BootCount = "sys.bootCount";
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
static bool       mqttStarted   = false;
static WiFiState  prevWifiState = WiFiState::Idle;

// EventBus payload (copied inline; scalars only).
struct RelayEventPayload { uint8_t channel; uint8_t state; };

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void appLogf(LogLevel level, const char* format, ...);
void onRelayStateChanged(RelayChannel channel, RelayState newState, void* ctx);
void onInputPressed(InputChannel channel, void* ctx);
void onInputReleased(InputChannel channel, void* ctx);
void onMqttEvent(MqttEvent event, MqttResult result, const char* topic, void* ctx);
void onMqttCommand(uint8_t channel, bool on, void* ctx);
void onBootCompleted(const Event& event, void* ctx);
void onStatusHeartbeat(const Event& event, void* ctx);
void publishAllRelayStates();
void publishStatusNow();
void restoreRelayStatesFromNvs();

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(300);

    eventBus.begin();
    logger.begin(eventBus);
    logger.info("[SETUP] IoT Framework — sketch_july21 (MQTT edition)");

    if (prefs.begin() == PreferencesResult::Success)
    {
        uint32_t bootCount = 0;
        prefs.load(PreferenceKey::BootCount, bootCount, 0u);
        ++bootCount;
        prefs.save(PreferenceKey::BootCount, bootCount);
        appLogf(LogLevel::Info, "[SETUP] NVS ok — boot #%lu",
                static_cast<unsigned long>(bootCount));
    }
    else
    {
        logger.error("[SETUP] NVS failed — running without persistence");
    }

    deviceState.begin();

    led.begin();
    led.setBrightness(LED_BRIGHTNESS);
    led.setState(StatusLedState::Booting);
    logger.info("[SETUP] LED ok");

    relays.configureRelay(RelayChannel::Relay1, RELAY_PIN[0], RelayActiveState::ActiveLow, RELAY_DESC[0]);
    relays.configureRelay(RelayChannel::Relay2, RELAY_PIN[1], RelayActiveState::ActiveLow, RELAY_DESC[1]);
    relays.configureRelay(RelayChannel::Relay3, RELAY_PIN[2], RelayActiveState::ActiveLow, RELAY_DESC[2]);
    relays.configureRelay(RelayChannel::Relay4, RELAY_PIN[3], RelayActiveState::ActiveLow, RELAY_DESC[3]);
    relays.onStateChanged(onRelayStateChanged);
    relays.begin();

    restoreRelayStatesFromNvs();   // recover instantly after a power cut
    logger.info("[SETUP] Relays ok");

    inputs.configureInput(InputChannel::Input1, SWITCH_PIN[0], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW-MainLight");
    inputs.configureInput(InputChannel::Input2, SWITCH_PIN[1], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW-Light");
    inputs.configureInput(InputChannel::Input3, SWITCH_PIN[2], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW-InnerEntrance");
    inputs.configureInput(InputChannel::Input4, SWITCH_PIN[3], InputType::TTP223, InputMode::Floating, InputActiveState::ActiveHigh, "SW-Entrance");
    inputs.onPressed(onInputPressed);
    inputs.onReleased(onInputReleased);
    inputs.begin();
    logger.info("[SETUP] Inputs ok");

    // MQTT — configure now; connection deferred until WiFi is up.
    mqtt.configure(MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, DEVICE_ID, MQTT_CA);
    mqtt.onEvent(onMqttEvent);
    mqtt.onCommand(onMqttCommand);
    logger.info("[SETUP] MQTT configured");

    scheduler.begin(eventBus);
    scheduler.scheduleOnce(EventType::BootCompleted, BOOT_COMPLETE_DELAY_MS);
    scheduler.scheduleRepeating(EventType::SyncStarted, HEARTBEAT_INTERVAL_MS);
    eventBus.subscribe(EventType::BootCompleted, onBootCompleted);
    eventBus.subscribe(EventType::SyncStarted,   onStatusHeartbeat);
    logger.info("[SETUP] Scheduler ok");

    appLogf(LogLevel::Info, "[SETUP] heap: free=%lu  largestBlock=%lu",
            static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(ESP.getMaxAllocHeap()));

    // Disable modem-sleep BEFORE WiFi begins (ESP32 GPIO errata → phantom edges).
    WiFi.setSleep(false);
    wifiMgr.begin();
    prevWifiState = wifiMgr.getState();
    led.setState(StatusLedState::Idle);
    logger.info("[SETUP] WiFi begin — waiting for connection...");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop()
{
    eventBus.loop();
    logger.loop();
    scheduler.loop();
    prefs.loop();
    deviceState.loop();

    wifiMgr.loop();
    inputs.loop();
    led.loop();
    relays.loop();

    // Start MQTT once WiFi connects; MqttManager then self-manages reconnects.
    if (!mqttStarted && wifiMgr.isConnected())
    {
        mqttStarted = true;
        appLogf(LogLevel::Info, "[LOOP] heap before MQTT: free=%lu  largestBlock=%lu",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        mqtt.begin();
        led.setState(StatusLedState::Busy);
        logger.info("[LOOP] WiFi up — MQTT started");
    }

    mqtt.loop();

    // React to WiFi state transitions (mirror into DeviceStateManager + LED).
    const WiFiState currWifi = wifiMgr.getState();
    if (currWifi != prevWifiState)
    {
        prevWifiState = currWifi;
        deviceState.setWiFiState(currWifi);
        switch (currWifi)
        {
            case WiFiState::Connected:     eventBus.publish(EventType::WiFiConnected,     EventSource::Application); break;
            case WiFiState::Reconnecting:  eventBus.publish(EventType::WiFiReconnecting,  EventSource::Application); led.setState(StatusLedState::Warning); break;
            case WiFiState::Disconnected:  eventBus.publish(EventType::WiFiDisconnected,  EventSource::Application); led.setState(StatusLedState::Warning); break;
            default: break;
        }
    }

    deviceState.setOnline(wifiMgr.isConnected() && mqtt.isConnected());
}

// ---------------------------------------------------------------------------
// appLogf
// ---------------------------------------------------------------------------
void appLogf(LogLevel level, const char* format, ...)
{
    char buf[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    switch (level)
    {
        case LogLevel::Debug:   logger.debug(buf);   break;
        case LogLevel::Info:    logger.info(buf);    break;
        case LogLevel::Warning: logger.warning(buf); break;
        default:                logger.error(buf);   break;
    }
}

// ---------------------------------------------------------------------------
// onRelayStateChanged — fires on every genuine relay transition
// (physical touch, cloud command, boot restore). Records runtime state, NVS,
// and the event bus. Cloud publish is done by the caller so we don't double
// publish during the NVS restore loop at boot.
// ---------------------------------------------------------------------------
void onRelayStateChanged(RelayChannel channel, RelayState newState, void* /*ctx*/)
{
    const uint8_t idx = static_cast<uint8_t>(channel);
    deviceState.setRelayState(channel, newState);

    if (idx < 4)
        prefs.save(PreferenceKey::RelayState[idx], newState == RelayState::On);

    const RelayEventPayload payload = { static_cast<uint8_t>(channel), static_cast<uint8_t>(newState) };
    eventBus.publish(EventType::RelayStateChanged, EventSource::Application, &payload, sizeof(payload));
}

// ---------------------------------------------------------------------------
// onInputPressed — physical switch touched: toggle locally, publish to cloud.
// Works fully offline; MqttManager queues the publish and replays on reconnect.
// ---------------------------------------------------------------------------
void onInputPressed(InputChannel channel, void* /*ctx*/)
{
    const uint8_t idx     = static_cast<uint8_t>(channel);
    const auto    relayCh = static_cast<RelayChannel>(idx);

    deviceState.setSwitchState(static_cast<SwitchChannel>(idx), SwitchState::Pressed);
    eventBus.publish(EventType::SwitchPressed, EventSource::Application);

    relays.toggle(relayCh);                 // onRelayStateChanged handles state + NVS
    const bool newState = relays.isOn(relayCh);
    appLogf(LogLevel::Info, "[INPUT] %s -> %s", RELAY_DESC[idx], newState ? "ON" : "OFF");

    const MqttResult r = mqtt.publishRelayState(idx + 1, newState);
    if (r == MqttResult::Queued)
        appLogf(LogLevel::Info, "[MQTT] relay%u queued (offline)", idx + 1);
    else if (r != MqttResult::Success)
        appLogf(LogLevel::Warning, "[MQTT] relay%u publish failed (result=%d)", idx + 1, static_cast<int>(r));
}

// ---------------------------------------------------------------------------
// onInputReleased — bookkeeping only
// ---------------------------------------------------------------------------
void onInputReleased(InputChannel channel, void* /*ctx*/)
{
    const uint8_t idx = static_cast<uint8_t>(channel);
    deviceState.setSwitchState(static_cast<SwitchChannel>(idx), SwitchState::Released);
    eventBus.publish(EventType::SwitchReleased, EventSource::Application);
}

// ---------------------------------------------------------------------------
// onMqttCommand — relay command from the platform (ha/<id>/relay/<n>/set)
// Apply the requested state, then publish the resulting state back so the
// dashboard's optimistic value reconciles with hardware truth.
// ---------------------------------------------------------------------------
void onMqttCommand(uint8_t channel, bool on, void* /*ctx*/)
{
    if (channel < 1 || channel > 4) return;
    const auto relayCh = static_cast<RelayChannel>(channel - 1);

    relays.setState(relayCh, on ? RelayState::On : RelayState::Off);
    appLogf(LogLevel::Info, "[CMD] relay%u -> %s (cloud)", channel, on ? "ON" : "OFF");
    mqtt.publishRelayState(channel, relays.isOn(relayCh));
}

// ---------------------------------------------------------------------------
// onMqttEvent — connection lifecycle → LED + republish on (re)connect
// ---------------------------------------------------------------------------
void onMqttEvent(MqttEvent event, MqttResult /*result*/, const char* /*topic*/, void* /*ctx*/)
{
    switch (event)
    {
        case MqttEvent::Connecting:
            led.setState(StatusLedState::Busy);
            logger.info("[MQTT] connecting...");
            break;

        case MqttEvent::Connected:
            led.setState(StatusLedState::Success);
            logger.info("[MQTT] connected");
            deviceState.setDeviceMode(DeviceMode::Normal);
            publishAllRelayStates();   // sync hardware truth to the cloud
            publishStatusNow();
            break;

        case MqttEvent::Disconnected:
            led.setState(StatusLedState::Warning);
            logger.warning("[MQTT] disconnected");
            break;

        case MqttEvent::Error:
            led.setState(StatusLedState::Error);
            logger.error("[MQTT] connection error — retrying with backoff");
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// onBootCompleted — 2 s after setup()
// ---------------------------------------------------------------------------
void onBootCompleted(const Event& /*event*/, void* /*ctx*/)
{
    deviceState.setDeviceMode(DeviceMode::Normal);
}

// ---------------------------------------------------------------------------
// onStatusHeartbeat — every 5 minutes
// ---------------------------------------------------------------------------
void onStatusHeartbeat(const Event& /*event*/, void* /*ctx*/)
{
    appLogf(LogLevel::Info, "[STATUS] uptime=%lus  wifi=%s  rssi=%d dBm  heap=%lu",
            static_cast<unsigned long>(deviceState.getUptime() / 1000UL),
            wifiMgr.isConnected() ? "up" : "down",
            static_cast<int>(wifiMgr.getRSSI()),
            static_cast<unsigned long>(ESP.getFreeHeap()));
    publishStatusNow();
}

// ---------------------------------------------------------------------------
// publishAllRelayStates / publishStatusNow
// ---------------------------------------------------------------------------
void publishAllRelayStates()
{
    for (uint8_t i = 0; i < 4; i++)
        mqtt.publishRelayState(i + 1, relays.isOn(static_cast<RelayChannel>(i)));
}

void publishStatusNow()
{
    uint32_t bootCount = 0;
    if (prefs.isReady()) prefs.load(PreferenceKey::BootCount, bootCount, 0u);
    mqtt.publishStatus(FIRMWARE_VERSION,
                       static_cast<int>(wifiMgr.getRSSI()),
                       ESP.getFreeHeap(),
                       bootCount,
                       static_cast<uint32_t>(deviceState.getUptime() / 1000UL));
}

// ---------------------------------------------------------------------------
// restoreRelayStatesFromNvs — apply last states at boot, before network
// ---------------------------------------------------------------------------
void restoreRelayStatesFromNvs()
{
    if (!prefs.isReady()) return;
    for (uint8_t i = 0; i < 4; i++)
    {
        bool savedOn = false;
        prefs.load(PreferenceKey::RelayState[i], savedOn, false);
        if (savedOn)
        {
            relays.setState(static_cast<RelayChannel>(i), RelayState::On);
            appLogf(LogLevel::Info, "[SETUP] restored %s -> ON", RELAY_DESC[i]);
        }
    }
}
