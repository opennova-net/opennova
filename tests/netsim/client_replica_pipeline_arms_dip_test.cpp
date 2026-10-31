// The REMOTE ARMS DIP — a pure client's only feedback that a peer reloaded.
//
// Retail's reload broadcast does NOT put a peer into the 65/66 reload pose on a client.
// NapiNPClientMsg_WeaponReload_0x049 branches on the addressed entity's item type and, for
// a remote PERSON, stamps the dip window and returns without ever touching the +0x372
// window the pose selector reads [orig: @0x42c0a0 — type test @0x42c105, stamp
// entity+0x371 = 80 @0x42c10b, early return @0x42c113; the pose window is written only by
// WeaponSlot_ReloadAmmo @0x54173c, which that branch never reaches]. Only a HOST runs the
// refill on its own copy of the peer @0x514f03, which is why the reload CLIP is visible
// when we host and never when we merely join.
//
// So this pins the dip, not the clip. The arithmetic that matters:
//   * the window decrements in BOTH branches — twice per tick — so an 80 stamp dips for
//     40 ticks, not 80 [orig: @0x4b5cb5 and @0x4b5cdb..0x4b5ce7],
//   * the decay term drops a whole 0x2800000 BEFORE the eighth-step ease each tick the
//     window is open [orig: @0x4b5cb7 += 0xFD800000, then @0x4b5cc7..0x4b5cd5],
//   * the ease keeps running once the window closes, so the term returns toward zero.
//
// Asset-free and wire-free: it drives ClientReplicaPipeline::tick_arms_dip() directly.

#include <runtime/replication/client_replica_pipeline.h>

#include <cstdint>
#include <cstdio>
#include "../common/test_expect.h"
#include "../common/title_fixture.h"

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;

using test_expect::expect;

// A view holding one row of each class, so the person filter can be exercised.
ns::ClientReplicaPipeline make_view() {
	ns::ClientReplicaPipeline view([](uint16_t) { return nw::EntityClass::Player; });
	test_titles::jo_folds(view);
	ns::ClientEntityState &player = view.state().upsert(0x0001);
	player.cls = nw::EntityClass::Player;
	ns::ClientEntityState &infantry = view.state().upsert(0x0002);
	infantry.cls = nw::EntityClass::Infantry;
	ns::ClientEntityState &vehicle = view.state().upsert(0x1003);
	vehicle.cls = nw::EntityClass::Vehicle;
	return view;
}


bool test_window_lasts_forty_ticks() {
	ns::ClientReplicaPipeline view = make_view();
	view.state().find(0x0001)->arms_dip_ticks = 80;

	for (int tick = 0; tick < 39; ++tick) view.tick_arms_dip();
	if (!expect(view.state().find(0x0001)->arms_dip_ticks == 2,
	            "39 ticks of a two-per-tick decrement leave 2 on an 80 stamp"))
		return false;

	view.tick_arms_dip();
	if (!expect(view.state().find(0x0001)->arms_dip_ticks == 0,
	            "the 80-tick stamp closes on tick 40, not tick 80"))
		return false;

	// It must not run negative once closed.
	for (int tick = 0; tick < 10; ++tick) view.tick_arms_dip();
	return expect(view.state().find(0x0001)->arms_dip_ticks == 0,
	              "a closed window stays at zero");
}


bool test_decay_dips_then_recovers() {
	ns::ClientReplicaPipeline view = make_view();
	view.state().find(0x0001)->arms_dip_ticks = 80;

	// Each open tick subtracts a whole step and then eases by (v+4)>>3, so the term
	// runs steadily negative and approaches the fixed point where the ease cancels the
	// step: -0x2800000 == (v+4)>>3, i.e. v ~= -8 * 0x2800000.
	int32_t previous = 0;
	for (int tick = 0; tick < 40; ++tick) {
		view.tick_arms_dip();
		const int32_t now = view.state().find(0x0001)->pitch_kick_accum;
		if (!expect(now < previous, "the decay term falls monotonically while the window is open"))
			return false;
		previous = now;
	}
	const int32_t floor_value = view.state().find(0x0001)->pitch_kick_accum;
	if (!expect(floor_value < -0x11000000 && floor_value > -0x15000000,
	            "the dip settles near the analytic fixed point (~-8 * 0x2800000)"))
		return false;

	// Window closed: the ease alone pulls it back toward level.
	for (int tick = 0; tick < 200; ++tick) view.tick_arms_dip();
	const int32_t rested = view.state().find(0x0001)->pitch_kick_accum;
	return expect(rested > floor_value && rested <= 0 && rested > -0x10000,
	              "the term recovers toward zero once the window closes");
}


bool test_only_persons_dip() {
	ns::ClientReplicaPipeline view = make_view();
	// A stamp on a vehicle row must not integrate — retail's remote branch is gated on
	// ItemType_Person, for which our decoded rows carry Player/Infantry.
	view.state().find(0x1003)->arms_dip_ticks = 80;
	view.state().find(0x0002)->arms_dip_ticks = 80;

	for (int tick = 0; tick < 5; ++tick) view.tick_arms_dip();

	if (!expect(view.state().find(0x1003)->arms_dip_ticks == 80 &&
	                    view.state().find(0x1003)->pitch_kick_accum == 0,
	            "a vehicle row is skipped entirely by the dip integrator"))
		return false;
	// An Infantry row's update is org1's, which never reads the window: the stamp stays and
	// the term only eases, so from level it stays level [orig: Entity_UpdateInfantryAI
	// @0x4B9910 - the ease @0x4BE883..0x4BE891; the window is read only @0x4B5CAB / @0x4B5CDB].
	return expect(view.state().find(0x0002)->arms_dip_ticks == 80 &&
	                      view.state().find(0x0002)->pitch_kick_accum == 0,
	              "an infantry row's org1 block has no dip window");
}


bool test_unstamped_rows_stay_level() {
	ns::ClientReplicaPipeline view = make_view();
	for (int tick = 0; tick < 50; ++tick) view.tick_arms_dip();
	// The ease runs unconditionally, but from zero it must stay at zero rather than
	// drifting on the +4 rounding.
	return expect(view.state().find(0x0001)->pitch_kick_accum == 0 &&
	                      view.state().find(0x0002)->pitch_kick_accum == 0,
	              "a row that never reloaded never moves");
}

} // namespace


// D-NET-330: a remote player's torso roll (+0x2DC), the player-body rule in the same
// block: it chases the Roll word a sixteenth ((roll - torso + 8) >> 4), the lag clamped
// to +-20 degrees (0x0E38E380); prone idle decays it a sixteenth toward level, the prone
// rolls step it 0x4000000. An Infantry row takes org1's own rule: the same chase and clamps,
// prone idle a thirty-second step, no roll states.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - prone idle @0x4b5d05, the chase and
//  clamps @0x4b5d4e / @0x4b5d61, the rolls @0x4b700e / @0x4b701d; Entity_UpdateInfantryAI
//  @0x4B9910 - prone idle @0x4BE897..0x4BE8AC, the chase @0x4BE8B0..0x4BE8BD, the clamps
//  @0x4BE8C8..0x4BE8EA]
bool test_torso_roll_chases_the_roll_word() {
	ns::ClientReplicaPipeline view = make_view();
	ns::ClientEntityState &player = *view.state().find(0x0001);
	ns::ClientEntityState &infantry = *view.state().find(0x0002);
	player.roll_bam = 0x08000000;
	infantry.roll_bam = 0x08000000;
	view.tick_arms_dip();
	bool ok = expect(player.torso_roll == 0x00800000, "torso roll: a sixteenth of the lag");
	ok = expect(infantry.torso_roll == 0x00800000, "torso roll: an Infantry row chases a sixteenth too") && ok;
	// A roll the sixteenth leaves more than 20 degrees behind: the lag clamps.
	player.torso_roll = 0;
	player.roll_bam = 0x10000000;
	view.tick_arms_dip();
	ok = expect(player.torso_roll == 0x10000000 - 0x0E38E380, "torso roll: the lag clamps at -20 degrees") && ok;
	player.roll_bam = 0;
	player.torso_roll = 0x20000000;
	view.tick_arms_dip();
	ok = expect(player.torso_roll == 0x0E38E380, "torso roll: the lag clamps at +20 degrees") && ok;
	player.rm_state = 48; // idle prone
	view.tick_arms_dip();
	ok = expect(player.torso_roll == 0x0E38E380 - ((0x0E38E380 + 8) >> 4),
			"torso roll: prone idle decays it a sixteenth") && ok;
	player.rm_state = 41; // the left roll
	player.torso_roll = 0;
	view.tick_arms_dip();
	ok = expect(player.torso_roll == -0x4000000, "torso roll: the left roll steps it") && ok;
	// The Infantry row: its lag clamps the same, prone idle decays a thirty-second, and the
	// left roll state is no case of its block (it chases).
	infantry.roll_bam = 0;
	infantry.torso_roll = 0x20000000;
	view.tick_arms_dip();
	ok = expect(infantry.torso_roll == 0x0E38E380, "torso roll: an Infantry row's lag clamps") && ok;
	infantry.rm_state = 48; // idle prone
	view.tick_arms_dip();
	ok = expect(infantry.torso_roll == 0x0E38E380 - ((0x0E38E380 + 16) >> 5),
			"torso roll: an Infantry row's prone idle decays a thirty-second") && ok;
	infantry.rm_state = 41;
	infantry.torso_roll = 0;
	infantry.roll_bam = 0x08000000;
	view.tick_arms_dip();
	ok = expect(infantry.torso_roll == 0x00800000, "torso roll: an Infantry row has no roll steps") && ok;
	return ok;
}

int main() {
	bool ok = true;
	ok = test_torso_roll_chases_the_roll_word() && ok;
	ok = test_window_lasts_forty_ticks() && ok;
	ok = test_decay_dips_then_recovers() && ok;
	ok = test_only_persons_dip() && ok;
	ok = test_unstamped_rows_stay_level() && ok;
	if (ok) std::printf("client_replica_pipeline_arms_dip: OK\n");
	return ok ? 0 : 1;
}
