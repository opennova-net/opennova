// ADR 0010 Phase 5 (join leg) — parse the NWJoin.dll `.joi` connection string.
//
// The join page carries the host address + tokens in its <TITLE>, e.g.
//   [NK=<enc>&CK=<enc>&NI=127.0.0.1&NP=64206&BK=986119&]
// This pins that parse_joi_connection_string captures NI/NP but selects the
// actual dial endpoint from decoded NK. NI/NP are proxy/display slots and can
// disagree with NK on live NovaWorld joins.

#include <net/novacrypto/url_cipher.h>
#include <net/novaworld/http_login.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const char *what) {
	if (!cond) {
		std::printf("  FAIL: %s\n", what);
		++g_failures;
	}
}

} // namespace

int main() {
	using namespace opennova;

	const std::string host_ip = "127.0.0.1";
	const std::string host_port = "64206";
	const std::string proxy_ip = "10.99.88.77";
	const std::string proxy_port = "12345";
	const std::string nk_plain = host_ip + ":" + host_port;  // "127.0.0.1:64206"
	const std::string app_id = "20";

	// Encode NK/CK the same way the server's NWJoin handler does (url_cipher,
	// NK/CK keys). The plaintext is shorter than the 21-byte key, so the modular
	// url_cipher_encode and the server's capped encode_token agree byte-for-byte.
	const std::string nk = url_cipher_encode(nk_plain, URL_CIPHER_KEY_NK);
	const std::string ck = url_cipher_encode(app_id, URL_CIPHER_KEY_CK);

	// Reproduce the rendered jop_2_join.joi <TITLE> line, surrounding whitespace
	// and all (the template wraps the bracket in newlines).
	const std::string body =
	    "<HTML><HEAD><TITLE>\n"
	    "[NK=" + nk + "&CK=" + ck + "&NI=" + proxy_ip + "&NP=" + proxy_port + "&BK=986119&LN=3&GS=sess-9&]\n"
	    "</TITLE></HEAD><BODY>Joining DEV Joinable...</BODY></HTML>";

	const JoiConnection conn = parse_joi_connection_string(body);

	check(conn.ok, "parse reports ok (decoded NK endpoint present)");
	check(conn.host_ip == host_ip, "decoded NK host ip selected for dialing");
	check(conn.host_port == host_port, "decoded NK host port selected for dialing");
	check(conn.ni == proxy_ip, "NI plaintext proxy/display slot preserved");
	check(conn.np == proxy_port, "NP plaintext proxy/display slot preserved");
	check(conn.bk == "986119", "BK is the literal 986119");
	// [orig: LN `atol` @0x54e33e; GS copied @0x54e38a]
	check(conn.ln == 3, "LN parses as the lobby number");
	check(conn.gs == "sess-9", "GS preserved verbatim");
	check(conn.nk == nk, "NK token captured verbatim");
	check(conn.ck == ck, "CK token captured verbatim");
	// CK decodes to the decimal the retail client atol()s and sends as the
	// ClientAuth APPID conn-tag; a NovaWorld host validates it (reject code 9).
	// app_id "20" decodes cleanly, so app_id == "20". [orig: net_config.bt =
	// atol(decoded CK) @0x569b8e serialized as APPID -> Server_ValidatePlayer
	// JoinRequest @0x512100 @0x5122c5; wire-witnessed stock CU APPID 2026-08-31]
	check(conn.app_id == "20", "APPID is atol(decoded CK) — the game-session join token");

	// The encoded NK decodes back to host:port (cross-check / real-NW path).
	check(url_cipher_decode(conn.nk, URL_CIPHER_KEY_NK) == nk_plain,
	      "url_cipher_decode(NK) recovers host:port");

	// A body without a bracketed run yields ok == false (don't crash / misparse).
	const JoiConnection empty = parse_joi_connection_string("<HTML>no title here</HTML>");
	check(!empty.ok, "missing connection string -> ok == false");
	check(empty.ln == 0 && empty.gs.empty(), "LN/GS default to 0/empty");

	// NK is the only dial authority: retail's parser returns 0 when the string has no
	// "NK=" at all (no PlaySetup, no dial), whatever NI/NP carry; NI/NP only feed the
	// proxy rendezvous. [orig: URL_ParseConnectionQueryString @0x54e13c strstr "NK=" ->
	// jz @0x54e146 (return 0); the event-7 caller UI_DispatchScreenEvent tests the result]
	const JoiConnection no_nk = parse_joi_connection_string(
	    "<TITLE>[CK=" + ck + "&NI=" + proxy_ip + "&NP=" + proxy_port + "&BK=986119&]</TITLE>");
	check(!no_nk.ok, "a .joi without NK is no join (NI/NP are not a dial fallback)");
	check(no_nk.host_ip.empty() && no_nk.host_port.empty(),
	      "without NK no dial endpoint is recovered from NI/NP");

	if (g_failures == 0) {
		std::printf("joi_parse: all checks passed\n");
		return 0;
	}
	std::printf("joi_parse: %d check(s) failed\n", g_failures);
	return 1;
}
