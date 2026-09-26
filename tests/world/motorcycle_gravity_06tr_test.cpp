// Regression for the airborne motorcycle in 06TR (Training: Motorcycle).
// The front-left instructor rides SSN 80; attaching the local player to its
// passenger seat starts the AI ride. When both wheel probes lose contact, the
// light solve must clear BYTE2(aiRef0) (m.grounded) as it sets Flags 0x2000,
// otherwise the mover rebuilds slide_z from the grounded forward row every
// tick and the occupied bike floats instead of accumulating gravity.
// Gated on OPENNOVA_JO_DIR (a retail install whose base mount carries
// 06TR.bms), else OPENNOVA_JO_ASSETS (the extracted tree, 06TR.bms loose at its
// root): JO:CA ships the mission in its jox01 archive, not the base set.
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using namespace opennova;
namespace w = opennova::world;

constexpr uint16_t kBikeSsn = 80;
constexpr uint16_t kInstructorSsn = 1715;
constexpr int kDriveWaitTicks = 600;
constexpr float kLift = 10.0f;

int failures = 0;

bool expect(bool cond, const char *msg) {
	if (cond)
		return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

} // namespace

int main() {
	const std::string install = retail::install();
	const std::string assets = retail::assets();
	if (install.empty() && assets.empty())
		return retail::skip("OPENNOVA_JO_DIR or OPENNOVA_JO_ASSETS (a retail JO install or extracted tree "
				"carrying 06TR.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	bool opened = !install.empty() && rig.open(install, "06TR.bms", error);
	if (!opened && !assets.empty())
		opened = rig.open(assets, "06TR.bms", error);
	if (!opened)
		return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "06TR boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's player spawned"))
		return 1;
	if (!expect(rig.has_terrain(), "06TR terrain loaded"))
		return 1;

	w::Entity *bike = rig.world.registry.by_net_id(kBikeSsn);
	if (!expect(bike != nullptr, "motorcycle SSN 80 is in the world"))
		return 1;
	const w::VehicleTraits *traits = rig.world.vehicles.traits.get(bike->item_id);
	if (!expect(traits != nullptr && traits->family == w::VehicleFamily::Bike,
				"SSN 80 uses the light motorcycle mover"))
		return 1;

	// The authored mount command may finish during the first few ticks. Pin the
	// exact instructor relationship before adding the local passenger.
	w::Entity *instructor = nullptr;
	for (int tick = 0; tick < 120; ++tick) {
		instructor = rig.world.registry.by_net_id(kInstructorSsn);
		bike = rig.world.registry.by_net_id(kBikeSsn);
		if (instructor != nullptr && bike != nullptr && instructor->mounted &&
				instructor->mount_target == bike->handle)
			break;
		rig.tick();
	}
	if (!expect(instructor != nullptr && instructor->mounted && bike != nullptr &&
						instructor->mount_target == bike->handle,
				"instructor SSN 1715 controls motorcycle SSN 80"))
		return 1;

	uint8_t passenger_bone = 0;
	bool passenger_found = false;
	for (const w::Seat &seat : bike->seats) {
		if (seat.type == w::SeatType::Passenger && !seat.occupant.valid()) {
			passenger_bone = seat.bone_index;
			passenger_found = true;
			break;
		}
	}
	if (!expect(passenger_found, "SSN 80 has a free passenger seat"))
		return 1;
	const w::EntityHandle player_handle = rig.world.cached.local_player;
	const w::EntityHandle bike_handle = bike->handle;
	if (!expect(rig.world.vehicles.process_attach(player_handle, bike_handle, passenger_bone),
				"the local player boards SSN 80 as passenger"))
		return 1;

	// Boarding triggers the training ride. Wait only until the real AI motor is
	// moving with confirmed wheel contact, then create a deterministic launch.
	bool moving = false;
	for (int tick = 0; tick < kDriveWaitTicks; ++tick) {
		rig.tick();
		bike = rig.world.registry.get(bike_handle);
		if (bike != nullptr && bike->veh.grounded && std::abs(bike->veh.speed) > 0x1000) {
			moving = true;
			break;
		}
	}
	if (!expect(moving, "the boarded instructor motorcycle starts driving"))
		return 1;

	const w::Vec3 launch = bike->position;
	if (!expect(rig.world.commands.set_entity_position(
						bike_handle, w::Vec3{ launch.x, launch.y, launch.z + kLift }),
				"the moving motorcycle is lifted clear of terrain"))
		return 1;
	rig.tick();
	bike = rig.world.registry.get(bike_handle);
	if (!expect(bike != nullptr, "the launched motorcycle remains in the world"))
		return 1;

	expect((bike->flags & w::kEntityFlagInAir) != 0,
			"both missed wheels set the motorcycle airborne flag");
	expect(!bike->veh.grounded, "both missed wheels clear the motorcycle contact byte");

	int32_t previous_slide = bike->veh.slide_z;
	for (int tick = 0; tick < 8; ++tick) {
		const int32_t expected_slide = std::min(previous_slide, int32_t{ 0x4000 }) - 250;
		rig.tick();
		bike = rig.world.registry.get(bike_handle);
		if (!expect(bike != nullptr, "the airborne motorcycle remains in the world"))
			return 1;
		expect(!bike->veh.grounded, "the airborne motorcycle stays off-contact");
		expect(bike->veh.slide_z == expected_slide,
				"the airborne motorcycle accumulates one gravity step per tick");
		previous_slide = bike->veh.slide_z;
	}

	const w::Entity *player = rig.world.registry.get(player_handle);
	instructor = rig.world.registry.by_net_id(kInstructorSsn);
	expect(player != nullptr && player->mounted && player->mount_target == bike_handle,
			"the passenger stays attached during the launch");
	expect(instructor != nullptr && instructor->mounted && instructor->mount_target == bike_handle,
			"the instructor stays attached during the launch");

	std::printf("motorcycle_gravity_06tr: ssn=%u launch_z=%.3f airborne_z=%.3f slide_z=%d "
				"grounded=%d\n",
			unsigned(kBikeSsn), launch.z + kLift, bike->position.z, bike->veh.slide_z,
			int(bike->veh.grounded));
	return failures == 0 ? 0 : 1;
}
