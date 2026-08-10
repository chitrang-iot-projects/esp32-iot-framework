#include "PreferencesManager.h"

#include <string.h>   // strcmp, strstr, strlen, strncpy
#include <ctype.h>    // isspace
#include <stdio.h>    // snprintf

// ---------------------------------------------------------------------------
// NVS configuration
// ---------------------------------------------------------------------------

/// Single NVS namespace shared by all application keys.
/// Keeping one namespace lets future backends (LittleFS, EEPROM) use a single
/// flat keyspace with no coordination complexity.
static constexpr const char* NVS_NAMESPACE = "pref";

/// Internal NVS key for the schema version record.
/// Starts with '_' so it cannot collide with the 8-char lowercase-hex keys
/// that are produced by hashing application keys.
static constexpr const char* NVS_VERSION_KEY = "_ver";

/// NVS physical key length: 8 lowercase hex digits + null terminator.
static constexpr size_t NVS_KEY_SIZE = 9u;

/// Stack buffer size for string duplicate-write detection.
/// Covers all typical IoT settings (SSIDs, passwords, URLs, tokens).
static constexpr size_t MAX_STRING_SIZE = 256u;

/// Schema version written to NVS on first boot for future migration support.
static constexpr uint32_t CURRENT_VERSION = 1u;

// ---------------------------------------------------------------------------
// Key hashing
//
// ESP32 NVS enforces a 15-character key limit.  Application keys like
// "mqtt.brokerHostname" exceed that limit, so every key is hashed with
// FNV-1a-32 and formatted as 8 lowercase hex digits (9 bytes with null).
// FNV-1a is deterministic, collision-resistant, and requires no heap.
// ---------------------------------------------------------------------------

static uint32_t fnv1a32(const char* data)
{
    uint32_t hash = 2166136261u;
    while (*data)
    {
        hash ^= static_cast<uint8_t>(*data++);
        hash *= 16777619u;
    }
    return hash;
}

/// Writes exactly 8 lowercase hex chars plus a null into nvsKey.
/// The caller must provide a buffer of at least NVS_KEY_SIZE bytes.
static void toNvsKey(const char* appKey, char* nvsKey)
{
    snprintf(nvsKey, NVS_KEY_SIZE,
             "%08x", static_cast<unsigned int>(fnv1a32(appKey)));
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

PreferencesManager::PreferencesManager()
    : m_state(PreferencesState::NotInitialized)
    , m_enabled(true)
{
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

PreferencesResult PreferencesManager::begin()
{
    // Read-write mode (false) so we can write the version key if absent.
    if (!m_prefs.begin(NVS_NAMESPACE, false))
    {
        m_state = PreferencesState::Error;
        return PreferencesResult::StorageError;
    }

    // Persist schema version on first boot so future firmware can detect
    // and migrate old NVS layouts without requiring application changes.
    if (!m_prefs.isKey(NVS_VERSION_KEY))
    {
        m_prefs.putUInt(NVS_VERSION_KEY, CURRENT_VERSION);
    }

    m_state = PreferencesState::Ready;
    return PreferencesResult::Success;
}

void PreferencesManager::loop()
{
    // Reserved for future background operations: deferred writes,
    // wear-level monitoring, storage diagnostics.
}

void PreferencesManager::enable()
{
    // Only transition from Disabled; leave Error / NotInitialized unchanged
    // so the application cannot silently mask a broken storage partition.
    if (m_state == PreferencesState::Disabled)
    {
        m_state = PreferencesState::Ready;
    }
    m_enabled = true;
}

void PreferencesManager::disable()
{
    m_enabled = false;
    if (m_state == PreferencesState::Ready)
    {
        m_state = PreferencesState::Disabled;
    }
}

bool PreferencesManager::isEnabled() const
{
    return m_enabled;
}

// ---------------------------------------------------------------------------
// Save overloads
// ---------------------------------------------------------------------------

PreferencesResult PreferencesManager::save(const char* key, bool value)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    // Skip the flash write when the stored value is already identical.
    if (m_prefs.isKey(nvsKey) && m_prefs.getBool(nvsKey) == value)
    {
        return PreferencesResult::Success;
    }

    if (!m_prefs.putBool(nvsKey, value))
    {
        return PreferencesResult::StorageError;
    }
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::save(const char* key, int value)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (m_prefs.isKey(nvsKey) && m_prefs.getInt(nvsKey) == value)
    {
        return PreferencesResult::Success;
    }

    if (m_prefs.putInt(nvsKey, value) == 0)
    {
        return PreferencesResult::StorageError;
    }
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::save(const char* key, long value)
{
    // On ESP32 (32-bit), long and int are the same width; reuse int storage.
    return save(key, static_cast<int>(value));
}

PreferencesResult PreferencesManager::save(const char* key, unsigned int value)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (m_prefs.isKey(nvsKey) && m_prefs.getUInt(nvsKey) == value)
    {
        return PreferencesResult::Success;
    }

    if (m_prefs.putUInt(nvsKey, value) == 0)
    {
        return PreferencesResult::StorageError;
    }
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::save(const char* key, unsigned long value)
{
    // On ESP32 (32-bit), unsigned long and unsigned int are the same width.
    return save(key, static_cast<unsigned int>(value));
}

PreferencesResult PreferencesManager::save(const char* key, float value)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    // Exact bit-pattern comparison is intentional: we are detecting a
    // duplicate write, not evaluating mathematical equality.
    if (m_prefs.isKey(nvsKey) && m_prefs.getFloat(nvsKey) == value)
    {
        return PreferencesResult::Success;
    }

    if (m_prefs.putFloat(nvsKey, value) == 0)
    {
        return PreferencesResult::StorageError;
    }
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::save(const char* key, double value)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (m_prefs.isKey(nvsKey) && m_prefs.getDouble(nvsKey) == value)
    {
        return PreferencesResult::Success;
    }

    if (m_prefs.putDouble(nvsKey, value) == 0)
    {
        return PreferencesResult::StorageError;
    }
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::save(const char* key, const char* value)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key))    return PreferencesResult::InvalidKey;
    if (value == nullptr)    return PreferencesResult::InvalidValue;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    // Read the existing string into a stack buffer and compare before writing.
    // This avoids flash wear for frequently polled settings.
    // Strings longer than MAX_STRING_SIZE - 1 chars fall through without dedup
    // protection; the write still succeeds.
    if (m_prefs.isKey(nvsKey))
    {
        char existing[MAX_STRING_SIZE];
        existing[0] = '\0';
        m_prefs.getString(nvsKey, existing, sizeof(existing));
        existing[sizeof(existing) - 1] = '\0'; // guard against a truncated read

        if (strcmp(existing, value) == 0)
        {
            return PreferencesResult::Success;
        }
    }

    const size_t len = strlen(value);
    const size_t written = m_prefs.putString(nvsKey, value);

    // putString returns strlen(value) on success; 0 on NVS error.
    // An empty string ("") has strlen 0, so we only treat 0 as an error for
    // non-empty values.
    if (written == 0 && len > 0)
    {
        return PreferencesResult::StorageError;
    }
    return PreferencesResult::Success;
}

// ---------------------------------------------------------------------------
// Load overloads
// ---------------------------------------------------------------------------

PreferencesResult PreferencesManager::load(const char* key, bool& value) const
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    value = m_prefs.getBool(nvsKey);
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::load(const char* key, int& value) const
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    value = m_prefs.getInt(nvsKey);
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::load(const char* key, long& value) const
{
    int temp = 0;
    PreferencesResult result = load(key, temp);
    if (result == PreferencesResult::Success)
    {
        value = static_cast<long>(temp);
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, unsigned int& value) const
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    value = m_prefs.getUInt(nvsKey);
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::load(const char* key, unsigned long& value) const
{
    unsigned int temp = 0u;
    PreferencesResult result = load(key, temp);
    if (result == PreferencesResult::Success)
    {
        value = static_cast<unsigned long>(temp);
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, float& value) const
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    value = m_prefs.getFloat(nvsKey);
    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::load(const char* key, double& value) const
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    value = m_prefs.getDouble(nvsKey);
    return PreferencesResult::Success;
}

// ---------------------------------------------------------------------------
// Load with default values
// ---------------------------------------------------------------------------

PreferencesResult PreferencesManager::load(const char* key, bool& value, bool defaultValue) const
{
    PreferencesResult result = load(key, value);
    if (result == PreferencesResult::KeyNotFound)
    {
        value = defaultValue;
        return PreferencesResult::Success;
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, int& value, int defaultValue) const
{
    PreferencesResult result = load(key, value);
    if (result == PreferencesResult::KeyNotFound)
    {
        value = defaultValue;
        return PreferencesResult::Success;
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, unsigned int& value, unsigned int defaultValue) const
{
    PreferencesResult result = load(key, value);
    if (result == PreferencesResult::KeyNotFound)
    {
        value = defaultValue;
        return PreferencesResult::Success;
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, unsigned long& value, unsigned long defaultValue) const
{
    PreferencesResult result = load(key, value);
    if (result == PreferencesResult::KeyNotFound)
    {
        value = defaultValue;
        return PreferencesResult::Success;
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, float& value, float defaultValue) const
{
    PreferencesResult result = load(key, value);
    if (result == PreferencesResult::KeyNotFound)
    {
        value = defaultValue;
        return PreferencesResult::Success;
    }
    return result;
}

PreferencesResult PreferencesManager::load(const char* key, double& value, double defaultValue) const
{
    PreferencesResult result = load(key, value);
    if (result == PreferencesResult::KeyNotFound)
    {
        value = defaultValue;
        return PreferencesResult::Success;
    }
    return result;
}

// ---------------------------------------------------------------------------
// String load — called by the char-array template in the header
// ---------------------------------------------------------------------------

PreferencesResult PreferencesManager::loadString(const char* key, char* buf, size_t maxLen) const
{
    if (buf == nullptr || maxLen == 0) return PreferencesResult::InvalidValue;

    buf[0] = '\0'; // guarantee a valid empty string on any non-Success return

    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    m_prefs.getString(nvsKey, buf, maxLen);
    buf[maxLen - 1] = '\0'; // defensive null in case the library truncates without terminating

    return PreferencesResult::Success;
}

// ---------------------------------------------------------------------------
// Key management
// ---------------------------------------------------------------------------

bool PreferencesManager::exists(const char* key) const
{
    if (m_state != PreferencesState::Ready) return false;
    if (!isValidKey(key)) return false;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    return m_prefs.isKey(nvsKey);
}

PreferencesResult PreferencesManager::remove(const char* key)
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;
    if (!isValidKey(key)) return PreferencesResult::InvalidKey;

    char nvsKey[NVS_KEY_SIZE];
    toNvsKey(key, nvsKey);

    if (!m_prefs.isKey(nvsKey)) return PreferencesResult::KeyNotFound;

    if (!m_prefs.remove(nvsKey)) return PreferencesResult::StorageError;

    return PreferencesResult::Success;
}

PreferencesResult PreferencesManager::clear()
{
    PreferencesResult check = checkReady();
    if (check != PreferencesResult::Success) return check;

    if (!m_prefs.clear()) return PreferencesResult::StorageError;

    // Restore the version key so future firmware upgrades can still identify
    // the schema version even after a factory-reset-style clear.
    m_prefs.putUInt(NVS_VERSION_KEY, CURRENT_VERSION);

    return PreferencesResult::Success;
}

// ---------------------------------------------------------------------------
// Information
// ---------------------------------------------------------------------------

PreferencesState PreferencesManager::getState() const
{
    return m_state;
}

bool PreferencesManager::isReady() const
{
    return m_state == PreferencesState::Ready;
}

uint32_t PreferencesManager::getStorageVersion() const
{
    if (m_state == PreferencesState::NotInitialized) return 0u;
    return m_prefs.getUInt(NVS_VERSION_KEY, 0u);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

PreferencesResult PreferencesManager::checkReady() const
{
    if (m_state == PreferencesState::NotInitialized) return PreferencesResult::StorageError;
    if (m_state == PreferencesState::Error)          return PreferencesResult::StorageError;
    if (m_state == PreferencesState::Busy)           return PreferencesResult::Busy;

    // Check enabled after structural state so Error always returns StorageError,
    // not Disabled, regardless of whether the app also called disable().
    if (!m_enabled || m_state == PreferencesState::Disabled) return PreferencesResult::Disabled;

    return PreferencesResult::Success;
}

bool PreferencesManager::isValidKey(const char* key) const
{
    if (key == nullptr || key[0] == '\0') return false;

    // Reject keys composed entirely of whitespace.
    bool hasNonWhitespace = false;
    for (const char* p = key; *p != '\0'; ++p)
    {
        if (!isspace(static_cast<unsigned char>(*p)))
        {
            hasNonWhitespace = true;
            break;
        }
    }
    if (!hasNonWhitespace) return false;

    // Reject path-traversal patterns that have no place in a flat key-value store.
    if (strstr(key, "..") != nullptr)  return false;
    if (strstr(key, "///") != nullptr) return false;

    return true;
}
