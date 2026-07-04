/**
 * =============================================================================
 *  sketch_jun27a.ino  —  IoT Framework Integration Test
 * =============================================================================
 *  Modules under test:
 *    Network\IoTWiFiManager    — WiFi lifecycle
 *    Cloud\FirebaseManager     — RTDB auth, read/write/stream, offline queue
 *    Device\RelayManager       — 4 active-low relay channels
 *    Device\StatusLedManager   — WS2812B status LED
 *    Input\InputManager        — 4 TTP223 capacitive touch switches
 *
 *  Hardware : B805 Main Hall — ESP32 board
 *  Device ID: PdMHOx6Pg6EIo9tmpMvV
 * -----------------------------------------------------------------------------
 *  BUILD NOTE (Arduino IDE 2.x):
 *    This sketch includes the module .cpp files directly via relative path.
 *    Arduino IDE only compiles .cpp files inside the sketch folder; the
 *    explicit #include "../Module/Module.cpp" lines below force the compiler
 *    to process each implementation file as part of this translation unit.
 *    No extra steps needed — just open and upload.
 *
 *    PlatformIO alternative (cleaner for production):
 *      lib_extra_dirs = ..
 *      build_flags    = -std=c++17
 * -----------------------------------------------------------------------------
 *  Credentials live in sketch_jun27a/secrets.h (gitignored).
 *  Copy secrets.h.example → secrets.h and fill in values before flashing.
 * =============================================================================
 */

// ---------------------------------------------------------------------------
// Module headers  (relative to sketch_jun27a/)
// ---------------------------------------------------------------------------

#include "../Network/IoTWiFiManager/IoTWiFiManager.h"
#include "../Cloud/FirebaseManager/FirebaseManager.h"
#include "../Device/RelayManager/RelayManager.h"
#include "../Device/StatusLedManager/StatusLedManager.h"
#include "../Input/InputManager/InputManager.h"
#include <WiFi.h>           // for WiFi.setSleep()
#include <ArduinoJson.h>    // for JsonDocument (transitively included via FirebaseManager.h)

// ---------------------------------------------------------------------------
// Module implementations
//
// Arduino IDE only compiles .cpp files that live inside the sketch folder.
// Including the .cpp files here forces the compiler to process them as part
// of this translation unit so the linker can resolve all symbols.
// Each .cpp includes its own .h; #pragma once prevents double inclusion.
// ---------------------------------------------------------------------------

#include "../Network/IoTWiFiManager/IoTWiFiManager.cpp"
#include "../Device/RelayManager/RelayManager.cpp"
#include "../Device/StatusLedManager/StatusLedManager.cpp"
#include "../Input/InputManager/InputManager.cpp"
#include "../Cloud/FirebaseManager/FirebaseManager.cpp"

// ---------------------------------------------------------------------------
// Credentials — loaded from secrets.h (gitignored, never committed)
// ---------------------------------------------------------------------------

#include "secrets.h"

static constexpr const char* WIFI_SSID     = SECRET_WIFI_SSID;
static constexpr const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
static constexpr const char* FB_API_KEY    = SECRET_FB_API_KEY;
static constexpr const char* FB_DB_URL     = SECRET_FB_DB_URL;
static constexpr const char* FB_EMAIL      = SECRET_FB_EMAIL;
static constexpr const char* FB_PASSWORD   = SECRET_FB_PASSWORD;

// ---------------------------------------------------------------------------
// Device identity & Firebase database paths
// ---------------------------------------------------------------------------

#define DEVICE_ID "PdMHOx6Pg6EIo9tmpMvV"

namespace FirebasePath
{
    // Parent node — subscribed for realtime stream (full JSON on connect,
    // per-key deltas on each dashboard change).
    constexpr const char* RelaysBase = "/devices/" DEVICE_ID "/relays";

    // Individual relay paths — written on every physical switch click.
    constexpr const char* Relay[4] =
    {
        "/devices/" DEVICE_ID "/relays/relay1",
        "/devices/" DEVICE_ID "/relays/relay2",
        "/devices/" DEVICE_ID "/relays/relay3",
        "/devices/" DEVICE_ID "/relays/relay4",
    };
}

// ---------------------------------------------------------------------------
// GPIO  —  matches B805 Main Hall physical wiring
// ---------------------------------------------------------------------------

// WS2812B data line for the status LED.
static constexpr uint8_t STATUS_LED_PIN = 4;
static constexpr uint8_t LED_BRIGHTNESS = 50;

// Relay outputs (active-low: LOW = relay ON).
static constexpr uint8_t RELAY_PIN[4] = { 25, 33, 32, 27 };

// TTP223 capacitive touch inputs.
// GPIO 36 & 39 are input-only on ESP32: no internal pull resistors.
// WiFi.setSleep(false) is mandatory: modem-sleep causes spurious level
// changes on these pins (ESP32 silicon errata).
static constexpr uint8_t SWITCH_PIN[4] = { 19, 18, 17, 16 };

// Relay / switch descriptions (index matches channel number).
static constexpr const char* RELAY_DESC[4] =
{
    "Main Light",
    "Light",
    "Inner Entrance",
    "Entrance Light",
};

// ---------------------------------------------------------------------------
// Module instances
// ---------------------------------------------------------------------------

IoTWiFiManager   wifiMgr(WIFI_SSID, WIFI_PASSWORD);
RelayManager     relays;
StatusLedManager led(STATUS_LED_PIN);
InputManager     inputs;
FirebaseManager  firebase;

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

// pendingPush[i]: true while a local relay toggle has NOT been confirmed by
// Firebase.  Prevents a delayed cloud echo from silently reverting a physical
// switch press.  Cleared by WriteCompleted (immediate send) or QueueFlushed
// (offline send confirmed after reconnect).
static bool pendingPush[4] = {};

// Signals loop() to pull the current relay state from Firebase.
// Set by the Firebase callback; consumed in loop() context because
// firebase.get() makes a blocking HTTPS call that must not run inside a
// callback (which executes on the firebase.loop() call stack).
static bool needsFirebaseSync = false;

// Ensures firebase.begin() is called exactly once, after WiFi connects.
static bool fbStarted = false;

// Tracks WiFi state so loop() can log transitions without repeating.
static WiFiState prevWifiState = WiFiState::Idle;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

void onInputClick(InputChannel channel, void* ctx);
void onFirebaseEvent(FirebaseEvent event, FirebaseResult result,
                     FirebaseDataType type, const char* path, void* ctx);
void applyRelayStatesFromFirebase();

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[SETUP] IoT Framework integration test");

    // --- Status LED — started first so boot progress is visible -------------
    led.begin();
    led.setBrightness(LED_BRIGHTNESS);
    led.setState(StatusLedState::Booting);
    Serial.println("[SETUP] LED ok");

    // --- Relays (all off at boot; cloud snapshot restores state on connect) -
    relays.configureRelay(RelayChannel::Relay1, RELAY_PIN[0], RelayActiveState::ActiveLow, RELAY_DESC[0]);
    relays.configureRelay(RelayChannel::Relay2, RELAY_PIN[1], RelayActiveState::ActiveLow, RELAY_DESC[1]);
    relays.configureRelay(RelayChannel::Relay3, RELAY_PIN[2], RelayActiveState::ActiveLow, RELAY_DESC[2]);
    relays.configureRelay(RelayChannel::Relay4, RELAY_PIN[3], RelayActiveState::ActiveLow, RELAY_DESC[3]);
    relays.begin();
    Serial.println("[SETUP] Relays ok");

    // --- Touch inputs -------------------------------------------------------
    // Floating mode: GPIO 36/39 are input-only; no pull resistors attempted.
    // onClick fires after a full touch-and-release cycle (debounced).
    inputs.configureInput(InputChannel::Input1, SWITCH_PIN[0],
                          InputType::TTP223, InputMode::Floating,
                          InputActiveState::ActiveHigh, "SW-MainLight");
    inputs.configureInput(InputChannel::Input2, SWITCH_PIN[1],
                          InputType::TTP223, InputMode::Floating,
                          InputActiveState::ActiveHigh, "SW-Light");
    inputs.configureInput(InputChannel::Input3, SWITCH_PIN[2],
                          InputType::TTP223, InputMode::Floating,
                          InputActiveState::ActiveHigh, "SW-InnerEntrance");
    inputs.configureInput(InputChannel::Input4, SWITCH_PIN[3],
                          InputType::TTP223, InputMode::Floating,
                          InputActiveState::ActiveHigh, "SW-Entrance");
    // onClick is a Phase 1 stub in InputManager — m_clickCb is stored but
    // tickInput() never fires it.  Use onPressed: fires immediately when the
    // TTP223 output goes HIGH (touch detected), which matches the reference
    // firmware's intent of instant relay toggle on contact.
    inputs.onPressed(onInputClick);
    inputs.begin();
    Serial.println("[SETUP] Inputs ok");

    // --- Firebase (configure now; begin() deferred until WiFi is up) --------
    firebase.configure(FB_API_KEY, FB_DB_URL, FB_EMAIL, FB_PASSWORD);
    firebase.onEvent(onFirebaseEvent);
    Serial.println("[SETUP] Firebase configured");

    // --- WiFi ---------------------------------------------------------------
    // Disable modem-sleep BEFORE begin(): modem-sleep causes spurious edges on
    // GPIO 36/39 (ESP32 errata) which would generate phantom switch events.
    WiFi.setSleep(false);
    wifiMgr.begin();
    prevWifiState = wifiMgr.getState();
    led.setState(StatusLedState::Idle);   // pulsing = waiting for WiFi
    Serial.println("[SETUP] WiFi begin — waiting for connection...");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------

void loop()
{
    wifiMgr.loop();
    inputs.loop();
    led.loop();

    // Start Firebase exactly once after WiFi connects.
    if (!fbStarted && wifiMgr.isConnected())
    {
        fbStarted = true;
        firebase.begin();
        // Subscribe to the relay parent; the stream delivers a full JSON
        // snapshot on (re)connect and key-level deltas on each change.
        firebase.subscribe(FirebasePath::RelaysBase);
        led.setState(StatusLedState::Busy);   // blinking = authenticating
        Serial.println("[LOOP] WiFi up — Firebase started");
    }

    firebase.loop();
    relays.loop();

    // Log WiFi state transitions.
    const WiFiState currWifi = wifiMgr.getState();
    if (currWifi != prevWifiState)
    {
        Serial.printf("[WIFI] t=%lu  state %d -> %d\n",
                      millis(), (int)prevWifiState, (int)currWifi);
        prevWifiState = currWifi;

        if (currWifi == WiFiState::Disconnected || currWifi == WiFiState::Reconnecting)
        {
            // Firebase's own Disconnected event handles the LED; this covers
            // the window before Firebase notices the WiFi loss.
            led.setState(StatusLedState::Warning);
        }
    }

    // Sync relay states from Firebase.  Must happen in loop() context — not
    // inside the Firebase callback — because firebase.get() is a blocking call.
    if (needsFirebaseSync && firebase.isReady())
    {
        needsFirebaseSync = false;
        applyRelayStatesFromFirebase();
    }
}

// ---------------------------------------------------------------------------
// onInputClick  —  physical switch clicked
//
// Toggles the matching relay immediately (works fully offline), then pushes
// the new state to Firebase.  If Firebase is offline, the manager queues the
// write and sends it automatically on reconnect.
// ---------------------------------------------------------------------------

void onInputClick(InputChannel channel, void* /*ctx*/)
{
    const uint8_t idx     = static_cast<uint8_t>(channel);
    const auto    relayCh = static_cast<RelayChannel>(idx);

    relays.toggle(relayCh);
    const bool newState = relays.isOn(relayCh);

    Serial.printf("[INPUT] t=%lu  %s -> %s\n",
                  millis(), RELAY_DESC[idx], newState ? "ON" : "OFF");

    // Mark local state as unconfirmed before the set() call.
    // If set() is immediate (Ready state), the WriteCompleted callback fires
    // inside set() and clears pendingPush before set() returns — so the flag
    // must be raised first, not after.
    pendingPush[idx] = true;

    const FirebaseResult r = firebase.set(FirebasePath::Relay[idx], newState);

    switch (r)
    {
        case FirebaseResult::Success:
            // WriteCompleted callback already fired inside set(); pendingPush cleared.
            Serial.printf("[FB]    relay%u sent immediately\n", idx + 1);
            break;

        case FirebaseResult::Queued:
            // Manager queued the write; QueueFlushed event clears pendingPush.
            Serial.printf("[FB]    relay%u queued (offline)\n", idx + 1);
            break;

        default:
            // QueueFull or other error: pendingPush stays true so the cloud
            // stream cannot silently revert this relay's local state.
            Serial.printf("[FB]    relay%u set failed (result=%d) — local state preserved\n",
                          idx + 1, (int)r);
            break;
    }
}

// ---------------------------------------------------------------------------
// onFirebaseEvent  —  single callback for all Firebase activity
//
// Updates the status LED and schedules relay syncs.
// Keeps this callback short and non-blocking (no firebase.get() calls here).
// ---------------------------------------------------------------------------

void onFirebaseEvent(FirebaseEvent    event,
                     FirebaseResult   result,
                     FirebaseDataType /*type*/,
                     const char*      path,
                     void*            /*ctx*/)
{
    switch (event)
    {
        // -- Connection progression ------------------------------------------

        case FirebaseEvent::Connected:
        case FirebaseEvent::Authenticated:
            led.setState(StatusLedState::Busy);
            Serial.println("[FB] Authenticating...");
            break;

        case FirebaseEvent::Ready:
            led.setState(StatusLedState::Success);
            // Always do a full relay sync on every (re)connect so the device
            // starts in the correct cloud state (not the boot default of all-off).
            needsFirebaseSync = true;
            Serial.println("[FB] Ready");
            break;

        case FirebaseEvent::RetryStarted:
            led.setState(StatusLedState::Warning);
            Serial.println("[FB] Retrying...");
            break;

        case FirebaseEvent::RetryCompleted:
            Serial.println("[FB] Retry succeeded");
            break;

        case FirebaseEvent::Disconnected:
            led.setState(StatusLedState::Warning);
            Serial.println("[FB] Disconnected — auto-recovery pending");
            break;

        case FirebaseEvent::Error:
        case FirebaseEvent::RetryLimitReached:
            led.setState(StatusLedState::Error);
            Serial.printf("[FB] Error (result=%d) — call firebase.reconnect() to retry\n",
                          (int)result);
            break;

        // -- Data events -----------------------------------------------------

        case FirebaseEvent::StreamUpdated:
            // Data changed at the subscribed path; schedule a relay refresh.
            needsFirebaseSync = true;
            Serial.printf("[FB] StreamUpdated  path=%s\n", path);
            break;

        case FirebaseEvent::WriteCompleted:
            // Clear the pending guard for this relay so cloud stream updates
            // are accepted again.
            for (uint8_t i = 0; i < 4; i++)
            {
                if (strcmp(path, FirebasePath::Relay[i]) == 0)
                {
                    pendingPush[i] = false;
                    Serial.printf("[FB] WriteCompleted relay%u confirmed\n", i + 1);
                    break;
                }
            }
            break;

        case FirebaseEvent::ReadCompleted:
            Serial.printf("[FB] ReadCompleted  path=%s\n", path);
            break;

        // -- Queue events ----------------------------------------------------

        case FirebaseEvent::QueueStarted:
            Serial.println("[FB] Queue flush started");
            break;

        case FirebaseEvent::QueueFlushed:
            // All queued writes delivered — local and cloud states agree.
            for (uint8_t i = 0; i < 4; i++) { pendingPush[i] = false; }
            Serial.println("[FB] QueueFlushed — all writes confirmed");
            break;

        case FirebaseEvent::QueueFull:
            Serial.printf("[FB] Queue full — relay write dropped for path=%s\n", path);
            break;

        case FirebaseEvent::QueueOperationFailed:
            Serial.printf("[FB] Queue op failed  path=%s  result=%d\n", path, (int)result);
            break;

        // -- Subscription events ---------------------------------------------

        case FirebaseEvent::SubscriptionAdded:
            Serial.printf("[FB] Subscribed  path=%s\n", path);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// applyRelayStatesFromFirebase()
//
// Reads the full relay JSON object from Firebase and applies each value to
// RelayManager, skipping any relay whose local state hasn't been confirmed yet.
//
// Called from loop() — never from inside a callback — because firebase.get()
// makes a synchronous HTTPS request that blocks for ~100–500 ms.
// ---------------------------------------------------------------------------

void applyRelayStatesFromFirebase()
{
    static constexpr const char* RELAY_KEYS[4] =
        { "relay1", "relay2", "relay3", "relay4" };

    JsonDocument doc;
    const FirebaseResult r = firebase.get(FirebasePath::RelaysBase, doc);
    if (r != FirebaseResult::Success)
    {
        Serial.printf("[SYNC] get failed (result=%d)\n", (int)r);
        return;
    }

    for (uint8_t i = 0; i < 4; i++)
    {
        // Skip if a local toggle is still in-flight to Firebase: the local
        // state is the source of truth until Firebase confirms the write.
        if (pendingPush[i]) { continue; }

        if (doc[RELAY_KEYS[i]].is<bool>())
        {
            const bool state   = doc[RELAY_KEYS[i]].as<bool>();
            const auto relayCh = static_cast<RelayChannel>(i);
            relays.setState(relayCh, state ? RelayState::On : RelayState::Off);
            Serial.printf("[SYNC] relay%u -> %s\n", i + 1, state ? "ON" : "OFF");
        }
    }
}
