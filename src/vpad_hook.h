#pragma once

#include <stdint.h>

namespace VPADHook {
    /** INITIALIZE_PLUGIN: reset per-channel state and seed the fallback snapshot. */
    void Init();

    /**
     * Request that per-channel button tracking is reset before the next
     * VPADRead (application start/end, HOME menu return, config menu close).
     * Lock-free; safe from any context.
     */
    void RequestReset();

    /** Most recent raw (unfiltered) left stick seen on a channel. */
    bool GetLastRawStick(int chan, float &x, float &y);
} // namespace VPADHook
