// The EffectWorld dynamic light/glow instance pool — the runtime system behind
// D-RLIT-4. Retail keeps one process-global table of 176-byte instances
// [orig: Light_InstanceTable @ 0x2732e28, capacity 4096, high-water count
// @ 0x2732de4; spawner LightPool_SpawnGlowEffect @ 0x5a8d50 returns
// slot | 0x8000]. Placed-model lights enter it at mission start — a walk over
// entity pools 1..2 spawning one instance per model light record
// [orig: Game_StartMission @ 0x525d19 -> sub_5227B0 -> Entity_SpawnGlowEffects
// @ 0x56c7c0: world position via the entity matrix, radius = atten_end * 65536
// (flt_7C32BC), packed color -1 (white), lifetime 1, fade -1, the record's
// RGB-gen block attached, subobject/blink-box attach, and the three authored
// disable flags folded to render flags 512/1024/2048 @ 0x56c8e7..0x56c91d].
// The four witnessed transient spawners routed here are ammo impact
// @ 0x40a2b3, death pieces @ 0x49351a, round spawn @ 0x4ec8da, and muzzle
// glow @ 0x56c987. Powerup registration @ 0x442ce6 remains unhosted.
//
// Per draw context retail queries the pool by AABB and takes the NEAREST
// instances: overlap test + center-distance sort, at most 64 handles
// [orig: collect_nearby_zones_by_aabb @ 0x5aa250 — distance metric
// sum(((d*d + 0x8000) >> 16)) per axis, bubble sort, skip flag bit 2].
// update_light_slots then group-gates the ordered list and enables the FIRST
// FOUR passers as D3D lights [orig: update_light_slots @ 0x5abc50 — owner
// entity at record dword 19, section at dword 20; an owned light passes only
// for the active interior/owner group]. The per-light parameters are
// [orig: Light_GetPointLightParams @ 0x5a9180]: color = record RGB (bytes
// * 1/256 at spawn) x EffectWorld_AmbientScale x intensity, then the optional
// RGB-gen multiply; attenuation {1, 0, 15/range^2, 1} with range =
// radius_fixed * 1.25 / 65536; the D3D-light fill adds a 1.5x diffuse boost
// [orig: Light_FillD3DPointLight @ 0x5aa450].
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
//    `MF_Light` flag (+36)]: spawn-once per entity (handle at entity+436),
//    radius 1.5 (98304), color 0xFFE0A0, then per shot re-armed to mode 4 /
//    duration 5, owner = shooter, position + blend 1.0 — five ticks after the
//    last shot the slot dies, and the cached handle means that entity never
//    glows again this life (the flash particle masks it).
//  - impact flash [orig: AmmoDef_ProcessImpactEffect @ 0x40a2b3]: ammo
//    `light_impact` radius/color/ticks, spawned radius/2 above the impact,
//    mode 2, gated on the impact-effect leg actually presenting; also sets
//    render flag 0x100 (no witnessed reader — not carried).
//  - death flash [orig: Entity_SpawnDeathPieces @ 0x49351a]: husk deaths
//    above water, non-decorations — 2x the piece model's bound radius, color
//    0xFFC080, mode 2, duration 31, corona disabled (flag 512).
//  - round glow [orig: RoundData_SpawnRound @ 0x4ec8da]: ammo `light_move`
//    radius/color, mode 1 / duration -1, terrain disabled (flag 1024), handle
//    at round+0x1B4, follows the round per tick and clears on release.
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
// [orig: Lighting_InitTextures @ 0x5a94f0], fog+blend mode 2 (additive).
// Owned lights additionally gate on the owner building's visible section
// bits (Terrain_IsBuildingSectionBitSet) — unported, with the flag-0x100
// impact recentering (corona drops radius/2 — the only witnessed reader of
// the impact spawn's 0x100 flag) and the fog-to-black fold (D-RLIT-4).
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
// Divergences tracked on D-RLIT-4: retail's setters write through stale
// handles into reused slots; OpenNova's generation lease intentionally
// rejects those writes. Retail derives the ambient scale from
// Env_TerrainColorRecip bytes / 128 in the same tick (@ 0x5aa1f0) where our
// presenter feeds the env light-state gain.
//
// Reimpl shape: positions stay in mission space (the retail Y-negation is the
// world->D3D fold the presenter replaces); the gen block is stored by value
// (retail stores a pointer into the loaded model). Remaining residuals live
// on the D-RLIT-4 row.
#pragma once

#include <renderer/light_runtime.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
	// Group culling pair [orig: record dwords 19/20 @ 0x5abc90..0x5abd1d].
	uint64_t owner_entity = 0;
	int32_t owner_section = 0;
	// The authored disable trio [orig: @ 0x56c8e7..0x56c91d -> render flags
	// 512 (corona) / 1024 (terrain) / 2048 (objects)].
	bool disable_corona = false;
	bool disable_terrain = false;
	bool disable_objects = false;
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
	// four-light cap. The default is the object/material pass.
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

// The corona pass inputs. The depth plane is retail's batch-sort camera
// plane in mission space: depth(p) = dot(normal, p) + w, growing in front of
// the camera — each segment's alpha is clamp(depth / base_half_size, 0..1)
// [orig: g_BatchSortDepthPlane reads @ 0x5ab2f8..0x5ab33c].
struct LightCoronaFrameInputs {
	std::array<int32_t, 3> camera_fixed{};
	std::array<float, 3> depth_plane_normal{};
	float depth_plane_w = 0.0f;
	std::array<float, 3> ambient_scale{1.0f, 1.0f, 1.0f};
	LightFlickerInputs flicker{};
	uint32_t frame_index = 0;  // the witnessed frame & 3 jitter phase
};

// One draw context for the per-draw selection pass: the draw's query AABB
// (mission 16.16) plus its active owner/interior groups — the shape retail
// hands update_light_slots per rendered entity [orig: update_light_slots
// @ 0x5abc50 consumes the per-draw collect @ 0x5aa250].
struct LightDrawContext {
	std::array<int32_t, 3> aabb_min_fixed{};
	std::array<int32_t, 3> aabb_max_fixed{};
	LightActiveGroups groups{};
};

struct LightDrawSelection {
	std::array<SelectedLight, 4> lights{};
	size_t count = 0;
};

class LightScene {
public:
	static constexpr size_t kCapacity = 4096;   // [orig: @ 0x5a8db1]
	static constexpr size_t kQueryLimit = 64;   // [orig: @ 0x5aa384]
	static constexpr size_t kSelectLimit = 4;   // [orig: @ 0x5abd28]

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

	// Group-gate the ordered handles and produce the first <= 4 passers'
	// witnessed parameters [orig: update_light_slots @ 0x5abc50;
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
	// the group-gated first-4 select with that draw's groups. The snapshot
	// carries only the collection inputs (retail's flag-bit-2 skip); target
	// disables stay a select-stage gate exactly as retail applies them.
	void select_for_draws(const LightDrawContext *draws, size_t draw_count,
			const LightSelectionOptions &options,
			const std::array<float, 3> &ambient_scale,
			const LightFlickerInputs &flicker,
			bool d3d_light_path,
			LightDrawSelection *out) const;

	// The corona billboard walk [orig: EffectWorld_RenderLightCoronas
	// @ 0x5aaf40, called per world scene @ 0x5c96ad and per mirror scene
	// @ 0x5c85fd]: every alive, un-hidden instance without the authored
	// corona-disable draws THREE additive camera-facing quads marching
	// toward the camera — step 0.1 x radius along normalize(cam - light),
	// half-sizes 0.5 x radius shrinking x0.66 per segment, color =
	// record rgb x blend x ambient scale x 1/16 (then the RgbGen multiply),
	// each segment scaled by clamp(camera-plane depth / (0.5 x radius), 0..1)
	// and skipped at <= 0. Admission: camera distance <= 100 wu (0x640000
	// fixed) and a per-frame +-512-fixed x/y jitter phased on frame & 3.
	// Residual gaps (doc'd on D-RLIT-4): the owner visible-section gate
	// (Terrain_IsBuildingSectionBitSet on owned lights), the flag-0x100
	// impact recentering, and the fog-to-black additive fold.
	size_t collect_corona_quads(const LightCoronaFrameInputs &inputs,
			std::vector<LightCoronaQuad> &out) const;

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
	mutable LightSceneReport report_{};
};

// The witnessed flicker register value for one light: the position hash into
// the weather wave ring [orig: Light_TickGenBlock @ 0x5a8ae0 -> the global
// FLICKER ctrl slot 0x83FD00 = 0x83FCE8 + 8 * 3].
int32_t light_flicker_value(const std::array<int32_t, 3> &position_fixed,
		const LightFlickerInputs &flicker);

} // namespace renderer
