#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/importer.h>
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
// - `<stem>.trn`: the settings naming them, the sector grid by the `layout` option, the water.
// No surface map and no foliage map are made: with none the game reads surface 1 everywhere [orig:
// Terrain_GetSurfaceTypeAtPosition @ 0x606519] and grows no foliage [orig:
// Foliage_SampleFoliageMapMask @ 0x60662B].
//
// The set file: a line a key and a file name (relative to the set's folder), `;` to the line's end a
// comment: `heightmap` (required: a PNG of 1024 x 1024 texels, grey at any depth or colour, read as its
// grey; or TrnGen's own `.raw`, 1 MiB of 8-bit heights, or 2 MiB of the game's own 16-bit heights),
// `colormap` (required: a PNG, TGA or PCX of 1024 x 1024), `detail` (a power of two a side), `tiles`
// (sides multiples of 64).

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
};

// The set's text read; false, with `why`, for a line of no key the set knows, or a set without its
// heightmap or colour map.
bool parse_terrain_set(const std::vector<uint8_t> &bytes, TerrainSet &out, std::string &why);
// The set as a file (CRLF lines, a comment heading it).
std::vector<uint8_t> write_terrain_set(const TerrainSet &set);

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
// `tiles`).
std::vector<std::string> terrain_output_names(const std::string &stem, bool tiles);

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

// The stretches of 256 texels along a row (the CDEP section's blocks) whose heights span more than a
// block holds: 32,766 raw (just under 128 world units), its width that of the range plus one within the
// field's 15 bits, as the shipped files write it. The CPT writer clamps each [orig:
// Terrain_LoadLodStorage @ 0x603635..0x6037A8, the 4-bit width].
int terrain_steep_blocks(const std::vector<uint16_t> &raw16);

bool run_terrain_import(ImportContext &context, ImportProduct &out);

} // namespace opennova::editor
