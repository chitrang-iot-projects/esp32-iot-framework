#include "SyncManager.h"
#include <stdio.h>    // snprintf for path construction
#include <string.h>   // strncpy for base path storage
#include <stdint.h>

// =============================================================================
// File-local constants
//
// Kept here rather than in the header to avoid exposing internal path and
// timing choices in the public API.
// =============================================================================

// Firebase Realtime Database path prefixes and leaf paths.
static constexpr const char* RELAY_PATH_PREFIX   = "/relays/";
static constexpr const char* SWITCH_PATH_PREFIX  = "/switches/";
static constexpr const char* WIFI_STATE_PATH     = "/device/wifi_state";
static constexpr const char* FIREBASE_STATE_PATH = "/device/firebase_state";
static constexpr const char* DEVICE_MODE_PATH    = "/device/mode";
static constexpr const char* DEVICE_ONLINE_PATH  = "/device/online";

// Relay and switch array sizes mirror DeviceStateManager::MAX_RELAYS/MAX_SWITCHES.
static constexpr uint8_t RELAY_COUNT  = 16u;
static constexpr uint8_t SWITCH_COUNT = 16u;

// Retry engine parameters.
static constexpr uint8_t  MAX_RETRY_COUNT = 5u;       // attempts before item is dropped
static constexpr uint32_t RETRY_BASE_MS   = 2000UL;   // first backoff delay (ms)
static constexpr uint32_t RETRY_MAX_MS    = 60000UL;  // ceiling for exponential backoff

// Scratch buffer for building Firebase paths.
// Worst case: base path (63) + "/switches/15" (12) + null = 76 — 96 is safe.
static constexpr uint8_t PATH_BUF_SIZE = 96u;

// =============================================================================
// Construction
// =============================================================================

SyncManager::SyncManager()
    : m_deviceState(nullptr)
    , m_firebase(nullptr)
    , m_initialized(false)
    , m_enabled(true)
    , m_state(SyncState::Idle)
    , m_lastResult(SyncResult::NoChanges)
    , m_lastSyncMs(0u)
    , m_queueCount(0u)
    , m_successCount(0u)
    , m_failCount(0u)
    , m_dropCount(0u)
    , m_totalRetryCount(0u)
{
    // Arrays cannot use member-initialiser lists in C++11; zero the queue here
    // so the object is safe to inspect before begin() is called.
    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        m_queue[i].inUse = false;
    }
    m_basePath[0] = '\0';
}

// =============================================================================
// Lifecycle
// =============================================================================

void SyncManager::begin(DeviceStateManager& deviceState, FirebaseManager& firebase,
                        const char* basePath)
{
    m_deviceState = &deviceState;
    m_firebase    = &firebase;

    // Store the optional path prefix; nullptr keeps the legacy root-level paths.
    if (basePath != nullptr) {
        strncpy(m_basePath, basePath, MAX_BASE_PATH_LEN - 1);
        m_basePath[MAX_BASE_PATH_LEN - 1] = '\0';
    } else {
        m_basePath[0] = '\0';
    }

    // Clear the queue — any items from a previous begin() are stale.
    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        m_queue[i].inUse = false;
    }

    m_queueCount       = 0u;
    m_state            = SyncState::Idle;
    m_lastResult       = SyncResult::NoChanges;
    m_lastSyncMs       = 0u;
    m_successCount     = 0u;
    m_failCount        = 0u;
    m_dropCount        = 0u;
    m_totalRetryCount  = 0u;

    m_initialized = true;
    m_enabled     = true;
}

void SyncManager::loop()
{
    if (!m_initialized || !m_enabled) return;

    const uint32_t now = millis();

    // Scan DeviceStateManager dirty flags and enqueue any newly changed
    // categories.  This runs every loop() tick so no state change is missed,
    // even when the queue is otherwise empty.
    enqueueDirtyFlags();

    // Process exactly one ready queue item this tick.  Processing one item
    // per loop() keeps execution time bounded and the call stack shallow.
    processNextQueueItem(now);
}

void SyncManager::enable()
{
    m_enabled = true;

    // Restore an accurate state descriptor so callers see Retrying when items
    // are waiting rather than the stale Disabled value.
    if (m_initialized) {
        m_state = (m_queueCount > 0u) ? SyncState::Retrying : SyncState::Idle;
    }
}

void SyncManager::disable()
{
    m_enabled = false;
    m_state   = SyncState::Disabled;
}

bool SyncManager::isEnabled() const
{
    return m_enabled;
}

// =============================================================================
// Synchronization triggers
// =============================================================================

SyncResult SyncManager::sync()
{
    SyncResult check = checkReady();
    if (check != SyncResult::Success) return check;

    enqueueDirtyFlags();

    return (m_queueCount > 0u) ? SyncResult::Success : SyncResult::NoChanges;
}

SyncResult SyncManager::syncAll()
{
    SyncResult check = checkReady();
    if (check != SyncResult::Success) return check;

    // Force every category into the queue regardless of dirty flag state.
    // This guarantees the cloud receives a complete snapshot of current state,
    // which is the correct recovery action after reconnection or reboot.
    SyncResult r = SyncResult::Success;

    if (enqueueItem(SyncItemType::RelayAll,       SyncPriority::High)   == SyncResult::QueueFull) r = SyncResult::QueueFull;
    if (enqueueItem(SyncItemType::SwitchAll,      SyncPriority::High)   == SyncResult::QueueFull) r = SyncResult::QueueFull;
    if (enqueueItem(SyncItemType::WiFiStatus,     SyncPriority::Normal) == SyncResult::QueueFull) r = SyncResult::QueueFull;
    if (enqueueItem(SyncItemType::FirebaseStatus, SyncPriority::Normal) == SyncResult::QueueFull) r = SyncResult::QueueFull;
    if (enqueueItem(SyncItemType::SystemStatus,   SyncPriority::Normal) == SyncResult::QueueFull) r = SyncResult::QueueFull;

    return r;
}

SyncResult SyncManager::cancelSync()
{
    if (!m_initialized) return SyncResult::NotInitialized;

    // Evict all queued items.  Dirty flags in DeviceStateManager are left
    // intact so the next loop() tick re-enqueues them automatically — the
    // cancel only stops the current run, it does not discard pending changes.
    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        m_queue[i].inUse = false;
    }
    m_queueCount = 0u;
    m_state      = SyncState::Idle;
    m_lastResult = SyncResult::Cancelled;

    return SyncResult::Cancelled;
}

SyncResult SyncManager::retrySync()
{
    SyncResult check = checkReady();
    if (check != SyncResult::Success) return check;

    // Reset the backoff delay on every queued item so they become eligible
    // for immediate processing rather than waiting their remaining delay.
    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        if (m_queue[i].inUse) {
            m_queue[i].nextAttemptMs = 0u;
        }
    }

    // Also sweep dirty flags so any category that was previously dropped
    // (exceeded max retries) gets a fresh queue entry.
    enqueueDirtyFlags();

    if (m_queueCount > 0u) {
        m_state = SyncState::Retrying;
    }

    return SyncResult::Success;
}

// =============================================================================
// State queries
// =============================================================================

SyncState SyncManager::getState() const
{
    return m_state;
}

SyncResult SyncManager::getLastResult() const
{
    return m_lastResult;
}

bool SyncManager::isBusy() const
{
    // Return true whenever work is pending so callers can gate on readiness
    // without needing to inspect the full SyncState.
    return m_queueCount > 0u;
}

bool SyncManager::isIdle() const
{
    return m_queueCount == 0u;
}

// =============================================================================
// Statistics
// =============================================================================

uint32_t SyncManager::getPendingCount()        const { return m_queueCount;       }
uint32_t SyncManager::getSuccessfulSyncCount() const { return m_successCount;     }
uint32_t SyncManager::getFailedSyncCount()     const { return m_failCount;        }
uint32_t SyncManager::getQueueSize()           const { return m_queueCount;       }
uint32_t SyncManager::getRetryCount()          const { return m_totalRetryCount;  }
uint32_t SyncManager::getDroppedSyncCount()    const { return m_dropCount;        }
uint32_t SyncManager::getLastSyncTime()        const { return m_lastSyncMs;       }

// =============================================================================
// Private helpers — validation
// =============================================================================

SyncResult SyncManager::checkReady() const
{
    if (!m_initialized) return SyncResult::NotInitialized;
    if (!m_enabled)     return SyncResult::Disabled;
    return SyncResult::Success;
}

// =============================================================================
// Private helpers — queue management
// =============================================================================

SyncResult SyncManager::enqueueItem(SyncItemType  type,
                                     SyncPriority   priority,
                                     SyncDirection  direction)
{
    // Duplicate protection: only the newest value matters, but since we always
    // read the current value from DeviceStateManager at execution time, an
    // existing entry for this type will capture the latest state when processed.
    // Re-enqueueing would waste a queue slot for no benefit.
    if (isTypeQueued(type)) return SyncResult::Success;

    if (m_queueCount >= QUEUE_CAPACITY) return SyncResult::QueueFull;

    // Find the first unused slot.
    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        if (!m_queue[i].inUse) {
            m_queue[i].type           = type;
            m_queue[i].direction      = direction;
            m_queue[i].priority       = priority;
            m_queue[i].retryCount     = 0u;
            m_queue[i].enqueuedMs     = millis();
            m_queue[i].nextAttemptMs  = 0u;  // immediately eligible
            m_queue[i].inUse          = true;
            ++m_queueCount;
            return SyncResult::Success;
        }
    }

    // Should not reach here (guarded by the count check above), but be safe.
    return SyncResult::QueueFull;
}

bool SyncManager::isTypeQueued(SyncItemType type) const
{
    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        if (m_queue[i].inUse && m_queue[i].type == type) return true;
    }
    return false;
}

void SyncManager::removeQueueItem(uint8_t index)
{
    if (index >= QUEUE_CAPACITY) return;
    if (!m_queue[index].inUse)   return;

    m_queue[index].inUse = false;

    if (m_queueCount > 0u) {
        --m_queueCount;
    }
}

int8_t SyncManager::findNextReadyItem(uint32_t now) const
{
    // Scan for the highest-priority (lowest numeric value) item whose
    // backoff delay has elapsed.  Linear scan over a small fixed-size array
    // is O(QUEUE_CAPACITY) = O(16) — constant time in practice.
    int8_t  bestIdx      = -1;
    uint8_t bestPriority = 0xFFu;  // worst possible; replaced on first match

    for (uint8_t i = 0u; i < QUEUE_CAPACITY; ++i) {
        if (!m_queue[i].inUse) continue;

        // Skip items still in their exponential-backoff window.
        if (m_queue[i].nextAttemptMs > now) continue;

        const uint8_t p = static_cast<uint8_t>(m_queue[i].priority);
        if (p < bestPriority) {
            bestPriority = p;
            bestIdx      = static_cast<int8_t>(i);
        }
    }

    return bestIdx;
}

// =============================================================================
// Private helpers — dirty flag processing
// =============================================================================

void SyncManager::enqueueDirtyFlags()
{
    // Read per-category dirty flags from DeviceStateManager and enqueue an
    // item for each set category.  Duplicate detection inside enqueueItem()
    // ensures we never add the same category twice while it awaits processing.

    if (m_deviceState->isRelayDirty()) {
        enqueueItem(SyncItemType::RelayAll, SyncPriority::High);
    }
    if (m_deviceState->isSwitchDirty()) {
        enqueueItem(SyncItemType::SwitchAll, SyncPriority::High);
    }
    if (m_deviceState->isWiFiDirty()) {
        enqueueItem(SyncItemType::WiFiStatus, SyncPriority::Normal);
    }
    if (m_deviceState->isFirebaseDirty()) {
        enqueueItem(SyncItemType::FirebaseStatus, SyncPriority::Normal);
    }
    if (m_deviceState->isSystemDirty()) {
        enqueueItem(SyncItemType::SystemStatus, SyncPriority::Normal);
    }

    // Update the state descriptor to reflect whether work is now pending.
    if (m_state == SyncState::Idle && m_queueCount > 0u) {
        m_state = SyncState::Retrying;  // items awaiting their first attempt
    }
}

void SyncManager::clearDirtyFlagForType(SyncItemType type)
{
    // Dirty flags are cleared only AFTER a successful sync so that no state
    // change is silently discarded by a premature clear.
    switch (type) {
        case SyncItemType::RelayAll:       m_deviceState->clearRelayDirty();    break;
        case SyncItemType::SwitchAll:      m_deviceState->clearSwitchDirty();   break;
        case SyncItemType::WiFiStatus:     m_deviceState->clearWiFiDirty();     break;
        case SyncItemType::FirebaseStatus: m_deviceState->clearFirebaseDirty(); break;
        case SyncItemType::SystemStatus:   m_deviceState->clearSystemDirty();   break;
        default:                                                                  break;
    }
}

// =============================================================================
// Private helpers — synchronization engine
// =============================================================================

void SyncManager::processNextQueueItem(uint32_t now)
{
    if (m_queueCount == 0u) {
        m_state = SyncState::Idle;
        return;
    }

    const int8_t idx = findNextReadyItem(now);
    if (idx < 0) {
        // All items are in their backoff window; nothing to do this tick.
        m_state = SyncState::Retrying;
        return;
    }

    // Do not attempt the write when the cloud backend is not ready.
    // Items remain in the queue and will be retried automatically on the next
    // tick that finds Firebase ready — no explicit timer needed here because
    // we re-evaluate every loop() call.
    if (!m_firebase->isReady()) {
        m_lastResult = SyncResult::CloudUnavailable;
        return;
    }

    m_state = SyncState::Processing;

    const SyncResult result = executeSyncItem(m_queue[static_cast<uint8_t>(idx)]);

    if (result == SyncResult::Success) {
        // Clear the corresponding dirty flag only on confirmed success so the
        // dirty flag acts as an accurate "not yet synced" indicator.
        clearDirtyFlagForType(m_queue[static_cast<uint8_t>(idx)].type);
        removeQueueItem(static_cast<uint8_t>(idx));
        ++m_successCount;
        m_lastResult = SyncResult::Success;
        m_lastSyncMs = now;
        m_state      = (m_queueCount > 0u) ? SyncState::Retrying : SyncState::Idle;
    } else {
        ++m_queue[static_cast<uint8_t>(idx)].retryCount;
        ++m_totalRetryCount;

        if (m_queue[static_cast<uint8_t>(idx)].retryCount >= MAX_RETRY_COUNT) {
            // Give up — the item has failed too many times.  Remove it so the
            // queue doesn't stay permanently full and starve other categories.
            removeQueueItem(static_cast<uint8_t>(idx));
            ++m_failCount;
            ++m_dropCount;
            m_lastResult = SyncResult::Failed;
            m_state      = (m_queueCount > 0u) ? SyncState::Retrying : SyncState::Idle;
        } else {
            // Schedule the next retry with exponential backoff to avoid
            // flooding the cloud service during an outage.
            const uint32_t delay = calculateBackoff(m_queue[static_cast<uint8_t>(idx)].retryCount);
            m_queue[static_cast<uint8_t>(idx)].nextAttemptMs = now + delay;
            m_lastResult = result;
            m_state      = SyncState::Retrying;
        }
    }
}

SyncResult SyncManager::executeSyncItem(const SyncItem& item)
{
    switch (item.type) {
        case SyncItemType::RelayAll:       return syncRelayAll();
        case SyncItemType::SwitchAll:      return syncSwitchAll();
        case SyncItemType::WiFiStatus:     return syncWiFiStatus();
        case SyncItemType::FirebaseStatus: return syncFirebaseStatus();
        case SyncItemType::SystemStatus:   return syncSystemStatus();
        default:                            return SyncResult::InvalidState;
    }
}

SyncResult SyncManager::syncRelayAll()
{
    char path[PATH_BUF_SIZE];

    for (uint8_t i = 0u; i < RELAY_COUNT; ++i) {
        const RelayChannel channel = static_cast<RelayChannel>(i);
        const bool         isOn    = (m_deviceState->getRelayState(channel) == RelayState::On);

        // snprintf truncates safely if the base path pushes past PATH_BUF_SIZE.
        snprintf(path, sizeof(path), "%s%s%u",
                 m_basePath, RELAY_PATH_PREFIX, static_cast<unsigned>(i));

        if (!isWriteAccepted(m_firebase->set(path, isOn))) {
            return SyncResult::Failed;
        }
    }

    return SyncResult::Success;
}

SyncResult SyncManager::syncSwitchAll()
{
    char path[PATH_BUF_SIZE];

    for (uint8_t i = 0u; i < SWITCH_COUNT; ++i) {
        const SwitchChannel channel  = static_cast<SwitchChannel>(i);
        const bool          isPressed = (m_deviceState->getSwitchState(channel) == SwitchState::Pressed);

        snprintf(path, sizeof(path), "%s%s%u",
                 m_basePath, SWITCH_PATH_PREFIX, static_cast<unsigned>(i));

        if (!isWriteAccepted(m_firebase->set(path, isPressed))) {
            return SyncResult::Failed;
        }
    }

    return SyncResult::Success;
}

SyncResult SyncManager::syncWiFiStatus()
{
    char path[PATH_BUF_SIZE];
    snprintf(path, sizeof(path), "%s%s", m_basePath, WIFI_STATE_PATH);

    const int wifiVal = static_cast<int>(m_deviceState->getWiFiState());

    if (!isWriteAccepted(m_firebase->set(path, wifiVal))) {
        return SyncResult::Failed;
    }

    return SyncResult::Success;
}

SyncResult SyncManager::syncFirebaseStatus()
{
    char path[PATH_BUF_SIZE];
    snprintf(path, sizeof(path), "%s%s", m_basePath, FIREBASE_STATE_PATH);

    const int fbVal = static_cast<int>(m_deviceState->getFirebaseState());

    if (!isWriteAccepted(m_firebase->set(path, fbVal))) {
        return SyncResult::Failed;
    }

    return SyncResult::Success;
}

SyncResult SyncManager::syncSystemStatus()
{
    char path[PATH_BUF_SIZE];

    const int  modeVal   = static_cast<int>(m_deviceState->getDeviceMode());
    const bool onlineVal = m_deviceState->isOnline();

    snprintf(path, sizeof(path), "%s%s", m_basePath, DEVICE_MODE_PATH);
    if (!isWriteAccepted(m_firebase->set(path, modeVal))) {
        return SyncResult::Failed;
    }

    snprintf(path, sizeof(path), "%s%s", m_basePath, DEVICE_ONLINE_PATH);
    if (!isWriteAccepted(m_firebase->set(path, onlineVal))) {
        return SyncResult::Failed;
    }

    return SyncResult::Success;
}

// =============================================================================
// Private helpers — retry engine
// =============================================================================

uint32_t SyncManager::calculateBackoff(uint8_t retryCount) const
{
    // Exponential backoff: 2 s, 4 s, 8 s, 16 s, 32 s, capped at 60 s.
    // Using a loop instead of bitshift to avoid undefined behaviour on
    // shift amounts ≥ 32 with large retryCount values.
    uint32_t delay = RETRY_BASE_MS;

    for (uint8_t i = 0u; i < retryCount; ++i) {
        delay *= 2u;
        if (delay >= RETRY_MAX_MS) {
            delay = RETRY_MAX_MS;
            break;
        }
    }

    return delay;
}

// =============================================================================
// Private helpers — utilities
// =============================================================================

bool SyncManager::isWriteAccepted(FirebaseResult result)
{
    // FirebaseResult::Queued means FirebaseManager accepted the write into its
    // own offline queue and will deliver it when the connection is restored.
    // Treat this the same as Success — data is not lost, just deferred.
    return result == FirebaseResult::Success
        || result == FirebaseResult::Queued;
}
