#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/model/node.h>
#include <editor/preview/mission_poses.h>
#include <runtime/world/collision.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova {
class StampedFiles;
}

namespace opennova::editor {

class MissionGround;
class MissionScene;

// The ground the game puts a mission's entities on, entity class by entity class (the deep-integration
// plan's DI-28): where the game leaves each one standing, against what lies under it there. The rules,
// each the game's own:
//
// - A marker draws no body and nothing grounds it: not checked.
// - An item or a building is drawn where its record stands it: the spawn copies the record's x, y and z
//   [orig: Entity_SpawnFromBMSRecord @0x40EB30..0x40EB3F] and nothing applies a ground offset after
//   (docs/world/world-wac-ai-re.md section 12), unless its definition's move_function names a class whose
//   update moves it every tick [orig: EntityDef_LookupPhysicsCallback @0x4a9240, a whole-name stricmp
//   over g_EntityClassPhysicsTable @0x82abc8; a name it lacks takes row 0, null]. Eight rows keep the
//   height: null (nullsub_2), envs (Entity_UpdateEnvSoundEmitter @0x4a8080), ewep
//   (Entity_UpdateTransformAndTurret @0x440ca0, which writes its turret's transform, not its own), door
//   (Entity_SetDefaultBoneCallbacks @0x4a91d0), genx (Entity_UpdateParentTransform @0x4a88b0: it moves only
//   with the entity it stands on), upfx (Entity_UpdateWaterPhysicsAndEffects @0x4a92e0), org0 (nullsub_28)
//   and chld (nullsub_82). Every other row moves it (the vehicles' and aircraft's movers, the rounds', an
//   elevator's fall and land, a toppling tower): the game places those itself, the player-controlled ones
//   set down on the ground under them at the mission's start [orig: Game_StartMission @0x525F97..0x525FED:
//   the ground probe from one unit up, plus the brain's floor, at least the water plane], the aircraft
//   flown. Such an item is the game's to place, never a finding.
// - What lies under a drawn static is what the game's ground probe meets in the column under its record:
//   the terrain's height there [orig: Terrain_RaycastHeightmapLoRes's column write @0x60cc12..0x60cc2d,
//   through Entity_RaycastCollision @0x413760], clipped by the type-1 solids of the other drawn items and
//   buildings the column crosses [orig: Entity_RaycastCollisionModel @0x413060, type 1 alone], probed
//   from one unit over the model's lowest point as the start's settle probes from one unit over the body
//   [orig: Game_StartMission @0x525FC1, height 0x10000]; or the water plane where it lies over that (the
//   start's settle takes the higher of the two [orig: @0x525FE0..0x525FED]). The model's span is its
//   collision box (the CMDL block) as its record places it [the placement CollisionWorld::target_view
//   builds]. It floats where its lowest point stands more than kMissionGroundSlack over what lies under it
//   and it touches no other entity's volume (a sign on a wall, a wire between poles, a crate inside a
//   building, a tunnel's contents in its pieces are meant so); it is buried where its highest point stands
//   under the terrain and it touches nothing. Nothing in the game sets either down.
// - A person (an organic its item poses, mission_poses.h) is set down by the warmup's ground solve where
//   its feet stand under one unit over the terrain, or under it at any depth [orig:
//   Entity_WarmUpOrganicAnimation @0x4B8BD8..0x4B8BF5]; a flying organic of an AI definition holds its
//   height [orig: Entity_SpawnFromBMSRecord @0x40EE2A..0x40EE33, Flags 0x80, org1's climb chase]; any other
//   starts in the air and stands on what the motor's ground tail finds under it, a model's floor or the
//   terrain [orig: Entity_MovementCollisionResolver @0x4B3D6E..0x4B3DA9], falling there under its class's
//   gravity (org1 [orig: Entity_UpdateInfantryAI @0x4b9910], org2 [orig: Entity_UpdateInfantryPlayerBody
//   @0x4b40e0]): the ground the game puts him on, no finding (a person a script mounts at the start stands
//   in the air on purpose). Under a class with no update (org0's nullsub_28) he hangs there: a finding.
//
// Each finding's fix sets the record's z where the rule stands it: a static's ground anchor on what lies
// under it, as the original editor's placer sets an item down (the height alone, docs/world/
// world-wac-ai-re.md section 12), else, where the anchor stands too high in the model to set it down by,
// its lowest point; a person's feet on the floor or the terrain under them.

// How far a static's lowest point may stand over what lies under it before it floats: one unit, the editor's
// slack (the unit the warmup sets a body down under [orig: Entity_WarmUpOrganicAnimation `cmp eax, 10000h`
// @0x4B8BE7]); an author's gap under it is left alone.
inline constexpr double kMissionGroundSlack = 1.0;

// How the game decides an entity's height (the rules above).
enum class MissionGroundRule : uint8_t {
	None, // a marker, or what the check cannot read (`why` says)
	Static, // drawn where its record stands it
	Person, // the warmup's ground solve, then the motor's ground tail
	Mover, // its class's update moves it: the game places it
};
// "none", "static", "person", "mover".
const char *mission_ground_rule_token(MissionGroundRule rule);

// What stands under an entity.
enum class MissionSupport : uint8_t { Nothing, Terrain, Water, Record };
// "nothing", "terrain", "water", "record".
const char *mission_support_token(MissionSupport on);

// What the check makes of an entity.
enum class MissionGroundState : uint8_t {
	Grounded, // on what lies under it (or the game's to place, or not checked)
	Floats, // a static whose model reaches nothing: its lowest point over what lies under it
	Buried, // a static wholly under the terrain, touching nothing
	Falls, // a person the warmup leaves in the air: his motor drops him onto what lies under him
	Hangs, // a person in the air whose class has no update: he stays there
};
// "grounded", "floats", "buried", "falls", "hangs".
const char *mission_ground_state_token(MissionGroundState state);

// One entity as the check found it.
struct MissionGroundVerdict {
	NodeId row = 0;
	NodeKind kind = 0;
	int64_t item = 0;
	std::string name; // its item's name in its catalog ("" none)
	MissionGroundRule rule = MissionGroundRule::None;
	// Why: a rule's leg ("marker", "no_item", "no_model", "no_terrain", "touches", "settled", "standing",
	// "flying", "move:<row>"), or the pose's status for a person the check does not pose ("class", "no_adm").
	std::string why;
	MissionGroundState state = MissionGroundState::Grounded;
	// The height the rule compares (a static's ground anchor, a person's feet), what stands under it, and
	// where: the terrain, the water plane, or an entity's solid (`on_row`, its item's name in `on_name`).
	double base = 0.0, support = 0.0;
	MissionSupport on = MissionSupport::Nothing;
	NodeId on_row = 0;
	std::string on_name;
	double terrain = 0.0; // the terrain's height under the record (where there is terrain)
	// A static's model's lowest and highest points as its record places it (its collision box; its anchor
	// alone where its model has none).
	double bottom = 0.0, top = 0.0;
	// The record's z where the rule stands it (Floats, Buried, Falls, Hangs), and whether the anchor sets it
	// there (else its lowest point, where the anchor would not: an anchor over the model's top).
	double fix_z = 0.0;
	bool by_anchor = false;
	// Whether the game leaves it off the ground: a finding (a person who falls lands where the game puts him).
	bool off() const {
		return state == MissionGroundState::Floats || state == MissionGroundState::Buried ||
				state == MissionGroundState::Hangs;
	}
};
io::JsonValue mission_ground_verdict_json(const MissionGroundVerdict &verdict);

// The files the check reads, each parsed once while its stamp stands (the check keeps it from one
// mission to the next): the item table the game reads, items.def [orig: ItemDefs_LoadAndValidate
// @0x4a1da0], and the models its items draw ("<graphic>.3di", assets::asset_file_name).
class MissionGroundReads {
public:
	struct Item {
		std::string name, graphic, move_function;
		int type = 0;
		int32_t scale_q16 = 0;
		PersonDefinition person;
	};
	struct Model {
		bool read = false;
		double anchor[3] = { 0.0, 0.0, 0.0 }; // its ground anchor, the model's own axes (mission_model_words)
		bool bounds = false; // a collision block: its CMDL box (model frame, mission axes, 16.16)
		int32_t box[6] = { 0, 0, 0, 0, 0, 0 };
		// Its collision volumes (null: none), and whether one is a type-1 solid, which the ground probe
		// clips; its solids' horizontal reach from the origin, 16.16.
		std::shared_ptr<const world::CollisionModel> collision;
		bool solid = false;
		int32_t reach_q16 = 0;
	};
	// An item's definition, its first row in items.def (null: none); every read through `files`.
	const Item *item(const FileSource &files, int64_t id);
	// A model by its graphic's name (null: none reads).
	const Model *model(const FileSource &files, const std::string &graphic);
	// Everything let go.
	void clear();
	// How many files it parsed in all (a test pins what a check reads again).
	size_t parsed() const { return parsed_; }

private:
	uint64_t items_stamp_ = 0;
	bool items_read_ = false;
	std::unordered_map<int64_t, Item> items_;
	struct Kept {
		uint64_t stamp = 0;
		Model model;
	};
	std::unordered_map<std::string, Kept> models_;
	size_t parsed_ = 0;
};

// Every entity of `scene` as the game grounds it (the rules above): `ground` the mission's terrain and
// water (MissionGround followed over the same files), `reads` the item table and the models, `files`
// what every file is read through (the catalog, the models, the people's tables and clips: a mission's every
// read noted there). In the scene's order.
std::vector<MissionGroundVerdict> mission_ground_verdicts(const MissionScene &scene, const MissionGround &ground,
		MissionGroundReads &reads, const std::shared_ptr<const StampedFiles> &files);

// The finding's words ("Wooden crate #12 stands 2.4 m above the terrain under its ground anchor: the game
// draws it where the record stands it.") and its fix's (label, detail) for an off verdict; `title` the
// entity's words.
std::string mission_ground_message(const MissionGroundVerdict &verdict, const std::string &title);
void mission_ground_fix_words(const MissionGroundVerdict &verdict, std::string &label, std::string &detail);

} // namespace opennova::editor
