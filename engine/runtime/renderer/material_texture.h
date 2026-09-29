#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <string>
#include <vector>

namespace opennova::renderer {

enum class MaterialTextureTransform : uint8_t { Unchanged, NormalFromAlpha, HorizonVolume, AmbientOcclusion, ChunkNormal, ChunkHorizon, ChunkOcclusion, Checkerboard };

// The reader a material row's loader decodes its one file with, chosen by the
// row's type and the name, never by the file's bytes: the DDS reader (the file
// handed whole to D3DX, which decodes it by its content: dds_reader_format),
// the TGA reader (a TGA header parse, which reads .TGA
// and .MDT files alike and no other format), the PCX reader, or the chunk
// container the dedicated producers read. None is a failed load (the
// dispatcher's checkerboard).
// [orig: Texture_LoadDDSFromPFF @ 0x56E3C0; Texture_DecompressDDSFromPFF32 @
// 0x56E450; CTerrainTileData_LoadTGAFromArchive @ 0x56E570;
// Texture_LoadPCXFromPFF32 @ 0x56EA30]
enum class MaterialTextureReader : uint8_t { None, Dds, Tga, Pcx, Chunk };
struct MaterialTextureSource {
	std::string file; // empty with None: the loader opens no file
	MaterialTextureReader reader = MaterialTextureReader::None;
};
// What the DDS reader decodes a file as. It hands the bytes whole to D3DX
// (GTexture_InitFromMemory's D3DXCreateTextureFromFileInMemoryEx), whose
// loader tries its formats in a fixed order and takes the first that accepts
// them: BMP, PPM, DDS, JPEG, PNG, PFM, HDR, TGA, DIB. So a .dds holding a
// TGA (or a PNG, JPEG or BMP) loads as that image. These are the formats of
// that order the port decodes, each by D3DX's own acceptance test: "BM" with
// a file size that fits, "DDS " with its 124-byte header, libjpeg's SOI
// marker, the PNG signature, and the TGA header checks. None for anything
// else: no bytes a PPM, PFM or HDR test accepts pass a later test of these
// (their second byte fails the TGA test's colour-map type), so skipping them
// changes no answer, and the port decodes neither those nor a bare DIB (tried
// after the TGA), which fail to the checkerboard where D3DX would decode them.
// [orig: D3DXTex::CImage::Load @ 0x6DF1DC, the order @ 0x6DF212..0x6DF242,
// the loop @ 0x6DF286..0x6DF397; LoadBMP @ 0x6DE17B; LoadDDS @
// 0x6DDA66..0x6DDA91; CImage_LoadJPEG @ 0x6DC576 (D3DX_JPEG_ReadSOI @
// 0x71BEF0); D3DXTex_LoadPNGFromMemory @
// 0x6DD513; CImage_LoadTGA @ 0x6DCA3F..0x6DCBB3; reached from
// GTexture_InitFromMemory @ 0x687DF0 through D3DXCreateTextureFromFileInMemoryEx
// @ 0x691C30]
enum class DdsReaderFormat : uint8_t { None, Bmp, Dds, Jpeg, Png, Tga };
DdsReaderFormat dds_reader_format(const uint8_t *bytes, size_t size);

// A test on a file name: whether the files hold it (`exists`), or whether the
// session serves its loose file first (`loose_first`: the loose-first search
// policy with that loose file there). An empty test answers false.
using MaterialTextureFileTest = std::function<bool(const std::string &)>;

// The file query Texture_LoadByNameWithChannel works on: the name cut three
// characters after its first '.' ("x.dds.tga" -> "x.dds").
// [orig: Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B4FA]
std::string material_texture_query(std::string_view name);
// The DDS sibling a loader probes: the name up to its last '.' plus ".dds".
// [orig: Texture_LoadByNameWithChannel @ 0x58B53C..0x58B598; Texture_LoadAsNormalMap
// @ 0x58C644; sub_58A430 @ 0x58A4B4]
std::string material_dds_sibling(std::string_view name);
// The one file a material row of runtime `type` (material_texture_runtime_type)
// loads and the reader that decodes it. The dispatcher picks the loader by the
// row's type [orig: Material_LoadStageTexture @ 0x5B16F0, switch @ 0x5B1737];
// each opens exactly one file, and a file its reader cannot decode (a missing
// one included) is a failed load, never a fall-through to another file:
// - 0, 2 and 8: the query (material_texture_query). A loose-first hit on it, or
//   a query holding ".MDT" (case-sensitive), takes the plain path; else the
//   query's DDS sibling, when the files have it, through the DDS reader; else
//   the plain path: the query through the TGA reader when its upper-cased name
//   holds .TGA or .MDT, else through the PCX reader when it holds .PCX, else
//   none. [orig: Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B6E6]
// - 1: the plain path on the whole name, no DDS probe.
//   [orig: Texture_LoadAndRegister @ 0x58B80E..0x58B881]
// - 4 and 5, the normal maps: a name whose upper-cased form holds .MDT through
//   the TGA reader; else one holding .TGA: its DDS sibling through the DDS
//   reader when the files have it and the name is no loose-first hit, else the
//   name through the TGA reader; else none, since the loader's PCX test reads
//   its second path, which this dispatcher passes empty.
//   [orig: Texture_LoadAsNormalMap @ 0x58C480: .MDT @ 0x58C54C, .TGA @ 0x58C612,
//   loose-first @ 0x58C684, the DDS probe @ 0x58C6CB, the PCX test @ 0x58C77D;
//   the empty path @ 0x5B178A]
// - 6 and 7, the horizon volume and the occlusion map: the normal maps' .TGA
//   rule alone, no .MDT. [orig: sub_58A430 @ 0x58A430 (.TGA @ 0x58A496,
//   loose-first @ 0x58A4F5, the DDS probe @ 0x58A53E), from sub_58A580 @ 0x58A5DA
//   and sub_58CE10 @ 0x58CE6A]
// - 16 to 18: the name as written, read as a chunk container whatever it is
//   called. [orig: dispatch @ 0x5B17D4, @ 0x5B17DD, @ 0x5B17E6; chunk loaders
//   @ 0x58F350, @ 0x58F470, @ 0x58F590]
// - any other type: none, the dispatcher's default (@ 0x5B17F4).
MaterialTextureSource material_texture_source(std::string_view name, uint8_t type,
		const MaterialTextureFileTest &exists, const MaterialTextureFileTest &loose_first = {});
// Whether a model's texture row loads only as a DDS: with no file there its
// loader (material_texture_source, over material_texture_runtime_type of the
// authored `type`) decodes nothing, and with every file there it takes a DDS
// sibling that is not the file it opens, so the row loads only when that
// sibling lies beside it. Only a row of runtime type 0, 2 or 8 whose query
// (material_texture_query) is no .tga, .mdt or .pcx file does. True with `opens`
// the query and `loads` the sibling; false for an empty name (a row with no
// file). The lookup the `.o3d` reader takes (formats/threedi/threedi_o3d_read.h,
// ThreediTextureLookup), which cannot include this header.
// [orig: Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B4FA,
// @ 0x58B53C..0x58B598, @ 0x58B66F..0x58B6E6]
bool material_texture_dds_only(const char *name, uint8_t type, std::string &opens, std::string &loads);

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
// [orig: ThreediGp_LoadFromFile memset @ 0x5B59E5; Material_ConvertDefinition
// switch @ 0x5B045B..0x5B04A0 over byte_5B0778]
uint8_t material_texture_runtime_type(uint8_t authored_type);

// Specialized producer types preserve their dimensionality: 6 and 17 are
// volumes, 7 is the retail white AO producer, 16/18 load NQ8B/AOC8 chunks.
// [orig: jpt_5B1737 switch @0x5B1737; the single result test @0x5B17F0 and
// the checkerboard default @0x5B17F4..0x5B1800]
// A normal map's .MDT is already converted; its .TGA, or that name's DDS
// sibling, carries height in A and output alpha in B.
// [orig: Texture_LoadAsNormalMap @0x58C480, the swap @0x58C715..0x58C737]
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
// [orig: Texture_LoadAsNormalMap @0x58C985..0x58CAED (the live type-4/5
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
// [orig: Texture_GenerateEnvironmentMap @0x58A220; AO generator @0x58CB90]
MaterialTexturePixels horizon_volume_from_height(const uint8_t *rgba, uint32_t width, uint32_t height);
MaterialTexturePixels ambient_occlusion_from_height(const uint8_t *rgba, uint32_t width, uint32_t height);
// [orig: NQ8B @0x58F350; HRZ8 @0x58F470; AOC8 @0x58F590]
MaterialTexturePixels load_material_chunk(const uint8_t *bytes, size_t size, uint8_t type);

inline constexpr uint32_t kMissingMaterialTextureSide = 128;
// Opaque gray 0x30 / 0x50 squares, four pixels wide.
// [orig: Render_CreateCheckerboardTexture @0x5B1600; default call @0x5B17F4]
std::vector<uint8_t> missing_material_texture_rgba();

} // namespace opennova::renderer
