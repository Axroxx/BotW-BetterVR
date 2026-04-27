[BetterVR_EntityController_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

HandleSetRigidBodyVelocityWithBreakpoint:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
stw r30, 0x10(r1)
stw r3, 0x08(r1)
stw r4, 0x0C(r1)
mr r30, r0

bla import.coreinit.hook_SetRigidBodyVelocity
cmpwi r3, 0
beq HandleSetRigidBodyVelocityWithBreakpoint_SkipNop
nop

HandleSetRigidBodyVelocityWithBreakpoint_SkipNop:
lwz r3, 0x08(r1)
lwz r4, 0x0C(r1)
lwz r30, 0x10(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
lis r12, ContinueAfterSetRigidBodyVelocityWithBreakpoint@ha
addi r12, r12, ContinueAfterSetRigidBodyVelocityWithBreakpoint@l
mtctr r12
bctr

RoomscaleVelocity_Delta = 0x20
RoomscaleVelocity_Start = 0x30
RoomscaleVelocity_Hit = 0x40
RoomscaleVelocity_LayerMask = 0x50
RoomscaleVelocity_Query = 0x60

ApplyPlayerVelocityWithRoomscale:
stwu r1, -0x100(r1)
mflr r0
stw r0, 0x104(r1)
stw r31, 0x14(r1)
stw r4, 0x08(r1)
stw r5, 0x0C(r1)
stw r6, 0x10(r1)
lwz r11, 0x00(r1)
stfs f12, 0x08(r11)

addi r6, r1, RoomscaleVelocity_Delta
bl import.coreinit.hook_InjectRoomscaleVelocity

lwz r31, 0x14(r1)
cmpwi r3, 0
beq ApplyPlayerVelocityWithRoomscale_Apply

li r0, 15
stw r0, RoomscaleVelocity_LayerMask(r1)

addi r3, r1, RoomscaleVelocity_Query
li r4, 0
addi r5, r1, RoomscaleVelocity_LayerMask
bl RayCastBodyQuery_ctor

addi r3, r1, RoomscaleVelocity_Query
addi r4, r31, 0x208
bl RayCast_setLayers

mr r3, r31
addi r4, r1, RoomscaleVelocity_Start
bl RigidBody_getPosition

addi r3, r1, RoomscaleVelocity_Query
addi r4, r1, RoomscaleVelocity_Start
addi r5, r1, RoomscaleVelocity_Delta
bl RayCast_setFromAndDelta

addi r3, r1, RoomscaleVelocity_Query
li r4, 0
bl RayCastBodyQuery_worldRayCast
cmpwi r3, 0
beq ApplyPlayerVelocityWithRoomscale_DestroyQuery

addi r3, r1, RoomscaleVelocity_Query
addi r4, r1, RoomscaleVelocity_Hit
bl RayCast_getHitPosition

lfs f0, RoomscaleVelocity_Hit + 0x0(r1)
lfs f1, RoomscaleVelocity_Start + 0x0(r1)
fsubs f0, f0, f1
stfs f0, RoomscaleVelocity_Delta + 0x0(r1)

lfs f0, RoomscaleVelocity_Hit + 0x4(r1)
lfs f1, RoomscaleVelocity_Start + 0x4(r1)
fsubs f0, f0, f1
stfs f0, RoomscaleVelocity_Delta + 0x4(r1)

lfs f0, RoomscaleVelocity_Hit + 0x8(r1)
lfs f1, RoomscaleVelocity_Start + 0x8(r1)
fsubs f0, f0, f1
stfs f0, RoomscaleVelocity_Delta + 0x8(r1)

ApplyPlayerVelocityWithRoomscale_DestroyQuery:
addi r3, r1, RoomscaleVelocity_Query
li r4, 2
bl RayCastBodyQuery_dtor

lwz r4, 0x08(r1)

lfs f0, 0x0(r4)
lfs f1, RoomscaleVelocity_Delta + 0x0(r1)
fadds f0, f0, f1
stfs f0, 0x0(r4)

lfs f0, 0x4(r4)
lfs f1, RoomscaleVelocity_Delta + 0x4(r1)
fadds f0, f0, f1
stfs f0, 0x4(r4)

lfs f0, 0x8(r4)
lfs f1, RoomscaleVelocity_Delta + 0x8(r1)
fadds f0, f0, f1
stfs f0, 0x8(r4)

ApplyPlayerVelocityWithRoomscale_Apply:
lwz r6, 0x10(r1)
lwz r5, 0x0C(r1)
lwz r4, 0x08(r1)
lwz r31, 0x14(r1)
mr r3, r31
bl ApplyPlayerVelocityInternal

lwz r0, 0x104(r1)
addi r1, r1, 0x100
mtlr r0
lis r12, ContinueAfterApplyPlayerVelocityWithRoomscale@ha
addi r12, r12, ContinueAfterApplyPlayerVelocityWithRoomscale@l
mtctr r12
bctr

blr

; hooks entity transform and velocity updates
0x0348970C = ba HandleSetRigidBodyVelocityWithBreakpoint
0x03485F68 = ba import.coreinit.hook_RemoveRigidBodyFromWorld
0x03486CD4 = ba import.coreinit.hook_SetRigidBodyTransform
0x03486F4C = ba import.coreinit.hook_SetRigidBodyScale
0x0380E8F0 = ba import.coreinit.hook_RemoveRagdollControllerFromWorld
0x0380F1D0 = ba import.coreinit.hook_SetRagdollControllerTransform
0x0380F258 = ba import.coreinit.hook_SetRagdollControllerScale
0x03486D80 = ba import.coreinit.hook_SetRigidBodyPosition
0x03489A80 = ba import.coreinit.hook_SetRigidBodyPositionAndRotation
0x0348AB20 = ba import.coreinit.hook_SetRigidBodyRotation


; hooks arrow shooting parameters (and other dynamically loaded bools/values inside AI Actions)
0x030EC840 = ba import.coreinit.hook_LoadDynamicVec3
0x030EC8C0 = bla import.coreinit.hook_LoadDynamicBool

; temporary sanity check: intercept Player::m_84_setMtx just before Actor::m_84_setMtx
; IDA: 0x02D66B58 = mr r4, r29
0x02D66B58 = bla import.coreinit.hook_TestPlayerSetMtxTeleport

; inject roomscale motion before the game's final player velocity apply helper
;0x0344DCEC = bla ApplyPlayerVelocityWithRoomscale

0x0344AC6C = ApplyPlayerVelocityInternal:
0x03489710 = ContinueAfterSetRigidBodyVelocityWithBreakpoint:
0x0344DCF8 = ContinueAfterApplyPlayerVelocityWithRoomscale:
0x034C9C70 = RayCast_setLayers:
0x034C9E1C = RayCast_setFromAndDelta:
0x034C9F68 = RayCast_getHitPosition:
0x034CC1F8 = RayCastBodyQuery_ctor:
0x034CC2CC = RayCastBodyQuery_worldRayCast:
0x034CC278 = RayCastBodyQuery_dtor:
0x0344C0B8 = RigidBody_getPosition:

0x1011A7A0 = str_IsShootByPlayer:
0x1011A7F4 = str_TargetPos:
0x030EC860 = ksys__act__ai__InlineParamPack__addBool:
0x030EC7DC = ksys__act__ai__InlineParamPack__addVec3:

; swap TargetPos and IsShootByPlayer to be able to mutate the TargetPos value AFTER we know that the arrow is shot by the player

; .origin = 0x02705AFC
; 
; lis r6, str_IsShootByPlayer@ha
; addi r6, r6, str_IsShootByPlayer@l
; lwz r5, 0x0E8(r29)
; stw r27, 0x014(r1)
; stw r6, 0x010(r1)
; lwz r8, 0x4EC(r5)
; mtctr r8
; mr r3, r29
; bctrl
; mr r4, r3
; li r6, -1
; addi r5, r1, 0x010
; addi r3, r1, 0x158
; bl ksys__act__ai__InlineParamPack__addBool
; 
; lis r4, str_TargetPos@ha
; addi r4, r4, str_TargetPos@l
; li r6, -1
; addi r5, r1, 0x010
; stw r4, 0x010(r1)
; addi r4, r1, 0x88
; addi r3, r1, 0x158
; stw r27, 0x14(r1)
; bl ksys__act__ai__InlineParamPack__addVec3
; 
; .origin = codecave
