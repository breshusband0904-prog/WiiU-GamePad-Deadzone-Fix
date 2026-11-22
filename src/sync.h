#pragma once

/**
 * Lock-free primitives used on the VPADRead path. Header-only and free of
 * wut / WUPS dependencies so the host tests can exercise them.
 *
 * Nothing here ever blocks or spins without bound: VPADRead may be called
 * from any game thread (and possibly from contexts where blocking is unsafe),
 * so the input path only uses atomic loads/stores and a single
 * exchange-based try-acquire.
 */

#include "drift_filter.h"

#include <atomic>
#include <stdint.h>

static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(std::atomic<float>::is_always_lock_free, "need lock-free float atomics");
static_assert(std::atomic<int32_t>::is_always_lock_free, "need lock-free int32 atomics");

/**
 * Seqlock-protected DriftParams.
 *
 * Writers must be serialized externally (settings.cpp uses an OSMutex, which is
 * never taken on the input path). Readers are wait-free: tryRead() makes at
 * most kMaxReadAttempts attempts and reports failure instead of retrying
 * forever, e.g. if a writer was preempted half-way through publish(). Callers
 * then fall back to their last consistent snapshot (readWithFallback()).
 */
class ParamSnapshot {
public:
    static constexpr int kMaxReadAttempts = 4;

    void publish(const DriftParams &p) noexcept {
        beginWrite();
        writeFields(p);
        endWrite();
    }

    bool tryRead(DriftParams &out) const noexcept {
        for (int attempt = 0; attempt < kMaxReadAttempts; attempt++) {
            const uint32_t s1 = mSeq.load(std::memory_order_acquire);
            if (s1 & 1u) {
                continue; // writer in progress
            }
            DriftParams tmp;
            tmp.deadzone      = mDeadzone.load(std::memory_order_relaxed);
            tmp.outer         = mOuter.load(std::memory_order_relaxed);
            tmp.sensitivity   = mSensitivity.load(std::memory_order_relaxed);
            tmp.offsetX       = mOffsetX.load(std::memory_order_relaxed);
            tmp.offsetY       = mOffsetY.load(std::memory_order_relaxed);
            tmp.offsetEnabled = mOffsetEnabled.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t s2 = mSeq.load(std::memory_order_relaxed);
            if (s1 == s2) {
                out = tmp;
                return true;
            }
        }
        return false;
    }

    /**
     * Updates lastGood with a fresh consistent snapshot when one can be read
     * within the bounded attempts; otherwise leaves lastGood unchanged.
     * Returns true if lastGood was refreshed.
     */
    bool readWithFallback(DriftParams &lastGood) const noexcept {
        DriftParams fresh;
        if (tryRead(fresh)) {
            lastGood = fresh;
            return true;
        }
        return false;
    }

    // The three write phases are public so tests can simulate a writer that is
    // interrupted between beginWrite() and endWrite().
    void beginWrite() noexcept {
        const uint32_t s = mSeq.load(std::memory_order_relaxed);
        mSeq.store(s + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void writeFields(const DriftParams &p) noexcept {
        mDeadzone.store(p.deadzone, std::memory_order_relaxed);
        mOuter.store(p.outer, std::memory_order_relaxed);
        mSensitivity.store(p.sensitivity, std::memory_order_relaxed);
        mOffsetX.store(p.offsetX, std::memory_order_relaxed);
        mOffsetY.store(p.offsetY, std::memory_order_relaxed);
        mOffsetEnabled.store(p.offsetEnabled, std::memory_order_relaxed);
    }

    void endWrite() noexcept {
        const uint32_t s = mSeq.load(std::memory_order_relaxed);
        mSeq.store(s + 1u, std::memory_order_release);
    }

private:
    std::atomic<uint32_t> mSeq{0};
    std::atomic<float> mDeadzone{0.10f};
    std::atomic<float> mOuter{1.0f};
    std::atomic<float> mSensitivity{1.0f};
    std::atomic<float> mOffsetX{0.0f};
    std::atomic<float> mOffsetY{0.0f};
    std::atomic<int32_t> mOffsetEnabled{0};
};

/**
 * Per-channel exclusive-access flag for the VPADRead path. tryAcquire() never
 * waits; if another (or a re-entrant) VPADRead is already processing this
 * channel, the caller uses drift_process_stateless() instead.
 */
class ChannelGuard {
public:
    bool tryAcquire() noexcept {
        return mBusy.exchange(1u, std::memory_order_acquire) == 0u;
    }

    void release() noexcept {
        mBusy.store(0u, std::memory_order_release);
    }

private:
    std::atomic<uint32_t> mBusy{0};
};
