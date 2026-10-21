// D-NET-352: a Serve Only host's own slot 0 has a body. Every mission start's round init
// walks every active slot, the host's own row first, and spawns its entity with the slot's
// class and team, owner connection 0, at a spawn marker; it binds as the host's local player
// (NapiNP_GetLocalConnectionId returns 0 with no client connection) and the mission start
// hides it, which no 0x0A writer clears for a row with no net player. So the row's body takes
// the first free pool-0 entry ahead of every joiner (a joiner's handle is one entry later than
// without it), every joiner's 0x0C carries it hidden owned by connection 0, and the walks
// meet it: PLAYER KILL 0 runs its death transaction (the 0x13 to every joiner), SWAPTEAM 0
// moves its team byte with the row's.
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - @0x516AE0..0x516AE4,
//  @0x516B56..0x516B8E; NapiNP_GetLocalConnectionId @0x4C6D40..0x4C6D4A; Game_StartMission
//  @0x525C95..0x525C9E; Entity_ValidatePtr @0x500910 (the row as the body's slot);
//  Server_ChangeEntityTeam @0x518D70; Server_BuildEntityPriorityList's hidden shift
//  @0x50EB83]
// The world and its joiner are the test's own.

#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_player_death.h>
#include <runtime/inmatch/server_spawn.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "host_test_setup.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

struct Rig {
	std::unique_ptr<w::World> wp = std::make_unique<w::World>();
	w::World &world = *wp;
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ns::LoopbackChannel wire;

	Rig() {
		world.registry.configure_pool(0, 8);
		world.rules.mp_session = true;
		// The Player row a body binds (no items.def here): a row with a def, which the
		// 0x0C does not pass over (D-NET-487).
		world.tables.player.has_item_def = true;
		world.tables.player.item_type_index = 1;
		world.tables.player.item_type = 3;
		inmatch::GameConfig settings;
		settings.max_players = 4;
		settings.game_type = 0x10000u; // a team game
		inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Socketless, /*host_key=*/0, nullptr, settings);
		ctx->world = &world;
		ctx->is_in_session = 1;
		inmatch::Server_InitNewRoundState(*ctx);
	}
	inmatch::NapiNPConnection &add_joiner() {
		inmatch::NapiNPConnection joiner;
		joiner.type = 1;
		joiner.connection_id = inmatch::kFirstJoinerDcb;
		joiner.self_id_seen = true;
		joiner.phase = inmatch::ConnectionPhase::Joined;
		ctx->np_protocol.connection_list.push_back(joiner);
		CHECK(inmatch::Server_ProcessPendingPlayerSpawns(*ctx, world) == 1);
		inmatch::NapiNPConnection &c = ctx->np_protocol.connection_list.back();
		c.link.transport = &wire;
		c.link.mode = ns::TransportMode::Client;
		c.burst.spawned = true;
		return c;
	}
};

void test_the_row_body_is_first_hidden_and_local() {
	Rig r;
	const inmatch::HostSlotRow &host = r.ctx->host_slot;
	CHECK(host.active && host.team == 1 && host.player_class == 8);
	const w::EntityHandle body = inmatch::Server_SpawnHostSlotBody(*r.ctx, r.world);
	CHECK(body.valid() && body == host.body);
	CHECK(body.pool() == 0 && body.slot() == 0);
	const w::Entity *e = r.world.registry.get(body);
	CHECK(e != nullptr);
	if (e == nullptr) return;
	CHECK((e->flags & w::kEntityFlagCarried) != 0 && (e->flags & w::kEntityFlagPlayer) != 0);
	CHECK(e->owner_connection_id == 0 && e->team == 1 && e->player_class == 8);
	CHECK(e->name == host.name && e->player_slot_id == 0);
	// The round init's 620-tick spawn protection, which no pass counts down for the row
	// [orig: Server_InitAllPlayerEntitiesForRound @0x516BBA].
	CHECK(e->damage_state == 620);
	CHECK(r.world.cached.local_player == body);
	// A joiner's body takes the next entry.
	const inmatch::NapiNPConnection &joiner = r.add_joiner();
	CHECK(joiner.link.owned_entity.pool() == 0 && joiner.link.owned_entity.slot() == 1);
	// Every joiner's 0x0C carries the hidden row body, owned by connection 0.
	bool seen = false;
	for (const OrganicSpawnRecord &rec : replication::build_pool0_organic_batch(r.world).records) {
		if (rec.slot_id != body.packed) continue;
		seen = true;
		CHECK(rec.owner_connection_id == 0 && (rec.minimap_flags & 1u) != 0);
	}
	CHECK(seen);
}

// The row's side bytes and class are the create's profile init's from the profile the host
// runs under (a game-launched Serve Only host's player profile), the class by the side
// requirement (-1: side B's, as the listen host's own slot picks it); the round init left a
// fresh record's (D-NET-352's FOUND 5).
// [orig: CNapiGameSession_CreateSession @0x4C9952..0x4C996C, @0x4C9C9A..0x4C9CCE ->
//  PlayerSession_InitFromProfile @0x50CA80 - @0x50CB67..0x50CBA2, the class @0x50CB7C]
void test_the_row_takes_the_hosts_profile() {
	Rig r;
	CHECK(r.ctx->config.host_side_request == -1);
	inmatch::CharacterJoinVars profile;
	profile.char_id[0] = 0x0301;
	profile.char_id[1] = 0x0402;
	profile.char_class[0] = 5;
	profile.char_class[1] = 6;
	profile.avatar[0] = 3;
	profile.avatar[1] = 4;
	inmatch::PlayerSession_InitHostRowFromProfile(*r.ctx, profile);
	const inmatch::HostSlotRow &host = r.ctx->host_slot;
	CHECK(host.player_class == 6);
	const w::EntityHandle body = inmatch::Server_SpawnHostSlotBody(*r.ctx, r.world);
	const w::Entity *e = r.world.registry.get(body);
	CHECK(e != nullptr);
	if (e == nullptr) return;
	// Team 1: side A's char id and avatar.
	CHECK(e->team == 1 && e->player_class == 6);
	CHECK(e->minimap_net_id == 0x0301 && e->anim_slot == 3);
}

void test_player_kill_0_runs_the_rows_transaction() {
	Rig r;
	w::MatchRules rules;
	rules.game_type = r.ctx->config.game_type; // a team game
	r.world.match.configure(rules);
	const w::EntityHandle body = inmatch::Server_SpawnHostSlotBody(*r.ctx, r.world);
	(void)r.add_joiner();
	ns::Datagram drained;
	while (r.wire.client_recv(drained)) {
	}
	inmatch::Server_RunPlayerDeath(*r.ctx, r.world, body);
	bool death = false;
	ns::Datagram dg;
	while (r.wire.client_recv(dg))
		if (dg.tag == s2c::ENTITY_DEATH && dg.body.size() >= 2 &&
				static_cast<uint16_t>(dg.body[0] | (dg.body[1] << 8)) == body.packed)
			death = true;
	CHECK(death);
	// The scorer resolves the row: one Death on its CPlayerStats (PLAYER LIST's Deaths
	// column) and on its team record [orig: GameEvent_PlayerDeath @0x516F06 ->
	//  GameEvent_ProcessScoring @0x53074E..0x5307A7].
	CHECK(r.ctx->host_slot.stats[w::MatchStats::kDeaths] == 1);
	CHECK(r.world.match.team_stats(r.ctx->host_slot.team)[w::MatchStats::kDeaths] == 1);
}

} // namespace

int main() {
	test_the_row_body_is_first_hidden_and_local();
	test_the_row_takes_the_hosts_profile();
	test_player_kill_0_runs_the_rows_transaction();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_host_slot_body\n");
	return 0;
}
