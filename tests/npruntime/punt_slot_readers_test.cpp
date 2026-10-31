// D-NET-652: a punt writes no slot byte, so the readers of the player slot itself keep reading a
// punted slot as in match (state 6) until the teardown's row memset; only the send fans stop at
// the punt (is_in_match's punt leg). Across a punt: the squad break-up still dissolves a punted
// member's link, the play-tick walk still counts it, and the breath sampler still samples it;
// the round end's state 7 stops the sampler as it stops the walk.
// [orig: CNapiNPConnection_QueueDisconnectEvent @0x4C7EF0 (the event alone);
//  Server_HandlePlayerDisconnect @0x51B87D (the row memset); Server_SendPlayerStateAndSquad
//  @0x518C45..0x518C60; Server_TickUpdate @0x51D960..0x51D977;
//  Server_UpdatePlayerBreathTimers @0x50D7A2..0x50D7BB]

#include "npruntime/conn_fixture.h"

#include <base/gameprofile/game_type.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_idle_timers.h>
#include <runtime/inmatch/server_squad.h>
#include <runtime/inmatch/server_tick.h> // Server_StageHostPunt
#include <runtime/inmatch/server_tick_maintenance.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include "../common/title_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

namespace {

int g_failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++g_failures;                                                            \
		}                                                                            \
	} while (0)

w::PlayerSpawn spawn_at(float x, uint8_t team) {
	w::PlayerSpawn s{};
	s.position = {x, 0.0f, 0.0f};
	s.team = team;
	return s;
}

} // namespace

int main() {
	auto world_owner = std::make_unique<w::World>();
	test_titles::jo_world(*world_owner);
	w::World &world = *world_owner;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	w::MatchRules rules;
	test_titles::jo_match(rules);
	rules.game_type = game_type::kTeamDeathmatch;
	world.match.configure(rules);
	const w::EntityHandle leader_body = w::spawn_remote_player(world, spawn_at(0.0f, 1));
	const w::EntityHandle member_body = w::spawn_remote_player(world, spawn_at(10.0f, 1));
	world.match.upsert_player({leader_body, 1, "Leader"});
	world.match.upsert_player({member_body, 1, "Member"});

	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ns::UdpSessionTransport leader_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport member_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(conn_fixture::make_seeded_conn(1, 1, &leader_wire, ns::TransportMode::Client,
			leader_body, true));
	roster.push_back(conn_fixture::make_seeded_conn(2, 1, &member_wire, ns::TransportMode::Client,
			member_body, true));
	inmatch::NapiNPConnection &leader = roster[0];
	inmatch::NapiNPConnection &member = roster[1];
	leader.reply.player_slot = 1;
	member.reply.player_slot = 2;
	leader.assigned_team = member.assigned_team = 1;
	leader.assigned_team_valid = member.assigned_team_valid = true;
	member.squad_leader = 1;

	// The member is punted: its disconnect event is staged, the send fans close.
	CHECK(inmatch::Server_StageHostPunt(member, 7));
	CHECK(!inmatch::is_in_match(member) && inmatch::slot_in_match(member));

	// The play-tick walk counts the punted slot until the teardown.
	const uint32_t ticks_before = world.match.player(member_body)->play_ticks;
	inmatch::advance_play_ticks(ctx, world);
	CHECK(world.match.player(member_body)->play_ticks == ticks_before + 1);

	// The breath sampler reaches it: its first test past the slot's (the dead flag clears the
	// counter) runs on the punted slot. [orig: @0x50D7C3..0x50D7C7]
	w::Entity *member_entity = world.registry.get(member_body);
	CHECK(member_entity != nullptr);
	member_entity->flags |= w::kEntityFlagDead;
	member.link.underwater_breath_samples = 5;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
	CHECK(member.link.underwater_breath_samples == 0);

	// The squad break-up's member test still sees the punted member in state 6: the leader's
	// break-up dissolves its link.
	inmatch::Server_DissolveSquadOf(ctx, leader);
	CHECK(member.squad_leader == 0xFF);

	// The round end's state 7: the sampler no longer reaches a slot (state 6 exactly).
	CHECK(world.match.finish(1, world));
	member.link.underwater_breath_samples = 5;
	inmatch::Server_UpdatePlayerBreathTimers(ctx, world);
	CHECK(member.link.underwater_breath_samples == 5);

	std::printf("punt_slot_readers: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
