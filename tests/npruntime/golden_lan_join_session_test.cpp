// Golden parity for the P2 per-connection handshake legs, driven by the local LAN host/join session
// capture (.scratch/golden/retail-lan-host-join-session.pcapng). Env-gated on NW_GOLDEN_LAN_JOIN_SESSION
// with a DEFAULT_* fallback; skips cleanly when the golden is absent (no committed derived oracle).
//
// WHAT THIS PROVES (the P2 bar, honestly scoped):
//   The promoted np handshake legs, fed the golden's real C2S 0x42 ClientAuth and seed-injected with
//   the golden's host_key / server SK / server SCRK, reproduce every WIRE-SIGNIFICANT field the legs
//   control on the S2C replies:
//     0x81 ServerHello : HK == golden host_key (the R1 host-key advertise), PN/PG echoed.
//     0x82 ServerAuth  : CI/CK echoed from ClientAuth, CR==1, SK/SCRK == seed-injected, NA echoed.
//   These are exactly the fields handle_client_hello / handle_client_join own.
//
// WHAT IS DEFERRED (documented, NOT faked green):
//   Full-datagram byte-parity of 0x81/0x82 is NOT asserted. The remaining divergence lives in the
//   libs/novaworld session builders (build_server_hello / build_server_auth), NOT the P2 legs. The
//   builders are now GRILLED (2026-06-27, D-NET Wave 3): 0x81 = NapiNPProtocol_SendServerInfoPacket
//   @0x6204b0 (FIXED — flat builder, SF unconditional, no PL; D-NET-16/18), 0x82 =
//   CNapiNPConnection_SendSessionInit @0x620ef0 (witnessed; see its IDB comment). Measured against this
//   golden, a LAN host's replies still differ from our builders in:
//     0x81: ServerHello.CI is the host node index (retail=2), not the client echo; identity strings
//           AP/BDAT/PV3/SN are the retail build's, not our defaults; UT is a live uptime.
//     0x82: MI is host-specific (retail=3, our default 0x113f — seed-injected in the test). The 0x82
//           builder is now faithful: CU gated on NovaWorld transport (LAN emits none; D-NET Wave 3),
//           CS = the WITNESSED engine_cs_fields template (D-NET-1), MI = the host-assigned dcb. The
//           residual byte diff is purely the host-specific seed values (CI host-node-index, identity
//           strings, live UT uptime), not builder logic.
//   The full byte diff is printed below for the record.

#include <npruntime/lan_discovery.h>
#include <npruntime/napi_np_protocol.h>

#include "host_test_setup.h"

#include <npwire/peer_addr.h>
#include <npwire/nw_session_framing.h>
#include <npwire/session_hello.h>
#include <npwire/session_keys.h>

#include <pcapio/pcap_reader.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef DEFAULT_LAN_JOIN_SESSION_PCAP
#define DEFAULT_LAN_JOIN_SESSION_PCAP ""
#endif

namespace {
using namespace opennova;
namespace np = opennova::np;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

bool decode(const net::PcapDatagram &p, uint8_t &op, std::vector<uint8_t> &body) {
	return nw_decode_inbound(p.payload.data(), p.payload.size(), op, body);
}

int first_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i)
		if (a[i] != b[i]) return static_cast<int>(i);
	return a.size() == b.size() ? -1 : static_cast<int>(n);
}
} // namespace

int main() {
	std::string path;
	if (const char *env = std::getenv("NW_GOLDEN_LAN_JOIN_SESSION"); env && *env)
		path = env;
	else
		path = DEFAULT_LAN_JOIN_SESSION_PCAP;

	std::vector<net::PcapDatagram> pkts;
	if (path.empty() || !net::read_pcap_udp_file(path, pkts)) {
		std::printf("[skip] golden LAN-join-session capture not found "
		            "(set NW_GOLDEN_LAN_JOIN_SESSION) — '%s'\n", path.c_str());
		return 0; // skip clean — CI stays green without the gitignored golden
	}

	// Partition: the host is the source of the 0x81/0x82 server replies; the joiner is the source of
	// the 0x42 ClientAuth. Grab the first of each + the joiner's port.
	std::vector<uint8_t> raw42, raw81, g81, g82;
	int joiner_port = 0;
	for (const auto &p : pkts) {
		uint8_t op = 0;
		std::vector<uint8_t> body;
		if (!decode(p, op, body)) continue;
		if (op == SESSION_OPCODE_CLIENT_AUTH && raw42.empty()) {
			raw42 = p.payload; // the REAL golden ClientAuth datagram (envelope on) — replayed below
			joiner_port = p.srcport;
		} else if (op == SESSION_OPCODE_SERVER_HELLO && g81.empty()) {
			raw81 = p.payload; // envelope on — the LAN browse gate consumes raw datagrams
			g81 = body;
		} else if (op == SESSION_OPCODE_SERVER_AUTH && g82.empty()) {
			g82 = body;
		}
	}
	if (!expect(!raw42.empty() && !g81.empty() && !g82.empty(),
	            "golden carries 0x42 ClientAuth + 0x81 ServerHello + 0x82 ServerAuth")) return 1;

	// Parse the golden replies to recover the seed values (host_key, server SK/SCRK) + the oracle
	// fields the legs must echo.
	ServerHello gh;
	ServerAuth ga;
	ClientAuth gca;
	if (!expect(parse_server_hello(g81.data(), g81.size(), gh), "golden 0x81 parses")) return 1;
	if (!expect(parse_server_auth(g82.data(), g82.size(), ga), "golden 0x82 parses")) return 1;
	{
		uint8_t op = 0;
		std::vector<uint8_t> body42;
		nw_decode_inbound(raw42.data(), raw42.size(), op, body42);
		if (!expect(parse_client_auth(body42.data(), body42.size(), gca), "golden 0x42 parses")) return 1;
	}

	// The LAN browse reply gate must accept a REAL retail 0x81: the gate keys on
	// the retail JO identity (is_game_server + PN/PG/PV1/PV2) and must tolerate
	// the retail build's extra tags (AP/BDAT/PV3, live UT) our own hosts omit.
	{
		np::LanDiscoveryServer row;
		if (!expect(np::parse_lan_discovery_reply(raw81.data(), raw81.size(), row),
		            "retail golden 0x81 passes the LAN browse reply gate")) return 1;
		if (!expect(row.server_name == gh.sn, "browse row carries the golden SN")) return 1;
		if (!expect(row.max_players == gh.mp, "browse row carries the golden MP")) return 1;
	}

	// The joiner peer: src port from the capture; IP from the host's reflected RIP (so the replies'
	// RIP/RPN are computed from the same address the host saw).
	const PeerAddr peer{ga.rip, static_cast<uint16_t>(joiner_port)};

	// Stand the host up through the real P0->P1->P2 lifecycle, seeding the golden's host_key so the
	// 0x81 HK is stamped (R1) and the golden's real 0x42 (auth.hk == gh.hk, pn == "JOINTOPERATIONS")
	// passes the join gate. Then seed-inject the golden's volatile server keys (the P1 "pass in,
	// don't sample" approach) so our 0x82 SK/SCRK match the golden's.
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless, gh.hk);
	ctx.server_key_mint.forced = true;
	ctx.server_key_mint.server_sk = ga.sk;
	ctx.server_key_mint.server_scrk = ga.scrk;

	// --- 0x81 leg: the capture lacks the 0x41 ClientHello, so reconstruct it from the ClientAuth
	//     identity block (retail re-sends the same identity on the 0x42 join), craft the datagram,
	//     and drive the real handle_client_hello leg. ---
	ClientHello ch;
	ch.nvs = gca.nvs; ch.co = gca.co; ch.ap = gca.ap; ch.bdat = gca.bdat; ch.pn = gca.pn;
	ch.pg = gca.pg; ch.pg_present = true; ch.pv1 = gca.pv1; ch.pv2 = gca.pv2; ch.ci = gca.ci;
	auto raw41 = nw_encode_outbound(SESSION_OPCODE_CLIENT_HELLO, client_hello_to_bytes(ch));

	auto r81 = np::handle_server_datagram(ctx, peer, raw41.data(), raw41.size(), 1);
	if (!expect(r81.outbound.size() == 1, "0x41 -> one ServerHello")) return 1;
	std::vector<uint8_t> o81;
	{
		uint8_t op = 0;
		if (!expect(nw_decode_inbound(r81.outbound[0].data(), r81.outbound[0].size(), op, o81) &&
		            op == SESSION_OPCODE_SERVER_HELLO, "our reply is a 0x81")) return 1;
	}
	ServerHello oh;
	if (!expect(parse_server_hello(o81.data(), o81.size(), oh), "our 0x81 parses")) return 1;
	if (!expect(oh.hk == gh.hk, "0x81 advertises the golden host_key (R1 host-key stamp)")) return 1;
	if (!expect(oh.pn == gh.pn, "0x81 PN echoes the client PN")) return 1;
	if (!expect(oh.pg == gh.pg, "0x81 PG echoes the client PG")) return 1;

	// --- 0x82 leg: replay the REAL golden ClientAuth datagram through handle_client_join. ---
	auto r82 = np::handle_server_datagram(ctx, peer, raw42.data(), raw42.size(), 2);
	if (!expect(r82.outbound.size() >= 1, "0x42 -> ServerAuth + post-handshake")) return 1;
	std::vector<uint8_t> o82;
	{
		uint8_t op = 0;
		if (!expect(nw_decode_inbound(r82.outbound[0].data(), r82.outbound[0].size(), op, o82) &&
		            op == SESSION_OPCODE_SERVER_AUTH, "our reply is a 0x82")) return 1;
	}
	ServerAuth oa;
	if (!expect(parse_server_auth(o82.data(), o82.size(), oa), "our 0x82 parses")) return 1;
	if (!expect(oa.ci == ga.ci, "0x82 CI echoes ClientAuth.ci")) return 1;
	if (!expect(oa.ck == ga.ck, "0x82 CK echoes ClientAuth.ck")) return 1;
	if (!expect(oa.cr == ga.cr && oa.cr == 1, "0x82 CR == accepted (1)")) return 1;
	if (!expect(oa.sk == ga.sk, "0x82 SK == seed-injected server key")) return 1;
	if (!expect(oa.scrk == ga.scrk, "0x82 SCRK == seed-injected server SCRK")) return 1;
	if (!expect(oa.na == ga.na, "0x82 NA echoes ClientAuth.na")) return 1;

	// --- Record the full-datagram divergence (deferred to a libs/novaworld builder grill). ---
	std::printf("[golden] handshake-leg field parity OK (0x81 HK/PN/PG; 0x82 CI/CK/CR/SK/SCRK/NA).\n");
	std::printf("[golden] full-datagram byte diff (DEFERRED — libs/novaworld builder field-set):\n");
	std::printf("         0x81: golden=%zuB ours=%zuB first_diff=%d (retail CI=%u game_server=%d; ours CI=%u)\n",
	            g81.size(), o81.size(), first_diff(g81, o81), gh.ci, gh.is_game_server, oh.ci);
	std::printf("         0x82: golden=%zuB ours=%zuB first_diff=%d (retail MI=0x%x cu#=%zu; ours MI=0x%x)\n",
	            g82.size(), o82.size(), first_diff(g82, o82), ga.mi, ga.cu.size(), oa.mi);

	std::printf("OK\n");
	return 0;
}
