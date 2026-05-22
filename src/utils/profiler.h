#pragma once

class BetterVRProfiler {
public:
    enum class Section : uint8_t {
        RoomscaleResolve = 0,
        RoomscaleBeginHook,
        WeaponHandHook,
        WeaponAttackHook,
        XRUpdateSpaces,
        XRUpdateActions,
        XRLocateViews,
        Layer3DRender,
        Layer2DRender,
        ImGuiUpdate,
        ImGuiRender,
        ImGuiDrawAndCopy,
        Count
    };

    static constexpr uint32_t kAverageWindowFrames = 120;
    static constexpr size_t kSectionCount = (size_t)Section::Count;

    struct Snapshot {
        const char* name = "";
        double lastFrameMs = 0.0;
        double averageFrameMs = 0.0;
        double maxFrameMs = 0.0;
        double lastCallMs = 0.0;
        double maxCallMs = 0.0;
        uint32_t lastFrameCalls = 0;
    };

    class Scope {
    public:
        explicit Scope(Section section): m_section(section) {
            if (!BetterVRProfiler::IsEnabled()) {
                return;
            }

            m_isActive = true;
            m_start = Clock::now();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        ~Scope() {
            if (!m_isActive) {
                return;
            }

            BetterVRProfiler::Record(m_section, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start));
        }

    private:
        using Clock = std::chrono::steady_clock;

        Section m_section;
        bool m_isActive = false;
        Clock::time_point m_start;
    };

    static constexpr uint32_t GetAverageWindowFrames() {
        return kAverageWindowFrames;
    }

    static bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

    static void SetEnabled(bool enabled) {
        bool wasEnabled = s_enabled.load(std::memory_order_relaxed);
        if (wasEnabled == enabled) {
            return;
        }

        s_enabled.store(enabled, std::memory_order_relaxed);
        if (wasEnabled && !enabled) {
            ResetPendingFrame();
        }
    }

    static void Record(Section section, std::chrono::nanoseconds duration) {
        if (!IsEnabled()) {
            return;
        }

        uint64_t durationNs = duration.count() > 0 ? (uint64_t)duration.count() : 0ull;
        auto& state = s_states[(size_t)section];
        state.pendingFrameTotalNs.fetch_add(durationNs, std::memory_order_relaxed);
        state.pendingFrameCalls.fetch_add(1, std::memory_order_relaxed);
        state.lastCallNs.store(durationNs, std::memory_order_relaxed);
        UpdateMax(state.maxCallNs, durationNs);
    }

    static void BeginSpan(Section section) {
        if (!IsEnabled()) {
            return;
        }

        s_states[(size_t)section].activeSpanStartNs.store(GetTimestampNs(), std::memory_order_relaxed);
    }

    static void EndSpan(Section section) {
        if (!IsEnabled()) {
            return;
        }

        auto& state = s_states[(size_t)section];
        uint64_t startNs = state.activeSpanStartNs.exchange(0, std::memory_order_relaxed);
        if (startNs == 0) {
            return;
        }

        uint64_t endNs = GetTimestampNs();
        Record(section, std::chrono::nanoseconds((std::chrono::nanoseconds::rep)(endNs > startNs ? (endNs - startNs) : 0ull)));
    }

    static void AdvanceFrame() {
        if (!IsEnabled()) {
            return;
        }

        for (auto& state : s_states) {
            uint64_t frameTotalNs = state.pendingFrameTotalNs.exchange(0, std::memory_order_relaxed);
            uint32_t frameCalls = state.pendingFrameCalls.exchange(0, std::memory_order_relaxed);

            state.lastFrameTotalNs.store(frameTotalNs, std::memory_order_relaxed);
            state.lastFrameCalls.store(frameCalls, std::memory_order_relaxed);
            UpdateMax(state.maxFrameTotalNs, frameTotalNs);

            uint32_t sampleCount = state.averageFrameSamples.load(std::memory_order_relaxed);
            uint64_t averageNs = state.averageFrameTotalNs.load(std::memory_order_relaxed);
            if (sampleCount < kAverageWindowFrames) {
                uint32_t nextSampleCount = sampleCount + 1;
                averageNs = ((averageNs * sampleCount) + frameTotalNs) / nextSampleCount;
                state.averageFrameSamples.store(nextSampleCount, std::memory_order_relaxed);
            }
            else {
                averageNs = ((averageNs * (kAverageWindowFrames - 1)) + frameTotalNs) / kAverageWindowFrames;
            }

            state.averageFrameTotalNs.store(averageNs, std::memory_order_relaxed);
        }
    }

    static void Reset() {
        for (auto& state : s_states) {
            state.pendingFrameTotalNs.store(0, std::memory_order_relaxed);
            state.pendingFrameCalls.store(0, std::memory_order_relaxed);
            state.lastFrameTotalNs.store(0, std::memory_order_relaxed);
            state.averageFrameTotalNs.store(0, std::memory_order_relaxed);
            state.maxFrameTotalNs.store(0, std::memory_order_relaxed);
            state.lastFrameCalls.store(0, std::memory_order_relaxed);
            state.averageFrameSamples.store(0, std::memory_order_relaxed);
            state.lastCallNs.store(0, std::memory_order_relaxed);
            state.maxCallNs.store(0, std::memory_order_relaxed);
            state.activeSpanStartNs.store(0, std::memory_order_relaxed);
        }
    }

    static Snapshot GetSnapshot(Section section) {
        size_t index = (size_t)section;
        const auto& state = s_states[index];
        return {
            .name = s_sectionNames[index],
            .lastFrameMs = NsToMs(state.lastFrameTotalNs.load(std::memory_order_relaxed)),
            .averageFrameMs = NsToMs(state.averageFrameTotalNs.load(std::memory_order_relaxed)),
            .maxFrameMs = NsToMs(state.maxFrameTotalNs.load(std::memory_order_relaxed)),
            .lastCallMs = NsToMs(state.lastCallNs.load(std::memory_order_relaxed)),
            .maxCallMs = NsToMs(state.maxCallNs.load(std::memory_order_relaxed)),
            .lastFrameCalls = state.lastFrameCalls.load(std::memory_order_relaxed),
        };
    }

    static std::array<Snapshot, kSectionCount> GetSnapshots() {
        std::array<Snapshot, kSectionCount> snapshots = {};
        for (size_t index = 0; index < snapshots.size(); ++index) {
            snapshots[index] = GetSnapshot((Section)index);
        }
        return snapshots;
    }

private:
    struct SectionState {
        std::atomic<uint64_t> pendingFrameTotalNs = 0;
        std::atomic<uint32_t> pendingFrameCalls = 0;
        std::atomic<uint64_t> lastFrameTotalNs = 0;
        std::atomic<uint64_t> averageFrameTotalNs = 0;
        std::atomic<uint64_t> maxFrameTotalNs = 0;
        std::atomic<uint32_t> lastFrameCalls = 0;
        std::atomic<uint32_t> averageFrameSamples = 0;
        std::atomic<uint64_t> lastCallNs = 0;
        std::atomic<uint64_t> maxCallNs = 0;
        std::atomic<uint64_t> activeSpanStartNs = 0;
    };

    inline static std::atomic_bool s_enabled = false;
    inline static std::array<SectionState, kSectionCount> s_states = {};
    inline static constexpr std::array<const char*, kSectionCount> s_sectionNames = {
        "Roomscale Resolve",
        "Roomscale Begin",
        "Weapon Hand Hook",
        "Weapon Attack Hook",
        "XR UpdateSpaces",
        "XR UpdateActions",
        "XR LocateViews",
        "Layer3D Render",
        "Layer2D Render",
        "ImGui Update",
        "ImGui Render",
        "ImGui Draw/Copy",
    };

    static constexpr double NsToMs(uint64_t durationNs) {
        return (double)durationNs / 1000000.0;
    }

    static uint64_t GetTimestampNs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static void UpdateMax(std::atomic<uint64_t>& destination, uint64_t value) {
        uint64_t current = destination.load(std::memory_order_relaxed);
        while (current < value && !destination.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    static void ResetPendingFrame() {
        for (auto& state : s_states) {
            state.pendingFrameTotalNs.store(0, std::memory_order_relaxed);
            state.pendingFrameCalls.store(0, std::memory_order_relaxed);
            state.activeSpanStartNs.store(0, std::memory_order_relaxed);
        }
    }
};
