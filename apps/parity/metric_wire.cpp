// The wire metrics: the per-direction tag histogram, the steady gameplay
// distributions, the session packet envelope, the outer handshake, the
// request/reply checks (C2S 0x3D pages, the RTT echo), the integrity stream, the
// session milestones, bandwidth, gameplay cadence and the scoreboard's clock (the
// initial reliable stream is metric_initial.cpp). Rules and tolerances are the
// retired verify_parity_matrix.ps1's (git show
// 5820432c1^:scripts/net/verify_parity_matrix.ps1), recast as distances.

#include "metric.h"

#include "normalization.h"

#include <base/io/tick_rate.h>
#include <net/npwire/flat_tlv.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace opennova::parity {

namespace {

// Bodies stable across a same-case matrix that drive team/deployment/loadout
// behavior: their exact observed sets compare, not merely tags and sizes.
constexpr uint16_t kStableC[] = { 0x00A, 0x00E, 0x02F, 0x047 };
constexpr uint16_t kStableS[] = { 0x100, 0x004, 0x008, 0x00B, 0x00F, 0x011, 0x01C, 0x02A, 0x02C,
	0x05A, 0x060, 0x066, 0x075, 0x076, 0x07B };

bool stable_identity(char dir, uint16_t identity) {
	if (dir == 'C')
		return std::find(std::begin(kStableC), std::end(kStableC), identity) != std::end(kStableC);
	return std::find(std::begin(kStableS), std::end(kStableS), identity) != std::end(kStableS);
}

// The reload round trip: the joiner's C2S 0x25 request and the host's S2C 0x49 echo
// (count.reload_echo).
bool reload_echo_identity(char dir, uint16_t identity) {
	return (dir == 'C' && identity == 0x025) || (dir == 'S' && identity == 0x049);
}

// A run's reload round trip on the wire (count.reload_echo): its first C2S 0x25's
// datagram, the first S2C 0x49 at or after it, and the longest entry spacing the
// auto-reload loop can have had: a request datagram that follows another with no C
// datagram between them carries the n requests the loop entered between the two
// sends, so n - 1 spacings fit in the gap. Wire time: the external capture, else the
// evidence capture through `end_ns` (trace.read_time). 0 where the run shows none.
struct ReloadTrip {
	uint64_t first_request_ns = 0;
	uint64_t first_echo_ns = 0;
	uint64_t loop_bound_ns = 0;
};

ReloadTrip reload_trip(const RunFacts &run, uint64_t end_ns) {
	static_assert(has_normalization("trace.read_time"));
	struct Mark {
		uint64_t ts_ns;
		char dir;
		uint16_t identity;
	};
	std::vector<Mark> marks;
	if (run.external_loaded) {
		for (const WireTimedEvent &event : run.external_events)
			marks.push_back({ event.ts_ns, event.dir, event.identity });
	} else {
		for (const wire::ParityEvent &event : run.trace.events)
			if (event.ts_ns <= end_ns)
				marks.push_back({ event.ts_ns, event.dir, event_identity(event) });
	}
	std::stable_sort(marks.begin(), marks.end(),
			[](const Mark &a, const Mark &b) { return a.ts_ns < b.ts_ns; });
	// The C datagrams (one capture time each) and the requests each carries.
	std::vector<std::pair<uint64_t, size_t>> sends;
	ReloadTrip trip;
	for (const Mark &mark : marks) {
		if (mark.dir == 'S') {
			if (mark.identity == 0x049 && trip.first_request_ns != 0 && trip.first_echo_ns == 0)
				trip.first_echo_ns = mark.ts_ns;
			continue;
		}
		if (sends.empty() || sends.back().first != mark.ts_ns)
			sends.push_back({ mark.ts_ns, 0 });
		if (mark.identity != 0x025)
			continue;
		++sends.back().second;
		if (trip.first_request_ns == 0)
			trip.first_request_ns = mark.ts_ns;
	}
	for (size_t i = 1; i < sends.size(); ++i) {
		if (sends[i - 1].second == 0 || sends[i].second < 2)
			continue;
		const uint64_t bound = (sends[i].first - sends[i - 1].first) / (sends[i].second - 1);
		if (trip.loop_bound_ns == 0 || bound < trip.loop_bound_ns)
			trip.loop_bound_ns = bound;
	}
	return trip;
}

MetricValue metric(char dir, const char *name, std::string key, const char *code) {
	MetricValue value;
	value.direction = dir;
	value.metric = name;
	value.key = std::move(key);
	value.code = code;
	return value;
}

// ---------------------------------------------------------------------------
// The tag histogram over the pair's common tag window: each run's first packet
// plus the shorter of the two comparison intervals (window.tag_common_length),
// with a presence grace past it (window.tag_presence_grace).

struct TagStats {
	size_t count = 0;
	std::vector<uint64_t> times; // each occurrence's capture time
	std::set<size_t> lengths;
	std::set<std::string> bodies; // raw bytes; material events only
	bool material = true;
	bool nonzero = false;
};

struct TagProfile {
	std::map<uint16_t, TagStats> tags;
	std::vector<uint16_t> first_order;
};

// The end of a run's tag window: its first packet plus the pair's common length.
uint64_t tag_window_end(const RunFacts &run, double common_ms) {
	return run.first_packet_ns + static_cast<uint64_t>(common_ms * 1e6);
}

TagProfile tag_profile(const RunFacts &run, char dir, uint64_t end_ns) {
	TagProfile profile;
	for (const wire::ParityEvent &event : run.trace.events) {
		if (event.dir != dir || event.ts_ns > end_ns)
			continue;
		const uint16_t identity = event_identity(event);
		auto inserted = profile.tags.try_emplace(identity);
		if (inserted.second)
			profile.first_order.push_back(identity);
		TagStats &stats = inserted.first->second;
		++stats.count;
		stats.times.push_back(event.ts_ns);
		stats.lengths.insert(event.length);
		if (!event.material) {
			stats.material = false;
			continue;
		}
		stats.bodies.insert(std::string(event.body.begin(), event.body.end()));
		for (const uint8_t byte : event.body)
			stats.nonzero = stats.nonzero || byte != 0;
	}
	return profile;
}

std::set<std::string> length_keys(const std::set<size_t> &lengths) {
	std::set<std::string> out;
	for (const size_t length : lengths)
		out.insert(std::to_string(length));
	return out;
}

// Whether `identity` occurs in `run` after `from_ns`, up to `to_ns`.
bool seen_between(const RunFacts &run, char dir, uint16_t identity, uint64_t from_ns, uint64_t to_ns) {
	for (const wire::ParityEvent &event : run.trace.events)
		if (event.dir == dir && event.ts_ns > from_ns && event.ts_ns <= to_ns &&
				event_identity(event) == identity)
			return true;
	return false;
}

// A stream's join: the run's C2S 0x0B, which the host runs through Server_OnPlayerJoin
// [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10 (the call @0x51ABA1)], setting the
// slot's state 6 [orig: Server_OnPlayerJoin @0x51A680 (the store @0x51A712)]: the last
// one at or before the stream's first send `stream_first` (a rotation's joiner joins
// each mission), else its first; else the first S2C 0x0F; 0 when neither
// (window.tag_stream_anchor).
uint64_t join_anchor(const RunFacts &run, uint64_t stream_first) {
	uint64_t first_join = 0, stream_join = 0, load = 0;
	for (const wire::ParityEvent &event : run.trace.events) {
		const uint16_t identity = event_identity(event);
		if (event.dir == 'C' && identity == 0x00B) {
			if (first_join == 0 || event.ts_ns < first_join)
				first_join = event.ts_ns;
			if (event.ts_ns <= stream_first && event.ts_ns > stream_join)
				stream_join = event.ts_ns;
		}
		if (event.dir == 'S' && identity == 0x00F && (load == 0 || event.ts_ns < load))
			load = event.ts_ns;
	}
	return stream_join != 0 ? stream_join : first_join != 0 ? first_join : load;
}

// The overlay buffer: 16 six-byte entries behind a count byte, flushed first when full
// [orig: Server_QueueOverlayStateEntry @0x50FEA0 (the full test @0x50FEA0..0x50FEAE, the
// entry store @0x50FECA..0x50FEFF)].
constexpr size_t kOverlayEntry = 6;
constexpr size_t kOverlayFull = 1 + 16 * kOverlayEntry;

// A run's S2C 0x40 lengths by kind (length.S.0x40.burst_tail): `drawn` holds the last
// 0x40 after a full one in the same datagram (one visit's burst) of every visit but
// the session's first (the join's, no draw); `fixed` every other short 0x40 (the join
// burst's tail, a lone visit's 0x40).
struct BurstTails {
	std::set<size_t> drawn, fixed;
};

BurstTails burst_tails(const RunFacts &run, uint64_t end_ns) {
	std::vector<const wire::ParityEvent *> overlay;
	for (const wire::ParityEvent &event : run.trace.events)
		if (event.dir == 'S' && event_identity(event) == 0x040 && event.ts_ns <= end_ns)
			overlay.push_back(&event);
	BurstTails out;
	for (size_t i = 0; i < overlay.size(); ++i) {
		const wire::ParityEvent &event = *overlay[i];
		if (event.length == kOverlayFull)
			continue;
		const bool last_of_visit = i + 1 == overlay.size() || overlay[i + 1]->frame != event.frame;
		const bool after_full = i > 0 && overlay[i - 1]->frame == event.frame &&
				overlay[i - 1]->length == kOverlayFull;
		const bool join_visit = overlay[0]->frame == event.frame;
		if (last_of_visit && after_full && !join_visit)
			out.drawn.insert(event.length);
		else
			out.fixed.insert(event.length);
	}
	return out;
}

void compare_tags(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	static_assert(has_normalization("event.gameplay_body"));
	static_assert(has_normalization("window.tag_common_length"));
	static_assert(has_normalization("window.tag_presence_grace"));
	constexpr uint64_t kPresenceGraceNs = 10ull * 1000000000ull;
	const double common_ms = std::min(rr.comparison_ms, other.comparison_ms);
	const uint64_t end_a = tag_window_end(rr, common_ms), end_b = tag_window_end(other, common_ms);
	const TagProfile a = tag_profile(rr, dir, end_a);
	const TagProfile b = tag_profile(other, dir, end_b);
	std::set<uint16_t> identities;
	for (const auto &entry : a.tags)
		identities.insert(entry.first);
	for (const auto &entry : b.tags)
		identities.insert(entry.first);
	for (const uint16_t identity : identities) {
		const auto in_a = a.tags.find(identity);
		const auto in_b = b.tags.find(identity);
		const bool has_a = in_a != a.tags.end(), has_b = in_b != b.tags.end();
		const std::string key = identity_key(identity);
		// An identity one side carries is not missing on the other when it lands
		// just past that side's window end: the cut, not the wire.
		const bool graced = has_a != has_b &&
				(has_a ? seen_between(other, dir, identity, end_b, end_b + kPresenceGraceNs)
					   : seen_between(rr, dir, identity, end_a, end_a + kPresenceGraceNs));
		MetricValue presence = metric(dir, "tag_presence", key,
				has_a && !has_b			  ? "MISSING_TAG"
						: !has_a && has_b ? "UNEXPECTED_TAG"
										  : "TAG_PRESENCE");
		presence.distance = has_a == has_b || graced ? 0.0 : 1.0;
		if (graced)
			presence.detail = "past the window end, inside the 10 s presence grace";
		presence.subject = has_b ? "present" : "absent";
		presence.reference = has_a ? "present" : "absent";
		out.push_back(presence);
		if (!has_a || !has_b)
			continue;
		const TagStats &sa = in_a->second, &sb = in_b->second;

		MetricValue count = metric(dir, "tag_count", key, "COUNT_MISMATCH");
		const double cadence = sa.count < 20 ? periodic_cadence_ms(sa.times, end_a) : 0.0;
		if (reload_echo_identity(dir, identity)) {
			// The auto-reload loop re-sends until the first echo lands: past the first
			// request the count is the echo's latency against the loop's period
			// (count.reload_echo).
			static_assert(has_normalization("count.reload_echo"));
			const size_t a_class = std::min<size_t>(sa.count, 2);
			const size_t b_class = std::min<size_t>(sb.count, 2);
			count.distance = a_class == b_class ? 0.0 : 1.0;
			count.floor = 0.0;
			count.subject = std::to_string(sb.count);
			count.reference = std::to_string(sa.count);
			count.detail = "a lone request against a looped one (1 against 2 or more)";
			if (b_class == 1 && a_class == 2) {
				// A lone request is retail's own outcome when its echo landed before the
				// loop's next entry: inside the longest spacing the reference's own
				// requests allow.
				const ReloadTrip lone = reload_trip(other, end_b);
				const ReloadTrip looped = reload_trip(rr, end_a);
				if (lone.first_echo_ns != 0 && looped.loop_bound_ns != 0) {
					const uint64_t latency = lone.first_echo_ns - lone.first_request_ns;
					const bool inside = latency < looped.loop_bound_ns;
					count.distance = inside ? 0.0 : 1.0;
					count.detail = "a lone request whose echo landed " +
							std::to_string(latency / 1000000) + " ms after it, " +
							(inside ? "inside" : "past") +
							" the reference loop's longest entry spacing (" +
							std::to_string(looped.loop_bound_ns / 1000000) + " ms)";
				}
			}
		} else if (cadence > 0.0) {
			const uint64_t first = *std::min_element(sb.times.begin(), sb.times.end());
			const double span_ms = end_b > first ? double(end_b - first) / 1e6 : 0.0;
			cadence_count_metric(count, double(sa.count), cadence, double(sb.count), span_ms);
		} else if (sa.count >= 20) {
			// A stream counts from each run's join (on a rotation, the join of the mission
			// the stream starts in) over the pair's shorter span from it, a timer-phase one
			// too (its first send's phase costs one occurrence); a stream that reaches the
			// joiner before its join keeps its own first (window.tag_stream_anchor).
			static_assert(has_normalization("window.tag_stream_anchor"));
			const uint64_t own_a = *std::min_element(sa.times.begin(), sa.times.end());
			const uint64_t own_b = *std::min_element(sb.times.begin(), sb.times.end());
			const uint64_t join_a = join_anchor(rr, own_a), join_b = join_anchor(other, own_b);
			const bool joined = join_a != 0 && join_b != 0;
			const uint64_t first_a = joined ? std::min(join_a, own_a) : own_a;
			const uint64_t first_b = joined ? std::min(join_b, own_b) : own_b;
			auto within = [](const std::vector<uint64_t> &times, uint64_t first, uint64_t span) {
				return double(std::count_if(times.begin(), times.end(),
						[&](uint64_t t) { return t >= first && t - first <= span; }));
			};
			const bool from_join = joined && own_a >= join_a && own_b >= join_b;
			if ((rr.info.ends_session || other.info.ends_session) && from_join) {
				// A scenario that ends the joiner's session stops a stream of the slot
				// at the disconnect, a readiness wait after the join: each run counts
				// from its join to its own coverage end, over that span; a load-phase
				// stream (one before the join) keeps the common span
				// (window.session_end).
				static_assert(has_normalization("window.session_end"));
				auto covered = [](const RunFacts &run, uint64_t end, uint64_t first) {
					const uint64_t until = std::min(end, coverage_end(run));
					return until > first ? until - first : 0;
				};
				const uint64_t span_a = covered(rr, end_a, first_a);
				const uint64_t span_b = covered(other, end_b, first_b);
				count_metric(count, within(sa.times, first_a, span_a), double(span_a) / 1e6,
						within(sb.times, first_b, span_b), double(span_b) / 1e6);
				count.detail += " over each run's covered play from its join";
			} else {
				const uint64_t span = std::min(end_a - first_a, end_b - first_b);
				const double span_ms = double(span) / 1e6;
				count_metric(count, within(sa.times, first_a, span), span_ms,
						within(sb.times, first_b, span), span_ms);
				count.detail += " over " + std::to_string(int64_t(span_ms / 1000.0)) +
						(joined ? " s from each run's join" : " s from each run's first");
			}
		} else {
			count_metric(count, double(sa.count), common_ms, double(sb.count), common_ms);
		}
		out.push_back(count);

		const bool gameplay = identity == gameplay_identity(dir);
		if (gameplay) {
			// The gameplay frame's length follows its contents: only a new zero
			// length or a collapse to one constant length is wire evidence.
			MetricValue zero = metric(dir, "length_set", key, "LENGTH_SET_MISMATCH");
			const bool new_zero = sb.lengths.count(0) && !sa.lengths.count(0);
			zero.distance = new_zero ? 1.0 : 0.0;
			zero.subject = new_zero ? "zero length" : "no new zero length";
			zero.reference = sa.lengths.count(0) ? "zero length" : "no zero length";
			out.push_back(zero);
			MetricValue constant = metric(dir, "length_constant", key, "LENGTH_CONSTANT");
			const bool collapsed = sa.lengths.size() > 1 && sb.count >= 8 && sb.lengths.size() == 1;
			constant.distance = collapsed ? 1.0 : 0.0;
			constant.subject = std::to_string(sb.lengths.size()) + " lengths";
			constant.reference = std::to_string(sa.lengths.size()) + " lengths";
			out.push_back(constant);
		} else if (!length_set_exempt(dir, identity)) {
			MetricValue lengths = metric(dir, "length_set", key, "LENGTH_SET_MISMATCH");
			std::set<size_t> na = sa.lengths, nb = sb.lengths;
			if (dir == 'S' && identity == 0x040) {
				// A drawn burst tail one entry from a drawn tail of the other run is
				// that draw (length.S.0x40.burst_tail).
				static_assert(has_normalization("length.S.0x40.burst_tail"));
				const BurstTails ta = burst_tails(rr, end_a), tb = burst_tails(other, end_b);
				auto drawn = [](size_t length, const BurstTails &own,
									 const std::set<size_t> &other_lengths, const BurstTails &other) {
					return own.drawn.count(length) && !own.fixed.count(length) &&
							!other_lengths.count(length) &&
							(other.drawn.count(length + kOverlayEntry) ||
									other.drawn.count(length - kOverlayEntry));
				};
				std::set<size_t> fa, fb;
				for (const size_t length : na)
					if (!drawn(length, ta, nb, tb))
						fa.insert(length);
				for (const size_t length : nb)
					if (!drawn(length, tb, na, ta))
						fb.insert(length);
				na = std::move(fa);
				nb = std::move(fb);
			}
			const std::set<std::string> la = length_keys(na), lb = length_keys(nb);
			lengths.distance = jaccard_distance(la, lb);
			lengths.subject = render_set(lb);
			lengths.reference = render_set(la);
			out.push_back(lengths);
		}
		if (gameplay || !sa.material || !sb.material)
			continue;

		MetricValue constant = metric(dir, "body_constant", key, "BODY_CONSTANT");
		const bool collapsed = sa.bodies.size() > 1 && sb.count >= 8 && sb.bodies.size() == 1;
		constant.distance = collapsed ? 1.0 : 0.0;
		constant.subject = std::to_string(sb.bodies.size()) + " distinct bodies";
		constant.reference = std::to_string(sa.bodies.size()) + " distinct bodies";
		out.push_back(constant);

		MetricValue zero = metric(dir, "body_all_zero", key, "BODY_ALL_ZERO");
		zero.distance = sa.nonzero && !sb.nonzero ? 1.0 : 0.0;
		zero.subject = sb.nonzero ? "nonzero bytes" : "all zero";
		zero.reference = sa.nonzero ? "nonzero bytes" : "all zero";
		out.push_back(zero);

		if (stable_identity(dir, identity)) {
			std::set<std::string> ka, kb;
			for (const std::string &raw : sa.bodies)
				ka.insert(stable_body_key(dir, identity, raw));
			for (const std::string &raw : sb.bodies)
				kb.insert(stable_body_key(dir, identity, raw));
			MetricValue stable = metric(dir, "stable_body_set", key, "STABLE_BODY_MISMATCH");
			stable.distance = jaccard_distance(ka, kb);
			stable.subject = render_set(kb, 160);
			stable.reference = render_set(ka, 160);
			out.push_back(stable);
		}
	}

	// First-occurrence order of the tags both carry. A C2S 0x22 re-request rides the
	// S2C 0x16's phase unless both runs' 0x16 rode the 0x11 (order.C.0x22.phase).
	const bool sync_phase = dir == 'C' &&
			(sync_request_phase_bound(rr) || sync_request_phase_bound(other));
	auto ordered = [dir, sync_phase](uint16_t identity) {
		return !first_order_excluded(dir, identity) && !(sync_phase && identity == 0x022);
	};
	std::vector<std::string> order_a, order_b;
	for (const uint16_t identity : a.first_order)
		if (b.tags.count(identity) && ordered(identity))
			order_a.push_back(identity_key(identity));
	for (const uint16_t identity : b.first_order)
		if (a.tags.count(identity) && ordered(identity))
			order_b.push_back(identity_key(identity));
	MetricValue order = metric(dir, "first_order", "", "FIRST_ORDER_MISMATCH");
	order.distance = double(order_inversions(order_a, order_b));
	order.subject = render_list(order_b);
	order.reference = render_list(order_a);
	order.detail = "pairwise inversions between the first-occurrence orders";
	out.push_back(order);
}

uint32_t u32_at(const std::vector<uint8_t> &b, size_t at) {
	return b[at] | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) |
			(uint32_t(b[at + 3]) << 24);
}

// C2S 0x3D answers S2C 0x68's `[u32 start]` with that start and up to 50 dwords
// (NetPacket_WriteEntityIndexList @0x42d950, the `current < count` gate @0x42d9c3):
// a reply's length compares for each start both runs answered.
void compare_page_replies(
		const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("reply.C.0x3d.start"));
	auto lengths = [](const RunFacts &run) {
		std::map<uint32_t, std::set<std::string>> by_start;
		for (const wire::ParityEvent &event : run.trace.events)
			if (event.dir == 'C' && event_identity(event) == 0x03D && event.material &&
					event.body.size() >= 4 && run.in_comparison(event.ts_ns))
				by_start[u32_at(event.body, 0)].insert(std::to_string(event.body.size()));
		return by_start;
	};
	const auto a = lengths(rr), b = lengths(other);
	if (a.empty() && b.empty())
		return;
	size_t common = 0, differing = 0;
	std::string detail;
	for (const auto &entry : a) {
		const auto found = b.find(entry.first);
		if (found == b.end())
			continue;
		++common;
		if (found->second == entry.second)
			continue;
		++differing;
		detail += (detail.empty() ? "start " : "; start ") + std::to_string(entry.first) + ": RR " +
				render_set(entry.second) + " vs " + render_set(found->second);
	}
	MetricValue value = metric('C', "reply_length", identity_key(0x03D), "REPLY_LENGTH_MISMATCH");
	value.distance = double(differing);
	value.subject = std::to_string(b.size()) + " starts";
	value.reference = std::to_string(a.size()) + " starts";
	value.detail = detail.empty() ? std::to_string(common) + " common starts agree" : detail;
	out.push_back(value);
}

// The RTT exchange (§5.34): S2C 0x57 / C2S 0x2C are `[u32 stamp][u8 flag]`, flag 1
// a ping stamped with the sender's GetTickCount, flag 0 the pong echoing the
// other side's ping (Client_ProcessNetworkFrame @0x42c41c, NapiNPClientMsg_0x057_RTT
// @0x432210). The 0x2C bodies are wall clocks (initial.C.0x2c): each pong must
// echo a host ping already seen, and the client's own pings must not run backward.
void compare_rtt_echo(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("initial.C.0x2c"));
	auto faults = [](const RunFacts &run, size_t &bad, size_t &pongs) {
		bad = pongs = 0;
		std::set<uint32_t> host_pings;
		bool have_ping = false;
		uint32_t last_ping = 0;
		for (const wire::ParityEvent &event : run.trace.events) {
			if (!event.material || event.body.size() != 5 || !run.in_comparison(event.ts_ns))
				continue;
			const uint16_t identity = event_identity(event);
			const uint32_t stamp = u32_at(event.body, 0);
			const uint8_t flag = event.body[4];
			if (event.dir == 'S' && identity == 0x057 && flag == 1) {
				host_pings.insert(stamp);
			} else if (event.dir == 'C' && identity == 0x02C && flag == 0) {
				++pongs;
				bad += host_pings.count(stamp) ? 0 : 1;
			} else if (event.dir == 'C' && identity == 0x02C && flag == 1) {
				bad += have_ping && int32_t(stamp - last_ping) < 0 ? 1 : 0;
				have_ping = true;
				last_ping = stamp;
			}
		}
	};
	size_t bad_a, pongs_a, bad_b, pongs_b;
	faults(rr, bad_a, pongs_a);
	faults(other, bad_b, pongs_b);
	MetricValue value = metric('C', "rtt_echo", identity_key(0x02C), "RTT_ECHO_MISMATCH");
	value.distance = double(bad_b);
	value.subject = std::to_string(bad_b) + " faults, " + std::to_string(pongs_b) + " pongs";
	value.reference = std::to_string(bad_a) + " faults, " + std::to_string(pongs_a) + " pongs";
	value.detail = "pongs echoing no earlier S2C 0x57 ping, and client pings running backward";
	out.push_back(value);
}

// ---------------------------------------------------------------------------
// Steady gameplay distributions and the session packet envelope

struct Distribution {
	std::map<std::string, size_t> counts;
	size_t total = 0;
};

void tally(Distribution &d, const std::string &key) {
	++d.counts[key];
	++d.total;
}

// One sample moves a key's share by 1 / n against the max(0.08, RR / 2) unit, so
// under 13 samples one sample's phase alone crosses a unit, on either side
// (steady.distribution_floor).
constexpr size_t kDistributionMinSamples = 13;

// Share delta in tolerance units: max(0.08, RR share / 2) is one unit.
MetricValue distribution_metric(char dir, const char *name, const char *code, const Distribution &a,
		const Distribution &b) {
	static_assert(has_normalization("steady.distribution_floor"));
	MetricValue value = metric(dir, name, "", code);
	value.floor = 1.0;
	value.subject =
			std::to_string(b.total) + " samples, " + std::to_string(b.counts.size()) + " keys";
	value.reference =
			std::to_string(a.total) + " samples, " + std::to_string(a.counts.size()) + " keys";
	if (a.total < kDistributionMinSamples || b.total < kDistributionMinSamples) {
		// Either side short is the window's phase, never a finding: the row skips.
		value.comparable = false;
		value.reference_short = true;
		value.code = "GAMEPLAY_DISTRIBUTION_UNEXERCISED";
		value.detail = "fewer than 13 samples after the first gameplay packet on a side";
		return value;
	}
	std::set<std::string> keys;
	for (const auto &entry : a.counts)
		keys.insert(entry.first);
	for (const auto &entry : b.counts)
		keys.insert(entry.first);
	std::vector<std::pair<double, std::string>> worst;
	for (const std::string &key : keys) {
		const auto fa = a.counts.find(key), fb = b.counts.find(key);
		const double share_a = fa == a.counts.end() ? 0.0 : double(fa->second) / double(a.total);
		const double share_b = fb == b.counts.end() ? 0.0 : double(fb->second) / double(b.total);
		if (std::max(share_a, share_b) < 0.02)
			continue;
		const double units = std::fabs(share_b - share_a) / std::max(0.08, share_a * 0.5);
		value.distance = std::max(value.distance, units);
		char buf[96];
		std::snprintf(buf, sizeof(buf), " RR=%.3f subject=%.3f", share_a, share_b);
		worst.emplace_back(units, key.substr(0, 100) + buf);
	}
	std::sort(worst.begin(), worst.end(),
			[](const auto &x, const auto &y) { return x.first > y.first; });
	for (size_t i = 0; i < worst.size() && i < 3; ++i)
		value.detail += (i ? "; " : "") + worst[i].second;
	return value;
}

// A steady packet's grouping: its flags and record multiset (each record's
// framing key, sorted). Where a reply lands among the tick's records is arrival
// phase; the order distribution judges order. A gameplay packet keys without the
// replies riding it (group.S.0x57.reply); a packet of replies alone keeps them.
std::string steady_group_key(const wire::ParityPacket &packet) {
	static_assert(has_normalization("group.steady_order"));
	static_assert(has_normalization("group.S.0x57.reply"));
	const bool gameplay = carries_low_tag(packet, gameplay_identity(packet.dir));
	std::vector<std::string> wires;
	for (const wire::ParityWireRecord &record : packet.records)
		if (!gameplay || !group_reply(packet.dir, record.full_tag))
			wires.push_back(wire_key(packet.dir, record));
	std::sort(wires.begin(), wires.end());
	char flags[8];
	std::snprintf(flags, sizeof(flags), "0x%02x", unsigned(packet.flags));
	return std::string(flags) + "|" + render_list(wires, 0);
}

void steady_profile(const RunFacts &run, char dir, Distribution &groups, Distribution &orders) {
	std::map<uint32_t, std::vector<const wire::ParityPacket *>> connections;
	for (const wire::ParityPacket &packet : run.trace.packets)
		if (packet.dir == dir && run.in_steady(packet.ts_ns))
			connections[packet.sid].push_back(&packet);
	for (const auto &connection : connections) {
		std::set<uint32_t> seen;
		std::vector<const wire::ParityPacket *> origin;
		for (const wire::ParityPacket *packet : connection.second)
			if (seen.insert(packet->seq).second)
				origin.push_back(packet);
		size_t boundary = origin.size();
		for (size_t i = 0; i < origin.size(); ++i)
			if (carries_low_tag(*origin[i], gameplay_identity(dir))) {
				boundary = i;
				break;
			}
		// The order runs over the producers' records: where a receive-pump reply lands
		// among them is its request's arrival phase (group.S.0x57.order).
		static_assert(has_normalization("group.S.0x57.order"));
		std::string previous;
		for (size_t i = boundary + 1; i < origin.size(); ++i) {
			if (origin[i]->records.empty())
				continue;
			tally(groups, steady_group_key(*origin[i]));
			for (const wire::ParityWireRecord &record : origin[i]->records) {
				if (group_reply(dir, record.full_tag))
					continue;
				const std::string identity = identity_key(record.full_tag);
				if (!previous.empty())
					tally(orders, previous + ">" + identity);
				previous = identity;
			}
		}
	}
}

void compare_steady(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	Distribution ga, oa, gb, ob;
	steady_profile(rr, dir, ga, oa);
	steady_profile(other, dir, gb, ob);
	out.push_back(distribution_metric(
			dir, "steady_group_distribution", "GAMEPLAY_GROUP_DISTRIBUTION", ga, gb));
	out.push_back(distribution_metric(
			dir, "steady_order_distribution", "GAMEPLAY_ORDER_DISTRIBUTION", oa, ob));
}

struct Envelope {
	size_t packets = 0, header_only = 0;
	std::set<std::string> header_flags;
	Distribution shapes;
};

Envelope envelope(const RunFacts &run, char dir) {
	Envelope e;
	for (const wire::ParityPacket &packet : run.trace.packets) {
		if (packet.dir != dir || !run.in_comparison(packet.ts_ns))
			continue;
		char flags[8];
		std::snprintf(flags, sizeof(flags), "0x%02x", unsigned(packet.flags));
		++e.packets;
		if (packet.records.empty()) {
			++e.header_only;
			e.header_flags.insert(flags);
			tally(e.shapes, std::string("header-only|flags=") + flags);
		} else {
			tally(e.shapes, std::string("records|flags=") + flags);
		}
	}
	return e;
}

void cadence_metric(std::vector<MetricValue> &out, char dir, const char *name, size_t rr_count,
		double rr_ms, size_t other_count, double other_ms) {
	MetricValue presence = metric(dir, "packet_class_presence", name, "SESSION_PACKET_CADENCE");
	presence.distance = (rr_count == 0) != (other_count == 0) ? 1.0 : 0.0;
	presence.subject = other_count ? "present" : "absent";
	presence.reference = rr_count ? "present" : "absent";
	presence.detail = "presence versus absence of a packet class is structural";
	out.push_back(presence);
	MetricValue cadence = metric(dir, "packet_cadence", name, "SESSION_PACKET_CADENCE");
	cadence.subject = std::to_string(other_count);
	cadence.reference = std::to_string(rr_count);
	if (rr_ms <= 0.0 || other_ms <= 0.0) {
		cadence.comparable = false;
		cadence.reference_short = rr_ms <= 0.0;
		cadence.detail = "a comparison interval is empty";
	} else {
		const double expected = double(rr_count) * other_ms / rr_ms;
		cadence.distance = std::fabs(double(other_count) - expected);
		cadence.floor = std::max(1.0, std::ceil(expected * 0.20));
		cadence.detail = "RR rate-normalized " + render_number(std::round(expected * 100) / 100);
	}
	out.push_back(cadence);
}

void compare_envelope(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	const Envelope a = envelope(rr, dir), b = envelope(other, dir);
	cadence_metric(
			out, dir, "session", a.packets, rr.comparison_ms, b.packets, other.comparison_ms);
	cadence_metric(out, dir, "header_only", a.header_only, rr.comparison_ms, b.header_only,
			other.comparison_ms);
	MetricValue flags = metric(dir, "header_only_flags", "", "SESSION_PACKET_HEADER_FLAGS");
	flags.distance = jaccard_distance(a.header_flags, b.header_flags);
	flags.subject = render_set(b.header_flags);
	flags.reference = render_set(a.header_flags);
	out.push_back(flags);
	MetricValue shapes = distribution_metric(dir, "packet_shape_distribution",
			"SESSION_PACKET_SHAPE_DISTRIBUTION", a.shapes, b.shapes);
	if (!shapes.comparable)
		shapes.code = "SESSION_PACKET_SHAPE_DISTRIBUTION";
	out.push_back(shapes);
}

// ---------------------------------------------------------------------------
// The outer handshake

// A CU chunk's decimal text value reduced to its low 16 bits ("" when it is not
// one decimal number).
std::string low16_of_decimal(const std::vector<uint8_t> &text) {
	uint64_t value = 0;
	size_t digits = 0;
	for (const uint8_t c : text) {
		if (c == 0)
			break;
		if (c < '0' || c > '9' || ++digits > 18)
			return {};
		value = value * 10 + uint64_t(c - '0');
	}
	return digits == 0 ? std::string() : "low16:" + std::to_string(value & 0xFFFFu);
}

// One flat-TLV field's comparison key, its value masked or reduced when the
// registry names it.
std::string handshake_field_key(uint8_t opcode, const FlatTlvField &field) {
	static_assert(has_normalization("handshake.0x42.CU.CI0"));
	const std::string name(field.name);
	const std::vector<uint8_t> value(field.value, field.value + field.present);
	const uint32_t size_class = handshake_length_class(opcode, name);
	const bool in_class = size_class != 0 &&
			(field.size == size_class || field.size == size_class + 1);
	static_assert(has_normalization("handshake.0x41.AP"));
	std::string key = name + "|" +
			(handshake_field_size_masked(opcode, name) ? std::string("*")
					: in_class ? std::to_string(size_class) + "-" + std::to_string(size_class + 1)
							   : std::to_string(field.size));
	if (name == "CU" && value.size() >= 5) {
		// [u8 type][name\0][u16 len][value]
		const uint8_t type = value[0];
		size_t p = 1;
		while (p < value.size() && value[p] != 0)
			++p;
		if (p + 3 <= value.size()) {
			const std::string cu_name(reinterpret_cast<const char *>(value.data() + 1), p - 1);
			const size_t length = size_t(value[p + 1]) | (size_t(value[p + 2]) << 8);
			const std::vector<uint8_t> cu_value(value.begin() + long(p + 3), value.end());
			std::string rendered = hex_of(cu_value);
			std::string rendered_length = std::to_string(length);
			if (handshake_cu_masked(opcode, cu_name)) {
				rendered = "<dynamic>";
			} else if (handshake_cu_low16(opcode, cu_name)) {
				// The printed decimal's width follows the residue's high word.
				const std::string low = low16_of_decimal(cu_value);
				if (!low.empty()) {
					rendered = low;
					rendered_length = "*";
					key = name + "|*";
				}
			}
			return key + "|type=" + std::to_string(type) + "|name=" + cu_name +
					"|value_length=" + rendered_length + "|value=" + rendered;
		}
	}
	return key + "|value=" +
			(handshake_field_masked(opcode, name) ? std::string("<dynamic>") : hex_of(value));
}

// Whether a session crypt key walks the manager LCG's cycle. Every draw is
// NapiPRNG_NextInRange(0, strlen(charset)) over the 16-bit LCG state' = state *
// 0x04B05731 + 1 (@0x62e450), whose low five bits step x' = 17x + 1 (0x5731 = 17
// mod 32), a full-period 32-cycle; index 31 is the charset's NUL and appends
// nothing (CNapiNPConnection_GenerateTxKey @0x61dfe0, charset g_TxKeyCharset
// @0x849f10). So every next character is the cycle successor of the one before,
// one NUL slot skipped.
constexpr char kTxKeyCharset[] = "0123456789BCDFGHJKLMNPQRSTVWXYZ";
constexpr uint32_t kTxKeyNulSlot = sizeof(kTxKeyCharset) - 1; // 31: the charset's NUL
static_assert(kTxKeyNulSlot == 31);

int tx_key_index(uint8_t c) {
	for (uint32_t i = 0; i < kTxKeyNulSlot; ++i)
		if (uint8_t(kTxKeyCharset[i]) == c)
			return int(i);
	return -1;
}

bool follows_key_cycle(const std::vector<uint8_t> &value) {
	constexpr uint32_t kNulSlot = kTxKeyNulSlot;
	const char *const kCharset = kTxKeyCharset;
	auto index_of = tx_key_index;
	size_t length = value.size();
	while (length > 0 && value[length - 1] == 0)
		--length;
	if (length < 2)
		return false;
	for (size_t i = 0; i + 1 < length; ++i) {
		const int from = index_of(value[i]);
		if (from < 0 || index_of(value[i + 1]) < 0)
			return false;
		uint32_t next = (17u * uint32_t(from) + 1u) & 31u;
		if (next == kNulSlot)
			next = (17u * next + 1u) & 31u;
		if (uint8_t(kCharset[next]) != value[i + 1])
			return false;
	}
	return true;
}

// The first handshake's SCRK, classified: "lcg-cycle", "other", or "absent".
std::string key_generator(const wire::ParityHandshake *handshake) {
	if (!handshake)
		return "absent";
	const uint8_t *data = handshake->body.data();
	const size_t len = handshake->body.size();
	size_t pos = 0;
	while (pos < len) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd || field.name.empty())
			break;
		if (field.name == "SCRK")
			return follows_key_cycle(std::vector<uint8_t>(field.value, field.value + field.present))
					? "lcg-cycle"
					: "other";
		pos = next;
	}
	return "absent";
}

const wire::ParityHandshake *first_handshake(const RunFacts &run, uint8_t opcode) {
	const wire::ParityHandshake *first = nullptr;
	for (const wire::ParityHandshake &handshake : run.trace.handshakes)
		if (handshake.opcode == opcode && handshake.decoded &&
				(!first || handshake.frame < first->frame))
			first = &handshake;
	return first;
}

std::vector<std::string> handshake_keys(const wire::ParityHandshake &handshake) {
	std::vector<std::string> keys;
	const uint8_t *data = handshake.body.data();
	const size_t len = handshake.body.size();
	size_t pos = 0;
	while (pos < len) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		// A field that runs past the body (its present bytes short of its size) is malformed
		// too: the walk ends there, so an overlapping run costs one key, not one per byte.
		if (next == kFlatTlvEnd || field.name.empty() || field.present < field.size) {
			keys.push_back("malformed@" + std::to_string(pos));
			break;
		}
		keys.push_back(handshake_field_key(handshake.opcode, field));
		pos = next;
	}
	return keys;
}

void compare_handshakes(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	const uint8_t opcodes[2] = { uint8_t(dir == 'C' ? 0x41 : 0x81),
		uint8_t(dir == 'C' ? 0x42 : 0x82) };
	for (const uint8_t opcode : opcodes) {
		char key[8];
		std::snprintf(key, sizeof(key), "0x%02x", unsigned(opcode));
		const wire::ParityHandshake *a = first_handshake(rr, opcode);
		const wire::ParityHandshake *b = first_handshake(other, opcode);
		if (!a || !b) {
			MetricValue missing = metric(dir, "handshake", key, "HANDSHAKE_MISSING");
			missing.distance = (a == nullptr) == (b == nullptr) ? 0.0 : 1.0;
			missing.subject = b ? "present" : "absent";
			missing.reference = a ? "present" : "absent";
			out.push_back(missing);
			continue;
		}
		const std::vector<std::string> ka = handshake_keys(*a), kb = handshake_keys(*b);
		MetricValue value = metric(dir, "handshake", key, "HANDSHAKE_MISMATCH");
		value.distance = double(edit_distance(ka, kb));
		value.subject = std::to_string(kb.size()) + " fields";
		value.reference = std::to_string(ka.size()) + " fields";
		for (size_t i = 0; i < std::max(ka.size(), kb.size()); ++i) {
			const std::string x = i < ka.size() ? ka[i] : "-", y = i < kb.size() ? kb[i] : "-";
			if (x == y)
				continue;
			value.detail = "field " + std::to_string(i + 1) + ": RR " + x.substr(0, 100) + " vs " +
					y.substr(0, 100);
			break;
		}
		out.push_back(value);
	}

	// The auth key's generator (handshake.0x42.SCRK.length masks only its length):
	// a retail key walks the manager LCG's cycle, an independently drawn one does not.
	const uint8_t auth = opcodes[1];
	char auth_key[8];
	std::snprintf(auth_key, sizeof(auth_key), "0x%02x", unsigned(auth));
	const std::string ga = key_generator(first_handshake(rr, auth));
	const std::string gb = key_generator(first_handshake(other, auth));
	MetricValue generator = metric(dir, "handshake_key_lcg", auth_key, "HANDSHAKE_KEY_GENERATOR");
	generator.distance = ga == gb ? 0.0 : 1.0;
	generator.subject = gb;
	generator.reference = ga;
	generator.detail = "SCRK as a walk of the 16-bit manager LCG (GenerateTxKey @0x61dfe0)";
	out.push_back(generator);

	// One enumerate announce and one join per session: retail joins on the
	// connection it enumerated from (CNapiNetwork_StartClientConnection @0x4ca160 ->
	// CNapiNPConnection_InitFromSession @0x626320), so a second hello is a second leg.
	for (const uint8_t opcode : opcodes) {
		auto count = [opcode](const RunFacts &run) {
			size_t n = 0;
			for (const wire::ParityHandshake &handshake : run.trace.handshakes)
				n += handshake.opcode == opcode && run.in_comparison(handshake.ts_ns) ? 1 : 0;
			return n;
		};
		char key[8];
		std::snprintf(key, sizeof(key), "0x%02x", unsigned(opcode));
		const size_t na = count(rr), nb = count(other);
		MetricValue value = metric(dir, "handshake_count", key, "HANDSHAKE_COUNT_MISMATCH");
		value.distance = double(nb > na ? nb - na : na - nb);
		value.subject = std::to_string(nb);
		value.reference = std::to_string(na);
		value.detail = "handshake datagrams of the bound session";
		out.push_back(value);
	}
}

// ---------------------------------------------------------------------------
// The integrity stream

struct Transaction {
	bool valid = false;
	bool join_adm = false; // ammo, before the joiner's first uplink (integrity.join_adm)
	uint32_t adm = UINT32_MAX; // ammo: the ADM the latest C2S 0x0C before it named
	uint32_t row = 0;
	uint32_t base_crc = 0; // the reply CRC with the challenge key XORed out
};

// The spans from each C2S 0x0B (a join) to the first C2S 0x0C after it, in which the
// host still holds the body's class-init ADM (integrity.join_adm).
std::vector<std::pair<uint64_t, uint64_t>> join_adm_windows(const RunFacts &run) {
	std::vector<std::pair<uint64_t, uint64_t>> out;
	bool open = false;
	for (const wire::ParityEvent &event : run.trace.events) {
		if (event.dir != 'C')
			continue;
		const uint16_t identity = event_identity(event);
		if (identity == 0x00B && !open) {
			out.emplace_back(event.ts_ns, UINT64_MAX);
			open = true;
		} else if (identity == 0x00C && open) {
			out.back().second = event.ts_ns;
			open = false;
		}
	}
	return out;
}

// Each ADM's longest stay in the joiner's uplinks: consecutive C2S 0x0C naming it, from
// the first to the last (integrity.held_adm).
std::map<uint32_t, uint64_t> adm_stays(const RunFacts &run) {
	std::map<uint32_t, uint64_t> out;
	bool open = false;
	uint32_t adm = 0;
	uint64_t first = 0, last = 0;
	auto finish = [&]() {
		if (open)
			out[adm] = std::max(out[adm], last - first);
	};
	for (const wire::ParityUplinkState &u : run.trace.uplinks) {
		if (!u.decoded || !run.in_comparison(u.ts_ns))
			continue;
		if (open && u.adm == adm) {
			last = u.ts_ns;
			continue;
		}
		finish();
		open = true;
		adm = u.adm;
		first = last = u.ts_ns;
	}
	finish();
	return out;
}

uint32_t u16_at(const std::vector<uint8_t> &b, size_t at) {
	return b[at] | (uint32_t(b[at + 1]) << 8);
}

// S2C 0x31 -> C2S 0x21 (ammo, the reply echoes the key) and S2C 0x30 -> C2S 0x20
// (entity) transactions whose challenge lies in the comparison interval.
std::vector<Transaction> transactions(const RunFacts &run, bool ammo) {
	static_assert(has_normalization("integrity.key"));
	const uint16_t challenge_id = ammo ? 0x031 : 0x030, reply_id = ammo ? 0x021 : 0x020;
	const size_t reply_size = ammo ? 9 : 5;
	const std::vector<wire::ParityEvent> &events = run.trace.events;
	// The 0x31 sends the ammo row of the body's equipped ADM, entity+0x2B0
	// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 (the lookup called @0x508605, the
	// row @0x50863E)], which the plyr class init writes [orig: PlayerClass_InitEntity
	// @0x4B1116] until the joiner's uplink does [orig: NetPacket_SerializePlayerState
	// @0x4C20A3] (or a fired round [orig: Server_ClientFiredRound @0x50BD56]).
	static_assert(has_normalization("integrity.join_adm"));
	const std::vector<std::pair<uint64_t, uint64_t>> windows =
			ammo ? join_adm_windows(run) : std::vector<std::pair<uint64_t, uint64_t>>{};
	std::vector<Transaction> out;
	size_t uplink = 0;
	uint32_t latest_adm = UINT32_MAX;
	for (size_t i = 0; i < events.size(); ++i) {
		const wire::ParityEvent &challenge = events[i];
		if (challenge.dir != 'S' || event_identity(challenge) != challenge_id ||
				!run.in_comparison(challenge.ts_ns))
			continue;
		for (; uplink < run.trace.uplinks.size() &&
				run.trace.uplinks[uplink].ts_ns <= challenge.ts_ns;
				++uplink)
			if (run.trace.uplinks[uplink].decoded)
				latest_adm = run.trace.uplinks[uplink].adm;
		const wire::ParityEvent *reply = nullptr;
		for (size_t j = i + 1; j < events.size(); ++j) {
			const uint16_t identity = event_identity(events[j]);
			if (events[j].dir == 'S' && identity == challenge_id)
				break;
			if (events[j].dir == 'C' && identity == reply_id) {
				reply = &events[j];
				break;
			}
		}
		Transaction t;
		t.adm = ammo ? latest_adm : UINT32_MAX;
		for (const auto &window : windows)
			t.join_adm = t.join_adm ||
					(challenge.ts_ns >= window.first && challenge.ts_ns < window.second);
		if (challenge.body.size() == 3 && reply && reply->body.size() == reply_size) {
			t.row = challenge.body[0];
			const uint32_t key = u16_at(challenge.body, 1);
			const uint32_t crc = u32_at(reply->body, 1);
			t.valid = reply->body[0] == t.row && (!ammo || u32_at(reply->body, 5) == key);
			t.base_crc = crc ^ key;
		}
		out.push_back(t);
	}
	return out;
}

// The scoreboard boundary that sends the integrity challenges: g_ScoreboardBroadcastTimer
// counts from 0 and fires past 0x136 [orig: Server_TickUpdate @0x51D7E0 (the reset
// register @0x51D807, the compare @0x51D901, the challenge call @0x51D912, the reset
// @0x51D917)], every 311 ticks of 16 ms.
constexpr uint64_t kIntegrityBoundaryNs = 311ull * uint64_t(io::kTickMs) * 1000000ull;

// A PUNT for a failed CRC: the reliable H:0x03 (identity 0x103) description.
size_t integrity_punts(const RunFacts &run) {
	size_t n = 0;
	for (const wire::ParityEvent &event : run.trace.events) {
		if (event_identity(event) != 0x103 || !event.material)
			continue;
		const std::string body(event.body.begin(), event.body.end());
		if (body.find("PUNT WCRC") != std::string::npos ||
				body.find("PUNT ACRC") != std::string::npos)
			++n;
	}
	return n;
}

void compare_integrity(const RunFacts &rr, const RunFacts &other, const CompareOptions &options,
		std::vector<MetricValue> &out) {
	static_assert(has_normalization("integrity.oracle"));
	// A retail host validates every reply it receives and punts a bad one
	// (NapiNPServerMsg_HandleAntiCheatCRCCheck @0x502050, @0x5021da): with no PUNT in
	// the session it is the oracle for a row RR never challenged.
	const bool retail_oracle = options.retail_host && integrity_punts(other) == 0;
	for (const bool ammo : { true, false }) {
		const char *family = ammo ? "ammo" : "entity";
		const std::vector<Transaction> ta = transactions(rr, ammo), tb = transactions(other, ammo);
		std::map<uint32_t, std::set<uint32_t>> oracle;
		std::set<std::string> rows_a, rows_b;
		// A 0x31 carries the row of the ADM the body holds at the boundary [orig:
		// Server_BroadcastWeaponSlotStateToPlayers @0x508540 (+0x2B0 @0x5085F6, the row
		// @0x50863E)], the ADM the joiner's uplink writes (@0x4C20A3), and the family flips
		// at every boundary (the read @0x50859E, the flip @0x50868E), so an ammo challenge
		// falls every second boundary: an ADM held under two boundaries in either run is
		// challenged or not by the stay's phase against them. Such a challenge leaves the row
		// sets and the unwitnessed rows; RR's CRC still joins the oracle and the run's still
		// checks against it (integrity.held_adm).
		static_assert(has_normalization("integrity.held_adm"));
		const std::map<uint32_t, uint64_t> stays_a = ammo ? adm_stays(rr)
														  : std::map<uint32_t, uint64_t>{},
										   stays_b = ammo ? adm_stays(other)
														  : std::map<uint32_t, uint64_t>{};
		auto stay_phase = [&stays_a, &stays_b](const Transaction &t) {
			if (t.adm == UINT32_MAX)
				return false;
			auto held = [&t](const std::map<uint32_t, uint64_t> &stays) {
				const auto found = stays.find(t.adm);
				return found != stays.end() && found->second >= 2 * kIntegrityBoundaryNs;
			};
			return !held(stays_a) || !held(stays_b);
		};
		// A challenge before the joiner's first uplink carries the class-init ADM's
		// row, there by the join's phase against the boundary: it leaves the rows,
		// the oracle and the CRC checks, its reply's shape still counts
		// (integrity.join_adm).
		for (const Transaction &t : ta)
			if (t.valid && !t.join_adm) {
				oracle[t.row].insert(t.base_crc);
				if (!stay_phase(t))
					rows_a.insert(std::to_string(t.row));
			}
		if (options.oracle_pool)
			for (const Transaction &t : transactions(*options.oracle_pool, ammo))
				if (t.valid && !t.join_adm)
					oracle[t.row].insert(t.base_crc);
		size_t invalid_a = 0, invalid_b = 0, crc_mismatch = 0, unwitnessed = 0;
		for (const Transaction &t : ta)
			invalid_a += t.valid ? 0 : 1;
		for (const Transaction &t : tb) {
			if (!t.valid) {
				++invalid_b;
				continue;
			}
			if (t.join_adm)
				continue;
			const bool phase = stay_phase(t);
			if (!phase)
				rows_b.insert(std::to_string(t.row));
			const auto found = oracle.find(t.row);
			if (found == oracle.end())
				unwitnessed += retail_oracle || phase ? 0 : 1;
			else if (!found->second.count(t.base_crc))
				++crc_mismatch;
		}
		MetricValue pairs = metric('B', "integrity_pairs", family,
				ammo ? "INTEGRITY_PAIR_INVALID" : "ENTITY_INTEGRITY_PAIR_INVALID");
		pairs.distance = double(invalid_b);
		pairs.subject = std::to_string(invalid_b) + "/" + std::to_string(tb.size()) + " invalid";
		pairs.reference = std::to_string(invalid_a) + "/" + std::to_string(ta.size()) + " invalid";
		pairs.detail = "challenges without a shape/row/key-correct reply";
		out.push_back(pairs);
		MetricValue crc = metric('B', "integrity_crc", family,
				ammo ? "INTEGRITY_CRC_MISMATCH" : "ENTITY_INTEGRITY_CRC_MISMATCH");
		crc.distance = double(crc_mismatch);
		crc.subject = std::to_string(crc_mismatch) + " mismatched";
		crc.reference = std::to_string(oracle.size()) + " oracle rows";
		crc.detail = "replies whose key-free CRC differs from the retail oracle's for the row";
		out.push_back(crc);
		MetricValue unseen =
				metric('B', "integrity_row_unwitnessed", family, "INTEGRITY_ROW_UNWITNESSED");
		unseen.distance = double(unwitnessed);
		unseen.subject = render_set(rows_b);
		unseen.reference = render_set(rows_a);
		unseen.detail = retail_oracle
				? "a retail host accepted every reply (no PUNT): every row is witnessed"
				: "answered rows no retail run challenged (no CRC oracle)";
		out.push_back(unseen);
		if (options.host_rows) {
			MetricValue rows = metric('B', "integrity_rows", family,
					ammo ? "HOST_AMMO_ROW_MISMATCH" : "HOST_DYNAMIC_ENTITY_ROW_MISMATCH");
			rows.distance = jaccard_distance(rows_a, rows_b);
			rows.subject = render_set(rows_b);
			rows.reference = render_set(rows_a);
			rows.detail = "the OpenNova host's challenge rows against RR's";
			out.push_back(rows);
		}
	}

	// The two families are one process-global toggle flipped at every scoreboard
	// boundary, each challenge immediately after a transient S2C 0x16 in the same
	// packet [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 (the toggle read
	// @0x50859E, the flip @0x50868E)], sent only to a slot in state 6 whose body's +0x24
	// bit 1 (mask 0x02) is clear (@0x508584, @0x508594..0x508598): within one mission a challenge's
	// family is its predecessor's flipped once per boundary between them; an S2C 0x25
	// ends the mission and the count with it (integrity.family_toggle).
	static_assert(has_normalization("integrity.family_toggle"));
	auto stream_faults = [](const RunFacts &run, size_t &repeats, size_t &ungrouped,
								 size_t &challenges) {
		repeats = ungrouped = challenges = 0;
		std::map<int, const wire::ParityPacket *> server_packets;
		for (const wire::ParityPacket &packet : run.trace.packets)
			if (packet.dir == 'S')
				server_packets.emplace(packet.frame, &packet);
		uint16_t previous = 0;
		uint64_t previous_ns = 0;
		for (const wire::ParityEvent &event : run.trace.events) {
			const uint16_t identity = event_identity(event);
			if (event.dir == 'S' && identity == 0x025 && run.in_comparison(event.ts_ns))
				previous = 0;
			if (event.dir != 'S' || (identity != 0x030 && identity != 0x031) ||
					!run.in_comparison(event.ts_ns))
				continue;
			++challenges;
			if (previous != 0) {
				const double boundaries =
						double(event.ts_ns - previous_ns) / double(kIntegrityBoundaryNs);
				const bool flipped = std::llround(boundaries) % 2 != 0;
				if ((previous != identity) != flipped)
					++repeats;
			}
			previous = identity;
			previous_ns = event.ts_ns;
			const auto owner = server_packets.find(event.frame);
			bool grouped = false;
			if (owner != server_packets.end()) {
				const std::vector<uint16_t> &tags = owner->second->tags;
				for (size_t i = 1; i < tags.size(); ++i)
					grouped = grouped || (tags[i] == identity && tags[i - 1] == 0x016);
			}
			if (!grouped)
				++ungrouped;
		}
	};
	size_t ra, ua, ca, rb, ub, cb;
	stream_faults(rr, ra, ua, ca);
	stream_faults(other, rb, ub, cb);
	MetricValue alternation = metric('B', "integrity_alternation", "", "INTEGRITY_NOT_ALTERNATING");
	alternation.distance = double(rb);
	alternation.subject = std::to_string(rb) + "/" + std::to_string(cb) + " off the toggle";
	alternation.reference = std::to_string(ra) + "/" + std::to_string(ca) + " off the toggle";
	alternation.detail = "challenges whose family is not the boundary toggle's, within one mission";
	out.push_back(alternation);
	MetricValue scoreboard = metric('B', "integrity_scoreboard", "", "INTEGRITY_SCOREBOARD_ORDER");
	scoreboard.distance = double(ub);
	scoreboard.subject = std::to_string(ub) + "/" + std::to_string(cb) + " without 0x16";
	scoreboard.reference = std::to_string(ua) + "/" + std::to_string(ca) + " without 0x16";
	out.push_back(scoreboard);

	MetricValue punt = metric('B', "integrity_punt", "", "INTEGRITY_PUNT");
	const size_t pa = integrity_punts(rr), pb = integrity_punts(other);
	punt.distance = double(pb);
	punt.subject = std::to_string(pb);
	punt.reference = std::to_string(pa);
	punt.detail = "PUNT WCRC / PUNT ACRC descriptions over the whole session";
	out.push_back(punt);
}

// ---------------------------------------------------------------------------
// Session milestones, bandwidth and gameplay cadence

std::vector<std::string> milestones(const RunFacts &run) {
	struct Milestone {
		const char *name;
		char dir;
		uint16_t selector;
		bool handshake;
	};
	// S2C 0x46 is no milestone: a joiner's first one answers the C2S 0x22 its first
	// 0x16 provokes, so it rides the scoreboard's phase (order.S.0x46). The player-sync
	// track's C2S 0x09 -> S2C 0x2C is one: the host runs the track only in slot state
	// 2, which the 0x09 sets (NapiNPServerMsg_HandlePlayerSyncRequest @0x513200, @0x513249;
	// Server_SendInitialGameStateToPlayer @0x51BBB4..0x51BBBB, its 0x2C @0x51BF5D).
	static_assert(has_normalization("order.S.0x46"));
	static const Milestone kMilestones[] = {
		{ "C:0x41", 'C', 0x41, true },
		{ "S:0x81", 'S', 0x81, true },
		{ "C:0x42", 'C', 0x42, true },
		{ "S:0x82", 'S', 0x82, true },
		{ "S:0x7b", 'S', 0x07B, false },
		{ "C:0x09", 'C', 0x009, false },
		{ "S:0x2c", 'S', 0x02C, false },
		{ "S:0x08", 'S', 0x008, false },
		{ "S:0x0b", 'S', 0x00B, false },
		{ "C:0x0b", 'C', 0x00B, false },
		{ "S:0x0f", 'S', 0x00F, false },
		{ "C:0x0c", 'C', 0x00C, false },
		{ "S:0x0a", 'S', 0x00A, false },
	};
	std::vector<std::pair<int, size_t>> found; // (first frame, definition order)
	std::vector<std::string> names;
	for (size_t m = 0; m < std::size(kMilestones); ++m) {
		const Milestone &milestone = kMilestones[m];
		int first = -1;
		if (milestone.handshake) {
			for (const wire::ParityHandshake &handshake : run.trace.handshakes)
				if (handshake.opcode == milestone.selector &&
						(first < 0 || handshake.frame < first))
					first = handshake.frame;
		} else {
			for (const wire::ParityEvent &event : run.trace.events)
				if (event.dir == milestone.dir && event_identity(event) == milestone.selector) {
					first = event.frame;
					break;
				}
		}
		if (first >= 0)
			found.emplace_back(first, m);
	}
	// E2's round end and rotation (a run whose host plays a next mission in the
	// window), in retail's order (window.rotation_load_gap):
	// - the round end sends each playing slot the zero tick seed, then the end-round
	//   board header [orig: Server_ProcessRoundEnd @0x5164F0 (the 0x61 @0x516790)]
	//   [orig: Server_ProcessRoundEnd @0x5164F0 (the 0x1D @0x516839)];
	// - the joiner's 0x1D pulls the stat board with C2S 0x2B
	//   [orig: NapiNPClientMsg_0x01D @0x430840 (the queue @0x430B03)], answered with
	//   S2C 0x56 [orig: NapiNPServerMsg_HandleReplayDataRequest @0x514FE0 (the send @0x51505A)];
	// - the 2790-tick linger [orig: Server_ProcessRoundEnd @0x5164F0 (the arm @0x5166C4)]
	//   ends in the teardown, which QUEUES every slot S2C 0x25
	//   [orig: Server_DisconnectAndResetAllPlayerSlots @0x516160 (the state-7 queue @0x516385)]
	//   [orig: Game_TeardownMission @0x522350 (the call @0x52269B)]; the 0x25s leave at the
	//   first load pump whose send countdown is 0: the load's head pump
	//   [orig: Game_StartMission @0x524360 (the pump @0x5245E8)] or another ahead of the
	//   terrain load (@0x524F76, before @0x524F7C) for a residual of 1 to 3, a later pump
	//   for a later residual, or the first tick beside the 0x7B under holdoff 12 (the
	//   suite's LAN mode 1);
	// - the joiner answers the 0x25 with its reload's C2S 0x48, the host's load end
	//   sends S2C 0x7B
	//   [orig: Game_StartMission @0x524360 (the Server_BroadcastPlayerInfoToAll call @0x526267)],
	//   and the joiner's reload takes the next mission's 0x0B in its player-sync track,
	//   then its first 0x0A.
	// A first-join 0x7B or 0x0B is never "next": both key after the 0x25. No 0x5D belongs
	// to the round end: it answers the C2S 0x32 of a 0x0F burst, a join leg. These legs
	// count through the steady end only: a capture's tail past it is the runner's.
	if (run.info.rotation) {
		const uint64_t until = run.info.steady_end_ns;
		// The S2C messages in capture order (a message's first fragment's frame).
		std::vector<const wire::ParityEvent *> s2c;
		for (const wire::ParityEvent &event : run.trace.events)
			if (event.dir == 'S' && event.ts_ns <= until)
				s2c.push_back(&event);
		std::stable_sort(s2c.begin(), s2c.end(),
				[](const wire::ParityEvent *a, const wire::ParityEvent *b) {
					return a->frame < b->frame;
				});
		const size_t none = s2c.size();
		auto first_from = [&](uint16_t identity, size_t from) {
			for (size_t i = from; i < s2c.size(); ++i)
				if (event_identity(*s2c[i]) == identity)
					return i;
			return none;
		};
		const size_t board = first_from(0x01D, 0);
		// The round end's seed is the 0x61 queued just ahead of its board, in the
		// zero form [orig: Server_ProcessRoundEnd @0x5164F0 (the body @0x516774)].
		size_t seed = none;
		for (size_t i = board; board != none && i-- > 0;)
			if (event_identity(*s2c[i]) == 0x061) {
				const std::vector<uint8_t> &body = s2c[i]->body;
				if (std::all_of(body.begin(), body.end(), [](uint8_t b) { return b == 0; }))
					seed = i;
				break;
			}
		const size_t pull = board != none ? first_from(0x056, board + 1) : none;
		const size_t reset = board != none ? first_from(0x025, board + 1) : none;
		const size_t announce = reset != none ? first_from(0x07B, reset + 1) : none;
		const size_t header = reset != none ? first_from(0x00B, reset + 1) : none;
		const size_t frame = header != none ? first_from(0x00A, header + 1) : none;
		auto frame_of = [&](size_t i) { return i != none ? s2c[i]->frame : -1; };
		// The joiner's reload opens with C2S 0x48
		// [orig: Game_StartMission @0x524360 (the 0x48 queue @0x524470)], answering the 0x25:
		// a 0x25 that leaves at a load pump puts the 0x48 ahead of the host's load-end 0x7B,
		// one that rides the first tick beside the 0x7B puts it after (a retail draw under
		// holdoff 12, which milestone_order's own A/A carries).
		int reload = -1;
		if (reset != none)
			for (const wire::ParityEvent &event : run.trace.events)
				if (event.dir == 'C' && event_identity(event) == 0x048 && event.ts_ns <= until &&
						event.frame > s2c[reset]->frame && (reload < 0 || event.frame < reload))
					reload = event.frame;
		const std::pair<int, const char *> extra[] = { { frame_of(seed), "S:0x61 round" },
			{ frame_of(board), "S:0x1d round" }, { frame_of(pull), "S:0x56 round" },
			{ frame_of(reset), "S:0x25 reset" }, { reload, "C:0x48 next" },
			{ frame_of(announce), "S:0x7b next" }, { frame_of(header), "S:0x0b next" },
			{ frame_of(frame), "S:0x0a next" } };
		for (size_t i = 0; i < std::size(extra); ++i)
			if (extra[i].first >= 0)
				found.emplace_back(extra[i].first, std::size(kMilestones) + i);
		std::sort(found.begin(), found.end());
		for (const auto &entry : found)
			names.push_back(entry.second < std::size(kMilestones)
							? std::string(kMilestones[entry.second].name)
							: std::string(extra[entry.second - std::size(kMilestones)].second));
		return names;
	}
	std::sort(found.begin(), found.end());
	for (const auto &entry : found)
		names.push_back(kMilestones[entry.second].name);
	return names;
}

// Presence and order are two rows, each judged against its own A/A: a window that
// cuts one retail run between legs widens the presence threshold only, so an
// inversion of ours still fails while the A/A order distance is 0.
void compare_milestones(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	const std::vector<std::string> a = milestones(rr), b = milestones(other);
	const std::set<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
	size_t presence = 0;
	for (const std::string &name : sa)
		presence += sb.count(name) ? 0 : 1;
	for (const std::string &name : sb)
		presence += sa.count(name) ? 0 : 1;
	MetricValue value = metric('B', "milestones", "", "MILESTONE_MISMATCH");
	value.distance = double(presence);
	value.subject = render_list(b);
	value.reference = render_list(a);
	value.detail = "milestones present on one side";
	out.push_back(value);
	MetricValue order = metric('B', "milestone_order", "", "MILESTONE_ORDER");
	order.distance = double(order_inversions(a, b));
	order.subject = value.subject;
	order.reference = value.reference;
	order.detail = "order inversions over the milestones both carry";
	out.push_back(order);
}

// The steady window's time a run's joiner lay dead through `end`: from each S2C 0x13
// naming its body (the modal uplink handle) to its next uplink, else to `end`. The 0x13
// zeroes the body's Health [orig: NapiNPClientMsg_EntityDeath @0x42EB50 (the store
// @0x42EBD6)] and the uplink builder refuses a Health of 0 [orig:
// Player_BuildTag0CInputBody @0x42A550 (@0x42A58F)] until the respawn's body sends
// (window.alive_span).
uint64_t dead_span_ns(const RunFacts &run, uint64_t end) {
	static_assert(has_normalization("window.alive_span"));
	std::map<uint32_t, size_t> handles;
	std::vector<uint64_t> sends;
	for (const wire::ParityUplinkState &state : run.trace.uplinks) {
		if (!state.decoded || !run.in_steady(state.ts_ns) || state.ts_ns > end)
			continue;
		++handles[state.handle & 0xFFFFu];
		sends.push_back(state.ts_ns);
	}
	if (handles.empty())
		return 0;
	uint32_t own = 0;
	size_t most = 0;
	for (const auto &entry : handles)
		if (entry.second > most) {
			own = entry.first;
			most = entry.second;
		}
	std::sort(sends.begin(), sends.end());
	uint64_t dead = 0, resumed = 0;
	for (const wire::ParityEvent &event : run.trace.events) {
		if (event.dir != 'S' || event_identity(event) != 0x013 || event.body.size() < 2 ||
				!run.in_steady(event.ts_ns) || event.ts_ns > end || event.ts_ns < resumed)
			continue;
		if (uint32_t(event.body[0] | (event.body[1] << 8)) != own)
			continue;
		const auto next = std::upper_bound(sends.begin(), sends.end(), event.ts_ns);
		resumed = next != sends.end() ? *next : end;
		dead += resumed - event.ts_ns;
	}
	return dead;
}

void compare_rates(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	// A run whose scenario ends the joiner's session counts its covered play over
	// that play's length: the steady window's rate would be the punt's timing
	// (window.session_end).
	static_assert(has_normalization("window.session_end"));
	auto seconds = [](const RunFacts &run, uint64_t end) {
		if (end <= run.info.steady_start_ns || end >= run.info.steady_end_ns)
			return run.info.steady_seconds;
		return double(end - run.info.steady_start_ns) / 1e9;
	};
	auto bytes_per_second = [dir, &seconds](const RunFacts &run) {
		const uint64_t end = coverage_end(run);
		double bytes = 0.0;
		for (const wire::ParityDatagram &datagram : run.trace.datagrams)
			if (run.in_steady(datagram.ts_ns) && datagram.ts_ns <= end &&
					datagram_direction(datagram, run.session) == dir)
				bytes += double(datagram.length);
		return bytes / seconds(run, end);
	};
	auto gameplay_per_second = [dir, &seconds](const RunFacts &run) {
		const uint64_t end = coverage_end(run);
		double samples = 0.0;
		if (dir == 'C') {
			for (const wire::ParityUplinkState &state : run.trace.uplinks)
				samples += state.decoded && run.in_steady(state.ts_ns) && state.ts_ns <= end
						? 1.0
						: 0.0;
			// The joiner's uplinks per second alive (window.alive_span).
			const double alive = seconds(run, end) - double(dead_span_ns(run, end)) / 1e9;
			if (alive > 0.0)
				return samples / alive;
		} else {
			for (const wire::ParityFrameState &state : run.trace.frames)
				samples += state.decoded && run.in_steady(state.ts_ns) && state.ts_ns <= end
						? 1.0
						: 0.0;
		}
		return samples / seconds(run, end);
	};
	const double ba = bytes_per_second(rr), bb = bytes_per_second(other);
	MetricValue bandwidth = metric(dir, "bandwidth", "", "BANDWIDTH_MISMATCH");
	bandwidth.distance = relative_distance(bb, ba);
	bandwidth.floor = 0.20;
	bandwidth.subject = render_number(std::round(bb)) + " B/s";
	bandwidth.reference = render_number(std::round(ba)) + " B/s";
	bandwidth.detail = "UDP payload bytes per second over the steady window";
	out.push_back(bandwidth);
	const double ga = gameplay_per_second(rr), gb = gameplay_per_second(other);
	MetricValue cadence = metric(dir, "gameplay_cadence", "", "GAMEPLAY_CADENCE");
	cadence.distance = relative_distance(gb, ga);
	cadence.floor = 0.20;
	cadence.subject = render_number(std::round(gb * 10) / 10) + "/s";
	cadence.reference = render_number(std::round(ga * 10) / 10) + "/s";
	cadence.detail = dir == 'C' ? "decoded C2S 0x0C uplinks per steady second the joiner lived"
								: "decoded S2C 0x0A frames per steady second";
	out.push_back(cadence);
}

// The scoreboard's one clock: S2C 0x16's only producer is Server_TickUpdate's
// boundary (@0x51D90D, the sole caller of Server_BuildAndBroadcastScoreboard
// @0x50D960) when g_ScoreboardBroadcastTimer passes 0x136 (@0x51D8F9..0x51D917):
// 311 ticks, ~4,976 ms. A list under 4,000 ms after the one before it (a send
// boundary of jitter allowed) rode another trigger. Wire time: the external capture.
constexpr uint64_t kScoreboardMinGapNs = 4000000000ull;

void compare_scoreboard_cadence(
		const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	static_assert(has_normalization("trace.read_time"));
	auto off_clock = [](const RunFacts &run, size_t &lists) {
		size_t n = 0;
		lists = 0;
		uint64_t previous = 0;
		for (const WireTimedEvent &event : run.external_events) {
			if (event.dir != 'S' || event.identity != 0x016)
				continue;
			if (lists++ > 0 && event.ts_ns - previous < kScoreboardMinGapNs)
				++n;
			previous = event.ts_ns;
		}
		return n;
	};
	MetricValue value =
			metric('S', "scoreboard_cadence", identity_key(0x016), "SCOREBOARD_OFF_CADENCE");
	value.detail = "S2C 0x16 lists under 4,000 ms after the previous one (external capture)";
	if (!rr.external_loaded || !other.external_loaded) {
		value.comparable = false;
		value.reference_short = !rr.external_loaded;
		value.subject = other.external_loaded ? "external capture" : "no external capture";
		value.reference = rr.external_loaded ? "external capture" : "no external capture";
		out.push_back(value);
		return;
	}
	size_t lists_a = 0, lists_b = 0;
	const size_t a = off_clock(rr, lists_a), b = off_clock(other, lists_b);
	value.distance = double(b > a ? b - a : a - b);
	value.subject = std::to_string(b) + " off-clock of " + std::to_string(lists_b);
	value.reference = std::to_string(a) + " off-clock of " + std::to_string(lists_a);
	out.push_back(value);
}

} // namespace

void compare_wire(const RunFacts &rr, const RunFacts &other, const CompareOptions &options,
		std::vector<MetricValue> &out) {
	for (const char dir : { 'C', 'S' }) {
		compare_tags(rr, other, dir, out);
		compare_initial(rr, other, dir, out);
		compare_steady(rr, other, dir, out);
		compare_envelope(rr, other, dir, out);
		compare_handshakes(rr, other, dir, out);
		compare_rates(rr, other, dir, out);
	}
	compare_page_replies(rr, other, out);
	compare_rtt_echo(rr, other, out);
	compare_integrity(rr, other, options, out);
	compare_milestones(rr, other, out);
	compare_scoreboard_cadence(rr, other, out);
}

} // namespace opennova::parity
