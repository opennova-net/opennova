#include <novaworld/join_identity.h>
#include <novacrypto/pubcrypto.h>

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

	const auto decoded_name = opennova::decode_pub_value(
		opennova::encode_pub_value(payloads.name_info, key), key);
	check(decoded_name == payloads.name_info, "PUBNAMEINFO round-trips");
	check(cstr_at(decoded_name, 0) == "FooPlayer", "PUBNAMEINFO carries nwhandle");
	check(!decoded_name.empty() && decoded_name.back() == 0,
	      "PUBNAMEINFO is NUL-terminated");

	const auto decoded_squad = opennova::decode_pub_value(
		opennova::encode_pub_value(payloads.squad_info, key), key);
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

} // namespace

int main() {
	check_foo_player_payloads_round_trip();
	check_empty_display_handle_is_not_encoded_as_blank_name();
	if (g_failures == 0) {
		std::printf("join_identity: all checks passed\n");
	}
	return g_failures == 0 ? 0 : 1;
}
