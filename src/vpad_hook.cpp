#include "utils/logger.h"
#include <wups.h>

#include <vpad/input.h>

#include <math.h>

/**
 * Stage 1: logging prototype.
 *
 * Modeled on Maschell's WUPSPluginPlayground/vpad_input_logger, ported to the
 * current WUPS API. The original VPADRead is called and its output is
 * returned UNMODIFIED. In DEBUG builds the left stick is logged so that the
 * resting drift of a specific GamePad can be measured.
 */

#ifdef DEBUG
static uint32_t sReadCounter = 0;
static uint32_t sLastHold    = 0;
static float sMinX = 0.0f, sMaxX = 0.0f, sMinY = 0.0f, sMaxY = 0.0f;
static bool sEnvelopeValid = false;

static constexpr uint32_t L_EMU_MASK = VPAD_STICK_L_EMULATION_LEFT | VPAD_STICK_L_EMULATION_RIGHT |
                                       VPAD_STICK_L_EMULATION_UP | VPAD_STICK_L_EMULATION_DOWN;

static void logSample(VPADChan chan, const VPADStatus *newest, int32_t count) {
    const float x = newest->leftStick.x;
    const float y = newest->leftStick.y;

    if (!sEnvelopeValid) {
        sMinX = sMaxX = x;
        sMinY = sMaxY = y;
        sEnvelopeValid = true;
    } else {
        if (x < sMinX) sMinX = x;
        if (x > sMaxX) sMaxX = x;
        if (y < sMinY) sMinY = y;
        if (y > sMaxY) sMaxY = y;
    }

    sReadCounter++;
    if ((sReadCounter % 30) == 0) {
        DEBUG_FUNCTION_LINE("chan %d samples %d mode %d | L x=%+.4f y=%+.4f r=%.4f | R x=%+.4f y=%+.4f",
                            chan, count, (int) VPADGetButtonProcMode(chan),
                            x, y, sqrtf(x * x + y * y),
                            newest->rightStick.x, newest->rightStick.y);
    }
    if ((sReadCounter % 300) == 0) {
        DEBUG_FUNCTION_LINE("L-stick envelope over last 300 reads: x [%+.4f, %+.4f] y [%+.4f, %+.4f]",
                            sMinX, sMaxX, sMinY, sMaxY);
        sEnvelopeValid = false;
    }

    if (newest->hold != sLastHold) {
        const uint32_t emu = newest->hold & L_EMU_MASK;
        DEBUG_FUNCTION_LINE("hold %08X (trigger %08X release %08X) L-emu:%s%s%s%s",
                            newest->hold, newest->trigger, newest->release,
                            (emu & VPAD_STICK_L_EMULATION_LEFT) ? " LEFT" : "",
                            (emu & VPAD_STICK_L_EMULATION_RIGHT) ? " RIGHT" : "",
                            (emu & VPAD_STICK_L_EMULATION_UP) ? " UP" : "",
                            (emu & VPAD_STICK_L_EMULATION_DOWN) ? " DOWN" : "");
        sLastHold = newest->hold;
    }
}
#endif

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffer, uint32_t buffer_size, VPADReadError *error) {
    const int32_t result = real_VPADRead(chan, buffer, buffer_size, error);
#ifdef DEBUG
    if (result > 0 && buffer != nullptr && (error == nullptr || *error == VPAD_READ_SUCCESS)) {
        // buffer[0] is the newest sample.
        logSample(chan, &buffer[0], result);
    }
#endif
    return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);
