// Phase 1 (ADR 0010) — drive the Godot-free ClientSession state machine
// through the full session handshake against apps/novaworld_server's own
// parsers/builders, in-process (no sockets — deterministic). Proves the two
// Phase 1 fixes and the D-NET-21 timing closure end to end:
//   1. ServerHello.hk is parsed and ECHOED in ClientAuth.hk (was hardcoded 0).
//   2. ClientConnected waits for the retail session-periodic boundary.
//   3. The 0x83 ServerProtocolMessage handler decodes the lobby stream and
//      runs the verify handshake to ServerVerifyResult -> Verified.
//
// The "server" here is a minimal responder using the real library functions
// (parse_client_hello/auth, build_server_hello/auth, LobbySession dispatch,
// the protocol_message + envelope + NWU codecs) — the same calls
// apps/novaworld_server/nw_udp_listener.cpp makes. If the client and server
// disagree on any layer (envelope, NWU, flat-TLV, scrk direction, the hk
// echo, the 0x43/0x83 framing), this test fails.

#include <net/novaworld/client_session.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/lobby_vars.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/npwire/protocol_message.h>
#include <net/novaworld/lobby_session.h>

#include <net/napi/envelope.h>
#include <net/napi/session.h>
#include <net/napi/tlv.h>
#include <net/novacrypto/nwu.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

bool expect(bool cond, const char *msg) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", msg);
		++g_failures;
	}
	return cond;
}

// ---- minimal server-side codec (mirrors nw_udp_listener.cpp) -------------

bool server_decode_inbound(const std::vector<uint8_t> &raw,
                           uint8_t &opcode_out, std::vector<uint8_t> &body_out) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out_size = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out_size) != 0) {
		return false;
	}
	stripped.resize(out_size);
	if (stripped.empty()) return false;
	opcode_out = stripped[0];
	body_out.assign(stripped.begin() + 1, stripped.end());
	if (!body_out.empty()) {
		nwu_encrypt(body_out.data(), body_out.size(), SESSION_NWU_KEY);
	}
	return true;
}

std::vector<uint8_t> server_encode_outbound(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) {
		nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
	}
	std::vector<uint8_t> with_opcode;
	with_opcode.push_back(opcode);
	with_opcode.insert(with_opcode.end(), body.begin(), body.end());
	std::vector<uint8_t> packet(with_opcode.size() + 4);
	size_t out_size = 0;
	if (napi_envelope_encode(with_opcode.data(), with_opcode.size(),
	                         packet.data(), packet.size(), &out_size) != 0) {
		return {};
	}
	packet.resize(out_size);
	return packet;
}

// Decode a client 0x43 datagram (envelope + outer NWU + inner SCRK) into its
// lobby containers — used to assert the verify request's structure (NW-S5).
bool decode_client_containers(const std::vector<uint8_t> &raw,
                              const std::string &client_scrk,
                              std::vector<NapiMessage> &out) {
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!server_decode_inbound(raw, opcode, body)) return false;
	if (opcode != SESSION_OPCODE_PROTOCOL_MESSAGE) return false;
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), client_scrk,
	                                      hdr, msgs)) {
		return false;
	}
	for (const auto &pm : msgs) {
		if (pm.flags.settings_update || pm.full_tag != 0) continue;
		size_t consumed = 0;
		std::vector<NapiMessage> cs;
		if (napi_stream_decode(pm.payload.data(), pm.payload.size(), cs,
		                       &consumed) != 0) {
			continue;
		}
		for (auto &c : cs) out.push_back(std::move(c));
	}
	return true;
}

// A tiny stateful server: enough of nw_udp_listener.cpp to answer one
// client's handshake. Returns the reply datagram (or empty for none).
struct MiniServer {
	uint32_t advertised_hk = 0x1234ABCDu;  // deliberately NON-default: a
	                                        // regression to hardcoded 0 (or the
	                                        // struct default) fails the echo check.
	uint32_t server_sk = 0xC0FFEE42u;
	std::string server_scrk = "ZZSERVERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABC"; // 61 chars
	std::string nwuid = "feedfacecafebeef0011223344556677889900aabbccddeeff0011223344"; // SessionInit NWUID the client must echo
	std::string client_scrk;               // learned from ClientAuth
	uint32_t client_ck = 0;
	uint32_t last_client_hk = 0;           // captured for the echo assertion
	// Retail protocol packets are 1-based in both directions; sequence zero is the
	// no-packet/force-send sentinel and never crosses the contiguous admission gate.
	uint32_t next_seq = 1;
	uint32_t last_client_seq = 0;          // the ack a server-initiated 0x83 carries
	LobbyState lobby;
	LobbySession session;

	MiniServer() {
		session.set_sess_id_generator([] { return std::string("deadbeefcafef00d1122334455667788"); });
	}

	// One lobby container as a layer-4 inner message (tag 0, LEN8/LEN16).
	static ProtocolMessage lobby_reply(std::vector<uint8_t> stream_bytes) {
		ProtocolMessage rpm;
		const size_t ss = stream_bytes.size();
		rpm.flags.raw = (ss > 0xFFu) ? 0x40u : 0x20u;
		rpm.flags.len16 = ss > 0xFFu;
		rpm.flags.len8 = ss <= 0xFFu;
		rpm.tag = 0;
		rpm.full_tag = 0;
		rpm.length = static_cast<uint32_t>(ss);
		rpm.payload = std::move(stream_bytes);
		return rpm;
	}

	static bool encode_container(const NapiMessage &container, std::vector<uint8_t> &out) {
		std::vector<NapiMessage> stream{container};
		out.assign(napi_stream_size(stream), 0);
		size_t ss = 0;
		if (napi_stream_encode(stream, out.data(), out.size(), &ss) != 0) return false;
		out.resize(ss);
		return true;
	}

	// Frame `messages` as the next S2C 0x83 (session_id = the client's CK, the
	// next contiguous seq, ack = the last client seq seen).
	std::vector<uint8_t> wrap(std::vector<ProtocolMessage> messages) {
		ProtocolPacketHeader rhdr;
		rhdr.session_id = client_ck;
		rhdr.seq_num = next_seq++;
		rhdr.ack_count = last_client_seq;
		rhdr.connection_flags = 0;
		std::vector<uint8_t> body_out;
		if (!encode_protocol_packet_plaintext(rhdr, messages, server_scrk, body_out)) return {};
		return server_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body_out));
	}

	// A server-initiated notification (ServerStopHosting, ServerCommand, ...): the
	// legs the service pushes without a client statement to answer.
	std::vector<uint8_t> push(const NapiMessage &container) {
		std::vector<uint8_t> sb;
		if (!encode_container(container, sb)) return {};
		std::vector<ProtocolMessage> replies;
		replies.push_back(lobby_reply(std::move(sb)));
		return wrap(std::move(replies));
	}

	std::vector<uint8_t> respond(const std::vector<uint8_t> &in) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		if (!server_decode_inbound(in, opcode, body)) return {};

		switch (opcode) {
		case SESSION_OPCODE_CLIENT_HELLO: {
			ClientHello hello;
			if (!expect(parse_client_hello(body.data(), body.size(), hello),
			            "server parses ClientHello")) return {};
			expect(hello.pn == "NOVAWORLDUDP", "ClientHello PN == NOVAWORLDUDP");
			// Real NovaWorld (HandleClientHello @ 0x6213B0) validates NVS + PG;
			// pin them so the retail-faithful identity can't regress.
			expect(hello.nvs == "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic",
			       "ClientHello NVS == Milota version string");
			expect(hello.pg_present, "ClientHello carries PG");
			expect((hello.pg == std::array<uint8_t, 16>{
			            0xF2, 0x0C, 0xEE, 0xD8, 0xCE, 0xE4, 0x8D, 0x44,
			            0x90, 0xB4, 0x1D, 0x42, 0xB3, 0x64, 0xAB, 0x71}),
			       "ClientHello PG == NOVAWORLDUDP protocol GUID");
			ServerHello reply = build_server_hello(hello, 0x7F000001u, 5000);
			reply.hk = advertised_hk;  // pin a known, non-default host key
			return server_encode_outbound(SESSION_OPCODE_SERVER_HELLO,
			                              server_hello_to_bytes(reply));
		}
		case SESSION_OPCODE_CLIENT_AUTH: {
			ClientAuth auth;
			if (!expect(parse_client_auth(body.data(), body.size(), auth),
			            "server parses ClientAuth")) return {};
			// Real NovaWorld re-runs the version gate on the 0x42 join
			// (HandleClientJoin @ 0x62B750) and silently drops it (no
			// ServerAuth -> session_join timeout) unless the identity block is
			// present: NVS/PN/PG/PV1 + PV2. Pin that the client re-sends it.
			expect(auth.nvs == "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic",
			       "ClientAuth NVS == Milota version string");
			expect(auth.pn == "NOVAWORLDUDP", "ClientAuth PN == NOVAWORLDUDP");
			expect(auth.pg_present, "ClientAuth carries PG");
			expect((auth.pg == std::array<uint8_t, 16>{
			            0xF2, 0x0C, 0xEE, 0xD8, 0xCE, 0xE4, 0x8D, 0x44,
			            0x90, 0xB4, 0x1D, 0x42, 0xB3, 0x64, 0xAB, 0x71}),
			       "ClientAuth PG == NOVAWORLDUDP protocol GUID");
			expect(auth.pv1 == "0.0.0 2/10/2004 EM", "ClientAuth PV1 == retail PV1");
			expect(auth.pv2 == "1", "ClientAuth PV2 == retail PV2 (is_server gate)");
			last_client_hk = auth.hk;
			client_ck = auth.ck;
			client_scrk = auth.scrk;
			ServerAuth reply = build_server_auth(auth, 0x7F000001u, 5000,
			                                     server_sk, server_scrk,
			                                     "NWServer", "http://127.0.0.1:8080",
			                                     nwuid);
			return server_encode_outbound(SESSION_OPCODE_SERVER_AUTH,
			                              server_auth_to_bytes(reply));
		}
		case SESSION_OPCODE_PROTOCOL_MESSAGE: {
			ProtocolPacketHeader hdr;
			std::vector<ProtocolMessage> messages;
			if (!expect(decode_protocol_packet_plaintext(body.data(), body.size(),
			                                             client_scrk, hdr, messages),
			            "server decodes 0x43 packet")) return {};

			last_client_seq = hdr.seq_num;
			std::vector<ProtocolMessage> replies;
			for (const auto &pm : messages) {
				if (pm.flags.settings_update || pm.full_tag != 0) continue;
				std::vector<NapiMessage> containers;
				size_t consumed = 0;
				if (napi_stream_decode(pm.payload.data(), pm.payload.size(),
				                       containers, &consumed) != 0) continue;
				for (const auto &outer : containers) {
					auto result = session.dispatch(outer, lobby, "127.0.0.1", 5000);
					for (auto &reply_container : result.reply_containers) {
						std::vector<uint8_t> sb;
						if (!encode_container(reply_container, sb)) continue;
						replies.push_back(lobby_reply(std::move(sb)));
					}
				}
			}
			if (replies.empty()) return {}; // ack-only (e.g. heartbeat)
			return wrap(std::move(replies));
		}
		default:
			return {};
		}
	}
};

NapiField str_field(const std::string &name, const std::string &value) {
	NapiField f;
	f.name = name;
	f.data.assign(value.begin(), value.end());
	return f;
}

// Run one fresh ClientSession through the whole lobby handshake against `server`
// (ClientHello .. ServerVerifyResult); true when it reached Verified.
bool bring_up(MiniServer &server, ClientSession &client) {
	std::vector<std::vector<uint8_t>> out;
	const auto s_hello = server.respond(client.start());
	if (!client.handle_datagram(s_hello.data(), s_hello.size(), out) || out.size() != 1) return false;
	const auto s_auth = server.respond(out[0]);
	out.clear();
	if (!client.handle_datagram(s_auth.data(), s_auth.size(), out) || !out.empty()) return false;
	client.process_periodic_update(out);
	if (out.size() != 1) return false;
	const auto s_start = server.respond(out[0]);
	out.clear();
	if (!client.handle_datagram(s_start.data(), s_start.size(), out) || out.size() != 1) return false;
	const auto s_result = server.respond(out[0]);
	out.clear();
	if (!client.handle_datagram(s_result.data(), s_result.size(), out)) return false;
	return client.is_verified();
}

// The peer-side session closes: S2C 0x86 ServerGoodBye keyed by the client's CK
// and the inner H:0x03 connection description. Both latch the peer's record
// (the tag becomes last_error), close the session, and the client's own 0x46
// then echoes the latched record.
// [orig: Nwu_HandleDisconnect @0x623ce0; CNapiNPConnection_HandleDescriptionPacket @0x621ae0]
void test_peer_disconnect_legs() {
	{
		MiniServer server;
		ClientSession::Config cfg;
		cfg.client_index = 0x00000031u;
		cfg.client_key = 0x31313131u;
		ClientSession client(cfg);
		if (!expect(bring_up(server, client), "0x86 leg: session reaches Verified")) return;

		const DisconnectEvent punt = make_disconnect_event(1, 2, 0, 0, "", 0, "NP.S:PT:STOP");
		// A goodbye keyed to another client's CK is not ours: ignored, still Verified.
		const auto foreign = server_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
				server_goodbye_to_bytes(cfg.client_key ^ 1u, punt));
		std::vector<std::vector<uint8_t>> out;
		expect(client.handle_datagram(foreign.data(), foreign.size(), out) &&
		               client.is_verified() && !client.disconnected_by_peer(),
		       "0x86 keyed to another CK is ignored");

		const auto goodbye = server_encode_outbound(SESSION_OPCODE_SERVER_GOODBYE,
				server_goodbye_to_bytes(cfg.client_key, punt));
		out.clear();
		expect(client.handle_datagram(goodbye.data(), goodbye.size(), out) && out.empty(),
		       "0x86 keyed to our CK is handled without a reply");
		expect(client.state() == ClientSession::State::Closed && client.disconnected_by_peer(),
		       "0x86 closes the session as a peer disconnect");
		expect(client.last_error() == "NP.S:PT:STOP", "0x86: the DDSTR tag is the error");
		expect(client.disconnect_event().dc == 2 && client.disconnect_event().ds == 1,
		       "0x86: the record's DC/DS are latched");
		// The client's own 0x46 echoes the latched record, keyed by the server's SK.
		const auto reply = client.build_goodbye();
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		expect(server_decode_inbound(reply, opcode, body) && opcode == SESSION_OPCODE_CLIENT_GOODBYE &&
		               body == client_goodbye_to_bytes(server.server_sk, punt),
		       "0x46 after a punt echoes the latched record");
	}
	{
		MiniServer server;
		ClientSession::Config cfg;
		cfg.client_index = 0x00000032u;
		cfg.client_key = 0x32323232u;
		ClientSession client(cfg);
		if (!expect(bring_up(server, client), "H:0x03 leg: session reaches Verified")) return;

		const DisconnectEvent description =
				make_disconnect_event(1, 2, 7, 0, "server shutting down", 4, "NP.S:PT:MSGCRE");
		std::vector<ProtocolMessage> messages;
		messages.push_back(make_protocol_message(
				hightag::DESCRIPTION_PACKET, connection_description_to_bytes(description),
				static_cast<uint8_t>(0x80u | PROTOCOL_MSG_FLAG_LEN8)));
		const auto datagram = server.wrap(std::move(messages));
		std::vector<std::vector<uint8_t>> out;
		expect(client.handle_datagram(datagram.data(), datagram.size(), out) && out.empty(),
		       "H:0x03 is terminal: no ack follows it");
		expect(client.state() == ClientSession::State::Closed && client.disconnected_by_peer(),
		       "H:0x03 closes the session as a peer disconnect");
		expect(client.last_error() == "NP.S:PT:MSGCRE" && client.disconnect_event().dpc == 4 &&
		               client.disconnect_event().dstr == "server shutting down",
		       "H:0x03: the description record is latched");
		// Once closed, further 0x83s are dropped.
		NapiMessage stop;
		stop.name = "ServerStopHosting";
		stop.fields.push_back(str_field("Success", "1"));
		const auto late = server.push(stop);
		out.clear();
		expect(client.handle_datagram(late.data(), late.size(), out) && out.empty() &&
		               client.take_notices().empty(),
		       "a closed session ignores later 0x83 traffic");
	}
}

// The connection's send-interval pump over the negotiated CS: the idle keepalive
// (a header-only 0x43 once nothing was sent for idle_send_interval_ms) and the
// CLNTTMOUT reap once receive silence exceeds timeout_ms.
// [orig: CNapiNPConnection_PumpStateMachine @0x6292e0 case 5; PumpSendIntervals @0x628fd0]
void test_send_interval_pump() {
	MiniServer server;
	ClientSession::Config cfg;
	cfg.client_index = 0x00000033u;
	cfg.client_key = 0x33333333u;
	ClientSession client(cfg);
	if (!expect(bring_up(server, client), "pump leg: session reaches Verified")) return;

	// The 0x82 CS overlay: the service template (240 s reap, 60 s idle, 1 s active, no queue leg).
	const ClientSession::ConnectionSettings &cs = client.connection_settings();
	expect(cs.timeout_ms == 240000 && cs.idle_send_interval_ms == 60000 &&
	               cs.active_send_interval_ms == 1000 && cs.packet_queue_interval_ms == -1,
	       "ServerAuth CS block overlays the client-direction template");

	std::vector<std::vector<uint8_t>> pumped;
	client.pump_send_intervals(pumped);
	expect(pumped.empty(), "nothing to send right after the handshake");
	client.set_clock_ms(static_cast<uint32_t>(cs.idle_send_interval_ms));
	client.pump_send_intervals(pumped);
	expect(pumped.empty(), "the keepalive waits until elapsed > idle_send_interval_ms");
	client.set_clock_ms(static_cast<uint32_t>(cs.idle_send_interval_ms) + 1);
	client.pump_send_intervals(pumped);
	if (expect(pumped.size() == 1, "one keepalive once the idle interval passed")) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader hdr;
		std::vector<ProtocolMessage> messages;
		expect(server_decode_inbound(pumped[0], opcode, body) &&
		               opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
		               decode_protocol_packet_plaintext(body.data(), body.size(), server.client_scrk,
		                                                hdr, messages) &&
		               messages.empty(),
		       "the keepalive is a header-only 0x43");
		expect(server.respond(pumped[0]).empty(), "the keepalive draws no reply");
	}
	pumped.clear();
	client.pump_send_intervals(pumped);
	expect(pumped.empty(), "the keepalive refreshed the send clock: no second one at the same tick");
	expect(client.is_verified(), "keepalives leave the session Verified");

	// Receive silence past timeout_ms: the reap latches CLNTTMOUT and closes locally.
	client.set_clock_ms(static_cast<uint32_t>(cs.timeout_ms) + 1);
	pumped.clear();
	client.pump_send_intervals(pumped);
	expect(pumped.empty(), "the reap sends nothing");
	expect(client.state() == ClientSession::State::Closed && !client.disconnected_by_peer(),
	       "the reap closes the session as a LOCAL disconnect");
	expect(client.last_error() == "NP.C:PT:CLNTTMOUT", "the reap tag is NP.C:PT:CLNTTMOUT");
	const DisconnectEvent &reap = client.disconnect_event();
	expect(reap.ds == 2 && reap.dc == 3 && reap.dp1 == static_cast<uint32_t>(cs.timeout_ms) + 1 &&
	               reap.dp2 == static_cast<uint32_t>(cs.timeout_ms),
	       "the reap record carries {2, 3, elapsed, timeout}");
	const auto goodbye = client.build_goodbye();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	expect(server_decode_inbound(goodbye, opcode, body) && opcode == SESSION_OPCODE_CLIENT_GOODBYE &&
	               body == client_goodbye_to_bytes(server.server_sk, reap),
	       "0x46 after the reap carries the CLNTTMOUT record");
}

// ServerLeaveNovaWorld: the punt. State 0, the session closes as a peer
// disconnect, the mission exit reason is 12.
// [orig: CNapiGameSession_HandlePuntNotification @0x4d20b0]
void test_leave_novaworld() {
	MiniServer server;
	ClientSession::Config cfg;
	cfg.client_index = 0x00000034u;
	cfg.client_key = 0x34343434u;
	ClientSession client(cfg);
	if (!expect(bring_up(server, client), "punt leg: session reaches Verified")) return;
	expect(client.mission_exit_reason() == 0, "no exit reason before the punt");

	NapiMessage leave;
	leave.name = "ServerLeaveNovaWorld";
	leave.fields.push_back(str_field("Success", "1"));
	leave.fields.push_back(str_field("MsgCode", "1005"));
	leave.fields.push_back(str_field("MsgParam1", "7"));
	leave.fields.push_back(str_field("MsgParam2", "0"));
	const auto datagram = server.push(leave);
	std::vector<std::vector<uint8_t>> out;
	expect(client.handle_datagram(datagram.data(), datagram.size(), out) && out.empty(),
	       "the punt is terminal: no ack follows it");
	expect(client.state() == ClientSession::State::Closed && client.disconnected_by_peer(),
	       "the punt closes the session as a peer disconnect");
	expect(client.mission_exit_reason() == 12, "the punt sets exit reason 12");
	const auto notices = client.take_notices();
	if (expect(notices.size() == 1 && notices[0].kind == ClientSession::Notice::Kind::LeaveNovaWorld,
	           "one LeaveNovaWorld notice")) {
		expect(notices[0].fields.success == 1 && notices[0].fields.msg_code == 1005 &&
		               notices[0].fields.msg_param1 == 7,
		       "the punt notice carries the Success/MsgCode/MsgParam triple");
	}
}

// Parse-side roundtrip for the new client-direction parsers, independent of
// the live handshake (locks parse_server_hello / parse_server_auth as exact
// inverses of the serializers).
void test_parser_roundtrip() {
	ClientHello ch;
	ch.pn = "NOVAWORLDUDP";
	ch.ci = 0x11223344u;
	ServerHello sh = build_server_hello(ch, 0x7F000001u, 5000);
	sh.hk = 0xABAD1DEAu;
	auto sh_bytes = server_hello_to_bytes(sh);
	ServerHello sh_parsed;
	expect(parse_server_hello(sh_bytes.data(), sh_bytes.size(), sh_parsed),
	       "parse_server_hello succeeds");
	expect(sh_parsed.hk == 0xABAD1DEAu, "parse_server_hello recovers hk");
	expect(sh_parsed.ci == 0x11223344u, "parse_server_hello recovers ci");
	expect(sh_parsed.pl.empty(), "parse_server_hello leaves absent PL empty");

	ClientAuth ca;
	ca.ci = 0x11223344u;
	ca.ck = 0x55667788u;
	ca.na = "jop:cus2";

	// client_auth_to_bytes <-> parse_client_auth round-trip, including the
	// identity block the real server validates (NW-S2). Locks the serializer
	// and parser as exact inverses for the 0x42 join.
	ClientAuth ca_id = ca;
	ca_id.nvs = "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";
	ca_id.co = "OpenNova";
	ca_id.ap = "OpennovaGodotClient.exe";
	ca_id.bdat = "Jul 21 2009 18:54:41";
	ca_id.pn = "NOVAWORLDUDP";
	ca_id.pg = std::array<uint8_t, 16>{0xF2, 0x0C, 0xEE, 0xD8, 0xCE, 0xE4, 0x8D, 0x44,
	                                   0x90, 0xB4, 0x1D, 0x42, 0xB3, 0x64, 0xAB, 0x71};
	ca_id.pg_present = true;
	ca_id.pv1 = "0.0.0 2/10/2004 EM";
	ca_id.pv2 = "1";
	ca_id.hk = 0x0FE0E112u;
	ca_id.scrk = "CLIENTSCRK0123456789";
	// NW-S3 CU chunks: the gate-issued session-auth codes + a binary one.
	ca_id.cu.push_back(make_client_cu_chunk(1, "UdpCode1", "1234567890"));
	ca_id.cu.push_back(make_client_cu_chunk(2, "UdpCode2", "0987654321"));
	auto ca_bytes = client_auth_to_bytes(ca_id);
	ClientAuth ca_parsed;
	expect(parse_client_auth(ca_bytes.data(), ca_bytes.size(), ca_parsed),
	       "parse_client_auth succeeds on round-trip");
	// The CU blobs survive the flat-TLV round-trip and decode to name/value.
	if (expect(ca_parsed.cu.size() == 2, "parse_client_auth recovers 2 CU chunks")) {
		uint8_t cu_type = 0; std::string cu_name, cu_value;
		expect(parse_client_cu_chunk(ca_parsed.cu[0].data(), ca_parsed.cu[0].size(),
		                             cu_type, cu_name, cu_value),
		       "CU[0] decodes");
		expect(cu_type == 1 && cu_name == "UdpCode1" && cu_value == "1234567890",
		       "CU[0] = type1 UdpCode1=1234567890");
		expect(parse_client_cu_chunk(ca_parsed.cu[1].data(), ca_parsed.cu[1].size(),
		                             cu_type, cu_name, cu_value),
		       "CU[1] decodes");
		expect(cu_type == 2 && cu_name == "UdpCode2" && cu_value == "0987654321",
		       "CU[1] = type2 UdpCode2=0987654321");
	}
	expect(ca_parsed.nvs == ca_id.nvs, "parse_client_auth recovers NVS");
	expect(ca_parsed.pn == "NOVAWORLDUDP", "parse_client_auth recovers PN");
	expect(ca_parsed.pg_present && ca_parsed.pg == ca_id.pg, "parse_client_auth recovers PG");
	expect(ca_parsed.pv1 == ca_id.pv1, "parse_client_auth recovers PV1");
	expect(ca_parsed.pv2 == "1", "parse_client_auth recovers PV2");
	expect(ca_parsed.ci == ca_id.ci, "parse_client_auth recovers CI");
	expect(ca_parsed.hk == ca_id.hk, "parse_client_auth recovers HK");
	expect(ca_parsed.ck == ca_id.ck, "parse_client_auth recovers CK");
	expect(ca_parsed.na == "jop:cus2", "parse_client_auth recovers NA");
	expect(ca_parsed.scrk == ca_id.scrk, "parse_client_auth recovers SCRK");

	ServerAuth sa = build_server_auth(ca, 0x7F000001u, 5000, 0xDEADBEEFu, "MYSCRK61");
	auto sa_bytes = server_auth_to_bytes(sa);
	ServerAuth sa_parsed;
	expect(parse_server_auth(sa_bytes.data(), sa_bytes.size(), sa_parsed),
	       "parse_server_auth succeeds");
	expect(sa_parsed.sk == 0xDEADBEEFu, "parse_server_auth recovers sk");
	expect(sa_parsed.cr == 1u, "parse_server_auth recovers cr");
	expect(sa_parsed.scrk == "MYSCRK61", "parse_server_auth recovers scrk");
	expect(sa_parsed.ci == 0x11223344u, "parse_server_auth recovers ci");
	expect(sa_parsed.client_cs.size() == sa.client_cs.size(), "parse_server_auth recovers client CS count");
	expect(sa_parsed.server_cs.size() == sa.server_cs.size(), "parse_server_auth recovers server CS count");
	expect(sa_parsed.cu.size() == sa.cu.size(), "parse_server_auth recovers CU count");
	if (sa_parsed.cu.size() == sa.cu.size() && !sa.cu.empty()) {
		expect(sa_parsed.cu[0].first == sa.cu[0].first, "parse_server_auth recovers first CU name");
		expect(sa_parsed.cu[0].second == sa.cu[0].second, "parse_server_auth recovers first CU value");
	}
}

void test_client_correlates_handshake_echoes() {
	ClientSession::Config cfg;
	cfg.client_index = 0x11223344u;
	cfg.client_key = 0x55667788u;
	ClientSession client(cfg);
	const std::vector<uint8_t> hello_datagram = client.start();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	expect(server_decode_inbound(hello_datagram, opcode, body),
	       "echo-guard decodes lobby ClientHello");
	ClientHello hello;
	expect(parse_client_hello(body.data(), body.size(), hello),
	       "echo-guard parses lobby ClientHello");

	ServerHello wrong_hello = build_server_hello(hello, 0x7F000001u, 5000);
	wrong_hello.ci ^= 1u;
	const std::vector<uint8_t> wrong_hello_datagram = server_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(wrong_hello));
	std::vector<std::vector<uint8_t>> out;
	expect(client.handle_datagram(
			       wrong_hello_datagram.data(), wrong_hello_datagram.size(), out) &&
	               out.empty() && client.state() == ClientSession::State::Hello,
	       "lobby ServerHello for another CI is ignored");

	ServerHello valid_hello = build_server_hello(hello, 0x7F000001u, 5000);
	const std::vector<uint8_t> valid_hello_datagram = server_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(valid_hello));
	out.clear();
	expect(client.handle_datagram(
			       valid_hello_datagram.data(), valid_hello_datagram.size(), out) &&
	               out.size() == 1 && client.state() == ClientSession::State::Auth,
	       "matching lobby ServerHello advances to Auth");
	ClientAuth auth;
	expect(server_decode_inbound(out[0], opcode, body) &&
	               parse_client_auth(body.data(), body.size(), auth),
	       "echo-guard parses lobby ClientAuth");
	ServerAuth wrong_auth = build_server_auth(
			auth, 0x7F000001u, 5000, 0xAABBCCDDu, "SERVER-ECHO-GUARD-SCRK");
	wrong_auth.ci ^= 2u;
	const std::vector<uint8_t> wrong_auth_datagram = server_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(wrong_auth));
	out.clear();
	expect(client.handle_datagram(
			       wrong_auth_datagram.data(), wrong_auth_datagram.size(), out) &&
	               client.state() == ClientSession::State::Auth &&
	               client.server_key() == 0,
	       "lobby ServerAuth with mismatched CI cannot install keys");

	ServerAuth wrong_key_auth = build_server_auth(
			auth, 0x7F000001u, 5000, 0xAABBCCDDu, "SERVER-ECHO-GUARD-SCRK");
	wrong_key_auth.ck ^= 4u;
	const std::vector<uint8_t> wrong_key_auth_datagram = server_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(wrong_key_auth));
	out.clear();
	expect(client.handle_datagram(
			       wrong_key_auth_datagram.data(), wrong_key_auth_datagram.size(), out) &&
	               client.state() == ClientSession::State::Auth &&
	               client.server_key() == 0,
	       "lobby ServerAuth with mismatched CK cannot install keys");
}

// The browser completes NWJoin after the UDP session has already verified.
// Its newly issued cookies must survive the next encoded ClientPlayRequest.
void test_play_uses_current_http_cookies() {
	LobbyHttpFlow flow;
	LobbyHttpContext context;
	context.startup_url = "http://gs.opennova.test/NWStart.dll?prepare=1";
	context.identity_vars = make_lobby_identity_vars(LobbyIdentityParams{});
	flow.set_context(context);
	ClientSession::Config cfg;
	cfg.cookie_vars = [&flow]() { return flow.session_cookie_vars(); };
	MiniServer server;
	ClientSession client(cfg);
	if (!expect(bring_up(server, client), "cookie refresh: session reaches Verified")) return;
	context.server_nwuid = client.server_nwuid();
	flow.set_context(context);
	flow.login("synthetic", "synthetic");
	flow.on_login_response(true, 200, {"Set-Cookie: EPASK=3:65537:fixture"}, {});
	flow.on_login_response(true, 200, {"Set-Cookie: LOGINSESSIONTAG=login"}, {});
	const auto login = flow.on_login_response(true, 200,
			{"Set-Cookie: NWHANDLE=synthetic", "Set-Cookie: PCID=account"}, {});
	if (!expect(login.kind == LoginResult::Kind::Succeeded, "cookie refresh: HTTP login succeeds")) return;
	flow.join(777);
	flow.on_join_response(true, 200, {"Set-Cookie: NWJOINSESSIONTAG=join"}, {});
	const std::string joi = "<TITLE>[NI=192.0.2.1&NP=32768&GS=x]</TITLE>";
	const auto joined = flow.on_join_response(true, 200,
			{"Set-Cookie: NWPF=28", "Set-Cookie: NWPF2=0", "Set-Cookie: PUBJOINTICKET=ticket-1"},
			std::vector<uint8_t>(joi.begin(), joi.end()));
	if (!expect(joined.kind == JoinResult::Kind::Resolved, "cookie refresh: NWJoin resolves")) return;
	const auto setup = make_play_setup_vars("Host", joined.host_ip,
			std::to_string(joined.host_port), joined.app_id, joined.ln);
	const auto request = client.build_play_request(setup);
	expect(!server.respond(request).empty(), "cookie refresh: service decodes the play request");
	const auto &cookies = server.lobby.play_state["Cookie"];
	expect(var_value(cookies, "NWPF") == "28", "play carries NWJoin NWPF instead of provoking NWEC09");
	expect(var_value(cookies, "NWPF2") == "0", "play carries NWJoin NWPF2");
	expect(var_value(cookies, "NWHANDLE") == "synthetic", "play carries the signed-in handle");
	expect(var_value(cookies, "PCID") == "account", "play carries the signed-in account");
	expect(var_value(cookies, "PUBJOINTICKET") == "ticket-1", "play carries the current join ticket");
	expect(var_value(cookies, "NWUID") == server.nwuid, "play retains the session NWUID");

	// A retry can replace existing cookies; neither the initial snapshot nor
	// the first successful NWJoin is a valid source for this request.
	server.respond(client.build_stop_playing());
	flow.join(778);
	flow.on_join_response(true, 200, {"Set-Cookie: NWJOINSESSIONTAG=retry"}, {});
	flow.on_join_response(true, 200,
			{"Set-Cookie: NWPF=38", "Set-Cookie: NWPF2=1", "Set-Cookie: PUBJOINTICKET=ticket-2",
			 "Set-Cookie: NWCDKIID=issued", "Set-Cookie: CountryName=remote"},
			std::vector<uint8_t>(joi.begin(), joi.end()));
	server.respond(client.build_play_request(setup));
	const auto &retry_cookies = server.lobby.play_state["Cookie"];
	expect(var_value(retry_cookies, "NWPF") == "38" && var_value(retry_cookies, "NWPF2") == "1",
	       "retry uses updated product cookies without hardcoding a product id");
	expect(var_value(retry_cookies, "PUBJOINTICKET") == "ticket-2", "retry replaces the join ticket");
	expect(var_value(retry_cookies, "NWCDKIID") == "issued", "initial empty identity cannot erase an issued cookie");
	for (const auto &kv : context.identity_vars) {
		if (kv.first == "CountryName")
			expect(var_value(retry_cookies, kv.first) == kv.second, "local country overlays browser cookies");
	}
	int cdkiid_count = 0;
	for (const auto &entry : retry_cookies) if (entry.name == "NWCDKIID") ++cdkiid_count;
	expect(cdkiid_count == 1, "the refreshed Cookie list contains no duplicate identity entry");

	server.respond(client.build_stop_playing());
	flow.reset();
	server.respond(client.build_play_request(setup));
	const auto &reset_cookies = server.lobby.play_state["Cookie"];
	expect(!var_has(reset_cookies, "NWPF") && !var_has(reset_cookies, "PUBJOINTICKET") &&
	               !var_has(reset_cookies, "PCID"), "cleared HTTP cookies cannot leak into another play request");
}

} // namespace

int main() {
	test_play_uses_current_http_cookies();
	test_parser_roundtrip();
	test_client_correlates_handshake_echoes();
	test_peer_disconnect_legs();
	test_send_interval_pump();
	test_leave_novaworld();

	MiniServer server;
	ClientSession::Config cfg;
	cfg.client_index = 0x00000001u;
	cfg.client_key   = 0x0BADF00Du;
	// NW-S5: configure the verify "Cookie" var-list so the verify request carries
	// the witnessed structure. NWUID is left empty -> the client must echo it from
	// the ServerSessionInit; NWCDKIID empty mirrors retail (verify is not
	// CD-key-gated). Asserted on d_verify_req below.
	cfg.cookie_vars = []() {
		return std::vector<std::pair<std::string, std::string>>{
			{"CountryName", "United States"},
			{"NWUID", ""},
			{"NWCDKIID", ""},
			{"NWPSSK", "ABCDEFGHIJKLMNOPQRSTUVW"},
		};
	};
	ClientSession client(cfg);

	// 1) ClientHello.
	auto d_hello = client.start();
	expect(client.state() == ClientSession::State::Hello, "client in Hello after start()");
	expect(!d_hello.empty(), "ClientHello datagram non-empty");

	// 2) ServerHello -> client emits ClientAuth, having learned hk.
	auto s_hello = server.respond(d_hello);
	expect(!s_hello.empty(), "ServerHello datagram non-empty");
	std::vector<std::vector<uint8_t>> out;
	expect(client.handle_datagram(s_hello.data(), s_hello.size(), out),
	       "client handles ServerHello");
	expect(client.state() == ClientSession::State::Auth, "client in Auth after ServerHello");
	expect(client.server_host_key() == server.advertised_hk,
	       "client learned the advertised host key");
	if (!expect(out.size() == 1, "ServerHello yields exactly one reply (ClientAuth)")) return 1;
	auto d_auth = out[0];

	// 3) Server parses ClientAuth — THE Phase 1 echo assertion.
	auto s_auth = server.respond(d_auth);
	expect(server.last_client_hk == server.advertised_hk,
	       "ClientAuth.hk ECHOES ServerHello.hk (Phase 1 fix; was hardcoded 0)");
	expect(!server.client_scrk.empty(), "server captured the client SCRK");
	expect(!s_auth.empty(), "ServerAuth datagram non-empty");

	// 4) ServerAuth -> client transitions to Verifying but emits nothing yet.
	// Retail sends ClientConnected only from the next periodic update after the
	// SessionInit handler established conn_state==5 && session_state==2.
	// [orig: CNapiGameSession_ProcessPeriodicUpdate @ 0x4d4400]
	out.clear();
	expect(client.handle_datagram(s_auth.data(), s_auth.size(), out),
	       "client handles ServerAuth");
	expect(client.state() == ClientSession::State::Verifying, "client in Verifying after ServerAuth");
	expect(client.server_key() == server.server_sk, "client learned server SK");
	expect(client.server_scrk() == server.server_scrk, "client learned server SCRK");
	if (!expect(out.empty(), "ServerAuth has no synchronous ClientConnected reply")) return 1;
	client.process_periodic_update(out);
	if (!expect(out.size() == 1, "next periodic update emits ClientConnected")) return 1;
	auto d_connected = out[0];
	std::vector<std::vector<uint8_t>> duplicate_periodic;
	client.process_periodic_update(duplicate_periodic);
	expect(duplicate_periodic.empty(), "later periodic updates do not repeat ClientConnected");

	// 5) ServerStartVerify -> client emits ClientRequestVerifyResult.
	auto s_start_verify = server.respond(d_connected);
	expect(!s_start_verify.empty(), "ServerStartVerify datagram non-empty");
	// A validly-encrypted packet addressed to another local session key is
	// quietly dropped before either sequence state or lobby semantics advance.
	// Feeding the original packet with the same sequence immediately afterward
	// must still admit it.
	{
		uint8_t wrong_opcode = 0;
		std::vector<uint8_t> wrong_body;
		ProtocolPacketHeader wrong_header;
		std::vector<ProtocolMessage> wrong_messages;
		expect(server_decode_inbound(s_start_verify, wrong_opcode, wrong_body) &&
		               wrong_opcode == SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
		               decode_protocol_packet_plaintext(
				               wrong_body.data(), wrong_body.size(),
				               server.server_scrk, wrong_header, wrong_messages),
		       "wrong-session regression decodes ServerStartVerify fixture");
		wrong_header.session_id ^= 0x01010101u;
		std::vector<uint8_t> rewritten_body;
		expect(encode_protocol_packet_plaintext(
			               wrong_header, wrong_messages, server.server_scrk,
			               rewritten_body),
		       "wrong-session regression rewrites only session_id");
		const auto wrong_session = server_encode_outbound(
				SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(rewritten_body));
		std::vector<std::vector<uint8_t>> rejected_out;
		expect(client.handle_datagram(
			               wrong_session.data(), wrong_session.size(), rejected_out) &&
		               rejected_out.empty() &&
		               client.state() == ClientSession::State::Verifying,
		       "wrong inbound lobby session_id is rejected without dispatch");

		const auto ack_probe = client.build_heartbeat();
		uint8_t probe_opcode = 0;
		std::vector<uint8_t> probe_body;
		ProtocolPacketHeader probe_header;
		std::vector<ProtocolMessage> probe_messages;
		expect(server_decode_inbound(ack_probe, probe_opcode, probe_body) &&
		               probe_opcode == SESSION_OPCODE_PROTOCOL_MESSAGE &&
		               decode_protocol_packet_plaintext(
				               probe_body.data(), probe_body.size(),
				               server.client_scrk, probe_header, probe_messages) &&
		               probe_header.ack_count == 0,
		       "wrong inbound lobby session_id cannot advance receive sequence");
	}
	out.clear();
	expect(client.handle_datagram(s_start_verify.data(), s_start_verify.size(), out),
	       "client handles ServerStartVerify");
	expect(client.state() == ClientSession::State::Verifying, "client still Verifying after ServerStartVerify");
	if (!expect(out.size() == 1, "ServerStartVerify yields one reply (ClientRequestVerifyResult)")) return 1;
	auto d_verify_req = out[0];

	// NW-S5 parity — the verify request carries SessIdString + a "Cookie"
	// var-list of ClientVar{VarFNum,VarName,VarValue}, with NWUID echoed from the
	// ServerSessionInit (matches the genuine .204 capture, frame 10166).
	{
		std::vector<NapiMessage> vcs;
		expect(decode_client_containers(d_verify_req, server.client_scrk, vcs),
		       "verify request decodes");
		if (expect(vcs.size() == 1 && vcs[0].name == "ClientRequestVerifyResult",
		           "verify container is ClientRequestVerifyResult")) {
			const auto &req = vcs[0];
			bool has_sid = false;
			for (const auto &f : req.fields)
				if (f.name == "SessIdString") has_sid = true;
			expect(has_sid, "verify carries SessIdString field");
			if (expect(req.children.size() == 1 &&
			               req.children[0].name == "ClientVarList",
			           "verify carries ClientVarList child")) {
				const auto &vl = req.children[0];
				bool cookie = false;
				for (const auto &f : vl.fields)
					if (f.name == "VarList" &&
					    std::string(f.data.begin(), f.data.end()) == "Cookie")
						cookie = true;
				expect(cookie, "var-list name is Cookie");
				expect(vl.children.size() == 4, "var-list has 4 ClientVar entries");
				std::string nwuid_value;
				for (const auto &cv : vl.children) {
					std::string vn, vv;
					for (const auto &f : cv.fields) {
						if (f.name == "VarName") vn.assign(f.data.begin(), f.data.end());
						if (f.name == "VarValue") vv.assign(f.data.begin(), f.data.end());
					}
					if (vn == "NWUID") nwuid_value = vv;
				}
				expect(nwuid_value == server.nwuid,
				       "NWUID echoes the SessionInit NWUID");
			}
		}
	}

	// 6) ServerVerifyResult -> client reaches Verified with the session token.
	auto s_verify_result = server.respond(d_verify_req);
	expect(!s_verify_result.empty(), "ServerVerifyResult datagram non-empty");
	out.clear();
	expect(client.handle_datagram(s_verify_result.data(), s_verify_result.size(), out),
	       "client handles ServerVerifyResult");
	expect(client.state() == ClientSession::State::Verified, "client VERIFIED after ServerVerifyResult");
	expect(client.is_verified(), "is_verified() true");
	expect(client.sess_id_string() == "deadbeefcafef00d1122334455667788",
	       "client captured SessIdString from ServerVerifyResult");

	// 7) Heartbeat is a well-formed (header-only) 0x43 the server accepts.
	auto hb = client.build_heartbeat();
	expect(!hb.empty(), "heartbeat datagram non-empty");
	auto hb_reply = server.respond(hb);
	expect(hb_reply.empty(), "heartbeat draws no reply (ack-only) and doesn't error");

	// 8) Host registration (F1) — a Verified session sends ClientHostRequest; the
	// gate registers the host (ServerHostResult) and its lobby state reflects the
	// advertised endpoint/name so /api/hosts + the GSB browser list it. This is
	// the host-side use of make_client_host_request the LAN-direct path lacked.
	// [orig: CNapiGameSession_SendHostRequest @ 0x4d3700]
	{
		auto host_req = make_client_host_request(
		    /*CurrentlyHosting*/ 1,
		    /*Cookie*/    {{0, "NWUID", server.nwuid}},
		    /*HostSetup*/ {{0, "AppId", "28"}, {0, "LobbyName", "jop_2_consumer"},
		                   {0, "MaxPlayers", "32"}},
		    /*Host*/      {{0, "ServerName", "OpenNova Host"}, {0, "Port", "32768"},
		                   {0, "Players", "1"}, {0, "Region", "us"}},
		    /*PlayerList*/{{0, "PlayerName", "Host"}});
		auto d_host = client.build_lobby_message(host_req);
		expect(!d_host.empty(), "host-request datagram non-empty (session Verified)");
		auto s_host = server.respond(d_host);
		expect(!s_host.empty(), "ServerHostResult datagram non-empty");
		expect(server.lobby.hosting, "gate marks lobby hosting");
		expect(server.lobby.server_name == "OpenNova Host", "gate stored ServerName");
		// The joinable endpoint is the observed UDP source; a positive Host.Port
		// overrides the observed port (retail sends the literal "-1").
		expect(server.lobby.host_ip == "127.0.0.1", "gate stored the observed source IP");
		expect(server.lobby.host_port == 32768, "gate stored the positive Host.Port override");
		expect(server.lobby.max_players == 32, "gate stored MaxPlayers");
		expect(server.lobby.player_count == 1, "gate stored Players");
		expect(server.lobby.game == "jop_2_consumer", "gate stored LobbyName");
		expect(server.lobby.rid != 0, "gate assigned a RID");
		// The client decodes ServerHostResult without protocol error (it stays Verified).
		out.clear();
		expect(client.handle_datagram(s_host.data(), s_host.size(), out),
		       "client handles ServerHostResult without error");
		expect(client.is_verified(), "client still Verified after ServerHostResult");
	}

	// 9) Host heartbeat (F1) — ClientHostUpdate refreshes player count / name and
	// is ack-only (no ServerHostResult). [orig: CNapiGameSession_SendHostUpdate
	// @ 0x4d3860]
	{
		auto host_upd = make_client_host_update(
		    /*Host*/      {{0, "Players", "2"}, {0, "ServerName", "OpenNova Host 2"}},
		    /*PlayerList*/{{0, "PlayerName", "Host"}, {1, "PlayerName", "Joiner"}});
		auto d_upd = client.build_lobby_message(host_upd);
		expect(!d_upd.empty(), "host-update datagram non-empty");
		auto s_upd = server.respond(d_upd);
		expect(s_upd.empty(), "ClientHostUpdate draws no reply (ack-only)");
		expect(server.lobby.player_count == 2, "gate update refreshed player count");
		expect(server.lobby.server_name == "OpenNova Host 2", "gate update refreshed ServerName");
	}

	// 10) The ServerHostResult of step 8 moved the host leg to Established (state 6)
	// and stored the HostCommands the service returned: the GSID (the in-match
	// host's 0x81 SUS1) and HostRequiresJoinTicket. The notice queue carries it.
	// [orig: HandleHostVerifyResponse @0x4d59d0 -> HandleHostCommandVarList @0x4d3440]
	{
		expect(client.host_state() == ClientSession::HostState::Established,
		       "ServerHostResult Success -> host leg Established");
		expect(client.host_result().success == 1 && client.host_result().msg_param2 == 17,
		       "host_result() carries the Success/MsgParam quartet");
		expect(!server.lobby.gsid.empty() && client.host_gsid() == server.lobby.gsid,
		       "host_gsid() is the service's HostCommands GSID");
		expect(client.host_requires_join_ticket() == 0, "HostRequiresJoinTicket parsed");
		const auto notices = client.take_notices();
		expect(notices.size() == 1 && notices[0].kind == ClientSession::Notice::Kind::HostResult,
		       "one HostResult notice queued");
		expect(client.take_notices().empty(), "take_notices() drains the queue");
	}

	// 11) The roster notifications only fire while Established, and the service's
	// occupancy follows its roster size on each (slot 1 already rode the update
	// above, so the delta is a new slot). [orig: SendPlayerAdded @0x4cfec0 /
	// SendPlayerRemoved @0x4d01a0]
	{
		HostPlayerSlot joiner;
		joiner.slot = 2;
		joiner.player_name = "Joiner";
		joiner.ip_and_port = "192.168.1.5:32768";
		joiner.team = "1";
		joiner.type = "0";
		const int before = server.lobby.player_count;
		const auto d_added = client.build_host_player_added(joiner);
		expect(!d_added.empty(), "ClientHostPlayerAdded builds while Established");
		expect(server.respond(d_added).empty(), "ClientHostPlayerAdded draws no reply");
		expect(server.lobby.player_count == before + 1, "the service counted the added player");
		const auto d_removed = client.build_host_player_removed(2);
		expect(!d_removed.empty(), "ClientHostPlayerRemoved builds while Established");
		expect(server.respond(d_removed).empty(), "ClientHostPlayerRemoved draws no reply");
		expect(server.lobby.player_count == before, "the service counted the removed player");
	}

	// 12) A datagram the envelope rejects is tossed (counted, logged) and the
	// session is untouched. [orig: NapiNPManager_PumpReceive @0x623010 @0x623206]
	{
		const std::vector<uint8_t> garbage{0x00, 0x01};
		std::vector<std::vector<uint8_t>> tossed_out;
		expect(client.handle_datagram(garbage.data(), garbage.size(), tossed_out) &&
		               tossed_out.empty(),
		       "a tossed datagram is not an error");
		expect(client.tossed_datagrams() == 1 && client.is_verified(),
		       "the toss is counted and the session stays Verified");
	}

	// 13) Every ServerStartVerify is answered, not only the first: the handler is
	// the bare locale read + SendVerifyRequest. [orig: SendLocaleAndVerify @0x4d57e0]
	{
		NapiMessage again;
		again.name = "ServerStartVerify";
		const auto d_again = server.push(again);
		std::vector<std::vector<uint8_t>> verify_out;
		expect(client.handle_datagram(d_again.data(), d_again.size(), verify_out) &&
		               verify_out.size() == 1,
		       "a second ServerStartVerify draws one reply");
		std::vector<NapiMessage> vcs;
		expect(!verify_out.empty() && decode_client_containers(verify_out[0], server.client_scrk, vcs) &&
		               vcs.size() == 1 && vcs[0].name == "ClientRequestVerifyResult",
		       "the second reply is another ClientRequestVerifyResult");
		expect(client.is_verified(), "re-verification leaves the session Verified");
	}

	// 14) ServerCommand: the "Cmd" param is tokenized (quoted runs are one token)
	// and the verb + target suffix resolved; an unknown verb yields no notice.
	// [orig: the ServerCommand handler loc_4D22F0]
	{
		NapiMessage cmd;
		cmd.name = "ServerCommand";
		cmd.fields.push_back(str_field("Cmd", "PuntPlayerByName \"Some Guy\" 42"));
		const auto d_cmd = server.push(cmd);
		std::vector<std::vector<uint8_t>> cmd_out;
		expect(client.handle_datagram(d_cmd.data(), d_cmd.size(), cmd_out) && cmd_out.size() == 1,
		       "ServerCommand is acked (header-only)");
		const auto notices = client.take_notices();
		if (expect(notices.size() == 1 && notices[0].kind == ClientSession::Notice::Kind::Command,
		           "one Command notice")) {
			const ServerCommand &sc = notices[0].command;
			expect(sc.verb == ServerCommandVerb::PuntPlayer && sc.target == ServerCommandTarget::ByName,
			       "PuntPlayerByName resolves verb + target");
			expect(sc.command == "PuntPlayerByName" && sc.args.size() == 2 &&
			               sc.args[0] == "Some Guy" && sc.args[1] == "42",
			       "the quoted callsign is one argument token");
		}
		NapiMessage unknown;
		unknown.name = "ServerCommand";
		unknown.fields.push_back(str_field("Cmd", "Frobnicate now"));
		const auto d_unknown = server.push(unknown);
		cmd_out.clear();
		expect(client.handle_datagram(d_unknown.data(), d_unknown.size(), cmd_out) &&
		               client.take_notices().empty(),
		       "an unknown verb falls through without a notice");
	}

	// 15) ServerStopHosting: the triple + the 52-row msgcode key; the host leg is
	// back to Idle (state 4). [orig: HandleServerMessage @0x4d1c50]
	{
		NapiMessage stop;
		stop.name = "ServerStopHosting";
		stop.fields.push_back(str_field("Success", "1"));
		stop.fields.push_back(str_field("MsgCode", "3"));
		stop.fields.push_back(str_field("MsgParam1", "0"));
		stop.fields.push_back(str_field("MsgParam2", "0"));
		const auto d_stop = server.push(stop);
		std::vector<std::vector<uint8_t>> stop_out;
		expect(client.handle_datagram(d_stop.data(), d_stop.size(), stop_out) && stop_out.size() == 1,
		       "ServerStopHosting is acked (header-only)");
		expect(client.host_state() == ClientSession::HostState::Idle,
		       "ServerStopHosting returns the host leg to Idle");
		const auto notices = client.take_notices();
		if (expect(notices.size() == 1 && notices[0].kind == ClientSession::Notice::Kind::StopHosting,
		           "one StopHosting notice")) {
			expect(notices[0].fields.msg_code == 3 &&
			               notices[0].msg_key == "NWUSERVERMSGCODE_FEATURENOTIMPLEMENTEDYET",
			       "MsgCode 3 -> its NWUSERVERMSGCODE_* key");
		}
		expect(client.build_host_player_added(HostPlayerSlot{}).empty(),
		       "no roster notifications once hosting stopped");
		expect(client.build_stop_hosting().empty(), "ClientStopHosting builds only from states 5/6");
	}

	// 16) The play leg: ClientPlayRequest (state 4 -> 7) with the PlaySetup vars,
	// ServerPlayResult Success -> Playing (state 8), ClientStopPlaying -> back to 4.
	// [orig: StartPlayingSession @0x4d45e0; HandleVerifyResponse @0x4d1e00; StopPlaying @0x4d0ec0]
	{
		expect(client.build_stop_playing().empty(), "ClientStopPlaying builds only from states 7/8");
		const std::vector<ClientVar> play_setup =
				make_play_setup_vars("OpenNova Host", "127.0.0.1", "32768", "28", 3);
		const auto d_play = client.build_play_request(play_setup);
		expect(!d_play.empty(), "ClientPlayRequest builds from a Verified idle session");
		expect(client.play_state() == ClientSession::PlayState::Requested, "play leg Requested");
		expect(client.build_play_request(play_setup).empty(), "no second ClientPlayRequest while Requested");
		std::vector<NapiMessage> pcs;
		if (expect(decode_client_containers(d_play, server.client_scrk, pcs) && pcs.size() == 1 &&
		                   pcs[0].name == "ClientPlayRequest",
		           "the play request container is ClientPlayRequest")) {
			bool has_flag = false;
			for (const auto &f : pcs[0].fields)
				if (f.name == "CurrentlyPlaying" && field_to_string(f) == "0") has_flag = true;
			expect(has_flag, "CurrentlyPlaying = 0 on the fresh play path");
			std::string lists;
			for (const auto &c : pcs[0].children) {
				for (const auto &f : c.fields)
					if (f.name == "VarList") lists += field_to_string(f) + " ";
			}
			expect(lists == "Cookie PlaySetup ", "the Cookie list precedes the PlaySetup list");
		}
		const auto s_play = server.respond(d_play);
		expect(!s_play.empty(), "ServerPlayResult datagram non-empty");
		std::vector<std::vector<uint8_t>> play_out;
		expect(client.handle_datagram(s_play.data(), s_play.size(), play_out),
		       "client handles ServerPlayResult");
		expect(client.play_state() == ClientSession::PlayState::Playing,
		       "ServerPlayResult Success -> Playing");
		expect(client.play_result().success == 1, "play_result() carries Success");
		const auto notices = client.take_notices();
		expect(notices.size() == 1 && notices[0].kind == ClientSession::Notice::Kind::PlayResult,
		       "one PlayResult notice");
		const auto d_stop_play = client.build_stop_playing();
		expect(!d_stop_play.empty() && client.play_state() == ClientSession::PlayState::Idle,
		       "ClientStopPlaying returns the play leg to Idle");
		expect(server.respond(d_stop_play).empty(), "ClientStopPlaying draws no reply");
		expect(client.mission_exit_reason() == 0, "a local stop sets no exit reason");
	}

	// 17) ServerStopPlaying: the triple, the play leg Idle, exit reason 12.
	// [orig: HandleServerDisconnectMsg @0x4d1fa0]
	{
		const auto d_play = client.build_play_request(
				make_play_setup_vars("OpenNova Host", "127.0.0.1", "32768", "28", 0));
		const auto s_play = server.respond(d_play);
		std::vector<std::vector<uint8_t>> play_out;
		expect(client.handle_datagram(s_play.data(), s_play.size(), play_out) &&
		               client.play_state() == ClientSession::PlayState::Playing,
		       "second play request reaches Playing");
		client.take_notices();
		NapiMessage stop;
		stop.name = "ServerStopPlaying";
		stop.fields.push_back(str_field("Success", "1"));
		stop.fields.push_back(str_field("MsgCode", "2001"));
		const auto d_stop = server.push(stop);
		play_out.clear();
		expect(client.handle_datagram(d_stop.data(), d_stop.size(), play_out) && play_out.size() == 1,
		       "ServerStopPlaying is acked (header-only)");
		expect(client.play_state() == ClientSession::PlayState::Idle &&
		               client.mission_exit_reason() == 12,
		       "ServerStopPlaying: play leg Idle, exit reason 12");
		const auto notices = client.take_notices();
		expect(notices.size() == 1 && notices[0].kind == ClientSession::Notice::Kind::StopPlaying &&
		               notices[0].fields.msg_code == 2001,
		       "one StopPlaying notice with the MsgCode");
		expect(client.is_verified(), "the session itself stays Verified");
	}

	// D-NET-20 pin: a client with NO cookie vars configured still emits the
	// ClientVarList(VarList="Cookie") parent — EMPTY, not absent (retail
	// serializes the var list with includeAll=1).
	// [orig: CNapiGameSession_SendVerifyRequest @ 0x4d3620 ->
	//  NapiStatement_SerializeVarList @ 0x4d0660]
	{
		MiniServer server2;
		ClientSession::Config cfg2;
		cfg2.client_index = 0x00000002u;
		cfg2.client_key   = 0x0BADF00Du;
		ClientSession client2(cfg2);
		std::vector<std::vector<uint8_t>> o2;
		auto h2 = client2.start();
		auto sh2 = server2.respond(h2);
		expect(client2.handle_datagram(sh2.data(), sh2.size(), o2),
		       "D-NET-20 flow: ServerHello handled");
		auto sa2 = server2.respond(o2[0]);
		o2.clear();
		expect(client2.handle_datagram(sa2.data(), sa2.size(), o2),
		       "D-NET-20 flow: ServerAuth handled");
		expect(o2.empty(), "D-NET-20 flow: ServerAuth has no synchronous reply");
		client2.process_periodic_update(o2);
		auto sv2 = server2.respond(o2[0]);
		o2.clear();
		expect(client2.handle_datagram(sv2.data(), sv2.size(), o2),
		       "D-NET-20 flow: ServerStartVerify handled");
		if (expect(o2.size() == 1, "D-NET-20 flow: one verify request emitted")) {
			std::vector<NapiMessage> vcs;
			expect(decode_client_containers(o2[0], server2.client_scrk, vcs),
			       "D-NET-20: empty-cfg verify request decodes");
			if (expect(vcs.size() == 1 && vcs[0].name == "ClientRequestVerifyResult",
			           "D-NET-20: verify container name")) {
				if (expect(vcs[0].children.size() == 1 &&
				               vcs[0].children[0].name == "ClientVarList",
				           "D-NET-20: the Cookie parent is emitted unconditionally")) {
					expect(vcs[0].children[0].children.empty(),
					       "D-NET-20: no vars configured -> the parent is EMPTY");
				}
			}
		}
	}

	// 0x46 addresses the receiver by ServerAuth.SK and carries the full retail
	// disconnect-stat TLV body, rather than sending ClientHello.CI as a lone
	// dword.
	{
		const auto goodbye = client.build_goodbye();
		uint8_t goodbye_opcode = 0;
		std::vector<uint8_t> goodbye_body;
		expect(server_decode_inbound(goodbye, goodbye_opcode, goodbye_body) &&
		               goodbye_opcode == SESSION_OPCODE_CLIENT_GOODBYE,
		       "ClientSession goodbye decodes as C2S 0x46");
		expect(goodbye_body == client_goodbye_to_bytes(server.server_sk),
		       "ClientSession goodbye body is keyed by ServerAuth.SK with retail TLVs");
		expect(client.state() == ClientSession::State::Closed,
		       "ClientSession closes after building goodbye");
	}

	if (g_failures == 0) {
		std::printf("OK: ClientSession handshake reached Verified (hk echo + 0x83 verify)\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
