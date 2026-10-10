#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_table.h>

namespace opennova::editor {

class AssetGraph;
struct JsonPage;
struct SessionView;

// The Place tool's palette (ADR 0046 S15, Placing and tweaking): every item a catalog of the project
// defines, by its name, in a group of what the game makes of it, with the model its graphic loads,
// the recently placed first. The group is the item's TYPE as items.def says it [orig:
// ItemDef_ParseProperty @ 0x49eb00; formats/def DefItemType] and, beside it, the pool a record of
// that TYPE is placed in (formats/mission/authoring.h, entity_kind_for_item_type): People land among
// the organics; Vehicles, Objects and Effects among the items; Buildings and Decoration among the
// buildings; Markers among the markers. A TYPE the table has no word for is Other. The palette is
// read from the asset graph (its Item symbols: the item's id, its name, its TYPE as the symbol's
// value; each catalog's `graphic` edges, resolved once per catalog), so it is what the drop of an
// item reads (preview/mission_items).

enum class MissionPaletteGroup : uint8_t { Recent, People, Vehicles, Objects, Buildings, Decoration, Markers, Effects, Other, kCount };
inline constexpr size_t kMissionPaletteGroupCount = size_t(MissionPaletteGroup::kCount);
// The group's words ("Recently placed", "People", "Vehicles", ...) and its token on the wire
// ("recent", "people", "vehicles", "objects", "buildings", "decoration", "markers", "effects", "other").
const char *mission_palette_group_words(MissionPaletteGroup group);
const char *mission_palette_group_token(MissionPaletteGroup group);
// The group an item of TYPE `type` stands in (-1, an unknown TYPE: Other).
MissionPaletteGroup mission_palette_group_of_type(int type);
// The pool's words a placed record of it stands in ("organics", "items", "buildings", "markers").
const char *mission_pool_words(MissionKind pool);

struct MissionPaletteItem {
	int64_t item = 0; // its id
	std::string name; // the catalog's name of it ("" none: its id stands for it)
	int type = -1; // its TYPE (-1: not known)
	MissionKind pool = MissionKind::Item; // the pool a placed record of it stands in
	MissionPaletteGroup group = MissionPaletteGroup::Other;
	std::string model; // the project's model file its graphic loads ("" none found)
	std::string file; // the catalog defining it
	bool recent = false; // among the recently placed
};

// One group of the palette: its items' places in MissionPalette::items, by name.
struct MissionPaletteSection {
	MissionPaletteGroup group = MissionPaletteGroup::Other;
	std::vector<size_t> items;
};

struct MissionPalette {
	std::vector<MissionPaletteItem> items; // every item that matches, each once
	// The groups that hold any, in MissionPaletteGroup's order: Recent first (the recently placed in
	// the order they were placed, most recent first; each in its own group too), then each TYPE's
	// group, its items by name (then id).
	std::vector<MissionPaletteSection> sections;
	size_t count = 0; // how many items the catalogs define (inert ones left out; an id once), matching or not
};

// The palette over `graph`'s Item symbols that match `filter` (its words found, case aside, in the
// item's name, its id or its model's file name; "" every item), the items `recent` names (ids, most
// recent first) in its Recent group. An inert symbol (one no lookup finds) is left out.
MissionPalette mission_palette(const AssetGraph &graph, const std::string &filter, const std::vector<int64_t> &recent);

// The wire form: {count, matching, groups: [{group, words, pool, count}], items: [a page of {item, name,
// type, type_words, group, pool, model, file, recent}]}; the page reads the items in the sections'
// order (a recent item twice: in Recent, and in its own group).
io::JsonValue mission_palette_to_json(const MissionPalette &palette, const JsonPage &page);

// An item's picture in the palette (ADR 0046 S23 C, the palette's thumbnails): its model seen from the side, the
// elevation the map's derivation makes of it (mission_model_outline, MissionOutlineView::Side: the edges of the faces
// that look at the viewer, a coarser LOD's past kMissionOutlineEdgesMax), lines in metres (forward, up). An editor's
// picture, not the game's: drawn from the model's own mesh in lines, as the 2D map draws a model's plan, so a row needs
// no device build of its own.
struct MissionPalettePicture {
	std::string model; // the project's model file
	bool read = false; // the model read (false: a file that does not read as a model, no picture)
	std::vector<float> lines; // four words a line: (forward, up) to (forward, up)
	float lo[2] = { 0.0f, 0.0f }, hi[2] = { 0.0f, 0.0f }; // the lines' box
	uint64_t stamp = 0;
};

// The palette's pictures (the texture thumbnails' pattern, preview/texture_thumbnails): each model's made once while
// its file's stamp stands, made off the frame: one asked for and not made is queued, the last picture of it (if any)
// answering meanwhile, and step() makes the queue's within the frame's budget.
class MissionPalettePictures {
public:
	// The picture of `model` (a project file): the one made while its stamp stands; else it is queued and the last made
	// answers (null for none yet).
	std::shared_ptr<const MissionPalettePicture> get(const SessionView &view, const std::string &model) const;
	// Makes the queued pictures, one at least, until `budget_us` microseconds have gone: true when one was made.
	bool step(const SessionView &view, int64_t budget_us);
	bool pending() const { return !queue_.empty(); }
	// How many pictures it made in all (a test counts what a stamp saves).
	size_t made() const { return made_; }
	void clear();

private:
	mutable std::map<std::string, std::shared_ptr<const MissionPalettePicture>> pictures_;
	mutable std::vector<std::string> queue_;
	size_t made_ = 0;
};

} // namespace opennova::editor
