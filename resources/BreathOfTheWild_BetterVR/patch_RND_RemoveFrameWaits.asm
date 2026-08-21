[BetterVR_RND_RemoveFrameWaits_V208]
moduleMatches = 0x6267BFD0

; Let OpenXR own frame pacing. The emulated VSync events themselves are left alone, as is FPS++'s
; cutscene FPS limiter, which is there to stop a few cutscenes crashing above 30/60 FPS.
; The commented-out waitForVsync in custom_sead_GameFramework_procFrame is unreachable code.
0x0309D018 = blr ; sead::GameFrameworkCafe::waitForNextFrame_
0x031FACC4 = blr ; sead::GameFrameworkCafe::waitForVsync

; GX2DrawDone stays intact, Cemu uses it as a command-stream synchronization marker.
0x031FAB00 = nop ; GX2SetGPUFence in sead::GameFrameworkCafe::presentAndDrawDone
