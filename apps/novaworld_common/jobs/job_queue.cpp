#include "job_queue.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <variant>

namespace opennova::novaworld_server::jobs {

namespace {

using db::BindValue;

BindValue text_or_null(const std::string &text) {
	return text.empty() ? BindValue(std::monostate{}) : BindValue(text);
}

BindValue seconds(int64_t ms) {
	return BindValue(static_cast<double>(std::max<int64_t>(ms, 0)) / 1000.0);
}

// A text[] literal of `items` (each quoted, its quotes and backslashes escaped).
std::string text_array(const std::vector<std::string> &items) {
	std::string out = "{";
	for (size_t i = 0; i < items.size(); ++i) {
		if (i > 0)
			out += ',';
		out += '"';
		for (const char c : items[i]) {
			if (c == '"' || c == '\\')
				out += '\\';
			out += c;
		}
		out += '"';
	}
	return out + "}";
}

std::string bigint_array(const std::vector<int64_t> &items) {
	std::string out = "{";
	for (size_t i = 0; i < items.size(); ++i)
		out += (i > 0 ? "," : "") + std::to_string(items[i]);
	return out + "}";
}

io::JsonValue parse_payload(const std::string &text) {
	io::JsonValue out;
	std::string error;
	if (!io::json_parse(text, out, error) || !out.is_object())
		return io::JsonValue::make_object();
	return out;
}

// A transaction of the claim's own: BEGIN, the claim lock, COMMIT; rolled back
// when it unwinds: claims serialize only with one another.
class ClaimTransaction {
public:
	explicit ClaimTransaction(db::Database &db) : db_(db) {
		assert(!db.in_transaction() && "a claim runs on a connection with no transaction open");
		db_.begin();
		open_ = true;
		db_.exec("SELECT pg_advisory_xact_lock($1);", { BindValue(db::kJobsClaimLock) });
	}
	~ClaimTransaction() {
		if (!open_)
			return;
		try {
			db_.rollback();
		} catch (const db::Error &) {
			// Lost with the connection: nothing is left open.
		}
	}
	ClaimTransaction(const ClaimTransaction &) = delete;
	ClaimTransaction &operator=(const ClaimTransaction &) = delete;
	void commit() {
		db_.commit();
		open_ = false;
	}

private:
	db::Database &db_;
	bool open_ = false;
};

// A secret dropped as its job ends: '' where one was (NULL stays NULL), so a
// row still says it carried one (the admin's retry refuses a job whose secret
// is gone) and never what it was.
constexpr const char *kDropSecret = "CASE WHEN secret IS NULL THEN NULL ELSE '' END";

// The claim's compare-and-set: the job, still running under this claim.
constexpr const char *kClaimed =
		"id = $1 AND state = 'running' AND worker_id = $2 AND attempts = $3";

std::vector<BindValue> claim_binds(const ClaimedJob &job) {
	return { BindValue(job.id), BindValue(job.worker_id),
		BindValue(static_cast<int64_t>(job.attempts)) };
}

} // namespace

const char *job_state_name(JobState state) {
	switch (state) {
		case JobState::Queued:
			return "queued";
		case JobState::Running:
			return "running";
		case JobState::Done:
			return "done";
		case JobState::Failed:
			return "failed";
	}
	return "queued";
}

int64_t enqueue(db::Database &db, const NewJob &job) {
	const auto rows =
			db.query("INSERT INTO jobs (kind, payload, secret, dedupe_key, schedule_key, run_at, "
					 "max_attempts) "
					 "VALUES ($1, $2::jsonb, $3, $4, $5, now() + make_interval(secs => $6), $7) "
					 "ON CONFLICT DO NOTHING RETURNING id;",
					{ BindValue(job.kind), BindValue(io::json_write(job.payload)),
							text_or_null(job.secret), text_or_null(job.dedupe_key),
							text_or_null(job.schedule_key), seconds(job.delay_ms),
							BindValue(static_cast<int64_t>(std::max(job.max_attempts, 1))) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

std::optional<ClaimedJob> claim(db::Database &db, const std::string &worker_id,
		const std::vector<std::string> &kinds, const std::map<std::string, int> &caps,
		int64_t lease_ms) {
	if (kinds.empty())
		return std::nullopt;
	ClaimTransaction tx(db);
	std::vector<std::string> allowed = kinds;
	std::vector<std::string> capped;
	for (const std::string &kind : kinds) {
		const auto it = caps.find(kind);
		if (it != caps.end() && it->second > 0)
			capped.push_back(kind);
	}
	if (!capped.empty()) {
		std::map<std::string, int64_t> running;
		for (const db::Row &r :
				db.query("SELECT kind, COUNT(*) FROM jobs WHERE state = 'running' AND "
						 "kind = ANY($1::text[]) GROUP BY kind;",
						{ BindValue(text_array(capped)) }))
			running[r.as_text(0).value_or("")] = r.as_int(1).value_or(0);
		allowed.erase(std::remove_if(allowed.begin(), allowed.end(),
							  [&](const std::string &kind) {
								  const auto cap = caps.find(kind);
								  const auto now = running.find(kind);
								  return cap != caps.end() && cap->second > 0 &&
										  now != running.end() && now->second >= cap->second;
							  }),
				allowed.end());
	}
	if (allowed.empty())
		return std::nullopt;
	const auto rows = db.query(
			"UPDATE jobs SET state = 'running', attempts = attempts + 1, worker_id = $1, "
			"lease_until = now() + make_interval(secs => $2), started_at = now() "
			"WHERE id = (SELECT id FROM jobs WHERE state = 'queued' AND run_at <= now() AND "
			"kind = ANY($3::text[]) ORDER BY run_at, id FOR UPDATE SKIP LOCKED LIMIT 1) "
			"RETURNING id, kind, payload::text, secret, attempts, max_attempts;",
			{ BindValue(worker_id), seconds(lease_ms), BindValue(text_array(allowed)) });
	tx.commit();
	if (rows.empty())
		return std::nullopt;
	const db::Row &r = rows.front();
	ClaimedJob job;
	job.id = r.as_int(0).value_or(0);
	job.kind = r.as_text(1).value_or("");
	job.payload = parse_payload(r.as_text(2).value_or("{}"));
	job.secret = r.as_text(3).value_or("");
	job.attempts = static_cast<int>(r.as_int(4).value_or(1));
	job.max_attempts = static_cast<int>(r.as_int(5).value_or(1));
	job.worker_id = worker_id;
	return job;
}

bool complete(db::Database &db, const ClaimedJob &job) {
	db.exec(std::string("UPDATE jobs SET state = 'done', finished_at = now(), lease_until = NULL, "
						"secret = ") +
					kDropSecret + " WHERE " + kClaimed + ";",
			claim_binds(job));
	return db.changes() > 0;
}

bool retry(db::Database &db, const ClaimedJob &job, const std::string &error, int64_t delay_ms) {
	std::vector<BindValue> binds = claim_binds(job);
	binds.push_back(seconds(delay_ms));
	binds.push_back(BindValue(error));
	db.exec(std::string(
					"UPDATE jobs SET "
					"state = CASE WHEN attempts >= max_attempts THEN 'failed' ELSE 'queued' END, "
					"finished_at = CASE WHEN attempts >= max_attempts THEN now() END, "
					"secret = CASE WHEN attempts >= max_attempts THEN ") +
					kDropSecret +
					" ELSE secret END, "
					"run_at = now() + make_interval(secs => $4), last_error = $5, "
					"lease_until = NULL, worker_id = NULL WHERE " +
					kClaimed + ";",
			binds);
	return db.changes() > 0;
}

bool fail(db::Database &db, const ClaimedJob &job, const std::string &error) {
	std::vector<BindValue> binds = claim_binds(job);
	binds.push_back(BindValue(error));
	db.exec(std::string("UPDATE jobs SET state = 'failed', finished_at = now(), last_error = $4, "
						"lease_until = NULL, secret = ") +
					kDropSecret + " WHERE " + kClaimed + ";",
			binds);
	return db.changes() > 0;
}

size_t extend_leases(db::Database &db, const std::string &worker_id,
		const std::vector<int64_t> &ids, int64_t lease_ms) {
	if (ids.empty())
		return 0;
	db.exec("UPDATE jobs SET lease_until = now() + make_interval(secs => $3) "
			"WHERE state = 'running' AND worker_id = $1 AND id = ANY($2::bigint[]);",
			{ BindValue(worker_id), BindValue(bigint_array(ids)), seconds(lease_ms) });
	return static_cast<size_t>(db.changes());
}

size_t reap_expired(db::Database &db) {
	db.exec(std::string(
					"UPDATE jobs SET "
					"state = CASE WHEN attempts >= max_attempts THEN 'failed' ELSE 'queued' END, "
					"finished_at = CASE WHEN attempts >= max_attempts THEN now() END, "
					"secret = CASE WHEN attempts >= max_attempts THEN ") +
			kDropSecret +
			" ELSE secret END, "
			"run_at = now(), lease_until = NULL, worker_id = NULL, "
			"last_error = 'its worker went away before it finished (its lease ran out)' "
			"WHERE state = 'running' AND lease_until < now();");
	return static_cast<size_t>(db.changes());
}

size_t prune_finished(db::Database &db, int64_t keep_s) {
	db.exec("DELETE FROM jobs WHERE state IN ('done', 'failed') AND finished_at < now() - "
			"make_interval(secs => $1);",
			{ BindValue(static_cast<double>(std::max<int64_t>(keep_s, 0))) });
	return static_cast<size_t>(db.changes());
}

int64_t retry_delay_ms(int attempt, int64_t base_ms, int64_t max_ms, uint32_t draw) {
	base_ms = std::max<int64_t>(base_ms, 1);
	max_ms = std::max(max_ms, base_ms);
	int64_t delay = base_ms;
	for (int i = 1; i < attempt && delay < max_ms; ++i)
		delay *= 2;
	delay = std::min(delay, max_ms);
	const int64_t half = delay / 2;
	return half + static_cast<int64_t>(draw % static_cast<uint64_t>(delay - half + 1));
}

bool job_pending(db::Database &db, const std::string &dedupe_key) {
	return !db.query("SELECT 1 FROM jobs WHERE dedupe_key = $1 AND state IN ('queued', 'running');",
					  { BindValue(dedupe_key) })
					.empty();
}

} // namespace opennova::novaworld_server::jobs
