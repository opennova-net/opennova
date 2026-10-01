// The authority's status page feed (inmatch/server_status_feed.h): the
// player-slot roster off the connection list (the active, local, in-game,
// loading, spectator, team, class, dead and idle fields; the stats), the
// slot table's capacity and limit, the server line's facts, the round
// tallies, the team records, the clocks and the total logins — the host's
// own connection and every joined remote counting one.
// [orig: Server_DrawStatusScreen @0x50a2d0; NapiNPServer_HandleNewConnection
//  @0x4c8203; CNapiNetwork_RandomizeTimeout @0x4c4da3]
#include "npruntime/conn_fixture.h"
#include "npruntime/host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_status_feed.h>
#include <runtime/world/world.h>

#include <cstdio>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

world::EntityHandle player(world::World &world, uint8_t slot, uint8_t team, uint8_t cls,
		int32_t health, const char *name) {
	world::Entity e;
	e.kind = world::EntityKind::Organic;
	e.player_class = cls;
	e.team = team;
	e.health = health;
	e.alive = health > 0;
	e.flags = world::kEntityFlagPlayer;
	e.engine_flags = world::kEntityFlagPlayer;
	const world::EntityHandle h = world.registry.spawn(0, e);
	world.match.upsert_player({h, slot, name});
	return h;
}

void test_roster_and_facts() {
	world::World world;
	world.registry.configure_pool(0, 8);
	world::MatchRules rules;
	rules.game_type = 0x10000u;
	world.match.configure(rules);
	world.preround_delay_seconds = 12;
	const world::EntityHandle host = player(world, 0, 1, 8, 100, "Host");
	const world::EntityHandle joe = player(world, 2, 2, 5, 0, "Joe");
	world.match.player(joe)->stats[world::MatchStats::kPoints] = 9;
	world.match.player(joe)->stats[world::MatchStats::kFlagCaptures] = 2;
	world.match.player(joe)->objective_ticks = 44;

	replication::LoopbackChannel host_wire;
	replication::LoopbackChannel joe_wire;
	replication::LoopbackChannel loading_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_mp_session_peer = 1;
	ctx.config.max_players = 4;
	ctx.config.server_name = "srv";
	ctx.transport_mode = inmatch::NetworkType::NovaWorld;
	ctx.novaworld_app_id = 4321;
	ctx.round_wins = {3, 1, 0, 0};
	ctx.rounds_played = 5;
	ctx.total_logins = 7;
	ctx.stats_frames_last_second = 61;
	ctx.stats_cpu_percent = 13;
	inmatch::NapiNPConnection h = conn_fixture::make_conn(1, 2, &host_wire,
			replication::TransportMode::Loopback, host, true);
	h.reply.player_slot = 0;
	h.reply.player_name = "Host";
	h.receive_inactive_ms = 9000; // the host's own slot has no reap clock
	ctx.np_protocol.connection_list.push_back(h);
	inmatch::NapiNPConnection j = conn_fixture::make_conn(2, 1, &joe_wire,
			replication::TransportMode::Client, joe, true);
	j.reply.player_slot = 2;
	j.reply.player_name = "Joe";
	j.receive_inactive_ms = 3500;
	ctx.np_protocol.connection_list.push_back(j);
	// A joiner added but still loading (no completed burst), spectating.
	inmatch::NapiNPConnection l = conn_fixture::make_conn(3, 1, &loading_wire,
			replication::TransportMode::Client, world::EntityHandle{}, false);
	l.phase = inmatch::ConnectionPhase::PlayerAdded;
	l.reply.player_slot = 3;
	l.reply.player_name = "Lo";
	l.link.spectator = true;
	l.assigned_team_valid = true;
	l.assigned_team = 1;
	ctx.np_protocol.connection_list.push_back(l);
	// A connection still holding only a reservation is not a slot row.
	inmatch::NapiNPConnection r = conn_fixture::make_conn(4, 1, nullptr,
			replication::TransportMode::Client, world::EntityHandle{}, false);
	r.phase = inmatch::ConnectionPhase::PlayerAdded;
	r.reply.player_slot = 1;
	r.reply.player_slot_reserved = true;
	ctx.np_protocol.connection_list.push_back(r);

	hud::ServerStatusPageState page;
	inmatch::fill_server_status_page(page, ctx, &world);
	CHECK(page.mp_session_peer && page.capacity == 4 && page.slot_limit == 4 &&
			page.slots.size() == 4);
	const hud::ServerStatusSlot &s0 = page.slots[0];
	CHECK(s0.active && s0.local && s0.in_game && !s0.loading && s0.team == 1 &&
			s0.class_word == 8 && !s0.entity_dead && s0.idle_seconds == 0 && s0.name == "Host");
	CHECK(!page.slots[1].active);
	const hud::ServerStatusSlot &s2 = page.slots[2];
	CHECK(s2.active && !s2.local && s2.in_game && s2.team == 2 && s2.class_word == 5 &&
			s2.entity_dead && s2.idle_seconds == 3 && s2.points == 9 && s2.flag_captures == 2 &&
			s2.objective_seconds == 44 && !s2.bot && s2.status_word == 0);
	const hud::ServerStatusSlot &s3 = page.slots[3];
	CHECK(s3.active && !s3.in_game && s3.loading && s3.spectator && s3.team == 1 &&
			s3.class_word == 0 && s3.name == "Lo");
	CHECK(page.novaworld && page.session_key == 4321 && page.server_name == "srv" &&
			page.game_type == 0x10000u);
	CHECK(page.round_wins_team1 == 3 && page.round_wins_team2 == 1 && page.rounds_played == 5 &&
			page.total_logins == 7 && page.frames == 61 && page.cpu_percent == 13);
	CHECK(page.pre_round_delay == 12 && page.round_time_remaining == world.match.remaining_ticks());
	// A dedicated host is not a peer; a LAN host carries no NovaWorld line.
	ctx.is_mp_session_peer = 0;
	ctx.transport_mode = inmatch::NetworkType::Lan;
	inmatch::fill_server_status_page(page, ctx, &world);
	CHECK(!page.mp_session_peer && !page.novaworld);
}

// The total logins: the host's own loopback coming up counts one, every
// server start zeroes the count first [orig: CNapiServer_OnHostStarted
// @0x4c94f0].
void test_total_logins() {
	inmatch::NapiNPServerCtx ctx;
	replication::LoopbackChannel local;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Lan, 0, &local);
	CHECK(ctx.total_logins == 1);
	ctx.total_logins = 5;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Lan, 0, &local);
	CHECK(ctx.total_logins == 1);
	inmatch::NapiNPServerCtx dedicated;
	inmatch::test::bring_up_host(dedicated, inmatch::ConnectionMode::HostOnly,
			inmatch::SocketMode::Lan);
	CHECK(dedicated.total_logins == 0);
}

} // namespace

int main() {
	test_roster_and_facts();
	test_total_logins();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("server_status_feed_test OK\n");
	return 0;
}
