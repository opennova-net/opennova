#include <novaworld/gate_response.h>

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
	if (!expect(r.cus.empty(), "CUS counted but ignored")) return false;
	if (!expect(r.pvt.empty(), "PVT counted but ignored")) return false;
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
	if (!expect(r.cus.empty(), "CUS ignored even when present")) return false;
	return true;
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
	if (!expect(r.met_ping == -42, "atoi_loose skips vertical-tab whitespace")) return false;
	if (!expect(r.met_ext == 7, "atoi_loose skips form-feed whitespace")) return false;
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
	if (!check_retail_loose_numeric_and_ipv4_edges()) return 1;
	if (!check_ignores_unknown_and_malformed()) return 1;
	if (!check_empty_returns_false()) return 1;
	std::printf("OK: gate response KV parser (quoted + unquoted; 19 retail VARs + CUS/PVT)\n");
	return 0;
}
