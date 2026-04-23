#pragma once

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include "nova_terrain_tile_info.h"

namespace godot {

class NovaTerrainTileEntry;
class NovaTerrainData;

// Shared editor+runtime tile-overlay renderer. Bakes NovaTerrainTileInfo
// entries into an ArrayMesh of textured 16x16 world-unit quads sampled against
// the terrain surface via `height_sampler` (Callable: (world_x, world_z) -> y).
//
// Engine provenance:
//   - jodemo.exe Terrain_DrawTileOverlays2D@0x5C79C0
//   - jodemo.exe sub_5C42B0@0x5C42B0
//   - docs/engine_spec_tiles.md 4.2-4.5
//
// Fidelity:
//   - Atlas UV transforms and fixed->world placement use shared libs/til helpers.
//   - The 3D ground-quad renderer remains an extension until the original in-world
//     tile path is isolated end to end.
//   - Outline flag (0x08) is NOT rendered in the engine's traced 3D path; runtime
//     keeps it disabled by default and the editor enables it for authoring feedback.
//
// The editor preview (TerrainTileOverlayPreview.gd) still owns ghost / hover /
// selection / selection-outline layers - those are authoring-only.
class NovaTerrainTileOverlay : public Node3D {
	GDCLASS(NovaTerrainTileOverlay, Node3D)

public:
	NovaTerrainTileOverlay();
	~NovaTerrainTileOverlay();

	void set_tile_info(const Ref<NovaTerrainTileInfo> &p_info);
	Ref<NovaTerrainTileInfo> get_tile_info() const;

	void set_tilestrip(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_tilestrip() const;

	// (world_x: float, world_z: float) -> float. Return value <= -1e6 indicates
	// "no terrain here"; the overlay treats it as 0.0. Mirrors the foliage
	// dispatcher's sampler convention.
	void set_height_sampler(const Callable &p_sampler);
	Callable get_height_sampler() const;

	// Direct runtime fast-path. When set, the overlay samples terrain heights
	// via NovaTerrainData::get_height_world_bilinear and skips the
	// Callable/Variant round-trip on every quad corner. Editor keeps this
	// unset so its live-sculpt-aware Callable still runs. Mirrors
	// NovaFoliageDispatcher::set_terrain_data.
	void set_terrain_data(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_terrain_data() const;

	void set_surface_offset(float p_offset);
	float get_surface_offset() const;

	void set_draw_outline_flag(bool p_enabled);
	bool get_draw_outline_flag() const;

	// Build / rebuild the overlay mesh from the current tile_info. Safe to call
	// repeatedly - existing child MeshInstance3Ds are reused.
	void rebuild();

	// Shared tile-atlas UV helper used by runtime C++ and editor authoring-only
	// ghost tiles. Returns TL, TR, BL, BR UVs or an empty array on invalid atlas.
	PackedVector2Array build_entry_uvs(const Ref<NovaTerrainTileEntry> &entry,
	                                  int atlas_width,
	                                  int atlas_height) const;

	// Clear both overlay and outline meshes.
	void clear();

	int get_entry_count_rendered() const;

protected:
	static void _bind_methods();

private:
	Ref<NovaTerrainTileInfo> tile_info_;
	Ref<Texture2D> tilestrip_;
	Callable height_sampler_;
	Ref<NovaTerrainData> terrain_data_;
	float surface_offset_ = 0.08f;
	bool draw_outline_flag_ = false;

	MeshInstance3D *overlay_instance_ = nullptr;
	MeshInstance3D *outline_instance_ = nullptr;
	Ref<StandardMaterial3D> overlay_material_;
	Ref<StandardMaterial3D> outline_material_;

	int entries_rendered_ = 0;

	void _ensure_children();
	float _sample_height(float world_x, float world_z) const;
};

} // namespace godot
