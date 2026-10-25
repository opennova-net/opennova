// The mission start's frame tick: retail zeroes it past the authority's pre pass and runs the
// rest of the start at 0, the pool-1 settle's single mover call included; the first frame's add
// brings it to 1 ahead of that frame's entity update. So the settle's staggered legs key on
// tick 0 + 36 * DcbId: a hull whose SSN is a multiple of 16 takes the movers' every-64-tick
// health leg in the settle call (D-NET-515: ours ran the settle at tick 1, where the key is odd).
// [orig: Game_StartMission @0x524360 - the zero @0x525B9F, the settle @0x525F80..0x526095
//  (`call ecx` @0x526010), no tick store between; Game_ProcessMainFrame `add tick, ebx`
//  @0x5265B4 ahead of the Entity_UpdateAllEntities call @0x52674B;
//  Vehicle_ProcessSelectorZeroGround @0x46E100 - the key g_CurrentTick + 36 * DcbId
//  @0x46E106..0x46E124, the authority's `test key, 3Fh` @0x46E1BC..0x46E1CB, the regen
//  @0x46E1CD..0x46E201]
#include "common/mission_rig.h"
#include "common/mission_set.h"

#include <base/gameprofile/game_type.h>
#include <runtime/world/vehicle_motor.h>
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

constexpr int kBuggyTypeId = 1291; // items.def 101291 "Drivable Dune Buggy" (PlayerControl)

// The deferred host start holds tick 0 until the start's end; the settle's mover call keys its
// health leg on that 0, and the start leaves the first frame's 1.
void test_the_settle_runs_at_the_zeroed_tick() {
	test_mission_set::Root root("mission_start_tick");
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
	expect(world.logic_tick == 0u, "the start's zero stands until the start ends");
	if (!expect(world.rules.projectile_authority, "an authority world")) return;

	w::Entity *buggy = nullptr;
	world.registry.for_each_in_pool(1, [&](const w::Entity &e) {
		if (e.item_id == kBuggyTypeId && buggy == nullptr) buggy = world.registry.get(e.handle);
	});
	if (!expect(buggy != nullptr, "the buggy is in pool 1")) return;
	w::VehicleTraits *traits = world.vehicles.traits.get_mutable(kBuggyTypeId);
	if (!expect(traits != nullptr && traits->player_control, "the buggy is a PlayerControl hull")) return;
	// A regenerating hull 100 under its ceiling, its DcbId a multiple of 16: (0 + 36 * 16) & 63
	// is 0, and (1 + 36 * 16) & 63 never is.
	traits->non_critical_regen = 7;
	traits->critical_hp = 0;
	buggy->net_id = 16;
	if (!expect(buggy->health_max > 200, "the buggy has its def's hp")) return;
	const int32_t before = buggy->health_max - 100;
	buggy->health = before;

	expect(rig->complete_mission_start(), "the deferred mission start runs");
	expect(buggy->health == before + 7, "the settle's mover call regenerates on tick 0's key");
	expect(world.logic_tick == 1u, "the start leaves the first frame's tick");
}

} // namespace

int main() {
	test_the_settle_runs_at_the_zeroed_tick();
	if (failures == 0) std::printf("mission_start_tick: all passed\n");
	return failures == 0 ? 0 : 1;
}
