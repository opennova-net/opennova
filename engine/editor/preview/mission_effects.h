#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/model/value.h>
#include <editor/preview/effect_catalog.h>
#include <runtime/assets/asset_store.h>
#include <runtime/particle/effect_closure.h>
#include <runtime/particle/effect_scene.h>

namespace opennova::editor {

class MissionPoses;
class MissionScene;
struct SessionView;

// One placed entity's particle slot as a mission's start attaches it (ADR 0046 DI-31), the slot
// items.def's `particlefx <effect> <userpoint>` authors [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails
// @ 0x522ee0]: its row, its item, the effect and the point as authored, and what the start makes of it
// (`status`): "attached" (an emitter at each of the model's first 16 points of the name, else one at the
// entity's origin: world::item_effect_attach_plan), else why none: "gated" (its pool's attrib gate turns it
// away: a powerup, an organic, world::item_effect_pool_allows), "controller" (a drivable item's, attached only
// while a driver controls it, which no record does at the start: world::item_effect_controller_allows),
// "no_model" (its graphic loads no model of the project). `points` names each emitter's point ("" the
// origin), `spawned` how many of them the scene made (its group and emitter limits, the game's, may refuse
// one).
struct MissionEffectSlot {
	NodeId row = 0;
	int64_t item = 0;
	std::string effect;
	std::string point;
	const char *status = "";
	std::vector<std::string> points;
	size_t spawned = 0;
};

// What a mission's items attach at its start, played on the preview clock (ADR 0046 DI-31; DI-21's
// DefinitionEffects for a whole mission): the engine's own effect scene opened over the closures of every
// effect the slots name (PreviewEffectCatalog::closures, the catalog the game would load), each attached
// slot's emitters spawned as the game's attached spawn makes them: at its point along its direction
// (effect_forward_pose, the device's EffectWorld::forward_pose), bound to the entity it follows
// (EffectBinding::FollowOwner; the owner's pose its transform as the placement draws it, the item's scale
// with it, a posed person lifted where its spawn stands it). The game binds a static's emitters to the world
// at its placement, never moved; here every slot follows its entity's row, so a move carries its effects as
// the engine moves an owner's groups (EffectScene::apply_owner_poses). The scene is stepped a game tick at a
// time as the clock runs; a jump of the clock (a seek, a step back, more ticks than one advance takes)
// empties it and spawns every slot again pre-aged by its age (EffectSpawnRequest::initial_age_ticks, bounded
// by kEffectInitialAgeTickLimit), its age counted from the tick it was first followed, or added. Live: an
// entity added, removed or given another item, a catalog or a model edited spawns or lets go of what moved
// (a let-go group drains, as the engine detaches one); an effect the scene lacks, or a particle file edited,
// opens the scene again over the new closures, every slot at its age. The scene is the device's picture
// (godot/src/authoring/preview_effects draws it).
class MissionEffects {
public:
	MissionEffects();
	~MissionEffects();
	MissionEffects(const MissionEffects &) = delete;
	MissionEffects &operator=(const MissionEffects &) = delete;

	// The scene's slots followed over the project's files (its catalog, the item catalogs the graph
	// resolves each item to, the models their graphics load, each read once while its stamp stands): true
	// when what it holds moved (a slot spawned, let go or moved; the scene opened again).
	bool refresh(const SessionView &view, const MissionScene &scene, const MissionPoses &poses);
	// The scene played to the clock's tick `tick`.
	void play_to(int32_t tick);
	// Nothing held, nothing played (the layer off, no mission).
	void close();

	// The scene the picture draws (null: no slot attached, or none whose effect the catalog finds); shared
	// with the device, which only reads it.
	const std::shared_ptr<particle::EffectScene> &scene() const { return scene_; }
	// Every entity whose item authors a particle slot, in the order the start walks them (the items, the
	// buildings, then the markers, each pool in the file's order).
	const std::vector<MissionEffectSlot> &slots() const { return slots_; }
	// The slot of row `row` (null: its item authors none).
	const MissionEffectSlot *slot(NodeId row) const;
	// What the game resolves an effect's name to (null: no slot names it).
	const particle::EffectClosure *closure_of(const std::string &effect) const;
	int32_t tick() const { return tick_; }
	// Moves whenever the scene is opened, changed or advanced (the snapshot a device drew is stale); how
	// many times it was opened and how many emitters were spawned in all (a test's measures).
	uint64_t serial() const { return serial_; }
	uint64_t opens() const { return opens_; }
	uint64_t spawns() const { return spawns_made_; }
	// The live groups of the slot `row` attached (their count; 0 for none).
	size_t alive(NodeId row) const;

	// The body's `effects`: {slots, attached, gated, controller, no_model, emitters, groups, particles, tick,
	// step_us (what the last play cost), effects: [{effect, slots, emitters, defined_in, spawns}]}.
	io::JsonValue to_json() const;

private:
	// An item's slot as its catalog's first row of its id authors it.
	struct ItemSlot {
		bool defined = false;
		std::string effect, point;
		uint32_t attrib = 0;
		int32_t scale_q16 = 0;
	};
	struct Catalog {
		uint64_t stamp = 0;
		std::unordered_map<int64_t, ItemSlot> items;
	};
	struct Model {
		uint64_t stamp = 0;
		assets::Model model;
	};
	// Where the graph resolves an item: its catalog and its graphic's model file ("" none).
	struct Resolved {
		std::string catalog, graphic;
	};
	// A slot as the scene holds it: its emitters' local poses, its owner's pose, the tick its age counts
	// from, the groups it spawned.
	struct Held {
		MissionEffectSlot slot;
		std::vector<particle::EffectPose> locals;
		particle::EffectPose owner;
		int32_t born = 0;
		std::vector<particle::EffectGroupId> groups;
	};

	const Catalog &catalog_(const SessionView &view, const std::string &file);
	const assets::Model &model_(const SessionView &view, const std::string &file);
	// The scene opened over the closures of `names` (none found: no scene).
	void open_(const std::vector<std::string> &names);
	// One held slot's emitters spawned, pre-aged by `age` ticks.
	void spawn_(Held &held, int32_t age);
	// One held slot's groups let go (they drain).
	void let_go_(Held &held);

	// What the slots were last followed over: the scene's serial, the poses', the graph's generation and the
	// asset source's (none moved and the catalog standing: nothing to follow).
	struct Followed {
		uint64_t scene = 0, graph = 0, files = 0;
		uint32_t poses = 0;
		bool operator==(const Followed &o) const {
			return scene == o.scene && graph == o.graph && files == o.files && poses == o.poses;
		}
	};
	Followed followed_key_;

	PreviewEffectCatalog catalog_files_;
	uint64_t catalog_serial_ = UINT64_MAX;
	uint64_t graph_generation_ = 0;
	bool graph_read_ = false;
	std::unordered_map<int64_t, Resolved> resolved_;
	std::map<std::string, Catalog> catalogs_;
	std::map<std::string, Model> models_;
	std::shared_ptr<particle::EffectScene> scene_;
	std::vector<std::string> names_; // the effects the scene was opened over, each once
	std::vector<particle::EffectClosure> closures_;
	std::vector<Held> held_; // the attached slots, in the start's order
	std::vector<MissionEffectSlot> slots_;
	std::unordered_map<NodeId, size_t> slot_index_;
	int wind_speed_ = 0, wind_direction_ = 0;
	bool played_ = false;
	bool followed_ = false; // a scene's slots followed once (those first followed are born at tick 0)
	int32_t tick_ = 0;
	uint64_t serial_ = 0;
	uint64_t opens_ = 0;
	uint64_t spawns_made_ = 0;
	int64_t step_us_ = 0; // what the last play cost, microseconds
};

} // namespace opennova::editor
