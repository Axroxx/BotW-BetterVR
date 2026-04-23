#include "pch.h"

#include "cemu_hooks.h"

bool shouldAdjustArrowTarget = false;

// CarryBox is used to carry all the veggies in the game while cooking
// GameSceneSubsys12::x(GameSceneSubsys12::sInstance) returns 0 for carryable, 1 is for carryable


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

    // get LR from stack, using lwz r0, 0x1C(r1)
    BEType<uint32_t> addr;
    readMemory(hCPU->gpr[1] + 0x1C, &addr);

    // we overwrote the .z setting instruction with the jump, so run the original code
    vec3Param.value.z = hCPU->fpr[0].fp0;
    writeMemory(hCPU->gpr[31], &vec3Param);

    //// now hook the value if needed
    //const char* paramString = (char*)(vec3Param.keyPtr.getLE() + s_memoryBaseAddress);
    //Log::print<PPC>("[{:08X}] Original velocity parameter for {}: {}", addr.getLE(), paramString, vec3Param.value);

    // only hook the arrow target position for now
    //if (std::string(paramString) == "TargetPos" && shouldAdjustArrowTarget) {
    //    //Log::print<INFO>("Original target: {}", vec3Param.value.getLE());
    //    s_lastScreenAimOriginalTarget = vec3Param.value.getLE();
    //    s_lastScreenAimValid = false;
    //}

    //velocityParam.value = glm::fvec3(0.0f, 0.0f, 0.0f);
}

void CemuHooks::hook_SetRigidBodyVelocity(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03489710;
    hCPU->gpr[0] = hCPU->sprNew.LR;
    uint32_t lr = hCPU->sprNew.LR;

    //uint32_t rigidBodyAddr = hCPU->gpr[3];
    //BEVec3 velocity;
    //readMemory(hCPU->gpr[4], &velocity);
    //double timeDeltaMaybe = hCPU->fpr[1].fp0;

    //float debugLineLength = glm::max(s_lastScreenAimDistance, 10.0f);
    //if (s_lastGameplayAimValid) {
    //    DebugDraw::instance().Line(s_lastGameplayAimOrigin, s_lastGameplayAimOrigin + (s_lastGameplayAimDirection * debugLineLength), IM_COL32(0, 255, 255, 255), 2.0f);
    //}
    //if (s_lastRenderAimValid) {
    //    DebugDraw::instance().Line(s_lastRenderAimOrigin, s_lastRenderAimOrigin + (s_lastRenderAimDirection * debugLineLength), IM_COL32(255, 0, 255, 255), 2.0f);
    //}
    //if (s_lastScreenAimValid) {
    //    DebugDraw::instance().Line(s_lastScreenAimOrigin, s_lastScreenAimAdjustedTarget, IM_COL32(255, 255, 0, 255), 2.0f);
    //    DebugDraw::instance().Line(s_lastScreenAimOriginalTarget, s_lastScreenAimOriginalTarget + glm::fvec3(0.0f, 0.5f, 0.0f), IM_COL32(255, 64, 64, 255), 1.0f);
    //    DebugDraw::instance().Line(s_lastScreenAimAdjustedTarget, s_lastScreenAimAdjustedTarget + glm::fvec3(0.0f, 0.5f, 0.0f), IM_COL32(64, 255, 64, 255), 1.0f);
    //}
    //

    //if (lr == 0x020E735C) {
    //    Log::print<PPC>("[{:08X}] Set rigid body velocity of {:08X}: {} (timeDeltaMaybe = {})", hCPU->sprNew.LR, rigidBodyAddr, velocity, timeDeltaMaybe);
    //    //velocity = glm::fvec3(0.0f, 0.0f, 0.0f);
    //    //velocity.x = 0.0f;
    //    writeMemory(hCPU->gpr[4], &velocity);
    //}
}

void CemuHooks::hook_SetRigidBodyTransform(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486CD8;
    hCPU->gpr[0] = hCPU->sprNew.LR;
    uint32_t lr = hCPU->sprNew.LR;

    //uint32_t rigidBodyAddr = hCPU->gpr[3];
    //BEMatrix34 transform;
    //readMemory(hCPU->gpr[4], &transform);

    //DebugDraw::instance().Dot(transform.getPos().getLE(), 5.0f, IM_COL32(0, 0, 255, 255));
    //
    //Log::print<INFO>("[{:08X}] Set rigid body transform of {:08X}: pos = {}, rot = {}", lr, rigidBodyAddr, transform.getPos().getLE(), transform.getRotLE());

    //if (lr == 0x020E3D0C || lr == 0x02C19A18) {
    //    shouldAdjustArrowTarget = false;

    //    glm::fvec3 originalPos = transform.getPos().getLE();
    //    glm::fquat originalRot = transform.getRotLE();
    //    Log::print<INFO>("[ARROW TRANSFORM] Original pos = {}, rot = {}", originalPos, originalRot);

    //    auto renderer = VRManager::instance().XR->GetRenderer();
    //    if (renderer != nullptr) {
    //        auto rightPoseOpt = renderer->GetPose(EyeSide::RIGHT);
    //        if (rightPoseOpt.has_value()) {
    //            glm::fvec3 headsetPos = ToGLM(rightPoseOpt->position);
    //            glm::fquat headsetRot = ToGLM(rightPoseOpt->orientation);

    //            auto [swing, baseYaw] = swingTwistY(s_wsCameraRotation);
    //            glm::fvec3 headsetPosNoHeight = headsetPos;
    //            //headsetPosNoHeight.y = 0.0f;
    //            glm::fvec3 aimOrigin = s_wsCameraPosition + (baseYaw * headsetPosNoHeight);
    //            glm::fquat aimRotation = baseYaw * headsetRot;
    //            glm::fvec3 aimDirection = glm::normalize(aimRotation * glm::fvec3(0.0f, 0.0f, -1.0f));

    //            transform.setPos(aimOrigin);
    //            transform.setRotLE(aimRotation);
    //            //writeMemory(hCPU->gpr[4], &transform);

    //            s_lastScreenAimOrigin = aimOrigin;
    //            s_lastScreenAimAdjustedTarget = aimOrigin + (aimDirection * s_lastScreenAimDistance);
    //            s_lastScreenAimValid = true;

    //            Log::print<INFO>("[Arrow Stuff] New pos = {}, rot = {}, aimDir = {}", aimOrigin, aimRotation, aimDirection);
    //        }
    //    }
    //}
}

void CemuHooks::hook_SetRigidBodyPosition(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03486D84;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    //uint32_t lr = hCPU->sprNew.LR;
    //uint32_t rigidBodyAddr = hCPU->gpr[3];
    //BEVec3 position;
    //readMemory(hCPU->gpr[4], &position);
    ////Log::print<INFO>("[{:08X}] Set rigid body position of {:08X}: pos = {}", lr, rigidBodyAddr, position);

    //DebugDraw::instance().Dot(position.getLE(), 5.0f, IM_COL32(255, 0, 0, 255));
}

void CemuHooks::hook_SetRigidBodyPositionAndRotation(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x03489A84;
    hCPU->gpr[11] = hCPU->gpr[1];

    //uint32_t lr = hCPU->sprNew.LR;
    //uint32_t rigidBodyAddr = hCPU->gpr[3];
    //BEMatrix34 transform;
    //readMemory(hCPU->gpr[4], &transform);

    //DebugDraw::instance().Dot(transform.getPos().getLE(), 5.0f, IM_COL32(255, 255, 0, 255));
    //Log::print<INFO>("[{:08X}] Set rigid body position and rotation of {:08X}: pos = {}, rotation = {}", lr, rigidBodyAddr, transform.getPos().getLE(), transform.getRotLE());
}

void CemuHooks::hook_SetRigidBodyRotation(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = 0x0348AB24;
    hCPU->gpr[0] = hCPU->sprNew.LR;

    //uint32_t lr = hCPU->sprNew.LR;
    //uint32_t rigidBodyAddr = hCPU->gpr[3];
    //BEMatrix34 transform;
    //readMemory(hCPU->gpr[4], &transform);

    //Log::print<INFO>("[{:08X}] Set rigid body rotation of {:08X}: rotation = {}", lr, rigidBodyAddr, transform.getRotLE());
}