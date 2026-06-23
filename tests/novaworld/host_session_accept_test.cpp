// Drives a crafted JointOperations joiner through novaworld::HostSessionAccept
// end-to-end — handshake (0x41/0x42), SESSION-framed (0x43) mission/spawn flow
// to Spawned, the PeerSpawned pose recovered from the joiner's C2S 0x0C, the
// post-spawn PeerC2SInMatch surfacing, and the in-match S2C wrap.
//
// No sockets: the crafted-datagram style of game_session_test + session
// round-trips. nw_encode_outbound is the client's exact inverse of the
// component's nw_decode_inbound, so it doubles as the joiner-side framer; the
// component's internally-generated server SCRK is recovered from the ServerAuth
// reply, so the encrypted 0x83 replies decode without any test accessor.

#include <novaworld/host_session_accept.h>
#include <novaworld/nw_session_framing.h>
#include <novaworld/protocol_message.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;

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

// Craft an inbound NW-UDP datagram (opcode + plaintext body) the way a joiner
// would: nw_encode_outbound is envelope_encode∘nwu_decrypt, the exact inverse
// of the component's nw_decode_inbound (envelope_decode∘nwu_encrypt).
std::vector<uint8_t> craft(uint8_t opcode, std::vector<uint8_t> body) {
	return nw_encode_outbound(opcode, std::move(body));
}

// Craft an inbound 0x43 SESSION datagram carrying `messages`, inner-encrypted
// with the joiner's SCRK (== the component's stored client_scrk).
std::vector<uint8_t> craft_session(std::string_view client_scrk, uint32_t seq,
                                   const std::vector<ProtocolMessage> &messages) {
	ProtocolPacketHeader hdr;
	hdr.session_id = 0; // component reads seq_num only; session_id unchecked on receive
	hdr.seq_num = seq;
	hdr.ack_count = 0;
	hdr.connection_flags = 0;
	std::vector<uint8_t> body;
	encode_protocol_packet_plaintext(hdr, messages, client_scrk, body);
	return craft(SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(body));
}

// The 48-byte C2S 0x0C body: 5-byte entity sub-header + 43-byte extended uplink
// (mirrors game_session_test check_client_position_is_state_only).
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

bool has_event(const HostSessionAccept::HandleResult &r, HostAcceptEvent::Kind kind) {
	for (const auto &e : r.events) if (e.kind == kind) return true;
	return false;
}

const HostAcceptEvent *find_event(const HostSessionAccept::HandleResult &r,
                                  HostAcceptEvent::Kind kind) {
	for (const auto &e : r.events) if (e.kind == kind) return &e;
	return nullptr;
}

bool reply_has_tag(const std::vector<ProtocolMessage> &msgs, uint8_t tag) {
	for (const auto &m : msgs) if (m.tag == tag) return true;
	return false;
}

bool run() {
	HostSessionAccept accept;
	accept.start();

	const PeerAddr peer{0x0100007Fu, 30000}; // 127.0.0.1:30000 (LE octet pack)
	const std::string client_scrk = "TESTCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB"; // 61
	const uint32_t client_ck = 0xDEADBEEFu;

	// --- 0x41 ClientHello -> 0x81 ServerHello ---
	ClientHello hello;
	hello.pn = "JointOperations";
	hello.ci = 1;
	auto r41 = accept.handle_datagram(peer, nullptr, 0); // empty: must not crash / no events
	(void)r41;
	{
		auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
		auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 1);
		if (!expect(r.outbound.size() == 1, "0x41 produces one outbound (ServerHello)")) return false;
		if (!expect(has_event(r, HostAcceptEvent::Kind::PeerHandshakeAdvanced),
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
		auth.na = "jop:cus2";
		auth.scrk = client_scrk;
		auto dg = craft(SESSION_OPCODE_CLIENT_AUTH, client_auth_to_bytes(auth));
		auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 2);
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
		accept.handle_datagram(peer, dg.data(), dg.size(), 100);
	}
	// Drain the mission bootstrap queue + climb entity_batch_count (the spawn
	// gate needs entity_batch_count > 0). elapsed=300 crosses the 250ms tag10
	// interval each tick once the queue is drained.
	for (int i = 0; i < 40; ++i) accept.tick_handshakes(300, static_cast<uint32_t>(1000 + i));

	// Transition marker + a few more streaming ticks (faithful to the wire order).
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x09, {})});
		accept.handle_datagram(peer, dg.data(), dg.size(), 200);
	}
	for (int i = 0; i < 6; ++i) accept.tick_handshakes(300, static_cast<uint32_t>(2000 + i));

	// Joiner reports its pose via C2S 0x0C BEFORE spawn — caches client_* so the
	// PeerSpawned event carries it.
	const uint16_t up_handle = 0x0003, up_type = 0x14B9, up_vehicle = 0x2222;
	const uint32_t up_x = 0x11223344u, up_y = 0x55667788u, up_z = 0x99AABBCCu;
	const uint16_t up_head = 0x1234, up_pitch = 0x5678;
	{
		auto body = make_uplink_0c(up_handle, up_type, up_vehicle, up_x, up_y, up_z, up_head, up_pitch);
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x0C, body)});
		auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 300);
		if (!expect(!has_event(r, HostAcceptEvent::Kind::PeerC2SInMatch),
		            "pre-spawn 0x0C does not surface PeerC2SInMatch (no connection yet)")) return false;
	}
	{
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x22, {0x00, 0xF7, 0x1C})});
		accept.handle_datagram(peer, dg.data(), dg.size(), 320);
	}
	accept.tick_handshakes(300, 2500);

	// --- The loadout/status burst trips the spawn gate -> PeerSpawned ---
	{
		auto dg = craft_session(client_scrk, seq++, {
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x2F, std::vector<uint8_t>(35, 0)),
				make_protocol_message(0x0B, std::vector<uint8_t>(13, 0)),
		});
		auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 450);

		const HostAcceptEvent *spawn = find_event(r, HostAcceptEvent::Kind::PeerSpawned);
		if (!expect(spawn != nullptr, "loadout burst trips the gate -> PeerSpawned")) return false;
		if (!expect(spawn->peer == peer, "PeerSpawned names the joiner peer")) return false;
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
		if (!expect(accept.peer_spawned(peer), "peer marked spawned after the gate")) return false;

		// The game-start bundle must be in the framed reply: 0x25 RESET_AND_START
		// then 0x0F WORLD-STATE-LOAD (the joiner's spawn pos/slot).
		if (!expect(!r.outbound.empty(), "spawn burst produces a framed 0x83 reply")) return false;
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> msgs;
		if (!expect(decode_s2c(r.outbound.back(), server_scrk, hdr, msgs),
		            "spawn reply decodes as a 0x83 SESSION packet")) return false;
		if (!expect(hdr.session_id == client_ck, "S2C session_id == joiner's ClientAuth.ck")) return false;
		if (!expect(reply_has_tag(msgs, 0x25), "game-start bundle carries 0x25 reset-and-start")) return false;
		if (!expect(reply_has_tag(msgs, 0x0F), "game-start bundle carries 0x0F world-state-load")) return false;
	}

	// --- Post-spawn: a C2S 0x0C now surfaces PeerC2SInMatch for NetSystem ---
	{
		auto body = make_uplink_0c(up_handle, up_type, 0xFFFF, up_x + 1, up_y, up_z, up_head, up_pitch);
		auto dg = craft_session(client_scrk, seq++, {make_protocol_message(0x0C, body)});
		auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 500);
		const HostAcceptEvent *c2s = find_event(r, HostAcceptEvent::Kind::PeerC2SInMatch);
		if (!expect(c2s != nullptr, "post-spawn 0x0C surfaces PeerC2SInMatch")) return false;
		if (!expect(c2s->in_match_c2s.size() == 1 && c2s->in_match_c2s[0].tag == 0x0C,
		            "PeerC2SInMatch carries the 0x0C message")) return false;
	}

	// --- In-match S2C wrap: NetSystem's 0x0A body -> a 0x83 SESSION datagram ---
	{
		std::vector<uint8_t> frame_body = {0x01, 0x02, 0x03, 0x04, 0x05};
		std::vector<uint8_t> dg;
		if (!expect(accept.frame_in_match_s2c(peer, 0x0A, frame_body, dg),
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
		if (!expect(!accept.frame_in_match_s2c(stranger, 0x0A, {0x00}, dg),
		            "frame_in_match_s2c rejects an unknown peer")) return false;
	}

	// --- Goodbye drops the peer ---
	{
		auto dg = craft(SESSION_OPCODE_CLIENT_GOODBYE, std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00});
		auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 600);
		if (!expect(has_event(r, HostAcceptEvent::Kind::PeerGoodbye), "0x46 emits PeerGoodbye")) return false;
		if (!expect(!accept.peer_spawned(peer), "peer cleared after goodbye")) return false;
		if (!expect(accept.peer_count() == 0, "peer table empty after goodbye")) return false;
	}

	return true;
}

bool run_non_jo_peer_is_ignored() {
	// A NOVAWORLDUDP (lobby) hello must not register a JO peer — the owner
	// routes lobby PNs elsewhere; the component drops it.
	HostSessionAccept accept;
	accept.start();
	const PeerAddr peer{0x0100007Fu, 31000};
	ClientHello hello;
	hello.pn = "NOVAWORLDUDP";
	hello.ci = 1;
	auto dg = craft(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(hello));
	auto r = accept.handle_datagram(peer, dg.data(), dg.size(), 1);
	if (!expect(r.outbound.empty(), "lobby PN produces no JO ServerHello")) return false;
	if (!expect(accept.peer_count() == 0, "lobby PN registers no JO peer")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = run() && ok;
	ok = run_non_jo_peer_is_ignored() && ok;
	return ok ? 0 : 1;
}
