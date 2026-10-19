#include <net/novaworld/lobby_repository.h>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace opennova::lobbydb {

namespace {

using db::BindValue;

BindValue str(const std::string &s) { return BindValue(s); }
BindValue i64(int64_t v) { return BindValue(v); }
BindValue null() { return BindValue(std::monostate{}); }
BindValue opt_int(std::optional<int64_t> v) { return v ? BindValue(*v) : null(); }
// A RID column: NULL for none (0).
BindValue rid_bind(uint32_t rid) { return rid == 0 ? null() : i64(rid); }
BindValue flag(bool b) { return BindValue(db::Bool{ b }); }
// A lobby_id column: NULL for none (0).
BindValue lobby_bind(int64_t lobby_id) {
	return lobby_id == 0 ? null() : i64(lobby_id);
}

// A time column as epoch ms: NULL for 0.
BindValue ms_bind(int64_t ms) { return ms == 0 ? null() : i64(ms); }

constexpr const char *kSessionColumns =
		"id, connection_id, peer_ip, peer_port, state, pn, identity, user_id, rid, "
		"hosting, listed, host_user_id, server_name, mission_name, players, max_players, "
		"(extract(epoch FROM connected_at) * 1000)::bigint, "
		"(extract(epoch FROM last_seen_at) * 1000)::bigint, COALESCE(lobby_id, 0), "
		"playing_rid, play_addr, COALESCE((extract(epoch FROM playing_since) * 1000)::bigint, 0)";

constexpr const char *kInstanceColumns =
		"id, endpoint, state, (extract(epoch FROM started_at) * 1000)::bigint, "
		"(extract(epoch FROM last_seen_at) * 1000)::bigint, "
		"(extract(epoch FROM now() - last_seen_at) * 1000)::bigint, task_id";

LobbyInstance row_to_instance(const db::Row &r) {
	LobbyInstance i;
	i.id = r.as_int(0).value_or(0);
	i.endpoint = r.as_text(1).value_or("");
	i.state = r.as_text(2).value_or("");
	i.started_ms = r.as_int(3).value_or(0);
	i.last_seen_ms = r.as_int(4).value_or(0);
	i.seen_ago_ms = r.as_int(5).value_or(0);
	i.task_id = r.as_text(6).value_or("");
	return i;
}

SessionRow row_to_session(const db::Row &r) {
	SessionRow s;
	s.id = r.as_int(0).value_or(0);
	s.connection_id = static_cast<uint32_t>(r.as_int(1).value_or(0));
	s.peer_ip = r.as_text(2).value_or("");
	s.peer_port = static_cast<int>(r.as_int(3).value_or(0));
	s.state = r.as_text(4).value_or("");
	s.pn = r.as_text(5).value_or("");
	s.identity = r.as_text(6).value_or("");
	s.user_id = r.as_int(7);
	s.rid = static_cast<uint32_t>(r.as_int(8).value_or(0));
	s.hosting = r.as_int(9).value_or(0) != 0;
	s.listed = r.as_int(10).value_or(0) != 0;
	s.host_user_id = r.as_int(11);
	s.server_name = r.as_text(12).value_or("");
	s.mission_name = r.as_text(13).value_or("");
	s.players = static_cast<int>(r.as_int(14).value_or(0));
	s.max_players = static_cast<int>(r.as_int(15).value_or(0));
	s.connected_ms = r.as_int(16).value_or(0);
	s.last_seen_ms = r.as_int(17).value_or(0);
	s.lobby_id = r.as_int(18).value_or(0);
	s.playing_rid = static_cast<uint32_t>(r.as_int(19).value_or(0));
	s.play_addr = r.as_text(20).value_or("");
	s.playing_since_ms = r.as_int(21).value_or(0);
	return s;
}

const char *kind_name_of(CommandKind kind) {
	switch (kind) {
		case CommandKind::ServerCommand:
			return "server_command";
		case CommandKind::StopHosting:
			return "stop_hosting";
		case CommandKind::EndSession:
			return "end_session";
		case CommandKind::ForgetOwner:
			return "forget_owner";
	}
	return "";
}

std::optional<CommandKind> kind_of(const std::string &name) {
	for (const CommandKind kind : { CommandKind::ServerCommand, CommandKind::StopHosting,
				 CommandKind::EndSession, CommandKind::ForgetOwner }) {
		if (name == kind_name_of(kind))
			return kind;
	}
	return std::nullopt;
}

// Inserts a command for session `session_id`, with the session's lobby_id (so a command goes to
// the lobby that holds its session), unless the session went since it was read (its delete
// cascades its commands, so none waits for it): false then. A NULL bind in the SELECT list has no
// column to take its type from, so the statement casts each.
bool insert_for_session(db::Database &db, CommandKind kind, int64_t session_id, uint32_t rid,
		std::optional<int64_t> account, BindValue cmd, BindValue msg_code, BindValue msg_param1,
		BindValue msg_param2) {
	try {
		db.exec("INSERT INTO lobby_commands (lobby_id, kind, session_id, rid, account_id, cmd, "
				"msg_code, msg_param1, msg_param2, expires_at) "
				"SELECT lobby_id, $2::text, id, $3::bigint, $4::bigint, $5::text, $6::bigint, "
				"$7::bigint, $8::bigint, now() + make_interval(secs => $9) "
				"FROM lobby_sessions WHERE id = $1;",
				{ i64(session_id), str(kind_name_of(kind)), rid_bind(rid), opt_int(account), std::move(cmd),
						std::move(msg_code), std::move(msg_param1), std::move(msg_param2),
						i64(kCommandTtlSeconds) });
	} catch (const db::Error &e) {
		// 23503: the session's delete committed after the insert read it.
		if (e.sqlstate() == "23503")
			return false;
		throw;
	}
	return db.changes() > 0;
}

PushResult queue_push(db::Database &db, CommandKind kind, uint32_t rid, std::optional<int64_t> owner,
		BindValue cmd, BindValue msg_code, BindValue msg_param1, BindValue msg_param2) {
	if (rid == 0)
		return PushResult::UnknownRid;
	const auto rows = db.query("SELECT id, hosting FROM lobby_sessions WHERE rid = $1 "
							   "AND ($2::bigint IS NULL OR host_user_id = $2::bigint) "
							   "ORDER BY hosting DESC, id LIMIT 1;",
			{ i64(rid), opt_int(owner) });
	if (rows.empty())
		return PushResult::UnknownRid;
	if (rows.front().as_int(1).value_or(0) == 0)
		return PushResult::NotHosted;
	if (!insert_for_session(db, kind, rows.front().as_int(0).value_or(0), rid, owner, std::move(cmd),
				std::move(msg_code), std::move(msg_param1), std::move(msg_param2))) {
		return PushResult::UnknownRid;
	}
	return PushResult::Queued;
}

} // namespace

const char *command_kind_name(CommandKind kind) { return kind_name_of(kind); }

int64_t register_instance(
		db::Database &db, const std::string &endpoint, bool draining, const std::string &task_id) {
	const auto rows = db.query("INSERT INTO lobby_instances (endpoint, state, task_id) "
							   "VALUES ($1, $2, $3) RETURNING id;",
			{ str(endpoint), str(draining ? "draining" : "active"), str(task_id) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

std::optional<std::string> touch_instance(db::Database &db, int64_t id) {
	const auto rows = db.query(
			"UPDATE lobby_instances SET last_seen_at = now() WHERE id = $1 RETURNING state;",
			{ i64(id) });
	if (rows.empty())
		return std::nullopt;
	return rows.front().as_text(0).value_or("");
}

bool set_instance_state(db::Database &db, int64_t id, const std::string &state) {
	if (state != "active" && state != "draining")
		return false;
	db.exec("UPDATE lobby_instances SET state = $2 WHERE id = $1;", { i64(id), str(state) });
	return db.changes() > 0;
}

void remove_instance(db::Database &db, int64_t id) {
	db.exec("DELETE FROM lobby_instances WHERE id = $1;", { i64(id) });
}

std::size_t reap_stale_instances(db::Database &db, int64_t max_age_seconds, int64_t keep) {
	db.exec("DELETE FROM lobby_instances WHERE last_seen_at < now() - make_interval(secs => $1) "
			"AND id <> $2;",
			{ i64(max_age_seconds), i64(keep) });
	return static_cast<std::size_t>(db.changes());
}

std::vector<LobbyInstance> list_instances(db::Database &db) {
	std::vector<LobbyInstance> out;
	for (const auto &r : db.query(
				 std::string("SELECT ") + kInstanceColumns + " FROM lobby_instances ORDER BY id;"))
		out.push_back(row_to_instance(r));
	return out;
}

LiveLobbies live_lobbies(db::Database &db) {
	const auto rows = db.query(
			"SELECT endpoint, count(*) OVER () FROM lobby_instances WHERE state = 'active' "
			"AND last_seen_at >= now() - make_interval(secs => $1) ORDER BY id DESC LIMIT 1;",
			{ i64(kInstanceLiveSeconds) });
	LiveLobbies live;
	if (!rows.empty()) {
		live.endpoint = rows.front().as_text(0).value_or("");
		live.instances = static_cast<std::size_t>(rows.front().as_int(1).value_or(0));
	}
	return live;
}

uint32_t next_rid(db::Database &db) {
	// Each value, and whether a listing or a session row holds the RID it makes (each indexed).
	for (int attempt = 0; attempt < kRidAttempts; ++attempt) {
		const auto rows =
				db.query("SELECT r, EXISTS (SELECT 1 FROM active_hosts WHERE rid = r) OR "
						 "EXISTS (SELECT 1 FROM lobby_sessions WHERE rid = r) "
						 "FROM (SELECT $1::bigint | nextval('lobby_rid_seq') AS r) AS minted;",
						{ i64(0x0A000000) });
		if (rows.empty())
			return 0;
		if (rows.front().as_int(1).value_or(0) == 0)
			return static_cast<uint32_t>(rows.front().as_int(0).value_or(0));
	}
	return 0;
}

std::string session_addr(const SessionRow &row) {
	return row.peer_ip + ":" + std::to_string(row.peer_port);
}

bool same_columns(const SessionRow &a, const SessionRow &b) {
	return a.connection_id == b.connection_id && a.peer_ip == b.peer_ip &&
			a.peer_port == b.peer_port && a.state == b.state && a.pn == b.pn &&
			a.identity == b.identity && a.user_id == b.user_id && a.rid == b.rid &&
			a.hosting == b.hosting && a.listed == b.listed && a.host_user_id == b.host_user_id &&
			a.server_name == b.server_name && a.mission_name == b.mission_name &&
			a.players == b.players && a.max_players == b.max_players &&
			a.playing_rid == b.playing_rid && a.play_addr == b.play_addr &&
			a.playing_since_ms == b.playing_since_ms;
}

int64_t insert_session(db::Database &db, const SessionRow &row) {
	const auto rows = db.query(
			"INSERT INTO lobby_sessions (connection_id, peer_ip, peer_port, state, pn, identity, "
			"user_id, rid, hosting, listed, host_user_id, server_name, mission_name, players, "
			"max_players, connected_at, last_seen_at, lobby_id, playing_rid, play_addr, "
			"playing_since) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, "
			"$15, to_timestamp($16::float8 / 1000), to_timestamp($17::float8 / 1000), $18, $19, "
			"$20, to_timestamp($21::float8 / 1000)) RETURNING id;",
			{ i64(row.connection_id), str(row.peer_ip), i64(row.peer_port), str(row.state),
					str(row.pn), str(row.identity), opt_int(row.user_id), rid_bind(row.rid),
					flag(row.hosting), flag(row.listed), opt_int(row.host_user_id),
					str(row.server_name), str(row.mission_name), i64(row.players),
					i64(row.max_players), i64(row.connected_ms), i64(row.last_seen_ms),
					lobby_bind(row.lobby_id), rid_bind(row.playing_rid), str(row.play_addr),
					ms_bind(row.playing_since_ms) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

void update_session(db::Database &db, const SessionRow &row) {
	db.exec("UPDATE lobby_sessions SET connection_id = $2, peer_ip = $3, peer_port = $4, "
			"state = $5, pn = $6, identity = $7, user_id = $8, rid = $9, hosting = $10, "
			"listed = $11, host_user_id = $12, server_name = $13, mission_name = $14, "
			"players = $15, max_players = $16, playing_rid = $17, play_addr = $18, "
			"playing_since = to_timestamp($19::float8 / 1000) WHERE id = $1;",
			{ i64(row.id), i64(row.connection_id), str(row.peer_ip), i64(row.peer_port),
					str(row.state), str(row.pn), str(row.identity), opt_int(row.user_id),
					rid_bind(row.rid), flag(row.hosting), flag(row.listed), opt_int(row.host_user_id),
					str(row.server_name), str(row.mission_name), i64(row.players),
					i64(row.max_players), rid_bind(row.playing_rid), str(row.play_addr),
					ms_bind(row.playing_since_ms) });
}

void update_session_listing(db::Database &db, int64_t id, uint32_t rid, bool hosting, bool listed,
		std::optional<int64_t> host_user_id, const std::string &server_name,
		const std::string &mission_name, int players, int max_players) {
	db.exec("UPDATE lobby_sessions SET rid = $2, hosting = $3, listed = $4, host_user_id = $5, "
			"server_name = $6, mission_name = $7, players = $8, max_players = $9 WHERE id = $1;",
			{ i64(id), rid_bind(rid), flag(hosting), flag(listed), opt_int(host_user_id),
					str(server_name), str(mission_name), i64(players), i64(max_players) });
}

void delete_session(db::Database &db, int64_t id) {
	db.exec("DELETE FROM lobby_sessions WHERE id = $1;", { i64(id) });
}

std::size_t delete_sessions_except(
		db::Database &db, int64_t lobby_id, const std::vector<int64_t> &keep) {
	if (lobby_id == 0)
		return 0;
	// An array literal ("{1,2}") the statement casts; an empty one keeps none.
	std::string ids = "{";
	for (std::size_t i = 0; i < keep.size(); ++i) {
		if (i != 0)
			ids += ',';
		ids += std::to_string(keep[i]);
	}
	ids += '}';
	db.exec("DELETE FROM lobby_sessions WHERE lobby_id = $1 AND id <> ALL($2::bigint[]);",
			{ i64(lobby_id), str(ids) });
	return static_cast<std::size_t>(db.changes());
}

void touch_sessions(db::Database &db, const std::vector<SessionSeen> &seen) {
	if (seen.empty())
		return;
	// Two array literals ("{1,2}") the statement casts, one row of the update per pair.
	std::string ids = "{";
	std::string times = "{";
	for (std::size_t i = 0; i < seen.size(); ++i) {
		if (i != 0) {
			ids += ',';
			times += ',';
		}
		ids += std::to_string(seen[i].id);
		times += std::to_string(seen[i].last_seen_ms);
	}
	ids += '}';
	times += '}';
	db.exec("UPDATE lobby_sessions s SET last_seen_at = to_timestamp(v.ms::float8 / 1000) "
			"FROM unnest($1::bigint[], $2::bigint[]) AS v(id, ms) WHERE s.id = v.id;",
			{ str(ids), str(times) });
}

std::vector<SessionRow> list_sessions(db::Database &db) {
	std::vector<SessionRow> out;
	for (const auto &r : db.query(std::string("SELECT ") + kSessionColumns +
				 " FROM lobby_sessions ORDER BY id;"))
		out.push_back(row_to_session(r));
	return out;
}

std::optional<int64_t> login_account(db::Database &db, const std::string &session_tag) {
	if (session_tag.empty())
		return std::nullopt;
	const auto rows = db.query("SELECT s.user_id FROM active_user_sessions s "
							   "JOIN players p ON p.id = s.user_id "
							   "WHERE s.session_tag = $1 AND s.credentials_gen = p.credentials_gen "
							   "LIMIT 1;",
			{ str(session_tag) });
	if (rows.empty())
		return std::nullopt;
	return rows.front().as_int(0);
}

const char *push_result_name(PushResult result) {
	switch (result) {
		case PushResult::Queued:
			return "queued";
		case PushResult::UnknownRid:
			return "unknown_rid";
		case PushResult::NotHosted:
			return "not_hosting";
	}
	return "unknown";
}

PushResult queue_server_command(
		db::Database &db, uint32_t rid, const std::string &cmd, std::optional<int64_t> owner) {
	return queue_push(db, CommandKind::ServerCommand, rid, owner, str(cmd), null(), null(), null());
}

PushResult queue_stop_hosting(db::Database &db, uint32_t rid, int msg_code, int msg_param1,
		int msg_param2, std::optional<int64_t> owner) {
	return queue_push(db, CommandKind::StopHosting, rid, owner, null(), i64(msg_code),
			i64(msg_param1), i64(msg_param2));
}

const char *end_session_result_name(EndSession::Result result) {
	switch (result) {
		case EndSession::Result::Queued:
			return "queued";
		case EndSession::Result::NotFound:
			return "not_found";
		case EndSession::Result::Hosting:
			return "hosting";
	}
	return "unknown";
}

EndSession queue_end_session(db::Database &db, int64_t session_id) {
	EndSession out;
	const auto rows = db.query(
			std::string("SELECT ") + kSessionColumns + " FROM lobby_sessions WHERE id = $1;",
			{ i64(session_id) });
	if (rows.empty()) {
		out.result = EndSession::Result::NotFound;
		return out;
	}
	out.session = row_to_session(rows.front());
	if (out.session.hosting) {
		out.result = EndSession::Result::Hosting;
		return out;
	}
	out.result = insert_for_session(db, CommandKind::EndSession, out.session.id, 0, std::nullopt,
						 null(), null(), null(), null())
			? EndSession::Result::Queued
			: EndSession::Result::NotFound;
	return out;
}

void queue_forget_owner(db::Database &db, int64_t user_id) {
	db.exec("INSERT INTO lobby_commands (lobby_id, kind, account_id, expires_at) "
			"SELECT id, $1, $2, now() + make_interval(secs => $3) FROM lobby_instances;",
			{ str(kind_name_of(CommandKind::ForgetOwner)), i64(user_id), i64(kCommandTtlSeconds) });
}

std::vector<Command> take_commands(db::Database &db, int64_t lobby_id, std::size_t limit) {
	std::vector<Command> out;
	if (lobby_id == 0)
		return out;
	// lobby_id = $2 (never NULL here), so the (lobby_id, id) index serves it.
	const auto rows =
			db.query("DELETE FROM lobby_commands WHERE id IN "
					 "(SELECT id FROM lobby_commands WHERE lobby_id = $2 ORDER BY id LIMIT $1) "
					 "RETURNING id, kind, session_id, rid, account_id, cmd, msg_code, msg_param1, "
					 "msg_param2, expires_at <= now();",
					{ i64(static_cast<int64_t>(limit)), i64(lobby_id) });
	out.reserve(rows.size());
	for (const auto &r : rows) {
		const auto kind = kind_of(r.as_text(1).value_or(""));
		if (!kind)
			continue; // no kind the table's CHECK admits
		Command c;
		c.id = r.as_int(0).value_or(0);
		c.kind = *kind;
		c.session_id = r.as_int(2).value_or(0);
		c.rid = static_cast<uint32_t>(r.as_int(3).value_or(0));
		c.account = r.as_int(4);
		c.cmd = r.as_text(5).value_or("");
		c.msg_code = static_cast<int>(r.as_int(6).value_or(0));
		c.msg_param1 = static_cast<int>(r.as_int(7).value_or(0));
		c.msg_param2 = static_cast<int>(r.as_int(8).value_or(0));
		c.expired = r.as_int(9).value_or(0) != 0;
		out.push_back(std::move(c));
	}
	std::sort(out.begin(), out.end(), [](const Command &a, const Command &b) { return a.id < b.id; });
	return out;
}

} // namespace opennova::lobbydb
