// ADR 0010 Phase 5 (proto switch) — the JointOperations ClientHello.
//
// After NWJoin the client opens a session to the host and sends a ClientHello
// whose PN flips the connection protocol from the lobby (NOVAWORLDUDP) to the
// in-match game (JointOperations). This pins that ClientSession::Config::
// jointoperations() builds that hello: PN == "JointOperations" and a PG distinct
// from the lobby GUID. The default (NOVAWORLDUDP) hello is unchanged — the
// per-PN PG selector must not regress the lobby path.
//
// PG/PV1 for JointOperations are PROVISIONAL pending the StartPlaying @ 0x4d45e0
// grill; this test pins the provisional placeholder so a later real-GUID change
// is a deliberate, visible edit here.

#include <novaworld/client_session.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>

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
	// The provisional JointOperations PG placeholder (mirror of jointoperations_pg()
	// in client_session.cpp). Update both together when the real GUID is grilled.
	const std::array<uint8_t, 16> kProvisionalJoPg{
	    'J', 'O', '-', 'P', 'R', 'O', 'V', 'I', 'S', '-', 'P', 'G', 0, 0, 0, 0};

	// JointOperations hello.
	ClientSession jo{ClientSession::Config::jointoperations()};
	ClientHello jo_hello;
	check(decode_hello(jo.start(), jo_hello), "JO hello decodes");
	check(jo_hello.pn == "JointOperations", "JO hello PN == JointOperations");
	check(jo_hello.pg_present, "JO hello carries PG");
	check(jo_hello.pg == kProvisionalJoPg, "JO hello PG == provisional JO GUID");

	// Default (lobby) hello — unchanged.
	ClientSession nw{};  // default Config -> NOVAWORLDUDP
	ClientHello nw_hello;
	check(decode_hello(nw.start(), nw_hello), "NOVAWORLDUDP hello decodes");
	check(nw_hello.pn == "NOVAWORLDUDP", "default hello PN == NOVAWORLDUDP");
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
