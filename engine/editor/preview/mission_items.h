#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
	// Its entity's bound radius, metres (mission_item_bound_radius: what a pick of its mark tests); 0
	// for none (no model, or a model with no collision block).
	double radius = 0.0;
};

// The bound radius an entity of an item gets, metres, as the game's entity init stamps it at entity+0
// (world::entity_bound_radius_q16 [orig: Entity_InitFromModel @0x40dc30]): its model's GHDR radius
// scaled by the item's SCALE (`scale_q16`, 0 unscaled), at least its first husk's (`husk_q16`, where
// it has one), padded by 0x1000, and none where the model has no collision block. It is the sphere
// about the entity's position a ray of the game takes an entity by first (the proximity slots' broad
// phase [orig: Projectile_RaycastProximitySlots @0x4e5340]: a slot whose sphere the ray passes
// within), which the editor picks a mark by (ADR 0046, the polish: the game picks nothing with a
// pointer, and its ray's rule is the one it has).
double mission_item_bound_radius(int32_t model_q16, bool collision, int32_t scale_q16, bool has_husk, int32_t husk_q16);

// The bound radius of each item's entity (mission_item_bound_radius) over the project's graph and
// files, for the marks of a mission's picture: kept while the graph's generation stands, each model
// and catalog read once while its file's stamp stands, an item read when it is first asked for.
class MissionItemBounds {
public:
	// The radii of `items` as the project defines them now (an item asked for again while the graph
	// stands is not read again).
	void refresh(const SessionView &view, const std::vector<int64_t> &items);
	// Metres by item id; an item with no bound (none of the project's catalogs defines it, its model is
	// not in the project or has no collision block) is not listed.
	const std::unordered_map<int64_t, float> &radii() const { return radii_; }
	// How many model and catalog files it has read in all (a test pins what a refresh reads).
	size_t files_read() const { return files_read_; }

private:
	struct Model {
		uint64_t stamp = 0;
		bool read = false;
		bool collision = false;
		int32_t radius_q16 = 0;
	};
	struct Catalog {
		uint64_t stamp = 0;
		std::unordered_map<int64_t, int32_t> scale_q16; // by item id, the first definition of an id
	};
	const Model &model_(const SessionView &view, const std::string &file);
	const Catalog &catalog_(const SessionView &view, const std::string &file);

	uint64_t generation_ = 0;
	bool graph_ = false;
	std::unordered_set<int64_t> asked_;
	std::unordered_map<int64_t, float> radii_;
	std::map<std::string, Model> models_;
	std::map<std::string, Catalog> catalogs_;
	size_t files_read_ = 0;
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
