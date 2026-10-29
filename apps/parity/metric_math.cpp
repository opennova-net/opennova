#include "metric.h"

#include "normalization.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace opennova::parity {

std::vector<MetricValue> compare_runs(
		const RunFacts &rr, const RunFacts &other, const CompareOptions &options) {
	std::vector<MetricValue> out;
	compare_wire(rr, other, options, out);
	compare_state(rr, other, options, out);
	return out;
}

MetricValue make_metric(char dir, std::string name, std::string key, const char *code) {
	MetricValue value;
	value.direction = dir;
	value.metric = std::move(name);
	value.key = std::move(key);
	value.code = code;
	return value;
}

std::string identity_key(uint16_t identity) {
	char buf[16];
	std::snprintf(buf, sizeof(buf), "0x%02x:%u", unsigned(identity & 0xFFu),
			(identity & 0x100u) ? 1u : 0u);
	return buf;
}

uint16_t event_identity(const wire::ParityEvent &event) {
	return identity_of(static_cast<uint8_t>(event.full_tag & 0xFFu), event.settings_update);
}

uint16_t gameplay_identity(char dir) {
	return dir == 'C' ? 0x00C : 0x00A;
}

namespace {

// S2C 0x64's chunk of the 180-byte session block, its per-session random bytes zeroed:
// [u32 transfer id][u32 180][u32 offset] then the block from `offset`.
std::vector<uint8_t> masked_session_block(const std::string &raw) {
	static_assert(has_normalization("body.S.0x64.session_block"));
	std::vector<uint8_t> bytes(raw.begin(), raw.end());
	constexpr size_t kHeader = 12;
	if (bytes.size() < kHeader)
		return bytes;
	const uint32_t offset = uint32_t(bytes[8]) | uint32_t(bytes[9]) << 8 |
			uint32_t(bytes[10]) << 16 | uint32_t(bytes[11]) << 24;
	for (size_t i = kHeader; i < bytes.size(); ++i) {
		const size_t at = size_t(offset) + (i - kHeader); // the byte's place in the block
		if (at < 36 || (at >= 148 && at < 180))
			bytes[i] = 0;
	}
	return bytes;
}

} // namespace

std::string stable_body_key(char dir, uint16_t identity, const std::string &raw) {
	if (dir == 'S' && identity == 0x064)
		return hex_of(masked_session_block(raw));
	// S2C 0x61's armed tick seed is ((rand() & 0xFE) + 1) << 16 [orig:
	// Server_SendRandomSeedToPlayer @0x5101A0 (the rand call @0x5101BF, and 0xFE @0x5101CB,
	// add 1 @0x5101D1, shl 16 @0x5101D4)]: 00 00 XX 00 with XX odd compares as that form,
	// the drawn byte masked; the zero disarm and any other body stay exact
	// (body.S.0x61.tick_seed).
	static_assert(has_normalization("body.S.0x61.tick_seed"));
	if (dir == 'S' && identity == 0x061 && raw.size() == 4 && raw[0] == 0 && raw[1] == 0 &&
			raw[3] == 0 && (uint8_t(raw[2]) & 1u) != 0)
		return "0000**00";
	// C2S 0x0B: the side password, its NUL, the two map checksums and the engine build's packed
	// version [orig: Game_StartMission @0x524360 - NetPacket_WriteStringAndThreeInts @0x52622e
	//  over g_SidePassword, the two zeros and g_GameVersionPacked @0x52620e]: the version masked
	// (initial.C.0x0b.version), the password and the checksums exact; any other body byte for
	// byte.
	static_assert(has_normalization("initial.C.0x0b.version"));
	if (dir == 'C' && identity == 0x00B) {
		const size_t nul = raw.find('\0');
		if (nul != std::string::npos && raw.size() == nul + 1 + 12) {
			const std::string head(raw.begin(), raw.end() - 4);
			return hex_of(std::vector<uint8_t>(head.begin(), head.end())) + "********";
		}
	}
	const std::string hex = hex_of(std::vector<uint8_t>(raw.begin(), raw.end()));
	const uint32_t prefix = stable_body_prefix(dir, identity);
	if (prefix == 0)
		return hex;
	if (raw.size() < prefix)
		return "short:" + hex;
	return hex.substr(size_t(prefix) * 2);
}

std::string wire_key(char dir, const wire::ParityWireRecord &record) {
	static_assert(has_normalization("group.S.0x0a.length"));
	static_assert(has_normalization("group.fragment_length"));
	static_assert(has_normalization("group.length_exempt"));
	constexpr uint8_t kFragment = 0x04 | 0x02; // FRAG_CONT | FRAG_END (protocol_message.h)
	constexpr uint8_t kLengthField = 0x40 | 0x20; // LEN16 | LEN8 (protocol_message.h)
	const bool exempt = length_set_exempt(dir, record.full_tag);
	char buf[64];
	if (exempt)
		std::snprintf(buf, sizeof(buf), "|raw=%02x|len=*|skip=",
				unsigned(record.raw_flags & ~kLengthField & 0xFFu));
	else if (group_length_masked(dir, record.full_tag))
		std::snprintf(buf, sizeof(buf), "|raw=%02x|len=*|skip=", unsigned(record.raw_flags));
	else if (record.raw_flags & kFragment)
		std::snprintf(buf, sizeof(buf), "|raw=%02x|len=frag|skip=", unsigned(record.raw_flags));
	else
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
		wires += wire_key(packet.dir, packet.records[i]);
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

// The joiner re-requests an unbound S2C 0x16 row with C2S 0x22
// [orig: NapiNPClientMsg_PlayerList @0x42FAE0 (the queue call @0x42FC35)]; only a
// 0x16 riding the 0x11's own packet fixes the 0x22's place against the 0x0A
// answering it.
bool sync_request_phase_bound(const RunFacts &run) {
	static_assert(has_normalization("order.C.0x22.phase"));
	for (const wire::ParityPacket &packet : run.trace.packets)
		if (packet.dir == 'S' && carries_identity(packet, 0x011))
			return !carries_identity(packet, 0x016);
	return true;
}

double p90(std::vector<double> values) {
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const size_t rank = (values.size() * 9 + 9) / 10; // ceil(0.9 n), at least 1
	return values[rank - 1];
}

double jaccard_distance(const std::set<std::string> &a, const std::set<std::string> &b) {
	if (a.empty() && b.empty())
		return 0.0;
	size_t shared = 0;
	for (const std::string &value : a)
		if (b.count(value))
			++shared;
	const size_t joined = a.size() + b.size() - shared;
	return 1.0 - double(shared) / double(joined);
}

size_t order_inversions(const std::vector<std::string> &a, const std::vector<std::string> &b) {
	std::map<std::string, size_t> position;
	for (size_t i = 0; i < b.size(); ++i)
		position.emplace(b[i], i);
	std::vector<size_t> ranks;
	for (const std::string &item : a) {
		const auto found = position.find(item);
		if (found != position.end())
			ranks.push_back(found->second);
	}
	size_t inversions = 0;
	for (size_t i = 0; i < ranks.size(); ++i)
		for (size_t j = i + 1; j < ranks.size(); ++j)
			if (ranks[i] > ranks[j])
				++inversions;
	return inversions;
}

size_t edit_distance(const std::vector<std::string> &a, const std::vector<std::string> &b) {
	std::vector<size_t> row(b.size() + 1);
	for (size_t j = 0; j <= b.size(); ++j)
		row[j] = j;
	for (size_t i = 1; i <= a.size(); ++i) {
		size_t diagonal = row[0];
		row[0] = i;
		for (size_t j = 1; j <= b.size(); ++j) {
			const size_t above = row[j];
			row[j] = std::min(
					{ row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u) });
			diagonal = above;
		}
	}
	return row[b.size()];
}

namespace {

std::string capped(std::string text, size_t cap) {
	if (cap != 0 && text.size() > cap) {
		text.resize(cap);
		text += "...";
	}
	return text;
}

} // namespace

std::string render_set(const std::set<std::string> &values, size_t cap) {
	std::string out = "{";
	bool first = true;
	for (const std::string &value : values) {
		if (!first)
			out += ",";
		out += value;
		first = false;
		if (cap != 0 && out.size() > cap)
			break;
	}
	return capped(out + "}", cap);
}

std::string render_list(const std::vector<std::string> &values, size_t cap) {
	std::string out = "[";
	for (size_t i = 0; i < values.size(); ++i) {
		if (i != 0)
			out += ",";
		out += values[i];
		if (cap != 0 && out.size() > cap)
			break;
	}
	return capped(out + "]", cap);
}

std::string render_number(double value) {
	char buf[48];
	if (value == std::floor(value) && std::fabs(value) < 1e15)
		std::snprintf(buf, sizeof(buf), "%.0f", value);
	else
		std::snprintf(buf, sizeof(buf), "%.4g", value);
	return buf;
}

std::string hex_of(const std::vector<uint8_t> &bytes, size_t cap) {
	static const char kDigits[] = "0123456789abcdef";
	std::string out;
	const size_t n = cap == 0 ? bytes.size() : std::min(cap, bytes.size());
	out.reserve(n * 2 + 3);
	for (size_t i = 0; i < n; ++i) {
		out.push_back(kDigits[bytes[i] >> 4]);
		out.push_back(kDigits[bytes[i] & 15]);
	}
	if (n < bytes.size())
		out += "...";
	return out;
}

double relative_distance(double subject, double reference) {
	if (reference == 0.0)
		return subject == 0.0 ? 0.0 : 1.0;
	return std::fabs(subject / reference - 1.0);
}

void count_metric(
		MetricValue &value, double rr_count, double rr_ms, double other_count, double other_ms) {
	double expected = rr_count;
	if (rr_ms > 0.0 && other_ms > 0.0 && rr_count >= 20.0)
		expected = rr_count * other_ms / rr_ms;
	value.distance = std::fabs(other_count - expected);
	value.floor = std::max(1.0, std::ceil(expected * 0.20));
	value.subject = render_number(other_count);
	value.reference = render_number(rr_count);
	value.detail = "RR rate-normalized " + render_number(std::round(expected * 100.0) / 100.0);
}

double periodic_cadence_ms(std::vector<uint64_t> times_ns, uint64_t end_ns) {
	if (times_ns.size() < 3)
		return 0.0;
	std::sort(times_ns.begin(), times_ns.end());
	if (times_ns.back() > end_ns)
		return 0.0;
	std::vector<double> gaps;
	for (size_t i = 1; i < times_ns.size(); ++i)
		gaps.push_back(double(times_ns[i] - times_ns[i - 1]) / 1e6);
	std::vector<double> sorted = gaps;
	std::sort(sorted.begin(), sorted.end());
	const double median = (sorted[(sorted.size() - 1) / 2] + sorted[sorted.size() / 2]) / 2.0;
	if (median <= 0.0)
		return 0.0;
	for (const double gap : gaps)
		if (std::fabs(gap - median) > 0.25 * median)
			return 0.0;
	// A stream still running at the interval's end, not a burst that stopped.
	if (double(end_ns - times_ns.back()) / 1e6 > 1.25 * median)
		return 0.0;
	return median;
}

void cadence_count_metric(MetricValue &value, double rr_count, double cadence_ms,
		double other_count, double other_span_ms) {
	const double expected = 0.5 + std::max(0.0, other_span_ms) / cadence_ms;
	value.distance = std::fabs(other_count - expected);
	value.floor = std::max(1.0, std::ceil(expected * 0.20));
	value.subject = render_number(other_count);
	value.reference = render_number(rr_count);
	value.detail = "RR cadence " + render_number(std::round(cadence_ms) / 1000.0) + " s over " +
			render_number(std::round(other_span_ms / 100.0) / 10.0) + " s from the first: " +
			render_number(std::round(expected * 100.0) / 100.0);
}

} // namespace opennova::parity
