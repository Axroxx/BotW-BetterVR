#pragma once

#include "rendering/openxr.h"

namespace ImGuiMenus {
    inline constexpr uint8_t SETTINGS_TAB = 0;
    inline constexpr uint8_t DEBUG_TAB = 1;
    inline constexpr uint8_t HELP_TAB = 2;
    inline constexpr uint8_t FPS_OVERLAY_TAB = 3;
    inline constexpr uint8_t CREDITS_TAB = 4;
    inline constexpr uint8_t CUSTOM_ATTACK_SENSITIVITY_TAB = 5;

    void DrawWeaponSensitivityOverlays();
    bool IsWeaponSensitivityOverlayVisible(OpenXR::EyeSide side);
    void SetWeaponSensitivityOverlayVisible(OpenXR::EyeSide side, bool visible);
}
