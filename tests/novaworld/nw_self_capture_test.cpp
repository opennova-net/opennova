// Self-capture coverage diff — nw_golden_diff's SELF MODE, and the second NET-0
// tier-1 gate next to nw_codec_identity (docs/maturity-program.md).
//
// Runs a deterministic in-process opennova host + opennova joiner session — the
// SAME host owner loop apps/nw_server runs (npruntime/host_session.h) driving the
// SAME headless joiner npruntime_two_endpoint_socket drives (np::ClientRuntime),
// composed over the public IDatagramSocket seam with an in-memory adapter instead
// of a bound socket (no timing, no ephemeral ports) — tees every datagram BOTH
// directions into npwire's CaptureDatagram form, decodes through the SAME shared
// pipeline the retail diff uses (decode_capture_to_messages: envelope CRC -> NWU
// -> per-session SCRK -> 0x43/0x83 -> reassembly), and compares the live message
// coverage per (direction, wire tag) against the COMMITTED opennova-produced
// fixture (fixtures/novaworld/self-capture-session.pcap, LFS).
//
// Unlike the retail mode there is NO noise floor and NO allowlist: both sides of
// the diff are ours and the session script is fixed, so the comparison is EXACT
// set equality in both directions. A GAP (the fixture carries a tag the live
// session no longer emits) or a SPURIOUS tag (the live session emits a tag the
// fixture doesn't) fails the build. This is the gate the codec identity vectors
// cannot be: the vectors pin each encoder's BYTES in isolation; this pins WHICH
// messages an actual join + play session puts on the wire — a lost burst stage,
// a dropped housekeeping send, or a spuriously-added tag flips it red even when
// every individual encoder still hashes clean.
//
// Gating: NONE — tier 1, default CI, cannot skip. The fixture contains only
// opennova-produced bytes (zero retail material), so it is committable; a
// missing/unreadable fixture is a hard FAIL, not a skip.
//
// Regenerating the fixture is a WIRE-COVERAGE CHANGE: run with
// OPENNOVA_WRITE_SELF_FIXTURE=1 to rewrite it (the write path re-reads and
// re-verifies the file before reporting OK), and justify the coverage delta in
// the same commit (the newly implemented tag, or its D-NET entry) — never
// regenerate to make an unexplained diff pass.

#include <npruntime/client_runtime.h>
#include <npruntime/host_session.h> // the host owner loop (SAME loop apps/nw_server runs)

#include <netsim/idatagram_socket.h>
#include <netsim/loopback_channel.h>

#include <npwire/ingame_decode.h>          // PlayerExtendedUplink (the §5.10 0x0C body)
#include <npwire/ingame_message_catalog.h> // ingame_message_name
#include <npwire/wire_capture.h>           // CaptureDatagram / decode_capture_to_messages

#include <mission/bms.h>

#include <world/ai.h>
#include <world/entity.h>
#include <world/geom.h>
#include <world/world.h>

#include <pcapio/pcap_reader.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef SELF_CAPTURE_FIXTURE
#define SELF_CAPTURE_FIXTURE ""
#endif

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
namespace w = opennova::world;

namespace {

using Key = std::pair<char, uint8_t>; // (dir, low-byte tag)

// Fixed loopback endpoints — the fixture's ports and the live session's ports are
// the same constants, so the decoder's per-session keying is identical for both.
constexpr uint16_t kHostPort = 32768;
constexpr uint16_t kJoinerPort = 30000;
const PeerAddr kJoinerPeer{0x0100007Fu, kJoinerPort}; // 127.0.0.1:30000

// In-memory netsim::IDatagramSocket — the same adapter seam apps/common's
// NetDatagramSocket (real socket) and the Godot NovaUdpPump wrapper fill, backed
// by a deque: the joiner "sends" by pushing onto `inbound`; the host's send_to
// lands in `outbound` for the test loop to record + deliver. Keeps the tier-1
// gate free of real sockets (deterministic, port-fixed, no recv timeouts).
struct MemoryDatagramSocket : ns::IDatagramSocket {
	std::deque<std::vector<uint8_t>> inbound;                 // joiner -> host (all from kJoinerPeer)
	std::vector<std::vector<uint8_t>> outbound;               // host -> joiner, drained per pump

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbound.empty()) return 0;
		const std::vector<uint8_t> d = std::move(inbound.front());
		inbound.pop_front();
		if (d.size() > cap) return -1;
		std::copy(d.begin(), d.end(), buf);
		from = kJoinerPeer;
		return static_cast<int>(d.size());
	}
	void send_to(const PeerAddr & /*to*/, const uint8_t *data, std::size_t len) override {
		outbound.emplace_back(data, data + len); // single-peer session: always the joiner
	}
};

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// The scripted session: a fixed-config in-process host + a fixed-name joiner run
// join -> InMatch/deployed -> 12 in-match frames (the npruntime_two_endpoint_socket
// script minus the real sockets). Every datagram both directions is teed into
// `recorded` in wire order. Returns false if the session never reaches in-match
// (a degenerate capture must never be compared or written).
bool run_session(std::vector<CaptureDatagram> &recorded) {
	// Host World — mirrors npruntime_initial_state_burst: a 6002 start marker
	// (spawn-select + a 0x20 pool-3 record) and a pool-2 building so the 0x10
	// static page carries a real record; a minimal in-memory 0x0B BMS mission.
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(2, 16);
	world.registry.configure_pool(3, 16);
	{
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {50.0f, 60.0f, 1.0f};
		start.yaw = 0;
		world.registry.spawn(3, start);
	}
	{
		w::Entity bld;
		bld.kind = w::EntityKind::Building;
		bld.item_id = 0x044c;
		bld.position = {-396.6f, 360.1f, 11.2f};
		bld.yaw = 0;
		bld.engine_flags = 0x04020400u;
		bld.ammo_count = 0xFF;
		bld.sub_type = 0xFF;
		world.registry.spawn(2, bld);
	}
	bms::File mission;
	mission.header.magic[0] = 'B';
	mission.header.magic[1] = 'M';
	mission.header.magic[2] = 'S';
	mission.header.magic[3] = static_cast<char>(bms::kMinVersion);

	// Host bring-up — fixed identity end to end (GameConfig defaults are fixed
	// strings/values; the host key is pinned) so the coverage never depends on
	// anything minted at run time. Headless Listen host, same as apps/nw_server.
	np::HostOwner owner;
	ns::LoopbackChannel host_loop;
	owner.host_loopback = &host_loop;
	owner.ctx.world = &world;
	owner.ctx.mission = &mission;
	np::HostConfig host_cfg;
	host_cfg.config.server_name = "OpenNova self-capture host";
	host_cfg.config.max_players = 16;
	host_cfg.socket_mode = np::SocketMode::Lan;
	host_cfg.host_key = 0x0FE0E112u; // deterministic (HostConfig: "deterministic for tests")
	host_cfg.serve_and_play = false;
	np::start_host_session(owner, host_cfg);

	MemoryDatagramSocket sock;
	np::ClientRuntime client("SelfCapture");

	int cap_seq = 0;
	auto record = [&](int src, int dst, const std::vector<uint8_t> &d) {
		CaptureDatagram c;
		c.frame_index = ++cap_seq; // 1-based, matching pcap capture order
		c.src_port = src;
		c.dst_port = dst;
		c.payload = d;
		recorded.push_back(std::move(c));
	};
	auto ship_joiner = [&](const std::vector<uint8_t> &d) {
		if (d.empty()) return;
		record(kJoinerPort, kHostPort, d); // C2S
		sock.inbound.push_back(d);
	};
	auto pump_and_deliver = [&]() {
		np::host_session_pump(owner, sock);
		for (const std::vector<uint8_t> &d : sock.outbound) {
			record(kHostPort, kJoinerPort, d); // S2C
			client.receive(d.data(), d.size());
		}
		sock.outbound.clear();
	};

	uint32_t tick = 1;

	// Phase A: handshake + the retail post-auth mission exchange + the §5.2a spawn-gate burst ->
	// InMatch + deployed. The fixture deliberately covers the newly retained 0x01 -> 0x02 -> 0x7B
	// flow and ServerAuth initialization tags instead of the old abbreviated self-session.
	ship_joiner(client.start());
	bool ready = false;
	for (int f = 0; f < 400 && !ready; ++f) {
		pump_and_deliver();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship_joiner(d);
		++tick;
		ready = client.in_match() && client.deployed();
	}
	if (!expect(ready, "joiner reached InMatch + deployed over the in-memory socket")) return false;

	// Phase B: 12 in-match frames — the C2S 0x0C uplink -> S2C 0x0A round-trip
	// (+ the §5.44 0x2C RTT housekeeping each deployed frame).
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
		pump_and_deliver();
		for (std::vector<uint8_t> &d : client.Client_ProcessNetworkFrame(tick)) ship_joiner(d); // fold + 0x2C
		++tick;
	}
	return true;
}

std::map<Key, long> histogram(const std::vector<CaptureDatagram> &caps) {
	std::map<Key, long> h;
	for (const InGameMessage &m : decode_capture_to_messages(caps))
		h[{m.dir, static_cast<uint8_t>(m.tag & 0xFF)}] += 1;
	return h;
}

std::map<Key, long> histogram(const std::vector<net::PcapDatagram> &pkts) {
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
	return histogram(caps);
}

const char *name_of(Key k) {
	const char *n = ingame_message_name(k.first, k.second);
	return n ? n : "?";
}

// EXACT set equality both directions: every fixture key must be live (else GAP)
// and every live key must be in the fixture (else SPURIOUS) — C2S and S2C alike,
// since both endpoints are ours. Prints the full table either way.
int compare_coverage(const std::map<Key, long> &live, const std::map<Key, long> &fixture) {
	std::set<Key> keys;
	for (const auto &kv : live) keys.insert(kv.first);
	for (const auto &kv : fixture) keys.insert(kv.first);

	std::printf("%-3s %-6s %-22s %8s %8s  %s\n", "dir", "tag", "name", "live", "fixture",
	            "status");
	int gaps = 0, spurious = 0;
	for (Key k : keys) {
		const long lc = live.count(k) ? live.at(k) : 0;
		const long fc = fixture.count(k) ? fixture.at(k) : 0;
		const char *status = "OK";
		if (lc == 0) {
			status = "GAP";
			++gaps;
		} else if (fc == 0) {
			status = "SPURIOUS";
			++spurious;
		}
		char tagbuf[8];
		std::snprintf(tagbuf, sizeof(tagbuf), "0x%02x", k.second);
		std::printf("%-3c %-6s %-22s %8ld %8ld  %s\n", k.first, tagbuf, name_of(k), lc, fc,
		            status);
	}

	std::printf("\nGAPS (fixture has it, live session doesn't): %d\n", gaps);
	std::printf("SPURIOUS (live session has it, fixture doesn't): %d\n", spurious);
	return gaps + spurious;
}

} // namespace

int main() {
	std::vector<CaptureDatagram> recorded;
	if (!run_session(recorded)) return 1;

	const std::map<Key, long> live = histogram(recorded);
	if (live.empty()) {
		std::printf("FAIL: the live session decoded to zero messages (handshake missing — "
		            "SCRK unrecoverable)\n");
		return 1;
	}

	const std::string fixture_path = SELF_CAPTURE_FIXTURE;

	// Regen mode: rewrite the committed fixture from this run, then re-read and
	// re-verify it so a bad write can never be committed green.
	if (const char *regen = std::getenv("OPENNOVA_WRITE_SELF_FIXTURE"); regen && *regen) {
		std::vector<net::PcapDatagram> dgrams;
		dgrams.reserve(recorded.size());
		for (const CaptureDatagram &c : recorded) {
			net::PcapDatagram p;
			p.srcport = c.src_port;
			p.dstport = c.dst_port;
			p.frame_index = c.frame_index;
			// Deterministic timestamps: 16 ms (the 62 Hz frame) per capture record,
			// so regen output doesn't churn on wall clock.
			p.ts_nanos = static_cast<uint64_t>(c.frame_index) * 16000000ull;
			p.payload = c.payload;
			dgrams.push_back(std::move(p));
		}
		const std::vector<uint8_t> pcap = net::build_pcap_udp(dgrams);
		std::FILE *f = std::fopen(fixture_path.c_str(), "wb");
		if (!f) {
			std::printf("FAIL: cannot open '%s' for writing\n", fixture_path.c_str());
			return 1;
		}
		std::fwrite(pcap.data(), 1, pcap.size(), f);
		std::fclose(f);
		std::printf("wrote %zu datagrams (%zu bytes) to %s\n", dgrams.size(), pcap.size(),
		            fixture_path.c_str());

		std::vector<net::PcapDatagram> reread;
		if (!net::read_pcap_udp_file(fixture_path, reread)) {
			std::printf("FAIL: the freshly written fixture does not read back\n");
			return 1;
		}
		if (compare_coverage(live, histogram(reread)) != 0) {
			std::printf("FAIL: the freshly written fixture does not reproduce the live "
			            "coverage\n");
			return 1;
		}
		std::printf("OK — fixture regenerated and re-verified. Justify the coverage delta "
		            "in the same commit.\n");
		return 0;
	}

	// Normal mode: tier 1, cannot skip — a missing fixture is a failure.
	std::vector<net::PcapDatagram> fixture_pkts;
	if (fixture_path.empty() || !net::read_pcap_udp_file(fixture_path, fixture_pkts)) {
		std::printf("FAIL: committed fixture missing/unreadable — '%s'. If it is an LFS "
		            "pointer, run: git lfs pull --include=\"fixtures/novaworld/**\". To "
		            "(re)generate: OPENNOVA_WRITE_SELF_FIXTURE=1 %s\n",
		            fixture_path.c_str(), "nw_self_capture_test");
		return 1;
	}
	const std::map<Key, long> fixture = histogram(fixture_pkts);
	if (fixture.empty()) {
		std::printf("FAIL: fixture decoded to zero messages — regenerate it "
		            "(OPENNOVA_WRITE_SELF_FIXTURE=1)\n");
		return 1;
	}
	// Tripwire: the tag-SET gate cannot see a shrinking fixture — a regen that drops
	// half the session still carries every tag once. Pin a decoded-message floor and a
	// datagram floor so a silent fixture shrink fails loudly; raise them deliberately
	// (with the coverage-delta justification) when the driven session legitimately grows.
	// The DATAGRAM floor was lowered 100 -> 85 on 2026-07-25. The datagram count is the
	// weaker of the two signals: it counts envelopes, and this branch's ACK/pacing fixes
	// legitimately removed ~47 content-free header-only datagrams from the driven session
	// (139 -> 92) with the decoded-message content unchanged. That shrink was invisible
	// until a tag delta forced the first regen since. The MESSAGE floor is the real
	// content tripwire and stays where it is.
	long fixture_total_messages = 0;
	for (const auto &kv : fixture) fixture_total_messages += kv.second;
	if (fixture_pkts.size() < 85 || fixture_total_messages < 120) {
		std::printf("FAIL: committed fixture shrank below the pinned floor (%zu datagrams, "
		            "%ld decoded messages; floors 85/120) — a regen dropped part of the "
		            "session\n",
		            fixture_pkts.size(), fixture_total_messages);
		return 1;
	}

	if (compare_coverage(live, fixture) != 0) {
		std::printf("FAIL: live opennova<->opennova coverage diverges from the committed "
		            "self-capture fixture — see GAP/SPURIOUS rows above. If the change is "
		            "intentional (a tag implemented or retired), regenerate with "
		            "OPENNOVA_WRITE_SELF_FIXTURE=1 and justify the delta in the same "
		            "commit; otherwise fix the regression.\n");
		return 1;
	}
	std::printf("OK\n");
	return 0;
}
