#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/model/node.h>
#include <runtime/world/infantry.h>

namespace opennova::anim {
class AdmRootMotion;
class RigFiles;
}
namespace opennova::terrain {
struct TerrainHeightField;
}
namespace opennova::bms {
struct File;
}

namespace opennova {
class StampedFiles;
}

namespace opennova::editor {

class MissionGround;
struct MissionEntityMark;
class MissionScene;
class ProjectAssetSource;
struct MissionFiles;
struct MissionWorld;
struct SessionView;

// What a person's spawn reads of its definition (its item's first row: its class, its .adm, its
// attributes) and of its record (its SSN, its route, its attributes).
struct PersonDefinition {
	std::string ai_function;
	std::string anim_def;
	uint32_t attrib = 0;
	// What its clips' events read of it (S23 C: the mission's people heard and firing as they play their clips): its
	// sound profiles, the body its move_function runs, its organic ammo and launch points (ClipSoundItem's).
	std::string sound_profile, sound_profile_female, move_function;
	std::string ammo[4];
	std::string launch[3];
};
// A placed person's body as the game spawns it (DI-38; docs/world/world-wac-ai-re.md section
// 41): the state its definition's organic init requests and the playhead its warmup leaves,
// before its first AI tick, computed by the engine's own spawn (world::organic_spawn_pose over
// the person's .adm and clips in the project) from what the init reads of its record and its
// item's definition: the record's route (waypoint_id) and Guarding attribute (an `aidata`
// definition's AI slot carries them), its id (the warmup's update count), and the variant rings
// every person of its .adm spawned before it in the file's order served.
struct MissionPose {
	NodeId row = 0;
	int64_t item = 0;
	// Whether the game poses it: "posed"; else why it stands as its model is bound:
	// "no_item" (no item catalog of the project defines its item), "class" (its ai_function names
	// another class than the persons' org0 and org1, whose definition callback is the init),
	// "no_adm" (its item names no .adm: the game binds no animation map), "no_clips" (its .adm,
	// default.adm in its place where the project lacks it, registers no clip).
	std::string status;
	std::string ai_function; // the item's, as its catalog spells it
	std::string adm;         // the .adm the game loads for it ("" none)
	bool ai_slot = false;    // its definition carries aidata: the record's route and guard reach the init
	// The state the init requests (g_AnimStateNameTable's index; world::infantry_anim_key names its
	// .adm row) and the record's field that chose it: "idle", "route" (a walk), "guard" (the
	// Guarding attribute), "route_126", "route_127" (the reserved routes).
	int state = -1;
	const char *because = "";
	uint32_t updates = 0; // the warmup's dual updates, its final one included
	world::InfantryBodyPose pose; // as the game's present rows would publish it
	// How far the warmup's vertical root motion lifts the body over its record's z, metres (its
	// final capsule bottom, the hips' height, and the clip's vertical deltas), and the final
	// capsule bottom (16.16), which the ground solve after it reads.
	double rise = 0.0;
	int32_t capsule_bottom = 0;
	// Where the spawn stands the person's origin over its record's z, metres: the rise, less the
	// ground solve's clearance where it is below one unit, the motor's ground tail probing the
	// terrain and the body's candidates (a model's floor, mission_pose_floor_clearance; over the
	// terrain alone, world::terrain_settle_clearance, with no mission world to probe). The device
	// draws the person there. The rise alone with no terrain read.
	double lift = 0.0;
	bool settled = false; // the solve moved it
	// A ground was read for it (a terrain): unsettled there, the warmup's solve leaves it in the air, the resolver
	// marking it airborne, and its think exits until it lands [orig: Entity_WarmUpOrganicAnimation
	// @0x4B8BD8..0x4B8BF5; Entity_UpdateInfantryAI @0x4BAA61..0x4BAA66].
	bool stood = false;
	NodeId on = 0;        // the record whose floor the solve met (0: the terrain, or nothing)
	std::string clip, source_clip; // the .bad each channel plays ("" none)
	uint32_t stamp = 0; // moves when anything above does
	// The definition it was posed from, its .adm's id in the poses' root motion, and its channels as the init and
	// the warmup leave them (world::OrganicSpawnBody's), which the people's play ticks on (mission_people.h).
	PersonDefinition definition;
	int adm_id = -1;
	world::InfantryState channels;
	// What its think reads of the record (S27, mission_people.h): its SSN (the think's stagger), the AI slot's alert
	// byte (the record's alert_state, which reaches the slot an `aidata` definition takes; 0 without one) and whether
	// it guards (Flags 0x40, the Guarding attribute through the same slot) [orig: Entity_SpawnFromBMSRecord, inside the
	// aidata branch @0x40ED5C: Flags 0x40 @0x40EDA5, the alert byte @0x40EEA4].
	int ssn = 0;
	uint8_t alert = 0;
	bool guarding = false;
};

struct PersonRecord {
	int ssn = 0;
	int route = 0;
	uint32_t attributes = 0;
	uint8_t alert = 0; // its alert_state
};

// What a person's spawn and its think read of its record (its item, attributes, route, SSN and alert_state).
struct MissionPoseInput {
	NodeId row = 0;
	int64_t item = 0;
	uint32_t attributes = 0;
	int route = 0;
	int ssn = 0;
	uint8_t alert = 0;
	bool operator==(const MissionPoseInput &o) const {
		return row == o.row && item == o.item && attributes == o.attributes && route == o.route && ssn == o.ssn &&
				alert == o.alert;
	}
};

// Every person of `inputs` (a mission's organics, in the file's order) posed as the game spawns it
// (MissionPose, with the lift its rise alone; mission_pose_clearance stands it): `item_of` answers an
// item's definition (null: no catalog defines it), `has_file` whether the project has a file of the
// name (the .adm the game loads in place of one it lacks), `files` serves the tables and clips through
// `motion`, every .adm read once. What MissionPoses runs, and the ground check (DI-28).
void mission_pose_people(const std::vector<MissionPoseInput> &inputs,
		const std::function<const PersonDefinition *(int64_t)> &item_of,
		const std::function<bool(const std::string &)> &has_file, const std::shared_ptr<const StampedFiles> &files,
		anim::AdmRootMotion &motion, std::vector<MissionPose> &out, world::AnimVariantRings *rings_out = nullptr);

// The warmup's ground solve over the terrain alone for a posed person whose record stands at (x, y, z):
// its feet's height over the terrain's column there, 16.16, once the warmup has lifted it by its rise
// (world::terrain_settle_clearance; the game sets a body down where it is under one unit [orig:
// Entity_WarmUpOrganicAnimation @0x4B8BD8..0x4B8BF5]). INT32_MAX with no terrain read.
int32_t mission_pose_clearance(const MissionPose &pose, double x, double y, double z,
		const terrain::TerrainHeightField *terrain);

// The same solve over a mission's world (MissionWorld, its collision's candidate slices built as the
// load builds them before the people's init [orig: Entity_InitAllFromModels @0x40E460, its
// Entity_BuildAllProximityLists call @0x40E5A1 ahead of the pool-0 definition callbacks
// @0x40E5DA..0x40E60B]) for the posed
// person `body` stands for: the body lifted by its rise, the motor's ground tail probing from its
// height raised to the 6144 grid down 2.0 u through the terrain and the body's candidate slice (each
// candidate's type-1 solids), its feet's clearance over what it meets (16.16) and `on` the record
// whose floor that is (`marks` the scene's; 0: the terrain, or nothing) [orig:
// Entity_WarmUpOrganicAnimation @0x4B8BD8..0x4B8BF5 -> Entity_MovementCollisionResolver's
// ground-settle tail @0x4B3D6E..0x4B3DA9 -> Entity_RaycastGroundHeightAndObject @0x414320].
// INT32_MAX for a handle the world holds no body of.
int32_t mission_pose_floor_clearance(MissionWorld &world, world::EntityHandle body, const MissionPose &pose,
		const std::vector<MissionEntityMark> &marks, NodeId &on);

// What a mission's people stand on (MissionPoses::stand): the mission's terrain (null: none read),
// and, for the floors the game's solve meets too, the mission as it stands (its file composed),
// its ground (its terrain, the char map, the water) and each item's bound (metres, how far a
// record's candidates reach) — with any of them null the terrain alone.
inline constexpr double kMissionFloorPad = 4.0;    // a pool-0 body's candidate pad, units
inline constexpr double kMissionFloorReach = 20.0; // an item of no known bound: kept within this, metres
struct MissionPeopleFloor {
	const terrain::TerrainHeightField *terrain = nullptr;
	const bms::File *mission = nullptr;
	const MissionGround *ground = nullptr;
	const std::unordered_map<int64_t, float> *bounds = nullptr;
};

// The poses of a mission's people (MissionViewport's, ADR 0046 S14's device draws them): every
// organic of the scene posed again where its records, the project's graph or a file the poses
// read (a catalog, an .adm, a clip) moved; each catalog and each .adm read once while its stamp
// stands.
class MissionPoses {
public:
	MissionPoses();
	~MissionPoses();
	MissionPoses(const MissionPoses &) = delete;
	MissionPoses &operator=(const MissionPoses &) = delete;

	// True when a pose changed (the device poses its people again).
	bool refresh(const SessionView &view, const MissionScene &scene);
	// Each posed person stood where the spawn's ground solve stands it over `floor` (no terrain
	// read: the rise alone), at its record as `scene` holds it now: with the mission given, over its
	// world (the posed people and the items and buildings within their candidates' reach, loaded as
	// the game's load spawns them over the project's files, MissionWorld), else over the terrain
	// alone. True when a person's lift moved (its stamp with it).
	bool stand(const MissionScene &scene, const MissionPeopleFloor &floor);
	// Whether a file the floors' world read moved since (a model's floor edited: stand them again).
	bool floor_files_moved() const;
	// The floors' world as stand() last made it, for the people who play in a world of their own (S27,
	// mission_people.h): its serial (0: none made, no mission or no terrain read; moves each time it is made), a
	// fresh load of the same world (the same records over the same files and ground: null with none), and each
	// posed person's body in it by row (the same handles in every load).
	uint64_t floor_serial() const { return floor_world_ ? floor_serial_ : 0; }
	std::unique_ptr<MissionWorld> load_floor_world() const;
	const std::map<NodeId, world::EntityHandle> &floor_bodies() const { return floor_bodies_; }
	void clear();
	// A row's pose (null: no organic row of the scene).
	const MissionPose *pose(NodeId row) const;
	const std::vector<MissionPose> &poses() const { return poses_; }
	// How many it poses; how many times it posed every person, and how many files it parsed in
	// all (a test pins what a refresh does).
	size_t posed() const;
	size_t runs() const { return runs_; }
	size_t files_read() const { return files_read_; }
	// Moves whenever a pose's stamp does (a pose or a lift changed).
	uint32_t serial() const { return serial_; }
	// The root motion the people were posed through (their .adm ids its), and the .adms' ring heads as the last spawn
	// left them: what their play ticks on with (mission_people.h). Null before a pose.
	anim::AdmRootMotion *motion() const { return motion_.get(); }
	const world::AnimVariantRings &rings() const { return rings_; }
	// How many times the people were posed (moves with each pose_all_).
	size_t generation() const { return runs_; }

private:
	struct Catalog {
		uint64_t stamp = 0;
		std::unordered_map<int64_t, PersonDefinition> items; // each id's first row
	};
	const Catalog &catalog_(const std::string &file);
	void pose_all_();

	std::shared_ptr<const ProjectAssetSource> source_;
	std::shared_ptr<StampedFiles> files_; // what the clips read, by the stamp they read it at
	std::shared_ptr<MissionFiles> floor_files_; // what the floors' world read, kept while it stands
	// The floors' world as last made, its people's bodies by row, and the key of what it holds
	// (stand()).
	std::unique_ptr<MissionWorld> floor_world_;
	std::map<NodeId, world::EntityHandle> floor_bodies_;
	uint64_t floor_key_ = 0;
	uint64_t floor_serial_ = 0; // moves each time the world is made
	std::unique_ptr<bms::File> floor_mission_; // the records it loaded, and over which ground
	const MissionGround *floor_ground_ = nullptr;
	std::unique_ptr<anim::AdmRootMotion> motion_;
	world::AnimVariantRings rings_;
	std::unordered_map<std::string, Catalog> catalogs_;
	std::vector<MissionPoseInput> inputs_;
	std::unordered_map<int64_t, std::string> resolved_; // each item's catalog file ("" none)
	uint64_t graph_ = 0;
	uint64_t files_generation_ = 0;
	bool read_ = false;
	std::vector<MissionPose> poses_;
	std::unordered_map<NodeId, size_t> rows_;
	uint32_t serial_ = 0;
	size_t runs_ = 0;
	size_t files_read_ = 0;
};

// One person posed as the game spawns it (DI-38): `pose` from its status on (its row and item the
// caller's), through `motion` over the project's files (`has_file` whether it has a file of the
// name, `files` noting what the pose reads, `rig_files` over them), the .adm's ring heads `rings`
// served in the order the game's spawn serves them. mission_pose_people runs it for every person
// of a mission in the file's order; a definition's picture (DI-21) for its one person, as the
// first of its .adm.
void pose_person(const PersonDefinition &definition, const PersonRecord &record,
		const std::function<bool(const std::string &)> &has_file, const StampedFiles &files,
		const anim::RigFiles &rig_files, anim::AdmRootMotion &motion, world::AnimVariantRings &rings,
		MissionPose &pose);

// A pose on the wire (the mission viewport's items: an organic's `pose`): {status, ai_function,
// adm, ai_slot, state, row (its .adm row, "anim_idle"), because, updates, rise, alert (its AI slot's
// alert byte), playing {state, row, phase, variant, clip, parked}, and while it blends from {state,
// row, phase, variant, clip} and weight}.
io::JsonValue mission_pose_json(const MissionPose &pose);

} // namespace opennova::editor
