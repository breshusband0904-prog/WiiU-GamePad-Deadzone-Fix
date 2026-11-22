#include "config_menu.h"
#include "settings.h"
#include "utils/logger.h"
#include "vpad_hook.h"

#include <wups.h>
#include <wups/button_combo/api.h>
#include <wups/config/WUPSConfigCategory.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemButtonCombo.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config_api.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string_view>

namespace {
    constexpr const char *COMBO_LABEL = "GamePad Drift Fix: toggle";
    constexpr uint32_t COMBO_HOLD_MS  = 1000;
    constexpr WUPSButtonCombo_Buttons DEFAULT_COMBO =
            WUPS_BUTTON_COMBO_BUTTON_ZL | WUPS_BUTTON_COMBO_BUTTON_ZR | WUPS_BUTTON_COMBO_BUTTON_STICK_L;

    std::optional<WUPSButtonComboAPI::ButtonCombo> sToggleCombo;

    void toggleComboCallback(WUPSButtonCombo_ControllerTypes, WUPSButtonCombo_ComboHandle, void *) {
        [[maybe_unused]] const bool nowEnabled = Settings::ToggleFromCombo();
        VPADHook::RequestReset();
        DEBUG_FUNCTION_LINE("Toggle combo: drift compensation %s (saved at next safe point)", nowEnabled ? "ON" : "OFF");
    }

    void boolChanged(ConfigItemBoolean *item, bool newValue) {
        const std::string_view id(item->identifier);
        if (id == Settings::KEY_ENABLED) {
            Settings::SetEnabled(newValue);
        } else if (id == Settings::KEY_OFFSET_ENABLED) {
            Settings::SetBool(Settings::KEY_OFFSET_ENABLED, newValue);
        }
    }

    void intChanged(ConfigItemIntegerRange *item, int32_t newValue) {
        Settings::SetInt(item->identifier, newValue);
    }

    void comboChanged(ConfigItemButtonCombo *, uint32_t newCombo) {
        Settings::SetToggleCombo(newCombo);
    }

    WUPSConfigAPICallbackStatus menuOpened(WUPSConfigCategoryHandle rootHandle) {
        Settings::FlushPendingSave("config menu opened");
        const Settings::Values v = Settings::Current();

        WUPSConfigCategory root(rootHandle);
        try {
            root.add(WUPSConfigItemBoolean::Create(Settings::KEY_ENABLED, "Enable drift compensation",
                                                   Settings::DEFAULT_ENABLED, v.enabled, &boolChanged));

            char readout[96];
            float rx, ry;
            if (VPADHook::GetLastRawStick(0, rx, ry)) {
                std::snprintf(readout, sizeof(readout), "Left stick when menu opened: x=%+.3f y=%+.3f r=%.3f",
                              rx, ry, std::sqrt(rx * rx + ry * ry));
            } else {
                std::snprintf(readout, sizeof(readout), "Left stick when menu opened: no GamePad data yet");
            }
            root.add(WUPSConfigItemStub::Create(readout));

            auto dz = WUPSConfigCategory::Create("Deadzone and sensitivity");
            dz.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_DEADZONE, "Inner deadzone (%)",
                                                      Settings::DEFAULT_DEADZONE_PCT, v.deadzonePct,
                                                      Settings::DEADZONE_MIN, Settings::DEADZONE_MAX, &intChanged));
            dz.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_OUTER, "Outer limit (%)",
                                                      Settings::DEFAULT_OUTER_PCT, v.outerPct,
                                                      Settings::OUTER_MIN, Settings::OUTER_MAX, &intChanged));
            dz.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_SENSITIVITY, "Sensitivity (%)",
                                                      Settings::DEFAULT_SENSITIVITY_PCT, v.sensitivityPct,
                                                      Settings::SENSITIVITY_MIN, Settings::SENSITIVITY_MAX, &intChanged));
            root.add(std::move(dz));

            auto off = WUPSConfigCategory::Create("Center-offset correction");
            off.add(WUPSConfigItemBoolean::Create(Settings::KEY_OFFSET_ENABLED, "Apply center offset",
                                                  Settings::DEFAULT_OFFSET_ENABLED, v.offsetEnabled, &boolChanged));
            off.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_OFFSET_X, "Offset X (x0.01)",
                                                       Settings::DEFAULT_OFFSET, v.offsetX,
                                                       Settings::OFFSET_MIN, Settings::OFFSET_MAX, &intChanged));
            off.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_OFFSET_Y, "Offset Y (x0.01)",
                                                       Settings::DEFAULT_OFFSET, v.offsetY,
                                                       Settings::OFFSET_MIN, Settings::OFFSET_MAX, &intChanged));
            off.add(WUPSConfigItemStub::Create("Set the offset to the resting values shown above."));
            root.add(std::move(off));

            if (sToggleCombo) {
                root.add(WUPSConfigItemButtonCombo::Create(Settings::KEY_TOGGLE_COMBO, "Toggle combo (hold 1s)",
                                                           DEFAULT_COMBO, sToggleCombo->getHandle(), &comboChanged));
            } else {
                root.add(WUPSConfigItemStub::Create("Toggle combo unavailable (see log)"));
            }
        } catch (std::exception &e) {
            DEBUG_FUNCTION_LINE_ERR("Creating config menu failed: %s", e.what());
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
        return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
    }

    void menuClosed() {
        Settings::SaveAll();
        VPADHook::RequestReset();
    }
} // namespace

namespace ConfigMenu {
    void Init() {
        WUPSConfigAPIOptionsV1 configOptions = {.name = "GamePad Drift Fix"};
        if (WUPSConfigAPI_Init(configOptions, menuOpened, menuClosed) != WUPSCONFIG_API_RESULT_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("Failed to init config api");
        }

        const auto combo                   = (WUPSButtonCombo_Buttons) Settings::Current().toggleCombo;
        WUPSButtonCombo_ComboStatus status = WUPS_BUTTON_COMBO_COMBO_STATUS_INVALID_STATUS;
        WUPSButtonCombo_Error err          = WUPS_BUTTON_COMBO_ERROR_UNKNOWN_ERROR;
        sToggleCombo                       = WUPSButtonComboAPI::CreateComboHold(COMBO_LABEL, combo, COMBO_HOLD_MS,
                                                                                 &toggleComboCallback, nullptr, status, err);
        if (!sToggleCombo || err != WUPS_BUTTON_COMBO_ERROR_SUCCESS) {
            DEBUG_FUNCTION_LINE_ERR("Failed to create toggle combo: %s", WUPSButtonComboAPI::GetStatusStr(err));
            sToggleCombo.reset();
        } else if (status == WUPS_BUTTON_COMBO_COMBO_STATUS_CONFLICT) {
            DEBUG_FUNCTION_LINE_WARN("Toggle combo conflicts with another combo; rebind it in the config menu");
        }
    }

    void Deinit() {
        sToggleCombo.reset();
    }

    uint32_t DefaultCombo() {
        return (uint32_t) DEFAULT_COMBO;
    }
} // namespace ConfigMenu
