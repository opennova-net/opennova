#pragma once

#include <functional>
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
// map), with the model its graphic names (and its graphic_enemy, where it names another), or a
// weapon whose animadm names it, with its gfx1 (the first person's view model). The record's name,
// its file, the model's file name ("" where none).
struct MapPlayer {
	std::string record;
	std::string file;
	std::string model;
	std::string enemy_model; // an item's graphic_enemy where it names another model; "" else
	bool first_person = false;
	const GraphEdge *edge = nullptr;
};
std::vector<MapPlayer> map_players(const AssetGraph &graph, const AssetScan &scan, const std::string &map_path);

// The maps a model plays (S17: from the model to its animations): each record pairing the model with
// a map (preview_model_fields: an item's graphic or graphic_enemy beside its anim_def, a weapon's gfx1
// beside its animadm), the map's file and the record's name; a map the project lacks plays as
// default.adm where the project has it [orig: AnimMap_LoadAdmFile @ 0x40CD00..0x40CD25].
struct ModelAnimation {
	std::string map;    // the map's file, project-relative (default.adm's in place of one the project lacks)
	std::string record; // the record's name
	std::string file;   // the record's file
	std::string via;    // how: "" an item's graphic; "as an enemy", "first person", "in place of X.adm"
};
std::vector<ModelAnimation> model_animations(const AssetGraph &graph, const AssetScan &scan,
                                             const std::string &model_path);

// The models a record plays a map on (preview_model_fields: an item's graphic or graphic_enemy beside
// its anim_def, a weapon's gfx1 beside its animadm), by their file names (the scan's), sorted without
// case and once each: the rigs a new clip most likely plays on, which the Plays on choice offers first.
std::vector<std::string> animated_models(const AssetGraph &graph, const AssetScan &scan);

// What a clip no map of the project names, or a map no item or weapon names, comes to, in a sentence:
// the files the game names itself play without one naming them (failsafe.bad in place of a clip that
// does not load, the menu's player preview's Dt1rst.bad and PI_Idle.BAD, default.adm for an item
// whose map the game lacks); with a base layer mounted its files make no edges, so the sentence says
// only that no file of the project names it; else that the game never plays it.
std::string clip_unused_words(const AssetGraph &graph, const std::string &clip_path);
std::string map_unused_words(const AssetGraph &graph, const std::string &map_path);

// Whether a clip a map's row names registers as the game loads the map: its .bad (anim::bad_file_name: the name cut at
// its last '.' and ".bad" appended) is in the project, or failsafe.bad stands in for it [orig:
// AnimMap_FindOrLoadBoneFile @ 0x40C030, the name @ 0x40C094..0x40C0C6, the failsafe @
// 0x40C25B..0x40C2A1, none @ 0x40C260]. An empty predicate takes every named clip as registering.
using ClipLoads = std::function<bool(const std::string &clip)>;
ClipLoads project_clip_loads(const AssetScan &scan);

// What the game does with the map's row (or the row of the clip) at `address`, a sentence each: the
// slot's meaning, what becomes of it where none of its clips registers, the turn where several do, the
// reset row's rules and the slots the map leaves out (by what the game does with each), a key naming no
// slot, a slot an earlier row names. [orig: as documents/animation_slots.h and
// animation_map_document.h cite]
std::vector<std::string> map_row_notes(const AnimationMapDocument &document, const NodeAddress &address,
                                       const ClipLoads &loads = {});

// The slots no row of the map authors (no row names them, or none of their rows' clips registers):
// a body's map's of 1..239, a weapon's map's (one naming only first-person slots past its reset) of
// 240..251. Each serves the reset row's first clip; what the game does with it is the slot's own
// rule (animation_slot_absence).
std::vector<int> map_unauthored_slots(const AnimationMapDocument &document, const ClipLoads &loads = {});

} // namespace opennova::editor
