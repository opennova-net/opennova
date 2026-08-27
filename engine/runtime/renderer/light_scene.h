// The EffectWorld dynamic light/glow instance pool — the runtime system behind
// D-RLIT-4. Retail keeps one process-global table of 176-byte instances
// [orig: Light_InstanceTable @ 0x2732e28, capacity 4096, high-water count
// @ 0x2732de4; spawner LightPool_SpawnGlowEffect @ 0x5a8d50 returns
// slot | 0x8000]. Placed-model lights enter it at mission start — a walk over
// entity pools 1..2 spawning one instance per model light record
// [orig: Game_StartMission @ 0x525d19 -> Game_SpawnAllEntityGlowEffects @0x5227b0 -> Entity_SpawnGlowEffects
// @ 0x56c7c0: world position via the entity matrix, radius = atten_end * 65536
// (flt_7C32BC), packed color -1 (white), lifetime 1, fade -1, the record's
// RGB-gen block attached, subobject/blink-box attach, and the three authored
// disable flags folded to render flags 512/1024/2048 @ 0x56c8e7..0x56c91d].
// The four witnessed transient spawners routed here are ammo impact
// @ 0x40a2b3, death pieces @ 0x49351a, round spawn @ 0x4ec8da, and muzzle
// glow @ 0x56c987. The only late model-light call is powerup_respawn
// @ 0x442b40/@0x442ba0, registered @ 0x442ce6; the wire-node spawn router
// carries that replacement path.
//
// Per draw context retail queries the pool by AABB and takes the NEAREST
// instances: overlap test + center-distance sort, at most 64 handles
// [orig: collect_nearby_zones_by_aabb @ 0x5aa250 — distance metric
// sum(((d*d + 0x8000) >> 16)) per axis, bubble sort, skip flag bit 2].
// THE DRAW IS ONE ENTITY, THE QUERY BOX IS THAT ENTITY'S OWN BOUND: both
// sector walks build min/max = position -/+ boundRadius (entity+0, per axis)
// and hand it to the live select [orig: setup_terrain_effect_for_entity
// @ 0x5c74fb..0x5c753a; Terrain_RenderSectorModels @ 0x5c5ea6..0x5c5ee4 ->
// @ 0x5c5f12 / @ 0x5c602e]. Light_SelectAndEnableForDraw @ 0x5ab9d0 runs the
// same overlap + nearest sort into Light_VisibleHandles/Light_VisibleCount
// and D3D-LightEnables the first <= 4 of them (the > 4 clamp @ 0x5abbeb;
// update_light_slots @ 0x5abc50 is its xref-less twin) — that enable set is
// the fixed-function fallback's only consumer and is torn down again per
// batch entry (below). The lights a drawn strip actually receives come from
// the BATCH ENTRY: each collector re-walks the sorted visible list in order,
// gates Light_PassesActiveGroups @ 0x5a9120 [owner entity at record dword
// 19, section at dword 20; an owned light passes only for the active
// interior/owner group — called from collect_render_objects_for_batch
// @ 0x5d91f8, collect_render_batches_for_entity @ 0x5d96b8,
// render_terrain_sector_batch @ 0x60969f and RenderSlot_UpdateEntityLight
// @ 0x5d6b89] plus the objects-enable flag (render flag 0x800 clear
// @ 0x5a9010, called @ 0x5d920c / @ 0x5d96cc), and stores AT MOST THREE
// handles in entry dwords 5..7 with the count in dword 8 [orig: `cmp esi, 3;
// jge` @ 0x5d9226..0x5d9229 in collect_render_objects_for_batch @ 0x5d8f20;
// the twin @ 0x5d96e6..0x5d96e9 in collect_render_batches_for_entity
// @ 0x5d94b0]. CRenderBatchQueue_FlushBatches @ 0x5d9f50 walks exactly those
// three slots (`X[2] = 3` @ 0x5da26b), pushes the survivors as
// PointLightCoordArray/ColorArray/AttenArray with CurNumPointLights = the
// entry count for the shader pass [orig: Light_GetPointLightParams
// @ 0x5da6a8; the ID3DXEffect count-setter vtable call @ 0x5da6ed and the
// three vector-array vtable calls @ 0x5da71a/@ 0x5da740/@ 0x5da766 through
// the handles stored @ 0x5af51b..0x5af566], and for the fixed-function pass first
// disables EVERY enabled D3D light (CEffectWorld_ClearActiveSamplerStates
// @ 0x5da5de) and re-enables only the entry's (Light_ApplyAsD3DLight
// @ 0x5da61a). The highest-quality object path therefore lights with at
// most three dynamic lights per strip; the 4-light D3D enable never reaches
// a shader-lit strip. The per-ROBJ gate re-uses the ENTITY query — retail
// never re-collects per ROBJ (Lighting_SetOwnerLightGroup(0, robjIndex)
// @ 0x5d8ff7 only moves the owner-group section between the two walks). The
// per-light parameters are
// [orig: Light_GetPointLightParams @ 0x5a9180]: color = record RGB (bytes
// * 1/256 at spawn) x EffectWorld_AmbientScale x intensity, then the optional
// RGB-gen multiply; attenuation {1, 0, 15/range^2, 1} with range =
// radius_fixed * 1.25 / 65536; the D3D-light fill adds a 1.5x diffuse boost
// [orig: Light_FillD3DPointLight @ 0x5aa450].
//
// FOLIAGE IS NOT A DELIVERY TARGET ON THE LOCKED HIGHEST-QUALITY PATH. The
// far-patch loop does call Light_SelectAndEnableForDraw @ 0x60a5dc, but
// Foliage_LoadDefAssets first creates Foliage_WindSwayVS @ 0x601278 and
// Foliage_SetupFarSlotDraw installs it in the descriptor @ 0x60087a..0x600883.
// The complete vs_1_1 literal @ 0x7de648 declares position/color/texcoord only,
// never normal/light input, and writes oD0 = c6; Foliage_LightmapBlendPS then
// uses that oD0 plus cached-tile c0/c1. SetLight/LightEnable can affect foliage
// only when VS creation failed and the FVF fixed-function fallback runs. That
// fallback is excluded by the highest-quality-retail-path capture contract.
//
// The flicker: reading an instance with a gen block first runs
// Light_TickGenBlock, which hashes the light's fixed position into the
// weather wave ring and WRITES THE GLOBAL FLICKER CONTROL REGISTER —
// 0x83FD00 = ctrl value slot 0x83FCE8 + 8 * ordinal 3 (THREEDI_CTRL_FLICKER)
// — so each light flickers with a position-phased sample of the shared ring
// [orig: Light_TickGenBlock @ 0x5a8ae0: index = (z>>15) + (y>>14) + (x>>14)
// + Env_WaveRingIndex, value = Env_WaveAmpRing[index & 0xFF]]. The RGB-gen
// styles then evaluate exactly like model lights (styles 113/114 read the
// ctrl value) via renderer::eval_light_runtime.
//
// Lifecycle: record dword 15 is a FADE MODE, dwords 16/17 the countdown
// (current/initial), float 14 the blend the params multiply. The per-frame
// tick [orig: EffectWorld_TickInstancesAndLightScale @ 0x5aa170, called from
// Game_ProcessMainFrame @ 0x5267a1 — the 62 Hz main loop] decrements a
// positive counter; when it expires, mode 5 hides the slot (flag bit 2, slot
// kept — SetBlendAmount >= 0.001 un-hides it) and every other mode zeroes the
// slot dead; while counting, modes 2 and 5 render blend = current/initial (a
// linear fade-out). Mode 1 with duration -1 is the permanent model-light
// shape. The witnessed transient spawners route through this pool:
//  - muzzle glow [orig: Entity_UpdateMuzzleGlowEffect @ 0x56c960, from
//    WeaponSlot_FireAndSpawnEffects @ 0x53f597 (AI/authority fire, at the
//    fire position) and ActionSlot_SpawnEffect @ 0x402080 (the action-row
//    FIRE arm, at the action-transform muzzle point), both gated on the ammo
//    `MF_Light` flag (+36)]: the handle at entity+436 is shared with model
//    LGHT (Entity_SpawnGlowEffects overwrites it for every record
//    @ 0x56c925..0x56c92c). A nonzero final model-light lease is re-armed and
//    moved directly; only a zero word spawns radius 1.5/color 0xFFE0A0. Every
//    shot sets mode 4/duration 5, owner=shooter/section 0, position and blend
//    1.0. Five ticks later the slot dies while the cached word stays nonzero.
//  - impact flash [orig: AmmoDef_ProcessImpactEffect @ 0x40a2b3]: ammo
//    `light_impact` radius/color/ticks, spawned radius/2 above the impact,
//    mode 2, gated on the impact-effect leg actually presenting; also sets
//    render flag 0x100 — read by the corona walk, which re-centers the
//    billboards radius/2 below the light (carried as
//    corona_lower_half_radius; witnessed 2026-08-20 @ 0x5ab037..0x5ab05c).
//  - death flash [orig: Entity_SpawnDeathPieces @ 0x49351a]: husk deaths
//    above water, non-decorations — 2x the piece model's bound radius, color
//    0xFFC080, mode 2, duration 31, corona disabled (flag 512).
//  - round glow [orig: RoundData_SpawnRound @ 0x4ec8da]: ammo `light_move`
//    radius/color, mode 1 / duration -1, terrain disabled (flag 1024), handle
//    at round+0x1B4, follows the round per tick and clears on release.
// Model LGHT itself never follows. The spawner transforms its point with the
// entity matrix once @ 0x56c82d..0x56c85d; `subobject` is first read later by
// the owner-group branch @ 0x56c89a. SetPositionAndBounds @ 0x5a9070 has only
// projectile and muzzle callers. No death/husk path calls the model spawner;
// Entity_Destroy clears exactly the final entity+0x1B4 handle once
// @ 0x43e903..0x43e916, leaving earlier authored instances for mission reset.
// CORONAS (witnessed 2026-08-20): every alive, un-hidden instance without
// render flag 512 draws additive camera-facing billboards
// [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40 (ex
// CEffect_RenderFoliageBillboards — misnamed; renamed in the IDB), called
// once per world scene @ 0x5c96ad and once per mirror scene @ 0x5c85fd]:
// THREE segments marching toward the camera (step 0.1 x radius along
// normalize(cam - light)), half-sizes 0.5 x radius shrinking x0.66 per
// segment, color = record rgb x blend x EffectWorld_AmbientScale x 1/16
// then the RgbGen multiply, per-segment alpha = clamp(camera-plane depth /
// (0.5 x radius), 0..1) with <= 0 skipped, admission = camera distance
// <= 100 wu (0x640000 fixed) and a +-512-fixed x/y jitter phased on
// frame & 3; texture = the procedural 128x128 "texlightcrn" radial
// (intensity = 255 x (0.4 - 0.45 x d), d = sqrt(((x-64)/64)^2 +
// ((y-64)/64)^2), border texels 0) via Light_CoronaShader @ 0x2732db8
// [orig: Lighting_InitTextures @ 0x5a94f0], fog+blend mode 2 (additive
// with FOGCOLOR forced black @ 0x677740 case 2 — fog fades coronas OUT,
// never toward the fog color). Owned lights additionally gate on the owner
// building's visible-section bits [orig: Terrain_IsBuildingSectionBitSet
// @ 0x5c6960 over g_BuildingSectionVisMask @ 0x297f250 — the
// OcclusionWorld::section_mask domain; a non-pool-2 owner passes
// unconditionally @ 0x5c6978], and a render-flag-0x100 instance (the
// impact flash, spawned radius/2 above the impact) re-centers its corona
// by dropping radius/2 — the flag's only witnessed reader
// [orig: @ 0x5ab037..0x5ab05c]. All three legs are ported in
// collect_corona_quads.
//
// SPOT/TARGET DELIVERY IS DEAD CODE IN JO (witnessed 2026-08-20): the only
// spawner that marks an instance as a spot projector (flag 0x10000, the
// projection matrix at bytes 88..152, direction floats 38..40, near/far
// 41/42) is caller-less [orig: LightPool_SpawnSpotProjectorEffect
// @ 0x5a9fd0 — zero xrefs and zero data refs in Jointops.exe].
// Entity_SpawnGlowEffects passes only position + radius, so a model LGHT
// record's falloff byte, rotation, and view_proj never reach the runtime —
// every model light renders as an omni point light, and the spotlight
// projected-texture legs in CRenderBatchQueue_FlushBatches
// (Light_IsSpotlight @ 0x5a9040 -> get_light_projection_info @ 0x5aa5c0)
// are unreachable. LightSpawnParams therefore carries no spot fields.
//
// Intentional safety divergence: retail's setters write through stale handles
// into reused slots; OpenNova's generation lease rejects those writes.
//
// The ambient scale the select multiplies (EffectWorld_AmbientScale{R,G,B}
// @ 0x840b24..0x840b2c) is the fog/ambient modulator's packed colour x 1/64
// [orig: EffectWorld_UnpackModulatorToAmbientScale @ 0x5aaf1d..0x5aaf37, sole
// caller Environment_ApplyFogAndAmbient @ 0x57e464] — the env light-state
// gain the presenter feeds. The same per-frame tick that decays the pool
// ALSO unpacks Env_TerrainColorRecip bytes x 1/128 into flt_2732DA{C,8,4}
// (@ 0x5aa21d..0x5aa23f), but that triple is a SEPARATE factor consumed only
// by the terrain projected pass (light_terrain_pass.h
// terrain_per_channel_factor; env::terrain_color_recip_packed is the producer)
// — an earlier note here had conflated the two.
//
// Reimpl shape: positions stay in mission space (the retail Y-negation is the
// world->D3D fold the presenter replaces); the gen block is stored by value
// (retail stores a pointer into the loaded model). D-RLIT-4 is closed by the
// live/static object, terrain, corona, lifecycle, and max-quality dead-foliage
// proofs above.
#pragma once

#include <runtime/renderer/light_runtime.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {
struct TerrainLightPatchBounds;
struct TerrainLightPassInputs;
struct TerrainLightPatchRows;
} // namespace opennova::renderer

namespace renderer {

struct LightGenBlock {
	uint8_t style = 0;  // 0 = static color (no gen multiply)
	uint8_t phase = 0;
	uint16_t rate = 0;
	std::array<uint8_t, 4> color_start{};
	std::array<uint8_t, 4> color_end{};
};

struct LightSpawnParams {
	// Mission-space 16.16 world position.
	std::array<int32_t, 3> position_fixed{};
	// atten_end * 65536 [orig: Entity_SpawnGlowEffects @ 0x56c876].
	int32_t radius_fixed = 0;
	// packedColor bytes / 256 at spawn [orig: @ 0x5a8e51]; -1 spawns white.
	std::array<uint8_t, 3> rgb{255, 255, 255};
	float intensity = 1.0f;  // record float +56, spawned 1.0 [orig: @ 0x5a8e1d]
	// Fade mode (record dword 15) and countdown duration (dwords 16/17)
	// [orig: @ 0x5a8e54/@ 0x5a8e63]. Mode 1 / duration -1 = permanent (the
	// model-light spawn); mode 2 = fade out then die; mode 5 = fade out then
	// hide; other modes count down with no blend ramp, then die.
	int32_t fade_mode = 1;
	int32_t fade_duration = -1;
	bool has_gen = false;
	LightGenBlock gen{};
	// Group culling pair [orig: record dwords 19/20, read by
	// Light_PassesActiveGroups @ 0x5a9134 / @ 0x5a915c].
	uint64_t owner_entity = 0;
	int32_t owner_section = 0;
	// The authored disable trio [orig: @ 0x56c8e7..0x56c91d -> render flags
	// 512 (corona) / 1024 (terrain) / 2048 (objects)].
	bool disable_corona = false;
	bool disable_terrain = false;
	bool disable_objects = false;
	// Render flag 0x100 (the impact-flash spawn): the light rides radius/2
	// above the impact, and the corona walk re-centers its billboards by
	// dropping radius/2 — the flag's only witnessed reader
	// [orig: set @ AmmoDef_ProcessImpactEffect 0x40a2b3; read @
	// EffectWorld_RenderLightCoronas 0x5ab037..0x5ab05c].
	bool corona_lower_half_radius = false;
};

// The low value preserves retail's slot | 0x8000 handle form
// [orig: @ 0x5a8e94]. The generation is an OpenNova safety lease: it prevents
// an expired gameplay handle from mutating a later occupant of the same slot.
struct LightHandle {
	uint16_t retail_value = 0;
	uint32_t generation = 0;
	bool is_null() const { return retail_value == 0 || generation == 0; }
};

inline bool operator==(LightHandle lhs, LightHandle rhs) {
	return lhs.retail_value == rhs.retail_value &&
			lhs.generation == rhs.generation;
}

inline bool operator!=(LightHandle lhs, LightHandle rhs) {
	return !(lhs == rhs);
}

struct LightActiveGroups {
	uint64_t interior_group_entity = 0;
	int32_t interior_group_section = 0;
	uint64_t owner_group_entity = 0;
	int32_t owner_group_section = 0;
};

enum class LightSelectionTarget {
	Objects,
	Terrain,
};

struct LightSelectionOptions {
	// Target-disable flags are eligibility gates and are applied before the
	// three-light cap. The default is the object/material pass.
	LightSelectionTarget target = LightSelectionTarget::Objects;
	// Retail supplies per-draw owner/interior groups. A camera-global adapter
	// has no such draw context and must opt into this explicitly named
	// approximation rather than silently treating empty groups as a wildcard.
	bool admit_owned_unscoped = false;
};

struct SelectedLight {
	std::array<float, 3> position{};  // mission-space float world units
	float position_w = 0.0f;          // 65536 / radius_fixed [orig: @ 0x5a91d4]
	std::array<float, 3> color{};
	std::array<float, 4> attenuation{};
	float range = 0.0f;  // radius_fixed * 1.25 / 65536
	bool lights_terrain = true;
	bool lights_objects = true;
	LightHandle handle{};
};

struct LightFlickerInputs {
	// Env_WaveAmpRing / Env_WaveRingIndex (engine/formats/env env_weather.h).
	const int32_t *amp_ring = nullptr;
	size_t amp_ring_size = 0;
	uint8_t ring_index = 0;
	uint32_t time_ms = 0;  // waveform styles' clock (eval_light_runtime)
};

struct LightSceneReport {
	size_t live = 0;
	size_t high_water = 0;
	size_t last_query = 0;
	size_t last_selected = 0;
};

// One additive corona billboard quad, camera-facing at `center` with
// `half_size` world-unit extents along the camera right/up axes and the
// premultiplied additive color (segment fade folded in)
// [orig: EffectWorld_RenderLightCoronas @ 0x5aaf40, ex
// CEffect_RenderFoliageBillboards — renamed 2026-08-20].
struct LightCoronaQuad {
	std::array<float, 3> center{};  // mission-space float world units
	float half_size = 0.0f;
	std::array<float, 3> rgb{};
};

// The procedural corona texture "texlightcrn": 128x128 ARGB words,
// intensity = trunc(255 x (0.4 - 0.45 x d)) clamped >= 0 with
// d = sqrt((|x-64|/64)^2 + (|y-64|/64)^2) filling the gray channels under
// alpha 255, and the border row/column written as 0x00000000 (alpha 0 —
// the gray there is already inside the clamp's zero region)
// [orig: Lighting_InitTextures @ 0x5a973a..0x5a97ff — abs32 per axis times
// flt_7C3DCC = 1/64, dbl_7DA050 = 0.4, dbl_7DA058 = 0.45, dbl_7D9F98 = 255,
// ftol truncation, the < 0 clamp @ 0x5a9793, 0x010101 * i | 0xFF000000
// @ 0x5a97a2, the border store @ 0x5a97a8]. One texel's word.
inline constexpr int kCoronaTextureSize = 128;
uint32_t corona_texture_argb(int x, int y);

// One owner's live section-visibility mask for the corona walk: bit N set =
// COBJ section N draws this frame (the OcclusionWorld::section_mask domain)
// [orig: g_BuildingSectionVisMask @ 0x297f250, tested per owned corona via
// Terrain_IsBuildingSectionBitSet @ 0x5c6960].
struct LightCoronaOwnerMask {
	uint64_t owner_entity = 0;
	uint32_t section_mask = 0;
};

// The corona pass inputs. The depth plane is retail's batch-sort camera
// plane in mission space: depth(p) = dot(normal, p) + w, growing in front of
// the camera — each segment's alpha is clamp(depth / base_half_size, 0..1)
// [orig: g_BatchSortDepthPlane reads @ 0x5ab2f8..0x5ab33c].
// Fog: the corona pass runs the PRIMARY device fog with the fog color forced
// BLACK (additive fades out, never toward the fog color)
// [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x5aafb6 -> case 2
// FOGCOLOR 0xFF000000 @ 0x677740]; the factor is the witnessed device
// policy (exp ln64/end for type 0, linear with the type-derived starts for
// 2/3) shared with the object/terrain shaders.
struct LightCoronaFrameInputs {
	std::array<int32_t, 3> camera_fixed{};
	std::array<float, 3> depth_plane_normal{};
	float depth_plane_w = 0.0f;
	std::array<float, 3> ambient_scale{1.0f, 1.0f, 1.0f};
	LightFlickerInputs flicker{};
	uint32_t frame_index = 0;  // the witnessed frame & 3 jitter phase
	bool fog_enabled = false;
	int32_t fog_type = 0;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	// Owned coronas gate on their owner's visible-section bits; an owner
	// absent from this table passes (retail: a non-pool-2 owner index falls
	// outside the mask array and Terrain_IsBuildingSectionBitSet returns
	// TRUE @ 0x5c6978).
	const LightCoronaOwnerMask *owner_masks = nullptr;
	size_t owner_mask_count = 0;
};

// The batch-entry light cap: a drawn strip carries at most three dynamic
// lights [orig: collect_render_objects_for_batch @ 0x5d9226..0x5d9229 and
// collect_render_batches_for_entity @ 0x5d96e6..0x5d96e9 break the visible
// walk at the third stored handle; CRenderBatchQueue_FlushBatches reads the
// three entry slots @ 0x5da26b]. The 4 of Light_SelectAndEnableForDraw
// @ 0x5abbeb is only the transient D3D LightEnable count the flush tears
// down per entry (@ 0x5da5de) — never a shader-visible count.
inline constexpr size_t kLightSelectLimit = 3;

// One draw context for the per-draw selection pass: the draw's query AABB
// (mission 16.16) plus its active owner/interior groups — the shape retail
// hands the per-draw select per rendered entity [orig:
// Light_SelectAndEnableForDraw @ 0x5ab9d0 fed with position -/+ boundRadius
// @ 0x5c74fb..0x5c753a; the batch collectors gate the resulting list through
// Light_PassesActiveGroups @ 0x5a9120 and the objects-enable flag
// @ 0x5d920c]. Retail collects ONCE per entity and only re-gates per ROBJ;
// a per-ROBJ caller must therefore stamp the entity's query box on every
// ROBJ draw it splits out (same box -> same ordered list, different groups).
struct LightDrawContext {
	std::array<int32_t, 3> aabb_min_fixed{};
	std::array<int32_t, 3> aabb_max_fixed{};
	LightActiveGroups groups{};
};

struct LightDrawSelection {
	std::array<SelectedLight, kLightSelectLimit> lights{};
	size_t count = 0;
};

class LightScene {
public:
	static constexpr size_t kCapacity = 4096;   // [orig: @ 0x5a8db1]
	static constexpr size_t kQueryLimit = 64;   // [orig: @ 0x5aa384]
	static constexpr size_t kSelectLimit = kLightSelectLimit; // the 3-cap [orig: @ 0x5d9229]

	// The witnessed transient spawners' constants (the pool comment above).
	// Muzzle glow: radius 98304 (1.5 wu), color 0xFFE0A0, re-armed per shot to
	// mode 4 / 5 ticks [orig: Entity_UpdateMuzzleGlowEffect @ 0x56c960].
	static constexpr int32_t kMuzzleGlowRadiusFixed = 0x18000;
	static constexpr uint32_t kMuzzleGlowColorRgb = 0xFFE0A0;
	static constexpr int kMuzzleGlowFadeMode = 4;
	static constexpr int kMuzzleGlowFadeTicks = 5;
	// Death flash: color 0xFFC080, mode 2 / 31 ticks, corona disabled
	// [orig: Entity_SpawnDeathPieces @ 0x49351a].
	static constexpr uint32_t kDeathFlashColorRgb = 0xFFC080;
	static constexpr int kDeathFlashFadeMode = 2;
	static constexpr int kDeathFlashFadeTicks = 31;

	LightHandle spawn(const LightSpawnParams &params);
	void despawn(LightHandle handle);
	void set_position(LightHandle handle, const std::array<int32_t, 3> &position_fixed);
	bool alive(LightHandle handle) const;
	void clear();

	// The witnessed instance setters. Retail writes these through stale
	// handles into whatever occupies the slot; we require the live slot's
	// generation to match (the divergence note in the header map).
	// [orig: LightInstance_SetFadeModeAndDuration @ 0x5a8f80 — d15 = mode,
	// then d17 and d16 = duration]
	void set_fade(LightHandle handle, int32_t mode, int32_t duration);
	// [orig: CEffectInstance_SetBlendAmount @ 0x5a8ee0 — f14 = amount,
	// hidden (flag bit 2) tracks amount < 0.001]
	void set_blend(LightHandle handle, float amount);
	// [orig: LightInstance_SetOwnerGroup @ 0x5a8fb0 — record dwords 19/20]
	void set_owner(LightHandle handle, uint64_t owner_entity, int32_t owner_section);

	// The per-frame decay [orig: EffectWorld_TickInstancesAndLightScale
	// @ 0x5aa170]: run once per 62 Hz tick. Expired mode-5 slots hide, other
	// expired slots die; modes 2/5 blend down linearly while counting.
	void tick();

	// AABB overlap + nearest-first handle list (the witnessed per-draw
	// collection) [orig: collect_nearby_zones_by_aabb @ 0x5aa250].
	size_t query(const std::array<int32_t, 3> &query_min_fixed,
			const std::array<int32_t, 3> &query_max_fixed,
			std::array<LightHandle, kQueryLimit> &out_handles) const;

	// Camera-global approximation: unlike the witnessed per-draw query above,
	// this gathers every overlap before sorting and taking the nearest 64. It
	// prevents slot-order starvation when one global list feeds object shaders.
	size_t query_camera_global(
			const std::array<int32_t, 3> &query_min_fixed,
			const std::array<int32_t, 3> &query_max_fixed,
			std::array<LightHandle, kQueryLimit> &out_handles) const;

	// Group-gate the ordered handles and produce the first <= 3 passers'
	// witnessed parameters [orig: Light_PassesActiveGroups @ 0x5a9120 and the
	// objects-enable gate @ 0x5a9010 in the batch-entry walk
	// @ 0x5d91e0..0x5d9229, which breaks at the third stored handle;
	// Light_GetPointLightParams @ 0x5a9180]. d3d_light_path applies the
	// 1.5x diffuse boost [orig: Light_FillD3DPointLight @ 0x5aa450].
	size_t select(const LightHandle *handles, size_t handle_count,
			const LightActiveGroups &groups,
			const LightSelectionOptions &options,
			const std::array<float, 3> &ambient_scale,
			const LightFlickerInputs &flicker,
			bool d3d_light_path,
			std::array<SelectedLight, kSelectLimit> &out) const;

	// The witnessed per-draw pass: for each draw context run the capped
	// slot-order collect (first 64, then nearest sort — the exact
	// @ 0x5aa250 shape) against a one-pass snapshot of the live pool, then
	// the group-gated first-3 select with that draw's groups. The snapshot
	// carries only the collection inputs (retail's flag-bit-2 skip); target
	// disables stay a select-stage gate exactly as retail applies them.
	void select_for_draws(const LightDrawContext *draws, size_t draw_count,
			const LightSelectionOptions &options,
			const std::array<float, 3> &ambient_scale,
			const LightFlickerInputs &flicker,
			bool d3d_light_path,
			LightDrawSelection *out) const;

	// Monotonic identity for changes which can alter a draw's ordered handle
	// selection: pool membership, position/AABB, owner groups, or hidden state.
	// Color-only changes (fade blend, RGB-gen time/weather, ambient gain) do
	// not advance it because callers can cheaply reevaluate parameters for an
	// already-selected handle set. This lets retained render devices cache the
	// expensive static-draw broadphase without freezing animated light color.
	uint64_t selection_revision() const { return selection_revision_; }

	// The corona billboard walk [orig: EffectWorld_RenderLightCoronas
	// @ 0x5aaf40, called per world scene @ 0x5c96ad and per mirror scene
	// @ 0x5c85fd]: every alive, un-hidden instance without the authored
	// corona-disable draws THREE additive camera-facing quads marching
	// toward the camera — step 0.1 x radius along normalize(cam - light),
	// half-sizes 0.5 x radius shrinking x0.66 per segment, color =
	// record rgb x blend x ambient scale x 1/16 (then the RgbGen multiply),
	// each segment scaled by clamp(camera-plane depth / (0.5 x radius), 0..1)
	// and skipped at <= 0. Admission: camera distance <= 100 wu (0x640000
	// fixed), the owned-light visible-section gate (inputs.owner_masks
	// [orig: Terrain_IsBuildingSectionBitSet @ 0x5c6960, gated @ 0x5ab027]),
	// and a per-frame +-512-fixed x/y jitter phased on frame & 3. A
	// corona_lower_half_radius instance (retail render flag 0x100) drops
	// radius/2 first [orig: @ 0x5ab053..0x5ab05c], and the fog-to-black
	// fold multiplies the color by the primary device fog factor per
	// segment [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x5aafb6].
	size_t collect_corona_quads(const LightCoronaFrameInputs &inputs,
			std::vector<LightCoronaQuad> &out) const;

	// The terrain projected pass's per-patch collect + gate + constant build
	// (light_terrain_pass.h carries the contract; defined in
	// light_terrain_pass.cpp) [orig: render_terrain_sector_batch
	// @0x6095f9..0x6098bc + Light_SetupTerrainProjectedPass @0x5aa830]. Per
	// patch: the slot-order collect capped at SIXTEEN then nearest-first
	// (collect_nearby_zones_by_aabb @0x5aa250 with the 16 cap @0x609658), the
	// group gate with BOTH groups cleared (@0x60967c/@0x609685 — so every
	// owned light fails), the alive + !terrain-disabled gate, and one row per
	// survivor with NO three-light cap. Returns the total row count.
	size_t collect_terrain_pass_rows(
			const opennova::renderer::TerrainLightPatchBounds *patches,
			size_t patch_count,
			const opennova::renderer::TerrainLightPassInputs &inputs,
			opennova::renderer::TerrainLightPatchRows *out) const;

	LightSceneReport inspect() const;

private:
	struct Slot {
		bool live = false;
		uint32_t generation = 0;
		// Flag bit 2: a hidden slot stays allocated but no query returns it
		// [orig: the bit-2 skip in collect_nearby_zones_by_aabb @ 0x5aa250,
		// toggled by CEffectInstance_SetBlendAmount @ 0x5a8ee0].
		bool hidden = false;
		LightSpawnParams params{};
		// Live lifecycle state (record f14 / d16 / d17).
		float blend = 1.0f;
		int32_t fade_counter = 0;
		int32_t fade_initial = 0;
		std::array<int32_t, 3> aabb_min{};
		std::array<int32_t, 3> aabb_max{};
	};

	const Slot *slot_for(LightHandle handle) const;
	Slot *slot_for(LightHandle handle);
	size_t query_impl(const std::array<int32_t, 3> &query_min_fixed,
			const std::array<int32_t, 3> &query_max_fixed,
			bool collect_all_before_cap,
			std::array<LightHandle, kQueryLimit> &out_handles) const;

	std::vector<Slot> slots_;
	// Never reset by clear(): a handle issued before clear must not alias the
	// first occupant of the rebuilt slot vector.
	uint32_t next_generation_ = 1;
	uint64_t selection_revision_ = 1;
	mutable LightSceneReport report_{};
};

// The group a model LGHT record's pool instance is owned by. Retail decides
// this once per record inside the spawner, in a fixed branch order
// [orig: Entity_SpawnGlowEffects @ 0x56c89a..0x56c8db].
struct ModelLightOwnerInputs {
	// Record byte +32, the authored attach subobject [orig: @ 0x56c89a].
	uint8_t attach_bone = 0;
	// The entity whose model carries the record.
	uint64_t spawning_entity = 0;
	// Retail skips the blink query outright when the spawning entity's
	// ItemDef type is Building, so a building's own unattached records stay
	// world lights even though its blink volumes contain them (the query has
	// no self-exclusion) [orig: the ItemType_Building gate @ 0x56c7ec].
	bool spawner_is_building = false;
	// The blink query at the SPAWNING ENTITY's position, run once before the
	// record walk: whether it hit any blink volume, and slot 0's decoded
	// owner/section [orig: Entity_QueryBlinkBoxesAtPoint @ 0x56c7fc, the
	// count test @ 0x56c8bd, Pool_GetEntryUnchecked(2, hit >> 20) @ 0x56c8c9
	// and (hit >> 12) & 0x1F @ 0x56c8db]. The packed-hit decode itself lives
	// with the packing (world::BlinkAccum).
	bool blink_hit = false;
	uint64_t blink_owner_entity = 0;
	int32_t blink_section = 0;
};

struct ModelLightOwner {
	uint64_t entity = 0;  // 0 = unowned: a world light, gated by nothing
	int32_t section = 0;
};

// An attached record is owned by its own entity + subobject (cabin
// self-lights); an unattached record spawned INSIDE a blink box is owned by
// the containing building + that volume's section (interior room lights);
// every other record — the fire barrels — spawns unowned and lights the
// world [orig: Entity_SpawnGlowEffects @ 0x56c89f / @ 0x56c8bd].
ModelLightOwner resolve_model_light_owner(const ModelLightOwnerInputs &inputs);

// The witnessed flicker register value for one light: the position hash into
// the weather wave ring [orig: Light_TickGenBlock @ 0x5a8ae0 -> the global
// FLICKER ctrl slot 0x83FD00 = 0x83FCE8 + 8 * 3].
int32_t light_flicker_value(const std::array<int32_t, 3> &position_fixed,
		const LightFlickerInputs &flicker);

} // namespace renderer
