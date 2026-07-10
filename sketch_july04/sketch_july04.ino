/**
 * =============================================================================
 *  sketch_july04.ino  —  Full IoT Framework Integration (all 11 modules)
 * =============================================================================
 *  Successor of sketch_jun27a.  Same hardware, same pins, same Firebase paths,
 *  same user-visible behaviour — now driven through the complete framework:
 *
 *    Events\EventBus            — pub/sub backbone; decouples app glue code
 *    Logging\LoggerManager      — central logging (subscribes to all events)
 *    Scheduling\Scheduler       — boot-complete event + periodic status heartbeat
 *    Storage\PreferencesManager — relay state persistence + boot counter (NVS)
 *    Core\DeviceStateManager    — runtime state database (single source of truth)
 *    Synchronization\SyncManager— pushes device status to Firebase automatically
 *    Network\IoTWiFiManager     — WiFi lifecycle
 *    Cloud\FirebaseManager      — RTDB auth, read/write/stream, offline queue
 *    Device\RelayManager        — 4 active-low relay channels
 *    Device\StatusLedManager    — WS2812B status LED
 *    Input\InputManager         — 4 TTP223 capacitive touch switches
 *
 *  Hardware : B805 Main Hall — ESP32 board
 *  Device ID: PdMHOx6Pg6EIo9tmpMvV
 * -----------------------------------------------------------------------------
 *  RESPONSIBILITY SPLIT (who writes what to Firebase)
 *
 *    Application  → /devices/<id>/relays/relayN   (dashboard contract; written
 *                   on every physical toggle, exactly as sketch_jun27a did)
 *    SyncManager  → /devices/<id>/device/*        (wifi_state, firebase_state,
 *                   mode, online — device status telemetry)
 *
 *    Relay/switch dirty flags are cleared by the application right after it
 *    records them in DeviceStateManager: the app owns relay cloud sync through
 *    its proven per-relay path, so SyncManager must not also upload all 16
 *    relay slots (16 blocking HTTPS writes per toggle) to a second path scheme.
 * -----------------------------------------------------------------------------
 *  NEW BEHAVIOUR vs sketch_jun27a (additive only)
 *    - Relay states persist in NVS: after a power cut the relays restore
 *      instantly at boot, before WiFi.  The cloud snapshot on (re)connect
 *      remains authoritative, exactly as before.
 *    - Boot counter in NVS, logged at startup.
 *    - Framework events (WiFi/Firebase/relay/switch/sync) flow through
 *      EventBus and are logged automatically by LoggerManager.
 *    - Every 5 minutes a status heartbeat logs uptime / RSSI / free heap.
 * -----------------------------------------------------------------------------
 *  BUILD NOTE (Arduino IDE 2.x):
 *    Module .cpp files are included directly (single-translation-unit trick);
 *    Arduino IDE only compiles .cpp files inside the sketch folder.
 *
 *  Credentials live in sketch_july04/secrets.h (gitignored).
 *  Copy secrets.h.example → secrets.h and fill in values before flashing.
 * =============================================================================
 */

// ---------------------------------------------------------------------------
// Module headers  (relative to sketch_july04/)
// ---------------------------------------------------------------------------

#include "../Events/EventBus/EventBus.h"
#include "../Logging/LoggerManager/LoggerManager.h"
#include "../Scheduling/Scheduler/Scheduler.h"
#include "../Storage/PreferencesManager/PreferencesManager.h"
#include "../Network/IoTWiFiManager/IoTWiFiManager.h"
#include "../Cloud/FirebaseManager/FirebaseManager.h"
#include "../Core/DeviceStateManager/DeviceStateManager.h"
#include "../Synchronization/SyncManager/SyncManager.h"
#include "../Device/RelayManager/RelayManager.h"
#include "../Device/StatusLedManager/StatusLedManager.h"
#include "../Input/InputManager/InputManager.h"

#include <WiFi.h>           // for WiFi.setSleep()
#include <ArduinoJson.h>    // for JsonDocument
#include <stdarg.h>         // for the appLogf() helper

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
#include "../Cloud/FirebaseManager/FirebaseManager.cpp"
#include "../Core/DeviceStateManager/DeviceStateManager.cpp"
#include "../Synchronization/SyncManager/SyncManager.cpp"

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
    // Root of everything this device owns in the database.
    // SyncManager prefixes all of its status writes with this.
    constexpr const char* DeviceBase = "/devices/" DEVICE_ID;

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
// Persistent storage keys (PreferencesManager hashes them to NVS keys)
// ---------------------------------------------------------------------------

namespace PreferenceKey
{
    constexpr const char* RelayState[4] =
    {
        "relay.1.state",
        "relay.2.state",
        "relay.3.state",
        "relay.4.state",
    };

    constexpr const char* BootCount = "sys.bootCount";
}

// ---------------------------------------------------------------------------
// GPIO  —  matches B805 Main Hall physical wiring (identical to sketch_jun27a)
// ---------------------------------------------------------------------------

// WS2812B data line for the status LED.
static constexpr uint8_t STATUS_LED_PIN = 4;
static constexpr uint8_t LED_BRIGHTNESS = 50;

// Relay outputs (active-low: LOW = relay ON).
static constexpr uint8_t RELAY_PIN[4] = { 25, 33, 32, 27 };

// TTP223 capacitive touch inputs.
// WiFi.setSleep(false) stays mandatory: modem-sleep can cause spurious GPIO
// level changes while WiFi is active (ESP32 silicon errata).
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
// Scheduler intervals
// ---------------------------------------------------------------------------

// One-shot event marking the end of the boot phase (DeviceMode → Normal).
static constexpr uint32_t BOOT_COMPLETE_DELAY_MS = 2000UL;

// Periodic status heartbeat: logs uptime / RSSI / heap.
static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 300000UL;   // 5 minutes

// ---------------------------------------------------------------------------
// Module instances
// ---------------------------------------------------------------------------

EventBus           eventBus;
LoggerManager      logger;
Scheduler          scheduler;
PreferencesManager prefs;
DeviceStateManager deviceState;
IoTWiFiManager     wifiMgr(WIFI_SSID, WIFI_PASSWORD);
FirebaseManager    firebase;
SyncManager        syncManager;
RelayManager       relays;
StatusLedManager   led(STATUS_LED_PIN);
InputManager       inputs;

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

// Tracks WiFi state so loop() can react to transitions without repeating.
static WiFiState prevWifiState = WiFiState::Idle;

// ---------------------------------------------------------------------------
// EventBus payloads
//
// Copied inline by EventBus (max 64 bytes), so plain structs of scalars only.
// ---------------------------------------------------------------------------

struct RelayEventPayload
{
    uint8_t channel;   // RelayChannel value
    uint8_t state;     // RelayState value
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

void appLogf(LogLevel level, const char* format, ...);
void onRelayStateChanged(RelayChannel channel, RelayState newState, void* ctx);
void onInputPressed(InputChannel channel, void* ctx);
void onInputReleased(InputChannel channel, void* ctx);
void onFirebaseEvent(FirebaseEvent event, FirebaseResult result,
                     FirebaseDataType type, const char* path, void* ctx);
void onBootCompleted(const Event& event, void* ctx);
void onStatusHeartbeat(const Event& event, void* ctx);
void applyRelayStatesFromFirebase();
void restoreRelayStatesFromNvs();

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(300);

    // --- Framework core: EventBus first, then Logger so every event that
    //     follows is captured ----------------------------------------------
    eventBus.begin();
    logger.begin(eventBus);
    logger.info("[SETUP] IoT Framework full integration — sketch_july04");

    // --- Persistent storage ------------------------------------------------
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

    // --- Runtime state database (before anything writes into it) -----------
    deviceState.begin();

    // --- Status LED — started early so boot progress is visible ------------
    led.begin();
    led.setBrightness(LED_BRIGHTNESS);
    led.setState(StatusLedState::Booting);
    logger.info("[SETUP] LED ok");

    // --- Relays -------------------------------------------------------------
    relays.configureRelay(RelayChannel::Relay1, RELAY_PIN[0], RelayActiveState::ActiveLow, RELAY_DESC[0]);
    relays.configureRelay(RelayChannel::Relay2, RELAY_PIN[1], RelayActiveState::ActiveLow, RELAY_DESC[1]);
    relays.configureRelay(RelayChannel::Relay3, RELAY_PIN[2], RelayActiveState::ActiveLow, RELAY_DESC[2]);
    relays.configureRelay(RelayChannel::Relay4, RELAY_PIN[3], RelayActiveState::ActiveLow, RELAY_DESC[3]);
    relays.onStateChanged(onRelayStateChanged);
    relays.begin();

    // Restore the last known relay states from NVS so the room recovers
    // instantly after a power cut — no WiFi needed.  The cloud snapshot on
    // (re)connect remains authoritative and can still override these.
    restoreRelayStatesFromNvs();
    logger.info("[SETUP] Relays ok");

    // --- Touch inputs -------------------------------------------------------
    // onClick is a Phase 1 stub in InputManager — use onPressed: fires
    // immediately when the TTP223 output goes HIGH (touch detected), matching
    // the reference firmware's instant relay toggle on contact.
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
    inputs.onPressed(onInputPressed);
    inputs.onReleased(onInputReleased);
    inputs.begin();
    logger.info("[SETUP] Inputs ok");

    // --- Firebase (configure now; begin() deferred until WiFi is up) --------
    firebase.configure(FB_API_KEY, FB_DB_URL, FB_EMAIL, FB_PASSWORD);
    firebase.onEvent(onFirebaseEvent);
    logger.info("[SETUP] Firebase configured");

    // --- Synchronization ----------------------------------------------------
    // SyncManager owns device status telemetry under /devices/<id>/device/*.
    // Relay cloud sync stays with the application (see header note).
    syncManager.begin(deviceState, firebase, FirebasePath::DeviceBase);
    logger.info("[SETUP] SyncManager ok");

    // --- Scheduler + application event subscriptions ------------------------
    scheduler.begin(eventBus);
    scheduler.scheduleOnce(EventType::BootCompleted, BOOT_COMPLETE_DELAY_MS);
    // SyncStarted doubles as the periodic status-heartbeat tick.
    scheduler.scheduleRepeating(EventType::SyncStarted, HEARTBEAT_INTERVAL_MS);

    eventBus.subscribe(EventType::BootCompleted, onBootCompleted);
    eventBus.subscribe(EventType::SyncStarted,   onStatusHeartbeat);
    logger.info("[SETUP] Scheduler ok");

    // Heap snapshot after all modules initialised — Firebase TLS needs a large
    // contiguous block, so log both total free and the largest allocatable block.
    appLogf(LogLevel::Info, "[SETUP] heap: free=%lu  largestBlock=%lu",
            static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(ESP.getMaxAllocHeap()));

    // --- WiFi ---------------------------------------------------------------
    // Disable modem-sleep BEFORE begin(): modem-sleep causes spurious GPIO
    // edges (ESP32 errata) which would generate phantom switch events.
    WiFi.setSleep(false);
    wifiMgr.begin();
    prevWifiState = wifiMgr.getState();
    led.setState(StatusLedState::Idle);   // dark = waiting for WiFi
    logger.info("[SETUP] WiFi begin — waiting for connection...");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------

void loop()
{
    // Framework core first so queued events and timers fire promptly.
    eventBus.loop();
    logger.loop();
    scheduler.loop();
    prefs.loop();
    deviceState.loop();

    // Connectivity and hardware.
    wifiMgr.loop();
    inputs.loop();
    led.loop();
    relays.loop();

    // Start Firebase exactly once after WiFi connects.
    if (!fbStarted && wifiMgr.isConnected())
    {
        fbStarted = true;
        // Heap right before the first auth: the storm-protected init task needs
        // a 16 KB contiguous stack + TLS buffers.  If largestBlock is too small
        // the task cannot spawn and the connection retries until it errors out.
        appLogf(LogLevel::Info, "[LOOP] heap before Firebase: free=%lu  largestBlock=%lu",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        firebase.begin();
        // Subscribe to the relay parent; the stream delivers a full JSON
        // snapshot on (re)connect and key-level deltas on each change.
        firebase.subscribe(FirebasePath::RelaysBase);
        led.setState(StatusLedState::Busy);   // blinking = authenticating
        logger.info("[LOOP] WiFi up — Firebase started");
    }

    firebase.loop();
    syncManager.loop();

    // React to WiFi state transitions.
    const WiFiState currWifi = wifiMgr.getState();
    if (currWifi != prevWifiState)
    {
        prevWifiState = currWifi;
        deviceState.setWiFiState(currWifi);

        switch (currWifi)
        {
            case WiFiState::Connected:
                eventBus.publish(EventType::WiFiConnected, EventSource::Application);
                break;

            case WiFiState::Reconnecting:
                eventBus.publish(EventType::WiFiReconnecting, EventSource::Application);
                // Firebase's own Disconnected event handles the LED; this covers
                // the window before Firebase notices the WiFi loss.
                led.setState(StatusLedState::Warning);
                break;

            case WiFiState::Disconnected:
                eventBus.publish(EventType::WiFiDisconnected, EventSource::Application);
                led.setState(StatusLedState::Warning);
                break;

            default:
                break;
        }
    }

    // Composite online flag — duplicate writes are deduplicated internally,
    // and a genuine change marks the system category dirty so SyncManager
    // pushes /devices/<id>/device/online automatically.
    deviceState.setOnline(wifiMgr.isConnected() && firebase.isReady());

    // Sync relay states from Firebase.  Must happen in loop() context — not
    // inside the Firebase callback — because firebase.get() is a blocking call.
    if (needsFirebaseSync && firebase.isReady())
    {
        needsFirebaseSync = false;
        applyRelayStatesFromFirebase();
    }
}

// ---------------------------------------------------------------------------
// appLogf  —  printf-style helper on top of LoggerManager
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
// onRelayStateChanged  —  fires on every genuine relay transition
//
// Single place where a relay change fans out to the rest of the framework:
// runtime state, NVS persistence, and the event bus.  Works no matter who
// caused the change (touch switch, cloud sync, future scenes/scheduler).
// ---------------------------------------------------------------------------

void onRelayStateChanged(RelayChannel channel, RelayState newState, void* /*ctx*/)
{
    const uint8_t idx = static_cast<uint8_t>(channel);

    // Record in the runtime state database...
    deviceState.setRelayState(channel, newState);

    // ...but clear the relay dirty flag immediately: the application owns
    // relay cloud sync through its per-relay dashboard path.  Leaving the flag
    // set would make SyncManager upload all 16 relay slots on every toggle.
    deviceState.clearRelayDirty();

    // Persist so the relay recovers to this state after a power cut.
    // PreferencesManager skips the flash write when the value is unchanged.
    if (idx < 4)
    {
        prefs.save(PreferenceKey::RelayState[idx], newState == RelayState::On);
    }

    // Announce on the bus (LoggerManager logs it; future modules can react).
    const RelayEventPayload payload =
    {
        static_cast<uint8_t>(channel),
        static_cast<uint8_t>(newState)
    };
    eventBus.publish(EventType::RelayStateChanged, EventSource::Application,
                     &payload, sizeof(payload));
}

// ---------------------------------------------------------------------------
// onInputPressed  —  physical switch touched
//
// Toggles the matching relay immediately (works fully offline), then pushes
// the new state to Firebase.  If Firebase is offline, the manager queues the
// write and sends it automatically on reconnect.
// ---------------------------------------------------------------------------

void onInputPressed(InputChannel channel, void* /*ctx*/)
{
    const uint8_t idx     = static_cast<uint8_t>(channel);
    const auto    relayCh = static_cast<RelayChannel>(idx);

    // Record the switch state; cleared-dirty for the same reason as relays —
    // switch activity is transient input, not state the cloud needs mirrored.
    deviceState.setSwitchState(static_cast<SwitchChannel>(idx), SwitchState::Pressed);
    deviceState.clearSwitchDirty();
    eventBus.publish(EventType::SwitchPressed, EventSource::Application);

    relays.toggle(relayCh);   // onRelayStateChanged handles state + NVS + event
    const bool newState = relays.isOn(relayCh);

    appLogf(LogLevel::Info, "[INPUT] %s -> %s",
            RELAY_DESC[idx], newState ? "ON" : "OFF");

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
            appLogf(LogLevel::Debug, "[FB] relay%u sent immediately", idx + 1);
            break;

        case FirebaseResult::Queued:
            // Manager queued the write; QueueFlushed event clears pendingPush.
            appLogf(LogLevel::Info, "[FB] relay%u queued (offline)", idx + 1);
            break;

        default:
            // QueueFull or other error: pendingPush stays true so the cloud
            // stream cannot silently revert this relay's local state.
            appLogf(LogLevel::Warning,
                    "[FB] relay%u set failed (result=%d) — local state preserved",
                    idx + 1, static_cast<int>(r));
            break;
    }
}

// ---------------------------------------------------------------------------
// onInputReleased  —  touch ended; bookkeeping only
// ---------------------------------------------------------------------------

void onInputReleased(InputChannel channel, void* /*ctx*/)
{
    const uint8_t idx = static_cast<uint8_t>(channel);

    deviceState.setSwitchState(static_cast<SwitchChannel>(idx), SwitchState::Released);
    deviceState.clearSwitchDirty();
    eventBus.publish(EventType::SwitchReleased, EventSource::Application);
}

// ---------------------------------------------------------------------------
// onFirebaseEvent  —  single callback for all Firebase activity
//
// Updates the status LED, mirrors state into DeviceStateManager, and schedules
// relay syncs.  Kept short and non-blocking (no firebase.get() calls here).
// ---------------------------------------------------------------------------

void onFirebaseEvent(FirebaseEvent    event,
                     FirebaseResult   result,
                     FirebaseDataType /*type*/,
                     const char*      path,
                     void*            /*ctx*/)
{
    // Mirror the connection state into the runtime database.  A genuine change
    // marks the Firebase category dirty; SyncManager uploads it when Ready.
    deviceState.setFirebaseState(firebase.getState());

    switch (event)
    {
        // -- Connection progression ------------------------------------------

        case FirebaseEvent::Connected:
        case FirebaseEvent::Authenticated:
            led.setState(StatusLedState::Busy);
            break;

        case FirebaseEvent::Ready:
            led.setState(StatusLedState::Success);
            // Always do a full relay sync on every (re)connect so the device
            // matches the cloud state (NVS restore may be stale).
            needsFirebaseSync = true;
            eventBus.publish(EventType::FirebaseReady, EventSource::Application);
            break;

        case FirebaseEvent::RetryStarted:
            led.setState(StatusLedState::Warning);
            logger.info("[FB] Retrying...");
            break;

        case FirebaseEvent::RetryCompleted:
            logger.info("[FB] Retry succeeded");
            break;

        case FirebaseEvent::Disconnected:
            led.setState(StatusLedState::Warning);
            eventBus.publish(EventType::FirebaseDisconnected, EventSource::Application);
            break;

        case FirebaseEvent::Error:
        case FirebaseEvent::RetryLimitReached:
        {
            // A single failed operation (e.g. a write rejected by security
            // rules on a specific path) does NOT mean the session is dead —
            // firebase.isReady() tells us whether the connection itself is
            // still live.  Treating every per-operation error as fatal was
            // masking a healthy connection behind a permanent red LED while
            // reads/writes on other paths kept succeeding.  Only a genuinely
            // lost connection (isReady() false) is shown as Error; a live
            // session with one rejected operation is a Warning instead.
            const bool connectionLost = !firebase.isReady();
            led.setState(connectionLost ? StatusLedState::Error : StatusLedState::Warning);
            eventBus.publish(EventType::FirebaseError, EventSource::Application);
            appLogf(connectionLost ? LogLevel::Error : LogLevel::Warning,
                    "[FB] Error (result=%d) path=%s — %s",
                    static_cast<int>(result), path,
                    connectionLost ? "connection lost; call firebase.reconnect() to retry"
                                   : "operation rejected; connection still Ready (check Firebase rules for this path)");
            break;
        }

        // -- Data events -----------------------------------------------------

        case FirebaseEvent::StreamUpdated:
            // Data changed at the subscribed path; schedule a relay refresh.
            needsFirebaseSync = true;
            appLogf(LogLevel::Debug, "[FB] StreamUpdated  path=%s", path);
            break;

        case FirebaseEvent::WriteCompleted:
            // Clear the pending guard for this relay so cloud stream updates
            // are accepted again.
            for (uint8_t i = 0; i < 4; i++)
            {
                if (strcmp(path, FirebasePath::Relay[i]) == 0)
                {
                    pendingPush[i] = false;
                    appLogf(LogLevel::Debug, "[FB] WriteCompleted relay%u confirmed", i + 1);
                    break;
                }
            }
            break;

        case FirebaseEvent::ReadCompleted:
            appLogf(LogLevel::Debug, "[FB] ReadCompleted  path=%s", path);
            break;

        // -- Queue events ----------------------------------------------------

        case FirebaseEvent::QueueStarted:
            logger.info("[FB] Queue flush started");
            break;

        case FirebaseEvent::QueueFlushed:
            // All queued writes delivered — local and cloud states agree.
            for (uint8_t i = 0; i < 4; i++) { pendingPush[i] = false; }
            logger.info("[FB] QueueFlushed — all writes confirmed");
            break;

        case FirebaseEvent::QueueFull:
            appLogf(LogLevel::Warning, "[FB] Queue full — write dropped for path=%s", path);
            break;

        case FirebaseEvent::QueueOperationFailed:
            appLogf(LogLevel::Warning, "[FB] Queue op failed  path=%s  result=%d",
                    path, static_cast<int>(result));
            break;

        // -- Subscription events ---------------------------------------------

        case FirebaseEvent::SubscriptionAdded:
            appLogf(LogLevel::Info, "[FB] Subscribed  path=%s", path);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// onBootCompleted  —  scheduled once, 2 s after setup()
// ---------------------------------------------------------------------------

void onBootCompleted(const Event& /*event*/, void* /*ctx*/)
{
    // Booting → Normal marks the system category dirty; SyncManager uploads
    // /devices/<id>/device/mode once Firebase is Ready.
    deviceState.setDeviceMode(DeviceMode::Normal);
}

// ---------------------------------------------------------------------------
// onStatusHeartbeat  —  scheduled every 5 minutes (SyncStarted tick)
// ---------------------------------------------------------------------------

void onStatusHeartbeat(const Event& /*event*/, void* /*ctx*/)
{
    appLogf(LogLevel::Info,
            "[STATUS] uptime=%lus  wifi=%s  rssi=%d dBm  heap=%lu  syncs=%lu ok/%lu drop",
            static_cast<unsigned long>(deviceState.getUptime() / 1000UL),
            wifiMgr.isConnected() ? "up" : "down",
            static_cast<int>(wifiMgr.getRSSI()),
            static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(syncManager.getSuccessfulSyncCount()),
            static_cast<unsigned long>(syncManager.getDroppedSyncCount()));
}

// ---------------------------------------------------------------------------
// restoreRelayStatesFromNvs()
//
// Applies the last persisted relay states at boot, before any network is up.
// Missing keys default to Off (first boot behaves exactly like sketch_jun27a).
// ---------------------------------------------------------------------------

void restoreRelayStatesFromNvs()
{
    if (!prefs.isReady())
    {
        return;   // NVS failed in setup(); boot with all relays off
    }

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
        appLogf(LogLevel::Warning, "[SYNC] get failed (result=%d)", static_cast<int>(r));
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
            appLogf(LogLevel::Info, "[SYNC] relay%u -> %s", i + 1, state ? "ON" : "OFF");
        }
    }
}
