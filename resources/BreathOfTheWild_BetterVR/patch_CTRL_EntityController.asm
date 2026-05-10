[BetterVR_EntityController_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

RoomscaleScratch = 0x20
RoomscaleCurrent = RoomscaleScratch + 0x00
RoomscaleCastFrom = RoomscaleScratch + 0x0C
RoomscaleCastDelta = RoomscaleScratch + 0x18
RoomscaleHit = RoomscaleScratch + 0x24
RoomscaleGroundHit = RoomscaleScratch + 0x30
RoomscaleQueryType = RoomscaleScratch + 0x34
RoomscaleSweepRadius = RoomscaleScratch + 0x38
RoomscaleOwnLayer = 0x60
RoomscaleSavedLR = 0x70
RoomscaleQuery = 0x80
RoomscaleShapeVtable = RoomscaleQuery + 0x3C

ApplyRoomscaleAfterPlayerVelocity:
stwu r1, -0x130(r1)
mflr r0
stw r0, 0x134(r1)
stw r30, 0x14(r1)
stw r31, 0x18(r1)

mr r3, r31
addi r4, r1, RoomscaleScratch
bla import.coreinit.hook_BeginRoomscaleMovement
lwz r31, 0x18(r1)

cmpwi r3, 0
beq ApplyRoomscaleAfterPlayerVelocity_Continue

mr r3, r31
addi r4, r1, RoomscaleCurrent
bl RigidBody_getPosition

ApplyRoomscaleAfterPlayerVelocity_Loop:
addi r3, r1, RoomscaleScratch
bla import.coreinit.hook_PrepareRoomscaleRaycast
lwz r31, 0x18(r1)
cmpwi r3, 2
beq ApplyRoomscaleAfterPlayerVelocity_Warp
cmpwi r3, 1
bne ApplyRoomscaleAfterPlayerVelocity_Continue

lwz r0, RoomscaleQueryType(r1)
cmpwi r0, 1
beq ApplyRoomscaleAfterPlayerVelocity_GroundProbe

li r0, 3
stw r0, RoomscaleOwnLayer(r1)
li r6, 0
li r7, 0x80
li r8, 0
stw r8, 0x08(r1)
lis r9, nullPosition@ha
addi r9, r9, nullPosition@l
lis r10, emptyString@ha
addi r10, r10, emptyString@l
addi r3, r1, RoomscaleQuery
addi r4, r1, RoomscaleOwnLayer
addi r5, r1, RoomscaleGroundHit
lfs f1, RoomscaleSweepRadius(r1)
bl RoomscaleSphereCast_ctor

lwz r12, RoomscaleQuery + 0x04(r1)
lwz r0, 0x0C(r12)
ori r0, r0, 0x0005
stw r0, 0x0C(r12)
lwz r0, 0x14(r12)
li r6, 0x0005
andc r0, r0, r6
stw r0, 0x14(r12)

addi r3, r1, RoomscaleQuery
addi r4, r1, RoomscaleCastFrom
addi r5, r1, RoomscaleHit
bl ShapeCast_setStartAndEnd

lwz r12, RoomscaleShapeVtable(r1)
lwz r0, 0x1C(r12)
mtctr r0
li r4, 1
addi r3, r1, RoomscaleQuery
bctrl
mr r30, r3

addi r3, r1, RoomscaleQuery
li r4, 2
bl RoomscaleSphereCast_dtor
b ApplyRoomscaleAfterPlayerVelocity_Notify

ApplyRoomscaleAfterPlayerVelocity_GroundProbe:

addi r3, r1, RoomscaleQuery
li r4, 0
addi r5, r1, RoomscaleGroundHit
bl RayCastBodyQuery_ctor

addi r3, r1, RoomscaleQuery
addi r4, r31, 0x208
bl RayCast_setLayers

addi r3, r1, RoomscaleQuery
addi r4, r1, RoomscaleCastFrom
addi r5, r1, RoomscaleCastDelta
bl RayCast_setFromAndDelta

addi r3, r1, RoomscaleQuery
li r4, 0
bl RayCastBodyQuery_worldRayCast
mr r30, r3
cmpwi r30, 0
beq ApplyRoomscaleAfterPlayerVelocity_Consume

addi r3, r1, RoomscaleQuery
addi r4, r1, RoomscaleHit
bl RayCast_getHitPosition

ApplyRoomscaleAfterPlayerVelocity_Consume:
addi r3, r1, RoomscaleQuery
li r4, 2
bl RayCastBodyQuery_dtor

ApplyRoomscaleAfterPlayerVelocity_Notify:
addi r3, r1, RoomscaleScratch
mr r4, r30
bla import.coreinit.hook_ConsumeRoomscaleRaycast
lwz r31, 0x18(r1)
cmpwi r3, 2
beq ApplyRoomscaleAfterPlayerVelocity_Warp
cmpwi r3, 1
beq ApplyRoomscaleAfterPlayerVelocity_Loop
b ApplyRoomscaleAfterPlayerVelocity_Continue

ApplyRoomscaleAfterPlayerVelocity_Warp:
addi r3, r1, RoomscaleCurrent
addi r4, r1, RoomscaleScratch
bla import.coreinit.hook_BuildRoomscaleWarpTransform
lwz r31, 0x18(r1)

lwz r11, 0xE8(r31)
extrwi. r0, r11, 1, 15
beq ApplyRoomscaleAfterPlayerVelocity_WarpMainBody

lwz r3, 0x230(r31)
addi r4, r1, RoomscaleScratch
li r5, 1
mflr r12
stw r12, RoomscaleSavedLR(r1)
lis r0, ReturnAfterOriginalSetRigidBodyTransformSecondary@ha
ori r0, r0, ReturnAfterOriginalSetRigidBodyTransformSecondary@l
lis r12, OriginalSetRigidBodyTransform@ha
addi r12, r12, OriginalSetRigidBodyTransform@l
mtctr r12
bctr

ReturnAfterOriginalSetRigidBodyTransformSecondary:
lwz r12, RoomscaleSavedLR(r1)
mtlr r12
b ApplyRoomscaleAfterPlayerVelocity_Continue

ApplyRoomscaleAfterPlayerVelocity_WarpMainBody:
lwz r3, 0x4(r31)
addi r4, r1, RoomscaleScratch
li r5, 1
mflr r12
stw r12, RoomscaleSavedLR(r1)
lis r0, ReturnAfterOriginalSetRigidBodyTransformMain@ha
ori r0, r0, ReturnAfterOriginalSetRigidBodyTransformMain@l
lis r12, OriginalSetRigidBodyTransform@ha
addi r12, r12, OriginalSetRigidBodyTransform@l
mtctr r12
bctr

ReturnAfterOriginalSetRigidBodyTransformMain:
lwz r12, RoomscaleSavedLR(r1)
mtlr r12
mr r3, r31
bl SyncPhysicsFieldAfterSetPosition

ApplyRoomscaleAfterPlayerVelocity_Continue:
lwz r31, 0x18(r1)
lwz r30, 0x14(r1)
lwz r0, 0x134(r1)
addi r1, r1, 0x130
mtlr r0
lwz r3, 0x4(r31)
lis r12, ContinueAfterApplyRoomscaleAfterPlayerVelocity@ha
addi r12, r12, ContinueAfterApplyRoomscaleAfterPlayerVelocity@l
mtctr r12
bctr

0x0344DCF8 = bla ApplyRoomscaleAfterPlayerVelocity

0x03489710 = ContinueAfterLogSetRigidBodyVelocity:
0x0344DCFC = ContinueAfterApplyRoomscaleAfterPlayerVelocity:
0x0344B864 = SyncPhysicsFieldAfterSetPosition:
0x0344C0B8 = RigidBody_getPosition:
0x03486D84 = OriginalSetRigidBodyPosition:
0x03486CD8 = OriginalSetRigidBodyTransform:
0x034D0170 = ShapeCast_setStartAndEnd:
0x034C9C70 = RayCast_setLayers:
0x034C9E1C = RayCast_setFromAndDelta:
0x034C9F68 = RayCast_getHitPosition:
0x034CC1F8 = RayCastBodyQuery_ctor:
0x034CC2CC = RayCastBodyQuery_worldRayCast:
0x034CC278 = RayCastBodyQuery_dtor:
0x034D1B60 = RoomscaleSphereCast_ctor:
0x034D1DD0 = RoomscaleSphereCast_dtor:
0x10549DD0 = nullPosition:
0x10549FEC = emptyString:
