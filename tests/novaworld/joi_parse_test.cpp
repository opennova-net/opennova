// ADR 0010 Phase 5 (join leg) — parse the NWJoin.dll `.joi` connection string.
//
// The join page carries the host address + tokens in its <TITLE>, e.g.
//   [NK=<enc>&CK=<enc>&NI=127.0.0.1&NP=64206&BK=986119&]
// This pins that parse_joi_connection_string extracts the plaintext host
// address (NI/NP) and that the url_cipher-encoded NK round-trips to the same
// host:port through url_cipher_decode (the keys the server's NWJoin handler
// encodes with). Server side: apps/novaworld_server/http_listener.cpp:1537-1576
// + templates/jop_2_join.joi.

#include <novacrypto/url_cipher.h>
#include <novaworld/http_login.h>

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
	    "[NK=" + nk + "&CK=" + ck + "&NI=" + host_ip + "&NP=" + host_port + "&BK=986119&]\n"
	    "</TITLE></HEAD><BODY>Joining DEV Joinable...</BODY></HTML>";

	const JoiConnection conn = parse_joi_connection_string(body);

	check(conn.ok, "parse reports ok (NI + NP present)");
	check(conn.ni == host_ip, "NI is the plaintext host ip");
	check(conn.np == host_port, "NP is the plaintext host port");
	check(conn.bk == "986119", "BK is the literal 986119");
	check(conn.nk == nk, "NK token captured verbatim");
	check(conn.ck == ck, "CK token captured verbatim");

	// The encoded NK decodes back to host:port (cross-check / real-NW path).
	check(url_cipher_decode(conn.nk, URL_CIPHER_KEY_NK) == nk_plain,
	      "url_cipher_decode(NK) recovers host:port");

	// A body without a bracketed run yields ok == false (don't crash / misparse).
	const JoiConnection empty = parse_joi_connection_string("<HTML>no title here</HTML>");
	check(!empty.ok, "missing connection string -> ok == false");

	if (g_failures == 0) {
		std::printf("joi_parse: all checks passed\n");
		return 0;
	}
	std::printf("joi_parse: %d check(s) failed\n", g_failures);
	return 1;
}
