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
	// What a record placed of the item takes from its catalog row, as the original editor's placement does
	// (D-MIS-10) [orig: JOTACmed.exe MissionItem_InitFromDefinition @ 0x44dbf0]; `seeded` where its row was
	// read (an item a drop makes has none: a new record's own values stand). The team its Good and Evil
	// words give (1, 2, Evil over Good; 0 neither) [orig: @ 0x44dd51..0x44dd86]; its AI class, its sid's first
	// eight characters, and its AI script, its default_aip's where the project has the profile's .aip [orig:
	// @ 0x44dccc..0x44dd2e; sub_44C8E0 @ 0x44c8e0, the eight bytes each]; and its four AI keys [orig:
	// @ 0x44dc46..0x44dcc2].
	bool seeded = false;
	int team = 0;
	std::string ai_class, ai_script;
	int32_t min_engagement = 16, max_engagement = 320, max_attack = 16, fire_timer = 10;
};

// The bound radius an entity of an item gets, metres, as the game's entity init stamps it at entity+0
// (world::entity_bound_radius_q16 [orig: Entity_InitFromModel @0x40dc30]): its model's GHDR radius
// scaled by the item's SCALE (`scale_q16`, 0 unscaled), at least its first husk's (`husk_q16`, where
// it has one), padded by 0x1000, and none where the model has no collision block. It is the sphere
// about the entity's position the game's ray tests first (the proximity slots' broad phase [orig:
// Projectile_RaycastProximitySlots @0x4e5340]); the game's choice among the entities that pass is a
// face hit, which the editor asks its device for (ViewportDevice::ray_between), going by the sphere
// only for a click with no device to ask.
double mission_item_bound_radius(int32_t model_q16, bool collision, int32_t scale_q16, bool has_husk, int32_t husk_q16);

// What a mission viewport reads of the items its entities name (ADR 0046 S14, the polish): an item's
// facts a drop reads (mission_item_facts) and the bound radius each item's entities are picked by
// (mission_item_bound_radius), over the project's graph and files. Each model and catalog is parsed once
// while its file's stamp stands (the asset source's: an open catalog's edits, a model exported again);
// an item's radius is asked once while the graph's generation stands and while the files it read stand.
class MissionItemCache {
public:
	// The item `item` as the project defines it now; false, with why, when no catalog of the project
	// does. A graphic that loads no model of the project leaves `model` empty, the anchor at the origin
	// and no bound.
	bool facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error);
	// The facts an item of `type` drawing the model `file` (a project path) would have, the item `item` not
	// in any catalog yet (ADR 0046 DI-12: the item a model's drop makes, then places): its pool by its TYPE,
	// the model and its ground anchor, read as facts() reads an item's graphic. False where the file does
	// not read as a model.
	bool model_facts(const SessionView &view, int64_t item, int type, const std::string &file, MissionItemFacts &out);
	// The radii of `items` as the project defines them now: every item asked again where the graph is
	// another; where the asset source's generation moved, the items whose files moved (a SCALE edited in
	// an open catalog, a model written again); an item not yet asked, asked.
	void refresh(const SessionView &view, const std::vector<int64_t> &items);
	// Metres by item id; an item with no bound (none of the project's catalogs defines it, its model is
	// not in the project or has no collision block) is not listed.
	const std::unordered_map<int64_t, float> &radii() const { return radii_; }
	// How many model and catalog files it has parsed in all, and how many items it has asked (a test pins
	// what a refresh reads).
	size_t files_read() const { return files_read_; }
	size_t items_asked() const { return items_asked_; }

private:
	struct Model {
		uint64_t stamp = 0;
		bool read = false;
		bool collision = false;
		int32_t radius_q16 = 0;
		double anchor[3] = { 0.0, 0.0, 0.0 }; // its ground anchor in the mission's frame
	};
	struct Catalog {
		uint64_t stamp = 0;
		std::unordered_map<int64_t, int32_t> scale_q16; // by item id, the first definition of an id
		// By item id, the first definition of an id: what a record placed of it takes (MissionItemFacts).
		struct Seed {
			bool good = false, evil = false;
			std::string sid, default_aip;
			int32_t min_engagement = 16, max_engagement = 320, max_attack = 16, fire_timer = 10;
		};
		std::unordered_map<int64_t, Seed> seeds;
	};
	// The files an asked item read: its catalog, its graphic's model, its first husk's model.
	struct Reads {
		std::string catalog, graphic, husk;
	};
	const Model &model_(const SessionView &view, const std::string &file);
	const Catalog &catalog_(const SessionView &view, const std::string &file);
	// An item's radius from its files (none listed for none).
	void ask_(const SessionView &view, int64_t item, const Reads &reads);

	uint64_t graph_generation_ = 0;
	bool graph_ = false;
	uint64_t files_generation_ = 0;
	std::unordered_map<int64_t, Reads> asked_;
	std::unordered_map<int64_t, float> radii_;
	std::map<std::string, Model> models_;
	std::map<std::string, Catalog> catalogs_;
	size_t files_read_ = 0;
	size_t items_asked_ = 0;
};

// The pool a record of TYPE `type` is placed in, as the game's editor places it
// (entity_kind_for_item_type): a person among the organics, a building, a decoration or foliage among
// the buildings, a marker among the markers, anything else among the items.
MissionKind mission_item_pool_of_type(int type);

// The item `item` as the project defines it, read afresh (MissionItemCache::facts over a cache of its
// own; a viewport asks its own cache).
bool mission_item_facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error);

// The items whose graphic loads the model file `file` (a logical name or a project-relative path), in
// the catalogs' order, each once.
std::vector<int64_t> mission_items_of_model(const SessionView &view, const std::string &file);

// A model-space point (threedi_user_point_position's axes) in the mission's frame: the model's x, y,
// z are the presentation frame's -x, y, z (the placer's godot_vec3), which is the mission's
// (-x, -z, y).
void mission_model_point(const float model[3], double out[3]);

} // namespace opennova::editor
