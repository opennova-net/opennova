#include "nova_terrain_surface_inputs.h"

#include "nova_terrain_data.h"
#include "nova_terrain_tile_info.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>

#include <terrain/texture_preprocess.h>
#include <til/til_overlay_bake.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace godot;

namespace {

bool image_to_rgba8(const Ref<Image> &p_source,
		opennova::terrain::Rgba8Image &r_out) {
	if (p_source.is_null() || p_source->is_empty()) {
		return false;
	}
	Ref<Image> image = p_source->duplicate();
	if (image.is_null()) {
		return false;
	}
	if (image->is_compressed() && image->decompress() != OK) {
		return false;
	}
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}

	const int width = image->get_width();
	const int height = image->get_height();
	if (width <= 0 || height <= 0) {
		return false;
	}
	const size_t base_bytes = static_cast<size_t>(width) * height * 4;
	const PackedByteArray bytes = image->get_data();
	if (bytes.size() < static_cast<int64_t>(base_bytes)) {
		return false;
	}
	r_out.width = static_cast<uint32_t>(width);
	r_out.height = static_cast<uint32_t>(height);
	r_out.pixels.assign(bytes.ptr(), bytes.ptr() + base_bytes);
	return true;
}

bool texture_to_rgba8(const Ref<Texture2D> &p_texture,
		opennova::terrain::Rgba8Image &r_out) {
	return p_texture.is_valid() && image_to_rgba8(p_texture->get_image(), r_out);
}

PackedByteArray to_packed_bytes(const std::vector<uint8_t> &p_bytes) {
	PackedByteArray packed;
	packed.resize(static_cast<int64_t>(p_bytes.size()));
	if (!p_bytes.empty()) {
		std::memcpy(packed.ptrw(), p_bytes.data(), p_bytes.size());
	}
	return packed;
}

Ref<Texture2D> texture_from_rgba8(
		const opennova::terrain::Rgba8Image &p_source,
		bool p_generate_mipmaps) {
	if (!p_source.is_valid()) {
		return {};
	}
	Ref<Image> image = Image::create_from_data(
		static_cast<int>(p_source.width), static_cast<int>(p_source.height), false,
		Image::FORMAT_RGBA8, to_packed_bytes(p_source.pixels));
	if (image.is_null() ||
			(p_generate_mipmaps && image->generate_mipmaps() != OK)) {
		return {};
	}
	return ImageTexture::create_from_image(image);
}

Ref<Texture2D> texture_from_retail_mips(
		const std::vector<opennova::terrain::Rgba8Image> &p_retail_levels) {
	if (p_retail_levels.empty() || !p_retail_levels.front().is_valid()) {
		return {};
	}

	std::vector<uint8_t> complete_chain;
	uint32_t expected_width = p_retail_levels.front().width;
	uint32_t expected_height = p_retail_levels.front().height;
	for (const auto &level : p_retail_levels) {
		if (!level.is_valid() || level.width != expected_width ||
				level.height != expected_height) {
			return {};
		}
		complete_chain.insert(
			complete_chain.end(), level.pixels.begin(), level.pixels.end());
		expected_width = std::max(1u, expected_width >> 1);
		expected_height = std::max(1u, expected_height >> 1);
	}

	// Retail stops at 4x4. Godot requires a complete mip pyramid, so append the
	// generated 2x2/1x1 tail; the terrain shader clamps sampling to retail's end.
	const auto &last = p_retail_levels.back();
	Ref<Image> terminal = Image::create_from_data(
		static_cast<int>(last.width), static_cast<int>(last.height), false,
		Image::FORMAT_RGBA8, to_packed_bytes(last.pixels));
	if (terminal.is_null() || terminal->generate_mipmaps() != OK) {
		return {};
	}
	const PackedByteArray terminal_bytes = terminal->get_data();
	const size_t last_base_bytes = last.pixels.size();
	if (terminal_bytes.size() < static_cast<int64_t>(last_base_bytes)) {
		return {};
	}
	complete_chain.insert(complete_chain.end(),
		terminal_bytes.ptr() + last_base_bytes,
		terminal_bytes.ptr() + terminal_bytes.size());

	Ref<Image> image = Image::create_from_data(
		static_cast<int>(p_retail_levels.front().width),
		static_cast<int>(p_retail_levels.front().height), true,
		Image::FORMAT_RGBA8, to_packed_bytes(complete_chain));
	if (image.is_null() || image->is_empty()) {
		return {};
	}
	return ImageTexture::create_from_image(image);
}

bool live_depth_to_u16(const Ref<NovaTerrainData> &p_data,
		std::vector<uint16_t> &r_depth, uint32_t &r_width, uint32_t &r_height) {
	if (p_data.is_null()) {
		return false;
	}
	const PackedByteArray raw = p_data->get_depth_raw16();
	if (raw.is_empty() || (raw.size() & 1) != 0) {
		return false;
	}
	const int64_t sample_count = raw.size() / 2;
	Ref<Image> live_image = p_data->get_heightmap_image();
	if (live_image.is_valid() && !live_image->is_empty() &&
			static_cast<int64_t>(live_image->get_width()) * live_image->get_height() == sample_count) {
		r_width = static_cast<uint32_t>(live_image->get_width());
		r_height = static_cast<uint32_t>(live_image->get_height());
	} else {
		const int64_t side = static_cast<int64_t>(
			std::llround(std::sqrt(static_cast<double>(sample_count))));
		if (side <= 0 || side * side != sample_count) {
			return false;
		}
		r_width = static_cast<uint32_t>(side);
		r_height = static_cast<uint32_t>(side);
	}

	r_depth.resize(static_cast<size_t>(sample_count));
	const uint8_t *bytes = raw.ptr();
	for (int64_t i = 0; i < sample_count; ++i) {
		r_depth[static_cast<size_t>(i)] = static_cast<uint16_t>(
			static_cast<uint16_t>(bytes[i * 2]) |
			(static_cast<uint16_t>(bytes[i * 2 + 1]) << 8));
	}
	return true;
}

} // namespace

void NovaTerrainSurfaceInputs::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_data", "terrain_data"),
		&NovaTerrainSurfaceInputs::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"),
		&NovaTerrainSurfaceInputs::get_terrain_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
		PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainData"),
		"set_terrain_data", "get_terrain_data");

	ClassDB::bind_method(D_METHOD("set_tile_info_override", "tile_info"),
		&NovaTerrainSurfaceInputs::set_tile_info_override);
	ClassDB::bind_method(D_METHOD("get_tile_info_override"),
		&NovaTerrainSurfaceInputs::get_tile_info_override);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info_override",
		PROPERTY_HINT_RESOURCE_TYPE, "NovaTerrainTileInfo"),
		"set_tile_info_override", "get_tile_info_override");

	ClassDB::bind_method(D_METHOD("set_tile_overlay_enabled", "enabled"),
		&NovaTerrainSurfaceInputs::set_tile_overlay_enabled);
	ClassDB::bind_method(D_METHOD("get_tile_overlay_enabled"),
		&NovaTerrainSurfaceInputs::get_tile_overlay_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tile_overlay_enabled"),
		"set_tile_overlay_enabled", "get_tile_overlay_enabled");

	ClassDB::bind_method(D_METHOD("rebuild", "terrain_data", "tile_info", "tile_overlay_enabled"),
		&NovaTerrainSurfaceInputs::rebuild,
		DEFVAL(Ref<NovaTerrainTileInfo>()), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("rebuild_blend"),
		&NovaTerrainSurfaceInputs::rebuild_blend);
	ClassDB::bind_method(D_METHOD("rebuild_heightfield"),
		&NovaTerrainSurfaceInputs::rebuild_heightfield);
	ClassDB::bind_method(D_METHOD("rebuild_detail_textures"),
		&NovaTerrainSurfaceInputs::rebuild_detail_textures);
	ClassDB::bind_method(D_METHOD("rebuild_tile_overlay"),
		&NovaTerrainSurfaceInputs::rebuild_tile_overlay);
	ClassDB::bind_method(D_METHOD("clear_derived_textures"),
		&NovaTerrainSurfaceInputs::clear_derived_textures);
	ClassDB::bind_method(D_METHOD("clear_tile_overlay"),
		&NovaTerrainSurfaceInputs::clear_tile_overlay);
	ClassDB::bind_method(D_METHOD("apply_to_material", "material"),
		&NovaTerrainSurfaceInputs::apply_to_material);

	ClassDB::bind_method(D_METHOD("get_colormap_texture"),
		&NovaTerrainSurfaceInputs::get_colormap_texture);
	ClassDB::bind_method(D_METHOD("get_detailmap_texture"),
		&NovaTerrainSurfaceInputs::get_detailmap_texture);
	ClassDB::bind_method(D_METHOD("get_blend_texture"),
		&NovaTerrainSurfaceInputs::get_blend_texture);
	ClassDB::bind_method(D_METHOD("get_detail_c1_texture"),
		&NovaTerrainSurfaceInputs::get_detail_c1_texture);
	ClassDB::bind_method(D_METHOD("get_detail_c2_texture"),
		&NovaTerrainSurfaceInputs::get_detail_c2_texture);
	ClassDB::bind_method(D_METHOD("get_detail_c3_texture"),
		&NovaTerrainSurfaceInputs::get_detail_c3_texture);
	ClassDB::bind_method(D_METHOD("get_normalized_blend_texture"),
		&NovaTerrainSurfaceInputs::get_normalized_blend_texture);
	ClassDB::bind_method(D_METHOD("get_detail_coefficient_texture"),
		&NovaTerrainSurfaceInputs::get_detail_coefficient_texture);
	ClassDB::bind_method(D_METHOD("get_paired_detail_texture", "layer"),
		&NovaTerrainSurfaceInputs::get_paired_detail_texture);
	ClassDB::bind_method(D_METHOD("get_detail2_texture"),
		&NovaTerrainSurfaceInputs::get_detail2_texture);
	ClassDB::bind_method(D_METHOD("has_detail2"),
		&NovaTerrainSurfaceInputs::has_detail2);
	ClassDB::bind_method(D_METHOD("get_detail2_density"),
		&NovaTerrainSurfaceInputs::get_detail2_density);
	ClassDB::bind_method(D_METHOD("get_heightfield_normal_texture"),
		&NovaTerrainSurfaceInputs::get_heightfield_normal_texture);
	ClassDB::bind_method(D_METHOD("get_tile_overlay_texture"),
		&NovaTerrainSurfaceInputs::get_tile_overlay_texture);
	ClassDB::bind_method(D_METHOD("get_detail_density"),
		&NovaTerrainSurfaceInputs::get_detail_density);
	ClassDB::bind_method(D_METHOD("has_normalized_blend"),
		&NovaTerrainSurfaceInputs::has_normalized_blend);
	ClassDB::bind_method(D_METHOD("has_detail_coefficient"),
		&NovaTerrainSurfaceInputs::has_detail_coefficient);
	ClassDB::bind_method(D_METHOD("has_paired_detail", "layer"),
		&NovaTerrainSurfaceInputs::has_paired_detail);
	ClassDB::bind_method(D_METHOD("has_heightfield_normal"),
		&NovaTerrainSurfaceInputs::has_heightfield_normal);
	ClassDB::bind_method(D_METHOD("has_tile_overlay"),
		&NovaTerrainSurfaceInputs::has_tile_overlay);
	ClassDB::bind_method(D_METHOD("get_diagnostics"),
		&NovaTerrainSurfaceInputs::get_diagnostics);
}

void NovaTerrainSurfaceInputs::set_terrain_data(
		const Ref<NovaTerrainData> &p_data) {
	if (terrain_data == p_data) {
		return;
	}
	terrain_data = p_data;
	clear_derived_textures();
	clear_tile_overlay();
}

Ref<NovaTerrainData> NovaTerrainSurfaceInputs::get_terrain_data() const {
	return terrain_data;
}

void NovaTerrainSurfaceInputs::set_tile_info_override(
		const Ref<NovaTerrainTileInfo> &p_info) {
	if (tile_info_override == p_info) {
		return;
	}
	tile_info_override = p_info;
	clear_tile_overlay();
}

Ref<NovaTerrainTileInfo> NovaTerrainSurfaceInputs::get_tile_info_override() const {
	return tile_info_override;
}

void NovaTerrainSurfaceInputs::set_tile_overlay_enabled(bool p_enabled) {
	if (tile_overlay_enabled == p_enabled) {
		return;
	}
	tile_overlay_enabled = p_enabled;
	clear_tile_overlay();
}

bool NovaTerrainSurfaceInputs::get_tile_overlay_enabled() const {
	return tile_overlay_enabled;
}

bool NovaTerrainSurfaceInputs::rebuild(const Ref<NovaTerrainData> &p_data,
		const Ref<NovaTerrainTileInfo> &p_tile_info,
		bool p_tile_overlay_enabled) {
	set_terrain_data(p_data);
	set_tile_info_override(p_tile_info);
	set_tile_overlay_enabled(p_tile_overlay_enabled);
	if (terrain_data.is_null()) {
		return false;
	}
	rebuild_heightfield();
	rebuild_blend();
	rebuild_detail_textures();
	rebuild_tile_overlay();
	return true;
}

bool NovaTerrainSurfaceInputs::rebuild_blend() {
	normalized_blend_texture.unref();
	if (terrain_data.is_null()) {
		return false;
	}

	opennova::terrain::Rgba8Image source;
	const Ref<Image> live_blend = terrain_data->get_blendmap_image();
	const bool have_source = live_blend.is_valid()
		? image_to_rgba8(live_blend, source)
		: texture_to_rgba8(terrain_data->get_detailblendmap(), source);
	if (have_source) {
		// Retail creates the DBlendmap quadrants with flags 0x100001: no
		// mip-suppression bit, so they carry the full box-filtered auto chain.
		// [orig: PolyTrn_InitTextures @ 0x60b2c9..0x60b2dd;
		// GTexture_CreateFromPixelData @ 0x6877c7..0x6878be]
		normalized_blend_texture = texture_from_rgba8(
			opennova::terrain::normalize_detail_blend_map(source), true);
	}
	return normalized_blend_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::rebuild_heightfield() {
	heightfield_normal_texture.unref();
	if (terrain_data.is_null()) {
		return false;
	}

	std::vector<uint16_t> depth;
	uint32_t width = 0;
	uint32_t height = 0;
	if (live_depth_to_u16(terrain_data, depth, width, height)) {
		heightfield_normal_texture = texture_from_rgba8(
			opennova::terrain::build_heightfield_normal_map(
				depth, width, height,
				terrain_data->get_trn().get_quadrant_locks()), false);
	}
	return heightfield_normal_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::rebuild_detail_textures() {
	detail_coefficient_texture.unref();
	for (auto &texture : paired_detail_textures) {
		texture.unref();
	}
	paired_detail2_texture.unref();
	mipped_colormap_texture.unref();
	if (terrain_data.is_null()) {
		return false;
	}

	opennova::terrain::Rgba8Image detail_source;
	if (texture_to_rgba8(terrain_data->get_detailmap(), detail_source)) {
		detail_coefficient_texture = texture_from_rgba8(
			opennova::terrain::build_detail_coefficient_map(detail_source), true);
	}

	opennova::terrain::Rgba8Image far_source;
	const bool have_far = texture_to_rgba8(
		terrain_data->get_detailmapdist(), far_source);
	const Ref<Texture2D> authored_layers[3] = {
		terrain_data->get_detailmap_c1(),
		terrain_data->get_detailmap_c2(),
		terrain_data->get_detailmap_c3(),
	};
	if (have_far) {
		for (int layer = 0; layer < 3; ++layer) {
			opennova::terrain::Rgba8Image base_source;
			if (texture_to_rgba8(authored_layers[layer], base_source)) {
				paired_detail_textures[layer] = texture_from_retail_mips(
					opennova::terrain::build_paired_detail_mip_chain(
						base_source, far_source));
			}
		}
	}

	// The second detail is the ps.1.4 splat's stage-3 dp3 input, paired with
	// its own far texture when authored, otherwise carrying plain box mips.
	// [orig: PolyTrn_InitTextures @ 0x60af97/0x60aff9 (load + optional
	// create-with-blend); stage bind @ 0x6043ff]
	opennova::terrain::Rgba8Image detail2_source;
	if (texture_to_rgba8(terrain_data->get_detailmap2(), detail2_source)) {
		opennova::terrain::Rgba8Image far2_source;
		if (texture_to_rgba8(terrain_data->get_detailmapdist2(), far2_source)) {
			paired_detail2_texture = texture_from_retail_mips(
				opennova::terrain::build_paired_detail_mip_chain(
					detail2_source, far2_source));
		}
		if (paired_detail2_texture.is_null()) {
			paired_detail2_texture = texture_from_rgba8(detail2_source, true);
		}
	}

	// Retail's t0 is the per-patch tile-cache render target whose resolution
	// drops with the patch LOD (1024 >> lod per 512u quadrant); a box-mipped
	// colormap is the byte-closest ported surrogate while the RT lifecycle
	// stays open under D-TERRAIN-7.
	opennova::terrain::Rgba8Image colormap_source;
	if (texture_to_rgba8(terrain_data->get_colormap(), colormap_source)) {
		mipped_colormap_texture = texture_from_rgba8(colormap_source, true);
	}
	return detail_coefficient_texture.is_valid() ||
		paired_detail_textures[0].is_valid() ||
		paired_detail_textures[1].is_valid() ||
		paired_detail_textures[2].is_valid() ||
		paired_detail2_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::rebuild_tile_overlay() {
	clear_tile_overlay();
	if (!tile_overlay_enabled || terrain_data.is_null()) {
		return false;
	}

	Ref<NovaTerrainTileInfo> tile_info = tile_info_override;
	if (tile_info.is_null()) {
		tile_info = terrain_data->get_tileinfo_resource();
	}
	const Ref<Texture2D> tilestrip = terrain_data->get_tilestrip_tex();
	if (tile_info.is_null() || tilestrip.is_null() ||
			tile_info->get_entry_count() <= 0) {
		return false;
	}

	opennova::terrain::Rgba8Image atlas;
	if (!texture_to_rgba8(tilestrip, atlas)) {
		return false;
	}
	constexpr int overlay_dimension = 1024;
	std::vector<uint8_t> overlay_rgba;
	if (!opennova::til_bake_overlay_rgba(tile_info->to_native(),
			atlas.pixels.data(), static_cast<int>(atlas.width),
			static_cast<int>(atlas.height), overlay_dimension,
			overlay_dimension, overlay_rgba)) {
		return false;
	}

	opennova::terrain::Rgba8Image overlay;
	overlay.width = overlay_dimension;
	overlay.height = overlay_dimension;
	overlay.pixels = std::move(overlay_rgba);
	tile_overlay_texture = texture_from_rgba8(overlay, false);
	return tile_overlay_texture.is_valid();
}

void NovaTerrainSurfaceInputs::clear_derived_textures() {
	detail_coefficient_texture.unref();
	paired_detail2_texture.unref();
	mipped_colormap_texture.unref();
	normalized_blend_texture.unref();
	heightfield_normal_texture.unref();
	for (auto &texture : paired_detail_textures) {
		texture.unref();
	}
}

void NovaTerrainSurfaceInputs::clear_tile_overlay() {
	tile_overlay_texture.unref();
}

bool NovaTerrainSurfaceInputs::apply_to_material(
		const Ref<ShaderMaterial> &p_material) const {
	if (p_material.is_null()) {
		return false;
	}
	p_material->set_shader_parameter("u_colormap", get_colormap_texture());
	p_material->set_shader_parameter("u_detailmap", get_detailmap_texture());
	p_material->set_shader_parameter("u_blendmap", get_blend_texture());
	p_material->set_shader_parameter("u_detail_c1", get_detail_c1_texture());
	p_material->set_shader_parameter("u_detail_c2", get_detail_c2_texture());
	p_material->set_shader_parameter("u_detail_c3", get_detail_c3_texture());
	p_material->set_shader_parameter("u_detail2", paired_detail2_texture);
	p_material->set_shader_parameter("u_has_detail2", has_detail2());
	p_material->set_shader_parameter("u_detail2_density",
		static_cast<float>(get_detail2_density()));
	p_material->set_shader_parameter(
		"u_heightfield_normal", heightfield_normal_texture);
	p_material->set_shader_parameter(
		"u_has_heightfield_normal", has_heightfield_normal());
	p_material->set_shader_parameter("u_detail_density",
		static_cast<float>(get_detail_density()));
	p_material->set_shader_parameter("u_tile_overlay", tile_overlay_texture);
	p_material->set_shader_parameter("u_has_tile_overlay", has_tile_overlay());
	return true;
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_colormap_texture() const {
	if (mipped_colormap_texture.is_valid()) {
		return mipped_colormap_texture;
	}
	return terrain_data.is_valid() ? terrain_data->get_colormap() : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_detailmap_texture() const {
	return terrain_data.is_valid() ? terrain_data->get_detailmap() : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_blend_texture() const {
	if (normalized_blend_texture.is_valid()) {
		return normalized_blend_texture;
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailblendmap() : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_detail_c1_texture() const {
	if (paired_detail_textures[0].is_valid()) {
		return paired_detail_textures[0];
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailmap_c1() : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_detail_c2_texture() const {
	if (paired_detail_textures[1].is_valid()) {
		return paired_detail_textures[1];
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailmap_c2() : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_detail_c3_texture() const {
	if (paired_detail_textures[2].is_valid()) {
		return paired_detail_textures[2];
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailmap_c3() : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_normalized_blend_texture() const {
	return normalized_blend_texture;
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_detail_coefficient_texture() const {
	return detail_coefficient_texture;
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_paired_detail_texture(int p_layer) const {
	return p_layer >= 0 && p_layer < 3
		? paired_detail_textures[p_layer] : Ref<Texture2D>();
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_detail2_texture() const {
	return paired_detail2_texture;
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_heightfield_normal_texture() const {
	return heightfield_normal_texture;
}

Ref<Texture2D> NovaTerrainSurfaceInputs::get_tile_overlay_texture() const {
	return tile_overlay_texture;
}

int NovaTerrainSurfaceInputs::get_detail_density() const {
	return terrain_data.is_valid() ? terrain_data->get_detail_density() : 0;
}

int NovaTerrainSurfaceInputs::get_detail2_density() const {
	return terrain_data.is_valid() ? terrain_data->get_detail_density2() : 0;
}

bool NovaTerrainSurfaceInputs::has_normalized_blend() const {
	return normalized_blend_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::has_detail_coefficient() const {
	return detail_coefficient_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::has_paired_detail(int p_layer) const {
	return p_layer >= 0 && p_layer < 3 && paired_detail_textures[p_layer].is_valid();
}

bool NovaTerrainSurfaceInputs::has_detail2() const {
	return paired_detail2_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::has_heightfield_normal() const {
	return heightfield_normal_texture.is_valid();
}

bool NovaTerrainSurfaceInputs::has_tile_overlay() const {
	return tile_overlay_texture.is_valid();
}

Dictionary NovaTerrainSurfaceInputs::get_diagnostics() const {
	Dictionary result;
	result["terrain_data_available"] = terrain_data.is_valid();
	result["normalized_blend"] = has_normalized_blend();
	result["detail_coefficient"] = has_detail_coefficient();
	result["paired_detail_c1"] = has_paired_detail(0);
	result["paired_detail_c2"] = has_paired_detail(1);
	result["paired_detail_c3"] = has_paired_detail(2);
	result["paired_detail2"] = has_detail2();
	result["heightfield_normal"] = has_heightfield_normal();
	result["tile_overlay_enabled"] = tile_overlay_enabled;
	result["tile_overlay"] = has_tile_overlay();
	return result;
}
