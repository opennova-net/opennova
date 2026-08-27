#pragma once

// The mission load plan: the stage order Game_StartMission runs and the
// loading-screen percentage each stage presents. The shell's mission load
// walks this plan (its stages are device legs — environment node, terrain
// build, object placement, runtime start, audio, effect world); the plan
// itself, the anchor values and the order witnesses live here so the
// sequence is the engine's and ctest-pinned.
//
// [orig: Game_StartMission @0x524360 — the loading screen is re-presented
// with constant per-stage percentages 2,3,4,6,20,26,...,41,45,50,60,70,90,
// 95,100 (LoadingScreen_UpdateAndPresent calls @0x52498f..0x525d29); our
// pipeline has fewer stages than the original's ~30 call sites, so each stage
// boundary reports the nearest witnessed value
// (docs/interface/loading-screen-re.md D-LOADSCR-1)]. Order witnesses the
// stages carry:
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
//    [orig: CEffectWorld_RebuildAllModelBuffers @0x5871CF from @0x525A6E];
//  - the game music context opens and its vars seed at the audio stage
//    [orig: @0x525581-0x52561b];
//  - the effect system loads every .ptl and its textures at load, so the
//    first live spawn pays nothing [orig: CEffectSystem_Init @0x5f6070 from
//    @0x524980]; the placed pools' glow lights spawn after it
//    [orig: Game_SpawnAllEntityGlowEffects @0x5227b0 from @0x525d19];
//  - the blink letter bits clear at mission start [orig: @0x525c45].

namespace opennova::mission {

enum class MissionLoadStage : int {
	kEnvironment = 0, // the .env load, the mission overrides, the clock
	kTerrain,         // the .trn / tile-info build
	kObjects,         // object placement (the per-model loops)
	kRuntime,         // runtime start + the loaded-model page freeze
	kAudio,           // mission audio + the game music context
	kEffects,         // the effect world + its catalog warm
	kFinish,          // water/minimap present enable, before ready
	kCount,
};

struct MissionLoadStep {
	MissionLoadStage stage;
	int progress_percent; // presented when the stage STARTS
	const char *name;
};

inline constexpr MissionLoadStep kMissionLoadPlan[] = {
	{ MissionLoadStage::kEnvironment, 2, "environment" },
	{ MissionLoadStage::kTerrain, 6, "terrain" },
	{ MissionLoadStage::kObjects, 26, "objects" },
	{ MissionLoadStage::kRuntime, 41, "runtime" },
	{ MissionLoadStage::kAudio, 70, "audio" },
	{ MissionLoadStage::kEffects, 90, "effects" },
	{ MissionLoadStage::kFinish, 95, "finish" },
};
inline constexpr int kMissionLoadStageCount =
		static_cast<int>(sizeof(kMissionLoadPlan) / sizeof(kMissionLoadPlan[0]));
static_assert(kMissionLoadStageCount == static_cast<int>(MissionLoadStage::kCount),
		"every load stage has one plan row");

// The value the finished load presents.
inline constexpr int kMissionLoadProgressComplete = 100;

// The percentage a stage presents when it starts; out-of-range stages report
// the completion value so a caller can never present a regression.
inline int mission_load_progress_percent(MissionLoadStage stage) {
	const int index = static_cast<int>(stage);
	if (index < 0 || index >= kMissionLoadStageCount) return kMissionLoadProgressComplete;
	return kMissionLoadPlan[index].progress_percent;
}

} // namespace opennova::mission
