// Drives a crafted JointOperations joiner through the npruntime server legs
// (opennova::np::handle_server_datagram / tick_connections / frame_in_match_s2c)
// end-to-end — handshake (0x41/0x42), SESSION-framed (0x43) mission/spawn flow to
// Spawned, the PeerSpawned pose recovered from the joiner's C2S 0x0C, the post-spawn
// PeerC2SInMatch surfacing, and the in-match S2C wrap.
//
// This is the P2 port of tests/novaworld/host_session_accept_test.cpp: the same
// crafted-datagram flow, retargeted onto the promoted np free functions over a
// NapiNPServerCtx (connection state on NapiNPConnection; the 0x43 spawn machine still
// driven through ctx.game_runtime). nw_encode_outbound is the client's exact inverse of
// the server's nw_decode_inbound, so it doubles as the joiner-side framer; the server's
// internally-generated SCRK is recovered from the ServerAuth reply, so the encrypted
// 0x83 replies decode without any test accessor.

#include <npruntime/napi_np_protocol.h>

#include "host_test_setup.h"

#include <netsim/loopback_channel.h>

#include <novaworld/nw_session_framing.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;

// The host advertises this key in ServerHello.hk; the join leg checks ClientAuth.hk against it, so
// crafted joiners echo it. [orig: NapiNPProtocol_HandleClientJoin @0x62b750 HK gate]
constexpr uint32_t kHostKey = 0x0FE0E112u;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

void push_u8(std::vector<uint8_t> &b, uint8_t v) { b.push_back(v); }
void push_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t((v >> 8) & 0xFF));
}
void push_u32(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back(uint8_t(v & 0xFF));
	b.push_back(uint8_t((v >> 8) & 0xFF));
	b.push_back(uint8_t((v >> 16) & 0xFF));
	b.push_back(uint8_t((v >> 24) & 0xFF));
}

uint16_t le16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// Craft an inbound NW-UDP datagram (opcode + plaintext body) the way a joiner would:
// nw_encode_outbound is envelope_encode∘nwu_decrypt, the exact inverse of the server's
// nw_decode_inbound (envelope_decode∘nwu_encrypt).
std::vector<uint8_t> craft(uint8_t opcode, std::vector<uint8_t> body) {
	return nw_encode_outbound(opcode, std::move(body));
}

// Craft an inbound 0x43 SESSION datagram carrying `messages`, inner-encrypted with the
// joiner's SCRK (== the server's stored client_scrk).
std::vector<uint8_t> craft_session(std::string_view client_scrk, uint32_t seq,
                                   const std::vector<ProtocolMessage> &messages) {
	ProtocolPacketHeader hdr;
	hdr.session_id = 0; // server reads seq_num only; session_id unchecked on receive
	hdr.seq_num = seq;
	hdr.ack_count = 0;
	hdr.connection_flags = 0;
	std::vector<uint8_t> body;
	encode_protocol_packet_plaintext(hdr, messages, client_scrk, body);
	return craft(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

// The 48-byte C2S 0x0C body: 5-byte entity sub-header + 43-byte extended uplink.
std::vector<uint8_t> make_uplink_0c(uint16_t handle, uint16_t type_id, uint16_t vehicle,
                                    uint32_t x, uint32_t y, uint32_t z,
                                    uint16_t heading, uint16_t pitch) {
	std::vector<uint8_t> p;
	push_u16(p, handle);
	push_u16(p, type_id);
	push_u8(p, 0x0A); // sub_op = extended (type-10)
	push_u16(p, vehicle);
	push_u32(p, x);
	push_u32(p, y);
	push_u32(p, z);
	push_u16(p, heading);
	push_u16(p, pitch);
	for (int i = 0; i < 25; ++i) push_u8(p, 0); // remainder of the 43-byte body
	return p;
}

// Decode a server 0x83 SESSION reply with the recovered server SCRK.
bool decode_s2c(const std::vector<uint8_t> &datagram, std::string_view server_scrk,
                ProtocolPacketHeader &hdr, std::vector<ProtocolMessage> &messages) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!nw_decode_inbound(datagram.data(), datagram.size(), opcode, body)) return false;
	if (opcode != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE) return false;
	return decode_protocol_packet_plaintext(body.data(), body.size(), server_scrk, hdr, messages);
}

bool has_event(const np::HandleResult &r, np::HostAcceptEvent::Kind kind) {
	for (const auto &e : r.events) if (e.kind == kind) return true;
	return false;
}

const np::HostAcceptEvent *find_event(const np::HandleResult &r, np::HostAcceptEvent::Kind kind) {
	for (const auto &e : r.events) if (e.kind == kind) return &e;
	return nullptr;
}

bool reply_has_tag(const std::vector<ProtocolMessage> &msgs, uint8_t tag) {
	for (const auto &m : msgs) if (m.tag == tag) return true;
	return false;
}

bool run() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	const PeerAddr peer{0x0100007Fu, 30000}; // 127.0.0.1:30000 (LE octet pack)
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB"; // 61
	const uint32_t client_ck = 0xDEADBEEFu;

	// --- 0x41 ClientHello -> 0x81 ServerHello ---
	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "TestJoiner"; // the joiner's player name (CO) — must reach PeerSpawned
	hello.ci = 1;
	auto r41 = np::handle_server_datagram(ctx, peer, nullptr, 0); // empty: must not crash / no events
	(void)r41;
	{
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
		if (!expect(r.outbound.size() == 1, "0x41 produces one outbound (ServerHello)")) return false;
		if (!expect(has_event(r, np::HostAcceptEvent::Kind::PeerHandshakeAdvanced),
		            "0x41 emits PeerHandshakeAdvanced")) return false;
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(r.outbound[0].data(), r.outbound[0].size(), op, body) &&
		            op == SESSION_OPCODE_SERVER_HELLO, "0x41 reply decodes to 0x81")) return false;
		ServerHello sh;
		if (!expect(parse_server_hello(body.data(), body.size(), sh), "0x81 parses")) return false;
		if (!expect(sh.ci == hello.ci, "ServerHello echoes ClientHello.ci")) return false;
	}

	// --- 0x42 ClientAuth -> 0x82 ServerAuth (recover server SCRK) ---
	std::string server_scrk;
	{
		ClientAuth auth;
		auth.pn = "JointOperations";
		auth.ci = 1;
		auth.ck = client_ck;
		auth.hk = kHostKey; // echo ServerHello.hk (the join HK gate)
		auth.na = "jop:cus2";
		auth.scrk = client_scrk;
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 2);
		if (!expect(r.outbound.size() == 1, "0x42 produces one outbound (ServerAuth)")) return false;
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(r.outbound[0].data(), r.outbound[0].size(), op, body) &&
		            op == SESSION_OPCODE_SERVER_AUTH, "0x42 reply decodes to 0x82")) return false;
		ServerAuth sa;
		if (!expect(parse_server_auth(body.data(), body.size(), sa), "0x82 parses")) return false;
		if (!expect(sa.cr == 1, "ServerAuth accepts (cr == 1)")) return false;
		if (!expect(sa.ck == client_ck, "ServerAuth echoes ClientAuth.ck")) return false;
		if (!expect(sa.scrk.size() == 61, "ServerAuth SCRK is 61 chars")) return false;
		server_scrk = sa.scrk;
	}

	// --- 0x43 mission request, then drive the periodic emitter to World streaming ---
	uint32_t seq = 1;
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x37, {})});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 100);
	}
	// Drain the mission bootstrap queue + climb entity_batch_count (the spawn gate needs
	// entity_batch_count > 0). elapsed=300 crosses the 250ms tag10 interval each tick once
	// the queue is drained.
	for (int i = 0; i < 40; ++i) np::tick_connections(ctx, 300, static_cast<uint32_t>(1000 + i));

	// Transition marker + a few more streaming ticks (faithful to the wire order).
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x09, {})});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 200);
	}
	for (int i = 0; i < 6; ++i) np::tick_connections(ctx, 300, static_cast<uint32_t>(2000 + i));

	// Joiner reports its pose via C2S 0x0C BEFORE spawn — caches client_* so the
	// PeerSpawned event carries it.
	const uint16_t up_handle = 0x0003, up_type = 0x14B9, up_vehicle = 0x2222;
	const uint32_t up_x = 0x11223344u, up_y = 0x55667788u, up_z = 0x99AABBCCu;
	const uint16_t up_head = 0x1234, up_pitch = 0x5678;
	{
		auto body = make_uplink_0c(up_handle, up_type, up_vehicle, up_x, up_y, up_z, up_head, up_pitch);
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x0C, body)});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 300);
		if (!expect(!has_event(r, np::HostAcceptEvent::Kind::PeerC2SInMatch),
		            "pre-spawn 0x0C does not surface PeerC2SInMatch (no connection yet)")) return false;
	}
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x22, {0x00, 0xF7, 0x1C})});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 320);
	}
	np::tick_connections(ctx, 300, 2500);

	// --- The loadout/status burst trips the spawn gate -> PeerSpawned ---
	{
		auto dg = craft_session(client_scrk, seq++, {
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
		});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 450);

		const np::HostAcceptEvent *spawn = find_event(r, np::HostAcceptEvent::Kind::PeerSpawned);
		if (!expect(spawn != nullptr, "loadout burst trips the gate -> PeerSpawned")) return false;
		if (!expect(spawn->peer == peer, "PeerSpawned names the joiner peer")) return false;
		if (!expect(spawn->peer_name == "TestJoiner",
		            "PeerSpawned carries the joiner's ClientHello.co player name")) return false;
		if (!expect(spawn->pose.pos_valid, "PeerSpawned pose is from the joiner uplink")) return false;
		if (!expect(spawn->pose.entity_handle == up_handle, "PeerSpawned entity handle matches 0x0C")) return false;
		if (!expect(spawn->pose.item_type_id == up_type, "PeerSpawned item type matches 0x0C")) return false;
		if (!expect(spawn->pose.pos_x == static_cast<int32_t>(up_x) &&
		            spawn->pose.pos_y == static_cast<int32_t>(up_y) &&
		            spawn->pose.pos_z == static_cast<int32_t>(up_z),
		            "PeerSpawned position matches 0x0C")) return false;
		if (!expect(spawn->pose.heading == static_cast<int16_t>(up_head) &&
		            spawn->pose.pitch == static_cast<int16_t>(up_pitch),
		            "PeerSpawned heading/pitch match 0x0C")) return false;
		if (!expect(np::connection_spawned(ctx, peer), "peer marked spawned after the gate")) return false;

		// The game-start bundle must carry 0x0F WORLD-STATE-LOAD (the joiner's spawn pos/slot) but
		// MUST NOT carry 0x25 RESET_AND_START — 0x25 re-arms the spawn gate g_spawn_success_gate
		// after the joiner's Game_StartMission cleared it, blocking deploy + arming the reason=4
		// kick (net-re §5.38c / D-NET-99; retail host_and_join_lan.pcapng sends no 0x25 before deploy).
		if (!expect(!r.outbound.empty(), "spawn burst produces a framed 0x83 reply")) return false;
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> msgs;
		if (!expect(decode_s2c(r.outbound.back(), server_scrk, hdr, msgs),
		            "spawn reply decodes as a 0x83 SESSION packet")) return false;
		if (!expect(hdr.session_id == client_ck, "S2C session_id == joiner's ClientAuth.ck")) return false;
		if (!expect(!reply_has_tag(msgs, 0x25), "game-start bundle does NOT carry 0x25 (re-arms spawn gate)")) return false;
		if (!expect(reply_has_tag(msgs, 0x0F), "game-start bundle carries 0x0F world-state-load")) return false;
	}

	// --- Post-spawn: a C2S 0x0C now surfaces PeerC2SInMatch for NetSystem ---
	{
		auto body = make_uplink_0c(up_handle, up_type, 0xFFFF, up_x + 1, up_y, up_z, up_head, up_pitch);
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x0C, body)});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 500);
		const np::HostAcceptEvent *c2s = find_event(r, np::HostAcceptEvent::Kind::PeerC2SInMatch);
		if (!expect(c2s != nullptr, "post-spawn 0x0C surfaces PeerC2SInMatch")) return false;
		if (!expect(c2s->in_match_c2s.size() == 1 && c2s->in_match_c2s[0].tag == 0x0C,
		            "PeerC2SInMatch carries the 0x0C message")) return false;
	}

	// --- In-match S2C wrap: NetSystem's 0x0A body -> a 0x83 SESSION datagram ---
	{
		std::vector<uint8_t> frame_body = {0x01, 0x02, 0x03, 0x04, 0x05};
		std::vector<uint8_t> dg;
		if (!expect(np::frame_in_match_s2c(ctx, peer, 0x0A, frame_body, dg),
		            "frame_in_match_s2c succeeds for a spawned peer")) return false;
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> msgs;
		if (!expect(decode_s2c(dg, server_scrk, hdr, msgs), "in-match S2C decodes as 0x83")) return false;
		if (!expect(hdr.session_id == client_ck, "in-match S2C session_id == joiner's ck")) return false;
		if (!expect(msgs.size() == 1 && msgs[0].tag == 0x0A, "in-match S2C carries one 0x0A")) return false;
		if (!expect(msgs[0].payload == frame_body, "in-match S2C 0x0A payload round-trips")) return false;
	}

	// Unknown peer can't be framed.
	{
		std::vector<uint8_t> dg;
		const PeerAddr stranger{0x0100007Fu, 40000};
		if (!expect(!np::frame_in_match_s2c(ctx, stranger, 0x0A, {0x00}, dg),
		            "frame_in_match_s2c rejects an unknown peer")) return false;
	}

	// --- Goodbye drops the peer ---
	{
		auto dg = craft(SESSION_OPCODE_CLIENT_GOODBYE, std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 600);
		if (!expect(has_event(r, np::HostAcceptEvent::Kind::PeerGoodbye), "0x46 emits PeerGoodbye")) return false;
		if (!expect(!np::connection_spawned(ctx, peer), "peer cleared after goodbye")) return false;
		if (!expect(np::connection_count(ctx) == 0, "peer table empty after goodbye")) return false;
	}

	return true;
}

// F3 dcb-timing: the joiner's own organic-spawn 0x0C must be admitted during world streaming
// (PeerEnteredWorldStreaming) STRICTLY BEFORE the game-start bundle (PeerSpawned), so the retail
// client's load-time Player_InitPlayer @0x4e15f0 finds its dcb in pool 0 (else
// Player_FatalPlayerDcbNotFound fatals). This proves the event ORDERING in the server legs.
bool run_joiner_0c_streams_before_game_start() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	const PeerAddr peer{0x0100007Fu, 30500}; // 127.0.0.1:30500
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB"; // 61
	const uint32_t client_ck = 0xCAFEF00Du;

	// --- handshake + auth ---
	{
		ClientHello hello;
		hello.pn = "JointOperations";
		hello.co = "StreamJoiner"; // the joiner's player name, echoed into the 0x0C name
		hello.ci = 1;
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	}
	{
		ClientAuth auth;
		auth.pn = "JointOperations";
		auth.ci = 1;
		auth.ck = client_ck;
		auth.hk = kHostKey; // echo ServerHello.hk (the join HK gate)
		auth.na = "jop:cus2";
		auth.scrk = client_scrk;
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 2);
	}

	uint32_t seq = 1;
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x37, {})});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 100);
	}

	// The joiner reports its own ConnectionId (unk_18 = its dcb) in a 0x48 client-ack during the
	// early handshake. The host must learn this and stamp it into the joiner's 0x0C entity_flags
	// (else Player_FindLocalPlayerEntity fails). Use a non-sequential value to prove the host
	// carries the WIRE value, not a guess. [F3, capture2.pcapng ack=0x113F]
	const uint32_t kJoinerDcb = 0x0000113Fu;
	{
		std::vector<uint8_t> ack = {0x3F, 0x11, 0x00, 0x00}; // LE 0x113F
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x48, ack)});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 110);
	}

	// Capture the FIRST PeerEnteredWorldStreaming (from either the periodic tick path or the
	// datagram path) and whether the peer was still pre-spawn at that moment.
	bool got_stream = false;
	bool spawned_at_stream = true;
	uint32_t stream_self_id = 0;
	std::string stream_name;
	auto scan_ticks = [&](const std::vector<np::TickOut> &outs) {
		for (const auto &t : outs)
			for (const auto &e : t.events)
				if (e.kind == np::HostAcceptEvent::Kind::PeerEnteredWorldStreaming && !got_stream) {
					got_stream = true;
					stream_name = e.peer_name;
					stream_self_id = e.self_id;
					spawned_at_stream = np::connection_spawned(ctx, peer);
				}
	};
	auto scan_res = [&](const np::HandleResult &r) {
		for (const auto &e : r.events)
			if (e.kind == np::HostAcceptEvent::Kind::PeerEnteredWorldStreaming && !got_stream) {
				got_stream = true;
				stream_name = e.peer_name;
				stream_self_id = e.self_id;
				spawned_at_stream = np::connection_spawned(ctx, peer);
			}
	};

	// Drain the mission bootstrap + climb entity_batch_count past 0 (streaming begins).
	for (int i = 0; i < 40; ++i) {
		auto o = np::tick_connections(ctx, 300, static_cast<uint32_t>(1000 + i));
		scan_ticks(o);
	}
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x09, {})});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 200);
		scan_res(r);
	}
	for (int i = 0; i < 6; ++i) {
		auto o = np::tick_connections(ctx, 300, static_cast<uint32_t>(2000 + i));
		scan_ticks(o);
	}

	if (!expect(got_stream, "PeerEnteredWorldStreaming surfaces once world streaming begins")) return false;
	if (!expect(stream_self_id == kJoinerDcb,
	            "streaming event carries the joiner's 0x48-ack ConnectionId for the 0x0C entity_flags")) return false;
	if (!expect(stream_name == "StreamJoiner",
	            "streaming event carries the joiner's ClientHello.co for the 0x0C name-match")) return false;
	if (!expect(!spawned_at_stream,
	            "joiner is NOT yet spawned when its 0x0C streams (0x0C precedes the game-start bundle)")) return false;
	if (!expect(!np::connection_spawned(ctx, peer), "still pre-spawn after the streaming ticks")) return false;

	// Now trip the spawn gate: the loadout/status burst -> PeerSpawned, strictly AFTER the
	// streaming event observed above.
	{
		auto body = make_uplink_0c(0x0003, 0x14B9, 0x2222, 0x11223344u, 0x55667788u, 0x99AABBCCu,
		                           0x1234, 0x5678);
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x0C, body)});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 300);
	}
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x22, {0x00, 0xF7, 0x1C})});
		np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 320);
	}
	np::tick_connections(ctx, 300, 2500);
	{
		auto dg = craft_session(client_scrk, seq++, {
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
		});
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 450);
		if (!expect(find_event(r, np::HostAcceptEvent::Kind::PeerSpawned) != nullptr,
		            "loadout burst trips the gate -> PeerSpawned (strictly after streaming)")) return false;
	}
	if (!expect(np::connection_spawned(ctx, peer), "peer spawned after the gate")) return false;
	return true;
}

bool run_bound_entity_handle_drives_tag51() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	const PeerAddr peer{0x0100007Fu, 30600};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t client_ck = 0xBEEFCAFEu;

	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "SlotJoiner";
	hello.ci = 1;
	auto hello_dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	np::handle_server_datagram(ctx, peer, hello_dg.data(), hello_dg.size(), 1);

	std::string server_scrk;
	ClientAuth auth;
	auth.pn = "JointOperations";
	auth.ci = 1;
	auth.ck = client_ck;
	auth.hk = kHostKey; // echo ServerHello.hk (the join HK gate)
	auth.na = "jop:cus2";
	auth.scrk = client_scrk;
	auto auth_dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto auth_r = np::handle_server_datagram(ctx, peer, auth_dg.data(), auth_dg.size(), 2);
	if (!expect(auth_r.outbound.size() == 1, "auth produces one outbound")) return false;
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!expect(nw_decode_inbound(auth_r.outbound[0].data(), auth_r.outbound[0].size(), op, body) &&
	            op == SESSION_OPCODE_SERVER_AUTH, "auth reply decodes to 0x82")) return false;
	ServerAuth sa;
	if (!expect(parse_server_auth(body.data(), body.size(), sa), "0x82 parses")) return false;
	server_scrk = sa.scrk;

	uint32_t seq = 1;
	auto mission_dg = craft_session(client_scrk, seq++, {make_protocol_message(0x37, {})});
	np::handle_server_datagram(ctx, peer, mission_dg.data(), mission_dg.size(), 100);
	if (!expect(np::bind_connection_player(ctx, peer, 1, 0x0005),
			"server binds the connection to its allocated player entity handle")) return false;

	auto spawn_req = craft_session(client_scrk, seq++, {make_protocol_message(0x29, {0x00, 0x00})});
	auto spawn_r = np::handle_server_datagram(ctx, peer, spawn_req.data(), spawn_req.size(), 200);
	if (!expect(!spawn_r.outbound.empty(), "0x29 produces a framed 0x83 reply")) return false;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!expect(decode_s2c(spawn_r.outbound.back(), server_scrk, hdr, msgs),
	            "0x29 reply decodes as a 0x83 SESSION packet")) return false;
	const ProtocolMessage *tag51 = nullptr;
	for (const ProtocolMessage &m : msgs) {
		if (m.tag == 0x51) tag51 = &m;
	}
	if (!expect(tag51 != nullptr, "0x29 emits tag=0x51")) return false;
	if (!expect(tag51->payload.size() == 8, "tag=0x51 payload is 8 bytes")) return false;
	if (!expect(le16(tag51->payload.data() + 2) == 0x0005,
			"tag=0x51 entity slot uses the bound connection handle")) return false;
	return true;
}

bool run_bound_peer_identity_drives_player_sync() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);

	const PeerAddr peer{0x0100007Fu, 30700};
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t client_ck = 0xABCD1234u;

	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "SlotJoiner";
	hello.ci = 1;
	auto hello_dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	np::handle_server_datagram(ctx, peer, hello_dg.data(), hello_dg.size(), 1);

	ClientAuth auth;
	auth.pn = "JointOperations";
	auth.ci = 1;
	auth.ck = client_ck;
	auth.hk = kHostKey; // echo ServerHello.hk (the join HK gate)
	auth.na = "jop:cus2";
	auth.scrk = client_scrk;
	auto auth_dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
	auto auth_r = np::handle_server_datagram(ctx, peer, auth_dg.data(), auth_dg.size(), 2);
	if (!expect(auth_r.outbound.size() == 1, "auth produces one outbound")) return false;
	uint8_t op = 0;
	std::vector<uint8_t> body;
	if (!expect(nw_decode_inbound(auth_r.outbound[0].data(), auth_r.outbound[0].size(), op, body) &&
	            op == SESSION_OPCODE_SERVER_AUTH, "auth reply decodes to 0x82")) return false;
	ServerAuth sa;
	if (!expect(parse_server_auth(body.data(), body.size(), sa), "0x82 parses")) return false;
	const std::string server_scrk = sa.scrk;

	uint32_t seq = 1;
	auto mission_dg = craft_session(client_scrk, seq++, {make_protocol_message(0x37, {})});
	np::handle_server_datagram(ctx, peer, mission_dg.data(), mission_dg.size(), 100);
	if (!expect(np::bind_connection_player(ctx, peer, 1, 0x0005),
			"server binds the connection to its allocated player entity handle")) return false;

	for (int i = 0; i < 40; ++i) {
		np::tick_connections(ctx, 300, static_cast<uint32_t>(1000 + i));
	}
	auto sync_ack = craft_session(client_scrk, seq++, {
			make_protocol_message(0x22, {0x00, 0xF7, 0x1C}),
	});
	np::handle_server_datagram(ctx, peer, sync_ack.data(), sync_ack.size(), 2000);

	const ProtocolMessage *tag46 = nullptr;
	std::vector<ProtocolMessage> decoded_msgs;
	for (int i = 0; i < 20 && tag46 == nullptr; ++i) {
		auto outs = np::tick_connections(ctx, 300, static_cast<uint32_t>(3000 + i));
		for (const np::TickOut &out : outs) {
			for (const std::vector<uint8_t> &dg : out.outbound) {
				ProtocolPacketHeader hdr;
				std::vector<ProtocolMessage> msgs;
				if (!decode_s2c(dg, server_scrk, hdr, msgs)) continue;
				for (const ProtocolMessage &m : msgs) {
					if (m.tag == 0x46) {
						decoded_msgs.push_back(m);
						tag46 = &decoded_msgs.back();
						break;
					}
				}
				if (tag46) break;
			}
			if (tag46) break;
		}
	}
	if (!expect(tag46 != nullptr, "client player-sync ack produces tag=0x46")) return false;
	if (!expect(tag46->payload.size() >= 12, "tag=0x46 payload carries slot/entity/name")) return false;
	if (!expect(tag46->payload[0] == 1, "first remote peer uses player slot 1")) return false;
	if (!expect(tag46->payload[3] == 5, "player sync entity slot uses the peer entity handle")) return false;
	if (!expect(std::strncmp(reinterpret_cast<const char *>(tag46->payload.data() + 4),
			"SlotJoiner", 10) == 0, "player sync name uses ClientHello.co")) return false;
	return true;
}

// Build a crafted 0x42 ClientAuth datagram with the given PN / HK (for the rejection cases).
std::vector<uint8_t> craft_auth(const std::string &pn, uint32_t hk, uint32_t ck,
                                std::string_view scrk) {
	ClientAuth auth;
	auth.pn = pn;
	auth.ci = 1;
	auth.ck = ck;
	auth.hk = hk;
	auth.na = "jop:cus2";
	auth.scrk = scrk;
	return craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
}

bool run_non_jo_peer_is_ignored() {
	// The join legs validate the JO identity + HK echo (the @0x62b750 gate): a lobby (non-JO) PN, a
	// non-JO 0x42, and a wrong-HK 0x42 must all be dropped with no reply and no connection.
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 31000};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

	// (a) non-JO ClientHello -> no ServerHello, no node.
	{
		ClientHello hello;
		hello.pn = "NOVAWORLDUDP";
		hello.ci = 1;
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
		if (!expect(r.outbound.empty(), "lobby PN produces no JO ServerHello")) return false;
		if (!expect(np::connection_count(ctx) == 0, "lobby PN registers no JO connection")) return false;
	}
	// (b) non-JO ClientAuth -> no ServerAuth, no node (the join re-validates PN).
	{
		auto dg = craft_auth("NOVAWORLDUDP", kHostKey, 0xDEADBEEFu, scrk);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 2);
		if (!expect(r.outbound.empty(), "non-JO 0x42 produces no ServerAuth")) return false;
		if (!expect(np::connection_count(ctx) == 0, "non-JO 0x42 registers no connection")) return false;
	}
	// (c) JO ClientAuth with the WRONG host key -> dropped (HK echo gate).
	{
		auto dg = craft_auth("JointOperations", kHostKey ^ 0x1u, 0xDEADBEEFu, scrk);
		auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 3);
		if (!expect(r.outbound.empty(), "wrong-HK 0x42 produces no ServerAuth")) return false;
		if (!expect(np::connection_count(ctx) == 0, "wrong-HK 0x42 registers no connection")) return false;
	}
	return true;
}

// Fix #2: a live handshake against a host that was NOT brought up (host_running == 0) is rejected.
bool run_handshake_rejected_when_host_down() {
	np::NapiNPServerCtx ctx;
	np::configure_session_runtime(ctx); // runtime only — NO create_session, so host_running stays 0
	if (!expect(ctx.np_protocol.host_running == 0, "host not running before create_session")) return false;
	const PeerAddr peer{0x0100007Fu, 31100};

	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "EarlyBird";
	hello.ci = 1;
	auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto r = np::handle_server_datagram(ctx, peer, dg.data(), dg.size(), 1);
	if (!expect(r.outbound.empty(), "0x41 to a down host produces no ServerHello")) return false;
	if (!expect(np::connection_count(ctx) == 0, "0x41 to a down host registers no connection")) return false;
	return true;
}

// Fix #1: the full P0->P1->P2 listen-host bring-up preserves the host's own type-2 loopback through
// configure_session_runtime, and a remote joiner is added alongside it (not in place of it).
bool run_listen_host_lifecycle() {
	netsim::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::NapiGameSettings settings;
	settings.max_players = 8; // co-op listen host: host loopback + up to 7 joiners (capacity gate)
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        kHostKey, &loopback, settings);

	if (!expect(ctx.is_in_session == 1, "listen host is in session")) return false;
	if (!expect(ctx.is_authority == 1 && ctx.is_mp_session_peer == 1, "HostClient = host + client")) return false;
	if (!expect(ctx.np_protocol.host_running == 1, "listen host is running")) return false;
	if (!expect(ctx.np_protocol.host_key == kHostKey, "host key seeded")) return false;
	// The loopback survived configure_session_runtime (only type-1 remote nodes are cleared).
	if (!expect(np::connection_count(ctx) == 1, "loopback preserved through configure_session_runtime")) return false;
	if (!expect(ctx.np_protocol.connection_list[0].type == 2, "preserved node is the type-2 loopback")) return false;

	// A remote joiner handshakes -> a type-1 node is added ALONGSIDE the loopback.
	const PeerAddr peer{0x0100007Fu, 31200};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	ClientHello hello;
	hello.pn = "JointOperations";
	hello.co = "RemoteJoiner";
	hello.ci = 1;
	auto h = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto rh = np::handle_server_datagram(ctx, peer, h.data(), h.size(), 1);
	if (!expect(rh.outbound.size() == 1, "remote 0x41 -> one ServerHello")) return false;
	auto a = craft_auth("JointOperations", kHostKey, 0xDEADBEEFu, scrk);
	auto ra = np::handle_server_datagram(ctx, peer, a.data(), a.size(), 2);
	if (!expect(ra.outbound.size() == 1, "remote 0x42 -> one ServerAuth")) return false;

	if (!expect(np::connection_count(ctx) == 2, "joiner added alongside the loopback")) return false;
	int loopbacks = 0, remotes = 0;
	for (const auto &c : ctx.np_protocol.connection_list) {
		if (c.type == 2) ++loopbacks;
		else if (c.type == 1) ++remotes;
	}
	if (!expect(loopbacks == 1 && remotes == 1, "exactly one loopback + one remote joiner")) return false;
	return true;
}

// Defect #1: a retransmitted 0x42 ClientAuth (normal lossy UDP) for an already-joined connection
// must RE-SEND the cached ServerAuth, not re-mint the server SCRK/SK. A re-mint rotates the keys the
// joiner already latched from the first 0x82, so its later 0x83s would fail to decrypt and the join
// would silently stall. [orig: HandleClientJoin @0x62b750 — conn_state==1 && CI && CK match ->
// NapiNPConnection_SendSessionInit @0x620ef0]
bool run_retransmit_0x42_keeps_keys() {
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey);
	const PeerAddr peer{0x0100007Fu, 30800};
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	const uint32_t ck = 0x12345678u;

	auto first_auth = [&](uint32_t now, ServerAuth &out_sa) -> bool {
		auto a = craft_auth("JointOperations", kHostKey, ck, scrk); // craft_auth uses CI = 1
		auto r = np::handle_server_datagram(ctx, peer, a.data(), a.size(), now);
		if (!expect(r.outbound.size() == 1, "0x42 -> one ServerAuth")) return false;
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!expect(nw_decode_inbound(r.outbound[0].data(), r.outbound[0].size(), op, body) &&
		            op == SESSION_OPCODE_SERVER_AUTH, "0x42 reply decodes to 0x82")) return false;
		return expect(parse_server_auth(body.data(), body.size(), out_sa), "0x82 parses");
	};

	ServerAuth sa1, sa2;
	if (!first_auth(1, sa1)) return false;
	if (!first_auth(2, sa2)) return false; // identical retransmit (same CI + CK)

	// SK is server-minted per join (make_random_session_u32) — the meaningful regression teeth: it
	// would change on a re-mint. SCRK and MI (the connection_id) must also be stable.
	if (!expect(sa2.sk == sa1.sk, "retransmit re-sends the SAME ServerAuth SK (no re-mint)")) return false;
	if (!expect(sa2.scrk == sa1.scrk, "retransmit re-sends the SAME ServerAuth SCRK (no re-mint)")) return false;
	if (!expect(sa2.mi == sa1.mi, "retransmit re-sends the SAME MI (connection_id)")) return false;
	if (!expect(np::connection_count(ctx) == 1, "retransmit does not create a second node")) return false;
	return true;
}

// Defect #3: the join leg enforces capacity. A dedicated host with max_players == 2 admits two
// joiners; the third 0x42 is rejected with no ServerAuth and no node. [orig:
// CNapiNetwork_ValidateJoinRequest @0x4c61b0 — current_player_count >= max_players]
bool run_capacity_rejects_when_full() {
	np::NapiNPServerCtx ctx;
	np::NapiGameSettings settings;
	settings.max_players = 2; // dedicated host: two joiner slots, no host loopback
	np::test::bring_up_host(ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan, kHostKey,
	                        nullptr, settings);
	const std::string scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";

	auto join = [&](const PeerAddr &p, uint32_t ck, uint32_t now) {
		auto a = craft_auth("JointOperations", kHostKey, ck, scrk);
		return np::handle_server_datagram(ctx, p, a.data(), a.size(), now);
	};

	if (!expect(join(PeerAddr{0x0100007Fu, 32001}, 0x1111u, 1).outbound.size() == 1,
	            "joiner 1 admitted (ServerAuth)")) return false;
	if (!expect(join(PeerAddr{0x0100007Fu, 32002}, 0x2222u, 2).outbound.size() == 1,
	            "joiner 2 admitted (ServerAuth)")) return false;
	if (!expect(np::connection_count(ctx) == 2, "two joiners fill the server")) return false;

	auto r3 = join(PeerAddr{0x0100007Fu, 32003}, 0x3333u, 3);
	if (!expect(r3.outbound.empty(), "over-capacity 0x42 produces no ServerAuth")) return false;
	if (!expect(np::connection_count(ctx) == 2, "over-capacity join creates no node")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = run() && ok;
	ok = run_joiner_0c_streams_before_game_start() && ok;
	ok = run_bound_entity_handle_drives_tag51() && ok;
	ok = run_bound_peer_identity_drives_player_sync() && ok;
	ok = run_non_jo_peer_is_ignored() && ok;
	ok = run_handshake_rejected_when_host_down() && ok;
	ok = run_listen_host_lifecycle() && ok;
	ok = run_retransmit_0x42_keeps_keys() && ok;
	ok = run_capacity_rejects_when_full() && ok;
	return ok ? 0 : 1;
}
