#pragma once

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <foliage/model_dispatcher.h>
#include <foliage/placement.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "nova_terrain_foliage_def.h"

namespace godot {

class Image;
class NovaTerrainData;

// Foliage adapter for the shared engine-spec placement core. Renders the two
// witnessed retail tiers, and only those (docs/foliage/foliage-re.md):
//
//   - FAR tier: the near-camera carpet. The terrain feed collects 16u leaf
//     cells whose clamped-AABB 3D distance from the camera is <= 42.0
//     (<= 128 per frame) [orig: Terrain_TraverseQuadtreeNode @ 0x60905c ->
//     Terrain_CollectNearFoliagePatches @ 0x603e60]. Each new cell is baked
//     ONCE into a persistent per-slot pool - up to 36 placements, each a
//     complete copy of the def source mesh at XZ scale 1/Y scale .5, terrain
//     sampled under every transformed vertex, source-Y wind weight in
//     COLOR.r [orig: Foliage_UpdateFarCellSlots @ 0x601b30;
//     generate_foliage_instances_0 @ 0x5ffdd0]. Per frame the pool only
//     toggles visibility and refreshes the witnessed per-cell draw params:
//     fade alpha 1 through distance 20 then 1-(d-20)/22, alpha-test ref 180
//     under 33 else 8 [orig: render_terrain_lightmaps @ 0x60a171..0x60a53b].
//     Godot's per-node frustum culling stands in for the traversal's frustum
//     gate; the host enumerates the 42u disc directly (D-FOLIAGE-7).
//   - NEAR/MODEL tier: full 3DI geometry stamped in clusters around anchors
//     (the host equivalent of visible sector entities >= 38.0 view depth),
//     via the libs ModelDispatcher [orig: Terrain_RenderSectorEntitiesBySide
//     @ 0x5c7d50; Foliage_UpdateModelTiles @ 0x601f50;
//     Foliage_GenerateModelTileInstances @ 0x600980].
//
// There is no camera-carpet grid, per-center quadrant walk, or any other
// non-retail coverage algorithm here: the jodemo-era per-entity dispatcher
// (sub_5C1940) was removed when the retail slot-pool architecture was
// witnessed. Editor previews run this same path.
class NovaFoliageDispatcher : public Node3D {
	GDCLASS(NovaFoliageDispatcher, Node3D)

public:
	NovaFoliageDispatcher();
	~NovaFoliageDispatcher();

	// Inputs ----------------------------------------------------------------

	void set_foliage_defs(const Array &p_defs);
	Array get_foliage_defs() const;

	// Per-slot model Mesh (the def's `graphic` 3DI). Parallel array to
	// foliage_defs. Both tiers stamp its authored geometry; FAR uses XZ 1/Y .5,
	// MODEL uses XZ .75/Y .5 plus its bound-square ground fit. A null entry
	// disables the slot. Swaps invalidate both placement caches.
	void set_slot_meshes(const Array &p_meshes);
	Array get_slot_meshes() const;

	// Per-slot ":fd" textures (flat-0x808080, smoothed alpha), parallel to
	// foliage_defs - BOTH tiers bind them [orig: Foliage_LoadDefAssets
	// @ 0x601260 tail; Foliage_DrawModelTileSlot @ 0x601d90]. Missing/null
	// entries do not invent a raw-diffuse replacement.
	void set_slot_fd_textures(const Array &p_textures);
	Array get_slot_fd_textures() const;

	// NEAR-tier anchors in Godot world space, refreshed per tick by hosts.
	// The witnessed driver is per-SECTOR-ENTITY (the .trn/.bms-placed world
	// models) [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]; placed
	// world objects are the host equivalent. Does NOT invalidate any cache.
	void set_model_anchors(const PackedVector3Array &p_anchors);
	PackedVector3Array get_model_anchors() const;

	// Host visibility mapping for the model tier: retail only dispatches
	// VISIBLE sector entities (sector render + occlusion test); the host
	// approximates with a view-depth range gate. Anchors beyond this many
	// world units of view depth are skipped. Default 512 = the distance where
	// the witnessed alpha-ref curve reaches its floor (4096/512 = 8).
	void set_model_anchor_range(float p_range);
	float get_model_anchor_range() const;

	// The dispatch view's frustum half-angles (vertical fov + aspect). Retail
	// only dispatches VISIBLE sector entities [orig:
	// Terrain_RenderSectorEntitiesBySide @ 0x5c7d50 — sector render +
	// occlusion gate]; the host gates anchors on the view frustum (occlusion
	// stays un-hosted — the host culls strictly less than retail). A fov of 0
	// disables the gate (headless probes/tests keep the old depth-only walk).
	void set_model_view_fov(float p_fov_y_deg, float p_aspect);

	// In-place ":fd" bake of an RGBA8 Image (pow2 dimensions required):
	// alpha smoothed by the witnessed 3x3 kernel, RGB flattened to exactly
	// 0x808080 [orig: Foliage_LoadDefAssets @ 0x601260 tail]. Returns false
	// (image untouched) for non-RGBA8 or non-pow2 input.
	static bool bake_fd_image(const Ref<Image> &p_image);

	// Callable receiving (world_x: float, world_z: float) -> height: float.
	// Return value <= -1e6 indicates "no terrain here; skip candidate".
	// Runtime analogue: Terrain_SampleHeightBilinear@0x5C6770.
	void set_height_sampler(const Callable &p_sampler);
	Callable get_height_sampler() const;

	// Callable receiving (world_x: float, world_z: float) -> int. Returns the
	// foliage-map palette index at that world position (0 means empty).
	// Runtime analogue: Foliage_SampleFoliageMapMask @ 0x606620. MODEL only.
	void set_foliage_sampler(const Callable &p_sampler);
	Callable get_foliage_sampler() const;

	// Callable receiving the retail surface-query boundary
	// (world_x: float, native_z: float) -> raw uint8 slot mask. FAR passes
	// native_z=-candidate_world_z exactly as witnessed; unlike MODEL's
	// foliage_sampler, this value is never translated through def.match.
	void set_surface_sampler(const Callable &p_sampler);
	Callable get_surface_sampler() const;

	// Direct runtime fast path. When set, dispatch uses NovaTerrainData
	// height, raw surface-mask, and foliage-map queries directly and skips
	// Callable/Variant boxing. Editor leaves this unset so the
	// live-sculpt-aware Callable path still runs.
	void set_terrain_data(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_terrain_data() const;

	// Optional source for FAR pass T1 (the terrain light/colormap texture). The
	// editor uses it while placement remains on live-sculpt Callable samplers;
	// runtime normally obtains the same texture from terrain_data.
	void set_colormap_source(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_colormap_source() const;

	// Main API --------------------------------------------------------------

	// Per-frame dispatch. `centre` is the camera position - the witnessed
	// 42.0 collect distance, per-cell fade, and high/low pass metrics all
	// measure from it. `view_xform` feeds only the MODEL tier's 38.0
	// view-depth gate; the identity transform passes that gate so headless
	// tests exercise the walk.
	void dispatch(Vector3 centre, Transform3D view_xform = Transform3D());

	// Drop all pools, caches, and draw nodes. Call on paint / def edits.
	void reset();

	// Introspection
	int get_total_instances() const;
	int get_cached_cells() const;
	Dictionary get_dispatch_stats() const;

	// Model-tier test/debug introspection: one Dictionary per current model
	// instance for the slot ({center, hbase, yaw, corners, transform, color,
	// custom, bound_center, bound_radius}) - the GUT transform-vs-libs-corner
	// parity pin reads these (ADR 0018 public-seam testability).
	Array get_model_tile_debug(int p_slot) const;

	// One Dictionary per RENDERED model batch ({slot, tile_key, anchor,
	// view_depth, anchor_distance, alpha_ref, wind_counter, wind_phase,
	// instance_count, submissions}). Retail submits a shared tile once per
	// qualifying sector entity (the wind counter advances per submission
	// [orig: Foliage_UploadModelTileVSConstants @ 0x600f00]) and each later
	// immediate-mode draw overwrites the earlier one (z-write on, LESSEQUAL
	// [orig: Foliage_DrawModelTileSlot @ 0x601e33]); the retained host keeps
	// ONE batch per (slot, tile) carrying the LAST submission's state, with
	// `submissions` counting the retail-equivalent draws.
	Array get_model_draw_debug() const;

	// FAR test/debug introspection: one Dictionary per placement in every
	// ACTIVE pooled cell ({center, yaw, cell_key, distance, fade, alpha_ref}).
	Array get_far_tile_debug(int p_slot) const;

protected:
	static void _bind_methods();

private:
	// FAR: one persistent baked cell per (slot, packed retail cell key)
	// [orig: Foliage_UpdateFarCellSlots @ 0x601b30 - resident keys are only
	// re-stamped; new keys bake into an LRU-evicted slot]. `node` is null
	// when the cell baked empty (cached emptiness is retail behavior: a
	// zero-count slot stays resident and draws nothing).
	struct FarCellEntry {
		MeshInstance3D *node = nullptr;
		std::vector<opennova::foliage::PlacementInstance> placements;
		int64_t last_touched = 0;
		bool active = false;
		float distance = 0.0f;
	};
	struct FarVisibleCell {
		uint32_t key = 0;
		int32_t cell_x_int = 0;  // 16u-aligned world ints, CELL_GRID key basis
		int32_t cell_z_int = 0;  // (+16 biased, the walk's witnessed Z bias)
		float distance = 0.0f;   // clamped-box 3D distance from the camera
	};

	// Per pooled MODEL draw node: what its MultiMesh currently holds, so
	// stable draws skip the instance re-upload entirely. Nodes are KEYED by
	// (slot, tile_key) — the retained model the original's tile cache carries
	// [orig: the 1000-entry per-def cache @ 0x601f50: entries own their tile's
	// content across frames; only the 8-frame-stagger regeneration rewrites
	// one]. Pairing draw nodes to batches by list ORDER re-uploaded whole
	// MultiMeshes whenever the batch order shifted (anchors moving between
	// quadrants) — the 76k-uploads/15.8 ms-frame churn the perf probe caught.
	// The material is the node's own (per-draw alpha ref/wind phase are
	// witnessed per-draw state); created once per node from the slot base.
	struct ModelDrawNodeState {
		int slot = -1;
		uint32_t tile_key = 0xFFFFFFFFu;
		int64_t generation = -1;
		int count = 0;
		uint64_t last_used_frame = 0;  // LRU reuse stamp (0 = never used)
		bool in_use = false;           // used by a batch this dispatch
		Ref<ShaderMaterial> material;
	};

	struct DispatchStats {
		int64_t dispatch_calls = 0;
		int64_t far_us = 0;
		int64_t material_us = 0;
		int64_t model_us = 0;
		// FAR (witnessed pool mechanics).
		int64_t far_cells_visible = 0;   // last dispatch
		int64_t far_pool_hits = 0;
		int64_t far_pool_misses = 0;
		int64_t far_cells_baked = 0;     // cumulative place_cell+mesh bakes
		int64_t far_instances_baked = 0; // cumulative placements baked
		// Model tier (last dispatch / accumulated in the libs dispatchers).
		int64_t model_anchors_in_range = 0;
		int64_t model_tiles_emitted = 0;
		int64_t model_instances = 0;
		int64_t model_uploads = 0;       // cumulative change-detected re-uploads
	};

	// FAR pool state.
	std::array<std::unordered_map<uint32_t, FarCellEntry>,
	           opennova::FOLIAGE_MAX_DEFS> far_cells_;
	std::vector<FarVisibleCell> far_visible_;
	int64_t far_frame_counter_ = 0;

	// NEAR/MODEL tier: one shared-core model dispatcher per foliage slot.
	std::array<opennova::foliage::ModelDispatcher, opennova::FOLIAGE_MAX_DEFS> model_dispatchers_{};
	struct ModelDrawBatch {
		int slot = 0;
		uint32_t tile_key = 0;
		int64_t generation = 0;
		Vector3 anchor;
		float view_depth = 0.0f;
		float anchor_distance = 0.0f;
		float alpha_ref = 8.0f;  // D3D alpha-ref byte domain [8, 128]
		int64_t wind_counter = 0;
		float wind_phase = 0.0f; // counter * 0.001
		int submissions = 1;     // retail-equivalent draws folded into this batch
		// Borrowed view into the slot dispatcher's cache entry (valid until
		// its next walk(); see ModelTileDraw). Upload paths copy from here
		// only when `generation` changed.
		const opennova::foliage::ModelInstance *instances = nullptr;
		int instance_count = 0;
	};
	std::vector<ModelDrawBatch> model_draw_batches_;
	std::vector<MultiMeshInstance3D *> model_draw_nodes_;
	std::vector<ModelDrawNodeState> model_draw_node_states_;
	// (slot << 32) | tile_key -> node index: the keyed pool lookup.
	std::unordered_map<uint64_t, size_t> model_node_by_key_;
	uint64_t model_draw_frame_ = 0;
	PackedVector3Array model_anchors_;
	float model_anchor_range_ = 512.0f;
	float model_view_tan_half_h_ = 0.0f;  // 0 = frustum gate off
	float model_view_tan_half_v_ = 0.0f;
	int32_t model_frame_counter_ = 0;
	// Retail increments this once per TILE DRAW
	// [orig: Foliage_ModelWindPhaseCounter @ 0x3162170, bumped in
	// Foliage_UploadModelTileVSConstants @ 0x600f00].
	int64_t model_wind_counter_ = 0;

	// Per-slot model bounds derived from the slot mesh AABB - the host analog
	// of the retail def-table bounds (XZ min/max over the raw 3DI verts ->
	// center + Chebyshev radius) [orig: Foliage_LoadDefAssets @ 0x601260].
	struct SlotModelBounds {
		bool valid = false;
		float center_x = 0.0f;  // (cx, cz) in Godot mesh space
		float center_z = 0.0f;
		float radius = 1.0f;    // max(halfExtentX, halfExtentZ), Chebyshev
		float max_y = 1.0f;     // max vertex Y from the mesh AABB
	};

	Ref<Shader> foliage_far_shader_;
	Ref<Shader> foliage_model_shader_;
	Ref<ShaderMaterial> foliage_materials_[4];
	Ref<ShaderMaterial> foliage_model_materials_[4];
	SlotModelBounds slot_bounds_[4];

	// Configuration
	Array foliage_defs_;
	Array slot_meshes_;
	Array slot_fd_textures_;
	Callable height_sampler_;
	Callable foliage_sampler_;
	Callable surface_sampler_;
	Ref<NovaTerrainData> terrain_data_;
	Ref<NovaTerrainData> colormap_source_;

	DispatchStats dispatch_stats_;

	// Helpers ---------------------------------------------------------------

	bool _has_sampling_source() const;
	float _sample_height_world(float p_world_x, float p_world_z) const;
	void _collect_far_cells(const Vector3 &camera_pos);
	void _dispatch_far_tier(const Vector3 &camera_pos);
	void _bake_far_cell_into(int slot_index,
	                         const FarVisibleCell &cell,
	                         const Ref<NovaTerrainFoliageDef> &def,
	                         FarCellEntry &entry);
	void _apply_far_cell_state(int slot_index, FarCellEntry &entry, float distance);
	void _evict_far_overflow(int slot_index);
	void _dispatch_model_tier(const Transform3D &view_xform, const Dictionary &defs_by_match);
	void _update_model_draw_nodes();
	void _clear_model_draw_nodes();
	size_t _acquire_model_draw_node(uint64_t p_key);
	void _clear_far_cells();
	// Source-model -> Godot yaw mapping: rotY(yaw + pi/2), witnessed at the
	// MODEL draw transform. FAR owns a separate emitter mapping.
	Basis _engine_yaw_basis(float yaw_radians) const;
	// The derived model-instance transform (see the .cpp derivation comment):
	// translate(center, hbase) * rotY(yaw + pi/2) * scale(0.75, 0.5, 0.75) *
	// translate(-(cx, 0, cz)).
	Transform3D _model_instance_transform(const opennova::foliage::ModelInstance &inst,
	                                      float hbase,
	                                      const SlotModelBounds &bounds) const;
	void _refresh_slot_bounds();
	Ref<Texture2D> _slot_fd_texture(int slot_index) const;
	void _make_model_sampler_bindings(
	    const Dictionary &defs_by_match,
	    opennova::foliage::PlacementSamplers &out_samplers) const;
	void _update_slot_material(int slot_index);
	void _update_model_slot_material(int slot_index);
	bool _scatter_cell(int slot_index,
	                   int cell_x_int, int cell_z_int,
	                   const Ref<NovaTerrainFoliageDef> &def,
	                   std::vector<opennova::foliage::PlacementInstance> &out_placements);
	Dictionary _build_defs_by_match() const;
	void _clear_children();
	Ref<Mesh> _build_far_mesh(
	    int slot_index,
	    const std::vector<opennova::foliage::PlacementInstance> &placements) const;
};

} // namespace godot
