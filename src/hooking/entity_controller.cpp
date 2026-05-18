#include "pch.h"

#include "cemu_hooks.h"
#include "instance.h"
#include "utils/debug_draw.h"
#include "utils/game_utils.h"

enum RoomscaleHookResult : uint32_t {
    RoomscaleHookResult_Skip = 0,
    RoomscaleHookResult_Cast = 1,
    RoomscaleHookResult_Warp = 2,
};

enum RoomscaleQueryType : uint32_t {
    RoomscaleQueryType_Sweep = 0,
    RoomscaleQueryType_GroundProbe = 1,
};

enum RoomscaleResolvePhase : uint32_t {
    RoomscaleResolvePhase_ForwardSweep = 0,
    RoomscaleResolvePhase_StepUpSweep = 1,
    RoomscaleResolvePhase_GroundProbe = 2,
};

struct RoomscaleRaycastScratch {
    BEVec3 currentPos = {};
    BEVec3 castFrom = {};
    BEVec3 castDelta = {};
    BEVec3 hitPos = {};
    BEType<uint32_t> groundHit = 0;
    BEType<uint32_t> queryType = RoomscaleQueryType_Sweep;
    BEType<float> sweepRadius = 0.35f;
    BEType<uint32_t> reserved = 0;
};

static_assert(offsetof(RoomscaleRaycastScratch, currentPos) == 0x00, "Roomscale scratch current position offset mismatch");
static_assert(offsetof(RoomscaleRaycastScratch, castFrom) == 0x0C, "Roomscale scratch cast from offset mismatch");
static_assert(offsetof(RoomscaleRaycastScratch, castDelta) == 0x18, "Roomscale scratch cast delta offset mismatch");
static_assert(offsetof(RoomscaleRaycastScratch, hitPos) == 0x24, "Roomscale scratch hit offset mismatch");
static_assert(offsetof(RoomscaleRaycastScratch, groundHit) == 0x30, "Roomscale scratch ground-hit offset mismatch");
static_assert(offsetof(RoomscaleRaycastScratch, queryType) == 0x34, "Roomscale scratch query-type offset mismatch");
static_assert(offsetof(RoomscaleRaycastScratch, sweepRadius) == 0x38, "Roomscale scratch sweep-radius offset mismatch");
static_assert(sizeof(RoomscaleRaycastScratch) == 0x40, "Roomscale scratch size mismatch");

struct RoomscaleState {
    bool hasPreviousHeadPos = false;
    bool hasAppliedHeadPos = false;
    bool isResolving = false;
    bool isProbeOnly = false;
    bool isBlocked = false;
    bool isBlockedByCollision = false;
    bool hasActiveStep = false;
    bool isBinarySearchingStep = false;
    bool currentStepBlockedByCollision = false;
    bool currentStepTriedStepUp = false;
    glm::fvec3 previousHeadPos = {};
    glm::fvec3 appliedHeadPos = {};
    glm::fvec3 trackedHeadPos = {};
    glm::fvec3 remainingRawDelta = {};
    glm::fvec3 remainingWorldDelta = {};
    glm::fvec3 currentStepRawDelta = {};
    glm::fvec3 currentStepWorldDelta = {};
    glm::fvec3 currentStepCandidateWorldPos = {};
    glm::fvec3 currentStepLastSafeWorldPos = {};
    float currentStepLowScale = 0.0f;
    float currentStepHighScale = 1.0f;
    float currentStepProbeScale = 1.0f;
    float currentStepLastSafeScale = 0.0f;
    uint32_t currentStepSearchIteration = 0;
    uint32_t attemptIndex = 0;
    uint32_t maxAttempts = 0;
    RoomscaleResolvePhase resolvePhase = RoomscaleResolvePhase_ForwardSweep;
};

enum class RoomscaleBodyDriveMode : uint8_t {
    DisabledNotFirstPerson,
    DisabledNotInGame,
    DisabledNotStanding,
    DisabledNoRenderer,
    DisabledNoPositional,
    DisabledNoPlayer,
    DisabledSpecialMovement,
    Enabled,
};

static RoomscaleState s_roomscaleState;
static std::atomic<float> s_roomscaleFadeAmount = 0.0f;
static RoomscaleBodyDriveMode s_roomscaleBodyDriveMode = RoomscaleBodyDriveMode::DisabledNotInGame;

constexpr float kMaxRoomscaleJumpDistanceSq = 0.25f;
constexpr float kMinRoomscaleMoveDistanceSq = 0.000001f;
constexpr float kRoomscaleFadeStartDistance = 0.0f;
constexpr float kRoomscaleFadeFullDistance = 0.15f;
constexpr float kRoomscaleHeadCollisionRadius = 0.10f;
constexpr float kRoomscaleStepDistance = 0.0625f;
constexpr float kRoomscaleGroundSnapMaxRise = 0.10f;
constexpr float kRoomscaleStepUpHeight = 0.30f;
constexpr float kRoomscaleBinarySearchScaleResolution = 1.0f / 16.0f;
constexpr float kRoomscaleDebugFallbackBodyRadius = 0.35f;
constexpr float kRoomscaleDebugFallbackBodyHalfHeight = 0.9f;
constexpr uint32_t kRoomscaleBinarySearchIterations = 4;
constexpr uint32_t kMaxRoomscaleRaycastAttempts = 8;
constexpr uint32_t kRoomscaleGroundHit = 15;

struct RoomscaleDebugBody {
    glm::fvec3 centerOffset = glm::fvec3(0.0f, kRoomscaleDebugFallbackBodyHalfHeight, 0.0f);
    float radius = kRoomscaleDebugFallbackBodyRadius;
    float halfHeight = kRoomscaleDebugFallbackBodyHalfHeight;
    float sweepRadius = kRoomscaleDebugFallbackBodyRadius;
    float groundProbeUp = 0.2f;
    float groundProbeDown = 0.75f;
};

struct RoomscaleDebugPalette {
    uint32_t pathColor = 0;
    uint32_t pathSecondaryColor = 0;
    uint32_t bodyColor = 0;
    uint32_t floorColor = 0;
    uint32_t markerColor = 0;
    uint32_t successColor = 0;
    uint32_t blockedColor = 0;
    uint32_t xrayBlockedColor = 0;
};

static uint32_t ScaleRoomscaleDebugColor(uint32_t color, float rgbScale, float alphaScale = 1.0f) {
    auto scaleChannel = [&](int shift, float scale) {
        uint8_t value = (uint8_t)((color >> shift) & 0xFFu);
        return (uint8_t)glm::clamp((int)std::lround((float)value * scale), 0, 255);
    };

    return DebugDrawColor(scaleChannel(0, rgbScale), scaleChannel(8, rgbScale), scaleChannel(16, rgbScale), scaleChannel(24, alphaScale));
}

static RoomscaleDebugPalette GetRoomscaleDebugPalette(bool isGroundProbe, bool isProbeOnly) {
    if (isGroundProbe) {
        return {
            .pathColor = DebugDrawColor(196, 104, 255, 255),
            .pathSecondaryColor = DebugDrawColor(220, 160, 255, 168),
            .bodyColor = DebugDrawColor(188, 112, 255, 118),
            .floorColor = DebugDrawColor(212, 144, 255, 156),
            .markerColor = DebugDrawColor(255, 84, 220, 255),
            .successColor = DebugDrawColor(172, 132, 255, 232),
            .blockedColor = DebugDrawColor(255, 72, 220, 210),
            .xrayBlockedColor = DebugDrawColor(255, 72, 220, 128),
        };
    }

    if (isProbeOnly) {
        return {
            .pathColor = DebugDrawColor(96, 255, 136, 255),
            .pathSecondaryColor = DebugDrawColor(156, 255, 184, 180),
            .bodyColor = DebugDrawColor(96, 255, 136, 156),
            .floorColor = DebugDrawColor(120, 255, 164, 148),
            .markerColor = DebugDrawColor(72, 255, 128, 255),
            .successColor = DebugDrawColor(188, 255, 168, 228),
            .blockedColor = DebugDrawColor(72, 220, 104, 220),
            .xrayBlockedColor = DebugDrawColor(72, 220, 104, 136),
        };
    }

    return {
        .pathColor = DebugDrawColor(88, 212, 255, 255),
        .pathSecondaryColor = DebugDrawColor(144, 232, 255, 180),
        .bodyColor = DebugDrawColor(88, 212, 255, 144),
        .floorColor = DebugDrawColor(128, 224, 255, 144),
        .markerColor = DebugDrawColor(120, 255, 255, 255),
        .successColor = DebugDrawColor(96, 255, 136, 228),
        .blockedColor = DebugDrawColor(255, 88, 88, 224),
        .xrayBlockedColor = DebugDrawColor(255, 88, 88, 132),
    };
}

static RoomscaleDebugBody GetRoomscaleDebugBody() {
    RoomscaleDebugBody body = {};

    PlayerBase player = {};
    if (!GameUtils::TryReadPlayerBase(player)) {
        return body;
    }

    const glm::fvec3 localMin = player.aabb.min.getLE();
    const glm::fvec3 localMax = player.aabb.max.getLE();
    const glm::fvec3 localHalfExtents = (localMax - localMin) * 0.5f;
    const bool hasFiniteBounds = std::isfinite(localMin.x) && std::isfinite(localMin.y) && std::isfinite(localMin.z) && std::isfinite(localMax.x) && std::isfinite(localMax.y) && std::isfinite(localMax.z);
    if (!hasFiniteBounds || glm::any(glm::lessThanEqual(localHalfExtents, glm::fvec3(0.0f)))) {
        return body;
    }

    body.centerOffset = (localMin + localMax) * 0.5f;
    body.radius = std::max(localHalfExtents.x, localHalfExtents.z);
    body.halfHeight = std::max(localHalfExtents.y, body.radius * 0.5f);
    body.sweepRadius = glm::clamp(body.radius, 0.2f, 0.45f);

    float footOffset = std::max(-localMin.y, 0.0f);
    body.groundProbeUp = std::max(footOffset + 0.05f, 0.2f);
    body.groundProbeDown = std::max(footOffset + 0.25f, 0.5f);
    return body;
}

static void DrawRoomscaleDebugPositionMarker(const glm::fvec3& pos, float radius, uint32_t color, bool xray = false) {
    DebugDraw::instance().Dot(pos, radius, color, xray);
}

static bool IsRoomscaleGroundProbe(const RoomscaleRaycastScratch& scratch) {
    return scratch.queryType.getLE() == RoomscaleQueryType_GroundProbe;
}

static void DrawRoomscaleDebugCast(const RoomscaleRaycastScratch& scratch, bool isProbeOnly) {
    if (!GetSettings().ShouldShowRoomPhysics() || IsRoomscaleGroundProbe(scratch)) {
        return;
    }

    const RoomscaleDebugBody body = GetRoomscaleDebugBody();
    const RoomscaleDebugPalette palette = GetRoomscaleDebugPalette(false, isProbeOnly);
    const glm::fvec3 castFrom = scratch.castFrom.getLE();
    const glm::fvec3 castTo = castFrom + scratch.castDelta.getLE();
    const float sweepRadius = std::max(scratch.sweepRadius.getLE(), 0.01f);

    DebugDraw::instance().Line(castFrom, castTo, palette.pathColor, 1.0f, true);
    DebugDraw::instance().PhysicsBody(castFrom, body.radius, body.halfHeight, ScaleRoomscaleDebugColor(palette.bodyColor, 0.85f, 0.28f), 1.0f, 0, true);
    DebugDraw::instance().PhysicsBody(castTo, body.radius, body.halfHeight, ScaleRoomscaleDebugColor(palette.bodyColor, 0.75f, 0.22f), 1.0f, 0, true);
    DebugDraw::instance().Sphere(castFrom, sweepRadius, palette.pathColor, 0, true);
    DebugDraw::instance().Sphere(castTo, sweepRadius, palette.pathSecondaryColor, 0, true);
    DrawRoomscaleDebugPositionMarker(castFrom, 0.05f, palette.markerColor, true);
    DrawRoomscaleDebugPositionMarker(castTo, 0.05f, palette.pathSecondaryColor, true);
}

static void DrawRoomscaleDebugCastResult(const RoomscaleRaycastScratch& scratch, bool isProbeOnly, bool hadHit) {
    if (!GetSettings().ShouldShowRoomPhysics()) {
        return;
    }

    const bool isGroundProbe = IsRoomscaleGroundProbe(scratch);
    const RoomscaleDebugPalette palette = GetRoomscaleDebugPalette(isGroundProbe, isProbeOnly);
    const glm::fvec3 castFrom = scratch.castFrom.getLE();
    const glm::fvec3 castTo = castFrom + scratch.castDelta.getLE();
    if (!hadHit || isGroundProbe) {
        return;
    }

    const glm::fvec3 hitPos = scratch.hitPos.getLE();
    const float sweepRadius = std::max(scratch.sweepRadius.getLE(), 0.01f);
    DebugDraw::instance().Line(castFrom, hitPos, palette.blockedColor, 1.0f, true);
    if (glm::dot(hitPos - castTo, hitPos - castTo) > 0.000001f) {
        DebugDraw::instance().Line(hitPos, castTo, ScaleRoomscaleDebugColor(palette.blockedColor, 0.45f, 0.35f), 1.0f, true);
    }
    DebugDraw::instance().Sphere(hitPos, sweepRadius, ScaleRoomscaleDebugColor(palette.blockedColor, 0.85f, 0.28f), 0, true);
    DrawRoomscaleDebugPositionMarker(hitPos, 0.08f, palette.blockedColor, true);
}

static void ResetRoomscaleActiveStep() {
    s_roomscaleState.hasActiveStep = false;
    s_roomscaleState.isBinarySearchingStep = false;
    s_roomscaleState.currentStepBlockedByCollision = false;
    s_roomscaleState.currentStepTriedStepUp = false;
    s_roomscaleState.currentStepRawDelta = glm::fvec3(0.0f);
    s_roomscaleState.currentStepWorldDelta = glm::fvec3(0.0f);
    s_roomscaleState.currentStepCandidateWorldPos = glm::fvec3(0.0f);
    s_roomscaleState.currentStepLastSafeWorldPos = glm::fvec3(0.0f);
    s_roomscaleState.currentStepLowScale = 0.0f;
    s_roomscaleState.currentStepHighScale = 1.0f;
    s_roomscaleState.currentStepProbeScale = 1.0f;
    s_roomscaleState.currentStepLastSafeScale = 0.0f;
    s_roomscaleState.currentStepSearchIteration = 0;
    s_roomscaleState.resolvePhase = RoomscaleResolvePhase_ForwardSweep;
}

static void BeginRoomscaleActiveStep(uint32_t remainingSteps) {
    s_roomscaleState.hasActiveStep = true;
    s_roomscaleState.isBinarySearchingStep = false;
    s_roomscaleState.currentStepBlockedByCollision = false;
    s_roomscaleState.currentStepTriedStepUp = false;
    s_roomscaleState.currentStepRawDelta = s_roomscaleState.remainingRawDelta / (float)remainingSteps;
    s_roomscaleState.currentStepWorldDelta = s_roomscaleState.remainingWorldDelta / (float)remainingSteps;
    s_roomscaleState.currentStepCandidateWorldPos = glm::fvec3(0.0f);
    s_roomscaleState.currentStepLastSafeWorldPos = glm::fvec3(0.0f);
    s_roomscaleState.currentStepLowScale = 0.0f;
    s_roomscaleState.currentStepHighScale = 1.0f;
    s_roomscaleState.currentStepProbeScale = 1.0f;
    s_roomscaleState.currentStepLastSafeScale = 0.0f;
    s_roomscaleState.currentStepSearchIteration = 0;
    s_roomscaleState.resolvePhase = RoomscaleResolvePhase_ForwardSweep;
}

static bool IsRoomscaleGroundProbeValid(const glm::fvec3& candidateWorldPos, const glm::fvec3& hitPos, float maxRise) {
    if (!glm::all(glm::isfinite(hitPos))) {
        return false;
    }

    return (hitPos.y - candidateWorldPos.y) <= maxRise;
}

static glm::fvec3 ResolveRoomscaleGroundPosition(const glm::fvec3& candidateWorldPos, const glm::fvec3& hitPos) {
    glm::fvec3 resolvedWorldPos = candidateWorldPos;
    resolvedWorldPos.y = hitPos.y;
    return resolvedWorldPos;
}

static glm::fmat3 GetRoomscaleRoomToWorldRotation() {
    return glm::fmat3(CemuHooks::s_lastCameraMtx);
}

static glm::fmat3 GetRoomscaleWorldToRoomRotation() {
    return glm::transpose(GetRoomscaleRoomToWorldRotation());
}

static bool QueueNextRoomscaleBinarySearchProbe(bool candidateWasSafe) {
    if (candidateWasSafe) {
        s_roomscaleState.currentStepLastSafeScale = s_roomscaleState.currentStepProbeScale;
        s_roomscaleState.currentStepLowScale = s_roomscaleState.currentStepProbeScale;
    }
    else {
        s_roomscaleState.currentStepHighScale = s_roomscaleState.currentStepProbeScale;
        s_roomscaleState.isBinarySearchingStep = true;
    }

    if (!s_roomscaleState.isBinarySearchingStep) {
        return false;
    }

    if (s_roomscaleState.currentStepSearchIteration >= kRoomscaleBinarySearchIterations || (s_roomscaleState.currentStepHighScale - s_roomscaleState.currentStepLowScale) <= kRoomscaleBinarySearchScaleResolution) {
        return false;
    }

    s_roomscaleState.currentStepSearchIteration++;
    s_roomscaleState.currentStepProbeScale = (s_roomscaleState.currentStepLowScale + s_roomscaleState.currentStepHighScale) * 0.5f;
    s_roomscaleState.currentStepTriedStepUp = false;
    s_roomscaleState.resolvePhase = RoomscaleResolvePhase_ForwardSweep;
    return true;
}

static void ClearRoomscaleResolveState() {
    s_roomscaleState.isResolving = false;
    s_roomscaleState.isProbeOnly = false;
    s_roomscaleState.remainingRawDelta = glm::fvec3(0.0f);
    s_roomscaleState.remainingWorldDelta = glm::fvec3(0.0f);
    s_roomscaleState.attemptIndex = 0;
    s_roomscaleState.maxAttempts = 0;
    ResetRoomscaleActiveStep();
}

static void ResetRoomscaleState() {
    s_roomscaleState.hasPreviousHeadPos = false;
    s_roomscaleState.hasAppliedHeadPos = false;
    s_roomscaleState.isBlocked = false;
    s_roomscaleState.isBlockedByCollision = false;
    s_roomscaleState.previousHeadPos = glm::fvec3(0.0f);
    s_roomscaleState.appliedHeadPos = glm::fvec3(0.0f);
    s_roomscaleState.trackedHeadPos = glm::fvec3(0.0f);
    ClearRoomscaleResolveState();
    s_roomscaleFadeAmount.store(0.0f, std::memory_order_relaxed);
}

static void UpdateRoomscaleFade() {
    if (!s_roomscaleState.hasAppliedHeadPos || !s_roomscaleState.isBlockedByCollision) {
        s_roomscaleFadeAmount.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const RoomscaleDebugBody body = GetRoomscaleDebugBody();
    glm::fvec2 residual = glm::fvec2(s_roomscaleState.trackedHeadPos.x - s_roomscaleState.appliedHeadPos.x, s_roomscaleState.trackedHeadPos.z - s_roomscaleState.appliedHeadPos.z);
    float residualDistance = glm::length(residual);
    float headPenetrationDistance = residualDistance - std::max(body.sweepRadius - kRoomscaleHeadCollisionRadius, 0.0f);
    float fadeAmount = glm::smoothstep(kRoomscaleFadeStartDistance, kRoomscaleFadeFullDistance, headPenetrationDistance);
    s_roomscaleFadeAmount.store(glm::clamp(fadeAmount, 0.0f, 1.0f), std::memory_order_relaxed);
}

static glm::fvec3 GetRoomscaleResidual() {
    if (!s_roomscaleState.hasAppliedHeadPos) {
        return glm::fvec3(0.0f);
    }

    glm::fvec3 residual = s_roomscaleState.trackedHeadPos - s_roomscaleState.appliedHeadPos;
    residual.y = 0.0f;
    return residual;
}

static glm::fvec3 RemoveResolvedRoomscaleDelta(glm::fvec3 roomDelta, glm::fvec3 residual) {
    residual.y = 0.0f;
    float residualDistance = glm::length(residual);
    if (residualDistance < 0.000001f) {
        return roomDelta;
    }

    glm::fvec3 residualDir = residual / residualDistance;
    float alongResidual = glm::dot(roomDelta, residualDir);
    if (alongResidual >= 0.0f) {
        return roomDelta;
    }

    float cancelDistance = std::min(-alongResidual, residualDistance);
    return roomDelta + (residualDir * cancelDistance);
}

static std::optional<glm::fvec3> TryGetCurrentHeadPos() {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        return std::nullopt;
    }

    std::optional<glm::fmat4> middlePose = renderer->GetMiddlePose();
    if (!middlePose.has_value()) {
        return std::nullopt;
    }

    glm::fvec3 headPos = glm::fvec3(middlePose.value()[3]);
    if (!glm::all(glm::isfinite(headPos))) {
        return std::nullopt;
    }

    return headPos;
}

static const char* GetRoomscaleBodyDriveModeName(RoomscaleBodyDriveMode mode) {
    switch (mode) {
        case RoomscaleBodyDriveMode::DisabledNotFirstPerson:
            return "disabled (not first-person)";
        case RoomscaleBodyDriveMode::DisabledNotInGame:
            return "disabled (not in game)";
        case RoomscaleBodyDriveMode::DisabledNotStanding:
            return "disabled (play mode is not standing)";
        case RoomscaleBodyDriveMode::DisabledNoRenderer:
            return "disabled (renderer unavailable)";
        case RoomscaleBodyDriveMode::DisabledNoPositional:
            return "disabled (no positional tracking)";
        case RoomscaleBodyDriveMode::DisabledNoPlayer:
            return "disabled (player unavailable)";
        case RoomscaleBodyDriveMode::DisabledSpecialMovement:
            return "disabled (special movement state)";
        case RoomscaleBodyDriveMode::Enabled:
            return "enabled";
    }

    return "disabled (unknown)";
}

static void SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode mode) {
    if (s_roomscaleBodyDriveMode == mode) {
        return;
    }

    s_roomscaleBodyDriveMode = mode;
    Log::print<PPC>("Roomscale body drive mode switched: {}", GetRoomscaleBodyDriveModeName(mode));
}

static void PrimeRoomscaleBaseline(const glm::fvec3& headPos) {
    s_roomscaleState.previousHeadPos = headPos;
    s_roomscaleState.appliedHeadPos = headPos;
    s_roomscaleState.trackedHeadPos = headPos;
    s_roomscaleState.hasPreviousHeadPos = true;
    s_roomscaleState.hasAppliedHeadPos = true;
    s_roomscaleState.isBlocked = false;
    s_roomscaleState.isBlockedByCollision = false;
    ClearRoomscaleResolveState();
    UpdateRoomscaleFade();
}

static bool ShouldDrivePlayerBodyWithVR(const glm::fvec3& currentHeadPos, PlayerBase& player) {
    if (!CemuHooks::IsFirstPerson()) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNotFirstPerson);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!CemuHooks::IsInGame()) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNotInGame);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (GetSettings().GetPlayMode() != PlayMode::STANDING) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNotStanding);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNoRenderer);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!VRManager::instance().XR->m_capabilities.supportsPositional) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNoPositional);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!GameUtils::TryReadPlayerBase(player)) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledNoPlayer);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    PlayerMoveBitFlags moveBits = player.moveBitFlags.getLE();
    OpenXR::GameState gameState = VRManager::instance().XR->m_gameState.load();
    bool shouldDisablePlayerBodyDrive = gameState.is_climbing || gameState.is_paragliding || gameState.is_riding_mount || HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_SWIMMING_OR_CLIMBING | PlayerMoveBitFlags::IS_SWIMMING);
    if (shouldDisablePlayerBodyDrive) {
        SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::DisabledSpecialMovement);
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    SetRoomscaleBodyDriveMode(RoomscaleBodyDriveMode::Enabled);
    return true;
}

static bool TryGetRoomscaleMoveDelta(uint32_t controller, glm::fvec3& currentHeadPos, glm::fvec3& roomDelta, glm::fvec3& worldDelta) {
    currentHeadPos = glm::fvec3(0.0f);
    roomDelta = glm::fvec3(0.0f);
    worldDelta = glm::fvec3(0.0f);

    uint32_t playerController = 0;
    if (!GameUtils::TryGetPlayerCharacterController(playerController) || controller != playerController) {
        return false;
    }

    std::optional<glm::fvec3> currentHeadPosOpt = TryGetCurrentHeadPos();
    if (!currentHeadPosOpt.has_value()) {
        ResetRoomscaleState();
        return false;
    }

    currentHeadPos = currentHeadPosOpt.value();
    s_roomscaleState.trackedHeadPos = currentHeadPos;

    PlayerBase player = {};
    if (!ShouldDrivePlayerBodyWithVR(currentHeadPos, player)) {
        return false;
    }

    if (!s_roomscaleState.hasPreviousHeadPos) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    glm::fvec3 previousResidual = s_roomscaleState.previousHeadPos - s_roomscaleState.appliedHeadPos;
    previousResidual.y = 0.0f;
    roomDelta = currentHeadPos - s_roomscaleState.previousHeadPos;
    s_roomscaleState.previousHeadPos = currentHeadPos;

    roomDelta.y = 0.0f;
    if (glm::dot(roomDelta, roomDelta) < kMinRoomscaleMoveDistanceSq) {
        UpdateRoomscaleFade();
        return false;
    }

    if (glm::dot(roomDelta, roomDelta) > kMaxRoomscaleJumpDistanceSq) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    const glm::fmat3 roomToWorld = GetRoomscaleRoomToWorldRotation();
    worldDelta = roomToWorld * roomDelta;
    worldDelta.y = 0.0f;
    glm::fvec3 previousResidualWorld = roomToWorld * previousResidual;
    previousResidualWorld.y = 0.0f;
    worldDelta = RemoveResolvedRoomscaleDelta(worldDelta, previousResidualWorld);
    if (glm::dot(worldDelta, worldDelta) < kMinRoomscaleMoveDistanceSq) {
        UpdateRoomscaleFade();
        return false;
    }

    roomDelta = GetRoomscaleWorldToRoomRotation() * worldDelta;
    roomDelta.y = 0.0f;

    return true;
}

glm::fvec3 CemuHooks::GetAppliedRoomscaleHeadPosition() {
    if (s_roomscaleState.hasAppliedHeadPos) {
        return s_roomscaleState.appliedHeadPos;
    }

    std::optional<glm::fvec3> currentHeadPos = TryGetCurrentHeadPos();
    if (currentHeadPos.has_value()) {
        return currentHeadPos.value();
    }

    return glm::fvec3(0.0f);
}

float CemuHooks::GetRoomscaleFadeAmount() {
    return s_roomscaleFadeAmount.load(std::memory_order_relaxed);
}


void CemuHooks::hook_BeginRoomscaleMovement(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t controller = hCPU->gpr[3];
    uint32_t scratchPtr = hCPU->gpr[4];
    RoomscaleRaycastScratch scratch = {};
    writeMemory(scratchPtr, &scratch);
    hCPU->gpr[3] = RoomscaleHookResult_Skip;
    ClearRoomscaleResolveState();

    uint32_t playerController = 0;
    if (!GameUtils::TryGetPlayerCharacterController(playerController) || controller != playerController) {
        return;
    }

    glm::fvec3 currentHeadPos = {};
    glm::fvec3 roomDelta = {};
    glm::fvec3 worldDelta = {};
    if (!TryGetRoomscaleMoveDelta(controller, currentHeadPos, roomDelta, worldDelta)) {
        glm::fvec3 residual = GetRoomscaleResidual();
        glm::fvec3 residualWorldDelta = GetRoomscaleRoomToWorldRotation() * residual;
        residualWorldDelta.y = 0.0f;
        if (glm::dot(residualWorldDelta, residualWorldDelta) < kMinRoomscaleMoveDistanceSq) {
            s_roomscaleState.isBlocked = false;
            s_roomscaleState.isBlockedByCollision = false;
            UpdateRoomscaleFade();
            return;
        }

        float residualMoveDistance = glm::length(residualWorldDelta);
        uint32_t maxAttempts = std::min<uint32_t>(std::max<uint32_t>((uint32_t)std::ceil(residualMoveDistance / kRoomscaleStepDistance), 1u), kMaxRoomscaleRaycastAttempts);

        s_roomscaleState.remainingRawDelta = residual;
        s_roomscaleState.remainingWorldDelta = residualWorldDelta;
        s_roomscaleState.attemptIndex = 0;
        s_roomscaleState.maxAttempts = maxAttempts;
        s_roomscaleState.isResolving = true;
        s_roomscaleState.isProbeOnly = true;
        hCPU->gpr[3] = RoomscaleHookResult_Cast;
        return;
    }
    (void)currentHeadPos;

    float moveDistance = glm::length(worldDelta);
    uint32_t maxAttempts = std::min<uint32_t>(std::max<uint32_t>((uint32_t)std::ceil(moveDistance / kRoomscaleStepDistance), 1u), kMaxRoomscaleRaycastAttempts);

    s_roomscaleState.remainingRawDelta = roomDelta;
    s_roomscaleState.remainingWorldDelta = worldDelta;
    s_roomscaleState.attemptIndex = 0;
    s_roomscaleState.maxAttempts = maxAttempts;
    s_roomscaleState.isResolving = true;
    s_roomscaleState.isProbeOnly = false;
    hCPU->gpr[3] = RoomscaleHookResult_Cast;
}

void CemuHooks::hook_PrepareRoomscaleRaycast(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    uint32_t scratchPtr = hCPU->gpr[3];
    hCPU->gpr[3] = RoomscaleHookResult_Skip;

    if (!s_roomscaleState.isResolving) {
        return;
    }

    if (s_roomscaleState.attemptIndex >= s_roomscaleState.maxAttempts || glm::dot(s_roomscaleState.remainingWorldDelta, s_roomscaleState.remainingWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    RoomscaleRaycastScratch scratch = {};
    readMemory(scratchPtr, &scratch);

    uint32_t remainingSteps = s_roomscaleState.maxAttempts - s_roomscaleState.attemptIndex;
    if (remainingSteps == 0) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    if (!s_roomscaleState.hasActiveStep) {
        BeginRoomscaleActiveStep(remainingSteps);
    }

    if (glm::dot(s_roomscaleState.currentStepWorldDelta, s_roomscaleState.currentStepWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    const RoomscaleDebugBody body = GetRoomscaleDebugBody();
    const glm::fvec3 currentWorldPos = scratch.currentPos.getLE();
    const float probeScale = glm::clamp(s_roomscaleState.currentStepProbeScale, 0.0f, 1.0f);

    scratch.groundHit = kRoomscaleGroundHit;
    scratch.sweepRadius = body.sweepRadius;
    if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_ForwardSweep) {
        const glm::fvec3 sweepDelta = s_roomscaleState.currentStepWorldDelta * probeScale;

        scratch.queryType = RoomscaleQueryType_Sweep;
        // currentPos tracks the controller/body origin, so center the sphere sweep on the body.
        scratch.castFrom = currentWorldPos + body.centerOffset;
        scratch.castDelta = sweepDelta;
        scratch.hitPos = scratch.castFrom.getLE() + sweepDelta;
        s_roomscaleState.currentStepCandidateWorldPos = currentWorldPos + sweepDelta;
    }
    else if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_StepUpSweep) {
        const glm::fvec3 sweepDelta = s_roomscaleState.currentStepWorldDelta * probeScale;

        scratch.queryType = RoomscaleQueryType_Sweep;
        scratch.castFrom = currentWorldPos + body.centerOffset + glm::fvec3(0.0f, kRoomscaleStepUpHeight, 0.0f);
        scratch.castDelta = sweepDelta;
        scratch.hitPos = scratch.castFrom.getLE() + sweepDelta;
        s_roomscaleState.currentStepCandidateWorldPos = currentWorldPos + sweepDelta + glm::fvec3(0.0f, kRoomscaleStepUpHeight, 0.0f);
    }
    else {
        scratch.queryType = RoomscaleQueryType_GroundProbe;
        scratch.castFrom = glm::fvec3(s_roomscaleState.currentStepCandidateWorldPos.x, s_roomscaleState.currentStepCandidateWorldPos.y + body.groundProbeUp, s_roomscaleState.currentStepCandidateWorldPos.z);
        scratch.castDelta = glm::fvec3(0.0f, -(body.groundProbeUp + body.groundProbeDown), 0.0f);
        scratch.hitPos = glm::fvec3(0.0f);
    }

    DrawRoomscaleDebugCast(scratch, s_roomscaleState.isProbeOnly);
    writeMemory(scratchPtr, &scratch);
    hCPU->gpr[3] = RoomscaleHookResult_Cast;
}

void CemuHooks::hook_BuildRoomscaleWarpTransform(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t currentPosPtr = hCPU->gpr[3];
    uint32_t transformPtr = hCPU->gpr[4];

    BEVec3 currentPos = {};
    BEMatrix34 transform = {};
    readMemory(currentPosPtr, &currentPos);
    readMemory(transformPtr, &transform);

    transform.setPos(currentPos.getLE());
    writeMemory(transformPtr, &transform);
}

void CemuHooks::hook_ConsumeRoomscaleRaycast(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    uint32_t scratchPtr = hCPU->gpr[3];
    bool hadHit = hCPU->gpr[4] != 0;
    hCPU->gpr[3] = RoomscaleHookResult_Skip;

    if (!s_roomscaleState.isResolving) {
        return;
    }

    RoomscaleRaycastScratch scratch = {};
    readMemory(scratchPtr, &scratch);
    uint32_t remainingSteps = s_roomscaleState.maxAttempts - s_roomscaleState.attemptIndex;
    if (remainingSteps == 0) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_ForwardSweep) {
        DrawRoomscaleDebugCastResult(scratch, s_roomscaleState.isProbeOnly, hadHit);
        if (hadHit) {
            s_roomscaleState.currentStepBlockedByCollision = true;
            if (!s_roomscaleState.currentStepTriedStepUp) {
                s_roomscaleState.currentStepTriedStepUp = true;
                s_roomscaleState.resolvePhase = RoomscaleResolvePhase_StepUpSweep;
                hCPU->gpr[3] = RoomscaleHookResult_Cast;
                return;
            }

            if (QueueNextRoomscaleBinarySearchProbe(false)) {
                hCPU->gpr[3] = RoomscaleHookResult_Cast;
                return;
            }

            if (s_roomscaleState.currentStepLastSafeScale > 0.0f) {
                scratch.currentPos = s_roomscaleState.currentStepLastSafeWorldPos;
                s_roomscaleState.appliedHeadPos += s_roomscaleState.currentStepRawDelta * s_roomscaleState.currentStepLastSafeScale;
                writeMemory(scratchPtr, &scratch);
            }

            s_roomscaleState.isBlocked = true;
            s_roomscaleState.isBlockedByCollision = true;
            UpdateRoomscaleFade();
            ClearRoomscaleResolveState();
            hCPU->gpr[3] = RoomscaleHookResult_Warp;
            return;
        }

        s_roomscaleState.resolvePhase = RoomscaleResolvePhase_GroundProbe;
        hCPU->gpr[3] = RoomscaleHookResult_Cast;
        return;
    }

    if (s_roomscaleState.resolvePhase == RoomscaleResolvePhase_StepUpSweep) {
        DrawRoomscaleDebugCastResult(scratch, s_roomscaleState.isProbeOnly, hadHit);
        if (hadHit) {
            s_roomscaleState.currentStepBlockedByCollision = true;
            if (QueueNextRoomscaleBinarySearchProbe(false)) {
                hCPU->gpr[3] = RoomscaleHookResult_Cast;
                return;
            }

            if (s_roomscaleState.currentStepLastSafeScale > 0.0f) {
                scratch.currentPos = s_roomscaleState.currentStepLastSafeWorldPos;
                s_roomscaleState.appliedHeadPos += s_roomscaleState.currentStepRawDelta * s_roomscaleState.currentStepLastSafeScale;
                writeMemory(scratchPtr, &scratch);
            }

            s_roomscaleState.isBlocked = true;
            s_roomscaleState.isBlockedByCollision = true;
            UpdateRoomscaleFade();
            ClearRoomscaleResolveState();
            hCPU->gpr[3] = RoomscaleHookResult_Warp;
            return;
        }

        s_roomscaleState.resolvePhase = RoomscaleResolvePhase_GroundProbe;
        hCPU->gpr[3] = RoomscaleHookResult_Cast;
        return;
    }

    glm::fvec3 groundProbeBaseWorldPos = s_roomscaleState.currentStepCandidateWorldPos;
    if (s_roomscaleState.currentStepTriedStepUp) {
        groundProbeBaseWorldPos.y -= kRoomscaleStepUpHeight;
    }

    float maxGroundRise = s_roomscaleState.currentStepTriedStepUp ? (kRoomscaleStepUpHeight + kRoomscaleGroundSnapMaxRise) : kRoomscaleGroundSnapMaxRise;
    bool allowAirStep = !hadHit && !s_roomscaleState.currentStepTriedStepUp;
    bool groundValid = allowAirStep || (hadHit && IsRoomscaleGroundProbeValid(groundProbeBaseWorldPos, scratch.hitPos.getLE(), maxGroundRise));
    // If there is no landing surface below an ordinary horizontal move, keep the stepped body move and let BotW's own gravity handle the fall.
    glm::fvec3 snappedCandidateWorldPos = (groundValid && hadHit) ? ResolveRoomscaleGroundPosition(groundProbeBaseWorldPos, scratch.hitPos.getLE()) : groundProbeBaseWorldPos;
    DrawRoomscaleDebugCastResult(scratch, s_roomscaleState.isProbeOnly, groundValid && hadHit);

    if (!groundValid) {
        if (QueueNextRoomscaleBinarySearchProbe(false)) {
            hCPU->gpr[3] = RoomscaleHookResult_Cast;
            return;
        }

        if (s_roomscaleState.currentStepLastSafeScale > 0.0f) {
            scratch.currentPos = s_roomscaleState.currentStepLastSafeWorldPos;
            s_roomscaleState.appliedHeadPos += s_roomscaleState.currentStepRawDelta * s_roomscaleState.currentStepLastSafeScale;
            writeMemory(scratchPtr, &scratch);
        }

        s_roomscaleState.isBlocked = true;
        s_roomscaleState.isBlockedByCollision = false;
        UpdateRoomscaleFade();
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    s_roomscaleState.currentStepLastSafeWorldPos = snappedCandidateWorldPos;
    if (QueueNextRoomscaleBinarySearchProbe(true)) {
        hCPU->gpr[3] = RoomscaleHookResult_Cast;
        return;
    }

    if (s_roomscaleState.isBinarySearchingStep) {
        if (s_roomscaleState.currentStepLastSafeScale > 0.0f) {
            scratch.currentPos = s_roomscaleState.currentStepLastSafeWorldPos;
            s_roomscaleState.appliedHeadPos += s_roomscaleState.currentStepRawDelta * s_roomscaleState.currentStepLastSafeScale;
            writeMemory(scratchPtr, &scratch);
        }

        s_roomscaleState.isBlocked = true;
        s_roomscaleState.isBlockedByCollision = s_roomscaleState.currentStepBlockedByCollision;
        UpdateRoomscaleFade();
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    scratch.currentPos = snappedCandidateWorldPos;
    s_roomscaleState.appliedHeadPos += s_roomscaleState.currentStepRawDelta;
    s_roomscaleState.remainingRawDelta -= s_roomscaleState.currentStepRawDelta;
    s_roomscaleState.remainingWorldDelta -= s_roomscaleState.currentStepWorldDelta;
    s_roomscaleState.attemptIndex++;
    s_roomscaleState.hasActiveStep = false;
    s_roomscaleState.resolvePhase = RoomscaleResolvePhase_ForwardSweep;
    writeMemory(scratchPtr, &scratch);

    if (glm::dot(s_roomscaleState.remainingWorldDelta, s_roomscaleState.remainingWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        s_roomscaleState.isBlocked = false;
        s_roomscaleState.isBlockedByCollision = false;
        UpdateRoomscaleFade();
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    if (s_roomscaleState.attemptIndex >= s_roomscaleState.maxAttempts) {
        s_roomscaleState.isBlocked = true;
        s_roomscaleState.isBlockedByCollision = false;
        UpdateRoomscaleFade();
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    hCPU->gpr[3] = RoomscaleHookResult_Cast;
}


#define LOG_PLAYER_BODY(...)                     \
    do {                                         \
        if (GameUtils::IsTrackedPlayerBody(hCPU->gpr[3])) { \
            Log::print<PPC>(__VA_ARGS__);        \
        }                                        \
    } while (0)

void CemuHooks::hook_SetRigidBodyVelocity(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    BEVec3 velocity;
    readMemory(hCPU->gpr[4], &velocity);
    LOG_PLAYER_BODY("[{:08X}] SetVelocity {}", hCPU->gpr[0], velocity.getLE());
}

void CemuHooks::hook_SetRigidBodyTransform(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486CD8;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    BEMatrix34 transform;
    readMemory(hCPU->gpr[4], &transform);
    LOG_PLAYER_BODY("[{:08X}] SetTransform pos={} rot={}", hCPU->sprNew.LR, transform.getPos().getLE(), transform.getRotLE());
}

void CemuHooks::hook_SetRigidBodyScale(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486F50;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    LOG_PLAYER_BODY("[{:08X}] SetScale {}", hCPU->sprNew.LR, (float)hCPU->fpr[1].fp0);
}

void CemuHooks::hook_SetRigidBodyPosition(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486D84;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    BEVec3 position;
    readMemory(hCPU->gpr[4], &position);
    LOG_PLAYER_BODY("[{:08X}] SetPosition {}", hCPU->sprNew.LR, position.getLE());
}

void CemuHooks::hook_SetRigidBodyPositionAndRotation(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03489A84;
    hCPU->gpr[11] = hCPU->gpr[1];

    BEMatrix34 transform;
    readMemory(hCPU->gpr[4], &transform);
    LOG_PLAYER_BODY("[{:08X}] SetPositionAndRotation pos={} rot={}", hCPU->sprNew.LR, transform.getPos().getLE(), transform.getRotLE());
}


bool shouldAdjustArrowTarget = false;

void CemuHooks::hook_LoadDynamicBool(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    hCPU->sprNew.LR = hCPU->gpr[0];

    InlineParamBool boolParam;
    readMemory(hCPU->gpr[30], &boolParam);

    const char* paramString = (char*)(boolParam.keyPtr.getLE() + s_memoryBaseAddress);
    Log::print<PPC>("[{:08X}] Loaded dynamic bool parameter for {}: {}", hCPU->gpr[0], paramString, boolParam.value.getLE());

    if (std::string(paramString) == "IsShootByPlayer" && boolParam.value.getLE() == 1) {
        Log::print<PPC>("Arrow is being shot by the player!");
        shouldAdjustArrowTarget = true;
    }
}

void CemuHooks::hook_LoadDynamicVec3(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x030EC844;

    InlineParamVec3 vec3Param;
    readMemory(hCPU->gpr[31], &vec3Param);

    vec3Param.value.z = hCPU->fpr[0].fp0;
    writeMemory(hCPU->gpr[31], &vec3Param);
}
