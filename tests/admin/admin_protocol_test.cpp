// The remote-admin client codec (net/admin/admin_protocol.h) against the retail server's side:
// the packet framing, the challenge check, the login the server's own decrypt recovers, the
// reply classification, and the parsers over replies printed with the server's formats. The
// server's side is the engine's own (net/admin/admin_server.h).
// [orig: CAdminServer_SendResponse @0x402d40; CAdminServer_HandleLogin @0x405870;
//  Crypto_DecryptBuffer @0x4375b0; CAdminServer_HandlePlayer @0x403fe9;
//  CAdminServer_HandleMissionCommand @0x406447; CAdminServer_HandleGet @0x403888]

#include <base/io/crt_rand.h>
#include <net/admin/admin_protocol.h>
#include <net/admin/admin_server.h>

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

// A challenge as the server sends it (the payload of its first packet), from a seeded stream.
struct NoCommands final : opennova::AdminCommandHandler {
	bool dispatch(const opennova::AdminSession &, std::string_view, std::vector<std::string> &) override {
		return true;
	}
	std::string status_report() override { return {}; }
};

std::vector<uint8_t> server_challenge(uint32_t seed) {
	NoCommands handler;
	opennova::io::CrtRand rand;
	rand.seed(seed);
	opennova::AdminServer server({}, handler, rand);
	const opennova::AdminServer::Accepted accepted = server.accept(0x0100007Fu);
	return std::vector<uint8_t>(accepted.send.begin() + opennova::ADMIN_PACKET_HEADER_BYTES, accepted.send.end());
}

// The server's decrypt of a login, and HandleLogin's NULs at bytes 31 and 63.
void server_decrypt_login(uint8_t *buf, size_t size, const std::vector<uint8_t> &challenge) {
	opennova::admin_decrypt_buffer(buf, size, challenge.data(), 32);
	buf[31] = 0;
	buf[63] = 0;
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
		std::vector<uint8_t> challenge = server_challenge(7);
		expect(admin_challenge_valid(challenge), "the server's challenge is accepted");
		challenge[0] = 2;
		expect(!admin_challenge_valid(challenge), "a challenge whose byte 0 is not 1 is refused, as RAT.exe does");
		expect(!admin_challenge_valid(std::vector<uint8_t>(32, 1)), "a short challenge is refused");
	}

	// The login: the server's own decrypt recovers the two fields.
	{
		const std::vector<uint8_t> challenge = server_challenge(0x1234);
		auto login = admin_encode_login(challenge, "Admin", "s3cret");
		server_decrypt_login(login.data(), login.size(), challenge);
		expect(std::strcmp(reinterpret_cast<const char *>(login.data()), "Admin") == 0, "the server decodes the user");
		expect(std::strcmp(reinterpret_cast<const char *>(login.data() + 32), "s3cret") == 0,
		       "the server decodes the password");
		expect(login[64] == 0, "the trailing byte decodes to zero");

		const std::string long_name(40, 'u');
		auto clipped = admin_encode_login(challenge, long_name, "pw");
		server_decrypt_login(clipped.data(), clipped.size(), challenge);
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

	// The reply plan: how many packets each command draws (§6.9's dispatcher), split as it
	// splits, its verbs case aside.
	{
		const auto plan_is = [](const char *line, AdminReplyFraming framing, int count) {
			const AdminReplyPlan p = admin_reply_plan(line);
			return p.framing == framing && (framing != AdminReplyFraming::Exact || p.count == count);
		};
		using F = AdminReplyFraming;
		expect(plan_is("QUIT", F::Exact, 0) && plan_is("quit now", F::Exact, 0), "QUIT draws none");
		expect(plan_is("", F::Exact, 1) && plan_is("   ", F::Exact, 1) && plan_is("NOSUCHVERB", F::Exact, 1),
		       "an empty line and an unknown verb draw the usage");
		for (const char *line : {"GET", "GET GAMESTATE", "get gamesettings", "MISSION", "MISSION LIST",
		                         "MISSION AVAILABLE", "MISSION ADD a.bms 1", "MISSION REMOVE 2", "MISSION CLEAR",
		                         "MISSION CYCLE", "MISSION SETNEXT 1", "WEAPON LIST", "CMD say hi", "CHAT",
		                         "CHAT GET", "CHAT SEND hello there", "ADMINUSER LIST", "BANLIST", "PETERRABBIT",
		                         "PLAYER", "PLAYER LIST", "player list", "PLAYER PUNT", "PLAYER PUNT 3",
		                         "PLAYER PUNT ALL", "PLAYER SWAPTEAM 2", "PLAYER KILL ALL", "PLAYER ZEROSCORE 4",
		                         "PLAYER CEASEFIRE 1", "GOTO", "SET", "SET ServerPassword", "SET Bogus",
		                         "SET ServerName a b c", "set killlimit 20"}) {
			expect(plan_is(line, F::Exact, 1), line);
		}
		expect(plan_is("SET Bogus 1", F::Exact, 2) && plan_is("SET LevelRestrict 1", F::Exact, 2),
		       "SET with a key it does not write draws the usage and Setting Not Found");
		expect(plan_is("GOTO GAMESTATE", F::Exact, 2) && plan_is("GOTO ELSEWHERE", F::Exact, 2),
		       "GOTO's other forms draw their reply and the cycle tail");
		expect(plan_is("GOTO MENUSTATE", F::MenuState, 0) && plan_is("goto menustate", F::MenuState, 0),
		       "GOTO MENUSTATE: the first reply decides");
		expect(plan_is("PLAYER BAN 3", F::BanTarget, 0), "PLAYER BAN <#>: the first reply decides");
		expect(plan_is("PLAYER BAN ALL", F::QuietGap, 0) && plan_is("PLAYER BAN all", F::QuietGap, 0),
		       "PLAYER BAN ALL: per banned slot");
		expect(plan_is("PLAYER WAVE 3", F::QuietGap, 0), "a PLAYER sub-verb the server does not know");
		expect(plan_is("PLAYER\tLIST", F::Exact, 1) && plan_is("  PLAYER  BAN\t3 ", F::BanTarget, 0),
		       "tabs and runs of spaces split as the dispatcher's isspace does");
		// A cp1252 byte >= 0x80 is no separator: "PLAYER\xA0LIST" is one token, an unknown verb.
		expect(plan_is("PLAYER\xA0LIST", F::Exact, 1), "a high byte does not split");

		const AdminReplyPlan ban = admin_reply_plan("PLAYER BAN 3");
		expect(!admin_replies_complete(ban, {}), "BAN: none read yet");
		expect(admin_replies_complete(ban, {"OK - Player Banned."}), "BAN with no ban list: the punt's reply");
		expect(admin_replies_complete(ban, {"ERROR - Player Not Found."}), "BAN on no slot");
		expect(admin_replies_complete(ban, {"ERROR - Player #3 \"x\" doesn't have a PCID."}), "BAN with no PCID");
		expect(!admin_replies_complete(ban, {"OK - Banned Player #3 \"x\"."}), "BAN listed: a second follows");
		expect(admin_replies_complete(ban, {"OK - Banned Player #3 \"x\".", "OK - Player Banned."}),
		       "BAN listed: both read");
		const AdminReplyPlan menu = admin_reply_plan("GOTO MENUSTATE");
		expect(admin_replies_complete(menu, {"OK - Server is cycling..."}), "MENUSTATE in a match: the tail");
		expect(!admin_replies_complete(menu, {"ERROR - Already in 'Menu' State"}), "MENUSTATE in the menu");
		expect(admin_replies_complete(menu, {"ERROR - Already in 'Menu' State",
		                                     "ERROR - Must be in Game State to cycle server."}),
		       "MENUSTATE in the menu: its error and the tail");
		const AdminReplyPlan two = admin_reply_plan("SET Bogus 1");
		expect(!admin_replies_complete(two, {"USAGE -  SET [...]"}) &&
		               admin_replies_complete(two, {"USAGE -  SET [...]", "ERROR - Setting Not Found."}),
		       "an exact count");
		expect(!admin_replies_complete(admin_reply_plan("PLAYER BAN ALL"), {"a", "b", "c"}),
		       "the quiet gap is never complete by count");
		expect(admin_replies_complete(admin_reply_plan("QUIT"), {}), "QUIT is complete at once");
	}

	if (g_failures == 0) {
		std::printf("admin_protocol: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
