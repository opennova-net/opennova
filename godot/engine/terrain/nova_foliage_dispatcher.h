#pragma once

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <foliage/dispatcher.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "nova_terrain_foliage_def.h"

namespace godot {

class NovaTerrainData;

// Foliage adapter for the shared engine-spec placement/dispatcher core.
//
// Engine provenance:
//   - Jointops.exe terrain_update_foliage_tiles@0x601F50 (visible-center dispatcher)
//   - Jointops.exe generate_foliage_instances@0x600980 (per-cell placement)
//   - Jointops.exe sub_606620@0x606620 (foliage map sampler)
//
// Fidelity:
//   - ENGINE_CENTERS uses shared `opennova::foliage::Dispatcher` instances and
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

	// Per-slot Mesh to use for MultiMesh rendering. Parallel array to
	// foliage_defs. Null / missing entries fall back to a BoxMesh placeholder
	// so the slot is still visible. Mesh-only swap; does not invalidate the
	// shared placement caches.
	void set_slot_meshes(const Array &p_meshes);
	Array get_slot_meshes() const;

	// Callable receiving (world_x: float, world_z: float) -> height: float.
	// Return value <= -1e6 indicates "no terrain here; skip candidate".
	// Runtime analogue: Terrain_SampleHeightBilinear@0x5C6770.
	void set_height_sampler(const Callable &p_sampler);
	Callable get_height_sampler() const;

	// Callable receiving (world_x: float, world_z: float) -> int. Returns the
	// foliage-map palette index at that world position (0 means empty).
	// Runtime analogue: Terrain_GetFoliageMapValue@0x5C65E0.
	void set_foliage_sampler(const Callable &p_sampler);
	Callable get_foliage_sampler() const;

	// Direct runtime fast path. When set, runtime dispatch uses
	// NovaTerrainData::get_height_world_bilinear / get_foliage_index_world
	// directly and skips Callable/Variant boxing. Editor leaves this unset so
	// the live-sculpt-aware Callable path still runs.
	void set_terrain_data(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_terrain_data() const;

	// Optional colormap-only source for ground-color tinting. The editor preview
	// sets this (its NovaTerrainData) so foliage is tinted from the colormap while
	// placement keeps using the live-sculpt Callable samplers (terrain_data left
	// unset). Runtime ignores it because terrain_data already supplies the colormap.
	void set_colormap_source(const Ref<NovaTerrainData> &p_data);
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

	// Cell-grid cache capacity. ENGINE_CENTERS uses the shared engine-spec
	// 128-entry LRU inside each slot dispatcher.
	void set_lru_capacity(int p_capacity);
	int get_lru_capacity() const;

	void set_quad_half_width(float p_width);
	float get_quad_half_width() const;

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
		std::vector<Transform3D> transforms;
		std::vector<Color> colors;
		int64_t touch = 0;
	};

	// CELL_GRID algorithm: cell (cell_x, cell_z, slot) -> placed instances.
	std::unordered_map<CellKey, LRUEntry, CellKeyHash> lru_;
	int64_t touch_counter_ = 0;

	// ENGINE_CENTERS algorithm: one shared-core dispatcher per foliage slot.
	std::array<opennova::foliage::Dispatcher, opennova::FOLIAGE_MAX_DEFS> engine_dispatchers_{};
	std::array<std::vector<Transform3D>, opennova::FOLIAGE_MAX_DEFS> engine_transforms_{};
	std::array<std::vector<Color>, opennova::FOLIAGE_MAX_DEFS> engine_colors_{};
	int32_t engine_frame_counter_ = 0;

	// Per-slot rendered children (one MultiMeshInstance3D per def slot).
	MultiMeshInstance3D *mm_by_slot_[4] = {};
	Ref<Shader> foliage_shader_;
	Ref<ShaderMaterial> foliage_materials_[4];

	// Configuration
	Array foliage_defs_;
	Array slot_meshes_;
	Callable height_sampler_;
	Callable foliage_sampler_;
	Ref<NovaTerrainData> terrain_data_;
	Ref<NovaTerrainData> colormap_source_;
	int dispatch_algorithm_ = DISPATCH_ALGORITHM_ENGINE_CENTERS;
	int render_algorithm_ = DISPATCH_ALGORITHM_ENGINE_CENTERS;
	int cell_grid_radius_ = 8;
	int lru_capacity_ = 128;
	float quad_half_width_ = 2.0f;
	float surface_offset_ = 0.05f;
	int32_t engine_view_radius_fixed_ = 0x40000;

	bool mm_dirty_ = true;

	// Helpers ---------------------------------------------------------------

	bool _has_sampling_source() const;
	void _dispatch_engine_centers(const PackedVector3Array &centers,
	                              const Transform3D &view_xform,
	                              const Dictionary &defs_by_match);
	void _dispatch_cell_grid(Vector3 centre, const Dictionary &defs_by_match);
	void _rebuild_multimeshes();
	void _update_slot_material(int slot_index, const Ref<Mesh> &slot_mesh);
	Color _sample_ground_color(const opennova::foliage::PlacementInstance &inst,
	                           float quad_half_width) const;
	bool _scatter_cell(int slot_index,
	                   int cell_x_int, int cell_z_int,
	                   const Ref<NovaTerrainFoliageDef> &def,
	                   const Dictionary &defs_by_match,
	                   std::vector<Transform3D> &out_transforms,
	                   std::vector<Color> &out_colors);
	void _append_render_instance(const opennova::foliage::PlacementInstance &inst,
	                             float quad_half_width,
	                             std::vector<Transform3D> &out_transforms,
	                             std::vector<Color> &out_colors) const;
	float _sample_height(float world_x, float world_z) const;
	Dictionary _build_defs_by_match() const;
	void _clear_children();
	Ref<Mesh> _fallback_mesh() const;
};

} // namespace godot
