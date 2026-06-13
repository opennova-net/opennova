#pragma once

#include <novaworld/db/sqlite.h>
#include <novaworld/lobby_session.h>      // LobbyState

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova {

// Thin SQL wrapper around the `active_hosts` + `host_players` tables.
// All methods take a Database& and call into the same prepared-statement
// path as the rest of libs/novaworld — single-threaded ownership of the
// handle is the caller's responsibility (the standalone server's UDP
// thread + Crow worker threads share a single Database, so all of
// host_repository.* sits behind the listener-side mutex that wraps the
// sqlite handle).
//
// Lifecycle (mirrors onnw/onnw/hosts.py + the lobby-session dispatch):
//   ClientHostRequest  -> upsert_host() (INSERT OR REPLACE)
//   ClientHostUpdate   -> update_host() (refresh ClientHostUpdate fields)
//   GOODBYE / timeout  -> remove_host_by_peer()
//   ClientHostPlayerAdded   -> add_player()
//   ClientHostPlayerRemoved -> remove_player_by_peer() (a peer can leave
//     at most one host at a time so the unique-by-peer index is enough)
//   server boot        -> clear_all() (drop stale rows from prev run)
namespace hostdb {

struct HostRow {
	uint32_t    rid = 0;
	std::string gsid;
	std::string game;
	std::string app_id;
	std::string server_name;
	std::string host_ip;
	int         host_port = 0;
	std::string host_key;
	std::string pcid_key;
	int         player_count = 0;
	int         max_players = 0;
	std::string region;
	std::string game_type;
	std::string mission_name;
	std::string country;
	std::string password;
	std::string locked;
	std::string dedicated;
	std::string stat;
	std::string exp;
	std::string exp_bits;
	std::string ver1;
	std::string joicon2;
	std::optional<int64_t> host_user_id;
	std::string peer_ip;
	int         peer_port = 0;
};

struct PlayerRow {
	uint32_t    host_rid = 0;
	std::optional<int64_t> user_id;
	std::string nwhandle;
	std::string peer_ip;
	int         peer_port = 0;
};

void clear_all(db::Database &db);

void upsert_host(db::Database &db, const HostRow &row);
void update_host(db::Database &db, const HostRow &row);
void remove_host_by_rid(db::Database &db, uint32_t rid);
void remove_host_by_peer(db::Database &db, const std::string &peer_ip, int peer_port);

// Backstop sweep: delete active_hosts whose updated_at is older than
// `window_seconds` (crash orphans the normal teardown missed). Returns the
// number of rows removed. host_players cascade. policy, not wire-witnessed.
int prune_stale_hosts(db::Database &db, int64_t window_seconds);

void add_player(db::Database &db, const PlayerRow &row);
void remove_player_by_peer(db::Database &db, const std::string &peer_ip, int peer_port);

std::vector<HostRow>   list_hosts(db::Database &db);
// Hosts for one game slug (active_hosts.game == game). Mirrors onnet's
// per-game GSB query (onnw/hosts.py::fetch_hosts_by_game) so /jop_2.gsb and
// /dfx2_0.gsb don't cross-contaminate. Uses idx_active_hosts_game.
std::vector<HostRow>   list_hosts_by_game(db::Database &db, const std::string &game);
std::optional<HostRow> find_host_by_rid(db::Database &db, uint32_t rid);
std::vector<PlayerRow> list_players(db::Database &db, uint32_t host_rid);

// Aggregates used by /api/stats and /api/lobbies.
struct LobbyAggregate {
	int games        = 0;
	int lobbies      = 0;
	int players      = 0;
};
LobbyAggregate aggregate(db::Database &db);

// Build a HostRow from the in-memory LobbyState the lobby session
// already maintains. Caller supplies peer addr (lobby_session has
// remote_ip / remote_port already; this is just the field-by-field
// copy). No DB access here.
HostRow row_from_lobby(const LobbyState &lobby,
                       const std::string &peer_ip, int peer_port,
                       std::optional<int64_t> host_user_id = std::nullopt);

} // namespace hostdb

} // namespace opennova
