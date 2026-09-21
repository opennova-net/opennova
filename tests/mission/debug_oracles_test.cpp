// The F3 debug oracles over the kernel (runtime/mission/debug_oracles.h): the
// hitbox view's bounded stand-in for a pool-0 entity without collision
// sections (its center lifted by the organic stand-in height), the dead /
// hidden and local-player exclusions, the entity cap; the pick's zero ray
// (nothing, the tick stamped) and its empty-world miss.
#include <runtime/mission/debug_oracles.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/geom.h>
#include <runtime/world/round_sim.h>

#include <cstdio>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

int main() {
	mission::MissionKernel kernel;
	world::World &w = kernel.world;

	// An empty world: nothing to report, nothing to pick.
	mission::DebugHitboxReport report;
	mission::collect_debug_hitboxes(kernel, report);
	CHECK(report.entities.empty() && report.organics.empty());
	w.logic_tick = 77;
	mission::DebugPick pick;
	const double from[3] = { 0.0, 0.0, 2.0 };
	const double zero[3] = { 0.0, 0.0, 0.0 };
	mission::debug_pick_entity(kernel, from, zero, 100.0, pick);
	CHECK(!pick.hit && pick.blocked == mission::DebugPick::Blocked::None && pick.tick == 77);
	const double dir[3] = { 1.0, 0.0, 0.0 };
	mission::debug_pick_entity(kernel, from, dir, 100.0, pick);
	CHECK(!pick.hit && pick.blocked == mission::DebugPick::Blocked::None);

	// Pool-0 organics without collision sections take the bounded stand-in;
	// a dead / hidden row (engine_flags 0x02000001) is skipped.
	w.registry.configure_pool(0, 8);
	world::Entity organic;
	organic.kind = world::EntityKind::Organic;
	organic.alive = true;
	organic.position = { 10.0f, 20.0f, 3.0f };
	const world::EntityHandle a = w.registry.spawn(0, organic);
	organic.position = { 30.0f, 40.0f, 5.0f };
	organic.engine_flags |= 0x1u;
	const world::EntityHandle hidden = w.registry.spawn(0, organic);
	CHECK(a.valid() && hidden.valid());
	mission::collect_debug_hitboxes(kernel, report);
	CHECK(report.organics.size() == 1);
	if (report.organics.size() == 1) {
		const mission::DebugHitboxOrganic &o = report.organics[0];
		CHECK(o.handle == a && o.fallback && o.section == 1 && !o.masked);
		CHECK(o.center[0] == world::to_fixed(10.0f) && o.center[1] == world::to_fixed(20.0f) &&
				o.center[2] == world::to_fixed(3.0f + world::kOrganicStandInCenterZ));
		CHECK(o.radius_q16 == world::to_fixed(world::kOrganicStandInRadius) &&
				o.authored_radius_q16 == o.radius_q16);
	}

	// The local player is never its own hitbox row.
	w.cached.local_player = a;
	mission::collect_debug_hitboxes(kernel, report);
	CHECK(report.organics.empty());

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("debug_oracles_test OK\n");
	return 0;
}
