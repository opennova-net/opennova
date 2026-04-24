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

// Runtime foliage adapter for the shared engine-spec placement/dispatcher core.
//
// Engine provenance:
//   - Jointops.exe Foliage_RenderAtPosition@0x5C1940 (per-slot dispatcher)
//   - Jointops.exe Foliage_BuildPatchData@0x5C0240 (per-cell placement)
//   - docs/engine_spec_foliage.md 4.3-4.4
//
// Fidelity:
//   - Runtime path uses four shared `opennova::foliage::Dispatcher` instances
//     and `place_cell` for the original quadrant/LRU/stagger logic.
//   - Editor preview keeps a wider cell-grid path as an authoring-only extension
//     because the original visible-entity driver does not exist in the editor.
//   - Runtime still dispatches around a single supplied centre until a true
//     visible-entity list exists, so that mode is closer to the engine but not
//     yet byte-for-byte identical.
class NovaFoliageDispatcher : public Node3D {
	GDCLASS(NovaFoliageDispatcher, Node3D)

public:
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

	// Configuration ---------------------------------------------------------

	// Editor-only extension: preview grid radius in 16u cells.
	void set_preview_cell_radius(int p_radius);
	int get_preview_cell_radius() const;

	// Editor-only extension: preview-grid cache capacity. Runtime uses the
	// shared engine-spec 128-entry LRU inside each slot dispatcher.
	void set_lru_capacity(int p_capacity);
	int get_lru_capacity() const;

	void set_quad_half_width(float p_width);
	float get_quad_half_width() const;

	void set_surface_offset(float p_offset);
	float get_surface_offset() const;

	// Main API --------------------------------------------------------------

	// Runtime mode: dispatch shared-core foliage around `centre`, using
	// `view_xform` (world → view) to compute the 38.0 near-plane reject the
	// engine performs via Math_TransformPoint(view_matrix, center). Identity
	// transform (default arg) skips the reject — editor / test callers that
	// don't have a live camera transform rely on that.
	// Editor mode: `view_xform` is ignored; preview scans a wider cell grid
	// around `centre`.
	void dispatch(Vector3 centre, Transform3D view_xform = Transform3D());

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

	// Editor preview extension: cell (cell_x, cell_z, slot) -> placed instances.
	std::unordered_map<CellKey, LRUEntry, CellKeyHash> lru_;
	int64_t touch_counter_ = 0;

	// Runtime shared-core dispatchers, one per foliage slot.
	std::array<opennova::foliage::Dispatcher, opennova::FOLIAGE_MAX_DEFS> runtime_dispatchers_{};
	std::array<std::vector<Transform3D>, opennova::FOLIAGE_MAX_DEFS> runtime_transforms_{};
	std::array<std::vector<Color>, opennova::FOLIAGE_MAX_DEFS> runtime_colors_{};
	int32_t runtime_frame_counter_ = 0;

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
	int preview_cell_radius_ = 8;
	int lru_capacity_ = 128;
	float quad_half_width_ = 2.0f;
	float surface_offset_ = 0.05f;

	bool mm_dirty_ = true;

	// Helpers ---------------------------------------------------------------

	bool _is_runtime_dispatch_mode() const;
	void _dispatch_runtime(Vector3 centre, const Transform3D &view_xform, const Dictionary &defs_by_match);
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
