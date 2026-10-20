// D-VEH-1 regression: the parked transport trucks of the shipped 00TRa rest
// their wheel-contact ORIGIN on the terrain under the authority vehicle
// pass (the same pass a player's SP listen server runs). Before the witnessed
// probe-box provenance landed (the CMDL header bbox Z pair,
// docs/world/vehicle-client-movers-re.md §3) the per-COBJ AABB-union stand-in
// floated every DTruck by its below-origin wheel depth (~0.33 u): the
// user-visible floating wheels right of the 00TRa spawn.
//
// Two legs: the parked pose after the settle window, and a discriminating drop
// test that lifts one LIVE truck 2 u (with a planar nudge — the solve's sleep
// fast-path compares the pose planar-only, vehicle-client-movers-re.md §7) and
// requires the ground solve to settle it BACK to the wheel-contact rest.
// The mission is the mission set's truck_rest.bms (tests/fixtures/mission_set_gen.cpp): 00TRa's
// three trucks by their SSNs around the single-player start, the minted mtruck in the shipped
// DTruck1's shape (its CMDL floor 0.01 over the origin), on the minted Tmap terrain.
#include "common/mission_set.h"
#include "common/mission_rig.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kSettleTick = 620;
// The CMDL floor sits at +0.01 for DTruck1/2, so the settled origin lands at
// ground - 0.01; modest slack for slope under the wheelbase (the solve
// averages four pad corners).
constexpr float kMaxRestError = 0.15f;
// The union stand-in floated the hull ~0.334 u — anything near that is the
// regression this test exists to catch.
constexpr float kOldFloat = 0.334f;
// 00TRa's three placed trucks (bms ids): 11 + 6 = "Drivable Transport Truck"
// (11 is the truck right of the player spawn), 1714 = the armory truck.
// Spawn-area trucks first — the drop test picks the first.
constexpr uint16_t kTruckSsns[] = {11, 1714, 6};

} // namespace

int main() {
	test_mission_set::Root root("truck_rest");
	if (!expect(root.ok(), "the mission set stages")) return 1;
	testrig::MissionRig rig;
	std::string error;
	if (!expect(rig.open(root.path(), "truck_rest.bms", error), "truck_rest.bms opens")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "the mission boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.has_terrain(), "the mission terrain loaded (the ground solve needs it)")) return 1;

	while (rig.world.logic_tick < static_cast<uint32_t>(kSettleTick)) rig.tick();

	for (const uint16_t ssn : kTruckSsns) {
		const w::Entity *veh = rig.world.registry.by_net_id(ssn);
		char msg[96];
		std::snprintf(msg, sizeof(msg), "ssn %u is in the world", unsigned(ssn));
		if (!expect(veh != nullptr, msg)) continue;
		const float ground = rig.ground_height(veh->position.x, veh->position.y);
		if (std::isnan(ground)) {
			std::printf("truck-rest: ssn=%u off-terrain (skipped)\n", unsigned(ssn));
			continue;
		}
		const float rest_error = veh->position.z - ground;
		const bool ok = std::fabs(rest_error) <= kMaxRestError;
		std::printf("truck-rest: ssn=%u pool=%d pos=(%.3f, %.3f, %.3f) ground=%.3f rest_error=%+.3f alive=%d hidden=%d %s\n",
				unsigned(ssn), veh->handle.pool(), float(veh->position.x), float(veh->position.y), float(veh->position.z),
				ground, rest_error, int(veh->alive), int(veh->hidden), ok ? "OK" : "FLOATING");
		std::snprintf(msg, sizeof(msg), "ssn %u rests its origin on the terrain", unsigned(ssn));
		if (!expect(ok, msg) && std::fabs(rest_error - kOldFloat) < 0.1f)
			std::printf("truck-rest: ssn=%u floats by the pre-D-VEH-1 wheel depth\n", unsigned(ssn));
	}

	// The drop test on the first LIVE pool-1 truck: the authority vehicle pass
	// walks pool-1 rows; a static-kind placement (pool 2) never runs a motor —
	// in retail either.
	const w::Entity *drop = nullptr;
	for (const uint16_t ssn : kTruckSsns) {
		const w::Entity *veh = rig.world.registry.by_net_id(ssn);
		if (veh != nullptr && veh->alive && !veh->hidden && veh->handle.pool() == 1) {
			drop = veh;
			break;
		}
	}
	if (expect(drop != nullptr, "a live pool-1 truck exists for the drop test")) {
		const w::EntityHandle handle = drop->handle;
		const uint16_t ssn = drop->net_id;
		const w::Vec3 pos = drop->position;
		rig.world.commands.set_entity_position(handle, w::Vec3{pos.x + 0.5f, pos.y, pos.z + 2.0f});
		rig.tick(310);
		const w::Entity *after = rig.world.registry.get(handle);
		if (expect(after != nullptr, "the lifted truck is still in the world")) {
			const float ground = rig.ground_height(after->position.x, after->position.y);
			const float rest_error = after->position.z - ground;
			std::printf("truck-rest: drop-test ssn=%u settled=%.3f ground=%.3f rest_error=%+.3f\n",
					unsigned(ssn), float(after->position.z), ground, rest_error);
			if (!expect(std::fabs(after->position.z - (pos.z + 2.0f)) >= 0.05f,
						"the drop test settled — the ground solve ran"))
				;
			else if (!expect(std::fabs(rest_error) <= kMaxRestError,
							 "the dropped truck rests its origin on the terrain") &&
					std::fabs(rest_error - kOldFloat) < 0.1f)
				std::printf("truck-rest: drop-test floats by the pre-D-VEH-1 wheel depth\n");
		}

		// The hard fall: dropped from 40 u, the truck's fall passes the authority's
		// crash bound (|vz| > 0x7000) in the air; the request latches the crash at
		// the next suspension fit, and the hull comes down a wreck.
		// [orig: Entity_ProcessTrackedVehiclePhysics -- the hard-fall request
		//  @0x47d771..0x47d7a8, the latch Entity_ProcessWheeledVehicleSuspension
		//  @0x46b1a6..0x46b314]
		const w::Vec3 rest = rig.world.registry.get(handle)->position;
		rig.world.commands.set_entity_position(handle, w::Vec3{rest.x, rest.y, rest.z + 40.0f});
		rig.tick(310);
		const w::Entity *fallen = rig.world.registry.get(handle);
		if (expect(fallen != nullptr, "the dropped truck is still in the world")) {
			std::printf("truck-rest: hard fall ssn=%u z %.3f -> %.3f crashed=%d\n", unsigned(ssn), rest.z + 40.0f,
					float(fallen->position.z), int(fallen->veh.crashed));
			expect(fallen->position.z < rest.z + 5.0f, "the dropped truck came down");
			expect(fallen->veh.crashed != 0, "a 40 u fall latches the truck's crash");
		}
	}

	if (failures == 0) std::printf("truck_rest_00tra: every placed DTruck rests its origin on the terrain\n");
	return failures == 0 ? 0 : 1;
}
