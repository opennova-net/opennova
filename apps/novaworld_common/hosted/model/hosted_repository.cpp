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
		"time_limit, kill_limit, max_score, respawn, start_delay, team_ff, missions, "
		"provider, machine_id, machine_address, admin_port, restarts, "
		"failure_reason, created_at, updated_at, running_at, ended_at, own_machine, "
		"machine_ref, slot, game_port, generation, serve_image, "
		"data_set_id, expansion_id, base_tree_id, expansion_tree_id, update_requested, address_id";

// The servers whose pins are behind their data sets' current trees, as a WHERE over
// hosted_servers s (live ones).
constexpr const char *kOutdatedSql =
		"EXISTS (SELECT 1 FROM data_sets b WHERE b.id = s.data_set_id "
		"        AND b.current_tree_id IS NOT NULL AND b.current_tree_id IS DISTINCT FROM s.base_tree_id) "
		"OR EXISTS (SELECT 1 FROM data_sets e WHERE e.id = s.expansion_id "
		"        AND e.current_tree_id IS NOT NULL AND e.current_tree_id IS DISTINCT FROM s.expansion_tree_id)";

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
	st.missions = split_missions(r.as_text(13).value_or(""));
	s.provider = r.as_text(14).value_or("");
	s.container = r.as_text(15).value_or("");
	s.machine_address = r.as_text(16).value_or("");
	s.admin_port = static_cast<int>(r.as_int(17).value_or(0));
	s.restarts = static_cast<int>(r.as_int(18).value_or(0));
	s.failure_reason = r.as_text(19).value_or("");
	s.created_at = r.as_text(20).value_or("");
	s.updated_at = r.as_text(21).value_or("");
	s.running_at = r.as_text(22).value_or("");
	s.ended_at = r.as_text(23).value_or("");
	s.own_machine = r.as_int(24).value_or(0) != 0;
	s.machine_ref = r.as_int(25);
	s.slot = static_cast<int>(r.as_int(26).value_or(-1));
	s.game_port = static_cast<int>(r.as_int(27).value_or(0));
	s.generation = r.as_int(28).value_or(0);
	s.serve_image = r.as_text(29).value_or("");
	// A deleted data set's row leaves its id NULL (ON DELETE SET NULL): 0 here.
	st.data_set_id = r.as_int(30).value_or(0);
	st.expansion_id = r.as_int(31);
	s.base_tree_id = r.as_int(32);
	s.expansion_tree_id = r.as_int(33);
	s.update_requested = r.as_int(34).value_or(0) != 0;
	s.address_id = r.as_int(35);
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

// Whether static address `address_id` (address_repository.h) may be the new
// server's: the owner's, ready, and no live server's.
bool address_free_for(db::Database &db, int64_t address_id, int64_t owner_user_id) {
	return !db.query(std::string("SELECT 1 FROM hosted_addresses a WHERE a.id = $1 AND a.owner_user_id = $2 "
								 "AND a.state = 'ready' AND NOT EXISTS (SELECT 1 FROM hosted_servers s "
								 "WHERE s.address_id = a.id AND s.") +
							kLiveServerSql + ");",
						  { BindValue(address_id), BindValue(owner_user_id) })
					.empty();
}

void insert_event(db::Database &db, int64_t id, const std::string &from, const std::string &to,
		const std::string &detail) {
	db.exec("INSERT INTO hosted_server_events (server_id, from_state, to_state, detail) "
			"VALUES ($1, $2, $3, $4);",
			{ BindValue(id), BindValue(from), BindValue(to), BindValue(detail) });
}

// A stop's cause (and a limits stop's limit) on the server's open start, which
// its end keeps.
void set_stop_cause(
		db::Database &db, int64_t id, StartCause cause, const std::optional<StopLimit> &limit) {
	db.exec("UPDATE hosted_server_starts SET cause = $2, limit_kind = $3, limit_count = $4 "
			"WHERE server_id = $1 AND ended_at IS NULL AND cause IS NULL;",
			{ BindValue(id), BindValue(std::string(start_cause_text(cause))),
					limit ? BindValue(std::string(limit_kind_text(limit->kind))) : BindValue(std::monostate{}),
					limit ? BindValue(static_cast<int64_t>(limit->count)) : BindValue(std::monostate{}) });
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
		const std::vector<OfferedDataSet> &offered, int cap, int64_t now, bool own_machine,
		const PlacementPolicy *fleet, std::optional<int64_t> address_id) {
	RequestResult out;
	if (check_settings(settings, offered)) {
		out.outcome = RequestOutcome::InvalidSettings;
		return out;
	}
	db::Transaction tx(db);
	// The hosted-start lock (database.h), before the first read: one start at a
	// time counts the account's servers, the global cap and the fleet's room,
	// and each read below sees every start that committed before it.
	db.exec("SELECT pg_advisory_xact_lock($1);", { BindValue(db::kHostedStartLock) });
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
	// The entitlement read in the transaction, which holds the hosted-start
	// lock: no other start lands between its count and this row.
	const billing::Entitlement entitlement = billing::entitlement_of(db, owner_user_id, now);
	if (entitlement.max_servers == 0) {
		out.outcome = RequestOutcome::NotEntitled;
		return out;
	}
	if (!entitlement.can_start) {
		out.outcome = RequestOutcome::PastDue;
		return out;
	}
	// Each pool its own places: a shared server within the shared servers, one
	// on a dedicated machine within the dedicated machines.
	out.limit = own_machine ? entitlement.own_machines : entitlement.shared_servers;
	out.used = own_machine ? count_own_live_of(db, owner_user_id) : count_shared_live_of(db, owner_user_id);
	if (out.used >= out.limit) {
		out.outcome = own_machine ? RequestOutcome::NoOwnMachine : RequestOutcome::AtLimit;
		return out;
	}
	if (count_live(db) >= cap || (fleet && !fleet_has_room(db, *fleet, own_machine))) {
		out.outcome = RequestOutcome::AtCapacity;
		return out;
	}
	// Its address, read under the same lock: no other start takes it between.
	if (address_id && (!own_machine || !address_free_for(db, *address_id, owner_user_id))) {
		out.outcome = RequestOutcome::AddressUnavailable;
		return out;
	}
	const auto inserted = db.query(
			"INSERT INTO hosted_servers (owner_user_id, state, data_set_id, game_name, max_players, "
			"game_password, time_limit, kill_limit, max_score, respawn, start_delay, team_ff, "
			"expansion_id, missions, own_machine, address_id) "
			"VALUES ($1, 'requested', $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15) "
			"RETURNING id;",
			{ BindValue(owner_user_id), BindValue(settings.data_set_id), BindValue(settings.game_name),
					BindValue(static_cast<int64_t>(settings.max_players)),
					BindValue(settings.game_password),
					BindValue(static_cast<int64_t>(settings.time_limit)),
					BindValue(static_cast<int64_t>(settings.kill_limit)),
					BindValue(static_cast<int64_t>(settings.max_score)),
					BindValue(static_cast<int64_t>(settings.respawn)),
					BindValue(static_cast<int64_t>(settings.start_delay)),
					BindValue(db::Bool{ settings.team_ff }), optional_id(settings.expansion_id),
					BindValue(join_missions(settings.missions)), BindValue(db::Bool{ own_machine }),
					optional_id(address_id) });
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

int count_shared_live_of(db::Database &db, int64_t owner_user_id) {
	return count_of(db.query(std::string("SELECT COUNT(*) FROM hosted_servers WHERE owner_user_id = $1 "
										 "AND NOT own_machine AND ") +
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

int count_own_live(db::Database &db) {
	return count_of(db.query(std::string("SELECT COUNT(*) FROM hosted_servers WHERE own_machine AND ") +
			kLiveServerSql + ";"));
}

int count_failed_since(db::Database &db, int64_t seconds) {
	const auto rows = db.query("SELECT COUNT(*) FROM hosted_servers WHERE state = 'failed' "
							   "AND ended_at > now() - make_interval(secs => $1);",
			{ BindValue(seconds) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

const char *start_cause_text(StartCause cause) {
	switch (cause) {
		case StartCause::Crash: return "crash";
		case StartCause::Fleet: return "fleet";
		case StartCause::Stop: return "stop";
		case StartCause::OwnerStop: return "owner_stop";
		case StartCause::AdminStop: return "admin_stop";
		case StartCause::Update: return "update";
		case StartCause::Limits: return "limits";
		case StartCause::NoData: return "no_data";
		case StartCause::Timeout: return "timeout";
		case StartCause::LinkLost: return "link_lost";
		case StartCause::DataUnavailable: return "data_unavailable";
	}
	return "fleet";
}

std::optional<StartCause> parse_start_cause(std::string_view text) {
	for (const StartCause cause : { StartCause::Crash, StartCause::Fleet, StartCause::Stop,
				 StartCause::OwnerStop, StartCause::AdminStop, StartCause::Update, StartCause::Limits,
				 StartCause::NoData, StartCause::Timeout, StartCause::LinkLost,
				 StartCause::DataUnavailable }) {
		if (text == start_cause_text(cause)) return cause;
	}
	return std::nullopt;
}

const char *limit_kind_text(LimitKind kind) {
	switch (kind) {
		case LimitKind::SharedServers: return "shared_servers";
		case LimitKind::NoSharedServer: return "no_shared_server";
		case LimitKind::NoSharedRoom: return "no_shared_room";
	}
	return "shared_servers";
}

std::optional<LimitKind> parse_limit_kind(std::string_view text) {
	for (const LimitKind kind :
			{ LimitKind::SharedServers, LimitKind::NoSharedServer, LimitKind::NoSharedRoom }) {
		if (text == limit_kind_text(kind)) return kind;
	}
	return std::nullopt;
}

void set_start_files(db::Database &db, int64_t server_id, int64_t generation, const std::string &serve_image,
		int64_t base_tree_id, std::optional<int64_t> expansion_tree_id, const std::string &log_stream) {
	db.exec("UPDATE hosted_server_starts SET serve_image = $3, base_tree_id = $4, expansion_tree_id = $5, "
			"log_stream = $6 WHERE server_id = $1 AND generation = $2;",
			{ BindValue(server_id), BindValue(generation), BindValue(serve_image), BindValue(base_tree_id),
					expansion_tree_id ? BindValue(*expansion_tree_id) : BindValue(std::monostate{}),
					BindValue(log_stream) });
}

void mark_start_running(db::Database &db, int64_t server_id, int64_t generation) {
	db.exec("UPDATE hosted_server_starts SET running_at = now() WHERE server_id = $1 AND generation = $2 "
			"AND running_at IS NULL;",
			{ BindValue(server_id), BindValue(generation) });
}

bool end_start(db::Database &db, int64_t server_id, int64_t generation, StartCause cause, const StartExit &exit) {
	db.exec("UPDATE hosted_server_starts SET ended_at = now(), cause = COALESCE(cause, $3), exit_code = $4, "
			"oom = $5, reason = $6 WHERE server_id = $1 AND generation = $2 AND ended_at IS NULL;",
			{ BindValue(server_id), BindValue(generation), BindValue(std::string(start_cause_text(cause))),
					exit.exit_code ? BindValue(*exit.exit_code) : BindValue(std::monostate{}),
					BindValue(db::Bool{ exit.oom }), BindValue(exit.reason) });
	return db.changes() > 0;
}

std::vector<ServerStart> list_starts(db::Database &db, int64_t server_id, size_t limit) {
	std::vector<ServerStart> out;
	for (const db::Row &r : db.query("SELECT server_id, generation, machine_id, slot, serve_image, base_tree_id, "
									 "expansion_tree_id, log_stream, placed_at, running_at, ended_at, exit_code, "
									 "oom, reason, cause, limit_kind, limit_count FROM hosted_server_starts "
									 "WHERE server_id = $1 ORDER BY generation DESC LIMIT $2;",
				 { BindValue(server_id), BindValue(static_cast<int64_t>(limit)) })) {
		ServerStart s;
		s.server_id = r.as_int(0).value_or(0);
		s.generation = r.as_int(1).value_or(0);
		s.machine_id = r.as_int(2).value_or(0);
		s.slot = static_cast<int>(r.as_int(3).value_or(0));
		s.serve_image = r.as_text(4).value_or("");
		s.base_tree_id = r.as_int(5);
		s.expansion_tree_id = r.as_int(6);
		s.log_stream = r.as_text(7).value_or("");
		s.placed_at = r.as_text(8).value_or("");
		s.running_at = r.as_text(9).value_or("");
		s.ended_at = r.as_text(10).value_or("");
		s.exit_code = r.as_int(11);
		s.oom = r.as_int(12).value_or(0) != 0;
		s.reason = r.as_text(13).value_or("");
		if (const auto cause = r.as_text(14)) s.cause = parse_start_cause(*cause);
		const auto kind = r.as_text(15);
		const auto limit_kind = kind ? parse_limit_kind(*kind) : std::nullopt;
		if (limit_kind) s.limit = StopLimit{ *limit_kind, static_cast<int>(r.as_int(16).value_or(0)) };
		out.push_back(std::move(s));
	}
	return out;
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

bool clear_server_address(db::Database &db, int64_t id) {
	db.exec("UPDATE hosted_servers SET address_id = NULL, updated_at = now() WHERE id = $1 AND address_id IS NOT NULL;",
			{ BindValue(id) });
	return db.changes() > 0;
}

MoveOutcome clear_own_machine(db::Database &db, int64_t id, State state, const PlacementPolicy &fleet, int64_t now) {
	db::Transaction tx(db);
	// The hosted-start lock (database.h), as a start's judgement takes it.
	db.exec("SELECT pg_advisory_xact_lock($1);", { BindValue(db::kHostedStartLock) });
	const auto row = db.query("SELECT owner_user_id FROM hosted_servers "
							  "WHERE id = $1 AND own_machine AND state = $2 AND owner_user_id IS NOT NULL;",
			{ BindValue(id), BindValue(std::string(state_name(state))) });
	if (row.empty()) return MoveOutcome::Gone;
	// A shared server's start: a shared server of its owner's free, then the
	// fleet's room for it.
	const int64_t owner = row.front().as_int(0).value_or(0);
	if (count_shared_live_of(db, owner) >= billing::entitlement_of(db, owner, now).shared_servers)
		return MoveOutcome::NoSharedServer;
	if (!fleet_has_room(db, fleet, false)) return MoveOutcome::NoRoom;
	db.exec("UPDATE hosted_servers SET own_machine = false, address_id = NULL, updated_at = now() "
			"WHERE id = $1 AND own_machine AND state = $2;",
			{ BindValue(id), BindValue(std::string(state_name(state))) });
	if (db.changes() == 0) return MoveOutcome::Gone;
	tx.commit();
	return MoveOutcome::Moved;
}

bool reset_restarts(db::Database &db, int64_t id) {
	db.exec("UPDATE hosted_servers SET restarts = 0, updated_at = now() "
			"WHERE id = $1 AND state = 'running' AND restarts > 0;",
			{ BindValue(id) });
	return db.changes() > 0;
}

bool request_stop(db::Database &db, int64_t id, const std::string &detail, StartCause cause,
		const std::optional<StopLimit> &limit) {
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
	set_stop_cause(db, id, cause, limit);
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
		set_stop_cause(db, id, StartCause::AdminStop, std::nullopt);
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

// --- the data sets' pins ---

int count_outdated_servers_of(db::Database &db, int64_t id) {
	const auto rows = db.query(std::string("SELECT count(*) FROM hosted_servers s WHERE ") +
					kLiveServerSql + " AND (s.data_set_id = $1 OR s.expansion_id = $1) AND (" +
					kOutdatedSql + ");",
			{ BindValue(id) });
	return rows.empty() ? 0 : static_cast<int>(rows.front().as_int(0).value_or(0));
}

void set_tree_pins(db::Database &db, int64_t server_id, int64_t base_tree_id,
		std::optional<int64_t> expansion_tree_id) {
	db.exec("UPDATE hosted_servers SET base_tree_id = $2, expansion_tree_id = $3, "
			"update_requested = false, updated_at = now() WHERE id = $1;",
			{ BindValue(server_id), BindValue(base_tree_id), optional_id(expansion_tree_id) });
}

UpdateOutcome request_update(db::Database &db, int64_t server_id) {
	db::Transaction tx(db);
	const auto rows = db.query(
			std::string("SELECT update_requested, (") + kLiveServerSql + ") AS live, (" + kOutdatedSql +
					") AS outdated FROM hosted_servers s WHERE id = $1 FOR NO KEY UPDATE;",
			{ BindValue(server_id) });
	if (rows.empty())
		return UpdateOutcome::NotFound;
	const bool requested = rows.front().as_int(0).value_or(0) != 0;
	const bool live = rows.front().as_int(1).value_or(0) != 0;
	const bool outdated = rows.front().as_int(2).value_or(0) != 0;
	if (!live)
		return UpdateOutcome::NotLive;
	if (requested) {
		tx.commit();
		return UpdateOutcome::Requested;
	}
	if (!outdated)
		return UpdateOutcome::UpToDate;
	db.exec("UPDATE hosted_servers SET update_requested = true, updated_at = now() WHERE id = $1;",
			{ BindValue(server_id) });
	tx.commit();
	return UpdateOutcome::Requested;
}

size_t request_update_all(db::Database &db, std::optional<int64_t> data_set_id) {
	std::string sql = std::string("UPDATE hosted_servers s SET update_requested = true, updated_at = now() WHERE ") +
			kLiveServerSql + " AND NOT update_requested AND (" + kOutdatedSql + ")";
	if (data_set_id) {
		db.exec(sql + " AND (s.data_set_id = $1 OR s.expansion_id = $1);", { BindValue(*data_set_id) });
	} else {
		db.exec(sql + ";");
	}
	return static_cast<size_t>(db.changes());
}

bool server_outdated(db::Database &db, int64_t server_id) {
	const auto rows = db.query(
			std::string("SELECT 1 FROM hosted_servers s WHERE id = $1 AND ") + kLiveServerSql + " AND (" +
					kOutdatedSql + ");",
			{ BindValue(server_id) });
	return !rows.empty();
}

} // namespace opennova::novaworld_server::hosted
