// The wire metrics: the per-direction tag histogram, the initial reliable stream,
// the steady gameplay distributions, the session packet envelope, the outer
// handshake, the integrity stream, the session milestones, bandwidth and
// gameplay cadence. Rules and tolerances are the retired verify_parity_matrix.ps1's
// (git show 5820432c1^:scripts/net/verify_parity_matrix.ps1), recast as distances.

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

uint16_t gameplay_identity(char dir) {
	return dir == 'C' ? 0x00C : 0x00A;
}

MetricValue metric(char dir, const char *name, std::string key, const char *code) {
	MetricValue value;
	value.direction = dir;
	value.metric = name;
	value.key = std::move(key);
	value.code = code;
	return value;
}

std::string hex_key(const std::string &raw) {
	return hex_of(std::vector<uint8_t>(raw.begin(), raw.end()));
}

// A stable body's comparison key: the registered prefix (S2C 0x0F's live tick)
// stripped.
std::string stable_body_key(char dir, uint16_t identity, const std::string &raw) {
	const uint32_t prefix = stable_body_prefix(dir, identity);
	if (prefix == 0)
		return hex_key(raw);
	if (raw.size() < prefix)
		return "short:" + hex_key(raw);
	return hex_key(raw.substr(prefix));
}

// ---------------------------------------------------------------------------
// The tag histogram over the comparison interval

struct TagStats {
	size_t count = 0;
	std::set<size_t> lengths;
	std::set<std::string> bodies; // raw bytes; material events only
	bool material = true;
	bool nonzero = false;
};

struct TagProfile {
	std::map<uint16_t, TagStats> tags;
	std::vector<uint16_t> first_order;
};

TagProfile tag_profile(const RunFacts &run, char dir) {
	TagProfile profile;
	for (const wire::ParityEvent &event : run.trace.events) {
		if (event.dir != dir || !run.in_comparison(event.ts_ns))
			continue;
		const uint16_t identity = event_identity(event);
		auto inserted = profile.tags.try_emplace(identity);
		if (inserted.second)
			profile.first_order.push_back(identity);
		TagStats &stats = inserted.first->second;
		++stats.count;
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

void compare_tags(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	static_assert(has_normalization("event.gameplay_body"));
	const TagProfile a = tag_profile(rr, dir);
	const TagProfile b = tag_profile(other, dir);
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
		MetricValue presence = metric(dir, "tag_presence", key,
				has_a && !has_b			  ? "MISSING_TAG"
						: !has_a && has_b ? "UNEXPECTED_TAG"
										  : "TAG_PRESENCE");
		presence.distance = has_a == has_b ? 0.0 : 1.0;
		presence.subject = has_b ? "present" : "absent";
		presence.reference = has_a ? "present" : "absent";
		out.push_back(presence);
		if (!has_a || !has_b)
			continue;
		const TagStats &sa = in_a->second, &sb = in_b->second;

		MetricValue count = metric(dir, "tag_count", key, "COUNT_MISMATCH");
		count_metric(
				count, double(sa.count), rr.comparison_ms, double(sb.count), other.comparison_ms);
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
		} else {
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

// ---------------------------------------------------------------------------
// The initial reliable stream

std::string wire_key(const wire::ParityWireRecord &record) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "|raw=%02x|len=%u|skip=", unsigned(record.raw_flags),
			unsigned(record.encoded_length));
	return identity_key(record.full_tag) + buf +
			(record.skip_bytes.empty() ? std::string("-") : hex_of(record.skip_bytes));
}

std::string group_key(const wire::ParityPacket &packet) {
	char flags[8];
	std::snprintf(flags, sizeof(flags), "0x%02x", unsigned(packet.flags));
	std::string identities, wires;
	for (size_t i = 0; i < packet.records.size(); ++i) {
		if (i != 0) {
			identities += ",";
			wires += ",";
		}
		identities += identity_key(packet.records[i].full_tag);
		wires += wire_key(packet.records[i]);
	}
	return std::string(flags) + "|" + identities + "|wire=" + wires;
}

bool carries_low_tag(const wire::ParityPacket &packet, uint16_t identity) {
	for (const uint16_t tag : packet.tags)
		if ((tag & 0xFFu) == (identity & 0xFFu))
			return true;
	return false;
}

bool carries_identity(const wire::ParityPacket &packet, uint16_t identity) {
	for (const uint16_t tag : packet.tags)
		if (tag == identity)
			return true;
	return false;
}

// Each origin packet's grouping, through the first gameplay packet. A NACK
// replay repeats an earlier (SID, sequence); only the origin counts.
std::vector<std::string> initial_groups(const RunFacts &run, char dir) {
	static_assert(has_normalization("packet.sid_seq_ack"));
	std::map<std::pair<uint32_t, uint32_t>, int> earliest;
	for (const wire::ParityPacket &packet : run.trace.packets) {
		if (packet.dir != dir)
			continue;
		const auto key = std::make_pair(packet.sid, packet.seq);
		const auto found = earliest.find(key);
		if (found == earliest.end() || packet.frame < found->second)
			earliest[key] = packet.frame;
	}
	std::vector<std::string> groups;
	for (const wire::ParityPacket &packet : run.trace.packets) {
		if (packet.dir != dir || packet.records.empty())
			continue;
		if (earliest[std::make_pair(packet.sid, packet.seq)] != packet.frame)
			continue;
		groups.push_back(group_key(packet));
		if (carries_low_tag(packet, gameplay_identity(dir)))
			break;
	}
	return groups;
}

// The frames of each direction's packets through its first gameplay packet.
std::set<int> initial_frames(const RunFacts &run) {
	std::set<int> frames;
	for (const char dir : { 'C', 'S' }) {
		for (const wire::ParityPacket &packet : run.trace.packets) {
			if (packet.dir != dir)
				continue;
			frames.insert(packet.frame);
			if (carries_identity(packet, gameplay_identity(dir)))
				break;
		}
	}
	return frames;
}

void compare_initial(
		const RunFacts &rr, const RunFacts &other, char dir, std::vector<MetricValue> &out) {
	const std::vector<std::string> ga = initial_groups(rr, dir), gb = initial_groups(other, dir);
	MetricValue grouping = metric(dir, "initial_grouping", "", "INITIAL_PACKET_GROUPING");
	grouping.distance = double(edit_distance(ga, gb));
	grouping.subject = std::to_string(gb.size()) + " groups";
	grouping.reference = std::to_string(ga.size()) + " groups";
	for (size_t i = 0; i < std::max(ga.size(), gb.size()); ++i) {
		const std::string x = i < ga.size() ? ga[i] : "-", y = i < gb.size() ? gb[i] : "-";
		if (x == y)
			continue;
		grouping.detail = "first difference at group " + std::to_string(i + 1) + ": RR " +
				x.substr(0, 120) + " vs " + y.substr(0, 120);
		break;
	}
	out.push_back(grouping);

	// The initial material bodies, byte for byte, but for the registered dynamic ones.
	auto sequences = [dir](const RunFacts &run) {
		const std::set<int> frames = initial_frames(run);
		std::map<uint16_t, std::vector<std::string>> by_identity;
		for (const wire::ParityEvent &event : run.trace.events) {
			if (event.dir != dir || !event.material || !frames.count(event.frame))
				continue;
			const uint16_t identity = event_identity(event);
			if (initial_body_exempt(dir, identity))
				continue;
			by_identity[identity].push_back(stable_body_key(
					dir, identity, std::string(event.body.begin(), event.body.end())));
		}
		return by_identity;
	};
	const auto sa = sequences(rr), sb = sequences(other);
	for (const auto &entry : sa) {
		const auto found = sb.find(entry.first);
		if (found == sb.end())
			continue; // presence is the histogram's
		const std::vector<std::string> &x = entry.second, &y = found->second;
		size_t differing = std::max(x.size(), y.size()) - std::min(x.size(), y.size());
		for (size_t i = 0; i < std::min(x.size(), y.size()); ++i)
			if (x[i] != y[i])
				++differing;
		MetricValue body =
				metric(dir, "initial_body", identity_key(entry.first), "INITIAL_BODY_MISMATCH");
		body.distance = double(differing);
		body.subject = render_list(y, 160);
		body.reference = render_list(x, 160);
		body.detail = "bodies differing from RR's initial sequence, position by position";
		out.push_back(body);
	}
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
			tally(groups, group_key(*origin[i]));
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

// One flat-TLV field's comparison key, its value masked when the registry names it.
std::string handshake_field_key(uint8_t opcode, const FlatTlvField &field) {
	const std::string name(field.name);
	const std::vector<uint8_t> value(field.value, field.value + field.size);
	std::string key = name + "|" + std::to_string(field.size);
	if (name == "CU" && field.size >= 5) {
		// [u8 type][name\0][u16 len][value]
		const uint8_t type = value[0];
		size_t p = 1;
		while (p < value.size() && value[p] != 0)
			++p;
		if (p + 3 <= value.size()) {
			const std::string cu_name(reinterpret_cast<const char *>(value.data() + 1), p - 1);
			const size_t length = size_t(value[p + 1]) | (size_t(value[p + 2]) << 8);
			const std::vector<uint8_t> cu_value(value.begin() + long(p + 3), value.end());
			return key + "|type=" + std::to_string(type) + "|name=" + cu_name +
					"|value_length=" + std::to_string(length) + "|value=" +
					(handshake_cu_masked(opcode, cu_name) ? std::string("<dynamic>")
														  : hex_of(cu_value));
		}
	}
	return key + "|value=" +
			(handshake_field_masked(opcode, name) ? std::string("<dynamic>") : hex_of(value));
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
		if (next == kFlatTlvEnd || field.name.empty()) {
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
uint32_t u32_at(const std::vector<uint8_t> &b, size_t at) {
	return b[at] | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) |
			(uint32_t(b[at + 3]) << 24);
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

void compare_integrity(const RunFacts &rr, const RunFacts &other, const CompareOptions &options,
		std::vector<MetricValue> &out) {
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
				++unwitnessed;
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
		crc.reference = std::to_string(oracle.size()) + " RR rows";
		crc.detail = "replies whose key-free CRC differs from RR's for the same row";
		out.push_back(crc);
		MetricValue unseen =
				metric('B', "integrity_row_unwitnessed", family, "INTEGRITY_ROW_UNWITNESSED");
		unseen.distance = double(unwitnessed);
		unseen.subject = render_set(rows_b);
		unseen.reference = render_set(rows_a);
		unseen.detail = "answered rows RR never challenged (no CRC oracle)";
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

	// A PUNT for a failed CRC: the reliable H:0x03 (identity 0x103) description.
	auto punts = [](const RunFacts &run) {
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
	};
	MetricValue punt = metric('B', "integrity_punt", "", "INTEGRITY_PUNT");
	const size_t pa = punts(rr), pb = punts(other);
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
	static const Milestone kMilestones[] = {
		{ "C:0x41", 'C', 0x41, true },
		{ "S:0x81", 'S', 0x81, true },
		{ "C:0x42", 'C', 0x42, true },
		{ "S:0x82", 'S', 0x82, true },
		{ "S:0x7b", 'S', 0x07B, false },
		{ "S:0x08", 'S', 0x008, false },
		{ "S:0x0b", 'S', 0x00B, false },
		{ "S:0x46", 'S', 0x046, false },
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
	compare_integrity(rr, other, options, out);
	compare_milestones(rr, other, out);
}

} // namespace opennova::parity
