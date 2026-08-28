// D-VEH-1 regression on the retail data: the parked transport trucks on 00TRa
// rest their wheel-contact ORIGIN on the terrain under the authority vehicle
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
// Gated on OPENNOVA_JO_DIR (a retail install carrying 00TRa.bms).
#include "common/retail_mission_rig.h"
#include "common/retail_paths.h"

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
// 00TRa's three placed DTrucks (bms ids): 11 + 6 = "Drivable Transport Truck"
// (DTruck1; 11 is the truck right of the player spawn), 1714 = the armory
// truck (DTruck2). Spawn-area trucks first — the drop test picks the first.
constexpr uint16_t kTruckSsns[] = {11, 1714, 6};

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying 00TRa.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "00TRa.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "00TRa boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.has_terrain(), "the mission terrain loaded (the ground solve needs it)")) return 1;

	while (rig.world.logic_tick < static_cast<uint32_t>(kSettleTick)) rig.tick();

	for (const uint16_t ssn : kTruckSsns) {
		const w::Entity *veh = rig.by_net_id(ssn);
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
				unsigned(ssn), veh->handle.pool(), veh->position.x, veh->position.y, veh->position.z,
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
		const w::Entity *veh = rig.by_net_id(ssn);
		if (veh != nullptr && veh->alive && !veh->hidden && veh->handle.pool() == 1) {
			drop = veh;
			break;
		}
	}
	if (expect(drop != nullptr, "a live pool-1 truck exists for the drop test")) {
		const w::EntityHandle handle = drop->handle;
		const uint16_t ssn = drop->net_id;
		const w::Vec3 pos = drop->position;
		rig.set_entity_position(handle, w::Vec3{pos.x + 0.5f, pos.y, pos.z + 2.0f});
		rig.tick(310);
		const w::Entity *after = rig.world.registry.get(handle);
		if (expect(after != nullptr, "the lifted truck is still in the world")) {
			const float ground = rig.ground_height(after->position.x, after->position.y);
			const float rest_error = after->position.z - ground;
			std::printf("truck-rest: drop-test ssn=%u settled=%.3f ground=%.3f rest_error=%+.3f\n",
					unsigned(ssn), after->position.z, ground, rest_error);
			if (!expect(std::fabs(after->position.z - (pos.z + 2.0f)) >= 0.05f,
						"the drop test settled — the ground solve ran"))
				;
			else if (!expect(std::fabs(rest_error) <= kMaxRestError,
							 "the dropped truck rests its origin on the terrain") &&
					std::fabs(rest_error - kOldFloat) < 0.1f)
				std::printf("truck-rest: drop-test floats by the pre-D-VEH-1 wheel depth\n");
		}
	}

	if (failures == 0) std::printf("truck_rest_00tra: every placed DTruck rests its origin on the terrain\n");
	return failures == 0 ? 0 : 1;
}
