#pragma once

#include "rendering/openxr.h"

struct ImFont;

namespace ImGuiMenus {
    // Pages of the mod menu, in the order they appear in the sidebar and cycle with the triggers/shoulder buttons
    inline constexpr uint8_t PLAYSTYLE_PAGE = 0;
    inline constexpr uint8_t COMFORT_PAGE = 1;
    inline constexpr uint8_t COMBAT_PAGE = 2;
    inline constexpr uint8_t CONTROLS_PAGE = 3;
    inline constexpr uint8_t INTERFACE_PAGE = 4;
    inline constexpr uint8_t PERFORMANCE_PAGE = 5;
    inline constexpr uint8_t SYSTEM_PAGE = 6;
    inline constexpr uint8_t GUIDE_PAGE = 7;
    inline constexpr uint8_t CREDITS_PAGE = 8;
    inline constexpr uint8_t DEBUG_PAGE = 9;
    inline constexpr uint8_t PAGE_COUNT = 10;

    bool IsPageAvailable(uint8_t page);

    extern ImFont* g_titleFont;

    void DrawFPSOverlay(class RND_Renderer* renderer);
    void DrawWeaponSensitivityOverlays();
    bool IsWeaponSensitivityOverlayVisible(OpenXR::EyeSide side);
    void SetWeaponSensitivityOverlayVisible(OpenXR::EyeSide side, bool visible);
}
