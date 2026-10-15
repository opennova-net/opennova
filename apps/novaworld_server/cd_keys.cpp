#include "cd_keys.h"

#include <base/os_random/os_random.h>
#include <net/novacrypto/cdkey.h>

#include <cctype>
#include <cstdio>
#include <utility>

namespace opennova::novaworld_server::cdkeys {

namespace {

using db::BindValue;

// A fresh serial that collides with a stored one is drawn again, this many
// times at most (64 random bits: never, in practice).
constexpr int kSerialDraws = 8;

constexpr const char *kKeyColumns =
		"k.id, k.title, k.serial, k.wire_key, k.user_id, p.username, k.created_by, k.created_at, "
		"k.assigned_at, k.revoked_at, "
		"(SELECT COUNT(*) FROM cd_key_activations a WHERE a.key_id = k.id) "
		"FROM cd_keys k LEFT JOIN players p ON p.id = k.user_id ";

constexpr const char *kActivationColumns =
		"a.id, a.key_id, k.title, k.serial, a.nwpssk, a.nwact, a.created_at "
		"FROM cd_key_activations a JOIN cd_keys k ON k.id = a.key_id ";

CdKey row_to_key(const db::Row &r) {
	CdKey k;
	k.id = r.as_int(0).value_or(0);
	k.title = r.as_text(1).value_or("");
	k.serial = r.as_text(2).value_or("");
	k.wire_key = r.as_text(3).value_or("");
	if (const auto user_id = r.as_int(4))
		k.account = KeyAccount{ *user_id, r.as_text(5).value_or("") };
	k.created_by = r.as_text(6).value_or("");
	k.created_at = r.as_text(7).value_or("");
	k.assigned_at = r.as_text(8).value_or("");
	k.revoked_at = r.as_text(9).value_or("");
	k.activations = r.as_int(10).value_or(0);
	return k;
}

Activation row_to_activation(const db::Row &r) {
	Activation a;
	a.id = r.as_int(0).value_or(0);
	a.key_id = r.as_int(1).value_or(0);
	a.title = r.as_text(2).value_or("");
	a.serial = r.as_text(3).value_or("");
	a.nwpssk = r.as_text(4).value_or("");
	a.nwact = r.as_text(5).value_or("");
	a.created_at = r.as_text(6).value_or("");
	return a;
}

std::vector<CdKey> query_keys(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<CdKey> out;
	for (const auto &r : db.query(std::string("SELECT ") + kKeyColumns + tail, binds))
		out.push_back(row_to_key(r));
	return out;
}

std::vector<Activation> query_activations(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<Activation> out;
	for (const auto &r : db.query(std::string("SELECT ") + kActivationColumns + tail, binds))
		out.push_back(row_to_activation(r));
	return out;
}

// A new key row, unassigned or (`user_id`) assigned now: its id.
int64_t insert_key(db::Database &db, std::string_view title, std::optional<int64_t> user_id,
		const std::string &created_by) {
	for (int draw = 0; draw < kSerialDraws; ++draw) {
		const std::string serial = make_serial(title);
		const std::string wire_key = cdkey::make_wire_key(serial);
		if (!db.query("SELECT 1 FROM cd_keys WHERE serial = $1 OR wire_key = $2;",
					   { BindValue(serial), BindValue(wire_key) })
						.empty())
			continue;
		const auto inserted = db.query(
				"INSERT INTO cd_keys (title, serial, wire_key, user_id, created_by, assigned_at) "
				"VALUES ($1, $2, $3, $4, $5, CASE WHEN $4::bigint IS NULL THEN NULL ELSE now() END) "
				"RETURNING id;",
				{ BindValue(std::string(title)), BindValue(serial), BindValue(wire_key),
						user_id ? BindValue(*user_id) : BindValue(std::monostate{}),
						BindValue(created_by) });
		return inserted.front().as_int(0).value_or(0);
	}
	throw db::Error("cd_keys: no unused serial after " + std::to_string(kSerialDraws) + " draws");
}

// The machine `nwpssk` names activated under `key` (live), inside the caller's
// transaction: the stored NWACT when that machine has one, else a new row.
ActivateResult record_activation(db::Database &db, const CdKey &key, const std::string &nwpssk) {
	ActivateResult out;
	const auto stored =
			db.query("SELECT nwact FROM cd_key_activations WHERE key_id = $1 AND nwpssk = $2;",
					{ BindValue(key.id), BindValue(nwpssk) });
	if (!stored.empty()) {
		out.outcome = ActivateOutcome::Reactivated;
		out.nwact = stored.front().as_text(0).value_or("");
	} else {
		out.outcome = ActivateOutcome::Activated;
		out.nwact = cdkey::make_activation(key.wire_key, nwpssk);
		db.exec("INSERT INTO cd_key_activations (key_id, nwpssk, nwact) VALUES ($1, $2, $3);",
				{ BindValue(key.id), BindValue(nwpssk), BindValue(out.nwact) });
	}
	out.key = find_key(db, key.id);
	return out;
}

} // namespace

bool title_activates(std::string_view title) {
	return title == "dfx2";
}

std::string make_serial(std::string_view title) {
	std::string serial;
	for (const char c : title)
		serial.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	const uint64_t bits = os_random_u64();
	char hex[17];
	std::snprintf(hex, sizeof(hex), "%016llX", static_cast<unsigned long long>(bits));
	for (int group = 0; group < 4; ++group) {
		serial.push_back('-');
		serial.append(hex + group * 4, 4);
	}
	return serial;
}

const char *CdKey::state() const {
	if (revoked())
		return "revoked";
	return account ? "assigned" : "unassigned";
}

std::vector<CdKey> generate_keys(
		db::Database &db, std::string_view title, size_t count, const std::string &created_by) {
	std::vector<int64_t> ids;
	{
		db::Transaction tx(db);
		for (size_t i = 0; i < count; ++i)
			ids.push_back(insert_key(db, title, std::nullopt, created_by));
		tx.commit();
	}
	std::vector<CdKey> out;
	for (const int64_t id : ids) {
		if (auto key = find_key(db, id))
			out.push_back(std::move(*key));
	}
	return out;
}

std::vector<CdKey> list_keys(db::Database &db, const KeyFilter &filter, int64_t *total) {
	std::string where = "WHERE 1 = 1";
	std::vector<BindValue> binds;
	// The next bind's placeholder, $<n>.
	const auto next = [&binds] { return "$" + std::to_string(binds.size() + 1); };
	if (!filter.title.empty()) {
		where += " AND k.title = " + next();
		binds.emplace_back(filter.title);
	}
	if (filter.state == "unassigned")
		where += " AND k.user_id IS NULL AND k.revoked_at IS NULL";
	else if (filter.state == "assigned")
		where += " AND k.user_id IS NOT NULL AND k.revoked_at IS NULL";
	else if (filter.state == "revoked")
		where += " AND k.revoked_at IS NOT NULL";
	if (!filter.search.empty()) {
		const std::string search = "lower(" + next() + "::text)";
		where += " AND (strpos(lower(k.serial), " + search + ") > 0 OR strpos(lower(k.wire_key), " + search +
				 ") > 0 OR strpos(lower(COALESCE(p.username, '')), " + search + ") > 0)";
		binds.emplace_back(filter.search);
	}
	if (total != nullptr) {
		const auto counted =
				db.query("SELECT COUNT(*) FROM cd_keys k LEFT JOIN players p ON p.id = k.user_id " +
								where + ";",
						binds);
		*total = counted.empty() ? 0 : counted.front().as_int(0).value_or(0);
	}
	const std::string limit = next();
	binds.emplace_back(static_cast<int64_t>(filter.limit));
	const std::string offset = next();
	binds.emplace_back(static_cast<int64_t>(filter.offset));
	return query_keys(db, where + " ORDER BY k.id DESC LIMIT " + limit + " OFFSET " + offset + ";", binds);
}

std::optional<CdKey> find_key(db::Database &db, int64_t id) {
	auto keys = query_keys(db, "WHERE k.id = $1;", { BindValue(id) });
	if (keys.empty())
		return std::nullopt;
	return std::move(keys.front());
}

std::vector<CdKey> keys_of_account(db::Database &db, int64_t user_id) {
	return query_keys(db, "WHERE k.user_id = $1 ORDER BY k.id DESC;", { BindValue(user_id) });
}

AssignOutcome assign_key(db::Database &db, int64_t key_id, int64_t user_id) {
	db::Transaction tx(db);
	const auto key = find_key(db, key_id);
	if (!key)
		return AssignOutcome::NoSuchKey;
	if (key->revoked())
		return AssignOutcome::Revoked;
	if (key->account)
		return AssignOutcome::AlreadyAssigned;
	if (db.query("SELECT 1 FROM players WHERE id = $1 AND kind = 'player';", { BindValue(user_id) })
					.empty())
		return AssignOutcome::NoSuchAccount;
	if (!db.query("SELECT 1 FROM cd_keys WHERE user_id = $1 AND title = $2 AND revoked_at IS NULL;",
				   { BindValue(user_id), BindValue(key->title) })
					.empty())
		return AssignOutcome::AccountHasKey;
	db.exec("UPDATE cd_keys SET user_id = $1, assigned_at = now() "
			"WHERE id = $2 AND user_id IS NULL AND revoked_at IS NULL;",
			{ BindValue(user_id), BindValue(key_id) });
	tx.commit();
	return AssignOutcome::Assigned;
}

RevokeOutcome revoke_key(db::Database &db, int64_t key_id) {
	db::Transaction tx(db);
	const auto key = find_key(db, key_id);
	if (!key)
		return RevokeOutcome::NoSuchKey;
	if (key->revoked())
		return RevokeOutcome::AlreadyRevoked;
	db.exec("UPDATE cd_keys SET revoked_at = now() WHERE id = $1 AND revoked_at IS NULL;",
			{ BindValue(key_id) });
	tx.commit();
	return RevokeOutcome::Revoked;
}

std::vector<Activation> activations_of_key(db::Database &db, int64_t key_id) {
	return query_activations(db, "WHERE a.key_id = $1 ORDER BY a.id;", { BindValue(key_id) });
}

std::vector<Activation> activations_of_account(db::Database &db, int64_t user_id) {
	return query_activations(db, "WHERE k.user_id = $1 ORDER BY a.id DESC;", { BindValue(user_id) });
}

ActivateResult activate_key(db::Database &db, int64_t key_id, const std::string &nwpssk) {
	ActivateResult out;
	if (!cdkey::is_fingerprint(nwpssk)) {
		out.outcome = ActivateOutcome::BadFingerprint;
		return out;
	}
	db::Transaction tx(db);
	const auto key = find_key(db, key_id);
	if (!key) {
		out.outcome = ActivateOutcome::NoSuchKey;
		return out;
	}
	if (key->revoked()) {
		out.outcome = ActivateOutcome::Revoked;
		out.key = key;
		return out;
	}
	out = record_activation(db, *key, nwpssk);
	tx.commit();
	return out;
}

ActivateResult activate_account(db::Database &db, int64_t user_id, std::string_view title,
		const std::string &nwpssk, bool auto_mint) {
	ActivateResult out;
	if (!title_activates(title)) {
		out.outcome = ActivateOutcome::TitleHasNoKeys;
		return out;
	}
	if (!cdkey::is_fingerprint(nwpssk)) {
		out.outcome = ActivateOutcome::BadFingerprint;
		return out;
	}
	db::Transaction tx(db);
	const auto keys = query_keys(db, "WHERE k.user_id = $1 AND k.title = $2 ORDER BY k.id DESC;",
			{ BindValue(user_id), BindValue(std::string(title)) });
	for (const CdKey &key : keys) {
		if (key.revoked())
			continue;
		out = record_activation(db, key, nwpssk);
		tx.commit();
		return out;
	}
	if (!keys.empty()) {
		out.outcome = ActivateOutcome::Revoked;
		out.key = keys.front();
		return out;
	}
	if (!auto_mint) {
		out.outcome = ActivateOutcome::NoKey;
		return out;
	}
	const auto minted = find_key(db, insert_key(db, title, user_id, "auto"));
	if (!minted) {
		out.outcome = ActivateOutcome::NoKey;
		return out;
	}
	out = record_activation(db, *minted, nwpssk);
	out.minted = true;
	tx.commit();
	return out;
}

bool needs_activation(std::string_view title, std::string_view nwcdkiid) {
	if (!title_activates(title))
		return false;
	std::string serial;
	return !cdkey::decode_wire_key(nwcdkiid, serial);
}

} // namespace opennova::novaworld_server::cdkeys
