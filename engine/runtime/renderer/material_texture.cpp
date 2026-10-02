#include "material_texture.h"

#include <base/io/strutil.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace opennova::renderer {

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
// cut, then ".dds" @0x7D8AD0; Texture_LoadAsNormalMap @0x58C644;
// sub_58A430 @0x58A4B4]
std::string material_dds_sibling(std::string_view name) {
	std::string result(name);
	const size_t dot = result.rfind('.');
	if (dot != std::string::npos)
		result.resize(dot);
	return result + ".dds";
}

namespace {

bool holds(const std::string &upper, const char *extension) {
	return upper.find(extension) != std::string::npos;
}

// The plain loaders dispatch on the upper-cased path in this order.
// [orig: Texture_LoadByNameWithChannel @0x58B66F..0x58B6E6;
// Texture_LoadAndRegister @0x58B80E..0x58B881]
MaterialTextureSource plain_source(std::string_view file) {
	const std::string upper = strutil::to_upper(file);
	if (holds(upper, ".TGA") || holds(upper, ".MDT"))
		return {std::string(file), MaterialTextureReader::Tga};
	if (holds(upper, ".PCX"))
		return {std::string(file), MaterialTextureReader::Pcx};
	return {};
}

// A .TGA name's height source: its DDS sibling unless the name is a
// loose-first hit or the files lack the sibling, else the name itself.
// [orig: Texture_LoadAsNormalMap @0x58C635..0x58C6F8; sub_58A430
// @0x58A4A8..0x58A556]
MaterialTextureSource tga_or_sibling(std::string_view name, const MaterialTextureFileTest &exists,
		const MaterialTextureFileTest &loose_first) {
	const std::string file(name);
	const std::string sibling = material_dds_sibling(name);
	if (!(loose_first && loose_first(file)) && exists && exists(sibling))
		return {sibling, MaterialTextureReader::Dds};
	return {file, MaterialTextureReader::Tga};
}

} // namespace

// [orig: Material_LoadStageTexture @0x5B16F0 (switch @0x5B1737)]
MaterialTextureSource material_texture_source(std::string_view name, uint8_t type,
		const MaterialTextureFileTest &exists, const MaterialTextureFileTest &loose_first) {
	const std::string upper = strutil::to_upper(name);
	switch (type) {
	case 0: case 2: case 8: {
		// [orig: Texture_LoadByNameWithChannel @0x58B4FE..0x58B5AD]
		const std::string query = material_texture_query(name);
		if ((loose_first && loose_first(query)) || query.find(".MDT") != std::string::npos)
			return plain_source(query);
		const std::string sibling = material_dds_sibling(query);
		if (exists && exists(sibling))
			return {sibling, MaterialTextureReader::Dds};
		return plain_source(query);
	}
	case 1:
		return plain_source(name);
	case 4: case 5:
		// [orig: Texture_LoadAsNormalMap @0x58C54C (.MDT), @0x58C612 (.TGA),
		// @0x58C77D (the PCX test on the empty second path)]
		if (holds(upper, ".MDT")) return {std::string(name), MaterialTextureReader::Tga};
		if (holds(upper, ".TGA")) return tga_or_sibling(name, exists, loose_first);
		return {};
	case 6: case 7:
		// [orig: sub_58A430 @0x58A496]
		if (holds(upper, ".TGA")) return tga_or_sibling(name, exists, loose_first);
		return {};
	case 16: case 17: case 18:
		// [orig: chunk loaders @0x58F350, @0x58F470, @0x58F590]
		return {std::string(name), MaterialTextureReader::Chunk};
	default:
		return {};
	}
}

namespace {

uint16_t u16_at(const uint8_t *bytes, size_t at) { return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8)); }
uint32_t u32_at(const uint8_t *bytes, size_t at) {
	return static_cast<uint32_t>(u16_at(bytes, at)) | static_cast<uint32_t>(u16_at(bytes, at + 2)) << 16;
}

// D3DX's TGA test before it reads any pixel: a colour-map type of 0 or 1, an
// image type of 1..3 (+8 run-length), both sides nonzero, a colour map's
// entry depth of 15, 16, 24 or 32, the pixel depth the image type takes (a
// mapped image 8 bits with a map, a true-colour one 15, 16, 24 or 32, a grey
// one 8), and room for the image ID and the colour map.
// [orig: CImage_LoadTGA @0x6DCA52..0x6DCB94, the map size @0x6DCBB3]
bool d3dx_takes_tga(const uint8_t *bytes, size_t size) {
	if (size < 18) return false;
	const uint8_t map_type = bytes[1], image_type = bytes[2], map_depth = bytes[7], depth = bytes[16];
	if ((map_type & 0xFE) != 0 || (image_type & 0xF4) != 0 || u16_at(bytes, 12) == 0 || u16_at(bytes, 14) == 0)
		return false;
	const auto true_colour = [](uint8_t bits) { return bits == 15 || bits == 16 || bits == 24 || bits == 32; };
	if (map_type != 0 && !true_colour(map_depth)) return false;
	switch (image_type & 3) {
	case 1:
		if (map_type == 0 || depth != 8) return false;
		break;
	case 2:
		if (!true_colour(depth)) return false;
		break;
	case 3:
		if (depth != 8) return false;
		break;
	default:
		return false;
	}
	const size_t after_header = size - 18;
	if (after_header < bytes[0]) return false;
	return after_header - bytes[0] >= static_cast<size_t>((map_depth + 7) >> 3) * u16_at(bytes, 5);
}

} // namespace

// [orig: D3DXTex::CImage::Load @0x6DF1DC: BMP, PPM, DDS, JPEG, PNG, PFM, HDR,
// TGA, DIB @0x6DF212..0x6DF242]
DdsReaderFormat dds_reader_format(const uint8_t *bytes, size_t size) {
	if (bytes == nullptr) return DdsReaderFormat::None;
	// [orig: LoadBMP @0x6DE17B..0x6DE195]
	if (size >= 14 && bytes[0] == 'B' && bytes[1] == 'M' && u32_at(bytes, 2) <= size) return DdsReaderFormat::Bmp;
	// [orig: LoadDDS @0x6DDA6E..0x6DDA91: "DDS " and a 124-byte header]
	if (size >= 4 + 124 && u32_at(bytes, 0) == 0x20534444u) return DdsReaderFormat::Dds;
	// [orig: CImage_LoadJPEG @0x6DC576 -> jpeg_read_header @0x71ACA0; the SOI
	// marker first, D3DX_JPEG_ReadSOI @0x71BF57]
	if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8) return DdsReaderFormat::Jpeg;
	// [orig: D3DXTex_LoadPNGFromMemory @0x6DD536, D3DX_PNG_SigCmp @0x71D37A]
	static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (size >= 8 && std::equal(png, png + 8, bytes)) return DdsReaderFormat::Png;
	if (d3dx_takes_tga(bytes, size)) return DdsReaderFormat::Tga;
	return DdsReaderFormat::None;
}

bool material_texture_dds_only(const char *name, uint8_t type, std::string &opens, std::string &loads) {
	if (name == nullptr || name[0] == '\0') return false;
	const uint8_t runtime = material_texture_runtime_type(type);
	const MaterialTextureSource bare = material_texture_source(name, runtime, {});
	const MaterialTextureSource served = material_texture_source(name, runtime, [](const std::string &) { return true; });
	if (bare.reader != MaterialTextureReader::None || served.reader != MaterialTextureReader::Dds) return false;
	opens = material_texture_query(name);
	loads = served.file;
	return !strutil::iequals(loads, opens);
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
