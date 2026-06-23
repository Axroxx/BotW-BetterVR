[BetterVR_DisableGraphicalFeatures_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

; ============================================================================
; Quality Preset from the Graphics graphic pack controls which graphical
; features are enabled. This patch gates the god-rays / volume-mask calls in
; gsys::ModelScene::drawProcedural (0x039AB208), r4==2 pass:
;   0x039AB3B8  bl gsys::ModelSceneFx::drawVolumeMask
;   0x039AB3CC  bl gsys::ModelScene::SyncWithGPUMaybe
; At Low (0) or Medium (1) quality, these two calls are skipped (god-rays off).
; At High (2) they run normally.
; The $qualityPreset value is written to 0x10416BF8 by the Graphics pack.
; Shadows are disabled at Low (0) quality, enabled at Medium (1) and High (2).
; ============================================================================

0x039CFA10 = gsys_ModelSceneFx_drawVolumeMask:
0x039A9D28 = gsys_ModelScene_SyncWithGPUMaybe:
0x10416BF8 = g_qualityPreset:
0x039DDFC8 = continue_isDepthShadowEnabled:

; --------------------------------------------------------------------------------
; 0x039AB3B8: bl drawVolumeMask(r3, r4, r5, r6, r7)
stub_skipGodraysVolumeMask:
lis r12, g_qualityPreset@ha
lwz r12, g_qualityPreset@l(r12)
cmpwi r12, 2
blt skipVolumeMask_exit ; Low/Medium -> skip the call

mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
lis r12, gsys_ModelSceneFx_drawVolumeMask@ha
addi r12, r12, gsys_ModelSceneFx_drawVolumeMask@l
mtctr r12
bctrl
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0

skipVolumeMask_exit:
blr

0x039AB3B8 = bla stub_skipGodraysVolumeMask

; --------------------------------------------------------------------------------
; 0x039AB3CC: bl SyncWithGPUMaybe(r3, r4, r5, r6)
stub_skipGodraysSyncGpu:
lis r12, g_qualityPreset@ha
lwz r12, g_qualityPreset@l(r12)
cmpwi r12, 2
blt skipSyncGpu_exit ; Low/Medium -> skip the call

mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
lis r12, gsys_ModelScene_SyncWithGPUMaybe@ha
addi r12, r12, gsys_ModelScene_SyncWithGPUMaybe@l
mtctr r12
bctrl
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0

skipSyncGpu_exit:
blr

0x039AB3CC = bla stub_skipGodraysSyncGpu

; --------------------------------------------------------------------------------
; 0x039DDFC4: entry of gsys::ModelSceneShadow::isDepthShadowEnabled
; At Low quality (0), return false to disable depth shadows.
stub_skipDepthShadow:
li r11, $qualityPreset
cmpwi r11, 0
bne run_depth_shadow_check

li r3, 0
blr

run_depth_shadow_check:
lwz r12, 4(r3)
lis r11, continue_isDepthShadowEnabled@ha
addi r11, r11, continue_isDepthShadowEnabled@l
mtctr r11
bctr

0x039DDFC4 = ba stub_skipDepthShadow
