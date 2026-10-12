#include <net/novaworld/gate_response.h>

#include <cstdio>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool check_minimal_response() {
	const std::string body =
			"VAR POSTIPADDRESS 10.0.0.1\r\n"
			"VAR POSTIPPORT 8080\r\n"
			"VAR UDPNOVAWORLD 1.2.3.4:64206\r\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "basic 3-line response parses")) return false;
	if (!expect(r.var_count == 3, "var_count == 3")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{10, 0, 0, 1}),
			"POSTIPADDRESS -> 10.0.0.1")) return false;
	if (!expect(r.post_port == 8080, "POSTIPPORT -> 8080")) return false;
	if (!expect(r.udp_novaworld == "1.2.3.4:64206", "UDPNOVAWORLD preserved whole")) return false;
	return true;
}

bool check_all_known_keys() {
	const std::string body =
			"VAR POSTIPADDRESS 192.168.0.1\n"
			"VAR POSTIPPORT 80\n"
			"VAR METIPADDRESS 10.1.2.3\n"
			"VAR METIPPORT 8081\n"
			"VAR METLABEL some-label\n"
			"VAR METPING 42\n"
			"VAR METEXT 7\n"
			"VAR STARTUPURL http://example.com/start\n"
			"VAR UDPNOVAWORLD 172.16.0.5:64206\n"
			"VAR UDPCODE1 abc123\n"
			"VAR UDPCODE2 xyz789\n"
			"VAR REFLECTEDIPADDRESS 203.0.113.7\n"
			"VAR REFLECTEDPORTNUMBER 55555\n"
			"VAR LOBBYNAME jop_2_consumer\n"
			"VAR USEJUNCTION 1\n"
			"VAR CLEARJUNCTION 0\n"
			"VAR GLSVSSREQUEST briefing-req\n"
			"VAR GLSVSSRIMS 3\n"
			"VAR GLSVSSAGRMS 4\n"
			"VAR CUS custom-tier-4\n"
			"VAR PVT private-flag\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "full response parses")) return false;
	if (!expect(r.var_count == 21, "21 VARs absorbed")) return false;
	if (!expect(r.lobby_name == "jop_2_consumer", "LOBBYNAME")) return false;
	if (!expect(r.use_junction == 1, "USEJUNCTION")) return false;
	if (!expect(r.clear_junction == 0, "CLEARJUNCTION")) return false;
	if (!expect(r.glsvss_request == "briefing-req", "GLSVSSREQUEST")) return false;
	if (!expect(r.glsvss_rims == 3, "GLSVSSRIMS")) return false;
	if (!expect(r.glsvss_agrms == 4, "GLSVSSAGRMS")) return false;
	if (!expect(r.met_ip == "10.1.2.3", "METIPADDRESS")) return false;
	if (!expect(r.met_port == 8081, "METIPPORT")) return false;
	if (!expect(r.met_label == "some-label", "METLABEL")) return false;
	if (!expect(r.met_ping == 42, "METPING")) return false;
	if (!expect(r.met_ext == 7, "METEXT")) return false;
	if (!expect(r.startup_url == "http://example.com/start", "STARTUPURL")) return false;
	if (!expect(r.udp_code1 == "abc123", "UDPCODE1")) return false;
	if (!expect(r.udp_code2 == "xyz789", "UDPCODE2")) return false;
	if (!expect((r.reflected_ip == std::array<uint8_t, 4>{203, 0, 113, 7}),
			"REFLECTEDIPADDRESS")) return false;
	if (!expect(r.reflected_port == 55555, "REFLECTEDPORTNUMBER")) return false;
	return true;
}

bool check_case_insensitive_keys() {
	const std::string body = "var postipaddress 127.0.0.1\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "lowercase var/key parses")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{127, 0, 0, 1}),
			"case-insensitive match")) return false;
	return true;
}

bool check_ignores_unknown_and_malformed() {
	const std::string body =
			"VAR UNKNOWN1 ignored\n"
			"\n"                                   // blank line
			"not-a-VAR line\n"                     // wrong tag
			"VAR\n"                                // too few tokens
			"VAR POSTIPADDRESS 8.8.8.8\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "response with one good line parses")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{8, 8, 8, 8}),
			"good line absorbed")) return false;
	if (!expect(r.var_count == 1, "only the known line is counted")) return false;
	return true;
}

// The REAL gate (and our server's gate_listener.cpp) quote every key and
// value: `VAR "POSTIPADDRESS" "127.0.0.1"`. Retail's tokenizer
// (String_TokenizeQuotedToArray @ 0x616d60) strips the quotes; ours must too,
// or the parse rejects the reply and the client logs "bad gate response".
bool check_quoted_response() {
	const std::string body =
			"GATEPROTOCOL \"1.0\"\r\n"                        // non-VAR line, skipped
			"VAR \"POSTIPADDRESS\" \"127.0.0.1\"\r\n"
			"VAR \"POSTIPPORT\" \"7597\"\r\n"
			"VAR \"UDPNOVAWORLD\" \"127.0.0.1:64206\"\r\n"
			"VAR \"STARTUPURL\" \"http://127.0.0.1:8080\"\r\n"
			"VAR \"LOBBYNAME\" \"jop 2 consumer\"\r\n"        // value with internal space
			"VAR \"CUS\" \"\"\r\n";                            // empty quoted value
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "quoted gate response parses")) return false;
	if (!expect(r.var_count == 6, "6 quoted VARs absorbed (GATEPROTOCOL skipped)")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{127, 0, 0, 1}),
			"quoted POSTIPADDRESS -> 127.0.0.1")) return false;
	if (!expect(r.post_port == 7597, "quoted POSTIPPORT -> 7597")) return false;
	if (!expect(r.udp_novaworld == "127.0.0.1:64206", "quoted UDPNOVAWORLD stripped")) return false;
	if (!expect(r.startup_url == "http://127.0.0.1:8080", "quoted STARTUPURL stripped")) return false;
	if (!expect(r.lobby_name == "jop 2 consumer", "quoted value keeps its internal space")) return false;
	return true;
}

// The tokenizer's whitespace is the CRT isspace under the game's ".ACP" LC_CTYPE, pinned to
// cp1252 (D-NET-381): an unquoted 0xA0 separates the tokens as a space does, and a quoted one
// stays in its value. [orig: CNapiFileReader_ReadAndTokenizeLine @0x6336E0 ->
// String_TokenizeQuotedToArray @0x616d60 — isspace @0x616da6]
bool check_nbsp_separates_tokens() {
	const std::string body =
			"VAR\xA0" "POSTIPADDRESS\xA0" "10.0.0.9\r\n"
			"VAR \"LOBBYNAME\"\xA0\"jop\xA0" "2\"\r\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "an 0xA0-separated response parses")) return false;
	if (!expect(r.var_count == 2, "both 0xA0-separated VAR lines absorbed")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{10, 0, 0, 9}), "0xA0 splits the VAR line's tokens"))
		return false;
	return expect(r.lobby_name == "jop\xA0" "2", "a quoted 0xA0 stays in the value");
}

bool check_retail_loose_numeric_and_ipv4_edges() {
	const std::string body =
			"VAR POSTIPADDRESS 300.513.999.256trailing\n"
			"VAR POSTIPPORT 70000\n"
			"VAR REFLECTEDIPADDRESS 1.2.3.4garbage\n"
			"VAR REFLECTEDPORTNUMBER 4294967295\n"
			"VAR METPING \"\v-42ms\"\n"
			"VAR METEXT \"\f7tail\"\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r), "loose retail edge response parses")) return false;
	if (!expect((r.post_ip == std::array<uint8_t, 4>{44, 1, 231, 0}),
			"POSTIPADDRESS octets mask to uint8 and ignore tail")) return false;
	if (!expect(r.post_port == 70000u, "POSTIPPORT stores full 32-bit value")) return false;
	if (!expect((r.reflected_ip == std::array<uint8_t, 4>{1, 2, 3, 4}),
			"REFLECTEDIPADDRESS ignores chars after fourth octet")) return false;
	if (!expect(r.reflected_port == 4294967295u, "REFLECTEDPORTNUMBER stores u32 max")) return false;
	if (!expect(r.met_ping == -42, "METPING's atol skips a leading vertical tab")) return false;
	if (!expect(r.met_ext == 7, "METEXT's atol skips a leading form feed")) return false;
	return true;
}

bool check_retail_port_literal_radixes() {
	const std::string body =
			"VAR POSTIPPORT 0x1DADh\n"
			"VAR METIPPORT 016655\n"
			"VAR REFLECTEDPORTNUMBER %1110110101101\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r),
			"multi-radix retail port response parses")) return false;
	if (!expect(r.post_port == 7597u, "POSTIPPORT accepts C-style hex")) return false;
	if (!expect(r.met_port == 7597u, "METIPPORT accepts leading-zero octal")) return false;
	if (!expect(r.reflected_port == 7597u,
			"REFLECTEDPORTNUMBER accepts percent binary")) return false;
	return true;
}

// The dotted quad, rule by rule (D-NET-386). Each octet is parse_simple_decimal: a digit at
// once, then digits, accumulated in a wrapping 32-bit int; the first three must be followed
// DIRECTLY by '.', the fourth by anything; each octet keeps its low byte; a failed parse
// stores nothing (the line still counts). The digit test is the CRT isdigit under the game's
// ".ACP" LC_CTYPE, pinned to cp1252 (D-NET-388), so 0xB2 / 0xB3 / 0xB9 are digits worth
// their signed byte less '0'.
// [orig: Network_ParseIPv4AddressOctets @0x62DC10 -> parse_simple_decimal @0x62DB90, from
//  CNapiGateManager_ProcessResponse @0x4CF159 / @0x4CF395, the stores @0x4CF16D / @0x4CF3A9]
bool parses_post_ip(const std::string &text, const std::array<uint8_t, 4> &expected) {
	const std::string body = "VAR \"POSTIPADDRESS\" \"" + text + "\"\n";
	opennova::GateResponse r;
	return opennova::gate_response_parse(body, r) && r.var_count == 1 && r.post_ip == expected;
}

// A rejected address leaves the earlier line's value in place and still counts.
bool rejects_post_ip(const std::string &text) {
	const std::string body = "VAR POSTIPADDRESS 9.9.9.9\n"
			"VAR \"POSTIPADDRESS\" \"" + text + "\"\n";
	opennova::GateResponse r;
	return opennova::gate_response_parse(body, r) && r.var_count == 2 &&
			r.post_ip == std::array<uint8_t, 4>{9, 9, 9, 9};
}

bool check_ipv4_witnessed_rules() {
	using Quad = std::array<uint8_t, 4>;
	// Well formed.
	if (!expect(parses_post_ip("1.2.3.4", Quad{1, 2, 3, 4}), "a dotted quad parses")) return false;
	if (!expect(parses_post_ip("001.02.3.0004", Quad{1, 2, 3, 4}), "leading zeros are digits")) return false;
	// No white space anywhere before an octet: not the C six, not 0xA0.
	if (!expect(rejects_post_ip(" 1.2.3.4"), "a leading space fails")) return false;
	if (!expect(rejects_post_ip("\t1.2.3.4"), "a leading tab fails")) return false;
	if (!expect(rejects_post_ip("\xA0" "1.2.3.4"), "a leading 0xA0 fails")) return false;
	if (!expect(rejects_post_ip("1. 2.3.4"), "a space after a dot fails")) return false;
	if (!expect(rejects_post_ip("1.2.3. 4"), "a space before the fourth octet fails")) return false;
	// No sign.
	if (!expect(rejects_post_ip("-1.2.3.4"), "a minus fails")) return false;
	if (!expect(rejects_post_ip("+1.2.3.4"), "a plus fails")) return false;
	if (!expect(rejects_post_ip("1.-2.3.4"), "a signed second octet fails")) return false;
	// The dot right after each of the first three octets.
	if (!expect(rejects_post_ip("1x.2.3.4"), "junk before the dot fails")) return false;
	if (!expect(rejects_post_ip("1 .2.3.4"), "a space before the dot fails")) return false;
	if (!expect(rejects_post_ip("1..2.3.4"), "an empty octet fails")) return false;
	if (!expect(rejects_post_ip("1,2.3.4"), "a comma is no dot")) return false;
	// Four octets.
	if (!expect(rejects_post_ip("1.2.3"), "three octets fail")) return false;
	if (!expect(rejects_post_ip("1.2.3."), "an empty fourth octet fails")) return false;
	if (!expect(rejects_post_ip(""), "an empty value fails")) return false;
	// Anything after the fourth octet is ignored.
	if (!expect(parses_post_ip("1.2.3.4xyz", Quad{1, 2, 3, 4}), "a tail after the fourth octet is ignored"))
		return false;
	if (!expect(parses_post_ip("1.2.3.4.5", Quad{1, 2, 3, 4}), "a fifth octet is ignored")) return false;
	if (!expect(parses_post_ip("1.2.3.4 5", Quad{1, 2, 3, 4}), "a quoted tail after a space is ignored"))
		return false;
	// Past 255 an octet keeps its low byte. Only that byte is observable, so a 32-bit
	// accumulator and a wider one read alike; what the long octets pin is that the sum
	// wraps rather than saturates (a strtoul-style clamp would leave 0xFF).
	if (!expect(parses_post_ip("256.257.511.1000", Quad{0, 1, 255, 232}), "an octet keeps its low byte"))
		return false;
	if (!expect(parses_post_ip("4294967297.4294967552.0.0", Quad{1, 0, 0, 0}),
			"a long octet keeps the low byte of its wrapping sum, not a saturated one")) return false;
	// cp1252's superscript digits are digits worth their signed byte less '0':
	// 0xB9 -> -119 (0x89), 0xB2 -> -126, 1 then 0xB2 -> 10 - 126 = -116 (0x8C), 0xB3 -> -125 (0x83).
	if (!expect(parses_post_ip("\xB9.2.3.4", Quad{0x89, 2, 3, 4}), "0xB9 reads as a digit")) return false;
	if (!expect(parses_post_ip("1\xB2.0.0.\xB3", Quad{0x8C, 0, 0, 0x83}), "0xB2 / 0xB3 read as digits"))
		return false;
	// Every other high byte is no digit.
	if (!expect(rejects_post_ip("\xBC.2.3.4"), "0xBC (one quarter) is no digit")) return false;
	// REFLECTEDIPADDRESS is the same parse.
	{
		const std::string body = "VAR REFLECTEDIPADDRESS 7.7.7.7\n"
				"VAR \"REFLECTEDIPADDRESS\" \" 1.2.3.4\"\n"
				"VAR \"REFLECTEDIPADDRESS\" \"300.2.3.4junk\"\n"
				"VAR \"REFLECTEDIPADDRESS\" \"1x.2.3.4\"\n";
		opennova::GateResponse r;
		if (!expect(opennova::gate_response_parse(body, r) && r.var_count == 4,
				"the REFLECTEDIPADDRESS lines all count")) return false;
		if (!expect((r.reflected_ip == Quad{44, 2, 3, 4}),
				"REFLECTEDIPADDRESS keeps the last good parse")) return false;
	}
	return true;
}

// The numeric keys are the CRT atol, whose white space is the locale's: a quoted value led by
// 0xA0 reads its number (D-NET-384). [orig: CNapiGateManager_ProcessResponse — _atol for
// METPING @0x4CF292, METEXT @0x4CF2BC, USEJUNCTION @0x4CF40F, CLEARJUNCTION @0x4CF439,
// GLSVSSRIMS @0x4CF487, GLSVSSAGRMS @0x4CF4AE]
bool check_numeric_keys_skip_the_nbsp() {
	const std::string body =
			"VAR \"METPING\" \"\xA0" "42\"\n"
			"VAR \"METEXT\" \"\xA0\xA0-7x\"\n"
			"VAR \"USEJUNCTION\" \"\xA0\t1\"\n"
			"VAR \"CLEARJUNCTION\" \"\x85" "1\"\n"
			"VAR \"GLSVSSRIMS\" \"\xA0" "99999999999\"\n"
			"VAR \"GLSVSSAGRMS\" \"3\xA0" "4\"\n";
	opennova::GateResponse r;
	if (!expect(opennova::gate_response_parse(body, r) && r.var_count == 6, "the numeric keys parse")) return false;
	if (!expect(r.met_ping == 42, "METPING skips a leading 0xA0")) return false;
	if (!expect(r.met_ext == -7, "METEXT skips a 0xA0 run before the sign")) return false;
	if (!expect(r.use_junction == 1, "USEJUNCTION skips 0xA0 then a tab")) return false;
	if (!expect(r.clear_junction == 0, "0x85 is no white space")) return false;
	if (!expect(r.glsvss_rims == 2147483647, "GLSVSSRIMS saturates at 32 bits")) return false;
	if (!expect(r.glsvss_agrms == 3, "an inner 0xA0 stops the number")) return false;
	return true;
}

bool check_empty_returns_false() {
	opennova::GateResponse r;
	if (!expect(!opennova::gate_response_parse("", r), "empty response returns false")) return false;
	if (!expect(!opennova::gate_response_parse("junk without VAR\n", r),
			"no VAR lines returns false")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_minimal_response()) return 1;
	if (!check_all_known_keys()) return 1;
	if (!check_case_insensitive_keys()) return 1;
	if (!check_quoted_response()) return 1;
	if (!check_nbsp_separates_tokens()) return 1;
	if (!check_retail_loose_numeric_and_ipv4_edges()) return 1;
	if (!check_retail_port_literal_radixes()) return 1;
	if (!check_ipv4_witnessed_rules()) return 1;
	if (!check_numeric_keys_skip_the_nbsp()) return 1;
	if (!check_ignores_unknown_and_malformed()) return 1;
	if (!check_empty_returns_false()) return 1;
	std::printf("OK: gate response KV parser (quoted + unquoted; 19 retail VARs + CUS/PVT)\n");
	return 0;
}
