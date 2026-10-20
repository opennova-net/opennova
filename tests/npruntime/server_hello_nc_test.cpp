// npruntime_server_hello_nc: the host's 0x81 NC and the connection counter at the host's creates
// (net parity lane L1).
//   - D-NET-523: NC is the game protocol's server-side (type-1) node count, written iff nonzero
//     [orig: NapiNPProtocol_SendServerInfoPacket @0x6204B0 - proto+0xED4 @0x620A4D, the gate
//     @0x620A53..0x620A6C; CNapiNPConnection_Create @0x62B1BD..0x62B1CB; CNapiNPConnection_Destroy
//     @0x62A5FF]; a listen host holds one from its session start, its self-join's
//     [orig: CNapiGameSession_CreateSession @0x4C9B6A..0x4C9B9C; NapiNPProtocol_HandleClientJoin
//     @0x62BF06]. The jox01 witness: every retail listen host's 0x81 in the parity-20261011 suite
//     carries NC 1 before any joiner.
//   - D-NET-713: every create steps the connection counter [orig: CNapiNPConnection_Create
//     @0x62B282..0x62B296]: a listen host's session create twice, a 0x42 past the protocol's gates
//     once (its create precedes the validate callback, @0x62BF06 ahead of @0x62C0B1).

#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/loopback_channel.h>

#include "host_test_setup.h"

#include <net/npwire/lan_discovery.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <net/title/title_identity.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace inmatch = opennova::inmatch;
using opennova::PeerAddr;

const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);
constexpr uint32_t kHostKey = 0x0BADF00Du;
const char kScrk[] = "JOINERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFG";

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// One browse probe's 0x81; `nc` its NC (0 when the TLV is absent: the writer skips a zero).
bool probe_nc(inmatch::NapiNPServerCtx &ctx, uint32_t &nc) {
	const PeerAddr browser{0x0100007Fu, 41000};
	const std::vector<uint8_t> probe =
			opennova::build_lan_discovery_probe(kJo, 0x11223344u, 0xFFFFFFFFu, 32786u);
	const inmatch::HandleResult r =
			inmatch::handle_server_datagram(ctx, browser, probe.data(), probe.size());
	if (r.outbound.size() != 1) return false;
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	if (!opennova::nw_decode_inbound(r.outbound[0].data(), r.outbound[0].size(), opcode, body) ||
			opcode != opennova::SESSION_OPCODE_SERVER_HELLO)
		return false;
	opennova::ServerHello hello;
	if (!opennova::parse_server_hello(body.data(), body.size(), hello)) return false;
	nc = hello.nc;
	return true;
}

// One 0x42 from `peer`.
void join(inmatch::NapiNPServerCtx &ctx, const PeerAddr &peer, uint32_t ck, uint32_t hk) {
	opennova::ClientAuth auth = opennova::make_title_client_auth(kJo, 1, ck, hk, "Joiner", kScrk);
	const std::vector<uint8_t> datagram = opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_CLIENT_AUTH, opennova::client_auth_to_bytes(auth));
	(void)inmatch::handle_server_datagram(ctx, peer, datagram.data(), datagram.size());
}

// The counter's steps between two reads: each read is itself a step.
uint32_t steps_since(uint32_t before) { return opennova::next_connection_index() - before - 1u; }

bool run_listen_host_answers_its_own_node() {
	auto loopback = std::make_unique<opennova::replication::LoopbackChannel>();
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 4;
	const uint32_t before = opennova::next_connection_index();
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Lan, kHostKey, loopback.get(), config);
	if (!expect(steps_since(before) == 2, "a listen host's session create steps the counter twice"))
		return false;
	uint32_t nc = 0;
	if (!expect(probe_nc(*ctx, nc) && nc == 1, "a listen host answers NC 1 before any joiner"))
		return false;
	const PeerAddr joiner{0x0100007Fu, 41001};
	uint32_t mark = opennova::next_connection_index();
	join(*ctx, joiner, 0xC0FFEE01u, kHostKey);
	if (!expect(steps_since(mark) == 1, "an accepted 0x42 steps the counter once")) return false;
	if (!expect(probe_nc(*ctx, nc) && nc == 2, "the joiner's node makes NC 2")) return false;
	// The same client's retransmitted 0x42 re-sends its ServerAuth: no create, no step.
	mark = opennova::next_connection_index();
	join(*ctx, joiner, 0xC0FFEE01u, kHostKey);
	if (!expect(steps_since(mark) == 0, "a same-key retransmit steps nothing")) return false;
	(void)inmatch::destroy_connection(*ctx, joiner, nullptr);
	return expect(probe_nc(*ctx, nc) && nc == 1, "the joiner's teardown returns NC to 1");
}

bool run_serve_only_host_counts_its_joiners() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::GameConfig config;
	config.max_players = 1;
	config.use_lineup_queue = 0;
	const uint32_t before = opennova::next_connection_index();
	inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
			inmatch::SocketMode::Lan, kHostKey, nullptr, config);
	if (!expect(steps_since(before) == 0, "a Serve Only host's session create steps nothing"))
		return false;
	uint32_t nc = 99;
	if (!expect(probe_nc(*ctx, nc) && nc == 0, "a Serve Only host answers with no NC")) return false;
	// A 0x42 refused at the protocol's own gates (HK) creates nothing.
	uint32_t mark = opennova::next_connection_index();
	join(*ctx, PeerAddr{0x0100007Fu, 41010}, 0xC0FFEE10u, kHostKey + 1u);
	if (!expect(steps_since(mark) == 0 && probe_nc(*ctx, nc) && nc == 0,
	            "an HK refusal steps nothing and leaves NC absent")) return false;
	// The first joiner fills the cap of 1.
	mark = opennova::next_connection_index();
	join(*ctx, PeerAddr{0x0100007Fu, 41011}, 0xC0FFEE11u, kHostKey);
	if (!expect(steps_since(mark) == 1 && probe_nc(*ctx, nc) && nc == 1,
	            "an accepted joiner steps once and makes NC 1")) return false;
	// A capacity refusal follows the create: it steps, and its node is gone with the handler.
	mark = opennova::next_connection_index();
	join(*ctx, PeerAddr{0x0100007Fu, 41012}, 0xC0FFEE12u, kHostKey);
	if (!expect(inmatch::connection_count(*ctx) == 1, "the cap refuses the second joiner"))
		return false;
	if (!expect(steps_since(mark) == 1, "a capacity refusal still steps the counter")) return false;
	if (!expect(probe_nc(*ctx, nc) && nc == 1, "the refused 0x42 leaves NC 1")) return false;
	// The join lock refuses after the create too.
	ctx->join_locked = true;
	mark = opennova::next_connection_index();
	join(*ctx, PeerAddr{0x0100007Fu, 41013}, 0xC0FFEE13u, kHostKey);
	return expect(steps_since(mark) == 1 && probe_nc(*ctx, nc) && nc == 1,
	              "a lock refusal steps the counter and leaves NC 1");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_listen_host_answers_its_own_node()) ++failures;
	if (!run_serve_only_host_counts_its_joiners()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
