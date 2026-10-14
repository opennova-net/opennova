#include <editor/preview/mission_world.h>

#include <cmath>
#include <map>
#include <utility>

#include <base/io/fixed.h>
#include <editor/preview/mission_ground_facts.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/promote.h>

namespace opennova::editor {

namespace {

// The pool a promoted entity's kind is placed in, as the scene names it.
bool pool_of(world::EntityKind kind, MissionPool &out) {
	switch (kind) {
	case world::EntityKind::Item: out = MissionPool::Item; return true;
	case world::EntityKind::Building: out = MissionPool::Building; return true;
	case world::EntityKind::Marker: out = MissionPool::Marker; return true;
	case world::EntityKind::Organic: out = MissionPool::Organic; return true;
	}
	return false;
}

} // namespace

MissionFiles::MissionFiles(const std::shared_ptr<const FileSource> &files) {
	stamped = std::make_shared<StampedFiles>(files);
	index.mount_source(stamped);
	store = std::make_unique<assets::AssetStore>(&index);
	std::vector<uint8_t> bytes;
	if (stamped->read("items.def", bytes) && !bytes.empty() &&
	    def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0)
		items_read = true;
}

MissionFiles::~MissionFiles() {
	if (items_read) def::def_free_items(&items);
}

void MissionWorld::load(const std::shared_ptr<MissionFiles> &read, const bms::File &mission, const MissionGround &ground,
		const std::function<void(world::World &)> &placed) {
	files = read;
	mission::PromoteOptions options;
	if (files->items_read) {
		const def::DefItemsFile *items = &files->items;
		options.item_attributes = [items](int32_t type_id) -> uint32_t {
			const def::DefItemDef *def = mission::find_item_def(*items, int(type_id) + int(mission::kItemIdOffset));
			return def ? def->attrib : 0u;
		};
	}
	mission::promote_mission(mission, world, options);
	world.ai.is_authority = true;
	if (files->items_read)
		mission::resolve_item_traits(world, files->items, [](int32_t) -> uint8_t { return 0; }, files->store.get());
	terrain = ground.height_field();
	water_height = ground.water() ? float(ground.water_height()) : 0.0f;
	world.tables.surface_map = ground.surface_sampler();
	world.env.water_z = ground.water() ? int32_t(std::lround(ground.water_height() * io::kFp16OneD)) : 0;
	pose.set_assets(files->store.get());
	collision.terrain = terrain;
	collision.set_pose_provider(&pose);
	world.collision = &collision;
	world.pose_provider = &pose;
	world.ai.collision = &collision;
	if (files->items_read) {
		const mission::CollisionResolveDeps deps{ collision, occlusion, pose, *files->store };
		collidable = mission::resolve_collision_instances(world, files->items, state, deps);
	}
	if (placed) placed(world);
	collision.build_initial_tables(world);
}

const MissionEntityMark *MissionWorld::mark_of(const world::Entity &entity,
		const std::vector<MissionEntityMark> &marks) const {
	MissionPool pool = MissionPool::Item;
	if (entity.item_id < 0 || !pool_of(entity.kind, pool)) return nullptr;
	const int index = int(world::spawn_origin_index(entity.spawn_origin));
	for (const MissionEntityMark &mark : marks)
		if (mark.pool == pool && mark.index == index) return &mark;
	return nullptr;
}

void mission_world_thin(bms::File &mission, const std::vector<MissionEntityMark> &marks,
		const std::function<bool(const MissionEntityMark &)> &keep) {
	std::map<std::pair<int, int>, const MissionEntityMark *> by_record;
	for (const MissionEntityMark &mark : marks) by_record[{ int(mark.pool), mark.index }] = &mark;
	const auto thin = [&](std::vector<bms::Entity> &records, MissionPool pool) {
		for (size_t i = 0; i < records.size(); ++i) {
			const auto found = by_record.find({ int(pool), int(i) });
			if (found == by_record.end() || !keep(*found->second)) records[i].type_id = -1;
		}
	};
	thin(mission.items, MissionPool::Item);
	thin(mission.buildings, MissionPool::Building);
	thin(mission.markers, MissionPool::Marker);
	thin(mission.organics, MissionPool::Organic);
}

} // namespace opennova::editor
