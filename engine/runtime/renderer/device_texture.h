#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <runtime/renderer/texture_dxt.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::renderer {

// The device texture the game makes of a texture file (render-material-re.md, "The device texture"):
// its sides after the halvings its creation flags ask for, its levels, its format and the bytes they
// hold. A texture is made in D3D's managed pool, so the runtime keeps a copy of every level in the
// game's own (32-bit) process besides the card's: what a texture costs the game is every level's bytes.
// These are the creation rules alone; the texels are the loaders' (texture_load_rules.h).

// The creation flags the size rules read: a cap on the larger side (a model normal map's 512), and the
// object texture detail's one or two halvings [orig: GTexture_DownsampleToLimits @ 0x687170].
inline constexpr uint32_t kTextureFlagCap512 = 0x1000;
inline constexpr uint32_t kTextureFlagCap256 = 0x2000;
inline constexpr uint32_t kTextureFlagCap128 = 0x4000;
inline constexpr uint32_t kTextureFlagHalveOnce = 0x10000;
inline constexpr uint32_t kTextureFlagHalveTwice = 0x20000;

// The object texture detail: the four levels of the options' setting (game.cfg `object_texdetail`
// [orig: Config_ParseSettingsLine @ 0x550CFB], clamped to 0..3 [orig: Settings_ClampGraphicsOptions
// @ 0x54D4F8..0x54D50D], which the game's render-settings readout labels "Object Tex Detail" [orig:
// Debug_DrawRenderSettings @ 0x44BD04]), from 0, the lowest, to 3, full detail. The session's settings are
// copied in with the rest [orig: apply_session_settings_to_globals @ 0x551574, 0x34 bytes from 0x25507C4,
// the setting at +0xC] and Game_StartMission hands the level to the setter below as each mission starts
// [orig: @ 0x524A77..0x524A7D].
inline constexpr int kObjectTexDetailLevels = 4;
inline constexpr int kObjectTexDetailFull = 3;

// The halving flag a model texture row of `slot` loads with at object texture detail `level`: the setter
// stores one flag word for slot 1, one for slot 2 and one for slots 8 and 9 (level 0: two halvings each;
// 1: one, one and two; 2: none, one and one; 3 and up: none), and the stage loader passes the slot's word
// as the creation flags, slots 3 to 7 (the normal maps and the height producers) none
// [orig: Material_SetTextureDetailLoadFlags @ 0x5B1F40; Material_LoadStageTexture @ 0x5B16F4..0x5B1723, the slot table
// @ 0x5B181C].
uint32_t object_texdetail_flags(int level, uint8_t slot);

// The terrain texture detail: game.cfg `terrain_texdetail` (0..3, 3 full detail) [orig:
// Config_ParseSettingsLine @ 0x551135], copied into the session's settings with the rest [orig:
// Game_ApplySessionSettingsToGlobals @ 0x551565..0x551574, g_TerrainTexDetail @ 0x24D2044], which
// Terrain_Init loads and hands the terrain's settings setter [orig: Terrain_Init @ 0x60FC0D, the push
// @ 0x60FC2A..0x60FC36]; nothing writes it mid-mission.
inline constexpr int kTerrainTexDetailFull = 3;
// The word a fresh profile starts at: the first-launch quality pass writes quality_levels[1] = 3 on a
// card with the shader-model-2 mark and a CPU past 2980 MHz, else 2, one less without more than 120 MB
// of video memory, plus the preset offset (0 on the first launch), then the 0..3 clamp; the reference
// machine has all three, so full detail [orig: Renderer_ComputeQualityLevels @0x58617E..0x586193
// (caps[8] && caps[0] ? 3 : 2, --without caps[3]), the clamp @0x58629A..0x5862A9; Renderer_ComputeHardwareQualityLevels's caps
// @0x5877F5 (CPU > 2980 MHz), @0x587834 (display memory > 0x7800000), @0x58787A (the SM2 mark);
// the first launch's offset 0 @0x53ED0D].
inline constexpr int kTerrainTexDetailFreshProfile = kTerrainTexDetailFull;

// The halving flag the terrain's detail (splat) layers load with at terrain texture detail `level`:
// two halvings at 0, one at 1 or 2, none at any other [orig: Terrain_ApplyDetailSettings @ 0x605DBF (0x20000),
// @ 0x605DE4 (0x10000), @ 0x605DF4..0x605E0A (2 -> 0x10000, else 0) into g_TerrainDetailTextureFlags
// @ 0x31A1820, which PolyTrn_InitTextures ORs into the layers' word @ 0x60AB95..0x60ABE5].
uint32_t terrain_texdetail_flags(int level);

// The largest side the reference card takes (D3DCAPS9 MaxTextureWidth, which CD3DDevice_InitializeDisplay
// stores for the halvings [orig: @ 0x679AC2..0x679ACA, dword_32A623C]); a card that takes less halves a
// texture past its side. Not a witness: the reference machine, as kReferenceTextureDxtCaps is.
inline constexpr uint32_t kReferenceMaxTextureSide = 8192;

// How many times the game halves a texture it builds from pixels before it makes the device texture:
// while either side exceeds the cap (`flags`' 0x1000, 0x2000 or 0x4000, else the card's side), then
// twice for 0x20000 or once for 0x10000 whatever the size, then while either side exceeds the card's
// [orig: GTexture_DownsampleToLimits @ 0x687170, the cap @ 0x68718E..0x6871BD, the detail
// @ 0x6871C8..0x6871E1, the card @ 0x6871EA].
uint32_t pixel_texture_halvings(uint32_t width, uint32_t height, uint32_t flags,
		uint32_t max_side = kReferenceMaxTextureSide);

// A device texture's format: the A8R8G8B8 every texture built from pixels is (outside the terrain's DXT
// layers), a DDS's DXT1, DXT3 or DXT5, or another uncompressed format of `bits` a texel.
enum class DeviceTextureFormat : uint8_t { A8R8G8B8, Dxt1, Dxt3, Dxt5, Uncompressed };
const char *device_texture_format_name(DeviceTextureFormat format);

struct DeviceTexture {
	DeviceTextureFormat format = DeviceTextureFormat::A8R8G8B8;
	uint32_t bits = 32; // an uncompressed format's bits a texel
	uint32_t width = 0, height = 0;
	uint32_t levels = 0;
	// The halvings from the file's sides (a pixel texture's), or the levels of a DDS's chain skipped.
	uint32_t halvings = 0;
	// A DDS loaded whole although the detail asked for a smaller one: its chain held too few levels to
	// skip (GTexture_InitFromMemory @ 0x68820A).
	bool whole = false;
	// A DXT5 the game stores as DXT1, every block of its first level opaque (GTexture_ConvertDXTAlphaBlocks).
	bool dxt5_as_dxt1 = false;
	// Every level's bytes: what the texture holds in the game's memory.
	uint64_t bytes = 0;
	// What the game's own texture memory counter adds for it, its first level alone (texture_stat_bytes).
	uint64_t stat_bytes = 0;
};

// One level's bytes on the device: a DXT's blocks (4 x 4 texels a block, 8 bytes for DXT1 and 16 for
// DXT3 or DXT5, at least one block a side), else bits a texel.
uint64_t device_level_bytes(uint32_t width, uint32_t height, DeviceTextureFormat format, uint32_t bits = 32);

// The levels of a full chain: one per halving of the larger side, to 1 x 1 (D3DX's complete chain).
uint32_t full_chain_levels(uint32_t width, uint32_t height);

// What the game's texture memory counter adds for a texture of `width` x `height`: DXT1 half a byte a
// texel, DXT3 and DXT5 one, any other format its bytes a texel, the first level alone [orig:
// GTexture_StatBytesForFormat @ 0x686EF0; added to g_TextureStatBytes by GTexture_CreateFromPixelData_0
// @ 0x6877A3..0x6877AB and GTexture_InitFromMemory @ 0x688392..0x688398].
uint64_t texture_stat_bytes(uint32_t width, uint32_t height, DeviceTextureFormat format, uint32_t bits = 32);

// The device texture of a texture the game builds from pixels (a TGA, MDT or PCX its readers decode,
// every normal map), `width` x `height` as decoded: halved by pixel_texture_halvings (a side halved to 0
// D3DX makes 1 [orig: D3DXTex_ValidateAndAdjustTextureParams @ 0x690A2B, @ 0x690A37]), A8R8G8B8, with
// the levels GTexture_CreateFromPixelData_0 asks for (texture_level_count, a count of 0 D3DX's full chain)
// [orig: GTexture_CreateFromPixelData_0 @ 0x687785..0x68781A].
DeviceTexture pixel_device_texture(uint32_t width, uint32_t height, uint32_t flags,
		uint32_t max_side = kReferenceMaxTextureSide);

// One level of a device texture as its texels read back: RGBA8, row-major.
struct DeviceTextureLevel {
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

// The levels of the device texture the game builds from `width` x `height` RGBA8 pixels under
// `flags`, as their texels decode: halved as pixel_texture_halvings asks (each halving
// GTexture_Downsample2x2_RGBA8, a side able to reach 0), in the format select_texture_dxt_format
// picks for `caps`, at D3DX's sides (d3dx_texture_side: a 0 side made 1, a DXT side rounded up to a
// multiple of 4) with D3DX's levels for the request over the halved sides (texture_level_count; at
// most `level_limit` of them when it is not 0). Level 0 is the creator's FILTER_NONE load: its
// source rect the creator's own un-halved sides, at the halved pitch (load_rgba8_filter_none), so a
// halved texture widened by the rounding reads on into the next halved row and its unwritten tail
// as zero. When the count it asked for is above 1 every later level is D3DX's box filter of the
// level before (an A8R8G8B8 level of the bytes it was stored as, a DXT level of its blocks); a
// texture that asked for 0 levels (the smaller side 2 or less, no 0x40000) gets D3DX's full chain
// with level 0 alone loaded and its levels 1+ zero here (D-RMAT-27). A DXT texture's blocks go to
// `dxt` too when it is given (its unloaded levels zero blocks).
// [orig: GTexture_CreateFromPixelData_0 @ 0x687717..0x687766 (the format), @ 0x687785 (the
// halvings), @ 0x6877BA..0x6877E0 (the count), @ 0x68781A (D3DXCreateTexture), @ 0x6878A0 (level 0,
// D3DXLoadSurfaceFromMemory: no destination rect @ 0x687895, the source rect {0, 0, width, height}
// of the creator's arguments @ 0x687869..0x68789C, the pitch 4 x the halved width
// @ 0x687876..0x68788A, D3DX_FILTER_NONE @ 0x68787E), @ 0x6878B1..0x6878BE (D3DXFilterTexture,
// D3DX_FILTER_BOX, each level from the one before, for a count above 1)].
std::vector<DeviceTextureLevel> pixel_device_texture_levels(const uint8_t *rgba, uint32_t width,
		uint32_t height, uint32_t flags, const TextureDxtCaps &caps = kReferenceTextureDxtCaps,
		uint32_t level_limit = 0, uint32_t max_side = kReferenceMaxTextureSide,
		std::vector<DxtSurface> *dxt = nullptr);

// What a DDS file states of itself, as GTexture_InitFromMemory reads it.
struct DdsSource {
	uint32_t width = 0, height = 0;
	uint32_t levels = 1; // the file's chain (its mip count, 0 read as 1)
	DeviceTextureFormat format = DeviceTextureFormat::Dxt5;
	uint32_t bits = 32; // an uncompressed format's
	// Every block of a DXT5's first level has both alpha endpoints 255 (dxt5_first_level_opaque).
	bool dxt5_opaque = false;
};

// The device texture of a DDS the game loads through D3DX (a model row's `.dds`, the archive loader's):
// an all-opaque DXT5 stored as DXT1 on a card that takes DXT1 and keeps no DXT5 [orig:
// GTexture_InitFromMemory @ 0x687E90..0x687EB9; GTexture_ConvertDXTAlphaBlocks @ 0x687AC0]; the detail's
// halvings skip the top levels of its chain (0x20000 two, 0x10000 one [orig: @ 0x687E3B..0x687E49]), and
// once a skip is taken so does each halving the cap or the card asks for [orig: @ 0x6881AC..0x6881FC]; a
// chain too short for the skip loads whole [orig: @ 0x68820A, @ 0x6882DE]; with no skip the file goes
// through D3DX with MipLevels 0, its complete chain at the file's sides whatever the cap [orig: @ 0x688315].
// On a card without the format the levels from the skip are decoded into A8R8G8B8 [orig: @ 0x687F5D,
// @ 0x687F9F..0x6880E0]; the reference card takes both DXTs.
DeviceTexture dds_device_texture(const DdsSource &source, uint32_t flags,
		const TextureDxtCaps &caps = kReferenceTextureDxtCaps, uint32_t max_side = kReferenceMaxTextureSide);

// A DDS's format as its device texture takes it, by the name of the Direct3D format D3DX matches
// the file's pixel format to (dds::DdsFormat::name): the DXTs by their blocks (DXT2 and DXT4, the
// premultiplied twins, by DXT3's and DXT5's), A8R8G8B8 (or no name), any other format uncompressed
// at its bits a texel (DdsSource::bits).
DeviceTextureFormat dds_device_format(std::string_view format_name);

// The side the DDS reader's texture is made at for an image side of `side`: the reader asks D3DX
// for a texture of D3DX_DEFAULT sides, which rounds each up to a power of two, and under Filter NONE
// copies the image into its top-left corner, transparent black past it [orig: GTexture_InitFromMemory
// @ 0x68830E..0x688310; D3DXCreateTextureFromFileInMemoryEx_Internal @ 0x6914D9..0x6914EE,
// @ 0x69150B..0x691520; CBlt::BltNone @ 0x6E0D57] (render-material-re D-RMAT-18).
uint32_t d3dx_default_texture_side(uint32_t side);

// The creation flags a model texture row's loader (`loader` Stage, Plain or Normal: material_texture
// .h's dispatch by the row's runtime type) makes its texture with at object texture detail `level`:
// the stage and plain loaders the row's slot's detail word (object_texdetail_flags), the normal-map
// loader that word and the 512 cap [orig: Material_LoadStageTexture @ 0x5B173E..0x5B1742 (stage),
// @ 0x5B174F..0x5B1758 (plain), @ 0x5B1782..0x5B1790 (normal: or eax, 1000h)].
uint32_t model_row_texture_flags(TextureLoader loader, uint8_t slot, int level);

// The device texture a model texture row's loader makes of the file it opens at object texture detail
// `level`, under model_row_texture_flags: `file` its sides and, where it is a DDS (`dds`), what it
// states of itself. Only the stage loader reads a DDS through D3DX (dds_device_texture) [orig:
// Texture_LoadByNameWithChannel @ 0x58B616, GTexture_FindOrCreateByName over GTexture_InitFromMemory];
// it builds every other file from its pixels (pixel_device_texture) [orig: @ 0x58B74D,
// GTexture_FindOrCreateFromData], the plain loader reads pixels alone [orig: Texture_LoadAndRegister
// @ 0x58B920], and the normal-map loader decodes a DDS into pixels before it converts them [orig:
// Texture_LoadAsNormalMap @ 0x58C6CB, Texture_DecompressDDSFromPFF32 @ 0x56E450], so its texture is
// always built from pixels.
DeviceTexture model_row_device_texture(TextureLoader loader, uint8_t slot, int level, const DdsSource &file,
		bool dds);

// GTexture_ConvertDXTAlphaBlocks's test of a DXT5's first level: the first word of each of its blocks (both
// alpha endpoints) 0xFFFF, over max(1, width / 4) x max(1, height / 4) blocks from the level's start
// [orig: GTexture_ConvertDXTAlphaBlocks @ 0x687B28..0x687B6A]. False for bytes too few to hold them.
bool dxt5_first_level_opaque(const uint8_t *blocks, size_t size, uint32_t width, uint32_t height);

} // namespace opennova::renderer
