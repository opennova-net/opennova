// The player placement's retail legs (D-NET-NEW-L4-1, D-NET-431, D-NET-503's
// residual): a hidden player body runs no update at all; an add that finds no
// start marker keeps the zeroed entity's words (position 0, heading word 0); a
// deploy whose pick finds no marker leaves the body where it stands; and the
// scored arm, a revive and the deploy's hold arm warm the body up (the organic
// warm-up's dual updates, root rise and ground solve).
// [orig: Entity_UpdateInfantryPlayerBody - the hidden-bit return
//  @0x4B411E..0x4B4127; Entity_SpawnFromAnimSlotProperty - the memset @0x43C3FE,
//  the placement @0x43C459; Server_PositionPlayerForSpawn -
//  Entity_FindBestSpawnPoint @0x50D56C, the warm-up @0x50D572, the revive's
//  @0x50D62D; Entity_FindBestSpawnPoint - no marker, no store @0x50CD20;
//  Server_DeployPlayer - the hold arm's warm-up @0x517842;
//  Entity_WarmUpOrganicAnimation @0x4B8B20]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "conn_fixture.h"
#include "host_test_setup.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A TDM host with two remote players, no start markers and no spawn zones.
struct Fixture {
	inmatch::NapiNPServerCtx ctx;
	std::unique_ptr<w::World> heap = std::make_unique<w::World>();
	w::World &world = *heap;
	std::vector<ns::UdpSessionTransport> transports;
	std::vector<w::EntityHandle> players;

	Fixture() {
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.game_type = 0x10000u; // TDM
		ctx.config.respawn_timeout = 0;
		world.rules.mp_session = true;
		world.rules.logic_authority = true;
		world.registry.configure_pool(0, 16);
		world.registry.configure_pool(3, 16);
		ctx.world = &world;
		transports.reserve(2);
		for (size_t i = 0; i < 2; ++i) {
			transports.emplace_back(ns::UdpSessionTransport::Role::Host);
			w::PlayerSpawn spawn;
			spawn.position = {10.0f * float(i + 1), 0.0f, 0.0f};
			spawn.team = static_cast<uint8_t>(i + 1);
			players.push_back(w::spawn_remote_player(world, spawn));
		}
		for (size_t i = 0; i < 2; ++i) {
			inmatch::NapiNPConnection c = conn_fixture::make_seeded_conn(
					static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i), 1, &transports[i],
					ns::TransportMode::Client, players[i], true);
			c.reply.player_slot = static_cast<uint8_t>(i + 1);
			c.assigned_team = static_cast<uint8_t>(i + 1);
			c.assigned_team_valid = true;
			c.admission_stage = inmatch::GameAdmissionStage::Complete;
			ctx.np_protocol.connection_list.push_back(std::move(c));
		}
	}
	inmatch::NapiNPConnection &conn(size_t i) { return ctx.np_protocol.connection_list[i]; }
	w::Entity &entity(size_t i) { return *world.registry.get(players[i]); }
};

// D-NET-503's residual: a hidden player body (a held slot's) runs no update, so
// its think cadence word stands; unhidden, the update runs again.
// [orig: Entity_UpdateInfantryPlayerBody `test dl,1; jnz` @0x4B411E..0x4B4127,
//  the cadence @0x4B4BC9..0x4B4BE9]
void test_a_hidden_player_body_runs_nothing() {
	Fixture f;
	f.conn(0).link.respawn_pending = true; // a held slot, whose body stays hidden
	f.entity(0).flags |= w::kEntityFlagCarried;
	f.entity(0).spawn_phase = 5;
	f.entity(1).spawn_phase = 5;
	inmatch::Server_TickUpdate(f.ctx);
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.entity(0).spawn_phase == 5);
	CHECK(f.entity(1).spawn_phase == 3);
	f.conn(0).link.respawn_pending = false;
	f.entity(0).flags &= ~w::kEntityFlagCarried;
	inmatch::Server_TickUpdate(f.ctx);
	CHECK(f.entity(0).spawn_phase == 4);
}

// D-NET-NEW-L4-1: a deploy whose no-pick arm finds no start marker leaves the
// body where it died; the spawn-state reset then takes that spot as its origin.
// [orig: Server_PositionPlayerForSpawn - Entity_FindBestSpawnPoint @0x50D56C;
//  Entity_FindBestSpawnPoint - matchCount 0 @0x50CD20, no store;
//  Entity_ResetToSpawnState @0x4B9632..0x4B9644]
void test_a_markerless_deploy_leaves_the_body_where_it_stands() {
	Fixture f;
	w::Entity &body = f.entity(0);
	const w::Vec3 spawned = body.spawn_position;
	body.position = {33.0f, -21.0f, 4.0f};
	body.flags |= w::kEntityFlagDead;
	body.engine_flags |= w::kEntityFlagDead;
	body.health = 0;
	CHECK(!(spawned.x == 33.0f && spawned.y == -21.0f));
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &f.ctx;
	(void)inmatch::dispatch_session_replies(f.ctx.config, f.conn(0),
			{make_protocol_message(c2s::RESPAWN_REQUEST, {0xFF, 0xFF})}, 100,
			f.ctx.np_protocol.connection_list, &f.world, inputs);
	CHECK((body.flags & w::kEntityFlagDead) == 0u);
	CHECK(body.health > 0);
	CHECK(body.position.x == 33.0f && body.position.y == -21.0f && body.position.z == 4.0f);
	CHECK(body.spawn_position.x == 33.0f && body.spawn_position.y == -21.0f);
}

// D-NET-NEW-L4-1: a deathmatch add with no start marker keeps the zeroed
// entity's words: position 0 and heading word 0, whose mission yaw is 90. The
// host's own body and a joiner's alike, so every S2C 0x0A player record ships
// yaw byte 0x00, the heading word's high byte (D-NET-NEW-LP-1: 03TR's retail
// host sends 0x00 for both bodies; the add's old heading 16384 sent 0x40).
// [orig: Entity_SpawnFromAnimSlotProperty - the memset @0x43C3FE, the
//  placement @0x43C459; Server_PositionPlayerForSpawn - the 6095 / 6002 arms
//  @0x50D234 / @0x50D310, Entity_FindBestSpawnPoint @0x50D56C;
//  NetPacket_SerializePlayerState - the yaw byte @0x4C0C5D]
void test_a_markerless_add_takes_heading_word_zero() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 16);
	world->registry.configure_pool(3, 16);
	ns::LoopbackChannel loop;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.server_name = "DM";
	settings.max_players = 2;
	settings.game_type = 0; // deathmatch: the 6095 / 6002 chain
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, /*host_key=*/0, &loop, settings);
	ctx.world = world.get();
	inmatch::Server_InitNewRoundState(ctx);
	CHECK(inmatch::Server_ProcessPendingPlayerSpawns(ctx, *world) == 1);
	const w::Entity *player = world->registry.get(world->cached.local_player);
	const w::AiEntity *motor = world->ai.for_handle(world->cached.local_player);
	CHECK(player != nullptr && motor != nullptr);
	if (player == nullptr || motor == nullptr) return;
	CHECK(player->position.x.q16 == 0 && player->position.y.q16 == 0);
	CHECK(motor->heading == 0);
	CHECK(player->yaw == 90);
	// A joiner's add on the same markerless mission: the same words.
	ns::UdpSessionTransport joiner_transport(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			inmatch::kFirstJoinerDcb, inmatch::NapiNPConnection::kTypeServerSide,
			&joiner_transport, ns::TransportMode::Client, w::EntityHandle{}, false));
	const w::EntityHandle joiner_h = inmatch::Server_BuildPlayerInfoAndAdd(
			ctx, ctx.np_protocol.connection_list.back(), *world, /*round_init=*/false);
	const w::Entity *joiner = world->registry.get(joiner_h);
	const w::AiEntity *joiner_motor = world->ai.for_handle(joiner_h);
	CHECK(joiner != nullptr && joiner_motor != nullptr);
	if (joiner == nullptr || joiner_motor == nullptr) return;
	CHECK(joiner->position.x.q16 == 0 && joiner->position.y.q16 == 0);
	CHECK(joiner_motor->heading == 0);
	// The 0x0A player record's yaw byte, both bodies.
	player = world->registry.get(world->cached.local_player);
	CHECK((static_cast<uint32_t>(ns::snapshot_of(*player).euler_z) >> 24) == 0x00u);
	CHECK((static_cast<uint32_t>(ns::snapshot_of(*joiner).euler_z) >> 24) == 0x00u);
	// The pick found no marker and still asks for the warm-up.
	const w::SpawnPointResult pick = w::resolve_player_spawn_pose(*world,
			w::EntityHandle{}, w::EntityHandle{}, 0, 0, 0);
	CHECK(!pick.found && pick.warm_up);
}

// A root-motion source whose every clip lifts the root and stands its capsule
// one unit over the origin.
struct Source final : w::IRootMotionSource {
	bool has_clip(int, int) const override { return true; }
	int32_t clip_length_ticks(int, int, int) const override { return 10000; }
	bool advance(int, int, int32_t &phase, w::RootMotionFrame &out) override {
		++phase;
		out = {};
		out.dz = 100;
		out.capsule_bottom = 65536;
		out.capsule_top = 2 * 65536;
		return true;
	}
};

// D-NET-431: the warm-up on a player body with its animation channel lifts an
// unparented body by its root motion and capsule bottom (no collision here, so
// no ground solve), leaves a parented one's height, and does nothing without a
// channel. [orig: Entity_WarmUpOrganicAnimation @0x4B8B20 - the channel test
//  @0x4B8B32..0x4B8B39, the loop's `add [esi+0Ch]` @0x4B8B7E, the parent test
//  @0x4B8BA5, the final bottom @0x4B8BB4]
void test_the_warm_up_lifts_an_unparented_player_body() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 16);
	Source source;
	w::PlayerSpawn spawn;
	spawn.position = {5.0f, 6.0f, 7.0f};
	spawn.net_id = 0; // a player carries no SSN, as the add stamps it (D-NET-112)
	const w::EntityHandle h = w::spawn_remote_player(*world, spawn);
	w::Entity *body = world->registry.get(h);
	w::AiEntity *motor = world->ai.for_handle(h);
	CHECK(body != nullptr && motor != nullptr);
	if (body == nullptr || motor == nullptr) return;
	const int32_t z0 = body->position.z.q16;
	w::warm_up_player_body(*world, *body); // no root-motion source: no channel
	CHECK(body->position.z.q16 == z0);
	world->ai.root_motion = &source;
	motor->inf.adm_id = 0;
	CHECK(motor->net_id == 0); // 10 dual updates
	w::warm_up_player_body(*world, *body);
	// The first update's rise is the clip's 100 (no previous capsule bottom);
	// each later update's is the bottom's change, 0 here; then the last
	// frame's bottom (65536) and the extra update's change (0).
	// [orig: Entity_WarmUpOrganicAnimation - the count @0x4B8B3F..0x4B8B56,
	//  the per-update add @0x4B8B7E, the bottom @0x4B8BB4, the last update's
	//  delta @0x4B8BD2; AnimMap_UpdateEntity - the delta against slot+76
	//  @0x40B88E..0x40B899, its store @0x40B8A0]
	CHECK(body->position.z.q16 == z0 + 100 + 65536);
	CHECK(body->position.x.q16 == 5 * 65536 && body->position.y.q16 == 6 * 65536);
	CHECK(motor->self == body); // the motor works on the body's own words (D-NET-507)
	const int32_t z1 = body->position.z.q16;
	body->mount_target = h; // any parent
	w::warm_up_player_body(*world, *body);
	CHECK(body->position.z.q16 == z1);
}

} // namespace

int main() {
	test_a_hidden_player_body_runs_nothing();
	test_a_markerless_deploy_leaves_the_body_where_it_stands();
	test_a_markerless_add_takes_heading_word_zero();
	test_the_warm_up_lifts_an_unparented_player_body();
	if (failures == 0) std::printf("player_placement: all passed\n");
	return failures == 0 ? 0 : 1;
}
