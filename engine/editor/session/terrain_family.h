#pragma once

#include <array>
#include <memory>
#include <string>

#include <editor/documents/terrain_planes.h>
#include <formats/trn/trn.h>

namespace opennova::editor {

class DocumentBase;
struct SessionView;

// One file of a terrain's family as the game resolves it for a view (ADR 0046 S25, the shared seams): the project
// file a brush edits for that plane ("" none), the name the game reads it by, where a stroke there is refused and
// why (a base layer's file: the import fix; an import's output: its source routes in its place), and the pane's
// words for where the edits go ("dvxi5.cpt, in place"; "painted into dvxi5_h.png, made into the terrain as you
// save").
struct TerrainFamilyFile {
	std::string path;
	std::string name;
	// An import's output routes to the source image the import makes it from (`path` that image's).
	std::string import_output;
	std::string refusal;
	std::string words;
	// The terrain names no file for it, or one the project lacks (the pane's Add action; a mission's .til not made yet).
	bool missing = false;
	// Add makes it (S25 B4, session/terrain_add_map.h): a surface, foliage or blend map an authored .trn of the project
	// names none of, or an imported terrain's set names no image for.
	bool addable = false;
	bool resolved() const { return !path.empty() && refusal.empty(); }
};

// The terrain files a view's brush edits (S25): for a mission, as the game resolves them for it (the header's
// terrain, the .trn's .cpt and maps, which another .trn may name: D-TERRAIN-17; the environment's overrides:
// D-TERRAIN-18) and its <mission>.til (the game reads it in place of the terrain's own); for a terrain under the
// mission its viewport draws it with (none: the terrain's own .til). `error` says why nothing resolves.
struct TerrainFamily {
	std::string view;
	std::string terrain;
	std::string terrain_name;
	std::array<TerrainFamilyFile, kTerrainPlaneCount> planes;
	TerrainFamilyFile tiles;
	std::string error;
	// S25 B2: the tile set the view's tiles draw from, by the name the game reads it by (trn_mission_tilestrip: the
	// mission's tile set where its header names one, else the .trn's polytrn_tilestrip, as a .TGA; "" none).
	std::string tile_set;
	// S25 B4: the terrain's configuration as the game reads it for the view (its sector grid, origin and wraps: the
	// routing every map lookup takes); null where nothing resolves.
	std::shared_ptr<const TrnConfig> trn;
	const TerrainFamilyFile &plane(TerrainPlane p) const { return planes[static_cast<size_t>(p)]; }
	TerrainFamilyFile &plane(TerrainPlane p) { return planes[static_cast<size_t>(p)]; }
};

// The family of `document` (a mission or a terrain) in the session's view; false with `out.error` for a document
// that is neither, or whose terrain does not resolve.
bool resolve_terrain_family(const SessionView &view, const DocumentBase &document, TerrainFamily &out);

} // namespace opennova::editor
