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

#include <net/netsim/client_replica_pipeline.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

using namespace opennova;
using namespace opennova::netsim;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

FrameUpdate phase0_frame(uint8_t penalty, uint8_t revive, uint8_t hold) {
	FrameUpdate fu;
	fu.mount_handle = 0xFFFF;
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
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(phase0_frame(7, 120, 3)));
	CHECK(view.state().respawn_penalty_seconds == 7);
	CHECK(view.state().local_revive_seconds == 120);
	CHECK(view.state().spawn_hold_seconds == 3);
	// A phase-1 frame (the timer sub-block) leaves the phase-0 landings alone.
	FrameUpdate timer;
	timer.mount_handle = 0xFFFF;
	timer.local_tail_present = true;
	timer.flags2 = 1; // the encoder keys the sub-block on flags2
	timer.sub_block = 1;
	timer.timer.present = true;
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(timer));
	CHECK(view.state().respawn_penalty_seconds == 7);
	CHECK(view.state().local_revive_seconds == 120);
	CHECK(view.state().spawn_hold_seconds == 3);
	// The next phase-0 frame replaces them.
	view.apply(s2c::PER_FRAME_UPDATE, encode_frame_update(phase0_frame(0, 119, 0)));
	CHECK(view.state().respawn_penalty_seconds == 0);
	CHECK(view.state().local_revive_seconds == 119);
	CHECK(view.state().spawn_hold_seconds == 0);
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

int main() {
	test_sub_block_0_timers_fold_and_retain();
	test_self_wave_zone();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("client_replica_death_test OK\n");
	return 0;
}
