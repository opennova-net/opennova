// D-NET-127 — the S2C 0x46 per-field values the host's player-slot table
// carries. Field 0x1000 is 1 for an active slot whose spectator latch
// (slot+100567) is set and whose loading byte (slot+100579) is clear, else 0;
// the client lands it as the roster slot's spectator byte.
// [orig: NetPacket_SerializePlayerSync0x46 @0x505E80 (@0x506197..0x5061cc);
//  NapiNPClientMsg_PlayerSync @0x431370; NapiNPServerMsg_0x022 @0x514C90]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <cstdio>
#include <memory>
#include <vector>
#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

void test_spectator_field_follows_the_slot() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	ns::LoopbackChannel a, b, c, d;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(conn_fixture::make_conn(1, 1, &a, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // the requester, a player
	roster.push_back(conn_fixture::make_conn(2, 1, &b, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // a spectator in the match
	roster.push_back(conn_fixture::make_conn(3, 1, &c, ns::TransportMode::Client,
			world.registry.spawn(0, person), false)); // a spectator still loading
	roster.push_back(conn_fixture::make_conn(4, 1, &d, ns::TransportMode::Client,
			world.registry.spawn(0, person), true)); // a player in the match
	for (size_t i = 0; i < roster.size(); ++i) {
		roster[i].reply.player_slot = static_cast<uint8_t>(i);
		roster[i].phase = inmatch::ConnectionPhase::PlayerAdded;
	}
	roster[1].link.spectator = true;
	roster[2].link.spectator = true;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const auto field_1000 = [&](uint8_t slot) -> int {
		const uint16_t fields = kPlayerSyncHasLateJoinFlag;
		for (const ProtocolMessage &m : inmatch::dispatch_session_replies(ctx.config, roster[0],
				{make_protocol_message(c2s::PLAYER_SYNC_REQUEST,
						{slot, uint8_t(fields), uint8_t(fields >> 8)})},
				100, roster, &world, inputs)) {
			if (m.tag != s2c::PLAYER_SYNC) continue;
			PlayerSync sync;
			if (!decode_player_sync(m.payload.data(), m.payload.size(), sync)) return -2;
			if (sync.removal || (sync.field_bitmask & kPlayerSyncHasLateJoinFlag) == 0) return -3;
			return sync.field_1000;
		}
		return -1;
	};
	CHECK(field_1000(0) == 0);
	CHECK(field_1000(1) == 1);
	CHECK(field_1000(2) == 0); // the loading byte is still set
	CHECK(field_1000(3) == 0);
}

} // namespace

int main() {
	test_spectator_field_follows_the_slot();
	std::printf("npruntime_player_sync_fields: %d failures\n", failures);
	return failures ? 1 : 0;
}
