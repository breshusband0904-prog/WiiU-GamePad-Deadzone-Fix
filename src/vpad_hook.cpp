#include "vpad_hook.h"
#include "drift_filter.h"
#include "settings.h"
#include "sync.h"
#include "utils/logger.h"

#include <wups.h>

#include <vpad/input.h>

#include <atomic>
#include <cmath>
#include <cstddef>

/**
 * VPADRead replacement.
 *
 * Calls the original VPADRead, then filters the left stick of every returned
 * sample. Only VPADStatus::leftStick and the four left-stick emulated
 * direction bits of hold/trigger/release can change; everything else is
 * returned exactly as the system reported it.
 *
 * Input-path rules: no blocking locks, no unbounded loops, no storage I/O.
 */

// drift_filter.c works on the VPADStatus prefix through DriftSampleHeader.
static_assert(offsetof(VPADStatus, hold) == offsetof(DriftSampleHeader, hold));
static_assert(offsetof(VPADStatus, trigger) == offsetof(DriftSampleHeader, trigger));
static_assert(offsetof(VPADStatus, release) == offsetof(DriftSampleHeader, release));
static_assert(offsetof(VPADStatus, leftStick) + offsetof(VPADVec2D, x) == offsetof(DriftSampleHeader, x));
static_assert(offsetof(VPADStatus, leftStick) + offsetof(VPADVec2D, y) == offsetof(DriftSampleHeader, y));
static_assert(sizeof(VPADStatus) >= sizeof(DriftSampleHeader));
static_assert((uint32_t) VPAD_STICK_L_EMULATION_LEFT == DRIFT_EMU_LEFT);
static_assert((uint32_t) VPAD_STICK_L_EMULATION_RIGHT == DRIFT_EMU_RIGHT);
static_assert((uint32_t) VPAD_STICK_L_EMULATION_UP == DRIFT_EMU_UP);
static_assert((uint32_t) VPAD_STICK_L_EMULATION_DOWN == DRIFT_EMU_DOWN);

namespace {
    // VPADGetButtonProcMode(): wut releases before 2026-06-01 (as shipped in the
    // devkitppc:20260504 image) declare it as returning BOOL; newer wut returns
    // VPADButtonProcMode with VPAD_BUTTON_PROC_MODE_TIGHT == 1. Same ABI and value.
    constexpr int kButtonProcModeTight = 1;

    struct Channel {
        ChannelGuard guard;
        // Guarded by `guard`:
        DriftChannelState state{};
        DriftParams lastGood{};
        bool haveLastGood = false;
        // Lock-free:
        std::atomic<bool> resetRequested{true};
        std::atomic<float> rawX{0.0f};
        std::atomic<float> rawY{0.0f};
        std::atomic<bool> rawValid{false};
#ifdef DEBUG
        uint32_t logCounter = 0; // guarded by `guard`
#endif
    };

    Channel sChannels[2];
} // namespace

namespace VPADHook {
    void Init() {
        for (auto &c : sChannels) {
            if (c.guard.tryAcquire()) {
                drift_state_reset(&c.state);
                c.haveLastGood = Settings::Snapshot().readWithFallback(c.lastGood);
                c.guard.release();
            }
            c.resetRequested.store(true, std::memory_order_release);
        }
    }

    void RequestReset() {
        for (auto &c : sChannels) {
            c.resetRequested.store(true, std::memory_order_release);
        }
    }

    bool GetLastRawStick(int chan, float &x, float &y) {
        if (chan < 0 || chan > 1) return false;
        const Channel &c = sChannels[chan];
        if (!c.rawValid.load(std::memory_order_acquire)) return false;
        x = c.rawX.load(std::memory_order_relaxed);
        y = c.rawY.load(std::memory_order_relaxed);
        return true;
    }
} // namespace VPADHook

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffer, uint32_t buffer_size, VPADReadError *error) {
    const int32_t result = real_VPADRead(chan, buffer, buffer_size, error);

    const uint32_t count = drift_should_process(result, error != nullptr, error != nullptr ? (int32_t) *error : 0,
                                                buffer, buffer_size, (int32_t) chan);
    if (count == 0) {
        return result;
    }

    Channel &c        = sChannels[chan];
    const float rawX  = buffer[0].leftStick.x;
    const float rawY  = buffer[0].leftStick.y;
    const bool enable = Settings::IsEnabled();
    c.rawX.store(rawX, std::memory_order_relaxed);
    c.rawY.store(rawY, std::memory_order_relaxed);
    c.rawValid.store(true, std::memory_order_release);

    const int tight = (int) VPADGetButtonProcMode(chan) == kButtonProcModeTight;

    if (c.guard.tryAcquire()) {
        if (c.resetRequested.exchange(false, std::memory_order_acq_rel)) {
            drift_state_reset(&c.state);
        }
        // Bounded seqlock read; on failure keep using the last consistent snapshot.
        if (Settings::Snapshot().readWithFallback(c.lastGood)) {
            c.haveLastGood = true;
        }
        if (c.haveLastGood) {
            drift_process(&c.state, buffer, sizeof(VPADStatus), count, tight, enable, &c.lastGood);
        } else {
            // No consistent settings ever read: leave input untouched and
            // resync tracking once settings are available.
            drift_state_reset(&c.state);
        }
#ifdef DEBUG
        const bool doLog = (++c.logCounter % 30) == 0;
#endif
        c.guard.release();

#ifdef DEBUG
        if (doLog) {
            DEBUG_FUNCTION_LINE("chan %d n=%u %s %s | raw x=%+.4f y=%+.4f r=%.4f -> out x=%+.4f y=%+.4f | hold %08X trig %08X rel %08X",
                                (int) chan, (unsigned) count, tight ? "TIGHT" : "LOOSE", enable ? "ON " : "OFF",
                                rawX, rawY, std::sqrt(rawX * rawX + rawY * rawY),
                                buffer[0].leftStick.x, buffer[0].leftStick.y,
                                (unsigned) buffer[0].hold, (unsigned) buffer[0].trigger, (unsigned) buffer[0].release);
        }
#endif
    } else {
        // Concurrent or re-entrant read on the same channel: never wait.
        // Filter without touching channel history.
        DriftParams p;
        if (Settings::Snapshot().tryRead(p)) {
            drift_process_stateless(buffer, sizeof(VPADStatus), count, enable, &p);
        }
        DEBUG_FUNCTION_LINE_VERBOSE("chan %d busy, used stateless filter", chan);
    }

    return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);
