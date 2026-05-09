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

struct RoomscaleRaycastScratch {
    BEVec3 currentPos = {};
    BEVec3 castFrom = {};
    BEVec3 castDelta = {};
    BEVec3 hitPos = {};
    BEType<uint32_t> layerMask = 0;
    BEType<uint32_t> reserved = 0;
};

struct RoomscaleState {
    bool hasPreviousHeadPos = false;
    bool hasAppliedHeadPos = false;
    bool isResolving = false;
    bool isProbeOnly = false;
    bool isBlocked = false;
    glm::fvec3 previousHeadPos = {};
    glm::fvec3 appliedHeadPos = {};
    glm::fvec3 trackedHeadPos = {};
    glm::fvec3 remainingRawDelta = {};
    glm::fvec3 remainingWorldDelta = {};
    uint32_t attemptIndex = 0;
    uint32_t maxAttempts = 0;
};

static RoomscaleState s_roomscaleState;
static std::atomic<float> s_roomscaleFadeAmount = 0.0f;

constexpr float kMaxRoomscaleJumpDistanceSq = 0.25f;
constexpr float kMinRoomscaleMoveDistanceSq = 0.000001f;
constexpr float kRoomscaleFadeStartDistance = 0.10f;
constexpr float kRoomscaleFadeFullDistance = 0.25f;
constexpr float kRoomscaleStepDistance = 0.0625f;
constexpr uint32_t kMaxRoomscaleRaycastAttempts = 8;
constexpr uint32_t kRoomscaleLayerMask = 15;

static void ClearRoomscaleResolveState() {
    s_roomscaleState.isResolving = false;
    s_roomscaleState.isProbeOnly = false;
    s_roomscaleState.remainingRawDelta = glm::fvec3(0.0f);
    s_roomscaleState.remainingWorldDelta = glm::fvec3(0.0f);
    s_roomscaleState.attemptIndex = 0;
    s_roomscaleState.maxAttempts = 0;
}

static void ResetRoomscaleState() {
    s_roomscaleState.hasPreviousHeadPos = false;
    s_roomscaleState.hasAppliedHeadPos = false;
    s_roomscaleState.isBlocked = false;
    s_roomscaleState.previousHeadPos = glm::fvec3(0.0f);
    s_roomscaleState.appliedHeadPos = glm::fvec3(0.0f);
    s_roomscaleState.trackedHeadPos = glm::fvec3(0.0f);
    ClearRoomscaleResolveState();
    s_roomscaleFadeAmount.store(0.0f, std::memory_order_relaxed);
}

static void UpdateRoomscaleFade() {
    if (!s_roomscaleState.hasAppliedHeadPos || !s_roomscaleState.isBlocked) {
        s_roomscaleFadeAmount.store(0.0f, std::memory_order_relaxed);
        return;
    }

    glm::fvec2 residual = glm::fvec2(s_roomscaleState.trackedHeadPos.x - s_roomscaleState.appliedHeadPos.x, s_roomscaleState.trackedHeadPos.z - s_roomscaleState.appliedHeadPos.z);
    float residualDistance = glm::length(residual);
    float fadeAmount = glm::smoothstep(kRoomscaleFadeStartDistance, kRoomscaleFadeFullDistance, residualDistance);
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
    if (!std::isfinite(headPos.x) || !std::isfinite(headPos.y) || !std::isfinite(headPos.z)) {
        return std::nullopt;
    }

    return headPos;
}

static void PrimeRoomscaleBaseline(const glm::fvec3& headPos) {
    s_roomscaleState.previousHeadPos = headPos;
    s_roomscaleState.appliedHeadPos = headPos;
    s_roomscaleState.trackedHeadPos = headPos;
    s_roomscaleState.hasPreviousHeadPos = true;
    s_roomscaleState.hasAppliedHeadPos = true;
    s_roomscaleState.isBlocked = false;
    ClearRoomscaleResolveState();
    UpdateRoomscaleFade();
}

static bool ShouldDrivePlayerBodyWithVR(const glm::fvec3& currentHeadPos, PlayerBase& player) {
    if (!CemuHooks::IsFirstPerson() || !CemuHooks::IsInGame() || GetSettings().GetPlayMode() != PlayMode::STANDING) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr || !VRManager::instance().XR->m_capabilities.supportsPositional) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    if (!GameUtils::TryReadPlayerBase(player)) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

    PlayerMoveBitFlags moveBits = player.moveBitFlags.getLE();
    OpenXR::GameState gameState = VRManager::instance().XR->m_gameState.load();
    bool shouldDisablePlayerBodyDrive = gameState.is_climbing || gameState.is_paragliding || gameState.is_riding_mount || HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_SWIMMING_OR_CLIMBING | PlayerMoveBitFlags::IS_SWIMMING);
    if (shouldDisablePlayerBodyDrive) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return false;
    }

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

    roomDelta = RemoveResolvedRoomscaleDelta(roomDelta, previousResidual);
    roomDelta.y = 0.0f;
    if (glm::dot(roomDelta, roomDelta) < kMinRoomscaleMoveDistanceSq) {
        UpdateRoomscaleFade();
        return false;
    }

    worldDelta = glm::fmat3(CemuHooks::s_lastCameraMtx) * roomDelta;
    worldDelta.y = 0.0f;
    if (glm::dot(worldDelta, worldDelta) < kMinRoomscaleMoveDistanceSq) {
        UpdateRoomscaleFade();
        return false;
    }

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
        glm::fvec3 residualWorldDelta = glm::fmat3(CemuHooks::s_lastCameraMtx) * residual;
        residualWorldDelta.y = 0.0f;
        if (!s_roomscaleState.isBlocked || glm::dot(residualWorldDelta, residualWorldDelta) < kMinRoomscaleMoveDistanceSq) {
            UpdateRoomscaleFade();
            return;
        }

        s_roomscaleState.remainingRawDelta = glm::fvec3(0.0f);
        s_roomscaleState.remainingWorldDelta = residualWorldDelta;
        s_roomscaleState.attemptIndex = 0;
        s_roomscaleState.maxAttempts = 1;
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

    glm::fvec3 stepWorldDelta = s_roomscaleState.remainingWorldDelta / (float)remainingSteps;
    if (glm::dot(stepWorldDelta, stepWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    scratch.castFrom = scratch.currentPos;
    scratch.castDelta = stepWorldDelta;
    scratch.layerMask = kRoomscaleLayerMask;
    writeMemory(scratchPtr, &scratch);
    hCPU->gpr[3] = RoomscaleHookResult_Cast;
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

    glm::fvec3 stepRawDelta = s_roomscaleState.remainingRawDelta / (float)remainingSteps;
    glm::fvec3 stepWorldDelta = s_roomscaleState.remainingWorldDelta / (float)remainingSteps;
    glm::fvec3 currentWorldPos = scratch.currentPos.getLE();

    if (s_roomscaleState.isProbeOnly) {
        s_roomscaleState.isBlocked = hadHit;
        UpdateRoomscaleFade();
        ClearRoomscaleResolveState();
        return;
    }

    if (hadHit) {
        glm::fvec3 hitPos = scratch.hitPos.getLE();
        glm::fvec3 worldAccepted = hitPos - currentWorldPos;
        float acceptedScale = 0.0f;
        float stepLengthSq = glm::dot(stepWorldDelta, stepWorldDelta);
        if (stepLengthSq >= kMinRoomscaleMoveDistanceSq) {
            acceptedScale = glm::clamp(glm::dot(worldAccepted, stepWorldDelta) / stepLengthSq, 0.0f, 1.0f);
        }

        scratch.currentPos = hitPos;
        s_roomscaleState.appliedHeadPos += stepRawDelta * acceptedScale;
        s_roomscaleState.isBlocked = true;
        writeMemory(scratchPtr, &scratch);
        UpdateRoomscaleFade();
        ClearRoomscaleResolveState();
        hCPU->gpr[3] = RoomscaleHookResult_Warp;
        return;
    }

    currentWorldPos += stepWorldDelta;
    scratch.currentPos = currentWorldPos;
    s_roomscaleState.appliedHeadPos += stepRawDelta;
    s_roomscaleState.remainingRawDelta -= stepRawDelta;
    s_roomscaleState.remainingWorldDelta -= stepWorldDelta;
    s_roomscaleState.attemptIndex++;
    writeMemory(scratchPtr, &scratch);

    if (s_roomscaleState.attemptIndex >= s_roomscaleState.maxAttempts || glm::dot(s_roomscaleState.remainingWorldDelta, s_roomscaleState.remainingWorldDelta) < kMinRoomscaleMoveDistanceSq) {
        s_roomscaleState.isBlocked = false;
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
