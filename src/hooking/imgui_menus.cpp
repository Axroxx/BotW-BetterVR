#include "pch.h"

#include "imgui_menus.h"

#include "cemu_hooks.h"
#include "imgui_internal.h"
#include "instance.h"
#include "rendering/renderer.h"
#include "rendering/openxr.h"
#include "utils/profiler.h"
#include "utils/mod_settings.h"
#include "weapon.h"
#include "supporters_generated.h"

static constexpr ImVec4 kDescriptionColor = ImVec4(0.63f, 0.71f, 0.78f, 1.0f);
static constexpr ImVec4 kSectionCaptionColor = ImVec4(0.40f, 0.66f, 1.0f, 1.0f);
static constexpr ImVec4 kHintColor = ImVec4(0.35f, 0.95f, 0.45f, 1.0f);

static void DrawPageHeader(const char* icon, const char* title, const char* subtitle) {
    ImGui::Spacing();
    if (ImGuiMenus::g_titleFont != nullptr) {
        ImGui::PushFont(ImGuiMenus::g_titleFont);
        ImGui::Text("%s  %s", icon, title);
        ImGui::PopFont();
    }
    else {
        ImGui::Text("%s  %s", icon, title);
    }
    if (subtitle != nullptr && subtitle[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, kDescriptionColor);
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
}

static void DrawSectionCaption(const char* label) {
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kSectionCaptionColor);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
}

static void DrawPageNote(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDescriptionColor);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static bool BeginSettingsSection(const char* id, const char* caption = nullptr) {
    if (caption != nullptr) {
        DrawSectionCaption(caption);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, 9.0f));
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("##widget", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    return true;
}

static void EndSettingsSection() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

template <typename DrawWidget>
static void DrawSetting(const char* label, const char* description, DrawWidget&& drawWidget) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (description != nullptr && description[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, kDescriptionColor);
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
    ImGui::PushItemWidth(-FLT_MIN);
    drawWidget();
    ImGui::PopItemWidth();
    ImGui::PopID();
}

struct MenuPageEntry {
    uint8_t page;
    const char* label;
};

static constexpr MenuPageEntry kSettingsPages[] = {
    { ImGuiMenus::PLAYSTYLE_PAGE, ICON_KI_PLAYER "  Playstyle" },
    { ImGuiMenus::COMFORT_PAGE, ICON_KI_BOOT "  Comfort" },
    { ImGuiMenus::COMBAT_PAGE, ICON_KI_SWORD "  Combat" },
    { ImGuiMenus::CONTROLS_PAGE, ICON_KI_GAMEPAD "  Controls" },
    { ImGuiMenus::INTERFACE_PAGE, ICON_KI_UI "  Interface" },
    { ImGuiMenus::PERFORMANCE_PAGE, ICON_KI_GAUGE "  Performance" },
    { ImGuiMenus::SYSTEM_PAGE, ICON_KI_COG "  System" },
};

static constexpr MenuPageEntry kMorePages[] = {
    { ImGuiMenus::GUIDE_PAGE, ICON_KI_HELP "  Controller Guide" },
    { ImGuiMenus::CREDITS_PAGE, ICON_KI_HEART "  Credits" },
    { ImGuiMenus::DEBUG_PAGE, ICON_KI_WRENCH "  Debug" },
};

struct SidebarNavTarget {
    ImGuiID itemId = 0;
    ImGuiID focusScopeId = 0;
    ImRect rectRel = {};
};

static void DrawSidebarGroup(const char* caption, std::span<const MenuPageEntry> entries, std::atomic_uint8_t& selectedPage, SidebarNavTarget& navTarget) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDescriptionColor);
    ImGui::TextUnformatted(caption);
    ImGui::PopStyleColor();
    for (const MenuPageEntry& entry : entries) {
        if (!ImGuiMenus::IsPageAvailable(entry.page)) {
            continue;
        }
        const bool isSelectedPage = selectedPage == entry.page;
        if (ImGui::Selectable(entry.label, isSelectedPage, ImGuiSelectableFlags_None, ImVec2(0.0f, 30.0f))) {
            selectedPage = entry.page;
        }
        if (isSelectedPage) {
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            navTarget.itemId = g.LastItemData.ID;
            navTarget.focusScopeId = g.CurrentFocusScopeId;
            navTarget.rectRel = ImGui::WindowRectAbsToRel(ImGui::GetCurrentWindow(), g.LastItemData.NavRect);
        }
    }
}

static SidebarNavTarget DrawSidebar(std::atomic_uint8_t& selectedPage) {
    SidebarNavTarget navTarget;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    DrawSidebarGroup("SETTINGS", kSettingsPages, selectedPage, navTarget);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    DrawSidebarGroup("MORE", kMorePages, selectedPage, navTarget);
    ImGui::PopStyleVar();
    return navTarget;
}

// imgui only links items sideways when their vertical middles overlap, which the sidebar entries and the setting widgets
// rarely do, so a left/right move between both panes finds no target at all and gets dropped
static void ApplyPaneNavFallback(ImGuiWindow* sidebarWindow, ImGuiWindow* contentWindow, const SidebarNavTarget& sidebarNavTarget) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (!g.NavMoveSubmitted || (g.NavMoveDir != ImGuiDir_Left && g.NavMoveDir != ImGuiDir_Right)) {
        return;
    }
    if (g.NavMoveResultLocal.ID != 0 || g.NavMoveResultOther.ID != 0) {
        return;
    }

    if (g.NavMoveDir == ImGuiDir_Left && contentWindow != nullptr && g.NavWindow == contentWindow && sidebarWindow != nullptr && sidebarNavTarget.itemId != 0) {
        ImGui::SetNavWindow(sidebarWindow);
        ImGui::SetNavID(sidebarNavTarget.itemId, ImGuiNavLayer_Main, sidebarNavTarget.focusScopeId, sidebarNavTarget.rectRel);
        ImGui::SetNavCursorVisible(true);
    }
    else if (g.NavMoveDir == ImGuiDir_Right && sidebarWindow != nullptr && g.NavWindow == sidebarWindow && contentWindow != nullptr && (contentWindow->DC.NavLayersActiveMask & (1 << ImGuiNavLayer_Main)) != 0) {
        ImGui::SetNavWindow(contentWindow);
        ImGui::NavInitWindow(contentWindow, false);
        ImGui::SetNavCursorVisible(true);
    }
}

static void DrawSupporterWall() {
    if (Supporters::List.empty()) {
        return;
    }

    ImGui::SeparatorText("Supporters");
    ImGui::TextWrapped("These people are funding the development of the mod. Thank you!");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    int columns = std::clamp((int)(ImGui::GetContentRegionAvail().x / 260.0f), 1, 4);
    if (ImGui::BeginTable("##Supporters", columns, ImGuiTableFlags_NoSavedSettings)) {
        for (const auto& supporter : Supporters::List) {
            const char* icon = ICON_KI_STAR;
            if (supporter.platform == Supporters::Platform::GITHUB) {
                icon = ICON_KI_GITHUB;
            }
            else if (supporter.platform == Supporters::Platform::PATREON) {
                icon = ICON_KI_HEART;
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s %s", icon, supporter.name);
        }
        ImGui::EndTable();
    }
}

static void DrawCompactProfilerSummary() {
    auto snapshots = BetterVRProfiler::GetSnapshots();
    std::sort(snapshots.begin(), snapshots.end(), [](const BetterVRProfiler::Snapshot& lhs, const BetterVRProfiler::Snapshot& rhs) {
        return lhs.averageFrameMs > rhs.averageFrameMs;
    });

    ImGui::Separator();
    int rowsDrawn = 0;
    for (const auto& snapshot : snapshots) {
        if (std::max(snapshot.lastFrameMs, snapshot.averageFrameMs) < 0.01 && snapshot.maxFrameMs < 0.01) {
            continue;
        }

        ImGui::Text("%s %.2f/%.2f ms x%u", snapshot.name, snapshot.lastFrameMs, snapshot.averageFrameMs, snapshot.lastFrameCalls);
        rowsDrawn++;
        if (rowsDrawn >= 3) {
            break;
        }
    }

    if (rowsDrawn == 0) {
        ImGui::TextDisabled("Profiler warming up...");
    }
}

static void DrawDetailedProfilerSummary() {
    auto snapshots = BetterVRProfiler::GetSnapshots();
    std::sort(snapshots.begin(), snapshots.end(), [](const BetterVRProfiler::Snapshot& lhs, const BetterVRProfiler::Snapshot& rhs) {
        return std::max(lhs.lastFrameMs, lhs.averageFrameMs) > std::max(rhs.lastFrameMs, rhs.averageFrameMs);
    });

    ImGui::SeparatorText("BetterVR CPU Profiling");
    if (ImGui::Button("Reset BetterVR Profiling")) {
        BetterVRProfiler::Reset();
    }

    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##BetterVRProfiling", 7, tableFlags)) {
        return;
    }

    ImGui::TableSetupColumn("Section");
    ImGui::TableSetupColumn("Last Frame");
    ImGui::TableSetupColumn("Avg Frame");
    ImGui::TableSetupColumn("Max Frame");
    ImGui::TableSetupColumn("Calls");
    ImGui::TableSetupColumn("Last Call");
    ImGui::TableSetupColumn("Max Call");
    ImGui::TableHeadersRow();

    for (const auto& snapshot : snapshots) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(snapshot.name);

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f ms", snapshot.lastFrameMs);

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.3f ms", snapshot.averageFrameMs);

        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.3f ms", snapshot.maxFrameMs);

        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%u", snapshot.lastFrameCalls);

        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%.3f ms", snapshot.lastCallMs);

        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%.3f ms", snapshot.maxCallMs);
    }

    ImGui::EndTable();
}

static void DrawBetterVRProfiler(bool detailed) {
    if (detailed) {
        DrawDetailedProfilerSummary();
    }
    else {
        DrawCompactProfilerSummary();
    }
}

enum class OverlayProfilerMode : uint8_t {
    NONE = 0,
    COMPACT = 1,
    DETAILED = 2,
};

static void DrawFPSOverlayContent(RND_Renderer* renderer, bool renderText, OverlayProfilerMode profilerMode) {
    const float predictedDisplayPeriodMs = (float)renderer->GetPredictedDisplayPeriodMs();
    const float predictedHz = GetSettings().performanceOverlayFrequency;

    const float appMs = (float)renderer->GetLastFrameTimeMs();      // Total frame time (includes wait)
    const float workMs = (float)renderer->GetLastFrameWorkTimeMs(); // GPU Work time only (excludes wait)
    const float waitMs = (float)renderer->GetLastWaitTimeMs();
    const float overheadMs = (float)renderer->GetLastOverheadMs();

    // --- 2. Convert to FPS ---
    const float appFps = appMs > 0.0000001f ? (1000.0f / appMs) : 0.0f;

    // "Theoretical FPS": How fast you COULD run if you didn't have to wait for V-Sync/OpenXR
    const float workFps = workMs >= 0.0000001f ? (1000.0f / workMs) : 0.0f;

    // Calculate percentage of the frame budget used (still useful in % terms)
    const float workPct = predictedDisplayPeriodMs > 0.0f ? (workMs / predictedDisplayPeriodMs) * 100.0f : 0.0f;

    // --- 3. Text Summary ---
    if (renderText) {
        ImGui::Text("The visualization refresh rate of your headset is set to %.0f Hz", predictedHz);
        ImGui::Text("Currently Running At %.1f FPS", appFps);
        ImGui::Text("");
        ImGui::Text("OpenXR waited %.1f ms so that it can interpolate/have low latency.", waitMs);
        ImGui::Text("Theoretically, it'd run at %.1f FPS if that didn't matter", workFps);
    }

    if (predictedHz > 0.0f && workFps >= 0.0f) {
        auto rateForDivisor = [predictedHz](int divisor) -> double {
            return divisor > 0 ? (predictedHz / (double)divisor) : 0.0;
        };

        auto chooseBestDivisor = [&](double fps) -> int {
            // Pick the refresh divisor (1x, 1/2x, 1/3x, 1/4x) that is able to be achieved using the workFPS (frame time minus OpenXR wait time)
            for (int div = 1; div <= 4; ++div) {
                if (fps >= rateForDivisor(div)) {
                    return div;
                }
            }
            return 4;
        };

        // Use theoretical FPS (GPU work time) as the basis for which step we're closest to.
        const int currentDiv = chooseBestDivisor(workFps);
        const double currentTarget = rateForDivisor(currentDiv);
        const int nextDiv = std::max(1, currentDiv - 1);
        const double nextTarget = rateForDivisor(nextDiv);
        const double missingNext = std::max(0.0, nextTarget - (double)workFps);

        if (renderText) {
            if (currentDiv == 1) {
                ImGui::Text("Its reaching the full refresh rate you've set (%.0f hz)", currentTarget);
                ImGui::Text("You've got ~%.1f FPS of headroom to spare", (float)std::max(0.0, (double)workFps - nextTarget));
            }
            else {
                ImGui::Text("It is however able to reliably reach %.0f FPS (%.0f Hz / %d)", currentTarget, predictedHz, currentDiv);
                ImGui::Text("You'd need to get %.1f FPS more to get it to switch to %.0f FPS", missingNext, nextTarget);
            }
        }
    }

    // --- 4. History Buffers (Storing FPS now) ---
    static float history_app_fps[60] = {};
    static float history_work_fps[60] = {};
    static int offset = 0;

    history_app_fps[offset] = appFps;
    history_work_fps[offset] = workFps;
    offset = (offset + 1) % 60;

    // --- 5. Plotting ---
    const double targetFps = predictedHz;
    const double halfFps = predictedHz / 2.0;
    const double thirdFps = predictedHz / 3.0;
    const double fourthFps = predictedHz / 4.0;

    if (ImPlot::BeginPlot("##Frametime", ImVec2(300, 150), ImPlotFlags_NoFrame | ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes(nullptr, "##FPS", ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoInitialFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Always);

        if (targetFps >= 0.000000001f) {
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, predictedHz * 1.5f, ImPlotCond_Always);

            // 1. Target Refresh Rate (Green)
            ImPlot::SetNextLineStyle(ImVec4(0, 1, 0, 0.5f));
            ImPlot::PlotInfLines("##Target", &targetFps, 1, ImPlotInfLinesFlags_Horizontal);
            ImPlot::TagY(targetFps, ImVec4(0, 1, 0, 0.5f), "%.0f Hz", targetFps);

            // 2. Half Rate (ASW/Reprojection threshold) (Yellow)
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 0, 0.5f));
            ImPlot::PlotInfLines("##1/2 Rate", &halfFps, 1, ImPlotInfLinesFlags_Horizontal);
            ImPlot::TagY(halfFps, ImVec4(1, 1, 0, 0.5f), "%.0f Hz", halfFps);

            // 3. Third Rate (Red)
            ImPlot::SetNextLineStyle(ImVec4(1, 0, 0, 0.5f));
            ImPlot::PlotInfLines("##1/3 Rate", &thirdFps, 1, ImPlotInfLinesFlags_Horizontal);
        }
        else {
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 144.0, ImPlotCond_Always);
        }

        // --- Draw Graphs ---
        // 1. Theoretical Max FPS (Work Time) - Purple/Pink
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 1.0f, 1.0f));
        ImPlot::PlotLine("Theoretical Max", history_work_fps, 60, 1.0, 0.0, 0, offset);

        // 2. Actual FPS (App Time) - Blue
        // This represents what is actually hitting the screen (capped by Wait).
        ImPlot::SetNextFillStyle(ImVec4(0.4f, 0.4f, 1.0f, 0.50f));
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.0f, 1.0f));
        ImPlot::PlotShaded("Actual", history_app_fps, 60, 0.0, 1.0, 0.0, 0, offset);
        ImPlot::PlotLine("##TotalLine", history_app_fps, 60, 1.0, 0.0, 0, offset);

        // Current FPS Tag
        if (appFps > 0.0f) {
            const double currentFps = appFps;
            ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 0.65f));
            ImPlot::PlotInfLines("##Current", &currentFps, 1, ImPlotInfLinesFlags_Horizontal);
            ImPlot::TagY(currentFps, ImVec4(1, 1, 1, 0.8f), "%.1f FPS", currentFps);
        }

        ImPlot::EndPlot();
    }

    if (profilerMode != OverlayProfilerMode::NONE) {
        DrawBetterVRProfiler(profilerMode == OverlayProfilerMode::DETAILED);
    }
}

namespace WeaponAttackDebugger {
    struct WeaponLiveDebugState {
        bool hasWeapon = false;
        uint32_t weaponPtr = 0;
        std::string actorName;
        float baseDamage = 0.0f;
        float multiplier = 1.0f;
        float estimatedDamage = 0.0f;
        uint32_t setupDamage = 0;
        uint32_t finalizedDamage = 0;
        uint32_t damageMgrDamage = 0;
    };

    struct AttackPeriodSummary {
        AttackType attackType = AttackType::None;
        float damageIntegral = 0.0f;
        float duration = 0.0f;
        float peakDamage = 0.0f;
    };

    struct AttackPeriodTracker {
        bool wasActive = false;
        XrTime previousSampleTime = 0;
        AttackType currentAttackType = AttackType::None;
        float currentDamageIntegral = 0.0f;
        float currentDuration = 0.0f;
        float currentPeakDamage = 0.0f;
        std::array<AttackPeriodSummary, 3> history = {};
        size_t historyCount = 0;
    };

    constexpr ImVec4 kPassColor = ImVec4(0.35f, 0.95f, 0.45f, 1.0f);
    constexpr ImVec4 kFailColor = ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
    constexpr ImVec4 kNeutralColor = ImVec4(0.90f, 0.85f, 0.45f, 1.0f);
    constexpr uint32_t kMotionPlotSampleCount = 30;
    std::array<AttackPeriodTracker, 2> s_attackPeriodTrackers = {};
    std::array<bool, 2> s_showWeaponSensitivityOverlay = { false, false };

    const char* ToString(AttackType attackType) {
        switch (attackType) {
            case AttackType::Slash:
                return "Slash";
            case AttackType::Stab:
                return "Stab";
            default:
                return "None";
        }
    }

    const char* ToString(WeaponType weaponType) {
        switch (weaponType) {
            case WeaponType::SmallSword:
                return "Small Sword";
            case WeaponType::LargeSword:
                return "Large Sword";
            case WeaponType::Spear:
                return "Spear";
            case WeaponType::Bow:
                return "Bow";
            case WeaponType::Shield:
                return "Shield";
            default:
                return "Unknown";
        }
    }

    const char* ToString(OpenXR::EyeSide side) {
        return side == EyeSide::LEFT ? "Left Hand" : "Right Hand";
    }

    void DrawMotionPlots(OpenXR::EyeSide side, const WeaponMotionAnalyser& analyser);

    void DrawStatusValue(const char* label, float value, float threshold, const char* unit, bool passed, bool higherIsBetter = true) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", label);
        ImGui::SameLine(220.0f);
        const char* comparator = higherIsBetter ? ">=" : "<=";
        ImGui::TextColored(passed ? kPassColor : kFailColor, "%.3f %s  %s  %.3f %s", value, unit, comparator, threshold, unit);
    }

    void DrawStatusText(const char* label, const char* value, const ImVec4& color) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", label);
        ImGui::SameLine(220.0f);
        ImGui::TextColored(color, "%s", value);
    }

    float ClampProgress(float value, float maxValue) {
        if (maxValue <= 0.0f) {
            return 0.0f;
        }
        return std::clamp(value / maxValue, 0.0f, 1.0f);
    }

    WeaponLiveDebugState ReadWeaponLiveDebugState(uint32_t weaponPtr) {
        WeaponLiveDebugState state = {};
        if (weaponPtr == 0) {
            return state;
        }

        Weapon weapon = {};
        CemuHooks::readMemory(weaponPtr, &weapon);

        state.hasWeapon = true;
        state.weaponPtr = weaponPtr;
        state.actorName = weapon.name.getLE();
        state.setupDamage = weapon.setupAttackSensor.powerForPlayers.getLE();
        state.finalizedDamage = weapon.finalizedAttackSensor.powerForPlayers.getLE();
        state.multiplier = weapon.finalizedAttackSensor.multiplier.getLE();
        if (state.multiplier <= 0.0f) {
            state.multiplier = weapon.setupAttackSensor.multiplier.getLE();
        }
        if (state.multiplier <= 0.0f) {
            state.multiplier = 1.0f;
        }

        if (const uint32_t damageMgrPtr = weapon.damageMgrPtr.getLE(); damageMgrPtr != 0) {
            DamageMgr damageMgr = {};
            CemuHooks::readMemory(damageMgrPtr, &damageMgr);
            state.damageMgrDamage = damageMgr.damage.getLE();
        }

        state.baseDamage = (float)state.finalizedDamage;
        if (state.baseDamage <= 0.0f) {
            state.baseDamage = (float)state.setupDamage;
        }
        if (state.baseDamage <= 0.0f) {
            state.baseDamage = (float)state.damageMgrDamage;
        }

        state.estimatedDamage = state.baseDamage * state.multiplier;
        return state;
    }

    void PushHistoryEntry(AttackPeriodTracker& tracker) {
        if (tracker.currentDuration <= 0.0f && tracker.currentDamageIntegral <= 0.0f && tracker.currentPeakDamage <= 0.0f) {
            return;
        }

        for (size_t i = tracker.history.size() - 1; i > 0; --i) {
            tracker.history[i] = tracker.history[i - 1];
        }

        tracker.history[0] = AttackPeriodSummary {
            .attackType = tracker.currentAttackType,
            .damageIntegral = tracker.currentDamageIntegral,
            .duration = tracker.currentDuration,
            .peakDamage = tracker.currentPeakDamage,
        };
        tracker.historyCount = std::min(tracker.historyCount + 1, tracker.history.size());
    }

    void UpdateAttackPeriodTracker(OpenXR::EyeSide side, const WeaponDebugSnapshot& snapshot, const WeaponLiveDebugState& liveState) {
        auto& tracker = s_attackPeriodTrackers[side];
        const bool isActive = snapshot.attackActive || snapshot.hitboxEnabled;
        const float liveDamage = liveState.estimatedDamage;

        if (isActive && tracker.wasActive && snapshot.sampleTime > tracker.previousSampleTime) {
            const float dt = std::clamp((float)(snapshot.sampleTime - tracker.previousSampleTime) / 1e9f, 0.0f, 0.25f);
            tracker.currentDamageIntegral += liveDamage * dt;
            tracker.currentDuration += dt;
        }

        if (isActive) {
            tracker.currentAttackType = snapshot.lockedAttackType;
            tracker.currentPeakDamage = std::max(tracker.currentPeakDamage, liveDamage);
            tracker.previousSampleTime = snapshot.sampleTime;
        }
        else if (tracker.wasActive) {
            PushHistoryEntry(tracker);
            tracker.currentAttackType = AttackType::None;
            tracker.currentDamageIntegral = 0.0f;
            tracker.currentDuration = 0.0f;
            tracker.currentPeakDamage = 0.0f;
            tracker.previousSampleTime = snapshot.sampleTime;
        }

        if (!tracker.wasActive && isActive) {
            tracker.currentAttackType = snapshot.lockedAttackType;
            tracker.currentDamageIntegral = 0.0f;
            tracker.currentDuration = 0.0f;
            tracker.currentPeakDamage = liveDamage;
            tracker.previousSampleTime = snapshot.sampleTime;
        }

        if (!isActive && !tracker.wasActive) {
            tracker.previousSampleTime = snapshot.sampleTime;
        }

        tracker.wasActive = isActive;
    }

    void DrawAttackPeriodSummary(OpenXR::EyeSide side) {
        const auto& tracker = s_attackPeriodTrackers[side];
        ImGui::Text("Current period: %s | %.3f dmg*s | %.3f s | peak %.1f",
            ToString(tracker.currentAttackType),
            tracker.currentDamageIntegral,
            tracker.currentDuration,
            tracker.currentPeakDamage
        );

        if (tracker.historyCount == 0) {
            ImGui::TextDisabled("Last 3 periods: none yet");
            return;
        }

        ImGui::Text("Last 3 periods:");
        for (size_t i = 0; i < tracker.historyCount; ++i) {
            const auto& entry = tracker.history[i];
            ImGui::BulletText("%s | %.3f dmg*s | %.3f s | peak %.1f",
                ToString(entry.attackType),
                entry.damageIntegral,
                entry.duration,
                entry.peakDamage
            );
        }
    }

    void DrawSensitivityPanel(OpenXR::EyeSide side, const WeaponMotionAnalyser& analyser) {
        const WeaponDebugSnapshot snapshot = analyser.GetDebugSnapshot();
        const WeaponLiveDebugState liveState = ReadWeaponLiveDebugState(CemuHooks::m_heldWeapons[side]);
        UpdateAttackPeriodTracker(side, snapshot, liveState);

        ImGui::PushID((int)side);
        DrawStatusText("Held weapon", liveState.hasWeapon ? liveState.actorName.c_str() : "None", liveState.hasWeapon ? kPassColor : kNeutralColor);
        ImGui::Text("Weapon type: %s | Attack: %s | Active: %s | Hitbox: %s",
            ToString(snapshot.weaponType),
            ToString(snapshot.lockedAttackType),
            snapshot.attackActive ? "Yes" : "No",
            snapshot.hitboxEnabled ? "Yes" : "No"
        );
        ImGui::Text("Current damage: %.1f | Multiplier: %.2f | Estimated output: %.1f",
            liveState.baseDamage,
            liveState.multiplier,
            liveState.estimatedDamage
        );
        ImGui::Text("Bad samples: %.3f / %.3f s | Swing power: %.2f | Arm calibration: %.3f m",
            snapshot.badSampleAccumTime,
            snapshot.maxBadDuration,
            snapshot.swingPower,
            snapshot.calibratedArmLength
        );

        if (ImGui::CollapsingHeader("Stab", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawStatusValue("Stab speed", snapshot.forwardStabSpeed, snapshot.stabSpeedThreshold, "m/s", snapshot.stabSpeedOk);
            DrawStatusValue("Stab acceleration", snapshot.forwardStabAcceleration, snapshot.stabAccThreshold, "m/s^2", snapshot.stabAccelOk);
            DrawStatusValue("Stab direction", snapshot.stabDirectionAbsZ, snapshot.stabLinearSteadinessThreshold, "ratio", snapshot.stabDirectionOk);
            DrawStatusValue("Stab angular steadiness", snapshot.stabAngularXY, snapshot.stabAngularSteadinessThreshold, "rad/s", snapshot.stabAngularOk, false);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Stab duration");
            ImGui::SameLine(220.0f);
            ImGui::TextColored(snapshot.stabDurationOk ? kPassColor : kFailColor, "%.3f s / %.3f s", snapshot.goodStabAccumTime, snapshot.minGoodStabDuration);
            ImGui::SameLine();
            ImGui::ProgressBar(ClampProgress(snapshot.goodStabAccumTime, snapshot.minGoodStabDuration), ImVec2(120.0f, 0.0f));
        }

        if (ImGui::CollapsingHeader("Swing", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawStatusValue("Swing speed", snapshot.slashAngularSpeed, snapshot.slashSpeedThreshold, "rad/s", snapshot.slashSpeedOk);
            DrawStatusValue("Swing acceleration", snapshot.slashAngularAcceleration, snapshot.slashAccThreshold, "rad/s^2", snapshot.slashAccelOk);
            DrawStatusValue("Swing velocity gate", snapshot.slashAngularSpeed, snapshot.slashVelocityThreshold, "rad/s", snapshot.slashVelocityOk);
            DrawStatusValue("Swing drift", snapshot.slashAngularDrift, snapshot.slashAccDriftThreshold, "rad/s^2", snapshot.slashDriftOk, false);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Swing duration");
            ImGui::SameLine(220.0f);
            ImGui::TextColored(snapshot.slashDurationOk ? kPassColor : kFailColor, "%.3f s / %.3f s", snapshot.goodSwingAccumTime, snapshot.minGoodSwingDuration);
            ImGui::SameLine();
            ImGui::ProgressBar(ClampProgress(snapshot.goodSwingAccumTime, snapshot.minGoodSwingDuration), ImVec2(120.0f, 0.0f));
        }

        ImGui::SeparatorText("Attack Periods");
        DrawAttackPeriodSummary(side);

        if (ImGui::CollapsingHeader("Motion Graphs")) {
            DrawMotionPlots(side, analyser);
        }

        ImGui::PopID();
    }

    void DrawMotionPlots(OpenXR::EyeSide side, const WeaponMotionAnalyser& analyser) {
        const WeaponDebugSnapshot snapshot = analyser.GetDebugSnapshot();
        const auto& samples = analyser.GetSamples();
        const uint32_t lastSampleIdx = analyser.GetLastSampleIndex();
        const auto oldestIdx = [&](uint32_t j) { return (lastSampleIdx + 1 + WeaponMotionAnalyser::MAX_SAMPLES - kMotionPlotSampleCount + j) % WeaponMotionAnalyser::MAX_SAMPLES; };

        ImGui::PushID((int)side);
        ImGui::Text("%s", ToString(side));
        ImGui::Text("Weapon: %s | Showing last %u samples", ToString(snapshot.weaponType), kMotionPlotSampleCount);
        ImGui::Text("Attack: %s | Active: %s | Bad: %.3f s | Power: %.2f",
            ToString(snapshot.lockedAttackType),
            snapshot.attackActive ? "Yes" : "No",
            snapshot.badSampleAccumTime,
            snapshot.swingPower
        );

        {
            std::array<float, kMotionPlotSampleCount> t{}, avX{}, avY{}, avZ{}, maskSlash{}, maskStab{};
            for (uint32_t j = 0; j < kMotionPlotSampleCount; ++j) {
                const auto& s = samples[oldestIdx(j)];
                t[j] = static_cast<float>(j);
                avX[j] = s.localAngularVelocity.x;
                avY[j] = s.localAngularVelocity.y;
                avZ[j] = s.localAngularVelocity.z;
                maskSlash[j] = (s.attackType == AttackType::Slash) ? 100.0f : -100.0f;
                maskStab[j] = (s.attackType == AttackType::Stab) ? 100.0f : -100.0f;
            }

            if (ImPlot::BeginPlot("Weapon Steadiness", ImVec2(-1.0f, 120.0f), ImPlotFlags_NoTitle)) {
                ImPlot::SetupAxes("Sample", "Angular Velocity", ImPlotAxisFlags_Lock, ImPlotAxisFlags_Lock);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, kMotionPlotSampleCount - 1, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -15, 15, ImPlotCond_Always);

                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0, 0.3f), 3);
                ImPlot::PlotShaded("Slash", t.data(), maskSlash.data(), kMotionPlotSampleCount, -100.0f);
                ImPlot::SetNextLineStyle(ImVec4(1, 0.5f, 0, 0.3f), 3);
                ImPlot::PlotShaded("Stab", t.data(), maskStab.data(), kMotionPlotSampleCount, -100.0f);

                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0.5f, 0.5f), 2);
                ImPlot::PlotLine("X", t.data(), avX.data(), kMotionPlotSampleCount);
                ImPlot::PlotLine("Y", t.data(), avY.data(), kMotionPlotSampleCount);
                ImPlot::PlotLine("Z", t.data(), avZ.data(), kMotionPlotSampleCount);
                ImPlot::EndPlot();
            }
        }

        {
            std::array<float, kMotionPlotSampleCount> t{}, lvX{}, lvY{}, lvZ{}, accZ{}, maskSlash{}, maskStab{};
            for (uint32_t j = 0; j < kMotionPlotSampleCount; ++j) {
                const auto& s = samples[oldestIdx(j)];
                t[j] = static_cast<float>(j);
                lvX[j] = s.localLinearVelocity.x;
                lvY[j] = s.localLinearVelocity.y;
                lvZ[j] = s.localLinearVelocity.z;
                accZ[j] = s.smoothedLinearAcceleration.z;
                maskSlash[j] = (s.attackType == AttackType::Slash) ? 100.0f : -100.0f;
                maskStab[j] = (s.attackType == AttackType::Stab) ? 100.0f : -100.0f;
            }

            if (ImPlot::BeginPlot("Controller Linear Velocity", ImVec2(-1.0f, 120.0f), ImPlotFlags_NoTitle)) {
                ImPlot::SetupAxes("Sample", "Linear Velocity", ImPlotAxisFlags_Lock, ImPlotAxisFlags_Lock);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, kMotionPlotSampleCount - 1, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -6, 6, ImPlotCond_Always);

                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0, 0.01f), 3);
                ImPlot::PlotShaded("Slash", t.data(), maskSlash.data(), kMotionPlotSampleCount, -100.0f);
                ImPlot::SetNextLineStyle(ImVec4(1, 0.5f, 0, 0.01f), 3);
                ImPlot::PlotShaded("Stab", t.data(), maskStab.data(), kMotionPlotSampleCount, -100.0f);

                ImPlot::SetNextLineStyle(ImVec4(0, 1, 0.5f, 0.5f), 2);
                ImPlot::PlotLine("X", t.data(), lvX.data(), kMotionPlotSampleCount);
                ImPlot::PlotLine("Y", t.data(), lvY.data(), kMotionPlotSampleCount);
                ImPlot::PlotLine("Z", t.data(), lvZ.data(), kMotionPlotSampleCount);
                ImPlot::PlotLine("Acc Z (smoothed)", t.data(), accZ.data(), kMotionPlotSampleCount);
                ImPlot::EndPlot();
            }
        }

        if (side == EyeSide::LEFT) {
            ImGui::Separator();
        }
        ImGui::PopID();
    }
}

namespace ImGuiMenus {
    ImFont* g_titleFont = nullptr;

    bool IsPageAvailable(uint8_t page) {
        if (page == DEBUG_PAGE) {
            return GetSettings().IsDebuggingToolsEnabled();
        }
        return page < PAGE_COUNT;
    }

    void DrawFPSOverlay(RND_Renderer* renderer) {
        ImGui::SetNextWindowBgAlpha(0.4f);

        ImVec2 windowSize = ImGui::GetIO().DisplaySize;

        const ImVec2 pad(10.0f, 10.0f);
        ImGui::SetNextWindowPos(ImVec2(windowSize.x - pad.x, pad.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        OverlayProfilerMode profilerMode = OverlayProfilerMode::NONE;
        if (GetSettings().performanceOverlay == PerformanceOverlayMode::WINDOW_AND_VR_WITH_PROFILER) {
            profilerMode = OverlayProfilerMode::COMPACT;
        }

        if (ImGui::Begin("AppMS Overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove)) {
            DrawFPSOverlayContent(renderer, false, profilerMode);
        }
        ImGui::End();
    }

    void DrawWeaponSensitivityOverlays() {
        for (uint8_t sideIdx = 0; sideIdx < WeaponAttackDebugger::s_showWeaponSensitivityOverlay.size(); ++sideIdx) {
            if (!WeaponAttackDebugger::s_showWeaponSensitivityOverlay[sideIdx]) {
                continue;
            }

            OpenXR::EyeSide side = (OpenXR::EyeSide)sideIdx;
            std::string windowName = std::format("Attack Debugger - {}", WeaponAttackDebugger::ToString(side));
            if (ImGui::Begin(windowName.c_str(), &WeaponAttackDebugger::s_showWeaponSensitivityOverlay[sideIdx])) {
                WeaponAttackDebugger::DrawSensitivityPanel(side, CemuHooks::m_motionAnalyzers[side]);
            }
            ImGui::End();
        }
    }

    bool IsWeaponSensitivityOverlayVisible(OpenXR::EyeSide side) {
        return WeaponAttackDebugger::s_showWeaponSensitivityOverlay[side];
    }

    void SetWeaponSensitivityOverlayVisible(OpenXR::EyeSide side, bool visible) {
        WeaponAttackDebugger::s_showWeaponSensitivityOverlay[side] = visible;
    }

}

void RND_Renderer::ImGuiOverlay::DrawPlaystylePage(bool* changed) {
    auto& settings = GetSettings();
    const CameraMode cameraMode = settings.cameraMode;

    DrawPageHeader(ICON_KI_PLAYER, "Playstyle", "How you want to inhabit Hyrule.");

    if (BeginSettingsSection("##CameraMode")) {
        DrawSetting("Camera Mode", "First person puts you inside Link's body with motion controls. Third person keeps the original camera and plays with a regular controller.", [&]() {
            settings.cameraMode.AddRadioToGUI(changed, true);
        });
        EndSettingsSection();
    }

    if (cameraMode == CameraMode::THIRD_PERSON) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped(ICON_KI_INFO_CIRCLE "  Use a regular controller and set up its input in Cemu!");
        ImGui::PopStyleColor();

        if (BeginSettingsSection("##ThirdPerson", "Third-Person Camera")) {
            DrawSetting("Camera Distance", "How far the camera floats behind Link.", [&]() {
                settings.thirdPlayerDistance.AddSliderToGUI(changed, 0.5f, 0.65f);
            });
            DrawSetting("Shoot Arrows From Camera", "Aims the bow from the camera's point of view instead of from Link's hands.", [&]() {
                settings.thirdPersonBowCameraAim.AddToGUI(changed);
            });
            EndSettingsSection();
        }
    }
    else {
        if (BeginSettingsSection("##FirstPerson", "Body & Position")) {
            DrawSetting("Play Position", "Standing lets your real-world steps move Link around the room. Pick Seated to keep Link's body planted while you sit.", [&]() {
                settings.playMode.AddRadioToGUI(changed, true);
            });
            DrawSetting("Height Offset", "Raises or lowers your eye height in the game.", [&]() {
                auto formatHeight = [&](float height) {
                    if (height < -.01f) {
                        float heightInches = height * -39.3700787f;
                        int32_t heightFeet = std::floor(heightInches / 12.0f);
                        heightInches -= heightFeet * 12.0f;
                        return std::format("-{0:.02f}m / {1}\'{2:.02f}\"", -height, heightFeet, heightInches);
                    }
                    if (height > .01f) {
                        float heightInches = height * 39.3700787f;
                        int32_t heightFeet = std::floor(heightInches / 12.0f);
                        heightInches -= heightFeet * 12.0f;
                        return std::format("+{0:.02f}m / {1}\'{2:.02f}\"", height, heightFeet, heightInches);
                    }
                    return std::string("0.0");
                };
                settings.playerHeightOffset.AddToGUI(changed, -0.5f, 1.0f, formatHeight);
            });
            EndSettingsSection();
        }
    }
}

void RND_Renderer::ImGuiOverlay::DrawComfortPage(bool* changed) {
    auto& settings = GetSettings();
    const CameraMode cameraMode = settings.cameraMode;

    DrawPageHeader(ICON_KI_BOOT, "Comfort", "Options that keep the camera predictable and motion sickness away.");

    if (cameraMode == CameraMode::FIRST_PERSON) {
        if (BeginSettingsSection("##Turning", "Turning")) {
            DrawSetting("Turn Mode", "Snap turning rotates you in fixed steps, which most players find more comfortable than smooth turning.", [&]() {
                settings.turnMode.AddComboToGUI(changed);
            });
            EndSettingsSection();
        }
    }

    if (BeginSettingsSection("##Cutscenes", "Cutscenes")) {
        if (cameraMode == CameraMode::FIRST_PERSON) {
            DrawSetting("Camera In Cutscenes", "Whether story cutscenes play out through Link's eyes or through the game's original third-person camera.", [&]() {
                settings.cutsceneCameraMode.AddComboToGUI(changed);
            });
        }
        DrawSetting("Black Bars In Third-Person Cutscenes", "Frames third-person cutscenes with cinematic black bars.", [&]() {
            settings.useBlackBarsForCutscenes.AddToGUI(changed);
        });
        if (cameraMode == CameraMode::FIRST_PERSON) {
            DrawSetting("Keep First-Person Cutscenes Still", "Never lets a cutscene move the first-person camera for you.", [&]() {
                settings.alwaysPreventFirstPersonCutsceneCameraMovement.AddToGUI(changed);
            });
        }
        EndSettingsSection();
    }

    if (cameraMode == CameraMode::FIRST_PERSON) {
        if (BeginSettingsSection("##Body", "Body")) {
            DrawSetting("Prevent Ragdolling", "Stops Link from being knocked down and dragged around, which can be disorienting in first person.", [&]() {
                settings.preventFirstPersonRagdoll.AddToGUI(changed);
            });
            EndSettingsSection();
        }
    }
    else {
        DrawPageNote("More comfort options become available in First Person mode.");
    }
}

void RND_Renderer::ImGuiOverlay::DrawCombatPage(bool* changed) {
    auto& settings = GetSettings();

    DrawPageHeader(ICON_KI_SWORD, "Combat", "How your swings and stabs register.");

    if (settings.cameraMode != CameraMode::FIRST_PERSON) {
        DrawPageNote("Motion-based combat is only used in First Person mode. Switch the Camera Mode on the Playstyle page to use these options.");
        return;
    }

    if (BeginSettingsSection("##Attacks", "Melee Attacks")) {
        DrawSetting("Attack Sensitivity", "How decisive a motion needs to be before it counts as an attack. Relaxed lets light swings land, while Normal asks for a more deliberate motion.", [&]() {
            settings.swingSensitivity.AddRadioToGUI(changed);
        });
        EndSettingsSection();
    }
}

void RND_Renderer::ImGuiOverlay::DrawControlsPage(bool* changed) {
    auto& settings = GetSettings();

    DrawPageHeader(ICON_KI_GAMEPAD, "Controls", "How your VR controllers translate into game input.");

    if (BeginSettingsSection("##Sticks", "Thumbsticks")) {
        if (settings.cameraMode == CameraMode::FIRST_PERSON) {
            DrawSetting("Walking Direction", "Walk in the direction you are looking, or in the direction your controller points.", [&]() {
                settings.walkingDirection.AddComboToGUI(changed);
            });
        }
        DrawSetting("Thumbstick Deadzone", "Ignores small stick movements, preventing drift when the stick rests slightly off-center.", [&]() {
            settings.stickDeadzone.AddPercentToGUI(changed, 0.0f, 50.0f);
        });
        DrawSetting("Stick Direction Threshold", "How far the stick must be pushed before it counts as a direction press in menus.", [&]() {
            settings.axisThreshold.AddPercentToGUI(changed, 10.0f, 90.0f);
        });
        EndSettingsSection();
    }

    if (VRManager::instance().XR->m_capabilities.isOculusLinkRuntime) {
        if (BeginSettingsSection("##Fixes", "Controller Fixes")) {
            DrawSetting("Angular Velocity Fixer", "Works around Oculus Link reporting controller rotation speeds incorrectly, which breaks swing detection. Leave this on Auto unless swings feel wrong.", [&]() {
                settings.buggyAngularVelocity.AddComboToGUI(changed);
            });
            EndSettingsSection();
        }
    }
    else {
        settings.buggyAngularVelocity = AngularVelocityFixerMode::AUTO;
    }
}

void RND_Renderer::ImGuiOverlay::DrawInterfacePage(bool* changed) {
    auto& settings = GetSettings();

    DrawPageHeader(ICON_KI_UI, "Interface", "Where the game's menus, HUD and aiming aids appear in your view.");

    if (BeginSettingsSection("##Hud", "HUD & Menus")) {
        DrawSetting("UI Follows Where You Look", "Gently re-centers the HUD and menus as you look around.", [&]() {
            settings.uiFollowsGaze.AddToGUI(changed);
        });
        DrawSetting("Menu & HUD Placement", "How far away and how large the game's menus and HUD appear.", [&]() {
            float distance = settings.hudDistance;
            if (ImGui::SliderFloat("##hudDistance", &distance, settings.hudDistance.min, settings.hudDistance.max, FormatDistance(distance).c_str())) {
                settings.hudDistance = std::clamp(distance, settings.hudDistance.min, settings.hudDistance.max);
                *changed = true;
            }
            ImGui::PushItemWidth(-(ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x));
            float size = settings.hudSize;
            if (ImGui::SliderFloat("##hudSize", &size, settings.hudSize.min, settings.hudSize.max, "%.2fx size")) {
                settings.hudSize = std::clamp(size, settings.hudSize.min, settings.hudSize.max);
                *changed = true;
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                settings.hudDistance.Reset();
                settings.hudSize.Reset();
                *changed = true;
            }
        });
        EndSettingsSection();
    }

    if (BeginSettingsSection("##Aiming", "Aiming")) {
        DrawSetting("Bow Arc Opacity", "Visibility of the predicted arrow arc while aiming your bow.", [&]() {
            settings.bowArcOpacity.AddPercentToGUI(changed, 0.0f, 100.0f);
        });
        EndSettingsSection();
    }
}

void RND_Renderer::ImGuiOverlay::DrawSystemPage(bool* changed) {
    auto& settings = GetSettings();

    DrawPageHeader(ICON_KI_COG, "System", "Startup behavior and maintenance.");

    if (BeginSettingsSection("##Startup", "Startup")) {
        DrawSetting("Boot Directly Into BotW", "Skips Cemu's game list and launches straight into Breath of the Wild.", [&]() {
            settings.bootDirectlyIntoGame.AddToGUI(changed);
        });
        EndSettingsSection();
    }

    if (BeginSettingsSection("##Developer", "Developer")) {
        DrawSetting("Enable Debugger Tools", "Unlocks the Debug page with entity inspection and logging options. Reduces performance while enabled.", [&]() {
            settings.enableDebuggerTools.AddToGUI(changed);
        });
        EndSettingsSection();
    }

    if (BeginSettingsSection("##Maintenance", "Maintenance")) {
        DrawSetting("Reset All Settings", "Puts every BetterVR setting back to its default value.", [&]() {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.12f, 0.12f, 1.0f));
            if (ImGui::Button("Reset Everything")) {
                for (ModSettingBase* option : settings.GetOptions()) {
                    option->Reset();
                }
                *changed = true;
            }
            ImGui::PopStyleColor(3);
        });
        EndSettingsSection();
    }
}

static void DrawLogToggle(const char* label, BoolSetting& setting, bool* changed) {
    bool enabled = setting;
    if (ImGui::Checkbox(label, &enabled)) {
        setting = enabled;
        *changed = true;
    }
}

void RND_Renderer::ImGuiOverlay::DrawDebugPage(bool* changed) {
    DrawPageHeader(ICON_KI_WRENCH, "Debug", "Inspection and logging tools for mod development.");

    auto* entityDebugger = VRManager::instance().Hooks->m_entityDebugger.get();
    if (entityDebugger != nullptr) {
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::SeparatorText("World-Space Overlay");
        entityDebugger->DrawWorldSpaceOverlaySettings(changed);
    }

    if (entityDebugger != nullptr) {
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::SeparatorText("Entity Debugger");
        entityDebugger->DrawEntityInspectorContent();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SeparatorText("Attack Debugger");
    bool showLeftOverlay = ImGuiMenus::IsWeaponSensitivityOverlayVisible(EyeSide::LEFT);
    if (ImGui::Checkbox("Left Hand", &showLeftOverlay)) {
        ImGuiMenus::SetWeaponSensitivityOverlayVisible(EyeSide::LEFT, showLeftOverlay);
    }
    ImGui::SameLine();
    bool showRightOverlay = ImGuiMenus::IsWeaponSensitivityOverlayVisible(EyeSide::RIGHT);
    if (ImGui::Checkbox("Right Hand", &showRightOverlay)) {
        ImGuiMenus::SetWeaponSensitivityOverlayVisible(EyeSide::RIGHT, showRightOverlay);
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SeparatorText("Log Categories");

    auto& settings = GetSettings();
    DrawLogToggle("Rendering", settings.logRendering, changed);
    DrawLogToggle("Interop", settings.logInterop, changed);
    DrawLogToggle("Controls", settings.logControls, changed);
    DrawLogToggle("PPC Hooks", settings.logPpc, changed);
    DrawLogToggle("OpenXR Debug Utils", settings.logXrDebugUtils, changed);
    DrawLogToggle("Arrow Shot Capture", settings.logArrowShotCapture, changed);
    DrawLogToggle("Verbose", settings.logVerbose, changed);

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SeparatorText("Log Line Prefix");
    DrawLogToggle("Timestamps", settings.logTimestamps, changed);
    DrawLogToggle("Process & Thread IDs", settings.logThreadIds, changed);
}

void RND_Renderer::ImGuiOverlay::DrawGuidePage() {
    if (m_helpImages.empty()) {
        return;
    }

    auto moveHelpImage = [&](int direction) {
        const int helpImageCount = (int)m_helpImages.size();
        m_currentHelpImage = (uint32_t)((m_currentHelpImage + helpImageCount + direction) % helpImageCount);
    };

    const bool hasMultipleHelpImages = m_helpImages.size() > 1;
    m_currentHelpImage = std::min<uint32_t>(m_currentHelpImage, (uint32_t)m_helpImages.size() - 1);
    const auto& image = m_helpImages[m_currentHelpImage];

    std::string slideText = std::format("{} ({}/{})", image.m_title, m_currentHelpImage + 1, m_helpImages.size());
    float availableWidth = ImGui::GetContentRegionAvail().x;
    float sideWidth = availableWidth * 0.4f;
    float centerWidth = availableWidth * 0.2f;
    float rowStartX = ImGui::GetCursorPosX();
    float buttonWidth = ImGui::CalcTextSize(ICON_KI_ARROW_RIGHT).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float leftButtonX = rowStartX + std::max((sideWidth - buttonWidth) * 0.5f, 0.0f);
    float textWidth = ImGui::CalcTextSize(slideText.c_str()).x;
    float textX = rowStartX + sideWidth + std::max((centerWidth - textWidth) * 0.5f, 0.0f);
    float rightButtonX = rowStartX + sideWidth + centerWidth + std::max((sideWidth - buttonWidth) * 0.5f, 0.0f);

    ImGui::SetCursorPosX(leftButtonX);
    if (!hasMultipleHelpImages) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(ICON_KI_ARROW_LEFT "##PrevHelpImage")) {
        moveHelpImage(-1);
    }
    if (!hasMultipleHelpImages) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine(textX);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(slideText.c_str());

    ImGui::SameLine(rightButtonX);
    if (!hasMultipleHelpImages) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(ICON_KI_ARROW_RIGHT "##NextHelpImage")) {
        moveHelpImage(1);
    }
    if (!hasMultipleHelpImages) {
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    if (ImGui::BeginChild("HelpImageCarousel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        ImVec2 availableSize = ImGui::GetContentRegionAvail();
        float widthScale = availableSize.x / (float)image.m_image->GetWidth();
        float heightScale = availableSize.y / (float)image.m_image->GetHeight();
        float imageScale = std::min(std::min(widthScale, heightScale), 1.0f);
        ImVec2 imageSize = ImVec2((float)image.m_image->GetWidth() * imageScale, (float)image.m_image->GetHeight() * imageScale);

        ImVec2 cursorPos = ImGui::GetCursorPos();
        if (availableSize.x > imageSize.x) {
            cursorPos.x += (availableSize.x - imageSize.x) * 0.5f;
        }
        if (availableSize.y > imageSize.y) {
            cursorPos.y += (availableSize.y - imageSize.y) * 0.5f;
        }
        ImGui::SetCursorPos(cursorPos);
        ImGui::Image((ImTextureID)image.m_imageDS, imageSize);
    }
    ImGui::EndChild();
}

void RND_Renderer::ImGuiOverlay::DrawPerformancePage(bool* changed) {
    auto& settings = GetSettings();

    DrawPageHeader(ICON_KI_GAUGE, "Performance", "Check how smoothly the mod is running and configure the FPS overlay.");

    if (BeginSettingsSection("##Overlay", "FPS Overlay")) {
        DrawSetting("Show FPS Overlay", "Draws a live FPS graph in the corner while you play.", [&]() {
            settings.performanceOverlay.AddComboToGUI(changed);
        });
        if (settings.performanceOverlay != PerformanceOverlayMode::DISABLE) {
            static const uint32_t freqOptions[] = { 30, 60, 72, 80, 90, 120, 144 };
            uint32_t currentFreq = settings.performanceOverlayFrequency;
            int freqIdx = 5;
            for (int i = 0; i < std::size(freqOptions); i++) {
                if (freqOptions[i] == currentFreq) {
                    freqIdx = i;
                    break;
                }
            }

            DrawSetting("Headset Refresh Rate", "The refresh rate your headset runs at, used to draw the graph's target lines.", [&]() {
                if (ImGui::SliderInt("##RefreshRate", &freqIdx, 0, (int)std::size(freqOptions) - 1, std::format("{} Hz", freqOptions[freqIdx]).c_str())) {
                    settings.performanceOverlayFrequency = freqOptions[freqIdx];
                    *changed = true;
                }
            });
        }
        EndSettingsSection();
    }

    if (settings.performanceOverlay != PerformanceOverlayMode::DISABLE) {
        DrawSectionCaption("Live Frame Timings");
        DrawFPSOverlayContent(VRManager::instance().XR->GetRenderer(), true, OverlayProfilerMode::NONE);
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    DrawBetterVRProfiler(true);
}

void RND_Renderer::ImGuiOverlay::DrawCreditsPage() {
    DrawPageHeader(ICON_KI_HEART, "Credits", "The people who make BetterVR possible.");

    ImGui::SeparatorText("Project Links");
    ImGui::TextLinkOpenURL(ICON_KI_GITHUB " https://github.com/Crementif/BotW-BetterVR");
    ImGui::Text("");

    ImGui::SeparatorText("Donate to Support Development");
    ImGui::TextWrapped("Hey there!");
    ImGui::Text("");
    ImGui::TextWrapped("This mod is free and open-source, but it took a lot of late nights to create.");
    ImGui::TextWrapped("If you're enjoying the mod and want to vote on new features, you can donate here. Thanks!");
    ImGui::TextLinkOpenURL("https://github.com/sponsors/Crementif/");
    ImGui::Text("");
    ImGui::Text("- Crementif");

    DrawSupporterWall();

    ImGui::SeparatorText("Credits");
    ImGui::Text("Crementif: Main Developer");
    ImGui::Text("Holydh: Inputs and Gestures");
    ImGui::Text("Acudofy: Sword and Stab Analysis System");
    ImGui::Text("leoetlino: Made the BotW Decomp project, which was useful while reverse engineering");
    ImGui::Text("Exzap: Technical support and optimization help");
    ImGui::Text("Mako Marci: Made Logo, Controller Guide and Trailer");
    ImGui::Text("Tim, Mako Marci, Solarwolf07, Elliott Tate and Derra: Helped with QA testing, recording, feedback and support");
    ImGui::Text("");

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
}

static const char* GetControllerModMenuPrompt() {
    auto& caps = VRManager::instance().XR->m_capabilities;
    switch (caps.activeControllerType) {
        case ControllerType::OculusTouch:
        case ControllerType::Pico4:
        case ControllerType::HPReverbG2:
        case ControllerType::ViveCosmos:
            return "Hold the " ICON_KI_BUTTON_X " button on your left controller for 1 second to open the mod menu.\nAlternatively, for regular gamepad controllers, use the " ICON_KI_BUTTON_START " button.";
        case ControllerType::ValveIndex:
            return "Hold the " ICON_KI_BUTTON_A " button on your left controller for 1 second to open the mod menu.\nAlternatively, for regular gamepad controllers, use the " ICON_KI_BUTTON_START " button.";
        default:
            return "Hold the " ICON_KI_BUTTON_START " button on your gamepad from Cemu's input settings for 1 second to open the mod menu.";
    }
}

static const char* GetControllerMenuNavPrompt() {
    auto& caps = VRManager::instance().XR->m_capabilities;
    switch (caps.activeControllerType) {
        case ControllerType::OculusTouch:
        case ControllerType::Pico4:
        case ControllerType::HPReverbG2:
        case ControllerType::ViveCosmos:
        case ControllerType::ValveIndex:
            return ICON_KI_STICK_LEFT_TOP " Navigate      " ICON_KI_BUTTON_A " Select      " ICON_KI_BUTTON_B " Exit      " ICON_KI_BUTTON_LT " " ICON_KI_BUTTON_RT " Switch Page";
        default:
            return ICON_KI_STICK_LEFT_TOP " Navigate      " ICON_KI_BUTTON_A " Select      " ICON_KI_BUTTON_B " Exit      " ICON_KI_BUTTON_L " " ICON_KI_BUTTON_R " Switch Page";
    }
}

static void DrawTutorialPromptAndControllerVerification(bool isMenuOpen) {
    auto& settings = GetSettings();

    float alphaForNotify = settings.tutorialPromptShown ? 0.9f : 0.98f;
    float timeLimit = settings.tutorialPromptShown ? 14.0f : 60.0f;

    if (!settings.tutorialPromptShown) {
        if (isMenuOpen || ImGui::GetTime() > timeLimit) {
            settings.tutorialPromptShown = true;
            ImGui::SaveIniSettingsToDisk("BetterVR_settings.ini");
        }
    }

    if (isMenuOpen || ImGui::GetTime() >= timeLimit) {
        return;
    }

    ImVec2 fullWindowWidth = ImVec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    ImGui::SetNextWindowBgAlpha(alphaForNotify);
    ImGui::SetNextWindowPos(fullWindowWidth * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("HelpNotify", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs)) {
        if (!settings.tutorialPromptShown) {
            ImGui::Text("First-Time Setup:");
            ImGui::Separator();
            ImGui::Text("To get started, open the BetterVR menu to configure various settings and to see the controller guide.");
            ImGui::Text("This is where you can adjust the camera mode, player height, and other options to suit your preferences.");
            ImGui::Spacing();
            ImGui::Text("%s", GetControllerModMenuPrompt());
            ImGui::Text("");
            ImGui::Text("TO CONTINUE: Try holding the button and open the menu");
        }
        else {
            ImGui::Text("%s", GetControllerModMenuPrompt());
        }
    }
    ImGui::End();
}

void RND_Renderer::ImGuiOverlay::DrawHelpMenu() {
    auto& isMenuOpen = VRManager::instance().XR->m_isMenuOpen;
    auto& settings = GetSettings();

    DrawTutorialPromptAndControllerVerification(isMenuOpen);

    static bool wasMenuPrevOpened = false;
    static bool shouldLogSavedSettingsOnClose = false;

    if (!isMenuOpen && wasMenuPrevOpened) {
        wasMenuPrevOpened = false;
        if (shouldLogSavedSettingsOnClose) {
            Settings_LogSavedSettings();
            shouldLogSavedSettingsOnClose = false;
        }
    }

    if (!isMenuOpen) {
        return;
    }

    if (isMenuOpen && !wasMenuPrevOpened) {
        ImGui::SetNextWindowFocus();
        wasMenuPrevOpened = true;
        shouldLogSavedSettingsOnClose = false;
    }

    ImVec2 displaySize = ImVec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    ImVec2 windowSize = displaySize * ImVec2(0.925f, 1.0f);

    auto& selectedPage = VRManager::instance().XR->m_currMenuTab;
    if (!ImGuiMenus::IsPageAvailable(selectedPage)) {
        selectedPage = ImGuiMenus::PLAYSTYLE_PAGE;
    }

    bool shouldStayOpen = true;

    ImGui::SetNextWindowPos(displaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    if (ImGui::Begin("BetterVR Settings & Help##Settings", &shouldStayOpen, ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        bool changed = false;
        float footerHeight = ImGui::GetFrameHeight() * 1.5f;
        ImGuiWindow* sidebarWindow = nullptr;
        ImGuiWindow* contentWindow = nullptr;
        SidebarNavTarget sidebarNavTarget;

        float sidebarWidth = std::min(215.0f, windowSize.x * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05490196f, 0.07843138f, 0.09411765f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 12.0f));
        if (ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, -footerHeight), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding)) {
            sidebarWindow = ImGui::GetCurrentWindow();
            sidebarNavTarget = DrawSidebar(selectedPage);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::SameLine(0.0f, 0.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 8.0f));
        ImGui::PushID(selectedPage);
        if (ImGui::BeginChild("Content", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            contentWindow = ImGui::GetCurrentWindow();
            switch (selectedPage) {
                case ImGuiMenus::PLAYSTYLE_PAGE: DrawPlaystylePage(&changed); break;
                case ImGuiMenus::COMFORT_PAGE: DrawComfortPage(&changed); break;
                case ImGuiMenus::COMBAT_PAGE: DrawCombatPage(&changed); break;
                case ImGuiMenus::CONTROLS_PAGE: DrawControlsPage(&changed); break;
                case ImGuiMenus::INTERFACE_PAGE: DrawInterfacePage(&changed); break;
                case ImGuiMenus::PERFORMANCE_PAGE: DrawPerformancePage(&changed); break;
                case ImGuiMenus::SYSTEM_PAGE: DrawSystemPage(&changed); break;
                case ImGuiMenus::GUIDE_PAGE: DrawGuidePage(); break;
                case ImGuiMenus::CREDITS_PAGE: DrawCreditsPage(); break;
                case ImGuiMenus::DEBUG_PAGE: DrawDebugPage(&changed); break;
                default: break;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar();

        if (settings.bootDirectlyIntoGame) {
            CemuHooks* hooks = VRManager::instance().Hooks.get();
            if (hooks != nullptr) {
                const uint64_t currentTitleId = hooks->GetCurrentTitleId();
                if (currentTitleId != 0) {
                    const std::string formattedTitleId = std::format("{:016X}", currentTitleId);
                    if (settings.bootDirectlyTitleId.Get() != formattedTitleId) {
                        settings.bootDirectlyTitleId.Set(formattedTitleId);
                        changed = true;
                    }
                }
            }
        }

        if (changed) {
            shouldLogSavedSettingsOnClose = true;
            ImGui::SaveIniSettingsToDisk("BetterVR_settings.ini");
        }

        ImGui::Separator();
        ImGui::Spacing();
        const char* navText = GetControllerMenuNavPrompt();
        float textWidth = ImGui::CalcTextSize(navText).x;
        float availW = ImGui::GetContentRegionAvail().x;
        if (availW > textWidth) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - textWidth) * 0.5f);
        }
        ImGui::TextUnformatted(navText);

        ApplyPaneNavFallback(sidebarWindow, contentWindow, sidebarNavTarget);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);

    if (!shouldStayOpen) {
        isMenuOpen = false;
    }
}
