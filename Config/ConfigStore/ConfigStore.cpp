#include "ConfigStore.h"

#include <string.h>

static constexpr const char* NS = "hacfg";

void ConfigStore::begin()
{
    // Preferences is opened per-operation below to avoid holding the NVS
    // handle across the long-lived loop(); open once here to validate access.
    m_prefs.begin(NS, /*readOnly=*/false);
    m_prefs.end();
}

void ConfigStore::getStr(const char* key, char* out, size_t len) const
{
    m_prefs.begin(NS, true);
    String v = m_prefs.getString(key, "");
    m_prefs.end();
    strncpy(out, v.c_str(), len - 1);
    out[len - 1] = '\0';
}

// ---- WiFi --------------------------------------------------------------

bool ConfigStore::hasWifi() const
{
    char ssid[CFG_MAX];
    getStr("wifi.ssid", ssid, sizeof(ssid));
    return ssid[0] != '\0';
}

void ConfigStore::getWifiSsid(char* out, size_t len) const { getStr("wifi.ssid", out, len); }
void ConfigStore::getWifiPass(char* out, size_t len) const { getStr("wifi.pass", out, len); }

void ConfigStore::saveWifi(const char* ssid, const char* pass)
{
    m_prefs.begin(NS, false);
    m_prefs.putString("wifi.ssid", ssid);
    m_prefs.putString("wifi.pass", pass);
    m_prefs.end();
}

// ---- MQTT --------------------------------------------------------------

bool ConfigStore::hasMqtt() const
{
    char user[CFG_MAX];
    getStr("mqtt.user", user, sizeof(user));
    return user[0] != '\0';
}

void ConfigStore::getMqttHost(char* out, size_t len) const { getStr("mqtt.host", out, len); }
void ConfigStore::getMqttUser(char* out, size_t len) const { getStr("mqtt.user", out, len); }
void ConfigStore::getMqttPass(char* out, size_t len) const { getStr("mqtt.pass", out, len); }

uint16_t ConfigStore::getMqttPort() const
{
    m_prefs.begin(NS, true);
    uint16_t p = m_prefs.getUShort("mqtt.port", 8883);
    m_prefs.end();
    return p;
}

void ConfigStore::saveMqtt(const char* host, uint16_t port, const char* user, const char* pass)
{
    m_prefs.begin(NS, false);
    m_prefs.putString("mqtt.host", host);
    m_prefs.putUShort("mqtt.port", port);
    m_prefs.putString("mqtt.user", user);
    m_prefs.putString("mqtt.pass", pass);
    m_prefs.end();
}

// ---- factory reset -----------------------------------------------------

void ConfigStore::clear()
{
    m_prefs.begin(NS, false);
    m_prefs.clear();
    m_prefs.end();
}
