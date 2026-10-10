#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/model/edit.h>

namespace opennova::editor {

struct SessionView;
struct MissionModelOutline;

// What a mission's drop reads of an item (ADR 0046 S14), through the project's asset graph and its
// files: the item a catalog of the project defines by its id (its name, its TYPE: the item symbol's
// value, data commit 6), the pool its TYPE puts a record in as the game's editor places it
// (formats/mission/authoring.h, entity_kind_for_item_type), the model its `graphic` loads, and that
// model's ground anchor (threedi_3di3_ground_anchor: its `ground` user point, else its origin) in
// the model's own axes, and the item's SCALE. The anchor is an author-time bake
// (docs/world/world-wac-ai-re.md section 12): an entity dropped on the terrain is stored at the ground
// point less the anchor's words, unrotated and unscaled, as the original editor stores it, and stored
// positions render as they are.
struct MissionItemFacts {
	int64_t item = 0;
	std::string name; // the catalog's name of it ("" none)
	int type = -1; // its TYPE (-1: not known)
	MissionKind pool = MissionKind::Item;
	std::string model; // the project's model file its graphic loads ("": none found)
	// Its ground anchor in the model's own axes as the game's placement matrix takes them (the file's
	// words, metres: mission_model_words), and the item's SCALE (16.16; 0 unscaled).
	double anchor[3] = { 0.0, 0.0, 0.0 };
	int32_t scale_q16 = 0;
	// Its entity's bound radius, metres (mission_item_bound_radius: what a pick of its mark tests); 0
	// for none (no model, or a model with no collision block).
	double radius = 0.0;
	// What a record placed of the item takes from its catalog row, as the original editor's placement does
	// (D-MIS-10) [orig: JOTACmed.exe MissionItem_InitFromDefinition @ 0x44dbf0]; `seeded` where its row was
	// read (an item a drop makes has none: a new record's own values stand). The team its Good and Evil
	// words give (1, 2, Evil over Good; 0 neither) [orig: @ 0x44dd51..0x44dd86]; its AI class, its sid up to
	// its first '.', eight characters at most, which the original's writer takes from the row on every save
	// (MissionDocument::set_item_classes) [orig: ItemsDef_ParseToken's sid arm, sub_462CB0 @ 0x462cb0 giving it
	// "iai" and the four characters cut; sub_44C8E0 @ 0x44cabe..0x44caf2]; its AI script, its default_aip's where
	// the project has the profile's .aip [orig: @ 0x44dccc..0x44dd2e; sub_44C8E0 @ 0x44c8e0, the eight bytes];
	// and its four AI keys [orig: @ 0x44dc46..0x44dcc2].
	bool seeded = false;
	int team = 0;
	std::string ai_class, ai_script;
	int32_t min_engagement = 16, max_engagement = 320, max_attack = 16, fire_timer = 10;
	// Its model seen from above (preview/mission_map_outline.h), where the cache keeps outlines (the 2D map's); null
	// otherwise, and for a model with no mesh.
	std::shared_ptr<const MissionModelOutline> outline;
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
	// `outlines`: each model read keeps its outline seen from above too (the 2D map's cache, S23 C).
	explicit MissionItemCache(bool outlines = false) : outlines_(outlines) {}
	// The item `item` as the project defines it now; false, with why, when no catalog of the project
	// does. A graphic that loads no model of the project leaves `model` empty, the anchor at the origin
	// and no bound.
	bool facts(const SessionView &view, int64_t item, MissionItemFacts &out, std::string &error);
	// The item's catalog row alone, no model read (facts' item, name, TYPE, pool and the values a record placed
	// of it takes); false where no catalog of the project defines it.
	bool row(const SessionView &view, int64_t item, MissionItemFacts &out);
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
		double anchor[3] = { 0.0, 0.0, 0.0 }; // its ground anchor in the model's own axes (mission_model_words)
		std::shared_ptr<const MissionModelOutline> outline; // where the cache keeps outlines
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

	bool outlines_ = false;
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

// The Sets that give each record a batch of `mission` places of an item what the item's catalog row holds
// (D-MIS-10), as the original editor's placement does [orig: JOTACmed.exe MissionItem_InitFromDefinition
// @ 0x44dbf0]: for an Add of an entity with its item (a drop, the Place tool's stop, the outline's or an MCP
// Add) and the item set on a record that named none (the outline's Add, then its item), its team, its AI
// script and its four AI keys (its AI class is the row's on every save: MissionDocument::set_item_classes);
// appended to `edits` after them, a field the batch sets on that record itself left to the batch. The one
// place a placement takes its row (DocumentSet's edit of a mission).
void plan_item_seeds(const SessionView &view, MissionItemCache &cache, const MissionDocument &mission,
                     std::vector<Edit> &edits);

// The AI class each item `mission` places is written with (MissionDocument::set_item_classes): its catalog
// row's (MissionItemFacts::ai_class), for every item a catalog of the project defines.
std::shared_ptr<const MissionItemClasses> mission_item_classes(const SessionView &view, MissionItemCache &cache,
                                                               const MissionDocument &mission);

// The items whose graphic loads the model file `file` (a logical name or a project-relative path), in
// the catalogs' order, each once.
std::vector<int64_t> mission_items_of_model(const SessionView &view, const std::string &file);

// A model-space point (threedi_user_point_position's axes) as the game's placement matrix takes it: the
// file's words (forward, left, up, metres), which threedi_user_point_position hands back as (-left,
// up, forward). Its z is the point's height over the model's origin whatever the entity's heading.
void mission_model_words(const float model[3], double out[3]);

// Where a model's point `words` (mission_model_words) of an entity at the angles `pitch`, `yaw`, `roll`
// (the record's degrees) stands from the entity's position, in the mission's frame, as the game draws
// it: the placement matrix Rz(90 - yaw) x Ry(-pitch) x Rx(roll) over the point scaled by the item's
// SCALE (`scale_q16`, 0 unscaled) [orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40, its
// userpoint carried by Entity_GetAttachmentWorldPosition @ 0x4B2670], through the engine's own placement
// basis (mission::bms_to_presentation_basis, which the device places the item's model by): what the 2D map
// draws a model's plan at. It is not what a drop bakes: the original editor stores the ground point less
// the words unrotated and unscaled (mission_ground_bake).
void mission_anchor_offset(const double words[3], int32_t scale_q16, double pitch, double yaw, double roll, double out[3]);

// What a drop on the terrain subtracts from the ground point to store an entity's position, as the original
// editor's place-object dialog does: the model's Ground user point's +0/+4/+8 words, unrotated and unscaled
// (`words`, mission_model_words: the point's x, y and z words in the mission's x, y and z) [orig: dfx2med.exe
// sub_401A90 @ 0x401f6e, its scatter loop @ 0x4021fe; docs/world/world-wac-ai-re.md section 12]. The ground
// command's terrain conform subtracts the height word alone (`out[2]`) [orig: dfx2med.exe sub_44D920,
// sub_43BAD0].
void mission_ground_bake(const double words[3], double out[3]);

} // namespace opennova::editor
