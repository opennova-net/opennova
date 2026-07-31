#include "nova_terrain_data.h"

#include "nova_terrain_foliage_def.h"
#include "nova_terrain_foliage_map.h"
#include "nova_terrain_tile_info.h"

#include <til/til_io.h>
#include <terrain/brush.h>
#include <terrain/cdep_constraint.h>
#include <terrain/coords.h>
#include <terrain/height_field.h>
#include <terrain/lighting.h>
#include <terrain/terrain_raycast.h>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "util/pcx_texture_bridge.h"
#include "util/nova_data_format.h"
#include "util/texture_path_resolver.h"

#include <godot_cpp/classes/file_access.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <sstream>

using namespace godot;

namespace {

// Copy a FORMAT_RF heightmap image's floats out for a CDEP kernel pass. Returns
// false (leaving out untouched) if the image is missing, the wrong format, or
// too small. memcpy keeps it free of alignment / strict-aliasing concerns.
static bool extract_heightmap_floats(const Ref<Image> &image, std::vector<float> &out, int &out_w, int &out_h) {
	if (image.is_null() || image->get_format() != Image::FORMAT_RF) {
		return false;
	}
	out_w = image->get_width();
	out_h = image->get_height();
	const int64_t count = static_cast<int64_t>(out_w) * static_cast<int64_t>(out_h);
	if (count <= 0) {
		return false;
	}
	const PackedByteArray pixels = image->get_data();
	if (pixels.size() < count * 4) {
		return false;
	}
	out.resize(static_cast<size_t>(count));
	std::memcpy(out.data(), pixels.ptr(), static_cast<size_t>(count) * sizeof(float));
	return true;
}

// Write a mutated mip-0 buffer back into the SAME Image, PRESERVING its mipmap
// state. The editor builds ImageTexture from these images and calls update(),
// which rejects a mismatched mipmap flag; stripping mipmaps (set_data with
// use_mipmaps=false on a mipmapped image) would freeze the live preview after
// the first dab. When the image has mipmaps we overwrite only mip-0 and keep the
// existing (now-stale) lower levels and the flag, exactly as the old set_pixel
// path did; otherwise we write the flat mip-0 buffer.
static void write_image_mip0_preserving_mipmaps(const Ref<Image> &image, int w, int h,
                                                Image::Format format, const uint8_t *mip0, size_t mip0_bytes) {
	if (image->has_mipmaps()) {
		PackedByteArray pixels = image->get_data();
		std::memcpy(pixels.ptrw(), mip0, mip0_bytes);
		image->set_data(w, h, true, format, pixels);
	} else {
		PackedByteArray pixels;
		pixels.resize(static_cast<int64_t>(mip0_bytes));
		std::memcpy(pixels.ptrw(), mip0, mip0_bytes);
		image->set_data(w, h, false, format, pixels);
	}
}

// Write CDEP-clamped / brushed floats back into the same Image object (FORMAT_RF)
// so the editor's shared ref and get_depth_raw16() observe the change.
static void write_heightmap_floats(const Ref<Image> &image, const std::vector<float> &heights, int w, int h) {
	write_image_mip0_preserving_mipmaps(image, w, h, Image::FORMAT_RF,
	                                    reinterpret_cast<const uint8_t *>(heights.data()),
	                                    heights.size() * sizeof(float));
}

// RGBA8 byte buffer round-trip for the colour/blend brushes. The editable
// colormap/blendmap are always FORMAT_RGBA8 (created via _create_color_image,
// loaded via normalize_image), so the brush kernels read/write raw bytes.
static bool extract_rgba8(const Ref<Image> &image, std::vector<uint8_t> &out, int &out_w, int &out_h) {
	if (image.is_null() || image->get_format() != Image::FORMAT_RGBA8) {
		return false;
	}
	out_w = image->get_width();
	out_h = image->get_height();
	const int64_t count = static_cast<int64_t>(out_w) * static_cast<int64_t>(out_h);
	if (count <= 0) {
		return false;
	}
	const PackedByteArray pixels = image->get_data();
	if (pixels.size() < count * 4) {
		return false;
	}
	out.resize(static_cast<size_t>(count) * 4);
	std::memcpy(out.data(), pixels.ptr(), out.size());
	return true;
}

static void write_rgba8(const Ref<Image> &image, const std::vector<uint8_t> &bytes, int w, int h) {
	write_image_mip0_preserving_mipmaps(image, w, h, Image::FORMAT_RGBA8, bytes.data(), bytes.size());
}

static Ref<NovaTerrainFoliageDef> foliage_def_to_object(const opennova::FoliageDef &def) {
	Ref<NovaTerrainFoliageDef> object;
	object.instantiate();
	object->copy_from_native(def);
	return object;
}

static bool foliage_def_from_variant(const Variant &value, opennova::FoliageDef &out_def) {
	if (value.get_type() == Variant::OBJECT) {
		Object *object = value;
		if (const NovaTerrainFoliageDef *def = Object::cast_to<NovaTerrainFoliageDef>(object)) {
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
	// sector id, unclamped local offset. Delegates to the shared kernel so the
	// editor brush paths (which add bounds-reject / id-clamp / local-clamp via
	// coords_editor_options) and this sampler stay one implementation.
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

// Builds a portable TerrainHeightField over the loaded CPT depth buffer + TRN
// sector layout, the shared libs/terrain sampler the runtime AI also uses. The
// height samplers don't read water, so it's left default here; the AI-grounding
// field (NovaSimulation::set_terrain_height_field) supplies the water plane.
opennova::terrain::TerrainHeightField height_field_from(const opennova::CptFile &cpt,
                                                        const opennova::TrnConfig &trn) {
	opennova::terrain::TerrainHeightField field;
	if (cpt.depth_buffer.empty()) {
		return field;
	}
	field.heightmap = cpt.depth_buffer.data();
	field.dim = static_cast<int>(std::sqrt(static_cast<double>(cpt.depth_buffer.size())));
	field.layout.sector_grid = &trn.sector_grid[0][0];
	height_field_apply_trn(field, trn);
	return field;
}

// Builds the editor-mode sector layout from the GDScript-exposed members. The
// authored extent is clamped to [1,16] to match EditorTerrainMesh.set_sector_layout
// so the bounds-reject guard rejects exactly the cells the editor mesh does.
// Caller must ensure grid has >= 256 entries.
opennova::terrain::SectorLayout editor_layout_from(const godot::PackedInt32Array &grid,
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

// Borrow the live editable heightmap's mip-0 as float32 pixels for the
// live-surface height samplers. False when no editable image is mounted, it is
// not FORMAT_RF (the guard matches extract_heightmap_floats, the brushes'
// contract), or the buffer is short; out_pixels shares the image's buffer and
// keeps the float view alive.
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

// Per-point core of the live-surface height samplers: editor world->source
// transform, then edge-clamped bilinear over the FORMAT_RF mip-0 floats. NAN
// when the point is off the active sectors. Shared by the batch
// sample_heights_world_live and the scalar sample_height_world_live so the two
// can never disagree.
float sample_live_height_at(const opennova::terrain::SectorLayout &layout,
                            const float *heights, int w, int h,
                            double world_x, double world_z) {
	const opennova::terrain::CoordsResult<double> r = opennova::terrain::coords_world_to_source<double>(
	        layout, world_x, world_z, opennova::terrain::coords_editor_options());
	if (!r.valid) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	// Round the source coords through float32 first (the scalar coords API,
	// world_to_source_coords, hands GDScript a float32 Vector2), then floor,
	// edge clamp, clamped fractions, bilinear in 64-bit.
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
// Reimpl substrate behind NovaTerrainData::raycast_terrain: one
// TerrainRaycastSampler (terrain/terrain_raycast.h) over BOTH height
// substrates — the LIVE editable FORMAT_RF image when mounted (what the
// brushes mutate and the placement raycasts must see), else the BAKED CPT
// heights — the same live-vs-baked split as sample_height_world_live vs
// get_height_world_bilinear. Kind classification runs the editor-mode
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

opennova::terrain::CoordsQuadrantLocks godot::coords_locks_from(const opennova::TrnConfig &trn) {
	const opennova::TerrainQuadrantLocks source = trn.get_quadrant_locks();
	opennova::terrain::CoordsQuadrantLocks locks{};
	for (int quadrant = 0; quadrant < static_cast<int>(source.size()); ++quadrant) {
		locks.set(quadrant, source[quadrant].x != 0, source[quadrant].y != 0);
	}
	return locks;
}

void godot::height_field_apply_trn(opennova::terrain::TerrainHeightField &field,
                                   const opennova::TrnConfig &trn) {
	field.layout.origin_x = trn.origin_x;
	field.layout.origin_y = trn.origin_y;
	field.locks = coords_locks_from(trn);
}

// ---------------------------------------------------------------------------
// Macros for texture property boilerplate
// ---------------------------------------------------------------------------

#define IMPL_TEX_PROP(field, setter, getter) \
	void NovaTerrainData::setter(const Ref<Texture2D> &p_tex) { field = p_tex; _sync_trn_texture_filenames_from_refs(); _notify_terrain_changed(); } \
	Ref<Texture2D> NovaTerrainData::getter() const { return field; }

#define BIND_TEX_PROP(prop, setter, getter) \
	ClassDB::bind_method(D_METHOD(#setter, "texture"), &NovaTerrainData::setter); \
	ClassDB::bind_method(D_METHOD(#getter), &NovaTerrainData::getter); \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, #prop, PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), #setter, #getter);

// ---------------------------------------------------------------------------
// _bind_methods
// ---------------------------------------------------------------------------

void NovaTerrainData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_trn_path", "path"), &NovaTerrainData::set_trn_path);
	ClassDB::bind_method(D_METHOD("get_trn_path"), &NovaTerrainData::get_trn_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "trn_path", PROPERTY_HINT_FILE, "*.trn"), "set_trn_path", "get_trn_path");

	ClassDB::bind_method(D_METHOD("load"), &NovaTerrainData::load);
	ClassDB::bind_method(D_METHOD("load_from_resource_root", "resource_root", "name"), &NovaTerrainData::load_from_resource_root);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaTerrainData::is_loaded);
	ClassDB::bind_method(D_METHOD("get_depth_raw16"), &NovaTerrainData::get_depth_raw16);
	ClassDB::bind_method(D_METHOD("set_heightmap_image", "image"), &NovaTerrainData::set_heightmap_image);
	ClassDB::bind_method(D_METHOD("get_heightmap_image"), &NovaTerrainData::get_heightmap_image);
	ClassDB::bind_method(D_METHOD("heightmap_image_from_raw16", "raw16"), &NovaTerrainData::heightmap_image_from_raw16);
	ClassDB::bind_method(D_METHOD("set_colormap_image", "image"), &NovaTerrainData::set_colormap_image);
	ClassDB::bind_method(D_METHOD("get_colormap_image"), &NovaTerrainData::get_colormap_image);
	ClassDB::bind_method(D_METHOD("set_blendmap_image", "image"), &NovaTerrainData::set_blendmap_image);
	ClassDB::bind_method(D_METHOD("get_blendmap_image"), &NovaTerrainData::get_blendmap_image);
	ClassDB::bind_method(D_METHOD("cdep_clamp_blocks_in_rect", "rect"), &NovaTerrainData::cdep_clamp_blocks_in_rect);
	ClassDB::bind_method(D_METHOD("cdep_count_violations"), &NovaTerrainData::cdep_count_violations);
	ClassDB::bind_method(D_METHOD("cdep_clamp_all_violations"), &NovaTerrainData::cdep_clamp_all_violations);
	ClassDB::bind_method(D_METHOD("brush_raise_lower", "cx", "cz", "radius", "amount", "hardness", "clip"),
	                     &NovaTerrainData::brush_raise_lower);
	ClassDB::bind_method(D_METHOD("brush_smooth", "cx", "cz", "radius", "strength", "hardness", "clip"),
	                     &NovaTerrainData::brush_smooth);
	ClassDB::bind_method(D_METHOD("brush_flatten", "cx", "cz", "radius", "target_height", "strength", "hardness", "clip"),
	                     &NovaTerrainData::brush_flatten);
	ClassDB::bind_method(D_METHOD("brush_sample_flatten_target", "world_x", "world_z"),
	                     &NovaTerrainData::brush_sample_flatten_target);
	ClassDB::bind_method(D_METHOD("brush_blend_paint", "channel", "cx", "cz", "radius", "strength", "hardness", "clip"),
	                     &NovaTerrainData::brush_blend_paint);
	ClassDB::bind_method(D_METHOD("brush_colormap_paint", "color", "cx", "cz", "radius", "strength", "hardness", "clip"),
	                     &NovaTerrainData::brush_colormap_paint);
	ClassDB::bind_method(D_METHOD("brush_colormap_clone", "source", "src_cx", "src_cy", "dst_cx", "dst_cy", "radius", "strength", "hardness", "clip"),
	                     &NovaTerrainData::brush_colormap_clone);
	ClassDB::bind_method(D_METHOD("brush_sample_colormap", "world_x", "world_z"),
	                     &NovaTerrainData::brush_sample_colormap);
	ClassDB::bind_method(D_METHOD("get_height", "world_pos"), &NovaTerrainData::get_height);
	ClassDB::bind_method(D_METHOD("get_height_world", "world_pos"), &NovaTerrainData::get_height_world);
	ClassDB::bind_method(D_METHOD("get_height_world_bilinear", "world_pos"), &NovaTerrainData::get_height_world_bilinear);
	ClassDB::bind_method(D_METHOD("get_colormap_color_world", "world_x", "world_z"), &NovaTerrainData::get_colormap_color_world);
	ClassDB::bind_method(D_METHOD("get_modulated_colormap_color_world", "world_x", "world_z", "light_color"),
	                     &NovaTerrainData::get_modulated_colormap_color_world);
	ClassDB::bind_method(D_METHOD("get_detail_foliage_index_world", "world_x", "world_z"),
	                     &NovaTerrainData::get_detail_foliage_index_world);
	ClassDB::bind_method(D_METHOD("get_foliage_index_world", "world_x", "world_z"), &NovaTerrainData::get_foliage_index_world);
	ClassDB::bind_method(D_METHOD("world_to_source_coords", "world_x", "world_z"), &NovaTerrainData::world_to_source_coords);
	ClassDB::bind_method(D_METHOD("world_to_runtime_source_coords", "world_x", "world_z"),
	                     &NovaTerrainData::world_to_runtime_source_coords);
	ClassDB::bind_method(D_METHOD("world_to_sector_cell", "world_x", "world_z"), &NovaTerrainData::world_to_sector_cell);
	ClassDB::bind_method(D_METHOD("sample_heights_world_live", "world_xz"), &NovaTerrainData::sample_heights_world_live);
	ClassDB::bind_method(D_METHOD("sample_height_world_live", "world_x", "world_z"),
	                     &NovaTerrainData::sample_height_world_live);
	ClassDB::bind_method(D_METHOD("raycast_terrain", "from", "to"), &NovaTerrainData::raycast_terrain);
	ClassDB::bind_method(D_METHOD("world_to_cell_source_coords", "world_x", "world_z", "row", "col"),
	                     &NovaTerrainData::world_to_cell_source_coords);
	ClassDB::bind_method(D_METHOD("get_cell_atlas_rect", "row", "col"), &NovaTerrainData::get_cell_atlas_rect);
	ClassDB::bind_method(D_METHOD("get_tile_count"), &NovaTerrainData::get_tile_count);
	ClassDB::bind_method(D_METHOD("load_foliage_indices"), &NovaTerrainData::load_foliage_indices);
	ClassDB::bind_method(D_METHOD("set_sector_grid", "value"), &NovaTerrainData::set_sector_grid);
	ClassDB::bind_method(D_METHOD("get_sector_grid"), &NovaTerrainData::get_sector_grid);
	ClassDB::bind_method(D_METHOD("get_foliage_map"), &NovaTerrainData::get_foliage_map);
	ClassDB::bind_method(D_METHOD("set_foliage_map", "value"), &NovaTerrainData::set_foliage_map);
	ClassDB::bind_method(D_METHOD("get_foliage_defs"), &NovaTerrainData::get_foliage_defs);
	ClassDB::bind_method(D_METHOD("set_foliage_defs", "value"), &NovaTerrainData::set_foliage_defs);
	ClassDB::bind_method(D_METHOD("set_trn_texture_filename", "slot_id", "filename"), &NovaTerrainData::set_trn_texture_filename);
	ClassDB::bind_method(D_METHOD("get_trn_texture_filename", "slot_id"), &NovaTerrainData::get_trn_texture_filename);
	ClassDB::bind_method(D_METHOD("set_polydata_filename", "filename"), &NovaTerrainData::set_polydata_filename);
	ClassDB::bind_method(D_METHOD("get_polydata_filename"), &NovaTerrainData::get_polydata_filename);
	ClassDB::bind_method(D_METHOD("set_tileinfo_filename", "filename"), &NovaTerrainData::set_tileinfo_filename);
	ClassDB::bind_method(D_METHOD("get_tileinfo_filename"), &NovaTerrainData::get_tileinfo_filename);
	ClassDB::bind_method(D_METHOD("get_tileinfo_resource"), &NovaTerrainData::get_tileinfo_resource);

	ClassDB::bind_method(D_METHOD("import_pcx_slot", "slot_id", "path"), &NovaTerrainData::import_pcx_slot);
	ClassDB::bind_method(D_METHOD("save_pcx_slot", "slot_id", "path"), &NovaTerrainData::save_pcx_slot);
	ClassDB::bind_method(D_METHOD("reset_pcx_slot_default", "slot_id", "width", "height"), &NovaTerrainData::reset_pcx_slot_default);
	ClassDB::bind_method(D_METHOD("get_pcx_slot_state", "slot_id"), &NovaTerrainData::get_pcx_slot_state);
	ClassDB::bind_method(D_METHOD("set_pcx_slot_state", "slot_id", "state"), &NovaTerrainData::set_pcx_slot_state);

	// Identity
	ClassDB::bind_method(D_METHOD("set_terrain_name", "value"), &NovaTerrainData::set_terrain_name);
	ClassDB::bind_method(D_METHOD("get_terrain_name"), &NovaTerrainData::get_terrain_name);
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
	ClassDB::bind_method(D_METHOD("set_detail_density", "value"), &NovaTerrainData::set_detail_density);
	ClassDB::bind_method(D_METHOD("get_detail_density"), &NovaTerrainData::get_detail_density);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_density", PROPERTY_HINT_RANGE, "1,512,1"), "set_detail_density", "get_detail_density");
	ClassDB::bind_method(D_METHOD("set_detail_density2", "value"), &NovaTerrainData::set_detail_density2);
	ClassDB::bind_method(D_METHOD("get_detail_density2"), &NovaTerrainData::get_detail_density2);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_density2", PROPERTY_HINT_RANGE, "1,128,1"), "set_detail_density2", "get_detail_density2");

	// Sectors
	ADD_GROUP("Sectors", "");
	ClassDB::bind_method(D_METHOD("set_sector_count", "value"), &NovaTerrainData::set_sector_count);
	ClassDB::bind_method(D_METHOD("get_sector_count"), &NovaTerrainData::get_sector_count);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sector_count", PROPERTY_HINT_RANGE, "1,16,1"), "set_sector_count", "get_sector_count");
	ClassDB::bind_method(D_METHOD("set_sector_rows", "value"), &NovaTerrainData::set_sector_rows);
	ClassDB::bind_method(D_METHOD("get_sector_rows"), &NovaTerrainData::get_sector_rows);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sector_rows", PROPERTY_HINT_RANGE, "1,16,1"), "set_sector_rows", "get_sector_rows");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "sector_grid"), "set_sector_grid", "get_sector_grid");
	ClassDB::bind_method(D_METHOD("set_origin_x", "value"), &NovaTerrainData::set_origin_x);
	ClassDB::bind_method(D_METHOD("get_origin_x"), &NovaTerrainData::get_origin_x);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "origin_x"), "set_origin_x", "get_origin_x");
	ClassDB::bind_method(D_METHOD("set_origin_y", "value"), &NovaTerrainData::set_origin_y);
	ClassDB::bind_method(D_METHOD("get_origin_y"), &NovaTerrainData::get_origin_y);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "origin_y"), "set_origin_y", "get_origin_y");
	ClassDB::bind_method(D_METHOD("set_wrap_x", "value"), &NovaTerrainData::set_wrap_x);
	ClassDB::bind_method(D_METHOD("get_wrap_x"), &NovaTerrainData::get_wrap_x);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wrap_x"), "set_wrap_x", "get_wrap_x");
	ClassDB::bind_method(D_METHOD("set_wrap_y", "value"), &NovaTerrainData::set_wrap_y);
	ClassDB::bind_method(D_METHOD("get_wrap_y"), &NovaTerrainData::get_wrap_y);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wrap_y"), "set_wrap_y", "get_wrap_y");
	ClassDB::bind_method(D_METHOD("get_quadrant_locks"),
	                     &NovaTerrainData::get_quadrant_locks);

	// Environment
	ADD_GROUP("Environment", "");
	ClassDB::bind_method(D_METHOD("set_water_height", "value"), &NovaTerrainData::set_water_height);
	ClassDB::bind_method(D_METHOD("get_water_height"), &NovaTerrainData::get_water_height);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "water_height"), "set_water_height", "get_water_height");
	ClassDB::bind_method(D_METHOD("set_horizon", "value"), &NovaTerrainData::set_horizon);
	ClassDB::bind_method(D_METHOD("get_horizon"), &NovaTerrainData::get_horizon);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "horizon"), "set_horizon", "get_horizon");

	// Foliage
	ADD_GROUP("Foliage", "");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "foliage_defs", PROPERTY_HINT_ARRAY_TYPE, "NovaTerrainFoliageDef"),
	             "set_foliage_defs",
	             "get_foliage_defs");

	// Sector/atlas layout constants (single-sourced from libs/terrain_query
	// terrain/coords.h; see the header declarations).
	BIND_CONSTANT(SECTOR_SIZE);
	BIND_CONSTANT(SECTOR_GRID_DIM);
	BIND_CONSTANT(ATLAS_SIZE);
	BIND_CONSTANT(SECTOR_ID_MAX);

	ADD_SIGNAL(MethodInfo("terrain_changed"));
}

NovaTerrainData::NovaTerrainData() {
	sector_grid.resize(256);
}
NovaTerrainData::~NovaTerrainData() {}

// ---------------------------------------------------------------------------
// PCX map data — palette-indexed slots (charmap, foliagemap). Palette and
// indices are owned internally; slot_id dispatches to the right field set.
// ---------------------------------------------------------------------------

static void _fill_grayscale_palette(uint8_t palette[256][3]) {
	for (int i = 0; i < 256; i++) {
		palette[i][0] = static_cast<uint8_t>(i);
		palette[i][1] = static_cast<uint8_t>(i);
		palette[i][2] = static_cast<uint8_t>(i);
	}
}

static void _fill_default_charmap_palette(uint8_t palette[256][3]) {
	_fill_grayscale_palette(palette);
	const struct {
		uint8_t index;
		uint8_t r;
		uint8_t g;
		uint8_t b;
	} entries[] = {
		{0, 0, 0, 0},        // Null
		{1, 153, 118, 61},   // Dirt
		{2, 0, 210, 0},      // Grass
		{3, 204, 239, 244},  // Snow
		{4, 152, 152, 152},  // Cement
		{5, 255, 255, 0},    // Sand
		{6, 255, 128, 0},    // PackedDirt
		{7, 0, 0, 255},      // Unused
		{8, 255, 0, 0},      // Unused
		{9, 114, 64, 0},     // Mud
		{10, 160, 190, 219}, // Ice
		{11, 161, 0, 161},   // Unused
		{12, 255, 0, 186},   // Rock/Stone
		{13, 158, 78, 0},    // Wood
		{14, 0, 201, 203},   // Metal
		{15, 255, 255, 255}, // Unused
	};
	for (const auto &entry : entries) {
		palette[entry.index][0] = entry.r;
		palette[entry.index][1] = entry.g;
		palette[entry.index][2] = entry.b;
	}
}

static uint8_t _default_fill_index_for_pcx_slot(const String &slot_id) {
	return slot_id == "charmap" ? 1 : 0;
}

// Slot dispatch — maps slot_id to the per-slot state. Extend here when new
// PCX-backed slots (e.g. additional surface maps) are introduced.
struct NovaTerrainData::PcxSlotRefs {
	std::vector<uint8_t>* indices;
	uint8_t (*palette)[3];
	int* width;
	int* height;
	Ref<Texture2D>* tex;
	std::string* trn_filename;
};

bool NovaTerrainData::_resolve_pcx_slot(const String &slot_id, PcxSlotRefs &out) {
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

void NovaTerrainData::_sync_foliage_map_resource_from_slot() {
	if (foliage_map_resource.is_null()) {
		foliage_map_resource.instantiate();
	}
	foliage_map_resource->copy_from_native(
			foliage_map_from_slot_data(foliagemap_indices, foliagemap_palette, foliagemap_width, foliagemap_height));
}

void NovaTerrainData::_apply_foliage_map_to_slot(const opennova::FoliageMap &map) {
	const opennova::FoliageMap normalized = opennova::foliage_has_size(map)
			? map
			: opennova::foliage_make_default_map(opennova::FOLIAGE_HEIGHTMAP_SIZE, opennova::FOLIAGE_HEIGHTMAP_SIZE, 0);
	foliagemap_width = normalized.width;
	foliagemap_height = normalized.height;
	foliagemap_indices = normalized.indices;
	std::memcpy(foliagemap_palette, normalized.palette, sizeof(foliagemap_palette));
	foliagemap_tex = opennova::build_indexed_texture(foliagemap_indices, foliagemap_palette, foliagemap_width, foliagemap_height);
}

Error NovaTerrainData::import_pcx_slot(const String &slot_id, const String &path) {
	PackedByteArray bytes;
	if (!read_nova_payload_file(path, bytes)) {
		UtilityFunctions::push_error("import_pcx_slot: cannot open ", path);
		return ERR_FILE_CANT_OPEN;
	}

	return _import_pcx_slot_bytes(slot_id, path.get_file(), bytes);
}

Error NovaTerrainData::_import_pcx_slot_bytes(const String &slot_id, const String &filename, const PackedByteArray &bytes) {
	PcxSlotRefs refs;
	if (!_resolve_pcx_slot(slot_id, refs)) {
		UtilityFunctions::push_error("import_pcx_slot: unknown slot '", slot_id, "'");
		return ERR_INVALID_PARAMETER;
	}

	int w = 0, h = 0;
	if (!opennova::decode_pcx_with_palette(bytes.ptr(), bytes.size(),
	                                        *refs.indices, refs.palette, w, h)) {
		UtilityFunctions::push_error("import_pcx_slot: decode failed for ", filename);
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

Error NovaTerrainData::save_pcx_slot(const String &slot_id, const String &path) const {
	PcxSlotRefs refs;
	if (!const_cast<NovaTerrainData*>(this)->_resolve_pcx_slot(slot_id, refs)) {
		UtilityFunctions::push_error("save_pcx_slot: unknown slot '", slot_id, "'");
		return ERR_INVALID_PARAMETER;
	}
	int w = *refs.width, h = *refs.height;
	if (w <= 0 || h <= 0 || (int)refs.indices->size() < w * h) {
		UtilityFunctions::push_error("save_pcx_slot: '", slot_id, "' not initialized");
		return ERR_UNCONFIGURED;
	}
	PackedByteArray bytes = opennova::encode_pcx_indices(refs.indices->data(), w, h, refs.palette);
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		UtilityFunctions::push_error("save_pcx_slot: cannot open for write ", path);
		return ERR_FILE_CANT_WRITE;
	}
	f->store_buffer(bytes);
	f->close();
	return OK;
}

void NovaTerrainData::reset_pcx_slot_default(const String &slot_id, int width, int height) {
	PcxSlotRefs refs;
	if (!_resolve_pcx_slot(slot_id, refs)) return;
	if (width <= 0 || height <= 0) return;
	*refs.width = width;
	*refs.height = height;
	refs.indices->assign(width * height, _default_fill_index_for_pcx_slot(slot_id));
	if (slot_id == "charmap") {
		_fill_default_charmap_palette(refs.palette);
	} else {
		_fill_grayscale_palette(refs.palette);
	}
	refs.trn_filename->clear();
	*refs.tex = opennova::build_indexed_texture(*refs.indices, refs.palette, width, height);
	if (slot_id == "foliagemap") {
		_sync_foliage_map_resource_from_slot();
	}
	_notify_terrain_changed();
}

Dictionary NovaTerrainData::get_pcx_slot_state(const String &slot_id) const {
	PcxSlotRefs refs;
	if (!const_cast<NovaTerrainData *>(this)->_resolve_pcx_slot(slot_id, refs)) {
		return Dictionary();
	}
	const int width = *refs.width;
	const int height = *refs.height;
	if (width <= 0 || height <= 0 || refs.indices->size() < static_cast<size_t>(width * height)) {
		return Dictionary();
	}

	PackedByteArray indices;
	indices.resize(static_cast<int64_t>(refs.indices->size()));
	if (!refs.indices->empty()) {
		std::memcpy(indices.ptrw(), refs.indices->data(), refs.indices->size());
	}

	PackedByteArray palette;
	palette.resize(256 * 3);
	std::memcpy(palette.ptrw(), refs.palette, 256 * 3);

	Dictionary result;
	result["width"] = width;
	result["height"] = height;
	result["indices"] = indices;
	result["palette"] = palette;
	return result;
}

void NovaTerrainData::set_pcx_slot_state(const String &slot_id, const Dictionary &state) {
	PcxSlotRefs refs;
	if (!_resolve_pcx_slot(slot_id, refs)) {
		UtilityFunctions::push_error("set_pcx_slot_state: unknown slot '", slot_id, "'");
		return;
	}

	const int width = std::max(static_cast<int>(state.get("width", *refs.width)), 1);
	const int height = std::max(static_cast<int>(state.get("height", *refs.height)), 1);
	const int expected = width * height;
	const PackedByteArray indices = state.get("indices", PackedByteArray());
	const PackedByteArray palette = state.get("palette", PackedByteArray());
	if (indices.size() < expected || palette.size() < 256 * 3) {
		UtilityFunctions::push_error("set_pcx_slot_state: invalid payload for slot '", slot_id, "'");
		return;
	}

	*refs.width = width;
	*refs.height = height;
	refs.indices->resize(static_cast<size_t>(expected));
	std::memcpy(refs.indices->data(), indices.ptr(), static_cast<size_t>(expected));
	std::memcpy(refs.palette, palette.ptr(), 256 * 3);
	*refs.tex = opennova::build_indexed_texture(*refs.indices, refs.palette, width, height);
	if (slot_id == "foliagemap") {
		_sync_foliage_map_resource_from_slot();
	}
	_notify_terrain_changed();
}

// ---------------------------------------------------------------------------
// Property accessors
// ---------------------------------------------------------------------------

void NovaTerrainData::set_trn_path(const String &p_path) { trn_path = p_path; }
String NovaTerrainData::get_trn_path() const { return trn_path; }

void NovaTerrainData::set_terrain_name(const String &p_name) { terrain_name = p_name; _notify_terrain_changed(); }
String NovaTerrainData::get_terrain_name() const { return terrain_name; }

void NovaTerrainData::set_colormap(const Ref<Texture2D> &p_tex) {
	colormap = p_tex;
	_invalidate_colormap_cpu_cache();
	_sync_trn_texture_filenames_from_refs();
	_notify_terrain_changed();
}

Ref<Texture2D> NovaTerrainData::get_colormap() const { return colormap; }

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

void NovaTerrainData::set_detail_density(int p_val) { detail_density = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_detail_density() const { return detail_density; }
void NovaTerrainData::set_detail_density2(int p_val) { detail_density2 = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_detail_density2() const { return detail_density2; }
void NovaTerrainData::set_sector_count(int p_val) { sector_count = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_sector_count() const { return sector_count; }
void NovaTerrainData::set_sector_rows(int p_val) { sector_rows = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_sector_rows() const { return sector_rows; }
void NovaTerrainData::set_origin_x(int p_val) { origin_x = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_origin_x() const { return origin_x; }
void NovaTerrainData::set_origin_y(int p_val) { origin_y = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_origin_y() const { return origin_y; }
void NovaTerrainData::set_water_height(int p_val) { water_height = p_val; _notify_terrain_changed(); }
int NovaTerrainData::get_water_height() const { return water_height; }
void NovaTerrainData::set_wrap_x(bool p_val) { wrap_x = p_val; _notify_terrain_changed(); }
bool NovaTerrainData::get_wrap_x() const { return wrap_x; }
void NovaTerrainData::set_wrap_y(bool p_val) { wrap_y = p_val; _notify_terrain_changed(); }
bool NovaTerrainData::get_wrap_y() const { return wrap_y; }
PackedInt32Array NovaTerrainData::get_quadrant_locks() const {
	PackedInt32Array locks;
	locks.resize(8);
	const opennova::TerrainQuadrantLocks source = trn.get_quadrant_locks();
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		locks.set(quadrant * 2, source[quadrant].x);
		locks.set(quadrant * 2 + 1, source[quadrant].y);
	}
	return locks;
}
void NovaTerrainData::set_horizon(double p_val) { horizon = p_val; _notify_terrain_changed(); }
double NovaTerrainData::get_horizon() const { return horizon; }
void NovaTerrainData::set_sector_grid(const PackedInt32Array &p_grid) {
	sector_grid.resize(256);
	for (int i = 0; i < 256; i++) {
		sector_grid.set(i, i < p_grid.size() ? p_grid[i] : 0);
	}
	_notify_terrain_changed();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

String NovaTerrainData::_texture_to_filename(const Ref<Texture2D> &p_tex) {
	if (p_tex.is_null()) return "";
	String path = p_tex->get_path();
	if (path.is_empty()) return "";
	return path.get_file();
}

void NovaTerrainData::_invalidate_colormap_cpu_cache() const {
	colormap_cpu_image.unref();
	colormap_cpu_width = 0;
	colormap_cpu_height = 0;
}

bool NovaTerrainData::_ensure_colormap_cpu_cache() const {
	if (colormap_cpu_image.is_valid() && colormap_cpu_width > 0 && colormap_cpu_height > 0) {
		return true;
	}
	if (colormap.is_null()) {
		return false;
	}

	Ref<Image> image = colormap->get_image();
	if (image.is_null() || image->is_empty()) {
		return false;
	}
	if (image->is_compressed()) {
		const Error err = image->decompress();
		if (err != OK) {
			return false;
		}
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	colormap_cpu_width = image->get_width();
	colormap_cpu_height = image->get_height();
	if (colormap_cpu_width <= 0 || colormap_cpu_height <= 0) {
		_invalidate_colormap_cpu_cache();
		return false;
	}
	colormap_cpu_image = image;
	return true;
}

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
// filename fields, which are either (a) owned by Ref<Texture2D> setters
// (covered by _sync_trn_texture_filenames_from_refs below), or (b) set
// explicitly via set_trn_texture_filename().
void NovaTerrainData::_sync_trn_scalars_from_properties() {
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
// silently overwrite filenames that set_trn_texture_filename had
// explicitly set (the Ref still carries its source path even after the
// user asked for a new export name).
void NovaTerrainData::_sync_trn_texture_filenames_from_refs() {
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

void NovaTerrainData::_notify_terrain_changed() {
	// Always sync scalars — cheap and needed so `trn` stays consistent for
	// saves. Deliberately DO NOT sync texture filenames here: that would
	// clobber filenames set via set_trn_texture_filename (e.g. Save Project's
	// rename-from-imported-to-exported step). Texture Ref setters call the
	// filename sync themselves.
	_sync_trn_scalars_from_properties();
	emit_signal("terrain_changed");
	emit_changed();
}

// ---------------------------------------------------------------------------
// Texture loading helper — delegates to shared util/texture_path_resolver.h.
// res:// textures load through ResourceLoader (imported .ctex / NovaTexture
// loader), so they survive export; absolute paths decode raw bytes.
// ---------------------------------------------------------------------------

static Ref<Texture2D> _load_texture_from_dir(const String &dir, const String &filename) {
	return opennova::load_texture_from_dir(dir, filename);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

Error NovaTerrainData::load() {
	loaded = false;
	cpt = opennova::CptFile();
	trn = opennova::TrnConfig();
	resource_root.unref();

	if (trn_path.is_empty()) {
		UtilityFunctions::push_warning("NovaTerrainData: trn_path must be set before loading");
		return ERR_INVALID_PARAMETER;
	}

	PackedByteArray trn_bytes;
	if (!read_nova_payload_file(trn_path, trn_bytes)) {
		UtilityFunctions::push_warning("NovaTerrainData: Cannot open TRN: ", trn_path);
		return ERR_FILE_CANT_READ;
	}
	std::string trn_content(reinterpret_cast<const char *>(trn_bytes.ptr()), static_cast<size_t>(trn_bytes.size()));

	return _load_from_trn_text(trn_content, trn_path);
}

Error NovaTerrainData::load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	loaded = false;
	cpt = opennova::CptFile();
	trn = opennova::TrnConfig();
	resource_root.unref();

	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		UtilityFunctions::push_warning("NovaTerrainData: resource root must be configured before loading");
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		UtilityFunctions::push_warning("NovaTerrainData: resource filename is empty");
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray trn_bytes = p_resource_root->read_file(file);
	if (trn_bytes.is_empty()) {
		UtilityFunctions::push_warning("NovaTerrainData: Cannot open TRN from resource root: ", file);
		return ERR_FILE_CANT_READ;
	}

	trn_path = file;
	resource_root = p_resource_root;
	const std::string trn_content(reinterpret_cast<const char *>(trn_bytes.ptr()), static_cast<size_t>(trn_bytes.size()));
	return _load_from_trn_text(trn_content, file);
}

Error NovaTerrainData::_load_from_trn_text(const std::string &trn_content, const String &source_label) {
	(void)source_label;
	std::string error;
	std::istringstream trn_stream(trn_content);
	if (!opennova::load_trn(trn_stream, trn, error)) {
		UtilityFunctions::push_warning("NovaTerrainData: TRN parse failed: ", error.c_str());
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
			UtilityFunctions::push_warning("NovaTerrainData: ", slot, " texture '", filename,
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
						"NovaTerrainData: failed to import ", slot_id,
						" from mounted resource '", mounted_filename, "' (error ", err, "); resetting slot to default");
					reset_pcx_slot_default(slot_id, 1024, 1024);
				}
			} else {
				if (!filename.is_empty())
					UtilityFunctions::push_warning("NovaTerrainData: ", slot_id, " not found for '", filename, "' under ", trn_dir);
				reset_pcx_slot_default(slot_id, 1024, 1024);
			}
		} else {
			String resolved = opennova::resolve_texture_path(trn_dir, filename);
			if (!resolved.is_empty()) {
				Error err = import_pcx_slot(slot_id, resolved);
				if (err != OK) {
					UtilityFunctions::push_warning(
						"NovaTerrainData: failed to import ", slot_id,
						" from '", resolved, "' (error ", err, "); resetting slot to default");
					reset_pcx_slot_default(slot_id, 1024, 1024);
				}
			} else {
				if (!filename.is_empty())
					UtilityFunctions::push_warning("NovaTerrainData: ", slot_id, " not found for '", filename, "' under ", trn_dir);
				reset_pcx_slot_default(slot_id, 1024, 1024);
			}
		}
	}
	tilestrip_tex = load_tex("tilestrip", tilestrip_filename);
	trn.tilestrip = tilestrip_filename.utf8().get_data();
	_invalidate_colormap_cpu_cache();

	// CPT is an export-time bake artefact; editor projects legitimately save
	// a .trn without one (see plan: "Make CPT optional"). Missing/empty
	// polydata is not an error — load() still succeeds, cpt stays empty, and
	// consumers that need CPT (NovaTerrain::_build_terrain @ nova_terrain.cpp:571,
	// get_height* guards @ nova_terrain_data.cpp:714/739/758) already early-out
	// gracefully.
	if (trn.polydata.empty()) {
		loaded = true;
		UtilityFunctions::print_verbose("NovaTerrainData: Loaded terrain '", terrain_name,
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
		UtilityFunctions::push_warning("NovaTerrainData: CPT '", cpt_path,
			"' missing; continuing without baked terrain (run Export to generate it)");
		return OK;
	}

	if (!opennova::load_cpt(cpt_bytes.ptr(), cpt_bytes.size(), cpt, error)) {
		UtilityFunctions::push_warning("NovaTerrainData: CPT parse failed: ", error.c_str());
		return ERR_FILE_CANT_READ;
	}

	loaded = true;

	UtilityFunctions::print_verbose("NovaTerrainData: Loaded terrain '", terrain_name,
		"' — ", static_cast<int>(cpt.tiles.size()), " tiles, depth buffer ",
		static_cast<int>(cpt.depth_buffer.size()), " pixels");

	return OK;
}

bool NovaTerrainData::is_loaded() const {
	return loaded;
}

PackedByteArray NovaTerrainData::get_depth_raw16() const {
	PackedByteArray out;

	// Prefer the live editable heightmap (FORMAT_RF, 1 float per cell). The
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

void NovaTerrainData::set_heightmap_image(const Ref<Image> &p_image) {
	heightmap_image = p_image;
}

Ref<Image> NovaTerrainData::get_heightmap_image() const {
	return heightmap_image;
}

void NovaTerrainData::set_colormap_image(const Ref<Image> &p_image) {
	colormap_image = p_image;
}

Ref<Image> NovaTerrainData::get_colormap_image() const {
	return colormap_image;
}

void NovaTerrainData::set_blendmap_image(const Ref<Image> &p_image) {
	blendmap_image = p_image;
}

Ref<Image> NovaTerrainData::get_blendmap_image() const {
	return blendmap_image;
}

int NovaTerrainData::cdep_count_violations() const {
	std::vector<float> heights;
	int w = 0, h = 0;
	if (!extract_heightmap_floats(heightmap_image, heights, w, h)) {
		return 0;
	}
	return opennova::terrain::cdep_count_violations(heights.data(), w, h);
}

int NovaTerrainData::cdep_clamp_all_violations() {
	std::vector<float> heights;
	int w = 0, h = 0;
	if (!extract_heightmap_floats(heightmap_image, heights, w, h)) {
		return 0;
	}
	const int clamped = opennova::terrain::cdep_clamp_all_violations(heights.data(), w, h);
	if (clamped > 0) {
		write_heightmap_floats(heightmap_image, heights, w, h);
	}
	return clamped;
}

int NovaTerrainData::cdep_clamp_blocks_in_rect(const Rect2i &p_rect) {
	std::vector<float> heights;
	int w = 0, h = 0;
	if (!extract_heightmap_floats(heightmap_image, heights, w, h)) {
		return 0;
	}
	const opennova::terrain::CdepRect rect{
		p_rect.position.x,
		p_rect.position.y,
		p_rect.position.x + p_rect.size.x,
		p_rect.position.y + p_rect.size.y,
	};
	const int clamped = opennova::terrain::cdep_clamp_blocks_in_rect(heights.data(), w, h, rect);
	if (clamped > 0) {
		write_heightmap_floats(heightmap_image, heights, w, h);
	}
	return clamped;
}

// Height brushes (libs/terrain/brush.h). Each pulls the editable FORMAT_RF
// heightmap once, mutates the dab via the shared kernel, and writes it back to
// the SAME Image so the editor's shared ref and get_depth_raw16() stay current.
// The brush session calls cdep_clamp_blocks_in_rect after each dab.
void NovaTerrainData::brush_raise_lower(int cx, int cz, int radius, double amount, double hardness,
                                        const Rect2i &p_clip) {
	std::vector<float> heights;
	int w = 0, h = 0;
	if (!extract_heightmap_floats(heightmap_image, heights, w, h)) {
		return;
	}
	const opennova::terrain::BrushRect clip{p_clip.position.x, p_clip.position.y, p_clip.size.x, p_clip.size.y};
	if (opennova::terrain::brush_raise_lower(heights.data(), w, h, cx, cz, radius, amount, hardness, clip)) {
		write_heightmap_floats(heightmap_image, heights, w, h);
	}
}

void NovaTerrainData::brush_smooth(int cx, int cz, int radius, double strength, double hardness,
                                   const Rect2i &p_clip) {
	std::vector<float> heights;
	int w = 0, h = 0;
	if (!extract_heightmap_floats(heightmap_image, heights, w, h)) {
		return;
	}
	const opennova::terrain::BrushRect clip{p_clip.position.x, p_clip.position.y, p_clip.size.x, p_clip.size.y};
	if (opennova::terrain::brush_smooth(heights.data(), w, h, cx, cz, radius, strength, hardness, clip)) {
		write_heightmap_floats(heightmap_image, heights, w, h);
	}
}

void NovaTerrainData::brush_flatten(int cx, int cz, int radius, double target_height, double strength,
                                    double hardness, const Rect2i &p_clip) {
	std::vector<float> heights;
	int w = 0, h = 0;
	if (!extract_heightmap_floats(heightmap_image, heights, w, h)) {
		return;
	}
	const opennova::terrain::BrushRect clip{p_clip.position.x, p_clip.position.y, p_clip.size.x, p_clip.size.y};
	if (opennova::terrain::brush_flatten(heights.data(), w, h, cx, cz, radius, target_height, strength, hardness, clip)) {
		write_heightmap_floats(heightmap_image, heights, w, h);
	}
}

double NovaTerrainData::brush_sample_flatten_target(double world_x, double world_z) const {
	// Mirrors TerrainEditorBrushes.sample_flatten_target: nearest-pixel sample of
	// the editable heightmap at int(world)-truncated, edge-clamped coords.
	if (heightmap_image.is_null() || heightmap_image->get_format() != Image::FORMAT_RF) {
		return 0.0;
	}
	const int img_w = heightmap_image->get_width();
	const int img_h = heightmap_image->get_height();
	if (img_w <= 0 || img_h <= 0) {
		return 0.0;
	}
	const int sx = std::clamp(static_cast<int>(world_x), 0, img_w - 1);
	const int sz = std::clamp(static_cast<int>(world_z), 0, img_h - 1);
	return static_cast<double>(heightmap_image->get_pixel(sx, sz).r);
}

// Colour / blend brushes (libs/terrain/brush.h). blend paints the editable
// detail-blend buffer; colormap paint/clone the editable colour buffer. Each
// pulls the RGBA8 bytes once, mutates the dab via the byte-parity kernel, and
// writes back to the SAME Image (only when a pixel was touched).
void NovaTerrainData::brush_blend_paint(int channel, int cx, int cz, int radius, double strength,
                                        double hardness, const Rect2i &p_clip) {
	std::vector<uint8_t> bytes;
	int w = 0, h = 0;
	if (!extract_rgba8(blendmap_image, bytes, w, h)) {
		return;
	}
	const opennova::terrain::BrushRect clip{p_clip.position.x, p_clip.position.y, p_clip.size.x, p_clip.size.y};
	if (opennova::terrain::brush_blend_paint(bytes.data(), w, h, channel, cx, cz, radius, strength, hardness, clip)) {
		write_rgba8(blendmap_image, bytes, w, h);
	}
}

void NovaTerrainData::brush_colormap_paint(const Color &color, int cx, int cz, int radius, double strength,
                                           double hardness, const Rect2i &p_clip) {
	std::vector<uint8_t> bytes;
	int w = 0, h = 0;
	if (!extract_rgba8(colormap_image, bytes, w, h)) {
		return;
	}
	const opennova::terrain::BrushRect clip{p_clip.position.x, p_clip.position.y, p_clip.size.x, p_clip.size.y};
	if (opennova::terrain::brush_colormap_paint(bytes.data(), w, h, color.r, color.g, color.b, color.a,
	                                            cx, cz, radius, strength, hardness, clip)) {
		write_rgba8(colormap_image, bytes, w, h);
	}
}

void NovaTerrainData::brush_colormap_clone(const Ref<Image> &source, int src_cx, int src_cy, int dst_cx,
                                           int dst_cy, int radius, double strength, double hardness,
                                           const Rect2i &p_clip) {
	std::vector<uint8_t> dst;
	int w = 0, h = 0;
	if (!extract_rgba8(colormap_image, dst, w, h)) {
		return;
	}
	std::vector<uint8_t> src;
	int sw = 0, sh = 0;
	if (!extract_rgba8(source, src, sw, sh)) {
		return;
	}
	const opennova::terrain::BrushRect clip{p_clip.position.x, p_clip.position.y, p_clip.size.x, p_clip.size.y};
	if (opennova::terrain::brush_colormap_clone(dst.data(), w, h, src.data(), sw, sh, src_cx, src_cy,
	                                            dst_cx, dst_cy, radius, strength, hardness, clip)) {
		write_rgba8(colormap_image, dst, w, h);
	}
}

Color NovaTerrainData::brush_sample_colormap(double world_x, double world_z) const {
	// Mirrors TerrainEditorBrushes.sample_colormap: nearest-pixel get_pixel at
	// int-truncated, edge-clamped coords (single pixel, so no full-buffer copy).
	if (colormap_image.is_null() || colormap_image->get_format() != Image::FORMAT_RGBA8) {
		return Color(0.0f, 0.0f, 0.0f, 1.0f);
	}
	const int img_w = colormap_image->get_width();
	const int img_h = colormap_image->get_height();
	if (img_w <= 0 || img_h <= 0) {
		return Color(0.0f, 0.0f, 0.0f, 1.0f);
	}
	const int sx = std::clamp(static_cast<int>(world_x), 0, img_w - 1);
	const int sz = std::clamp(static_cast<int>(world_z), 0, img_h - 1);
	return colormap_image->get_pixel(sx, sz);
}

Ref<Image> NovaTerrainData::heightmap_image_from_raw16(const PackedByteArray &p_raw16) const {
	const int64_t pixel_count = p_raw16.size() / 2;
	const int side = static_cast<int>(std::llround(std::sqrt(static_cast<double>(pixel_count))));
	if (side <= 0 || static_cast<int64_t>(side) * side != pixel_count) {
		return Ref<Image>();
	}

	PackedByteArray floats;
	floats.resize(pixel_count * 4);
	uint8_t *dst = floats.ptrw();
	const uint8_t *src = p_raw16.ptr();
	for (int64_t i = 0; i < pixel_count; ++i) {
		const int low = src[i * 2];
		const int high = src[i * 2 + 1];
		const float height = static_cast<float>(static_cast<double>(low | (high << 8)) / 256.0);
		std::memcpy(dst + i * 4, &height, sizeof(float));
	}
	return Image::create_from_data(side, side, false, Image::FORMAT_RF, floats);
}

// The three height queries now delegate to the shared libs/terrain sampler
// (terrain/height_field.h) so the runtime AI grounding + the editor sample one
// implementation. Bodies were lifted verbatim into height_field.cpp; this class
// keeps only the loaded-guard + world->Godot coordinate boundary.
float NovaTerrainData::get_height(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return 0.0f;
	return opennova::terrain::height_field_height_bilinear(height_field_from(cpt, trn),
	                                                        p_world_pos.x, p_world_pos.z);
}

float NovaTerrainData::get_height_world(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return 0.0f;
	return opennova::terrain::height_field_height_world(height_field_from(cpt, trn),
	                                                     p_world_pos.x, p_world_pos.z);
}

float NovaTerrainData::get_height_world_bilinear(const Vector3 &p_world_pos) const {
	if (!loaded || cpt.depth_buffer.empty()) return 0.0f;
	return opennova::terrain::height_field_height_world_bilinear(height_field_from(cpt, trn),
	                                                              p_world_pos.x, p_world_pos.z);
}

Color NovaTerrainData::get_colormap_color_world(float world_x, float world_z) const {
	// Engine: Terrain_GetModulatedColorAtPos@0x005C5FE0 indexes the colormap
	// directly as x & 0x3FF, (-z) & 0x3FF. It does not go through sector-grid
	// quadrant remapping; the foliage render-emitter caller is still pending a
	// verified retail anchor.
	if (!_ensure_colormap_cpu_cache()) {
		return Color(1.0f, 1.0f, 1.0f, 1.0f);
	}

	const int x = static_cast<int>(std::floor(world_x));
	const int z = static_cast<int>(std::floor(-world_z));
	const int sample_x = ((x % colormap_cpu_width) + colormap_cpu_width) % colormap_cpu_width;
	const int sample_z = ((z % colormap_cpu_height) + colormap_cpu_height) % colormap_cpu_height;
	return colormap_cpu_image->get_pixel(sample_x, sample_z);
}

Color NovaTerrainData::get_modulated_colormap_color_world(float world_x,
                                                          float world_z,
                                                          const Color &light_color) const {
	auto to_byte = [](float value) -> uint32_t {
		return static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
	};
	const Color base = get_colormap_color_world(world_x, world_z);
	const uint32_t base_argb = (to_byte(base.a) << 24) | (to_byte(base.r) << 16) |
	                           (to_byte(base.g) << 8) | to_byte(base.b);
	const uint32_t light_argb = 0xFF000000u | (to_byte(light_color.r) << 16) |
	                            (to_byte(light_color.g) << 8) | to_byte(light_color.b);
	const uint32_t modulated = opennova::terrain::terrain_modulate_color_argb(base_argb, light_argb);
	return Color(static_cast<float>((modulated >> 16) & 0xFFu) / 255.0f,
	             static_cast<float>((modulated >> 8) & 0xFFu) / 255.0f,
	             static_cast<float>(modulated & 0xFFu) / 255.0f,
	             static_cast<float>((modulated >> 24) & 0xFFu) / 255.0f);
}

Vector2 NovaTerrainData::world_to_source_coords(double world_x, double world_z) const {
	// Editor brush/eyedropper world->atlas mapping. Mirrors
	// EditorTerrainMesh.world_to_source_coords: bounds-reject + clampi(id,0,4) +
	// clampf(local, 0, 512-0.001), computed in double (GDScript float is 64-bit)
	// then stored to a float32 Vector2 exactly as the GDScript original did.
	// Returns the (-1,-1) sentinel for out-of-extent / empty cells.
	if (sector_grid.size() < 256) {
		return Vector2(-1.0f, -1.0f);
	}
	const opennova::terrain::SectorLayout layout =
	        editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	const opennova::terrain::CoordsResult<double> r = opennova::terrain::coords_world_to_source<double>(
	        layout, world_x, world_z, opennova::terrain::coords_editor_options());
	if (!r.valid) {
		return Vector2(-1.0f, -1.0f);
	}
	return Vector2(static_cast<real_t>(r.source_x), static_cast<real_t>(r.source_z));
}

Vector2 NovaTerrainData::world_to_runtime_source_coords(float world_x, float world_z) const {
	if (sector_grid.size() < 256) {
		return Vector2(-1.0f, -1.0f);
	}
	const opennova::terrain::SectorLayout layout =
	        editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	const opennova::terrain::CoordsResult<float> r =
	        opennova::terrain::coords_world_to_source<float>(
	                layout, world_x, world_z, opennova::terrain::coords_runtime_options());
	if (!r.valid) {
		return Vector2(-1.0f, -1.0f);
	}
	return Vector2(r.source_x, r.source_z);
}

Vector2i NovaTerrainData::world_to_sector_cell(double world_x, double world_z) const {
	// World -> authored sector-grid cell. Mirrors EditorTerrainMesh's
	// extent-guarded cell lookup: floor(world / SECTOR_SIZE) minus the origin,
	// the (-1,-1) sentinel outside the authored rows/cols, and NO grid-value
	// check (an empty cell still reports its row/col). An unloaded document has
	// sector_rows == 0, so every point rejects (the mesh's degenerate-bounds
	// guard).
	const int col = static_cast<int>(std::floor(world_x / static_cast<double>(SECTOR_SIZE))) - origin_x;
	const int row = static_cast<int>(std::floor(world_z / static_cast<double>(SECTOR_SIZE))) - origin_y;
	if (row < 0 || row >= sector_rows || col < 0 || col >= sector_count) {
		return Vector2i(-1, -1);
	}
	return Vector2i(row, col);
}

PackedFloat32Array NovaTerrainData::sample_heights_world_live(const PackedVector2Array &world_xz) const {
	PackedFloat32Array out;
	out.resize(world_xz.size());
	float *out_ptr = out.ptrw();
	// No editable image / no layout: every point is off the live surface.
	PackedByteArray pixels;
	int w = 0;
	int h = 0;
	if (sector_grid.size() < 256 || !live_heightmap_pixels(heightmap_image, pixels, w, h)) {
		const float nan = std::numeric_limits<float>::quiet_NaN();
		for (int i = 0; i < world_xz.size(); ++i) {
			out_ptr[i] = nan;
		}
		return out;
	}
	// One layout for the whole batch (the scalar path rebuilds it per call); one
	// pointer over the float32 mip-0 instead of a get_pixel Variant call per tap.
	const opennova::terrain::SectorLayout layout =
	        editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	const float *heights = reinterpret_cast<const float *>(pixels.ptr());
	const Vector2 *points = world_xz.ptr();
	for (int i = 0; i < world_xz.size(); ++i) {
		out_ptr[i] = sample_live_height_at(layout, heights, w, h, points[i].x, points[i].y);
	}
	return out;
}

float NovaTerrainData::sample_height_world_live(double world_x, double world_z) const {
	// Scalar twin of sample_heights_world_live: same guards, one point through
	// the same per-point core, NAN for off-mesh / no-data.
	// EditorTerrainMesh.sample_world_height forwards here.
	PackedByteArray pixels;
	int w = 0;
	int h = 0;
	if (sector_grid.size() < 256 || !live_heightmap_pixels(heightmap_image, pixels, w, h)) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	const opennova::terrain::SectorLayout layout =
	        editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	return sample_live_height_at(layout, reinterpret_cast<const float *>(pixels.ptr()), w, h,
	                             world_x, world_z);
}

Vector3 NovaTerrainData::raycast_terrain(const Vector3 &p_from, const Vector3 &p_to) const {
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
	// [orig: @ 0x60ccf7]) — that is runtime-substrate behavior; this binding is
	// the editor-mode surface, so no-data misses: the same deliberate
	// editor-guard divergence class as the sampler's kOutOfExtent
	// (terrain/terrain_raycast.h header note).
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
	substrate.layout = editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);

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

Vector2 NovaTerrainData::world_to_cell_source_coords(double world_x, double world_z, int row, int col) const {
	// Explicit-cell variant with an UNCLAMPED local offset (the caller owns the
	// cell choice). Mirrors EditorTerrainMesh.world_to_cell_source_coords; returns
	// the (-1e9,-1e9) sentinel for an empty / out-of-extent cell.
	if (sector_grid.size() < 256) {
		return Vector2(-1e9f, -1e9f);
	}
	const opennova::terrain::SectorLayout layout =
	        editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	const opennova::terrain::CellCoordsResult<double> r =
	        opennova::terrain::coords_world_to_cell_source<double>(layout, world_x, world_z, row, col);
	if (!r.valid) {
		return Vector2(-1e9f, -1e9f);
	}
	return Vector2(static_cast<real_t>(r.source_x), static_cast<real_t>(r.source_z));
}

Rect2i NovaTerrainData::get_cell_atlas_rect(int row, int col) const {
	// 512x512 quadrant window into the 1024 atlas for a cell, or a zero rect for
	// an empty / out-of-extent cell. Mirrors EditorTerrainMesh.get_cell_atlas_rect.
	if (sector_grid.size() < 256) {
		return Rect2i(0, 0, 0, 0);
	}
	const opennova::terrain::SectorLayout layout =
	        editor_layout_from(sector_grid, origin_x, origin_y, sector_count, sector_rows);
	const opennova::terrain::CoordsRect r = opennova::terrain::coords_cell_atlas_rect(layout, row, col);
	return Rect2i(r.x, r.z, r.w, r.h);
}

int NovaTerrainData::get_tile_count() const {
	return static_cast<int>(cpt.tiles.size());
}

int NovaTerrainData::get_detail_foliage_index_fixed(int32_t world_x_fixed,
                                                    int32_t world_z_fixed) const {
	// [orig: Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0]
	if (!loaded || foliage_map_resource.is_null()) {
		return 0;
	}
	return static_cast<int>(foliage_map_resource->sample_detail_flat_wrap(
			world_x_fixed, world_z_fixed));
}

int NovaTerrainData::get_detail_foliage_index_world(double world_x, double world_z) const {
	if (!loaded || foliage_map_resource.is_null()) {
		return 0;
	}
	return foliage_map_resource->sample_detail_index_world(world_x, world_z);
}

int NovaTerrainData::get_foliage_index_world(float world_x, float world_z) const {
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

Dictionary NovaTerrainData::load_foliage_indices() const {
	Dictionary result;
	if (foliagemap_width <= 0 || foliagemap_height <= 0 ||
	    foliagemap_indices.size() < static_cast<size_t>(foliagemap_width * foliagemap_height)) {
		return result;
	}

	PackedByteArray data;
	data.resize(static_cast<int64_t>(foliagemap_indices.size()));
	std::memcpy(data.ptrw(), foliagemap_indices.data(), foliagemap_indices.size());

	PackedByteArray palette;
	palette.resize(256 * 3);
	std::memcpy(palette.ptrw(), foliagemap_palette, sizeof(foliagemap_palette));

	result["data"] = data;
	result["width"] = foliagemap_width;
	result["height"] = foliagemap_height;
	result["palette"] = palette;
	return result;
}

PackedInt32Array NovaTerrainData::get_sector_grid() const {
	return sector_grid;
}

Ref<NovaTerrainFoliageMap> NovaTerrainData::get_foliage_map() const {
	if (foliage_map_resource.is_null()) {
		const_cast<NovaTerrainData *>(this)->_sync_foliage_map_resource_from_slot();
	}
	return foliage_map_resource;
}

void NovaTerrainData::set_foliage_map(const Ref<NovaTerrainFoliageMap> &p_map) {
	if (p_map.is_null()) {
		_apply_foliage_map_to_slot(
				opennova::foliage_make_default_map(opennova::FOLIAGE_HEIGHTMAP_SIZE, opennova::FOLIAGE_HEIGHTMAP_SIZE, 0));
		foliage_map_resource.unref();
	} else {
		_apply_foliage_map_to_slot(p_map->to_native());
		foliage_map_resource = p_map;
	}
	_notify_terrain_changed();
}

Array NovaTerrainData::get_foliage_defs() const {
	Array arr;
	for (const auto& def : trn.foliage_defs) {
		arr.push_back(foliage_def_to_object(def));
	}
	return arr;
}

void NovaTerrainData::set_foliage_defs(const Array &p_defs) {
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

void NovaTerrainData::set_trn_texture_filename(const String &slot_id, const String &filename) {
	const std::string native = filename.utf8().get_data();
	if (slot_id == "colormap") trn.colormap = native;
	else if (slot_id == "detailmap") trn.detailmap = native;
	else if (slot_id == "detailmap_c1") trn.detailmap_c1 = native;
	else if (slot_id == "detailmap_c2") trn.detailmap_c2 = native;
	else if (slot_id == "detailmap_c3") trn.detailmap_c3 = native;
	else if (slot_id == "detailmap2") trn.detailmap2 = native;
	else if (slot_id == "detailmapdist") trn.detailmapdist = native;
	else if (slot_id == "detailmapdist2") trn.detailmapdist2 = native;
	else if (slot_id == "detailblendmap") trn.detailblendmap = native;
	else if (slot_id == "charmap") trn.charmap = native;
	else if (slot_id == "foliagemap") trn.foliagemap = native;
	else if (slot_id == "tilestrip") trn.tilestrip = native;
	else {
		UtilityFunctions::push_error("NovaTerrainData: unknown TRN texture field '", slot_id, "'");
		return;
	}
	_notify_terrain_changed();
}

String NovaTerrainData::get_trn_texture_filename(const String &slot_id) const {
	if (slot_id == "colormap") return String(trn.colormap.c_str());
	if (slot_id == "detailmap") return String(trn.detailmap.c_str());
	if (slot_id == "detailmap_c1") return String(trn.detailmap_c1.c_str());
	if (slot_id == "detailmap_c2") return String(trn.detailmap_c2.c_str());
	if (slot_id == "detailmap_c3") return String(trn.detailmap_c3.c_str());
	if (slot_id == "detailmap2") return String(trn.detailmap2.c_str());
	if (slot_id == "detailmapdist") return String(trn.detailmapdist.c_str());
	if (slot_id == "detailmapdist2") return String(trn.detailmapdist2.c_str());
	if (slot_id == "detailblendmap") return String(trn.detailblendmap.c_str());
	if (slot_id == "charmap") return String(trn.charmap.c_str());
	if (slot_id == "foliagemap") return String(trn.foliagemap.c_str());
	if (slot_id == "tilestrip") return String(trn.tilestrip.c_str());
	return "";
}

void NovaTerrainData::set_polydata_filename(const String &filename) {
	trn.polydata = filename.utf8().get_data();
	_notify_terrain_changed();
}

String NovaTerrainData::get_polydata_filename() const {
	return String(trn.polydata.c_str());
}

void NovaTerrainData::set_tileinfo_filename(const String &filename) {
	trn.tileinfo = filename.utf8().get_data();
	tileinfo_resource_cache.unref();
	tileinfo_resource_cache_path = String();
	_notify_terrain_changed();
}

String NovaTerrainData::get_tileinfo_filename() const {
	return String(trn.tileinfo.c_str());
}

Ref<NovaTerrainTileInfo> NovaTerrainData::get_tileinfo_resource() const {
	const String filename = String(trn.tileinfo.c_str());
	if (filename.is_empty() || (trn_path.is_empty() && resource_root.is_null())) {
		tileinfo_resource_cache.unref();
		tileinfo_resource_cache_path = String();
		return Ref<NovaTerrainTileInfo>();
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
		UtilityFunctions::push_warning("NovaTerrainData: tileinfo file not found at ", lookup);
		tileinfo_resource_cache.unref();
		tileinfo_resource_cache_path = String();
		return Ref<NovaTerrainTileInfo>();
	}

	opennova::TilFile til;
	std::string error;
	if (!opennova::load_til(bytes.ptr(), static_cast<size_t>(bytes.size()), til, error)) {
		UtilityFunctions::push_warning("NovaTerrainData: tileinfo parse failed for ",
			lookup, ": ", String(error.c_str()));
		tileinfo_resource_cache.unref();
		tileinfo_resource_cache_path = String();
		return Ref<NovaTerrainTileInfo>();
	}

	Ref<NovaTerrainTileInfo> resource;
	resource.instantiate();
	resource->copy_from_native(til);
	tileinfo_resource_cache = resource;
	tileinfo_resource_cache_path = lookup;
	return resource;
}
