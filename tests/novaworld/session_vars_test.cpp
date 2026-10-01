// The session-variable list (npwire/session_vars.h): the authority's stream in
// the serializer's key order, and the client parse -- the clears, the
// case-insensitive keys, the Napi_CopyString caps, EXP_FANFARE's retention and
// a truncated stream ending the walk.
// [orig: Game_SerializeMissionInfoToDataStream @0x523620;
//  Client_ParseServerSessionVariables @0x5202f0]
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <net/npwire/session_vars.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

void kv(std::vector<uint8_t> &out, const char *key, const std::vector<uint8_t> &value) {
	for (const char *p = key; *p; ++p) out.push_back(uint8_t(*p));
	out.push_back(0);
	const uint32_t n = uint32_t(value.size());
	out.push_back(uint8_t(n));
	out.push_back(uint8_t(n >> 8));
	out.push_back(uint8_t(n >> 16));
	out.push_back(uint8_t(n >> 24));
	out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> text(const std::string &s) {
	std::vector<uint8_t> v(s.begin(), s.end());
	v.push_back(0);
	return v;
}

void test_the_stream_round_trips_in_key_order() {
	SessionVars in;
	in.server_name = "Host";
	in.mission_name = "Dormant Volcano Isle";
	in.game_type = 0x10010;
	in.custom_text = "Put your message here.";
	in.mission_file = "ASH_I5A.BMS";
	in.exp_fanfare = 0x1405;
	const std::vector<uint8_t> stream = encode_session_vars(in);
	std::vector<uint8_t> expected;
	kv(expected, "SERVERNAME", text("Host"));
	kv(expected, "MISSIONNAME", text("Dormant Volcano Isle"));
	kv(expected, "GAMETYPE", { 0x10, 0x00, 0x01, 0x00 });
	kv(expected, "CUSTOMTEXT", text("Put your message here."));
	kv(expected, "MISSIONFILENAME", text("ASH_I5A.BMS"));
	kv(expected, "EXP_FANFARE", { 0x05, 0x14 });
	CHECK(stream == expected);
	SessionVars out;
	decode_session_vars(stream.data(), stream.size(), out);
	CHECK(out.server_name == in.server_name && out.mission_name == in.mission_name &&
			out.game_type == in.game_type && out.custom_text == in.custom_text &&
			out.mission_file == in.mission_file && out.exp_fanfare == in.exp_fanfare);
}

void test_the_parse_caps_and_keys() {
	std::vector<uint8_t> stream;
	kv(stream, "servername", text(std::string(40, 'a')));
	kv(stream, "MissionName", text(std::string(70, 'b')));
	kv(stream, "CUSTOMTEXT", text(std::string(600, 'c')));
	kv(stream, "MISSIONFILENAME", text(std::string(70, 'd')));
	kv(stream, "UNKNOWN", { 1, 2, 3 });
	SessionVars out;
	decode_session_vars(stream.data(), stream.size(), out);
	CHECK(out.server_name == std::string(31, 'a'));
	CHECK(out.mission_name == std::string(63, 'b'));
	CHECK(out.custom_text == std::string(511, 'c'));
	CHECK(out.mission_file == std::string(63, 'd'));
}

void test_the_clears_and_the_fanfare_retention() {
	SessionVars vars;
	vars.server_name = "old";
	vars.mission_name = "old";
	vars.game_type = 7;
	vars.custom_text = "old";
	vars.mission_file = "old";
	vars.exp_fanfare = 0x0203;
	std::vector<uint8_t> stream;
	kv(stream, "SERVERNAME", text("new"));
	decode_session_vars(stream.data(), stream.size(), vars);
	CHECK(vars.server_name == "new" && vars.mission_name.empty() && vars.game_type == 0 &&
			vars.custom_text.empty() && vars.mission_file.empty());
	CHECK(vars.exp_fanfare == 0x0203);
	decode_session_vars(nullptr, 0, vars);
	CHECK(vars.server_name.empty() && vars.exp_fanfare == 0x0203);
}

void test_a_truncated_stream_ends_the_walk() {
	std::vector<uint8_t> stream;
	kv(stream, "SERVERNAME", text("biggy"));
	kv(stream, "EXP_FANFARE", { 5, 20 });
	std::vector<uint8_t> truncated(stream.begin(), stream.begin() + 20);
	SessionVars out;
	decode_session_vars(truncated.data(), truncated.size(), out);
	CHECK(out.exp_fanfare == 0);
	std::vector<uint8_t> no_length(stream.begin(), stream.begin() + 13);
	decode_session_vars(no_length.data(), no_length.size(), out);
	CHECK(out.server_name.empty());
}

} // namespace

int main() {
	test_the_stream_round_trips_in_key_order();
	test_the_parse_caps_and_keys();
	test_the_clears_and_the_fanfare_retention();
	test_a_truncated_stream_ends_the_walk();
	if (failures == 0) std::printf("session_vars_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
