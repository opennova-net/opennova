#pragma once

#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/lobby_session.h>      // LobbyState, HostRosterSlot
#include <net/novaworld/lobby_update.h>       // LobbyStatusBlob

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// Thin SQL wrapper around the `active_hosts`, `host_players`, `host_roster`
// and `host_keys` tables. All methods take a Database& and call into the same
// prepared-statement path as the rest of engine/net/novaworld. The handle is
// the calling thread's own connection (the standalone server leases one per
// thread from a db::ConnectionPool); a write that spans several statements
// (clear_all, replace_roster, apply_status_blob) is one db::Transaction on it,
// so readers on other connections see it whole. A caller that writes a host
// row and its roster puts both in one Transaction (replace_roster's own then
// nests as a savepoint), so the row's player count and its roster agree; a
// read that spans the two takes a db::ReadSnapshot.
//
// Lifecycle (the lobby-session dispatch):
//   ClientHostRequest  -> upsert_host() (insert, or update in place) + replace_roster()
//   ClientHostUpdate   -> update_host() (refresh the Host columns) + replace_roster()
//   POST status blob   -> apply_status_blob() (the Host columns, keyed by HostKey)
//   GOODBYE / timeout  -> remove_host_by_rid() (lobby_session's teardown)
//   ClientHostPlayerAdded   -> add_player()
//   ClientHostPlayerRemoved -> remove_player_by_peer() (a peer can leave
//     at most one host at a time so the unique-by-peer index is enough)
//   server boot        -> clear_all() (drop stale rows from prev run)
//
// The listing's owner (host_keys, migration 0009; service policy, the retail
// service's own check is unwitnessed): /NWHost.dll records each HOSTKEY it
// mints with the account the request's retail login names (record_host_key),
// the lobby session claims the key a connection presents (its first
// ClientHostUpdate's, or a re-host's ClientHostRequest's) for its RID
// (claim_host_key) and the row carries the account as host_user_id; the main
// tick's sweep expires idle keys (prune_host_keys).
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
	// The remaining retail Host columns (see LobbyState).
	std::string time_left;
	std::string time_of_day;
	std::string msg;
	std::string mod;
	std::string age;
	std::string pb_server;
	std::string level_range;
	std::string bb_mode;
	std::string skins;
	std::string tracers;
	std::string pix;
	// The account that hosted it (claim_host_key), unset for a listing whose
	// HostKey names no account. A write stores NULL for an account deleted
	// since, so a live listing's refresh never trips the players key.
	std::optional<int64_t> host_user_id;
	std::string peer_ip;
	int         peer_port = 0;
	// Read back only (the row's own timestamps; a write leaves created_at and
	// sets updated_at itself): UTC 'YYYY-MM-DD HH:MM:SS'.
	std::string created_at;
	std::string updated_at;
};

struct PlayerRow {
	uint32_t    host_rid = 0;
	std::optional<int64_t> user_id;
	std::string nwhandle;
	std::string peer_ip;
	int         peer_port = 0;
};

// Every active_hosts / host_players / host_roster row, and every host_keys
// claim: the RIDs they name were the previous run's.
void clear_all(db::Database &db);

void upsert_host(db::Database &db, const HostRow &row);
void update_host(db::Database &db, const HostRow &row);
void remove_host_by_rid(db::Database &db, uint32_t rid);

// Backstop sweep: delete active_hosts whose updated_at is older than
// `window_seconds` (crash orphans the normal teardown missed). Returns the
// number of rows removed. host_players cascade. policy, not wire-witnessed.
int prune_stale_hosts(db::Database &db, int64_t window_seconds);

void add_player(db::Database &db, const PlayerRow &row);
void remove_player_by_peer(db::Database &db, const std::string &peer_ip, int peer_port);

// The host-reported PlayerList (one row per slot). Replaced wholesale on
// every ClientHostRequest / ClientHostUpdate; cascades with the host row.
void replace_roster(db::Database &db, uint32_t host_rid,
                    const std::vector<HostRosterSlot> &roster);
std::vector<HostRosterSlot> list_roster(db::Database &db, uint32_t host_rid);

// Fold a parsed POST status blob (the plaintext Lobby_UpdateServerInfo
// heartbeat) into the host row that owns its HostKey. Returns false when no
// active host carries that key (the blob is then ignored). The blob's values
// are lobby-sanitized on the wire (' ', '?', '@', '=' -> '+', empty -> "---")
// and are stored as received.
bool apply_status_blob(db::Database &db, const LobbyStatusBlob &blob);

std::vector<HostRow>   list_hosts(db::Database &db);
// Hosts for one game slug (active_hosts.game == game) so /jop_2.gsb and
// /dfx2_0.gsb don't cross-contaminate. Uses idx_active_hosts_game.
std::vector<HostRow>   list_hosts_by_game(db::Database &db, const std::string &game);
// The listings one account hosted (host_user_id), oldest first.
std::vector<HostRow>   list_hosts_by_owner(db::Database &db, int64_t user_id);
std::optional<HostRow> find_host_by_rid(db::Database &db, uint32_t rid);
std::optional<HostRow> find_host_by_key(db::Database &db, const std::string &host_key);
std::vector<PlayerRow> list_players(db::Database &db, uint32_t host_rid);

// Aggregates used by /api/stats and /api/lobbies.
struct LobbyAggregate {
	int games        = 0;
	int lobbies      = 0;
	int players      = 0;
};
LobbyAggregate aggregate(db::Database &db);

// Build a HostRow from the in-memory LobbyState the lobby session
// already maintains, its owner (LobbyState::host_user_id) included. Caller
// supplies peer addr (lobby_session has remote_ip / remote_port already; this
// is just the field-by-field copy). No DB access here.
HostRow row_from_lobby(const LobbyState &lobby,
                       const std::string &peer_ip, int peer_port);

// --- The hosting keys (host_keys) ---

// The HOSTKEY /NWHost.dll mints: 48 letters A..P, 24 random bytes a nibble
// each. Nothing else is ever recorded or looked up.
inline constexpr std::size_t kHostKeyChars = 48;
bool is_host_key(std::string_view key);
// The stored form of a key: its SHA-256 as 64 lower-case hex digits.
std::string host_key_hash(std::string_view key);
// A key lasts this long from its last use: its claim, or the sweep while a
// listed server holds it (prune_host_keys).
inline constexpr int64_t kHostKeyIdleSeconds = 24 * 60 * 60;
// The sweep counts a held key used at most this often, so a listed server's
// key never ages out and the sweep writes it once an hour, not every pass.
inline constexpr int64_t kHostKeyTouchSeconds = 60 * 60;
// The keys one account keeps: a mint past them deletes the oldest, but never a
// key a listed server holds.
inline constexpr std::size_t kHostKeysPerUser = 20;

// Records a key /NWHost.dll minted for `user_id` (its hash), and trims the
// account's keys to the newest kHostKeysPerUser and those a listed server
// holds, in one transaction. A key that is no HOSTKEY records nothing. Throws
// db::SqliteError.
void record_host_key(db::Database &db, std::string_view key, int64_t user_id);

// A key a connection presents, claimed for the connection's RID: the account it
// was minted for, or nullopt for a key that is no HOSTKEY, unknown, idle past
// kHostKeyIdleSeconds, minted for an account that is no longer active, or
// claimed by another RID that is still listed (a key backs one listing at a
// time; a parked connection's re-host keeps its RID and so its claim, and a key
// whose listing is gone can be claimed again). A claim moves last_used_at
// forward. Throws db::SqliteError.
std::optional<int64_t> claim_host_key(db::Database &db, std::string_view key, uint32_t rid);

// The main tick's sweep: every key a listed server holds counts as used (at
// most once per kHostKeyTouchSeconds), then every key unused for
// kHostKeyIdleSeconds goes. Returns how many went. Throws db::SqliteError.
std::size_t prune_host_keys(db::Database &db);

// Project one stored host (plus its roster) onto the GSB browser row the
// retail IB3 browser reads: every FLDS column carries the host-reported
// value, and the row tail carries the roster's PlayerName list, whose u16
// count the browser adds to its player total
// [orig: NapiGameList_ProcessEncryptedResponse @0x63dafc..0x63db96].
GsbServerEntry gsb_entry_from_host(const HostRow &host,
                                   const std::vector<HostRosterSlot> &roster);

} // namespace hostdb

} // namespace opennova
