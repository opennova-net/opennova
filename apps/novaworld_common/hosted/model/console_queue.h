#pragma once

// The console routes' commands, through the database (migration 0008,
// hosted_console_commands and hosted_console_replies): the web routes never
// hold an admin link (apps/novaworld_fleet/hosted/admin_link.h); the fleet
// leader does. A route writes its command with the deadline its budget allows
// and waits for its row to be done; the fleet's ConsoleDrain
// (apps/novaworld_fleet/hosted/console_drain.h) claims the queued commands
// (FOR UPDATE SKIP LOCKED) under its leader's generation, runs each on its
// server's link off the drain's thread (a thread a command, so one slow server
// holds up no other), and writes what came back: the outcome, whether the
// command was sent, each reply packet in the server's code page. A command an
// earlier leader claimed and never answered (it went away) the next one's
// drain answers link down; a route that has no done row past its wait says the
// command was not sent (only a done row says it was). The console ring
// (hosted_server_console) is still the link's to write. Service policy, not a
// port.

#include "admin_link_status.h"

#include <net/novaworld/db/database.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

// How long past its deadline a route waits for the fleet's own answer to a
// command it handed the link (its TimedOut says whether it was sent).
inline constexpr int64_t kConsoleAnswerGraceMs = 500;
// How often a route reads its command's row while it waits.
inline constexpr int64_t kConsolePollMs = 15;

// The route's run: `command` on server `server_id`'s link (as AdminLink::run),
// through the fleet, waiting at most `wait` (and the grace) for its answer; a
// command still queued when the wait ends is withdrawn, never sent, and one
// claimed but not answered by then is TimedOut, not sent (whether its leader
// sent it is unknown). The calling thread holds no lease. Throws
// db::Error on a database failure.
AdminCommandResult run_console_command(db::ConnectionPool &pool, int64_t server_id,
		const std::string &command, const AdminCommandOptions &options,
		std::chrono::milliseconds wait);

} // namespace opennova::novaworld_server::hosted
