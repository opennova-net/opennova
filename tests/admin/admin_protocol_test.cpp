// The remote-admin client codec (net/admin/admin_protocol.h) against the retail server's side:
// the packet framing, the challenge check, the login the server's own decrypt recovers, the
// reply classification, and the parsers over replies printed with the server's formats.
// [orig: CAdminServer_SendResponse @0x402d40; CAdminServer_HandleLogin @0x405870;
//  Crypto_DecryptBuffer @0x4375b0; CAdminServer_HandlePlayer @0x403fe9;
//  CAdminServer_HandleMissionCommand @0x406447; CAdminServer_HandleGet @0x403888]

#include "admin_server_side.h"

#include <net/admin/admin_protocol.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::string hex(const std::array<uint8_t, opennova::ADMIN_LOGIN_BYTES> &bytes) {
	std::string out;
	char t[3];
	for (uint8_t b : bytes) {
		std::snprintf(t, sizeof(t), "%02x", b);
		out += t;
	}
	return out;
}

std::string player_row(const char *name, int slot, int team) {
	char row[128];
	std::snprintf(row, sizeof(row), "%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n", name, slot, team, 8, 7, 2, 235);
	return row;
}

} // namespace

int main() {
	using namespace opennova;

	// Framing: the marker, the total length counting the header, the payload.
	{
		const uint8_t payload[] = {'Q', 'U', 'I', 'T', 0};
		const std::vector<uint8_t> packet = admin_encode_packet(payload, sizeof(payload));
		const std::vector<uint8_t> want = {0x00, 0x00, 0x0D, 0x0A, 13, 0, 0, 0, 'Q', 'U', 'I', 'T', 0};
		expect(packet == want, "a packet is 00 00 0D 0A, the u32le total length, the payload");
		size_t size = 0;
		expect(admin_decode_header(packet.data(), size) && size == 5, "the header announces the payload");
		std::vector<uint8_t> bad = packet;
		bad[3] = 0x0B;
		expect(!admin_decode_header(bad.data(), size), "a wrong marker is not a packet");
		const uint8_t short_len[] = {0x00, 0x00, 0x0D, 0x0A, 7, 0, 0, 0};
		expect(!admin_decode_header(short_len, size), "a length below the header is not a packet");
		expect(admin_encode_command(ADMIN_COMMAND_PLAYER_LIST) ==
		               std::vector<uint8_t>({'P', 'L', 'A', 'Y', 'E', 'R', ' ', 'L', 'I', 'S', 'T', 0}),
		       "a command is its text and the NUL");
	}

	// The challenge: 33 bytes, byte 0 is 1, the NUL last.
	{
		std::vector<uint8_t> challenge = admin_test::server_challenge(7);
		expect(admin_challenge_valid(challenge), "the server's challenge is accepted");
		challenge[0] = 2;
		expect(!admin_challenge_valid(challenge), "a challenge whose byte 0 is not 1 is refused, as RAT.exe does");
		expect(!admin_challenge_valid(std::vector<uint8_t>(32, 1)), "a short challenge is refused");
	}

	// The login: the server's own decrypt recovers the two fields.
	{
		const std::vector<uint8_t> challenge = admin_test::server_challenge(0x1234);
		auto login = admin_encode_login(challenge, "Admin", "s3cret");
		admin_test::server_decrypt_login(login.data(), login.size(), challenge);
		expect(std::strcmp(reinterpret_cast<const char *>(login.data()), "Admin") == 0, "the server decodes the user");
		expect(std::strcmp(reinterpret_cast<const char *>(login.data() + 32), "s3cret") == 0,
		       "the server decodes the password");
		expect(login[64] == 0, "the trailing byte decodes to zero");

		const std::string long_name(40, 'u');
		auto clipped = admin_encode_login(challenge, long_name, "pw");
		admin_test::server_decrypt_login(clipped.data(), clipped.size(), challenge);
		expect(clipped[30] == 'u' && clipped[31] == 0, "a field keeps 31 characters, as the server reads it");

		// The contributor's vectors from opennova-net/WolfRAT2's _jo_encrypt (challenge[i] =
		// (i*37+11) & 0xFF, 0 -> 1): the cipher alone, byte for byte.
		std::vector<uint8_t> vector_key;
		for (int i = 0; i < 32; ++i) {
			const uint8_t b = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
			vector_key.push_back(b ? b : 1);
		}
		vector_key.push_back(0);
		expect(hex(admin_encode_login(vector_key, "admin", "secret")) ==
		               "74b58687b819aa6b5c7dce4f00e1f233a445161748a93afbec0d5e53f5e3e528a775464778d96a2b1c3d8e0fc0a1b2f36405d6"
		               "d70869fabbaccd1e9fbe9aafe755",
		       "the login matches WolfRAT2 (admin/secret)");
		expect(hex(admin_encode_login(vector_key, "someone", "pw2")) ==
		               "74b58687b819aa6b5c7dce4f00e1f233a445161748a93afbec0d5edf9071b43aa475464778d96a2b1c3d8e0fc0a1b2f36405d6"
		               "d70869fabbaccd830dbf96aff267",
		       "the login matches WolfRAT2 (someone/pw2)");
	}

	// Reply text and classification.
	{
		std::string text;
		const std::string ok = "OK - User: Admin successfully logged in.";
		std::vector<uint8_t> payload(ok.begin(), ok.end());
		payload.push_back(0);
		expect(admin_reply_text(payload, text) && text == ok, "a reply is its text up to the NUL");
		payload.pop_back();
		expect(!admin_reply_text(payload, text), "a reply without its NUL is not a server reply");
		expect(admin_login_accepted(ok), "the success reply logs in");
		expect(!admin_login_accepted("USAGE - [QUIT | GET]"), "anything else does not");
		expect(admin_reply_is_error("ERROR - Not in Game State."), "an ERROR reply");
		expect(admin_reply_is_error("USAGE - [QUIT | GET | SET | MISSION | PLAYER | WEAPON | CMD]"),
		       "the verb usage reply");
		expect(admin_reply_is_error("USAGE -  GET [GAMESTATE] [GAMESETTINGS]"), "a sub-command usage reply");
		expect(!admin_reply_is_error("No missions in queue."), "a data reply");
	}

	// PLAYER LIST, printed with the server's header and row formats.
	{
		const std::string reply = std::string("NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n") +
		                          player_row("Host", 0, 1) + player_row("Hamshop", 1, 1) +
		                          player_row("Two Words", 3, 2) + player_row("ALongerNameThan16", 12, 2);
		const auto players = admin_parse_player_list(reply);
		expect(players.size() == 3, "the header and the host's own slot 0 are not players");
		if (players.size() == 3) {
			expect(players[0].name == "Hamshop" && players[0].slot == 1 && players[0].team == "1", "a row");
			expect(players[1].name == "Two Words" && players[1].slot == 3 && players[1].team == "2",
			       "a name keeps its inner space, the slot is the server's");
			expect(players[2].name == "ALongerNameThan16" && players[2].slot == 12,
			       "%-16s pads but never cuts a name");
		}
		expect(admin_parse_player_list("NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n").empty(),
		       "the header alone is no players");
	}

	// MISSION LIST: index, file, then the five tags.
	{
		expect(admin_parse_current_mission("0: CP01.bms - () () () <CURRENT MISSION> <>\n") == "CP01",
		       "the current entry's file without its extension");
		expect(admin_parse_current_mission("0: A.bms - () () () <> <NEXT MISSION>\n"
		                                   "1: AS - Black Rock TAC.npj - (2x) (IS FLIPPED) () <CURRENT MISSION> <>\n") ==
		               "AS - Black Rock TAC",
		       "a file name holding ' - ', with the tags found from the end");
		expect(admin_parse_current_mission("No missions in queue.").empty(), "the empty queue");
		expect(admin_parse_current_mission("0: A.bms - () () () <> <NEXT MISSION>\n").empty(),
		       "no entry current");
	}

	// GET GAMESETTINGS: the GameTime row.
	{
		expect(admin_parse_time_left_minutes("Tracers              = 1\nGameTime             = 21/25\n") == 21,
		       "the remaining minutes");
		expect(admin_parse_time_left_minutes("GameTime             = 0/0\n") == -1, "an untimed round");
		expect(admin_parse_time_left_minutes("GameTime             = 0/30\n") == -1, "a spent round");
		expect(admin_parse_time_left_minutes("Tracers              = 1\n") == -1, "no GameTime row");
	}

	if (g_failures == 0) {
		std::printf("admin_protocol: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
