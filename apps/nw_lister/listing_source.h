#pragma once

#include <net/napi/session.h>              // ServerCommand
#include <net/novaworld/client_session.h>  // ClientSession::PlayerEnterResult
#include <net/novaworld/lobby_vars.h>      // HostRegistration / HostPlayerSlot

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace opennova::nw_lister {

// The process exit codes.
enum ExitCode : int {
	kExitStopped = 0,          // stopped and deregistered
	kExitUsage = 1,            // bad command line
	kExitBadInput = 2,         // the listing or credentials file did not load
	kExitNetwork = 3,          // a socket could not be opened
	kExitFailed = 4,           // the session, the login, the HOSTKEY or the host request failed
	kExitStoppedByService = 5, // the service stopped the hosting (ServerStopHosting)
};

// What a Lister lists, and the server behind it. The Lister owns the NovaWorld session (the gate,
// the NWU session, the login and HOSTKEY, the host role) and the keys only the session knows
// (LobbyName, HostKey, AppId, the PCIDKey ring, the machine locale); a source owns the columns and
// the roster, and takes the session's host-direction traffic. opennova-nw-lister's sources are
// the listing file and the game server's admin port (FileListingSource); opennova-serve's is the
// live in-process match.
class ListingSource {
public:
	virtual ~ListingSource() = default;

	// Before the session starts: read what the first registration needs. False with `exit_code`
	// (an ExitCode) when there is nothing to list.
	virtual bool start(int &exit_code) = 0;
	// Look for changes at the lister's wall clock; `force` asks for a fresh read whatever the
	// source's own cadence. True when the columns or the roster changed: the lister then hands
	// the host role the columns (they ride its next refresh) and syncs the roster (it goes out at
	// once).
	virtual bool refresh(uint32_t now_ms, bool force) = 0;
	// The listing columns.
	virtual HostRegistration registration() const = 0;
	// The PlayerList roster, given the slots the session lists now (`current`, by slot).
	virtual std::vector<HostPlayerSlot> wanted_roster(const std::map<int, HostPlayerSlot> &current) const = 0;

	// The host request's outcome: hosting (ServerHostResult success), or the NWEC tag it failed
	// with (a refusal, the 60 s poll, ServerStopHosting's message key).
	virtual void on_host_result(bool hosting, const std::string &failure) {
		(void)hosting;
		(void)failure;
	}
	// A ServerCommand for the listed server.
	virtual void on_command(const ServerCommand &command) = 0;
	// ServerPlayerEnterResult for a joiner the server announced.
	virtual void on_player_enter_result(const ClientSession::PlayerEnterResult &result) { (void)result; }
	// The lister is ending: release the source's own resources.
	virtual void stop() {}
};

} // namespace opennova::nw_lister
