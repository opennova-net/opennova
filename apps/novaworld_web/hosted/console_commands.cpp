#include "console_commands.h"

#include "core/loggable.h"
#include "hosted/hosted_settings.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>

#include <string>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

ConsoleCommand make(ConsoleVerb verb, std::string line) {
	ConsoleCommand c;
	c.verb = verb;
	c.line = std::move(line);
	return c;
}

ConsoleRefusal refusal(const char *error, std::string message) {
	return ConsoleRefusal{ error, std::move(message) };
}

bool starts_with(const std::string &text, const char *prefix) {
	return strutil::starts_with_icase(text, prefix);
}

ConsoleOutcome ok() {
	return ConsoleOutcome{};
}

ConsoleOutcome failed(int status, const char *error, const char *message) {
	return ConsoleOutcome{ status, error, message };
}

// A reply this verb never draws from a retail server: the server lacks the right
// or the line, which the service composed, did not parse there.
ConsoleOutcome unexpected() {
	return failed(502, "admin_error", "the server answered the command with something else");
}

// The PLAYER sub-verbs' shared target miss, answered before the sub-verb is
// read. [orig: CAdminServer_HandlePlayer @0x40407E..0x404136]
constexpr const char *kPlayerNotFound = "ERROR - Player Not Found.";
constexpr const char *kNotInGame = "ERROR - Not in Game State.";
constexpr const char *kMustBeInGame = "ERROR - Must be in Game State to cycle server.";

} // namespace

const char *console_verb_name(ConsoleVerb verb) {
	switch (verb) {
		case ConsoleVerb::PlayerList: return "players";
		case ConsoleVerb::PlayerPunt: return "punt";
		case ConsoleVerb::PlayerBan: return "ban";
		case ConsoleVerb::PlayerSwapTeam: return "swap";
		case ConsoleVerb::PlayerKill: return "kill";
		case ConsoleVerb::MissionAvailable: return "missions";
		case ConsoleVerb::MissionList: return "rotation";
		case ConsoleVerb::MissionAdd: return "rotation add";
		case ConsoleVerb::MissionRemove: return "rotation remove";
		case ConsoleVerb::MissionSetNext: return "rotation next";
		case ConsoleVerb::MissionCycle: return "rotation cycle";
		case ConsoleVerb::ChatGet: return "chat";
		case ConsoleVerb::ChatSend: return "chat send";
		case ConsoleVerb::GameSettings: return "settings";
		case ConsoleVerb::GameState: return "scene";
	}
	return "?";
}

bool owner_may_run(ConsoleVerb verb) {
	return verb != ConsoleVerb::GameSettings;
}

bool console_verb_writes(ConsoleVerb verb) {
	switch (verb) {
		case ConsoleVerb::PlayerList:
		case ConsoleVerb::MissionAvailable:
		case ConsoleVerb::MissionList:
		case ConsoleVerb::ChatGet:
		case ConsoleVerb::GameSettings:
		case ConsoleVerb::GameState:
			return false;
		default:
			return true;
	}
}

std::optional<ConsoleCommand> console_command(ConsoleVerb verb) {
	switch (verb) {
		case ConsoleVerb::PlayerList: return make(verb, "PLAYER LIST");
		case ConsoleVerb::MissionAvailable: return make(verb, "MISSION AVAILABLE");
		case ConsoleVerb::MissionList: return make(verb, "MISSION LIST");
		case ConsoleVerb::MissionCycle: return make(verb, "MISSION CYCLE");
		case ConsoleVerb::ChatGet: return make(verb, "CHAT GET");
		case ConsoleVerb::GameSettings: return make(verb, "GET GAMESETTINGS");
		case ConsoleVerb::GameState: return make(verb, "GET GAMESTATE");
		default: return std::nullopt;
	}
}

std::optional<ConsoleCommand> console_player_command(ConsoleVerb verb, uint64_t slot, ConsoleRefusal &why) {
	const char *sub = nullptr;
	switch (verb) {
		case ConsoleVerb::PlayerPunt: sub = "PUNT"; break;
		case ConsoleVerb::PlayerBan: sub = "BAN"; break;
		case ConsoleVerb::PlayerSwapTeam: sub = "SWAPTEAM"; break;
		case ConsoleVerb::PlayerKill: sub = "KILL"; break;
		default: return std::nullopt;
	}
	if (slot < kConsoleSlotMin || slot > kConsoleSlotMax) {
		why = refusal("invalid_slot", "a player slot is 1 to 255 (slot 0 is the server's own)");
		return std::nullopt;
	}
	return make(verb, std::string("PLAYER ") + sub + " " + std::to_string(slot));
}

std::optional<ConsoleCommand> console_rotation_command(ConsoleVerb verb, uint64_t index, ConsoleRefusal &why) {
	const char *sub = nullptr;
	switch (verb) {
		case ConsoleVerb::MissionRemove: sub = "REMOVE"; break;
		case ConsoleVerb::MissionSetNext: sub = "SETNEXT"; break;
		default: return std::nullopt;
	}
	if (index > kConsoleIndexMax) {
		why = refusal("invalid_index", "a rotation index is 0 to " + std::to_string(kConsoleIndexMax));
		return std::nullopt;
	}
	return make(verb, std::string("MISSION ") + sub + " " + std::to_string(index));
}

std::optional<ConsoleCommand> console_mission_add(const std::string &file, bool swap_sides, ConsoleRefusal &why) {
	if (!is_mission_file_name(file)) {
		why = refusal("invalid_mission", "a mission is a .bms file name of letters, digits, _ and -");
		return std::nullopt;
	}
	// The launch option argument always given: with none the server sets it to 1.
	// [orig: CAdminServer_HandleMissionAdd @0x403D9F..0x403EDD]
	return make(ConsoleVerb::MissionAdd, "MISSION ADD " + file + (swap_sides ? " 1" : " 0"));
}

std::optional<ConsoleCommand> console_chat_send(const std::string &text, ConsoleRefusal &why) {
	if (has_control_byte(text) || has_c1_control(text)) {
		why = refusal("invalid_text", "the text holds a control character");
		return std::nullopt;
	}
	std::string bytes;
	if (!utf8_to_cp1252(text, bytes)) {
		why = refusal("invalid_text", "the text holds a character the game's code page (cp1252) has none for");
		return std::nullopt;
	}
	const std::string trimmed(strutil::trim_view(bytes));
	if (trimmed.empty()) {
		why = refusal("invalid_text", "the text is empty");
		return std::nullopt;
	}
	if (trimmed.size() > kChatTextMaxBytes) {
		why = refusal("invalid_text", "the text is longer than " + std::to_string(kChatTextMaxBytes) +
						" characters, all a server chat line keeps");
		return std::nullopt;
	}
	return make(ConsoleVerb::ChatSend, "CHAT SEND " + trimmed);
}

// Each verb's replies, as the server view in docs/net/novaworld-net-re.md §6.9 words them.
ConsoleOutcome console_outcome(ConsoleVerb verb, const std::vector<std::string> &replies) {
	if (replies.empty()) {
		// Every curated verb draws a reply (a PLAYER sub-verb only draws none for a sub-verb
		// the server does not know, which no route sends).
		return unexpected();
	}
	const std::string &first = replies.front();
	if (starts_with(first, "USAGE - ")) return unexpected();
	switch (verb) {
		case ConsoleVerb::PlayerList:
			if (first == kNotInGame) return failed(409, "not_in_game", "the server is not in a match");
			// The table, its header first; any other ERROR is none the verb draws.
			// [orig: CAdminServer_HandlePlayer @0x403F63, @0x403F7C]
			return starts_with(first, "ERROR - ") ? unexpected() : ok();
		case ConsoleVerb::PlayerPunt:
		case ConsoleVerb::PlayerSwapTeam:
		case ConsoleVerb::PlayerKill: {
			if (first == kPlayerNotFound) return failed(404, "player_not_found", "no player holds this slot");
			if (first == "ERROR - Cannot punt Host.") return failed(400, "invalid_slot", "slot 0 is the server's own");
			const char *want = verb == ConsoleVerb::PlayerPunt ? "OK - Player punted."
					: verb == ConsoleVerb::PlayerSwapTeam      ? "OK - Player Swapped."
															   : "OK - Player Killed.";
			return first == want ? ok() : unexpected();
		}
		case ConsoleVerb::PlayerBan:
			// [orig: CAdminServer_HandlePlayer @0x40445D..0x404539]
			if (first == kPlayerNotFound) return failed(404, "player_not_found", "no player holds this slot");
			if (first.find("doesn't have a PCID") != std::string::npos)
				return failed(409, "no_pcid", "the player has no PCID to ban (a LAN player)");
			if (first.find("not connected to a network") != std::string::npos)
				return failed(409, "not_connected", "the player has no network connection to ban");
			return replies.back() == "OK - Player Banned." ? ok() : unexpected();
		case ConsoleVerb::MissionAdd:
			if (first == "ERROR - File Not Found.") return failed(404, "mission_not_found", "the server has no such mission");
			return starts_with(first, "OK - Entry Added") ? ok() : unexpected();
		case ConsoleVerb::MissionRemove:
			return first == "OK - Mission Removed." ? ok() : unexpected();
		case ConsoleVerb::MissionSetNext:
			return first == "OK - Next Mission Set." ? ok() : unexpected();
		case ConsoleVerb::MissionCycle:
			if (first == kMustBeInGame) return failed(409, "not_in_game", "the server is not in a match");
			return first == "OK - Server is cycling..." ? ok() : unexpected();
		case ConsoleVerb::ChatSend:
			return first == "OK - Chat sent." ? ok() : unexpected();
		case ConsoleVerb::MissionAvailable:
		case ConsoleVerb::MissionList:
		case ConsoleVerb::ChatGet:
		case ConsoleVerb::GameSettings:
			return starts_with(first, "ERROR - ") ? unexpected() : ok();
		case ConsoleVerb::GameState:
			return console_scene(replies).empty() ? unexpected() : ok();
	}
	return unexpected();
}

std::string console_scene(const std::vector<std::string> &replies) {
	static constexpr const char *kPrefix = "OK - Current State = ";
	if (replies.size() != 1 || !starts_with(replies[0], kPrefix)) return {};
	const std::string scene = replies[0].substr(std::char_traits<char>::length(kPrefix));
	return scene == "Game" || scene == "Menus" || scene == "Unknown" ? scene : std::string();
}

} // namespace opennova::novaworld_server::hosted
