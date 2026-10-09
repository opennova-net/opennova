#include <net/novaworld/host_repository.h>

#include <base/io/strutil.h>

namespace opennova::hostdb {

namespace {

opennova::db::BindValue opt_int(std::optional<int64_t> v) {
	if (!v) return opennova::db::BindValue(std::monostate{});
	return opennova::db::BindValue(*v);
}

opennova::db::BindValue str(const std::string &s) {
	return opennova::db::BindValue(s);
}

opennova::db::BindValue i64(int64_t v) { return opennova::db::BindValue(v); }

HostRow row_to_host(const opennova::db::Row &r) {
	HostRow h;
	h.rid          = static_cast<uint32_t>(r.as_int(0).value_or(0));
	h.gsid         = r.as_text(1).value_or("");
	h.game         = r.as_text(2).value_or("");
	h.app_id       = r.as_text(3).value_or("");
	h.server_name  = r.as_text(4).value_or("");
	h.host_ip      = r.as_text(5).value_or("");
	h.host_port    = static_cast<int>(r.as_int(6).value_or(0));
	h.host_key     = r.as_text(7).value_or("");
	h.pcid_key     = r.as_text(8).value_or("");
	h.player_count = static_cast<int>(r.as_int(9).value_or(0));
	h.max_players  = static_cast<int>(r.as_int(10).value_or(0));
	h.region       = r.as_text(11).value_or("");
	h.game_type    = r.as_text(12).value_or("");
	h.mission_name = r.as_text(13).value_or("");
	h.country      = r.as_text(14).value_or("");
	h.password     = r.as_text(15).value_or("N");
	h.locked       = r.as_text(16).value_or("N");
	h.dedicated    = r.as_text(17).value_or("Y");
	h.stat         = r.as_text(18).value_or("N");
	h.exp          = r.as_text(19).value_or("");
	h.exp_bits     = r.as_text(20).value_or("");
	h.ver1         = r.as_text(21).value_or("");
	h.joicon2      = r.as_text(22).value_or("");
	h.time_left    = r.as_text(23).value_or("");
	h.time_of_day  = r.as_text(24).value_or("");
	h.msg          = r.as_text(25).value_or("");
	h.mod          = r.as_text(26).value_or("");
	h.age          = r.as_text(27).value_or("");
	h.pb_server    = r.as_text(28).value_or("");
	h.level_range  = r.as_text(29).value_or("");
	h.bb_mode      = r.as_text(30).value_or("");
	h.skins        = r.as_text(31).value_or("");
	h.tracers      = r.as_text(32).value_or("");
	h.pix          = r.as_text(33).value_or("");
	if (auto v = r.as_int(34)) h.host_user_id = *v;
	h.peer_ip      = r.as_text(35).value_or("");
	h.peer_port    = static_cast<int>(r.as_int(36).value_or(0));
	return h;
}

PlayerRow row_to_player(const opennova::db::Row &r) {
	PlayerRow p;
	p.host_rid = static_cast<uint32_t>(r.as_int(0).value_or(0));
	if (auto v = r.as_int(1)) p.user_id = *v;
	p.nwhandle = r.as_text(2).value_or("");
	p.peer_ip  = r.as_text(3).value_or("");
	p.peer_port = static_cast<int>(r.as_int(4).value_or(0));
	return p;
}

constexpr const char *HOST_COLUMNS =
	"rid, gsid, game, app_id, server_name, host_ip, host_port, "
	"host_key, pcid_key, player_count, max_players, region, "
	"game_type, mission_name, country, password, locked, dedicated, stat, "
	"exp, exp_bits, ver1, joicon2, "
	"time_left, time_of_day, msg, mod, age, pb_server, level_range, bb_mode, "
	"skins, tracers, pix, "
	"host_user_id, peer_ip, peer_port";

std::vector<opennova::db::BindValue> host_binds(const HostRow &h) {
	return {
		str(h.gsid), str(h.game), str(h.app_id),
		str(h.server_name), str(h.host_ip), i64(h.host_port),
		str(h.host_key), str(h.pcid_key),
		i64(h.player_count), i64(h.max_players), str(h.region),
		str(h.game_type), str(h.mission_name), str(h.country),
		str(h.password), str(h.locked), str(h.dedicated), str(h.stat),
		str(h.exp), str(h.exp_bits), str(h.ver1), str(h.joicon2),
		str(h.time_left), str(h.time_of_day), str(h.msg), str(h.mod), str(h.age),
		str(h.pb_server), str(h.level_range), str(h.bb_mode),
		str(h.skins), str(h.tracers), str(h.pix),
		opt_int(h.host_user_id), str(h.peer_ip), i64(h.peer_port),
	};
}

std::vector<HostRow> query_hosts(opennova::db::Database &db, const std::string &where,
                                 const std::vector<opennova::db::BindValue> &binds) {
	const std::string sql =
		std::string("SELECT ") + HOST_COLUMNS + " FROM active_hosts " + where;
	auto rows = db.query(sql, binds);
	std::vector<HostRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_host(r));
	return out;
}

} // namespace

void clear_all(opennova::db::Database &db) {
	opennova::db::Transaction tx(db);
	db.exec("DELETE FROM host_roster;");
	db.exec("DELETE FROM host_players;");
	db.exec("DELETE FROM active_hosts;");
	tx.commit();
}

void upsert_host(opennova::db::Database &db, const HostRow &h) {
	auto binds = host_binds(h);
	binds.insert(binds.begin(), i64(h.rid));
	db.exec(
		"INSERT OR REPLACE INTO active_hosts ("
		" rid, gsid, game, app_id, server_name, host_ip, host_port,"
		" host_key, pcid_key, player_count, max_players, region,"
		" game_type, mission_name, country, password, locked, dedicated, stat,"
		" exp, exp_bits, ver1, joicon2,"
		" time_left, time_of_day, msg, mod, age, pb_server, level_range, bb_mode,"
		" skins, tracers, pix,"
		" host_user_id, peer_ip, peer_port, updated_at) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
		"CURRENT_TIMESTAMP);",
		binds);
}

void update_host(opennova::db::Database &db, const HostRow &h) {
	auto binds = host_binds(h);
	binds.push_back(i64(h.rid));
	db.exec(
		"UPDATE active_hosts SET "
		" gsid=?, game=?, app_id=?, server_name=?, host_ip=?, host_port=?,"
		" host_key=?, pcid_key=?, player_count=?, max_players=?, region=?,"
		" game_type=?, mission_name=?, country=?, password=?, locked=?,"
		" dedicated=?, stat=?, exp=?, exp_bits=?, ver1=?, joicon2=?,"
		" time_left=?, time_of_day=?, msg=?, mod=?, age=?, pb_server=?,"
		" level_range=?, bb_mode=?, skins=?, tracers=?, pix=?,"
		" host_user_id=?, peer_ip=?, peer_port=?, updated_at=CURRENT_TIMESTAMP "
		"WHERE rid=?;",
		binds);
}

void remove_host_by_rid(opennova::db::Database &db, uint32_t rid) {
	db.exec("DELETE FROM active_hosts WHERE rid=?;", {i64(rid)});
}

int prune_stale_hosts(opennova::db::Database &db, int64_t window_seconds) {
	// policy: a backstop for rows the normal teardown (GOODBYE /
	// ClientStopHosting / heartbeat-timeout -> erase_lobby_state) somehow
	// missed (a DB write that landed while its in-memory lobby_state was
	// already gone). `updated_at` is refreshed on every ClientHostUpdate
	// heartbeat, so anything older than the window is a crash orphan.
	// host_players / host_roster rows cascade-delete (ON DELETE CASCADE;
	// sqlite is built with DEFAULT_FOREIGN_KEYS=1).
	const std::string modifier = "-" + std::to_string(window_seconds) + " seconds";
	db.exec("DELETE FROM active_hosts WHERE updated_at < datetime('now', ?);",
	        {str(modifier)});
	return db.changes();
}

void add_player(opennova::db::Database &db, const PlayerRow &p) {
	db.exec(
		"INSERT OR REPLACE INTO host_players ("
		" host_rid, user_id, nwhandle, peer_ip, peer_port) "
		"VALUES (?,?,?,?,?);",
		{i64(p.host_rid), opt_int(p.user_id), str(p.nwhandle),
		 str(p.peer_ip), i64(p.peer_port)});
}

void remove_player_by_peer(opennova::db::Database &db,
                           const std::string &peer_ip, int peer_port) {
	db.exec("DELETE FROM host_players WHERE peer_ip=? AND peer_port=?;",
	        {str(peer_ip), i64(peer_port)});
}

void replace_roster(opennova::db::Database &db, uint32_t host_rid,
                    const std::vector<HostRosterSlot> &roster) {
	// One transaction, so a reader on another connection never sees the
	// roster between the delete and the last insert (partial or empty).
	opennova::db::Transaction tx(db);
	db.exec("DELETE FROM host_roster WHERE host_rid=?;", {i64(host_rid)});
	for (const auto &s : roster) {
		db.exec(
			"INSERT OR REPLACE INTO host_roster ("
			" host_rid, slot, player_name, ip_and_port, pcid, team, type) "
			"VALUES (?,?,?,?,?,?,?);",
			{i64(host_rid), i64(s.slot), str(s.player_name), str(s.ip_and_port),
			 str(s.pcid), str(s.team), str(s.type)});
	}
	tx.commit();
}

std::vector<HostRosterSlot> list_roster(opennova::db::Database &db, uint32_t host_rid) {
	auto rows = db.query(
		"SELECT slot, player_name, ip_and_port, pcid, team, type "
		"FROM host_roster WHERE host_rid=? ORDER BY slot;",
		{i64(host_rid)});
	std::vector<HostRosterSlot> out;
	out.reserve(rows.size());
	for (const auto &r : rows) {
		HostRosterSlot s;
		s.slot        = static_cast<int>(r.as_int(0).value_or(0));
		s.player_name = r.as_text(1).value_or("");
		s.ip_and_port = r.as_text(2).value_or("");
		s.pcid        = r.as_text(3).value_or("");
		s.team        = r.as_text(4).value_or("");
		s.type        = r.as_text(5).value_or("");
		out.push_back(std::move(s));
	}
	return out;
}

bool apply_status_blob(opennova::db::Database &db, const LobbyStatusBlob &blob) {
	if (blob.host_key.empty()) return false;
	// One transaction for the read-modify-write: the row and its roster change
	// together, and no ClientHostUpdate can commit between this read and this
	// write (and be overwritten with the columns the read saw).
	opennova::db::Transaction tx(db);
	auto host = find_host_by_key(db, blob.host_key);
	if (!host) return false;
	HostRow h = *host;
	auto take = [&blob](std::string &dst, const char *key) {
		if (auto v = lobby_status_value(blob, key); !v.empty()) dst = v;
	};
	take(h.server_name,  "ServerName");
	take(h.game_type,    "GameType");
	take(h.mission_name, "MissionName");
	take(h.region,       "Region");
	// A count that is no number (or out of int's range) keeps the stored one.
	if (const auto n = strutil::parse_int(lobby_status_value(blob, "Players"))) h.player_count = *n;
	if (const auto n = strutil::parse_int(lobby_status_value(blob, "MaxPlayers"))) h.max_players = *n;
	take(h.dedicated,   "Dedicated");
	take(h.locked,      "Locked");
	take(h.skins,       "Skins");
	take(h.time_left,   "TimeLeft");
	take(h.password,    "Password");
	take(h.tracers,     "Tracers");
	take(h.mod,         "Mod");
	take(h.country,     "Country");
	take(h.msg,         "Msg");
	take(h.age,         "Age");
	take(h.time_of_day, "TimeOfDay");
	take(h.pcid_key,    "PCIDKey");
	take(h.stat,        "Stat");
	take(h.level_range, "LevelRange");
	take(h.bb_mode,     "BBMode");
	take(h.pb_server,   "PBServer");
	take(h.exp,         "Exp");
	update_host(db, h);
	if (blob.send_player_names) {
		std::vector<HostRosterSlot> roster;
		roster.reserve(blob.player_names.size());
		int slot = 0;
		for (const auto &name : blob.player_names) {
			HostRosterSlot s;
			s.slot = slot++;
			s.player_name = name;
			roster.push_back(std::move(s));
		}
		replace_roster(db, h.rid, roster);
	}
	tx.commit();
	return true;
}

std::vector<HostRow> list_hosts(opennova::db::Database &db) {
	return query_hosts(db, "ORDER BY created_at;", {});
}

std::vector<HostRow> list_hosts_by_game(opennova::db::Database &db,
                                        const std::string &game) {
	return query_hosts(db, "WHERE game=? ORDER BY created_at;", {str(game)});
}

std::optional<HostRow> find_host_by_rid(opennova::db::Database &db, uint32_t rid) {
	auto rows = query_hosts(db, "WHERE rid=? LIMIT 1;", {i64(rid)});
	if (rows.empty()) return std::nullopt;
	return rows.front();
}

std::optional<HostRow> find_host_by_key(opennova::db::Database &db,
                                        const std::string &host_key) {
	auto rows = query_hosts(db, "WHERE host_key=? LIMIT 1;", {str(host_key)});
	if (rows.empty()) return std::nullopt;
	return rows.front();
}

std::vector<PlayerRow> list_players(opennova::db::Database &db, uint32_t host_rid) {
	auto rows = db.query(
		"SELECT host_rid, user_id, nwhandle, peer_ip, peer_port "
		"FROM host_players WHERE host_rid=? ORDER BY joined_at;",
		{i64(host_rid)});
	std::vector<PlayerRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_player(r));
	return out;
}

LobbyAggregate aggregate(opennova::db::Database &db) {
	LobbyAggregate agg;
	{
		auto rows = db.query("SELECT COUNT(*) FROM games;");
		if (!rows.empty()) agg.games = static_cast<int>(rows.front().as_int(0).value_or(0));
	}
	{
		auto rows = db.query(
			"SELECT COUNT(*), COALESCE(SUM(player_count),0) FROM active_hosts;");
		if (!rows.empty()) {
			agg.lobbies = static_cast<int>(rows.front().as_int(0).value_or(0));
			agg.players = static_cast<int>(rows.front().as_int(1).value_or(0));
		}
	}
	return agg;
}

HostRow row_from_lobby(const LobbyState &lobby,
                       const std::string &peer_ip, int peer_port,
                       std::optional<int64_t> host_user_id) {
	HostRow h;
	h.rid          = lobby.rid;
	h.gsid         = lobby.gsid;
	h.game         = lobby.game;
	h.app_id       = lobby.app_id;
	h.server_name  = lobby.server_name;
	h.host_ip      = lobby.host_ip.empty() ? peer_ip : lobby.host_ip;
	h.host_port    = lobby.host_port == 0 ? peer_port : lobby.host_port;
	h.host_key     = lobby.host_key;
	h.pcid_key     = lobby.pcid_key;
	h.player_count = lobby.player_count;
	h.max_players  = lobby.max_players;
	h.region       = lobby.region;
	h.game_type    = lobby.game_type;
	h.mission_name = lobby.mission_name;
	h.country      = lobby.country;
	h.password     = lobby.password;
	h.locked       = lobby.locked;
	h.dedicated    = lobby.dedicated;
	h.stat         = lobby.stat;
	h.exp          = lobby.exp;
	h.exp_bits     = lobby.exp_bits;
	h.ver1         = lobby.ver1;
	h.joicon2      = lobby.joicon2;
	h.time_left    = lobby.time_left;
	h.time_of_day  = lobby.time_of_day;
	h.msg          = lobby.msg;
	h.mod          = lobby.mod;
	h.age          = lobby.age;
	h.pb_server    = lobby.pb_server;
	h.level_range  = lobby.level_range;
	h.bb_mode      = lobby.bb_mode;
	h.skins        = lobby.skins;
	h.tracers      = lobby.tracers;
	h.pix          = lobby.pix;
	h.host_user_id = host_user_id;
	h.peer_ip      = peer_ip;
	h.peer_port    = peer_port;
	return h;
}

GsbServerEntry gsb_entry_from_host(const HostRow &h,
                                   const std::vector<HostRosterSlot> &roster) {
	GsbServerEntry e;
	e.rid = h.rid;      // host id — the GSB row's first u32 (the join rid)
	e.ip  = h.host_ip;  // row dword1: the ping-target IPv4 retail's browser formats
	                    // from entry+4 on the XXXX finalize
	                    // [orig: NapiGameList_StartPingSweep @ 0x63BCF0]. The joiner
	                    // resolves the connect address from the NK token.
	auto or_default = [](const std::string &v, const std::string &fallback) {
		return v.empty() ? fallback : v;
	};
	e.server_name  = or_default(h.server_name, "Unnamed Server");
	e.game_type    = or_default(h.game_type, e.game_type);
	e.mission_name = h.mission_name;
	e.region       = h.region;
	e.players      = h.player_count;
	e.max_players  = h.max_players;
	e.dedicated    = or_default(h.dedicated, e.dedicated);
	e.time_left    = or_default(h.time_left, e.time_left);
	e.password     = or_default(h.password, e.password);
	e.country      = or_default(h.country, h.region);
	e.msg          = h.msg;
	e.age          = or_default(h.age, e.age);
	e.time_of_day  = h.time_of_day;
	e.stat         = or_default(h.stat, e.stat);
	e.level_range  = h.level_range;
	e.locked       = or_default(h.locked, e.locked);
	e.tracers      = or_default(h.tracers, e.tracers);
	e.skins        = or_default(h.skins, e.skins);
	e.bb_mode      = or_default(h.bb_mode, e.bb_mode);
	e.mod          = h.mod;
	e.pix          = or_default(h.pix, e.pix);
	e.pb_server    = or_default(h.pb_server, e.pb_server);
	const bool dfx2 = h.game == "dfx2_consumer";
	e.ver1         = or_default(h.ver1, dfx2 ? "1" : "3");
	e.exp          = h.exp;
	e.exp_bits     = or_default(h.exp_bits, dfx2 ? "1" : "3");
	e.joicon2      = or_default(h.joicon2, e.joicon2);
	e.player_names.reserve(roster.size());
	for (const auto &s : roster) {
		if (!s.player_name.empty()) e.player_names.push_back(s.player_name);
	}
	return e;
}

} // namespace opennova::hostdb
