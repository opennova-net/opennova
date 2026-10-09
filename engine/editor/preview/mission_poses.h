#pragma once

#include <cstdint>
#include <functional>
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

namespace opennova {
class StampedFiles;
}

namespace opennova::editor {

class MissionScene;
class ProjectAssetSource;
struct SessionView;

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
	// ground solve's clearance where it is below one unit, over the terrain alone
	// (world::terrain_settle_clearance; the game's solve also stands a person on a model's floor,
	// which the editor does not read). The device draws the person there. The rise alone with no
	// terrain read.
	double lift = 0.0;
	bool settled = false; // the terrain's solve moved it
	std::string clip, source_clip; // the .bad each channel plays ("" none)
	uint32_t stamp = 0; // moves when anything above does
};

// Whether an item's ai_function names a person class whose definition callback is the organic init
// (org0, org1): the init that poses its spawn and resolves its ammo bytes and launch points
// (world::resolve_organic_weapons).
bool person_class(const std::string &ai_function);

// What a person's spawn reads of its definition (its item's first row: its class, its .adm, its
// attributes) and of its record (its SSN, its route, its attributes).
struct PersonDefinition {
	std::string ai_function;
	std::string anim_def;
	uint32_t attrib = 0;
};
struct PersonRecord {
	int ssn = 0;
	int route = 0;
	uint32_t attributes = 0;
};

// What a person's spawn reads of its record (its item, attributes, route and SSN).
struct MissionPoseInput {
	NodeId row = 0;
	int64_t item = 0;
	uint32_t attributes = 0;
	int route = 0;
	int ssn = 0;
	bool operator==(const MissionPoseInput &o) const {
		return row == o.row && item == o.item && attributes == o.attributes && route == o.route && ssn == o.ssn;
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
		anim::AdmRootMotion &motion, std::vector<MissionPose> &out);

// The warmup's ground solve over the terrain alone for a posed person whose record stands at (x, y, z):
// its feet's height over the terrain's column there, 16.16, once the warmup has lifted it by its rise
// (world::terrain_settle_clearance; the game sets a body down where it is under one unit [orig:
// Entity_WarmUpOrganicAnimation @0x4B8BD8..0x4B8BF5]). INT32_MAX with no terrain read.
int32_t mission_pose_clearance(const MissionPose &pose, double x, double y, double z,
		const terrain::TerrainHeightField *terrain);
// The clearance under which the warmup sets a body down: one unit [orig: `cmp eax, 10000h` @0x4B8BE7].
inline constexpr int32_t kMissionPoseSettle = 65536;

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
	// Each posed person stood where the spawn's ground solve stands it over `terrain` (null: none
	// read, the rise alone), at its record as `scene` holds it now; cheap, so run on every follow.
	// True when a person's lift moved (its stamp with it).
	bool stand(const MissionScene &scene, const terrain::TerrainHeightField *terrain);
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

private:
	struct Catalog {
		uint64_t stamp = 0;
		std::unordered_map<int64_t, PersonDefinition> items; // each id's first row
	};
	const Catalog &catalog_(const std::string &file);
	void pose_all_();

	std::shared_ptr<const ProjectAssetSource> source_;
	std::shared_ptr<StampedFiles> files_; // what the clips read, by the stamp they read it at
	std::unique_ptr<anim::AdmRootMotion> motion_;
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
// adm, ai_slot, state, row (its .adm row, "anim_idle"), because, updates, rise, playing {state,
// row, phase, variant, clip, parked}, and while it blends from {state, row, phase, variant, clip}
// and weight}.
io::JsonValue mission_pose_json(const MissionPose &pose);

} // namespace opennova::editor
