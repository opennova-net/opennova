#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <formats/foliage/foliage.h>
#include <formats/pcx/pcx.h>

namespace opennova::editor {

// The terrain importer (ADR 0046 S20): a terrain made from ordinary images. Its source is a terrain set
// (`<stem>.tset`, a short text the new_terrain request writes in art/terrain/, or a modder by hand)
// naming the images beside it, each an input of the import (import_context.h), so a change to any of
// them imports the terrain again; its record's options are the terrain's numbers in world units. The
// outputs are the files the game reads for a terrain, each named after the set's stem:
// - `<stem>.cpt`: the heights and the ground mesh, made by TrnGen.exe's own bake (editor/terrain,
//   terrain_bake.h), its depth section CDEP (the game loads no other [orig: Terrain_LoadLodStorage @
//   0x6037B2]);
// - `<stem>_c.tga`: the colour map, 1024 x 1024, 24-bit (the game's colour map is exactly that size
//   and read by its TGA reader alone [orig: PolyTrn_InitTextures @ 0x60B3A7]);
// - `<stem>_dt.tga`: the ground's detail, the three splat layers (_c1.._c3) and `<stem>_dm.tga`, the
//   detail coefficient map (polytrn_detailmap, whose name the game requires [orig:
//   Terrain_LoadEnvironmentConfig @ 0x610A34]): the set's detail image, or 512 x 512 mid grey, which
//   the detail's modulate x2 leaves the colour map's colours as they are;
// - `<stem>_d1.tga`: the blend map, 1024 x 1024 all red, the first splat layer everywhere (a blend
//   map's key turns the blend on, and the file must then load [orig: PolyTrn_InitTextures @
//   0x60B18E..0x60B1A6]);
// - `<stem>_t.tga`: the tile set's atlas, 32-bit, its sides multiples of 64 (the game cuts it in
//   64-texel cells [orig: Terrain_LoadTileSetAtlas @ 0x604B7C]), when the set names one;
// - `<stem>.til`: the terrain's own tile placement, empty (a mission's `<mission>.til` comes first
//   [orig: Terrain_Init @ 0x60FCFD]; with neither the game places no tile);
// - `<stem>_m.pcx`: the surface map (the .trn's polytrn_charmap, retail's `_m` name), when the set names
//   one: 8-bit, each index the surface class the game reads there (footsteps, impact effects, the physics
//   of rounds and throwables), the char map legend (formats/trn/charmap_legend.h) its palette; with none
//   the game reads surface 1 everywhere [orig: sub_605A10 @ 0x605A31; Terrain_GetSurfaceTypeAtPosition @
//   0x606519];
// - `<stem>_f.pcx`: the foliage map (the .trn's polytrn_foliagemap, retail's `_f` name), when the set names
//   one: 8-bit, each index a foliage code, its palette the source's (the game keeps the indices alone and
//   turns each into the definitions whose `match` codes hold it [orig: Foliage_LoadFoliageMapPCX @
//   0x605AD0, the remap @ 0x605B73..0x605B8A; Foliage_RemapPixelToDefMask @ 0x5FF4E0]); with none the game
//   grows no foliage [orig: Foliage_SampleFoliageMapMask @ 0x60662B];
// - `<stem>.trn`: the settings naming them, the sector grid by the `layout` option, the water, and the
//   set's foliage definitions as its `foliage` blocks.
// TrnGen.exe makes none of the maps: it bakes the heights alone (its project names a depth map, the
// output and the locks), the char map, the foliage map and the `.trn` being NovaLogic's own, written beside
// it (terrain-re.md, "A terrain from images").
//
// The set file: a line a key and a file name (relative to the set's folder), `;` to the line's end a
// comment: `heightmap` (required: a PNG of 1024 x 1024 texels, grey at any depth or colour, read as its
// grey; or TrnGen's own `.raw`, 1 MiB of 8-bit heights, or 2 MiB of the game's own 16-bit heights),
// `colormap` (required: a PNG, TGA or PCX of 1024 x 1024), `detail` (a power of two a side), `tiles`
// (sides multiples of 64), `surface` (a PNG, TGA or PCX, square, 256, 512 or 1024 a side, laid over the
// heightmap as the colour map is: an 8-bit PCX's or a palette PNG's indices are the classes, 0 to 19; any
// other's colours each a class's legend colour exactly, its alpha ignored), `foliagemap` (a PNG, TGA or
// PCX, square, a power of two at most 1024 a side (the game keeps its width alone, as the rows' length and
// the power of two it samples the 1024-unit atlas by), laid over the heightmap as the colour map is: an
// indexed image's indices are the codes, a grey image's levels; code 0 grows nothing). And up to four
// `foliage` blocks, each the .trn's own (`foliage`, its keys, `end`): `graphic` (the model it places),
// `match` (one to four codes, 1 to 255: it grows where the foliage map holds one of them), `color_lower`
// and `color_upper` (0 to 2: the shipped files' comment, 0 match the ground, 1 a 50% blend, 2 its own
// colour; the game's generator writes over the colour they pick before it draws, foliage-re.md) and
// `attrib` (`forceon`: it grows on placed tiles too; `shadow`, which the game reads and never uses) [orig:
// Terrain_ParseConfigCallback @ 0x60F330, its four slots; Foliage_RemapPixelToDefMask @ 0x5FF4E0, the four
// codes it compares; Foliage_GenerateInstances_0 @ 0x6002DB..0x60030A, the colour overwritten].

inline constexpr int kTerrainImporterVersion = 1;
inline constexpr const char *kTerrainSetExtension = ".tset";
// The longest stem a terrain set may have: its longest output, `<stem>_dt.tga`, fits an archive's 16
// characters.
inline constexpr size_t kTerrainStemMax = 9;

struct TerrainSet {
	std::string heightmap;
	std::string colormap;
	std::string detail;
	std::string tiles;
	std::string surface;
	std::string foliagemap;
	std::vector<FoliageDef> foliage; // the .trn's foliage blocks, in their order (their slots)
};

// The set's text read; false, with `why`, for a line of no key the set knows, a set without its
// heightmap or colour map, or a foliage block the game would not read as written (a key no block takes, no
// `end`, a fifth block, no graphic, no codes or one past 1..255, a colour mode past 0..2, an attrib other
// than forceon or shadow).
bool parse_terrain_set(const std::vector<uint8_t> &bytes, TerrainSet &out, std::string &why);
// The set as a file (CRLF lines, a comment heading it).
std::vector<uint8_t> write_terrain_set(const TerrainSet &set);

// Foliage definitions on one line, as the new_terrain request takes them: each the foliage block's keys and
// their values in a row (`graphic onfern1.3di match 253 color_upper 2`), several split by `|`. False, with
// `why`, as parse_terrain_set refuses a block.
bool parse_foliage_definitions(const std::string &text, std::vector<FoliageDef> &out, std::string &why);

const std::vector<ImportOptionRow> &terrain_import_option_rows();

// The options as an import reads them, each left out its fallback.
struct TerrainImportSettings {
	double top = 127.5;  // world units of the heightmap's white
	double water = 0.0;  // the sea's height in world units, 0 none
	std::string layout = "island";
};
// False, with `why` and the option's key in `field`, for a value no row takes.
bool terrain_import_settings(const ImportOptions &options, TerrainImportSettings &out, std::string &why,
                             std::string &field);

// Whether `stem` names a terrain: letters, digits and underscores, at most kTerrainStemMax of them.
bool terrain_stem_fits(const std::string &stem, std::string &why);

// The outputs' names for a stem, in the order the import makes them (the tile set's atlas only when
// `tiles`, the surface map only when `surface`, the foliage map only when `foliage`).
std::vector<std::string> terrain_output_names(const std::string &stem, bool tiles, bool surface, bool foliage);

// A heightmap as the bake takes it, from the file `name` holds: an 8-bit map's texels (TrnGen's
// input) or 16-bit heights (raw16, 1/256 world unit a step), scaled by `top`. False, with `why`, for a
// file of another size or one that does not read.
struct TerrainHeights {
	std::vector<uint8_t> depth8;
	std::vector<uint16_t> depth16;
};
bool decode_terrain_heightmap(const std::string &name, const std::vector<uint8_t> &bytes, double top,
                              TerrainHeights &out, std::string &why);

// One of the set's images as the import reads it (`key`: colormap, detail or tiles), checked as the
// game takes it: a colour map exactly 1024 x 1024, a detail's sides powers of two, a tile set's
// multiples of 64. False, with `why` naming the file, for one that does not read or does not fit.
bool decode_terrain_image(const std::string &key, const std::string &name, const std::vector<uint8_t> &bytes,
                          RgbaImage &out, std::string &why);

// The set's surface map as the import writes it: its classes as indices, the legend its palette. An
// indexed image (an 8-bit PCX, a palette PNG) is read by its indices, a colour image by its colours, each
// looked up in the legend exactly (no nearest colour). False, with `why` naming the file, for one that does
// not read, is not square with a side of 256, 512 or 1024, or holds a texel of no class (an index past 19,
// a colour the legend lacks: the first named by its place, column and row from the top left).
bool decode_terrain_surface(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                            std::string &why);

// The set's foliage map as the import writes it: its codes as indices, an indexed image's palette kept (the
// game reads none of it: a viewer shows the map as it was painted), a grey image's a grey ramp. An indexed
// image (an 8-bit PCX, a palette PNG) is read by its indices, an image whose every texel is grey by its
// levels. False, with `why` naming the file, for one that does not read, holds colour, or is not square with
// a side a power of two at most 1024 [orig: Foliage_LoadFoliageMapPCX @ 0x605B44..0x605B60, the width's
// log2; Foliage_SampleFoliageMapMask @ 0x60669C..0x60662D, the sample shifted by 10 less it].
bool decode_terrain_foliage(const std::string &name, const std::vector<uint8_t> &bytes, IndexedImage8 &out,
                            std::string &why);

// What a foliage map grows by the definitions (`map` the codes, `defs` the slots): the codes it holds that
// no definition selects (code 0 aside), and the definitions none of whose codes it holds, each a line for
// the import's warnings ("" none).
std::vector<std::string> terrain_foliage_notes(const IndexedImage8 &map, const std::vector<FoliageDef> &defs);

bool run_terrain_import(ImportContext &context, ImportProduct &out);

} // namespace opennova::editor
