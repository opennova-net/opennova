#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <base/vfs/file_source.h>
#include <base/vfs/file_stamps.h>
#include <editor/preview/mission_scene.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/world.h>

namespace opennova::editor {

class MissionGround;

// The project's files as a mission's load reads them: mounted (each read noted with its stamp), the asset store over
// them (each model read once), items.def read as the load reads it (none: no traits are resolved and no collision
// model is attached). Kept while the files stand, for every world loaded over them.
struct MissionFiles {
	explicit MissionFiles(const std::shared_ptr<const FileSource> &files);
	~MissionFiles();
	MissionFiles(const MissionFiles &) = delete;
	MissionFiles &operator=(const MissionFiles &) = delete;

	std::shared_ptr<StampedFiles> stamped;
	ResourceIndex index;
	std::unique_ptr<assets::AssetStore> store;
	def::DefItemsFile items{};
	bool items_read = false;
};

// A mission's records in the engine's own world as the game's load spawns them, for what the mission view runs of the
// world (DI-23: the Shoot tool's rounds; DI-38: the people's ground solve): promoted [orig: Mission_LoadBMSFile
// @ 0x40F4E0], their items' traits [orig: Entity_InitFromItemDef @ 0x49E550] and collision models (BVOL/BPLN, CFAC)
// attached over `files`, the ground the mission's (its terrain's heights, its surfaces, its water), the collision's
// tables built as the load builds them, candidate slices included. Nothing ticks here: its owner runs what it needs of
// it.
struct MissionWorld {
	MissionWorld() = default;
	MissionWorld(const MissionWorld &) = delete;
	MissionWorld &operator=(const MissionWorld &) = delete;

	// `mission` promoted over `ground` and `files` (a record an owner loads empty carries type_id -1:
	// mission_world_thin); `placed` (when given) runs on the promoted world before the collision's tables are built.
	void load(const std::shared_ptr<MissionFiles> &files, const bms::File &mission, const MissionGround &ground,
			const std::function<void(world::World &)> &placed = {});
	// The scene's mark of a promoted entity, by the promote's back-reference (its pool and its record's index); null
	// for an entity no mark is of.
	const MissionEntityMark *mark_of(const world::Entity &entity, const std::vector<MissionEntityMark> &marks) const;

	std::shared_ptr<MissionFiles> files;
	world::World world;
	world::CollisionWorld collision;
	world::OcclusionWorld occlusion;
	world::EntityPoseProvider pose;
	mission::CollisionResolveState state;
	const terrain::TerrainHeightField *terrain = nullptr;
	float water_height = 0.0f;
	int collidable = 0; // the entities a collision model was attached to
};

// Each record of `mission`'s pools whose mark `keep` refuses loaded empty in its place (type_id -1: its index kept,
// so every other record lands where the game's load puts it); `marks` the scene's, by pool and record index.
void mission_world_thin(bms::File &mission, const std::vector<MissionEntityMark> &marks,
		const std::function<bool(const MissionEntityMark &)> &keep);

} // namespace opennova::editor
