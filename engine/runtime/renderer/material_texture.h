#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <vector>

namespace opennova::renderer {

enum class MaterialTextureTransform : uint8_t { Unchanged, NormalFromAlpha, Checkerboard };

// The texture-row dispatcher selects the loader by TYPE, not the sampler slot.
// MDT is already a normal map; TGA carries height in A and output alpha in B.
// [orig: sub_5B16F0 @0x5B16F0; load_texture_as_normalmap @0x58C480]
// DDS sibling wins over a TGA unless the session allows an existing loose
// TGA override. The selected file is decoded exactly: a broken DDS does not
// fall through to a different extension. [orig: load_texture_as_normalmap @0x58C480]
std::string normal_material_filename(std::string_view name,
        bool loose_tga_preferred, bool dds_exists);

MaterialTextureTransform material_texture_transform(
		uint8_t type, std::string_view name, bool loaded);

// Paired one-sided differences, dimension-1 wrapping, Z=2, and the original
// float stores before normalization/green packing. Input/output are RGBA8.
// [orig: Texture_ApplyNormalMapFilter @0x58BD90;
// load_texture_as_normalmap @0x58C985..0x58CAED]
std::vector<uint8_t> normal_map_from_height_rgba(const uint8_t *rgba,
		uint32_t width, uint32_t height, float scale,
		uint8_t height_channel = 2, uint8_t alpha_channel = 3);

inline constexpr uint32_t kMissingMaterialTextureSide = 128;
// Opaque gray 0x30 / 0x50 squares, four pixels wide.
// [orig: Render_CreateCheckerboardTexture @0x5B1600; default call @0x5B17F4]
std::vector<uint8_t> missing_material_texture_rgba();

} // namespace opennova::renderer
