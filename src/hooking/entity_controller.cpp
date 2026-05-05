#include "pch.h"

#include "cemu_hooks.h"
#include "instance.h"
#include "utils/game_utils.h"

struct RoomscaleState {
    bool hasPreviousHeadPos = false;
    glm::fvec3 previousHeadPos = {};
};

static RoomscaleState s_roomscaleState;

static void PrimeRoomscaleBaseline(const glm::fvec3& headPos) {
    s_roomscaleState.previousHeadPos = headPos;
    s_roomscaleState.hasPreviousHeadPos = true;
}

constexpr float kMaxRoomscaleJumpDistanceSq = 0.25f;
constexpr float kMinRoomscaleMoveDistanceSq = 0.000001f;


void CemuHooks::hook_GetRoomscaleDelta(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t controller = hCPU->gpr[3];
    BEVec3 deltaBE = {};
    writeMemory(hCPU->gpr[4], &deltaBE);
    hCPU->gpr[3] = 0;

    uint32_t playerController = 0;
    if (!GameUtils::TryGetPlayerCharacterController(playerController) || controller != playerController) {
        return;
    }

    if (!IsFirstPerson() || !IsInGame() || GetSettings().GetPlayMode() != PlayMode::STANDING) {
        s_roomscaleState.hasPreviousHeadPos = false;
        return;
    }

    auto* renderer = VRManager::instance().XR->GetRenderer();
    if (renderer == nullptr || !VRManager::instance().XR->m_capabilities.supportsPositional) {
        s_roomscaleState.hasPreviousHeadPos = false;
        return;
    }

    std::optional<glm::fmat4> middlePose = renderer->GetMiddlePose();
    if (!middlePose.has_value()) {
        s_roomscaleState.hasPreviousHeadPos = false;
        return;
    }

    glm::fvec3 currentHeadPos = glm::fvec3(middlePose.value()[3]);
    if (!std::isfinite(currentHeadPos.x) || !std::isfinite(currentHeadPos.y) || !std::isfinite(currentHeadPos.z)) {
        s_roomscaleState.hasPreviousHeadPos = false;
        return;
    }

    PlayerBase player;
    if (!GameUtils::TryReadPlayerBase(player)) {
        s_roomscaleState.hasPreviousHeadPos = false;
        return;
    }

    PlayerMoveBitFlags moveBits = player.moveBitFlags.getLE();
    OpenXR::GameState gameState = VRManager::instance().XR->m_gameState.load();
    bool shouldDisableRoomscale = gameState.is_climbing || gameState.is_paragliding || gameState.is_riding_mount || HAS_FLAG(moveBits, PlayerMoveBitFlags::IS_SWIMMING_OR_CLIMBING | PlayerMoveBitFlags::IS_SWIMMING);
    if (shouldDisableRoomscale) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return;
    }

    if (!s_roomscaleState.hasPreviousHeadPos) {
        PrimeRoomscaleBaseline(currentHeadPos);
        return;
    }

    glm::fvec3 roomDelta = currentHeadPos - s_roomscaleState.previousHeadPos;
    PrimeRoomscaleBaseline(currentHeadPos);

    roomDelta.y = 0.0f;
    if (glm::dot(roomDelta, roomDelta) < kMinRoomscaleMoveDistanceSq) {
        return;
    }

    if (glm::dot(roomDelta, roomDelta) > kMaxRoomscaleJumpDistanceSq) {
        return;
    }

    glm::fvec3 worldDelta = glm::fmat3(s_lastCameraMtx) * roomDelta;
    worldDelta.y = 0.0f;
    if (glm::dot(worldDelta, worldDelta) < kMinRoomscaleMoveDistanceSq) {
        return;
    }

    deltaBE = BEVec3{};
    deltaBE = worldDelta;
    writeMemory(hCPU->gpr[4], &deltaBE);
    hCPU->gpr[3] = 1;
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