#include "hosted_repository.h"

#include <net/novaworld/host_repository.h>

#include <array>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr std::array<const char *, 8> kStateNames = {
	"requested", "provisioning", "booting", "listing", "running", "stopping", "stopped", "failed",
};

// The SQL test for a live row.
constexpr const char *kLiveSql = "state NOT IN ('stopped', 'failed')";

constexpr const char *kServerColumns =
		"id, owner_user_id, service_user_id, state, game_name, max_players, game_password, "
		"time_limit, kill_limit, max_score, respawn, start_delay, team_ff, expansion, missions, "
		"provider, machine_id, machine_address, admin_port, restarts, "
		"failure_reason, created_at, updated_at, running_at, ended_at";

BindValue optional_id(std::optional<int64_t> id) {
	if (!id) return BindValue(std::monostate{});
	return BindValue(*id);
}

HostedServer row_to_server(const db::Row &r) {
	HostedServer s;
	s.id = r.as_int(0).value_or(0);
	s.owner_user_id = r.as_int(1);
	s.service_user_id = r.as_int(2);
	s.state = state_from_name(r.as_text(3).value_or("")).value_or(State::Failed);
	HostedSettings &st = s.settings;
	st.game_name = r.as_text(4).value_or("");
	st.max_players = static_cast<int>(r.as_int(5).value_or(0));
	st.game_password = r.as_text(6).value_or("");
	st.time_limit = static_cast<int>(r.as_int(7).value_or(0));
	st.kill_limit = static_cast<int>(r.as_int(8).value_or(0));
	st.max_score = static_cast<int>(r.as_int(9).value_or(0));
	st.respawn = static_cast<int>(r.as_int(10).value_or(0));
	st.start_delay = static_cast<int>(r.as_int(11).value_or(0));
	st.team_ff = r.as_int(12).value_or(0) != 0;
	st.expansion = r.as_text(13).value_or("");
	st.missions = split_missions(r.as_text(14).value_or(""));
	s.provider = r.as_text(15).value_or("");
	s.machine_id = r.as_text(16).value_or("");
	s.machine_address = r.as_text(17).value_or("");
	s.admin_port = static_cast<int>(r.as_int(18).value_or(0));
	s.restarts = static_cast<int>(r.as_int(19).value_or(0));
	s.failure_reason = r.as_text(20).value_or("");
	s.created_at = r.as_text(21).value_or("");
	s.updated_at = r.as_text(22).value_or("");
	s.running_at = r.as_text(23).value_or("");
	s.ended_at = r.as_text(24).value_or("");
	return s;
}

std::vector<HostedServer> query_servers(
		db::Database &db, const std::string &tail, const std::vector<BindValue> &binds) {
	std::vector<HostedServer> out;
	for (const auto &r : db.query(std::string("SELECT ") + kServerColumns + " FROM hosted_servers " + tail, binds))
		out.push_back(row_to_server(r));
	return out;
}

void insert_event(db::Database &db, int64_t id, const std::string &from, const std::string &to,
		const std::string &detail) {
	db.exec("INSERT INTO hosted_server_events (server_id, from_state, to_state, detail) "
			"VALUES (?, ?, ?, ?);",
			{ BindValue(id), BindValue(from), BindValue(to), BindValue(detail) });
}

} // namespace

const char *state_name(State state) {
	const auto index = static_cast<size_t>(state);
	return index < kStateNames.size() ? kStateNames[index] : "failed";
}

std::optional<State> state_from_name(std::string_view name) {
	for (size_t i = 0; i < kStateNames.size(); ++i) {
		if (name == kStateNames[i]) return static_cast<State>(i);
	}
	return std::nullopt;
}

bool is_live(State state) { return state != State::Stopped && state != State::Failed; }

RequestResult request_server(db::Database &db, int64_t owner_user_id,
		const HostedSettings &settings, int cap) {
	RequestResult out;
	if (check_settings(settings)) {
		out.outcome = RequestOutcome::InvalidSettings;
		return out;
	}
	db::Transaction tx(db);
	const auto user = db.query("SELECT can_host, account_status FROM players "
							   "WHERE id = ? AND kind = 'player';",
			{ BindValue(owner_user_id) });
	if (user.empty()) {
		out.outcome = RequestOutcome::NoSuchUser;
		return out;
	}
	if (user.front().as_int(0).value_or(0) == 0 || user.front().as_text(1).value_or("") != "active") {
		out.outcome = RequestOutcome::NotApproved;
		return out;
	}
	const auto live = db.query(std::string("SELECT id FROM hosted_servers WHERE owner_user_id = ? AND ") +
					kLiveSql + " LIMIT 1;",
			{ BindValue(owner_user_id) });
	if (!live.empty()) {
		out.outcome = RequestOutcome::AlreadyLive;
		out.id = live.front().as_int(0).value_or(0);
		return out;
	}
	if (count_live(db) >= cap) {
		out.outcome = RequestOutcome::AtCapacity;
		return out;
	}
	db.exec("INSERT INTO hosted_servers (owner_user_id, state, game_name, max_players, "
			"game_password, time_limit, kill_limit, max_score, respawn, start_delay, team_ff, "
			"expansion, missions) VALUES (?, 'requested', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
			{ BindValue(owner_user_id), BindValue(settings.game_name),
					BindValue(static_cast<int64_t>(settings.max_players)),
					BindValue(settings.game_password),
					BindValue(static_cast<int64_t>(settings.time_limit)),
					BindValue(static_cast<int64_t>(settings.kill_limit)),
					BindValue(static_cast<int64_t>(settings.max_score)),
					BindValue(static_cast<int64_t>(settings.respawn)),
					BindValue(static_cast<int64_t>(settings.start_delay)),
					BindValue(static_cast<int64_t>(settings.team_ff ? 1 : 0)),
					BindValue(settings.expansion), BindValue(join_missions(settings.missions)) });
	out.id = db.last_insert_rowid();
	insert_event(db, out.id, "", state_name(State::Requested), "requested by its owner");
	tx.commit();
	out.outcome = RequestOutcome::Created;
	return out;
}

std::optional<AdminLogin> admin_login_of(db::Database &db, int64_t id) {
	const auto rows = db.query("SELECT admin_port, admin_user, admin_password FROM hosted_servers "
							   "WHERE id = ? AND admin_user <> '';",
			{ BindValue(id) });
	if (rows.empty()) return std::nullopt;
	AdminLogin login;
	login.port = static_cast<int>(rows.front().as_int(0).value_or(0));
	login.user = rows.front().as_text(1).value_or("");
	login.password = rows.front().as_text(2).value_or("");
	return login;
}

std::optional<HostedServer> find_server(db::Database &db, int64_t id) {
	auto rows = query_servers(db, "WHERE id = ? LIMIT 1;", { BindValue(id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::optional<HostedServer> latest_server_of(db::Database &db, int64_t owner_user_id) {
	auto rows = query_servers(db,
			std::string("WHERE owner_user_id = ? ORDER BY (") + kLiveSql + ") DESC, id DESC LIMIT 1;",
			{ BindValue(owner_user_id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::vector<HostedServer> list_live(db::Database &db) {
	return query_servers(db, std::string("WHERE ") + kLiveSql + " ORDER BY id;", {});
}

std::vector<HostedServer> list_servers(db::Database &db, size_t limit) {
	return query_servers(db, std::string("ORDER BY (") + kLiveSql + ") DESC, id DESC LIMIT ?;",
			{ BindValue(static_cast<int64_t>(limit)) });
}

int count_live(db::Database &db) {
	const auto rows = db.query(std::string("SELECT COUNT(*) FROM hosted_servers WHERE ") + kLiveSql + ";");
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

std::vector<HostedEvent> list_events(db::Database &db, int64_t id, size_t limit) {
	std::vector<HostedEvent> out;
	const auto rows = db.query("SELECT id, from_state, to_state, detail, at FROM ("
							   "SELECT * FROM hosted_server_events WHERE server_id = ? "
							   "ORDER BY id DESC LIMIT ?) ORDER BY id;",
			{ BindValue(id), BindValue(static_cast<int64_t>(limit)) });
	out.reserve(rows.size());
	for (const auto &r : rows) {
		HostedEvent e;
		e.id = r.as_int(0).value_or(0);
		e.from_state = r.as_text(1).value_or("");
		e.to_state = r.as_text(2).value_or("");
		e.detail = r.as_text(3).value_or("");
		e.at = r.as_text(4).value_or("");
		out.push_back(std::move(e));
	}
	return out;
}

bool transition(db::Database &db, int64_t id, State from, State to, const std::string &detail,
		TransitionOptions options) {
	const std::string to_name = state_name(to);
	db::Transaction tx(db);
	db.exec("UPDATE hosted_servers SET state = ?, updated_at = CURRENT_TIMESTAMP, "
			"restarts = restarts + ?, "
			"machine_id = CASE WHEN ? THEN '' ELSE machine_id END, "
			"machine_address = CASE WHEN ? THEN '' ELSE machine_address END, "
			"running_at = CASE WHEN ? = 'running' THEN CURRENT_TIMESTAMP ELSE running_at END, "
			"ended_at = CASE WHEN ? IN ('stopped', 'failed') THEN CURRENT_TIMESTAMP ELSE ended_at END, "
			"failure_reason = CASE WHEN ? = 'failed' THEN ? ELSE failure_reason END "
			"WHERE id = ? AND state = ?;",
			{ BindValue(to_name), BindValue(static_cast<int64_t>(options.count_restart ? 1 : 0)),
					BindValue(static_cast<int64_t>(options.clear_machine ? 1 : 0)),
					BindValue(static_cast<int64_t>(options.clear_machine ? 1 : 0)),
					BindValue(to_name), BindValue(to_name), BindValue(to_name), BindValue(detail),
					BindValue(id), BindValue(std::string(state_name(from))) });
	if (db.changes() == 0) return false;
	insert_event(db, id, state_name(from), to_name, detail);
	if (options.end_service_account) {
		db.exec("DELETE FROM players WHERE kind = 'service' AND id = "
				"(SELECT service_user_id FROM hosted_servers WHERE id = ?);",
				{ BindValue(id) });
	}
	tx.commit();
	return true;
}

bool reset_restarts(db::Database &db, int64_t id) {
	db.exec("UPDATE hosted_servers SET restarts = 0, updated_at = CURRENT_TIMESTAMP "
			"WHERE id = ? AND state = 'running' AND restarts > 0;",
			{ BindValue(id) });
	return db.changes() > 0;
}

bool request_stop(db::Database &db, int64_t id, const std::string &detail) {
	db::Transaction tx(db);
	const auto rows = db.query("SELECT state FROM hosted_servers WHERE id = ?;", { BindValue(id) });
	if (rows.empty()) return false;
	const auto state = state_from_name(rows.front().as_text(0).value_or(""));
	if (!state || !is_live(*state) || *state == State::Stopping) return false;
	db.exec("UPDATE hosted_servers SET state = 'stopping', updated_at = CURRENT_TIMESTAMP "
			"WHERE id = ? AND state = ?;",
			{ BindValue(id), BindValue(std::string(state_name(*state))) });
	if (db.changes() == 0) return false;
	insert_event(db, id, state_name(*state), state_name(State::Stopping), detail);
	tx.commit();
	return true;
}

size_t request_stop_all(db::Database &db, const std::string &detail) {
	size_t stopped = 0;
	db::Transaction tx(db);
	for (const auto &r : db.query(std::string("SELECT id, state FROM hosted_servers WHERE ") + kLiveSql +
					 " AND state <> 'stopping';")) {
		const int64_t id = r.as_int(0).value_or(0);
		const std::string from = r.as_text(1).value_or("");
		db.exec("UPDATE hosted_servers SET state = 'stopping', updated_at = CURRENT_TIMESTAMP "
				"WHERE id = ? AND state = ?;",
				{ BindValue(id), BindValue(from) });
		if (db.changes() == 0) continue;
		insert_event(db, id, from, state_name(State::Stopping), detail);
		++stopped;
	}
	tx.commit();
	return stopped;
}

void set_service_account(db::Database &db, int64_t id, std::optional<int64_t> user_id) {
	db.exec("UPDATE hosted_servers SET service_user_id = ?, updated_at = CURRENT_TIMESTAMP "
			"WHERE id = ?;",
			{ optional_id(user_id), BindValue(id) });
}

void set_machine(db::Database &db, int64_t id, const std::string &provider,
		const std::string &machine_id, const std::string &address) {
	db.exec("UPDATE hosted_servers SET provider = ?, machine_id = ?, machine_address = ?, "
			"updated_at = CURRENT_TIMESTAMP WHERE id = ?;",
			{ BindValue(provider), BindValue(machine_id), BindValue(address), BindValue(id) });
}

void set_admin_login(db::Database &db, int64_t id, int admin_port, const std::string &user,
		const std::string &password) {
	db.exec("UPDATE hosted_servers SET admin_port = ?, admin_user = ?, admin_password = ?, "
			"updated_at = CURRENT_TIMESTAMP WHERE id = ?;",
			{ BindValue(static_cast<int64_t>(admin_port)), BindValue(user), BindValue(password),
					BindValue(id) });
}

void append_console(db::Database &db, int64_t server_id, const std::vector<ConsoleLine> &lines, size_t keep) {
	if (lines.empty()) return;
	db::Transaction tx(db);
	for (const ConsoleLine &line : lines) {
		db.exec("INSERT INTO hosted_server_console (server_id, kind, actor, text, at) VALUES (?, ?, ?, ?, ?);",
				{ BindValue(server_id), BindValue(line.kind), BindValue(line.actor), BindValue(line.text),
						BindValue(line.at) });
	}
	// The ring: everything older than the newest `keep` goes.
	db.exec("DELETE FROM hosted_server_console WHERE server_id = ? AND id <= ("
			"SELECT id FROM hosted_server_console WHERE server_id = ? ORDER BY id DESC LIMIT 1 OFFSET ?);",
			{ BindValue(server_id), BindValue(server_id), BindValue(static_cast<int64_t>(keep)) });
	tx.commit();
}

std::vector<ConsoleLine> list_console(db::Database &db, int64_t server_id, int64_t after_id, size_t limit) {
	std::vector<ConsoleLine> out;
	const auto rows = db.query("SELECT id, kind, actor, text, at FROM ("
							   "SELECT * FROM hosted_server_console WHERE server_id = ? AND id > ? "
							   "ORDER BY id DESC LIMIT ?) ORDER BY id;",
			{ BindValue(server_id), BindValue(after_id), BindValue(static_cast<int64_t>(limit)) });
	out.reserve(rows.size());
	for (const auto &r : rows) {
		ConsoleLine line;
		line.id = r.as_int(0).value_or(0);
		line.kind = r.as_text(1).value_or("");
		line.actor = r.as_text(2).value_or("");
		line.text = r.as_text(3).value_or("");
		line.at = r.as_text(4).value_or("");
		out.push_back(std::move(line));
	}
	return out;
}

std::optional<uint32_t> listing_rid_of(db::Database &db, int64_t account_user_id) {
	const auto rows = db.query("SELECT h.rid FROM active_hosts h JOIN host_keys k "
							   "ON k.claimed_rid = h.rid WHERE k.user_id = ? "
							   "ORDER BY h.created_at DESC, h.rid DESC LIMIT 1;",
			{ BindValue(account_user_id) });
	if (rows.empty()) return std::nullopt;
	return static_cast<uint32_t>(rows.front().as_int(0).value_or(0));
}

std::optional<HostedAccount> hosted_server_of_account(db::Database &db, int64_t account_user_id) {
	const auto rows = db.query("SELECT id, owner_user_id, state FROM hosted_servers "
							   "WHERE service_user_id = ? ORDER BY id DESC LIMIT 1;",
			{ BindValue(account_user_id) });
	if (rows.empty()) return std::nullopt;
	HostedAccount out;
	out.server_id = rows.front().as_int(0).value_or(0);
	out.owner_user_id = rows.front().as_int(1);
	out.state = state_from_name(rows.front().as_text(2).value_or("")).value_or(State::Failed);
	return out;
}

std::vector<int64_t> listing_accounts_of(db::Database &db, int64_t user_id) {
	std::vector<int64_t> out{ user_id };
	for (const auto &r : db.query(std::string("SELECT service_user_id FROM hosted_servers "
											  "WHERE owner_user_id = ? AND service_user_id IS NOT NULL AND ") +
						 kLiveSql + " ORDER BY id;",
				 { BindValue(user_id) })) {
		if (const auto account = r.as_int(0)) out.push_back(*account);
	}
	return out;
}

std::optional<int64_t> owned_listing_account(db::Database &db, uint32_t rid, int64_t user_id) {
	for (const int64_t account : listing_accounts_of(db, user_id)) {
		if (hostdb::listing_owned_by(db, rid, account)) return account;
	}
	return std::nullopt;
}

} // namespace opennova::novaworld_server::hosted
