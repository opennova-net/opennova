#include "console_drain.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;
using Clock = std::chrono::steady_clock;

int64_t steady_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
			.count();
}

// A command's answer written to its row (gone already when its route gave up).
void write_answer(db::Database &db, int64_t id, const AdminCommandResult &result) {
	db::Transaction tx(db);
	for (size_t n = 0; n < result.replies.size(); ++n) {
		db.exec("INSERT INTO hosted_console_replies (command_id, n, text) "
				"SELECT $1, $2, $3 WHERE EXISTS (SELECT 1 FROM hosted_console_commands WHERE id = "
				"$1);",
				{ BindValue(id), BindValue(static_cast<int64_t>(n)),
						BindValue(result.replies[n]) });
	}
	db.exec("UPDATE hosted_console_commands SET state = 'done', outcome = $2, sent = $3, "
			"finished_at = now() "
			"WHERE id = $1;",
			{ BindValue(id), BindValue(std::string(admin_command_outcome_name(result.outcome))),
					BindValue(db::Bool{ result.sent }) });
	tx.commit();
}

} // namespace

ConsoleDrain::ConsoleDrain(db::ConnectionPool &pool, AdminLinks &links, int64_t poll_ms) :
		pool_(pool), links_(links), poll_ms_(std::max<int64_t>(poll_ms, 1)) {}

ConsoleDrain::~ConsoleDrain() {
	stop();
}

void ConsoleDrain::start() {
	if (thread_.joinable())
		return;
	// Its leader's generation, which it claims under: the row's now (the drain
	// starts once the fleet program leads).
	try {
		const auto rows =
				pool_.acquire()->query("SELECT generation FROM controller_leader WHERE id = 1;");
		generation_ = rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
	} catch (const db::Error &e) {
		std::fprintf(stderr,
				"[hosted] WARN the console drain: its leader's generation did not read: %s\n",
				e.what());
	}
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = false;
	}
	thread_ = std::thread([this] { run(); });
}

void ConsoleDrain::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	for (Running &r : running_) {
		if (r.thread.joinable())
			r.thread.join();
	}
	running_.clear();
}

size_t ConsoleDrain::reap() {
	for (auto it = running_.begin(); it != running_.end();) {
		if (!it->done.load()) {
			++it;
			continue;
		}
		if (it->thread.joinable())
			it->thread.join();
		it = running_.erase(it);
	}
	return running_.size();
}

void ConsoleDrain::execute(int64_t id, int64_t server_id, std::string line,
		AdminCommandOptions options, std::chrono::milliseconds left) {
	AdminCommandResult result;
	const std::shared_ptr<AdminLink> link = links_.find(server_id);
	if (link)
		result = link->run(line, options, left); // else LinkDown
	try {
		auto lease = pool_.acquire();
		write_answer(*lease, id, result);
	} catch (const db::Error &e) {
		std::fprintf(stderr,
				"[hosted] WARN console command %lld: its answer could not be written: %s\n",
				static_cast<long long>(id), e.what());
	}
}

size_t ConsoleDrain::drain_once() {
	const size_t busy = reap();
	if (busy >= kMaxRunning)
		return 0;
	struct Claimed {
		int64_t id;
		int64_t server_id;
		std::string line;
		AdminCommandOptions options;
		int64_t left_ms;
	};
	std::vector<Claimed> claimed;
	{
		auto lease = pool_.acquire();
		db::Database &db = *lease;
		for (const db::Row &r :
				db.query("UPDATE hosted_console_commands SET state = 'running', "
						 "claimed_generation = $2 WHERE id IN (SELECT "
						 "id FROM "
						 "hosted_console_commands WHERE state = 'queued' ORDER BY id FOR UPDATE "
						 "SKIP LOCKED LIMIT $1) "
						 "RETURNING id, server_id, line, actor, record, "
						 "CAST(extract(epoch FROM deadline_at - now()) * 1000 AS bigint);",
						{ BindValue(static_cast<int64_t>(kMaxRunning - busy)),
								BindValue(generation_) })) {
			Claimed c;
			c.id = r.as_int(0).value_or(0);
			c.server_id = r.as_int(1).value_or(0);
			c.line = r.as_text(2).value_or("");
			c.options.actor = r.as_text(3).value_or("service");
			c.options.record = r.as_int(4).value_or(0) != 0;
			c.left_ms = r.as_int(5).value_or(0);
			claimed.push_back(std::move(c));
		}
		std::sort(claimed.begin(), claimed.end(),
				[](const Claimed &a, const Claimed &b) { return a.id < b.id; });
		// The rows whose routes went away, a minute past their deadline; and the
		// commands an earlier leader claimed and never answered (it went away
		// with them): link down, not sent as far as anyone can tell. At its first
		// turn, then every 10 s.
		const int64_t now = steady_ms();
		if (now - last_cleanup_ >= 10 * 1000) {
			last_cleanup_ = now;
			db.exec("DELETE FROM hosted_console_commands WHERE deadline_at < now() - interval '1 "
					"minute';");
			db.exec("UPDATE hosted_console_commands SET state = 'done', outcome = $2, "
					"sent = false, finished_at = now() "
					"WHERE state = 'running' AND claimed_generation < $1;",
					{ BindValue(generation_),
							BindValue(std::string(
									admin_command_outcome_name(AdminCommandOutcome::LinkDown))) });
			if (db.changes() > 0)
				std::fprintf(stderr,
						"[hosted] WARN the console drain: %d command(s) an earlier leader "
						"claimed answered link down\n",
						db.changes());
		}
		for (const Claimed &c : claimed) {
			if (c.left_ms > 0)
				continue;
			// Its route's budget ran out before the fleet took it: not sent.
			AdminCommandResult late;
			late.outcome = AdminCommandOutcome::TimedOut;
			write_answer(db, c.id, late);
		}
	}
	size_t started = 0;
	for (Claimed &c : claimed) {
		if (c.left_ms <= 0)
			continue;
		Running &r = running_.emplace_back();
		Running *slot = &r;
		r.thread = std::thread([this, slot, c = std::move(c)]() mutable {
			execute(c.id, c.server_id, std::move(c.line), std::move(c.options),
					std::chrono::milliseconds(c.left_ms));
			slot->done.store(true);
		});
		++started;
	}
	return started;
}

void ConsoleDrain::run() {
	std::string last_error;
	for (;;) {
		try {
			drain_once();
			last_error.clear();
		} catch (const std::exception &e) {
			if (last_error != e.what())
				std::fprintf(stderr, "[hosted] WARN the console drain: %s\n", e.what());
			last_error = e.what();
		}
		std::unique_lock<std::mutex> lock(mu_);
		if (cv_.wait_for(lock, std::chrono::milliseconds(poll_ms_), [this] { return stopping_; }))
			return;
	}
}

} // namespace opennova::novaworld_server::hosted
