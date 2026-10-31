// npruntime_orphan_row_walks: the slot walks that take an orphaned row (a row a re-add left
// behind, D-NET-821) as any active row, beyond the per-slot block's sends:
//   - the team imbalance counts the row's team, and the round init's auto-balance moves it,
//     its pair keyed by the row's own add stamp (the pair list sorted ascending, so the row
//     the shortest time in the server moves first);
//   - the Attack-and-Defend teardown's swap arm gives the row its restore byte and swaps it;
//   - the list builder hides an orphaned spectator row's body from every other row;
//   - the row's net player is found by the net player's identity, which the C2S 0x48's id
//     re-stamp does not move, and no other connection carrying the row's old id takes it;
//   - Server_TickUpdate's per-row walks: the play ticks count the row's body; the periodic
//     second's walk steps the row's dead-age counter, sends its 0x6E to its net player and
//     punts that net player at the dead-too-long limit (past the player's leave the counter
//     alone steps); the downed resend sends the row's downed body to its team;
//   - the scoreboard boundary's integrity family sends the row's request to its net player;
//   - the round end sends the row's 0x61 and 0x1D to its net player, in slot order with the
//     player's own row's pair, and moves the row to state 7;
//   - a C2S 0x22 naming the row's slot answers the row's 0x46, its PCID the live net
//     player's (none past the player's leave), not the removal record;
//   - a chat line or a ruled radio call the row admits reaches its net player, which takes
//     one per row.
// [orig: Server_CalcTeamImbalance @0x4FCAE0 (+4 @0x4FCB10, +416 @0x4FCB19);
//  Server_AutoBalanceTeams @0x4FCC30 (+4 @0x4FCC74, +5 @0x4FCC7A, the key @0x4FCC82, the walk
//  @0x4FCCDC..0x4FCDD8); CPairList_ShellSort @0x526C00 (`cmp` @0x526C77, `jl` @0x526C7B);
//  Server_PlayerAdd @0x51CBC0 (the add stamp @0x51CD95, +0x1C @0x51CD4E); Game_TeardownMission
//  (the live test @0x522470..0x522479, the byte @0x52247B..0x522485, the swap
//  @0x52248B..0x5224AA); Entity_ValidatePtr @0x500910 (@0x500930..0x500938);
//  Server_BuildEntityPriorityList (+0x17D01 / +0x17D00 @0x50E6F4 / @0x50E6FD);
//  Server_TickUpdate (the play-tick walk @0x51D95E..0x51D98A, the periodic walk's 0x6E to the
//  row @0x51E09D..0x51E0CF and the t7 punt @0x51E18E, the downed resend's row walk
//  @0x51E310..0x51E378); Server_LogCRCMismatchPunt @0x517ED0 (the row's +0x1C @0x517F47);
//  Server_ProcessRoundEnd (the row walk @0x516730..0x51687D: the 0x61 @0x516790, the 0x1D
//  @0x516839, game state 11 @0x516846, state 7 @0x51685E)]

#include <runtime/inmatch/map_change.h>       // swap_sides_or_latch
#include <runtime/inmatch/mission_rotation.h> // HostRotation
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/punt_log.h>     // PuntLog (the sweep's _PUNT.TXT lines)
#include <runtime/inmatch/server_files.h>  // ServerFileSink
#include <runtime/inmatch/server_entity_state.h> // snapshot_entity_states
#include <runtime/inmatch/server_chat.h>         // Server_HandleChatMessage
#include <runtime/inmatch/server_radio_call.h>   // Server_HandleRadioCall
#include <runtime/inmatch/server_medic.h>        // Server_ResendTeamDownedStates
#include <runtime/inmatch/server_message_dispatch.h> // dispatch_session_replies (the C2S 0x22)
#include <runtime/inmatch/server_net_quality.h>  // Server_SampleHostNetQuality
#include <runtime/inmatch/server_tick.h>         // Server_ProcessRoundEnd
#include <runtime/inmatch/server_tick_maintenance.h> // the play ticks, the periodic walk
#include <runtime/inmatch/udp_session_transport.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/inmatch/server_team_change.h>  // the imbalance and the auto-balance
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <base/io/rotating_prng.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../common/title_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

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
	test_titles::jo_config(ctx.config);
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
	test_titles::jo_config(ctx.config);
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
	test_titles::jo_config(ctx.config);
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
	test_titles::jo_config(ctx.config);
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
	test_titles::jo_world(*world);
	world->registry.configure_pool(0, 8);
	world->rules.mp_session = true;
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	test_titles::jo_config(ctx->config);
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
	test_titles::jo_config(ctx.config);
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

// A session host with one net player (slot 3, in the match, its own live body) whose re-add
// orphaned its earlier row (slot 2, in the match, a body of its own).
struct OrphanRig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	bool world_jo = test_titles::jo_world(*world);
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	bool ctx_jo = test_titles::jo_config(ctx->config);
	ns::UdpSessionTransport transport{ns::UdpSessionTransport::Role::Host};
	w::EntityHandle own_body;
	w::EntityHandle orphan_body;

	OrphanRig() {
		world->registry.configure_pool(0, 8);
		world->rules.mp_session = true;
		ctx->world = world.get();
		ctx->is_authority = 1;
		ctx->is_in_session = 1;
		w::PlayerSpawn s;
		s.position = {10.0f, 10.0f, 2.0f};
		s.team = 1;
		own_body = w::spawn_remote_player(*world, s);
		s.position = {20.0f, 10.0f, 2.0f};
		orphan_body = w::spawn_remote_player(*world, s);
		inmatch::NapiNPConnection player = row(3, 1, 0);
		player.burst.spawned = true;
		player.slot_state = inmatch::PlayerSlotState::Joined; // both rows in state 6
		player.link.owned_entity = own_body;
		player.link.transport = &transport;
		player.link.mode = ns::TransportMode::Client;
		player.net_player_serial = ++ctx->last_net_player_serial;
		inmatch::NapiNPConnection orphaned = player;
		orphaned.reply.player_slot = 2;
		orphaned.link.owned_entity = orphan_body;
		orphaned.link.transport = nullptr;
		ctx->np_protocol.connection_list.push_back(player);
		ctx->orphaned_player_rows.push_back(orphan_of(orphaned));
		for (const auto &[body, slot] : {std::pair{own_body, uint8_t{3}},
				std::pair{orphan_body, uint8_t{2}}}) {
			w::MatchPlayerIdentity identity;
			identity.entity = body;
			identity.slot = slot;
			world->match.upsert_player(identity);
		}
	}
	inmatch::NapiNPConnection &net_player() { return ctx->np_protocol.connection_list[0]; }
	inmatch::NapiNPConnection &orphan() { return ctx->orphaned_player_rows[0].row; }
	std::vector<uint8_t> drain() {
		std::vector<uint8_t> tags;
		ns::Datagram d;
		while (transport.pop_outbound(d)) tags.push_back(d.tag);
		return tags;
	}
};

void test_the_play_ticks_count_an_orphaned_row() {
	OrphanRig r;
	inmatch::advance_play_ticks(*r.ctx, *r.world);
	const w::MatchPlayer *orphan = r.world->match.player(r.orphan_body);
	const w::MatchPlayer *own = r.world->match.player(r.own_body);
	CHECK(orphan != nullptr && orphan->play_ticks == 1);
	CHECK(own != nullptr && own->play_ticks == 1);
}

// The host's net-quality sample walks the slot table: the orphaned row (its ring at 300 ms) is
// sampled beside its net player's current row (100 ms), the per-row means averaging 200 ms, the
// ping metric 255 * 200 / 1000 = 51; walking the connections alone would sample 100 ms, 25
// (D-NET-828). [orig: CNetQuality_UpdateMetrics @0x4C52C0 - g_PlayerSlots walked
// @0x4C5373..0x4C540B, the gate @0x4C5376..0x4C5398, the ping term @0x4C5442..0x4C5471]
void test_the_net_quality_walk_samples_an_orphaned_row() {
	OrphanRig r;
	r.ctx->config.max_players = 8; // an eight-row slot table
	for (const w::EntityHandle body : {r.own_body, r.orphan_body})
		r.world->match.player(body)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	for (uint32_t i = 0; i < 10; ++i) {
		r.net_player().reply.rtt_ring[i] = 100;
		r.orphan().reply.rtt_ring[i] = 300;
	}
	r.ctx->stats_avg_fps = 62;
	r.ctx->net_quality_sample_countdown = 0;
	inmatch::Server_SampleHostNetQuality(*r.ctx);
	const int32_t cursor = r.ctx->host_quality_window.cursor;
	CHECK(cursor == 1);
	CHECK(r.ctx->host_quality_window.ping[cursor] == 51);
	CHECK(r.ctx->host_quality_window.loss[cursor] == 1);
}

// The walk's count starts at the slot table's capacity and the ring's ten-sample loop leaves 10
// in it at every sampled row, so the walk ends nine rows past the last sampled one: over a
// 32-row table, the orphaned row at slot 2 (300 ms) and its net player's row at slot 12
// (100 ms, ten past) sample the orphaned row alone, its 300 clamped 255, 255 * 255 / 1000 = 65;
// at slot 11 (nine past) both, 51. A first sampled row deep in the table is reached (slots 20
// and 29, and 20 alone with the other at 30), and the walk stops at the table's end (slots 30
// and 31), where retail's reads on past g_PlayerSlots (PERMANENT, ratified 2026-10-22, Q12-2 (a),
// D-NET-828).
// [orig: CNetQuality_UpdateMetrics @0x4C52C0 - the count from g_PlayerSlotCapacity @0x4C535B, the
//  10 @0x4C53CC, `--count` @0x4C5400, `jg` @0x4C540B, the ping term @0x4C5442..0x4C5457]
void test_the_net_quality_walk_ends_nine_rows_past_a_sampled_row() {
	OrphanRig r;
	r.ctx->config.max_players = 32; // a 32-row slot table
	for (const w::EntityHandle body : {r.own_body, r.orphan_body})
		r.world->match.player(body)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	for (uint32_t i = 0; i < 10; ++i) {
		r.net_player().reply.rtt_ring[i] = 100;
		r.orphan().reply.rtt_ring[i] = 300;
	}
	r.ctx->stats_avg_fps = 62;
	const auto ping_with = [&r](uint8_t orphan_slot, uint8_t player_slot) {
		r.orphan().reply.player_slot = orphan_slot;
		r.net_player().reply.player_slot = player_slot;
		r.ctx->net_quality_sample_countdown = 0;
		inmatch::Server_SampleHostNetQuality(*r.ctx);
		return r.ctx->host_quality_window.ping[r.ctx->host_quality_window.cursor];
	};
	CHECK(ping_with(2, 12) == 65);
	CHECK(ping_with(2, 11) == 51);
	CHECK(ping_with(20, 29) == 51);
	CHECK(ping_with(20, 30) == 65);
	CHECK(ping_with(30, 31) == 51);
}

// The orphaned row's body has been dead 360 periodic seconds: the walk's next second counts
// 361, sends the row's 0x6E to its net player and punts that player (t7). Past the player's
// leave the counter alone steps.
void test_the_periodic_walk_takes_an_orphaned_row() {
	OrphanRig r;
	if (w::Entity *body = r.world->registry.get(r.orphan_body)) {
		body->flags |= w::kEntityFlagDead;
		body->health = 0;
	}
	r.orphan().reply.dead_live_seconds = 360;
	(void)r.drain();
	inmatch::update_periodic_slot_walk(*r.ctx, *r.world);
	const std::vector<uint8_t> tags = r.drain();
	int status = 0;
	for (uint8_t t : tags) status += t == s2c::SPAWN_WAVE_STATUS ? 1 : 0;
	CHECK(status == 1);
	CHECK(r.orphan().reply.dead_live_seconds == 361);
	CHECK(r.net_player().host_disconnect_sent);
	r.ctx->np_protocol.connection_list.clear();
	inmatch::update_periodic_slot_walk(*r.ctx, *r.world);
	CHECK(r.orphan().reply.dead_live_seconds == 362);
	CHECK(r.drain().empty());
}

// The team-mode downed resend: the orphaned row's downed body goes to the row's team, its
// net player's current row included.
void test_the_downed_resend_takes_an_orphaned_row() {
	OrphanRig r;
	w::MatchRules rules;
	test_titles::jo_match(rules);
	rules.game_type = kTeamGame;
	r.world->match.configure(rules);
	if (w::Entity *body = r.world->registry.get(r.orphan_body)) {
		body->flags |= w::kEntityFlagDead;
		body->health = 0;
	}
	r.orphan().link.death_cause_revivable = true;
	(void)r.drain();
	inmatch::Server_ResendTeamDownedStates(*r.ctx, *r.world);
	int syncs = 0;
	for (uint8_t t : r.drain()) syncs += t == s2c::PLAYER_SYNC ? 1 : 0;
	CHECK(syncs == 1);
}

// The scoreboard boundary's integrity family walks every row: the orphaned row's request (the
// entity form here) goes to its net player beside the player's own row's.
void test_the_integrity_family_takes_an_orphaned_row() {
	OrphanRig r;
	r.ctx->scoreboard_broadcast_timer = 0x136u; // the boundary is the next step
	r.ctx->integrity_entity_family_next = true;
	(void)r.drain();
	inmatch::emit_scoreboard_boundary(*r.ctx, *r.world);
	int lists = 0;
	int requests = 0;
	for (uint8_t t : r.drain()) {
		lists += t == s2c::PLAYER_LIST ? 1 : 0;
		requests += t == s2c::ENTITY_CHECKSUM_REQ ? 1 : 0;
	}
	CHECK(lists == 2);
	CHECK(requests == 2);
}

// The round end: each state-6 row's 0x61 then its 0x1D go to its net player in slot order,
// the orphaned slot 2's pair ahead of the player's own slot 3's; both rows enter state 7 and
// the net player game state 11.
void test_the_round_end_takes_an_orphaned_row() {
	OrphanRig r;
	(void)r.drain();
	inmatch::Server_ProcessRoundEnd(*r.ctx, *r.world, 1);
	std::vector<uint8_t> pairs;
	for (uint8_t t : r.drain())
		if (t == s2c::TICK_SEED || t == s2c::END_ROUND_HEADER) pairs.push_back(t);
	CHECK((pairs == std::vector<uint8_t>{s2c::TICK_SEED, s2c::END_ROUND_HEADER, s2c::TICK_SEED,
			s2c::END_ROUND_HEADER}));
	CHECK(r.orphan().slot_state == inmatch::PlayerSlotState::RoundOver);
	CHECK(r.net_player().slot_state == inmatch::PlayerSlotState::RoundOver);
	CHECK(r.net_player().burst.game_state == 11);
}

// The tick head's per-row block (Server_UpdateAllActivePlayerSlots) takes the orphaned row
// (slot 2, ahead of its net player's slot 3): its body's spawn protection counts down, its
// overlay visit runs (the cooldown re-armed at 13, the phase stepped), its frontier bit clears,
// and its live quartet's countdown ends: the 0x39 / 0x42, the 0x43 and the 0x68 go to its net
// player and its counts step; the player's own row, short of the live age, runs no quartet.
// Past the player's leave the row's words still step and nothing is sent.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 - the active byte @0x518840, the state
//  test @0x518874..0x518878, the overlay visit @0x5188D6, the frontier send_mask 0xA0
//  @0x518974, the quartet's sends @0x518A67 / @0x518A73 / @0x518A79]
void test_the_slot_walk_takes_an_orphaned_row() {
	OrphanRig r;
	for (w::EntityHandle body : {r.own_body, r.orphan_body})
		if (w::Entity *e = r.world->registry.get(body)) e->damage_state = 5;
	if (w::MatchPlayer *p = r.world->match.player(r.orphan_body))
		p->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	inmatch::NapiNPConnection &o = r.orphan();
	o.reply.frontier_hint_pending = true;
	o.reply.control_request_countdown = 1;
	(void)r.drain();
	inmatch::update_all_active_player_slots(*r.ctx, *r.world);
	const w::Entity *orphan_body = r.world->registry.get(r.orphan_body);
	const w::Entity *own_body = r.world->registry.get(r.own_body);
	CHECK(orphan_body != nullptr && orphan_body->damage_state == 4);
	CHECK(own_body != nullptr && own_body->damage_state == 4);
	CHECK(o.reply.minimap_overlay_cooldown == 13 && o.reply.minimap_pool1_phase == 1);
	CHECK(!o.reply.frontier_hint_pending);
	CHECK(o.reply.control_request_countdown == inmatch::CONTROL_REQUEST_PERIOD_TICKS);
	CHECK(o.reply.charattr_unanswered_count == 1 && o.reply.time_sync_unanswered_count == 1);
	CHECK(r.net_player().reply.charattr_unanswered_count == 0);
	int challenges = 0;
	int latches = 0;
	int pings = 0;
	int pages = 0;
	for (uint8_t t : r.drain()) {
		challenges += t == s2c::CHARATTR_CRC_CHALLENGE ? 1 : 0;
		latches += t == s2c::CHARATTR_DISABLED_PROPERTIES ? 1 : 0;
		pings += t == s2c::TIME_SYNC_PING ? 1 : 0;
		pages += t == s2c::LOADED_MODEL_PAGE_REQUEST ? 1 : 0;
	}
	CHECK(challenges == 1 && latches == 1 && pings == 1 && pages == 1);
	// The player leaves: the row's words step on, nothing is sent.
	r.ctx->np_protocol.connection_list.clear();
	o.reply.control_request_countdown = 1;
	inmatch::update_all_active_player_slots(*r.ctx, *r.world);
	CHECK(orphan_body->damage_state == 3);
	CHECK(o.reply.minimap_overlay_cooldown == 12);
	CHECK(o.reply.charattr_unanswered_count == 2);
	CHECK(r.drain().empty());
}

// The 1 Hz violation sweep walks the slot table: the orphaned row's body has ten suicides, so
// its punt-log arm punts its net player (t6), the player's own row aside.
// [orig: Server_CheckPlayerViolations @0x51ABD0 - the slot walk @0x51ABE0..0x51AD2E, the
//  suicide arm Server_LogCRCMismatchPunt(slot, 6) called @0x51AD17; Server_LogCRCMismatchPunt
//  @0x517ED0 - the row's +0x1C @0x517F47]
void test_the_violation_sweep_takes_an_orphaned_row() {
	OrphanRig r;
	if (w::MatchPlayer *p = r.world->match.player(r.orphan_body))
		p->stats[w::MatchStats::kSuicides] = 10;
	inmatch::check_player_violations(*r.ctx, *r.world);
	CHECK(r.net_player().host_disconnect_sent);
}

// A _PUNT.TXT sink in memory.
class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

// The sweep takes a row in state 6 alone, an orphaned row too: after the round end (every row
// in state 7) the orphaned row's ten suicides punt nobody and write no "#S>9" line, however
// many of the linger's periodic seconds run the sweep. Ahead of the round end the same row
// writes its line and punts its net player.
// [orig: Server_CheckPlayerViolations @0x51ABD0 - `cmp dword ptr [esi+20h], 6` @0x51AC09, jnz
//  @0x51AC0D, the suicide arm's Server_WritePuntLog call @0x51AD0D and its punt @0x51AD17;
//  Server_ProcessRoundEnd - state 7 @0x51685E]
void test_the_violation_sweep_takes_no_row_after_the_round_end() {
	MemoryFiles files;
	inmatch::PuntLog punt_log(files);
	punt_log.set_time_text([] { return std::string("T"); });
	punt_log.arm_punt_log();
	const std::string armed = files.files["_PUNT.TXT"];
	{
		OrphanRig r;
		r.ctx->logs.punt = &punt_log;
		if (w::MatchPlayer *p = r.world->match.player(r.orphan_body))
			p->stats[w::MatchStats::kSuicides] = 10;
		inmatch::Server_ProcessRoundEnd(*r.ctx, *r.world, 1);
		CHECK(r.orphan().slot_state == inmatch::PlayerSlotState::RoundOver);
		for (int second = 0; second < 3; ++second)
			inmatch::check_player_violations(*r.ctx, *r.world);
		CHECK(!r.net_player().host_disconnect_sent);
		CHECK(files.files["_PUNT.TXT"] == armed);
	}
	OrphanRig r;
	r.ctx->logs.punt = &punt_log;
	if (w::MatchPlayer *p = r.world->match.player(r.orphan_body))
		p->stats[w::MatchStats::kSuicides] = 10;
	inmatch::check_player_violations(*r.ctx, *r.world);
	CHECK(r.net_player().host_disconnect_sent);
	CHECK(files.files["_PUNT.TXT"].size() > armed.size());
}

// D-NET-652: the integrity family's gate reads the row's own words alone, so a punted
// state-6 row (its description gone, its teardown not yet run) still takes its draw, one
// PRNG_Next16, while its closed queue drops the request: the stream steps exactly once and
// the row's transport carries nothing.
// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 - state 6 @0x508580, the entity
//  @0x50858A, PRNG_Next16 called @0x5085AE behind +0x15F40 == 0 @0x5085A6, no punt test;
//  CNapiNPConnection_QueueMessage @0x628640 - the +0x70D test @0x62864D]
void test_a_punted_row_takes_its_integrity_draw() {
	OrphanRig r;
	r.ctx->orphaned_player_rows.clear(); // the player's own row alone
	if (w::MatchPlayer *p = r.world->match.player(r.own_body))
		p->stats[w::MatchStats::kSuicides] = 10;
	inmatch::check_player_violations(*r.ctx, *r.world); // the suicide arm punts it (t6)
	CHECK(r.net_player().host_disconnect_sent);
	CHECK(r.net_player().slot_state == inmatch::PlayerSlotState::Joined);
	(void)r.drain();
	r.ctx->scoreboard_broadcast_timer = 0x136u; // the boundary is the next step
	r.ctx->integrity_entity_family_next = true;
	uint32_t want = r.world->prng16_state;
	(void)io::rotating_prng_next16(want);
	inmatch::emit_scoreboard_boundary(*r.ctx, *r.world);
	CHECK(r.world->prng16_state == want);
	CHECK(r.drain().empty());
}

// D-NET-1404: a C2S 0x22 naming an orphaned row's slot answers the row's record (its slot, its
// body), the PCID read through the row's live net player and empty once the player has left; the
// row is active with a body, so it is never the 0x8000 removal record.
// [orig: NapiNPServerMsg_0x022 @0x514C90 - g_PlayerSlots[slot] @0x514CF9..0x514CFF;
//  NetPacket_SerializePlayerSync0x46 @0x505E80 - the removal tests @0x505ECB / @0x505EDC, the
//  PCID through the net player @0x506041..0x5060AC]
void test_the_player_sync_reply_takes_an_orphaned_row() {
	OrphanRig r;
	r.ctx->config.max_players = 8; // slots 2 and 5 in the table
	r.net_player().account.pcid = "PC-NOW";
	r.net_player().account.squad_id = 77;
	r.orphan().account.pcid = "PC-OLD";
	// The asker: another added player.
	inmatch::NapiNPConnection asker = row(4, 2, 0);
	asker.connection_id = 9;
	r.ctx->np_protocol.connection_list.push_back(asker);
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = r.ctx.get();
	const auto ask = [&](uint8_t slot, PlayerSync &out) {
		inmatch::NapiNPConnection &from = r.ctx->np_protocol.connection_list.back();
		const std::vector<ProtocolMessage> msgs{
				make_protocol_message(c2s::PLAYER_SYNC_REQUEST, {slot, 0x10, 0x08})};
		const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				r.ctx->config, from, msgs, 100, r.ctx->np_protocol.connection_list, r.world.get(),
				inputs);
		return replies.size() == 1 && replies[0].tag == s2c::PLAYER_SYNC &&
				decode_player_sync(replies[0].payload.data(), replies[0].payload.size(), out);
	};
	PlayerSync sync;
	CHECK(ask(2, sync) && !sync.removal && sync.slot_id == 2 && sync.id_label == "PC-NOW" &&
			sync.account_id == 77 &&
			sync.entity_slot_id == static_cast<uint8_t>(r.orphan_body.packed & 0xFFu));
	// The player leaves: the row still answers, through no net player.
	r.ctx->np_protocol.connection_list.erase(r.ctx->np_protocol.connection_list.begin());
	sync = PlayerSync{};
	CHECK(ask(2, sync) && !sync.removal && sync.slot_id == 2 && sync.id_label.empty() &&
			sync.account_id == 0);
	// A slot no row holds answers the removal record.
	sync = PlayerSync{};
	CHECK(ask(5, sync) && sync.removal);
}

// A chat line's walk is the slot table's: an orphaned row the line admits sends it to the row's
// net player, which so takes a copy for its own row and one for the orphaned row; past the
// player's leave the row has no net player and sends nothing.
// [orig: NapiNPServer_HandleChatMessage @0x513760 - the everyone arm's loop over g_PlayerSlots
//  (the row's +0x1C @0x5140AA, its state @0x5140B7), the send's mask 0x20;
//  NapiNPServer_SendFiltered @0x4C8A1F]
void test_a_chat_line_reaches_an_orphaned_rows_net_player() {
	OrphanRig r;
	r.net_player().burst.game_state = 10; // the join's
	ns::UdpSessionTransport sender_transport{ns::UdpSessionTransport::Role::Host};
	w::PlayerSpawn s;
	s.position = {30.0f, 10.0f, 2.0f};
	s.team = 1;
	inmatch::NapiNPConnection sender = row(4, 1, 0);
	sender.burst.spawned = true;
	sender.burst.game_state = 10;
	sender.slot_state = inmatch::PlayerSlotState::Joined;
	sender.link.owned_entity = w::spawn_remote_player(*r.world, s);
	sender.link.transport = &sender_transport;
	sender.link.mode = ns::TransportMode::Client;
	sender.reply.player_name = "Sender";
	r.ctx->np_protocol.connection_list.push_back(sender);
	ChatUplink line;
	line.channel = 1;
	line.text = "hello";
	const auto chat_count = [&]() {
		size_t n = 0;
		ns::Datagram d;
		while (r.transport.pop_outbound(d)) n += d.tag == s2c::CHAT_BROADCAST ? 1 : 0;
		return n;
	};
	(void)inmatch::Server_HandleChatMessage(*r.ctx, r.ctx->np_protocol.connection_list.back(),
			line, 5000, *r.world);
	CHECK(chat_count() == 2); // its own row's copy and the orphaned row's
	// The player leaves: the orphaned row has no net player, the line reaches no one there.
	r.ctx->np_protocol.connection_list.erase(r.ctx->np_protocol.connection_list.begin());
	(void)inmatch::Server_HandleChatMessage(*r.ctx, r.ctx->np_protocol.connection_list.back(),
			line, 7000, *r.world);
	CHECK(chat_count() == 0);
}

// The radio call's per-slot fan walks the slot table too: a ruled call reaches an orphaned row on
// the sender's slot team through the row's net player, a copy per row.
// [orig: NapiNPServerMsg_HandleRadioCall - the walk @0x514510..0x5147EC, the row's +0x1C
//  @0x51452C, NetPlayer +0xA0 == 10 @0x514537]
void test_a_ruled_radio_call_reaches_an_orphaned_rows_net_player() {
	OrphanRig r;
	r.net_player().burst.game_state = 10; // the join's
	ns::UdpSessionTransport sender_transport{ns::UdpSessionTransport::Role::Host};
	w::PlayerSpawn s;
	s.position = {30.0f, 10.0f, 2.0f};
	s.team = 1;
	inmatch::NapiNPConnection sender = row(4, 1, 0);
	sender.burst.spawned = true;
	sender.burst.game_state = 10;
	sender.slot_state = inmatch::PlayerSlotState::Joined;
	sender.link.owned_entity = w::spawn_remote_player(*r.world, s);
	sender.link.transport = &sender_transport;
	sender.link.mode = ns::TransportMode::Client;
	r.ctx->np_protocol.connection_list.push_back(sender);
	RadioCallRequest request;
	request.value = 7; // RAD_7: a rule (type -1), so the per-slot fan
	const std::vector<ProtocolMessage> own = inmatch::Server_HandleRadioCall(r.ctx.get(),
			r.ctx->np_protocol.connection_list.back(), request, r.ctx->np_protocol.connection_list,
			*r.world);
	size_t calls = 0;
	ns::Datagram d;
	while (r.transport.pop_outbound(d)) calls += d.tag == s2c::TRACKED_PLAYER_VOICE ? 1 : 0;
	CHECK(own.size() == 1 && calls == 2); // the sender's own; the net player's row and the orphan's
}

} // namespace

int main() {
	test_the_imbalance_counts_an_orphaned_row();
	test_auto_balance_moves_an_orphaned_row_by_its_own_add_stamp();
	test_auto_balance_keys_every_row_by_its_add_stamp();
	test_the_swap_arm_swaps_an_orphaned_row();
	test_an_orphaned_spectator_rows_body_is_hidden();
	test_the_net_player_is_bound_by_identity();
	test_the_play_ticks_count_an_orphaned_row();
	test_the_net_quality_walk_samples_an_orphaned_row();
	test_the_net_quality_walk_ends_nine_rows_past_a_sampled_row();
	test_the_periodic_walk_takes_an_orphaned_row();
	test_the_downed_resend_takes_an_orphaned_row();
	test_the_integrity_family_takes_an_orphaned_row();
	test_the_round_end_takes_an_orphaned_row();
	test_the_slot_walk_takes_an_orphaned_row();
	test_the_violation_sweep_takes_an_orphaned_row();
	test_the_violation_sweep_takes_no_row_after_the_round_end();
	test_a_punted_row_takes_its_integrity_draw();
	test_the_player_sync_reply_takes_an_orphaned_row();
	test_a_chat_line_reaches_an_orphaned_rows_net_player();
	test_a_ruled_radio_call_reaches_an_orphaned_rows_net_player();
	if (failures != 0) {
		std::printf("npruntime_orphan_row_walks: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_orphan_row_walks: ok\n");
	return 0;
}
