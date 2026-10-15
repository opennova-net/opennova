#include "hosted_repository.h"

#include "billing/entitlement.h"
#include "fleet_repository.h"

#include <net/novaworld/host_repository.h>

#include <array>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr std::array<const char *, 8> kStateNames = {
	"requested", "provisioning", "booting", "listing", "running", "stopping", "stopped", "failed",
};

constexpr const char *kServerColumns =
		"id, owner_user_id, service_user_id, state, game_name, max_players, game_password, "
		"time_limit, kill_limit, max_score, respawn, start_delay, team_ff, expansion, missions, "
		"provider, machine_id, machine_address, admin_port, restarts, "
		"failure_reason, created_at, updated_at, running_at, ended_at, game, own_machine, "
		"machine_ref, slot, game_port, generation, serve_image";

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
	s.container = r.as_text(16).value_or("");
	s.machine_address = r.as_text(17).value_or("");
	s.admin_port = static_cast<int>(r.as_int(18).value_or(0));
	s.restarts = static_cast<int>(r.as_int(19).value_or(0));
	s.failure_reason = r.as_text(20).value_or("");
	s.created_at = r.as_text(21).value_or("");
	s.updated_at = r.as_text(22).value_or("");
	s.running_at = r.as_text(23).value_or("");
	s.ended_at = r.as_text(24).value_or("");
	st.game = r.as_text(25).value_or("");
	s.own_machine = r.as_int(26).value_or(0) != 0;
	s.machine_ref = r.as_int(27);
	s.slot = static_cast<int>(r.as_int(28).value_or(-1));
	s.game_port = static_cast<int>(r.as_int(29).value_or(0));
	s.generation = r.as_int(30).value_or(0);
	s.serve_image = r.as_text(31).value_or("");
	return s;
}

int count_of(const std::vector<db::Row> &rows) {
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
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
			"VALUES ($1, $2, $3, $4);",
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

RequestResult request_server(db::Database &db, int64_t owner_user_id, const HostedSettings &settings,
		const std::vector<DataSet> &offered, int cap, int64_t now, bool own_machine, const PlacementPolicy *fleet) {
	RequestResult out;
	if (check_settings(settings, offered)) {
		out.outcome = RequestOutcome::InvalidSettings;
		return out;
	}
	db::Transaction tx(db);
	const auto user = db.query("SELECT account_status FROM players WHERE id = $1 AND kind = 'player';",
			{ BindValue(owner_user_id) });
	if (user.empty()) {
		out.outcome = RequestOutcome::NoSuchUser;
		return out;
	}
	if (user.front().as_text(0).value_or("") != "active") {
		out.outcome = RequestOutcome::NotEntitled;
		return out;
	}
	// The entitlement read in the transaction, which holds the writer lock: no
	// other start lands between its count and this row.
	const billing::Entitlement entitlement = billing::entitlement_of(db, owner_user_id, now);
	if (entitlement.max_servers == 0) {
		out.outcome = RequestOutcome::NotEntitled;
		return out;
	}
	if (!entitlement.can_start) {
		out.outcome = RequestOutcome::PastDue;
		return out;
	}
	out.max_servers = entitlement.max_servers;
	out.used = count_live_of(db, owner_user_id);
	if (out.used >= entitlement.max_servers) {
		out.outcome = RequestOutcome::AtLimit;
		return out;
	}
	if (own_machine && count_own_live_of(db, owner_user_id) >= entitlement.own_machines) {
		out.outcome = RequestOutcome::NoOwnMachine;
		return out;
	}
	if (count_live(db) >= cap || (fleet && !fleet_has_room(db, *fleet, own_machine))) {
		out.outcome = RequestOutcome::AtCapacity;
		return out;
	}
	const auto inserted = db.query(
			"INSERT INTO hosted_servers (owner_user_id, state, game, game_name, max_players, "
			"game_password, time_limit, kill_limit, max_score, respawn, start_delay, team_ff, "
			"expansion, missions, own_machine) "
			"VALUES ($1, 'requested', $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14) "
			"RETURNING id;",
			{ BindValue(owner_user_id), BindValue(settings.game), BindValue(settings.game_name),
					BindValue(static_cast<int64_t>(settings.max_players)),
					BindValue(settings.game_password),
					BindValue(static_cast<int64_t>(settings.time_limit)),
					BindValue(static_cast<int64_t>(settings.kill_limit)),
					BindValue(static_cast<int64_t>(settings.max_score)),
					BindValue(static_cast<int64_t>(settings.respawn)),
					BindValue(static_cast<int64_t>(settings.start_delay)),
					BindValue(db::Bool{ settings.team_ff }),
					BindValue(settings.expansion), BindValue(join_missions(settings.missions)),
					BindValue(db::Bool{ own_machine }) });
	out.id = inserted.front().as_int(0).value_or(0);
	insert_event(db, out.id, "", state_name(State::Requested), "requested by its owner");
	tx.commit();
	out.outcome = RequestOutcome::Created;
	return out;
}

std::optional<AdminLogin> admin_login_of(db::Database &db, int64_t id) {
	const auto rows = db.query("SELECT admin_port, admin_user, admin_password FROM hosted_servers "
							   "WHERE id = $1 AND admin_user <> '';",
			{ BindValue(id) });
	if (rows.empty()) return std::nullopt;
	AdminLogin login;
	login.port = static_cast<int>(rows.front().as_int(0).value_or(0));
	login.user = rows.front().as_text(1).value_or("");
	login.password = rows.front().as_text(2).value_or("");
	return login;
}

std::optional<HostedServer> find_server(db::Database &db, int64_t id) {
	auto rows = query_servers(db, "WHERE id = $1 LIMIT 1;", { BindValue(id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::optional<HostedServer> owned_server(db::Database &db, int64_t id, int64_t owner_user_id) {
	auto rows = query_servers(db, "WHERE id = $1 AND owner_user_id = $2 LIMIT 1;",
			{ BindValue(id), BindValue(owner_user_id) });
	if (rows.empty()) return std::nullopt;
	return std::move(rows.front());
}

std::vector<HostedServer> live_servers_of(db::Database &db, int64_t owner_user_id) {
	return query_servers(db, std::string("WHERE owner_user_id = $1 AND ") + kLiveServerSql + " ORDER BY id;",
			{ BindValue(owner_user_id) });
}

std::vector<HostedServer> list_servers_of(db::Database &db, int64_t owner_user_id, size_t ended_limit) {
	std::vector<HostedServer> out = live_servers_of(db, owner_user_id);
	for (HostedServer &s : query_servers(db,
				 std::string("WHERE owner_user_id = $1 AND NOT (") + kLiveServerSql + ") ORDER BY id DESC LIMIT $2;",
				 { BindValue(owner_user_id), BindValue(static_cast<int64_t>(ended_limit)) }))
		out.push_back(std::move(s));
	return out;
}

int count_live_of(db::Database &db, int64_t owner_user_id) {
	return count_of(db.query(
			std::string("SELECT COUNT(*) FROM hosted_servers WHERE owner_user_id = $1 AND ") + kLiveServerSql + ";",
			{ BindValue(owner_user_id) }));
}

int count_own_live_of(db::Database &db, int64_t owner_user_id) {
	return count_of(db.query(std::string("SELECT COUNT(*) FROM hosted_servers WHERE owner_user_id = $1 "
										 "AND own_machine AND ") +
					kLiveServerSql + ";",
			{ BindValue(owner_user_id) }));
}

std::vector<HostedServer> list_live(db::Database &db) {
	return query_servers(db, std::string("WHERE ") + kLiveServerSql + " ORDER BY id;", {});
}

std::vector<HostedServer> list_servers(db::Database &db, size_t limit) {
	return query_servers(db, std::string("ORDER BY (") + kLiveServerSql + ") DESC, id DESC LIMIT $1;",
			{ BindValue(static_cast<int64_t>(limit)) });
}

std::vector<HostedServer> list_fleet_servers(
		db::Database &db, std::optional<int64_t> owner_user_id, ServerFilter filter, size_t limit) {
	std::string where = owner_user_id ? "WHERE owner_user_id = $2" : "WHERE TRUE";
	if (filter == ServerFilter::Live) where += std::string(" AND ") + kLiveServerSql;
	if (filter == ServerFilter::Ended) where += std::string(" AND NOT (") + kLiveServerSql + ")";
	std::vector<BindValue> binds = { BindValue(static_cast<int64_t>(limit)) };
	if (owner_user_id) binds.push_back(BindValue(*owner_user_id));
	return query_servers(db, where + " ORDER BY (" + kLiveServerSql + ") DESC, id DESC LIMIT $1;", binds);
}

int count_live(db::Database &db) {
	const auto rows = db.query(std::string("SELECT COUNT(*) FROM hosted_servers WHERE ") + kLiveServerSql + ";");
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

int count_failed_since(db::Database &db, int64_t seconds) {
	const auto rows = db.query("SELECT COUNT(*) FROM hosted_servers WHERE state = 'failed' "
							   "AND ended_at > now() - make_interval(secs => $1);",
			{ BindValue(seconds) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

std::vector<HostedEvent> list_events(db::Database &db, int64_t id, size_t limit) {
	std::vector<HostedEvent> out;
	const auto rows = db.query("SELECT id, from_state, to_state, detail, at FROM ("
							   "SELECT * FROM hosted_server_events WHERE server_id = $1 "
							   "ORDER BY id DESC LIMIT $2) AS newest ORDER BY id;",
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
	db.exec("UPDATE hosted_servers SET state = $1, updated_at = now(), "
			"restarts = restarts + $2, "
			"machine_id = CASE WHEN $3 THEN '' ELSE machine_id END, "
			"machine_address = CASE WHEN $3 THEN '' ELSE machine_address END, "
			"running_at = CASE WHEN $1 = 'running' THEN now() ELSE running_at END, "
			"ended_at = CASE WHEN $1 IN ('stopped', 'failed') THEN now() ELSE ended_at END, "
			"failure_reason = CASE WHEN $1 = 'failed' THEN $4 ELSE failure_reason END "
			"WHERE id = $5 AND state = $6;",
			{ BindValue(to_name), BindValue(static_cast<int64_t>(options.count_restart ? 1 : 0)),
					BindValue(db::Bool{ options.clear_machine }), BindValue(detail),
					BindValue(id), BindValue(std::string(state_name(from))) });
	if (db.changes() == 0) return false;
	insert_event(db, id, state_name(from), to_name, detail);
	if (options.end_service_account) {
		db.exec("DELETE FROM players WHERE kind = 'service' AND id = "
				"(SELECT service_user_id FROM hosted_servers WHERE id = $1);",
				{ BindValue(id) });
	}
	tx.commit();
	return true;
}

bool reset_restarts(db::Database &db, int64_t id) {
	db.exec("UPDATE hosted_servers SET restarts = 0, updated_at = now() "
			"WHERE id = $1 AND state = 'running' AND restarts > 0;",
			{ BindValue(id) });
	return db.changes() > 0;
}

bool request_stop(db::Database &db, int64_t id, const std::string &detail) {
	db::Transaction tx(db);
	const auto rows = db.query("SELECT state FROM hosted_servers WHERE id = $1;", { BindValue(id) });
	if (rows.empty()) return false;
	const auto state = state_from_name(rows.front().as_text(0).value_or(""));
	if (!state || !is_live(*state) || *state == State::Stopping) return false;
	db.exec("UPDATE hosted_servers SET state = 'stopping', updated_at = now() "
			"WHERE id = $1 AND state = $2;",
			{ BindValue(id), BindValue(std::string(state_name(*state))) });
	if (db.changes() == 0) return false;
	insert_event(db, id, state_name(*state), state_name(State::Stopping), detail);
	tx.commit();
	return true;
}

size_t request_stop_all(db::Database &db, const std::string &detail) {
	size_t stopped = 0;
	db::Transaction tx(db);
	for (const auto &r : db.query(std::string("SELECT id, state FROM hosted_servers WHERE ") + kLiveServerSql +
					 " AND state <> 'stopping';")) {
		const int64_t id = r.as_int(0).value_or(0);
		const std::string from = r.as_text(1).value_or("");
		db.exec("UPDATE hosted_servers SET state = 'stopping', updated_at = now() "
				"WHERE id = $1 AND state = $2;",
				{ BindValue(id), BindValue(from) });
		if (db.changes() == 0) continue;
		insert_event(db, id, from, state_name(State::Stopping), detail);
		++stopped;
	}
	tx.commit();
	return stopped;
}

void set_service_account(db::Database &db, int64_t id, std::optional<int64_t> user_id) {
	db.exec("UPDATE hosted_servers SET service_user_id = $1, updated_at = now() "
			"WHERE id = $2;",
			{ optional_id(user_id), BindValue(id) });
}

void set_machine(db::Database &db, int64_t id, const std::string &provider,
		const std::string &container, const std::string &address) {
	db.exec("UPDATE hosted_servers SET provider = $1, machine_id = $2, machine_address = $3, "
			"updated_at = now() WHERE id = $4;",
			{ BindValue(provider), BindValue(container), BindValue(address), BindValue(id) });
}

void set_serve_image(db::Database &db, int64_t id, const std::string &image) {
	db.exec("UPDATE hosted_servers SET serve_image = $1, updated_at = now() WHERE id = $2;",
			{ BindValue(image), BindValue(id) });
}

void set_admin_login(db::Database &db, int64_t id, int admin_port, const std::string &user,
		const std::string &password) {
	db.exec("UPDATE hosted_servers SET admin_port = $1, admin_user = $2, admin_password = $3, "
			"updated_at = now() WHERE id = $4;",
			{ BindValue(static_cast<int64_t>(admin_port)), BindValue(user), BindValue(password),
					BindValue(id) });
}

void append_console(db::Database &db, int64_t server_id, const std::vector<ConsoleLine> &lines, size_t keep) {
	if (lines.empty()) return;
	db::Transaction tx(db);
	for (const ConsoleLine &line : lines) {
		db.exec("INSERT INTO hosted_server_console (server_id, kind, actor, text, at) "
				"VALUES ($1, $2, $3, $4, $5);",
				{ BindValue(server_id), BindValue(line.kind), BindValue(line.actor), BindValue(line.text),
						BindValue(line.at) });
	}
	// The ring: everything older than the newest `keep` goes.
	db.exec("DELETE FROM hosted_server_console WHERE server_id = $1 AND id <= ("
			"SELECT id FROM hosted_server_console WHERE server_id = $1 ORDER BY id DESC LIMIT 1 OFFSET $2);",
			{ BindValue(server_id), BindValue(static_cast<int64_t>(keep)) });
	tx.commit();
}

std::vector<ConsoleLine> list_console(db::Database &db, int64_t server_id, int64_t after_id, size_t limit) {
	std::vector<ConsoleLine> out;
	const auto rows = db.query("SELECT id, kind, actor, text, at FROM ("
							   "SELECT * FROM hosted_server_console WHERE server_id = $1 AND id > $2 "
							   "ORDER BY id DESC LIMIT $3) AS newest ORDER BY id;",
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
							   "ON k.claimed_rid = h.rid WHERE k.user_id = $1 "
							   "ORDER BY h.created_at DESC, h.rid DESC LIMIT 1;",
			{ BindValue(account_user_id) });
	if (rows.empty()) return std::nullopt;
	return static_cast<uint32_t>(rows.front().as_int(0).value_or(0));
}

std::optional<HostedAccount> hosted_server_of_account(db::Database &db, int64_t account_user_id) {
	const auto rows = db.query("SELECT id, owner_user_id, state FROM hosted_servers "
							   "WHERE service_user_id = $1 ORDER BY id DESC LIMIT 1;",
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
											  "WHERE owner_user_id = $1 AND service_user_id IS NOT NULL AND ") +
						 kLiveServerSql + " ORDER BY id;",
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
