// NW-S3 closure — the 0x42 join carries the retail CU var set.
//
// Two things are proven here:
//   1. make_novaworld_join_cu() builds the exact 11-chunk set retail's
//      CNapiGameSession_ConnectToNovaWorld @ 0x4d4640 builds, in order, type 2,
//      with the gate-sourced values threaded into GateTag/MetTag/UdpCode1/2.
//   2. A ClientSession configured with that set actually EMITS the chunks in its
//      generated ClientAuth (0x42) — i.e. the "our client sends no CU chunks"
//      gap from docs/net §8 NW-S3 is closed end to end through the real builder
//      (build_client_auth -> client_auth_to_bytes -> envelope/NWU), decoded back
//      with parse_client_auth + parse_client_cu_chunk.
//
// Self-contained: drives the Godot-free ClientSession against a minimal
// ServerHello crafted from the real library codecs (the same calls
// apps/novaworld_server makes). No sockets.

#include <novaworld/client_session.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <napi/envelope.h>
#include <novacrypto/nwu.h>

#include <cstdint>
#include <cstdio>
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

// Server-direction encode of an inbound datagram for the client (mirrors
// apps/novaworld_server/nw_udp_listener.cpp + the loopback test): NWU-encrypt is
// the server's nwu_decrypt (names swapped vs onnet), opcode-prefix, CRC-envelope.
std::vector<uint8_t> encode_inbound_for_client(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
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

// Recover the plaintext (opcode, body) of a datagram the CLIENT sent: strip the
// CRC envelope, peel the opcode, then nwu_encrypt the body (client-side encrypt
// is our nwu_decrypt, so the recovering side applies nwu_encrypt).
bool decode_outbound_from_client(const std::vector<uint8_t> &raw,
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
	if (!body_out.empty()) nwu_encrypt(body_out.data(), body_out.size(), SESSION_NWU_KEY);
	return true;
}

struct ExpectedCu {
	const char *name;
	std::string value;
};

} // namespace

int main() {
	// ---- 1. make_novaworld_join_cu builds the retail set ------------------
	NovaWorldJoinCu in;
	in.gate_tag = "jop:cus2";
	in.met_tag = "metlabel-xyz";
	in.udp_code1 = "1234567890";
	in.udp_code2 = "0987654321";
	auto cu = make_novaworld_join_cu(in);

	const ExpectedCu expected[] = {
	    {"Application", "OpennovaGodotClient.exe"},
	    {"BuildDateAndTime", "Jul 21 2009 18:54:41"},
	    {"Debug", "0"},
	    {"CountryName", ""},
	    {"Language", ""},
	    {"TimeZoneBias", ""},
	    {"GateTag", "jop:cus2"},
	    {"MetTag", "metlabel-xyz"},
	    {"UdpCode1", "1234567890"},
	    {"UdpCode2", "0987654321"},
	    {"MaxPacketSize", "1300"},
	};
	constexpr size_t kCount = sizeof(expected) / sizeof(expected[0]);

	if (expect(cu.size() == kCount, "make_novaworld_join_cu yields 11 chunks")) {
		for (size_t i = 0; i < kCount; ++i) {
			expect(cu[i].name == expected[i].name,
			       (std::string("CU[") + std::to_string(i) + "] name == " + expected[i].name).c_str());
			expect(cu[i].value == expected[i].value,
			       (std::string("CU[") + std::to_string(i) + "] value matches").c_str());
			expect(cu[i].type == 2,
			       (std::string("CU[") + std::to_string(i) + "] type == 2 (retail .204)").c_str());
		}
	}

	// ---- 2. ClientSession EMITS them in its generated ClientAuth (0x42) ----
	ClientSession::Config cfg;
	cfg.client_index = 0x11223344u;
	cfg.client_key = 0x55667788u;
	cfg.cu_vars = cu;

	ClientSession session(cfg);
	auto hello_dg = session.start();
	expect(!hello_dg.empty(), "ClientSession.start() produced a ClientHello");

	// Recover the ClientHello so we can craft a matching ServerHello.
	uint8_t op = 0;
	std::vector<uint8_t> body;
	expect(decode_outbound_from_client(hello_dg, op, body), "decode ClientHello datagram");
	expect(op == SESSION_OPCODE_CLIENT_HELLO, "first datagram is the 0x41 ClientHello");
	ClientHello hello;
	expect(parse_client_hello(body.data(), body.size(), hello), "parse ClientHello");

	ServerHello sh = build_server_hello(hello, 0x7F000001u, 5000);
	auto server_hello_dg = encode_inbound_for_client(
	    SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(sh));

	std::vector<std::vector<uint8_t>> out;
	expect(session.handle_datagram(server_hello_dg.data(), server_hello_dg.size(), out),
	       "client accepts ServerHello");
	expect(out.size() == 1, "client replies with exactly one datagram (the ClientAuth)");

	if (!out.empty()) {
		uint8_t auth_op = 0;
		std::vector<uint8_t> auth_body;
		expect(decode_outbound_from_client(out[0], auth_op, auth_body),
		       "decode ClientAuth datagram");
		expect(auth_op == SESSION_OPCODE_CLIENT_AUTH, "reply is the 0x42 ClientAuth");

		ClientAuth auth;
		expect(parse_client_auth(auth_body.data(), auth_body.size(), auth),
		       "parse ClientAuth");
		// The whole point of NW-S3: the join is no longer CU-empty.
		if (expect(auth.cu.size() == kCount,
		           "ClientAuth carries all 11 CU chunks (NW-S3 closed)")) {
			for (size_t i = 0; i < kCount; ++i) {
				uint8_t t = 0;
				std::string name, value;
				if (!expect(parse_client_cu_chunk(auth.cu[i].data(), auth.cu[i].size(),
				                                  t, name, value),
				            (std::string("ClientAuth CU[") + std::to_string(i) + "] decodes").c_str())) {
					continue;
				}
				expect(t == 2 && name == expected[i].name && value == expected[i].value,
				       (std::string("ClientAuth CU[") + std::to_string(i) + "] == "
				        + expected[i].name).c_str());
			}
		}
	}

	if (g_failures == 0) {
		std::printf("client_session_cu_test: all assertions passed\n");
		return 0;
	}
	std::fprintf(stderr, "client_session_cu_test: %d failure(s)\n", g_failures);
	return 1;
}
