#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cpt/cpt_io.h>
#include <terrain/coords.h>
#include <terrain/height_field.h>
#include <trn/trn_io.h>

#include "resource_index/nova_resource_root.h"

#include <cstdint>
#include <string>
#include <vector>

namespace godot {

class NovaTerrainFoliageDef;
class NovaTerrainFoliageMap;
class NovaTerrainTileInfo;

// The .trn's four lock_* pairs as the portable neighbour-tap policy. One converter
// for every heightmap tap in the runtime: the height samplers (via the height field),
// the render mesh, and the collision heightfield — so no site can silently keep the
// old unconditional full-atlas wrap.
opennova::terrain::CoordsQuadrantLocks coords_locks_from(const opennova::TrnConfig &trn);

// Everything a TerrainHeightField takes from the TRN: the sector origins and the
// neighbour-tap locks. The heightmap and sector-grid POINTERS stay the caller's
// (NovaSimulation owns copies that outlive the resource), but the derived members
// live here so a second field builder cannot silently miss one — which is exactly
// how the sim's grounding field kept the pre-lock full-atlas wrap, and the player
// kept falling through ground the mesh drew as solid.
void height_field_apply_trn(opennova::terrain::TerrainHeightField &field,
                            const opennova::TrnConfig &trn);

class NovaTerrainData : public Resource {
	GDCLASS(NovaTerrainData, Resource)

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
	Ref<NovaResourceRoot> resource_root;

	// Palette + indices for PCX-backed map data (hidden from GDScript;
	// exposed only via high-level import/save/reset methods).
	std::vector<uint8_t> charmap_indices;
	uint8_t charmap_palette[256][3] = {};
	int charmap_width = 0;
	int charmap_height = 0;
	std::vector<uint8_t> foliagemap_indices;
	uint8_t foliagemap_palette[256][3] = {};
	int foliagemap_width = 0;
	int foliagemap_height = 0;
	Ref<NovaTerrainFoliageMap> foliage_map_resource;
	// Editable FORMAT_RF height buffer (1 float per cell = raw16_units / 256).
	// The editor holds this same Image by reference and brush kernels mutate it
	// in place, so NovaTerrainData stays the authoritative owner of the live
	// depth and get_depth_raw16() reflects edits without a separate sync step.
	Ref<Image> heightmap_image;
	// Editable colormap / detail-blend buffers, owned and shared by reference
	// with the editor exactly like heightmap_image. Distinct from `colormap`
	// (the derived display/runtime Texture2D) and `colormap_cpu_image` (a lazy
	// read cache); these are the paintable source-of-truth images that save
	// reads back from.
	Ref<Image> colormap_image;
	Ref<Image> blendmap_image;
	// Lazy-loaded on first call to get_tileinfo_resource(). Cached keyed by
	// the source path so an edit to trn.tileinfo re-loads on next request.
	mutable Ref<NovaTerrainTileInfo> tileinfo_resource_cache;
	mutable String tileinfo_resource_cache_path;

	void _sync_trn_scalars_from_properties();
	void _sync_trn_texture_filenames_from_refs();
	void _notify_terrain_changed();
	void _sync_foliage_map_resource_from_slot();
	void _apply_foliage_map_to_slot(const opennova::FoliageMap &map);
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
	// Sector/atlas layout constants, single-sourced from libs/terrain_query
	// (terrain/coords.h) and bound to GDScript so editor scripts reference the
	// engine's numbers instead of re-declaring them: SECTOR_SIZE world units per
	// sector edge, the SECTOR_GRID_DIM x SECTOR_GRID_DIM authored grid, the
	// ATLAS_SIZE source atlas (a 2x2 quadrant grid of sectors), and sector ids
	// 0 (empty) .. SECTOR_ID_MAX (the four quadrants).
	static constexpr int SECTOR_SIZE = opennova::terrain::COORDS_SECTOR_SIZE;
	static constexpr int SECTOR_GRID_DIM = opennova::terrain::COORDS_SECTOR_GRID_DIM;
	static constexpr int ATLAS_SIZE = opennova::terrain::COORDS_ATLAS_SIZE;
	static constexpr int SECTOR_ID_MAX = opennova::terrain::COORDS_SECTOR_ID_MAX;

	NovaTerrainData();
	~NovaTerrainData();

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
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	// Returns the live depth as little-endian raw16 (value = clamp(height*256)).
	// Reads the editable heightmap_image when present, else the loaded CPT.
	PackedByteArray get_depth_raw16() const;
	void set_heightmap_image(const Ref<Image> &p_image);
	Ref<Image> get_heightmap_image() const;
	// Build a FORMAT_RF height image (height = raw_u16_LE / 256) from a raw16
	// depth buffer; the exact inverse of get_depth_raw16's conversion.
	Ref<Image> heightmap_image_from_raw16(const PackedByteArray &p_raw16) const;
	void set_colormap_image(const Ref<Image> &p_image);
	Ref<Image> get_colormap_image() const;
	void set_blendmap_image(const Ref<Image> &p_image);
	Ref<Image> get_blendmap_image() const;
	// CDEP per-block range enforcement on the editable heightmap (raw16 domain;
	// see libs/terrain/cdep_constraint.h). These mutate the FORMAT_RF
	// heightmap_image in place via get_data()/set_data(), so the editor's shared
	// Image ref and get_depth_raw16() stay current without a separate sync.
	// clamp_* return the number of blocks clamped; count returns the number of
	// over-range blocks.
	int cdep_clamp_blocks_in_rect(const Rect2i &p_rect);
	int cdep_count_violations() const;
	int cdep_clamp_all_violations();
	// Height brushes (libs/terrain/brush.h). Mutate the editable FORMAT_RF
	// heightmap in place (the editor's shared Image); cx/cz/radius/clip are atlas
	// pixel coordinates. Math runs in double to match the GDScript originals.
	void brush_raise_lower(int cx, int cz, int radius, double amount, double hardness, const Rect2i &p_clip);
	void brush_smooth(int cx, int cz, int radius, double strength, double hardness, const Rect2i &p_clip);
	void brush_flatten(int cx, int cz, int radius, double target_height, double strength, double hardness, const Rect2i &p_clip);
	double brush_sample_flatten_target(double world_x, double world_z) const;
	// Colour / blend brushes (byte-parity RGBA8). blend paints the editable
	// detail-blend buffer; colormap paint/clone the editable colour buffer.
	void brush_blend_paint(int channel, int cx, int cz, int radius, double strength, double hardness, const Rect2i &p_clip);
	void brush_colormap_paint(const Color &color, int cx, int cz, int radius, double strength, double hardness, const Rect2i &p_clip);
	void brush_colormap_clone(const Ref<Image> &source, int src_cx, int src_cy, int dst_cx, int dst_cy, int radius, double strength, double hardness, const Rect2i &p_clip);
	Color brush_sample_colormap(double world_x, double world_z) const;
	float get_height(const Vector3 &p_world_pos) const;
	float get_height_world(const Vector3 &p_world_pos) const;
	float get_height_world_bilinear(const Vector3 &p_world_pos) const;
	Color get_colormap_color_world(float world_x, float world_z) const;
	Color get_modulated_colormap_color_world(float world_x, float world_z, const Color &light_color) const;
	// Detail grass reads the flat, 1024-wrapped foliagemap. The fixed overload
	// keeps runtime candidates on their original Q16 coordinates; the world
	// overload is the public/GDScript seam.
	int get_detail_foliage_index_fixed(int32_t world_x_fixed, int32_t world_z_fixed) const;
	int get_detail_foliage_index_world(double world_x, double world_z) const;
	// MODEL masks and gameplay queries use the sector-grid-routed foliagemap.
	int get_foliage_index_world(float world_x, float world_z) const;
	// Editor world->atlas coordinate transforms (libs/terrain_query/coords.h). The
	// editor-mode guards (bounds-reject, sector-id clamp to [0,4], local clamp to
	// [0, 512-0.001]) reproduce EditorTerrainMesh's GDScript originals so the
	// editor brush/eyedropper paths share the runtime sampler's implementation.
	// world_to_source_coords returns Vector2(-1,-1) and world_to_cell_source_coords
	// returns Vector2(-1e9,-1e9) for out-of-extent / empty cells (the sentinels the
	// GDScript callers branch on); get_cell_atlas_rect returns a zero Rect2i.
	Vector2 world_to_source_coords(double world_x, double world_z) const;
	// Runtime world->atlas transform: wraps the 16x16 sector grid and preserves
	// raw sector ids/local offsets like the retail terrain samplers.
	Vector2 world_to_runtime_source_coords(float world_x, float world_z) const;
	Vector2 world_to_cell_source_coords(double world_x, double world_z, int row, int col) const;
	// World -> authored sector-grid cell (row, col), or (-1,-1) outside the
	// authored extent. Mirrors EditorTerrainMesh's extent-guarded cell lookup:
	// floor(world / SECTOR_SIZE) minus the origin, with NO grid-value check (an
	// empty cell still reports its row/col).
	Vector2i world_to_sector_cell(double world_x, double world_z) const;
	// Batch bilinear height sample of the LIVE editable heightmap (the FORMAT_RF
	// Image the brushes mutate in place) — NOT the baked CPT, which height edits
	// leave stale (see get_height_world_bilinear vs terrain_editor.
	// sample_height_world). One float per input (world_x, world_z) pair; NAN when
	// the point is off the active sectors or no editable image is mounted. The
	// per-point core is shared with sample_height_world_live (editor-mode remap,
	// float32 source coords, edge-clamped bilinear) so the scalar and batch
	// samplers can never disagree; the sector layout is built once for the whole
	// batch.
	PackedFloat32Array sample_heights_world_live(const PackedVector2Array &world_xz) const;
	// Scalar twin of sample_heights_world_live: one point through the same
	// per-point core, same NAN semantics. EditorTerrainMesh.sample_world_height
	// forwards here (it translates NAN to its legacy -1e6 sentinel).
	float sample_height_world_live(double world_x, double world_z) const;
	// Segment raycast against the terrain surface — the ENG-3 B1 port
	// (libs/terrain_query/terrain_raycast.h) [orig: Terrain_RaycastHeightmapLoRes
	// @ 0x60cb80; Terrain_RaycastHeightmapHiRes_0 @ 0x60e710]; witness record
	// docs/terrain/terrain-re.md §Runtime terrain queries. Returns the refined
	// world-space hit (x, height, z), or Vector3(NAN, NAN, NAN) when the segment
	// is clear or no terrain data is mounted. Samples the LIVE editable heightmap
	// when mounted, else the baked CPT heights (the same substrate split as
	// sample_height_world_live vs get_height_world_bilinear); the working segment
	// is reimpl-clipped to the authored-extent XZ AABB before entering the 16.16
	// fixed-point core.
	Vector3 raycast_terrain(const Vector3 &p_from, const Vector3 &p_to) const;
	Rect2i get_cell_atlas_rect(int row, int col) const;
	int get_tile_count() const;

	// GDScript-facing accessors for foliage + sector grid
	Dictionary load_foliage_indices() const;
	void set_sector_grid(const PackedInt32Array &p_grid);
	PackedInt32Array get_sector_grid() const;
	Ref<NovaTerrainFoliageMap> get_foliage_map() const;
	void set_foliage_map(const Ref<NovaTerrainFoliageMap> &p_map);
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
	Ref<NovaTerrainTileInfo> get_tileinfo_resource() const;

	// Generic PCX slot API. Palette+indices are owned internally; slot_id
	// picks which slot to operate on ("charmap" or "foliagemap" today).
	// Each call rebuilds the corresponding Texture2D preview cache.
	Error import_pcx_slot(const String &slot_id, const String &path);
	Error save_pcx_slot(const String &slot_id, const String &path) const;
	void reset_pcx_slot_default(const String &slot_id, int width, int height);
	Dictionary get_pcx_slot_state(const String &slot_id) const;
	void set_pcx_slot_state(const String &slot_id, const Dictionary &state);

	const opennova::CptFile& get_cpt() const { return cpt; }
	const opennova::TrnConfig& get_trn() const { return trn; }
	// C++-side raster access for the sim's surface-type sampler (the charmap
	// indices ARE the engine's surface classes; [orig: the runtime buffer
	// Terrain_GetSurfaceTypeAtPosition samples @ 0x6065c6]).
	const std::vector<uint8_t> &get_charmap_indices() const { return charmap_indices; }
	int get_charmap_width() const { return charmap_width; }
	int get_charmap_height() const { return charmap_height; }
};

} // namespace godot
