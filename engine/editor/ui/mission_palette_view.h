#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/preview/mission_palette.h>

namespace opennova::editor {

class AssetGraph;

// The payload of an item's row dragged out of the palette: its id as text, which the mission
// canvas's drop target takes (a drop of the item where it is let go).
inline constexpr const char *kItemDragPayload = "OPENNOVA_ITEM";

// The Place tool's palette panel (ADR 0046 S15, Placing and tweaking), beside the mission's picture:
// a search box, then the items in their groups (preview/mission_palette: Recently placed first, each
// group a header that folds, its pool in its tooltip), each row the item's name with its model's
// file muted after it and its id in its tooltip. A click picks the item, which every click on the
// picture then places; a row dragged over the picture places one where it is let go. It keeps the
// palette it made until the graph, the filter or the recent items move (a JO catalog holds some
// thousand items).
class MissionPaletteView {
public:
	// Draws in the current window's room; the item picked this frame (0: none). `picked` the item the
	// tool places now (its row shown selected).
	int64_t draw(const AssetGraph *graph, uint64_t generation, const std::vector<int64_t> &recent, int64_t picked);

	// The name of an item the palette lists ("" when it does not).
	std::string name_of(int64_t item) const;
	// The palette made for the last draw.
	const MissionPalette &palette() const { return palette_; }

private:
	char filter_[64] = {};
	bool made_ = false;
	uint64_t generation_ = 0;
	std::string made_filter_;
	std::vector<int64_t> made_recent_;
	MissionPalette palette_;
};

} // namespace opennova::editor
