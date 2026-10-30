#include "console_drain.h"
#include "hosted/model/console_configuration.h"
#include <net/admin/admin_protocol.h>
#include <base/io/cp1252.h>
#include <base/io/strutil.h>

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
			"finished_at = now(), configuration_saved = $4, settings_revision = $5, "
			"delivery_unknown = $6, refused_command = $7 "
			"WHERE id = $1;",
			{ BindValue(id), BindValue(std::string(admin_command_outcome_name(result.outcome))),
					BindValue(db::Bool{ result.sent }),
					BindValue(db::Bool{ result.configuration_saved }),
					BindValue(result.settings_revision),
					BindValue(db::Bool{ result.delivery_unknown }),
					BindValue(result.refused_command) });
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
				pool_.acquire()->query("SELECT generation FROM fleet_leader WHERE id = 1;");
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

void ConsoleDrain::execute(int64_t id, int64_t server_id, int64_t server_generation,
		std::string line, AdminCommandOptions options, std::chrono::milliseconds left) {
	AdminCommandResult result;
	const std::shared_ptr<AdminLink> link = links_.find(server_id);
	std::optional<HostedServer> before;
	io::JsonValue patch = io::JsonValue::make_object();
	try {
		gamecfg::WeaponRoster weapons;
		{
			auto lease = pool_.acquire();
			before = find_server(*lease, server_id);
			if (before && before->base_tree_id) {
				if (const auto tree = find_tree(*lease, *before->base_tree_id))
					weapons = tree->weapons;
				if (before->expansion_tree_id)
					if (const auto tree = find_tree(*lease, *before->expansion_tree_id);
							tree && !tree->weapons.empty())
						weapons = tree->weapons;
			}
		}
		if (link && before && before->generation == server_generation &&
				link->target().generation == server_generation) {
			if (options.expected_revision &&
					*options.expected_revision != before->settings_revision) {
				result.outcome = AdminCommandOutcome::Refused;
				result.replies = {
					"ERROR - Configuration changed; reload before applying this edit."
				};
			} else {
				options.finish_sent = true;
				if (options.settings_patch.empty())
					result = link->run(line, options, left);
				else {
					std::string error, field;
					io::JsonValue requested;
					if (!io::json_parse(options.settings_patch, requested, error))
						result.outcome = AdminCommandOutcome::Refused;
					else {
						const auto edit = configuration_edit(requested, weapons);
						HostedSettings accepted = before->settings;
						(void)read_settings_document(edit.next_launch, accepted, field, error);
						result.outcome = AdminCommandOutcome::Answered;
						for (const auto &command : edit.commands) {
							const auto reply = link->run(command, options, std::chrono::seconds(5));
							result.sent = result.sent || reply.sent;
							result.replies.insert(result.replies.end(), reply.replies.begin(),
									reply.replies.end());
							if (reply.outcome != AdminCommandOutcome::Answered) {
								result.outcome = reply.outcome;
								break;
							}
							if (!console_reply_succeeded(reply.replies)) {
								// The server refused it: the edit stops here, its earlier
								// commands' fields saved, and the answer names this one.
								result.refused_command = command;
								break;
							}
							(void)read_settings_document(console_settings_patch(command, weapons),
									accepted, field, error);
						}
						patch = settings_document(accepted);
					}
				}
			}
		}
		if (options.settings_patch.empty() && result.outcome == AdminCommandOutcome::Answered &&
				console_reply_succeeded(result.replies)) {
			patch = console_settings_patch(line, weapons);
			if (console_changes_rotation(line)) {
				const auto args = admin_split_command(line);
				if (args.size() >= 2 && strutil::iequals(args[1], "CLEAR")) {
					// A cleared live rotation leaves the saved one for the next launch: a
					// start needs at least one mission (check_settings), and a restart
					// from an empty one would write a host file with none.
				} else {
					AdminCommandOptions read;
					read.record = false;
					read.finish_sent = true;
					const auto scene = link->run("GET GAMESTATE", read, std::chrono::seconds(5));
					if (scene.outcome == AdminCommandOutcome::Answered && !scene.replies.empty() &&
							scene.replies.front().find("= Game") != std::string::npos) {
						const auto rotation =
								link->run("MISSION LIST", read, std::chrono::seconds(5));
						if (rotation.outcome == AdminCommandOutcome::Answered &&
								!rotation.replies.empty()) {
							auto rows = io::JsonValue::make_array();
							for (const auto &entry :
									admin_parse_mission_list(rotation.replies.front()))
								if (!entry.one_shot) {
									auto row = io::JsonValue::make_object();
									row.set("file", io::json_string(cp1252_to_utf8(entry.file)));
									row.set("swap_sides",
											io::JsonValue::make_bool(entry.swap_sides));
									rows.push(std::move(row));
								}
							// An emptied live rotation keeps the saved one (the CLEAR rule above).
							if (!rows.array.empty())
								patch.set("rotation", std::move(rows));
						}
					}
				}
			}
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hosted] WARN console command %lld: %s\n", static_cast<long long>(id),
				e.what());
	}
	result.delivery_unknown = result.sent && result.outcome != AdminCommandOutcome::Answered;
	// Retry only persistence, never the TCP command. The result and configuration
	// commit together, even after the HTTP caller has gone away. Checking the row
	// also handles a COMMIT whose acknowledgement was lost.
	for (;;) {
		try {
			auto lease = pool_.acquire();
			db::Transaction tx(*lease);
			const auto row = lease->query(
					"SELECT state FROM hosted_console_commands WHERE id = $1 FOR UPDATE;",
					{ BindValue(id) });
			if (row.empty() || row.front().as_text(0).value_or("") == "done")
				return;
			result.configuration_saved = false;
			result.settings_revision = 0;
			if (!patch.object.empty() && before) {
				// A concurrent saved edit wins; never replace a newer revision or process.
				std::string field, error;
				HostedSettings settings = before->settings;
				if (read_settings_document(patch, settings, field, error)) {
					result.configuration_saved = save_server_settings(*lease, server_id,
							before->settings_revision, server_generation, settings);
					if (result.configuration_saved)
						result.settings_revision = before->settings_revision + 1;
				}
			}
			write_answer(*lease, id, result);
			tx.commit();
			return;
		} catch (const db::Error &e) {
			std::fprintf(stderr,
					"[hosted] WARN console command %lld: its answer could not be written: %s\n",
					static_cast<long long>(id), e.what());
		}
		std::unique_lock<std::mutex> lock(mu_);
		if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; }))
			return;
	}
}

size_t ConsoleDrain::drain_once() {
	const size_t busy = reap();
	if (busy >= kMaxRunning)
		return 0;
	struct Claimed {
		int64_t id;
		int64_t server_id;
		int64_t server_generation;
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
						 "hosted_console_commands q WHERE state = 'queued' AND NOT EXISTS "
						 "(SELECT 1 FROM hosted_console_commands older WHERE older.server_id = "
						 "q.server_id "
						 "AND older.state IN ('queued', 'running') AND older.id < q.id) "
						 "ORDER BY id FOR UPDATE "
						 "SKIP LOCKED LIMIT $1) "
						 "RETURNING id, server_id, line, actor, record, "
						 "CAST(extract(epoch FROM deadline_at - now()) * 1000 AS bigint), "
						 "server_generation, settings_revision, settings_patch::text;",
						{ BindValue(static_cast<int64_t>(kMaxRunning - busy)),
								BindValue(generation_) })) {
			Claimed c;
			c.id = r.as_int(0).value_or(0);
			c.server_id = r.as_int(1).value_or(0);
			c.line = r.as_text(2).value_or("");
			c.options.actor = r.as_text(3).value_or("service");
			c.options.record = r.as_int(4).value_or(0) != 0;
			c.left_ms = r.as_int(5).value_or(0);
			c.server_generation = r.as_int(6).value_or(0);
			c.options.expected_revision = r.as_int(7);
			c.options.settings_patch = r.as_text(8).value_or("");
			claimed.push_back(std::move(c));
		}
		std::sort(claimed.begin(), claimed.end(),
				[](const Claimed &a, const Claimed &b) { return a.id < b.id; });
		// The rows whose routes went away, a minute past their deadline; and the
		// commands an earlier leader claimed and never answered: delivery is
		// uncertain, so record it without replaying. At its first turn, then
		// every 10 s.
		const int64_t now = steady_ms();
		if (now - last_cleanup_ >= 10 * 1000) {
			last_cleanup_ = now;
			db.exec("DELETE FROM hosted_console_commands WHERE state != 'running' AND deadline_at "
					"< now() - interval '1 "
					"minute';");
			db.exec("UPDATE hosted_console_commands SET state = 'done', outcome = $2, "
					"delivery_unknown = true, finished_at = now() "
					"WHERE state = 'running' AND claimed_generation < $1;",
					{ BindValue(generation_),
							BindValue(std::string(
									admin_command_outcome_name(AdminCommandOutcome::LinkDown))) });
			if (db.changes() > 0)
				std::fprintf(stderr,
						"[hosted] WARN the console drain: %d command(s) an earlier leader "
						"claimed have unknown delivery\n",
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
			execute(c.id, c.server_id, c.server_generation, std::move(c.line), std::move(c.options),
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
