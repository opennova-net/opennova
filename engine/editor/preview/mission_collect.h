#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/model/node.h>
#include <editor/preview/mission_scene.h>
#include <runtime/inmatch/occlusion_collect.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/world/occlusion_camera.h>

namespace opennova::bms {
struct File;
}
namespace opennova::mission {
class MissionKernel;
}

namespace opennova::editor {

class MissionGround;
struct MissionFiles;

// What one collect over a mission view's camera leaves its picture to draw (MissionCollect::collect): the records
// it drops (no pass draws them: hidden, and neither posed nor evaluated), and each building record's verdict as the
// game's batch draw takes it (world/occlusion_feed.h pack_building_visibility: visible << 32 | the raw section mask,
// and the def's forced sections the part draw ORs over it).
struct MissionCollectVerdicts {
	std::vector<NodeId> dropped;
	struct Building {
		NodeId row = 0;
		int64_t packed = 0;
		int64_t forced = 0;
	};
	std::vector<Building> buildings;
};

// The mission view's collect (ADR 0046; the maintainer's rule: the editor is a small module over the engine, so the
// view shows and animates exactly what the game's main view would from the same camera). The mission's records in the
// engine's own world as the game's load spawns them (mission_world_load_into, MissionWorld's load: promoted, their
// traits and collision models attached, the collision's tables built), in a bare MissionKernel, the collect's
// context; then the game's mission-start occlusion init over it (MissionKernel::occlusion_init_mission: the ground
// links, the tables, the start's blink stamp, the building portals). Per drawn frame the game's main-view collect over
// the view's camera: the frame words built (OcclusionWorld::build_frame), the entity walk
// (inmatch::collect_view_verdicts: the statics, pools 0 and 1, each through the render gates and its own latch) and
// the building verdicts (inmatch::collect_building_verdicts). Nothing ticks: the picture has no simulation (DI-31),
// so a record keeps the state the start leaves it in. Each record's entity carries its place among the scene's marks
// as its identity (the embedder's, as the game's carries the file's id), which the verdicts map back to its row.
class MissionCollect {
public:
	MissionCollect();
	~MissionCollect();
	MissionCollect(const MissionCollect &) = delete;
	MissionCollect &operator=(const MissionCollect &) = delete;

	// `mission` loaded over `files` and `ground`, `marks` the scene's (each record's row, by pool and index).
	void load(const std::shared_ptr<MissionFiles> &files, const bms::File &mission, const MissionGround &ground,
			const std::vector<MissionEntityMark> &marks);
	// A record moved under a gesture: its entity takes the record's place and angles as the load's spawn writes them
	// (the next load, at the gesture's end, builds the tables over it again). False: no entity of its row.
	bool move(const MissionEntityMark &mark);
	void close();
	bool loaded() const { return kernel_ != nullptr; }
	// The files the load read (null before one).
	const std::shared_ptr<MissionFiles> &files() const { return files_; }

	// One collect over `view` (the presentation frame's camera, as the game's main view hands it over:
	// world::occlusion_camera_from_view). Ticks the records' latches as the game's collect does.
	void collect(const world::OcclusionViewSpec &view, MissionCollectVerdicts &out);

private:
	std::unique_ptr<mission::MissionKernel> kernel_;
	std::shared_ptr<MissionFiles> files_;
	mission::CollisionResolveState collision_state_;
	std::vector<NodeId> rows_; // by identity - 1
	std::unordered_map<NodeId, uint16_t> handles_; // each row's entity
	inmatch::WireOcclusionLatchMap wire_latch_; // no wire rows: the walk's own scratch
	std::vector<int32_t> culled_;
	std::vector<int32_t> culled_wire_;
	std::vector<world::Vec3> anchors_;
	std::vector<inmatch::BuildingVerdict> buildings_;
};

} // namespace opennova::editor
