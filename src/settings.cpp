#include "settings.h"
#include "utils/logger.h"

#include <coreinit/mutex.h>
#include <wups/storage.h>

#include <atomic>
#include <string_view>

namespace {
    OSMutex sWriterMutex;
    std::atomic<bool> sMutexReady{false};

    // Input-path visible state
    std::atomic<bool> sEnabled{Settings::DEFAULT_ENABLED};
    ParamSnapshot sSnapshot;

    // Writer-side state (guarded by sWriterMutex)
    Settings::Values sValues = {
            Settings::DEFAULT_ENABLED,
            Settings::DEFAULT_DEADZONE_PCT,
            Settings::DEFAULT_OUTER_PCT,
            Settings::DEFAULT_SENSITIVITY_PCT,
            Settings::DEFAULT_OFFSET_ENABLED,
            Settings::DEFAULT_OFFSET,
            Settings::DEFAULT_OFFSET,
            0,
    };

    // Set by the toggle combo callback; consumed by FlushPendingSave().
    std::atomic<bool> sEnabledDirty{false};

    class WriterLock {
    public:
        WriterLock() {
            if (sMutexReady.load(std::memory_order_acquire)) {
                OSLockMutex(&sWriterMutex);
                mLocked = true;
            }
        }
        ~WriterLock() {
            if (mLocked) OSUnlockMutex(&sWriterMutex);
        }
        WriterLock(const WriterLock &)            = delete;
        WriterLock &operator=(const WriterLock &) = delete;

    private:
        bool mLocked = false;
    };

    int32_t clampInt(int32_t v, int32_t lo, int32_t hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Must be called with the writer lock held.
    void publishLocked() {
        DriftParams p;
        p.deadzone      = (float) sValues.deadzonePct / 100.0f;
        p.outer         = (float) sValues.outerPct / 100.0f;
        p.sensitivity   = (float) sValues.sensitivityPct / 100.0f;
        p.offsetX       = (float) sValues.offsetX / 100.0f;
        p.offsetY       = (float) sValues.offsetY / 100.0f;
        p.offsetEnabled = sValues.offsetEnabled ? 1 : 0;
        drift_params_sanitize(&p);
        sSnapshot.publish(p);
    }

    template<typename T>
    void storeLogged(const char *key, const T &value) {
        WUPSStorageError err = WUPSStorageAPI::Store(key, value);
        if (err != WUPS_STORAGE_ERROR_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("Failed to store %s: %s (%d)", key, WUPSStorageAPI_GetStatusStr(err), err);
        }
    }

    void saveLogged(const char *reason) {
        WUPSStorageError err = WUPSStorageAPI::SaveStorage();
        if (err != WUPS_STORAGE_ERROR_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("SaveStorage (%s) failed: %s (%d)", reason, WUPSStorageAPI_GetStatusStr(err), err);
        }
    }

    template<typename T>
    void loadOrDefault(const char *key, T &value, const T &def) {
        WUPSStorageError err = WUPSStorageAPI::GetOrStoreDefault(key, value, def);
        if (err != WUPS_STORAGE_ERROR_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("GetOrStoreDefault(%s) failed: %s (%d)", key, WUPSStorageAPI_GetStatusStr(err), err);
            value = def;
        }
    }
} // namespace

namespace Settings {
    void Init(uint32_t defaultToggleCombo) {
        OSInitMutex(&sWriterMutex);
        sMutexReady.store(true, std::memory_order_release);

        WriterLock lock;
        Values v = sValues;
        loadOrDefault(KEY_ENABLED, v.enabled, DEFAULT_ENABLED);
        loadOrDefault(KEY_DEADZONE, v.deadzonePct, DEFAULT_DEADZONE_PCT);
        loadOrDefault(KEY_OUTER, v.outerPct, DEFAULT_OUTER_PCT);
        loadOrDefault(KEY_SENSITIVITY, v.sensitivityPct, DEFAULT_SENSITIVITY_PCT);
        loadOrDefault(KEY_OFFSET_ENABLED, v.offsetEnabled, DEFAULT_OFFSET_ENABLED);
        loadOrDefault(KEY_OFFSET_X, v.offsetX, DEFAULT_OFFSET);
        loadOrDefault(KEY_OFFSET_Y, v.offsetY, DEFAULT_OFFSET);
        loadOrDefault(KEY_TOGGLE_COMBO, v.toggleCombo, defaultToggleCombo);

        v.deadzonePct    = clampInt(v.deadzonePct, DEADZONE_MIN, DEADZONE_MAX);
        v.outerPct       = clampInt(v.outerPct, OUTER_MIN, OUTER_MAX);
        v.sensitivityPct = clampInt(v.sensitivityPct, SENSITIVITY_MIN, SENSITIVITY_MAX);
        v.offsetX        = clampInt(v.offsetX, OFFSET_MIN, OFFSET_MAX);
        v.offsetY        = clampInt(v.offsetY, OFFSET_MIN, OFFSET_MAX);
        if (v.toggleCombo == 0) v.toggleCombo = defaultToggleCombo;
        sValues = v;

        publishLocked();
        sEnabled.store(v.enabled, std::memory_order_release);
        saveLogged("init");
    }

    void OnApplicationStart() {
        // Plugin memory survives title switches; re-init the (unlocked) mutex so
        // no stale kernel-side state from the previous process is reused.
        sMutexReady.store(false, std::memory_order_release);
        OSInitMutex(&sWriterMutex);
        sMutexReady.store(true, std::memory_order_release);
    }

    Values Current() {
        WriterLock lock;
        Values v  = sValues;
        v.enabled = sEnabled.load(std::memory_order_acquire);
        return v;
    }

    bool IsEnabled() {
        return sEnabled.load(std::memory_order_relaxed);
    }

    const ParamSnapshot &Snapshot() {
        return sSnapshot;
    }

    void SetEnabled(bool enabled) {
        WriterLock lock;
        sEnabled.store(enabled, std::memory_order_release);
        sValues.enabled = enabled;
        sEnabledDirty.store(false, std::memory_order_release);
        storeLogged(KEY_ENABLED, enabled);
    }

    void SetInt(const char *key, int32_t v) {
        WriterLock lock;
        const std::string_view k(key);
        if (k == KEY_DEADZONE) {
            sValues.deadzonePct = v = clampInt(v, DEADZONE_MIN, DEADZONE_MAX);
        } else if (k == KEY_OUTER) {
            sValues.outerPct = v = clampInt(v, OUTER_MIN, OUTER_MAX);
        } else if (k == KEY_SENSITIVITY) {
            sValues.sensitivityPct = v = clampInt(v, SENSITIVITY_MIN, SENSITIVITY_MAX);
        } else if (k == KEY_OFFSET_X) {
            sValues.offsetX = v = clampInt(v, OFFSET_MIN, OFFSET_MAX);
        } else if (k == KEY_OFFSET_Y) {
            sValues.offsetY = v = clampInt(v, OFFSET_MIN, OFFSET_MAX);
        } else {
            DEBUG_FUNCTION_LINE_ERR("Unknown int setting %s", key);
            return;
        }
        publishLocked();
        storeLogged(key, v);
    }

    void SetBool(const char *key, bool v) {
        WriterLock lock;
        if (std::string_view(key) == KEY_OFFSET_ENABLED) {
            sValues.offsetEnabled = v;
            publishLocked();
            storeLogged(key, v);
        } else {
            DEBUG_FUNCTION_LINE_ERR("Unknown bool setting %s", key);
        }
    }

    void SetToggleCombo(uint32_t combo) {
        WriterLock lock;
        sValues.toggleCombo = combo;
        storeLogged(KEY_TOGGLE_COMBO, combo);
    }

    bool ToggleFromCombo() {
        // No lock and no I/O here: the calling context of button-combo
        // callbacks is not documented.
        bool expected = sEnabled.load(std::memory_order_relaxed);
        while (!sEnabled.compare_exchange_weak(expected, !expected, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        }
        sEnabledDirty.store(true, std::memory_order_release);
        return !expected;
    }

    void FlushPendingSave(const char *reason) {
        if (!sEnabledDirty.load(std::memory_order_acquire)) return;
        WriterLock lock;
        if (!sEnabledDirty.exchange(false, std::memory_order_acq_rel)) return;
        const bool enabled = sEnabled.load(std::memory_order_acquire);
        sValues.enabled    = enabled;
        storeLogged(KEY_ENABLED, enabled);
        saveLogged(reason);
        DEBUG_FUNCTION_LINE("Persisted toggle state (%s): %s", reason, enabled ? "enabled" : "disabled");
    }

    void SaveAll() {
        WriterLock lock;
        const bool enabled = sEnabled.load(std::memory_order_acquire);
        sValues.enabled    = enabled;
        sEnabledDirty.store(false, std::memory_order_release);
        storeLogged(KEY_ENABLED, enabled);
        saveLogged("config menu closed");
    }
} // namespace Settings
