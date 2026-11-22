#pragma once

/**
 * Pure-C drift filter core. No wut / WUPS headers, so it can be unit-tested on
 * the host (see tests/). The VPADRead hook in vpad_hook.cpp is a thin wrapper
 * around drift_process().
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Left-stick emulated-direction bits as defined by wut's vpad/input.h
 * (VPAD_STICK_L_EMULATION_*). vpad_hook.cpp static_asserts they match. */
#define DRIFT_EMU_LEFT          0x40000000u
#define DRIFT_EMU_RIGHT         0x20000000u
#define DRIFT_EMU_UP            0x10000000u
#define DRIFT_EMU_DOWN          0x08000000u
#define DRIFT_EMU_MASK          (DRIFT_EMU_LEFT | DRIFT_EMU_RIGHT | DRIFT_EMU_UP | DRIFT_EMU_DOWN)

/* A system-reported emulated direction is only kept when the *filtered* analog
 * vector has at least this magnitude... */
#define DRIFT_EMU_MIN_MAGNITUDE 0.25f
/* ...and lies within 67.5 degrees of that direction (cos 67.5 deg), so both
 * bits of an 8-way diagonal survive. */
#define DRIFT_EMU_COS_MAX_ANGLE 0.38268343f

typedef struct DriftParams {
    float deadzone;    /* inner radial deadzone, 0 .. 0.9 */
    float outer;       /* raw magnitude mapped to full deflection, deadzone+0.05 .. 1 */
    float sensitivity; /* output gain after rescaling, 0.1 .. 4 */
    float offsetX;     /* subtracted from raw X when offsetEnabled, -0.5 .. 0.5 */
    float offsetY;
    int32_t offsetEnabled;
} DriftParams;

/* Layout of the first 20 bytes of VPADStatus (checked by static_assert in
 * vpad_hook.cpp): hold@0x00, trigger@0x04, release@0x08, leftStick@0x0C. */
typedef struct DriftSampleHeader {
    uint32_t hold;
    uint32_t trigger;
    uint32_t release;
    float x;
    float y;
} DriftSampleHeader;

typedef struct DriftChannelState {
    uint32_t prevEmuHold; /* emulated-direction bits delivered on the newest sample of the last call */
    int32_t synced;       /* 0 after reset: prevEmuHold is not trustworthy */
    int32_t lastEnabled;  /* filtering was enabled on the last processed call */
} DriftChannelState;

void drift_params_default(DriftParams *p);
/* Clamp every field into its documented range; non-finite values become defaults. */
void drift_params_sanitize(DriftParams *p);

/* Filters one stick vector in place. Returns 1 if the output was zeroed by
 * the deadzone. Non-finite input is left unchanged (returns 0). */
int drift_filter_stick(const DriftParams *p, float *x, float *y);

/* Which emulated-direction bits are consistent with filtered vector (fx, fy).
 * Non-finite input returns DRIFT_EMU_MASK (no information: keep system bits). */
uint32_t drift_emu_keep_mask(float fx, float fy);

/* Number of samples the hook should process (0 = leave the buffer untouched).
 * hasError/errorCode: pass hasError=0 when VPADRead got a NULL error pointer. */
uint32_t drift_should_process(int32_t result, int32_t hasError, int32_t errorCode,
                              const void *buffer, uint32_t bufferCount, int32_t chan);

void drift_state_reset(DriftChannelState *s);

/**
 * Processes `count` samples in place. Sample i lives at base + i*stride and
 * starts with a DriftSampleHeader; index 0 is the NEWEST sample (VPADRead order).
 *
 * tight != 0 : VPAD_BUTTON_PROC_MODE_TIGHT (per-sample button history).
 * tight == 0 : VPAD_BUTTON_PROC_MODE_LOOSE (all samples share one button state).
 *
 * Only the left stick and the four left-stick emulated-direction bits of
 * hold/trigger/release are ever changed. Emulated bits are only cleared, never
 * added. When enabled == 0 the samples are left untouched, except for one
 * call after an enabled->disabled transition where emulated-bit edges are
 * recomputed so no held bit loses its release / gains a phantom trigger.
 */
void drift_process(DriftChannelState *state, void *base, uint32_t stride, uint32_t count,
                   int tight, int enabled, const DriftParams *p);

/**
 * Fallback used when the channel state is busy (concurrent / re-entrant read):
 * filters the analog value and clears inconsistent emulated bits from hold and
 * trigger, without touching or using channel history.
 */
void drift_process_stateless(void *base, uint32_t stride, uint32_t count, int enabled, const DriftParams *p);

#ifdef __cplusplus
}
#endif
