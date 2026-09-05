// 01TR ("Training: Land Vehicles") on the retail data: the Drivable Dune
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
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 01TR.bms).
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

constexpr int kBuggyTypeId = 1291; // items.def 101291 "Drivable Dune Buggy"

const w::Entity *nearest_buggy(testrig::RetailMissionRig &rig, const w::Vec3 &from) {
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

const w::Entity *gun_child_of(testrig::RetailMissionRig &rig, w::EntityHandle carrier) {
	const w::Entity *found = nullptr;
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (found == nullptr && e.emplacement_parent == carrier) found = &e;
	});
	return found;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying 01TR.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "01TR.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "01TR boots")) {
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
			w::Vec3{bpos.x + 1.6f, bpos.y, bpos.z});
	rig.tick(31);
	if (!expect(rig.local.toggle_mount(), "USE mounts the buggy")) return 1;
	rig.tick(31);
	const w::Entity *pl = rig.local.player();
	if (!expect(pl != nullptr && pl->mounted, "the player is mounted after the toggle")) return 1;
	std::printf("buggy: mounted target=%s seat=%d type=%d\n",
			pl->mount_target == buggy_h ? "buggy" : pl->mount_target == gun_h ? "gun child" : "OTHER",
			int(pl->mount_seat), int(pl->mount_type));
	if (!expect(pl->mount_target == buggy_h || pl->mount_target == gun_h,
				"the toggle mounted the buggy family")) return 1;

	// --- Symptom 1: USE from the buggy family EXITS, never cycles onto the gun.
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
	if (!expect(rig.local.toggle_mount(), "USE remounts the buggy")) return 1;
	rig.tick(31);
	if (!expect(rig.local.select_numbered_seat(0) || rig.local.player()->mount_target == buggy_h,
				"seat key 1 takes the buggy's control seat")) return 1;
	rig.tick(31);
	pl = rig.local.player();
	if (!expect(pl->mounted && pl->mount_target == buggy_h, "the player drives the buggy")) return 1;
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
	std::printf("buggy_01tr: %s\n", failures == 0 ? "OK" : "FAILED");
	return failures == 0 ? 0 : 1;
}
