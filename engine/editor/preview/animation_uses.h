#pragma once

#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/node.h>

namespace opennova::editor {

class AnimationMapDocument;
class AssetGraph;
struct GraphEdge;

// What uses an animation and what it is used for, in a modder's words (ADR 0046 S17, the
// animation lane): the maps and slots that play a clip, the items and weapons that play a map (on
// which model), the maps a model plays, and what the game does with a map's row. All of it from the
// asset graph's edges and the documents' own rows; the meanings from documents/animation_slots.h.

// A map's row that names a clip: the map's file (project-relative), the slot's key and words, and
// the edge (its record, its locator: what a Go to opens).
struct ClipUse {
	std::string map;
	std::string key;
	std::string words; // "walk forward"; the key as written where it names no slot
	const GraphEdge *edge = nullptr;
};
// Every map row naming the clip at `clip_path` (the graph's Animation edges into its file), in the
// graph's order.
std::vector<ClipUse> clip_uses(const AssetGraph &graph, const AssetScan &scan, const std::string &clip_path);

// A record that plays a map: an item whose anim_def names it (a soldier's, a crew's: the body's
// map), with the model its graphic names, or a weapon whose animadm names it (the first person's
// view model). The record's name, its file, the model's file name ("" where none, or a weapon's).
struct MapPlayer {
	std::string record;
	std::string file;
	std::string model;
	bool first_person = false;
	const GraphEdge *edge = nullptr;
};
std::vector<MapPlayer> map_players(const AssetGraph &graph, const AssetScan &scan, const std::string &map_path);

// The maps a model plays (S17: from the model to its animations): each item whose graphic names
// the model and whose anim_def names a map of the project, the map's file and the item's name.
struct ModelAnimation {
	std::string map;    // the map's file, project-relative
	std::string record; // the item's name
	std::string file;   // the item's file
};
std::vector<ModelAnimation> model_animations(const AssetGraph &graph, const AssetScan &scan,
                                             const std::string &model_path);

// The models an item plays a map on (each item's graphic beside its anim_def), by their file names
// (the scan's), sorted without case and once each: the rigs a new clip most likely plays on, which
// the Plays on choice offers first.
std::vector<std::string> animated_models(const AssetGraph &graph, const AssetScan &scan);

// What the game does with the map's row (or the row of the clip) at `address`, a sentence each: the
// slot's meaning, the turn rule where the row plays several clips, the reset row's rules and the
// body slots the map leaves out (which play its first clip), a key naming no slot, a slot an
// earlier row names. [orig: as documents/animation_slots.h and animation_map_document.h cite]
std::vector<std::string> map_row_notes(const AnimationMapDocument &document, const NodeAddress &address);

// The body slots (1..239) no row of the map names: they play the reset row's first clip.
std::vector<int> map_unauthored_slots(const AnimationMapDocument &document);

} // namespace opennova::editor
