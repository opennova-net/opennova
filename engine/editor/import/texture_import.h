#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <formats/pcx/pcx.h>

namespace opennova::editor {

// The image importer (ADR 0046 d10, S8; S18): a PNG made into the texture file its uses read, by the
// options its import record holds, each a row of image_import_option_rows (the import_options query
// answers them, set_import_options sets them):
// - `format`, what it writes: `tga` (32-bit, its alpha kept: the form every TGA loader reads), `tga24`
//   (24-bit, no alpha: a terrain colour map's form), `pcx` (8-bit indexed, 256 colours, no alpha: the
//   loading screens', sky clouds' and foliage maps' reader), `dds` (the D3DX codec's DXT5 or DXT1 with
//   its mip chain, or A8R8G8B8: the form of the game's model textures, which their loader reads before
//   a `.tga` of the name), `mdt` (a 32-bit TGA under `.mdt`: a model's finished normal map) or `png`
//   (the menus' loader alone reads one);
// - `name`, the output's file name (left out: the source's stem and the format's extension);
// - `alpha`: `source`, `opaque`, `luminance` (the archive loader's (85 x (r + g + b)) >> 8 of each
//   texel), `threshold:<n>` (255 above n, 0 at or below: a cut-out's own test) or `key:#RRGGBB` (that
//   colour clear, every other opaque);
// - `size`: `source`, `pow2_down`, `pow2_up`, `<W>x<H>` or `fit:<W>x<H>` (inside it, its shape kept);
// - `palette` (pcx): `median_cut` (the colours as they are when 256 or fewer, else a median cut) or
//   `exact` (refused past 256 colours);
// - `dds` (dds): `dxt5`, `dxt1` or `argb`; `mips` (a DXT dds): `full` (every level to 1 x 1, each the
//   D3DX box filter of the level before, decoded from its own blocks, as the game's texture creator
//   builds its levels) or `none`;
// - `green`: `game` or `flip` (a normal map drawn with the other green: the game's tangent frame runs
//   down the texture, ADR 0047).
// A resize is editor tooling, not a port: halving by 2 x 2 boxes, any other size by the average of the
// texels each output texel covers.

inline constexpr int kImageImporterVersion = 2;

const std::vector<ImportOptionRow> &image_import_option_rows();

// The options as an import reads them, every one left out its fallback.
struct ImageImportSettings {
	std::string format, name, alpha, size, palette, dds, mips, green;
};
ImageImportSettings image_import_settings(const ImportOptions &options);
// The extension a format writes, with its dot (".tga" for tga24, ".mdt" for mdt).
std::string image_format_extension(const std::string &format);
// The file an import of `source_name` writes: the name option, else the source's stem and the format's
// extension.
std::string image_import_output_name(const std::string &source_name, const ImageImportSettings &settings);

// The pieces, each on its own (a test reads them): the sides `size` asks of an image of `width` x
// `height` (false, with `why`, for a value no row takes); an image resized; `alpha` applied (false, with
// `why`); green flipped.
bool image_target_size(const std::string &size, uint32_t width, uint32_t height, uint32_t &out_width,
                       uint32_t &out_height, std::string &why);
RgbaImage resize_image(const RgbaImage &image, uint32_t width, uint32_t height);
bool apply_image_alpha(RgbaImage &image, const std::string &alpha, std::string &why);
void flip_image_green(RgbaImage &image);
// The image written as `settings` say (format, palette, dds, mips): its bytes, or false with `why`; a
// warning (a PCX dropping a translucent source's alpha) in `note`.
bool encode_image(const RgbaImage &image, const ImageImportSettings &settings, std::vector<uint8_t> &out,
                  std::string &why, std::string &note);

// The importer's run (importers.cpp's row): the source decoded, then resized, its green and alpha, then
// written.
bool run_image_import(ImportContext &context, ImportProduct &out);

} // namespace opennova::editor
