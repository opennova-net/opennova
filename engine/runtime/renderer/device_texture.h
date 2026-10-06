#pragma once

#include <cstddef>
#include <cstdint>

#include <runtime/renderer/texture_dxt.h>

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
// [orig: Render_SetObjectTexDetailFlags @ 0x5B1F40; Material_LoadStageTexture @ 0x5B16F4..0x5B1723, the slot table
// @ 0x5B181C].
uint32_t object_texdetail_flags(int level, uint8_t slot);

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
	// A DXT5 the game stores as DXT1, every block of its first level opaque (convert_dxt_alpha_blocks).
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
// GTexture_InitFromMemory @ 0x687E90..0x687EB9; convert_dxt_alpha_blocks @ 0x687AC0]; the detail's
// halvings skip the top levels of its chain (0x20000 two, 0x10000 one [orig: @ 0x687E3B..0x687E49]), and
// once a skip is taken so does each halving the cap or the card asks for [orig: @ 0x6881AC..0x6881FC]; a
// chain too short for the skip loads whole [orig: @ 0x68820A, @ 0x6882DE]; with no skip the file goes
// through D3DX with MipLevels 0, its complete chain at the file's sides whatever the cap [orig: @ 0x688315].
// On a card without the format the levels from the skip are decoded into A8R8G8B8 [orig: @ 0x687F5D,
// @ 0x687F9F..0x6880E0]; the reference card takes both DXTs.
DeviceTexture dds_device_texture(const DdsSource &source, uint32_t flags,
		const TextureDxtCaps &caps = kReferenceTextureDxtCaps, uint32_t max_side = kReferenceMaxTextureSide);

// convert_dxt_alpha_blocks's test of a DXT5's first level: the first word of each of its blocks (both
// alpha endpoints) 0xFFFF, over max(1, width / 4) x max(1, height / 4) blocks from the level's start
// [orig: convert_dxt_alpha_blocks @ 0x687B28..0x687B6A]. False for bytes too few to hold them.
bool dxt5_first_level_opaque(const uint8_t *blocks, size_t size, uint32_t width, uint32_t height);

} // namespace opennova::renderer
