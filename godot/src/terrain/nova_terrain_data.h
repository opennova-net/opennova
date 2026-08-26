#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cpt/cpt_io.h>
#include <terrain_query/coords.h>
#include <terrain_query/height_field.h>
#include <trn/trn_io.h>

#include "resource_index/nova_resource_root.h"

#include <cstdint>
#include <string>
#include <vector>

namespace godot {

class TerrainFoliageDef;
class TerrainFoliageMap;
class TerrainTileInfo;

// The .trn's four lock_* pairs as the portable neighbour-tap policy. One converter
// for every heightmap tap in the runtime: the height samplers (via the height field),
// the render mesh, and the collision heightfield — so no site can silently keep the
// old unconditional full-atlas wrap.
opennova::terrain::CoordsQuadrantLocks coords_locks_from(const opennova::TrnConfig &trn);

// Everything a TerrainHeightField takes from the TRN: the sector origins and the
// neighbour-tap locks. The heightmap and sector-grid POINTERS stay the caller's
// (Simulation owns copies that outlive the resource), but the derived members
// live here so a second field builder cannot silently miss one — which is exactly
// how the sim's grounding field kept the pre-lock full-atlas wrap, and the player
// kept falling through ground the mesh drew as solid.
void height_field_apply_trn(opennova::terrain::TerrainHeightField &field,
                            const opennova::TrnConfig &trn);

class TerrainData : public Resource {
	GDCLASS(TerrainData, Resource)

private:
	String trn_path;

	// Identity
	String terrain_name;

	// Texture resources (displayed as drag-and-drop in inspector)
	Ref<Texture2D> colormap;
	Ref<Texture2D> detailmap;
	Ref<Texture2D> detailmap_c1;
	Ref<Texture2D> detailmap_c2;
	Ref<Texture2D> detailmap_c3;
	Ref<Texture2D> detailmap2;
	Ref<Texture2D> detailmapdist;
	Ref<Texture2D> detailmapdist2;
	Ref<Texture2D> detailblendmap;
	Ref<Texture2D> charmap_tex;
	Ref<Texture2D> foliagemap_tex;
	Ref<Texture2D> tilestrip_tex;
	mutable Ref<Image> colormap_cpu_image;
	mutable int colormap_cpu_width = 0;
	mutable int colormap_cpu_height = 0;

	// Numeric/bool properties
	int detail_density = 128;
	int detail_density2 = 8;
	int sector_count = 0;
	int sector_rows = 0;
	int origin_x = 0;
	int origin_y = 0;
	int water_height = 0;
	bool wrap_x = false;
	bool wrap_y = false;
	double horizon = 0.0;
	PackedInt32Array sector_grid;

	// Parsed data (not exposed)
	opennova::CptFile cpt;
	opennova::TrnConfig trn;
	bool loaded = false;
	// Monotonic identity for every terrain_changed publication. Zero is kept
	// as the never-observed sentinel used by dependent page caches.
	uint64_t change_revision_ = 0;
	Ref<ResourceRoot> resource_root;

	// Palette + indices for PCX-backed runtime map data.
	std::vector<uint8_t> charmap_indices;
	uint8_t charmap_palette[256][3] = {};
	int charmap_width = 0;
	int charmap_height = 0;
	std::vector<uint8_t> foliagemap_indices;
	uint8_t foliagemap_palette[256][3] = {};
	int foliagemap_width = 0;
	int foliagemap_height = 0;
	Ref<TerrainFoliageMap> foliage_map_resource;
	// Optional live source images retained for runtime surface-input consumers.
	Ref<Image> heightmap_image;
	Ref<Image> colormap_image;
	Ref<Image> blendmap_image;
	// Lazy-loaded on first call to get_tileinfo_resource(). Cached keyed by
	// the source path so an edit to trn.tileinfo re-loads on next request.
	mutable Ref<TerrainTileInfo> tileinfo_resource_cache;
	mutable String tileinfo_resource_cache_path;

	void _sync_trn_scalars_from_properties();
	void _sync_trn_texture_filenames_from_refs();
	void _notify_terrain_changed();
	void _sync_foliage_map_resource_from_slot();
	void _invalidate_colormap_cpu_cache() const;
	bool _ensure_colormap_cpu_cache() const;

	// Extract bare filename from a Texture2D's resource path
	static String _texture_to_filename(const Ref<Texture2D> &p_tex);

	struct PcxSlotRefs;
	bool _resolve_pcx_slot(const String &slot_id, PcxSlotRefs &out);
	Error _import_pcx_slot_bytes(const String &slot_id, const String &filename, const PackedByteArray &bytes);
	Error _load_from_trn_text(const std::string &trn_content, const String &source_label);

protected:
	static void _bind_methods();

public:
	// Sector/atlas layout constants, single-sourced from engine/runtime/terrain_query
	// (terrain_query/coords.h) and bound to GDScript so editor scripts reference the
	// engine's numbers instead of re-declaring them: SECTOR_SIZE world units per
	// sector edge, the SECTOR_GRID_DIM x SECTOR_GRID_DIM authored grid, the
	// ATLAS_SIZE source atlas (a 2x2 quadrant grid of sectors), and sector ids
	// 0 (empty) .. SECTOR_ID_MAX (the four quadrants).
	static constexpr int SECTOR_SIZE = opennova::terrain::COORDS_SECTOR_SIZE;
	static constexpr int SECTOR_GRID_DIM = opennova::terrain::COORDS_SECTOR_GRID_DIM;
	static constexpr int ATLAS_SIZE = opennova::terrain::COORDS_ATLAS_SIZE;
	static constexpr int SECTOR_ID_MAX = opennova::terrain::COORDS_SECTOR_ID_MAX;

	TerrainData();
	~TerrainData();

	void set_trn_path(const String &p_path);
	String get_trn_path() const;

	void set_terrain_name(const String &p_name);
	String get_terrain_name() const;

	// Texture accessors
	void set_colormap(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_colormap() const;
	void set_detailmap(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmap() const;
	void set_detailmap_c1(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmap_c1() const;
	void set_detailmap_c2(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmap_c2() const;
	void set_detailmap_c3(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmap_c3() const;
	void set_detailmap2(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmap2() const;
	void set_detailmapdist(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmapdist() const;
	void set_detailmapdist2(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailmapdist2() const;
	void set_detailblendmap(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_detailblendmap() const;
	void set_charmap_tex(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_charmap_tex() const;
	void set_foliagemap_tex(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_foliagemap_tex() const;
	void set_tilestrip_tex(const Ref<Texture2D> &p_tex);
	Ref<Texture2D> get_tilestrip_tex() const;
	// C++ runtime auxiliary-texture seam: resolves through the same mounted VFS
	// (or loose TRN directory) as authored terrain slots without exposing the
	// ResourceRoot or a second lookup policy.
	Ref<Texture2D> load_source_texture(const String &p_filename) const;

	// Numeric accessors
	void set_detail_density(int p_val);
	int get_detail_density() const;
	void set_detail_density2(int p_val);
	int get_detail_density2() const;
	void set_sector_count(int p_val);
	int get_sector_count() const;
	void set_sector_rows(int p_val);
	int get_sector_rows() const;
	void set_origin_x(int p_val);
	int get_origin_x() const;
	void set_origin_y(int p_val);
	int get_origin_y() const;
	void set_water_height(int p_val);
	int get_water_height() const;
	void set_wrap_x(bool p_val);
	bool get_wrap_x() const;
	void set_wrap_y(bool p_val);
	bool get_wrap_y() const;
	PackedInt32Array get_quadrant_locks() const;
	void set_horizon(double p_val);
	double get_horizon() const;

	Error load();
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	uint64_t get_change_revision() const { return change_revision_; }
	// Returns depth as little-endian raw16 (value = clamp(height*256)). Reads an
	// optional live heightmap image when present, else the loaded CPT.
	PackedByteArray get_depth_raw16() const;
	Ref<Image> get_heightmap_image() const;
	Ref<Image> get_colormap_image() const;
	Ref<Image> get_blendmap_image() const;
	float get_height(const Vector3 &p_world_pos) const;
	float get_height_world(const Vector3 &p_world_pos) const;
	float get_height_world_bilinear(const Vector3 &p_world_pos) const;
	// Godot-space form of the portable terrain-normal query; this device
	// declaration adds no runtime behavior of its own.
	// [orig: WacScript_SpawnEffectAtSsnEntity @0x4F23A0 -> terrain normal
	// tables generated by Terrain_GenerateNormalMap @0x603210]
	Vector3 get_surface_normal_world(const Vector3 &p_world_pos) const;
	Color get_colormap_color_world(float world_x, float world_z) const;
	// Exact gameplay-map shore mask. Retail builds a 256x256 `depthspin`
	// texture by reducing the raw 1024x1024 CPT heights 4:1 (four taps,
	// sum >> 10), then linearly samples it and alpha-tests the UNORM8 result
	// against the integer water plane at raster time. This equivalent keeps
	// BOTH operands in an RG8 data texture (R = reduced terrain height,
	// G = integer water plane); the HUD's map-water shader performs the same
	// sample-then-quantize comparison, so shoreline texels resolve exactly —
	// a pre-thresholded binary mask is NOT equivalent there.
	Ref<ImageTexture> build_minimap_water_mask(
			float p_water_height_wu = NAN) const;
	Color get_modulated_colormap_color_world(float world_x, float world_z, const Color &light_color) const;
	// Detail grass reads the flat, 1024-wrapped foliagemap. The fixed overload
	// keeps runtime candidates on their original Q16 coordinates; the world
	// overload is the public/GDScript seam.
	int get_detail_foliage_index_fixed(int32_t world_x_fixed, int32_t world_z_fixed) const;
	int get_detail_foliage_index_world(double world_x, double world_z) const;
	// MODEL masks and gameplay queries use the sector-grid-routed foliagemap.
	int get_foliage_index_world(float world_x, float world_z) const;
	// Runtime world->atlas transform: wraps the 16x16 sector grid and preserves
	// raw sector ids/local offsets like the retail terrain samplers.
	Vector2 world_to_runtime_source_coords(float world_x, float world_z) const;
	// Segment raycast against the terrain surface — the ENG-3 B1 port
	// (engine/runtime/terrain_query/terrain_raycast.h); witness record
	// docs/terrain/terrain-re.md §Runtime terrain queries. Returns the refined
	// world-space hit (x, height, z), or Vector3(NAN, NAN, NAN) when the segment
	// is clear or no terrain data is mounted. Samples an optional live heightmap
	// when mounted, else the baked CPT heights; the working segment
	// is reimpl-clipped to the authored-extent XZ AABB before entering the 16.16
	// fixed-point core.
	Vector3 raycast_terrain(const Vector3 &p_from, const Vector3 &p_to) const;
	int get_tile_count() const;

	// GDScript-facing accessors for foliage + sector grid
	Dictionary load_foliage_indices() const;
	void set_sector_grid(const PackedInt32Array &p_grid);
	PackedInt32Array get_sector_grid() const;
	Ref<TerrainFoliageMap> get_foliage_map() const;
	Array get_foliage_defs() const;
	void set_foliage_defs(const Array &p_defs);
	void set_trn_texture_filename(const String &slot_id, const String &filename);
	String get_trn_texture_filename(const String &slot_id) const;
	void set_polydata_filename(const String &filename);
	String get_polydata_filename() const;
	void set_tileinfo_filename(const String &filename);
	String get_tileinfo_filename() const;
	// Lazy-load the .til referenced by trn.tileinfo, resolved relative to
	// trn_path's directory. Cached across calls; invalidated on filename
	// change. Returns a null Ref if the file is missing or unparseable.
	Ref<TerrainTileInfo> get_tileinfo_resource() const;

	// The .trn text write (ADR 0032 direct document I/O): engine save_trn
	// emits the config from scratch; replaces the deleted ResourceFormat pair.
	Error save_to_path(const String &p_path) const;

	const opennova::CptFile& get_cpt() const { return cpt; }
	const opennova::TrnConfig& get_trn() const { return trn; }
	// C++-side raster access for the sim's cited portable surface-type sampler;
	// the charmap indices are the engine's surface classes.
	const std::vector<uint8_t> &get_charmap_indices() const { return charmap_indices; }
	int get_charmap_width() const { return charmap_width; }
	int get_charmap_height() const { return charmap_height; }
};

} // namespace godot
