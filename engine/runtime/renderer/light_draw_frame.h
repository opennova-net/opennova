#pragma once

// The per-draw light pass over one view: the draw contexts a frame's drawn
// models declare, the selection the pool hands each one, the retained static
// rows, and the corona walk's frame inputs. The pool and its witness map are
// light_scene.h; this is the policy that turns a view's drawn entities into
// the pool's draw contexts, so an embedder only gathers each drawn model's
// facts and applies the selections to its device.
//
// THE DRAWN SET: every entity the scene walk draws selects its lights right
// before its draw, and nothing else gates the select (no camera distance):
// [orig: Terrain_RenderSectorModels @ 0x5c602e / @ 0x5c5f12 for the sector
// models; Terrain_RenderSectorEntities @ 0x5c7bf1 and
// Terrain_RenderSectorEntitiesBySide @ 0x5c7f9a through
// Terrain_SetupEffectForEntity @ 0x5c753a for the statics, rounds and persons;
// Light_SelectAndEnableForDraw @ 0x5ab9d0 carries no distance test]. What is
// drawn is the collect's verdict [orig: Terrain_CollectVisibleEntitiesForTerrain
// @ 0x5c8d1a] (the occlusion collect and the LOD walk here). The weapon Inset
// pass re-runs the scene core, so its draws select again [orig:
// Render_WeaponInsetScene @ 0x5c9de9 -> Terrain_RenderWorldScene, the collect
// @ 0x5c94f0], as does a water mirror's reflected scene.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <formats/env/env_weather.h>
#include <runtime/renderer/light_scene.h>

namespace opennova::renderer {

// The objects-target select's frame inputs: the env light-state gain (the
// ported EffectWorld_AmbientScale) and the flicker ring.
struct LightFrameInputs {
	std::array<float, 3> ambient{1.0f, 1.0f, 1.0f};
	LightFlickerInputs flicker{};
};

// The flicker inputs over the weather's wave ring (null: no weather, the ring
// reads zero) at the frame's tick clock (light_scene.h light_flicker_value).
LightFlickerInputs light_flicker_inputs(const env::WeatherOscillator *oscillator,
		uint32_t time_ms);

// Every object delivery path publishes the Light_GetPointLightParams colour:
// retail's shader passes receive exactly that through PointLightColor /
// PointLightColorArray [orig: CRenderBatchQueue_FlushBatches @0x5da6a8 /
// @0x5da8ae], and the D3D fill's 1.5 [orig: Light_FillD3DPointLight
// @0x5aa4b2] reaches only the fixed-function D3D lights, which the FF shader
// helper applies itself. The render-slot pick reads the params directly too.
inline constexpr bool kObjectLightD3DFill = false;

// One drawn render model of an entity, as the per-draw pass reads it. Every
// model of an entity (its head and husk parts, its held weapon) shares the
// entity's query and groups (light_scene.h EntityLightQuery).
struct LightDrawSubject {
	EntityLightQuery query{};
	// The drawn entity's light-owner id (light_spawn.h LightOwnerIds); 0 for a
	// draw that declares no owner (the first-person pass).
	uint64_t owner_entity = 0;
	// The interior group: the building the entity stands inside and that
	// blink volume's section (0 outdoors).
	uint64_t interior_entity = 0;
	int32_t interior_section = 0;
	// A Building-type def: its draw is its own interior group at section zero
	// with the owner section re-scoped per ROBJ.
	bool building = false;
	// The draw runs in the person wave (its owner group is the drawn entity).
	bool person_wave = false;
	// The drawn level is skinned (the skinned collector keeps the owner group).
	bool skinned = false;
	// The ROBJs this view draws of the model, read only where a ROBJ can select
	// apart (a building, or a rigid draw at interior section zero).
	const int32_t *robjs = nullptr;
	size_t robj_count = 0;
};

// Which subject (and ROBJ) a draw context belongs to: `per_robj` draws apply
// to that one ROBJ, the others to the whole model.
struct LightDrawTarget {
	uint32_t subject = 0;
	int32_t robj_index = 0;
	bool per_robj = false;
};

// One view's per-draw pass. Retail collects ONCE per entity and only re-gates
// per ROBJ [orig: Lighting_SetOwnerLightGroup(0, robjIndex) @ 0x5d8ff7 between
// the two walks of Render_CollectRenderObjectsForBatch @ 0x5d8f20], so every
// ROBJ draw split out of a model carries the entity's query box.
class LightDrawFrame {
public:
	void clear();
	// One drawn model's contexts:
	// - a building: one per drawn ROBJ, the building its own interior group at
	//   section zero, owner (0, robj) [orig: Terrain_RenderSectorModels @ 0x5c5e07
	//   pushes building/section 0; the re-scope @ 0x5d8ff7]; a building that
	//   draws no ROBJ keeps one ROBJ-0 context, so it never falls back to the
	//   wider entity-owner admission;
	// - every other draw: the interior group it stands in, the owner group its
	//   submit declares (submit_owner_group); a rigid draw splits per drawn
	//   ROBJ only where the owner section can matter, an interior group at
	//   section zero (the gate falls back to the owner section there).
	void add(const LightDrawSubject &subject);
	// The witnessed per-draw select over every context added.
	void select(const LightScene &scene, const LightFrameInputs &inputs);

	const std::vector<LightDrawContext> &draws() const { return draws_; }
	const std::vector<LightDrawTarget> &targets() const { return targets_; }
	// Parallel to targets(), valid after select().
	const std::vector<LightDrawSelection> &selections() const { return selections_; }
	size_t subject_count() const { return subjects_; }
	bool any_per_robj_building() const { return any_building_; }

private:
	void push(const LightDrawContext &draw, int32_t robj, bool per_robj);

	std::vector<LightDrawContext> draws_;
	std::vector<LightDrawTarget> targets_;
	std::vector<LightDrawSelection> selections_;
	uint32_t subjects_ = 0;
	bool any_building_ = false;
};

// The retained static atlas rows: one row per batched static ROBJ, its
// identity fixed (a carved entity stays a zero row), each row's <= 3 lights
// selected per frame as retail's per-batch select does. Each row packs
// kStaticLightRowTexels RGBA float texels: the count in texel 0.x, then three
// (position.xyz, attenuation2) / (color.rgb, range) pairs with positions in
// the presentation frame, then the row's entity lighting lane
// (static_row_entity_lighting plus the mirror's CLIP arming).
inline constexpr int kStaticLightRowTexels = 10;
inline constexpr int kStaticLightRowLaneTexel = kStaticLightRowTexels - 1;

// One static row's lighting lane: renderer::static_row_entity_lighting
// (effectScale, interior lerp, daylight) and, in w, the water mirror's CLIP
// arming as an offset from the instance origin (a building's floor in the
// sector-model pass, the bound radius in the first entity wave; a static row
// is never a person). `contained` is the row's placement blink hit.
std::array<float, 4> static_light_row_lane(bool building, int32_t robj_index,
		float light_transfer, bool contained, int32_t model_floor_q16,
		int32_t entity_bound_radius_q16);

struct StaticLightRow {
	EntityLightQuery query{};
	LightActiveGroups groups{};
	std::array<float, 4> lane{1.0f, 0.0f, 0.0f, 0.0f};
	bool active = false;
};

class StaticLightRows {
public:
	// The row set and its revision (a new revision drops the cached
	// selection). Rows are indexed by their atlas row.
	void set_rows(int64_t revision, std::vector<StaticLightRow> rows);
	// One frame's select. The selected handles cache by the pool's selection
	// revision and the row revision (the broadphase and the group gates);
	// steady frames re-evaluate only gen-animated rows unless the pool's colour
	// revision or the gain moved, row by row. Returns the active rows lit.
	int select(const LightScene &scene, const LightFrameInputs &inputs);
	// The atlas: texture_rows() x kStaticLightRowTexels RGBA floats.
	const std::vector<float> &texels() const { return texels_; }
	int texture_rows() const;
	// Whether select() changed texels() since the last take_dirty().
	bool take_dirty();
	// Retire the published payload (zeroed, dirty) and drop the claim that it
	// mirrors the cached rows; the live rows stay.
	void clear_output();
	void reset();

	int row_count() const { return static_cast<int>(rows_.size()); }
	int active_draws() const { return active_draws_; }
	int lit_draws() const { return lit_draws_; }

private:
	struct Cached {
		int atlas_row = 0;
		LightActiveGroups groups{};
		std::array<LightHandle, LightScene::kSelectLimit> handles{};
		size_t count = 0;
		bool animated = false;
		size_t last_count = 0;
	};
	void write_row(int atlas_row, const LightDrawSelection &selection);
	void write_lane(int atlas_row);

	std::vector<StaticLightRow> rows_;
	int64_t revision_ = -1;
	std::vector<Cached> cached_;
	bool cache_valid_ = false;
	uint64_t cached_scene_revision_ = 0;
	uint64_t cached_color_revision_ = 0;
	std::array<float, 3> cached_ambient_{};
	std::vector<float> texels_;
	bool resident_ = false;
	bool dirty_ = false;
	int active_draws_ = 0;
	int lit_draws_ = 0;
};

// The corona walk's frame inputs for one view: the camera (mission 16.16) and
// its unit forward in mission space, the frame & 3 phase, the gain and
// flicker, the primary fog folded to black, and the owners' visible-section
// masks of that view's own collect (absent owners pass, light_scene.h).
struct LightCoronaFog {
	bool enabled = false;
	int32_t type = 0;
	float start = 0.0f;
	float end = 0.0f;
};

LightCoronaFrameInputs light_corona_frame_inputs(const std::array<int32_t, 3> &camera_fixed,
		const std::array<float, 3> &forward_mission, const LightFrameInputs &inputs,
		uint32_t frame_index, const LightCoronaFog &fog,
		const std::vector<LightCoronaOwnerMask> &owner_masks);

} // namespace opennova::renderer
