#pragma once

// ConfigStore — persistent device configuration in NVS.
//
// Holds everything the board learns at provisioning time so it survives
// reboots and power cuts: home WiFi credentials (entered in the captive
// portal) and MQTT credentials (fetched from the platform's /api/provision).
//
// Generic firmware — no per-board values are compiled in. Every board runs
// the same binary and fills this store during setup.
//
// Backed directly by the Arduino Preferences library (NVS namespace "hacfg").

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>

static constexpr uint16_t CFG_MAX = 128;

class ConfigStore
{
public:
    void begin();     // open NVS namespace

    // ---- WiFi (set by captive portal) ----
    bool hasWifi() const;
    void getWifiSsid(char* out, size_t len) const;
    void getWifiPass(char* out, size_t len) const;
    void saveWifi(const char* ssid, const char* pass);

    // ---- MQTT (set by cloud provisioning) ----
    bool hasMqtt() const;
    void getMqttHost(char* out, size_t len) const;
    uint16_t getMqttPort() const;
    void getMqttUser(char* out, size_t len) const;
    void getMqttPass(char* out, size_t len) const;
    void saveMqtt(const char* host, uint16_t port, const char* user, const char* pass);

    // Clear only WiFi (keeps MQTT creds) — used when saved WiFi never connects,
    // so the board reopens setup without re-provisioning.
    void clearWifi();

    // ---- factory reset ----
    // Clears WiFi + MQTT config (device identity from MAC is not stored).
    void clear();

private:
    mutable Preferences m_prefs;
    void getStr(const char* key, char* out, size_t len) const;
};
