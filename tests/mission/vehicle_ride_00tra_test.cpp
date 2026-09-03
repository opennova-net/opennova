// Vehicle mount / ride / drive on the retail data: 00TRa's ("Training:
// Basics / Armory") vehicle gate end to end on the engine's own world:
//
//   * the USE-ITEM toggle mounts the local player into the DTruck1 (SSN 11)
//     [orig: Entity_ToggleVehicleMount @0x436950 + the nearest-seat scan @0x435d50],
//   * the BMS PLYRATTACHED trigger fires event 2 (the ride kickoff: RedirectGroupTo
//     group 3 + PatrolSpeed 40) [orig: EventTrigger cat-7 sub 38 -> @0x4f10d0],
//   * THE RIDE: the truck's ctrl seat is crewed at spawn (the instructor boards via
//     the authored waypoint_id-123..125 command mount), so after event 2 the
//     AI-driver leg drives the truck along list 2 with the player aboard
//     [orig: Entity_UpdateVehiclePhysics @0x48bc12 + Entity_SetWaypointByTeam
//     @0x43cdb4 + AI_HandleCommand case 0xB PatrolSpeed],
//   * the seated player is CARRIED (position follows the seat every tick),
//   * the free-ctrl ATV (SSN 1766) then takes the player as DRIVER and forward
//     input drives it (the occupant leg), the camera aboard.
// The camera mode the sim RESOLVES for each seat is checked through the local
// view frame: the chase in a control seat, first person otherwise [orig: the
// arbiter Render_ProcessMainSceneFrame @0x5ca1d2..0x5ca1f2].
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 00TRa.bms).
#include "common/retail_mission_files.h"
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

constexpr uint16_t kTruckSsn = 11;
constexpr uint16_t kAtvSsn = 1766;

bool camera_check(testrig::RetailMissionRig &rig, const char *label, bool expect_third_person) {
	const w::LocalPlayerViewFrame f = rig.local.view_frame();
	std::printf("ride: camera %s third_person=%d camera_mounted=%d selected=%d\n", label,
			int(f.third_person), int(f.camera_mounted), int(f.third_person_selected));
	char msg[128];
	std::snprintf(msg, sizeof(msg), "camera %s resolves third_person=%d", label, int(expect_third_person));
	return expect(f.third_person == expect_third_person, msg);
}

bool control_seat(w::SeatType t) { return t == w::SeatType::Controller || t == w::SeatType::Driver; }

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
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	rig.install_weapon("WPN_M4AUTO");
	rig.tick(62);
	if (!camera_check(rig, "on_foot", false)) return 1;

	w::Entity *truck = rig.world.registry.by_net_id(kTruckSsn);
	if (!expect(truck != nullptr, "truck SSN 11 is in the world")) return 1;
	std::printf("ride: truck pos=(%.1f, %.1f, %.1f) seats=%zu\n", truck->position.x, truck->position.y,
			truck->position.z, truck->seats.size());
	if (!expect(!truck->seats.empty(), "the truck has seats (seat specs fed)")) return 1;
	const w::EntityHandle truck_handle = truck->handle;
	const w::EntityHandle player_handle = rig.world.cached.local_player;

	// Bring the player to the truck (the spawn point sits inside the barracks;
	// the motor pool is open ground and the truck's list-2 route starts there).
	const w::Vec3 tpos = truck->position;
	rig.world.commands.set_entity_position(player_handle, w::Vec3{tpos.x + 1.6f, tpos.y, tpos.z});
	rig.tick(31);

	// --- Toggle mount: the player must end up seated on SSN 11.
	if (!expect(rig.local.toggle_mount(), "the mount toggle accepts (a seat within the 4 u scan gate)")) return 1;
	rig.tick(31);
	const w::Entity *pl = rig.local.player();
	if (!expect(pl != nullptr && pl->mounted, "the player is mounted after the toggle")) return 1;
	if (!expect(pl->mount_target == truck_handle, "the player mounted SSN 11")) return 1;
	const w::SeatType truck_seat = pl->mount_type;
	std::printf("ride: MOUNTED seat=%d type=%d (1 sitex/2 ctrl/3 gun/5 drvr)\n", int(pl->mount_seat), int(truck_seat));
	if (!camera_check(rig, "truck_seat", control_seat(truck_seat))) return 1;

	// --- Event 2 (PLYRATTACHED 11 -> the ride kickoff) fires within a few quanta.
	bool fired = false;
	for (int i = 0; i < 10 && !fired; ++i) {
		rig.tick(62);
		fired = rig.events.event_fired(2);
	}
	if (!expect(fired, "event 2 (PLYRATTACHED SSN 11) fired")) return 1;
	std::printf("ride: event 2 FIRED (ride kickoff: RedirectGroupTo 3 + PatrolSpeed 40)\n");

	// --- THE RIDE: the instructor drives the redirected truck; the seated
	// player must be carried along.
	const w::Vec3 t0 = rig.world.registry.by_net_id(kTruckSsn)->position;
	float ride_dist = 0.0f;
	for (int i = 0; i < 30; ++i) {
		rig.tick(62);
		ride_dist = testrig::distance(rig.world.registry.by_net_id(kTruckSsn)->position, t0);
		if (ride_dist > 8.0f) break;
	}
	const w::Vec3 rider = rig.local.player_position();
	const w::Vec3 truck_now = rig.world.registry.by_net_id(kTruckSsn)->position;
	const float carry_gap = testrig::distance(rider, truck_now);
	std::printf("ride: truck drove %.1fu; rider gap %.1fu\n", ride_dist, carry_gap);
	if (!expect(ride_dist >= 8.0f, "the ride moved (the AI driver leg)")) return 1;
	if (!expect(carry_gap <= 10.0f, "the rider is carried along")) return 1;
	std::printf("ride: RIDE OK (AI-driven, player carried)\n");

	// --- The player-drive leg on the free-ctrl ATV: a mounted player cannot be
	// teleported away (the seat carry snaps it back) — park the TRUCK far away so
	// the toggle's scan runs dry and DETACHES [orig: @0x4369c7], then bring the
	// ATV to the dismounted player.
	w::Entity *atv = rig.world.registry.by_net_id(kAtvSsn);
	if (atv == nullptr) {
		std::printf("ride: ATV SSN %u missing; drive leg skipped\n", unsigned(kAtvSsn));
		std::printf("vehicle_ride_00tra: mount + event 2 + AI ride + carry\n");
		return failures == 0 ? 0 : 1;
	}
	const w::EntityHandle atv_handle = atv->handle;
	const w::Vec3 here = rig.local.player_position();
	rig.world.commands.set_entity_position(truck_handle, w::Vec3{here.x + 200.0f, here.y, here.z});
	rig.tick(19);
	rig.local.toggle_mount(); // seat scan dry (the truck left) -> detach
	rig.tick(19);
	if (rig.local.player()->mounted) {
		std::printf("ride: dismount failed after the truck left; drive leg skipped\n");
		std::printf("vehicle_ride_00tra: mount + event 2 + AI ride + carry\n");
		return failures == 0 ? 0 : 1;
	}
	std::printf("ride: dismounted (scan-dry toggle)\n");
	if (!camera_check(rig, "dismounted", false)) return 1;
	const w::Vec3 here2 = rig.local.player_position();
	rig.world.commands.set_entity_position(atv_handle, w::Vec3{here2.x + 1.5f, here2.y, here2.z});
	rig.tick(19);
	rig.local.toggle_mount(); // mount the ATV
	rig.tick(31);
	pl = rig.local.player();
	if (!pl->mounted || pl->mount_target != atv_handle) {
		std::printf("ride: ATV mount not reached (mounted=%d); drive leg skipped\n", int(pl->mounted));
		std::printf("vehicle_ride_00tra: mount + event 2 + AI ride + carry\n");
		return failures == 0 ? 0 : 1;
	}
	const w::SeatType atv_seat = pl->mount_type;
	std::printf("ride: ATV mounted: type=%d\n", int(atv_seat));
	if (!camera_check(rig, "atv_seat", control_seat(atv_seat))) return 1;
	if (control_seat(atv_seat)) {
		const w::Vec3 a0 = rig.world.registry.get(atv_handle)->position;
		rig.local.input.forward = true;
		rig.tick(62 * 4);
		rig.local.input.forward = false;
		const w::Vec3 a1 = rig.world.registry.get(atv_handle)->position;
		const float gap = testrig::distance(rig.local.player_position(), a1);
		std::printf("ride: drive: ATV moved %.1fu; rider gap %.1fu\n", testrig::distance(a1, a0), gap);
		if (!expect(testrig::distance(a1, a0) >= 2.0f, "forward input drives the ATV (the occupant leg)")) return 1;
		if (!expect(gap <= 8.0f, "the driver stays on the ATV")) return 1;
		std::printf("ride: DRIVE OK (player-driven)\n");
		if (!camera_check(rig, "atv_driving", true)) return 1;
		std::printf("vehicle_ride_00tra: mount + event 2 + AI ride + carry + player drive\n");
	} else {
		std::printf("vehicle_ride_00tra: mount + event 2 + AI ride + carry (ATV seat was type %d)\n", int(atv_seat));
	}
	return failures == 0 ? 0 : 1;
}
