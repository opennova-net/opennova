#pragma once

// The texture roles: every way the game uses a texture file, each a row of the loader that opens
// it (texture_load_rules.h), the file formats that loader takes, what the role asks of the file's
// sides and whether the loader reads its alpha at all. The RE record's table as data
// (docs/render/render-material-re.md, "Texture roles"); texture_roles.cpp holds each row's
// witness. Beside it: the def texture fields' roles, and what a model texture row's alpha is to
// the technique its material's shader selects.

#include <runtime/renderer/texture_load_rules.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::renderer {

enum class TextureRoleId : uint8_t {
	// Models (.3di material rows)
	ModelDiffuse,
	ModelDetail,
	ModelFlipFrame,
	ModelPlain,
	ModelNormalMap,
	ModelHeightNormal,
	ModelHorizon,
	ModelOcclusion,
	ModelChunk,
	// Terrain (.trn)
	TerrainColourMap,
	TerrainBlendMap,
	TerrainSplatDetail,
	TerrainFarDetail,
	TerrainDetailCoefficient,
	TerrainSecondDetail,
	TerrainTileAtlas,
	TerrainScorch,
	TerrainFoliageMap,
	TerrainCharMap,
	// Environment
	SkyCloud,
	WaterWake,
	WeatherDrop,
	PreviewCube,
	// Effects
	ParticleGraphic,
	ImpactScar,
	TracerSmoke,
	FaceTexture,
	SightCard,
	ViewEffect,
	// HUD
	HudColour,
	HudAlphaOnly,
	HudFileArt,
	HudAttitude,
	HudMfd,
	MapOverview,
	NetIcon,
	TipArt,
	BoardBox,
	// Menus and screens
	MenuImage,
	MenuFrameStencil,
	MenuFrameBrush,
	MenuCursor,
	LoadingScreen,
	BootSplash,
	SplashCursor,
	CinematicFade,
	kCount,
};
inline constexpr size_t kTextureRoleCount = static_cast<size_t>(TextureRoleId::kCount);

// The file formats a role's loader takes, a bit each.
enum TextureFormatBits : uint8_t {
	kTextureTga = 1 << 0,
	kTextureMdt = 1 << 1,
	kTexturePcx = 1 << 2,
	kTextureDds = 1 << 3,
	kTexturePng = 1 << 4,
};

// What a role asks of a texture's sides (`width` and `height` the row's numbers).
enum class TextureSizeRule : uint8_t {
	Any, // any size the device takes (halved past its largest)
	PowerOfTwo, // both sides powers of two (a wrap or a lookup reads past otherwise)
	Exact, // exactly `width` x `height`
	SquarePowerOfTwoAtMost, // square, a power of two, at most `width`
	MultipleOf, // each side a multiple of `width`
	AtMost, // halved to fit `width` a side
	QuadrantSplit, // at least as tall as wide: split in quadrants at its width, past it otherwise
	AtlasPage, // narrower than its atlas page and no taller: `width` (1024), or `height` (256) by its mode
	Unknown, // not witnessed yet
};

// One role: the loader its file goes through (the name rule that picks the file and its reader:
// texture_load_attempts for the loaders that name files, a model row's type for Normal, Producer
// and Chunk), the formats that loader takes, its size rule, and whether the loader reads the
// file's alpha at all (a PCX loaded opaque fails a role that reads it).
struct TextureRole {
	TextureRoleId id = TextureRoleId::kCount;
	TextureLoader loader = TextureLoader::Stage;
	uint8_t formats = 0;
	TextureSizeRule size = TextureSizeRule::Any;
	uint32_t width = 0, height = 0;
	bool reads_alpha = true;
};

// The role's row; the first row for an id past the table.
const TextureRole &texture_role(TextureRoleId id);
// Whether the role's loader takes a file of `extension` (".tga", ".dds", ...; any case).
bool texture_role_takes(const TextureRole &role, std::string_view extension);
// The extensions a role's loader takes, in the bits' order (".tga", ".mdt", ".pcx", ".dds", ".png").
std::vector<std::string> texture_role_extensions(const TextureRole &role);
// Whether a role's file is read by its own name as well as through its loader: a terrain's detail
// map, its splat details and its second detail go through STAGE (a .dds beside the name first) for
// the near texture, and the game reads the name as written through the TGA reader for the
// terrain's checksum and the far blend, and the detail map's (a .tga or a .pcx) through the TGA or
// PCX reader into the detail coefficient [orig: PolyTrn_InitTextures @ 0x60AAF4, @ 0x60AB4C,
// @ 0x60AC76..0x60AD2A, @ 0x60AFC9; Texture_GenerateNormalMap @ 0x58C116..0x58C159, from
// @ 0x60B155].
bool texture_role_read_by_name(TextureRoleId role);

// The role a weapon or item definition's texture field is loaded in, by the field's name: HUD art
// in the HUD loader's alpha mode for a weapon's HUD icon, its clip and round graphics, its two
// crosshairs, its commander reticle, its slot bar icon and an item's HUD image [orig:
// HUD_LoadAllTextures @ 0x59E248..0x59E26A, the ItemDef's +0xA74 in mode 1;
// WeaponDefs_ParseLineCallback @ 0x544966, @ 0x5449A6, @ 0x544A0B, @ 0x544A52]; a menu image for a
// weapon's loadout icon [orig: CTextureManager_LoadOrFindTexture @ 0x654980]; a sight card for a
// sight's texture. False for a field whose loader the game is not witnessed using (an item's
// shadow_texture the game never loads).
bool def_texture_field_role(std::string_view field, TextureRoleId &out);

// The material's flags as they fall on one of its texture rows (`row_type` its authored type,
// `slot` its slot): the alpha test's bits (cut out, inverted) kept on the row whose alpha the
// technique its shader selects tests (object_coverage_source: a diffuse row's for most, the normal
// map's for the unskinned tangent DOT3 effects), cleared on every other row, and on every row of a
// technique that tests no texture's alpha (a mirror's ReflectColor, the vertex alpha, SELFLUM's
// none).
uint8_t texture_row_material_flags(const std::string &shader, uint8_t material_flags, uint8_t row_type, uint8_t slot);

// What a model texture row's alpha is to the game, by the technique its material's shader selects
// (render-material-re.md "What the diffuse's alpha means by the material"): the alpha the
// material's test cuts out by (a diffuse's, or the normal map's for the unskinned tangent DOT3
// effects); the transparency a blended material draws by; the specular brightness the Phong
// effects read (Diffuse1.a; _psPhong2's PhongMap path, the brightness and the weight between its
// channels); what a detail row multiplies the diffuse's by (the _MT stage); the height a .tga
// normal row's normal map is made from; or nothing the technique reads (an opaque or additive
// material, a mirror's ReflectColor, SELFLUM).
enum class TextureAlphaMeaning : uint8_t { Unused, CutOut, Blend, Specular, PhongMapWeight, Detail, Height };
// Whether the game draws the alpha as transparency (a cut-out or a blend): else the texture draws
// opaque, whatever the alpha holds.
inline bool texture_alpha_is_transparency(TextureAlphaMeaning meaning) {
	return meaning == TextureAlphaMeaning::CutOut || meaning == TextureAlphaMeaning::Blend;
}
// A model texture row's (`row_type` its authored type, `slot` its slot, `material_flags` its
// material's as texture_row_material_flags left them on the row, `name` the file it names).
TextureAlphaMeaning texture_row_alpha_meaning(const std::string &shader, uint8_t material_flags, uint8_t row_type,
		uint8_t slot, const std::string &name);

} // namespace opennova::renderer
