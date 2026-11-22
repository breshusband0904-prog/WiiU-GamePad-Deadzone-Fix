#include "config_menu.h"
#include "settings.h"
#include "utils/logger.h"
#include "vpad_hook.h"

#include <wups.h>

/**
 * GamePad Drift Fix
 *
 * Hooks VPADRead (vpad_hook.cpp) and applies a configurable radial deadzone
 * to the GamePad's left stick. Compensation is OFF on a fresh install.
 */
WUPS_PLUGIN_NAME("GamePad Drift Fix");
WUPS_PLUGIN_DESCRIPTION("Radial deadzone / drift compensation for the Wii U GamePad left stick");
WUPS_PLUGIN_VERSION("v0.2.0");
WUPS_PLUGIN_AUTHOR("breshusband0904-prog");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("gamepad_drift_fix");

INITIALIZE_PLUGIN() {
    initLogging();
    Settings::Init(ConfigMenu::DefaultCombo());
    VPADHook::Init();
    ConfigMenu::Init();
    DEBUG_FUNCTION_LINE("GamePad Drift Fix loaded, compensation %s", Settings::IsEnabled() ? "ON" : "OFF");
    deinitLogging();
}

DEINITIALIZE_PLUGIN() {
    Settings::FlushPendingSave("plugin deinit");
    ConfigMenu::Deinit();
}

ON_APPLICATION_START() {
    initLogging();
    Settings::OnApplicationStart();
    VPADHook::RequestReset();
    DEBUG_FUNCTION_LINE("Application start, compensation %s", Settings::IsEnabled() ? "ON" : "OFF");
}

ON_RELEASE_FOREGROUND() {
    // HOME menu opened (or other loss of foreground): persist a pending toggle.
    Settings::FlushPendingSave("release foreground");
}

ON_ACQUIRED_FOREGROUND() {
    // Back from the HOME menu: button history may be stale.
    VPADHook::RequestReset();
}

ON_APPLICATION_REQUESTS_EXIT() {
    Settings::FlushPendingSave("application requests exit");
}

ON_APPLICATION_ENDS() {
    Settings::FlushPendingSave("application ends");
    VPADHook::RequestReset();
    deinitLogging();
}
