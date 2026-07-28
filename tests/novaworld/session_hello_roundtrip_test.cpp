// Roundtrip tests for the client-side serializers added in Phase A:
//   client_hello_to_bytes / parse_client_hello
//   client_auth_to_bytes  / parse_client_auth
// Plus focused wire-gating checks for server_hello_to_bytes.
//
// Builds a populated struct, serializes, parses the bytes back, then asserts
// every field matches. Catches drift in TLV ordering / size encoding so the
// Godot client and the standalone server stay byte-compatible without us
// having to launch retail every iteration.

#include <npwire/session_hello.h>

#include "../common/test_expect.h"

#include <cstdio>
#include <cstring>
#include <string>

using opennova::ClientAuth;
using opennova::ClientHello;
using opennova::ServerAuth;
using opennova::ServerHello;
using opennova::client_auth_to_bytes;
using opennova::client_hello_to_bytes;
using opennova::parse_client_auth;
using opennova::parse_client_hello;
using opennova::parse_server_auth;
using opennova::parse_server_hello;
using opennova::server_auth_to_bytes;
using opennova::server_hello_to_bytes;

namespace {

bool has_tlv_field(const std::vector<uint8_t> &bytes, const std::string &wanted) {
	size_t pos = 0;
	while (pos < bytes.size()) {
		size_t name_end = pos;
		while (name_end < bytes.size() && bytes[name_end] != 0) ++name_end;
		if (name_end + 2 >= bytes.size()) return false;

		const std::string name(reinterpret_cast<const char *>(bytes.data() + pos), name_end - pos);
		const uint16_t value_size = static_cast<uint16_t>(bytes[name_end + 1]) |
		                            (static_cast<uint16_t>(bytes[name_end + 2]) << 8);
		const size_t next = name_end + 3 + value_size;
		if (next > bytes.size()) return false;
		if (name == wanted) return true;
		pos = next;
	}
	return false;
}

int test_client_hello_roundtrip() {
	ClientHello src;
	src.nvs  = "OpenNova Godot Client 0.1";
	src.co   = "OpenNova";
	src.ap   = "OpennovaGodotClient.exe";
	src.bdat = "Apr 27 2026 00:00:00";
	src.pn   = "NOVAWORLDUDP";
	src.pv1  = "0.0.0 2/10/2004 EM";
	src.pv2  = "1";
	src.ci   = 0xCAFEBABEu;
	src.eip  = 0x7F000001u;
	src.epn  = 32768;
	for (int i = 0; i < 16; ++i) src.pg[i] = static_cast<uint8_t>(i * 17);
	src.pg_present = true;

	auto bytes = client_hello_to_bytes(src);
	TEST_EXPECT(!bytes.empty());

	ClientHello round;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), round));
	TEST_EXPECT(round.nvs  == src.nvs);
	TEST_EXPECT(round.co   == src.co);
	TEST_EXPECT(round.ap   == src.ap);
	TEST_EXPECT(round.bdat == src.bdat);
	TEST_EXPECT(round.pn   == src.pn);
	TEST_EXPECT(round.pv1  == src.pv1);
	TEST_EXPECT(round.pv2  == src.pv2);
	TEST_EXPECT(round.ci   == src.ci);
	TEST_EXPECT(round.eip  == src.eip);
	TEST_EXPECT(round.epn  == src.epn);
	TEST_EXPECT(round.pg_present);
	TEST_EXPECT(std::memcmp(round.pg.data(), src.pg.data(), 16) == 0);
	return 0;
}

int test_client_hello_minimal() {
	// Only the required PN field; everything else default. Parser should
	// still accept it (PN-only is the minimal valid HELLO).
	ClientHello src;
	src.pn = "NOVAWORLDUDP";

	auto bytes = client_hello_to_bytes(src);
	ClientHello round;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), round));
	TEST_EXPECT(round.pn == "NOVAWORLDUDP");
	TEST_EXPECT(round.nvs.empty());
	TEST_EXPECT(round.ci == 0);
	return 0;
}

int test_client_auth_roundtrip() {
	ClientAuth src;
	src.ci   = 0xCAFEBABEu;
	src.hk   = 0x0FE0E112u;
	src.ck   = 0xDEADBEEFu;
	src.na   = "jop:cus2";
	src.sip  = 0x7F000001u;
	src.spn  = 32768;
	src.scrk = "abcdefghijklmnopqrstuvwxyz1234567890ABCDEFGHIJKLMNOPQRSTUVWXY";
	src.cu.push_back(std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04});
	src.cu.push_back(std::vector<uint8_t>{0xFF, 0xEE});

	auto bytes = client_auth_to_bytes(src);
	TEST_EXPECT(!bytes.empty());

	ClientAuth round;
	TEST_EXPECT(parse_client_auth(bytes.data(), bytes.size(), round));
	TEST_EXPECT(round.ci   == src.ci);
	TEST_EXPECT(round.hk   == src.hk);
	TEST_EXPECT(round.ck   == src.ck);
	TEST_EXPECT(round.na   == src.na);
	TEST_EXPECT(round.sip  == src.sip);
	TEST_EXPECT(round.spn  == src.spn);
	TEST_EXPECT(round.scrk == src.scrk);
	TEST_EXPECT(round.cu.size() == 2);
	TEST_EXPECT(round.cu[0] == src.cu[0]);
	TEST_EXPECT(round.cu[1] == src.cu[1]);
	return 0;
}

int test_client_auth_minimum_for_acceptance() {
	// parse_client_auth requires ck != 0; everything else can be defaulted.
	ClientAuth src;
	src.ci = 1;
	src.ck = 0xCAFE0001u;

	auto bytes = client_auth_to_bytes(src);
	ClientAuth round;
	TEST_EXPECT(parse_client_auth(bytes.data(), bytes.size(), round));
	TEST_EXPECT(round.ci == 1);
	TEST_EXPECT(round.ck == 0xCAFE0001u);
	return 0;
}

int test_server_auth_rejection_roundtrip() {
	ServerAuth src;
	src.ci = 7;
	src.ck = 0x12345678u;
	src.cr = 0;
	src.jfc = 19;
	src.jfp = 2;
	src.jfs = "side password rejected";

	auto bytes = server_auth_to_bytes(src);
	TEST_EXPECT(!bytes.empty());

	ServerAuth round;
	TEST_EXPECT(parse_server_auth(bytes.data(), bytes.size(), round));
	TEST_EXPECT(round.cr == 0);
	TEST_EXPECT(round.jfc == 19);
	TEST_EXPECT(round.jfp == 2);
	TEST_EXPECT(round.jfs == "side password rejected");
	TEST_EXPECT(round.scrk.empty());
	return 0;
}

int test_server_hello_ut_is_nonzero_gated() {
	ServerHello hello;
	hello.ut = 0;
	TEST_EXPECT(!has_tlv_field(server_hello_to_bytes(hello), "UT"));

	hello.ut = 0x12345678u;
	TEST_EXPECT(has_tlv_field(server_hello_to_bytes(hello), "UT"));
	return 0;
}

int test_server_hello_omitted_metadata_parses_empty() {
	ServerHello hello;
	hello.p1 = 0;
	hello.p2 = 0;
	hello.np = 0;
	hello.mp = 0;
	hello.sus1.clear();
	hello.sus2.clear();

	const auto bytes = server_hello_to_bytes(hello);
	TEST_EXPECT(!has_tlv_field(bytes, "P1"));
	TEST_EXPECT(!has_tlv_field(bytes, "P2"));
	TEST_EXPECT(!has_tlv_field(bytes, "NP"));
	TEST_EXPECT(!has_tlv_field(bytes, "MP"));
	TEST_EXPECT(!has_tlv_field(bytes, "SUS1"));
	TEST_EXPECT(!has_tlv_field(bytes, "SUS2"));

	ServerHello parsed;
	TEST_EXPECT(parse_server_hello(bytes.data(), bytes.size(), parsed));
	TEST_EXPECT(parsed.p1 == 0);
	TEST_EXPECT(parsed.p2 == 0);
	TEST_EXPECT(parsed.np == 0);
	TEST_EXPECT(parsed.mp == 0);
	TEST_EXPECT(parsed.sus1.empty());
	TEST_EXPECT(parsed.sus2.empty());
	TEST_EXPECT(parsed.pl.empty());
	return 0;
}

} // namespace

int main() {
	if (test_client_hello_roundtrip() != 0) return 1;
	if (test_client_hello_minimal() != 0) return 1;
	if (test_client_auth_roundtrip() != 0) return 1;
	if (test_client_auth_minimum_for_acceptance() != 0) return 1;
	if (test_server_auth_rejection_roundtrip() != 0) return 1;
	if (test_server_hello_ut_is_nonzero_gated() != 0) return 1;
	if (test_server_hello_omitted_metadata_parses_empty() != 0) return 1;
	std::printf("OK: session hello/auth serializer roundtrip and gating\n");
	return 0;
}
