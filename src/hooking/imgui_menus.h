#pragma once

#include "rendering/openxr.h"

namespace ImGuiMenus {
    void DrawWeaponSensitivityOverlays();
    void DrawDebugOverlays();
    bool IsWeaponSensitivityOverlayVisible(::OpenXR::EyeSide side);
    void SetWeaponSensitivityOverlayVisible(::OpenXR::EyeSide side, bool visible);
}
