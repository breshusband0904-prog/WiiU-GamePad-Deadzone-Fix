#pragma once

#include <stdint.h>

namespace ConfigMenu {
    /** INITIALIZE_PLUGIN: register the config menu and the toggle combo. */
    void Init();
    /** DEINITIALIZE_PLUGIN: remove the toggle combo. */
    void Deinit();
    /** Default toggle combo (ZL + ZR + left stick click) as WUPSButtonCombo_Buttons. */
    uint32_t DefaultCombo();
} // namespace ConfigMenu
