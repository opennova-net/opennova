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

#include <novaworld/client_session.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>
#include <npwire/protocol_message.h>
#include <novaworld/lobby_session.h>

#include <napi/envelope.h>
#include <napi/session.h>
#include <napi/tlv.h>
#include <novacrypto/nwu.h>

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
	LobbyState lobby;
	LobbySession session;

	MiniServer() {
		session.set_sess_id_generator([] { return std::string("deadbeefcafef00d1122334455667788"); });
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
						std::vector<NapiMessage> stream{std::move(reply_container)};
						std::vector<uint8_t> sb(napi_stream_size(stream));
						size_t ss = 0;
						if (napi_stream_encode(stream, sb.data(), sb.size(), &ss) != 0) continue;
						sb.resize(ss);
						ProtocolMessage rpm;
						rpm.flags.raw = (ss > 0xFFu) ? 0x40u : 0x20u;
						rpm.flags.len16 = ss > 0xFFu;
						rpm.flags.len8 = ss <= 0xFFu;
						rpm.tag = 0;
						rpm.full_tag = 0;
						rpm.length = static_cast<uint32_t>(ss);
						rpm.payload = std::move(sb);
						replies.push_back(std::move(rpm));
					}
				}
			}
			if (replies.empty()) return {}; // ack-only (e.g. heartbeat)

			ProtocolPacketHeader rhdr;
			rhdr.session_id = client_ck;
			rhdr.seq_num = next_seq++;
			rhdr.ack_count = hdr.seq_num;
			rhdr.connection_flags = 0;
			std::vector<uint8_t> body_out;
			if (!encode_protocol_packet_plaintext(rhdr, replies, server_scrk, body_out)) return {};
			return server_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
			                              std::move(body_out));
		}
		default:
			return {};
		}
	}
};

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

} // namespace

int main() {
	test_parser_roundtrip();
	test_client_correlates_handshake_echoes();

	MiniServer server;
	ClientSession::Config cfg;
	cfg.client_index = 0x00000001u;
	cfg.client_key   = 0x0BADF00Du;
	// NW-S5: configure the verify "Cookie" var-list so the verify request carries
	// the witnessed structure. NWUID is left empty -> the client must echo it from
	// the ServerSessionInit; NWCDKIID empty mirrors retail (verify is not
	// CD-key-gated). Asserted on d_verify_req below.
	cfg.verify_cookie_vars = {
	    {"CountryName", "United States"},
	    {"NWUID", ""},
	    {"NWCDKIID", ""},
	    {"NWPSSK", "ABCDEFGHIJKLMNOPQRSTUVW"},
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
		    /*Host*/      {{0, "ServerName", "OpenNova Host"}, {0, "ServerIP", "127.0.0.1"},
		                   {0, "ServerPortNumber", "32768"}, {0, "Players", "1"},
		                   {0, "Region", "us"}},
		    /*PlayerList*/{{0, "Slot0", "Host"}});
		auto d_host = client.build_lobby_message(host_req);
		expect(!d_host.empty(), "host-request datagram non-empty (session Verified)");
		auto s_host = server.respond(d_host);
		expect(!s_host.empty(), "ServerHostResult datagram non-empty");
		expect(server.lobby.hosting, "gate marks lobby hosting");
		expect(server.lobby.server_name == "OpenNova Host", "gate stored ServerName");
		expect(server.lobby.host_ip == "127.0.0.1", "gate stored ServerIP");
		expect(server.lobby.host_port == 32768, "gate stored ServerPortNumber");
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
		    /*PlayerList*/{{0, "Slot0", "Host"}, {0, "Slot1", "Joiner"}});
		auto d_upd = client.build_lobby_message(host_upd);
		expect(!d_upd.empty(), "host-update datagram non-empty");
		auto s_upd = server.respond(d_upd);
		expect(s_upd.empty(), "ClientHostUpdate draws no reply (ack-only)");
		expect(server.lobby.player_count == 2, "gate update refreshed player count");
		expect(server.lobby.server_name == "OpenNova Host 2", "gate update refreshed ServerName");
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
