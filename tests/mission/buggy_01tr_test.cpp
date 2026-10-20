// 01TR ("Training: Land Vehicles") in miniature (the mission set's land_vehicles.bms:
// the minted mbuggy in the shipped Dbuggy1's shape, its gun child the synthetic mount
// in the shipped B50Cal's articulation): the Drivable Dune
// Buggy (items.def 101291) parked by the spawn carries an addeweap gun child
// (ewep01 -> 101419). Two USE-ITEM facts the retail engine holds:
//
//   * from the buggy's own seat, USE EXITS. The nearest-seat scan never offers
//     the rider's own carrier family (the carrier and the EWeap children riding
//     it): retail's LOS leg walks the hull, and an EWeap candidate on a vehicle
//     resolves its LOS target to that carrier [orig: Entity_FindNearestSeatOrArmory
//     @0x43615c..0x436183], so a seated rider never sees its own gun as free.
//     Before the fix our scan skipped only the carrier itself, so USE cycled
//     driver -> gun -> driver forever.
//   * the gun child rides the driven buggy (pose_emplacement_attachments over
//     the authored userpoint), so it never falls behind a moving carrier.
//
// And a wall-contact fact: the motorcycle parked west of the start (SSN 311)
// driven head-on at the boulder there (SSN 310) stops against the rock.
//
#include "common/mission_rig.h"
#include "common/mission_set.h"
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/player_weapon.h>

#include <algorithm>
#include <cstdlib>
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

constexpr int kBuggyTypeId = 1291; // items.def 101291 "Drivable Dune Buggy"

const w::Entity *nearest_buggy(testrig::MissionRig &rig, const w::Vec3 &from) {
	const w::Entity *best = nullptr;
	float best_d = 1e30f;
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (e.handle.pool() == 0 || e.item_id != kBuggyTypeId || !e.alive) return;
		const float d = testrig::distance(e.position, from);
		if (d < best_d) {
			best_d = d;
			best = &e;
		}
	});
	return best;
}

const w::Entity *gun_child_of(testrig::MissionRig &rig, w::EntityHandle carrier) {
	const w::Entity *found = nullptr;
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (found == nullptr && e.emplacement_parent == carrier) found = &e;
	});
	return found;
}

constexpr uint16_t kBoulderSsn = 310;   // land_vehicles.bms's rock formation
constexpr uint16_t kMotorcycleSsn = 311; // land_vehicles.bms's parked motorcycle
constexpr float kBoulderRadius = 2.3f;   // the minted rock's upper ring (mrock.3di)

// One drive at the boulder: the driven vehicle set down `back` units short of the rock's centre
// along `heading` (a planar unit vector, the way its nose points), then held on forward for
// `ticks`. `start`/`end` are the hull's distance along the heading from the rock's centre
// (negative: short of it); `closest` the nearest its centre came to the rock's; `last_second` how
// far it moved over the run's last 62 ticks, the throttle still held.
struct BoulderRun {
	float start = 0.0f, end = 0.0f, closest = 0.0f, last_second = 0.0f;
};

BoulderRun drive_at_boulder(testrig::MissionRig &rig, w::EntityHandle vehicle, const w::Vec3 &heading, float back,
		int ticks) {
	BoulderRun run;
	const w::Vec3 rock = rig.world.registry.by_net_id(kBoulderSsn)->position;
	const auto along = [&](const w::Vec3 &p) { return (p.x - rock.x) * heading.x + (p.y - rock.y) * heading.y; };
	rig.world.commands.set_entity_position(vehicle,
			w::Vec3{rock.x - heading.x * back, rock.y - heading.y * back, rock.z + 1.5f});
	rig.tick(62);
	run.start = along(rig.world.registry.get(vehicle)->position);
	run.closest = 1.0e30f;
	rig.local.input.forward = true;
	w::Vec3 second_before_end{};
	for (int t = 0; t < ticks; ++t) {
		if (t == ticks - 62) second_before_end = rig.world.registry.get(vehicle)->position;
		rig.tick();
		run.closest = std::min(run.closest, testrig::planar_distance(rig.world.registry.get(vehicle)->position, rock));
	}
	rig.local.input.forward = false;
	run.end = along(rig.world.registry.get(vehicle)->position);
	run.last_second = testrig::planar_distance(rig.world.registry.get(vehicle)->position, second_before_end);
	return run;
}

// The planar unit vector from `a` to `b`.
w::Vec3 planar_heading(const w::Vec3 &a, const w::Vec3 &b) {
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float len = std::sqrt(dx * dx + dy * dy);
	return len > 0.0f ? w::Vec3{dx / len, dy / len, 0.0f} : w::Vec3{1.0f, 0.0f, 0.0f};
}

} // namespace

int main() {
	test_mission_set::Root root("buggy");
	if (!expect(root.ok(), "the mission set stages")) return 1;
	testrig::MissionRig rig;
	std::string error;
	if (!expect(rig.open(root.path(), "land_vehicles.bms", error), "land_vehicles.bms opens")) {
		std::fprintf(stderr, "  %s", error.c_str());
		return 1;
	}
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "the mission boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	rig.install_weapon("WPN_M4AUTO");
	rig.tick(62);

	const w::Vec3 spawn = rig.local.player_position();
	const w::Entity *buggy = nearest_buggy(rig, spawn);
	if (!expect(buggy != nullptr, "a Drivable Dune Buggy is in the world")) return 1;
	const w::EntityHandle buggy_h = buggy->handle;
	const w::Entity *gun = gun_child_of(rig, buggy_h);
	std::printf("buggy: ssn=%u at %.1fu from spawn, seats=%zu, gun child=%s\n", unsigned(buggy->net_id),
			testrig::distance(buggy->position, spawn), buggy->seats.size(), gun != nullptr ? "yes" : "NO");
	if (!expect(!buggy->seats.empty(), "the buggy has seats (seat specs fed)")) return 1;
	if (!expect(gun != nullptr, "the buggy's addeweap gun child was promoted")) return 1;
	const w::EntityHandle gun_h = gun->handle;

	// Stand beside the buggy and mount it.
	const w::Vec3 bpos = rig.world.registry.get(buggy_h)->position;
	rig.world.commands.set_entity_position(rig.world.cached.local_player,
			w::Vec3{ bpos.x, bpos.y, bpos.z + buggy->bound_radius + 1.0f });
	rig.tick(31);
	// Query beside the live (settled) hull rather than the earlier spawn pose.
	const w::Vec3 live = rig.world.registry.get(buggy_h)->position;
	rig.world.commands.set_entity_position(
			rig.world.cached.local_player, w::Vec3{ live.x - 1.0f, live.y, live.z + 1.0f });
	rig.world.collision->build_initial_tables(rig.world);
	rig.local.player()->ground_target = {};
	if (!expect(rig.local.toggle_mount(), "USE mounts the buggy")) return 1;
	rig.tick(31);
	const w::Entity *pl = rig.local.player();
	if (!expect(pl != nullptr && pl->mounted, "the player is mounted after the toggle")) return 1;
	std::printf("buggy: mounted target=%s seat=%d type=%d\n",
			pl->mount_target == buggy_h ? "buggy" : pl->mount_target == gun_h ? "gun child" : "OTHER",
			int(pl->mount_seat), int(pl->mount_type));
	if (!expect(pl->mount_target == buggy_h || pl->mount_target == gun_h,
				"the toggle mounted the buggy family")) return 1;

	// The scan can choose the mounted gun, whose equip animation must finish
	// before USE is accepted. Exercise the same live idle/overheat gate.
	for (int i = 0; i < 250; ++i) {
		const auto *slot = w::active_local_weapon_slot(rig.world, rig.local.weapon);
		if (!slot || w::weapon_state_allows_mount_toggle(slot->current, slot->next))
			break;
		rig.tick();
	}
	// --- Symptom 1: USE from the buggy family exits through the hull query.
	if (!expect(rig.local.toggle_mount(), "the second USE is accepted")) return 1;
	rig.tick(31);
	pl = rig.local.player();
	std::printf("buggy: after second USE mounted=%d target=%s\n", int(pl->mounted),
			!pl->mounted ? "-" : pl->mount_target == buggy_h ? "buggy" : pl->mount_target == gun_h ? "gun child" : "OTHER");
	if (!expect(!pl->mounted, "USE from the buggy dismounts instead of swapping onto its own gun")) {
		// Keep going so the drive leg still reports.
		rig.local.toggle_mount();
		rig.tick(31);
		if (rig.local.player()->mounted) {
			std::printf("buggy: still mounted after a third USE; forcing a detach for the drive leg\n");
			rig.world.vehicles.detach(rig.world.cached.local_player);
			rig.tick(19);
		}
	}

	// --- Symptom 2: drive the buggy from its control seat; the gun child rides along.
	const w::Vec3 here = rig.local.player_position();
	rig.world.commands.set_entity_position(buggy_h, w::Vec3{here.x + 1.6f, here.y, here.z});
	rig.tick(19);
	const w::Vec3 remount = rig.world.registry.get(buggy_h)->position;
	rig.world.commands.set_entity_position(rig.world.cached.local_player,
			w::Vec3{ remount.x - 1.0f, remount.y, remount.z + 1.0f });
	rig.world.collision->build_initial_tables(rig.world);
	if (!expect(rig.local.toggle_mount(), "USE remounts the buggy")) return 1;
	rig.tick(31);
	if (!expect(rig.local.select_numbered_seat(0) || rig.local.player()->mount_target == buggy_h,
				"seat key 1 takes the buggy's control seat")) return 1;
	rig.tick(31);
	pl = rig.local.player();
	if (!expect(pl->mounted && pl->mount_target == buggy_h, "the player drives the buggy")) return 1;
	// Seat key 2 moves the driver off the controller to the family's next seat, and seat
	// key 1 takes the controller back. [orig: Entity_FindAvailableSeat @0x4368B0 ->
	//  Entity_RequestVehicleAttach @0x4365DD]
	{
		rig.tick(62);
		const w::Entity *driver = rig.local.player();
		const w::EntityHandle seat_target = driver->mount_target;
		const int seat = int(driver->mount_seat);
		expect(rig.local.select_numbered_seat(1), "seat key 2 moves the driver to the next seat");
		rig.tick(62);
		driver = rig.local.player();
		expect(driver->mounted && (driver->mount_target != seat_target || int(driver->mount_seat) != seat),
				"the driver sits in another seat of the buggy family");
		for (int i = 0; i < 250; ++i) {
			const auto *slot = w::active_local_weapon_slot(rig.world, rig.local.weapon);
			if (!slot || w::weapon_state_allows_mount_toggle(slot->current, slot->next)) break;
			rig.tick();
		}
		expect(rig.local.select_numbered_seat(0), "seat key 1 takes the controller back");
		rig.tick(62);
		driver = rig.local.player();
		if (!expect(driver->mounted && driver->mount_target == buggy_h && driver->mount_type == w::SeatType::Controller,
					"the player drives the buggy again")) return 1;
	}
	pl = rig.local.player();
	const w::Vec3 b0 = rig.world.registry.get(buggy_h)->position;
	const w::Vec3 g0 = rig.world.registry.get(gun_h)->position;
	rig.local.input.forward = true;
	rig.tick(62 * 4);
	rig.local.input.forward = false;
	const w::Entity *buggy_now = rig.world.registry.get(buggy_h);
	const w::Entity *gun_now = rig.world.registry.get(gun_h);
	if (!expect(buggy_now != nullptr && gun_now != nullptr, "the buggy and its gun child survive the drive")) return 1;
	const float drove = testrig::distance(buggy_now->position, b0);
	const float gun_moved = testrig::distance(gun_now->position, g0);
	const float gun_gap = testrig::distance(gun_now->position, buggy_now->position);
	std::printf("buggy: drove %.1fu; gun child moved %.1fu, gap to buggy %.1fu, alive=%d hidden=%d\n", drove,
			gun_moved, gun_gap, int(gun_now->alive), int(gun_now->hidden));
	if (!expect(drove >= 2.0f, "forward input drives the buggy")) return 1;
	expect(gun_gap <= 6.0f, "the gun child stays on the driven buggy");
	expect(gun_moved >= drove - 6.0f, "the gun child moved with the buggy");
	expect(gun_now->alive && !gun_now->hidden, "the gun child is still live and visible");

	// --- Wall contact: the motorcycle driven head-on at the boulder stops against it. The light
	// solve's probes meet the rock's flank as wall contacts, the planar push and its second probe
	// pass move the hull back out each tick, so it never rides into or through the rock.
	// [orig: Entity_ProcessLightVehiclePhysics @0x479600 -- the five-probe push @0x47A0AF..0x47A0BE]
	rig.world.vehicles.detach(rig.world.cached.local_player);
	rig.tick(19);
	const w::Entity *bike = rig.world.registry.by_net_id(kMotorcycleSsn);
	if (!expect(bike != nullptr, "the motorcycle SSN 311 is in the world")) return 1;
	const w::EntityHandle bike_h = bike->handle;
	const w::Vec3 bike_at = bike->position;
	rig.world.commands.set_entity_position(
			rig.world.cached.local_player, w::Vec3{ bike_at.x - 1.0f, bike_at.y, bike_at.z + 1.0f });
	rig.world.collision->build_initial_tables(rig.world);
	rig.local.player()->ground_target = {};
	if (!expect(rig.local.toggle_mount(), "USE mounts the motorcycle")) return 1;
	rig.tick(31);
	if (!expect(rig.local.select_numbered_seat(0) || rig.local.player()->mount_target == bike_h,
				"seat key 1 takes the motorcycle's control seat")) return 1;
	rig.tick(31);
	pl = rig.local.player();
	if (!expect(pl->mounted && pl->mount_target == bike_h, "the player rides the motorcycle")) return 1;
	const w::Vec3 m0 = rig.world.registry.get(bike_h)->position;
	rig.local.input.forward = true;
	rig.tick(62);
	rig.local.input.forward = false;
	const w::Vec3 bike_heading = planar_heading(m0, rig.world.registry.get(bike_h)->position);
	rig.tick(124); // coast to a stop before the run
	const BoulderRun bike_run = drive_at_boulder(rig, bike_h, bike_heading, 12.0f, 62 * 5);
	std::printf("motorcycle: at the boulder from %.2f to %.2f along its heading, closest %.2fu, %.2fu in the last second\n",
			bike_run.start, bike_run.end, bike_run.closest, bike_run.last_second);
	expect(bike_run.end - bike_run.start >= 4.0f, "the motorcycle drives up to the boulder");
	expect(bike_run.last_second < 0.5f, "the motorcycle is held at the boulder, its throttle still open");
	expect(bike_run.closest >= kBoulderRadius, "the motorcycle never enters the boulder");
	expect(bike_run.end < 0.0f, "the motorcycle stops short of the boulder, not past it");

	// --- Leaning: steered hard at speed, the motorcycle banks into the turn.
	// [orig: Entity_SmoothHeadingToTarget @0x45B2C0 -- the bike lean, its caller
	//  Entity_ProcessLightVehiclePhysics @0x479600 site @0x47A7B1..0x47A7D3]
	{
		const w::Vec3 rock = rig.world.registry.by_net_id(kBoulderSsn)->position;
		rig.world.commands.set_entity_position(bike_h,
				w::Vec3{rock.x - bike_heading.x * 60.0f, rock.y - bike_heading.y * 60.0f, rock.z + 1.5f});
		rig.tick(62);
		rig.local.input.forward = true;
		rig.tick(124);
		rig.local.input.left = true;
		int32_t banked = 0;
		for (int t = 0; t < 62; ++t) {
			rig.tick();
			banked = std::max(banked, std::abs(rig.world.registry.get(bike_h)->veh.air_roll_bam));
		}
		rig.local.input.left = false;
		rig.local.input.forward = false;
		std::printf("motorcycle: banked to %d BAM steering at speed\n", banked);
		expect(banked > 0, "the motorcycle banks into a turn taken at speed");
	}
	std::printf("buggy_01tr: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
