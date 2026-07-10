#pragma once

#include <godot_cpp/classes/mesh.hpp>
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

#include <foliage/dispatcher.h>
#include <foliage/model_dispatcher.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "nova_terrain_foliage_def.h"

namespace godot {

class Image;
class NovaTerrainData;

// Foliage adapter for the shared engine-spec placement/dispatcher core.
// Renders BOTH witnessed tiers (docs/foliage/foliage-re.md):
//
//   - FAR tier: up to 36 placements per cell, each receiving a complete copy
//     of the def source mesh at XZ scale 1/Y scale .5. Every transformed
//     source vertex samples terrain independently and carries source-Y wind
//     weight in red [orig: generate_foliage_instances_0 @ 0x5ffdd0].
//   - NEAR/MODEL tier: full 3DI geometry stamped in clusters around anchors
//     (the host equivalent of visible sector entities), via the libs
//     ModelDispatcher [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50;
//     Foliage_UpdateModelTiles @ 0x601f50;
//     Foliage_GenerateModelTileInstances @ 0x600980].
//
// FAR coverage:
//   - ENGINE_CENTERS gives each visible center independent per-slot
//     `opennova::foliage::Dispatcher` state and
//     `place_cell` for the original quadrant/LRU/stagger logic.
//   - CELL_GRID is a camera/editor coverage algorithm that scans a wider 16u
//     cell grid while still using the same per-cell placement kernel.
class NovaFoliageDispatcher : public Node3D {
	GDCLASS(NovaFoliageDispatcher, Node3D)

public:
	enum DispatchAlgorithm {
		DISPATCH_ALGORITHM_ENGINE_CENTERS = 0,
		DISPATCH_ALGORITHM_CELL_GRID = 1,
	};

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
// Runtime analogue: Foliage_SampleFoliageMapMask @ 0x606620.
	void set_foliage_sampler(const Callable &p_sampler);
	Callable get_foliage_sampler() const;

	// Callable receiving the retail surface-query boundary
	// (world_x: float, native_z: float) -> raw uint8 slot mask. FAR passes
	// native_z=-candidate_world_z exactly as witnessed; unlike MODEL's
	// foliage_sampler, this value is never translated through def.match.
	void set_surface_sampler(const Callable &p_sampler);
	Callable get_surface_sampler() const;

	// Direct runtime fast path. When set, runtime dispatch uses
// NovaTerrainData height, raw surface-mask, and foliage-map queries directly
// and skips Callable/Variant boxing. Editor leaves this unset so
	// the live-sculpt-aware Callable path still runs.
	void set_terrain_data(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_terrain_data() const;

	// Optional source for FAR pass T1 (the terrain light/colormap texture). The
	// editor uses it while placement remains on live-sculpt Callable samplers;
	// runtime normally obtains the same texture from terrain_data.
	void set_colormap_source(const Ref<NovaTerrainData> &p_data);
	// Legacy serialized compatibility. The former CPU terrain-tint payload was
	// disproven; retail overwrites FAR COLOR.r with source-Y wind weight.
	void set_terrain_tint(const Color &p_tint);
	Color get_terrain_tint() const;
	Ref<NovaTerrainData> get_colormap_source() const;

	// Configuration ---------------------------------------------------------

	// Coverage algorithm. ENGINE_CENTERS is the IDA-matched per-center path;
	// CELL_GRID scans a wider 16u cell grid around the supplied camera/preview center.
	void set_dispatch_algorithm(int p_algorithm);
	int get_dispatch_algorithm() const;

	// Cell-grid algorithm radius in 16u cells.
	void set_cell_grid_radius(int p_radius);
	int get_cell_grid_radius() const;

	// Compatibility alias for older editor code/scenes.
	void set_preview_cell_radius(int p_radius);
	int get_preview_cell_radius() const;

// Cell-grid cache capacity. ENGINE_CENTERS uses an independent engine-spec
// 128-entry LRU for each active (center, slot) pair.
	void set_lru_capacity(int p_capacity);
	int get_lru_capacity() const;

	// Legacy serialized compatibility; ignored by retail-shaped FAR emission.
	void set_quad_half_width(float p_width);
	float get_quad_half_width() const;

	// Legacy serialized compatibility; retail uses no extra terrain lift.
	void set_surface_offset(float p_offset);
	float get_surface_offset() const;

	void set_engine_view_radius_fixed(int p_radius);
	int get_engine_view_radius_fixed() const;

	// Main API --------------------------------------------------------------

	// Dispatch around `centre` using the configured coverage algorithm.
	// ENGINE_CENTERS uses `view_xform` to compute the 38.0 near-plane reject
	// from the original per-center path; identity transform skips that reject.
	// CELL_GRID ignores `view_xform` and scans a wider 16u cell grid.
	void dispatch(Vector3 centre, Transform3D view_xform = Transform3D());

	// Future visible-entity API: run ENGINE_CENTERS over all supplied centers
	// regardless of the configured single-center dispatch_algorithm.
	void dispatch_centers(PackedVector3Array centers, Transform3D view_xform = Transform3D());

	// Drop runtime/editor caches and MultiMesh state. Call on paint / def edits.
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

	// One Dictionary per retail-equivalent MODEL tile draw ({slot, tile_key,
	// anchor, view_depth, anchor_distance, alpha_ref, wind_counter, wind_phase,
	// instance_count}). A tile shared by two visible anchors appears twice:
	// retail submits it once per sector-entity walk and advances the wind
	// counter for every submission.
	Array get_model_draw_debug() const;

	// FAR test/debug introspection: one Dictionary per accepted placement
	// ({center, yaw}). The production batch mesh is inspectable through the
	// FoliageSlotN MultiMesh's single wrapped Mesh.
	Array get_far_tile_debug(int p_slot) const;

protected:
	static void _bind_methods();

private:
	// Cell key: pack (cell_x_int, cell_z_int, slot) into 64-bit. cell_x/z are
	// 16u-aligned world-unit ints; slot in [0, 3].
	struct CellKey {
		int32_t cell_x;
		int32_t cell_z;
		int slot;
		bool operator==(const CellKey &o) const noexcept {
			return cell_x == o.cell_x && cell_z == o.cell_z && slot == o.slot;
		}
	};
	struct CellKeyHash {
		size_t operator()(const CellKey &k) const noexcept {
			uint64_t h = static_cast<uint32_t>(k.cell_x);
			h = (h * 0x9E3779B185EBCA87ull) ^ static_cast<uint32_t>(k.cell_z);
			h = (h * 0xC2B2AE3D27D4EB4Full) ^ static_cast<uint32_t>(k.slot);
			return static_cast<size_t>(h);
		}
	};

	struct LRUEntry {
		std::vector<opennova::foliage::PlacementInstance> placements;
		int64_t touch = 0;
	};
	struct EngineCenterKey {
		opennova::foliage::Fixed16_16 x = 0;
		opennova::foliage::Fixed16_16 z = 0;
		bool operator==(const EngineCenterKey &o) const noexcept {
			return x == o.x && z == o.z;
		}
	};
	struct EngineCenterKeyHash {
		size_t operator()(const EngineCenterKey &k) const noexcept {
			uint64_t h = static_cast<uint32_t>(k.x);
			h = (h * 0x9E3779B185EBCA87ull) ^ static_cast<uint32_t>(k.z);
			return static_cast<size_t>(h);
		}
	};
	struct EngineCenterState {
		std::array<std::unique_ptr<opennova::foliage::Dispatcher>,
		           opennova::FOLIAGE_MAX_DEFS> dispatchers;

		opennova::foliage::Dispatcher &dispatcher_for_slot(int slot) {
			auto &dispatcher = dispatchers[slot];
			if (!dispatcher) {
				dispatcher = std::make_unique<opennova::foliage::Dispatcher>();
			}
			return *dispatcher;
		}

		int cached_cells() const noexcept {
			int total = 0;
			for (const auto &dispatcher : dispatchers) {
				if (dispatcher) {
					total += dispatcher->lru_occupancy();
				}
			}
			return total;
		}
	};
	struct DispatchStats {
		int64_t dispatch_calls = 0;
		int64_t coverage_skips = 0;
		int64_t rebuilt_slots = 0;
		int64_t instance_uploads = 0;
		int64_t cell_cache_hits = 0;
		int64_t cell_cache_misses = 0;
		// Model tier (last dispatch / accumulated in the libs dispatchers).
		int64_t model_anchors_in_range = 0;
		int64_t model_tiles_emitted = 0;
		int64_t model_instances = 0;
	};

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

	// A MODEL tile is a draw-state boundary, not merely an instance source.
	// Alpha ref is derived from the sector-entity anchor and the wind phase
	// counter advances once per tile submission, so tiles cannot be flattened
	// into a slot-wide MultiMesh without losing retail state.
	struct ModelDrawBatch {
		int slot = 0;
		uint32_t tile_key = 0;
		Vector3 anchor;
		float view_depth = 0.0f;
		float anchor_distance = 0.0f;
		float alpha_ref = 8.0f;  // D3D alpha-ref byte domain [8, 128]
		int64_t wind_counter = 0;
		float wind_phase = 0.0f; // counter * 0.001
		std::vector<opennova::foliage::ModelInstance> instances;
	};

	// CELL_GRID algorithm: cell (cell_x, cell_z, slot) -> placed instances.
	std::unordered_map<CellKey, LRUEntry, CellKeyHash> lru_;
	int64_t touch_counter_ = 0;
	DispatchStats dispatch_stats_;
	bool last_cell_grid_base_valid_ = false;
	int last_cell_grid_base_x_ = 0;
	int last_cell_grid_base_z_ = 0;

	// ENGINE_CENTERS algorithm: each visible center owns independent per-slot
	// retail Dispatcher state. NovaTerrain exposes at most its 256 patch-pool
	// centers; overflow callers are handled transiently so persistent residency
	// remains deterministic and bounded.
	static constexpr size_t ENGINE_CENTER_CACHE_CAPACITY = 256;
	std::unordered_map<EngineCenterKey, EngineCenterState, EngineCenterKeyHash>
	    engine_center_states_;
	std::array<std::vector<opennova::foliage::PlacementInstance>,
	           opennova::FOLIAGE_MAX_DEFS> engine_placements_{};
	int32_t engine_frame_counter_ = 0;

	// NEAR/MODEL tier: one shared-core model dispatcher per foliage slot.
	std::array<opennova::foliage::ModelDispatcher, opennova::FOLIAGE_MAX_DEFS> model_dispatchers_{};
	std::array<std::vector<opennova::foliage::ModelInstance>, opennova::FOLIAGE_MAX_DEFS>
	    model_instances_{};
	std::vector<ModelDrawBatch> model_draw_batches_;
	std::vector<MultiMeshInstance3D *> model_draw_nodes_;
	PackedVector3Array model_anchors_;
	float model_anchor_range_ = 512.0f;
	int32_t model_frame_counter_ = 0;
	// Retail increments this once per TILE DRAW
	// [orig: Foliage_ModelWindPhaseCounter @ 0x3162170, bumped in
	// Foliage_UploadModelTileVSConstants @ 0x600f00].
	int64_t model_wind_counter_ = 0;

	// FAR uses one dynamic full-mesh batch per slot. The MultiMesh wrapper has
	// one identity instance; its Mesh is the retail-shaped replicated vertex
	// buffer. MODEL children are held one per tile draw.
	MultiMeshInstance3D *mm_by_slot_[4] = {};
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
	// Retained for terrain/editor API compatibility. The disproven FAR
	// half-plus-bias terrain-color path no longer consumes this value.
	Color terrain_tint_ = Color(1.0f, 1.0f, 1.0f, 1.0f);
	int dispatch_algorithm_ = DISPATCH_ALGORITHM_ENGINE_CENTERS;
	int render_algorithm_ = DISPATCH_ALGORITHM_ENGINE_CENTERS;
	int cell_grid_radius_ = 8;
	int lru_capacity_ = 128;
	float quad_half_width_ = 2.0f;
	float surface_offset_ = 0.0f;
	int32_t engine_view_radius_fixed_ = 0x40000;

	bool mm_dirty_ = true;

	// Helpers ---------------------------------------------------------------

	bool _has_sampling_source() const;
	void _dispatch_engine_centers(const PackedVector3Array &centers,
	                              const Transform3D &view_xform);
	void _dispatch_cell_grid(Vector3 centre);
	void _dispatch_model_tier(const Transform3D &view_xform, const Dictionary &defs_by_match);
	void _rebuild_model_multimeshes();
	void _clear_model_draw_nodes();
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
	void _rebuild_multimeshes();
	void _update_slot_material(int slot_index);
	void _update_model_slot_material(int slot_index);
	bool _scatter_cell(int slot_index,
	                   int cell_x_int, int cell_z_int,
	                   const Ref<NovaTerrainFoliageDef> &def,
	                   std::vector<opennova::foliage::PlacementInstance> &out_placements);
	Dictionary _build_defs_by_match() const;
	void _retire_far_node(int slot_index);
	void _clear_children();
	void _invalidate_dispatch_coverage();
	Ref<Mesh> _build_far_mesh(
	    int slot_index,
	    const std::vector<opennova::foliage::PlacementInstance> &placements) const;
};

} // namespace godot
