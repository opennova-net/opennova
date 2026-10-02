#pragma once

// The game's texture loaders as rules: which file a loader opens for a name, which
// reader decodes it, and what the loader does to the pixels before it makes the
// texture (texture_load_rules.cpp holds the witnesses). A model texture row's own
// rule is material_texture.h (material_texture_source, made a load here by
// material_texture_load); the embedder reads each
// attempt's file, decodes it through the named reader (the TGA reader
// formats/tga/tga_read.h; the PCX reader formats/pcx decode_pcx_menu_rgba, which is
// Texture_LoadPCXFromPFF32's decode as well as the menu's; a DDS through D3DX's
// content sniff; a PNG, menus only) and applies the transform. No loader reads an
// alternate name: no `_O` twin, no png/jpg/bmp beside a .tga. Witness record:
// docs/render/render-material-re.md ("The game's texture loaders").

#include <runtime/renderer/material_texture.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::renderer {

// The reader a loader hands the file to.
enum class TextureReader : uint8_t {
	None,
	// D3DXCreateTextureFromFileInMemoryEx: the bytes decoded by their content, the
	// codecs tried in D3DX's order (dds_reader_codec_order).
	Dds,
	// The TGA reader (CTerrainTileData_LoadTGAFromArchive).
	Tga,
	// The particle manager's loose TGA leg (CTextureData_LoadTGA): the TGA reader with
	// unsigned sides and the height-stride flip (formats/tga TgaReaderForm::ParticleLoose).
	TgaParticleLoose,
	// The PCX reader (Texture_LoadPCXFromPFF32).
	Pcx,
	// The menus' PNG reader.
	Png,
};

// Where an attempt's file is read from.
enum class TextureFileSource : uint8_t {
	// The mounted file set, under the caller's lookup policy.
	Mounted,
	// The particle manager's loose texture folder, "<game directory>\tga\", opened
	// directly whatever the session's loose-first setting (CEffectSystem_Init sets it).
	ParticleTextureDir,
};

// What a loader does to the decoded pixels.
enum class TextureLoadTransform : uint8_t {
	None,
	// Every pixel white with its blue as alpha: the loader shifts each A8R8G8B8 word
	// left 24 and ORs 0xFFFFFF, so blue becomes alpha.
	WhiteAlphaFromBlue,
	// A PCX's alpha from its palette entry's luminance, (85 * (r + g + b)) >> 8
	// (formats/pcx decode_pcx_luminance_alpha, the 8-bit reader's indices).
	PaletteLuminanceAlpha,
};

struct TextureLoad {
	std::string file; // the name the loader opens (empty with reader None)
	TextureReader reader = TextureReader::None;
	TextureFileSource source = TextureFileSource::Mounted;
	TextureLoadTransform transform = TextureLoadTransform::None;
	// Only the alpha survives: the loader makes an A8 texture, drawn with the HUD's
	// alpha material (hud_alpha_material_argb).
	bool alpha_only = false;
};

// The game's texture loaders, by the role that calls them (the role table:
// docs/render/render-material-re.md "The game's texture loaders").
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
	// tile-set maps, the network icons, the board box, the start-mission cursor.
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
	// CTextureData_LoadTGA: particle graphics, the loose "tga\" folder first, then the
	// mounted name.
	Particle,
};

// What the embedder answers about the mounted file set.
struct TextureFileQuery {
	// Whether `name` is in the mounted set (the loader's FileSystem_FileExists).
	std::function<bool(const std::string &)> exists;
	// Whether a loose-first search finds `name` as a loose file (the session's `/d`
	// search, or the caller's forced lookup policy).
	std::function<bool(const std::string &)> loose_first_hit;
};

// The files `loader` tries for `name`, in order; the first that reads and decodes
// is the texture, none is a failed load. CineFade and Particle try two, every other
// loader one.
std::vector<TextureLoad> texture_load_attempts(TextureLoader loader, std::string_view name,
		const TextureFileQuery &files);

// Texture_LoadByNameWithChannel: the name cut three characters after its first '.',
// the .dds sibling first unless the cut name holds ".MDT" or a loose file wins under
// loose-first, else .TGA/.MDT through the TGA reader and .PCX through the PCX reader
// (material_texture.h material_texture_source, runtime type 0, carries the witnesses).
TextureLoad stage_texture_load(std::string_view query, bool loose_first_hit, bool dds_exists);

// A model texture row's image load: the one file and reader the row's loader picks
// (`source`, material_texture_source over the row's runtime `type`), with a type-1
// row's Texture_LoadAndRegister mask (plain_texture_load: an upper-case ".PCX" turns
// white with its blue as alpha). Reader None when that loader decodes no image: it
// opens nothing, or it reads a chunk container (types 16 to 18), which the chunk
// producers decode (material_texture.h load_material_chunk).
TextureLoad material_texture_load(const MaterialTextureSource &source, uint8_t type);

// Texture_LoadAndRegister (a model row of runtime type 1): the whole name, no DDS
// probe, .TGA/.MDT through the TGA reader and .PCX through the PCX reader; a name
// that holds ".PCX" in upper case as written turns white with its blue as alpha
// whichever reader read it ("X.PCX.TGA" too), "x.pcx" in lower case stays colour.
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
// `alpha_mode`; ".TGA" through the TGA reader, else ".PCX" through the PCX reader,
// anything else nothing; a name holding ".PCX" turns white with its blue as alpha
// whichever reader read it.
TextureLoad hud_texture_load(std::string_view name, bool alpha_mode);

// The transform on RGBA8 pixels (`pixels` of them). WhiteAlphaFromBlue as the loaders
// do it; `alpha_only` keeps the alpha and makes the colour white, the form an A8
// texture takes here: the colour the alpha material draws comes from the vertex
// colour alone (hud_alpha_material_argb), which a white texture leaves untouched.
// PaletteLuminanceAlpha is a decode, not a pixel transform: nothing happens here.
void apply_texture_load_transform(TextureLoadTransform transform, bool alpha_only, uint8_t *rgba,
		size_t pixels);

// The stage-0 colour op a material word selects, by its colour family (the word's
// bits 8..13). A texture draws with its own material's stage whatever loader read
// its pixels, so the word, not the loader, decides how a draw combines texel and
// vertex colour.
enum class MaterialColorStage : uint8_t {
	// Family 0x600: MODULATE2X(TEXTURE, DIFFUSE), saturated (hud_color_material_argb).
	Modulate2x,
	// Family 0xA00: ADD(DIFFUSE, DIFFUSE), the texture's colour never an argument
	// (hud_alpha_material_argb).
	AddDiffuse,
	// Every other family, and no material (word 0). The embedder draws these as texel
	// times vertex colour; the HUD's one other family, 0x400 SELECTARG1(TEXTURE) (the
	// network icons, the binocular digits), draws the same under the white diffuse its
	// callers pass.
	Other,
};
MaterialColorStage material_color_stage(uint32_t word);

// The material words the HUD loader makes its textures with: the colour mode's and
// the alpha mode's (hud_loader_material_word picks one).
inline constexpr uint32_t kHudColorMaterialWord = 0x651u;
inline constexpr uint32_t kHudAlphaMaterialWord = 0xA51u;
uint32_t hud_loader_material_word(bool alpha_mode);

// The vertex colour the HUD's alpha material 0xA51 draws an A8 texture with: colour
// op ADD(DIFFUSE, DIFFUSE), saturated, so twice the vertex colour (the texture's colour
// is never an argument); alpha MODULATE(TEXTURE, DIFFUSE), the vertex alpha the
// modulating draw multiplies by the texture's. `argb` is the quad's vertex colour.
uint32_t hud_alpha_material_argb(uint32_t argb);

// The colour a colour-family-0x600 material (the HUD loader's colour mode 0x651, the
// map icon strip, the waypoint indicator, the capture-point icons, the box styles,
// the tip icons) draws a texel with: colour op MODULATE2X(TEXTURE, DIFFUSE) on a
// device that reports modulate-2x, so twice the texel times the vertex colour,
// saturated per channel; alpha MODULATE(TEXTURE, DIFFUSE). The embedder runs the
// stage on the device, the texel unknown until then; `texel` and `diffuse` are
// A8R8G8B8.
uint32_t hud_color_material_argb(uint32_t texel, uint32_t diffuse);

// GTexture_DownsampleToLimits's cap halving: while either side exceeds `cap`, both
// sides halve with a 2x2 box. `rgba` holds width x height RGBA8 pixels; width and
// height are updated.
inline constexpr uint32_t kNormalMapSideCap = 512;
void halve_rgba_to_cap(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, uint32_t cap);

// The side cap a model row's texture loads with: 512 (flag 0x1000) for the normal maps
// (runtime types 4 and 5) and the occlusion producer (7); 0 (none) for every other
// type, the horizon volume (6) included, whose volume texture is never downsampled.
uint32_t material_texture_side_cap(uint8_t runtime_type);

// The codecs D3DXCreateTextureFromFileInMemoryEx tries on a "DDS" file's bytes, in
// order, the first that decodes wins. The embedder decodes DDS, JPEG, PNG and TGA
// (D3DX's TGA honours the origin bit) and the BMP and DIB pixels; PPM, PFM, HDR and
// the BMP core's header rules are renderer/d3dx_image_codecs.h's ports.
enum class DdsCodec : uint8_t { Bmp, Ppm, Dds, Jpeg, Png, Pfm, Hdr, Tga, Dib };
const std::vector<DdsCodec> &dds_reader_codec_order();

} // namespace opennova::renderer
