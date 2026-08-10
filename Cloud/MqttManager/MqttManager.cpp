#include "MqttManager.h"
#include <WiFi.h>
#include <string.h>
#include <stdio.h>

MqttManager* MqttManager::s_instance = nullptr;

MqttManager::MqttManager()
    : m_port(8883),
      m_caCert(nullptr),
      m_configured(false),
      m_began(false),
      m_enabled(true),
      m_state(MqttState::NotConfigured),
      m_lastAttemptMs(0),
      m_retryDelayMs(MQTT_RETRY_BASE_MS),
      m_qHead(0), m_qTail(0), m_qCount(0),
      m_client(m_net),
      m_eventCb(nullptr), m_eventCtx(nullptr),
      m_commandCb(nullptr), m_commandCtx(nullptr)
{
    m_host[0] = m_username[0] = m_password[0] = '\0';
    m_deviceId[0] = m_topicPrefix[0] = m_statusTopic[0] = '\0';
    m_cmdFilter[0] = m_clientId[0] = '\0';
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

MqttResult MqttManager::configure(const char* host, uint16_t port,
                                  const char* username, const char* password,
                                  const char* deviceId, const char* caCert)
{
    if (m_began)       return MqttResult::NotConfigured;
    if (!host || !*host || !deviceId || !*deviceId) return MqttResult::InvalidArg;

    strncpy(m_host, host, sizeof(m_host) - 1);            m_host[sizeof(m_host) - 1] = '\0';
    strncpy(m_deviceId, deviceId, sizeof(m_deviceId) - 1); m_deviceId[sizeof(m_deviceId) - 1] = '\0';
    if (username) { strncpy(m_username, username, sizeof(m_username) - 1); m_username[sizeof(m_username) - 1] = '\0'; }
    if (password) { strncpy(m_password, password, sizeof(m_password) - 1); m_password[sizeof(m_password) - 1] = '\0'; }
    m_port   = port ? port : 8883;
    m_caCert = (caCert && *caCert) ? caCert : nullptr;

    // Derive topics once.
    snprintf(m_topicPrefix, sizeof(m_topicPrefix), "ha/%s", m_deviceId);
    snprintf(m_statusTopic, sizeof(m_statusTopic), "%s/status", m_topicPrefix);
    snprintf(m_cmdFilter,   sizeof(m_cmdFilter),   "%s/relay/+/set", m_topicPrefix);
    snprintf(m_clientId,    sizeof(m_clientId),    "dev-%s", m_deviceId);

    m_configured = true;
    setState(MqttState::Configured);
    return MqttResult::Success;
}

void MqttManager::begin()
{
    if (!m_configured || m_began) return;
    m_began = true;
    s_instance = this;

    // TLS: pin the CA when provided, else skip validation for bring-up.
    if (m_caCert) m_net.setCACert(m_caCert);
    else          m_net.setInsecure();

    m_client.setServer(m_host, m_port);
    m_client.setBufferSize(MQTT_BUFFER_BYTES);
    m_client.setKeepAlive(30);
    m_client.setCallback(&MqttManager::messageTrampoline);

    m_lastAttemptMs = 0;           // connect on first loop()
    setState(MqttState::Connecting);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void MqttManager::loop()
{
    if (!m_began || !m_enabled) return;

    if (!isWiFiConnected())
    {
        if (m_state == MqttState::Connected)
        {
            setState(MqttState::Disconnected);
            fireEvent(MqttEvent::Disconnected);
        }
        return;   // nothing to do until WiFi returns
    }

    if (m_client.connected())
    {
        m_client.loop();
        return;
    }

    // Not connected: (re)connect with backoff.
    if (m_state == MqttState::Connected)
    {
        setState(MqttState::Disconnected);
        fireEvent(MqttEvent::Disconnected);
    }

    const uint32_t now = millis();
    if (m_lastAttemptMs != 0 && (now - m_lastAttemptMs) < m_retryDelayMs) return;
    attemptConnect(now);
}

void MqttManager::attemptConnect(uint32_t nowMs)
{
    m_lastAttemptMs = nowMs;
    setState(MqttState::Connecting);
    fireEvent(MqttEvent::Connecting);

    // Last-will: retained offline marker on the status topic.
    const bool ok = m_client.connect(
        m_clientId,
        m_username[0] ? m_username : nullptr,
        m_password[0] ? m_password : nullptr,
        m_statusTopic,                 // will topic
        1,                             // will QoS
        true,                          // will retained
        "{\"online\":false}");         // will payload

    if (!ok)
    {
        // Exponential backoff up to the cap.
        m_retryDelayMs = m_retryDelayMs < MQTT_RETRY_MAX_MS
                       ? m_retryDelayMs * 2 : MQTT_RETRY_MAX_MS;
        if (m_retryDelayMs > MQTT_RETRY_MAX_MS) m_retryDelayMs = MQTT_RETRY_MAX_MS;
        setState(MqttState::Error);
        fireEvent(MqttEvent::Error, MqttResult::NotConnected);
        return;
    }

    // Connected — reset backoff, subscribe, drain queue, announce.
    m_retryDelayMs = MQTT_RETRY_BASE_MS;
    m_client.subscribe(m_cmdFilter, 1);
    setState(MqttState::Connected);
    flushQueue();
    // The application republishes all relay states + status in this handler.
    fireEvent(MqttEvent::Connected);
}

// ---------------------------------------------------------------------------
// Incoming messages
// ---------------------------------------------------------------------------

void MqttManager::messageTrampoline(char* topic, uint8_t* payload, unsigned int len)
{
    if (s_instance) s_instance->handleMessage(topic, payload, len);
}

void MqttManager::handleMessage(const char* topic, const uint8_t* payload, unsigned int len)
{
    // Expect: ha/<deviceId>/relay/<n>/set
    const size_t prefixLen = strlen(m_topicPrefix);
    if (strncmp(topic, m_topicPrefix, prefixLen) != 0) return;

    const char* rest = topic + prefixLen;          // "/relay/<n>/set"
    if (strncmp(rest, "/relay/", 7) != 0) return;
    rest += 7;

    // Parse channel number up to the next '/'.
    uint8_t channel = 0;
    while (*rest >= '0' && *rest <= '9')
    {
        channel = channel * 10 + (uint8_t)(*rest - '0');
        ++rest;
    }
    if (strcmp(rest, "/set") != 0 || channel == 0) return;

    const bool on = (len > 0 && payload[0] == '1');
    fireEvent(MqttEvent::CommandReceived, MqttResult::Success, topic);
    if (m_commandCb) m_commandCb(channel, on, m_commandCtx);
}

// ---------------------------------------------------------------------------
// Publish
// ---------------------------------------------------------------------------

MqttResult MqttManager::publishRelayState(uint8_t channel, bool on)
{
    if (!m_enabled)    return MqttResult::Disabled;
    if (!m_configured) return MqttResult::NotConfigured;
    if (channel == 0)  return MqttResult::InvalidArg;

    char topic[MQTT_MAX_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/relay/%u", m_topicPrefix, (unsigned)channel);
    const char* payload = on ? "1" : "0";

    if (!m_client.connected()) return enqueue(topic, payload, true);
    return doPublish(topic, payload, true) ? MqttResult::Success : MqttResult::NotConnected;
}

MqttResult MqttManager::publishStatus(const char* firmwareVersion, int rssiDbm,
                                     uint32_t freeHeapBytes, uint32_t bootCount,
                                     uint32_t uptimeSeconds)
{
    if (!m_enabled)          return MqttResult::Disabled;
    if (!m_configured)       return MqttResult::NotConfigured;
    if (!m_client.connected()) return MqttResult::NotConnected;  // status not queued

    char payload[MQTT_MAX_PAYLOAD_LEN];
    snprintf(payload, sizeof(payload),
             "{\"online\":true,\"fw\":\"%s\",\"rssi\":%d,\"heap\":%lu,"
             "\"boot_count\":%lu,\"uptime_s\":%lu}",
             firmwareVersion ? firmwareVersion : "",
             rssiDbm,
             (unsigned long)freeHeapBytes,
             (unsigned long)bootCount,
             (unsigned long)uptimeSeconds);

    return doPublish(m_statusTopic, payload, true) ? MqttResult::Success : MqttResult::NotConnected;
}

bool MqttManager::doPublish(const char* topic, const char* payload, bool retained)
{
    const bool ok = m_client.publish(topic, (const uint8_t*)payload, strlen(payload), retained);
    if (!ok) fireEvent(MqttEvent::PublishFailed, MqttResult::NotConnected, topic);
    return ok;
}

// ---------------------------------------------------------------------------
// Offline queue
// ---------------------------------------------------------------------------

MqttResult MqttManager::enqueue(const char* topic, const char* payload, bool retained)
{
    if (m_qCount >= MQTT_MAX_QUEUE) return MqttResult::QueueFull;
    QueueEntry& e = m_queue[m_qTail];
    strncpy(e.topic, topic, sizeof(e.topic) - 1);     e.topic[sizeof(e.topic) - 1] = '\0';
    strncpy(e.payload, payload, sizeof(e.payload) - 1); e.payload[sizeof(e.payload) - 1] = '\0';
    e.retained = retained;
    m_qTail = (m_qTail + 1) % MQTT_MAX_QUEUE;
    ++m_qCount;
    return MqttResult::Queued;
}

void MqttManager::flushQueue()
{
    while (m_qCount > 0 && m_client.connected())
    {
        const QueueEntry& e = m_queue[m_qHead];
        if (!doPublish(e.topic, e.payload, e.retained)) break;  // retry later
        m_qHead = (m_qHead + 1) % MQTT_MAX_QUEUE;
        --m_qCount;
    }
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void MqttManager::enable()  { m_enabled = true; }
void MqttManager::disable() { m_enabled = false; if (m_client.connected()) m_client.disconnect(); setState(MqttState::Disabled); }
bool MqttManager::isEnabled() const { return m_enabled; }
bool MqttManager::isConnected() const { return const_cast<PubSubClient&>(m_client).connected(); }
MqttState MqttManager::getState() const { return m_state; }
int MqttManager::lastBrokerState() const { return const_cast<PubSubClient&>(m_client).state(); }

MqttResult MqttManager::reconnect()
{
    if (!m_began) return MqttResult::NotConfigured;
    m_retryDelayMs = MQTT_RETRY_BASE_MS;
    m_lastAttemptMs = 0;
    if (m_state == MqttState::Error || m_state == MqttState::Disconnected)
        setState(MqttState::Connecting);
    return MqttResult::Success;
}

void MqttManager::onEvent(EventCallback cb, void* context)   { m_eventCb = cb; m_eventCtx = context; }
void MqttManager::onCommand(CommandCallback cb, void* context) { m_commandCb = cb; m_commandCtx = context; }

void MqttManager::fireEvent(MqttEvent e, MqttResult r, const char* topic)
{
    if (m_eventCb) m_eventCb(e, r, topic ? topic : "", m_eventCtx);
}

void MqttManager::setState(MqttState s) { m_state = s; }

bool MqttManager::isWiFiConnected() { return WiFi.status() == WL_CONNECTED; }
