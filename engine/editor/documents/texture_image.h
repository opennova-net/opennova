#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::editor {

// A texture as the game reads it (ADR 0046 S18): the file's texels decoded by the reader the game
// picks for its name (never by its bytes: renderer/material_texture.h), with what it is in a modder's
// words. The decoders are the formats' (formats/tga tga_decode_game, formats/dds dds_read, formats/pcx
// decode_pcx_game, the PNG reader of the menus' loader); this is what the texture document, its
// viewport and the wire read of them.

// The reader a texture file's name picks: a .tga or an .mdt the TGA reader (a model's normal map made
// ahead is a TGA the object loader decodes so), a .pcx the PCX reader, a .dds D3DX's (which reads the
// bytes by their content, a DDS's first), a .png the menus' PNG reader.
enum class TextureReader : uint8_t { None, Tga, Dds, Pcx, Png };
// "tga", "dds", "pcx", "png", "none": its token on the wire.
const char *texture_reader_token(TextureReader reader);
TextureReader texture_reader_for(const std::string &name);

// What a texture's alpha holds over its first level's texels: none (every texel opaque), on or off
// (every texel 0 or 255: a cut-out), or graded (values between).
enum class TextureAlpha : uint8_t { None, Mask, Graded };
const char *texture_alpha_token(TextureAlpha alpha);

// One level of the texture: its sides and R, G, B, A a texel from the top row down.
struct TextureLevel {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;
};

// One fact of the texture in a modder's words: its label ("Size", "Compression") and its words, with
// a stable key the wire names it by.
struct TextureFact {
	std::string key;
	std::string label;
	std::string words;
};

// What the game reads of a texture file.
struct TextureImage {
	TextureReader reader = TextureReader::None;
	// The reader takes the file: its texels are what `levels` holds (a TGA form the reader zeroes
	// loads blank: `blank`). False where the game's load fails (`refusal` says why: a DDS D3DX does not
	// read, a PCX not of 8 bits a plane, a file no reader takes).
	bool loads = false;
	std::string refusal;
	// The file's texels are known (decoded): false for a form the port does not decode yet (a DDS's
	// bump or float formats), `undecoded` saying which.
	bool decoded = false;
	std::string undecoded;
	// The reader makes no texel of the file's own (a TGA form it zeroes or leaves unset): every texel
	// blank.
	bool blank = false;
	// The levels the file holds (a DDS's mip chain; one for a TGA, a PCX or a PNG), the first the
	// whole texture.
	std::vector<TextureLevel> levels;
	// An indexed texture's palette (R, G, B an entry) and each first-level texel's index; empty for
	// a texture of true colour.
	std::vector<uint8_t> palette;
	std::vector<uint8_t> indices;
	TextureAlpha alpha = TextureAlpha::None;
	// The levels the game's device texture has: a DDS's file chain, else the chain the game builds
	// from the texels (renderer::pixel_texture_mip_levels; 0 its full chain).
	uint32_t game_levels = 0;
	// The file stores its rows top first where the game reads every TGA bottom up: the game shows it
	// upside down (the picture shows it so).
	bool upside_down = false;
	// The file was BFC1-compressed (the models' TGA reader unpacks it).
	bool bfc1 = false;
	// The facts in words, in the order the info panel lists them.
	std::vector<TextureFact> facts;

	uint32_t width() const { return levels.empty() ? 0 : levels.front().width; }
	uint32_t height() const { return levels.empty() ? 0 : levels.front().height; }
	size_t palette_size() const { return palette.size() / 3; }
	const TextureFact *fact(const std::string &key) const;
};

// The file `name` holds `bytes` (as stored): what the game reads of it.
std::shared_ptr<const TextureImage> decode_texture(const std::string &name, const std::vector<uint8_t> &bytes);

// The texel at (x, y) of `level`, R, G, B, A; false off the level.
bool texture_texel(const TextureImage &image, size_t level, uint32_t x, uint32_t y, uint8_t rgba[4]);

} // namespace opennova::editor
