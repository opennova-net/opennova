#include "terrain/terrain_data.h"

#include "terrain/terrain_foliage_def.h"
#include "terrain/terrain_foliage_map.h"
#include "terrain/terrain_tile_info.h"

#include <formats/til/til_io.h>
#include <runtime/terrain_query/coords.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/terrain/lighting.h>
#include <runtime/terrain_query/terrain_raycast.h>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "util/pcx_texture_bridge.h"
#include "util/data_format.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/file_access.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <sstream>

using namespace godot;

namespace {

static Ref<TerrainFoliageDef> foliage_def_to_object(const opennova::FoliageDef &def) {
	Ref<TerrainFoliageDef> object;
	object.instantiate();
	object->copy_from_native(def);
	return object;
}

static bool foliage_def_from_variant(const Variant &value, opennova::FoliageDef &out_def) {
	if (value.get_type() == Variant::OBJECT) {
		Object *object = value;
		if (const TerrainFoliageDef *def = Object::cast_to<TerrainFoliageDef>(object)) {
			out_def = def->to_native();
			return true;
		}
	}

	if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = value;
		out_def.graphic = String(dict.get("graphic", "")).utf8().get_data();
		out_def.color_lower = static_cast<int>(dict.get("color_lower", static_cast<int>(opennova::FoliageColorMode::MatchGround)));
		out_def.color_upper = static_cast<int>(dict.get("color_upper", static_cast<int>(opennova::FoliageColorMode::MatchGround)));
		out_def.match = static_cast<int>(dict.get("match", -1));
		int attrib_flags = static_cast<int>(dict.get("attrib_flags", 0));
		if (static_cast<bool>(dict.get("shadow", false))) {
			attrib_flags |= opennova::FOLIAGE_ATTRIB_SHADOW;
		}
		if (static_cast<bool>(dict.get("force_on", false))) {
			attrib_flags |= opennova::FOLIAGE_ATTRIB_FORCE_ON;
		}
		out_def.attrib_flags = static_cast<uint8_t>(std::clamp(attrib_flags, 0, 255));
		out_def = opennova::foliage_normalize_def(out_def);
		return true;
	}

	return false;
}

static opennova::FoliageMap foliage_map_from_slot_data(const std::vector<uint8_t> &indices,
                                                       const uint8_t palette[256][3],
                                                       int width,
                                                       int height) {
	const int map_width = width > 0 ? width : opennova::FOLIAGE_HEIGHTMAP_SIZE;
	const int map_height = height > 0 ? height : opennova::FOLIAGE_HEIGHTMAP_SIZE;
	opennova::FoliageMap map = opennova::foliage_make_default_map(map_width, map_height, 0);
	if (width <= 0 || height <= 0 || indices.size() < static_cast<size_t>(width * height)) {
		return map;
	}
	map.indices = indices;
	std::memcpy(map.palette, palette, sizeof(map.palette));
	return map;
}

struct TerrainWorldSample {
	int sector_sx = 0;
	int sector_sz = 0;
	int sector_id = 0;
	float source_x = 0.0f;
	float source_z = 0.0f;
};

bool resolve_world_sample(const opennova::TrnConfig &trn,
                          float world_x,
                          float world_z,
                          TerrainWorldSample &out_sample) {
	// Runtime world->source mapping (jodemo.exe Terrain_SampleHeightBilinear
	// @0x5C6770 / Terrain_GetFoliageMapValue @0x5C65E0): & 0xF grid wrap, raw
	// sector id, unclamped local offset. Delegates to the shared coordinate
	// kernel used by the bounds-checked terrain raycast path below.
	out_sample = TerrainWorldSample{};
	opennova::terrain::SectorLayout layout;
	layout.sector_grid = &trn.sector_grid[0][0];
	layout.origin_x = trn.origin_x;
	layout.origin_y = trn.origin_y;
	const opennova::terrain::CoordsResult<float> r = opennova::terrain::coords_world_to_source<float>(
	        layout, world_x, world_z, opennova::terrain::coords_runtime_options());
	out_sample.sector_sx = r.sector_sx;
	out_sample.sector_sz = r.sector_sz;
	out_sample.sector_id = r.sector_id;
	out_sample.source_x = r.source_x;
	out_sample.source_z = r.source_z;
	return r.valid;
}

// The portable TerrainHeightField over the loaded CPT depth buffer + TRN
// sector layout comes from the engine's one field builder
// (<runtime/terrain_query/terrain_field_build.h>, ADR 0042 d4), the same
// sampler surface the runtime AI grounds on.
using opennova::terrain::height_field_from;

// Builds a sector layout from the GDScript-exposed members. Caller must ensure
// the grid has at least 256 entries.
opennova::terrain::SectorLayout sector_layout_from(const godot::PackedInt32Array &grid,
                                                   int origin_x, int origin_y,
                                                   int sector_count, int sector_rows) {
	opennova::terrain::SectorLayout layout;
	layout.sector_grid = grid.ptr();
	layout.origin_x = origin_x;
	layout.origin_y = origin_y;
	layout.sector_count = std::clamp(sector_count, 1, 16);
	layout.sector_rows = std::clamp(sector_rows, 1, 16);
	return layout;
}

// Borrow an optional live heightmap's mip-0 as float32 pixels for raycasts.
// False when no image is mounted, it is not FORMAT_RF, or the buffer is short;
// out_pixels shares the image's buffer and keeps the float view alive.
bool live_heightmap_pixels(const Ref<Image> &image, PackedByteArray &out_pixels, int &out_w, int &out_h) {
	if (image.is_null() || image->get_format() != Image::FORMAT_RF) {
		return false;
	}
	out_w = image->get_width();
	out_h = image->get_height();
	out_pixels = image->get_data();
	return out_w > 0 && out_h > 0 &&
	       out_pixels.size() >= static_cast<int64_t>(out_w) * out_h * 4;
}

// Per-point raycast sampling core: bounds-checked world->source transform,
// then edge-clamped bilinear over the FORMAT_RF mip-0 floats.
float sample_live_height_at(const opennova::terrain::SectorLayout &layout,
                            const float *heights, int w, int h,
                            double world_x, double world_z) {
	const opennova::terrain::CoordsResult<double> r = opennova::terrain::coords_world_to_source<double>(
	        layout, world_x, world_z, opennova::terrain::coords_editor_options());
	if (!r.valid) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	// Round the source coords through float32 first, then floor, edge clamp,
	// clamped fractions, and bilinear interpolation in 64-bit.
	const double source_x = static_cast<double>(static_cast<float>(r.source_x));
	const double source_z = static_cast<double>(static_cast<float>(r.source_z));
	const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, w - 1);
	const int z0 = std::clamp(static_cast<int>(std::floor(source_z)), 0, h - 1);
	const int x1 = std::min(x0 + 1, w - 1);
	const int z1 = std::min(z0 + 1, h - 1);
	const double fx = std::clamp(source_x - static_cast<double>(x0), 0.0, 1.0);
	const double fz = std::clamp(source_z - static_cast<double>(z0), 0.0, 1.0);
	const double h00 = heights[static_cast<size_t>(z0) * w + x0];
	const double h10 = heights[static_cast<size_t>(z0) * w + x1];
	const double h01 = heights[static_cast<size_t>(z1) * w + x0];
	const double h11 = heights[static_cast<size_t>(z1) * w + x1];
	const double hx0 = h00 + (h10 - h00) * fx;
	const double hx1 = h01 + (h11 - h01) * fx;
	return static_cast<float>(hx0 + (hx1 - hx0) * fz);
}

// --- The raycast sampler adapter (ENG-3 B1b) --------------------------------
// Reimpl substrate behind TerrainData::raycast_terrain: one
// TerrainRaycastSampler (terrain_query/terrain_raycast.h) over BOTH height
// substrates — the LIVE editable FORMAT_RF image when mounted (what the
// runtime raycasts can consume), else the BAKED CPT
// heights. Kind classification runs the bounds-checked
// world->source transform (coords_editor_options): bounds-reject ->
// kOutOfExtent (the editor-guard divergence the core documents), in-extent
// sector id <= 0 -> kEmpty (the witnessed height-0 floor), else kHeight. The
// POINT callback is the coarse floor-texel read; the BILINEAR callback reuses
// the substrate's shared bilinear core (sample_live_height_at /
// height_field_height_world_bilinear) so the raycast can never disagree with
// the scalar samplers. Heights cross the 16.16 boundary via
// llround(h * 65536.0) — for the baked raw16 substrate that is exactly
// raw16 << 8 (raw16/256 * 65536 == raw16 * 256, exact in float and double).
struct RaycastSubstrate {
	opennova::terrain::SectorLayout layout; // the authored editor extent (classification)
	// Live substrate (preferred when non-null).
	const float *live = nullptr;
	int live_w = 0;
	int live_h = 0;
	// Baked substrate (valid() when active).
	opennova::terrain::TerrainHeightField baked;
};

inline int32_t raycast_height_to_1616(double height_world) {
	return static_cast<int32_t>(std::llround(height_world * 65536.0));
}

// The editor-mode transform rejected the point: split the shared valid=false
// result back into the two sampler kinds using the cell the transform already
// derived (sector_sx/sz are filled before the bounds test).
inline opennova::terrain::TerrainRaycastSample::Kind raycast_classify_invalid(
		const opennova::terrain::SectorLayout &layout,
		const opennova::terrain::CoordsResult<double> &r) {
	const int grid_x = r.sector_sx - layout.origin_x;
	const int grid_z = r.sector_sz - layout.origin_y;
	const bool in_extent = grid_z >= 0 && grid_z < layout.sector_rows &&
	                       grid_x >= 0 && grid_x < layout.sector_count;
	return in_extent ? opennova::terrain::TerrainRaycastSample::kEmpty
	                 : opennova::terrain::TerrainRaycastSample::kOutOfExtent;
}

// The coarse POINT sample: one floor-texel read of the active substrate
// (the core's march tests it before the bilinear confirm).
opennova::terrain::TerrainRaycastSample raycast_sample_point(void *ctx, int32_t world_x_1616,
                                                             int32_t world_y_1616) {
	const RaycastSubstrate &s = *static_cast<const RaycastSubstrate *>(ctx);
	const double wx = world_x_1616 / 65536.0;
	const double wz = world_y_1616 / 65536.0;
	opennova::terrain::TerrainRaycastSample out;
	const opennova::terrain::CoordsResult<double> r = opennova::terrain::coords_world_to_source<double>(
	        s.layout, wx, wz, opennova::terrain::coords_editor_options());
	if (!r.valid) {
		out.kind = raycast_classify_invalid(s.layout, r);
		return out;
	}
	out.kind = opennova::terrain::TerrainRaycastSample::kHeight;
	if (s.live) {
		// Floor-texel read of the live image; source coords rounded through
		// float32 first, matching sample_live_height_at's boundary.
		const double source_x = static_cast<double>(static_cast<float>(r.source_x));
		const double source_z = static_cast<double>(static_cast<float>(r.source_z));
		const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, s.live_w - 1);
		const int z0 = std::clamp(static_cast<int>(std::floor(source_z)), 0, s.live_h - 1);
		out.height_1616 = raycast_height_to_1616(s.live[static_cast<size_t>(z0) * s.live_w + x0]);
	} else {
		// The baked point accessor (nearest texel, raw16/256 world units).
		out.height_1616 = raycast_height_to_1616(opennova::terrain::height_field_height_world(
		        s.baked, static_cast<float>(wx), static_cast<float>(wz)));
	}
	return out;
}

// The BILINEAR sample: the shared bilinear core of the active substrate.
opennova::terrain::TerrainRaycastSample raycast_sample_bilinear(void *ctx, int32_t world_x_1616,
                                                                int32_t world_y_1616) {
	const RaycastSubstrate &s = *static_cast<const RaycastSubstrate *>(ctx);
	const double wx = world_x_1616 / 65536.0;
	const double wz = world_y_1616 / 65536.0;
	opennova::terrain::TerrainRaycastSample out;
	const opennova::terrain::CoordsResult<double> r = opennova::terrain::coords_world_to_source<double>(
	        s.layout, wx, wz, opennova::terrain::coords_editor_options());
	if (!r.valid) {
		out.kind = raycast_classify_invalid(s.layout, r);
		return out;
	}
	out.kind = opennova::terrain::TerrainRaycastSample::kHeight;
	if (s.live) {
		const float h = sample_live_height_at(s.layout, s.live, s.live_w, s.live_h, wx, wz);
		if (std::isnan(h)) {
			// Unreachable (the classification above passed); keep the guard so a
			// disagreement degrades to "no terrain here" instead of NAN fixed-point.
			out.kind = opennova::terrain::TerrainRaycastSample::kOutOfExtent;
			return out;
		}
		out.height_1616 = raycast_height_to_1616(h);
	} else {
		out.height_1616 = raycast_height_to_1616(opennova::terrain::height_field_height_world_bilinear(
		        s.baked, static_cast<float>(wx), static_cast<float>(wz)));
	}
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Macros for texture property boilerplate
// ---------------------------------------------------------------------------

#define IMPL_TEX_PROP(field, setter, getter) \
	void TerrainData::setter(const Ref<Texture2D> &p_tex) { field = p_tex; _sync_trn_texture_filenames_from_refs(); _notify_terrain_changed(); } \
	Ref<Texture2D> TerrainData::getter() const { return field; }

#define BIND_TEX_PROP(prop, setter, getter) \
	ClassDB::bind_method(D_METHOD(#setter, "texture"), &TerrainData::setter); \
	ClassDB::bind_method(D_METHOD(#getter), &TerrainData::getter); \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, #prop, PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), #setter, #getter);

// ---------------------------------------------------------------------------
// _bind_methods
// ---------------------------------------------------------------------------

void TerrainData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_trn_path", "path"), &TerrainData::set_trn_path);
	ClassDB::bind_method(D_METHOD("get_trn_path"), &TerrainData::get_trn_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "trn_path", PROPERTY_HINT_FILE, "*.trn"), "set_trn_path", "get_trn_path");

	ClassDB::bind_method(D_METHOD("load"), &TerrainData::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &TerrainData::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &TerrainData::is_loaded);
	ClassDB::bind_method(D_METHOD("get_height", "world_pos"), &TerrainData::get_height);
	ClassDB::bind_method(D_METHOD("get_height_world", "world_pos"), &TerrainData::get_height_world);
	ClassDB::bind_method(D_METHOD("get_height_world_bilinear", "world_pos"), &TerrainData::get_height_world_bilinear);
	ClassDB::bind_method(D_METHOD("get_surface_normal_world", "world_pos"), &TerrainData::get_surface_normal_world);
	ClassDB::bind_method(D_METHOD("build_minimap_water_mask", "water_height_wu"),
			&TerrainData::build_minimap_water_mask, DEFVAL(NAN));
	ClassDB::bind_method(D_METHOD("get_detail_foliage_index_world", "world_x", "world_z"),
	                     &TerrainData::get_detail_foliage_index_world);
	ClassDB::bind_method(D_METHOD("get_foliage_index_world", "world_x", "world_z"), &TerrainData::get_foliage_index_world);
	ClassDB::bind_method(D_METHOD("world_to_runtime_source_coords", "world_x", "world_z"),
	                     &TerrainData::world_to_runtime_source_coords);
	ClassDB::bind_method(D_METHOD("raycast_terrain", "from", "to"), &TerrainData::raycast_terrain);
	ClassDB::bind_method(D_METHOD("get_tile_count"), &TerrainData::get_tile_count);
	ClassDB::bind_method(D_METHOD("set_sector_grid", "value"), &TerrainData::set_sector_grid);
	ClassDB::bind_method(D_METHOD("get_sector_grid"), &TerrainData::get_sector_grid);
	ClassDB::bind_method(D_METHOD("get_foliage_map"), &TerrainData::get_foliage_map);
	ClassDB::bind_method(D_METHOD("get_foliage_defs"), &TerrainData::get_foliage_defs);
	ClassDB::bind_method(D_METHOD("set_foliage_defs", "value"), &TerrainData::set_foliage_defs);
	ClassDB::bind_method(D_METHOD("set_tileinfo_filename", "filename"), &TerrainData::set_tileinfo_filename);
	ClassDB::bind_method(D_METHOD("get_tileinfo_filename"), &TerrainData::get_tileinfo_filename);
	ClassDB::bind_method(D_METHOD("get_tileinfo_resource"), &TerrainData::get_tileinfo_resource);

	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &TerrainData::save_to_path);

	// Identity
	ClassDB::bind_method(D_METHOD("set_terrain_name", "value"), &TerrainData::set_terrain_name);
	ClassDB::bind_method(D_METHOD("get_terrain_name"), &TerrainData::get_terrain_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "terrain_name"), "set_terrain_name", "get_terrain_name");

	// Textures (Texture2D resources — drag and drop in inspector)
	ADD_GROUP("Textures", "");
	BIND_TEX_PROP(colormap, set_colormap, get_colormap)
	BIND_TEX_PROP(detailmap, set_detailmap, get_detailmap)
	BIND_TEX_PROP(detailmap_c1, set_detailmap_c1, get_detailmap_c1)
	BIND_TEX_PROP(detailmap_c2, set_detailmap_c2, get_detailmap_c2)
	BIND_TEX_PROP(detailmap_c3, set_detailmap_c3, get_detailmap_c3)
	BIND_TEX_PROP(detailmap2, set_detailmap2, get_detailmap2)
	BIND_TEX_PROP(detailmapdist, set_detailmapdist, get_detailmapdist)
	BIND_TEX_PROP(detailmapdist2, set_detailmapdist2, get_detailmapdist2)
	BIND_TEX_PROP(detailblendmap, set_detailblendmap, get_detailblendmap)
	BIND_TEX_PROP(charmap_tex, set_charmap_tex, get_charmap_tex)
	BIND_TEX_PROP(foliagemap_tex, set_foliagemap_tex, get_foliagemap_tex)
	BIND_TEX_PROP(tilestrip_tex, set_tilestrip_tex, get_tilestrip_tex)

	// Detail density
	ADD_GROUP("Detail", "detail_");
	ClassDB::bind_method(D_METHOD("set_detail_density", "value"), &TerrainData::set_detail_density);
	ClassDB::bind_method(D_METHOD("get_detail_density"), &TerrainData::get_detail_density);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_density", PROPERTY_HINT_RANGE, "1,512,1"), "set_detail_density", "get_detail_density");
	ClassDB::bind_method(D_METHOD("set_detail_density2", "value"), &TerrainData::set_detail_density2);
	ClassDB::bind_method(D_METHOD("get_detail_density2"), &TerrainData::get_detail_density2);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_density2", PROPERTY_HINT_RANGE, "1,128,1"), "set_detail_density2", "get_detail_density2");

	// Sectors
	ADD_GROUP("Sectors", "");
	ClassDB::bind_method(D_METHOD("set_sector_count", "value"), &TerrainData::set_sector_count);
	ClassDB::bind_method(D_METHOD("get_sector_count"), &TerrainData::get_sector_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sector_count", PROPERTY_HINT_RANGE, "1,16,1"), "set_sector_count", "get_sector_count");
	ClassDB::bind_method(D_METHOD("set_sector_rows", "value"), &TerrainData::set_sector_rows);
	ClassDB::bind_method(D_METHOD("get_sector_rows"), &TerrainData::get_sector_rows);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sector_rows", PROPERTY_HINT_RANGE, "1,16,1"), "set_sector_rows", "get_sector_rows");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "sector_grid"), "set_sector_grid", "get_sector_grid");
	ClassDB::bind_method(D_METHOD("set_origin_x", "value"), &TerrainData::set_origin_x);
	ClassDB::bind_method(D_METHOD("get_origin_x"), &TerrainData::get_origin_x);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "origin_x"), "set_origin_x", "get_origin_x");
	ClassDB::bind_method(D_METHOD("set_origin_y", "value"), &TerrainData::set_origin_y);
	ClassDB::bind_method(D_METHOD("get_origin_y"), &TerrainData::get_origin_y);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "origin_y"), "set_origin_y", "get_origin_y");
	ClassDB::bind_method(D_METHOD("set_wrap_x", "value"), &TerrainData::set_wrap_x);
	ClassDB::bind_method(D_METHOD("get_wrap_x"), &TerrainData::get_wrap_x);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wrap_x"), "set_wrap_x", "get_wrap_x");
	ClassDB::bind_method(D_METHOD("set_wrap_y", "value"), &TerrainData::set_wrap_y);
	ClassDB::bind_method(D_METHOD("get_wrap_y"), &TerrainData::get_wrap_y);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wrap_y"), "set_wrap_y", "get_wrap_y");

	// Environment
	ADD_GROUP("Environment", "");
	ClassDB::bind_method(D_METHOD("set_water_height", "value"), &TerrainData::set_water_height);
	ClassDB::bind_method(D_METHOD("get_water_height"), &TerrainData::get_water_height);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "water_height"), "set_water_height", "get_water_height");
	ClassDB::bind_method(D_METHOD("set_horizon", "value"), &TerrainData::set_horizon);
	ClassDB::bind_method(D_METHOD("get_horizon"), &TerrainData::get_horizon);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "horizon"), "set_horizon", "get_horizon");

	// Foliage
	ADD_GROUP("Foliage", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "foliage_defs", PROPERTY_HINT_ARRAY_TYPE, "TerrainFoliageDef"),
	             "set_foliage_defs",
	             "get_foliage_defs");

	// Sector/atlas layout constants (single-sourced from engine/runtime/terrain_query
	// terrain_query/coords.h; see the header declarations).
	BIND_CONSTANT(SECTOR_SIZE);
	BIND_CONSTANT(SECTOR_GRID_DIM);
	BIND_CONSTANT(ATLAS_SIZE);
	BIND_CONSTANT(SECTOR_ID_MAX);

	ADD_SIGNAL(MethodInfo("terrain_changed"));
}

TerrainData::TerrainData() {
	sector_grid.resize(256);
}
TerrainData::~TerrainData() {}

// ---------------------------------------------------------------------------
// PCX map data — palette-indexed slots (charmap, foliagemap). Palette and
// indices are owned internally; slot_id dispatches to the right field set.
// ---------------------------------------------------------------------------

// Slot dispatch — maps slot_id to the per-slot state. Extend here when new
// PCX-backed slots (e.g. additional surface maps) are introduced.
struct TerrainData::PcxSlotRefs {
	std::vector<uint8_t>* indices;
	uint8_t (*palette)[3];
	int* width;
	int* height;
	Ref<Texture2D>* tex;
	std::string* trn_filename;
};

bool TerrainData::_resolve_pcx_slot(const String &slot_id, PcxSlotRefs &out) {
	if (slot_id == "charmap") {
		out.indices = &charmap_indices;
		out.palette = charmap_palette;
		out.width = &charmap_width;
		out.height = &charmap_height;
		out.tex = &charmap_tex;
		out.trn_filename = &trn.charmap;
		return true;
	}
	if (slot_id == "foliagemap") {
		out.indices = &foliagemap_indices;
		out.palette = foliagemap_palette;
		out.width = &foliagemap_width;
		out.height = &foliagemap_height;
		out.tex = &foliagemap_tex;
		out.trn_filename = &trn.foliagemap;
		return true;
	}
	return false;
}

void TerrainData::_sync_foliage_map_resource_from_slot() {
	if (foliage_map_resource.is_null()) {
		foliage_map_resource.instantiate();
	}
	foliage_map_resource->copy_from_native(
			foliage_map_from_slot_data(foliagemap_indices, foliagemap_palette, foliagemap_width, foliagemap_height));
}

Error TerrainData::_import_pcx_slot_bytes(const String &slot_id, const String &filename, const PackedByteArray &bytes) {
	PcxSlotRefs refs;
	if (!_resolve_pcx_slot(slot_id, refs)) {
		UtilityFunctions::push_error("TerrainData: unknown PCX slot '", slot_id, "'");
		return ERR_INVALID_PARAMETER;
	}

	int w = 0, h = 0;
	if (!opennova::decode_pcx_with_palette(bytes.ptr(), bytes.size(),
	                                        *refs.indices, refs.palette, w, h)) {
		UtilityFunctions::push_error("TerrainData: PCX decode failed for ", filename);
		return ERR_FILE_CORRUPT;
	}
	*refs.width = w;
	*refs.height = h;
	*refs.tex = opennova::build_indexed_texture(*refs.indices, refs.palette, w, h);
	*refs.trn_filename = filename.get_file().utf8().get_data();
	if (slot_id == "foliagemap") {
		_sync_foliage_map_resource_from_slot();
	}
	_notify_terrain_changed();
	return OK;
}

// ---------------------------------------------------------------------------
// Property accessors
// ---------------------------------------------------------------------------

void TerrainData::set_trn_path(const String &p_path) { trn_path = p_path; }
String TerrainData::get_trn_path() const { return trn_path; }

void TerrainData::set_terrain_name(const String &p_name) { terrain_name = p_name; _notify_terrain_changed(); }
String TerrainData::get_terrain_name() const { return terrain_name; }

void TerrainData::set_colormap(const Ref<Texture2D> &p_tex) {
	colormap = p_tex;
	_sync_trn_texture_filenames_from_refs();
	_notify_terrain_changed();
}

Ref<Texture2D> TerrainData::get_colormap() const { return colormap; }

IMPL_TEX_PROP(detailmap, set_detailmap, get_detailmap)
IMPL_TEX_PROP(detailmap_c1, set_detailmap_c1, get_detailmap_c1)
IMPL_TEX_PROP(detailmap_c2, set_detailmap_c2, get_detailmap_c2)
IMPL_TEX_PROP(detailmap_c3, set_detailmap_c3, get_detailmap_c3)
IMPL_TEX_PROP(detailmap2, set_detailmap2, get_detailmap2)
IMPL_TEX_PROP(detailmapdist, set_detailmapdist, get_detailmapdist)
IMPL_TEX_PROP(detailmapdist2, set_detailmapdist2, get_detailmapdist2)
IMPL_TEX_PROP(detailblendmap, set_detailblendmap, get_detailblendmap)
IMPL_TEX_PROP(charmap_tex, set_charmap_tex, get_charmap_tex)
IMPL_TEX_PROP(foliagemap_tex, set_foliagemap_tex, get_foliagemap_tex)
IMPL_TEX_PROP(tilestrip_tex, set_tilestrip_tex, get_tilestrip_tex)

void TerrainData::set_detail_density(int p_val) { detail_density = p_val; _notify_terrain_changed(); }
int TerrainData::get_detail_density() const { return detail_density; }
void TerrainData::set_detail_density2(int p_val) { detail_density2 = p_val; _notify_terrain_changed(); }
int TerrainData::get_detail_density2() const { return detail_density2; }
void TerrainData::set_sector_count(int p_val) { sector_count = p_val; _notify_terrain_changed(); }
int TerrainData::get_sector_count() const { return sector_count; }
void TerrainData::set_sector_rows(int p_val) { sector_rows = p_val; _notify_terrain_changed(); }
int TerrainData::get_sector_rows() const { return sector_rows; }
void TerrainData::set_origin_x(int p_val) { origin_x = p_val; _notify_terrain_changed(); }
int TerrainData::get_origin_x() const { return origin_x; }
void TerrainData::set_origin_y(int p_val) { origin_y = p_val; _notify_terrain_changed(); }
int TerrainData::get_origin_y() const { return origin_y; }
void TerrainData::set_water_height(int p_val) { water_height = p_val; _notify_terrain_changed(); }
int TerrainData::get_water_height() const { return water_height; }
void TerrainData::set_wrap_x(bool p_val) { wrap_x = p_val; _notify_terrain_changed(); }
bool TerrainData::get_wrap_x() const { return wrap_x; }
void TerrainData::set_wrap_y(bool p_val) { wrap_y = p_val; _notify_terrain_changed(); }
bool TerrainData::get_wrap_y() const { return wrap_y; }
PackedInt32Array TerrainData::get_quadrant_locks() const {
	PackedInt32Array locks;
	locks.resize(8);
	const opennova::TerrainQuadrantLocks source = trn.get_quadrant_locks();
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		locks.set(quadrant * 2, source[quadrant].x);
		locks.set(quadrant * 2 + 1, source[quadrant].y);
	}
	return locks;
}
void TerrainData::set_horizon(double p_val) { horizon = p_val; _notify_terrain_changed(); }
double TerrainData::get_horizon() const { return horizon; }
void TerrainData::set_sector_grid(const PackedInt32Array &p_grid) {
	sector_grid.resize(256);
	for (int i = 0; i < 256; i++) {
		sector_grid.set(i, i < p_grid.size() ? p_grid[i] : 0);
	}
	_notify_terrain_changed();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void _sync_texture_filename(const Ref<Texture2D> &texture, std::string &target_field) {
	if (texture.is_null()) {
		return;
	}
	String path = texture->get_path();
	if (path.is_empty()) {
		return;
	}
	const String filename = path.get_file();
	if (!filename.is_empty()) {
		target_field = filename.utf8().get_data();
	}
}

// Sync Godot scalar properties (name, sector grid, water, wrap, etc.) into
// the underlying TrnConfig. Always safe to run — does not touch texture
// filename fields, which are owned by the Ref<Texture2D> setters (covered by
// _sync_trn_texture_filenames_from_refs below) and the import/reset paths.
void TerrainData::_sync_trn_scalars_from_properties() {
	trn.name = terrain_name.utf8().get_data();
	trn.detail_density = detail_density;
	trn.detail_density2 = detail_density2;
	trn.sector_count = sector_count;
	trn.sector_rows = sector_rows;
	trn.origin_x = origin_x;
	trn.origin_y = origin_y;
	trn.water_height = water_height;
	trn.wrap_x = wrap_x ? 1 : 0;
	trn.wrap_y = wrap_y ? 1 : 0;
	trn.horizon = horizon;
	for (int gz = 0; gz < SECTOR_GRID_DIM; gz++) {
		for (int gx = 0; gx < SECTOR_GRID_DIM; gx++) {
			const int idx = gz * SECTOR_GRID_DIM + gx;
			trn.sector_grid[gz][gx] = idx < sector_grid.size() ? sector_grid[idx] : 0;
		}
	}
}

// Re-derive texture filename fields from the Ref<Texture2D> paths. Only
// called from IMPL_TEX_PROP setters — running this elsewhere would
// silently overwrite filenames the import/reset paths own (the Ref still
// carries its source path after a slot was renamed).
void TerrainData::_sync_trn_texture_filenames_from_refs() {
	_sync_texture_filename(colormap, trn.colormap);
	_sync_texture_filename(detailmap, trn.detailmap);
	_sync_texture_filename(detailmap_c1, trn.detailmap_c1);
	_sync_texture_filename(detailmap_c2, trn.detailmap_c2);
	_sync_texture_filename(detailmap_c3, trn.detailmap_c3);
	_sync_texture_filename(detailmap2, trn.detailmap2);
	_sync_texture_filename(detailmapdist, trn.detailmapdist);
	_sync_texture_filename(detailmapdist2, trn.detailmapdist2);
	_sync_texture_filename(detailblendmap, trn.detailblendmap);
	// PCX-backed slot filenames are owned by import/reset paths because their
	// preview textures are generated in memory and do not carry resource paths.
	_sync_texture_filename(tilestrip_tex, trn.tilestrip);
}

void TerrainData::_notify_terrain_changed() {
	// Always sync scalars — cheap and needed so `trn` stays consistent for
	// saves. Deliberately DO NOT sync texture filenames here: that would
	// clobber filenames the import/reset paths own. Texture Ref setters call
	// the filename sync themselves.
	_sync_trn_scalars_from_properties();
	++change_revision_;
	if (change_revision_ == 0) ++change_revision_;
	emit_signal("terrain_changed");
	emit_changed();
}

// ---------------------------------------------------------------------------
// Texture loading helper — delegates to shared util/texture_path_resolver.h.
// res:// textures load through ResourceLoader (the imported .ctex), so they
// survive export; absolute paths decode raw bytes.
// ---------------------------------------------------------------------------

static Ref<Texture2D> _load_texture_from_dir(const String &dir, const String &filename) {
	return opennova::load_texture_from_dir(dir, filename);
}

Ref<Texture2D> TerrainData::load_source_texture(
		const String &p_filename) const {
	if (p_filename.is_empty()) return {};
	if (resource_root.is_valid() && !resource_root->get_root_dir().is_empty()) {
		return resource_root->load_texture(p_filename);
	}
	if (trn_path.is_empty()) return {};
	return _load_texture_from_dir(trn_path.get_base_dir(), p_filename);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

Error TerrainData::load() {
	loaded = false;
	cpt = opennova::CptFile();
	trn = opennova::TrnConfig();
	resource_root.unref();

	if (trn_path.is_empty()) {
		UtilityFunctions::push_warning("TerrainData: trn_path must be set before loading");
		return ERR_INVALID_PARAMETER;
	}

	PackedByteArray trn_bytes;
	if (!read_nova_payload_file(trn_path, trn_bytes)) {
		UtilityFunctions::push_warning("TerrainData: Cannot open TRN: ", trn_path);
		return ERR_FILE_CANT_READ;
	}
	std::string trn_content(reinterpret_cast<const char *>(trn_bytes.ptr()), static_cast<size_t>(trn_bytes.size()));

	return _load_from_trn_text(trn_content, trn_path);
}

Error TerrainData::load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name) {
	loaded = false;
	cpt = opennova::CptFile();
	trn = opennova::TrnConfig();
	resource_root.unref();

	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		UtilityFunctions::push_warning("TerrainData: resource root must be configured before loading");
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		UtilityFunctions::push_warning("TerrainData: resource filename is empty");
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray trn_bytes = p_resource_root->read_file(file);
	if (trn_bytes.is_empty()) {
		UtilityFunctions::push_warning("TerrainData: Cannot open TRN from resource root: ", file);
		return ERR_FILE_CANT_READ;
	}

	trn_path = file;
	resource_root = p_resource_root;
	const std::string trn_content(reinterpret_cast<const char *>(trn_bytes.ptr()), static_cast<size_t>(trn_bytes.size()));
	return _load_from_trn_text(trn_content, file);
}

Error TerrainData::_load_from_trn_text(const std::string &trn_content, const String &source_label) {
	(void)source_label;
	std::string error;
	std::istringstream trn_stream(trn_content);
	if (!opennova::load_trn(trn_stream, trn, error)) {
		UtilityFunctions::push_warning("TerrainData: TRN parse failed: ", error.c_str());
		return ERR_FILE_CANT_READ;
	}

	// Sync scalar properties from parsed TRN
	terrain_name = String(trn.name.c_str());
	detail_density = trn.detail_density;
	detail_density2 = trn.detail_density2;
	sector_count = trn.sector_count;
	sector_rows = trn.sector_rows > 0 ? trn.sector_rows : trn.sector_count;
	origin_x = trn.origin_x;
	origin_y = trn.origin_y;
	water_height = trn.water_height;
	wrap_x = trn.wrap_x != 0;
	wrap_y = trn.wrap_y != 0;
	horizon = trn.horizon;
	sector_grid.resize(256);
	for (int gz = 0; gz < SECTOR_GRID_DIM; gz++) {
		for (int gx = 0; gx < SECTOR_GRID_DIM; gx++) {
			sector_grid.set(gz * SECTOR_GRID_DIM + gx, trn.sector_grid[gz][gx]);
		}
	}

	// Load textures from TRN filenames → Texture2D resources. Warn when a
	// referenced texture fails to resolve (e.g. a raw .tga stripped from an
	// exported PCK) so the failure is visible instead of silently untextured.
	const bool use_resource_root = resource_root.is_valid() && !resource_root->get_root_dir().is_empty();
	String trn_dir = use_resource_root ? resource_root->get_root_dir() : trn_path.get_base_dir();
	auto load_tex = [&](const char *slot, const String &filename) -> Ref<Texture2D> {
		Ref<Texture2D> tex = use_resource_root
				? resource_root->load_texture(filename)
				: _load_texture_from_dir(trn_dir, filename);
		if (tex.is_null() && !filename.is_empty()) {
			UtilityFunctions::push_warning("TerrainData: ", slot, " texture '", filename,
				"' did not resolve under ", trn_dir, " (terrain may render untextured)");
		}
		return tex;
	};
	const String charmap_filename = String(trn.charmap.c_str());
	const String foliagemap_filename = String(trn.foliagemap.c_str());
	const String tilestrip_filename = String(trn.tilestrip.c_str());
	colormap = load_tex("colormap", String(trn.colormap.c_str()));
	detailmap = load_tex("detailmap", String(trn.detailmap.c_str()));
	detailmap_c1 = load_tex("detailmap_c1", String(trn.detailmap_c1.c_str()));
	detailmap_c2 = load_tex("detailmap_c2", String(trn.detailmap_c2.c_str()));
	detailmap_c3 = load_tex("detailmap_c3", String(trn.detailmap_c3.c_str()));
	detailmap2 = load_tex("detailmap2", String(trn.detailmap2.c_str()));
	detailmapdist = load_tex("detailmapdist", String(trn.detailmapdist.c_str()));
	detailmapdist2 = load_tex("detailmapdist2", String(trn.detailmapdist2.c_str()));
	detailblendmap = load_tex("detailblendmap", String(trn.detailblendmap.c_str()));
	auto use_default_pcx_slot = [this](const String &slot_id) {
		PcxSlotRefs refs;
		if (!_resolve_pcx_slot(slot_id, refs)) {
			return;
		}
		constexpr int kDefaultMapSize = 1024;
		*refs.width = kDefaultMapSize;
		*refs.height = kDefaultMapSize;
		refs.indices->assign(kDefaultMapSize * kDefaultMapSize,
		                     slot_id == "charmap" ? 1 : 0);
		for (int index = 0; index < 256; ++index) {
			refs.palette[index][0] = static_cast<uint8_t>(index);
			refs.palette[index][1] = static_cast<uint8_t>(index);
			refs.palette[index][2] = static_cast<uint8_t>(index);
		}
		if (slot_id == "charmap") {
			static constexpr uint8_t kCharmapColors[][4] = {
				{0, 0, 0, 0}, {1, 153, 118, 61}, {2, 0, 210, 0},
				{3, 204, 239, 244}, {4, 152, 152, 152}, {5, 255, 255, 0},
				{6, 255, 128, 0}, {7, 0, 0, 255}, {8, 255, 0, 0},
				{9, 114, 64, 0}, {10, 160, 190, 219}, {11, 161, 0, 161},
				{12, 255, 0, 186}, {13, 158, 78, 0}, {14, 0, 201, 203},
				{15, 255, 255, 255},
			};
			for (const auto &entry : kCharmapColors) {
				refs.palette[entry[0]][0] = entry[1];
				refs.palette[entry[0]][1] = entry[2];
				refs.palette[entry[0]][2] = entry[3];
			}
		}
		*refs.tex = opennova::build_indexed_texture(
		        *refs.indices, refs.palette, kDefaultMapSize, kDefaultMapSize);
		if (slot_id == "foliagemap") {
			_sync_foliage_map_resource_from_slot();
		}
	};
	for (const char* slot : {"charmap", "foliagemap"}) {
		String slot_id(slot);
		String filename = (slot_id == "charmap")
			? charmap_filename
			: foliagemap_filename;
		if (use_resource_root) {
			PackedByteArray bytes = resource_root->read_file(filename);
			String mounted_filename = filename;
			if (bytes.is_empty()) {
				for (const String &candidate : opennova::texture_candidate_filenames(filename)) {
					bytes = resource_root->read_file(candidate);
					if (!bytes.is_empty()) {
						mounted_filename = candidate;
						break;
					}
				}
			}
			if (!bytes.is_empty()) {
				Error err = _import_pcx_slot_bytes(slot_id, mounted_filename, bytes);
				if (err != OK) {
					UtilityFunctions::push_warning(
						"TerrainData: failed to import ", slot_id,
						" from mounted resource '", mounted_filename, "' (error ", err, "); resetting slot to default");
					use_default_pcx_slot(slot_id);
				}
			} else {
				if (!filename.is_empty())
					UtilityFunctions::push_warning("TerrainData: ", slot_id, " not found for '", filename, "' under ", trn_dir);
				use_default_pcx_slot(slot_id);
			}
		} else {
			String resolved = opennova::resolve_texture_path(trn_dir, filename);
			if (!resolved.is_empty()) {
				PackedByteArray bytes;
				const Error err = read_nova_payload_file(resolved, bytes)
				        ? _import_pcx_slot_bytes(slot_id, resolved.get_file(), bytes)
				        : ERR_FILE_CANT_READ;
				if (err != OK) {
					UtilityFunctions::push_warning(
						"TerrainData: failed to import ", slot_id,
						" from '", resolved, "' (error ", err, "); resetting slot to default");
					use_default_pcx_slot(slot_id);
				}
			} else {
				if (!filename.is_empty())
					UtilityFunctions::push_warning("TerrainData: ", slot_id, " not found for '", filename, "' under ", trn_dir);
				use_default_pcx_slot(slot_id);
			}
		}
	}
	tilestrip_tex = load_tex("tilestrip", tilestrip_filename);
	trn.tilestrip = tilestrip_filename.utf8().get_data();

	// CPT is an export-time bake artefact; editor projects legitimately save
	// a .trn without one (see plan: "Make CPT optional"). Missing/empty
	// polydata is not an error — load() still succeeds, cpt stays empty, and
	// consumers that need CPT (Terrain::_build_terrain @ terrain.cpp:571,
	// get_height* guards @ terrain_data.cpp:714/739/758) already early-out
	// gracefully.
	if (trn.polydata.empty()) {
		loaded = true;
		UtilityFunctions::print_verbose("TerrainData: Loaded terrain '", terrain_name,
			"' (no CPT — editor project mode)");
		return OK;
	}

	PackedByteArray cpt_bytes;
	const String cpt_name = String(trn.polydata.c_str());
	const String cpt_path = trn_dir.path_join(cpt_name);
	if (use_resource_root) {
		cpt_bytes = resource_root->read_file(cpt_name);
	} else {
		read_nova_payload_file(cpt_path, cpt_bytes);
	}
	if (cpt_bytes.is_empty()) {
		loaded = true;
		UtilityFunctions::push_warning("TerrainData: CPT '", cpt_path,
			"' missing; continuing without baked terrain (run Export to generate it)");
		return OK;
	}

	if (!opennova::load_cpt(cpt_bytes.ptr(), cpt_bytes.size(), cpt, error)) {
		UtilityFunctions::push_warning("TerrainData: CPT parse failed: ", error.c_str());
		return ERR_FILE_CANT_READ;
	}

	loaded = true;

	UtilityFunctions::print_verbose("TerrainData: Loaded terrain '", terrain_name,
		"' — ", static_cast<int>(cpt.tiles.size()), " tiles, depth buffer ",
		static_cast<int>(cpt.depth_buffer.size()), " pixels");

	return OK;
}

bool TerrainData::is_loaded() const {
	return loaded;
}

PackedByteArray TerrainData::get_depth_raw16() const {
	PackedByteArray out;

	// Prefer the optional live heightmap (FORMAT_RF, 1 float per cell). The
	// conversion mirrors the former GDScript image_to_raw16 byte-for-byte:
	// value = clamp(int(height * 256), 0, 65535), stored little-endian.
	if (heightmap_image.is_valid()) {
		const int w = heightmap_image->get_width();
		const int h = heightmap_image->get_height();
		const int64_t count = static_cast<int64_t>(w) * static_cast<int64_t>(h);
		const PackedByteArray pixels = heightmap_image->get_data();
		if (count > 0 && pixels.size() >= count * 4) {
			out.resize(count * 2);
			uint8_t *dst = out.ptrw();
			const uint8_t *src = pixels.ptr();
			for (int64_t i = 0; i < count; ++i) {
				float f;
				std::memcpy(&f, src + i * 4, sizeof(float));
				int value = static_cast<int>(static_cast<double>(f) * 256.0);
				value = std::clamp(value, 0, 65535);
				dst[i * 2] = static_cast<uint8_t>(value & 0xFF);
				dst[i * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
			}
			return out;
		}
	}

	if (cpt.depth_buffer.empty()) {
		return out;
	}

	out.resize(static_cast<int64_t>(cpt.depth_buffer.size() * 2u));
	uint8_t *dst = out.ptrw();
	for (size_t i = 0; i < cpt.depth_buffer.size(); ++i) {
		const uint16_t value = cpt.depth_buffer[i];
		dst[i * 2u] = static_cast<uint8_t>(value & 0xFFu);
		dst[i * 2u + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
	}
	return out;
}

Ref<Image> TerrainData::get_heightmap_image() const {
	return heightmap_image;
}

Ref<Image> TerrainData::get_colormap_image() const {
	return colormap_image;
}

Ref<Image> TerrainData::get_blendmap_image() const {
	return blendmap_image;
}

// The three height queries now delegate to the shared engine/runtime/terrain sampler
// (terrain_query/height_field.h) so the runtime AI grounding + the editor sample one
// implementation. Bodies were lifted verbatim into height_field.cpp; this class
// keeps only the loaded-guard + world->Godot coordinate boundary.
float TerrainData::get_height(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return 0.0f;
	return opennova::terrain::height_field_height_bilinear(height_field_from(cpt, trn),
	                                                        p_world_pos.x, p_world_pos.z);
}

float TerrainData::get_height_world(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return 0.0f;
	return opennova::terrain::height_field_height_world(height_field_from(cpt, trn),
	                                                     p_world_pos.x, p_world_pos.z);
}

float TerrainData::get_height_world_bilinear(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return 0.0f;
	return opennova::terrain::height_field_height_world_bilinear(height_field_from(cpt, trn),
	                                                              p_world_pos.x, p_world_pos.z);
}

Vector3 TerrainData::get_surface_normal_world(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return Vector3(0.0f, 1.0f, 0.0f);
	const opennova::terrain::TerrainSurfaceNormal normal =
			opennova::terrain::height_field_surface_normal_world(
					height_field_from(cpt, trn), p_world_pos.x, p_world_pos.z);
	// The portable contract owns the recovered query. This device leg maps its
	// {source X, source Z, height} basis to Godot {X, Y-up, Z}.
	// [orig: Terrain_GenerateNormalMap @0x603210; WAC fx2ssn consumes the
	// terrain table at WacScript_SpawnEffectAtSsnEntity @0x4F23A0, see docs/terrain/terrain-re.md]
	return Vector3(static_cast<float>(normal.x),
	               static_cast<float>(normal.up),
	               static_cast<float>(normal.z));
}


Ref<ImageTexture> TerrainData::build_minimap_water_mask(
		float p_water_height_wu) const {
	// depthspin is built directly from the 1024x1024 raw16 height atlas. Each
	// output texel averages the four source taps at (4x,4z), (+2,0), (0,+2),
	// and (+2,+2), then keeps the integer world-height byte (sum >> 10).
	// Retail witness: PolyTrn_InitTextures @0x60BA20..0x60BB3B; the exact
	// address-level contract is recorded in hud_minimap.cpp and hud-re.md.
	constexpr int kSourcePx = 1024;
	constexpr int kMaskPx = 256;
	constexpr int kStep = 4;
	constexpr int kHalfStep = 2;
	if (!loaded || cpt.depth_buffer.size() !=
			static_cast<size_t>(kSourcePx) * kSourcePx) {
		return Ref<ImageTexture>();
	}
	const float water_wu = std::isnan(p_water_height_wu)
			? static_cast<float>(water_height) * 0.5f
			: p_water_height_wu;
	if (!std::isfinite(water_wu)) {
		return Ref<ImageTexture>();
	}
	// Env_WaterHeightFixed == 0 suppresses the entire pass. Positive SHIWORD
	// conversion truncates to the integer plane; retail clamps it at 254.
	const int water_int = std::clamp(
			static_cast<int>(std::floor(water_wu)), 0, 254);
	if (water_int <= 0) {
		return Ref<ImageTexture>();
	}

	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(kMaskPx) * kMaskPx * 2);
	bytes.fill(0);
	uint8_t *dst = bytes.ptrw();
	const uint16_t *height = cpt.depth_buffer.data();
	for (int z = 0; z < kMaskPx; ++z) {
		const int source_z = z * kStep;
		for (int x = 0; x < kMaskPx; ++x) {
			const int source_x = x * kStep;
			uint8_t *pixel = dst +
					(static_cast<int64_t>(z) * kMaskPx + x) * 2;
			const uint32_t sum =
					height[source_z * kSourcePx + source_x] +
					height[source_z * kSourcePx + source_x + kHalfStep] +
					height[(source_z + kHalfStep) * kSourcePx + source_x] +
					height[(source_z + kHalfStep) * kSourcePx +
							source_x + kHalfStep];
			const int terrain_height_int = static_cast<int>(sum >> 10);
			// Keep both operands until raster time: retail linearly samples the
			// height-alpha field and only THEN alpha-tests it against the water
			// plane. Thresholding these texels to a binary mask first changes the
			// contour and produces a broad filtered halo. RG8 is a linear data
			// texture; the map-water shader compares sampled R (terrain) to the
			// constant sampled G (water).
			pixel[0] = static_cast<uint8_t>(terrain_height_int);
			pixel[1] = static_cast<uint8_t>(water_int);
		}
	}
	const Ref<Image> mask = Image::create_from_data(kMaskPx, kMaskPx, false,
			Image::FORMAT_RG8, bytes);
	return mask.is_valid() ? ImageTexture::create_from_image(mask)
			: Ref<ImageTexture>();
}

Vector2 TerrainData::world_to_runtime_source_coords(float world_x, float world_z) const {
	if (sector_grid.size() < 256) {
		return Vector2(-1.0f, -1.0f);
	}
	const opennova::terrain::SectorLayout layout =
	        sector_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	const opennova::terrain::CoordsResult<float> r =
	        opennova::terrain::coords_world_to_source<float>(
	                layout, world_x, world_z, opennova::terrain::coords_runtime_options());
	if (!r.valid) {
		return Vector2(-1.0f, -1.0f);
	}
	return Vector2(r.source_x, r.source_z);
}

Vector3 TerrainData::raycast_terrain(const Vector3 &p_from, const Vector3 &p_to) const {
	// The ENG-3 B1 segment raycast [orig: Terrain_RaycastHeightmapLoRes
	// @ 0x60cb80; Terrain_RaycastHeightmapHiRes_0 @ 0x60e710] over the
	// RaycastSubstrate sampler adapter above; docs/terrain/terrain-re.md
	// §Runtime terrain queries. Godot plane coords map straight onto the core's
	// axis-agnostic (x, y): (world_x, world_z), with world_y as the core's
	// z = height axis, all quantized to 16.16 at this boundary.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const Vector3 miss(nan, nan, nan);
	if (!p_from.is_finite() || !p_to.is_finite()) {
		return miss;
	}

	// Substrate pick: the live editable image when mounted, else the baked CPT.
	// NO substrate at all -> the all-NAN miss. Retail's null-atlas raycast
	// returns HIT there instead ("blocked" is the runtime's no-data default
	// [orig: @ 0x60ccf7, see docs/terrain/terrain-re.md]) — that is runtime-substrate behavior; this binding is
	// the editor-mode surface, so no-data misses: the same deliberate
	// editor-guard divergence class as the sampler's kOutOfExtent
	// (terrain_query/terrain_raycast.h header note).
	RaycastSubstrate substrate;
	PackedByteArray live_pixels; // keeps the borrowed live mip-0 alive across the march
	if (live_heightmap_pixels(heightmap_image, live_pixels, substrate.live_w, substrate.live_h)) {
		substrate.live = reinterpret_cast<const float *>(live_pixels.ptr());
	} else if (loaded && !cpt.depth_buffer.empty()) {
		substrate.baked = height_field_from(cpt, trn);
	} else {
		return miss;
	}
	if (sector_grid.size() < 256) {
		return miss; // no authored layout to classify against (mirrors the live samplers)
	}
	substrate.layout = sector_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);

	// REIMPL bounding, not part of the witnessed core: clip the working segment
	// to the authored-extent XZ AABB (replacing the editor's old GDScript slab
	// test) plus a generous height band so the 16.16 quantization below cannot
	// overflow — callers pass long probe segments (the editor mouse ray uses
	// origin + dir * 100000).
	const double extent_min_x = static_cast<double>(substrate.layout.origin_x) *
	                            opennova::terrain::COORDS_SECTOR_SIZE;
	const double extent_max_x = extent_min_x + static_cast<double>(substrate.layout.sector_count) *
	                                                   opennova::terrain::COORDS_SECTOR_SIZE;
	const double extent_min_z = static_cast<double>(substrate.layout.origin_y) *
	                            opennova::terrain::COORDS_SECTOR_SIZE;
	const double extent_max_z = extent_min_z + static_cast<double>(substrate.layout.sector_rows) *
	                                                   opennova::terrain::COORDS_SECTOR_SIZE;
	constexpr double HEIGHT_BAND = 30000.0; // within int32 16.16 (±32768), far above any terrain
	const double dx = static_cast<double>(p_to.x) - static_cast<double>(p_from.x);
	const double dy = static_cast<double>(p_to.y) - static_cast<double>(p_from.y);
	const double dz = static_cast<double>(p_to.z) - static_cast<double>(p_from.z);
	double t0 = 0.0;
	double t1 = 1.0;
	const auto clip_axis = [&t0, &t1](double origin, double delta, double lo, double hi) -> bool {
		if (delta == 0.0) {
			return origin >= lo && origin <= hi;
		}
		const double ta = (lo - origin) / delta;
		const double tb = (hi - origin) / delta;
		t0 = std::max(t0, std::min(ta, tb));
		t1 = std::min(t1, std::max(ta, tb));
		return true;
	};
	if (!clip_axis(p_from.x, dx, extent_min_x, extent_max_x) ||
	    !clip_axis(p_from.z, dz, extent_min_z, extent_max_z) ||
	    !clip_axis(p_from.y, dy, -HEIGHT_BAND, HEIGHT_BAND) || t1 < t0) {
		return miss;
	}

	const auto to_1616 = [](double v) { return static_cast<int32_t>(std::llround(v * 65536.0)); };
	const int32_t start[3] = {
		to_1616(static_cast<double>(p_from.x) + dx * t0),
		to_1616(static_cast<double>(p_from.z) + dz * t0),
		to_1616(static_cast<double>(p_from.y) + dy * t0),
	};
	const int32_t end[3] = {
		to_1616(static_cast<double>(p_from.x) + dx * t1),
		to_1616(static_cast<double>(p_from.z) + dz * t1),
		to_1616(static_cast<double>(p_from.y) + dy * t1),
	};

	opennova::terrain::TerrainRaycastSampler sampler;
	sampler.point = &raycast_sample_point;
	sampler.bilinear = &raycast_sample_bilinear;
	sampler.ctx = &substrate;
	int32_t hit[3] = { 0, 0, 0 };
	if (!opennova::terrain::terrain_raycast_refined(sampler, start, end, hit)) {
		return miss;
	}
	return Vector3(static_cast<real_t>(hit[0] / 65536.0),
	               static_cast<real_t>(hit[2] / 65536.0),
	               static_cast<real_t>(hit[1] / 65536.0));
}

int TerrainData::get_tile_count() const {
	return static_cast<int>(cpt.tiles.size());
}

int TerrainData::get_detail_foliage_index_fixed(int32_t world_x_fixed,
                                                    int32_t world_z_fixed) const {
	// [orig: Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0, see docs/terrain/terrain-re.md]
	if (!loaded || foliage_map_resource.is_null()) {
		return 0;
	}
	return static_cast<int>(foliage_map_resource->sample_detail_flat_wrap(
			world_x_fixed, world_z_fixed));
}

int TerrainData::get_detail_foliage_index_world(double world_x, double world_z) const {
	if (!loaded || foliage_map_resource.is_null()) {
		return 0;
	}
	return foliage_map_resource->sample_detail_index_world(world_x, world_z);
}

int TerrainData::get_foliage_index_world(float world_x, float world_z) const {
	// Engine sub_5C65E0 (Terrain_GetFoliageMapValue) analogue. Uses the same
	// sector+origin+quadrant math as get_height_world_bilinear — the sector
	// grid is always 16×16; origin places the active region inside it with a
	// wraparound mask on the lookup.
	if (!loaded || foliage_map_resource.is_null()) {
		return 0;
	}

	TerrainWorldSample sample;
	if (!resolve_world_sample(trn, world_x, world_z, sample)) {
		return 0;
	}

	const int w = foliage_map_resource->get_width();
	const int h = foliage_map_resource->get_height();
	if (w <= 0 || h <= 0) {
		return 0;
	}
	const int map_x = foliage_map_resource->map_x_from_heightmap_x(sample.source_x);
	const int map_y = foliage_map_resource->map_y_from_heightmap_y(sample.source_z);
	if (map_x < 0 || map_x >= w || map_y < 0 || map_y >= h) {
		return 0;
	}
	return static_cast<int>(foliage_map_resource->get_index(map_x, map_y));
}

PackedInt32Array TerrainData::get_sector_grid() const {
	return sector_grid;
}

Ref<TerrainFoliageMap> TerrainData::get_foliage_map() const {
	if (foliage_map_resource.is_null()) {
		const_cast<TerrainData *>(this)->_sync_foliage_map_resource_from_slot();
	}
	return foliage_map_resource;
}

Array TerrainData::get_foliage_defs() const {
	Array arr;
	for (const auto& def : trn.foliage_defs) {
		arr.push_back(foliage_def_to_object(def));
	}
	return arr;
}

void TerrainData::set_foliage_defs(const Array &p_defs) {
	trn.foliage_defs.clear();
	int count = p_defs.size();
	if (count > opennova::FOLIAGE_MAX_DEFS) count = opennova::FOLIAGE_MAX_DEFS;
	for (int i = 0; i < count; i++) {
		opennova::FoliageDef def;
		if (foliage_def_from_variant(p_defs[i], def)) {
			trn.foliage_defs.push_back(opennova::foliage_normalize_def(def));
		}
	}
	_notify_terrain_changed();
}

void TerrainData::set_tileinfo_filename(const String &filename) {
	trn.tileinfo = filename.utf8().get_data();
	tileinfo_resource_cache.unref();
	tileinfo_resource_cache_path = String();
	_notify_terrain_changed();
}

String TerrainData::get_tileinfo_filename() const {
	return String(trn.tileinfo.c_str());
}

Ref<TerrainTileInfo> TerrainData::get_tileinfo_resource() const {
	const String filename = String(trn.tileinfo.c_str());
	if (filename.is_empty() || (trn_path.is_empty() && resource_root.is_null())) {
		tileinfo_resource_cache.unref();
		tileinfo_resource_cache_path = String();
		return Ref<TerrainTileInfo>();
	}

	const bool use_resource_root = resource_root.is_valid() && !resource_root->get_root_dir().is_empty();
	const String resolved = use_resource_root ? String() : opennova::resolve_sidecar_path(trn_path.get_base_dir(), filename, "til");
	const String lookup = use_resource_root
			? resource_root->get_root_dir().path_join(filename)
			: (resolved.is_empty() ? trn_path.get_base_dir().path_join(filename) : resolved);

	if (tileinfo_resource_cache.is_valid() && tileinfo_resource_cache_path == lookup) {
		return tileinfo_resource_cache;
	}

	PackedByteArray bytes;
	String lookup_name = filename;
	if (use_resource_root) {
		bytes = resource_root->read_file(lookup_name);
		if (bytes.is_empty() && lookup_name.get_extension().to_lower() != "til") {
			lookup_name = lookup_name.get_file() + String(".til");
			bytes = resource_root->read_file(lookup_name);
		}
	} else {
		read_nova_payload_file(lookup, bytes);
	}
	if (bytes.is_empty()) {
		UtilityFunctions::push_warning("TerrainData: tileinfo file not found at ", lookup);
		tileinfo_resource_cache.unref();
		tileinfo_resource_cache_path = String();
		return Ref<TerrainTileInfo>();
	}

	opennova::TilFile til;
	std::string error;
	if (!opennova::load_til(bytes.ptr(), static_cast<size_t>(bytes.size()), til, error)) {
		UtilityFunctions::push_warning("TerrainData: tileinfo parse failed for ",
			lookup, ": ", String(error.c_str()));
		tileinfo_resource_cache.unref();
		tileinfo_resource_cache_path = String();
		return Ref<TerrainTileInfo>();
	}

	Ref<TerrainTileInfo> resource;
	resource.instantiate();
	resource->copy_from_native(til);
	tileinfo_resource_cache = resource;
	tileinfo_resource_cache_path = lookup;
	return resource;
}

Error TerrainData::save_to_path(const String &p_path) const {
	opennova::TrnConfig trn_copy = get_trn();
	std::ostringstream oss;
	std::string error;
	if (!opennova::save_trn(oss, trn_copy, error)) {
		UtilityFunctions::push_warning("TerrainData.save_to_path: ", error.c_str());
		return ERR_FILE_CANT_WRITE;
	}
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) return ERR_FILE_CANT_WRITE;
	std::string content = oss.str();
	f->store_string(String::utf8(content.c_str(), content.size()));
	f->close();
	return OK;
}
