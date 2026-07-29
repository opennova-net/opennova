// P6 — two endpoints over REAL UDP sockets run a full join -> spawn -> play loop. Converts the P5
// in-process round-trip (client_runtime_test) to two bound loopback UDP sockets driven single-threaded
// (poll-pump, no threads/sleeps beyond the socket recv timeouts): a np::ClientRuntime joiner on one
// socket and the host owner-loop (npruntime/host_session.h, the SAME loop main.cpp runs) on the
// other. Bytes cross via real sendto/recvfrom — the only difference from the in-process test.
//
// WHAT THIS PROVES (the P6 bar):
//   (1) Over real sockets: handshake (0x41/0x42) -> the §5.2a spawn-gate burst -> the host streams the
//       joiner's NAMED dcb-bearing 0x0C -> the joiner name-matches and receives the applicable
//       deployment release -> InMatch (learns wire handle H).
//   (2) Per frame: ClientRuntime emits a framed C2S 0x0C (+ the §5.44 0x2C RTT housekeeping) over UDP ->
//       handle_server_datagram surfaces PeerC2SInMatch -> apply_in_match_c2s stages it -> Server_TickUpdate
//       drains+SNAPs the entity + fans an S2C 0x0A -> the owner reframes it 0x83 over UDP -> the client
//       folds it into ClientState. The peer SNAPs to the uplink, no synthetic host player exists, and
//       the client's ClientState anchor == the joiner's post-SNAP position.
//   (3) The host's emitted S2C stream (recorded at the joiner socket) appears in the §5.2a order, and —
//       when the gitignored golden is present (NW_GOLDEN_LAN_JOIN) — agrees pairwise with the retail LAN
//       host/join capture's S2C order on the common tags. Order-only (body byte-parity is deferred: our
//       world stream is built from our own minimal World, not the capture's mission).

#include <npruntime/host_session.h> // the host owner loop (promoted to libs/npruntime; SAME loop main.cpp runs)

#include "net_datagram_socket.h" // net::Socket-backed netsim::IDatagramSocket adapter (for the host loop)

#include <npruntime/client_runtime.h>
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_protocol.h>
#include <npruntime/server_session.h>
#include <npruntime/server_spawn.h>

#include "host_test_setup.h"

#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/wire_capture.h>

#include <mission/bms.h>

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include "net_sockets.h"
#include <pcapio/pcap_reader.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef DEFAULT_LAN_JOIN_PCAP
#define DEFAULT_LAN_JOIN_PCAP ""
#endif

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// first-occurrence index of `tag` (or -1), and the present-only relative-order check — copied from
// golden_lan_join_test so the §5.2a order assertions read identically against our own host's stream.
int first_of(const std::vector<uint8_t> &tags, uint8_t tag) {
	for (std::size_t i = 0; i < tags.size(); ++i)
		if (tags[i] == tag) return static_cast<int>(i);
	return -1;
}
bool order_ok(const std::vector<uint8_t> &tags, uint8_t before, uint8_t after) {
	const int a = first_of(tags, before);
	const int b = first_of(tags, after);
	return a >= 0 && b >= 0 && a < b;
}

// The host's view of the joiner's address: both sockets bind 127.0.0.1, so the joiner's PeerAddr is
// {0x0100007F, joiner_port} (the same packing NetDatagramSocket::to_peer produces from a recvfrom).
PeerAddr joiner_peer_addr(uint16_t joiner_port) { return PeerAddr{0x0100007Fu, joiner_port}; }

} // namespace

int main() {
	// ---- bind two real loopback UDP sockets ----
	if (net::startup() != 0) return (std::fprintf(stderr, "FAIL: net::startup\n"), 1);
	uint16_t host_port = 0, joiner_port = 0;
	net::ScopedSocket host_sock(net::udp_bind(0, &host_port));
	net::ScopedSocket joiner_sock(net::udp_bind(0, &joiner_port));
	if (!expect(host_sock.is_valid() && joiner_sock.is_valid(), "bound two loopback UDP sockets")) {
		net::shutdown();
		return 1;
	}
	const net::Endpoint host_ep{{127, 0, 0, 1}, host_port};
	// The host owner loop pumps the host socket through this adapter. recv_timeout_ms=30 lets the
	// single-threaded poll-pump block briefly for the joiner's datagram (the old host_owner_pump arg).
	net::NetDatagramSocket host_dgram(host_sock.get(), 30);

	// ---- host: a minimal World (one 6002 start marker => spawn-select + a 0x20 pool-3 record) + a
	//      minimal in-memory mission (0x0B BMS header). Mirrors initial_state_burst_test. ----
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	{
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {50.0f, 60.0f, 1.0f};
		start.yaw = 0;
		world.registry.spawn(3, start);
	}
	bms::File mission;
	mission.header.magic[0] = 'B';
	mission.header.magic[1] = 'M';
	mission.header.magic[2] = 'S';
	mission.header.magic[3] = static_cast<char>(bms::kMinVersion);

	// ---- host: stand up the dedicated HostOnly runtime (no local player). ----
	np::HostOwner owner;
	owner.ctx.world = &world;
	owner.ctx.mission = &mission;
	np::HostConfig host_cfg;
	host_cfg.config.server_name = "OpenNova nw-server";
	host_cfg.config.max_players = 16;
	host_cfg.socket_mode = np::SocketMode::Lan;
	host_cfg.serve_and_play = false;
	np::start_host_session(owner, host_cfg); // the SAME §5.0 bring-up apps/nw_server runs

	// ---- joiner: a headless ClientRuntime over the joiner socket ----
	const std::string kName = "SocketJoiner";
	np::ClientRuntime client(kName);

	// ---- capture both directions at the joiner boundary (for the §5.2a order decode; the 0x42/0x82
	//      handshake is included so decode_capture_to_messages recovers the SCRK). ----
	std::vector<CaptureDatagram> recorded;
	int cap_seq = 0;
	auto record = [&](int src, int dst, const uint8_t *p, std::size_t n) {
		CaptureDatagram c;
		c.frame_index = cap_seq++;
		c.src_port = src;
		c.dst_port = dst;
		c.payload.assign(p, p + n);
		recorded.push_back(std::move(c));
	};
	auto ship_joiner = [&](const std::vector<uint8_t> &d) {
		if (d.empty()) return;
		net::udp_send_to(joiner_sock.get(), host_ep, d.data(), d.size());
		record(joiner_port, host_port, d.data(), d.size()); // C2S
	};
	auto drain_joiner = [&]() {
		uint8_t rx[4096];
		net::Endpoint from{};
		for (;;) {
			const int n = net::udp_recv_from(joiner_sock.get(), rx, sizeof(rx), from, /*timeout_ms=*/10);
			if (n <= 0) break;
			client.receive(rx, static_cast<std::size_t>(n));
			record(host_port, joiner_port, rx, static_cast<std::size_t>(n)); // S2C
		}
	};

	uint32_t tick = 1;

	// ---- Phase A: handshake + spawn-gate burst -> InMatch + deployed, all over UDP ----
	ship_joiner(client.start());
	bool ready = false;
	for (int f = 0; f < 400 && !ready; ++f) {
		np::host_session_pump(owner, host_dgram);
		drain_joiner();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship_joiner(d);
		++tick;
		ready = client.in_match() && client.deployed();
	}
	if (!expect(ready, "joiner reached InMatch + deployed over real UDP sockets")) {
		net::shutdown();
		return 1;
	}

	// The dedicated host binds only the joiner (type-1, keyed by its addr);
	// owned_entity was assigned by the automatic spawn pipeline.
	const PeerAddr jpeer = joiner_peer_addr(joiner_port);
	w::EntityHandle Hh{}, host_h{};
	for (np::NapiNPConnection &c : owner.ctx.np_protocol.connection_list) {
		if (c.type == 1 && c.peer == jpeer) Hh = c.link.owned_entity;
		else if (c.type == 2) host_h = c.link.owned_entity;
	}
	if (!expect(Hh.valid(), "joiner pool-0 entity spawned + owned_entity bound")) { net::shutdown(); return 1; }
	if (!expect(client.self_handle() == Hh.packed, "client H == the host's wire handle for the joiner")) {
		net::shutdown();
		return 1;
	}
	if (!expect(!host_h.valid(), "HostOnly creates no synthetic type-2 player")) {
		net::shutdown();
		return 1;
	}

	// ---- Phase B: in-match per-frame play — the 0x0C -> 0x0A round-trip over UDP ----
	PlayerExtendedUplink up;
	up.carrier_handle = 0xFFFF;
	up.pos_x = w::to_fixed(100.0);
	up.pos_y = w::to_fixed(200.0);
	up.pos_z = w::to_fixed(-50.0);
	up.heading = 0x2000;
	up.pitch = 0x0100;
	for (int f = 0; f < 12; ++f) {
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(up, tick)) ship_joiner(d);
		++tick;
		np::host_session_pump(owner, host_dgram);
		drain_joiner();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship_joiner(d); // fold + 0x2C
		++tick;
	}

	// ---- asserts (mirror client_runtime_test, over the wire) ----
	const w::Entity *je = world.registry.get(Hh);
	bool ok = true;
	ok = expect(je != nullptr, "joiner entity present") && ok;
	if (je) {
		ok = expect(je->position.x == static_cast<float>(w::from_fixed(up.pos_x)) &&
		                    je->position.y == static_cast<float>(w::from_fixed(up.pos_y)) &&
		                    je->position.z == static_cast<float>(w::from_fixed(up.pos_z)),
		            "joiner entity SNAPped to the C2S 0x0C uplink pose (delivered over UDP)") && ok;
	}
	if (je) {
		ok = expect(client.state().anchor_x == w::to_fixed(je->position.x) &&
		                    client.state().anchor_y == w::to_fixed(je->position.y) &&
		                    client.state().anchor_z == w::to_fixed(je->position.z),
		            "client ClientState anchor == joiner post-SNAP position (S2C 0x0A folded over UDP)") && ok;
	}
	ok = expect(client.state().frames_applied >= 1, "client folded >= 1 S2C 0x0A over the wire") && ok;
	if (!ok) { net::shutdown(); return 1; }

	// ---- §5.2a S2C order: decode the recorded stream and assert our host's burst order. ----
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(recorded);
	std::vector<uint8_t> ours_s2c;
	for (const InGameMessage &m : msgs) {
		if (m.dir != 'S' || m.settings_update) continue;
		if (m.session != joiner_port) continue;
		ours_s2c.push_back(static_cast<uint8_t>(m.tag & 0xFF));
	}
	std::printf("[two-endpoint] our host->joiner S2C tags (%zu):", ours_s2c.size());
	for (uint8_t t : ours_s2c) std::printf(" %02X", t);
	std::printf("\n");
	if (!expect(!ours_s2c.empty(), "decoded our host->joiner S2C stream (SCRK recovered from handshake)")) {
		net::shutdown();
		return 1;
	}
	// Order assertions only for tags our host actually emits (the §5.2a serializers 0x45/0x7E/0x1A are
	// deferred-as-nothing per the structural-P3 rule, so 0x1A is absent here — its order vs the retail
	// capture is the env-gated cross-check below). Observed: 1C 0B 11 | 10 0C 20 | 0C(named) 0A...
	bool order = true;
	order = expect(order_ok(ours_s2c, 0x0B, 0x10), "§5.2a: 0x0B (player-sync) precedes 0x10 world-stream") && order;
	order = expect(order_ok(ours_s2c, 0x0B, 0x0C), "§5.2a: 0x0B precedes 0x0C organic spawns") && order;
	order = expect(order_ok(ours_s2c, 0x10, 0x0C), "§5.2a: 0x10 static batch precedes 0x0C organics") && order;
	order = expect(order_ok(ours_s2c, 0x0C, 0x20), "§5.2a: 0x0C organics precede 0x20 pool-3 sync") && order;
	if (!order) { net::shutdown(); return 1; }

	// ---- env-gated cross-check vs the retail LAN host/join golden (order agreement on common tags). ----
	std::string path;
	if (const char *env = std::getenv("NW_GOLDEN_LAN_JOIN"); env && *env) path = env;
	else path = DEFAULT_LAN_JOIN_PCAP;
	std::vector<net::PcapDatagram> pkts;
	if (path.empty() || !net::read_pcap_udp_file(path, pkts)) {
		std::printf("[skip] retail golden absent (set NW_GOLDEN_LAN_JOIN) — '%s'; our §5.2a order asserted above\n",
		            path.c_str());
		net::shutdown();
		std::printf("OK\n");
		return 0;
	}
	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (const net::PcapDatagram &p : pkts) {
		CaptureDatagram c;
		c.frame_index = p.frame_index;
		c.src_port = p.srcport;
		c.dst_port = p.dstport;
		c.payload = p.payload;
		caps.push_back(std::move(c));
	}
	std::vector<uint8_t> retail_s2c;
	for (const InGameMessage &m : decode_capture_to_messages(caps)) {
		if (m.dir != 'S' || m.settings_update) continue;
		retail_s2c.push_back(static_cast<uint8_t>(m.tag & 0xFF));
	}
	// Each §5.2a order relation we assert on our stream must also hold (or be absent) in the retail
	// stream — i.e. the retail capture never CONTRADICTS our order on the common world-stream tags.
	const std::pair<uint8_t, uint8_t> rels[] = {{0x0B, 0x10}, {0x0B, 0x0C}, {0x10, 0x0C}, {0x0C, 0x20}, {0x20, 0x1A}};
	bool agree = true;
	for (const auto &r : rels) {
		const int a = first_of(retail_s2c, r.first), b = first_of(retail_s2c, r.second);
		const bool contradicts = (a >= 0 && b >= 0 && a > b); // both present but in the WRONG order
		agree = expect(!contradicts, "retail golden S2C order agrees with §5.2a (no contradiction)") && agree;
	}
	if (!agree) { net::shutdown(); return 1; }
	std::printf("[two-endpoint] retail golden S2C order agrees with our §5.2a order on the common tags\n");

	net::shutdown();
	std::printf("OK\n");
	return 0;
}
