#include "limits_hold.h"

#include <base/io/strutil.h>

#include <utility>

namespace opennova::novaworld_server::hosted::limits_hold {

namespace {

using db::BindValue;

// A bigint[]'s text ("{1,2,3}") as its values.
std::set<int64_t> parse_ids(const std::string &text) {
	std::set<int64_t> out;
	std::string item;
	for (const char c : text) {
		if (c >= '0' && c <= '9') {
			item += c;
			continue;
		}
		if (!item.empty()) {
			if (const auto value = strutil::parse_llong(item))
				out.insert(static_cast<int64_t>(*value));
			item.clear();
		}
	}
	if (!item.empty()) {
		if (const auto value = strutil::parse_llong(item))
			out.insert(static_cast<int64_t>(*value));
	}
	return out;
}

std::string ids_text(const std::set<int64_t> &ids) {
	std::string out = "{";
	for (const int64_t id : ids)
		out += (out.size() > 1 ? "," : "") + std::to_string(id);
	return out + "}";
}

bool among(const std::set<int64_t> &owners, const std::set<int64_t> &held) {
	for (const int64_t o : owners) {
		if (held.count(o) == 0)
			return false;
	}
	return true;
}

// The row's state, read under its lock (inside the caller's transaction).
struct Row {
	bool held = false;
	std::set<int64_t> owners;
	uint64_t taken = 0;
};

Row lock_row(db::Database &db) {
	Row r;
	const auto rows = db.query("SELECT held, owners::text, taken_reconciles FROM "
							   "hosted_limits_hold WHERE id = 1 FOR UPDATE;");
	if (rows.empty())
		return r;
	r.held = rows.front().as_int(0).value_or(0) != 0;
	r.owners = parse_ids(rows.front().as_text(1).value_or("{}"));
	r.taken = static_cast<uint64_t>(rows.front().as_int(2).value_or(0));
	return r;
}

constexpr const char *kCleared =
		"held = false, reason = '', owners = '{}', last_owners = 0, servers = 0";

} // namespace

LimitsHoldView view(db::Database &db) {
	LimitsHoldView v;
	const auto rows = db.query(
			"SELECT held, reason, last_owners, servers FROM hosted_limits_hold WHERE id = 1;");
	if (rows.empty())
		return v;
	v.held = rows.front().as_int(0).value_or(0) != 0;
	v.reason = rows.front().as_text(1).value_or("");
	if (v.held) {
		v.owners = static_cast<size_t>(rows.front().as_int(2).value_or(0));
		v.servers = static_cast<size_t>(rows.front().as_int(3).value_or(0));
	}
	return v;
}

bool held(db::Database &db) {
	return view(db).held;
}

void reconciled(db::Database &db) {
	db.exec("UPDATE hosted_limits_hold SET reconciles = reconciles + 1 WHERE id = 1;");
}

uint64_t reconciles(db::Database &db) {
	const auto rows = db.query("SELECT reconciles FROM hosted_limits_hold WHERE id = 1;");
	return rows.empty() ? 0 : static_cast<uint64_t>(rows.front().as_int(0).value_or(0));
}

LimitsHeld hold(db::Database &db, const std::string &reason, const std::set<int64_t> &owners,
		size_t servers, uint64_t reconciles) {
	db::Transaction tx(db);
	const Row r = lock_row(db);
	LimitsHeld what = LimitsHeld::Still;
	if (!r.held)
		what = LimitsHeld::Now;
	else if (!among(owners, r.owners))
		what = LimitsHeld::Widened;
	if (what != LimitsHeld::Still) {
		db.exec("UPDATE hosted_limits_hold SET owners = $1::bigint[], taken_reconciles = $2 WHERE "
				"id = 1;",
				{ BindValue(ids_text(owners)), BindValue(static_cast<int64_t>(reconciles)) });
	}
	db.exec("UPDATE hosted_limits_hold SET held = true, reason = $1, last_owners = $2, servers = "
			"$3 WHERE id = 1;",
			{ BindValue(reason), BindValue(static_cast<int64_t>(owners.size())),
					BindValue(static_cast<int64_t>(servers)) });
	tx.commit();
	return what;
}

bool release_if_confirmed(db::Database &db, const std::set<int64_t> &owners, uint64_t reconciles) {
	db::Transaction tx(db);
	const Row r = lock_row(db);
	if (!r.held || reconciles <= r.taken || !among(owners, r.owners))
		return false;
	db.exec(std::string("UPDATE hosted_limits_hold SET ") + kCleared + " WHERE id = 1;");
	tx.commit();
	return true;
}

bool clear(db::Database &db) {
	const auto rows = db.query(std::string("UPDATE hosted_limits_hold SET ") + kCleared +
			" WHERE id = 1 AND held RETURNING 1;");
	return !rows.empty();
}

void force_next_pass(db::Database &db) {
	db.exec("UPDATE hosted_limits_hold SET force_next = true WHERE id = 1;");
}

bool take_force(db::Database &db) {
	return !db.query("UPDATE hosted_limits_hold SET force_next = false WHERE id = 1 AND force_next "
					 "RETURNING 1;")
					.empty();
}

} // namespace opennova::novaworld_server::hosted::limits_hold
