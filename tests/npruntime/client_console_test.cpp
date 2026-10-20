// D-NET-710: the client's console command walk (inmatch/client_console.h), each arm against
// the cited original's arithmetic: the five debug commands' lines, state and sends, the
// server command table's match (an ASCII name never meets a typed line; a negative length
// takes any; a high-byte name folds through the locale), the C2S 0x07 body's nibble swap,
// the walk's order, and the S2C 0x06 copy.
// [orig: NapiClientCmd_ParseAndExecute @0x4329C0; NapiClientCmd_HandleDebugCommand @0x42C4D0;
//  NapiClientCmd_HandleDebug @0x42CCE0; NetPacket_WriteObfuscatedString @0x42AEA0;
//  NapiNPClientMsg_HandleChatCommand @0x432BC0]
#include <runtime/inmatch/client_console.h>

#include <net/npwire/ingame_message_id.h>

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
		// byte sign-extends before the XOR), so the walk falls through to the bindings.
		Run r(true, false, false);
		r.entry(0, 0x00, 4, bytes("NULL", false));
		check(!r.run("NULL hello") && r.sends.empty(), "an ASCII name never matches");
		check(!r.run("null") && r.sends.empty(), "nor in lower case");
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
		bool bound = false;
		b.ctx.queue_binding = [&](const io::ConfigTokens &tokens) {
			bound = tokens.count == 2 && std::string(tokens.token(0)) == "ScreenShot";
			return bound;
		};
		check(b.run("ScreenShot 1") && bound, "an untaken line reaches the binding arm");
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
