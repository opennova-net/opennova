// teleport_tails: each teleport action ends in its own proximity tail, as retail's does
// (D-COL-23's remainder, L5-s35). The tables observed: the pool-1 dynamic table (a row spawned
// after the last build appears only once Pool01 runs), the statics table (a static spawned after
// the last build counts only once Pool2 runs) and a teleported row's own list (it clears the
// Indoors bit, with no blink volume near).
//   - BMS SingleTeleport: a pool-1 target builds every slice and its own list; a pool-2 target
//     rebuilds the statics first; neither publishes the pool-0 / pool-1 tables.
//   - BMS Teleport (the group): once the marker matched, members or none, the statics, the
//     pool-0 / pool-1 tables and every slice, then each member's own list.
//   - WAC teleSSN on a marker that is not a person: the statics for a decoration or building
//     marker alone, then the pool-0 / pool-1 tables, every slice and the marker's own list.
// [orig: EventAction_TeleportEntityToSpawn @0x43DFC0 - Entity_BuildProximityListsFromPools
//  called @0x43E14F, Entity_BuildProximityList @0x43E155; Entity_BuildProximityLists_Pool2
//  @0x43E1F2, Entity_BuildProximityListsFromPools @0x43E1F7, Entity_BuildProximityList
//  @0x43E1FD; Entity_TeleportTeamToSpawn @0x43D390 - Entity_BuildProximityLists_Pool2 called
//  @0x43D4F8, Entity_BuildProximityLists_Pool01 @0x43D4FD, Entity_BuildProximityListsFromPools
//  @0x43D502, Entity_BuildProximityList @0x43D53D / @0x43D57D / @0x43D5BD; WacCmd_TeleSsn
//  @0x4F7E00 - the type tests @0x4F7F34 / @0x4F7F39, Entity_BuildProximityLists_Pool2 called
//  @0x4F7F3E, Entity_BuildProximityLists_Pool01 @0x4F7F43, Entity_BuildProximityListsFromPools
//  @0x4F7F48, Entity_BuildProximityList @0x4F7F4E]

#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A world with the tables built: a teleport marker (WP_NUMBER 5) in pool 3 and one static in
// pool 2. Heap-held (the 1 MB test-main stack under MSVC).
struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<w::CollisionWorld> collision = std::make_unique<w::CollisionWorld>();
	w::EntityHandle marker;

	Rig() {
		for (int pool = 0; pool < 4; ++pool) world->registry.configure_pool(pool, 8);
		world->collision = collision.get();
		world->ai.collision = collision.get();
		w::Entity m;
		m.item_id = w::kParticleEffectMarkerTypeId;
		m.has_item_def = true;
		m.item_type = 4;
		m.wp_number = 5;
		m.position = {40.0f, 50.0f, 6.0f};
		marker = world->registry.spawn(3, m);
		spawn_static();
	}

	// A pool-1 row the dynamic table takes (an item with a bound radius).
	w::EntityHandle spawn_dynamic(uint16_t ssn = 0, int group = 0) {
		w::Entity e;
		e.kind = w::EntityKind::Item;
		e.has_item_def = true;
		e.item_type = 1;
		e.item_type_index = ssn != 0 || group != 0 ? 4 : 0;
		e.net_id = ssn;
		e.group_id = static_cast<uint8_t>(group);
		e.bound_radius = 1;
		e.position = {-10.0f, -10.0f, 0.0f};
		return world->registry.spawn(1, e);
	}

	// A pool-2 decoration the statics table takes: a loaded row, its def ordinal (+0x1C) set.
	w::EntityHandle spawn_static(uint16_t ssn = 0, int group = 0) {
		w::Entity e;
		e.kind = w::EntityKind::Building;
		e.has_item_def = true;
		e.item_type = 2;
		e.item_type_index = 4;
		e.net_id = ssn;
		e.group_id = static_cast<uint8_t>(group);
		e.bound_radius = 1;
		e.position = {-20.0f, -20.0f, 0.0f};
		return world->registry.spawn(2, e);
	}

	void build() { collision->build_initial_tables(*world); }
	bool in_dynamic_table(w::EntityHandle h) const {
		int32_t radius = 0;
		return collision->dynamic_table_radius_q16(h, radius);
	}
	w::Entity &row(w::EntityHandle h) { return *world->registry.get(h); }
	void mark_indoors(w::EntityHandle h) { row(h).flags |= w::kEntityFlagIndoors; }
	bool indoors(w::EntityHandle h) { return (row(h).flags & w::kEntityFlagIndoors) != 0; }
};

// SingleTeleport on a pool-1 target: its slices and its list, no table published.
bool run_a_pool1_target_builds_the_slices_and_its_list_alone() {
	Rig r;
	const w::EntityHandle target = r.spawn_dynamic(300);
	r.build();
	r.mark_indoors(target);
	const w::EntityHandle late_dynamic = r.spawn_dynamic();
	r.spawn_static();
	const int32_t statics = r.collision->static_count();
	if (!expect(r.world->commands.teleport_ssn_to_marker(300, 5) &&
	                    r.row(target).position.x == 40.0f,
	            "pool 1: the target is teleported")) return false;
	if (!expect(!r.indoors(target), "pool 1: the target's own list runs")) return false;
	if (!expect(!r.in_dynamic_table(late_dynamic), "pool 1: no pool-0 / pool-1 table is published"))
		return false;
	return expect(r.collision->static_count() == statics, "pool 1: the statics stand");
}

// SingleTeleport on a pool-2 target: the statics first, then its slices and its list.
bool run_a_pool2_target_rebuilds_the_statics_first() {
	Rig r;
	const w::EntityHandle target = r.spawn_static(400);
	r.build();
	r.mark_indoors(target);
	const w::EntityHandle late_dynamic = r.spawn_dynamic();
	r.spawn_static();
	const int32_t statics = r.collision->static_count();
	if (!expect(r.world->commands.teleport_ssn_to_marker(400, 5) &&
	                    r.row(target).position.x == 40.0f,
	            "pool 2: the target is teleported")) return false;
	if (!expect(r.collision->static_count() == statics + 1, "pool 2: the statics are rebuilt"))
		return false;
	if (!expect(!r.indoors(target), "pool 2: the target's own list runs")) return false;
	return expect(!r.in_dynamic_table(late_dynamic), "pool 2: no pool-0 / pool-1 table is published");
}

// The group Teleport: every table and every slice, then each member's list; a matched marker
// with no member still rebuilds them.
bool run_the_group_rebuilds_every_table_then_each_members_list() {
	Rig r;
	const w::EntityHandle dynamic_member = r.spawn_dynamic(0, 7);
	const w::EntityHandle static_member = r.spawn_static(0, 7);
	r.build();
	r.mark_indoors(dynamic_member);
	r.mark_indoors(static_member);
	const w::EntityHandle late_dynamic = r.spawn_dynamic();
	r.spawn_static();
	const int32_t statics = r.collision->static_count();
	if (!expect(r.world->commands.teleport_group_to_marker(7, 5) == 2,
	            "group: both members are teleported")) return false;
	if (!expect(r.in_dynamic_table(late_dynamic), "group: the pool-0 / pool-1 tables are published"))
		return false;
	if (!expect(r.collision->static_count() == statics + 1, "group: the statics are rebuilt"))
		return false;
	if (!expect(!r.indoors(dynamic_member) && !r.indoors(static_member),
	            "group: each member's own list runs")) return false;
	const w::EntityHandle later_dynamic = r.spawn_dynamic();
	if (!expect(r.world->commands.teleport_group_to_marker(9, 5) == 0,
	            "group: no member of group 9")) return false;
	return expect(r.in_dynamic_table(later_dynamic),
	              "group: a matched marker with no member still publishes the tables");
}

// teleSSN on a marker that is not a person: the statics only for a decoration or building
// marker, then the pool-0 / pool-1 tables, every slice and the marker's own list.
bool run_tele_ssn_rebuilds_the_statics_for_a_building_marker_alone() {
	Rig r;
	const w::EntityHandle source = r.spawn_dynamic(500);
	r.build();
	r.mark_indoors(r.marker);
	const w::EntityHandle late_dynamic = r.spawn_dynamic();
	r.spawn_static();
	const int32_t statics = r.collision->static_count();
	if (!expect(r.world->commands.wac_teleport_ssn(w::EntityTarget(source), 5),
	            "teleSSN: the marker is found")) return false;
	if (!expect(r.in_dynamic_table(late_dynamic), "teleSSN: the pool-0 / pool-1 tables are published"))
		return false;
	if (!expect(r.collision->static_count() == statics,
	            "teleSSN: a type-4 marker leaves the statics")) return false;
	if (!expect(!r.indoors(r.marker), "teleSSN: the marker's own list runs")) return false;
	r.row(r.marker).item_type = 5;
	if (!expect(r.world->commands.wac_teleport_ssn(w::EntityTarget(source), 5),
	            "teleSSN: the building marker is found")) return false;
	return expect(r.collision->static_count() == statics + 1,
	              "teleSSN: a building marker rebuilds the statics");
}

// The statics table lists every pool-2 row with an item type (+0x1C) and a def, Building-type
// first, whatever its collision instance or radius (a def'd row with no collision block lists at
// the pad's one unit), and no def'd row without +0x1C; a def-less row is a synthetic world's,
// listed only with an instance or a radius.
// [orig: Entity_BuildProximityLists_Pool2 @0x4B9430 - the +0x1C and def tests @0x4B945D /
//  @0x4B9466 (pass 1, Building) and @0x4B94F1 / @0x4B94FA (pass 2), the radius pad @0x4B947B]
bool run_the_statics_list_every_typed_defd_row() {
	Rig r; // one def'd decoration with +0x1C and a radius
	w::Entity bare; // def'd, +0x1C, no instance, no radius
	bare.kind = w::EntityKind::Building;
	bare.has_item_def = true;
	bare.item_type = w::kItemDefTypeBuilding;
	bare.item_type_index = 4;
	const w::EntityHandle bare_h = r.world->registry.spawn(2, bare);
	w::Entity untyped = bare; // def'd, no +0x1C
	untyped.item_type_index = 0;
	untyped.bound_radius = 1;
	r.world->registry.spawn(2, untyped);
	w::Entity defless; // no def, no instance, no radius
	defless.kind = w::EntityKind::Building;
	r.world->registry.spawn(2, defless);
	r.build();
	if (!expect(r.collision->static_building_count() == 1 && r.collision->static_count() == 2,
	            "statics: the typed def'd rows list, Building first")) return false;
	const w::CollisionWorld::StaticSlotView first = r.collision->static_slot(0);
	return expect(first.h == bare_h && first.radius == 1,
	              "statics: a row with no collision block lists at the pad's one unit");
}

// The statics change only at a Pool2 call: a pool-2 row spawned (its instance assigned) or
// removed after the first build stays as that build left the table through every tick build and
// every registry-change refresh, until the load / mission-start BuildAll (or a teleport's Pool2).
// [orig: Entity_BuildProximityLists_Pool2 @0x4B9430 - callers Entity_BuildAllProximityLists
//  @0x4C20F0 (Entity_InitAllFromModels @0x40E5A1, Game_StartMission @0x525C90) and the teleports
//  @0x43D4F8 / @0x43E1F2 / @0x4F7F3E; Entity_UpdateAllEntities @0x4C2100 - the tick's Pool01
//  call @0x4C240A]
bool run_the_statics_change_only_at_a_pool2_call() {
	Rig r;
	r.build();
	const int32_t statics = r.collision->static_count();
	const w::EntityHandle late = r.spawn_static();
	w::CollisionModel box;
	box.sections.push_back(w::CollisionSection{});
	r.collision->assign_entity(late, r.collision->add_model(std::move(box)));
	for (int i = 0; i < 17; ++i) r.collision->build_tick_tables(*r.world);
	r.collision->refresh_after_registry_change(*r.world);
	if (!expect(r.collision->static_count() == statics,
	            "statics timing: no tick build or refresh lists a late static")) return false;
	r.collision->build_all_tables(*r.world);
	return expect(r.collision->static_count() == statics + 1,
	              "statics timing: the BuildAll lists it");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_a_pool1_target_builds_the_slices_and_its_list_alone();
	ok &= run_a_pool2_target_rebuilds_the_statics_first();
	ok &= run_the_group_rebuilds_every_table_then_each_members_list();
	ok &= run_tele_ssn_rebuilds_the_statics_for_a_building_marker_alone();
	ok &= run_the_statics_list_every_typed_defd_row();
	ok &= run_the_statics_change_only_at_a_pool2_call();
	if (!ok) return 1;
	std::printf("teleport_tails_test: OK\n");
	return 0;
}
