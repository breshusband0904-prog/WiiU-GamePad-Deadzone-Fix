#pragma once

#include "drift_filter.h"
#include "sync.h"

#include <stdint.h>

/**
 * Plugin settings shared between the config menu, the toggle combo callback,
 * application hooks and the VPADRead hook.
 *
 * - The VPADRead hook only uses IsEnabled() (atomic load) and Snapshot()
 *   (bounded, wait-free seqlock read). It never blocks and never touches storage.
 * - All writers (config menu callbacks, init, flush) are serialized by an
 *   OSMutex that is never taken on the input path.
 * - The toggle combo callback only flips an atomic flag and marks the setting
 *   dirty; the SD write happens later in FlushPendingSave() from a safe context.
 */
namespace Settings {
    // Stored values (integers as shown in the config menu).
    struct Values {
        bool enabled;
        int32_t deadzonePct;    // 0..50
        int32_t outerPct;       // 50..100
        int32_t sensitivityPct; // 50..200
        bool offsetEnabled;
        int32_t offsetX; // -25..25 (hundredths)
        int32_t offsetY; // -25..25 (hundredths)
        uint32_t toggleCombo;
    };

    // Defaults for a fresh install. Compensation starts DISABLED.
    constexpr bool DEFAULT_ENABLED            = false;
    constexpr int32_t DEFAULT_DEADZONE_PCT    = 10;
    constexpr int32_t DEFAULT_OUTER_PCT       = 100;
    constexpr int32_t DEFAULT_SENSITIVITY_PCT = 100;
    constexpr bool DEFAULT_OFFSET_ENABLED     = false;
    constexpr int32_t DEFAULT_OFFSET          = 0;

    constexpr int32_t DEADZONE_MIN = 0, DEADZONE_MAX = 50;
    constexpr int32_t OUTER_MIN = 50, OUTER_MAX = 100;
    constexpr int32_t SENSITIVITY_MIN = 50, SENSITIVITY_MAX = 200;
    constexpr int32_t OFFSET_MIN = -25, OFFSET_MAX = 25;

    // Storage keys
    constexpr const char *KEY_ENABLED        = "enabled";
    constexpr const char *KEY_DEADZONE       = "deadzonePct";
    constexpr const char *KEY_OUTER          = "outerPct";
    constexpr const char *KEY_SENSITIVITY    = "sensitivityPct";
    constexpr const char *KEY_OFFSET_ENABLED = "offsetEnabled";
    constexpr const char *KEY_OFFSET_X       = "offsetX";
    constexpr const char *KEY_OFFSET_Y       = "offsetY";
    constexpr const char *KEY_TOGGLE_COMBO   = "toggleCombo";

    /** INITIALIZE_PLUGIN: init mutex, load (or create) storage, publish params. */
    void Init(uint32_t defaultToggleCombo);

    /** ON_APPLICATION_START: re-initialize the writer mutex (no writer can be active). */
    void OnApplicationStart();

    /** Copy of all values, for building the config menu. */
    Values Current();

    // --- input path (lock-free) ---
    bool IsEnabled();
    const ParamSnapshot &Snapshot();

    // --- writers (config menu thread / hooks) ---
    void SetEnabled(bool enabled);           // stores immediately (config menu)
    void SetInt(const char *key, int32_t v); // deadzone/outer/sensitivity/offsets
    void SetBool(const char *key, bool v);   // offsetEnabled
    void SetToggleCombo(uint32_t combo);

    /** Toggle combo callback: atomic flip + mark dirty, no I/O. Returns new state. */
    bool ToggleFromCombo();

    /** Persist pending changes (combo toggle) if any. Safe to call often. */
    void FlushPendingSave(const char *reason);

    /** Save everything (config menu closed). */
    void SaveAll();
} // namespace Settings
