#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <vector>

namespace opennova::renderer {

enum class MaterialTextureTransform : uint8_t { Unchanged, NormalFromAlpha, HorizonVolume, AmbientOcclusion, ChunkNormal, ChunkHorizon, ChunkOcclusion, Checkerboard };

// The texture-row dispatcher selects the loader by TYPE, not the sampler slot.
// MDT is already a normal map; TGA carries height in A and output alpha in B.
// [orig: sub_5B16F0 @0x5B16F0; load_texture_as_normalmap @0x58C480]
// DDS sibling wins over a TGA unless the session allows an existing loose
// TGA override. The selected file is decoded exactly: a broken DDS does not
// fall through to a different extension. [orig: load_texture_as_normalmap @0x58C480]
std::string normal_material_filename(std::string_view name,
        bool loose_tga_preferred, bool dds_exists);

// How a diffuse-family material row reaches its pixels: which file, and which
// retail decoder reads it. None is a failed load (the dispatcher's
// checkerboard).
enum class MaterialImageDecoder : uint8_t { None, Dds, Tga, Pcx };
struct MaterialImageSource {
	std::string file;
	MaterialImageDecoder decoder = MaterialImageDecoder::None;
};

// The file query Texture_LoadByNameWithChannel works on: the name cut three
// characters after its first '.' ("x.dds.tga" -> "x.dds").
// [orig: Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B4FA]
std::string material_texture_query(std::string_view name);
// The DDS sibling it probes: the query up to its last '.' plus ".dds".
// [orig: Texture_LoadByNameWithChannel @ 0x58B53C..0x58B598]
std::string material_dds_sibling(std::string_view query);
// Runtime types 0, 2 and 8: an existing loose file under loose-first, or a
// query containing ".MDT" (case-sensitive), takes the plain path; otherwise
// an existing DDS sibling wins and is decoded as DDS (a broken one fails, no
// fallback). The plain path decodes by the upper-cased extension: .TGA and
// .MDT through the TGA reader, .PCX through the PCX reader, anything else
// fails. [orig: Texture_LoadByNameWithChannel @ 0x58B4FE..0x58B6E6]
MaterialImageSource material_image_source(std::string_view query,
		bool loose_first_hit, bool dds_exists);
// Runtime type 1: the plain path on the full name, no DDS probe.
// [orig: load_texture_and_register @ 0x58B80E..0x58B881]
MaterialImageSource plain_material_image_source(std::string_view name);

// The mip chain of a texture built from decoded pixels (TGA/MDT/PCX rows,
// every normal map, the missing-texture checkerboard): one level per halving
// while the smaller side exceeds 2, so a 256x256 chain ends at 4x4; a texture
// whose smaller side is already 2 or less gets D3DX's full chain (0 here).
// DDS rows keep their file's chain instead.
// [orig: GTexture_CreateFromPixelData_0 @ 0x6877BC..0x6877D8 (MipLevels),
//  @ 0x6878B9 (D3DXFilterTexture BOX); GTexture_FindOrCreateFromData @ 0x676CC0]
uint32_t pixel_texture_mip_levels(uint32_t width, uint32_t height);

// The loader copies an authored texture type into the runtime row only for
// the dispatcher's producers (0..2, 4..8, 16..18); 3, 9..15 and anything past
// 18 leave the runtime byte at the record memset's zero, an ordinary diffuse
// load. material_texture_transform takes this runtime type.
// [orig: ThreediGp_LoadFromFile memset @ 0x5B59E5; convert_material_definition
// switch @ 0x5B045B..0x5B04A0 over byte_5B0778]
uint8_t material_texture_runtime_type(uint8_t authored_type);

// Specialized producer types preserve their dimensionality: 6 and 17 are
// volumes, 7 is the retail white AO producer, 16/18 load NQ8B/AOC8 chunks.
// [orig: jpt_5B1737 switch @0x5B1737; the single result test @0x5B17F0 and
// the checkerboard default @0x5B17F4..0x5B1800]
// A missing MDT is the one failed row retail does not checkerboard: the
// absent file jumps into the null-data kernel walk (jz @0x58C586), the pixel
// loop is skipped (jle @0x58C901) and GTexture_FindOrCreateFromData
// (call @0x58CB56) creates a 1x1 never-filled texture because D3DX corrects
// the 0x0 request to 1x1 (@0x690A2B / @0x690A37), so the nonzero handle skips
// the checkerboard @0x5B17F2. The port binds the checkerboard for it as its
// bounded fallback: an unfilled device texture is garbage (ADR 0003).
MaterialTextureTransform material_texture_transform(
		uint8_t type, std::string_view name, bool loaded);

// Paired one-sided differences, dimension-1 wrapping, Z=2, and the original
// float stores before normalization/green packing. Input/output are RGBA8.
// [orig: load_texture_as_normalmap @0x58C985..0x58CAED (the live type-4/5
// kernel); Texture_ApplyNormalMapFilter @0x58BD90..0x58C06C (its uncalled
// instruction-for-instruction twin)]
std::vector<uint8_t> normal_map_from_height_rgba(const uint8_t *rgba,
		uint32_t width, uint32_t height, float scale,
		uint8_t height_channel = 2, uint8_t alpha_channel = 3);

struct MaterialTexturePixels {
    uint32_t width = 0, height = 0, depth = 0;
    std::vector<uint8_t> rgba;
    explicit operator bool() const { return !rgba.empty(); }
};
// [orig: generate_environment_map @0x58A220; AO generator @0x58CB90]
MaterialTexturePixels horizon_volume_from_height(const uint8_t *rgba, uint32_t width, uint32_t height);
MaterialTexturePixels ambient_occlusion_from_height(const uint8_t *rgba, uint32_t width, uint32_t height);
// [orig: NQ8B @0x58F350; HRZ8 @0x58F470; AOC8 @0x58F590]
MaterialTexturePixels load_material_chunk(const uint8_t *bytes, size_t size, uint8_t type);

inline constexpr uint32_t kMissingMaterialTextureSide = 128;
// Opaque gray 0x30 / 0x50 squares, four pixels wide.
// [orig: Render_CreateCheckerboardTexture @0x5B1600; default call @0x5B17F4]
std::vector<uint8_t> missing_material_texture_rgba();

} // namespace opennova::renderer
