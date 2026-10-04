// The DEATH-screen facts the replica fold carries (client_replica_death.cpp):
//   * the three phase-0 0x0A sub-block-0 whole-second timers beside the
//     pre-round timer [orig: NapiNPClientMsg_0x00A @0x430084 / @0x43009f /
//     @0x4300c3], retained across a non-phase-0 frame like the client globals;
//   * the 0x6E self wave zone — word_A85BC0 reset at every fold and set to the
//     zone handle of the group whose members name the viewer
//     [orig: NapiNPClientMsg_HandleSquadRosterSync @0x4298f6 / @0x429a04].
#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

#include <runtime/replication/client_replica_pipeline.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

using namespace opennova;
using namespace opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

FrameUpdate phase0_frame(uint8_t penalty, uint8_t revive, uint8_t hold) {
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 0;
	fu.local_tail_present = true;
	fu.sub_block = 0;
	fu.weapon.present = true;
	fu.weapon.preround_timer = 0;
	fu.weapon.slot_state360 = penalty;
	fu.weapon.slot_state368 = revive;
	fu.weapon.slot_state364 = hold;
	return fu;
}

void test_sub_block_0_timers_fold_and_retain() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	FrameUpdate first = phase0_frame(7, 120, 3);
	// The breath samples ride the same sub-block
	// [orig: NapiNPClientMsg_0x00A @0x430104 -> word_A85B7C].
	first.weapon.slot_state460 = 40;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(first));
	CHECK(view.state().respawn_penalty_seconds == 7);
	CHECK(view.state().local_revive_seconds == 120);
	CHECK(view.state().spawn_hold_seconds == 3);
	CHECK(view.state().breath_samples == 40);
	// A phase-1 frame (the timer sub-block) leaves the phase-0 landings alone.
	FrameUpdate timer;
	timer.carried_handle = 0xFFFF;
	timer.local_tail_present = true;
	timer.flags2 = 1; // the encoder keys the sub-block on flags2
	timer.sub_block = 1;
	timer.timer.present = true;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(timer));
	CHECK(view.state().respawn_penalty_seconds == 7);
	CHECK(view.state().local_revive_seconds == 120);
	CHECK(view.state().spawn_hold_seconds == 3);
	CHECK(view.state().breath_samples == 40);
	// The next phase-0 frame replaces them.
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(phase0_frame(0, 119, 0)));
	CHECK(view.state().respawn_penalty_seconds == 0);
	CHECK(view.state().local_revive_seconds == 119);
	CHECK(view.state().spawn_hold_seconds == 0);
	CHECK(view.state().breath_samples == 0);
}

// [u8 groupCount] then { u16 zone, u16 index, u8 count, u16 countdown, u16 × count }.
std::vector<uint8_t> wave_body(uint16_t zone, uint16_t index, uint16_t countdown,
		std::vector<uint16_t> members) {
	std::vector<uint8_t> b;
	b.push_back(1);
	b.push_back(uint8_t(zone));
	b.push_back(uint8_t(zone >> 8));
	b.push_back(uint8_t(index));
	b.push_back(uint8_t(index >> 8));
	b.push_back(uint8_t(members.size()));
	b.push_back(uint8_t(countdown));
	b.push_back(uint8_t(countdown >> 8));
	for (uint16_t m : members) {
		b.push_back(uint8_t(m));
		b.push_back(uint8_t(m >> 8));
	}
	return b;
}

void test_self_wave_zone() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	view.set_viewer_handle(0x0005);
	view.apply(s2c::SPAWN_WAVE_STATUS, wave_body(0x1003, 2, 14, {0x0002, 0x0005}));
	CHECK(view.state().spawn_waves.known);
	CHECK(view.state().spawn_waves.self_zone_handle == 0x1003);
	CHECK(view.state().spawn_waves.value.groups.size() == 1 &&
			view.state().spawn_waves.value.groups[0].wave_countdown == 14);
	// A later fold without the viewer resets it to none (the -1 store at
	// every fold, before the group walk).
	view.apply(s2c::SPAWN_WAVE_STATUS, wave_body(0x1003, 2, 13, {0x0002}));
	CHECK(view.state().spawn_waves.self_zone_handle == 0xFFFF);
	// The empty-group body (the 1 Hz dead-player cadence) does the same.
	view.apply(s2c::SPAWN_WAVE_STATUS, wave_body(0x1003, 2, 12, {0x0005}));
	CHECK(view.state().spawn_waves.self_zone_handle == 0x1003);
	view.apply(s2c::SPAWN_WAVE_STATUS, {0x00});
	CHECK(view.state().spawn_waves.self_zone_handle == 0xFFFF);
	CHECK(view.state().spawn_waves.value.groups.empty());
}

} // namespace

// A minimal complete S2C 0x0F body: the 23-byte fixed header (game_flags at
// byte 22), the 128-entry zero score table, then zero waypoint/team-name
// counts — the exact end the strict decoder requires.
std::vector<uint8_t> world_state_load_body(uint8_t game_flags) {
	std::vector<uint8_t> b(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	b[22] = game_flags;
	return b;
}

// The deploy-map OVERLAY global (retail g_DeployScreenActive): armed by the
// 0x0F game_flags bit0 UNLESS the death screen is already up, then host-
// ASSIGNED every per-frame 0x0A from flags1 bit1 — set and cleared, no edge
// latch. [orig: NapiNPClientMsg_0x00F zero @0x42e2d8 + arm @0x42e2f8;
//  NapiNPClientMsg_0x00A @0x42ff82 g_DeployScreenActive = (flags1 >> 1) & 1]
void test_deploy_overlay_follows_the_host() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	CHECK(!view.state().deploy_overlay_active);
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x02));
	CHECK(view.state().deploy_check_secured_spawn && !view.state().deploy_overlay_active);
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x01));
	CHECK(!view.state().deploy_check_secured_spawn);
	CHECK(view.state().deploy_overlay_active);
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x00));
	CHECK(!view.state().deploy_overlay_active);
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 150;
	fu.local_tail_present = true;
	fu.flags1 = 0x02;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(view.state().deploy_overlay_active);
	fu.flags1 = 0x00;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(!view.state().deploy_overlay_active);
	// With the death screen up (flags1 bit0), the 0x0F bit0 arm is suppressed.
	fu.flags1 = 0x01;
	fu.health = 0;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(view.state().death_screen_active);
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x01));
	CHECK(!view.state().deploy_overlay_active);
}

// The death.mnu open latch over the frame loop's triggers: one open per
// arming, stamped result-blind, held off while another menu is up, and
// cleared only by the triggers falling - never by a dismiss.
// [orig: Render_ProcessMainSceneFrame gate @0x5cab67, latch @0x5cab70/@0x5cab8b;
//  Game_CloseInGameScreens @0x54b954 on the close-on-clear leg]
void test_death_menu_open_latch() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	auto take = [&](bool menu_open = false) {
		return view.state().take_death_menu_open(view.state().deploy_overlay_active, menu_open);
	};
	CHECK(!take()); // nothing armed
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x01));
	CHECK(!take(/*menu_open=*/true)); // another menu holds the open off
	CHECK(!view.state().death_menu_open_latch);
	CHECK(take());  // the one open
	CHECK(!take()); // latched (a dismiss does not re-arm)
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 150;
	fu.local_tail_present = true;
	fu.flags1 = 0x02; // the host keeps the bit set: still latched
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(!take());
	fu.flags1 = 0x00; // the trigger falls: the frame clears the latch
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(view.state().death_menu_open_latch); // the fold alone leaves it
	CHECK(!take());
	CHECK(!view.state().death_menu_open_latch);
	fu.flags1 = 0x02; // re-armed: the screen opens again
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(take());
	CHECK(!take());
	// The 0x0F zero drops the trigger the same way [orig: @0x42e2d8].
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x00));
	CHECK(!take());
	CHECK(!view.state().death_menu_open_latch);
	// A trigger other than the overlay (the local death) latches the same way.
	CHECK(view.state().take_death_menu_open(true, false));
	CHECK(!view.state().take_death_menu_open(true, false));
	CHECK(!view.state().take_death_menu_open(false, false));
	CHECK(!view.state().death_menu_open_latch);
}

// The count of spectator-tip commands the pass queued (replication::
// TipEventCommand), draining the rest.
int spectator_tips(ClientReplicaPipeline &view) {
	int n = 0;
	for (const ClientEffectCommand &c : view.drain_effect_commands())
		if (const auto *tip = std::get_if<TipEventCommand>(&c)) n += tip->event == 22 ? 1 : 0;
	return n;
}

// The count of HUD-blank commands the pass queued (replication::
// HudDetailBlankCommand), draining the rest.
int hud_blanks(ClientReplicaPipeline &view) {
	int n = 0;
	for (const ClientEffectCommand &c : view.drain_effect_commands())
		if (std::get_if<HudDetailBlankCommand>(&c) != nullptr) ++n;
	return n;
}

// The 0x0F's HUD blank is gated on the death screen: a fresh join's 0x0F (the
// 0x0A's death bit clear, as every live retail join carried it) leaves the
// live declutter level alone; a 0x0F folded with the death screen up queues the
// blank once; the 0x0A clearing the bit ahead of the 0x0F in the same frame
// (retail's pump order) queues nothing.
// [orig: NapiNPClientMsg_0x00F `cmp g_DeathScreenActive, 0` @0x42e3f5 /
//  @0x42e407 -> `mov g_HUDDetailLevel, 3` @0x42e412]
void test_world_state_load_blanks_the_hud_only_with_the_death_screen_up() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 150;
	fu.local_tail_present = true;
	fu.flags1 = 0x02; // the deploy overlay up, the death screen down
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x01));
	CHECK(!view.state().death_screen_active);
	CHECK(hud_blanks(view) == 0);
	fu.flags1 = 0x01;
	fu.health = 0;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(view.state().death_screen_active);
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x01));
	CHECK(hud_blanks(view) == 1);
	fu.flags1 = 0x00;
	fu.health = 150;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	view.apply(s2c::WORLD_STATE_LOAD, world_state_load_body(0x01));
	CHECK(hud_blanks(view) == 0);
}

// The death-screen rising edge raises the spectator tip (event 22) unless the
// round is over; a held bit raises nothing more.
// [orig: NapiNPClientMsg_0x00A @0x42ffd0..0x42ffdf — `cmp g_SpawnSuccessGate`
//  then CTipSystem_HandleEvent(22)]
void test_death_edge_raises_the_spectator_tip() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 0;
	fu.local_tail_present = true;
	fu.flags1 = 0x01;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(view.state().death_screen_active);
	CHECK(spectator_tips(view) == 1);
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(spectator_tips(view) == 0);
	fu.flags1 = 0x00;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(spectator_tips(view) == 0);
	// The round-over gate holds it.
	view.state().spawn_success_gate = true;
	fu.flags1 = 0x01;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	CHECK(view.state().death_screen_active);
	CHECK(spectator_tips(view) == 0);
}

// A KNOWN tag whose body fails its decoder counts as a malformed body, not
// an unknown tag — the arm that once bumped the wrong counter.
void test_truncated_known_body_counts_as_malformed() {
	auto owned = std::make_unique<ClientReplicaPipeline>();
	ClientReplicaPipeline &view = *owned;
	const std::size_t malformed_before = view.malformed_bodies();
	const std::size_t unknown_before = view.unknown_tags();
	std::vector<uint8_t> truncated = world_state_load_body(0x01);
	truncated.resize(10);
	view.apply(s2c::WORLD_STATE_LOAD, truncated);
	CHECK(view.malformed_bodies() == malformed_before + 1);
	CHECK(view.unknown_tags() == unknown_before);
	CHECK(!view.state().deploy_overlay_active); // nothing folded
}

int main() {
	test_sub_block_0_timers_fold_and_retain();
	test_self_wave_zone();
	test_deploy_overlay_follows_the_host();
	test_death_menu_open_latch();
	test_death_edge_raises_the_spectator_tip();
	test_world_state_load_blanks_the_hud_only_with_the_death_screen_up();
	test_truncated_known_body_counts_as_malformed();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_death_test OK\n");
	return 0;
}
