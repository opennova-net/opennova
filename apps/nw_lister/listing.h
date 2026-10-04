#pragma once

#include <net/novaworld/lobby_vars.h> // HostRegistration / HostPlayerSlot

#include <string>
#include <vector>

namespace opennova::lister {

// One listing, as the JSON file states it (README.md's table, one key per column). The
// registration keys the session owns (LobbyName, AppId, HostKey, PCIDKey) are not the file's.
struct Listing {
	HostRegistration columns;
	// The named players. A player without a "slot" carries slot -1: the lister keeps a stable
	// slot for each name.
	std::vector<HostPlayerSlot> players;
};

// Parse the listing JSON. On failure returns false and sets `error`.
bool load_listing(const std::string &path, Listing &out, std::string &error);

// KEY=VALUE credentials: NOVAWORLD_USER / NOVAWORLD_PASS for the account login, ADMIN_USER /
// ADMIN_PASS for the game server's remote-admin port (--admin). The caller registers every value
// as a log secret.
struct Credentials {
	std::string user;
	std::string pass;
	std::string admin_user;
	std::string admin_pass;
	bool present() const { return !user.empty() && !pass.empty(); }
	bool admin_present() const { return !admin_user.empty() && !admin_pass.empty(); }
};
bool load_credentials(const std::string &path, Credentials &out, std::string &error);

} // namespace opennova::lister
