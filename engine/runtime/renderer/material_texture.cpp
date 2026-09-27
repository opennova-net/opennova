#include "material_texture.h"

#include <base/io/strutil.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace opennova::renderer {

// [orig: Texture_LoadAsNormalMap @0x58C480]
std::string normal_material_filename(std::string_view name,
        bool loose_tga_preferred, bool dds_exists) {
    const std::string upper = strutil::to_upper(name);
    std::string result(name);
    if (upper.find(".MDT") == std::string::npos && upper.find(".TGA") != std::string::npos &&
            !loose_tga_preferred && dds_exists) {
        result.resize(result.find_last_of('.'));
        result += ".dds";
    }
    return result;
}

// [orig: Texture_LoadByNameWithChannel @0x58B4E1..0x58B4FA — strstr(".")
// then byte 4 past it = 0]
std::string material_texture_query(std::string_view name) {
	std::string query(name);
	const size_t dot = query.find('.');
	if (dot != std::string::npos && dot + 4 < query.size())
		query.resize(dot + 4);
	return query;
}

// [orig: Texture_LoadByNameWithChannel @0x58B53C..0x58B598 — strrchr('.')
// cut, then ".dds" @0x7D8AD0]
std::string material_dds_sibling(std::string_view query) {
	std::string result(query);
	const size_t dot = result.rfind('.');
	if (dot != std::string::npos)
		result.resize(dot);
	return result + ".dds";
}

namespace {

// The plain loaders dispatch on the upper-cased path in this order.
// [orig: Texture_LoadByNameWithChannel @0x58B66F..0x58B6E6;
// Texture_LoadAndRegister @0x58B80E..0x58B881]
MaterialImageSource plain_source(std::string_view file) {
	const std::string upper = strutil::to_upper(file);
	MaterialImageSource source{std::string(file), MaterialImageDecoder::None};
	if (upper.find(".TGA") != std::string::npos || upper.find(".MDT") != std::string::npos)
		source.decoder = MaterialImageDecoder::Tga;
	else if (upper.find(".PCX") != std::string::npos)
		source.decoder = MaterialImageDecoder::Pcx;
	return source;
}

} // namespace

// [orig: Texture_LoadByNameWithChannel @0x58B4FE..0x58B5AD]
MaterialImageSource material_image_source(std::string_view query,
		bool loose_first_hit, bool dds_exists) {
	if (loose_first_hit || query.find(".MDT") != std::string_view::npos)
		return plain_source(query);
	if (dds_exists)
		return {material_dds_sibling(query), MaterialImageDecoder::Dds};
	return plain_source(query);
}

MaterialImageSource plain_material_image_source(std::string_view name) {
	return plain_source(name);
}

// [orig: GTexture_CreateFromPixelData_0 @0x6877BC..0x6877D8]
uint32_t pixel_texture_mip_levels(uint32_t width, uint32_t height) {
	int32_t side = static_cast<int32_t>(std::min(width, height));
	uint32_t levels = 0;
	while (side > 2) {
		side >>= 1;
		++levels;
	}
	return levels;
}

// [orig: Material_ConvertDefinition @0x5B045B..0x5B04A0]
uint8_t material_texture_runtime_type(uint8_t authored_type) {
	if (authored_type == 3 || (authored_type >= 9 && authored_type <= 15) ||
			authored_type > 18)
		return 0;
	return authored_type;
}

// [orig: Material_LoadStageTexture @0x5B16F0; dedicated cases @0x5B179A (6),
// @0x5B17B7 (7), @0x5B17D4 (16), @0x5B17DD (17), @0x5B17E6 (18)]
MaterialTextureTransform material_texture_transform(
		uint8_t type, std::string_view name, bool loaded) {
	if (!loaded || type == 3 || (type >= 9 && type <= 15) || type > 18)
		return MaterialTextureTransform::Checkerboard;
    if (type == 16) return MaterialTextureTransform::ChunkNormal;
    if (type == 17) return MaterialTextureTransform::ChunkHorizon;
    if (type == 18) return MaterialTextureTransform::ChunkOcclusion;
    if (type == 6 || type == 7) {
        if (strutil::to_upper(name).find(".TGA") == std::string::npos) return MaterialTextureTransform::Checkerboard;
        return type == 6 ? MaterialTextureTransform::HorizonVolume : MaterialTextureTransform::AmbientOcclusion;
    }
	if (type != 4 && type != 5) return MaterialTextureTransform::Unchanged;
	const std::string upper = strutil::to_upper(name);
	if (upper.find(".MDT") != std::string::npos) return MaterialTextureTransform::Unchanged;
	if (upper.find(".TGA") != std::string::npos) return MaterialTextureTransform::NormalFromAlpha;
	// The object dispatcher passes an empty alternate PCX path.
	return MaterialTextureTransform::Checkerboard;
}

// [orig: Texture_LoadAsNormalMap @0x58C985..0x58CAED (the live type-4/5
// kernel); Texture_ApplyNormalMapFilter @0x58BD90..0x58C06C (its uncalled
// twin)]
std::vector<uint8_t> normal_map_from_height_rgba(const uint8_t *rgba,
		uint32_t width, uint32_t height, float scale,
		uint8_t height_channel, uint8_t alpha_channel) {
	if (!rgba || !width || !height || height_channel > 3 || alpha_channel > 3 ||
			width > std::numeric_limits<size_t>::max() / height / 4) return {};
	std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4);
	auto encode = [](double value) {
		return static_cast<uint8_t>(std::clamp(static_cast<int>((value + 1.0) * 127.5), 0, 255));
	};
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			auto sample = [&](uint32_t sx, uint32_t sy) {
				return static_cast<double>(rgba[(static_cast<size_t>(sy) * width + sx) * 4 + height_channel]);
			};
			const double center = sample(x, y);
			const double nx = -(sample((x + 1) & (width - 1), y) - center) * scale
					- (center - sample((x - 1) & (width - 1), y)) * scale;
			const double ny = -static_cast<float>((sample(x, (y - 1) & (height - 1)) - center) * scale)
					- (center - sample(x, (y + 1) & (height - 1))) * scale;
			const double inverse_length = 1.0 / std::sqrt(nx * nx + ny * ny + 4.0);
			const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
			result[offset] = encode(nx * inverse_length);
			result[offset + 1] = encode(static_cast<float>(-ny * inverse_length));
			result[offset + 2] = encode(2.0 * inverse_length);
			result[offset + 3] = rgba[offset + alpha_channel];
		}
	}
	return result;
}

// [orig: Render_CreateCheckerboardTexture @0x5B1600]
std::vector<uint8_t> missing_material_texture_rgba() {
	std::vector<uint8_t> result(kMissingMaterialTextureSide * kMissingMaterialTextureSide * 4);
	for (uint32_t y = 0; y < kMissingMaterialTextureSide; ++y) {
		for (uint32_t x = 0; x < kMissingMaterialTextureSide; ++x) {
			const size_t offset = (y * kMissingMaterialTextureSide + x) * 4;
			const uint8_t gray = ((x ^ y) & 4) ? 0x50 : 0x30;
			result[offset] = result[offset + 1] = result[offset + 2] = gray;
			result[offset + 3] = 0xFF;
		}
	}
	return result;
}

} // namespace opennova::renderer
