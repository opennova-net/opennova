#include <novaworld/host_repository.h>

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
	if (auto v = r.as_int(23)) h.host_user_id = *v;
	h.peer_ip      = r.as_text(24).value_or("");
	h.peer_port    = static_cast<int>(r.as_int(25).value_or(0));
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
	"host_user_id, peer_ip, peer_port";

} // namespace

void clear_all(opennova::db::Database &db) {
	db.exec("DELETE FROM host_players;");
	db.exec("DELETE FROM active_hosts;");
}

void upsert_host(opennova::db::Database &db, const HostRow &h) {
	db.exec(
		"INSERT OR REPLACE INTO active_hosts ("
		" rid, gsid, game, app_id, server_name, host_ip, host_port,"
		" host_key, pcid_key, player_count, max_players, region,"
		" game_type, mission_name, country, password, locked, dedicated, stat,"
		" exp, exp_bits, ver1, joicon2,"
		" host_user_id, peer_ip, peer_port, updated_at) "
		"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,CURRENT_TIMESTAMP);",
		{
			i64(h.rid), str(h.gsid), str(h.game), str(h.app_id),
			str(h.server_name), str(h.host_ip), i64(h.host_port),
			str(h.host_key), str(h.pcid_key),
			i64(h.player_count), i64(h.max_players), str(h.region),
			str(h.game_type), str(h.mission_name), str(h.country),
			str(h.password), str(h.locked), str(h.dedicated), str(h.stat),
			str(h.exp), str(h.exp_bits), str(h.ver1), str(h.joicon2),
			opt_int(h.host_user_id), str(h.peer_ip), i64(h.peer_port),
		});
}

void update_host(opennova::db::Database &db, const HostRow &h) {
	db.exec(
		"UPDATE active_hosts SET "
		" gsid=?, game=?, app_id=?, server_name=?, host_ip=?, host_port=?,"
		" host_key=?, pcid_key=?, player_count=?, max_players=?, region=?,"
		" game_type=?, mission_name=?, country=?, password=?, locked=?,"
		" dedicated=?, stat=?, exp=?, exp_bits=?, ver1=?, joicon2=?,"
		" host_user_id=?, peer_ip=?, peer_port=?, updated_at=CURRENT_TIMESTAMP "
		"WHERE rid=?;",
		{
			str(h.gsid), str(h.game), str(h.app_id), str(h.server_name),
			str(h.host_ip), i64(h.host_port), str(h.host_key), str(h.pcid_key),
			i64(h.player_count), i64(h.max_players), str(h.region),
			str(h.game_type), str(h.mission_name), str(h.country),
			str(h.password), str(h.locked), str(h.dedicated), str(h.stat),
			str(h.exp), str(h.exp_bits), str(h.ver1), str(h.joicon2),
			opt_int(h.host_user_id), str(h.peer_ip), i64(h.peer_port),
			i64(h.rid),
		});
}

void remove_host_by_rid(opennova::db::Database &db, uint32_t rid) {
	db.exec("DELETE FROM active_hosts WHERE rid=?;", {i64(rid)});
}

void remove_host_by_peer(opennova::db::Database &db,
                         const std::string &peer_ip, int peer_port) {
	db.exec("DELETE FROM active_hosts WHERE peer_ip=? AND peer_port=?;",
	        {str(peer_ip), i64(peer_port)});
}

int prune_stale_hosts(opennova::db::Database &db, int64_t window_seconds) {
	// policy: a backstop for rows the normal teardown (GOODBYE /
	// ClientStopHosting / heartbeat-timeout -> erase_lobby_state) somehow
	// missed (a DB write that landed while its in-memory lobby_state was
	// already gone). `updated_at` is refreshed on every ClientHostUpdate
	// heartbeat, so anything older than the window is a crash orphan.
	// host_players rows cascade-delete (ON DELETE CASCADE; sqlite is built
	// with DEFAULT_FOREIGN_KEYS=1).
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

std::vector<HostRow> list_hosts(opennova::db::Database &db) {
	const std::string sql =
		std::string("SELECT ") + HOST_COLUMNS +
		" FROM active_hosts ORDER BY created_at;";
	auto rows = db.query(sql);
	std::vector<HostRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_host(r));
	return out;
}

std::vector<HostRow> list_hosts_by_game(opennova::db::Database &db,
                                        const std::string &game) {
	const std::string sql =
		std::string("SELECT ") + HOST_COLUMNS +
		" FROM active_hosts WHERE game=? ORDER BY created_at;";
	auto rows = db.query(sql, {str(game)});
	std::vector<HostRow> out;
	out.reserve(rows.size());
	for (const auto &r : rows) out.push_back(row_to_host(r));
	return out;
}

std::optional<HostRow> find_host_by_rid(opennova::db::Database &db, uint32_t rid) {
	const std::string sql =
		std::string("SELECT ") + HOST_COLUMNS +
		" FROM active_hosts WHERE rid=? LIMIT 1;";
	auto rows = db.query(sql, {i64(rid)});
	if (rows.empty()) return std::nullopt;
	return row_to_host(rows.front());
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
	h.host_user_id = host_user_id;
	h.peer_ip      = peer_ip;
	h.peer_port    = peer_port;
	return h;
}

} // namespace opennova::hostdb
