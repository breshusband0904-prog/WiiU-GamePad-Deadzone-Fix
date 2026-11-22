#include "drift_filter.h"

#include <math.h>
#include <stddef.h>

#define DEFAULT_DEADZONE    0.10f
#define DEFAULT_OUTER       1.00f
#define DEFAULT_SENSITIVITY 1.00f
#define MIN_OUTER_GAP       0.05f

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float finite_or(float v, float fallback) {
    return isfinite(v) ? v : fallback;
}

void drift_params_default(DriftParams *p) {
    p->deadzone      = DEFAULT_DEADZONE;
    p->outer         = DEFAULT_OUTER;
    p->sensitivity   = DEFAULT_SENSITIVITY;
    p->offsetX       = 0.0f;
    p->offsetY       = 0.0f;
    p->offsetEnabled = 0;
}

void drift_params_sanitize(DriftParams *p) {
    p->deadzone      = clampf(finite_or(p->deadzone, DEFAULT_DEADZONE), 0.0f, 0.9f);
    p->outer         = clampf(finite_or(p->outer, DEFAULT_OUTER), p->deadzone + MIN_OUTER_GAP, 1.0f);
    p->sensitivity   = clampf(finite_or(p->sensitivity, DEFAULT_SENSITIVITY), 0.1f, 4.0f);
    p->offsetX       = clampf(finite_or(p->offsetX, 0.0f), -0.5f, 0.5f);
    p->offsetY       = clampf(finite_or(p->offsetY, 0.0f), -0.5f, 0.5f);
    p->offsetEnabled = p->offsetEnabled ? 1 : 0;
}

int drift_filter_stick(const DriftParams *p, float *x, float *y) {
    float vx = *x;
    float vy = *y;
    if (!isfinite(vx) || !isfinite(vy)) {
        return 0;
    }
    if (p->offsetEnabled) {
        vx -= p->offsetX;
        vy -= p->offsetY;
    }

    const float m  = sqrtf(vx * vx + vy * vy);
    const float dz = p->deadzone;
    if (m <= dz || m <= 0.0f) {
        *x = 0.0f;
        *y = 0.0f;
        return 1;
    }

    float outer = p->outer;
    if (outer < dz + MIN_OUTER_GAP) outer = dz + MIN_OUTER_GAP;

    float n = (m - dz) / (outer - dz);
    n       = clampf(n, 0.0f, 1.0f) * p->sensitivity;
    n       = clampf(n, 0.0f, 1.0f);

    const float scale = n / m;
    *x                = vx * scale;
    *y                = vy * scale;
    return 0;
}

uint32_t drift_emu_keep_mask(float fx, float fy) {
    if (!isfinite(fx) || !isfinite(fy)) {
        return DRIFT_EMU_MASK;
    }
    const float m = sqrtf(fx * fx + fy * fy);
    if (!(m >= DRIFT_EMU_MIN_MAGNITUDE)) {
        return 0;
    }
    const float c = m * DRIFT_EMU_COS_MAX_ANGLE;
    uint32_t keep = 0;
    if (-fx >= c) keep |= DRIFT_EMU_LEFT;
    if (fx >= c) keep |= DRIFT_EMU_RIGHT;
    if (fy >= c) keep |= DRIFT_EMU_UP;
    if (-fy >= c) keep |= DRIFT_EMU_DOWN;
    return keep;
}

uint32_t drift_should_process(int32_t result, int32_t hasError, int32_t errorCode,
                              const void *buffer, uint32_t bufferCount, int32_t chan) {
    if (result <= 0 || buffer == NULL || bufferCount == 0) return 0;
    if (hasError && errorCode != 0) return 0;
    if (chan < 0 || chan > 1) return 0;
    return ((uint32_t) result < bufferCount) ? (uint32_t) result : bufferCount;
}

void drift_state_reset(DriftChannelState *s) {
    s->prevEmuHold = 0;
    s->synced      = 0;
    s->lastEnabled = 0;
}

static DriftSampleHeader *sample_at(void *base, uint32_t stride, uint32_t i) {
    return (DriftSampleHeader *) ((uint8_t *) base + (size_t) i * stride);
}

/* Replace the emulated-direction bits of a sample with newEmu and its edges. */
static void write_emu(DriftSampleHeader *s, uint32_t newEmu, uint32_t trig, uint32_t rel) {
    s->hold    = (s->hold & ~DRIFT_EMU_MASK) | (newEmu & DRIFT_EMU_MASK);
    s->trigger = (s->trigger & ~DRIFT_EMU_MASK) | (trig & DRIFT_EMU_MASK);
    s->release = (s->release & ~DRIFT_EMU_MASK) | (rel & DRIFT_EMU_MASK);
}

/* Edges for one sample. With a trusted reference they are derived from it;
 * without one (unsynced) the system's own edges are kept, masked so that a
 * trigger never appears for a bit that is not held and a release never
 * appears for a bit that is still held. */
static void edges_for(const DriftSampleHeader *s, uint32_t newEmu, int haveRef, uint32_t ref,
                      uint32_t *trig, uint32_t *rel) {
    if (haveRef) {
        *trig = newEmu & ~ref;
        *rel  = ref & ~newEmu;
    } else {
        *trig = s->trigger & DRIFT_EMU_MASK & newEmu;
        *rel  = s->release & DRIFT_EMU_MASK & ~newEmu;
    }
}

void drift_process(DriftChannelState *state, void *base, uint32_t stride, uint32_t count,
                   int tight, int enabled, const DriftParams *p) {
    if (count == 0 || base == NULL) return;

    const int haveRef = state->synced;
    uint32_t ref      = state->prevEmuHold;

    if (!enabled) {
        /* Pure pass-through, except right after enabled -> disabled. */
        if (haveRef && state->lastEnabled) {
            if (tight) {
                for (uint32_t k = count; k-- > 0;) {
                    DriftSampleHeader *s = sample_at(base, stride, k);
                    uint32_t newEmu      = s->hold & DRIFT_EMU_MASK;
                    uint32_t trig, rel;
                    edges_for(s, newEmu, 1, ref, &trig, &rel);
                    write_emu(s, newEmu, trig, rel);
                    ref = newEmu;
                }
            } else {
                uint32_t newEmu = sample_at(base, stride, 0)->hold & DRIFT_EMU_MASK;
                for (uint32_t k = 0; k < count; k++) {
                    DriftSampleHeader *s = sample_at(base, stride, k);
                    uint32_t trig, rel;
                    edges_for(s, newEmu, 1, ref, &trig, &rel);
                    write_emu(s, newEmu, trig, rel);
                }
            }
        }
        state->prevEmuHold = sample_at(base, stride, 0)->hold & DRIFT_EMU_MASK;
        state->synced      = 1;
        state->lastEnabled = 0;
        return;
    }

    /* 1) Filter every sample's analog value in place. */
    for (uint32_t k = 0; k < count; k++) {
        DriftSampleHeader *s = sample_at(base, stride, k);
        drift_filter_stick(p, &s->x, &s->y);
    }

    /* 2) Emulated-direction bits + edges. */
    if (tight) {
        /* Oldest -> newest; each sample's reference is the one before it. */
        int chainRef = haveRef;
        for (uint32_t k = count; k-- > 0;) {
            DriftSampleHeader *s = sample_at(base, stride, k);
            uint32_t newEmu      = s->hold & DRIFT_EMU_MASK & drift_emu_keep_mask(s->x, s->y);
            uint32_t trig, rel;
            edges_for(s, newEmu, chainRef, ref, &trig, &rel);
            write_emu(s, newEmu, trig, rel);
            ref      = newEmu;
            chainRef = 1;
        }
        state->prevEmuHold = ref;
    } else {
        /* One button state for all samples, decided by the newest sample. */
        DriftSampleHeader *newest = sample_at(base, stride, 0);
        uint32_t newEmu           = newest->hold & DRIFT_EMU_MASK & drift_emu_keep_mask(newest->x, newest->y);
        for (uint32_t k = 0; k < count; k++) {
            DriftSampleHeader *s = sample_at(base, stride, k);
            uint32_t trig, rel;
            edges_for(s, newEmu, haveRef, ref, &trig, &rel);
            write_emu(s, newEmu, trig, rel);
        }
        state->prevEmuHold = newEmu;
    }
    state->synced      = 1;
    state->lastEnabled = 1;
}

void drift_process_stateless(void *base, uint32_t stride, uint32_t count, int enabled, const DriftParams *p) {
    if (!enabled || count == 0 || base == NULL) return;
    for (uint32_t k = 0; k < count; k++) {
        DriftSampleHeader *s = sample_at(base, stride, k);
        drift_filter_stick(p, &s->x, &s->y);
        const uint32_t keep = drift_emu_keep_mask(s->x, s->y) | ~DRIFT_EMU_MASK;
        s->hold &= keep;
        s->trigger &= keep;
    }
}
