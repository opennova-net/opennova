// ADR 0010 Phase 5 (proto switch) — the JointOperations ClientHello.
//
// After NWJoin the client opens a session to the host and sends a ClientHello
// whose PN flips the connection protocol from the lobby (NOVAWORLDUDP) to the
// in-match game (JOINTOPERATIONS). This pins the exact retail identity populated
// by CNapiNetwork_Init @ 0x4ca4a0. The default NOVAWORLDUDP hello remains
// unchanged, so the per-PN identity selector cannot regress the lobby path.

#include <novaworld/client_session.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <napi/envelope.h>
#include <novacrypto/nwu.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;

void check(bool cond, const char *what) {
	if (!cond) {
		std::printf("  FAIL: %s\n", what);
		++g_failures;
	}
}

// Decode a client session datagram exactly as the server does: strip the CRC
// envelope, peel the opcode, NWU-decrypt the body (server-side nwu_encrypt).
bool decode_hello(const std::vector<uint8_t> &raw, ClientHello &out) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out_size = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out_size) != 0) {
		return false;
	}
	stripped.resize(out_size);
	if (stripped.empty() || stripped[0] != SESSION_OPCODE_CLIENT_HELLO) return false;
	std::vector<uint8_t> body(stripped.begin() + 1, stripped.end());
	if (!body.empty()) nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	return parse_client_hello(body.data(), body.size(), out);
}

} // namespace

int main() {
	const std::array<uint8_t, 16> kRetailJoPg{
	    0x46, 0xD6, 0x74, 0xB0, 0xF9, 0x81, 0x5F, 0x47,
	    0x92, 0xDA, 0xDE, 0xA7, 0x24, 0x7F, 0x14, 0x68};

	// Exact retail JointOperations game-session hello.
	ClientSession jo{ClientSession::Config::jointoperations()};
	ClientHello jo_hello;
	check(decode_hello(jo.start(), jo_hello), "JO hello decodes");
	check(jo_hello.nvs ==
	              "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic",
	      "JO hello NVS == retail Milota version");
	check(jo_hello.co == "NovaLogic Inc, Calabasas CA U.S.A.",
	      "JO hello CO == retail NovaLogic company");
	check(jo_hello.ap == "Jointops.exe", "JO hello AP == retail executable");
	check(jo_hello.bdat == "Jul 21 2009 18:54:42", "JO hello BDAT == retail build stamp");
	check(jo_hello.pn == "JOINTOPERATIONS", "JO hello PN == JOINTOPERATIONS");
	check(jo_hello.pv1 == "0.0.0 1/12/2004 EM", "JO hello PV1 == game protocol date");
	check(jo_hello.pv2 == "16", "JO hello PV2 == retail game protocol version");
	check(jo_hello.pg_present, "JO hello carries PG");
	check(jo_hello.pg == kRetailJoPg, "JO hello PG == retail JO GUID");

	// Default (lobby) hello — unchanged.
	ClientSession nw{};  // default Config -> NOVAWORLDUDP
	ClientHello nw_hello;
	check(decode_hello(nw.start(), nw_hello), "NOVAWORLDUDP hello decodes");
	check(nw_hello.pn == "NOVAWORLDUDP", "default hello PN == NOVAWORLDUDP");
	check(nw_hello.pv1 == "0.0.0 2/10/2004 EM", "default hello PV1 unchanged");
	check(nw_hello.pg == (std::array<uint8_t, 16>{
	          0xF2, 0x0C, 0xEE, 0xD8, 0xCE, 0xE4, 0x8D, 0x44,
	          0x90, 0xB4, 0x1D, 0x42, 0xB3, 0x64, 0xAB, 0x71}),
	      "default hello PG == NOVAWORLDUDP GUID (unchanged)");

	// The selector actually switched the GUID.
	check(jo_hello.pg != nw_hello.pg, "JO and lobby PG GUIDs differ");

	if (g_failures == 0) {
		std::printf("jointops_hello: all checks passed\n");
		return 0;
	}
	std::printf("jointops_hello: %d check(s) failed\n", g_failures);
	return 1;
}
