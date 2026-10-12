#pragma once

#include <editor/documents/terrain_planes.h>

namespace opennova::editor {

class SessionCore;
class DocumentBase;
struct TerrainFamily;

// Add a surface map, a foliage map or a blend map (ADR 0046 S25 B4: a stroke's `add`) where the view's terrain has
// none its brush can paint (TerrainFamilyFile::addable). An authored terrain (its .trn a file of the project): the
// image made beside the .trn as <terrain>_m.pcx (8-bit, every texel class 1, the game's class where a terrain names no
// char map [orig: Terrain_GetSurfaceTypeAtPosition @ 0x6065C6 -> sub_605A10]), <terrain>_f.pcx (8-bit, every texel
// code 0: grows nothing) or <terrain>_d1.tga (32-bit, the first detail layer everywhere, as the terrain import makes
// one from no blendmap), one step of the file history, then the .trn's field naming it (polytrn_charmap,
// polytrn_foliagemap, polytrn_detailblendmap) set on its document as an edit from the view, which the view's Undo
// takes back; a file of that name the project holds already is named as it stands. An imported terrain: the image
// made beside its set as <set>_<key>.png (the blend map's seeded from the import's output, the others as above), the
// set's key written (surface, foliagemap, blendmap) and the set imported again, so the import makes the map and names
// it in the .trn. False, refused (terrain.stroke) with why, nothing changed.
bool add_terrain_map(SessionCore &core, const DocumentBase &view, const TerrainFamily &family, TerrainPlane plane);

} // namespace opennova::editor
