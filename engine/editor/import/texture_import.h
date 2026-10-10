#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <runtime/renderer/texture_authoring.h>

namespace opennova::editor {

// The image importer (ADR 0046 d10, S8; S18): a PNG made into the texture file its uses read, by the
// options its import record holds, each a row of image_import_option_rows (the import_options query
// answers them, set_import_options sets them). A TGA or a PCX is a source too where a record makes it one
// (Replace, Edit externally: importer.h's record_extensions), read as the game's reader reads it
// (renderer::decode_image_source). The options, each read and applied by the engine's image import
// (runtime/renderer/texture_authoring.h, which `opennova-3di texture` writes through too):
// - `format`, what it writes: `tga` (32-bit, its alpha kept: the form every TGA loader reads), `tga24`
//   (24-bit, no alpha: a terrain colour map's form), `pcx` (8-bit indexed, 256 colours, no alpha: the
//   loading screens', sky clouds' and foliage maps' reader), `dds` (DXT5 or DXT1 with its mip chain, or
//   A8R8G8B8: the form of the game's model textures, which their loader reads before a `.tga` of the
//   name), `mdt` (a 32-bit TGA under `.mdt`: a model's finished normal map) or `png`
//   (the menus' loader alone reads one);
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
//   texels, its blocks the authoring encoder's: runtime/renderer/dxt_encode.h) or `none`;
// - `green`: `game` or `flip` (a normal map drawn with the other green: the game's tangent frame runs
//   down the texture, ADR 0047).
// 3: a DDS's blocks the authoring encoder's, each level filtered from the source's
// (runtime/renderer/dxt_encode.h).
inline constexpr int kImageImporterVersion = 3;

const std::vector<ImportOptionRow> &image_import_option_rows();

// The file an import of `source_name` writes: the name option, else the source's stem and the format's
// extension.
std::string image_import_output_name(const std::string &source_name, const renderer::ImageImportSettings &settings);

// The importer's run (importers.cpp's row): the source decoded, then resized, its green and alpha, then
// written (renderer::decode_image_source, image_import_texels, encode_image).
bool run_image_import(ImportContext &context, ImportProduct &out);

} // namespace opennova::editor
