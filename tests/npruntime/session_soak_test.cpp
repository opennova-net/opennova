// The NP session layer soaked over an impaired link.
//
// A headless ClientRuntime joiner and the shared host owner loop (host_session_pump, the loop the
// game's host runs) exchange whole datagrams over an in-memory wire, one 16 ms tick at a time on a
// virtual clock. Each direction passes through ImpairedDatagramSocket
// (tests/npruntime/impaired_datagram_socket.h): seeded loss, duplication, reordering and a 100 ms
// one-way delay, with the host dictating a NovaWorld host's 12-tick send holdoff. Each case joins,
// deploys, plays for three minutes, heals the link and drains, then asserts what retail's session
// layer guarantees:
//   - the session survives (no reap, no punt) and stays in order: the joiner's inbound frontier
//     never regresses and the 0x0A stream keeps landing;
//   - every gap is recovered: after the heal both ordered queues are empty and the retained
//     reliable records are retired by the peers' ACKs;
//   - (the 2 % case) no gap stalls longer than retail's cadence allows. A gap opens when a later
//     packet arrives; the receiver sends its missing-sequence request from that receive pump, the
//     sender rebuilds the packet from its own receive pump, and the rebuild crosses the link. Each
//     lost request or rebuild costs one more send period until the next later packet re-arms the
//     request. [orig: NapiNPProtocol_PumpRecvQueues @0x6266a0 tail -> SendMissingSeqList
//     @0x6269ce; NapiNP_HandleResendList -> SendSessionPacket @0x6239b6; the server receive pump
//     every Server_TickUpdate @0x51d895; the client send gate @0x42c3dd]
// The decorator itself is pinned first: same seed, same schedule; delivery in (due, send) order.

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>

#include <net/npwire/idatagram_socket.h>
#include "impaired_datagram_socket.h"
#include <net/npwire/ingame_decode.h>

#include <base/io/tick_rate.h>

#include <formats/mission/bms.h>

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// One end of an in-memory datagram wire: send_to lands in the peer endpoint's inbox, tagged with
// this endpoint's address; recv_from drains this endpoint's inbox in arrival order.
class WireEndpoint final : public IDatagramSocket {
public:
	explicit WireEndpoint(PeerAddr self) : self_(self) {}
	void connect(WireEndpoint *peer) { peer_ = peer; }

	int recv_from(uint8_t *buf, std::size_t cap, PeerAddr &from) override {
		if (inbox_.empty()) return 0;
		std::pair<PeerAddr, std::vector<uint8_t>> datagram = std::move(inbox_.front());
		inbox_.pop_front();
		if (datagram.second.size() > cap) return -1;
		from = datagram.first;
		std::memcpy(buf, datagram.second.data(), datagram.second.size());
		return static_cast<int>(datagram.second.size());
	}
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t len) override {
		if (peer_ != nullptr) peer_->inbox_.emplace_back(self_, std::vector<uint8_t>(data, data + len));
	}

private:
	PeerAddr self_{};
	WireEndpoint *peer_ = nullptr;
	std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> inbox_;
};

constexpr PeerAddr kHostAddr{0x0200000Au, 9200};
constexpr PeerAddr kJoinerAddr{0x0300000Au, 51234};

// ---- the decorator alone ----

bool check_impairment_is_deterministic_and_ordered() {
	uint64_t now_ms = 0;
	auto clock = [&]() { return now_ms; };
	DatagramImpairment profile;
	profile.loss_ppm = 100000;      // 10 %
	profile.duplicate_ppm = 50000;  // 5 %
	profile.reorder_ppm = 100000;   // 10 %
	profile.delay_ms = 40;
	profile.jitter_ms = 8;
	profile.reorder_hold_ms = 30;
	profile.duplicate_gap_ms = 3;
	profile.seed = 0xC0FFEEull;

	struct Run {
		std::vector<uint32_t> order;
		DatagramImpairmentStats stats{};
		bool drained = false;
	};
	auto run = [&]() {
		Run out;
		now_ms = 0;
		auto sink = std::make_unique<WireEndpoint>(kHostAddr);
		auto source = std::make_unique<WireEndpoint>(kJoinerAddr);
		WireEndpoint *sink_raw = sink.get();
		source->connect(sink_raw);
		ImpairedDatagramSocket link(std::move(source), profile, clock);
		for (uint32_t i = 0; i < 2000; ++i) {
			const uint8_t bytes[4] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), 0, 0};
			link.send_to(kHostAddr, bytes, sizeof(bytes));
			now_ms += 1;
			link.release_due();
		}
		now_ms += 1000;
		link.release_due();
		uint8_t buf[16];
		PeerAddr from{};
		while (sink_raw->recv_from(buf, sizeof(buf), from) > 0)
			out.order.push_back(static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8));
		out.stats = link.stats();
		out.drained = link.in_flight() == 0;
		return out;
	};
	const Run first = run();
	const Run second = run();
	if (!expect(first.drained && second.drained,
	            "every scheduled datagram is delivered once the clock passes its due time"))
		return false;
	if (!expect(first.order == second.order, "the same seed yields the same delivery schedule"))
		return false;
	if (!expect(first.stats.delivered == first.order.size() &&
	                    first.stats.delivered ==
	                            first.stats.sent - first.stats.dropped + first.stats.duplicated,
	            "delivered = sent - dropped + duplicated"))
		return false;
	// 2000 draws at 10 % / 5 % / 10 %: each count sits well inside a 4-sigma band.
	if (!expect(first.stats.dropped > 120 && first.stats.dropped < 280 &&
	                    first.stats.duplicated > 50 && first.stats.duplicated < 140 &&
	                    first.stats.reordered > 110 && first.stats.reordered < 260,
	            "loss, duplication and reordering occur near their configured rates"))
		return false;
	// Jitter alone keeps FIFO order: only the held-back datagrams (and the late duplicates)
	// arrive behind a later one.
	std::size_t inversions = 0;
	for (std::size_t i = 1; i < first.order.size(); ++i)
		if (first.order[i] < first.order[i - 1]) ++inversions;
	return expect(inversions > 0 &&
	                      inversions <= first.stats.reordered + first.stats.duplicated,
	              "only held-back datagrams and duplicates arrive behind later ones");
}

// ---- the soak ----

struct GapClock {
	uint32_t quick_ticks = 0; // the loss-free recovery bound (see run_soak's cadence comment)
	uint32_t open_since = 0;  // tick the current stall began (0 = no gap)
	uint32_t worst_ticks = 0; // longest stall seen
	uint64_t total_ticks = 0; // summed stall length
	uint32_t stalls = 0;      // stalls closed
	uint32_t slow = 0;        // stalls longer than quick_ticks

	void observe(bool gap, uint32_t tick) {
		if (gap) {
			if (open_since == 0) open_since = tick;
			return;
		}
		if (open_since == 0) return;
		const uint32_t length = tick - open_since;
		worst_ticks = std::max(worst_ticks, length);
		total_ticks += length;
		++stalls;
		if (length > quick_ticks) ++slow;
		open_since = 0;
	}
	double mean_ms() const {
		return stalls ? static_cast<double>(total_ticks) * io::kTickMs / stalls : 0.0;
	}
};

struct SoakCase {
	const char *name;
	DatagramImpairment link; // the S2C profile; C2S takes the same with its own seed
	uint64_t c2s_seed;
	bool check_cadence;      // assert the per-gap recovery cadence (meaningful at low loss)
	uint32_t min_frames;     // 0x0A frames that must land in the three minutes of play
};

bool run_soak(const SoakCase &soak) {
	// ---- host: a minimal World (one 6002 start marker) + an in-memory mission ----
	w::World world;
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

	inmatch::HostOwner owner;
	owner.ctx.world = &world;
	owner.ctx.mission = &mission;
	inmatch::HostConfig host_cfg;
	host_cfg.config.server_name = "OpenNova Soak";
	host_cfg.config.max_players = 16;
	// A NovaWorld host's dictated period: one send boundary every 12 ticks (~194 ms).
	constexpr uint32_t kHoldoffTicks = 12;
	host_cfg.config.send_holdoff_ticks = kHoldoffTicks;
	host_cfg.socket_mode = inmatch::SocketMode::Lan;
	host_cfg.serve_and_play = false;
	inmatch::start_host_session(owner, host_cfg);

	// ---- the impaired wire ----
	uint64_t now_ms = 0;
	auto clock = [&]() { return now_ms; };
	auto host_end = std::make_unique<WireEndpoint>(kHostAddr);
	auto joiner_end = std::make_unique<WireEndpoint>(kJoinerAddr);
	host_end->connect(joiner_end.get());
	joiner_end->connect(host_end.get());
	const DatagramImpairment &s2c = soak.link;
	DatagramImpairment c2s = soak.link;
	c2s.seed = soak.c2s_seed;
	ImpairedDatagramSocket host_sock(std::move(host_end), s2c, clock);
	ImpairedDatagramSocket joiner_sock(std::move(joiner_end), c2s, clock);

	inmatch::ClientRuntime client("SoakJoiner", clock);

	uint32_t tick = 1;
	auto drain_joiner = [&]() {
		uint8_t rx[8192];
		PeerAddr from{};
		int n = 0;
		while ((n = joiner_sock.recv_from(rx, sizeof(rx), from)) > 0)
			client.receive(rx, static_cast<std::size_t>(n));
	};
	auto ship = [&](std::vector<std::vector<uint8_t>> datagrams) {
		for (const std::vector<uint8_t> &d : datagrams)
			if (!d.empty()) joiner_sock.send_to(kHostAddr, d.data(), d.size());
	};
	auto host_connection = [&]() -> const inmatch::NapiNPConnection * {
		for (const inmatch::NapiNPConnection &c : owner.ctx.np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeServerSide && c.peer == kJoinerAddr) return &c;
		return nullptr;
	};

	PlayerExtendedUplink up;
	up.carrier_handle = 0xFFFF;
	up.pos_x = w::to_fixed(52.0);
	up.pos_y = w::to_fixed(61.0);
	up.pos_z = w::to_fixed(1.0);
	up.heading = 0x2000;

	// The cadence. A gap opens when a later packet lands; the receiver's missing-sequence request
	// leaves from that receive pump, crosses the link, the sender rebuilds the packet from its own
	// next receive pump and the rebuild crosses back: at most one tick on each side plus two
	// worst-case one-way trips (delay + jitter + reorder hold). That is the loss-free recovery.
	// A request or rebuild lost on the link costs one more send period, until the next later
	// packet re-arms the request.
	const double tick_ms = static_cast<double>(io::kTickMs);
	const uint32_t one_way_ms = s2c.delay_ms + s2c.jitter_ms + s2c.reorder_hold_ms;
	const uint32_t quick_ticks =
			2u + (2u * one_way_ms + static_cast<uint32_t>(io::kTickMs) - 1u) /
					static_cast<uint32_t>(io::kTickMs);
	GapClock joiner_gaps;
	GapClock host_gaps;
	uint32_t last_frontier = 0;
	bool frontier_regressed = false;
	auto step = [&](bool in_play) {
		now_ms += static_cast<uint64_t>(io::kTickMs);
		host_sock.release_due();
		joiner_sock.release_due();
		inmatch::host_session_pump(owner, host_sock);
		drain_joiner();
		if (in_play) {
			up.pos_x += w::to_fixed(0.01);
			ship(client.Client_ProcessNetworkFrame(up, tick));
		} else {
			ship(client.Client_ProcessNetworkFrame(tick));
		}
		const uint32_t frontier = client.inbound_frontier_seq();
		if (frontier < last_frontier) frontier_regressed = true;
		last_frontier = frontier;
		joiner_gaps.observe(client.inbound_gap_depth() != 0, tick);
		const inmatch::NapiNPConnection *hc = host_connection();
		host_gaps.observe(hc != nullptr && !hc->seq.queued_inbound.empty(), tick);
		++tick;
	};

	// Phase A: handshake, admission, world stream and deployment, all over the impaired link. A
	// lost admission packet with nothing behind it waits for the sender's 10 s active probe, as
	// on retail, so the join gets a generous window.
	ship({client.start()});
	bool ready = false;
	for (int f = 0; f < 62 * 120 && !ready; ++f) {
		step(false);
		ready = client.in_match() && client.is_deployed();
	}
	std::printf("[soak %s] joined=%d at tick %u\n", soak.name, ready ? 1 : 0, tick);
	if (!expect(ready, "the joiner reaches the match and deploys over the impaired link")) {
		std::fprintf(stderr, "  stage=%s lost=%d reason=%s\n", client.admission_stage_name(),
				client.session_lost() ? 1 : 0, client.session_loss_reason().c_str());
		return false;
	}
	const uint32_t frames_at_join = client.state().frames_applied;
	// Phase A stalls include the admission exchange's own pacing; measure play only.
	joiner_gaps = GapClock{};
	host_gaps = GapClock{};
	joiner_gaps.quick_ticks = quick_ticks;
	host_gaps.quick_ticks = quick_ticks;

	// Phase B: three minutes of play under the impairment (930 send boundaries each way).
	constexpr int kPlayTicks = 62 * 180;
	for (int f = 0; f < kPlayTicks; ++f) step(true);
	const uint32_t frames_in_play = client.state().frames_applied - frames_at_join;

	// Phase C: heal the link (keep the latency) and drain.
	DatagramImpairment healed = s2c;
	healed.loss_ppm = 0;
	healed.duplicate_ppm = 0;
	healed.reorder_ppm = 0;
	healed.jitter_ms = 0;
	host_sock.set_profile(healed);
	joiner_sock.set_profile(healed);
	for (int f = 0; f < 62 * 4; ++f) step(true);

	const inmatch::NapiNPConnection *hc = host_connection();
	std::printf("[soak %s] s2c sent=%llu dropped=%llu dup=%llu reorder=%llu;"
	            " c2s sent=%llu dropped=%llu dup=%llu reorder=%llu\n",
			soak.name,
			static_cast<unsigned long long>(host_sock.stats().sent),
			static_cast<unsigned long long>(host_sock.stats().dropped),
			static_cast<unsigned long long>(host_sock.stats().duplicated),
			static_cast<unsigned long long>(host_sock.stats().reordered),
			static_cast<unsigned long long>(joiner_sock.stats().sent),
			static_cast<unsigned long long>(joiner_sock.stats().dropped),
			static_cast<unsigned long long>(joiner_sock.stats().duplicated),
			static_cast<unsigned long long>(joiner_sock.stats().reordered));
	std::printf("[soak %s] loss-free recovery %u ticks; joiner stalls=%u (slow %u) worst=%.0f ms"
	            " mean=%.1f ms; host stalls=%u (slow %u) worst=%.0f ms mean=%.1f ms;"
	            " 0x0A frames in play=%u\n",
			soak.name, quick_ticks, joiner_gaps.stalls, joiner_gaps.slow,
			joiner_gaps.worst_ticks * tick_ms, joiner_gaps.mean_ms(), host_gaps.stalls,
			host_gaps.slow, host_gaps.worst_ticks * tick_ms, host_gaps.mean_ms(), frames_in_play);

	bool ok = true;
	ok = expect(!client.session_lost() && client.in_match() && hc != nullptr,
	            "the session survives three minutes of impairment") && ok;
	ok = expect(!frontier_regressed, "the joiner's inbound frontier never regresses") && ok;
	ok = expect(frames_in_play >= soak.min_frames,
	            "the 0x0A stream keeps landing through the impairment") && ok;
	ok = expect(joiner_gaps.stalls > 0 && host_gaps.stalls > 0,
	            "the impairment opened gaps in both directions") && ok;
	ok = expect(client.inbound_gap_depth() == 0 && hc != nullptr && hc->seq.queued_inbound.empty(),
	            "every gap is recovered once the link heals") && ok;
	ok = expect(client.retained_outbound_depth() == 0,
	            "the host's ACKs retire every reliable C2S record") && ok;
	ok = expect(hc != nullptr && hc->seq.retained_outbound_message_count < 16,
	            "the joiner's ACKs retire the host's reliable S2C records") && ok;
	if (!soak.check_cadence) return ok;

	// No stall outlasts the loss-free recovery by more than two send periods (two consecutive
	// losses in one recovery chain are the deepest a 2 % link produces in this seeded run), and
	// only the recoveries that lost a request or a rebuild outlast it at all: at 2 % loss in each
	// direction that is a few percent of the gaps, so a quarter is a wide margin. A recovery that
	// waited for a send boundary instead of a receive pump (D-NET-226 / D-NET-229) misses the
	// loss-free bound for most gaps.
	const double worst_bound_ms = (quick_ticks + 2u * kHoldoffTicks) * tick_ms;
	ok = expect(joiner_gaps.worst_ticks * tick_ms <= worst_bound_ms,
	            "no S2C gap stalls the joiner longer than retail's recovery cadence allows") && ok;
	ok = expect(host_gaps.worst_ticks * tick_ms <= worst_bound_ms,
	            "no C2S gap stalls the host longer than retail's recovery cadence allows") && ok;
	ok = expect(joiner_gaps.slow * 4u <= joiner_gaps.stalls,
	            "S2C gaps recover at the receive-pump cadence") && ok;
	ok = expect(host_gaps.slow * 4u <= host_gaps.stalls,
	            "C2S gaps recover at the receive-pump cadence") && ok;
	return ok;
}

DatagramImpairment link_profile(uint32_t loss_ppm, uint32_t duplicate_ppm, uint32_t reorder_ppm,
		uint32_t reorder_hold_ms, uint64_t seed) {
	DatagramImpairment link;
	link.loss_ppm = loss_ppm;
	link.duplicate_ppm = duplicate_ppm;
	link.reorder_ppm = reorder_ppm;
	link.delay_ms = 100;
	link.jitter_ms = 10;
	link.reorder_hold_ms = reorder_hold_ms;
	link.duplicate_gap_ms = 5;
	link.seed = seed;
	return link;
}

} // namespace

int main() {
	bool ok = check_impairment_is_deterministic_and_ordered();
	// A lossy internet link: 2 % loss, 0.5 % duplication, 2 % of datagrams held back 40 ms.
	// One 0x0A per 12-tick boundary is 930 in three minutes; at 2 % loss most still land.
	ok = run_soak({"lossy", link_profile(20000, 5000, 20000, 40, 0x5EED5EEDull), 0xC25C25ull,
	                 /*check_cadence=*/true, /*min_frames=*/870}) && ok;
	// A bad one: 8 % loss, 8 % duplication, 8 % held back half a second. The session must still
	// hold together and recover every gap; the cadence bound does not apply at this loss.
	ok = run_soak({"harsh", link_profile(80000, 80000, 80000, 500, 0x0BADC0DEull), 0x0DDBA11ull,
	                 /*check_cadence=*/false, /*min_frames=*/780}) && ok;
	if (ok) std::printf("OK\n");
	return ok ? 0 : 1;
}
