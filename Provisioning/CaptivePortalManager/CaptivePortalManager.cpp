#include "CaptivePortalManager.h"

#include <WiFi.h>

static constexpr uint8_t DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

CaptivePortalManager::CaptivePortalManager(ConfigStore& store)
    : m_store(store), m_server(80) {}

// Escape the characters that would otherwise terminate the attribute an SSID is
// placed in. Without this an SSID containing a quote or ampersand either breaks
// the page or silently arrives back truncated.
static String htmlEscape(const String& in)
{
    String out;
    out.reserve(in.length() + 8);
    for (unsigned int i = 0; i < in.length(); i++)
    {
        const char c = in[i];
        switch (c)
        {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += c;           break;   // UTF-8 bytes pass through
        }
    }
    return out;
}

void CaptivePortalManager::begin(const char* apSuffix)
{
    char ssid[40];
    snprintf(ssid, sizeof(ssid), "HA-SETUP-%s", apSuffix);

    WiFi.mode(WIFI_AP_STA);            // AP for the portal, STA so we can scan
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid);                 // open network for easy first setup

    // Catch-all DNS so any hostname the phone probes resolves to us → the
    // captive-portal popup appears automatically.
    m_dns.start(DNS_PORT, "*", AP_IP);

    m_server.on("/", [this]() { handleRoot(); });
    m_server.on("/save", HTTP_POST, [this]() { handleSave(); });
    m_server.onNotFound([this]() { handleNotFound(); });
    m_server.begin();

    m_active = true;
}

void CaptivePortalManager::loop()
{
    if (!m_active) return;
    m_dns.processNextRequest();
    m_server.handleClient();
}

void CaptivePortalManager::handleRoot()
{
    // Scan networks (synchronous — acceptable while in setup mode).
    const int n = WiFi.scanNetworks();

    String page = F(
        // charset MUST come first and MUST be declared: without it the phone
        // guesses a legacy encoding, and any SSID character it cannot represent
        // comes back from the form as an HTML numeric reference (a network named
        // with an emoji arrived as the literal text "&#128225;"), which is then
        // stored and can never match the real network.
        "<!doctype html><html><head><meta charset='utf-8'><meta name=viewport "
        "content='width=device-width,initial-scale=1'>"
        "<title>Device Setup</title><style>"
        "body{font-family:system-ui,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#111}"
        "h1{font-size:20px}label{display:block;margin:14px 0 4px;font-size:13px;color:#555}"
        "select,input{width:100%;padding:10px;font-size:16px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box}"
        "button{width:100%;margin-top:20px;padding:12px;font-size:16px;background:#2563eb;color:#fff;border:0;border-radius:8px}"
        "</style></head><body><h1>Connect your device</h1>"
        // accept-charset pins the submission encoding even if a browser ignores
        // the meta tag — belt and braces on the bug described above.
        "<form method=POST action=/save accept-charset='utf-8'>"
        "<label>WiFi network</label>"
        "<input name=ssid list=nets autocomplete=off placeholder='type or pick' required>"
        "<datalist id=nets>");

    for (int i = 0; i < n; i++)
    {
        page += "<option value='";
        page += htmlEscape(WiFi.SSID(i));
        page += "'>";
    }

    page += F(
        "</datalist>"
        "<label>WiFi password</label><input type=password name=pass>"
        "<button type=submit>Connect</button></form>"
        "<p style='color:#888;font-size:12px;margin-top:20px'>The device will "
        "restart and connect. It then appears in your app to be added.</p>"
        "</body></html>");

    m_server.send(200, "text/html", page);
}

void CaptivePortalManager::handleSave()
{
    const String ssid = m_server.arg("ssid");
    const String pass = m_server.arg("pass");

    if (ssid.isEmpty())
    {
        m_server.send(400, "text/html", "<p>WiFi network is required. Go back and pick one.</p>");
        return;
    }

    m_store.saveWifi(ssid.c_str(), pass.c_str());
    m_server.send(200, "text/html",
        "<!doctype html><html><body style='font-family:system-ui;max-width:420px;"
        "margin:40px auto;padding:0 16px'><h2>Saved.</h2><p>The device is "
        "restarting and connecting to your WiFi. You can close this page and "
        "add it in your app.</p></body></html>");

    delay(800);        // let the response flush before reboot
    ESP.restart();
}

void CaptivePortalManager::handleNotFound()
{
    // Redirect every unknown request to the portal root — triggers the OS
    // captive-portal detection so the setup page opens on its own.
    m_server.sendHeader("Location", "http://192.168.4.1/", true);
    m_server.send(302, "text/plain", "");
}
