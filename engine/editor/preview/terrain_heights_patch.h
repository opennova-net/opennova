#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/cpt/cpt.h>

namespace opennova::editor {

class DocumentBase;
struct TerrainPatch;

// What a viewport's device takes of a heights document's change since its picture last showed it (ADR 0046 S25, the
// heights lane): the live atlas over each changed region (the heights a stroke wrote, or those a bake's landing drew
// back; the input where a bake is due, else the baked atlas) and the tiles a landing (or its undo) replaced, each by
// its index, all read from the open document, never its file. The device writes the heights into its terrain's,
// puts the tiles in place and shows the region again (godot/src/authoring/terrain_patch_applier).
struct HeightsDevicePatch {
	struct Region {
		int x = 0, y = 0, w = 0, h = 0; // atlas texels
		std::vector<uint16_t> heights;  // raw16, row-major, the top row first
	};
	std::string path; // the .cpt's project path
	std::vector<Region> regions;
	std::vector<int> tile_indices;
	std::vector<CptTile> tiles; // each index's new tile
	uint64_t serial = 0;        // the viewport's count of patches made, this one's
	bool empty() const { return regions.empty() && tile_indices.empty(); }
};

// The tiles a device last took of an open heights document, to say which a landing replaced: the document's own
// shared tiles (HeightsState::tiles), compared by identity.
class HeightsTilesFollow {
public:
	// The device's patch of `document` for `patch` (its regions, each clipped to the atlas; a whole patch the whole
	// atlas) and the tiles replaced since the last take or show. False, `out` untouched, where `document` is no
	// heights document that reads, or one other than the document last taken or shown (its picture is made again).
	bool take(const TerrainPatch &patch, const DocumentBase &document, HeightsDevicePatch &out);
	// The picture shows `document` as it is now (made again from its file, or first shown); null: none.
	void shown(const DocumentBase *document);

private:
	uint64_t identity_ = 0, load_ = 0;
	std::vector<std::shared_ptr<const CptTile>> tiles_;
};

} // namespace opennova::editor
