#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include <cpt/cpt_io.h>
#include <trn/trn_io.h>

#include <cstdint>
#include <vector>

namespace godot {

class NovaTerrainFoliageDef;
class NovaTerrainFoliageMap;
class NovaTerrainTileInfo;

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

protected:
	static void _bind_methods();

public:
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
	void set_horizon(double p_val);
	double get_horizon() const;

	Error load();
	bool is_loaded() const;
	PackedByteArray get_depth_raw16() const;
	float get_height(const Vector3 &p_world_pos) const;
	float get_height_world(const Vector3 &p_world_pos) const;
	float get_height_world_bilinear(const Vector3 &p_world_pos) const;
	Color get_colormap_color_world(float world_x, float world_z) const;
	Color get_modulated_colormap_color_world(float world_x, float world_z, const Color &light_color) const;
	// Returns the foliagemap palette index at the given world position, or 0
	// for "outside map / empty".
	// Engine: jodemo.exe sub_5C65E0@0x5C65E0
	// docs/engine_spec_foliage.md 4.4.4, docs/engine_spec_stampdown.md 4.5
	// Shared by runtime NovaFoliageDispatcher wiring and editor paint previews
	// so the world->sector->source mapping lives in exactly one place.
	int get_foliage_index_world(float world_x, float world_z) const;
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
};

} // namespace godot
