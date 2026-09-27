#include "terrain/terrain_surface_inputs.h"

#include "terrain/terrain_data.h"
#include "terrain/terrain_image_convert.h"
#include "terrain/terrain_tile_info.h"
#include "util/data_format.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <runtime/terrain/texture_preprocess.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace godot;

namespace {

Dictionary texture_diagnostics(const Ref<Texture2D> &p_texture) {
	Dictionary result;
	result["available"] = p_texture.is_valid();
	if (p_texture.is_null()) {
		result["size"] = Vector2i();
		result["mipmap_count"] = 0;
		result["level_count"] = 0;
		return result;
	}

	result["size"] = Vector2i(p_texture->get_width(), p_texture->get_height());
	const Ref<Image> image = p_texture->get_image();
	const int mipmap_count = image.is_valid() ? image->get_mipmap_count() : -1;
	result["mipmap_count"] = mipmap_count;
	result["level_count"] = mipmap_count >= 0 ? mipmap_count + 1 : 0;
	return result;
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

// Uploads the blocks as they are, so the GPU decodes them as the retail
// device did. Godot needs the pyramid down to 1x1 where retail stops at 4
// texels; the tail continues D3DXFilterTexture's box chain, and
// sample_retail_detail_mips never selects it.
Ref<Texture2D> texture_from_dxt_levels(
		const std::vector<opennova::renderer::DxtSurface> &p_levels) {
	using opennova::renderer::DxtSurface;
	if (p_levels.empty() || !p_levels.front().is_valid()) {
		return {};
	}
	std::vector<DxtSurface> chain = p_levels;
	uint32_t expected_width = chain.front().width;
	uint32_t expected_height = chain.front().height;
	for (const DxtSurface &level : chain) {
		if (!level.is_valid() || level.format != chain.front().format ||
				level.width != expected_width || level.height != expected_height) {
			return {};
		}
		expected_width = std::max(1u, expected_width >> 1);
		expected_height = std::max(1u, expected_height >> 1);
	}
	while (chain.back().width > 1 || chain.back().height > 1) {
		const DxtSurface last = chain.back();
		chain.push_back(opennova::renderer::encode_dxt_surface(
			opennova::renderer::box_filter_half(
				opennova::renderer::decode_dxt_surface(last), last.width, last.height),
			std::max(1u, last.width >> 1), std::max(1u, last.height >> 1),
			last.format));
	}
	std::vector<uint8_t> bytes;
	for (const DxtSurface &level : chain) {
		bytes.insert(bytes.end(), level.blocks.begin(), level.blocks.end());
	}
	const Image::Format format =
		chain.front().format == opennova::renderer::TextureDxtFormat::Dxt1
			? Image::FORMAT_DXT1 : Image::FORMAT_DXT5;
	Ref<Image> image = Image::create_from_data(
		static_cast<int>(chain.front().width), static_cast<int>(chain.front().height),
		true, format, to_packed_bytes(bytes));
	if (image.is_null() || image->is_empty()) {
		return {};
	}
	return ImageTexture::create_from_image(image);
}

bool live_depth_to_u16(const Ref<TerrainData> &p_data,
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

void TerrainSurfaceInputs::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_data", "terrain_data"),
		&TerrainSurfaceInputs::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"),
		&TerrainSurfaceInputs::get_terrain_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
		PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
		"set_terrain_data", "get_terrain_data");

	ClassDB::bind_method(D_METHOD("set_tile_info_override", "tile_info"),
		&TerrainSurfaceInputs::set_tile_info_override);
	ClassDB::bind_method(D_METHOD("get_tile_info_override"),
		&TerrainSurfaceInputs::get_tile_info_override);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "tile_info_override",
		PROPERTY_HINT_RESOURCE_TYPE, "TerrainTileInfo"),
		"set_tile_info_override", "get_tile_info_override");

	ClassDB::bind_method(D_METHOD("rebuild", "terrain_data", "tile_info"),
		&TerrainSurfaceInputs::rebuild, DEFVAL(Ref<TerrainTileInfo>()));
	ClassDB::bind_method(D_METHOD("rebuild_blend"),
		&TerrainSurfaceInputs::rebuild_blend);
	ClassDB::bind_method(D_METHOD("apply_to_material", "material"),
		&TerrainSurfaceInputs::apply_to_material);

	ClassDB::bind_method(D_METHOD("get_colormap_texture"),
		&TerrainSurfaceInputs::get_colormap_texture);
	ClassDB::bind_method(D_METHOD("get_blend_texture"),
		&TerrainSurfaceInputs::get_blend_texture);
	ClassDB::bind_method(D_METHOD("get_detail_c1_texture"),
		&TerrainSurfaceInputs::get_detail_c1_texture);
	ClassDB::bind_method(D_METHOD("get_normalized_blend_texture"),
		&TerrainSurfaceInputs::get_normalized_blend_texture);
	ClassDB::bind_method(D_METHOD("get_detail_coefficient_texture"),
		&TerrainSurfaceInputs::get_detail_coefficient_texture);
	ClassDB::bind_method(D_METHOD("get_detail_layer_texture", "layer"),
		&TerrainSurfaceInputs::get_detail_layer_texture);
	ClassDB::bind_method(D_METHOD("get_detail2_texture"),
		&TerrainSurfaceInputs::get_detail2_texture);
	ClassDB::bind_method(D_METHOD("has_detail2"),
		&TerrainSurfaceInputs::has_detail2);
	ClassDB::bind_method(D_METHOD("get_detail_density"),
		&TerrainSurfaceInputs::get_detail_density);
	ClassDB::bind_method(D_METHOD("has_normalized_blend"),
		&TerrainSurfaceInputs::has_normalized_blend);
	ClassDB::bind_method(D_METHOD("has_detail_coefficient"),
		&TerrainSurfaceInputs::has_detail_coefficient);
	ClassDB::bind_method(D_METHOD("has_detail_layer", "layer"),
		&TerrainSurfaceInputs::has_detail_layer);
	ClassDB::bind_method(D_METHOD("has_heightfield_normal"),
		&TerrainSurfaceInputs::has_heightfield_normal);
	ClassDB::bind_method(D_METHOD("get_diagnostics"),
		&TerrainSurfaceInputs::get_diagnostics);
}

void TerrainSurfaceInputs::set_terrain_data(
		const Ref<TerrainData> &p_data) {
	if (terrain_data == p_data) {
		return;
	}
	terrain_data = p_data;
	clear_derived_textures();
}

Ref<TerrainData> TerrainSurfaceInputs::get_terrain_data() const {
	return terrain_data;
}

void TerrainSurfaceInputs::set_tile_info_override(
		const Ref<TerrainTileInfo> &p_info) {
	if (tile_info_override == p_info) {
		return;
	}
	tile_info_override = p_info;
}

Ref<TerrainTileInfo> TerrainSurfaceInputs::get_tile_info_override() const {
	return tile_info_override;
}

bool TerrainSurfaceInputs::rebuild(const Ref<TerrainData> &p_data,
		const Ref<TerrainTileInfo> &p_tile_info) {
	set_terrain_data(p_data);
	set_tile_info_override(p_tile_info);
	if (terrain_data.is_null()) {
		return false;
	}
	rebuild_heightfield();
	rebuild_blend();
	rebuild_detail_textures();
	return true;
}

bool TerrainSurfaceInputs::rebuild_blend() {
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
		// GTexture_CreateFromPixelData_0 @ 0x6877c7..0x6878be, see docs/terrain/terrain-re.md]
		normalized_blend_texture = texture_from_rgba8(
			opennova::terrain::normalize_detail_blend_map(source), true);
	}
	return normalized_blend_texture.is_valid();
}

bool TerrainSurfaceInputs::rebuild_heightfield() {
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

bool TerrainSurfaceInputs::rebuild_detail_textures() {
	detail_coefficient_texture.unref();
	for (auto &texture : detail_layer_textures) {
		texture.unref();
	}
	paired_detail2_texture.unref();
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
	// Every layer is the DXT texture retail creates from it, paired with the
	// far texture when one is authored.
	for (int layer = 0; layer < 3; ++layer) {
		opennova::terrain::Rgba8Image base_source;
		if (texture_to_rgba8(authored_layers[layer], base_source)) {
			detail_layer_textures[layer] = texture_from_dxt_levels(
				opennova::terrain::build_detail_layer_levels(
					base_source, have_far ? &far_source : nullptr));
		}
	}

	// The second detail is the ps.1.4 splat's stage-3 dp3 input, paired with
	// its own far texture when authored, otherwise carrying plain box mips.
	// [orig: PolyTrn_InitTextures @ 0x60af97/0x60aff9 (load + optional
	// create-with-blend); stage bind @ 0x6043ff, see docs/terrain/terrain-re.md]
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
	return detail_coefficient_texture.is_valid() ||
		detail_layer_textures[0].is_valid() ||
		detail_layer_textures[1].is_valid() ||
		detail_layer_textures[2].is_valid() ||
		paired_detail2_texture.is_valid();
}

void TerrainSurfaceInputs::clear_derived_textures() {
	detail_coefficient_texture.unref();
	paired_detail2_texture.unref();
	normalized_blend_texture.unref();
	heightfield_normal_texture.unref();
	for (auto &texture : detail_layer_textures) {
		texture.unref();
	}
}

bool TerrainSurfaceInputs::apply_to_material(
		const Ref<ShaderMaterial> &p_material) const {
	if (p_material.is_null()) {
		return false;
	}
	p_material->set_shader_parameter("u_blendmap", get_blend_texture());
	p_material->set_shader_parameter("u_detail_c1", get_detail_c1_texture());
	p_material->set_shader_parameter("u_detail_c2", get_detail_c2_texture());
	p_material->set_shader_parameter("u_detail_c3", get_detail_c3_texture());
	p_material->set_shader_parameter("u_detail2", paired_detail2_texture);
	p_material->set_shader_parameter("u_has_detail2", has_detail2());
	p_material->set_shader_parameter("u_detail2_density",
		static_cast<float>(get_detail2_density()));
	p_material->set_shader_parameter("u_detail_density",
		static_cast<float>(get_detail_density()));
	// The ps.1.1 terrain light pass's t1: retail binds the generated detail
	// coefficient map to texture slot 8 for every batch
	// (renderer/light_terrain_pass.h, the stage map).
	p_material->set_shader_parameter("u_terrain_light_normal",
		detail_coefficient_texture);
	return true;
}

Ref<Texture2D> TerrainSurfaceInputs::get_colormap_texture() const {
	return terrain_data.is_valid() ? terrain_data->get_colormap() : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_detailmap_texture() const {
	return terrain_data.is_valid() ? terrain_data->get_detailmap() : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_blend_texture() const {
	if (normalized_blend_texture.is_valid()) {
		return normalized_blend_texture;
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailblendmap() : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_detail_c1_texture() const {
	if (detail_layer_textures[0].is_valid()) {
		return detail_layer_textures[0];
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailmap_c1() : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_detail_c2_texture() const {
	if (detail_layer_textures[1].is_valid()) {
		return detail_layer_textures[1];
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailmap_c2() : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_detail_c3_texture() const {
	if (detail_layer_textures[2].is_valid()) {
		return detail_layer_textures[2];
	}
	return terrain_data.is_valid()
		? terrain_data->get_detailmap_c3() : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_normalized_blend_texture() const {
	return normalized_blend_texture;
}

Ref<Texture2D> TerrainSurfaceInputs::get_detail_coefficient_texture() const {
	return detail_coefficient_texture;
}

Ref<Texture2D> TerrainSurfaceInputs::get_detail_layer_texture(int p_layer) const {
	return p_layer >= 0 && p_layer < 3
		? detail_layer_textures[p_layer] : Ref<Texture2D>();
}

Ref<Texture2D> TerrainSurfaceInputs::get_detail2_texture() const {
	return paired_detail2_texture;
}

Ref<Texture2D> TerrainSurfaceInputs::get_heightfield_normal_texture() const {
	return heightfield_normal_texture;
}

int TerrainSurfaceInputs::get_detail_density() const {
	return terrain_data.is_valid() ? terrain_data->get_detail_density() : 0;
}

int TerrainSurfaceInputs::get_detail2_density() const {
	return terrain_data.is_valid() ? terrain_data->get_detail_density2() : 0;
}

bool TerrainSurfaceInputs::has_normalized_blend() const {
	return normalized_blend_texture.is_valid();
}

bool TerrainSurfaceInputs::has_detail_coefficient() const {
	return detail_coefficient_texture.is_valid();
}

bool TerrainSurfaceInputs::has_detail_layer(int p_layer) const {
	return p_layer >= 0 && p_layer < 3 && detail_layer_textures[p_layer].is_valid();
}

bool TerrainSurfaceInputs::has_detail2() const {
	return paired_detail2_texture.is_valid();
}

bool TerrainSurfaceInputs::has_heightfield_normal() const {
	return heightfield_normal_texture.is_valid();
}

Dictionary TerrainSurfaceInputs::get_diagnostics() const {
	Dictionary result;
	result["terrain_data_available"] = terrain_data.is_valid();
	result["normalized_blend"] = has_normalized_blend();
	result["detail_coefficient"] = has_detail_coefficient();
	result["detail_layer_c1"] = has_detail_layer(0);
	result["detail_layer_c2"] = has_detail_layer(1);
	result["detail_layer_c3"] = has_detail_layer(2);
	result["paired_detail2"] = has_detail2();
	result["heightfield_normal"] = has_heightfield_normal();
	Ref<TerrainTileInfo> tile_info = tile_info_override;
	if (tile_info.is_null() && terrain_data.is_valid()) {
		tile_info = terrain_data->get_tileinfo_resource();
	}
	result["tile_info_available"] = tile_info.is_valid();
	result["tile_entry_count"] =
		tile_info.is_valid() ? tile_info->get_entry_count() : 0;
	result["tilestrip_available"] = terrain_data.is_valid() &&
		terrain_data->get_tilestrip_tex().is_valid();
	result["detail_density"] = get_detail_density();
	result["detail2_density"] = get_detail2_density();

	Dictionary textures;
	textures["colormap"] = texture_diagnostics(get_colormap_texture());
	textures["detailmap"] = texture_diagnostics(get_detailmap_texture());
	textures["blendmap"] = texture_diagnostics(get_blend_texture());
	textures["detail_c1"] = texture_diagnostics(get_detail_c1_texture());
	textures["detail_c2"] = texture_diagnostics(get_detail_c2_texture());
	textures["detail_c3"] = texture_diagnostics(get_detail_c3_texture());
	textures["detail2"] = texture_diagnostics(get_detail2_texture());
	textures["heightfield_normal"] =
		texture_diagnostics(get_heightfield_normal_texture());
	result["textures"] = textures;
	return result;
}
