#include "pch.h"

#include "instance.h"
#include "cemu_hooks.h"
#include "rendering/openxr.h"

struct Bone {
    std::string name;
    glm::vec3 localPos;
    glm::vec3 localRotEuler; // in radians
    glm::mat4 localMatrix;
    glm::mat4 worldMatrix;
    int parentIndex = -1;
    std::vector<int> childrenIndices;
    int indentLevel = 0;
};

class Skeleton {
public:
    void Parse(const std::string& data) {
        m_bones.clear();
        m_boneNameMap.clear();

        std::stringstream ss(data);
        std::string line;

        // this parses the bone hierarchy using the indentation levels to calculate parent-child relationships and model space transforms
        std::vector<std::pair<int, int>> parentStack;
        parentStack.push_back({ -1, -1 }); // Root parent is -1

        while (std::getline(ss, line)) {
            if (line.empty()) continue;

            int indent = 0;
            while (indent < line.length() && line[indent] == ' ') indent++;

            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;

            std::string content = line.substr(start);
            size_t p1 = content.find('|');
            if (p1 == std::string::npos) continue;

            std::string currentName = content.substr(0, p1);
            size_t lastChar = currentName.find_last_not_of(' ');
            if (lastChar != std::string::npos) currentName = currentName.substr(0, lastChar + 1);

            size_t p2 = content.find('|', p1 + 1);
            if (p2 == std::string::npos) continue;

            glm::vec3 pos, rotEuler;
            std::stringstream ssPos(content.substr(p1 + 1, p2 - p1 - 1));
            ssPos >> pos.x >> pos.y >> pos.z;
            std::stringstream ssRot(content.substr(p2 + 1));
            ssRot >> rotEuler.x >> rotEuler.y >> rotEuler.z;

            Bone bone;
            bone.name = currentName;
            bone.localPos = pos;
            bone.localRotEuler = rotEuler;
            bone.indentLevel = indent;
            bone.localMatrix = glm::translate(glm::identity<glm::mat4>(), pos) * glm::eulerAngleZYX(rotEuler.x, rotEuler.y, rotEuler.z);

            while (parentStack.size() > 1 && parentStack.back().first >= indent) {
                parentStack.pop_back();
            }

            bone.parentIndex = parentStack.back().second;

            int newIndex = (int)m_bones.size();
            if (bone.parentIndex != -1) {
                m_bones[bone.parentIndex].childrenIndices.push_back(newIndex);
            }

            m_bones.push_back(bone);
            m_boneNameMap[bone.name] = newIndex;

            parentStack.push_back({ indent, newIndex });
        }

        UpdateWorldMatrices();
    }

    void UpdateWorldMatrices() {
        for (auto& bone : m_bones) {
            if (bone.parentIndex == -1) {
                bone.worldMatrix = bone.localMatrix;
            }
            else {
                bone.worldMatrix = m_bones[bone.parentIndex].worldMatrix * bone.localMatrix;
            }
        }
    }

    glm::mat4 CalculateLocalMatrixFromWorld(int boneIndex, const glm::mat4& targetWorldMatrix) {
        if (boneIndex < 0 || boneIndex >= m_bones.size()) return glm::identity<glm::mat4>();

        const Bone& bone = m_bones[boneIndex];
        if (bone.parentIndex == -1) {
            return targetWorldMatrix;
        }

        const glm::mat4& parentWorldMatrix = m_bones[bone.parentIndex].worldMatrix;
        return glm::inverse(parentWorldMatrix) * targetWorldMatrix;
    }

    void SolveTwoBoneIK(int rootIdx, int midIdx, int endIdx, const glm::vec3& targetPos, const glm::vec3& poleVector, float boneForwardSign, float upperArmStretchBias = 0.75f) {
        if (rootIdx < 0 || rootIdx >= m_bones.size() ||
            midIdx < 0 || midIdx >= m_bones.size() ||
            endIdx < 0 || endIdx >= m_bones.size()) {
            return;
        }

        Bone& rootBone = m_bones[rootIdx];
        Bone& midBone = m_bones[midIdx];
        Bone& endBone = m_bones[endIdx];

        // get parent world matrix (clavicle)
        glm::mat4 parentWorld = glm::identity<glm::mat4>();
        if (rootBone.parentIndex != -1) {
            parentWorld = m_bones[rootBone.parentIndex].worldMatrix;
        }

        glm::vec3 rootPos = glm::vec3(parentWorld * glm::vec4(rootBone.localPos, 1.0f));

        // get lengths
        float l1_orig = glm::length(midBone.localPos);
        float l2_orig = glm::length(endBone.localPos);
        float l1 = l1_orig;
        float l2 = l2_orig;
        float totalLength = l1 + l2;

        // solve IK
        glm::vec3 dir = targetPos - rootPos;
        float dist = glm::length(dir);

        // weighted stretch: upper arm absorbs more of the extra distance
        float epsilon = 0.001f;
        if (dist > totalLength - epsilon) {
            float extra = dist - totalLength + epsilon;
            l1 += extra * upperArmStretchBias;
            l2 += extra * (1.0f - upperArmStretchBias);
        }

        // clamp distance
        dist = glm::clamp(dist, epsilon, l1 + l2 - epsilon);

        // law of cosines for angle at shoulder (alpha)
        float cosAlpha = (l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist);
        float alpha = glm::acos(glm::clamp(cosAlpha, -1.0f, 1.0f));

        // plane construction
        glm::vec3 dirNorm = glm::normalize(dir);
        glm::vec3 planeNormal = glm::normalize(glm::cross(dirNorm, poleVector));
        glm::vec3 ortho = glm::normalize(glm::cross(planeNormal, dirNorm));

        // arm 1 direction (world)
        glm::vec3 arm1Dir = glm::normalize(dirNorm * cos(alpha) + ortho * sin(alpha));

        // arm 2 direction (world)
        glm::vec3 elbowPos = rootPos + arm1Dir * l1;
        glm::vec3 arm2Dir = glm::normalize(targetPos - elbowPos);

        // construct rotation matrices
        glm::vec3 x1 = arm1Dir * boneForwardSign;
        glm::vec3 z1 = planeNormal;
        glm::vec3 y1 = glm::cross(z1, x1);
        glm::mat3 rot1World = glm::mat3(x1, -y1, -z1);

        glm::vec3 x2 = arm2Dir * boneForwardSign;
        glm::vec3 z2 = planeNormal;
        glm::vec3 y2 = glm::cross(z2, x2);
        glm::mat3 rot2World = glm::mat3(x2, -y2, -z2);

        // convert to local space, stretching the upper arm position more
        glm::mat4 arm1Local = glm::inverse(parentWorld) * glm::mat4(rot1World);
        arm1Local[3] = glm::vec4(rootBone.localPos, 1.0f);

        glm::mat4 arm1World = parentWorld * arm1Local;
        glm::mat4 arm2Local = glm::inverse(arm1World) * glm::mat4(rot2World);
        arm2Local[3] = glm::vec4(midBone.localPos * (l1 / l1_orig), 1.0f);

        // update skeleton
        rootBone.localMatrix = arm1Local;
        midBone.localMatrix = arm2Local;
        UpdateWorldMatrices();
    }

    int GetBoneIndex(const std::string& name) const {
        auto it = m_boneNameMap.find(name);
        if (it != m_boneNameMap.end()) return it->second;
        return -1;
    }

    Bone* GetBone(int index) {
        if (index < 0 || index >= m_bones.size()) return nullptr;
        return &m_bones[index];
    }

    Bone* GetBone(const std::string& name) {
        int idx = GetBoneIndex(name);
        if (idx == -1) return nullptr;
        return &m_bones[idx];
    }

private:
    std::vector<Bone> m_bones;
    std::map<std::string, int> m_boneNameMap;
};

const std::string SKELETON_DATA = R"(
Root | 0 0 0 | 0 0 0
  Skl_Root | 0 0.99426 0 | 0 0 0
    Spine_1 | 0 0 0 | 1.5708 0 1.5708
      Spine_2 | 0.136 0 0 | 0 0 0
        Clavicle_L | 0.23961 -0.00002 0.03291 | 0 -1.5708 0
          Arm_1_L | 0.15 0 0.01074 | 0 0 0
            Arm_1_Assist_L | 0.06 0.00002 0 | 0 0 0
            Arm_2_L | 0.24 0 0 | 0 0 0
              Elbow_L | 0.04151 -0.02934 0.00021 | 0 0 0
              Wrist_Assist_L | 0.25809 0.00002 -0.00012 | 0 0 0
              Wrist_L | 0.27718 0 0 | 0 0 0
                Weapon_L | 0.1069 0.00002 0.02769 | 1.5708 0 3.14159
          Clavicle_Assist_L | 0.116 0 0.0107 | 0 0 0
        Clavicle_R | 0.2396 -0.00002 -0.03291 | 3.14159 -1.5708 0
          Arm_1_R | -0.15 0 -0.01074 | 0 0 0
            Arm_1_Assist_R | -0.06 -0.00002 0 | 0 0 0
            Arm_2_R | -0.24 0 0 | 0 0 0
              Elbow_R | -0.04151 0.02934 -0.0002 | 0 0 0
              Wrist_Assist_R | -0.25809 -0.00002 0.00012 | 0 0 0
              Wrist_R | -0.27718 0 0 | 0 0 0
                Weapon_R | -0.1069 -0.00002 -0.02769 | 1.5708 0 0
          Clavicle_Assist_R | -0.116 0 -0.0107 | 0 0 0
        Neck | 0.26326 0 0 | 0 0 0
          Head | 0.12447 0 0 | 0 0 0
            Face_Root | 0 0 0 | 0 0 0
              Chin | 0.04787 0.05757 0 | 0 0 2.53073
              Eyeball_L | 0.07017 0.12036 0.04815 | 0 0 0
              Eyeball_R | 0.07017 0.12036 -0.04815 | 0 0 0
)";

/*
    Waist | 0 0 0 | 1.5708 0 -1.5708
      Leg_1_L | 0.10854 0.0165 -0.11209 | 0 0 0
        Knee_L | 0.39619 0.0308 0 | 0 0 0
        Leg_2_L | 0.42 0 -0.08727 | 0 0 0
      Leg_1_R | 0.10854 0.0165 0.11209 | 0 0 3.14159
        Knee_R | -0.39619 -0.0308 0 | 0 0 0
        Leg_2_R | -0.42 0 -0.08727 | 0 0 0
 */

static bool isFaceBone(const std::string_view& boneName) {
    if (boneName.starts_with("Eye" /*lid*/) || boneName.starts_with("Cheek") || boneName.starts_with("Lip") || boneName.starts_with("Hair") || boneName.starts_with("Ponytail") || boneName.starts_with("Momi")) {
        return true;
    }
    if (boneName == "Nose" || boneName == "Face_Root" || boneName == "Neck" || boneName == "Head" || boneName.starts_with("Teeth_") || boneName.starts_with("Chin")) {
        return true;
    }
    return false;
}

static bool isFaceMaterialTriggerBone(const std::string_view& boneName) {
    return boneName == "Face_Root" || boneName == "Head" || boneName == "Eyeball_L" || boneName == "Eyeball_R";
}

static Skeleton s_skeleton;
static bool s_skeletonParsed = false;
static glm::vec3 s_manualBodyOffset = glm::vec3(0.0f, 0.0f, -0.075f);
static glm::mat4 s_handCorrectionRotationLeft = glm::mat4(1.0f);
static glm::mat4 s_handCorrectionRotationRight = glm::mat4(1.0f);

static glm::fquat ExtractPlayerBodyYaw(const glm::fmat4& sourceMtx) {
    glm::fquat sourceRot = glm::quat_cast(sourceMtx);
    float yProj = glm::dot(glm::fvec3(sourceRot.x, sourceRot.y, sourceRot.z), glm::fvec3(0.0f, 1.0f, 0.0f));
    glm::fquat yawRot(sourceRot.w, 0.0f, yProj, 0.0f);
    float lenSq = glm::dot(yawRot, yawRot);
    yawRot = (lenSq > 0.000001f) ? yawRot * (1.0f / sqrtf(lenSq)) : glm::identity<glm::fquat>();

    // fix body inversion
    return yawRot * glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

static glm::fvec3 RemoveHeadsetHorizontalOffset(glm::fvec3 trackedPos) {
    glm::fvec3 appliedHeadPos = CemuHooks::GetAppliedRoomscaleHeadPosition();
    trackedPos.x -= appliedHeadPos.x;
    trackedPos.z -= appliedHeadPos.z;
    return trackedPos;
}

// Two-handed grip: while a large sword or spear is drawn, both hands steer the weapon and the left hand is
// pinned onto the hilt at Link's canonical grip spot. Recomputed once per frame at the Skl_Root bone.
static constexpr uint32_t TWO_HAND_WEAPON_ACTOR_MTX_OFFSET = 0x1F8; // same actor world matrix bow.cpp reads (kWeaponActorMtxOffset)
static constexpr float TWO_HAND_SWORD_LEFT_GRIP_OFFSET = -0.12f; // claymore: left hand on the hilt toward the pommel, behind the right hand
static constexpr float TWO_HAND_SPEAR_LEFT_GRIP_OFFSET = 0.40f; // spear: left hand leads on the shaft
static constexpr float TWO_HAND_MIN_HAND_SEPARATION = 0.10f; // below this the hand-to-hand axis is degenerate, keep the previous one
static constexpr float TWO_HAND_BLADE_AXIS_SMOOTHING = 0.15f; // per-frame blend toward the newly solved blade axis

struct TwoHandGripState {
    bool engaged = false;
    glm::vec3 rearOriginWorld = glm::vec3(0.0f);
    glm::vec3 leftPinnedWorld = glm::vec3(0.0f);
    glm::fquat rightGripRotWorld = glm::identity<glm::fquat>();
    glm::fquat leftGripRotWorld = glm::identity<glm::fquat>();
};
static TwoHandGripState s_twoHandGrip;

// blade-axis solver state: the weapon actor matrix in memory was produced by the wrist rotation we applied
// last frame, so the blade's direction in the corrected right-hand frame can be measured instead of assumed
static glm::fquat s_lastAppliedRightWristRot = glm::identity<glm::fquat>();
static bool s_lastAppliedRightWristRotValid = false;
static uint32_t s_bladeAxisWeaponPtr = 0;
static int s_bladeModelAxisIdx = -1; // 0..5 = +X,-X,+Y,-Y,+Z,-Z of the weapon model, latched per weapon
static glm::fvec3 s_bladeAxisInHand = glm::fvec3(0.0f);
static bool s_bladeAxisValid = false;
static glm::fvec3 s_lastGripAxisWorld = glm::fvec3(0.0f);

static glm::fquat MinimalArcQuat(const glm::fvec3& from, const glm::fvec3& to) {
    const glm::fvec3 fromDir = glm::normalize(from);
    const glm::fvec3 toDir = glm::normalize(to);
    const float alignment = glm::clamp(glm::dot(fromDir, toDir), -1.0f, 1.0f);
    if (alignment > 0.99999f)
        return glm::identity<glm::fquat>();

    if (alignment < -0.99999f) {
        // anti-parallel directions have no unique rotation axis, so pick any perpendicular one
        glm::fvec3 axis = glm::cross(fromDir, glm::fvec3(0.0f, 1.0f, 0.0f));
        if (glm::length2(axis) < 0.000001f)
            axis = glm::cross(fromDir, glm::fvec3(1.0f, 0.0f, 0.0f));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }

    return glm::angleAxis(std::acos(alignment), glm::normalize(glm::cross(fromDir, toDir)));
}

void CemuHooks::hook_ModifyBoneMatrix(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    hCPU->gpr[7] = 0;
    hCPU->gpr[8] = 0;

    const uint32_t gsysModelPtr = hCPU->gpr[3];
    const uint32_t matrixPtr = hCPU->gpr[4];
    const uint32_t scalePtr = hCPU->gpr[5];
    const uint32_t boneNamePtr = hCPU->gpr[6];
    if (!gsysModelPtr || !matrixPtr || !scalePtr || !boneNamePtr) return;

    const auto modelName = getMemory<sead::FixedSafeString100>(gsysModelPtr + 0x128);
    if (modelName.getLE() != "GameROMPlayer") return;

    std::string boneName((char*)(s_memoryBaseAddress + boneNamePtr));
    const bool isLeft = boneName.ends_with("_L");
    const OpenXR::EyeSide side = isLeft ? OpenXR::EyeSide::LEFT : OpenXR::EyeSide::RIGHT;

    // reset before any early-return below, otherwise a tracking loss can strand a stale engaged grip
    if (boneName == "Skl_Root")
        s_twoHandGrip = {};

    // helpers to write back matrix and scale
    auto writeBoneMatrix = [&](const glm::mat4x3& mtx, const glm::fvec3& scale) {
        BEMatrix34 m{ mtx };
        setMemory(matrixPtr, m);
        setMemory(scalePtr, scale);
    };
    auto writeBoneQuat = [&](const glm::vec3& pos, const glm::quat& rot, const glm::fvec3& scale) {
        BEMatrix34 m{ pos, rot };
        setMemory(matrixPtr, m);
        setMemory(scalePtr, scale);
    };

    const bool isThirdPerson = IsThirdPerson();

    if (isThirdPerson) {
        // the wrist is game-animated here, so last frame's stored rotation no longer matches the weapon matrix
        s_lastAppliedRightWristRotValid = false;
        if (isFaceBone(boneName)) {
            setMemory(scalePtr, glm::fvec3(1.0f));
        }
        return;
    }

    if (isFaceMaterialTriggerBone(boneName)) {
        hCPU->gpr[7] = 1;
        hCPU->gpr[8] = 1;
    }

    if (isFaceBone(boneName)) {
        return;
    }

    glm::fvec3 boneScale = getMemory<BEVec3>(scalePtr).getLE();
    const glm::fmat4 playerMtx4 = glm::fmat4(getMemory<BEMatrix34>(s_playerMtxAddress).getLEMatrix());
    const glm::mat4 cameraMtx = s_lastCameraMtx;

    const OpenXR::InputState inputs = VRManager::instance().XR->m_input.load();
    if (!inputs.shared.pose[side].isActive) {
        if (boneName == "Skl_Root")
            s_lastAppliedRightWristRotValid = false;
        return;
    }

    const auto& pose = inputs.shared.poseLocation[side];
    glm::fvec3 controllerPos = glm::fvec3();
    glm::fquat controllerRot = glm::identity<glm::fquat>();
    if (pose.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        controllerPos = ToGLM(pose.pose.position);
        controllerPos = RemoveHeadsetHorizontalOffset(controllerPos);
    }
    if (pose.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
        controllerRot = ToGLM(pose.pose.orientation);

    // one-time skeleton and hand correction initialization
    if (!s_skeletonParsed) {
        s_skeleton.Parse(SKELETON_DATA);
        s_skeletonParsed = true;

        // left: 90 Y -> -90 Z -> 30 Z
        glm::fquat wristL = glm::identity<glm::fquat>();
        wristL *= glm::angleAxis(glm::radians(90.0f), glm::fvec3(0, 1, 0));
        wristL *= glm::angleAxis(glm::radians(-90.0f), glm::fvec3(0, 0, 1));
        wristL *= glm::angleAxis(glm::radians(30.0f), glm::fvec3(0, 0, 1));

        // right: -90 Z -> -180 Y -> 270 X -> 30 Z tweak
        glm::fquat wristR = glm::identity<glm::fquat>();
        wristR *= glm::angleAxis(glm::radians(-90.0f), glm::fvec3(0, 0, 1));
        wristR *= glm::angleAxis(glm::radians(-180.0f), glm::fvec3(0, 1, 0));
        wristR *= glm::angleAxis(glm::radians(270.0f), glm::fvec3(1, 0, 0));
        wristR *= glm::angleAxis(glm::radians(30.0f), glm::fvec3(0, 0, 1));

        s_handCorrectionRotationLeft = glm::mat4_cast(wristL);
        s_handCorrectionRotationRight = glm::mat4_cast(wristR);
    }

    int boneIndex = s_skeleton.GetBoneIndex(boneName);
    if (boneIndex == -1)
        return;

    // compute the controller target in model space
    auto calcControllerTargetModel = [&]() -> glm::mat4 {
        glm::mat4 handCorrectionMtx = isLeft ? s_handCorrectionRotationLeft : s_handCorrectionRotationRight;
        glm::mat4 controllerMat = glm::translate(glm::identity<glm::mat4>(), controllerPos) * glm::mat4_cast(controllerRot) * handCorrectionMtx;
        glm::mat4 targetWorld = cameraMtx * controllerMat;

        const char* weaponName = isLeft ? "Weapon_L" : "Weapon_R";
        if (Bone* weapon = s_skeleton.GetBone(weaponName)) {
            glm::vec3 weaponOffset = glm::vec3(weapon->localMatrix[3]);
            targetWorld = targetWorld * glm::translate(glm::identity<glm::mat4>(), -weaponOffset);
        }

        return glm::inverse(playerMtx4) * targetWorld;
    };

    // like calcControllerTargetModel, but with both wrists aligned to the two-handed grip axis and the left hand pinned onto the hilt
    auto calcTwoHandGripTargetModel = [&]() -> glm::mat4 {
        const glm::vec3 gripOrigin = isLeft ? s_twoHandGrip.leftPinnedWorld : s_twoHandGrip.rearOriginWorld;
        const glm::fquat gripRot = isLeft ? s_twoHandGrip.leftGripRotWorld : s_twoHandGrip.rightGripRotWorld;
        glm::mat4 targetWorld = glm::translate(glm::identity<glm::mat4>(), gripOrigin) * glm::mat4_cast(gripRot);

        const char* weaponName = isLeft ? "Weapon_L" : "Weapon_R";
        if (Bone* weapon = s_skeleton.GetBone(weaponName)) {
            glm::vec3 weaponOffset = glm::vec3(weapon->localMatrix[3]);
            targetWorld = targetWorld * glm::translate(glm::identity<glm::mat4>(), -weaponOffset);
        }

        return glm::inverse(playerMtx4) * targetWorld;
    };

    glm::mat4 calculatedLocalMat = s_skeleton.GetBone(boneIndex)->localMatrix;

    // override the root transform so the body aligns with the headset yaw
    if (boneName == "Skl_Root") {
        s_twoHandGripDebug.skelRootPasses++;
        s_twoHandGripDebug.handSeparation = 0.0f;

        auto headsetPose = VRManager::instance().XR->GetRenderer()->GetMiddlePose();
        glm::mat4 headsetMtx = headsetPose.value_or(ToMat4(glm::fvec3(0)));

        // calculate eye offset from eyeball bones (once)
        static glm::vec3 eyeOffset = glm::vec3(0.0f);
        static bool offsetCalculated = false;
        if (!offsetCalculated) {
            Bone* eyeL = s_skeleton.GetBone("Eyeball_L");
            Bone* eyeR = s_skeleton.GetBone("Eyeball_R");
            Bone* sklRoot = s_skeleton.GetBone("Skl_Root");
            if (eyeL && eyeR && sklRoot) {
                glm::vec3 eyePos = (glm::vec3(eyeL->worldMatrix[3]) + glm::vec3(eyeR->worldMatrix[3])) * 0.5f;
                eyeOffset = eyePos - glm::vec3(sklRoot->worldMatrix[3]);
                offsetCalculated = true;
            }
        }

        // headset in model space
        glm::mat4 headsetModel = glm::inverse(playerMtx4) * cameraMtx * headsetMtx;
        headsetModel[3].x = 0.0f;
        headsetModel[3].z = 0.0f;

        glm::fquat yawRot = ExtractPlayerBodyYaw(headsetModel);

        // position the root so that yawRot * eyeOffset lands at the headset position
        glm::vec3 targetPos = glm::vec3(headsetModel[3]) - (yawRot * eyeOffset) + (yawRot * s_manualBodyOffset);

        // update skeleton for children (hands)
        if (Bone* rootBone = s_skeleton.GetBone("Skl_Root")) {
            rootBone->localMatrix = glm::translate(glm::identity<glm::mat4>(), targetPos) * glm::mat4_cast(yawRot);
            s_skeleton.UpdateWorldMatrices();
        }

        auto calcHandWorldMat = [&](OpenXR::EyeSide handSide) {
            const auto& handPoseLocation = inputs.shared.poseLocation[handSide];
            glm::fvec3 handPos = glm::fvec3();
            glm::fquat handRot = glm::identity<glm::fquat>();
            if (handPoseLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
                handPos = RemoveHeadsetHorizontalOffset(ToGLM(handPoseLocation.pose.position));
            if (handPoseLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
                handRot = ToGLM(handPoseLocation.pose.orientation);
            const glm::mat4 handCorrectionMtx = handSide == OpenXR::EyeSide::LEFT ? s_handCorrectionRotationLeft : s_handCorrectionRotationRight;
            return cameraMtx * glm::translate(glm::identity<glm::mat4>(), handPos) * glm::mat4_cast(handRot) * handCorrectionMtx;
        };

        const glm::mat4 rightHandWorld = calcHandWorldMat(OpenXR::EyeSide::RIGHT);
        const glm::fquat rightHandRotWorld = glm::quat_cast(glm::mat3(rightHandWorld));

        // solve where the blade actually points in the corrected right-hand frame: the weapon actor's world
        // matrix still reflects last frame's stored wrist rotation, so the model->hand chain falls out exactly
        const uint32_t rightWeaponPtr = m_heldWeapons[OpenXR::EyeSide::RIGHT];
        if (s_twoHandGripEnabled && rightWeaponPtr != 0 && s_lastAppliedRightWristRotValid) {
            BEMatrix34 weaponMtxBE = {};
            readMemory(rightWeaponPtr + TWO_HAND_WEAPON_ACTOR_MTX_OFFSET, &weaponMtxBE);

            // normalize the basis columns since weapon actors can be scaled
            glm::mat3 weaponRotWorld = glm::mat3(glm::mat4(weaponMtxBE.getLEMatrix()));
            bool weaponRotUsable = true;
            for (int col = 0; col < 3; col++) {
                const float axisLength = glm::length(weaponRotWorld[col]);
                if (!std::isfinite(axisLength) || axisLength < 0.0001f) {
                    weaponRotUsable = false;
                    break;
                }
                weaponRotWorld[col] /= axisLength;
            }

            if (weaponRotUsable) {
                if (s_bladeAxisWeaponPtr != rightWeaponPtr) {
                    s_bladeAxisWeaponPtr = rightWeaponPtr;
                    s_bladeAxisValid = false;
                    s_bladeModelAxisIdx = -1;
                }

                const glm::mat3 chainRot = glm::mat3_cast(glm::inverse(s_lastAppliedRightWristRot)) * weaponRotWorld;

                if (s_bladeModelAxisIdx == -1) {
                    // latch the weapon-model axis closest to the corrected hand's forward as the blade direction
                    float bestScore = -2.0f;
                    for (int axisIdx = 0; axisIdx < 6; axisIdx++) {
                        const glm::fvec3 candidate = glm::normalize(chainRot[axisIdx / 2] * (axisIdx % 2 == 0 ? 1.0f : -1.0f));
                        const float score = glm::dot(candidate, glm::fvec3(0.0f, 0.0f, -1.0f));
                        if (score > bestScore) {
                            bestScore = score;
                            s_bladeModelAxisIdx = axisIdx;
                        }
                    }
                    Log::print<CONTROLS>("Two-hand grip: latched blade model axis {} (score {}) for weapon {:#x}", s_bladeModelAxisIdx, bestScore, rightWeaponPtr);
                }

                const glm::fvec3 newBladeAxis = glm::normalize(chainRot[s_bladeModelAxisIdx / 2] * (s_bladeModelAxisIdx % 2 == 0 ? 1.0f : -1.0f));
                s_bladeAxisInHand = s_bladeAxisValid ? glm::normalize(glm::mix(s_bladeAxisInHand, newBladeAxis, TWO_HAND_BLADE_AXIS_SMOOTHING)) : newBladeAxis;
                s_bladeAxisValid = true;
            }
        }

        if (IsTwoHandGripEngaged() && s_bladeAxisValid) {
            const glm::mat4 leftHandWorld = calcHandWorldMat(OpenXR::EyeSide::LEFT);
            const glm::vec3 rearPos = glm::vec3(rightHandWorld[3]);
            const glm::vec3 rearToLeft = glm::vec3(leftHandWorld[3]) - rearPos;
            const float handSeparation = glm::length(rearToLeft);
            s_twoHandGripDebug.handSeparation = handSeparation;

            // the signed offset encodes both spacing and hand order: negative = left hand behind the right
            // toward the pommel (claymore), positive = left hand leading on the shaft (spear)
            const float leftGripOffset = s_handWeaponTypes[OpenXR::EyeSide::RIGHT] == WeaponType::Spear ? TWO_HAND_SPEAR_LEFT_GRIP_OFFSET : TWO_HAND_SWORD_LEFT_GRIP_OFFSET;

            glm::vec3 bladeDir = s_lastGripAxisWorld;
            if (handSeparation > TWO_HAND_MIN_HAND_SEPARATION) {
                bladeDir = (rearToLeft / handSeparation) * (leftGripOffset < 0.0f ? -1.0f : 1.0f);
                s_lastGripAxisWorld = bladeDir;
            }

            if (glm::length2(bladeDir) > 0.5f) {
                const glm::fquat leftHandRotWorld = glm::quat_cast(glm::mat3(leftHandWorld));
                // the left hand frame mirrors the right one across the body plane
                const glm::fvec3 leftBladeInHand = glm::fvec3(-s_bladeAxisInHand.x, s_bladeAxisInHand.y, s_bladeAxisInHand.z);

                s_twoHandGrip.engaged = true;
                s_twoHandGrip.rearOriginWorld = rearPos;
                s_twoHandGrip.leftPinnedWorld = rearPos + bladeDir * leftGripOffset;
                s_twoHandGrip.rightGripRotWorld = MinimalArcQuat(rightHandRotWorld * s_bladeAxisInHand, bladeDir) * rightHandRotWorld;
                s_twoHandGrip.leftGripRotWorld = MinimalArcQuat(leftHandRotWorld * leftBladeInHand, bladeDir) * leftHandRotWorld;
            }
        }

        // store the rotation the right wrist actually gets this frame; next frame's solve pairs it with the weapon matrix it produces
        s_lastAppliedRightWristRot = s_twoHandGrip.engaged ? s_twoHandGrip.rightGripRotWorld : rightHandRotWorld;
        s_lastAppliedRightWristRotValid = true;

        s_twoHandGripDebug.isBladeAxisValid = s_bladeAxisValid;
        s_twoHandGripDebug.bladeModelAxisIdx = s_bladeModelAxisIdx;
        s_twoHandGripDebug.bladeAxisInHand = s_bladeAxisInHand;
        s_twoHandGripDebug.isEngaged = s_twoHandGrip.engaged;

        writeBoneQuat(targetPos, yawRot, boneScale);
        return;
    }

    // solve upper arm IK so the arms reach the VR controllers
    if (boneName == "Arm_1_L" || boneName == "Arm_1_R" ||
        boneName == "Elbow_L" || boneName == "Elbow_R" ||
        boneName == "Wrist_Assist_L" || boneName == "Wrist_Assist_R") {

        int arm1Index = s_skeleton.GetBoneIndex(isLeft ? "Arm_1_L" : "Arm_1_R");
        int arm2Index = s_skeleton.GetBoneIndex(isLeft ? "Arm_2_L" : "Arm_2_R");
        int wristIndex = s_skeleton.GetBoneIndex(isLeft ? "Wrist_L" : "Wrist_R");

        if (arm1Index != -1 && arm2Index != -1 && wristIndex != -1) {
            glm::vec3 targetPos = glm::vec3((s_twoHandGrip.engaged ? calcTwoHandGripTargetModel() : calcControllerTargetModel())[3]);

            // pole vector (elbow hint): left-down-back / right-down-back, rotated by body yaw
            glm::vec3 poleDir = isLeft ? glm::vec3(1.0f, -1.0f, -0.5f) : glm::vec3(-1.0f, -1.0f, -0.5f);
            if (Bone* rootBone = s_skeleton.GetBone("Skl_Root"))
                poleDir = glm::quat_cast(rootBone->localMatrix) * poleDir;

            s_skeleton.SolveTwoBoneIK(arm1Index, arm2Index, wristIndex, targetPos, poleDir, isLeft ? 1.0f : -1.0f);
            calculatedLocalMat = s_skeleton.GetBone(boneIndex)->localMatrix;
        }
    }

    // align the wrist and wrist assist directly with the controller pose, or with the two-handed grip while it's engaged
    if (boneName == "Wrist_L" || boneName == "Wrist_R" ||
        boneName == "Wrist_Assist_L" || boneName == "Wrist_Assist_R") {
        calculatedLocalMat = s_skeleton.CalculateLocalMatrixFromWorld(boneIndex, s_twoHandGrip.engaged ? calcTwoHandGripTargetModel() : calcControllerTargetModel());
    }

    writeBoneMatrix(glm::mat4x3(calculatedLocalMat), boneScale);
}
