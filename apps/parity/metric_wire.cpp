// The wire metrics: the per-direction tag histogram, the steady gameplay
// distributions, the session packet envelope, the outer handshake, the
// request/reply checks (C2S 0x3D pages, the RTT echo), the integrity stream, the
// session milestones, bandwidth, gameplay cadence and the scoreboard's clock (the
// initial reliable stream is metric_initial.cpp). Rules and tolerances are the
// retired verify_parity_matrix.ps1's (git show
// 5820432c1^:scripts/net/verify_parity_matrix.ps1), recast as distances.

#include "metric.h"

#include "normalization.h"

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
		if (cadence > 0.0) {
			const uint64_t first = *std::min_element(sb.times.begin(), sb.times.end());
			const double span_ms = end_b > first ? double(end_b - first) / 1e6 : 0.0;
			cadence_count_metric(count, double(sa.count), cadence, double(sb.count), span_ms);
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
			const std::set<std::string> la = length_keys(sa.lengths), lb = length_keys(sb.lengths);
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

	// First-occurrence order of the tags both carry.
	std::vector<std::string> order_a, order_b;
	for (const uint16_t identity : a.first_order)
		if (b.tags.count(identity) && !first_order_excluded(dir, identity))
			order_a.push_back(identity_key(identity));
	for (const uint16_t identity : b.first_order)
		if (a.tags.count(identity) && !first_order_excluded(dir, identity))
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

constexpr size_t kDistributionMinSamples = 8;

// Share delta in tolerance units: max(0.08, RR share / 2) is one unit.
MetricValue distribution_metric(char dir, const char *name, const char *code, const Distribution &a,
		const Distribution &b) {
	MetricValue value = metric(dir, name, "", code);
	value.floor = 1.0;
	value.subject =
			std::to_string(b.total) + " samples, " + std::to_string(b.counts.size()) + " keys";
	value.reference =
			std::to_string(a.total) + " samples, " + std::to_string(a.counts.size()) + " keys";
	if (a.total < kDistributionMinSamples || b.total < kDistributionMinSamples) {
		value.comparable = false;
		value.reference_short = a.total < kDistributionMinSamples;
		value.code = "GAMEPLAY_DISTRIBUTION_UNEXERCISED";
		value.detail = "fewer than 8 samples after the first gameplay packet";
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
		std::string previous;
		for (size_t i = boundary + 1; i < origin.size(); ++i) {
			if (origin[i]->records.empty())
				continue;
			tally(groups, steady_group_key(*origin[i]));
			for (const wire::ParityWireRecord &record : origin[i]->records) {
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
	std::string key = name + "|" +
			(in_class ? std::to_string(size_class) + "-" + std::to_string(size_class + 1)
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
	uint32_t row = 0;
	uint32_t base_crc = 0; // the reply CRC with the challenge key XORed out
};

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
	std::vector<Transaction> out;
	for (size_t i = 0; i < events.size(); ++i) {
		const wire::ParityEvent &challenge = events[i];
		if (challenge.dir != 'S' || event_identity(challenge) != challenge_id ||
				!run.in_comparison(challenge.ts_ns))
			continue;
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
		for (const Transaction &t : ta)
			if (t.valid) {
				oracle[t.row].insert(t.base_crc);
				rows_a.insert(std::to_string(t.row));
			}
		if (options.oracle_pool)
			for (const Transaction &t : transactions(*options.oracle_pool, ammo))
				if (t.valid)
					oracle[t.row].insert(t.base_crc);
		size_t invalid_a = 0, invalid_b = 0, crc_mismatch = 0, unwitnessed = 0;
		for (const Transaction &t : ta)
			invalid_a += t.valid ? 0 : 1;
		for (const Transaction &t : tb) {
			if (!t.valid) {
				++invalid_b;
				continue;
			}
			rows_b.insert(std::to_string(t.row));
			const auto found = oracle.find(t.row);
			if (found == oracle.end())
				unwitnessed += retail_oracle ? 0 : 1;
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

	// The two families alternate on one phase, each challenge immediately after a
	// transient S2C 0x16 in the same packet.
	auto stream_faults = [](const RunFacts &run, size_t &repeats, size_t &ungrouped,
								 size_t &challenges) {
		repeats = ungrouped = challenges = 0;
		std::map<int, const wire::ParityPacket *> server_packets;
		for (const wire::ParityPacket &packet : run.trace.packets)
			if (packet.dir == 'S')
				server_packets.emplace(packet.frame, &packet);
		uint16_t previous = 0;
		for (const wire::ParityEvent &event : run.trace.events) {
			const uint16_t identity = event_identity(event);
			if (event.dir != 'S' || (identity != 0x030 && identity != 0x031) ||
					!run.in_comparison(event.ts_ns))
				continue;
			++challenges;
			if (previous == identity)
				++repeats;
			previous = identity;
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
	alternation.subject = std::to_string(rb) + "/" + std::to_string(cb) + " repeats";
	alternation.reference = std::to_string(ra) + "/" + std::to_string(ca) + " repeats";
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
	// 2, which the 0x09 sets (NapiNPServerMsg_0x009 @0x513200, @0x513249;
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
	std::sort(found.begin(), found.end());
	std::vector<std::string> out;
	for (const auto &entry : found)
		out.push_back(kMilestones[entry.second].name);
	return out;
}

void compare_milestones(const RunFacts &rr, const RunFacts &other, std::vector<MetricValue> &out) {
	const std::vector<std::string> a = milestones(rr), b = milestones(other);
	const std::set<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
	size_t presence = 0;
	for (const std::string &name : sa)
		presence += sb.count(name) ? 0 : 1;
	for (const std::string &name : sb)
		presence += sa.count(name) ? 0 : 1;
	MetricValue value = metric('B', "milestones", "", "MILESTONE_MISMATCH");
	value.distance = double(presence + order_inversions(a, b));
	value.subject = render_list(b);
	value.reference = render_list(a);
	value.detail = "milestones present on one side plus order inversions";
	out.push_back(value);
}

void compare_rates(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	auto bytes_per_second = [dir](const RunFacts &run) {
		double bytes = 0.0;
		for (const wire::ParityDatagram &datagram : run.trace.datagrams)
			if (run.in_steady(datagram.ts_ns) && datagram_direction(datagram, run.session) == dir)
				bytes += double(datagram.length);
		return bytes / run.info.steady_seconds;
	};
	auto gameplay_per_second = [dir](const RunFacts &run) {
		double samples = 0.0;
		if (dir == 'C') {
			for (const wire::ParityUplinkState &state : run.trace.uplinks)
				samples += state.decoded && run.in_steady(state.ts_ns) ? 1.0 : 0.0;
		} else {
			for (const wire::ParityFrameState &state : run.trace.frames)
				samples += state.decoded && run.in_steady(state.ts_ns) ? 1.0 : 0.0;
		}
		return samples / run.info.steady_seconds;
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
	cadence.detail = dir == 'C' ? "decoded C2S 0x0C uplinks per steady second"
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
