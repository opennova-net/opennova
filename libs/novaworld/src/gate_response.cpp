#include <novaworld/gate_response.h>

#include <cctype>
#include <cstdlib>
#include <vector>

namespace opennova {

namespace {

constexpr const char *LINE_TAG = "VAR";

bool ieq(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
		const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
		if (ca != cb) return false;
	}
	return true;
}

bool is_line_break(char c) { return c == '\n' || c == '\r'; }
bool is_ws(char c) { return c == ' ' || c == '\t'; }

// Split one line into whitespace-separated tokens. Empty / all-whitespace
// lines yield zero tokens.
std::vector<std::string_view> tokenize_line(std::string_view line) {
	std::vector<std::string_view> tokens;
	size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && is_ws(line[i])) ++i;
		if (i >= line.size()) break;
		const size_t start = i;
		while (i < line.size() && !is_ws(line[i])) ++i;
		tokens.emplace_back(line.substr(start, i - start));
	}
	return tokens;
}

bool parse_ipv4(std::string_view s, std::array<uint8_t, 4> &out) {
	std::array<uint8_t, 4> parts{0, 0, 0, 0};
	size_t part = 0;
	int v = -1;
	for (size_t i = 0; i <= s.size(); ++i) {
		const bool at_end = (i == s.size());
		const char c = at_end ? '.' : s[i];
		if (c == '.') {
			if (v < 0 || v > 255 || part >= 4) {
				return false;
			}
			parts[part++] = static_cast<uint8_t>(v);
			v = -1;
		} else if (c >= '0' && c <= '9') {
			if (v < 0) v = 0;
			v = v * 10 + (c - '0');
			if (v > 255) return false;
		} else {
			return false;
		}
	}
	if (part != 4) return false;
	out = parts;
	return true;
}

// Match NapiUtil_ParseNumberLiteral-style parsing: accept optional
// leading sign, decimal digits. Returns false on empty or malformed.
bool parse_int(std::string_view s, int &out) {
	if (s.empty()) return false;
	size_t i = 0;
	int sign = 1;
	if (s[0] == '-') { sign = -1; ++i; }
	else if (s[0] == '+') { ++i; }
	if (i >= s.size()) return false;
	long long acc = 0;
	for (; i < s.size(); ++i) {
		if (s[i] < '0' || s[i] > '9') return false;
		acc = acc * 10 + (s[i] - '0');
		if (acc > 2147483647LL) return false;
	}
	out = static_cast<int>(sign * acc);
	return true;
}

// Loose atoi matching the original's atol() semantics (stops at the first
// non-digit, ignores trailing garbage).
int atoi_loose(std::string_view s) {
	int sign = 1;
	size_t i = 0;
	if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
		if (s[0] == '-') sign = -1;
		++i;
	}
	long long acc = 0;
	for (; i < s.size(); ++i) {
		if (s[i] < '0' || s[i] > '9') break;
		acc = acc * 10 + (s[i] - '0');
	}
	return static_cast<int>(sign * acc);
}

} // namespace

bool gate_response_parse(std::string_view body, GateResponse &out) {
	out = GateResponse{};

	size_t i = 0;
	while (i < body.size()) {
		// Locate next line.
		const size_t start = i;
		while (i < body.size() && !is_line_break(body[i])) ++i;
		const std::string_view line = body.substr(start, i - start);
		// Skip over line break(s).
		while (i < body.size() && is_line_break(body[i])) ++i;

		const auto tokens = tokenize_line(line);
		if (tokens.size() < 3 || !ieq(tokens[0], LINE_TAG)) {
			continue; // matches `v7 >= 3 && str1 == "VAR"` gate in the binary
		}

		const std::string_view key = tokens[1];
		// Reconstruct the full value as the rest of the line after the
		// second token. This preserves embedded whitespace in e.g. URLs.
		const size_t value_start = static_cast<size_t>(tokens[2].data() - line.data());
		const std::string_view value = line.substr(value_start);

		bool matched = true;
		if (ieq(key, "POSTIPADDRESS")) {
			parse_ipv4(tokens[2], out.post_ip);
		} else if (ieq(key, "POSTIPPORT")) {
			int v = 0;
			if (parse_int(tokens[2], v) && v >= 0 && v <= 65535) {
				out.post_port = static_cast<uint16_t>(v);
			}
		} else if (ieq(key, "METIPADDRESS")) {
			out.met_ip = std::string(value);
		} else if (ieq(key, "METIPPORT")) {
			int v = 0;
			if (parse_int(tokens[2], v) && v >= 0 && v <= 65535) {
				out.met_port = static_cast<uint16_t>(v);
			}
		} else if (ieq(key, "METLABEL")) {
			out.met_label = std::string(value);
		} else if (ieq(key, "METPING")) {
			out.met_ping = atoi_loose(value);
		} else if (ieq(key, "METEXT")) {
			out.met_ext = atoi_loose(value);
		} else if (ieq(key, "STARTUPURL")) {
			out.startup_url = std::string(value);
		} else if (ieq(key, "UDPNOVAWORLD")) {
			out.udp_novaworld = std::string(value);
		} else if (ieq(key, "UDPCODE1")) {
			out.udp_code1 = std::string(value);
		} else if (ieq(key, "UDPCODE2")) {
			out.udp_code2 = std::string(value);
		} else if (ieq(key, "REFLECTEDIPADDRESS")) {
			parse_ipv4(tokens[2], out.reflected_ip);
		} else if (ieq(key, "REFLECTEDPORTNUMBER")) {
			int v = 0;
			if (parse_int(tokens[2], v) && v >= 0 && v <= 65535) {
				out.reflected_port = static_cast<uint16_t>(v);
			}
		} else if (ieq(key, "CUS")) {
			out.cus = std::string(value);
		} else if (ieq(key, "PVT")) {
			out.pvt = std::string(value);
		} else {
			matched = false;
		}
		if (matched) {
			++out.var_count;
		}
	}
	return out.var_count > 0;
}

} // namespace opennova
