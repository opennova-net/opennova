#include "console_queue.h"

#include <algorithm>
#include <thread>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;
using Clock = std::chrono::steady_clock;

int64_t steady_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
			.count();
}

// The row's answer, read and the row deleted (its replies with it).
AdminCommandResult take_answer(db::Database &db, int64_t id, const db::Row &row) {
	AdminCommandResult out;
	out.outcome = parse_admin_command_outcome(row.as_text(1).value_or(""))
						  .value_or(AdminCommandOutcome::LinkDown);
	out.sent = row.as_int(2).value_or(0) != 0;
	for (const db::Row &r :
			db.query("SELECT text FROM hosted_console_replies WHERE command_id = $1 ORDER BY n;",
					{ BindValue(id) }))
		out.replies.push_back(r.as_text(0).value_or(""));
	db.exec("DELETE FROM hosted_console_commands WHERE id = $1;", { BindValue(id) });
	return out;
}

} // namespace

AdminCommandResult run_console_command(db::ConnectionPool &pool, int64_t server_id,
		const std::string &command, const AdminCommandOptions &options,
		std::chrono::milliseconds wait) {
	int64_t id = 0;
	{
		auto lease = pool.acquire();
		const auto rows = lease->query(
				"INSERT INTO hosted_console_commands (server_id, line, actor, record, deadline_at) "
				"VALUES ($1, $2, $3, $4, now() + make_interval(secs => $5)) RETURNING id;",
				{ BindValue(server_id), BindValue(command), BindValue(options.actor),
						BindValue(db::Bool{ options.record }),
						BindValue(static_cast<double>(std::max<int64_t>(wait.count(), 0)) /
								1000.0) });
		id = rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
	}
	AdminCommandResult out;
	if (id == 0)
		return out; // LinkDown
	const int64_t give_up = steady_ms() + wait.count();
	const int64_t last = give_up + kConsoleAnswerGraceMs;
	for (;;) {
		{
			auto lease = pool.acquire();
			db::Database &db = *lease;
			const auto rows = db.query(
					"SELECT state, outcome, sent FROM hosted_console_commands WHERE id = $1;",
					{ BindValue(id) });
			if (rows.empty())
				return out; // the fleet let it go: LinkDown
			const std::string state = rows.front().as_text(0).value_or("");
			if (state == "done")
				return take_answer(db, id, rows.front());
			const int64_t now = steady_ms();
			if (state == "queued" && now >= give_up) {
				// Withdrawn while it waits: never sent.
				db.exec("DELETE FROM hosted_console_commands WHERE id = $1 AND state = 'queued';",
						{ BindValue(id) });
				if (db.changes() > 0) {
					out.outcome = AdminCommandOutcome::TimedOut;
					out.sent = false;
					return out;
				}
				continue; // claimed meanwhile: its answer comes
			}
			if (now >= last) {
				// No answer past the grace (its leader slow, or gone after its
				// claim): its answer goes with the row. Only a done row says the
				// command went out.
				db.exec("DELETE FROM hosted_console_commands WHERE id = $1;", { BindValue(id) });
				out.outcome = AdminCommandOutcome::TimedOut;
				out.sent = false;
				return out;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kConsolePollMs));
	}
}

} // namespace opennova::novaworld_server::hosted
