#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_table.h>

namespace opennova::editor {

struct SessionView;

// What a mission's drop reads of an item (ADR 0046 S14), through the project's asset graph and its
// files: the item a catalog of the project defines by its id (its name, its TYPE: the item symbol's
// value, data commit 6), the pool its TYPE puts a record in as the game's editor places it
// (formats/mission/authoring.h, entity_kind_for_item_type), the model its `graphic` loads, and that
// model's ground anchor (threedi_3di3_ground_anchor: its `ground` user point, else its origin) in
// the mission's frame. The anchor is an author-time bake (docs/world/world-wac-ai-re.md section 12):
// an entity dropped on the terrain is stored at the ground point less the anchor, and stored
// positions render as they are.
struct MissionItemFacts {
	int64_t item = 0;
	std::string name; // the catalog's name of it ("" none)
	int type = -1; // its TYPE (-1: not known)
	MissionKind pool = MissionKind::Item;
	std::string model; // the project's model file its graphic loads ("": none found)
	double anchor[3] = { 0.0, 0.0, 0.0 }; // mission x east, y north, z up
};

// The pool a record of TYPE `type` is placed in, as the game's editor places it
// (entity_kind_for_item_type): a person among the organics, a building, a decoration or foliage among
// the buildings, a marker among the markers, anything else among the items.
MissionKind mission_item_pool_of_type(int type);

// The item `item` as the project defines it: false, with why, when no catalog of the project does.
// A graphic that loads no model of the project leaves `model` empty and the anchor at the origin.
bool mission_item_facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error);

// The items whose graphic loads the model file `file` (a logical name or a project-relative path), in
// the catalogs' order, each once.
std::vector<int64_t> mission_items_of_model(const SessionView &view, const std::string &file);

// A model-space point (threedi_user_point_position's axes) in the mission's frame: the model's x, y,
// z are the presentation frame's -x, y, z (the placer's godot_vec3), which is the mission's
// (-x, -z, y).
void mission_model_point(const float model[3], double out[3]);

} // namespace opennova::editor
