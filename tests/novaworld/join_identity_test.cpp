#include <net/novaworld/join_identity.h>
#include <net/novacrypto/pubcrypto.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char *message) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++g_failures;
	}
}

std::string cstr_at(const std::vector<uint8_t> &bytes, size_t offset) {
	std::string out;
	for (size_t i = offset; i < bytes.size() && bytes[i] != 0; ++i) {
		out.push_back(static_cast<char>(bytes[i]));
	}
	return out;
}

void check_foo_player_payloads_round_trip() {
	const std::string key = "NGPAIIAHFONBCAHJEEPDLEHGOMOIBOIN";
	const auto payloads =
		opennova::build_pub_join_identity_plaintexts("00000003", "FooPlayer");

	const std::vector<uint8_t> expected_pcid = {
		'0', '0', '0', '0', '0', '0', '0', '3', 0,
	};
	check(payloads.pcid == expected_pcid, "PUBPCID plaintext is pcid plus NUL");

	std::vector<uint8_t> decoded_name;
	check(opennova::decode_pub_value(opennova::encode_pub_value(payloads.name_info, key), key,
	                                 decoded_name),
	      "PUBNAMEINFO decodes");
	check(decoded_name == payloads.name_info, "PUBNAMEINFO round-trips");
	check(cstr_at(decoded_name, 0) == "FooPlayer", "PUBNAMEINFO carries nwhandle");
	check(!decoded_name.empty() && decoded_name.back() == 0,
	      "PUBNAMEINFO is NUL-terminated");

	std::vector<uint8_t> decoded_squad;
	check(opennova::decode_pub_value(opennova::encode_pub_value(payloads.squad_info, key), key,
	                                 decoded_squad),
	      "PUBSQUADINFO decodes");
	check(decoded_squad == payloads.squad_info, "PUBSQUADINFO round-trips");
	check(decoded_squad.size() == 4 + 10 + 9,
	      "PUBSQUADINFO has u32 prefix, full name, short name");
	check(decoded_squad[0] == 0 && decoded_squad[1] == 0 &&
	      decoded_squad[2] == 0 && decoded_squad[3] == 0,
	      "PUBSQUADINFO prefix is zero");
	check(cstr_at(decoded_squad, 4) == "FooPlayer",
	      "PUBSQUADINFO display name is nwhandle");
	check(cstr_at(decoded_squad, 4 + 10) == "FooPlaye",
	      "PUBSQUADINFO short name is first 8 bytes");
}

void check_empty_display_handle_is_not_encoded_as_blank_name() {
	const auto payloads =
		opennova::build_pub_join_identity_plaintexts("00000003", "");
	check(!payloads.pcid.empty(), "PCID plaintext still builds with empty handle");
	check(payloads.name_info.empty(), "empty handle yields no PUBNAMEINFO payload");
	check(payloads.squad_info.empty(), "empty handle yields no PUBSQUADINFO payload");
}

// D-NET-295: the host's JOIN handler decrypts the PUBPCID / PUBSQUADINFO
// cookies under its cookie-key table, trying each slot's decimal key in order.
// [orig: NapiPacket_DecryptAndVerify @0x4c2ad0; NapiNPServer_HandlePlayerJoinMessage
//  @0x512cf9..0x512e50]
std::vector<uint8_t> bytes_of(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }

void check_host_loads_the_account_under_its_key_table() {
	opennova::SessionIdRing ring;
	ring.keys = {0x1000101u, 0x1234567u, 0, 0, 0, 0};
	ring.count = 2;
	ring.index = 1;
	const std::string key = std::to_string(0x1234567); // the second slot's key
	std::vector<uint8_t> squad = {0x78, 0x56, 0x34, 0x12};
	for (char c : std::string("Clan Name")) squad.push_back(static_cast<uint8_t>(c));
	squad.push_back(0);
	for (char c : std::string("CLN")) squad.push_back(static_cast<uint8_t>(c));
	squad.push_back(0);
	const opennova::JoinCookiePairs cookie = {
		{"PUBNAMEINFO", opennova::encode_pub_value(bytes_of(std::string("Foo", 4)), key)},
		{"pubpcid", opennova::encode_pub_value(bytes_of(std::string("00000007", 9)), key)},
		{"PUBSQUADINFO", opennova::encode_pub_value(squad, key)},
	};
	opennova::JoinAccount account;
	opennova::load_join_account(cookie, "PUB", ring, account);
	check(account.pcid == "00000007", "PCID decrypts under the second slot (field names match caselessly)");
	check(account.squad_id == 0x12345678u, "the squad id is SQUADINFO's leading dword");
	check(account.squad_name == "Clan Name" && account.squad_tag == "CLN",
	      "the squad name and tag are the two strings after it");

	opennova::SessionIdRing first_only = ring;
	first_only.count = 1; // the second key is outside the table
	opennova::JoinAccount none;
	opennova::load_join_account(cookie, "PUB", first_only, none);
	check(none.pcid.empty() && none.squad_id == 0 && none.squad_name.empty(),
	      "no key of the table verifies: nothing loads");
	opennova::JoinAccount other_prefix;
	opennova::load_join_account(cookie, "NW", ring, other_prefix);
	check(other_prefix.pcid.empty(), "the keys are prefixed with the local address");

	// The caps: the 32-byte PCID field keeps 31 chars, the name 64 and the tag 8.
	std::vector<uint8_t> long_squad = {1, 0, 0, 0};
	for (int i = 0; i < 70; ++i) long_squad.push_back('n');
	long_squad.push_back(0);
	for (int i = 0; i < 12; ++i) long_squad.push_back('t');
	long_squad.push_back(0);
	const opennova::JoinCookiePairs long_cookie = {
		{"PUBPCID", opennova::encode_pub_value(bytes_of(std::string(40, 'p')), key)},
		{"PUBSQUADINFO", opennova::encode_pub_value(long_squad, key)},
	};
	opennova::JoinAccount capped;
	opennova::load_join_account(long_cookie, "PUB", ring, capped);
	check(capped.pcid == std::string(31, 'p'), "the PCID keeps 31 chars");
	check(capped.squad_id == 1 && capped.squad_name == std::string(64, 'n') &&
	      capped.squad_tag == std::string(8, 't'), "the squad name keeps 64 chars, the tag 8");

	// A SQUADINFO shorter than its dword: the id is 0 and the name starts at byte 0.
	const opennova::JoinCookiePairs short_cookie = {
		{"PUBSQUADINFO", opennova::encode_pub_value(bytes_of("AB"), key)},
	};
	opennova::JoinAccount short_account;
	short_account.squad_id = 9;
	opennova::load_join_account(short_cookie, "PUB", ring, short_account);
	check(short_account.squad_id == 0 && short_account.squad_name == "AB" &&
	      short_account.squad_tag.empty(), "a short SQUADINFO carries no id");
}

} // namespace

int main() {
	check_foo_player_payloads_round_trip();
	check_empty_display_handle_is_not_encoded_as_blank_name();
	check_host_loads_the_account_under_its_key_table();
	if (g_failures == 0) {
		std::printf("join_identity: all checks passed\n");
	}
	return g_failures == 0 ? 0 : 1;
}
