#pragma once

#include <net/novaworld/lobby_vars.h> // HostRegistration / HostPlayerSlot / HostLobbyText

#include <map>
#include <string>
#include <vector>

namespace opennova::lister {

// One listing, as read from the JSON file (re-read every refresh cycle).
struct Listing {
	opennova::HostRegistration reg;        // HostSetup + Host columns
	opennova::HostLobbyText text;          // the STRNOVA/TimeOfDay tokens
	std::vector<opennova::HostPlayerSlot> players;
	int player_count_override = -1;        // "player_count": Players column without names
	std::string installed_exp_bits = "0";  // verify/host Cookie MyInstalledExpBits
	std::string lobby_name_override;       // empty = the gate's LOBBYNAME
};

// Parse the listing JSON. On failure returns false and sets `error`.
bool load_listing(const std::string &path, Listing &out, std::string &error);

// KEY=VALUE credentials (NOVAWORLD_USER / NOVAWORLD_PASS). Values are never
// logged; callers register them with log_add_secret().
struct Credentials {
	std::string user;
	std::string pass;
	bool present() const { return !user.empty() && !pass.empty(); }
};
bool load_credentials(const std::string &path, Credentials &out, std::string &error);

} // namespace opennova::lister
