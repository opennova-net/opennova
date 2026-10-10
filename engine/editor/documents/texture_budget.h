#pragma once

#include <cstdint>
#include <string>

#include <base/io/json.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_roles.h>
#include <runtime/renderer/device_texture.h>

namespace opennova::editor {

// What a texture costs the game (ADR 0046 S18, the texture budget): the device texture a model row's
// loader makes of the file it opens, at each object texture detail level, and what the same texture would
// cost as the `.dds` that loader reads first. The rules are the game's (runtime/renderer/device_texture.h,
// render-material-re.md "The device texture"); this reads them for a use, from the file's header alone.

// The model texture row loader a role's file is costed by (renderer::model_row_device_texture: the stage
// loader for a diffuse, a detail map or a flipbook frame, the plain loader, the normal-map loader); false
// for a role whose device texture is not witnessed yet (a terrain's, the HUD's, a menu's, a producer's
// volume).
bool texture_role_budget_loader(renderer::TextureRoleId role, renderer::TextureLoader &out);

// The cost a model texture holds past which the use check says so (texture.memory): 16 MB with its chain, a
// 2048 x 2048 texture uncompressed; no model texture the shipped game loads holds more than 1.3 MB (a 512 x
// 512 normal map, or a 1024 x 1024 DXT5: render-material-re.md, "The device texture").
inline constexpr uint64_t kTextureMemoryWarnBytes = uint64_t(16) * 1024 * 1024;

struct TextureBudget {
	bool known = false;
	renderer::TextureLoader loader = renderer::TextureLoader::Stage;
	uint8_t slot = 0;
	// S23 C: a role's budget no model row loads (texture_role_budget): its role (kCount for a model row's), how many
	// device textures of `detail`'s sides and levels the game makes of the file (a menu image's tiles, a terrain
	// map's four quadrants; 0 for a texture drawn into another's levels), whose `bytes` and `stat_bytes` are then
	// all of them together, and the setting whose levels `detail` runs over ("object_texdetail" a model row's,
	// "terrain_texdetail" the terrain's detail family's, "" where no setting halves it: the four levels alike).
	renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
	uint32_t count = 1;
	std::string setting = "object_texdetail";
	// The file the loader opens, by its logical name.
	std::string file;
	// The device texture at each object texture detail level, 0 the lowest, 3 full detail.
	renderer::DeviceTexture detail[renderer::kObjectTexDetailLevels];
	// The `.dds` the row's loader would read first, where it reads one and the file is no DDS: DXT5 for a
	// texture holding an alpha, else DXT1, with its full chain, at full detail.
	bool offers_dds = false;
	renderer::DeviceTexture as_dds;
	const renderer::DeviceTexture &full() const { return detail[renderer::kObjectTexDetailFull]; }
};

// The budget of the file `file` (its header as the loader's reader reads it) for a model row of `slot`
// loaded by `loader` (Stage, Plain or Normal); unknown where the header does not read.
TextureBudget texture_budget(const TextureHeader &header, const std::string &file, renderer::TextureLoader loader, uint8_t slot);

// S23 C: whether a role has a budget, a model row's (texture_role_budget_loader) or its own (texture_role_budget).
bool texture_role_has_budget(renderer::TextureRoleId role);
// The budget of the file `file` for a use of `role` that no model row loads, from the creation flags the role's
// loader makes its textures with (the fresh profile's texture compression word, texcompression_level 1, where the
// flags read it), at each level of the setting that halves it; unknown for a role whose device texture is not
// witnessed, or where the header does not read:
// - the HUD's: built from pixels with flags 0x140000, one level, A8R8G8B8 in colour mode and A8 alone in alpha
//   mode [orig: HUD_LoadImageAsTexture @ 0x5916AE..0x5916BE (pixel format 2), @ 0x5916FB..0x59170B (1);
//   GTexture_PixelFormatToD3DFormat @ 0x686D80: 2 is D3DFMT_A8, 1 A8R8G8B8];
// - a menu's image and cursor: cut into tiles, each its side's power of two (the card's largest side at most),
//   A8R8G8B8 with flags 0x140001, one level [orig: CTextureManager_LoadOrFindTexture @ 0x654DC4..0x654DD6;
//   GImage_CreateTiledTextures_0 @ 0x67A8B9 (the tile, sub_679DF0), @ 0x67AA30..0x67AA43
//   (flags | 1)]; a frame's stencil at its sides with 0x140000 and its brush (and mouse-over stencil) with 0x40000,
//   one level each [orig: CUIElement_ParseXMLDefinition @ 0x648899, @ 0x6488BC, @ 0x6488E2, through
//   GTexture_FindOrCreateFromData @ 0x654D92];
// - the terrain's colour and blend maps: four quadrants of half the file's width a side, flags 0x100001 with the
//   compression word (0x400200 at a texcompression_level of 1 or less: DXT1 on the reference card; none above:
//   A8R8G8B8), their full chains [orig: PolyTrn_InitTextures @ 0x60ABAD..0x60ABC6 (the word), @ 0x60B4FE..0x60B532,
//   @ 0x60B970..0x60B986]; the splat layers through the stage loader (a .dds beside the name first) with
//   0x400200, 0x8 and the terrain texture detail's halvings (0x20000 at level 0, 0x10000 at 1 and 2, none at 3),
//   DXT1 from pixels [orig: @ 0x60ABE5..0x60AC13; sub_605D70 @ 0x605DBF..0x605E0A from Terrain_Init
//   @ 0x60FC0D..0x60FC36]; the second detail map through the stage loader with 0x8 and the same halvings,
//   A8R8G8B8 from pixels [orig: @ 0x60AF80..0x60AF88]; its far pair drawn into its levels, no texture of its own
//   [orig: GTexture_CreateFromPixelDataWithAlphaBlend @ 0x60B01A]; the tile atlas at its sides with 0x100203, DXT5,
//   its full chain [orig: Terrain_LoadTileSetAtlas @ 0x604B24].
TextureBudget texture_role_budget(const TextureHeader &header, const std::string &file, renderer::TextureRoleId role);

// Bytes in words: "21.3 MB", "340 KB", "96 bytes".
std::string texture_bytes_words(uint64_t bytes);
// A device texture in words: "2048 x 2048, A8R8G8B8 (uncompressed), 10 levels: 21.3 MB".
std::string device_texture_words(const renderer::DeviceTexture &texture);
// The budget in a sentence: "21.3 MB in the game (2048 x 2048, A8R8G8B8 (uncompressed), 10 levels); 5.3 MB
// as a DXT5 .dds".
std::string texture_budget_words(const TextureBudget &budget);
// On the wire (texture_uses' `budget`): `loader`, `slot`, `file`, `detail` (a device texture a level, 0 to
// 3: `level`, `width`, `height`, `format`, `levels`, `bytes`, `stat_bytes`, `halvings`, `whole`,
// `dxt5_as_dxt1`), `as_dds` (one such, or null) and `words`.
io::JsonValue texture_budget_json(const TextureBudget &budget);

} // namespace opennova::editor
