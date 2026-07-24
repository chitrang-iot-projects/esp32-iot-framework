#pragma once

// CaptivePortalManager — first-boot WiFi setup, "like a new router".
//
// When the board has no WiFi configured it starts its own access point
// (HA-SETUP-<deviceId>) and serves a captive portal at 192.168.4.1. The
// installer connects with a phone, the OS auto-opens the page, they pick their
// home WiFi and enter the password. Credentials are saved to ConfigStore and
// the board reboots to connect.
//
// Only WiFi is collected here — MQTT credentials are fetched automatically
// from the platform after the board is online (see the sketch's cloud
// provisioning step). Keeps the installer experience to just "pick WiFi".
//
// Uses the ESP32 core libraries WiFi + DNSServer + WebServer.

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "../../Config/ConfigStore/ConfigStore.h"

class CaptivePortalManager
{
public:
    explicit CaptivePortalManager(ConfigStore& store);

    // Start SoftAP + DNS + HTTP server. apSuffix is the device id shown in the
    // SSID (HA-SETUP-<apSuffix>).
    void begin(const char* apSuffix);

    // Call every loop(); services DNS + HTTP. Never blocks.
    void loop();

    bool isActive() const { return m_active; }

private:
    ConfigStore& m_store;
    DNSServer    m_dns;
    WebServer    m_server;
    bool         m_active = false;

    void handleRoot();
    void handleSave();
    void handleNotFound();
};
