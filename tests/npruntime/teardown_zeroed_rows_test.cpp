// D-NET-822 (F1b, Q7-10 (a)): the mission teardown destroys pools 0..2 before its server
// pump, so that pump's handlers read the destroyed rows. Retail zeroes each row (Entity_Destroy's
// memset) and keeps the slot's pointer; ours despawns the row and keeps the slot's handle, and a
// handler reads a despawned row as the zeroed one:
// - the C2S 0x22's S2C 0x46 still sends the slot's full record, but its downed byte (field 0x08)
//   tests the zeroed Flags word's dead bit and reads 0;
// - the C2S 0x23's S2C 0x4C reads the requester's AI-record pointer as 0, so the team-255 rule of
//   a non-team game no longer applies and the slot's own team selects the entries.
// A person's destroy drops what it carries first: a carried flag (a pool-1 item, still live while
// pool 0 goes) lands at its carrier's pose and, on an in-session authority, its S2C 0x2F is queued
// right there, ahead of the teardown's pump; single player (out of a session) sends none. The map
// change's teardown runs in retail's order: the pools' destroy, the pump, then the slot reset's
// S2C 0x25.
// [orig: Game_TeardownMission @0x522350 (Entity_Destroy over pools 0, 1 and 2
//  @0x52236F..0x5223C0, the pump @0x522588, the slot reset @0x52269B); Entity_Destroy @0x43E810
//  (the def+0x5C == 3 test @0x43E8B1, Entity_DropCarriedObject @0x43E8B8, the memset @0x43EA70);
//  Entity_DropCarriedObject @0x439DF0 (`cmp is_authority` @0x439F23 / jz @0x439F29, `cmp
//  is_in_session` @0x439F2B / jz @0x439F31, Server_SendDestructibleDeathPacket called @0x439F34);
//  NetPacket_SerializePlayerSync0x46 @0x505E80 (the slot's +0 pointer's null test
//  @0x505EDC..0x505EDE, the dead-bit test `test byte ptr [edi+24h], 2` @0x506102, jz @0x506106); NetPacket_SerializeVisiblePlayersSnapshot @0x506320 (the body test
//  @0x506376..0x50637A, +0x68 @0x50637C..0x506381, the 0x200 test @0x506383..0x50638C)]
#include "npruntime/conn_fixture.h"

#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/visible_players.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <vector>
#include "../common/title_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;
using conn_fixture::make_conn;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

w::PlayerSpawn player_spawn(uint16_t net_id, uint8_t team) {
	w::PlayerSpawn spawn;
	spawn.position = {20.0f + static_cast<float>(net_id & 3u), 20.0f, 0.0f};
	spawn.net_id = net_id;
	spawn.team = team;
	return spawn;
}

void configure_pools(w::World &world) {
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	world.registry.configure_pool(2, 4);
}

// A carried flag: a pool-1 item the carrier's mounted child names.
w::EntityHandle give_flag(w::World &world, w::EntityHandle carrier) {
	w::Entity flag_row;
	flag_row.kind = w::EntityKind::Item;
	flag_row.item_id = 4091;
	flag_row.has_item_def = true;
	flag_row.alive = true;
	const w::EntityHandle flag = world.registry.spawn(1, flag_row);
	world.registry.get(carrier)->mounted_child = flag;
	world.registry.get(flag)->primary_occupant = carrier;
	return flag;
}

std::vector<const ProtocolMessage *> tagged(const std::vector<ProtocolMessage> &replies, uint8_t tag) {
	std::vector<const ProtocolMessage *> out;
	for (const ProtocolMessage &m : replies)
		if (m.tag == tag) out.push_back(&m);
	return out;
}

// The teardown pump's 0x46 and 0x4C over destroyed rows, against the same requests before the
// destroy: two players on slot team 1 in a Deathmatch (no team bit), the second dead with its
// automatic revive window open.
void test_pump_handlers_read_the_destroyed_rows() {
	auto kernel = std::make_unique<mission::MissionKernel>();
	test_titles::jo_world(kernel->world);
	w::World &world = kernel->world;
	configure_pools(world);
	const w::EntityHandle a = w::spawn_remote_player(world, player_spawn(0xFFF1, 1));
	const w::EntityHandle b = w::spawn_remote_player(world, player_spawn(0xFFF2, 1));
	CHECK(a.valid() && b.valid());
	if (!a.valid() || !b.valid()) return;
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_conn(7, inmatch::NapiNPConnection::kTypeServerSide, nullptr,
			ns::TransportMode::Client, a, true));
	roster.push_back(make_conn(8, inmatch::NapiNPConnection::kTypeServerSide, nullptr,
			ns::TransportMode::Client, b, true));
	for (std::size_t i = 0; i < roster.size(); ++i) {
		roster[i].reply.player_slot = static_cast<uint32_t>(i + 1);
		roster[i].reply.player_name = i == 0 ? "A" : "B";
		roster[i].assigned_team = 1;
		roster[i].assigned_team_valid = true;
	}
	w::Entity *body_b = world.registry.get(b);
	body_b->health = 0;
	body_b->alive = false;
	body_b->flags |= w::kEntityFlagDead;
	roster[1].link.auto_medic_enabled = true;
	roster[1].link.downed_revive_seconds = 9;
	inmatch::GameConfig config;
	test_titles::jo_config(config);
	config.game_type = game_type::kDeathmatch;
	config.max_players = 4;

	const auto ask = [&](uint8_t tag, std::vector<uint8_t> body) {
		return inmatch::dispatch_session_replies(config, roster[0],
				{make_protocol_message(tag, std::move(body))}, 100, roster, &world);
	};
	const auto snapshot = [&]() {
		VisiblePlayers seen;
		const std::vector<ProtocolMessage> replies = ask(0x23, {});
		const auto v = tagged(replies, s2c::VISIBLE_PLAYERS);
		CHECK(v.size() == 1);
		if (v.size() == 1) decode_visible_players(v[0]->payload.data(), v[0]->payload.size(), seen);
		return seen;
	};
	const auto downed = [&](PlayerSync &out) {
		const std::vector<ProtocolMessage> replies =
				ask(0x22, {2, static_cast<uint8_t>(kPlayerSyncHasDownedState), 0});
		const auto v = tagged(replies, s2c::PLAYER_SYNC);
		return v.size() == 1 && decode_player_sync(v[0]->payload.data(), v[0]->payload.size(), out);
	};

	// Live bodies: outside the team types the requester's body reads as team 255 (itself alone);
	// the dead slot's downed byte carries its revive window.
	VisiblePlayers live = snapshot();
	CHECK(live.entries.size() == 1 && live.entries[0].slot == 1);
	PlayerSync live_sync;
	CHECK(downed(live_sync));
	CHECK(!live_sync.removal && live_sync.downed_state == 9);

	kernel->destroy_mission_pools();
	CHECK(world.registry.get(a) == nullptr && world.registry.get(b) == nullptr);

	// The destroyed rows: the slot's team selects both players, each with the handle its slot
	// kept; the slot's record is still the full form, its downed byte 0.
	VisiblePlayers zeroed = snapshot();
	CHECK(zeroed.entries.size() == 2);
	if (zeroed.entries.size() == 2) {
		CHECK(zeroed.entries[0].slot == 1 && zeroed.entries[0].entity_handle == a.packed);
		CHECK(zeroed.entries[1].slot == 2 && zeroed.entries[1].entity_handle == b.packed);
	}
	PlayerSync zeroed_sync;
	CHECK(downed(zeroed_sync));
	CHECK(!zeroed_sync.removal && zeroed_sync.downed_state == 0 &&
			zeroed_sync.entity_slot_id == static_cast<uint8_t>(b.packed & 0xFFFu));
}

// The pools' destroy drops a carrier's flag at the carrier's pose (z + 0x4000) before the flag's
// own destroy; an in-session authority queues its 0x2F to every in-match slot; single player sends
// none.
void test_destroy_drops_carried_flag() {
	for (const bool in_session : {true, false}) {
		auto kernel = std::make_unique<mission::MissionKernel>();
		test_titles::jo_world(kernel->world);
		w::World &world = kernel->world;
		configure_pools(world);
		world.rules.mp_session = in_session;
		const w::EntityHandle carrier = w::spawn_remote_player(world, player_spawn(0xFFF1, 1));
		CHECK(carrier.valid());
		if (!carrier.valid()) return;
		const w::EntityHandle flag = give_flag(world, carrier);
		const w::EntityPosition carrier_pos = world.registry.get(carrier)->position;
		(void)world.match.drain_gameplay_events();
		auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
		test_titles::jo_config(ctx->config);
		ctx->is_authority = 1;
		ctx->is_in_session = 1;
		ctx->world = &world;
		ns::UdpSessionTransport observer(ns::UdpSessionTransport::Role::Host);
		ctx->np_protocol.connection_list.push_back(make_conn(8,
				inmatch::NapiNPConnection::kTypeServerSide, &observer, ns::TransportMode::Client,
				carrier, true));

		kernel->destroy_mission_pools();
		inmatch::route_teardown_drops(*ctx, world);

		CHECK(world.registry.get(carrier) == nullptr && world.registry.get(flag) == nullptr);
		std::vector<ns::Datagram> sent;
		ns::Datagram d;
		while (observer.pop_outbound(d)) sent.push_back(d);
		if (!in_session) {
			CHECK(sent.empty());
			continue;
		}
		CHECK(sent.size() == 1 && sent[0].tag == s2c::OBJECTIVE_ENTITY_STATE);
		if (sent.size() != 1) continue;
		ObjectiveEntityState state;
		std::size_t consumed = 0;
		CHECK(decode_objective_entity_state(sent[0].body.data(), sent[0].body.size(), state, consumed));
		CHECK(state.entity_handle == flag.packed);
		CHECK(state.pos_x == carrier_pos.x.q16 && state.pos_y == carrier_pos.y.q16 &&
				state.pos_z == static_cast<int32_t>(static_cast<uint32_t>(carrier_pos.z.q16) + 0x4000u));
	}
}

// The map change's teardown on a session host: the pools go first, the carried flag's 0x2F is
// queued then and taken by the teardown's pump; the slot reset's 0x25 is queued after the pump.
// With the boundary held closed, the pump's flush moves the 0x2F into the connection's pending
// queue and leaves the later 0x25 on the transport.
void test_map_change_destroys_before_its_pump() {
	auto role = std::make_unique<inmatch::HostRole>(inmatch::RoleKind::DedicatedHost);
	auto kernel = std::make_unique<mission::MissionKernel>();
	test_titles::jo_world(kernel->world);
	w::World &world = kernel->world;
	configure_pools(world);
	role->bind(*kernel);
	inmatch::NapiNPServerCtx &ctx = role->state.host_owner.ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.world = &world;
	const w::EntityHandle carrier = w::spawn_remote_player(world, player_spawn(0xFFF1, 1));
	CHECK(carrier.valid());
	if (!carrier.valid()) return;
	const w::EntityHandle flag = give_flag(world, carrier);
	(void)world.match.drain_gameplay_events();
	ns::UdpSessionTransport wire(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection conn = make_conn(8, inmatch::NapiNPConnection::kTypeServerSide, &wire,
			ns::TransportMode::Client, carrier, true);
	conn.peer = PeerAddr{0x0100007Fu, 33101};
	conn.s2c_send_boundary_open = false;
	conn.reply.player_slot = 1;
	ctx.np_protocol.connection_list.push_back(conn);

	const std::vector<mission_catalog::Row> catalog;
	CHECK(inmatch::begin_host_map_change(*role, catalog) == inmatch::MapChangeStep::RotationEnded);
	CHECK(role->map_change.torn_down);
	CHECK(world.registry.get(carrier) == nullptr && world.registry.get(flag) == nullptr);

	const auto pending = role->state.host_owner.pending_session_messages.find(conn.peer);
	CHECK(pending != role->state.host_owner.pending_session_messages.end());
	if (pending != role->state.host_owner.pending_session_messages.end()) {
		CHECK(pending->second.size() == 1 &&
				pending->second[0].tag == s2c::OBJECTIVE_ENTITY_STATE);
	}
	std::vector<uint8_t> after_pump;
	ns::Datagram d;
	while (wire.pop_outbound(d)) after_pump.push_back(d.tag);
	CHECK(after_pump == std::vector<uint8_t>{s2c::GAME_RESET});
}

} // namespace

int main() {
	test_pump_handlers_read_the_destroyed_rows();
	test_destroy_drops_carried_flag();
	test_map_change_destroys_before_its_pump();
	if (failures != 0) {
		std::printf("teardown_zeroed_rows_test: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("teardown_zeroed_rows_test: PASS");
	return 0;
}
