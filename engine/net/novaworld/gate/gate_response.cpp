#include <net/novaworld/gate_response.h>

#include <net/napi/literal.h>
#include <net/napi/session.h> // tokenize_quoted (String_TokenizeQuotedToArray)

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

namespace opennova {

namespace {

constexpr const char *LINE_TAG = "VAR";

bool ieq(std::string_view a, std::string_view b) { return opennova::strutil::iequals(a, b); }

bool is_line_break(char c) { return c == '\n' || c == '\r'; }

// One octet: a digit at once (no white space, no sign), then digits to the first
// non-digit, accumulated as `value * 10 + (byte - '0')` in a 32-bit int that wraps.
// The test is the CRT isdigit on the SIGN-EXTENDED byte under the game's ".ACP"
// LC_CTYPE, pinned to cp1252 (cp1252_isdigit, D-NET-388), so the superscripts 0xB2,
// 0xB3 and 0xB9 count as digits whose value is the signed byte less '0' (-126, -125,
// -119). True with `value` and `end` (the first non-digit) set; false, writing
// nothing, when the first byte is no digit. The string is NUL-terminated.
// [orig: parse_simple_decimal @0x62DB90 — the first test @0x62DBAB..0x62DBBB (movsx,
//  _isdigit), the loop @0x62DBC0..0x62DBDC (`lea esi, [edx+ecx*2-30h]` over the movsx
//  byte), the stores @0x62DBE0 / @0x62DBE5, return 1 @0x62DBEA or 0 @0x62DC06]
bool parse_simple_decimal(const char *s, int32_t &value, const char *&end) {
	if (!cp1252_isdigit(static_cast<uint8_t>(*s))) return false;
	uint32_t accumulated = 0;
	do {
		const int32_t digit = static_cast<int32_t>(static_cast<int8_t>(*s)) - '0';
		accumulated = accumulated * 10u + static_cast<uint32_t>(digit);
		++s;
	} while (cp1252_isdigit(static_cast<uint8_t>(*s)));
	value = static_cast<int32_t>(accumulated);
	end = s;
	return true;
}

} // namespace

// [orig: Network_ParseIPv4AddressOctets @0x62DC10: the octets @0x62DC26 / @0x62DC53 /
//  @0x62DC7A / @0x62DCA1, the dots @0x62DC3C / @0x62DC63 / @0x62DC8A, the failure return
//  @0x62DC32, the pack @0x62DCAD..0x62DCD2 (three movzx bytes and the fourth shifted out
//  past bit 31)]
bool parse_ipv4_octets(const std::string &text, std::array<uint8_t, 4> &out) {
	int32_t octets[4] = {0, 0, 0, 0};
	const char *p = text.c_str();
	for (int i = 0; i < 4; ++i) {
		if (i > 0) {
			if (*p != '.') return false;
			++p;
		}
		if (!parse_simple_decimal(p, octets[i], p)) return false;
	}
	for (int i = 0; i < 4; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(octets[i]);
	return true;
}

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

		// The retail quote-aware tokenizer (napi `tokenize_quoted`): whitespace (the CRT
		// isspace under the game's ".ACP" LC_CTYPE, pinned to cp1252, 0xA0 included;
		// D-NET-381) splits outside quotes, a `"` toggles and is dropped, so the quotes the
		// real gate wraps around every key and value (`VAR "POSTIPADDRESS" "127.0.0.1"`)
		// are stripped, and `""` is one empty token.
		// [orig: CNapiGateManager_ProcessResponse @0x4cf0b7 -> CNapiFileReader_ReadAndTokenizeLine
		//  @0x6336E0 -> String_TokenizeQuotedToArray @0x616d60 (the call @0x633758)]
		const auto tokens = tokenize_quoted(line);
		if (tokens.size() < 3 || !ieq(tokens[0], LINE_TAG)) {
			continue; // matches `v7 >= 3 && str1 == "VAR"` gate in the binary
		}

		const std::string &key = tokens[1];
		// Retail passes tokenValue (= tokens[2]) straight to the field
		// handlers; quoting makes the whole value a single token (internal
		// whitespace preserved), so there is no rest-of-line splice.
		const std::string &value = tokens[2];

		// The numeric keys are the CRT atol (io::retail_atol: the locale's white space,
		// 0xA0 included, a sign, saturating at 32 bits; D-NET-384).
		// [orig: _atol for METPING @0x4CF292, METEXT @0x4CF2BC, USEJUNCTION @0x4CF40F,
		//  CLEARJUNCTION @0x4CF439, GLSVSSRIMS @0x4CF487, GLSVSSAGRMS @0x4CF4AE]
		bool matched = true;
		if (ieq(key, "POSTIPADDRESS")) {
			parse_ipv4_octets(value, out.post_ip);
		} else if (ieq(key, "POSTIPPORT")) {
			uint32_t parsed = 0;
			if (napi_parse_literal_value(value, parsed)) out.post_port = parsed;
		} else if (ieq(key, "LOBBYNAME")) {
			out.lobby_name = std::string(value);
		} else if (ieq(key, "METIPADDRESS")) {
			out.met_ip = std::string(value);
		} else if (ieq(key, "METIPPORT")) {
			uint32_t parsed = 0;
			if (napi_parse_literal_value(value, parsed)) out.met_port = parsed;
		} else if (ieq(key, "METLABEL")) {
			out.met_label = std::string(value);
		} else if (ieq(key, "METPING")) {
			out.met_ping = io::retail_atol(value.c_str());
		} else if (ieq(key, "METEXT")) {
			out.met_ext = io::retail_atol(value.c_str());
		} else if (ieq(key, "STARTUPURL")) {
			out.startup_url = std::string(value);
		} else if (ieq(key, "UDPNOVAWORLD")) {
			out.udp_novaworld = std::string(value);
		} else if (ieq(key, "UDPCODE1")) {
			out.udp_code1 = std::string(value);
		} else if (ieq(key, "UDPCODE2")) {
			out.udp_code2 = std::string(value);
		} else if (ieq(key, "REFLECTEDIPADDRESS")) {
			parse_ipv4_octets(value, out.reflected_ip);
		} else if (ieq(key, "REFLECTEDPORTNUMBER")) {
			uint32_t parsed = 0;
			if (napi_parse_literal_value(value, parsed)) out.reflected_port = parsed;
		} else if (ieq(key, "USEJUNCTION")) {
			out.use_junction = io::retail_atol(value.c_str());
		} else if (ieq(key, "CLEARJUNCTION")) {
			out.clear_junction = io::retail_atol(value.c_str());
		} else if (ieq(key, "GLSVSSREQUEST")) {
			out.glsvss_request = std::string(value);
		} else if (ieq(key, "GLSVSSRIMS")) {
			out.glsvss_rims = io::retail_atol(value.c_str());
		} else if (ieq(key, "GLSVSSAGRMS")) {
			out.glsvss_agrms = io::retail_atol(value.c_str());
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
