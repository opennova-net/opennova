// npruntime_orphan_row_walks: the slot walks that take an orphaned row (a row a re-add left
// behind, D-NET-NEW-L1-24) as any active row, beyond the per-slot block's sends:
//   - the team imbalance counts the row's team, and the round init's auto-balance moves it,
//     its pair keyed by the row's own add stamp (the pair list sorted ascending, so the row
//     the shortest time in the server moves first);
//   - the Attack-and-Defend teardown's swap arm gives the row its restore byte and swaps it;
//   - the list builder hides an orphaned spectator row's body from every other row;
//   - the row's net player is found by the net player's identity, which the C2S 0x48's id
//     re-stamp does not move, and no other connection carrying the row's old id takes it.
// [orig: Server_CalcTeamImbalance @0x4FCAE0 (+4 @0x4FCB10, +416 @0x4FCB19);
//  Server_AutoBalanceTeams @0x4FCC30 (+4 @0x4FCC74, +5 @0x4FCC7A, the key @0x4FCC82, the walk
//  @0x4FCCDC..0x4FCDD8); CPairList_ShellSort @0x526C00 (`cmp` @0x526C77, `jl` @0x526C7B);
//  Server_PlayerAdd @0x51CBC0 (the add stamp @0x51CD95, +0x1C @0x51CD4E); Game_TeardownMission
//  (the live test @0x522470..0x522479, the byte @0x52247B..0x522485, the swap
//  @0x52248B..0x5224AA); Entity_ValidatePtr @0x500910 (@0x500930..0x500938);
//  Server_BuildEntityPriorityList (+0x17D01 / +0x17D00 @0x50E6F4 / @0x50E6FD)]

#include <runtime/inmatch/map_change.h>       // swap_sides_or_latch
#include <runtime/inmatch/mission_rotation.h> // HostRotation
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_entity_state.h> // snapshot_entity_states
#include <runtime/inmatch/server_team_change.h>  // the imbalance and the auto-balance
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

constexpr uint32_t kTeamGame = 0x10000u; // g_GameType & 0x10000

inmatch::NapiNPConnection row(uint8_t slot, uint8_t team, uint32_t added_ms) {
	inmatch::NapiNPConnection c;
	c.type = inmatch::NapiNPConnection::kTypeServerSide;
	c.phase = inmatch::ConnectionPhase::InMatch;
	c.reply.player_slot = slot;
	c.assigned_team = team;
	c.assigned_team_valid = true;
	c.row_added_host_ms = added_ms;
	return c;
}

inmatch::OrphanedPlayerRow orphan_of(const inmatch::NapiNPConnection &c) {
	inmatch::OrphanedPlayerRow o;
	o.row = c;
	return o;
}

void test_the_imbalance_counts_an_orphaned_row() {
	inmatch::NapiNPServerCtx ctx;
	ctx.config.game_type = kTeamGame;
	ctx.np_protocol.connection_list.push_back(row(1, 1, 0));
	CHECK(inmatch::Server_CalcTeamImbalance(ctx) == 1);
	ctx.orphaned_player_rows.push_back(orphan_of(row(2, 1, 0)));
	CHECK(inmatch::Server_CalcTeamImbalance(ctx) == 2);
	ctx.orphaned_player_rows.push_back(orphan_of(row(3, 2, 0)));
	CHECK(inmatch::Server_CalcTeamImbalance(ctx) == 1);
}

// Four rows on team 1, one of them orphaned, its net player's fresh row added later. At 1000 ms
// the keys are 900 (slot 1), 500 (the orphaned slot 2), 100 (the fresh slot 3) and 700 (slot
// 4); sorted ascending the walk moves slot 3, then the orphaned slot 2, and stops at a
// difference of 0. Keyed by the connection's join stamp instead, the orphan and its fresh row
// would share one key; walking the connections alone, slot 4 would move instead of the orphan.
void test_auto_balance_moves_an_orphaned_row_by_its_own_add_stamp() {
	inmatch::NapiNPServerCtx ctx;
	ctx.config.game_type = kTeamGame;
	ctx.config.auto_balance_min_difference = 1;
	ctx.np_protocol.host_run_duration_ms = 1000;
	ctx.np_protocol.connection_list.push_back(row(1, 1, 100));
	ctx.np_protocol.connection_list.push_back(row(3, 1, 900));
	ctx.np_protocol.connection_list.push_back(row(4, 1, 300));
	ctx.orphaned_player_rows.push_back(orphan_of(row(2, 1, 500)));
	CHECK(inmatch::Server_CalcTeamImbalance(ctx) == 4);
	inmatch::Server_AutoBalanceTeams(ctx, nullptr);
	const auto &conns = ctx.np_protocol.connection_list;
	CHECK(conns[0].assigned_team == 1); // slot 1, the longest in the server
	CHECK(conns[1].assigned_team == 2); // slot 3, the newest
	CHECK(conns[2].assigned_team == 1); // slot 4
	CHECK(ctx.orphaned_player_rows[0].row.assigned_team == 2);
	CHECK(inmatch::Server_CalcTeamImbalance(ctx) == 0);
}

// The key is every row's own add stamp, a connection's too, never its join-validation stamp:
// slot 1 validated first but was added last, so at 1000 ms its key is 200 against slot 2's
// 800 and the ascending walk moves slot 1 alone (by the validation stamps, 900 against 400,
// slot 2 would move).
void test_auto_balance_keys_every_row_by_its_add_stamp() {
	inmatch::NapiNPServerCtx ctx;
	ctx.config.game_type = kTeamGame;
	ctx.np_protocol.host_run_duration_ms = 1000;
	inmatch::NapiNPConnection first = row(1, 1, 800);
	first.join_validated_host_ms = 100;
	inmatch::NapiNPConnection second = row(2, 1, 200);
	second.join_validated_host_ms = 600;
	ctx.np_protocol.connection_list.push_back(first);
	ctx.np_protocol.connection_list.push_back(second);
	inmatch::Server_AutoBalanceTeams(ctx, nullptr);
	CHECK(ctx.np_protocol.connection_list[0].assigned_team == 2);
	CHECK(ctx.np_protocol.connection_list[1].assigned_team == 1);
}

// The swap arm: a live orphaned row takes its nonzero restore byte as its team, then swaps; a
// live orphaned row with no byte swaps its own team; one with no body is skipped.
void test_the_swap_arm_swaps_an_orphaned_row() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::NapiNPConnection converted = row(2, 0, 0);
	converted.spectator_restore_team = 2;
	converted.link.owned_entity = w::EntityHandle{0x0058};
	inmatch::NapiNPConnection kept = row(3, 1, 0);
	kept.link.owned_entity = w::EntityHandle{0x0059};
	inmatch::NapiNPConnection bodyless = row(4, 1, 0);
	ctx.orphaned_player_rows.push_back(orphan_of(converted));
	ctx.orphaned_player_rows.push_back(orphan_of(kept));
	ctx.orphaned_player_rows.push_back(orphan_of(bodyless));
	inmatch::HostRotation rotation;
	rotation.side_team[0] = 1;
	rotation.side_team[1] = 2;
	inmatch::swap_sides_or_latch(ctx, rotation);
	CHECK(ctx.orphaned_player_rows[0].row.assigned_team == 1);
	CHECK(ctx.orphaned_player_rows[1].row.assigned_team == 2);
	CHECK(ctx.orphaned_player_rows[2].row.assigned_team == 1);
	CHECK(rotation.side_team[0] == 2 && rotation.side_team[1] == 1);
}

// An orphaned spectator row's body is listed only to its own row: hidden from every other
// recipient, the fresh row of its own net player included.
void test_an_orphaned_spectator_rows_body_is_hidden() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	world->rules.mp_session = true;
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ctx->world = world.get();
	ctx->is_authority = 1;
	ctx->is_in_session = 1;
	w::PlayerSpawn s;
	s.position = {10.0f, 10.0f, 2.0f};
	s.team = 1;
	const w::EntityHandle fresh_body = w::spawn_remote_player(*world, s);
	s.position = {20.0f, 10.0f, 2.0f};
	const w::EntityHandle orphan_body = w::spawn_remote_player(*world, s);
	CHECK(fresh_body.valid() && orphan_body.valid());
	inmatch::NapiNPConnection fresh = row(3, 1, 0);
	fresh.link.owned_entity = fresh_body;
	ctx->np_protocol.connection_list.push_back(fresh);
	inmatch::NapiNPConnection spectator = row(2, 0, 0);
	spectator.link.owned_entity = orphan_body;
	spectator.link.spectator = true;
	ctx->orphaned_player_rows.push_back(orphan_of(spectator));
	const std::vector<GameEntitySnapshot> ents = inmatch::snapshot_entity_states(*ctx, *world);
	bool orphan_hidden = false;
	bool fresh_hidden = true;
	for (const GameEntitySnapshot &e : ents) {
		if (e.wire_handle == orphan_body.packed) orphan_hidden = e.owner_hidden;
		if (e.wire_handle == fresh_body.packed) fresh_hidden = e.owner_hidden;
	}
	CHECK(orphan_hidden);
	CHECK(!fresh_hidden);
}

// The row's net player is the connection whose identity the orphaning drew: a C2S 0x48's id
// re-stamp keeps it bound, and another connection that takes the row's old id never does.
void test_the_net_player_is_bound_by_identity() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::NapiNPConnection player = row(3, 1, 0);
	player.connection_id = 7;
	player.net_player_serial = ++ctx.last_net_player_serial;
	inmatch::NapiNPConnection orphaned = player;
	orphaned.reply.player_slot = 2;
	ctx.np_protocol.connection_list.push_back(player);
	ctx.orphaned_player_rows.push_back(orphan_of(orphaned));
	inmatch::OrphanedPlayerRow &orphan = ctx.orphaned_player_rows[0];
	CHECK(inmatch::orphan_net_player(ctx, orphan) == &ctx.np_protocol.connection_list[0]);
	// The 0x48 re-stamps the player's id; another peer then names the old id.
	ctx.np_protocol.connection_list[0].connection_id = 0x1234;
	inmatch::NapiNPConnection other = row(4, 2, 0);
	other.connection_id = 7;
	ctx.np_protocol.connection_list.insert(ctx.np_protocol.connection_list.begin(), other);
	CHECK(inmatch::orphan_net_player(ctx, orphan) == &ctx.np_protocol.connection_list[1]);
	// The player leaves: the row reads no net player.
	ctx.np_protocol.connection_list.pop_back();
	CHECK(inmatch::orphan_net_player(ctx, orphan) == nullptr);
	CHECK(orphan.row.link.transport == nullptr);
}

} // namespace

int main() {
	test_the_imbalance_counts_an_orphaned_row();
	test_auto_balance_moves_an_orphaned_row_by_its_own_add_stamp();
	test_auto_balance_keys_every_row_by_its_add_stamp();
	test_the_swap_arm_swaps_an_orphaned_row();
	test_an_orphaned_spectator_rows_body_is_hidden();
	test_the_net_player_is_bound_by_identity();
	if (failures != 0) {
		std::printf("npruntime_orphan_row_walks: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_orphan_row_walks: ok\n");
	return 0;
}
