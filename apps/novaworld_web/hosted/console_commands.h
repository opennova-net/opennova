#pragma once

// The hosted servers' console commands (ADR 0053 d5 and d9, proposed): the
// curated set of retail remote-admin commands the site's console routes
// (console_routes.h) may run on a hosted server's admin link (admin_link.h),
// each composed here from checked arguments, and what each one's replies come
// to. Service policy, not a witnessed limit: there is no passthrough of an
// arbitrary admin line, and nothing reaches the server's files or config (SET,
// WEAPON SET, CMD), its admin users and ban files (ADMINUSER, BANLIST), or its
// session's end (GOTO, MISSION CLEAR, QUIT) outside the supervisor's stop.
//
// The owners' commands (owner_may_run):
//   PLAYER LIST                 the players (read)
//   PLAYER PUNT <slot>          a punt (slot 1..255; 0 is the host's own)
//   PLAYER BAN <slot>           a ban: the PCID listed on a NovaWorld host, then the punt
//   PLAYER SWAPTEAM <slot>      the player to the other team
//   PLAYER KILL <slot>          the player killed
//   MISSION AVAILABLE           the mission catalog (read)
//   MISSION LIST                the rotation (read)
//   MISSION ADD <file> <0|1>    a map appended to the rotation, its sides swapped at the
//                               half or not (no insert position: retail's insert leaves the
//                               next-map cursor unset when the list grows; no ONESHOT), up
//                               to kConsoleRotationMax entries
//   MISSION REMOVE <index>      an entry removed (never the last: the routes read the
//                               rotation first, since an empty one ends the session)
//   MISSION SETNEXT <index>     the next map (an index the rotation has: the routes check,
//                               since the server answers OK either way)
//   MISSION CYCLE               the round ends now and the next map loads
//   CHAT GET                    the chat window (read)
//   CHAT SEND <text>            a server chat line
// The admins' add one read: GET GAMESETTINGS (the live settings, the game and
// side passwords among them). The routes also read GET GAMESTATE themselves (a
// rotation edit's scene check), never as a caller's command.
//
// Every argument is checked before a line is composed: a slot is 1..255 (the
// server compares `(u8)atol`, so 256 would be the host's 0), an index below
// kConsoleRotationMax, a mission a .bms name of letters, digits, '_' and '-'
// (is_mission_file_name), and chat text the host's cp1252 with no control byte
// (C1 included), not blank, at most kChatTextMaxBytes. The routes take the slot
// and the index from the path as unsigned integers, so no command line ever
// carries a caller's byte outside these rules.

#include "hosted/hosted_settings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

enum class ConsoleVerb : uint8_t {
	PlayerList,
	PlayerPunt,
	PlayerBan,
	PlayerSwapTeam,
	PlayerKill,
	MissionAvailable,
	MissionList,
	MissionAdd,
	MissionRemove,
	MissionSetNext,
	MissionCycle,
	ChatGet,
	ChatSend,
	GameSettings, // the admins' alone
	GameState,    // the routes' own scene check (GET GAMESTATE), no caller's
};
// The verb's name in the log ("punt", "rotation", ...).
const char *console_verb_name(ConsoleVerb verb);
// Whether an owner may run it on their own server: every verb but GameSettings.
bool owner_may_run(ConsoleVerb verb);
// Whether it changes the server: the console records it and its replies (a read
// the site polls is not recorded), and the owner's write bucket pays for it.
bool console_verb_writes(ConsoleVerb verb);

// A slot: the server matches `(u8)atol` of the target against each active
// slot's number, so 1..255 (0 is the server's own, and 256 would read as 0).
// [orig: CAdminServer_HandlePlayer @0x40407E..0x404136]
inline constexpr uint64_t kConsoleSlotMin = 1;
inline constexpr uint64_t kConsoleSlotMax = 255;
// The longest rotation the console builds (service policy, not a server limit:
// retail's rotation regrows as it fills, MissionRotation_Append @0x5019D0
// @0x501A0C): the start settings' kMissionsMax, so an index is below it and an
// add to a full rotation is refused.
inline constexpr uint64_t kConsoleRotationMax = kMissionsMax;
inline constexpr uint64_t kConsoleIndexMax = kConsoleRotationMax - 1;
// The chat text a server line keeps: CHAT SEND's sender cuts it to 59 in its
// flood check (and retail joins the words, each with a space, into a 122-byte
// buffer with no bound, D-NET-362, which 59 keeps within). The same line again
// within 0x500 main frames (about 20 s) is dropped there, and the reply says
// `OK - Chat sent.` all the same. [orig: CAdminServer_HandleChatCommand
//  @0x404BE8..0x404C74; Chat_CheckFloodControl @0x498F60 (message[59] = 0
//  @0x498F80, the repeat @0x499028)]
inline constexpr size_t kChatTextMaxBytes = 59;

// One command, as the link sends it: the line in the server's cp1252.
struct ConsoleCommand {
	ConsoleVerb verb = ConsoleVerb::PlayerList;
	std::string line;
};

// A refused argument: the error code the route answers 400 with, and why.
struct ConsoleRefusal {
	std::string error;
	std::string message;
};

// The commands with no argument: PlayerList, MissionAvailable, MissionList,
// MissionCycle, ChatGet, GameSettings, GameState. Nullopt for any other verb.
std::optional<ConsoleCommand> console_command(ConsoleVerb verb);
// PLAYER PUNT / BAN / SWAPTEAM / KILL <slot>.
std::optional<ConsoleCommand> console_player_command(ConsoleVerb verb, uint64_t slot, ConsoleRefusal &why);
// MISSION REMOVE / SETNEXT <index>.
std::optional<ConsoleCommand> console_rotation_command(ConsoleVerb verb, uint64_t index, ConsoleRefusal &why);
// MISSION ADD <file> <0|1>, the file as UTF-8.
std::optional<ConsoleCommand> console_mission_add(const std::string &file, bool swap_sides, ConsoleRefusal &why);
// CHAT SEND <text>, the text as UTF-8 (converted to cp1252).
std::optional<ConsoleCommand> console_chat_send(const std::string &text, ConsoleRefusal &why);

// What a command's replies come to: 200, or the error code and status the
// route answers with (our own words, never the server's text).
struct ConsoleOutcome {
	int status = 200;
	std::string error;
	std::string message;
};
ConsoleOutcome console_outcome(ConsoleVerb verb, const std::vector<std::string> &replies);
// GET GAMESTATE's scene: "Game" (the Game Loop), "Menus" or "Unknown"; "" for a
// reply that names none. [orig: CAdminServer_HandleGet @0x4033C4..0x40340B]
std::string console_scene(const std::vector<std::string> &replies);

} // namespace opennova::novaworld_server::hosted
