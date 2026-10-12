#include "metric.h"

#include "normalization.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>

namespace opennova::parity {

std::vector<MetricValue> compare_runs(
		const RunFacts &rr, const RunFacts &other, const CompareOptions &options) {
	std::vector<MetricValue> out;
	compare_wire(rr, other, options, out);
	compare_state(rr, other, out);
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

std::string stable_body_key(char dir, uint16_t identity, const std::string &raw) {
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
	constexpr uint8_t kFragment = 0x04 | 0x02; // FRAG_CONT | FRAG_END (protocol_message.h)
	char buf[64];
	if (group_length_masked(dir, record.full_tag))
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

} // namespace opennova::parity
