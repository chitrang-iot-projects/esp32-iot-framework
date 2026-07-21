#pragma once

// MqttManager — the single owner of all MQTT communication for this ESP32 IoT
// Framework. Replaces FirebaseManager for the new home-automation-platform,
// which uses an MQTT broker (EMQX Cloud) instead of Firebase RTDB.
//
// Contract (see home-automation-platform/ai-documents/MQTT_CONTRACT.md):
//   subscribe : ha/<deviceId>/relay/+/set     payload "1"|"0"
//   publish   : ha/<deviceId>/relay/<n>       "1"|"0"   retained
//   publish   : ha/<deviceId>/status          JSON      retained, every heartbeat + boot
//   LWT       : ha/<deviceId>/status          {"online":false}  retained
//
// No other module may call PubSubClient / WiFiClientSecure directly.
//
// Dependencies (install via Arduino Library Manager):
//   PubSubClient by Nick O'Leary   (v2.8+)
//   ArduinoJson  by bblanchon       (v7+)
//
// WiFiClientSecure + PubSubClient are stored as value-type members (no heap,
// matching the framework's no-new rule). PubSubClient's message callback is a
// plain function pointer with no context argument, so the bridge to the
// instance goes through a static s_instance pointer — the same technique
// FirebaseManager uses for its token-status callback. Only one MqttManager
// owns the connection at a time, so a single instance pointer suffices.

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Compile-time limits
// ---------------------------------------------------------------------------

static constexpr uint16_t MQTT_MAX_CONFIG_LEN = 128;  // host / user / pass
static constexpr uint16_t MQTT_MAX_ID_LEN     = 64;   // device id
static constexpr uint16_t MQTT_MAX_TOPIC_LEN  = 128;
static constexpr uint16_t MQTT_MAX_PAYLOAD_LEN = 256; // status JSON fits here
static constexpr uint16_t MQTT_BUFFER_BYTES   = 1024; // PubSubClient RX/TX buffer

// Offline publish queue: retained relay-state publishes that happened while
// disconnected are replayed on reconnect. Status is never queued (stale).
static constexpr uint8_t  MQTT_MAX_QUEUE = 12;

// Reconnect backoff.
static constexpr uint32_t MQTT_RETRY_BASE_MS = 2000UL;
static constexpr uint32_t MQTT_RETRY_MAX_MS  = 30000UL;

// ---------------------------------------------------------------------------
// Public enums
// ---------------------------------------------------------------------------

enum class MqttState : uint8_t
{
    NotConfigured,   // configure() not called
    Configured,      // credentials stored; begin() not called
    Connecting,      // attempting broker connection
    Connected,       // connected + subscribed; fully operational
    Disconnected,    // connection lost; auto-reconnect pending
    Disabled,        // disable() called
    Error            // repeated failures; reconnect() to retry
};

enum class MqttResult : uint8_t
{
    Success,
    Queued,          // publish accepted into offline queue (not yet sent)
    NotConnected,    // broker not connected and value not queued
    InvalidArg,      // null/empty topic or bad channel
    QueueFull,       // offline queue at capacity; publish dropped
    NotConfigured,   // configure() not called
    Disabled         // module disabled
};

enum class MqttEvent : uint8_t
{
    Connecting,
    Connected,        // (re)connected + subscribed — republish all state here
    Disconnected,
    CommandReceived,  // a relay/<n>/set arrived (delivered via CommandCallback)
    PublishFailed,
    Error
};

// ---------------------------------------------------------------------------
// MqttManager
// ---------------------------------------------------------------------------

class MqttManager
{
public:
    // General lifecycle/event callback (logging, LED, etc).
    using EventCallback = void (*)(MqttEvent event, MqttResult result,
                                   const char* topic, void* context);

    // Relay command from the cloud: channel is 1-based, on = desired state.
    using CommandCallback = void (*)(uint8_t channel, bool on, void* context);

    MqttManager();

    // -----------------------------------------------------------------------
    // Configuration (before begin()). Credentials stored, never exposed.
    //   caCert : PEM root CA for TLS validation. Pass nullptr/"" to skip
    //            validation (setInsecure) — convenient for first bring-up,
    //            but pin the CA for production.
    // -----------------------------------------------------------------------
    MqttResult configure(const char* host, uint16_t port,
                         const char* username, const char* password,
                         const char* deviceId, const char* caCert = nullptr);

    void begin();     // call once in setup() after configure()
    void loop();      // call every loop(); non-blocking

    void enable();
    void disable();
    bool isEnabled() const;

    // Force a fresh connection attempt (clears Error + backoff).
    MqttResult reconnect();

    bool      isConnected() const;
    bool      isReady()     const { return isConnected(); }
    MqttState getState()    const;

    // -----------------------------------------------------------------------
    // Publish helpers. When connected: sent immediately. When not connected:
    // relay state is queued (replayed on reconnect); status is dropped.
    // -----------------------------------------------------------------------
    MqttResult publishRelayState(uint8_t channel, bool on);
    MqttResult publishStatus(const char* firmwareVersion, int rssiDbm,
                            uint32_t freeHeapBytes, uint32_t bootCount,
                            uint32_t uptimeSeconds);

    void onEvent(EventCallback cb, void* context = nullptr);
    void onCommand(CommandCallback cb, void* context = nullptr);

private:
    struct QueueEntry
    {
        char topic[MQTT_MAX_TOPIC_LEN];
        char payload[8];   // "1"/"0"
        bool retained;
    };

    // Config.
    char     m_host[MQTT_MAX_CONFIG_LEN];
    uint16_t m_port;
    char     m_username[MQTT_MAX_CONFIG_LEN];
    char     m_password[MQTT_MAX_CONFIG_LEN];
    char     m_deviceId[MQTT_MAX_ID_LEN];
    const char* m_caCert;          // not owned; points at PROGMEM/const string
    char     m_topicPrefix[MQTT_MAX_ID_LEN + 8]; // "ha/<deviceId>"
    char     m_statusTopic[MQTT_MAX_TOPIC_LEN];   // "ha/<deviceId>/status"
    char     m_cmdFilter[MQTT_MAX_TOPIC_LEN];     // "ha/<deviceId>/relay/+/set"
    char     m_clientId[MQTT_MAX_ID_LEN + 8];

    // Lifecycle.
    bool      m_configured;
    bool      m_began;
    bool      m_enabled;
    MqttState m_state;

    // Reconnect timing.
    uint32_t m_lastAttemptMs;
    uint32_t m_retryDelayMs;

    // Offline queue (FIFO ring).
    QueueEntry m_queue[MQTT_MAX_QUEUE];
    uint8_t    m_qHead, m_qTail, m_qCount;

    // Transport. WiFiClientSecure must outlive the PubSubClient that wraps it.
    WiFiClientSecure m_net;
    PubSubClient     m_client;

    // Callbacks.
    EventCallback   m_eventCb;
    void*           m_eventCtx;
    CommandCallback m_commandCb;
    void*           m_commandCtx;

    // Static bridge for PubSubClient's contextless message callback.
    static MqttManager* s_instance;
    static void messageTrampoline(char* topic, uint8_t* payload, unsigned int len);
    void handleMessage(const char* topic, const uint8_t* payload, unsigned int len);

    // Helpers.
    void attemptConnect(uint32_t nowMs);
    bool doPublish(const char* topic, const char* payload, bool retained);
    MqttResult enqueue(const char* topic, const char* payload, bool retained);
    void flushQueue();
    void fireEvent(MqttEvent e, MqttResult r = MqttResult::Success, const char* topic = "");
    void setState(MqttState s);
    static bool isWiFiConnected();
};
