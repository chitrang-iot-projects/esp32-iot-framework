#pragma once

// SyncManager — the synchronization engine for the ESP32 IoT Framework.
//
// Responsibilities:
//   - Detect dirty runtime state via DeviceStateManager's dirty flags
//   - Queue pending synchronization items
//   - Write changed state to Firebase via FirebaseManager
//   - Clear dirty flags after successful synchronization
//   - Retry failed operations with exponential backoff
//   - Prevent duplicate sync items in the queue
//
// SyncManager owns:
//   synchronization queue, retry logic, dirty flag processing,
//   conflict prevention, synchronization statistics.
//
// SyncManager does NOT own:
//   runtime state, persistent storage, hardware control, WiFi, business logic.
//
// Module independence: SyncManager communicates ONLY with
//   DeviceStateManager and FirebaseManager.  It never calls any other manager.
//
// Synchronization flow:
//   RelayManager → DeviceStateManager (dirty flag set)
//                      ↓
//              SyncManager.loop()
//                      ↓ firebase.set()
//              FirebaseManager → Firebase
//                      ↓ (success)
//              clearRelayDirty()
//
// Typical usage:
//   SyncManager syncManager;
//   void setup() { syncManager.begin(deviceState, firebase); }
//   void loop()  { syncManager.loop(); }

#include <Arduino.h>
#include <stdint.h>
#include "../../Core/DeviceStateManager/DeviceStateManager.h"
#include "../../Cloud/FirebaseManager/FirebaseManager.h"

// ---------------------------------------------------------------------------
// SyncState
//
// High-level view of the synchronization engine lifecycle.
// ---------------------------------------------------------------------------
enum class SyncState : uint8_t
{
    Idle,        ///< Queue empty; no pending work.
    Processing,  ///< Actively executing a sync operation inside loop().
    Retrying,    ///< Items in queue waiting for exponential-backoff delay.
    Disabled,    ///< Manager disabled via disable(); no operations run.
    Error        ///< Unrecoverable fault; call begin() to reset.
};

// ---------------------------------------------------------------------------
// SyncResult
//
// Returned by every public synchronization operation.
// ---------------------------------------------------------------------------
enum class SyncResult : uint8_t
{
    Success,           ///< Operation completed or accepted by FirebaseManager.
    Busy,              ///< Engine is mid-operation; try again next loop().
    Cancelled,         ///< cancelSync() cleared the queue.
    NoChanges,         ///< No dirty flags set; nothing to synchronize.
    CloudUnavailable,  ///< Firebase not ready; items remain queued.
    QueueFull,         ///< Sync queue at capacity; item not accepted.
    Timeout,           ///< Operation did not complete within deadline (architecture reserved).
    Failed,            ///< Firebase write rejected; item scheduled for retry.
    Disabled,          ///< Manager is disabled.
    NotInitialized,    ///< begin() has not been called yet.
    InvalidState       ///< Queue item type is unrecognised.
};

// ---------------------------------------------------------------------------
// SyncDirection
//
// The direction of a synchronization item.
// Phase 1 uses LocalToCloud only; CloudToLocal is reserved for future stream
// change handling.
// ---------------------------------------------------------------------------
enum class SyncDirection : uint8_t
{
    LocalToCloud,  ///< Device state → Firebase (default).
    CloudToLocal   ///< Firebase change → Device state (future).
};

// ---------------------------------------------------------------------------
// SyncPriority
//
// Lower numeric value = higher scheduling priority.
// Relay and switch state carry High priority because they represent physical
// hardware; status/diagnostics carry Normal or Low.
// ---------------------------------------------------------------------------
enum class SyncPriority : uint8_t
{
    High   = 0,
    Normal = 1,
    Low    = 2
};

// ---------------------------------------------------------------------------
// SyncManager
// ---------------------------------------------------------------------------

class SyncManager
{
public:

    /// Capacity of the synchronisation queue (slots).
    /// 16 is sufficient for 5 categories + headroom for retries and syncAll().
    static constexpr uint8_t QUEUE_CAPACITY = 16u;

    /// Maximum length (including null terminator) of the optional base path
    /// prefix applied to every Firebase path this manager writes.
    static constexpr uint8_t MAX_BASE_PATH_LEN = 64u;

    // -----------------------------------------------------------------------
    // Construction
    //
    // Zero-initialises members.  No network access, no hardware, no side effects.
    // -----------------------------------------------------------------------
    SyncManager();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Binds dependencies and resets all sync state.
     *
     * Must be called once from setup() before loop() or any trigger function.
     * Safe to call again to fully reset the synchronization engine (clears
     * queue, counters, and state).
     *
     * @param deviceState Reference to the DeviceStateManager that holds dirty flags.
     * @param firebase    Reference to the FirebaseManager used for cloud writes.
     * @param basePath    Optional path prefix (e.g. "/devices/<id>") prepended to
     *                    every Firebase path this manager writes.  Keeps multiple
     *                    devices from colliding on the shared database root.
     *                    Pass nullptr (default) for the legacy root-level paths.
     *                    Must not end with '/'; truncated to MAX_BASE_PATH_LEN - 1.
     */
    void begin(DeviceStateManager& deviceState, FirebaseManager& firebase,
               const char* basePath = nullptr);

    /**
     * @brief Periodic update — drives the sync engine one step.
     *
     * Each call performs at most one sync queue operation so loop() always
     * returns quickly.  Also checks dirty flags and enqueues new items.
     *
     * Call from every Arduino loop() iteration.
     */
    void loop();

    /**
     * @brief Re-enables the manager after a disable() call.
     *
     * Restores the state to Idle (or Retrying if items remain in queue).
     */
    void enable();

    /**
     * @brief Suspends all sync operations.
     *
     * Items already in queue are preserved so they are processed when re-enabled.
     * New dirty-flag scanning is paused.
     */
    void disable();

    /**
     * @brief Returns whether the manager currently accepts operations.
     *
     * @return true when enabled, false when disabled.
     */
    bool isEnabled() const;

    // -----------------------------------------------------------------------
    // Synchronization triggers
    // -----------------------------------------------------------------------

    /**
     * @brief Scans dirty flags and enqueues any changed categories.
     *
     * Normally this happens automatically inside loop().  Call sync()
     * explicitly when an immediate sync is desired (e.g., after a critical
     * state change).
     *
     * @return NoChanges when no dirty flags are set, Success when items were
     *         queued, or the relevant error code.
     */
    SyncResult sync();

    /**
     * @brief Forces all state categories into the sync queue regardless of dirty flags.
     *
     * Use after device boot, WiFi reconnect, or Firebase reconnect to guarantee
     * the cloud has a complete and current copy of device state.
     *
     * @return Success when all categories were enqueued, QueueFull if overflow.
     */
    SyncResult syncAll();

    /**
     * @brief Clears the sync queue and stops in-progress operations.
     *
     * Dirty flags in DeviceStateManager are not cleared; the next loop()
     * call will automatically re-enqueue changed categories.
     *
     * @return Cancelled.
     */
    SyncResult cancelSync();

    /**
     * @brief Resets retry back-off on all queued items and re-scans dirty flags.
     *
     * Makes all currently queued items immediately eligible for processing so
     * they are tried on the next loop() tick rather than waiting for their
     * backoff delay to expire.
     *
     * @return Success, or the relevant error code.
     */
    SyncResult retrySync();

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the current synchronization engine state.
     *
     * @return Current SyncState.
     */
    SyncState getState() const;

    /**
     * @brief Returns the result of the most recent sync operation.
     *
     * @return Most recent SyncResult; NoChanges before any sync attempt.
     */
    SyncResult getLastResult() const;

    /**
     * @brief Returns true while items are pending in the queue.
     *
     * Note: processing is synchronous within loop(), so the caller sees the
     * post-loop state.  isBusy() returns true when the queue has entries
     * waiting to be sent or retried.
     *
     * @return true when the queue is non-empty.
     */
    bool isBusy() const;

    /**
     * @brief Returns true when the queue is empty and no sync work is pending.
     *
     * @return true when the queue is empty.
     */
    bool isIdle() const;

    // -----------------------------------------------------------------------
    // Statistics
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the number of items currently in the sync queue.
     * @return Pending item count.
     */
    uint32_t getPendingCount() const;

    /**
     * @brief Returns the total number of successfully completed sync operations.
     * @return Cumulative success count since begin().
     */
    uint32_t getSuccessfulSyncCount() const;

    /**
     * @brief Returns the total number of sync operations that exceeded max retries and were dropped.
     * @return Cumulative fail/drop count since begin().
     */
    uint32_t getFailedSyncCount() const;

    /**
     * @brief Returns the current queue fill level (alias for getPendingCount).
     * @return Current number of items in queue.
     */
    uint32_t getQueueSize() const;

    /**
     * @brief Returns the cumulative number of retry attempts made.
     * @return Total retries since begin().
     */
    uint32_t getRetryCount() const;

    /**
     * @brief Returns the number of sync items dropped after exceeding the retry limit.
     * @return Total dropped items since begin().
     */
    uint32_t getDroppedSyncCount() const;

    /**
     * @brief Returns the millis() timestamp of the last successful sync.
     *
     * Returns 0 if no successful sync has occurred since begin().
     *
     * @return Millisecond timestamp.
     */
    uint32_t getLastSyncTime() const;

private:

    // -----------------------------------------------------------------------
    // SyncItemType
    //
    // One-to-one mapping with DeviceStateManager's dirty-flag categories.
    // Keeping this internal prevents API surface sprawl.
    // -----------------------------------------------------------------------
    enum class SyncItemType : uint8_t
    {
        None,
        RelayAll,        ///< All relay states → /relays/<n>
        SwitchAll,       ///< All switch states → /switches/<n>
        WiFiStatus,      ///< WiFi connection state → /device/wifi_state
        FirebaseStatus,  ///< Firebase connection state → /device/firebase_state
        SystemStatus     ///< DeviceMode + online flag → /device/mode, /device/online
    };

    // -----------------------------------------------------------------------
    // SyncItem
    //
    // One slot in the fixed-size queue.  Holds all context needed to
    // process or retry a synchronization operation.
    // -----------------------------------------------------------------------
    struct SyncItem
    {
        SyncItemType  type;
        SyncDirection direction;
        SyncPriority  priority;
        uint8_t       retryCount;    ///< Number of failed attempts so far.
        uint32_t      enqueuedMs;    ///< millis() when this item entered the queue.
        uint32_t      nextAttemptMs; ///< millis() after which retry is allowed.
        bool          inUse;         ///< true = slot occupied.
    };

    // -----------------------------------------------------------------------
    // Dependencies (bound in begin())
    // -----------------------------------------------------------------------
    DeviceStateManager* m_deviceState;
    FirebaseManager*    m_firebase;

    /// Path prefix applied to every Firebase write; "" = legacy root paths.
    char m_basePath[MAX_BASE_PATH_LEN];

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    bool       m_initialized;
    bool       m_enabled;

    // -----------------------------------------------------------------------
    // Engine state
    // -----------------------------------------------------------------------
    SyncState  m_state;
    SyncResult m_lastResult;
    uint32_t   m_lastSyncMs;   ///< millis() of last successful sync.

    // -----------------------------------------------------------------------
    // Queue
    // -----------------------------------------------------------------------
    SyncItem m_queue[QUEUE_CAPACITY];
    uint8_t  m_queueCount;

    // -----------------------------------------------------------------------
    // Statistics
    // -----------------------------------------------------------------------
    uint32_t m_successCount;
    uint32_t m_failCount;
    uint32_t m_dropCount;
    uint32_t m_totalRetryCount;

    // -----------------------------------------------------------------------
    // Private helpers — validation
    // -----------------------------------------------------------------------

    /// Returns Success when initialized + enabled, otherwise the rejection code.
    SyncResult checkReady() const;

    // -----------------------------------------------------------------------
    // Private helpers — queue management
    // -----------------------------------------------------------------------

    /// Enqueue an item of the given type; skips if same type is already queued.
    SyncResult enqueueItem(SyncItemType type,
                           SyncPriority  priority,
                           SyncDirection direction = SyncDirection::LocalToCloud);

    /// Returns true if an item of the given type is already in the queue.
    bool isTypeQueued(SyncItemType type) const;

    /// Mark a queue slot as unused and decrement the count.
    void removeQueueItem(uint8_t index);

    /// Find the index of the highest-priority item whose nextAttemptMs <= now.
    /// Returns -1 if no item is ready.
    int8_t findNextReadyItem(uint32_t now) const;

    // -----------------------------------------------------------------------
    // Private helpers — dirty flag processing
    // -----------------------------------------------------------------------

    /// Scan DeviceStateManager dirty flags and enqueue any set categories.
    void enqueueDirtyFlags();

    /// Clear the DeviceStateManager dirty flag that corresponds to itemType.
    void clearDirtyFlagForType(SyncItemType type);

    // -----------------------------------------------------------------------
    // Private helpers — synchronization engine
    // -----------------------------------------------------------------------

    /// Process at most one ready queue item; called once per loop() tick.
    void processNextQueueItem(uint32_t now);

    /// Dispatch to the correct sync* function based on item type.
    SyncResult executeSyncItem(const SyncItem& item);

    /// Upload all relay states to Firebase (paths: /relays/<index>).
    SyncResult syncRelayAll();

    /// Upload all switch states to Firebase (paths: /switches/<index>).
    SyncResult syncSwitchAll();

    /// Upload the current WiFi state to Firebase (/device/wifi_state).
    SyncResult syncWiFiStatus();

    /// Upload the current Firebase state to Firebase (/device/firebase_state).
    SyncResult syncFirebaseStatus();

    /// Upload device mode and online flag to Firebase.
    SyncResult syncSystemStatus();

    // -----------------------------------------------------------------------
    // Private helpers — retry engine
    // -----------------------------------------------------------------------

    /// Compute the exponential-backoff delay in milliseconds for the given retry count.
    uint32_t calculateBackoff(uint8_t retryCount) const;

    // -----------------------------------------------------------------------
    // Private helpers — utilities
    // -----------------------------------------------------------------------

    /// Returns true when a FirebaseResult indicates the write was accepted.
    /// Both Success (sent immediately) and Queued (FirebaseManager holds it for
    /// delivery) are treated as successful outcomes — data is not lost.
    static bool isWriteAccepted(FirebaseResult result);
};
