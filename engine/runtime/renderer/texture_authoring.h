#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <formats/pcx/pcx.h>

namespace opennova::renderer {

// The image import's pieces (ADR 0046 d10, S8; S18; ADR 0047, `opennova-3di texture`): a picture (a PNG,
// a TGA or a PCX) made into the texture file its uses read, by the options an import names. The options:
// - `format`, what it writes: `tga` (32-bit, its alpha kept: the form every TGA loader reads), `tga24`
//   (24-bit, no alpha: a terrain colour map's form), `pcx` (8-bit indexed, 256 colours, no alpha: the
//   loading screens', sky clouds' and foliage maps' reader), `pcx24` (three planes of colour, no alpha),
//   `dds` (DXT5 or DXT1 with its mip chain, or A8R8G8B8: the form of the game's model textures, which
//   their loader reads before a `.tga` of the name), `mdt` (a 32-bit TGA under `.mdt`: a model's finished
//   normal map) or `png` (the menus' loader alone reads one);
// - `name`, the output's file name (left out: the source's stem and the format's extension);
// - `alpha`: `source`, `opaque`, `luminance` (the archive loader's (85 x (r + g + b)) >> 8 of each
//   texel), `threshold:<n>` (255 above n, 0 at or below: a cut-out's own test) or `key:#RRGGBB` (that
//   colour clear, every other opaque);
// - `size`: `source`, `pow2_down`, `pow2_up`, `<W>x<H>` or `fit:<W>x<H>` (inside it, its shape kept);
// - `palette` (pcx): `median_cut` (the colours as they are when 256 or fewer, else a median cut),
//   `exact` (refused past 256 colours) or `indices` (an 8-bit PCX source's texels written as the indices
//   and palette it holds, never quantized: a foliage or char map's indices are data the game reads; at
//   the source's size);
// - `normal` (tga): `normal` (the source a finished normal map, its colour written as it is) or `height`
//   (the source a height map: its brightness, (85 x (r + g + b)) >> 8, written into the alpha, from which
//   a model's normal row naming a .tga makes the normal map, and its alpha into the blue, which becomes
//   the map's alpha [orig: Texture_LoadAsNormalMap @0x58C985..0x58CAED]);
// - `dds` (dds): `dxt5`, `dxt1` or `argb`; `mips` (a DXT dds): `full` (every level to 1 x 1, as every DXT
//   DDS the game ships carries its chain, each the D3DX box filter of the level before over the source's
//   texels, its blocks the authoring encoder's: dxt_encode.h) or `none`;
// - `green`: `game` or `flip` (a normal map drawn with the other green: the game's tangent frame runs
//   down the texture, ADR 0047).
// A resize is tooling, not a port: halving by 2 x 2 boxes, any other size by the average of the texels
// each output texel covers.

// The options as an import reads them, every one left out its fallback.
struct ImageImportSettings {
	std::string format, name, alpha, size, palette, dds, mips, green, normal;
};

// A source as the import reads it: its texels (RGBA, the top row first) and, for an 8-bit PCX, its
// indices and palette. A source is the modder's picture, read as an image program reads it, never through
// the game's readers' faults (ADR 0046 S18): a PNG's through the PNG reader; a TGA's by the format
// (formats/tga tga_source.h decode_tga_source: its origin honoured, every depth and colour map); a PCX's by
// the format (formats/pcx decode_pcx_indexed and decode_pcx_rgb: each row's first `width` bytes of its
// bytes a line, an 8-bit file's texel the palette entry of its index, opaque). The import then writes the
// file the game reads. False, with `error`, for a file its reader refuses or a name of another extension.
struct ImageSource {
	RgbaImage image;
	bool indexed = false;
	IndexedImage8 indices;
};
bool decode_image_source(const std::string &name, const std::vector<uint8_t> &bytes, ImageSource &out,
                         std::string &error);
// Whether a source holds an alpha, by its first bytes alone (`head`: its header, a PNG's chunks up to its
// first image data): a PNG of grey or colour with alpha, or with a transparency chunk; a TGA with alpha bits
// or a colour map of 32 bits; never a PCX. What a use's needs weigh without decoding it (a sky's clouds).
bool image_source_has_alpha(const std::string &name, const std::vector<uint8_t> &head);
// `normal height` applied: each texel's brightness into its alpha, its alpha into its blue.
void height_into_alpha(RgbaImage &image);
// Whether `value` is one of the written forms an option's value may take beside its named values: an
// `alpha` of threshold:<0..255> or key:#RRGGBB; a `size` of <W>x<H> or fit:<W>x<H>, each side 1..16384.
bool image_alpha_form(const std::string &value);
bool image_size_form(const std::string &value);
// The settings of an import's options (option -> value, as an import record spells them).
ImageImportSettings image_import_settings(const std::map<std::string, std::string> &options);
// The extension a format writes, with its dot (".tga" for tga24, ".mdt" for mdt).
std::string image_format_extension(const std::string &format);

// The pieces, each on its own (a test reads them): the sides `size` asks of an image of `width` x
// `height` (false, with `why`, for a value no option takes); an image resized; `alpha` applied (false, with
// `why`); green flipped.
bool image_target_size(const std::string &size, uint32_t width, uint32_t height, uint32_t &out_width,
                       uint32_t &out_height, std::string &why);
RgbaImage resize_image(const RgbaImage &image, uint32_t width, uint32_t height);
bool apply_image_alpha(RgbaImage &image, const std::string &alpha, std::string &why);
void flip_image_green(RgbaImage &image);
// The texels an import of `settings` encodes from a decoded source (an 8-bit PCX's indices kept aside):
// resized to the size it asks for, its green flipped, its height into its alpha (`normal height` of a TGA)
// or its alpha made as asked; false, with `why` and the option at fault in `field`, for a value it cannot
// use. What an import writes, and what a texture's compare reads an import's output against.
bool image_import_texels(RgbaImage &image, const ImageImportSettings &settings, std::string &why, std::string &field);
// The image written as `settings` say (format, palette, dds, mips): its bytes, or false with `why`; a
// warning (a PCX dropping a translucent source's alpha) in `note`. Not `palette indices`, whose
// indices these texels do not carry: encode_image_indices writes it.
bool encode_image(const RgbaImage &image, const ImageImportSettings &settings, std::vector<uint8_t> &out,
                  std::string &why, std::string &note);
// Whether `settings` ask for the `palette indices` leg: a pcx written from the source's own indices.
bool image_keeps_source_indices(const ImageImportSettings &settings);
// That leg: an 8-bit PCX source's indices and palette written as an 8-bit PCX as they are, never
// quantized (a foliage or char map's indices are data the game reads), at the source's own size.
// False, with `why` and the option at fault in `field`, for a source of colours ("palette") or a
// `size` that asks for other sides ("size"); with `why` alone for a write that fails.
bool encode_image_indices(const ImageSource &source, const ImageImportSettings &settings, std::vector<uint8_t> &out,
                          std::string &why, std::string &field);

} // namespace opennova::renderer
