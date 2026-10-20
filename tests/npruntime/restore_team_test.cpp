// The slot's restore byte (+100568) across a map change (D-NET-591): a fresh
// row's byte is 0; the Attack-and-Defend teardown swap gives a live slot its
// nonzero byte as its team ahead of the 1 <-> 2 swap; each mission start's
// per-slot reset takes a spectator-latched slot with a nonzero byte out of the
// latch, hands it the byte as its team when its team is 0, and clears every
// slot's byte. A latched slot whose byte is 0 keeps its latch.
// [orig: Server_PlayerAdd - the row memset @0x51CD06; Game_TeardownMission -
//  the live test @0x522470..0x522479, the byte @0x52247B..0x522485, the swap
//  @0x52248B..0x5224AA; Server_ResetAllPlayerSlotFrameState @0x4FC320 - the
//  latch @0x4FC350, the byte @0x4FC362..0x4FC36A, the team test @0x4FC36C,
//  the latch clear @0x4FC372, the team store @0x4FC38C, the byte clear
//  @0x4FC398; Game_StartMission @0x5248AC..0x5248BC]
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/world/entity.h>

#include <cstdio>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

inmatch::NapiNPConnection slot(uint8_t team, bool team_valid, bool latched, uint8_t restore,
		uint16_t body) {
	inmatch::NapiNPConnection c;
	c.phase = inmatch::ConnectionPhase::PlayerAdded;
	c.link.owned_entity = world::EntityHandle{body};
	c.assigned_team = team;
	c.assigned_team_valid = team_valid;
	c.link.spectator = latched;
	c.spectator_restore_team = restore;
	return c;
}

void test_a_fresh_row_has_no_restore_byte() {
	const inmatch::NapiNPConnection fresh;
	CHECK(fresh.spectator_restore_team == 0);
}

// A first half's end: a slot converted from team 2 (latched, team 0, byte 2)
// takes team 2 and swaps to 1; a slot with no byte swaps its own team; the
// next mission start then releases the converted slot on its swapped team.
void test_the_swap_and_the_mission_start_restore_a_converted_slot() {
	inmatch::NapiNPServerCtx ctx;
	ctx.np_protocol.connection_list.push_back(slot(0, true, true, 2, 0x0058));
	ctx.np_protocol.connection_list.push_back(slot(1, true, false, 0, 0x0059));
	inmatch::HostRotation rotation;
	rotation.side_team[0] = 1;
	rotation.side_team[1] = 2;
	inmatch::swap_sides_or_latch(ctx, rotation);
	inmatch::NapiNPConnection &converted = ctx.np_protocol.connection_list[0];
	inmatch::NapiNPConnection &kept = ctx.np_protocol.connection_list[1];
	CHECK(converted.assigned_team == 1 && converted.link.spectator);
	CHECK(converted.spectator_restore_team == 2); // the swap keeps the byte
	CHECK(kept.assigned_team == 2);
	inmatch::Server_ResetAllPlayerSlotFrameState(ctx);
	CHECK(!converted.link.spectator);
	CHECK(converted.assigned_team == 1); // its team is not 0: kept
	CHECK(converted.spectator_restore_team == 0);
	CHECK(kept.assigned_team == 2 && !kept.link.spectator);
}

// The mission start alone: a latched team-0 slot takes its byte as its team;
// a latched slot whose byte is 0 (a team-0 convert) keeps its latch and team.
void test_the_mission_start_takes_the_byte_for_a_teamless_slot() {
	inmatch::NapiNPServerCtx ctx;
	ctx.np_protocol.connection_list.push_back(slot(0, true, true, 2, 0x0058));
	ctx.np_protocol.connection_list.push_back(slot(0, true, true, 0, 0x0059));
	inmatch::Server_ResetAllPlayerSlotFrameState(ctx);
	const inmatch::NapiNPConnection &restored = ctx.np_protocol.connection_list[0];
	const inmatch::NapiNPConnection &teamless = ctx.np_protocol.connection_list[1];
	CHECK(!restored.link.spectator && restored.assigned_team == 2 &&
			restored.spectator_restore_team == 0);
	CHECK(teamless.link.spectator && teamless.assigned_team == 0);
}

} // namespace

int main() {
	test_a_fresh_row_has_no_restore_byte();
	test_the_swap_and_the_mission_start_restore_a_converted_slot();
	test_the_mission_start_takes_the_byte_for_a_teamless_slot();
	if (failures == 0) std::printf("restore_team: all passed\n");
	return failures == 0 ? 0 : 1;
}
