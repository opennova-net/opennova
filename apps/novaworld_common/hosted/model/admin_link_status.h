#pragma once

// What the web routes read of the hosted servers' admin links (the links
// themselves are the fleet program's, apps/novaworld_fleet/hosted/admin_link.h):
// a link's state and status as the fleet last published it to its row
// (hosted_admin_links, migration 0008), and the shapes a console command goes
// and comes back in through the console queue (console_queue.h): its options,
// its outcome and its replies. Crow-free. Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

enum class AdminLinkState : uint8_t {
	Connecting, // logging in (the first time, or again after the backoff)
	Up,         // logged in: commands run
	Down,       // the last login failed or the session was lost; the backoff runs
	Stopped,    // ended
};
const char *admin_link_state_name(AdminLinkState state);
std::optional<AdminLinkState> parse_admin_link_state(std::string_view text);

struct AdminLinkStatus {
	AdminLinkState state = AdminLinkState::Connecting;
	std::string game_state;   // the last poll's scene: "Game", "Menus" or "Unknown"; "" before one
	std::string last_poll_at; // UTC 'YYYY-MM-DD HH:MM:SS' of the last answered poll, "" before one
	std::string up_since;     // UTC, the session's login; "" while not up
	std::string error;        // why the last login or session failed; "" while up
};

enum class AdminCommandOutcome : uint8_t {
	Answered, // sent, and every reply its count names read (an ERROR or USAGE reply is an answer)
	Refused,  // no command a retail server takes in one packet (empty, a control byte, too long)
	LinkDown, // no logged-in session to send it on, or the session was lost on it
	Busy,     // the queue is full
	TimedOut, // no answer within the caller's wait (`sent` says whether it went out)
	Stopped,  // the link is ending
};
const char *admin_command_outcome_name(AdminCommandOutcome outcome);
std::optional<AdminCommandOutcome> parse_admin_command_outcome(std::string_view text);

struct AdminCommandResult {
	AdminCommandOutcome outcome = AdminCommandOutcome::LinkDown;
	bool sent = false;
	// Each reply packet's text, in the server's code page (cp1252), in order.
	std::vector<std::string> replies;
};

struct AdminCommandOptions {
	std::string actor = "service"; // the console's sender: "owner", "admin" or "service"
	bool record = true;            // the command and its replies go to the console
};

// A row whose seen_at is older than this reads as no link: no fleet leader
// keeps it (published every pass, refreshed every 5 s).
inline constexpr int64_t kAdminLinkStaleS = 15;

// Server `server_id`'s link as the fleet last published it (the web routes'
// read); nullopt when it has none, or its row went stale.
std::optional<AdminLinkStatus> admin_link_status(db::Database &db, int64_t server_id);

} // namespace opennova::novaworld_server::hosted
