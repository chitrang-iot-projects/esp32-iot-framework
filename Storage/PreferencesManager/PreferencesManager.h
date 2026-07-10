#pragma once

// PreferencesManager — single owner of all persistent NVS storage on ESP32.
//
// Application code must never include <Preferences.h> or call NVS APIs
// directly.  Every stored value goes through this class so that the storage
// backend can be swapped (LittleFS, EEPROM, SD) without changing application
// code.
//
// Flash wear is limited automatically: every save() reads the current value
// before writing; if the value is unchanged the flash write is skipped.
//
// Lifecycle (mirrors every framework manager):
//   preferences.begin();      // call once in setup()
//   preferences.loop();       // call from main loop() — reserved for future work
//   preferences.enable();
//   preferences.disable();
//   preferences.isEnabled();
//
// Key convention — descriptive dot-separated strings, defined in a central
// PreferenceKey namespace owned by the application:
//   constexpr const char* WiFiSSID = "wifi.ssid";
//
// NVS physical keys are derived by hashing the application key with FNV-1a,
// so application-visible keys can be arbitrarily long while staying within
// the 15-character NVS key limit.

#include <Preferences.h>   // ESP32 NVS wrapper
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// PreferencesState
// ---------------------------------------------------------------------------

enum class PreferencesState : uint8_t
{
    NotInitialized, ///< begin() has not been called yet.
    Ready,          ///< Storage open and operational.
    Disabled,       ///< Suspended by the application via disable().
    Busy,           ///< Reserved for future async/background operations.
    Error           ///< Unrecoverable NVS failure; storage is unavailable.
};

// ---------------------------------------------------------------------------
// PreferencesResult
// ---------------------------------------------------------------------------

enum class PreferencesResult : uint8_t
{
    Success,      ///< Operation completed successfully.
    KeyNotFound,  ///< The requested key does not exist in storage.
    InvalidKey,   ///< Key is null, empty, whitespace-only, or contains forbidden patterns.
    InvalidValue, ///< Value pointer is null or the value cannot be stored.
    StorageFull,  ///< NVS partition has no space for a new entry.
    StorageError, ///< Low-level NVS read or write failure.
    Disabled,     ///< Manager is disabled; operation rejected.
    Busy,         ///< Manager is busy; try again later.
    UnknownError  ///< Unexpected internal failure.
};

// ---------------------------------------------------------------------------
// PreferencesManager
// ---------------------------------------------------------------------------

class PreferencesManager
{
public:

    // -----------------------------------------------------------------------
    // Construction
    //
    // Only initialises internal members.  No NVS access, no flash I/O.
    // Hardware is opened in begin() to follow the framework constructor rule.
    // -----------------------------------------------------------------------

    PreferencesManager();

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Opens the NVS namespace and prepares persistent storage.
     *
     * Must be called once from setup(), after which save() and load() are
     * available.  Writes the schema version key on first boot so future
     * firmware can detect and migrate old layouts automatically.
     *
     * @return Success, or StorageError if the NVS partition cannot be opened.
     */
    PreferencesResult begin();

    /**
     * @brief Periodic update hook — call from the main loop().
     *
     * Currently a no-op.  Reserved for future background operations such as
     * deferred writes, wear-level monitoring, and diagnostics.
     */
    void loop();

    /**
     * @brief Re-enables the manager after a disable() call.
     *
     * Transitions state from Disabled back to Ready.  Has no effect when the
     * manager is in Error or NotInitialized state.
     */
    void enable();

    /**
     * @brief Suspends all storage operations.
     *
     * Every save() / load() / exists() / remove() call returns Disabled until
     * enable() is called.  Does not close the NVS namespace or erase data.
     */
    void disable();

    /**
     * @brief Returns whether the manager is currently accepting operations.
     *
     * @return true when enabled, false when disabled.
     */
    bool isEnabled() const;

    // -----------------------------------------------------------------------
    // Save — one overload per supported type.
    //
    // Before writing, the current stored value is read and compared.  If the
    // value is identical the flash write is skipped and Success is returned.
    // This extends flash lifetime on devices that run continuously.
    // -----------------------------------------------------------------------

    /**
     * @brief Saves a boolean value to persistent storage.
     *
     * @param key   Descriptive dot-separated key (e.g. "relay.1.state").
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, bool value);

    /**
     * @brief Saves a signed 32-bit integer.
     *
     * On ESP32, int and long are both 32-bit; pass long values as int.
     *
     * @param key   Descriptive key string.
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, int value);

    /**
     * @brief Saves a long (signed 32-bit) integer.
     *
     * Delegates to save(key, int) — on ESP32, long and int are the same width.
     *
     * @param key   Descriptive key string.
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, long value);

    /**
     * @brief Saves an unsigned 32-bit integer.
     *
     * Overloads use the fundamental types (unsigned int, unsigned long) rather
     * than uint32_t: uint32_t aliases a DIFFERENT fundamental type across ESP32
     * core versions (unsigned int on 2.x, unsigned long on 3.x), and aliasing
     * would make two overloads collide as redefinitions.  uint32_t callers
     * resolve to whichever overload it aliases — both are provided.
     *
     * @param key   Descriptive key string.
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, unsigned int value);

    /**
     * @brief Saves an unsigned long (32-bit on ESP32) integer.
     *
     * Delegates to save(key, unsigned int) — same width on ESP32.
     *
     * @param key   Descriptive key string.
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, unsigned long value);

    /**
     * @brief Saves a single-precision float.
     *
     * @param key   Descriptive key string.
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, float value);

    /**
     * @brief Saves a double-precision float.
     *
     * @param key   Descriptive key string.
     * @param value Value to persist.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, double value);

    /**
     * @brief Saves a null-terminated string.
     *
     * @param key   Descriptive key string.
     * @param value Null-terminated string to persist.  Must not be nullptr.
     * @return PreferencesResult indicating the outcome.
     */
    PreferencesResult save(const char* key, const char* value);

    // -----------------------------------------------------------------------
    // Load — one overload per supported type.
    //
    // The output variable is left unchanged when the function returns anything
    // other than Success, so callers can safely pre-fill it with a default.
    // -----------------------------------------------------------------------

    /**
     * @brief Loads a boolean from storage.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, bool& value) const;

    /**
     * @brief Loads a signed 32-bit integer from storage.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, int& value) const;

    /**
     * @brief Loads a long (signed 32-bit) integer from storage.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, long& value) const;

    /**
     * @brief Loads an unsigned 32-bit integer from storage.
     *
     * See the save(unsigned int) note on why fundamental types are used
     * instead of uint32_t for these overloads.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, unsigned int& value) const;

    /**
     * @brief Loads an unsigned long (32-bit on ESP32) integer from storage.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, unsigned long& value) const;

    /**
     * @brief Loads a single-precision float from storage.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, float& value) const;

    /**
     * @brief Loads a double-precision float from storage.
     *
     * @param key   Key used when the value was saved.
     * @param value Output variable; unchanged on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    PreferencesResult load(const char* key, double& value) const;

    /**
     * @brief Loads a string from storage into a char array.
     *
     * The array size is deduced at compile time so the caller never passes an
     * explicit length argument.
     *
     * Example:
     * @code
     * char ssid[64];
     * preferences.load(PreferenceKey::WiFiSSID, ssid);
     * @endcode
     *
     * @tparam N   Buffer capacity including null terminator (deduced).
     * @param  key Key used when the value was saved.
     * @param  buf Destination buffer; set to "" on error or KeyNotFound.
     * @return KeyNotFound if absent, Success on success, or an error code.
     */
    template<size_t N>
    PreferencesResult load(const char* key, char (&buf)[N]) const
    {
        return loadString(key, buf, N);
    }

    // -----------------------------------------------------------------------
    // Load with default values
    //
    // Convenience overloads: if the key does not exist, value is set to
    // defaultValue and Success is returned.  Callers avoid an explicit
    // exists() check for optional settings.
    // -----------------------------------------------------------------------

    /**
     * @brief Loads a boolean, applying a default when the key is absent.
     *
     * @param key          Key to look up.
     * @param value        Output variable.
     * @param defaultValue Assigned to value when the key does not exist.
     * @return Success (key found or default applied), or an error code.
     */
    PreferencesResult load(const char* key, bool& value, bool defaultValue) const;

    /**
     * @brief Loads an integer, applying a default when the key is absent.
     *
     * @param key          Key to look up.
     * @param value        Output variable.
     * @param defaultValue Assigned to value when the key does not exist.
     * @return Success (key found or default applied), or an error code.
     */
    PreferencesResult load(const char* key, int& value, int defaultValue) const;

    /**
     * @brief Loads an unsigned int, applying a default when the key is absent.
     *
     * @param key          Key to look up.
     * @param value        Output variable.
     * @param defaultValue Assigned to value when the key does not exist.
     * @return Success (key found or default applied), or an error code.
     */
    PreferencesResult load(const char* key, unsigned int& value, unsigned int defaultValue) const;

    /**
     * @brief Loads an unsigned long, applying a default when the key is absent.
     *
     * @param key          Key to look up.
     * @param value        Output variable.
     * @param defaultValue Assigned to value when the key does not exist.
     * @return Success (key found or default applied), or an error code.
     */
    PreferencesResult load(const char* key, unsigned long& value, unsigned long defaultValue) const;

    /**
     * @brief Loads a float, applying a default when the key is absent.
     *
     * @param key          Key to look up.
     * @param value        Output variable.
     * @param defaultValue Assigned to value when the key does not exist.
     * @return Success (key found or default applied), or an error code.
     */
    PreferencesResult load(const char* key, float& value, float defaultValue) const;

    /**
     * @brief Loads a double, applying a default when the key is absent.
     *
     * @param key          Key to look up.
     * @param value        Output variable.
     * @param defaultValue Assigned to value when the key does not exist.
     * @return Success (key found or default applied), or an error code.
     */
    PreferencesResult load(const char* key, double& value, double defaultValue) const;

    // -----------------------------------------------------------------------
    // Key management
    // -----------------------------------------------------------------------

    /**
     * @brief Returns true if the key exists in persistent storage.
     *
     * Returns false for invalid keys, not-ready state, or absent keys.
     *
     * @param key Key string to check.
     * @return true if the key is present.
     */
    bool exists(const char* key) const;

    /**
     * @brief Removes a single key from persistent storage.
     *
     * @param key Key to remove.
     * @return KeyNotFound if absent, Success on removal, or an error code.
     */
    PreferencesResult remove(const char* key);

    /**
     * @brief Erases every value in the NVS namespace.
     *
     * The schema version key is immediately re-written after clearing so that
     * future migration logic can still identify the storage version.
     *
     * @return Success, or StorageError on NVS failure.
     */
    PreferencesResult clear();

    // -----------------------------------------------------------------------
    // Information
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the current operational state of the manager.
     *
     * @return PreferencesState value.
     */
    PreferencesState getState() const;

    /**
     * @brief Convenience check: true only when state is Ready.
     *
     * @return true when save/load operations will be accepted.
     */
    bool isReady() const;

    /**
     * @brief Returns the schema version stored in NVS.
     *
     * Written during begin() so future firmware upgrades can detect and
     * migrate older storage layouts without application involvement.
     *
     * @return Schema version (currently 1), or 0 if not yet initialised.
     */
    uint32_t getStorageVersion() const;

private:

    // -----------------------------------------------------------------------
    // Internal state
    // -----------------------------------------------------------------------

    mutable Preferences m_prefs;    ///< Underlying NVS handle.
    PreferencesState    m_state;    ///< Current operational state.
    bool                m_enabled;  ///< Application-level enable gate.

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Verifies the manager is initialised, not in error, and enabled.
     * @return Success when all conditions are met, otherwise the appropriate
     *         error code.
     */
    PreferencesResult checkReady() const;

    /**
     * @brief Validates a user-supplied key string.
     *
     * Rejects null, empty, whitespace-only keys, and path-traversal patterns
     * (".." and "///") that are not meaningful for flat NVS storage.
     *
     * @param key Candidate key.
     * @return true if the key is acceptable for NVS use.
     */
    bool isValidKey(const char* key) const;

    /**
     * @brief Underlying string load implementation used by the char[] template.
     *
     * @param key    Application key.
     * @param buf    Destination buffer; set to "" on any non-Success return.
     * @param maxLen Buffer capacity including the null terminator.
     * @return KeyNotFound, Success, or an error code.
     */
    PreferencesResult loadString(const char* key, char* buf, size_t maxLen) const;

    // NVS_KEY_SIZE, MAX_STRING_SIZE, and CURRENT_VERSION are defined as
    // file-local constants in PreferencesManager.cpp because they are
    // implementation details shared only with the static helper functions
    // in that translation unit.  Keeping them out of the header avoids
    // leaking NVS-specific sizing into the public interface.
};
