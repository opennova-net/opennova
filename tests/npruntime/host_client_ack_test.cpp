// npruntime_host_client_ack: the host reads nothing from a joiner's C2S 0x48 (net parity lane L1,
// D-NET-1028). Retail's server opcode table binds 0x48 (its row @0x82B9B8: id 0x48, the
// handler word NapiNPServerMsg_0x048 @0x510F30) to a bare `retn`, so the host never reads the
// body. A connection's id is the create's index that the 0x82's MI carried (D-NET-713), and its
// player's entity+0x78 is the add's copy of it [orig: Server_BuildPlayerInfoAndAdd @0x51D560 -
// conn->connection_id @0x51D5CB..0x51D5DB; Server_PlayerAdd @0x51CBC0 - slot+24 @0x51CEFB, the
// entity's ownerConnectionId @0x51D068]. Ours learned conn.connection_id from the 0x48 body and
// re-stamped the entity's owner word, so a joiner whose 0x48 named another id moved both.

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <runtime/replication/connection.h>

#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "conn_fixture.h"
#include "../common/test_expect.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

using test_expect::expect;

// An admitted, in-match joiner whose connection id is 3 (the create's index, the 0x82's MI) and
// whose player body carries it, sends one C2S 0x48 whose body names another id (0x113F).
bool run_the_host_takes_nothing_from_the_0x48_body() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	w::PlayerSpawn spawn;
	spawn.position = {0, 0, 0};
	spawn.net_id = 0xFFF1;
	spawn.owner_connection_id = 3;
	const w::EntityHandle body = w::spawn_remote_player(*world, spawn);
	if (!expect(body.valid(), "the add spawns the joiner's body")) return false;

	const PeerAddr peer{0x0100007Fu, 30123};
	const std::string client_scrk = "CLIENT-ACK-SCRK";
	inmatch::NapiNPServerCtx ctx;
	ctx.world = world.get();
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	inmatch::NapiNPConnection conn = conn_fixture::make_seeded_conn(
			3, 1, nullptr, ns::TransportMode::Client, body, true);
	conn.peer = peer;
	conn.client_scrk = client_scrk;
	conn.server_scrk = "SERVER-ACK-SCRK";
	conn.client_ck = 0x11223344u;
	conn.server_sk = 0x55667788u;
	conn.admission_stage = inmatch::GameAdmissionStage::Complete;
	conn.self_id_seen = true;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	const std::vector<uint8_t> ack_body = {0x3F, 0x11, 0x00, 0x00};
	SessionSequencing client_tx{1, 0};
	std::vector<uint8_t> session_body;
	if (!expect(frame_session_packet(client_tx, SessionCrypto{client_scrk, {}, 0x55667788u},
	                                 {make_protocol_message(c2s::CLIENT_ACK, ack_body)},
	                                 session_body),
	            "frame the C2S 0x48 session packet"))
		return false;
	const std::vector<uint8_t> datagram =
			nw_encode_outbound(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(session_body));
	(void)inmatch::handle_server_datagram(ctx, peer, datagram.data(), datagram.size(), 100);

	const inmatch::NapiNPConnection &after = ctx.np_protocol.connection_list[0];
	if (!expect(after.seq.last_inbound_seq == 1, "the host admits the packet")) return false;
	if (!expect(after.connection_id == 3, "the connection keeps the create's id")) return false;
	const w::Entity *e = world->registry.get(body);
	return expect(e != nullptr && e->owner_connection_id == 3,
	              "the player's entity+0x78 keeps the add's copy");
}

} // namespace

int main() {
	if (!run_the_host_takes_nothing_from_the_0x48_body()) return 1;
	std::printf("OK\n");
	return 0;
}
