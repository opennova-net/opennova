#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>

namespace opennova::renderer {
enum class TextureLoader : uint8_t;
}

namespace opennova::editor {

// The texture roles (ADR 0046 S18): every way the game uses a texture file, each a row of what loads it,
// which formats work, what size it must be, what its alpha means, how it is sampled and what the game
// does when the file is wrong or missing, each with its witness. The table is the RE record's
// (docs/render/render-material-re.md, "Texture roles") as data: the checks, the import defaults, the
// panel's words and the wire read it. Tooling, not a port: the witnesses are the loaders'.

// The loaders a role's file goes through (the name rule that picks the file and its reader).
enum class TextureLoader : uint8_t {
	Stage, // Texture_LoadByNameWithChannel: the .dds sibling first, else .tga/.mdt -> TGA, .pcx -> PCX
	Plain, // Texture_LoadAndRegister: the whole name, no DDS probe; an upper-case .PCX white, alpha = blue
	Normal, // Texture_LoadAsNormalMap: .mdt as is, .tga (or its .dds) converted from height
	Producer, // sub_58A430: a .tga (or its .dds) height into a horizon volume or occlusion map
	Chunk, // the NQ8B/HRZ8/AOC8 chunk loaders: a container under any name
	Archive, // Texture_LoadFromArchive: the .dds sibling first, else TGA/MDT, else PCX with luminance alpha
	Hud, // HUD_LoadImageAsTexture: .FULL/.ALPHA suffix, TGA, PCX white with alpha = blue, A8 in alpha mode
	File, // Texture_LoadFromFile_0: .tga -> TGA, anything else -> PCX
	Menu, // CTextureManager_LoadOrFindTexture: by the last extension, .tga else its .dds, .dds, .pcx, .png
	Ptl, // the particle atlas: TGA alone, a flipbook's frames _01.tga ..
	Tga, // the TGA reader opened directly
	Pcx, // the PCX reader opened directly
	Pcx8, // Texture_LoadPCXFromPFF8Bit: an 8-bit PCX's indices and palette
	Cube, // the cube map loader (DDS)
	kCount,
};
const char *texture_loader_token(TextureLoader loader);

// The file formats a role's loader takes, a bit each.
enum TextureFormatBits : uint8_t {
	kTextureTga = 1 << 0,
	kTextureMdt = 1 << 1,
	kTexturePcx = 1 << 2,
	kTextureDds = 1 << 3,
	kTexturePng = 1 << 4,
};

// What a role asks of a texture's sides.
enum class TextureSizeRule : uint8_t {
	Any, // any size the device takes (halved past its largest)
	PowerOfTwo, // both sides powers of two (a wrap or a lookup reads past otherwise)
	Exact, // exactly `width` x `height`
	SquarePowerOfTwoAtMost, // square, a power of two, at most `width`
	MultipleOf, // each side a multiple of `width`
	AtMost, // halved to fit `width` a side (no finding: a fact)
	Even, // each side even (a quadrant split)
	Unknown, // NEEDS-RE
};

enum class TextureRoleGroup : uint8_t { Model, Terrain, Environment, Effects, Hud, Menus, kCount };
const char *texture_role_group_words(TextureRoleGroup group);

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
	ShadowDecal,
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

// One role: its token on the wire and its words ("model diffuse"), its group, the loader and the
// formats it takes, its size rule (`width`, `height` its numbers), what the alpha means there, how it is
// sampled, what the game does with a wrong or missing file, and the witness the findings and the panel
// quote.
struct TextureRoleRow {
	TextureRoleId id = TextureRoleId::kCount;
	const char *token = "";
	const char *words = "";
	TextureRoleGroup group = TextureRoleGroup::Model;
	TextureLoader loader = TextureLoader::Stage;
	uint8_t formats = 0;
	TextureSizeRule size = TextureSizeRule::Any;
	uint32_t width = 0, height = 0;
	const char *alpha = "";
	const char *sampling = "";
	const char *missing = "";
	const char *witness = "";
	// Whether its loader reads the file's alpha at all (a PCX loaded opaque fails a role that reads it).
	bool reads_alpha = true;
};

const TextureRoleRow &texture_role_row(TextureRoleId id);
// The role a token names; false for none.
bool texture_role_from_token(const std::string &token, TextureRoleId &out);
// Whether the role's loader takes a file of `extension` (".tga", ".dds", ...; any case).
bool texture_role_takes(const TextureRoleRow &row, const std::string &extension);
// A size rule in words ("1024 x 1024", "powers of two", "64-pixel cells").
std::string texture_size_words(const TextureRoleRow &row);
// The extensions a role's loader takes, in the bits' order (".tga", ".mdt", ".pcx", ".dds", ".png").
std::vector<std::string> texture_role_extensions(const TextureRoleRow &row);
const char *texture_size_rule_token(TextureSizeRule rule);
// A role on the wire (the texture_roles query): its token, words, group, loader, formats, size
// {rule, words, width, height}, alpha, sampling, missing, whether its loader reads the alpha, witness.
io::JsonValue texture_role_json(const TextureRoleRow &row);

// What a texture reference gives its loader besides the name (GraphEdge::loader_arg,
// FieldUse::loader_arg, reference_file_candidates): a model texture row its row's type (0 to 255,
// renderer::material_texture_source picks by it); a use of another referrer its role, from
// kTextureRoleArg up, with what the referrer's own content says of it in the bits above: the game
// refuses the mission without the file (kTextureArgGates: a terrain's colour map, its blend map once
// splat details are authored), or the name is a mission's tile set, whose extension the game makes TGA
// (kTextureArgTileSet). -1: a use whose loader is not witnessed yet, the name as written.
inline constexpr int32_t kTextureRoleArg = 0x100;
inline constexpr int32_t kTextureArgGates = 0x10000;
inline constexpr int32_t kTextureArgTileSet = 0x20000;
int32_t texture_role_arg(TextureRoleId role, int32_t flags = 0);
// Whether the argument is a model texture row's type.
inline bool texture_arg_is_row_type(int32_t loader_arg) { return loader_arg >= 0 && loader_arg < kTextureRoleArg; }
// The role an argument names; false for a row's type or none.
bool texture_arg_role(int32_t loader_arg, TextureRoleId &role);
inline bool texture_arg_gates(int32_t loader_arg) { return loader_arg > 0 && (loader_arg & kTextureArgGates) != 0; }

// The game's loader a role's file goes through (runtime/renderer/texture_load_rules.h, which names the
// files it opens and the reader): false for a role no such loader reads, a model row's normal map,
// producer or chunk (its row's type picks: renderer::material_texture_source), a map read by its own
// name (a foliage or char map), a cube map.
bool texture_role_renderer_loader(TextureRoleId role, renderer::TextureLoader &out);

// What a model texture row says of its use beyond its type (FieldUse::use_context, GraphEdge::use_context):
// its slot, its flags (the flipbook bit), its material's flags (alpha test, inverted) and alpha-test
// reference, packed a byte each; a slot of 0 (none is) for no row.
struct TextureRowContext {
	uint8_t slot = 0, row_flags = 0, material_flags = 0, alpha_ref = 0;
};
inline uint32_t pack_texture_row_context(const TextureRowContext &row) {
	return uint32_t(row.slot) | uint32_t(row.row_flags) << 8 | uint32_t(row.material_flags) << 16 |
	       uint32_t(row.alpha_ref) << 24;
}
inline TextureRowContext unpack_texture_row_context(uint32_t packed) {
	return {uint8_t(packed), uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)};
}

} // namespace opennova::editor
