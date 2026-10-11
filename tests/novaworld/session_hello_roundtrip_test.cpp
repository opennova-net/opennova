// Roundtrip tests for the client-side serializers added in Phase A:
//   client_hello_to_bytes / parse_client_hello
//   client_auth_to_bytes  / parse_client_auth
// Plus focused wire-gating checks for server_hello_to_bytes.
//
// Builds a populated struct, serializes, parses the bytes back, then asserts
// every field matches. Catches drift in TLV ordering / size encoding so the
// Godot client and the standalone server stay byte-compatible without us
// having to launch retail every iteration.

#include <net/npwire/session_hello.h>
#include <net/npwire/session_ping.h>

#include "../common/test_expect.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// One flat-TLV field as a stock peer writes it: name, NUL, u16 size, value.
void append_field(std::vector<uint8_t> &out, const char *name, const std::vector<uint8_t> &value) {
	out.insert(out.end(), name, name + std::strlen(name) + 1);
	out.push_back(static_cast<uint8_t>(value.size()));
	out.push_back(static_cast<uint8_t>(value.size() >> 8));
	out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> cstr_value(const std::string &s) {
	std::vector<uint8_t> v(s.begin(), s.end());
	v.push_back(0);
	return v;
}

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

std::vector<std::string> tlv_field_names(const std::vector<uint8_t> &bytes) {
	std::vector<std::string> names;
	size_t pos = 0;
	while (pos < bytes.size()) {
		size_t name_end = pos;
		while (name_end < bytes.size() && bytes[name_end] != 0) ++name_end;
		if (name_end + 2 >= bytes.size()) return {};
		const uint16_t value_size = static_cast<uint16_t>(bytes[name_end + 1]) |
		                            (static_cast<uint16_t>(bytes[name_end + 2]) << 8);
		const size_t next = name_end + 3 + value_size;
		if (next > bytes.size()) return {};
		names.emplace_back(
				reinterpret_cast<const char *>(bytes.data() + pos), name_end - pos);
		pos = next;
	}
	return names;
}

int test_client_hello_roundtrip() {
	ClientHello src;
	src.nvs  = "OpenNova Godot Client 0.1";
	src.co   = "OpenNova";
	src.ap   = "OpennovaGodotClient.exe";
	src.bdat = "Apr 27 2026 00:00:00";
	src.de   = 0x10203040u;
	src.pn   = "NOVAWORLDUDP";
	src.pv1  = "0.0.0 2/10/2004 EM";
	src.pv2  = "1";
	src.pv3  = "third-version";
	src.ci   = 0xCAFEBABEu;
	src.pm   = 7;
	src.eip  = 0x7F000001u;
	src.epn  = 32768;
	src.et   = 0x55667788u;
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
	TEST_EXPECT(round.de   == src.de);
	TEST_EXPECT(round.pn   == src.pn);
	TEST_EXPECT(round.pv1  == src.pv1);
	TEST_EXPECT(round.pv2  == src.pv2);
	TEST_EXPECT(round.pv3  == src.pv3);
	TEST_EXPECT(round.ci   == src.ci);
	TEST_EXPECT(round.pm   == src.pm);
	TEST_EXPECT(round.eip  == src.eip);
	TEST_EXPECT(round.epn  == src.epn);
	TEST_EXPECT(round.et   == src.et);
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
	TEST_EXPECT(!has_tlv_field(bytes, "DE"));
	TEST_EXPECT(!has_tlv_field(bytes, "PV3"));
	TEST_EXPECT(!has_tlv_field(bytes, "CI"));
	TEST_EXPECT(!has_tlv_field(bytes, "PM"));
	TEST_EXPECT(!has_tlv_field(bytes, "EIP"));
	TEST_EXPECT(!has_tlv_field(bytes, "EPN"));
	TEST_EXPECT(!has_tlv_field(bytes, "ET"));

	// PM is written only for a NONZERO count: retail's announce builder has no
	// "present but zero" state [orig: NapiNPSession_SendAnnouncePacket
	// @0x61fa00 @0x61fcca/@0x61fcda], and the reader keeps no presence marker,
	// so a PM omitted at zero parses as 0 [orig: NapiNPProtocol_HandleClientHello
	// @0x6213B0 — PM dword @0x62173C, zero-initialised @0x621504].
	TEST_EXPECT(!has_tlv_field(bytes, "PM"));
	ClientHello zero_pm;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), zero_pm));
	TEST_EXPECT(zero_pm.pm == 0);
	src.pm = 3;
	bytes = client_hello_to_bytes(src);
	TEST_EXPECT(has_tlv_field(bytes, "PM"));
	ClientHello with_pm;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), with_pm));
	TEST_EXPECT(with_pm.pm == 3);

	// No tag is required to parse: a PM-only announce (no PN at all) is a
	// valid hello whose admission is decided by client_hello_admits.
	ClientHello pm_only;
	pm_only.pm = 1;
	bytes = client_hello_to_bytes(pm_only);
	ClientHello parsed_pm_only;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), parsed_pm_only));
	TEST_EXPECT(parsed_pm_only.pn.empty() && parsed_pm_only.pm == 1);
	TEST_EXPECT(!parse_client_hello(nullptr, 0, parsed_pm_only));
	return 0;
}

// [orig: NapiNPProtocol_HandleClientHello @0x6213b0 — the NVS/PN/PG/PV1
//  compares run only under `if (!protocol_version)`; jnz @0x6217c2 jumps a
//  nonzero PM straight to the ServerInfo reply @0x6218fc]
int test_client_hello_admits_nonzero_pm_without_identity() {
	ClientHello retail = opennova::make_jointoperations_client_hello(5);
	TEST_EXPECT(opennova::client_hello_admits(retail));

	ClientHello foreign;
	foreign.pn = "SOMEOTHERGAME";
	foreign.pv1 = "9.9.9";
	TEST_EXPECT(!opennova::client_hello_admits(foreign));
	foreign.pm = 2;
	TEST_EXPECT(opennova::client_hello_admits(foreign));

	ClientHello bare;
	bare.pm = 1;
	TEST_EXPECT(opennova::client_hello_admits(bare));
	bare.pm = 0;
	TEST_EXPECT(!opennova::client_hello_admits(bare));
	return 0;
}

// [orig: HandleClientJoin @0x62b750 and NapiNP_HandleServerJoinResponse
//  @0x629840 compare every tag through Napi_StrCaseEqual @0x616e70]
int test_client_and_server_auth_tag_names_are_case_insensitive() {
	ClientAuth src;
	src.ci = 4;
	src.hk = 0x0FE0E112u;
	src.ck = 0xDEADBEEFu;
	src.na = "jop:cus2";
	src.scrk = "KEY";
	auto bytes = client_auth_to_bytes(src);
	// Lower-case every tag NAME in a flat-TLV run ([name\0][LE16][value]),
	// leaving the values alone.
	auto lower_tags = [](std::vector<uint8_t> in) {
		size_t pos = 0;
		while (pos < in.size()) {
			size_t name_end = pos;
			while (name_end < in.size() && in[name_end] != 0) {
				if (in[name_end] >= 'A' && in[name_end] <= 'Z') in[name_end] = static_cast<uint8_t>(in[name_end] + 32);
				++name_end;
			}
			if (name_end + 2 >= in.size()) break;
			const uint16_t sz = static_cast<uint16_t>(in[name_end + 1]) |
			                    (static_cast<uint16_t>(in[name_end + 2]) << 8);
			pos = name_end + 3 + sz;
		}
		return in;
	};
	const auto lowered = lower_tags(bytes);
	ClientAuth round;
	TEST_EXPECT(parse_client_auth(lowered.data(), lowered.size(), round));
	TEST_EXPECT(round.ci == 4 && round.hk == 0x0FE0E112u && round.ck == 0xDEADBEEFu);
	TEST_EXPECT(round.na == "jop:cus2" && round.scrk == "KEY");

	ServerAuth sa;
	sa.ci = 4;
	sa.ck = 0xDEADBEEFu;
	sa.sk = 0x12345678u;
	sa.scrk = "SERVERKEY";
	sa.rip = 0x7F000001u;
	const auto sa_lowered = lower_tags(server_auth_to_bytes(sa));
	ServerAuth sa_round;
	TEST_EXPECT(parse_server_auth(sa_lowered.data(), sa_lowered.size(), sa_round));
	TEST_EXPECT(sa_round.sk == 0x12345678u && sa_round.scrk == "SERVERKEY" && sa_round.rip == 0x7F000001u);

	ServerHello sh;
	sh.hk = 0xABCDEF01u;
	sh.sn = "Host";
	const auto sh_lowered = lower_tags(server_hello_to_bytes(sh));
	ServerHello sh_round;
	TEST_EXPECT(parse_server_hello(sh_lowered.data(), sh_lowered.size(), sh_round));
	TEST_EXPECT(sh_round.hk == 0xABCDEF01u && sh_round.sn == "Host");
	return 0;
}

// [orig: CNapiNetwork_Init @0x4caa53..0x4caa76 (0x4000 ceiling, the signed
//  `jge` @0x4caa66); CNapiGameSession_InitNPConnection @0x4d3df4..0x4d3e17
//  (0x10000 ceiling, the signed `jge` @0x4d3e07)]
int test_cs_field13_follows_the_mpmaxpacketsize_clamp() {
	auto field13 = [](const std::vector<opennova::CsField> &cs) {
		for (const auto &f : cs) if (f.field_index == 13) return f.value;
		return 0u;
	};
	TEST_EXPECT(field13(opennova::jointoperations_cs_fields()) == 1300u);
	TEST_EXPECT(field13(opennova::jointoperations_cs_fields(0)) == 1300u);
	TEST_EXPECT(field13(opennova::jointoperations_cs_fields(50)) == 100u);
	TEST_EXPECT(field13(opennova::jointoperations_cs_fields(4000)) == 4000u);
	TEST_EXPECT(field13(opennova::jointoperations_cs_fields(20000)) == 16384u);
	// The ladder compares SIGNED: a negative value lands on the floor.
	TEST_EXPECT(field13(opennova::jointoperations_cs_fields(-5)) == 100u);
	TEST_EXPECT(field13(opennova::novaworld_service_cs_fields()) == 1300u);
	TEST_EXPECT(field13(opennova::novaworld_service_cs_fields(20000)) == 20000u);
	TEST_EXPECT(field13(opennova::novaworld_service_cs_fields(70000)) == 65536u);
	TEST_EXPECT(field13(opennova::novaworld_service_cs_fields(7)) == 100u);
	TEST_EXPECT(field13(opennova::novaworld_service_cs_fields(-5)) == 100u);
	return 0;
}

// A dword field shorter than four bytes loads a whole dword from its value
// pointer, so it takes the bytes that follow it in the datagram; past the
// datagram's end ours reads zero (D-NET-410).
// [orig: NapiNPProtocol_HandleClientHello @0x6213B0 - CI @0x62171E, PM @0x62173C,
//  EIP @0x621756, EPN @0x621774, ET @0x621792; NapiNP_ReadTLV @0x61DBE0 hands
//  back the value pointer for any length @0x61DCF7]
int test_client_hello_short_dword_takes_the_following_bytes() {
	// A two-byte CI, then a one-byte zero PM, then a four-byte EIP.
	std::vector<uint8_t> bytes;
	append_field(bytes, "CI", {0x05, 0x00});
	append_field(bytes, "PM", {0x00});
	append_field(bytes, "EIP", {0x11, 0x22, 0x33, 0x44});
	ClientHello parsed;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), parsed));
	// CI: its two bytes, then the PM field's name 'P' 'M'.
	TEST_EXPECT(parsed.ci == (0x05u | (uint32_t('P') << 16) | (uint32_t('M') << 24)));
	// PM: its zero byte, then 'E' 'I' 'P' of the next name: nonzero, so the
	// identity check is skipped [orig: @0x6217C2].
	TEST_EXPECT(parsed.pm ==
			((uint32_t('E') << 8) | (uint32_t('I') << 16) | (uint32_t('P') << 24)));
	TEST_EXPECT(opennova::client_hello_admits(parsed));
	TEST_EXPECT(parsed.eip == 0x44332211u);

	// A zero-length field still loads four bytes: here the next field's name and
	// its NUL, "EPN\0".
	bytes.clear();
	append_field(bytes, "ET", {});
	append_field(bytes, "EPN", {0x34, 0x12});
	ClientHello empty_value;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), empty_value));
	TEST_EXPECT(empty_value.et ==
			(uint32_t('E') | (uint32_t('P') << 8) | (uint32_t('N') << 16)));
	// The last field: its two bytes, then the two past the datagram's end,
	// which ours reads as zero (retail's stack decrypt buffer, D-NET-410).
	TEST_EXPECT(empty_value.epn == 0x1234u);
	return 0;
}

// The 0x42's dword fields load four bytes whatever their length, as the 0x41's
// do: a short value takes the bytes that follow it, zero past the datagram's
// end (D-NET-410).
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - CI @0x62BAE7, HK @0x62BB08,
//  CK @0x62BB29, SIP @0x62BBA9, SPN @0x62BBCA, NF @0x62BC6B, DCNT @0x62BC89,
//  RCNT @0x62BCA7]
int test_client_auth_short_dword_takes_the_following_bytes() {
	std::vector<uint8_t> bytes;
	append_field(bytes, "CK", {0xEF, 0xBE, 0xAD, 0xDE});
	append_field(bytes, "HK", {0x12, 0x34});
	append_field(bytes, "CI", {0x07});
	append_field(bytes, "SIP", {0x01, 0x02, 0x03, 0x04});
	append_field(bytes, "NF", {});
	append_field(bytes, "DCNT", {0x02, 0x00, 0x00, 0x00});
	append_field(bytes, "RCNT", {0x05});
	ClientAuth parsed;
	TEST_EXPECT(parse_client_auth(bytes.data(), bytes.size(), parsed));
	TEST_EXPECT(parsed.ck == 0xDEADBEEFu);
	// HK: its two bytes, then the CI field's name.
	TEST_EXPECT(parsed.hk == (0x3412u | (uint32_t('C') << 16) | (uint32_t('I') << 24)));
	// CI: its byte, then 'S' 'I' 'P'.
	TEST_EXPECT(parsed.ci ==
			(0x07u | (uint32_t('S') << 8) | (uint32_t('I') << 16) | (uint32_t('P') << 24)));
	TEST_EXPECT(parsed.sip == 0x04030201u);
	// NF, empty: the next field's name.
	TEST_EXPECT(parsed.nf == (uint32_t('D') | (uint32_t('C') << 8) | (uint32_t('N') << 16) |
			(uint32_t('T') << 24)));
	TEST_EXPECT(parsed.dcnt == 2u);
	// RCNT, the last field: its byte, then zero past the end.
	TEST_EXPECT(parsed.rcnt == 5u);
	return 0;
}

// A PG of any length gives the 16 bytes at its value pointer, in the 0x41 and
// the 0x42 alike: a 15-byte PG followed by a field named "hk" takes the 'h'
// (0x68, the JO GUID's last byte) and passes the identity; a 17-byte PG
// compares its first 16. An absent PG still fails it (retail's block stays
// zeroed). [orig: NapiNPProtocol_HandleClientHello @0x62165E..0x621672;
//  NapiNPProtocol_HandleClientJoin @0x62BA24..0x62BA38; NapiVarBlock_Init
//  @0x62E710]
int test_pg_of_any_length_loads_sixteen_bytes() {
	const opennova::ClientHello retail = opennova::make_jointoperations_client_hello(3);
	const std::vector<uint8_t> guid(retail.pg.begin(), retail.pg.end());
	TEST_EXPECT(guid[15] == 'h');
	const auto identity = [&retail](std::vector<uint8_t> &out) {
		append_field(out, "NVS", cstr_value(retail.nvs));
		append_field(out, "PN", cstr_value(retail.pn));
	};

	std::vector<uint8_t> short_pg;
	identity(short_pg);
	append_field(short_pg, "PG", std::vector<uint8_t>(guid.begin(), guid.begin() + 15));
	append_field(short_pg, "hk", {0x44, 0x33, 0x22, 0x11});
	append_field(short_pg, "PV1", cstr_value(retail.pv1));
	ClientHello hello;
	TEST_EXPECT(parse_client_hello(short_pg.data(), short_pg.size(), hello));
	TEST_EXPECT(hello.pg_present && hello.pg == retail.pg);
	TEST_EXPECT(opennova::matches_jointoperations_identity(hello));
	ClientAuth auth;
	append_field(short_pg, "CK", {0x01, 0x00, 0x00, 0x00});
	TEST_EXPECT(parse_client_auth(short_pg.data(), short_pg.size(), auth));
	TEST_EXPECT(auth.pg_present && auth.pg == retail.pg && auth.hk == 0x11223344u);
	TEST_EXPECT(opennova::matches_jointoperations_identity(auth));

	std::vector<uint8_t> long_pg;
	identity(long_pg);
	std::vector<uint8_t> seventeen = guid;
	seventeen.push_back(0xFF);
	append_field(long_pg, "PG", seventeen);
	append_field(long_pg, "PV1", cstr_value(retail.pv1));
	append_field(long_pg, "CK", {0x01, 0x00, 0x00, 0x00});
	TEST_EXPECT(parse_client_hello(long_pg.data(), long_pg.size(), hello));
	TEST_EXPECT(hello.pg == retail.pg && opennova::matches_jointoperations_identity(hello));
	TEST_EXPECT(parse_client_auth(long_pg.data(), long_pg.size(), auth));
	TEST_EXPECT(auth.pg == retail.pg && opennova::matches_jointoperations_identity(auth));

	std::vector<uint8_t> no_pg;
	identity(no_pg);
	append_field(no_pg, "PV1", cstr_value(retail.pv1));
	TEST_EXPECT(parse_client_hello(no_pg.data(), no_pg.size(), hello));
	TEST_EXPECT(!hello.pg_present && !opennova::matches_jointoperations_identity(hello));
	append_field(no_pg, "CK", {0x01, 0x00, 0x00, 0x00});
	TEST_EXPECT(parse_client_auth(no_pg.data(), no_pg.size(), auth));
	TEST_EXPECT(!auth.pg_present && !opennova::matches_jointoperations_identity(auth));
	return 0;
}

// The 0x41 / 0x42 string fields copy from the value pointer up to the first
// NUL, at most their buffer less one (NVS 127, PW 511, the rest 63), whatever
// the field's length: an inner NUL ends the string, a value without its NUL
// runs on into the next field's name and stops at that name's NUL, and the last
// field's copy stops at the datagram's end (zero past it, D-NET-410).
// [orig: Napi_CopyString @0x617E10; NapiNPProtocol_HandleClientHello @0x6213B0 -
//  NVS @0x621570, CO @0x62159E, PN @0x62163B, PV1 @0x62169F;
//  NapiNPProtocol_HandleClientJoin @0x62B750 - NVS @0x62B933, CO @0x62B961, PN
//  @0x62BA01, PV1 @0x62BA68, NA @0x62BB55, PW @0x62BB86, SCRK @0x62BC4B]
int test_hello_and_auth_strings_copy_to_the_first_nul() {
	const ClientHello retail = opennova::make_jointoperations_client_hello(3);
	const std::vector<uint8_t> guid(retail.pg.begin(), retail.pg.end());
	const auto bare = [](const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); };

	// An inner NUL ends PN and PV1, so the identity holds in the 0x41 and the 0x42.
	std::vector<uint8_t> inner;
	append_field(inner, "NVS", cstr_value(retail.nvs));
	std::vector<uint8_t> pn = cstr_value(retail.pn);
	pn.push_back('X');
	pn.push_back(0);
	append_field(inner, "PN", pn);
	append_field(inner, "PG", guid);
	std::vector<uint8_t> pv1 = cstr_value(retail.pv1);
	pv1.push_back('Y');
	append_field(inner, "PV1", pv1);
	ClientHello hello;
	TEST_EXPECT(parse_client_hello(inner.data(), inner.size(), hello));
	TEST_EXPECT(hello.pn == retail.pn && hello.pv1 == retail.pv1);
	TEST_EXPECT(opennova::matches_jointoperations_identity(hello));
	ClientAuth auth;
	TEST_EXPECT(parse_client_auth(inner.data(), inner.size(), auth));
	TEST_EXPECT(auth.pn == retail.pn && auth.pv1 == retail.pv1);
	TEST_EXPECT(opennova::matches_jointoperations_identity(auth));

	// A value without its NUL runs on into the next field's name; the last
	// field's stops at the datagram's end.
	std::vector<uint8_t> run_on;
	append_field(run_on, "CO", bare("Logic"));
	append_field(run_on, "AP", cstr_value("Jointops.exe"));
	append_field(run_on, "NA", bare("Player"));
	append_field(run_on, "PW", cstr_value("pw"));
	append_field(run_on, "SCRK", bare("KEY"));
	TEST_EXPECT(parse_client_hello(run_on.data(), run_on.size(), hello));
	TEST_EXPECT(hello.co == "LogicAP" && hello.ap == "Jointops.exe");
	TEST_EXPECT(parse_client_auth(run_on.data(), run_on.size(), auth));
	TEST_EXPECT(auth.co == "LogicAP" && auth.na == "PlayerPW" && auth.pw == "pw");
	TEST_EXPECT(auth.scrk == "KEY");

	// The buffers: NVS keeps 127, PW 511, CO / NA / SCRK 63.
	std::vector<uint8_t> caps;
	append_field(caps, "NVS", cstr_value(std::string(200, 'v')));
	append_field(caps, "CO", cstr_value(std::string(100, 'c')));
	append_field(caps, "NA", cstr_value(std::string(100, 'n')));
	append_field(caps, "PW", cstr_value(std::string(600, 'p')));
	append_field(caps, "SCRK", cstr_value(std::string(100, 's')));
	TEST_EXPECT(parse_client_hello(caps.data(), caps.size(), hello));
	TEST_EXPECT(hello.nvs == std::string(127, 'v') && hello.co == std::string(63, 'c'));
	TEST_EXPECT(parse_client_auth(caps.data(), caps.size(), auth));
	TEST_EXPECT(auth.nvs == std::string(127, 'v') && auth.co == std::string(63, 'c'));
	TEST_EXPECT(auth.na == std::string(63, 'n') && auth.pw == std::string(511, 'p'));
	TEST_EXPECT(auth.scrk == std::string(63, 's'));
	return 0;
}

// The client-side readers load each fixed-width field at its value pointer
// whatever its TLV length, as the 0x41 / 0x42 handlers do: a short value takes
// the bytes that follow it in the body, zero past its end (D-NET-410).
// [orig: Nwu_HandleServerHello @0x626D20 - CI @0x626F63, PG @0x62707A..0x627097,
//  HK @0x627146, TZB @0x6271F7, SF @0x627218, NP @0x627341, ET @0x627504;
//  NapiNP_HandleServerJoinResponse @0x629840 - CI @0x629A34, CK @0x629A76, CR
//  @0x629A97, the CS loads @0x629B4C..0x629B55 and tests @0x629B4F..0x629B6F, RIP
//  @0x629C3E, RCNT @0x629C7A; CNapiNPConnection_HandleDescriptionPacket @0x621AE0
//  - DC @0x621BCB, DP1 @0x621BEC, DPC @0x621C51; Nwu_HandlePing @0x623A70 - WR
//  @0x623C0F, MS @0x623C2A]
int test_client_side_readers_load_short_fields_whole() {
	// 0x81: a two-byte CI takes the next name "HK"; a 15-byte PG takes the 'S'
	// of "SN"; an empty TZB takes "SF", its NUL and SF's size byte; a one-byte
	// SF takes "NP" and its NUL; the last field, a one-byte ET, reads zero past
	// the end.
	const std::array<uint8_t, 16> guid = opennova::jointoperations_protocol_guid();
	std::vector<uint8_t> sh;
	append_field(sh, "CI", {0x09, 0x00});
	append_field(sh, "HK", {0x44, 0x33, 0x22, 0x11});
	append_field(sh, "PG", std::vector<uint8_t>(guid.begin(), guid.begin() + 15));
	append_field(sh, "SN", cstr_value("Host"));
	append_field(sh, "TZB", {});
	append_field(sh, "SF", {0x00});
	append_field(sh, "NP", {0x05, 0x00, 0x00, 0x00});
	append_field(sh, "ET", {0x07});
	ServerHello hello;
	TEST_EXPECT(parse_server_hello(sh.data(), sh.size(), hello));
	TEST_EXPECT(hello.ci == (0x09u | (uint32_t('H') << 16) | (uint32_t('K') << 24)));
	TEST_EXPECT(hello.hk == 0x11223344u && hello.sn == "Host");
	std::array<uint8_t, 16> short_pg = guid;
	short_pg[15] = 'S';
	TEST_EXPECT(hello.pg == short_pg);
	TEST_EXPECT(hello.tzb == (uint32_t('S') | (uint32_t('F') << 8) | (0x01u << 24)));
	TEST_EXPECT(hello.sf == ((uint32_t('N') << 8) | (uint32_t('P') << 16)));
	TEST_EXPECT(hello.np == 5u && hello.et == 7u);

	// 0x82: a one-byte CI takes "CK"; a two-byte CS takes its dword from the
	// next name, "RIP\0"; a direction byte of 2 files the client block; an index
	// of 15 is dropped; a seven-byte CS loads its first six; the last field, a
	// one-byte RCNT, reads zero past the end.
	std::vector<uint8_t> sa;
	append_field(sa, "CI", {0x04});
	append_field(sa, "CK", {0xEF, 0xBE, 0xAD, 0xDE});
	append_field(sa, "CR", {0x01, 0x00, 0x00, 0x00});
	append_field(sa, "CS", {0x01, 0x0D});
	append_field(sa, "RIP", {0x01, 0x02, 0x03, 0x04});
	append_field(sa, "CS", {0x02, 0x00, 0x10, 0x27, 0x00, 0x00});
	append_field(sa, "CS", {0x01, 0x0F, 0x01, 0x00, 0x00, 0x00});
	append_field(sa, "CS", {0x00, 0x05, 0xE8, 0x03, 0x00, 0x00, 0xFF});
	append_field(sa, "SCRK", cstr_value("KEY"));
	append_field(sa, "RCNT", {0x03});
	ServerAuth auth;
	TEST_EXPECT(parse_server_auth(sa.data(), sa.size(), auth));
	TEST_EXPECT(auth.ci == (0x04u | (uint32_t('C') << 8) | (uint32_t('K') << 16)));
	TEST_EXPECT(auth.ck == 0xDEADBEEFu && auth.cr == 1u && auth.rip == 0x04030201u);
	TEST_EXPECT(auth.client_cs.size() == 2 && auth.server_cs.size() == 1);
	TEST_EXPECT(auth.client_cs[0].field_index == 13 &&
			auth.client_cs[0].value ==
					(uint32_t('R') | (uint32_t('I') << 8) | (uint32_t('P') << 16)));
	TEST_EXPECT(auth.client_cs[1].field_index == 0 && auth.client_cs[1].value == 10000u);
	TEST_EXPECT(auth.server_cs[0].field_index == 5 && auth.server_cs[0].value == 1000u);
	TEST_EXPECT(auth.scrk == "KEY" && auth.rcnt == 3u);

	// The description record (and a goodbye's, the same walk): a two-byte DC
	// takes "DP"; the last field, a one-byte DPC, reads zero past the end.
	std::vector<uint8_t> desc;
	append_field(desc, "DC", {0x02, 0x00});
	append_field(desc, "DP1", {0x01, 0x00, 0x00, 0x00});
	append_field(desc, "DPC", {0x21});
	opennova::DisconnectEvent event;
	TEST_EXPECT(opennova::parse_disconnect_event(desc.data(), desc.size(), event));
	TEST_EXPECT(event.dc == (0x02u | (uint32_t('D') << 16) | (uint32_t('P') << 24)));
	TEST_EXPECT(event.dp1 == 1u && event.dpc == 0x21u);

	// The ping: an empty WR takes the 'M' of "MS" (a reply is wanted); the last
	// field, a two-byte MS, reads zero past the end. An empty last WR reads zero.
	std::vector<uint8_t> ping = {0x78, 0x56, 0x34, 0x12};
	append_field(ping, "WR", {});
	append_field(ping, "MS", {0x34, 0x12});
	opennova::SessionPingBody body;
	TEST_EXPECT(opennova::parse_session_ping_body(ping.data(), ping.size(), body));
	TEST_EXPECT(body.receiver_local_key == 0x12345678u && body.wants_reply &&
			body.timestamp_ms == 0x1234u);
	std::vector<uint8_t> last_wr = {0x78, 0x56, 0x34, 0x12};
	append_field(last_wr, "WR", {});
	TEST_EXPECT(opennova::parse_session_ping_body(last_wr.data(), last_wr.size(), body));
	TEST_EXPECT(!body.wants_reply);
	return 0;
}

int test_client_hello_tag_names_are_case_insensitive() {
	ClientHello src;
	src.pn = "NOVAWORLDUDP";
	auto bytes = client_hello_to_bytes(src);
	TEST_EXPECT(bytes.size() >= 2);
	bytes[0] = 'p';
	bytes[1] = 'n';
	ClientHello parsed;
	TEST_EXPECT(parse_client_hello(bytes.data(), bytes.size(), parsed));
	TEST_EXPECT(parsed.pn == src.pn);
	return 0;
}

int test_client_auth_roundtrip() {
	ClientAuth src;
	src.ci   = 0xCAFEBABEu;
	src.hk   = 0x0FE0E112u;
	src.ck   = 0xDEADBEEFu;
	src.na   = "jop:cus2";
	src.pw   = "a server password";
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
	TEST_EXPECT(round.pw   == src.pw);
	TEST_EXPECT(round.sip  == src.sip);
	TEST_EXPECT(round.spn  == src.spn);
	TEST_EXPECT(round.scrk == src.scrk);
	TEST_EXPECT(round.cu.size() == 2);
	TEST_EXPECT(round.cu[0] == src.cu[0]);
	TEST_EXPECT(round.cu[1] == src.cu[1]);
	return 0;
}

int test_client_auth_minimum_for_acceptance() {
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

// No field is required, CK included: a 0x42 whose CK is zero (an explicit zero
// dword, or no CK tag at all, which is how retail's builder writes a zero key)
// parses with CK 0, the remote key retail stores without a test.
// [orig: NapiNPProtocol_HandleClientJoin @0x62B750 - CK @0x62BB29, stored
//  @0x62BF7F; CNapiNPConnection_SendClientJoin @0x61fe20 gates the CK tag on
//  nonzero]
int test_client_auth_zero_ck_parses() {
	ClientAuth src = opennova::make_jointoperations_client_auth(
			1, /*client_key=*/0, 0x0FE0E112u, "ZeroKey", "SCRK");
	const auto absent = client_auth_to_bytes(src);
	TEST_EXPECT(!has_tlv_field(absent, "CK"));
	ClientAuth round;
	round.ck = 0xFFFFFFFFu;
	TEST_EXPECT(parse_client_auth(absent.data(), absent.size(), round));
	TEST_EXPECT(round.ck == 0 && round.na == "ZeroKey" && round.ci == 1);
	TEST_EXPECT(opennova::matches_jointoperations_identity(round));

	std::vector<uint8_t> explicit_zero;
	append_field(explicit_zero, "CI", {0x02, 0x00, 0x00, 0x00});
	append_field(explicit_zero, "CK", {0x00, 0x00, 0x00, 0x00});
	round.ck = 0xFFFFFFFFu;
	TEST_EXPECT(parse_client_auth(explicit_zero.data(), explicit_zero.size(), round));
	TEST_EXPECT(round.ck == 0 && round.ci == 2);
	TEST_EXPECT(!parse_client_auth(nullptr, 0, round));
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
	return 0;
}

int test_server_hello_game_metadata_has_retail_order() {
	ServerHello hello;
	hello.pg = opennova::jointoperations_protocol_guid();
	hello.p1 = 0x00010020u;
	hello.p2 = 0x00000904u;
	hello.np = 1;
	hello.mp = 4;
	hello.rip = 0xC0A80101u;
	hello.rpn = 32768;
	hello.sus1 = "live-session-user-string";
	hello.sus2 = "revx02";

	const auto bytes = server_hello_to_bytes(hello);
	// NC/EIP/EPN are zero here and therefore absent, like retail's writer.
	const std::vector<std::string> expected_names = {
			"CI", "CO", "AP", "BDAT", "PN", "PG", "PV1", "PV2", "PV3",
			"HK", "SN", "SF", "P1", "P2", "NP", "MP", "RIP", "RPN",
			"SUS1", "SUS2"};
	TEST_EXPECT(tlv_field_names(bytes) == expected_names);

	ServerHello parsed;
	TEST_EXPECT(parse_server_hello(bytes.data(), bytes.size(), parsed));
	TEST_EXPECT(parsed.p2 == 0x00000904u);
	TEST_EXPECT(parsed.sus1 == "live-session-user-string");
	TEST_EXPECT(parsed.rip == 0xC0A80101u && parsed.nc == 0 && parsed.eip == 0);
	return 0;
}

// Zero numerics, empty strings and a null GUID are omitted; CI/HK/SN/SF are
// the only unconditional tags. [orig: NapiNPProtocol_SendServerInfoPacket
//  @0x6204b0 — every string gated on `field[0]`, every dword on nonzero,
//  PG on !NapiGUID_IsNull @0x6206d5, HK @0x6207ad / SN @0x6207da / SF
//  @0x620898 unconditional]
int test_server_hello_zero_and_empty_fields_are_omitted() {
	ServerHello hello;
	hello.co.clear();
	hello.ap.clear();
	hello.bdat.clear();
	hello.pn.clear();
	hello.pv1.clear();
	hello.pv2.clear();
	hello.pv3.clear();
	hello.sn.clear();
	const auto bytes = server_hello_to_bytes(hello);
	const std::vector<std::string> expected_names = {"CI", "HK", "SN", "SF"};
	TEST_EXPECT(tlv_field_names(bytes) == expected_names);
	TEST_EXPECT(!has_tlv_field(bytes, "PG"));
	TEST_EXPECT(!has_tlv_field(bytes, "NC"));
	TEST_EXPECT(!has_tlv_field(bytes, "EIP"));
	TEST_EXPECT(!has_tlv_field(bytes, "EPN"));
	TEST_EXPECT(!has_tlv_field(bytes, "ET"));
	ServerHello parsed;
	TEST_EXPECT(parse_server_hello(bytes.data(), bytes.size(), parsed));
	// SN is unconditional, so its empty value round-trips; every field the bytes
	// omit parses as zero/empty, never as a builder default (the bytes carry only
	// CI/HK/SN/SF) [orig: Nwu_HandleServerHello @0x626d20 — every field zeroed
	// before the walk @0x626E3B..0x626F20].
	TEST_EXPECT(parsed.sn.empty() && !has_tlv_field(bytes, "PN") && !parsed.locale_block);
	TEST_EXPECT(parsed.co.empty() && parsed.ap.empty() && parsed.bdat.empty() &&
	            parsed.pn.empty() && parsed.pv1.empty() && parsed.pv2.empty() &&
	            parsed.pv3.empty());
	return 0;
}

// Every field the retail writer can emit, in its order, round-trips.
// [orig: writer @0x6204b0 CI..ET; reader Nwu_HandleServerHello @0x626d20]
int test_server_hello_full_field_set_has_retail_order() {
	ServerHello hello;
	hello.ci = 9;
	hello.de = 0x11u;
	hello.ut = 12345u;
	hello.pg = opennova::jointoperations_protocol_guid();
	hello.locale_block = true;
	hello.cn = "United States";
	hello.lng = "";  // an empty string inside the block still ships its NUL
	hello.tzb = 300u;
	hello.sf = 0;
	hello.p1 = 1; hello.p2 = 2; hello.p3 = 3; hello.p4 = 4;
	hello.p5 = 5; hello.p6 = 6; hello.p7 = 7; hello.p8 = 8;
	hello.np = 10;
	hello.mp = 32;
	hello.npw = 2;
	hello.nc = 1;
	hello.rip = 0x0A000001u;
	hello.rpn = 17479;
	hello.sus1 = "GSID-01-DEADBEEF";
	hello.sus2 = "jox01";
	hello.sus3 = "three";
	hello.sus4 = "four";
	hello.eip = 0x0A000002u;
	hello.epn = 32770;
	hello.et = 1;

	const auto bytes = server_hello_to_bytes(hello);
	const std::vector<std::string> expected_names = {
			"CI", "CO", "AP", "BDAT", "DE", "UT", "PN", "PG", "PV1", "PV2", "PV3",
			"HK", "SN", "CN", "LNG", "TZB", "SF",
			"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "NP", "MP", "NPW",
			"NC", "RIP", "RPN", "SUS1", "SUS2", "SUS3", "SUS4", "EIP", "EPN", "ET"};
	TEST_EXPECT(tlv_field_names(bytes) == expected_names);

	ServerHello parsed;
	TEST_EXPECT(parse_server_hello(bytes.data(), bytes.size(), parsed));
	TEST_EXPECT(parsed.ci == 9 && parsed.de == 0x11u && parsed.ut == 12345u);
	TEST_EXPECT(std::memcmp(parsed.pg.data(), hello.pg.data(), 16) == 0);
	TEST_EXPECT(parsed.locale_block && parsed.cn == "United States" && parsed.lng.empty() && parsed.tzb == 300u);
	TEST_EXPECT(parsed.p3 == 3 && parsed.p4 == 4 && parsed.p5 == 5 && parsed.p6 == 6 && parsed.p7 == 7 && parsed.p8 == 8);
	TEST_EXPECT(parsed.np == 10 && parsed.mp == 32 && parsed.npw == 2 && parsed.nc == 1);
	TEST_EXPECT(parsed.rip == 0x0A000001u && parsed.rpn == 17479);
	TEST_EXPECT(parsed.sus1 == "GSID-01-DEADBEEF" && parsed.sus2 == "jox01");
	TEST_EXPECT(parsed.sus3 == "three" && parsed.sus4 == "four");
	TEST_EXPECT(parsed.eip == 0x0A000002u && parsed.epn == 32770 && parsed.et == 1);
	return 0;
}

} // namespace

int main() {
	if (test_client_hello_roundtrip() != 0) return 1;
	if (test_client_hello_minimal() != 0) return 1;
	if (test_client_hello_tag_names_are_case_insensitive() != 0) return 1;
	if (test_client_hello_admits_nonzero_pm_without_identity() != 0) return 1;
	if (test_client_hello_short_dword_takes_the_following_bytes() != 0) return 1;
	if (test_client_auth_short_dword_takes_the_following_bytes() != 0) return 1;
	if (test_pg_of_any_length_loads_sixteen_bytes() != 0) return 1;
	if (test_hello_and_auth_strings_copy_to_the_first_nul() != 0) return 1;
	if (test_client_side_readers_load_short_fields_whole() != 0) return 1;
	if (test_client_auth_roundtrip() != 0) return 1;
	if (test_client_auth_minimum_for_acceptance() != 0) return 1;
	if (test_client_auth_zero_ck_parses() != 0) return 1;
	if (test_client_and_server_auth_tag_names_are_case_insensitive() != 0) return 1;
	if (test_cs_field13_follows_the_mpmaxpacketsize_clamp() != 0) return 1;
	if (test_server_auth_rejection_roundtrip() != 0) return 1;
	if (test_server_hello_ut_is_nonzero_gated() != 0) return 1;
	if (test_server_hello_omitted_metadata_parses_empty() != 0) return 1;
	if (test_server_hello_game_metadata_has_retail_order() != 0) return 1;
	if (test_server_hello_zero_and_empty_fields_are_omitted() != 0) return 1;
	if (test_server_hello_full_field_set_has_retail_order() != 0) return 1;
	std::printf("OK: session hello/auth serializer roundtrip and gating\n");
	return 0;
}
