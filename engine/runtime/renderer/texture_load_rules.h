#pragma once

// The game's texture loaders as rules: which file a loader opens for a name, which
// reader decodes it, and what the loader does to the pixels before it makes the
// texture (texture_load_rules.cpp holds the witnesses). The model diffuse rows'
// own rule is material_texture.h (material_image_source); the embedder reads each
// attempt's file, decodes it through the named reader (the TGA reader
// formats/tga/tga_read.h; the PCX reader formats/pcx decode_pcx_menu_rgba, which is
// Texture_LoadPCXFromPFF32's decode as well as the menu's; a DDS through its DXT
// decode; a PNG, menus only) and applies the transform. No loader reads an alternate
// name: no `_O` twin, no png/jpg/bmp beside a .tga. Witness record:
// docs/render/render-material-re.md ("Texture loaders").

#include <runtime/renderer/material_texture.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::renderer {

// The reader a loader hands the file to.
enum class TextureReader : uint8_t { None, Dds, Tga, Pcx, Png };

// What a loader does to the decoded pixels.
enum class TextureLoadTransform : uint8_t {
	None,
	// Every pixel white with its blue as alpha: the loader shifts each A8R8G8B8 word
	// left 24 and ORs 0xFFFFFF, so blue becomes alpha.
	WhiteAlphaFromBlue,
	// A PCX's alpha from its palette entry's luminance, (85 * (r + g + b)) >> 8
	// (formats/pcx decode_pcx_luminance_alpha decodes it whole).
	PaletteLuminanceAlpha,
};

struct TextureLoad {
	std::string file; // the name the loader opens (empty with reader None)
	TextureReader reader = TextureReader::None;
	TextureLoadTransform transform = TextureLoadTransform::None;
	// Only the alpha survives: the loader makes an A8 texture (the HUD's alpha mode).
	bool alpha_only = false;
};

// The game's texture loaders, by the role that calls them (the role table:
// docs/render/render-material-re.md "Texture loaders").
enum class TextureLoader : uint8_t {
	// Texture_LoadByNameWithChannel: model diffuse, scorch, rain and snow, the
	// binocular and NVG masks, sight cards, tip icons, terrain detail maps.
	Stage,
	// Texture_LoadAndRegister: a model row of runtime type 1.
	Plain,
	// Texture_LoadFromArchive with an empty alpha name: scars, the water wake, the
	// damage vignette.
	Archive,
	// Texture_LoadFromArchive naming the file twice, so a PCX takes its own palette
	// luminance as alpha: the sky maps, the tracer smoke.
	ArchiveSelfAlpha,
	// Texture_LoadFromFile_0: the map icon strip, the waypoint indicator, the
	// flag-point icons, the binocular digits, the NVG scale.
	File,
	// The TGA reader on the name alone: the terrain colour, blend, far detail and
	// tile-set maps, the network icons, the board box, the start-mission cursor,
	// particle graphics.
	Tga,
	// The PCX reader on the name alone: the loading screen, the map overview.
	Pcx,
	// The HUD loader in colour mode (sub_591750 mode 0).
	HudColor,
	// The HUD loader in alpha mode (sub_591750 mode 1): stance, parachute and armor
	// icons, scope art, every weapon and item def HUD image.
	HudAlpha,
	// CTextureManager_LoadOrFindTexture: menu images, the credits roll's images.
	Menu,
	// CinematicFadeEvent_LoadTexture: the end-of-round cine's full-screen images.
	CineFade,
};

// What the embedder answers about the mounted file set.
struct TextureFileQuery {
	// Whether `name` is in the mounted set (the loader's FileSystem_FileExists).
	std::function<bool(const std::string &)> exists;
	// Whether the `/d` loose-first search finds `name` as a loose file.
	std::function<bool(const std::string &)> loose_first_hit;
};

// The files `loader` tries for `name`, in order; the first that reads and decodes
// is the texture, none is a failed load. Every loader but CineFade tries one.
std::vector<TextureLoad> texture_load_attempts(TextureLoader loader, std::string_view name,
		const TextureFileQuery &files);

// Texture_LoadByNameWithChannel: the name cut three characters after its first '.',
// the .dds sibling first unless the cut name holds ".MDT" or a loose file wins under
// loose-first, else .TGA/.MDT through the TGA reader and .PCX through the PCX reader
// (material_texture.h material_image_source carries the witnesses).
TextureLoad stage_texture_load(std::string_view query, bool loose_first_hit, bool dds_exists);

// Texture_LoadAndRegister (a model row of runtime type 1): the whole name, no DDS
// probe; a name that holds ".PCX" in upper case as written turns white with its blue
// as alpha, "x.pcx" in lower case stays opaque colour.
TextureLoad plain_texture_load(std::string_view name);

// Texture_LoadFromArchive: the .dds sibling of the whole name first unless a loose
// file wins under loose-first, else .TGA/.MDT through the TGA reader, .PCX through the
// PCX reader; a PCX takes its palette's luminance as alpha only when the caller's
// alpha name is the PCX itself and exists (`self_alpha`), else it stays opaque.
TextureLoad archive_texture_load(std::string_view name, bool self_alpha, bool loose_first_hit,
		bool dds_exists);

// Texture_LoadFromFile_0: a name holding ".TGA" through the TGA reader, ANY other
// name through the PCX reader; `white_alpha` (the caller's flag 0x200000) turns a
// PCX white with its blue as alpha.
TextureLoad file_texture_load(std::string_view name, bool white_alpha);

// The HUD loader (sub_591750 -> HUD_LoadImageAsTexture): a ".FULL" suffix cut off
// makes it colour, an ".ALPHA" suffix cut off makes it alpha-only, else the caller's
// `alpha_mode`; ".TGA" through the TGA reader, ".PCX" through the PCX reader turned
// white with its blue as alpha, anything else nothing.
TextureLoad hud_texture_load(std::string_view name, bool alpha_mode);

// The transform on RGBA8 pixels (`pixels` of them). WhiteAlphaFromBlue as the loaders
// do it; `alpha_only` keeps the alpha and makes the colour white, the form an A8
// texture takes under the modulating draw: the retail HUD draws its alpha-only art
// with colour op ADD(TEXTURE, DIFFUSE) over an A8 read (colour 0), so the drawn
// colour is the vertex colour alone, which a white texture modulated by it gives.
// PaletteLuminanceAlpha is a decode, not a pixel transform: nothing happens here.
void apply_texture_load_transform(TextureLoadTransform transform, bool alpha_only, uint8_t *rgba,
		size_t pixels);

// GTexture_DownsampleToLimits's cap halving: while either side exceeds `cap`, both
// sides halve with a 2x2 box. A model normal map (runtime type 4 or 5) loads with
// the 512 cap. `rgba` holds width x height RGBA8 pixels; width and height are updated.
inline constexpr uint32_t kNormalMapSideCap = 512;
void halve_rgba_to_cap(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, uint32_t cap);

} // namespace opennova::renderer
