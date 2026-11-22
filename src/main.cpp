#include "utils/logger.h"
#include <wups.h>

/**
 * GamePad Drift Fix
 *
 * Stage 1: input-logging prototype. The VPADRead hook in vpad_hook.cpp only
 * observes input; nothing is modified yet.
 */
WUPS_PLUGIN_NAME("GamePad Drift Fix");
WUPS_PLUGIN_DESCRIPTION("Radial deadzone / drift compensation for the Wii U GamePad left stick");
WUPS_PLUGIN_VERSION("v0.1.0-proto");
WUPS_PLUGIN_AUTHOR("breshusband0904-prog");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();

INITIALIZE_PLUGIN() {
    initLogging();
    DEBUG_FUNCTION_LINE("GamePad Drift Fix (logging prototype) loaded");
    deinitLogging();
}

ON_APPLICATION_START() {
    initLogging();
    DEBUG_FUNCTION_LINE("Application started");
}

ON_APPLICATION_ENDS() {
    deinitLogging();
}
