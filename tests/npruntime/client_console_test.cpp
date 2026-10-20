// D-NET-710: the client's console command walk (inmatch/client_console.h), each arm against
// the cited original's arithmetic: the five debug commands' lines, state and sends, the
// server command table's match (an ASCII name never meets a typed line; a negative length
// takes any; a high-byte name folds through the locale), the C2S 0x07 body's nibble swap,
// the walk's order, the S2C 0x06 copy, the binding table's lookups (by token, and the CRC
// compare against the token's text) and Client_ProcessLocalCommand's arms.
// [orig: NapiClientCmd_ParseAndExecute @0x4329C0; NapiClientCmd_HandleDebugCommand @0x42C4D0;
//  NapiClientCmd_HandleDebug @0x42CCE0; NetPacket_WriteObfuscatedString @0x42AEA0;
//  NapiNPClientMsg_HandleChatCommand @0x432BC0; Client_ProcessLocalCommand @0x520BF0]
#include <runtime/inmatch/client_console.h>

#include <base/vfs/vfs.h> // vfs_version_crc (CRC_ComputeCustomTable)
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

int failures = 0;
void check(bool ok, const char *what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
}

struct Run {
	ClientConsoleState state;
	std::vector<ProtocolMessage> sends;
	std::vector<std::string> lines;
	replication::ClientServerCommandTable table{};
	ClientConsoleContext ctx;
	Run(bool in_session, bool novaworld, bool authority) {
		ctx.in_session = in_session;
		ctx.novaworld = novaworld;
		ctx.authority = authority;
		ctx.server_commands = &table;
		ctx.state = &state;
		ctx.queued = &sends;
		ctx.system_lines = &lines;
	}
	bool run(const std::string &line) {
		sends.clear();
		lines.clear();
		return client_parse_and_execute(line, ctx);
	}
	void entry(std::size_t slot, uint8_t id, uint8_t length, const std::vector<uint8_t> &name) {
		table[slot][0] = id;
		table[slot][1] = length;
		for (std::size_t i = 0; i < name.size() && i + 2 < table[slot].size(); ++i)
			table[slot][i + 2] = static_cast<uint8_t>(name[i] ^ 0xFE);
	}
};

// Four characters as the little-endian dword a token field's head reads as.
int32_t text_dword(const char *text) {
	uint32_t value = 0;
	for (int i = 0; i < 4 && text[i] != 0; ++i)
		value |= static_cast<uint32_t>(static_cast<uint8_t>(text[i])) << (8 * i);
	return static_cast<int32_t>(value);
}

// A line `prefix` + four bytes whose CRC_ComputeCustomTable value is `target`: the MSB-first
// table step is linear, so four steps from the prefix's state add
// T[i0] << 24 ^ T[i1] << 16 ^ T[i2] << 8 ^ T[i3] to it shifted out; each table entry's low
// byte names its index.
std::string forced_crc_line(const std::string &prefix, int32_t target) {
	uint32_t table[256];
	for (uint32_t i = 0; i < 256; ++i) {
		uint32_t c = i << 24;
		for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
		table[i] = c;
	}
	uint32_t rest = static_cast<uint32_t>(target);
	uint8_t index[4] = {};
	for (int k = 3; k >= 0; --k) {
		const int shift = 8 * (3 - k);
		const uint8_t want = static_cast<uint8_t>(rest >> shift);
		for (uint32_t i = 0; i < 256; ++i) {
			if (static_cast<uint8_t>(table[i]) == want) {
				index[k] = static_cast<uint8_t>(i);
				rest ^= table[i] << shift;
				break;
			}
		}
	}
	uint32_t crc = static_cast<uint32_t>(
			vfs_version_crc(reinterpret_cast<const uint8_t *>(prefix.data()), prefix.size()));
	std::string line = prefix;
	for (int k = 0; k < 4; ++k) {
		line.push_back(static_cast<char>(static_cast<uint8_t>(crc >> 24) ^ index[k]));
		crc = (crc << 8) ^ table[index[k]];
	}
	return line;
}

std::vector<uint8_t> bytes(const std::string &s, bool nul = true) {
	std::vector<uint8_t> out(s.begin(), s.end());
	if (nul) out.push_back(0);
	return out;
}

} // namespace

int main() {
	// The nibble swap through an 8-bit arithmetic shift, then XOR 0xEA: 'a' 0x61 ->
	// 0x16 ^ 0xEA = 0xFC; a high byte keeps the shift's sign nibble: 0xE9 -> 0xFE ^ 0xEA.
	check(client_console_obfuscate(0x61) == 0xFC, "obfuscate 'a'");
	check(client_console_obfuscate(0xE9) == 0x14, "obfuscate a high byte through sar");
	check(client_console_obfuscate(0x20) == 0xE8, "obfuscate ' '");

	{
		Run r(false, false, false);
		check(r.run("clientdebugmessagelist") && r.state.debug_message_list &&
				r.lines == std::vector<std::string>{"CLIENTDEBUGMESSAGELIST:  Enabled"},
				"CLIENTDEBUGMESSAGELIST enables");
		check(r.run("CLIENTDEBUGMESSAGELIST") && !r.state.debug_message_list &&
				r.lines == std::vector<std::string>{"CLIENTDEBUGMESSAGELIST:  Disabled"},
				"CLIENTDEBUGMESSAGELIST toggles off");
		check(r.run("DIRTYUPSTREAM") &&
				r.lines == std::vector<std::string>{"Usage:  DIRTYUPSTREAM [ PRE | POST | 0....5 ]"},
				"DIRTYUPSTREAM usage");
		check(r.run("dirtyupstream post") && r.state.dirty_upstream_post == 1 &&
				r.lines == std::vector<std::string>{"DIRTYUPSTREAM set to POST mode"}, "POST");
		check(r.run("DIRTYUPSTREAM PRE") && r.state.dirty_upstream_post == 0 &&
				r.lines == std::vector<std::string>{"DIRTYUPSTREAM set to PRE mode"}, "PRE");
		check(r.run("DIRTYUPSTREAM 3") && r.state.dirty_upstream_level == 3 &&
				r.lines == std::vector<std::string>{"DIRTYUPSTREAM set to 3"}, "level 3");
		check(r.run("DIRTYUPSTREAM 9") && r.state.dirty_upstream_level == 5 &&
				r.lines == std::vector<std::string>{"DIRTYUPSTREAM set to 5"}, "above 5 stores 5");
		check(r.run("DIRTYUPSTREAM -4") && r.state.dirty_upstream_level == 0 &&
				r.lines == std::vector<std::string>{"DIRTYUPSTREAM turned off"}, "below 0 stores 0");
		check(r.sends.empty(), "the debug words send nothing");
	}
	{
		// BAN: only an authority in a NovaWorld session sends the whole line as C2S 0x24.
		Run lan(true, false, true);
		check(lan.run("BAN 3") && lan.sends.empty() &&
				lan.lines == std::vector<std::string>{"BAN only works in multiplayer novaworld games."},
				"BAN off NovaWorld refuses");
		Run joiner(true, true, false);
		check(joiner.run("BAN 3") && joiner.sends.empty() &&
				joiner.lines == std::vector<std::string>{"You must be the server to BAN a player."},
				"BAN from a joiner refuses");
		Run host(true, true, true);
		check(host.run("ban 3") && host.sends.size() == 1 &&
				host.sends[0].tag == c2s::BAN_PUNT_COMMAND &&
				host.sends[0].payload == bytes("ban 3") && host.sends[0].retention_flushes == 0 &&
				host.lines.empty(),
				"BAN from the NovaWorld authority sends the whole line");
	}
	{
		// SERVER: any client; the line past the leading spaces, the word and its spaces.
		Run r(false, false, false);
		check(r.run("  server   PUNT 2 ") && r.sends.size() == 1 &&
				r.sends[0].tag == c2s::BAN_PUNT_COMMAND && r.sends[0].payload == bytes("PUNT 2 "),
				"SERVER sends the rest of the line as 0x24");
		check(r.run("SERVER") && r.sends.size() == 1 && r.sends[0].payload == bytes(""),
				"a bare SERVER sends an empty line");
		// GETPLAYERLIST: the player-sync request {0, 23799} then the bodiless 0x23.
		check(r.run("GETPLAYERLIST") && r.sends.size() == 2 &&
				r.sends[0].tag == c2s::PLAYER_SYNC_REQUEST &&
				r.sends[0].payload == std::vector<uint8_t>{0x00, 0xF7, 0x5C} &&
				r.sends[1].tag == c2s::VISIBLE_PLAYERS_REQUEST && r.sends[1].payload.empty(),
				"GETPLAYERLIST sends 0x22 then 0x23");
	}
	{
		// The table: a stock host's "NULL" entry never meets a typed line (the stored
		// byte sign-extends before the XOR), so the walk falls through to the bindings,
		// where "null" is row 65's token (action code 0): taken there, nothing sent.
		Run r(true, false, false);
		r.entry(0, 0x00, 4, bytes("NULL", false));
		check(r.run("NULL hello") && r.sends.empty(), "an ASCII name never matches");
		check(r.run("null") && r.sends.empty(), "nor in lower case");
		check(!r.run("hello") && r.sends.empty(), "an unbound line is not taken");
		// A negative length takes any line at once: the id, then the line past its
		// leading spaces, nibble-swapped, reliable for 310 flushes.
		r.entry(1, 0x05, 0x80, {});
		check(r.run("  XYZ ab") && r.sends.size() == 1 && r.sends[0].tag == c2s::RESERVED_NOOP_07 &&
				r.sends[0].retention_flushes == 310,
				"a negative length takes the line as C2S 0x07");
		std::vector<uint8_t> want{0x05};
		for (const char c : std::string("XYZ ab"))
			want.push_back(client_console_obfuscate(static_cast<uint8_t>(c)));
		want.push_back(0);
		check(!r.sends.empty() && r.sends[0].payload == want, "the 0x07 body");
	}
	{
		// A high-byte name: its stored byte stays positive, and the line's byte folds
		// through the locale (e-acute 0xE9 -> 0xC9); the parameters follow its spaces.
		Run r(true, false, false);
		r.entry(3, 0x09, 1, {0xC9});
		check(r.run(std::string("\xE9  go")) && r.sends.size() == 1 &&
				r.sends[0].payload ==
						std::vector<uint8_t>{0x09, client_console_obfuscate('g'),
						                     client_console_obfuscate('o'), 0x00},
				"a high-byte name matches through the locale's upper case");
	}
	{
		// The walk's order: the debug commands before the table; a line with no config
		// tokens ends it; the binding arm last.
		Run r(true, false, false);
		r.entry(0, 0x01, 0x80, {});
		check(r.run("GETPLAYERLIST") && r.sends.size() == 2, "the debug commands come first");
		check(!r.run("  ; a comment") && r.sends.empty(), "no tokens: nothing taken");
		Run b(true, false, false);
		std::vector<std::vector<int32_t>> actions;
		b.ctx.queue_action = [&](int32_t code, int32_t p1, int32_t p2, int32_t p3) {
			actions.push_back({code, p1, p2, p3});
		};
		// The binding arm: the first token against the re-laid table by name (any case),
		// the record index being the action code (row 69 PrintScreen is code 2, row 10
		// Crouch 169), then up to three atol parameters by the token count.
		check(b.run("printscreen") && actions.back() == std::vector<int32_t>{2, 0, 0, 0},
				"an untaken line reaches the binding arm: its action code, no parameters");
		check(b.run("Crouch 5 -6") && actions.back() == std::vector<int32_t>{169, 5, -6, 0},
				"two parameters");
		check(b.run("Crouch 1 2 3") && actions.back() == std::vector<int32_t>{169, 1, 2, 3},
				"three parameters");
		const std::size_t before = actions.size();
		check(!b.run("Crouch 1 2 3 4") && actions.size() == before,
				"more than three parameters: not taken");
		check(!b.run("no_such_binding") && actions.size() == before, "no binding: not taken");
	}
	{
		// The CRC compare reads the token's first four bytes as a dword (a zero record's
		// zeros): "Crou" is code 169's; 0 meets the first code no row carries (5).
		check(client_binding_by_crc(text_dword("Crou")) == 169,
				"the CRC compare reads the token's text");
		check(client_binding_by_crc(0) == 5, "a zero record's dword is 0");
		check(client_binding_by_token("NULL") == 0 && client_binding_by_token("nothing") == -1,
				"by token: case-insensitive");
	}
	{
		// Client_ProcessLocalCommand's arms: `pb_` behind a leading slash, SAVEKEYS,
		// SAVENFO, VERSION by prefix; out of a session the line's CRC queues the code.
		ClientLocalCommandSeams seams;
		int keys = 0, nfo = 0;
		std::string version;
		std::vector<std::vector<int32_t>> actions;
		seams.save_keys = [&] { ++keys; };
		seams.save_nfo = [&] { ++nfo; };
		seams.version_text = "V1.7.5.7";
		seams.title_text = "Joint Operations: Typhoon Rising";
		seams.version_line = [&](const std::string &text) { version = text; };
		seams.queue_action = [&](int32_t code, int32_t p1, int32_t p2, int32_t p3) {
			actions.push_back({code, p1, p2, p3});
		};
		seams.in_session = true;
		check(client_process_local_command("/PB_SV_RESTART", seams), "a pb_ line is taken");
		check(client_process_local_command("savekeys", seams) && keys == 1, "SAVEKEYS");
		check(client_process_local_command("SAVENFO now", seams) && nfo == 1, "SAVENFO");
		check(client_process_local_command("VERSION", seams) &&
						version == "Version V1.7.5.7  - Joint Operations: Typhoon Rising",
				"VERSION's line");
		const std::string forced = forced_crc_line("x", text_dword("Crou"));
		check(vfs_version_crc(reinterpret_cast<const uint8_t *>(forced.data()), forced.size()) ==
						text_dword("Crou"),
				"the forced line's CRC is the token's text");
		check(!client_process_local_command(forced, seams) && actions.empty(),
				"in a session the CRC arm does not run");
		seams.in_session = false;
		check(client_process_local_command(forced, seams) && actions.size() == 1 &&
						actions[0] == std::vector<int32_t>{169, 0, 0, 0},
				"out of a session a line whose CRC meets a token's text queues its code");
		check(!client_process_local_command("hello", seams) && actions.size() == 1,
				"out of a session any other line is not taken");
	}
	{
		// The 0x06 copy: to the NUL, the body's end, or 1023 characters.
		check(client_console_echo_line(bytes("SERVER x")) == "SERVER x", "copy to the NUL");
		check(client_console_echo_line(bytes("abc", false)) == "abc", "copy to the end");
		check(client_console_echo_line(std::vector<uint8_t>(2000, 'A')).size() == 1023,
				"copy at most 1023");
	}
	if (failures == 0) std::printf("client_console: all passed\n");
	return failures == 0 ? 0 : 1;
}
