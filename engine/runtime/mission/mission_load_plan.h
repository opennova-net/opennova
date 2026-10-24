#pragma once

// The mission load plan: the real stage order the shell's mission pipeline
// runs and the exact loading-screen checkpoint each stage presents. Progress
// changes only when one of these operations begins; redraw cadence never
// manufactures intermediate values.
//
// [orig: Game_StartMission @0x524360 — the loading screen is re-presented
// with constant per-stage percentages 2,3,4,6,20,26,...,41,45,50,60,70,90,
// 95,100 (LoadingScreen_UpdateAndPresent calls @0x52498f..0x525d29); our
// pipeline has fewer stages than the original's ~30 call sites. OpenNova uses
// deterministic ten-percentage-point checkpoints for actual operations and
// reserves 90 for local-world readiness and 100 for the mission start's last
// present (docs/interface/loading-screen-re.md D-LOADSCR-1)]. Order witnesses
// the stages carry:
//  - the shared .3DI definition cache is destroyed before the environment
//    and terrain loads (celestial resolves its models from the environment,
//    terrain-loaded foliage must stay present-but-excluded in the same
//    generation) [orig: EffectWorld_DestroyAllAndInitDeviceCaps @0x524A6F];
//  - the object stage pulses its constant value from inside the per-model
//    load loops [orig: the paired constant-value LoadingScreen_UpdateAndPresent
//    calls @0x524d9c/0x524e09, 0x524f32/0x524fe0];
//  - the non-foliage loaded-.3DI page freezes once, after the entity,
//    celestial, HUD and renderer resource loads and before the loading screen
//    drops — late network spawns must not change it
//    [orig: CEffectWorld_ResolveMaterialsAndRebuildChallengeList @0x5871CF from @0x525A6E];
//    its material resolve presents the loading screen at the stored checkpoint past
//    each listed model's resolve, the stage's value unchanged (the shell's
//    model_materials leg presents past each listed file's, D-NET-NEW-L3-37)
//    [orig: CEffectWorld_ResolveMaterialsAndRebuildChallengeList @0x5B3A80 - the
//    resolve call @0x5B3A92, LoadingScreen_PresentStoredProgress @0x586D70 called
//    @0x5B3A9C (slot 15 of the screen's vtable with the checkpoint it stored)];
//  - a present draws the loading screen alone: no world pass runs in it
//    (hud/loading_screen.h carries the witness), so the shell's present draws
//    its viewport's canvas with the 3D world off (D-NET-NEW-L3-37);
//  - the minimap's depthspin water texture is made inside the terrain's
//    mission textures (its texture slots and atlas, on both arms), ahead of the
//    freeze, not at the load's end (the shell's terrain_textures leg builds the
//    mask, D-NET-NEW-L3-37) [orig: PolyTrn_InitTextures @0x60AAA0 - the
//    depthspin texels @0x60BA20..0x60BB11, the texture made @0x60BB3B];
//  - the game music context opens and its vars seed before runtime compiles
//    WAC M# operands [orig: @0x525581..0x52561b before WAC init @0x525CB3];
//  - the effect system loads every .ptl and its textures at load, so the
//    first live spawn pays nothing [orig: CEffectSystem_Init @0x5f6070 from
//    @0x524980]; the placed pools' glow lights spawn after it
//    [orig: Game_SpawnAllEntityGlowEffects @0x5227b0 from @0x525d19];
//  - the blink letter bits clear at mission start [orig: @0x525c45];
//  - the HUD's overlay system inits on every mission start, every role (a
//    dedicated host's included, whether or not a player of its own will ever
//    draw it), past the rows' init and ahead of a joiner's first 0x2F: its
//    layout file and its map-grid origin scan. The shell's HUD builds there,
//    inside the load, not on the first frame past it (GameWorld's
//    hud_overlay_init leg, D-NET-NEW-L8-40)
//    [orig: Game_StartMission @0x524360 - Entity_InitAllFromModels @0x52567F,
//    HUD_InitOverlaySystem @0x525733 (no branch around it), the first 0x2F
//    @0x525836; HUD_InitOverlaySystem @0x5A4620 (hudpos @0x5A4931, the
//    type-2043 pool scan @0x5A4999)];
//  - the HUD's textures and their render materials load at the mission
//    textures stage past the freeze on the full arm, with the tip panel's art
//    and the view effects' after them; a dedicated host's reduced arm loads none
//    of them. Both arms load the connection icons ahead of the split, and after
//    it the HUD's fonts and the stdbox frame (its border, its camo tile and its
//    material) the status page draws (GameWorld's hud_textures_load leg and its
//    arm, D-NET-NEW-L8-40). Its device materials are made there, so the first
//    frame that draws the HUD makes none: the shell's HUD draws in the load's one
//    forced frame, off its mount, so its canvas pipelines compile in the load
//    (GameWorld's hud_pipelines leg, D-NET-NEW-LF-16)
//    [orig: Game_StartMission @0x524360 - the reduced flag @0x524958..0x52495C
//    (is_in_session, is_authority, !is_mp_session_peer), the call @0x525A6E,
//    HUD_InitAllFonts @0x525A9A, BoxTexture_LoadAndSetupUVRegions ("stdbox")
//    @0x525AC0; Render_InitMissionTextures @0x587120 - CNetworkIcons_LoadTextures
//    @0x587129 (both arms), the arm test @0x58713C, the freeze @0x5871CF,
//    HUD_LoadAllTextures @0x587205 (its materials, GfxShader_CreateForTexture
//    called @0x59E024 / @0x59E07E / @0x59E094 / @0x59E199 / @0x59E21F),
//    ViewFx_InitShadersAndTextures @0x587217, CTipSystem_Init @0x587270, the
//    reduced arm @0x58727A: its freeze @0x58728C and return];
//  - every pool's models (the weapon overlays with them) reload and every
//    pool's rows initialize from them ahead of the 0x0B, so the first frames
//    past the load build no model. The shell's one forced frame draws the
//    skinned models its first revealed frame would otherwise build (the local
//    body, its view model, one model per identity the wire walk cold-spawns),
//    freed past it, so their meshes, rigs and pipelines are made in the load
//    (GameWorld's person_pipelines leg, D-NET-NEW-LF-15); a joiner's L is born at
//    the push past the load, so its body and view model are its learned self
//    record's there
//    [orig: Game_StartMission @0x524360 - Game_ReloadEntityModelsAndCallbacks
//    called @0x52550E, Entity_InitAllFromModels called @0x52567F, the 0x0B's
//    queue called @0x52624B].

namespace opennova::mission {

enum class MissionLoadStage : int {
	kMissionSetup = 0, // root resolution, BMS parse, references and caches
	kEnvironment,     // the .env load, the mission overrides, the clock
	kTerrain,         // the .trn / tile-info build
	kObjects,         // object placement (the per-model loops)
	kRuntime,         // game music context, runtime start, loaded-model page freeze
	kAudio,           // mission sound-bank loading and ambient audio
	kEffects,         // the effect world
	kEffectsWarm,     // effect textures and first-draw pipeline warm-up
	kFinish,          // water/minimap present enable, before ready
	kCount,
};

struct MissionLoadStep {
	MissionLoadStage stage;
	int progress_percent; // presented when the stage STARTS
	const char *name;
};

inline constexpr MissionLoadStep kMissionLoadPlan[] = {
	{ MissionLoadStage::kMissionSetup, 0, "mission_setup" },
	{ MissionLoadStage::kEnvironment, 10, "environment" },
	{ MissionLoadStage::kTerrain, 20, "terrain" },
	{ MissionLoadStage::kObjects, 30, "objects" },
	{ MissionLoadStage::kRuntime, 40, "runtime" },
	{ MissionLoadStage::kAudio, 50, "audio" },
	{ MissionLoadStage::kEffects, 60, "effects" },
	{ MissionLoadStage::kEffectsWarm, 70, "effects_warm" },
	{ MissionLoadStage::kFinish, 80, "finish" },
};
inline constexpr int kMissionLoadStageCount =
		static_cast<int>(sizeof(kMissionLoadPlan) / sizeof(kMissionLoadPlan[0]));
static_assert(kMissionLoadStageCount == static_cast<int>(MissionLoadStage::kCount),
		"every load stage has one plan row");

// Local construction stops at world-ready. 100 is the mission start's last
// present: a joiner's mission start presents it past its second 0x2F's queue and
// ahead of its 0x0B's and the final wait, so the joiner shows 100 while it awaits
// its admission (R-L2-s38-1, D-NET-NEW-L8-41); a single player's or a host's shell
// presents it at the reveal, inside the load's own call
// [orig: Game_StartMission @0x524360 - the second 0x2F @0x525C2E, `push 64h`
// @0x525D22, LoadingScreen_UpdateAndPresent @0x525D29, LoadingScreen_ReleaseEffect
// @0x525D52, the 0x0B @0x52624B, the final wait @0x526295].
inline constexpr int kMissionLoadProgressWorldReady = 90;
inline constexpr int kMissionLoadProgressComplete = 100;

// The percentage a stage presents when it starts; out-of-range stages report
// the completion value so a caller can never present a regression.
inline int mission_load_progress_percent(MissionLoadStage stage) {
	const int index = static_cast<int>(stage);
	if (index < 0 || index >= kMissionLoadStageCount) return kMissionLoadProgressComplete;
	return kMissionLoadPlan[index].progress_percent;
}

} // namespace opennova::mission
