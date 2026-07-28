#include <novaworld/gate_response.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <io/strutil.h>

namespace opennova {

namespace {

constexpr const char *LINE_TAG = "VAR";

bool ieq(std::string_view a, std::string_view b) { return opennova::strutil::iequals(a, b); }

bool is_line_break(char c) { return c == '\n' || c == '\r'; }
bool is_ws(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// Quote-aware tokenizer, mirroring [orig: String_TokenizeQuotedToArray @
// 0x616d60]: whitespace separates tokens OUTSIDE quotes; a `"` toggles
// in-quote state and is NOT copied (so surrounding quotes are stripped);
// whitespace inside quotes stays part of the token; every other char
// (including `\`, which retail copies as-is) is copied literally. A token
// begins at the first non-whitespace char — an opening quote starts a
// (possibly empty) token, so `""` yields one empty token.
//
// The real gate quotes its VAR lines (`VAR "POSTIPADDRESS" "127.0.0.1"`); a
// whitespace-only split left the quotes attached to the key (`"POSTIPADDRESS"`),
// so no key matched and every real-gate reply was rejected as "bad gate
// response". Tokens are owned (retail copies into a buffer too) because
// quote-stripped tokens are not contiguous in the source line.
std::vector<std::string> tokenize_line(std::string_view line) {
	std::vector<std::string> tokens;
	std::string cur;
	bool in_quote = false;
	bool in_token = false;
	for (const char c : line) {
		if (!is_ws(c) || in_quote) {
			if (!in_token) {
				in_token = true;
				cur.clear();
			}
			if (c == '"') {
				in_quote = !in_quote;
			} else {
				cur.push_back(c);
			}
		} else if (in_token) {
			tokens.push_back(std::move(cur));
			cur.clear();
			in_token = false;
		}
	}
	if (in_token) {
		tokens.push_back(std::move(cur));
	}
	return tokens;
}

bool parse_ipv4(std::string_view s, std::array<uint8_t, 4> &out) {
	std::array<uint8_t, 4> parts{0, 0, 0, 0};
	size_t pos = 0;
	for (size_t part = 0; part < 4; ++part) {
		while (pos < s.size() && is_ws(s[pos])) ++pos;
		int sign = 1;
		if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
			if (s[pos] == '-') sign = -1;
			++pos;
		}
		uint64_t v = 0;
		bool any = false;
		while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
			any = true;
			v = v * 10u + static_cast<uint64_t>(s[pos] - '0');
			++pos;
		}
		if (!any) return false;
		const int64_t signed_v = sign < 0 ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
		parts[part] = static_cast<uint8_t>(signed_v);
		if (part < 3) {
			while (pos < s.size() && s[pos] != '.') ++pos;
			if (pos >= s.size()) return false;
			++pos;
		}
	}
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
	while (i < s.size() && is_ws(s[i])) ++i;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		if (s[i] == '-') sign = -1;
		++i;
	}
	long long acc = 0;
	for (; i < s.size(); ++i) {
		if (s[i] < '0' || s[i] > '9') break;
		acc = acc * 10 + (s[i] - '0');
	}
	return static_cast<int>(sign * acc);
}

uint32_t atou32_loose(std::string_view s) {
	int sign = 1;
	size_t i = 0;
	while (i < s.size() && is_ws(s[i])) ++i;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		if (s[i] == '-') sign = -1;
		++i;
	}
	uint64_t acc = 0;
	for (; i < s.size(); ++i) {
		if (s[i] < '0' || s[i] > '9') break;
		acc = acc * 10u + static_cast<uint64_t>(s[i] - '0');
	}
	if (sign < 0) {
		return static_cast<uint32_t>(-static_cast<int64_t>(acc));
	}
	return static_cast<uint32_t>(acc);
}

} // namespace

// [orig: CNapiGateManager_ProcessResponse @ 0x4ced20], tokenized by
// [orig: String_TokenizeQuotedToArray @ 0x616d60] — quote-aware, strips the
// double-quotes the real gate wraps around every key and value.
// Retail gates on `readResult >= 3 && Napi_StrCaseEqual(tokens[0], "VAR")`
// per line, then on the success path REQUIRES POSTIPADDRESS (dword_B5F490;
// "NO NW POST IP" -> state -9) and POSTIPPORT (dword_B5F494; "NO NW POST
// PORT" -> -9) unless the junction bypass `dword_B5FD2C` is set. This
// parser absorbs the same keys; the caller enforces the post-ip/port
// requirement.
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

		const std::string &key = tokens[1];
		// Retail passes tokenValue (= tokens[2]) straight to the field
		// handlers; quoting makes the whole value a single token (internal
		// whitespace preserved), so there is no rest-of-line splice.
		const std::string &value = tokens[2];

		bool matched = true;
		if (ieq(key, "POSTIPADDRESS")) {
			parse_ipv4(value, out.post_ip);
		} else if (ieq(key, "POSTIPPORT")) {
			out.post_port = atou32_loose(value);
		} else if (ieq(key, "LOBBYNAME")) {
			out.lobby_name = std::string(value);
		} else if (ieq(key, "METIPADDRESS")) {
			out.met_ip = std::string(value);
		} else if (ieq(key, "METIPPORT")) {
			out.met_port = atou32_loose(value);
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
			parse_ipv4(value, out.reflected_ip);
		} else if (ieq(key, "REFLECTEDPORTNUMBER")) {
			out.reflected_port = atou32_loose(value);
		} else if (ieq(key, "USEJUNCTION")) {
			out.use_junction = atoi_loose(value);
		} else if (ieq(key, "CLEARJUNCTION")) {
			out.clear_junction = atoi_loose(value);
		} else if (ieq(key, "GLSVSSREQUEST")) {
			out.glsvss_request = std::string(value);
		} else if (ieq(key, "GLSVSSRIMS")) {
			out.glsvss_rims = atoi_loose(value);
		} else if (ieq(key, "GLSVSSAGRMS")) {
			out.glsvss_agrms = atoi_loose(value);
		} else if (ieq(key, "CUS")) {
			// Phantom key: counted for read-result parity but not retained.
		} else if (ieq(key, "PVT")) {
			// Phantom key: counted for read-result parity but not retained.
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
