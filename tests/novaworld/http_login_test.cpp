// ADR 0010 Phase 3 — NovaWorld HTTP account login wire format.
//
// Proves the Godot-free login helpers (libs/novaworld/http_login) produce a
// POST /NWLogin.dll body the server's own EPASK path decodes, and that the
// cookie jar carries the post-login identity (NWHANDLE/PCID) onto every later
// request — including the GSB browse fetch (grill NW-S5/B3).
//
// The body parsing below replicates the server's parse_form_body + EPASK decode
// (apps/novaworld_server/http_listener.cpp: the anon-ns form helpers and
// handle_login_post in register_legacy_login_routes) so a green here means
// a real POST from this body authenticates without booting a socket.

#include <novacrypto/epask.h>
#include <novaworld/http_login.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char *what) {
	if (!cond) {
		std::printf("  FAIL: %s\n", what);
		++g_failures;
	}
}

// Mirror of apps/novaworld_server/http_listener.cpp url_decode (the body is
// emitted raw by the helpers, so this only collapses '+' and '%XX' if present).
std::string url_decode(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '+') {
			out.push_back(' ');
		} else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
			out.push_back(static_cast<char>((hex(s[i + 1]) << 4) | hex(s[i + 2])));
			i += 2;
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

// Mirror of apps/novaworld_server/http_listener.cpp parse_form_body.
std::map<std::string, std::string> parse_form_body(const std::string &body) {
	std::map<std::string, std::string> out;
	size_t pos = 0;
	while (pos < body.size()) {
		const auto amp = body.find('&', pos);
		const auto chunk_end = amp == std::string::npos ? body.size() : amp;
		const auto eq = body.find('=', pos);
		if (eq != std::string::npos && eq < chunk_end) {
			out.emplace(url_decode(body.substr(pos, eq - pos)),
			            url_decode(body.substr(eq + 1, chunk_end - eq - 1)));
		}
		if (amp == std::string::npos) break;
		pos = amp + 1;
	}
	return out;
}

} // namespace

int main() {
	using namespace opennova;

	// The server generates this once at boot and ships it as the EPASK cookie.
	const EpaskParams pub = generate_epask();

	// ---- 1. Credential body round-trips through the server's decode path ----
	{
		const std::string name = "test";
		const std::string password = "test";
		const std::vector<std::pair<std::string, std::string>> hidden = {
		    {"success", "jop_2_main.htm"},
		    {"failure", "jop_2_main.htm"},
		    {"relay", "jop_2_relay.htm"},
		    {"msgbase", "jop_2_msg.htm"},
		    {"pfid", "28"},
		    {"rememberlogin", "1"},
		};
		const std::string body = build_credentials_post_body(pub, name, password, hidden);

		const auto form = parse_form_body(body);

		// EPASK echoes the bundle the server issued; it must parse back to the
		// same exponent/modulus/key the server holds.
		check(form.count("EPASK") == 1, "body carries EPASK field");
		const EpaskParams echoed = epask_from_string(form.at("EPASK"));
		check(echoed.exponent == pub.exponent && echoed.modulus == pub.modulus &&
		          echoed.key == pub.key,
		      "echoed EPASK matches the issued bundle");

		// The server decrypts NAME/PASSWORD under the bundle it holds.
		check(form.count("NAME") == 1 && form.count("PASSWORD") == 1,
		      "body carries NAME and PASSWORD");
		check(epask_decrypt(form.at("NAME"), pub) == name, "NAME decrypts to plaintext");
		check(epask_decrypt(form.at("PASSWORD"), pub) == password,
		      "PASSWORD decrypts to plaintext");

		// Credentials must NOT be in the clear on the wire.
		check(form.at("NAME") != name, "NAME is encrypted on the wire");
		check(form.at("PASSWORD") != password, "PASSWORD is encrypted on the wire");

		// Hidden fields pass through verbatim.
		check(form.at("success") == "jop_2_main.htm", "hidden 'success' passes through");
		check(form.at("relay") == "jop_2_relay.htm", "hidden 'relay' passes through");
		check(form.at("pfid") == "28", "hidden 'pfid' passes through");

		// Field order is retail's: EPASK, NAME, PASSWORD, then hidden.
		check(body.rfind("EPASK=", 0) == 0, "EPASK is the first field");
		check(body.find("&NAME=") < body.find("&PASSWORD="),
		      "NAME precedes PASSWORD");
		check(body.find("&PASSWORD=") < body.find("&success="),
		      "credentials precede hidden fields");
	}

	// ---- 2. Set-Cookie parse + jar carries identity onto later requests ----
	{
		const std::vector<std::string> set_cookies = {
		    "EPASK=12345:67890:1700000000000123456; Path=/",
		    "YOURIP=203.0.113.7",
		    "LOGINSESSIONTAG=NWServer:NWLogin.dll:SESSIONTAG:42:deadbeef; Path=/; HttpOnly",
		    "NWHANDLE=TestPlayer; Path=/",
		    "CHAR=TestPlayer; Path=/",
		    "PCID=00000002; Expires=Wed, 09 Jun 2027 10:18:14 GMT",
		    "EXPBITS=3",
		};
		const auto parsed = parse_set_cookie_values(set_cookies);
		std::map<std::string, std::string> by_name;
		for (const auto &kv : parsed) by_name[kv.first] = kv.second;

		check(by_name["NWHANDLE"] == "TestPlayer", "NWHANDLE parsed without attributes");
		check(by_name["CHAR"] == "TestPlayer", "CHAR parsed without attributes");
		check(by_name["PCID"] == "00000002", "PCID parsed, Expires attribute dropped");
		check(by_name["LOGINSESSIONTAG"] == "NWServer:NWLogin.dll:SESSIONTAG:42:deadbeef",
		      "LOGINSESSIONTAG keeps its colons, drops Path/HttpOnly");
		check(by_name["EPASK"] == "12345:67890:1700000000000123456",
		      "EPASK cookie value preserved");

		CookieJar jar;
		jar.merge_set_cookie_values(set_cookies);
		check(jar.find("NWHANDLE") != nullptr && *jar.find("NWHANDLE") == "TestPlayer",
		      "jar stores NWHANDLE");
		check(jar.find("CHAR") != nullptr && *jar.find("CHAR") == "TestPlayer",
		      "jar stores CHAR");
		check(jar.find("PCID") != nullptr && *jar.find("PCID") == "00000002",
		      "jar stores PCID");

		// The per-cookie Cookie header lines the engine attaches to the
		// subsequent GSB fetch must carry the post-login identity (resolves
		// ADR 0010's open question). Retail emits one "name=value;" per cookie
		// [orig: CUIBrowser_SendHTTPRequest @ 0x658840].
		auto join_lines = [](const std::vector<std::string> &v) {
			std::string j;
			for (const auto &s : v) { j += s; j += "\n"; }
			return j;
		};
		const std::vector<std::string> lines = jar.cookie_header_lines();
		const std::string joined = join_lines(lines);
		check(joined.find("NWHANDLE=TestPlayer;") != std::string::npos,
		      "GSB-fetch cookie lines carry NWHANDLE with the trailing ';'");
		check(joined.find("PCID=00000002;") != std::string::npos,
		      "GSB-fetch cookie lines carry PCID with the trailing ';'");
		// Each cookie is its own line, not a merged header.
		for (const std::string &line : lines) {
			check(line.find("; ") == std::string::npos && line.back() == ';',
			      "each line is a single trailing-';' cookie, never merged");
		}

		// Re-setting a cookie updates in place without duplicating it.
		jar.set("NWHANDLE", "OtherPlayer");
		check(*jar.find("NWHANDLE") == "OtherPlayer", "jar updates a cookie in place");
		size_t count = 0;
		for (const std::string &line : jar.cookie_header_lines()) {
			if (line.rfind("NWHANDLE=", 0) == 0) ++count;
		}
		check(count == 1, "no duplicate NWHANDLE after update");

		// Subnet key [orig: Network_TruncateIPToSubnet @ 0x62dfe0]: a valid
		// dotted-decimal IPv4 keeps its first two octets; anything else passes
		// through unchanged.
		check(subnet_key("192.168.1.1") == "192.168", "IPv4 truncates to /16");
		check(subnet_key("10.0.5.200") == "10.0", "IPv4 keeps first two octets");
		check(subnet_key("nw.novalogic.com") == "nw.novalogic.com",
		      "a DNS host is unchanged");
		check(subnet_key("256.1.1.1") == "256.1.1.1",
		      "an out-of-range octet is not IPv4, unchanged");
		check(subnet_key("192.168.1") == "192.168.1",
		      "a 3-octet string is not IPv4, unchanged");
	}

	// ---- 3. Real-NW login body: EVERY field EPASK-encrypted except EPASK ----
	// Mirrors the genuine .204 POST /NWLogin.dll body (capture frame 27663), built
	// the way the binding builds it (build_login_post_body with per-field encrypt).
	{
		const std::string name = "ljim", password = "secret";
		const std::vector<LoginFormField> fields = {
		    {"EPASK", epask_to_string(pub), false},
		    {"NAME", name, true},
		    {"PASSWORD", password, true},
		    {"rememberlogindata", "", false},
		    {"rememberlogin", "0", true},
		    {"pfid", "28", true},
		    {"needtoagree", "jop_2_needtoagree.htm", true},
		    {"nodb", "jop_2_nodb.htm", true},
		    {"relay", "jop_2_relay.htm", true},
		    {"msgbase", "jop_2_msg.htm", true},
		    {"enterkey", "jop_2_key.htm", true},
		    {"failure", "jop_2_login.htm", true},
		    {"success", "jop_2_main.htm", true},
		};
		const std::string body = build_login_post_body(pub, fields);
		const auto form = parse_form_body(body);

		check(form.at("EPASK") == epask_to_string(pub), "EPASK echoed plaintext");
		check(epask_decrypt(form.at("NAME"), pub) == name, "NAME decrypts");
		check(epask_decrypt(form.at("PASSWORD"), pub) == password, "PASSWORD decrypts");
		// The hidden fields are encrypted on the wire and decrypt to their values.
		check(form.at("pfid") != "28" && epask_decrypt(form.at("pfid"), pub) == "28",
		      "pfid is encrypted and decrypts to 28");
		check(epask_decrypt(form.at("needtoagree"), pub) == "jop_2_needtoagree.htm",
		      "needtoagree decrypts");
		check(epask_decrypt(form.at("success"), pub) == "jop_2_main.htm",
		      "success decrypts");
		check(epask_decrypt(form.at("failure"), pub) == "jop_2_login.htm",
		      "failure decrypts");
		// rememberlogindata is the lone empty plaintext passthrough (matches retail).
		check(form.at("rememberlogindata").empty(), "rememberlogindata empty plaintext");
	}

	if (g_failures == 0) {
		std::printf("http_login: all checks passed\n");
		return 0;
	}
	std::printf("http_login: %d check(s) failed\n", g_failures);
	return 1;
}
