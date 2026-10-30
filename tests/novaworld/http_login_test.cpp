// ADR 0010 Phase 3 — NovaWorld HTTP account login wire format.
//
// Proves the Godot-free login helpers (engine/net/novaworld/http_login) produce a
// POST /NWLogin.dll body the server's own EPASK path decodes, and that the
// cookie jar carries the post-login identity (NWHANDLE/PCID) onto every later
// request — including the GSB browse fetch (grill NW-S5/B3).
//
// The body parsing below replicates the server's parse_form_body + EPASK decode
// (apps/novaworld_web/retail_pages.cpp: the form helpers, and handle_login_post in
// retail_login_routes.cpp) so a green here means
// a real POST from this body authenticates without booting a socket.

#include <net/novacrypto/epask.h>
#include <net/novaworld/http_login.h>
#include <net/protocol/protocol.h> // title_protocol (DFX2's CD key)

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

// The decrypted field, or "<rejected>" where epask_decrypt returns false.
std::string epask_decrypt(const std::string &ciphertext, const opennova::EpaskParams &params) {
	std::string out;
	return opennova::epask_decrypt(ciphertext, params, out) ? out : std::string("<rejected>");
}

// The parsed bundle (default params where epask_from_string rejects it).
opennova::EpaskParams epask_from_string(const std::string &text) {
	opennova::EpaskParams out;
	return opennova::epask_from_string(text, out) ? out : opennova::EpaskParams{};
}

// A bundle as the server mints it at boot.
opennova::EpaskParams fresh_epask() {
	opennova::EpaskParams params;
	check(opennova::generate_epask(params), "generate_epask converges");
	return params;
}

// Mirror of apps/novaworld_web/retail_pages.cpp url_decode (the body is
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

// Mirror of apps/novaworld_web/retail_pages.cpp parse_form_body.
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
	const EpaskParams pub = fresh_epask();

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
		std::string body;
		check(build_credentials_post_body(pub, name, password, body, hidden),
		      "the credential body builds under the issued bundle");

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
		// The answer's header lines as the transport hands them; the browser's parse reads them
		// as the header text (parse_set_cookie_headers; cookie_jar_test pins its walk).
		const std::vector<std::string> set_cookies = {
		    "Set-Cookie: EPASK=12345:67890:1700000000000123456; Path=/",
		    "Set-Cookie: YOURIP=203.0.113.7",
		    "Set-Cookie: LOGINSESSIONTAG=NWServer:NWLogin.dll:SESSIONTAG:42:deadbeef; Path=/; HttpOnly",
		    "Set-Cookie: NWHANDLE=TestPlayer; Path=/",
		    "Set-Cookie: CHAR=TestPlayer; Path=/",
		    "Set-Cookie: EXPBITS=3",
		};
		const std::vector<CookieEntry> parsed = parse_set_cookie_headers(set_cookies);
		std::map<std::string, std::string> by_name;
		for (const CookieEntry &entry : parsed) by_name[entry.name] = entry.value;

		check(parsed.size() == set_cookies.size(), "every Set-Cookie line parses");
		check(by_name["NWHANDLE"] == "TestPlayer", "NWHANDLE's value stops at its ';'");
		check(by_name["CHAR"] == "TestPlayer", "CHAR parsed");
		check(by_name["LOGINSESSIONTAG"] == "NWServer:NWLogin.dll:SESSIONTAG:42:deadbeef",
		      "LOGINSESSIONTAG keeps its colons");
		check(by_name["EPASK"] == "12345:67890:1700000000000123456",
		      "EPASK cookie value preserved");
		check(parsed[0].path && *parsed[0].path == "/" && !parsed[1].path,
		      "a Path attribute is kept, its absence a null path");

		CookieJar jar;
		jar.update(parsed);
		check(jar.find("NWHANDLE") != nullptr && *jar.find("NWHANDLE") == "TestPlayer",
		      "jar stores NWHANDLE");
		check(jar.find("CHAR") != nullptr && *jar.find("CHAR") == "TestPlayer",
		      "jar stores CHAR");
		check(jar.find("nwhandle") != nullptr, "the by-name read ignores case");

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
		check(joined.find("EXPBITS=3;") != std::string::npos,
		      "GSB-fetch cookie lines carry EXPBITS with the trailing ';'");
		// Each cookie is its own line, not a merged header.
		for (const std::string &line : lines) {
			check(line.find("; ") == std::string::npos && line.back() == ';',
			      "each line is a single trailing-';' cookie, never merged");
		}

		// The CD identity blob the game-session 0x00 JOIN relays is every PUB*
		// cookie packed [name\0][value\0] in insertion order (host codes 23/24/25).
		// [orig: Config_QueryMatchingEntries @0x64eb70 gather("PUB*") ->
		//  NapiNP_WriteClientAuthPayload @0x42a180 "CD" TLV]
		{
			CookieJar pub_jar;
			pub_jar.set("NWHANDLE", "TestPlayer");            // non-PUB, excluded
			pub_jar.set("PUBPCID", "HCMNFLPAOCOFBNLF");
			pub_jar.set("PUBJOINTICKET", "JT:0a00");
			const std::vector<uint8_t> blob = pub_jar.build_prefixed_blob("PUB");
			const std::string s(blob.begin(), blob.end());
			const std::string expected =
					std::string("PUBPCID\0HCMNFLPAOCOFBNLF\0", 25) +
					std::string("PUBJOINTICKET\0JT:0a00\0", 22);
			check(s == expected,
			      "CD blob packs PUB* cookies [name\\0][value\\0], excludes non-PUB");
			check(pub_jar.build_prefixed_blob("PUB").size() == 47,
			      "CD blob length is the packed byte total");
			CookieJar empty_jar;
			empty_jar.set("NWHANDLE", "x");
			check(empty_jar.build_prefixed_blob("PUB").empty(),
			      "no PUB* cookie -> empty CD blob (LAN / unauthenticated)");
			// The glob folds case: a lower-case pub* name is in the set (D-NET-1386).
			// [orig: String_WildcardMatchCaseInsensitive_0 @0x651820 - the fold @0x65190C..0x651923]
			CookieJar folded_jar;
			folded_jar.set("pubsquadinfo", "SQ");
			folded_jar.set("PUBPCID", "PC");
			check(folded_jar.build_prefixed_blob("PUB") ==
			              std::vector<uint8_t>({'p', 'u', 'b', 's', 'q', 'u', 'a', 'd', 'i', 'n', 'f', 'o',
			                      0, 'S', 'Q', 0, 'P', 'U', 'B', 'P', 'C', 'I', 'D', 0, 'P', 'C', 0}),
			      "the PUB* gather matches the prefix without case");
			// The join's CD: the set up to 0x7FE bytes, none past it (D-NET-1387).
			// [orig: NetPacket_BuildAnnouncePayload @0x4C4BF0 - the PUB* set's 1..0x7FE test @0x4C4C49..0x4C4C58]
			CookieJar fits_jar;
			// "PUBPCID\0" (8) + value + "\0" (1): a value of 0x7FE - 9 bytes fills the set exactly.
			fits_jar.set("PUBPCID", std::string(kJoinCdCookieMax - 9, 'a'));
			check(join_cd_cookie(fits_jar).size() == kJoinCdCookieMax,
			      "a CD set of exactly 0x7FE bytes goes whole");
			CookieJar long_jar;
			long_jar.set("PUBPCID", std::string(kJoinCdCookieMax - 8, 'a'));
			check(long_jar.build_prefixed_blob("PUB").size() == kJoinCdCookieMax + 1 &&
			              join_cd_cookie(long_jar).empty(),
			      "a CD set past 0x7FE bytes goes as none");
		}

		// Re-setting a cookie updates in place without duplicating it.
		jar.set("NWHANDLE", "OtherPlayer");
		check(*jar.find("NWHANDLE") == "OtherPlayer", "jar updates a cookie in place");
		size_t count = 0;
		for (const std::string &line : jar.cookie_header_lines()) {
			if (line.rfind("NWHANDLE=", 0) == 0) ++count;
		}
		check(count == 1, "no duplicate NWHANDLE after update");

		// The host key [orig: Network_TruncateIPToSubnet @0x62DFE0]: a host the dotted-quad
		// parse accepts (the `jnz` @0x62DFFF; no end pointer, octets past 255 kept as their
		// low byte) comes back whole; any other is reversed, cut at its second '.' and
		// reversed back, its last two dot-labels; fewer than two dots, unchanged.
		check(subnet_key("192.168.1.1") == "192.168.1.1", "an IPv4 host stays whole");
		check(subnet_key("256.1.1.1") == "256.1.1.1",
		      "an octet past 255 still parses, so the host stays whole");
		check(subnet_key("1.2.3.4x") == "1.2.3.4x",
		      "a tail after the fourth octet is not looked at, so the host stays whole");
		check(subnet_key("nw.novalogic.com") == "novalogic.com",
		      "a DNS host keeps its last two labels");
		check(subnet_key("192.168.1") == "168.1",
		      "a 3-octet string is no IPv4 and keeps its last two labels");
		check(subnet_key(" 1.2.3.4") == "3.4",
		      "a leading space fails the parse, so the last two labels are kept");
		check(subnet_key("nw.novalogic.com.") == "com.",
		      "a trailing dot is the last separator: only 'com.' is kept");
		check(subnet_key("novalogic.com") == "novalogic.com", "one dot, unchanged");
		check(subnet_key("localhost") == "localhost", "no dot, unchanged");
		check(subnet_key("").empty(), "an empty host, unchanged");
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
		std::string body;
		check(build_login_post_body(pub, fields, body), "the login body builds under the issued bundle");
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

	// ---- 4. Params the modexp gate rejects build no body ----
	// The field's encryption fails as a result, not a throw (ADR 0049 d5); the
	// flow refuses such a bundle before it gets here (LobbyHttpFlow's Prepare leg).
	{
		const EpaskParams rejected{0, 0, "k"};
		std::string body = "stale";
		check(!build_login_post_body(rejected, {{"NAME", "ljim", true}}, body) && body.empty(),
		      "an encrypted field under rejected params builds no body");
		check(build_login_post_body(rejected, {{"EPASK", "0:0:k", false}}, body) &&
		          body == "EPASK=0:0:k",
		      "a body with no encrypted field needs no usable params");
	}

	// ---- 5. The session's player name: the browser's CHAR cookie (D-NET-1299) ----
	// The session apply writes the CHAR cookie over the profile's name when it is not empty; the
	// join sends it as its ClientAuth NA (LN S2: retail's in-match NA is the CHAR cookie's value,
	// not NWHANDLE's). The getter finds the name case-insensitively and copies 31 bytes.
	// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - the getter called @0x551A96 with 31
	//  @0x551A7F, the copy @0x551AB0..0x551AC1; CookieJar_GetValueByName @0x64EA90]
	{
		CookieJar jar;
		check(novaworld_character_name(jar).empty(), "a jar with no CHAR names no character");
		jar.set("NWHANDLE", "ljim");
		check(novaworld_character_name(jar).empty(), "NWHANDLE is not the character");
		jar.set("CHAR", "Walker7");
		check(novaworld_character_name(jar) == "Walker7", "the CHAR cookie names the character");
		CookieJar lower;
		lower.set("char", "Walker7");
		check(novaworld_character_name(lower) == "Walker7", "the name is found case-insensitively");
		CookieJar cleared;
		cleared.set("CHAR", "");
		check(novaworld_character_name(cleared).empty(), "an empty CHAR names no character");
		CookieJar thirty;
		thirty.set("CHAR", std::string(30, 'x'));
		check(novaworld_character_name(thirty) == std::string(30, 'x'), "30 characters pass whole");
		CookieJar long_name;
		long_name.set("CHAR", std::string(31, 'y') + "zz");
		check(novaworld_character_name(long_name) == std::string(31, 'y'),
		      "a longer value keeps the 31 bytes the copy takes");
	}

	// consume_activation: the NovaWorld screen's update over the jar's NWACT, written as this
	// machine's activation (the section 9.7 NWACT and fingerprint of docs/titles/dfx2-vs-jo-re.md)
	// and cleared from the jar, a refused write's included; a protocol with no CD key runs none.
	// [orig: UI_ProcessLANSessionStateMachine @0x556ff0 (dfx2.exe) - the read @0x55753e, the empty
	//  test @0x557547, CDKey_WriteActivationFromNWACT called @0x557569, the clear @0x55757e]
	{
		const opennova::cdkey::KeyChains &chains = *opennova::title_protocol("dfx2")->cd_key;
		const char *machine = "ABCDEFGHIJKLMNOPQRSTUVW";
		const std::string nwact =
				"DQNWWKWDHZJHKFVXIHNJKEKYNSFFRIPKCOWFBPZIRXDVUSBLDCQDIQMRPDVQLBCDOPZRAYEREPP"
				"LAQYUYTMKDPRRYKCYXQGRVNIDZDVWMBTYOWSPQWUNOGWSNYIWCUOQJILP";
		const std::string wire = "BMCCDKDKDLFFNGFDJHMFMMALJCLEFPADLPHHDHEEDHOGCIDLKC";
		std::string stored;
		bool refuse = false;
		int writes = 0;
		opennova::cdkey::KeyStore store;
		store.read = [&stored](std::string &out) {
			out = stored;
			return !stored.empty();
		};
		store.write = [&](const std::string &text) {
			++writes;
			if (refuse)
				return false;
			stored = text;
			return true;
		};
		opennova::cdkey::KeyInfo info;
		CookieJar jar;
		check(!consume_activation(jar, info, &chains, machine, store) && writes == 0,
				"no NWACT, no activation");
		jar.set("NWACT", nwact);
		check(!consume_activation(jar, info, nullptr, machine, store) &&
						*jar.find("NWACT") == nwact,
				"a protocol with no CD key leaves the jar's NWACT");
		check(consume_activation(jar, info, &chains, machine, store), "the NWACT is consumed");
		check(stored == nwact && info.status == 1 && info.keys.cdkiid == wire,
				"it is stored and read back to the wire key");
		check(info.keys.exp1.empty(), "no CDKIIDEXP1: dfx2.exe never writes one");
		check(jar.find("NWACT") != nullptr && jar.find("NWACT")->empty(),
				"the jar's NWACT is cleared");
		check(!consume_activation(jar, info, &chains, machine, store), "a cleared NWACT is none");
		CookieJar lower;
		lower.set("nwact", nwact);
		check(consume_activation(lower, info, &chains, machine, store),
				"the NWACT is found case-insensitively");
		// No administrator: both writes refused, -1, the record's key as it was, the jar cleared.
		refuse = true;
		writes = 0;
		jar.set("NWACT", nwact);
		check(consume_activation(jar, info, &chains, machine, store) && info.status == -1 &&
						writes == 2 && info.keys.cdkiid == wire && jar.find("NWACT")->empty(),
				"a refused write leaves the key and still clears the jar");
	}

	if (g_failures == 0) {
		std::printf("http_login: all checks passed\n");
		return 0;
	}
	std::printf("http_login: %d check(s) failed\n", g_failures);
	return 1;
}
