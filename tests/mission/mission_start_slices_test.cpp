// The mission start's authority rebuild of the pool-0/1 proximity tables and every source's
// candidate slice, after the WAC initial run and ahead of the pool-1 settle: the first update and
// the settle's ground rays walk slices built from the positions the start left, not the ones the
// load's earlier builds saw.
// [orig: Game_StartMission @0x524360 - the is_authority arm @0x525E29 / @0x525E58, its
//  Entity_BuildProximityLists_Pool01 @0x525E63 and Entity_BuildProximityListsFromPools
//  @0x525E68 (the slices into +0x1BC / +0x1C0: @0x4B8F10 / @0x4B8F16), ahead of the pool-1
//  settle @0x525F68..0x526095; the earlier builds: Entity_InitAllFromModels @0x40E460 (the
//  Entity_BuildAllProximityLists call @0x40E5A1) and @0x525C90]
#include "common/mission_rig.h"
#include "common/mission_set.h"

#include <base/gameprofile/game_type.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>

using namespace opennova;
namespace w = opennova::world;

namespace {

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kBuggyTypeId = 1291; // items.def 101291 "Drivable Dune Buggy"

bool slice_holds(const w::CollisionWorld &collision, w::EntityHandle source, w::EntityHandle target) {
	int32_t count = 0;
	const w::EntityHandle *slice = collision.candidate_slice(source, count);
	for (int32_t i = 0; i < count; ++i)
		if (slice[i] == target) return true;
	return false;
}

// A rifleman moved onto the buggy between the load and the mission start: the slice the load
// built for him does not hold the hull; the start's authority rebuild does.
void test_the_start_rebuilds_the_slices_on_the_authority() {
	test_mission_set::Root root("mission_start_slices");
	if (!expect(root.ok(), "the mission set stages")) return;
	auto rig = std::make_unique<testrig::MissionRig>();
	std::string error;
	if (!expect(rig->open(root.path(), "land_vehicles.bms", error), "land_vehicles.bms opens")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return;
	}
	mission::KernelBootOptions options;
	options.defer_mission_start = true;
	options.game_type = game_type::for_mission_attribs(rig->mission.header.attrib_flags);
	if (!expect(static_cast<mission::MissionKernel &>(*rig).boot(options, error),
	            "the mission boots with its start deferred")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return;
	}
	w::World &world = rig->world;
	if (!expect(world.collision != nullptr && world.rules.projectile_authority,
	            "an authority world with its collision")) return;
	w::EntityHandle buggy, rifleman;
	world.registry.for_each([&](const w::Entity &e) {
		if (e.handle.pool() == 1 && e.item_id == kBuggyTypeId && !buggy.valid()) buggy = e.handle;
		if (e.handle.pool() == 0 && e.handle != world.cached.local_player && e.item_type_index != 0 &&
				!rifleman.valid())
			rifleman = e.handle;
	});
	if (!expect(buggy.valid() && rifleman.valid(), "the buggy and a rifleman are in the world")) return;

	// Lift him clear of everything and publish that slice (empty), then set him on the hull
	// with no rebuild.
	w::Entity &man = *world.registry.get(rifleman);
	const w::Vec3 hull = world.registry.get(buggy)->position;
	man.position = {hull.x, hull.y, hull.z + 2000.0f};
	world.collision->rebuild_candidate_slices(world);
	expect(world.collision->candidate_count(rifleman) == 0, "his slice far above the map is empty");
	man.position = hull;
	expect(!slice_holds(*world.collision, rifleman, buggy), "the published slice predates the move");

	expect(rig->complete_mission_start(), "the deferred mission start runs");
	expect(slice_holds(*world.collision, rifleman, buggy),
	       "the start's authority rebuild puts the hull in his slice");
}

} // namespace

int main() {
	test_the_start_rebuilds_the_slices_on_the_authority();
	if (failures == 0) std::printf("mission_start_slices: all passed\n");
	return failures == 0 ? 0 : 1;
}
