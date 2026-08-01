// npruntime host bring-up — the §5.0 connection-mode -> {is_authority, is_mp_session_peer}
// table and the socketless transport mode. The IDA-faithful in-process listen-server bring-up
// state writes [orig: SinglePlayer_StartMission @0x561af0].

#include "npruntime/server_session.h"
#include "npruntime/host_session.h"
#include "npruntime/joiner_connection.h"
#include "npruntime/server_message_dispatch.h"
#include "npruntime/server_tick.h"

#include "netsim/connection_fan.h"
#include "netsim/loopback_channel.h"
#include "netsim/idatagram_socket.h"
#include "netsim/udp_session_transport.h"

#include <npwire/nw_session_framing.h>
#include <npwire/entity_class.h>
#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>
#include <npwire/protocol_message.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <world/ai.h>
#include <world/world.h>

#include <cstdio>
#include <algorithm>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

void add_retail_game_environment(opennova::ClientAuth &auth) {
	for (const auto &field : {
			std::pair{"BT", "0"},
			std::pair{"VN", "2"},
			std::pair{"BN", "1"},
			std::pair{"DB", "0"},
			std::pair{"MBN", "20042002"},
			std::pair{"SOPD", "180"},
	}) {
		auth.cu.push_back(opennova::make_client_cu_chunk(
				2, field.first, field.second));
	}
}

using opennova::np::ConnectionMode;
using opennova::np::NapiNPServerCtx;
using opennova::np::SocketMode;

struct CaptureDatagramSocket final : opennova::netsim::IDatagramSocket {
	struct Incoming {
		opennova::PeerAddr peer;
		std::vector<uint8_t> bytes;
	};
	std::deque<Incoming> incoming;
	std::vector<std::vector<uint8_t>> sent;
	std::vector<opennova::PeerAddr> sent_to;

	int recv_from(uint8_t *data, std::size_t capacity, opennova::PeerAddr &from) override {
		if (incoming.empty()) return 0;
		Incoming item = std::move(incoming.front());
		incoming.pop_front();
		const std::size_t count = std::min(capacity, item.bytes.size());
		std::copy_n(item.bytes.begin(), count, data);
		from = item.peer;
		return static_cast<int>(count);
	}
	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent_to.push_back(to);
		sent.emplace_back(data, data + len);
	}
};

// [orig: CGameSession_SetConnectionMode @0x4c49f0] decomposes the mode into the is_host /
// is_client booleans exactly per the §5.0 table.
bool check_connection_mode_table() {
	struct Row {
		ConnectionMode mode;
		uint32_t is_host;
		uint32_t is_client;
	};
	const Row rows[] = {
	    {ConnectionMode::None, 0, 0},
	    {ConnectionMode::HostOnly, 1, 0},
	    {ConnectionMode::ClientOnly, 0, 1},
	    {ConnectionMode::HostClient, 1, 1}, // single-player / co-op listen server
	};
	for (const Row &r : rows) {
		NapiNPServerCtx ctx;
		opennova::np::set_connection_mode(ctx, r.mode);
		if (!expect(ctx.connection_mode == r.mode, "connection_mode stored")) return false;
		if (!expect(ctx.is_authority == r.is_host, "is_authority = is_host bit")) return false;
		if (!expect(ctx.is_mp_session_peer == r.is_client, "is_mp_session_peer = is_client bit"))
			return false;
	}
	return true;
}

// SP host = mode 3 + socketless transport (§5.0 steps 1-2): is_authority && is_mp_session_peer,
// socket_state == Socketless (no UDP socket opened).
bool check_single_player_signature() {
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);
	if (!expect(ctx.is_authority == 1 && ctx.is_mp_session_peer == 1, "SP is host + client"))
		return false;
	if (!expect(ctx.socket_state == SocketMode::Socketless, "SP is socketless")) return false;
	// Defaults that must hold before StartServer (P1).
	if (!expect(ctx.np_protocol.host_running == 0, "host not running before StartServer"))
		return false;
	if (!expect(ctx.is_in_session == 0, "no session before CreateSession")) return false;
	if (!expect(ctx.np_protocol.connection_list.empty(), "no connections before bring-up"))
		return false;
	return true;
}

// The full SP bring-up [orig: SinglePlayer_StartMission @0x561af0]:
// SetConnectionMode(3) -> SetTransportMode(1) -> CreateSession -> StartServer, registering the
// host's own loopback client connection. After it: host_running == 1, in session, one connection.
bool check_create_session_brings_up_host() {
	opennova::netsim::LoopbackChannel local_client;
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);

	opennova::np::GameConfig settings;
	settings.server_name = "SINGLEPLAYERGAME"; // §5.0 default
	settings.max_players = 1;

	opennova::np::SessionStartup startup;
	startup.host_key = 0xABCD1234;     // seed-injected (NapiNP_GenerateSessionKey result)
	startup.host_start_tick = 100000;  // seed-injected (GetTickCount)
	startup.session_seed_id = 654321;  // seed-injected

	opennova::np::create_session(ctx, settings, startup, &local_client);

	if (!expect(ctx.is_in_session == 1, "in session after CreateSession")) return false;
	if (!expect(ctx.np_protocol.host_running == 1, "host_running == 1 after StartServer"))
		return false;
	if (!expect(ctx.np_protocol.host_key == 0xABCD1234, "host_key stamped")) return false;
	if (!expect(ctx.np_protocol.host_start_tick == 100000, "host_start_tick stamped"))
		return false;
	if (!expect(ctx.np_protocol.session_seed_id == 654321, "session_seed_id stamped"))
		return false;
	if (!expect(ctx.np_protocol.session_name == "SINGLEPLAYERGAME", "session_name = server_name"))
		return false;
	if (!expect(ctx.np_protocol.max_players == 1, "MP TLV mirrors settings")) return false;
	if (!expect(ctx.np_protocol.connection_list.size() == 1, "one (loopback) connection"))
		return false;
	const opennova::np::NapiNPConnection &c = ctx.np_protocol.connection_list[0];
	if (!expect(c.type == 2, "host's own client is a type-2 connection")) return false;
	if (!expect(c.link.mode == opennova::netsim::TransportMode::Loopback, "mode 1 loopback"))
		return false;
	if (!expect(c.link.transport == &local_client, "transport bound (non-owning)")) return false;
	return true;
}

// A dedicated host (mode 1) starts the server but registers no local client connection.
bool check_dedicated_host_has_no_local_client() {
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(ctx, SocketMode::Lan);
	opennova::np::GameConfig settings;
	settings.max_players = 16;
	opennova::np::create_session(ctx, settings, opennova::np::SessionStartup{}, nullptr);
	if (!expect(ctx.np_protocol.host_running == 1, "dedicated host running")) return false;
	if (!expect(ctx.np_protocol.connection_list.empty(), "no local client on a dedicated host"))
		return false;
	return true;
}

// Production serve mode is a true HostOnly session: supplying the adapter's loopback must not
// create a type-2 local client or lazily spawn/count a phantom player. Volatile startup terms
// are minted by the production helper when the deterministic test override is zero.
bool check_production_serve_mode_has_no_phantom_and_mints_startup() {
	opennova::netsim::LoopbackChannel unused_local_view;
	opennova::np::HostOwner owner;
	owner.host_loopback = &unused_local_view;
	opennova::np::HostConfig config;
	config.socket_mode = SocketMode::Lan;
	config.config.max_players = 16;
	config.serve_and_play = false;
	config.host_key = 0;

	opennova::np::start_host_session(owner, config);
	if (!expect(owner.ctx.connection_mode == ConnectionMode::HostOnly &&
	                    owner.ctx.is_authority == 1 &&
	                    owner.ctx.is_mp_session_peer == 0,
	            "production serve mode starts a HostOnly session")) return false;
	if (!expect(owner.ctx.np_protocol.connection_list.empty(),
	            "production serve mode registers no type-2 phantom player")) return false;
	if (!expect(owner.ctx.np_protocol.host_key != 0,
	            "production startup mints a nonzero host key")) return false;
	if (!expect(owner.ctx.np_protocol.host_start_tick != 0,
	            "production startup records a nonzero monotonic start tick")) return false;
	if (!expect(owner.ctx.np_protocol.session_seed_id >= 100000 &&
	                    owner.ctx.np_protocol.session_seed_id <= 999999,
	            "production startup mints the retail six-digit session seed")) return false;
	return true;
}

// Messages queued at one host send boundary share one sequenced packet up to the negotiated packet
// ceiling. The retail gameplay capture carries the S2C 0x57 RTT reply beside the per-frame 0x0A;
// framing each UdpSessionTransport record separately doubles sequence/UDP traffic.
bool check_host_pump_batches_one_send_boundary() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);
	opennova::np::configure_session_runtime(owner.ctx);

	const opennova::PeerAddr peer{0x0100007Fu, 33100};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERBATCHSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	conn.client_scrk =
			"CLIENTBATCHSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	conn.server_sk = 0x55667788u;
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;

	// One real C2S 0x2C echo-request produces the 0x57 through
	// handle_server_datagram; the same tick's transport fan contributes 0x0A.
	opennova::ProtocolPacketHeader inbound_header;
	inbound_header.session_id = conn.server_sk;
	inbound_header.seq_num = 1;
	inbound_header.ack_count = 0;
	std::vector<uint8_t> inbound_body;
	if (!expect(opennova::encode_protocol_packet_plaintext(
	                    inbound_header,
	                    {opennova::make_protocol_message(0x2C, {1, 2, 3, 4, 1})},
	                    conn.client_scrk, inbound_body),
	            "batch fixture encodes the client RTT request")) return false;
	std::vector<uint8_t> inbound = opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(inbound_body));
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	peer_link.transport->host_send(0x0A, {0, 0, 0, 0});
	CaptureDatagramSocket socket;
	socket.incoming.push_back({peer, std::move(inbound)});
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1,
	            "host batches 0x57 and 0x0A queued in one send boundary into one datagram"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	if (!expect(opennova::nw_decode_inbound(
	                    socket.sent[0].data(), socket.sent[0].size(), opcode, session_body) &&
	                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	            "batched host datagram is one S2C session packet")) return false;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
	                    session_body.data(), session_body.size(),
	                    owner.ctx.np_protocol.connection_list.front().server_scrk,
	                    header, messages),
	            "batched host session packet decodes")) return false;
	return expect(messages.size() == 2 && messages[0].tag == 0x57 &&
	                      messages[1].tag == 0x0A,
	              "decoded packet preserves the queued 0x57 then 0x0A message order");
}

// The dictated CS field-3 period applies in both directions. The host keeps
// simulation/C2S full-rate, but remote 0x0A production and the S2C flush open
// only on the exact decrement-before-gate boundary. The high retail BANDWIDTH
// setting must still fit one 1300-byte session packet after its LEN16 envelope.
bool check_host_s2c_holdoff_and_frame_envelope() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	config.entity_send_budget = 1600;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);
	opennova::np::configure_session_runtime(owner.ctx);

	opennova::world::World world;
	world.registry.configure_pool(1, 80);
	for (int i = 0; i < 70; ++i) {
		opennova::world::Entity vehicle;
		vehicle.kind = opennova::world::EntityKind::Item;
		vehicle.item_id = 0x1004;
		vehicle.net_class_code = static_cast<uint8_t>(opennova::EntityClass::Vehicle);
		vehicle.health = 3000;
		vehicle.health_max = 3000;
		vehicle.position.x = static_cast<float>(i);
		if (!expect(world.registry.spawn(1, vehicle).valid(),
		            "frame-envelope fixture vehicle spawned")) return false;
	}
	owner.ctx.world = &world;
	opennova::netsim::set_entity_send_budget(1600);

	const opennova::PeerAddr peer{0x0100007Fu, 33120};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERHOLDOFFSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_scrk =
			"CLIENTHOLDOFFSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	opennova::np::arm_s2c_send_holdoff(remote, 3);

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() && remote.s2c_send_holdoff_countdown == 2,
	            "first host S2C holdoff tick stays closed")) return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() && remote.s2c_send_holdoff_countdown == 1,
	            "second host S2C holdoff tick stays closed")) return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1 && remote.s2c_send_holdoff_countdown == 3,
	            "third host tick opens one S2C boundary and reloads")) return false;
	if (!expect(world.logic_tick == 3,
	            "host simulation remains full-rate while S2C is held")) return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	if (!expect(opennova::nw_decode_inbound(
	                    socket.sent[0].data(), socket.sent[0].size(), opcode,
	                    session_body) &&
	                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	            "held host frame decodes as one S2C session packet")) return false;
	if (!expect(session_body.size() <= opennova::np::kGameSessionMaxPacketBytes,
	            "session body obeys the installed 1300-byte ceiling")) return false;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
	                    session_body.data(), session_body.size(),
	                    owner.ctx.np_protocol.connection_list.front().server_scrk,
	                    header, messages),
	            "held host frame decrypts")) return false;
	if (!expect(messages.size() == 1 && messages.front().tag == 0x0A &&
	                    messages.front().payload.size() > 1200 &&
	                    messages.front().payload.size() <=
	                            opennova::np::kMaxFrameUpdateBodyBytes,
	            "one fresh 0x0A fits its envelope-aware body cap")) return false;

	opennova::np::host_session_pump(owner, socket);
	return expect(socket.sent.size() == 1 && remote.s2c_send_holdoff_countdown == 2,
	              "next host S2C boundary remains closed for the full period");
}

// The retail send-block clock belongs to each NapiNPConnection. Peers admitted
// on different host ticks must retain their own phase; a session-global counter
// would incorrectly release both on the same boundary.
bool check_host_s2c_holdoff_is_per_connection() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);
	opennova::np::configure_session_runtime(owner.ctx);

	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	owner.ctx.world = &world;

	auto add_remote = [&](const opennova::PeerAddr &peer, const char *server_scrk,
	                      uint32_t client_ck) {
		opennova::np::PeerLink &peer_link = owner.peers[peer];
		peer_link.transport =
				std::make_unique<opennova::netsim::UdpSessionTransport>(
						opennova::netsim::UdpSessionTransport::Role::Host);
		opennova::np::NapiNPConnection conn;
		conn.peer = peer;
		conn.type = 1;
		conn.phase = opennova::np::ConnectionPhase::InMatch;
		conn.burst.spawned = true;
		conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
		conn.server_scrk = server_scrk;
		conn.client_ck = client_ck;
		conn.link.transport = peer_link.transport.get();
		conn.link.mode = opennova::netsim::TransportMode::Client;
		owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	};

	const opennova::PeerAddr peer_a{0x0100007Fu, 33130};
	const opennova::PeerAddr peer_b{0x0100007Fu, 33131};
	add_remote(peer_a,
	           "SERVERHOLDAPERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
	           0x11112222u);
	add_remote(peer_b,
	           "SERVERHOLDBPERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
	           0x33334444u);
	auto &remote_a = owner.ctx.np_protocol.connection_list[0];
	auto &remote_b = owner.ctx.np_protocol.connection_list[1];
	opennova::np::arm_s2c_send_holdoff(remote_a, 3);
	opennova::np::arm_s2c_send_holdoff(remote_b, 3);
	// Model B having been admitted two pump ticks earlier than A.
	remote_b.s2c_send_holdoff_countdown = 1;
	remote_b.s2c_send_boundary_open = false;

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent_to.size() == 1 && socket.sent_to[0] == peer_b &&
	                    remote_a.s2c_send_holdoff_countdown == 2 &&
	                    remote_b.s2c_send_holdoff_countdown == 3,
	            "earlier peer opens and reloads without releasing the later peer"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent_to.size() == 1 &&
	                    remote_a.s2c_send_holdoff_countdown == 1 &&
	                    remote_b.s2c_send_holdoff_countdown == 2,
	            "staggered peer clocks advance independently"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	return expect(socket.sent_to.size() == 2 && socket.sent_to[1] == peer_a &&
	                      remote_a.s2c_send_holdoff_countdown == 3 &&
	                      remote_b.s2c_send_holdoff_countdown == 1 &&
	                      world.logic_tick == 3,
	              "later peer reaches its own boundary while simulation stays full-rate");
}

// The same connection clock gates the framed initial-state stream. Those 0x83
// packets are produced before the live UdpSessionTransport exists, so the owner
// must retain them explicitly instead of bypassing the send block in step 2.
bool check_initial_stream_obeys_connection_holdoff() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	config.send_holdoff_ticks = 3;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);
	opennova::np::configure_session_runtime(owner.ctx);

	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	opennova::world::Entity player;
	player.kind = opennova::world::EntityKind::Organic;
	player.item_id = 0x14B9;
	const auto owned = world.registry.spawn(0, player);
	if (!expect(owned.valid(), "initial-stream fixture owns a pool-0 player"))
		return false;
	owner.ctx.world = &world;

	const opennova::PeerAddr peer{0x0100007Fu, 33135};
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::PlayerAdded;
	conn.admission_stage = opennova::np::GameAdmissionStage::Complete;
	conn.reply.roster_pushed = true;
	conn.link.owned_entity = owned;
	conn.server_scrk =
			"SERVERINITIALHOLDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x778899AAu;
	opennova::np::arm_s2c_send_holdoff(conn, config.send_holdoff_ticks);
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	const uint32_t initial_sequence = remote.seq.next_outbound_seq;

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() &&
	                    remote.s2c_send_holdoff_countdown == 2 &&
	                    remote.seq.next_outbound_seq == initial_sequence &&
	                    owner.pending_session_datagrams.find(peer) ==
	                            owner.pending_session_datagrams.end(),
	            "closed first boundary does not build an initial-stream packet"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() &&
	                    remote.s2c_send_holdoff_countdown == 1 &&
	                    remote.seq.next_outbound_seq == initial_sequence &&
	                    owner.pending_session_datagrams.find(peer) ==
	                            owner.pending_session_datagrams.end(),
	            "closed second boundary retains the initial-stream cursor"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(!socket.sent.empty() &&
	                    remote.s2c_send_holdoff_countdown == 3 &&
	                    remote.seq.next_outbound_seq > initial_sequence &&
	                    owner.pending_session_datagrams.find(peer) ==
	                            owner.pending_session_datagrams.end(),
	            "open third boundary builds and sends the initial-state packet"))
		return false;
	return std::all_of(socket.sent_to.begin(), socket.sent_to.end(),
	                   [&](const opennova::PeerAddr &to) { return to == peer; });
}

// The 1300-byte cap belongs to UDP session framing, not the host's type-2
// in-process presentation channel. Preserve its existing unbounded high-rate
// frame so a dense listen-server world is not artificially subrated.
bool check_host_loopback_does_not_inherit_udp_envelope() {
	opennova::netsim::LoopbackChannel loopback;
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);
	opennova::np::GameConfig config;
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, &loopback);
	if (!expect(ctx.np_protocol.connection_list.size() == 1,
	            "loopback envelope fixture has one host client")) return false;
	ctx.np_protocol.connection_list.front().burst.spawned = true;

	opennova::world::World world;
	world.registry.configure_pool(1, 80);
	for (int i = 0; i < 70; ++i) {
		opennova::world::Entity vehicle;
		vehicle.kind = opennova::world::EntityKind::Item;
		vehicle.item_id = 0x1004;
		vehicle.net_class_code = static_cast<uint8_t>(opennova::EntityClass::Vehicle);
		vehicle.health = 3000;
		vehicle.position.x = static_cast<float>(i);
		if (!expect(world.registry.spawn(1, vehicle).valid(),
		            "loopback envelope fixture vehicle spawned")) return false;
	}
	ctx.world = &world;
	opennova::netsim::set_entity_send_budget(1600);
	opennova::np::Server_TickUpdate(ctx);
	opennova::netsim::Datagram frame;
	if (!expect(loopback.client_recv(frame) && frame.tag == 0x0A,
	            "host loopback receives its full-rate frame")) return false;
	return expect(frame.body.size() > opennova::np::kMaxFrameUpdateBodyBytes,
	              "host loopback remains outside the UDP frame envelope");
}

// A sparse pool can make the high-water empty-slot sweep larger than one
// retail session packet. The owner must emit protocol FIRST/FINAL fragments,
// never silently discard the semantic message, and the client-side reassembler
// must recover the complete ordered destroy list.
bool check_sparse_empty_slot_sweep_fragments_without_loss() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);
	opennova::np::configure_session_runtime(owner.ctx);

	const opennova::PeerAddr peer{0x0100007Fu, 33140};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERFRAGSWEEPSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x55667788u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	opennova::np::arm_s2c_send_holdoff(remote, 0);

	opennova::world::World sparse_world;
	sparse_world.registry.configure_pool(0, 1024);
	opennova::world::Entity seed;
	const auto low = sparse_world.registry.spawn(0, seed);
	const auto high = sparse_world.registry.spawn_from(0, 1023, seed);
	if (!expect(low.valid() && low.slot() == 0 && high.valid() && high.slot() == 1023,
	            "sparse-sweep fixture establishes a 1024-slot high-water mark"))
		return false;

	std::vector<opennova::ProtocolMessage> replies =
			opennova::np::dispatch_session_replies(
					config, remote,
					{opennova::make_protocol_message(
							opennova::c2s::EMPTY_SLOT_SWEEP_REQUEST, {})},
					100, owner.ctx.np_protocol.connection_list, &sparse_world);
	if (!expect(replies.size() == 1 &&
	                    replies.front().tag == opennova::s2c::EMPTY_SLOT_SWEEP &&
	                    replies.front().payload.size() >
	                            opennova::np::kMaxFrameUpdateBodyBytes,
	            "sparse high-water sweep exceeds one framed-message payload"))
		return false;
	owner.pending_session_messages[peer] = std::move(replies);

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 2 && socket.sent_to.size() == 2 &&
	                    socket.sent_to[0] == peer && socket.sent_to[1] == peer,
	            "oversized sweep emits two ordered S2C datagrams"))
		return false;

	opennova::np::JoinerConnection joiner(
			"FragmentJoiner", [] { return uint64_t{0}; });
	joiner.seed_in_match(
			0x11223344u, remote.client_ck,
			"CLIENTFRAGSWEEPSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
			remote.server_scrk, 1, 0, 0, 0x14B9);
	const auto first_poll = joiner.handle_datagram(
			socket.sent[0].data(), socket.sent[0].size());
	if (!expect(first_poll.destroyed_pool0_slots.empty(),
	            "joiner does not dispatch an incomplete sweep fragment"))
		return false;
	const auto final_poll = joiner.handle_datagram(
			socket.sent[1].data(), socket.sent[1].size());
	if (!expect(final_poll.destroyed_pool0_slots.size() == 1022,
	            "joiner dispatches one complete sweep after the final fragment"))
		return false;
	for (std::size_t i = 0; i < final_poll.destroyed_pool0_slots.size(); ++i) {
		if (!expect(final_poll.destroyed_pool0_slots[i] == i + 1,
		            "joiner receives every fragmented pool slot in order"))
			return false;
	}

	opennova::ProtocolReassemblyState reassembly;
	std::vector<uint8_t> assembled;
	for (std::size_t i = 0; i < socket.sent.size(); ++i) {
		uint8_t opcode = 0;
		std::vector<uint8_t> session_body;
		if (!expect(opennova::nw_decode_inbound(
		                    socket.sent[i].data(), socket.sent[i].size(), opcode,
		                    session_body) &&
		                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
		                    session_body.size() <=
		                            opennova::np::kGameSessionMaxPacketBytes,
		            "each sweep fragment obeys the 1300-byte session ceiling"))
			return false;
		opennova::ProtocolPacketHeader header;
		std::vector<opennova::ProtocolMessage> messages;
		if (!expect(opennova::decode_protocol_packet_plaintext(
		                    session_body.data(), session_body.size(), remote.server_scrk,
		                    header, messages) &&
		                    messages.size() == 1 &&
		                    messages.front().tag == opennova::s2c::EMPTY_SLOT_SWEEP,
		            "sweep fragment decrypts as one 0x5D record"))
			return false;
		const opennova::ProtocolMessage &fragment = messages.front();
		if (i == 0) {
			if (!expect(fragment.flags.frag_cont && !fragment.flags.frag_end,
			            "first sweep piece carries the FIRST fragment flags"))
				return false;
		} else if (!expect(!fragment.flags.frag_cont && fragment.flags.frag_end,
		                   "last sweep piece carries the FINAL fragment flags")) {
			return false;
		}
		const bool complete =
				opennova::reassemble_protocol_payload(reassembly, fragment, assembled);
		if (!expect(complete == (i + 1 == socket.sent.size()),
		            "sweep dispatch occurs only after the final fragment"))
			return false;
	}

	opennova::DestroyEntityList decoded;
	if (!expect(opennova::decode_destroy_entity_list(
	                    assembled.data(), assembled.size(), decoded) &&
	                    decoded.pool0_indices.size() == 1022,
	            "reassembled sweep retains every sparse empty slot"))
		return false;
	for (std::size_t i = 0; i < decoded.pool0_indices.size(); ++i) {
		if (!expect(decoded.pool0_indices[i] == i + 1,
		            "reassembled sweep preserves pool-slot order"))
			return false;
	}
	return true;
}

// Same-address replacement is delivered as owner cleanup after the protocol has already admitted the
// fresh node. The cleanup event must erase only the old PeerLink; dropping again by endpoint would
// delete the replacement connection created earlier in the same handle_server_datagram call.
bool check_host_pump_reconnect_keeps_fresh_connection() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x0FE0E112u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);
	opennova::np::configure_session_runtime(owner.ctx);

	opennova::world::World world;
	opennova::world::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	opennova::world::Entity old_player;
	old_player.item_id = 0x2222u;
	old_player.name = "OldGhost";
	const opennova::world::EntityHandle old_entity =
			world.registry.spawn(0, old_player);
	owner.ctx.world = &world;

	const opennova::PeerAddr peer{0x0100007Fu, 33110};
	opennova::np::PeerLink &old_link = owner.peers[peer];
	old_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	old_link.announced = true;
	opennova::np::NapiNPConnection old;
	old.peer = peer;
	old.type = 1;
	old.phase = opennova::np::ConnectionPhase::PlayerAdded;
	old.client_ci = 1;
	old.client_ck = 0x11112222u;
	old.client_scrk =
			"OLDPUMPCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	old.server_sk = 0x33334444u;
	old.server_scrk =
			"OLDPUMPSERVERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	old.link.owned_entity = old_entity;
	old.link.transport = old_link.transport.get();
	old.reply.player_slot = 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(old));

	opennova::ClientAuth replacement = opennova::make_jointoperations_client_auth(
			2, 0x55556666u, startup.host_key, "ReplacementJoiner",
			"NEWPUMPCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
	add_retail_game_environment(replacement);
	std::vector<uint8_t> auth = opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_CLIENT_AUTH,
			opennova::client_auth_to_bytes(replacement));
	CaptureDatagramSocket socket;
	socket.incoming.push_back({peer, std::move(auth)});
	opennova::np::host_session_pump(owner, socket);

	if (!expect(socket.sent.size() == 2 && socket.sent_to.size() == 2 &&
	                    socket.sent_to[0] == peer && socket.sent_to[1] == peer,
	            "replacement sends immediate 0x82 then fresh boundary 0x83"))
		return false;
	uint8_t auth_opcode = 0;
	uint8_t settings_opcode = 0;
	std::vector<uint8_t> auth_body;
	std::vector<uint8_t> settings_body;
	if (!expect(opennova::nw_decode_inbound(
	                    socket.sent[0].data(), socket.sent[0].size(),
	                    auth_opcode, auth_body) &&
	                    opennova::nw_decode_inbound(
	                            socket.sent[1].data(), socket.sent[1].size(),
	                            settings_opcode, settings_body) &&
	                    auth_opcode == opennova::SESSION_OPCODE_SERVER_AUTH &&
	                    settings_opcode ==
	                            opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	            "replacement preserves auth-before-established packet order"))
		return false;
	if (!expect(owner.peers.find(peer) == owner.peers.end(),
	            "replacement cleanup removes the old announced PeerLink")) return false;
	if (!expect(owner.ctx.np_protocol.connection_list.size() == 1,
	            "replacement connection survives owner cleanup in the same pump")) return false;
	const opennova::np::NapiNPConnection &fresh =
			owner.ctx.np_protocol.connection_list.front();
	if (!expect(fresh.client_ci == replacement.ci &&
	                    fresh.client_ck == replacement.ck &&
	                    fresh.player_name == replacement.na,
	            "surviving connection carries the replacement identity")) return false;
	if (!expect(fresh.link.transport == nullptr,
	            "replacement does not inherit the erased PeerLink transport")) return false;
	opennova::ProtocolPacketHeader settings_header;
	std::vector<opennova::ProtocolMessage> settings_messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
	                    settings_body.data(), settings_body.size(), fresh.server_scrk,
	                    settings_header, settings_messages) &&
	                    settings_header.seq_num == 1 &&
	                    !settings_messages.empty(),
	            "replacement sends its retained sequence-1 initial settings"))
		return false;
	bool old_ghost_survives = false;
	world.registry.for_each([&](const opennova::world::Entity &entity) {
		if (entity.item_id == 0x2222u && entity.name == "OldGhost")
			old_ghost_survives = true;
	});
	return expect(!old_ghost_survives,
	              "replacement pump despawns the old authoritative entity even if its slot is reused");
}

} // namespace

int main() {
	bool ok = true;
	ok = check_connection_mode_table() && ok;
	ok = check_single_player_signature() && ok;
	ok = check_create_session_brings_up_host() && ok;
	ok = check_dedicated_host_has_no_local_client() && ok;
	ok = check_production_serve_mode_has_no_phantom_and_mints_startup() && ok;
	ok = check_host_pump_batches_one_send_boundary() && ok;
	ok = check_host_s2c_holdoff_and_frame_envelope() && ok;
	ok = check_host_s2c_holdoff_is_per_connection() && ok;
	ok = check_initial_stream_obeys_connection_holdoff() && ok;
	ok = check_host_loopback_does_not_inherit_udp_envelope() && ok;
	ok = check_sparse_empty_slot_sweep_fragments_without_loss() && ok;
	ok = check_host_pump_reconnect_keeps_fresh_connection() && ok;
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
