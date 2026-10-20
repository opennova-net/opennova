#pragma once

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova {

// Thin SQL wrapper around the lobby's tables (backend/migrations/0006_lobby_commands.sql,
// 0009_lobby_instances.sql, 0012_lobby_instances_share_endpoint.sql,
// 0020_lobby_live_view.sql, 0022_lobby_goodbyes.sql): `lobby_instances`, the
// lobbies that run (one per lobby process, each its own id, all announcing the deployment's one
// lobby address), `lobby_sessions`, the NovaWorld sessions each lobby holds, which
// that lobby alone writes and everything else reads, `lobby_goodbyes`, the goodbyes a lobby's
// clean stop leaves for the sessions it held, which the lobby their next packets reach sends,
// and `lobby_commands`, what the HTTP routes
// and the hosted servers' supervisor address to the lobby that holds a session, which that lobby
// alone takes and runs. A lobby is a NovaWorld UDP listener (apps/novaworld_lobby/
// nw_udp_listener.h) in a lobby process of its own; nothing else reaches its memory, so a route
// that would read its sessions or push to one goes through these tables. Service policy, not a
// port. Every function takes the calling thread's own connection and throws db::Error.
//
// The queue_* functions answer from the lobby's session rows what the lobby itself would have
// answered (a RID no session holds, one whose session does not host, a session whose address
// changed, one that hosts), and insert the command only when the lobby would take it; the lobby
// checks again as it runs it (lobby_commands' comment).
namespace lobbydb {

// --- lobby_instances --------------------------------------------------------

// A lobby that runs (lobby_instances' comment). The times are Unix epoch ms.
struct LobbyInstance {
	int64_t id = 0;
	std::string endpoint; // host:port, the address the gates hand clients (every instance's)
	std::string state; // "active" or "draining"
	// The task its lobby runs as (its program_samples rows' task_id); "" for none.
	std::string task_id;
	int64_t started_ms = 0;
	int64_t last_seen_ms = 0;
	int64_t seen_ago_ms = 0; // now less last_seen_at, by the database's clock
};

// A running lobby moves its row's last_seen_at every kInstanceTouchSeconds. One seen within
// kInstanceLiveSeconds runs: while one active instance does, the gates hand clients the lobby
// address. One not seen for kInstanceStaleSeconds ran on a process that is gone: a lobby's start
// and the hosts.prune job delete it with its rows. The stale window stands well past a database
// failover (RDS Multi-AZ: 60 to 120 s, when no lobby can move its row), the listings' own prune
// window (hostdb::prune_stale_hosts, 300 s); a running lobby whose row was reaped all the same
// registers again and writes its rows anew. A gate reads the live instances again every
// kGateRefreshSeconds.
inline constexpr int64_t kInstanceTouchSeconds = 5;
inline constexpr int64_t kInstanceLiveSeconds = 15;
inline constexpr int64_t kInstanceStaleSeconds = 300;
inline constexpr int64_t kGateRefreshSeconds = 5;

// A lobby starts, or registers again: a row of its own (a new id: instances share their
// endpoint, the deployment's one lobby address), active (draining when `draining`: a running
// lobby registering again keeps the drain its row had), started and seen now, naming the task
// it runs as (`task_id`, program_usage.h find_usage_task). Its id.
int64_t register_instance(db::Database &db, const std::string &endpoint, bool draining = false,
		const std::string &task_id = "");
// Its row's last_seen_at now, and the row's state ("active" or "draining": an admin's drain
// goes through the row); nullopt when the row is gone (reaped as stale while this lobby could
// not reach the database, its rows with it: the lobby registers again).
std::optional<std::string> touch_instance(db::Database &db, int64_t id);
// "active" or "draining"; false for no such instance (or another state).
bool set_instance_state(db::Database &db, int64_t id, const std::string &state);
// The lobby's clean stop: its row and, with it, every row that names it.
void remove_instance(db::Database &db, int64_t id);
// Every instance not seen for `max_age_seconds` but `keep`: deleted, their rows with them. How
// many went.
std::size_t reap_stale_instances(db::Database &db, int64_t max_age_seconds, int64_t keep = 0);
// Every instance, by id.
std::vector<LobbyInstance> list_instances(db::Database &db);

// What a gate hands clients (gate_listener.h): the instances that take clients (active, seen
// within kInstanceLiveSeconds) all announce the deployment's one lobby address, the load
// balancer's that spreads clients across them, so the newest one's endpoint is that address;
// and how many there are. endpoint "" and instances 0 with none.
struct LiveLobbies {
	std::string endpoint;
	std::size_t instances = 0;
};
LiveLobbies live_lobbies(db::Database &db);

// The next host RID (lobby_rid_seq): 0x0A000000 and a suffix of 1..0xFFFFFF, the same sequence
// for every instance, so no two mint one RID. The sequence cycles: a suffix a listing
// (active_hosts) or a session row still holds is passed over for the next, so a RID minted
// after the wrap never lands on a live one. 0 when every suffix tried (kRidAttempts) is held.
inline constexpr int kRidAttempts = 1024;
uint32_t next_rid(db::Database &db);

// --- lobby_sessions ---------------------------------------------------------

// One session row (lobby_sessions' comment names each column). The times are Unix epoch ms.
struct SessionRow {
	int64_t id = 0; // the row's own; insert_session sets it
	int64_t lobby_id = 0; // the instance that holds it (0: none, NULL)
	uint32_t connection_id = 0;
	std::string peer_ip;
	int peer_port = 0;
	std::string state; // "handshaking", "active" or "closing"
	std::string pn;
	std::string identity;
	// The account it signed in as (LobbyState::user_id); unset for none.
	std::optional<int64_t> user_id;
	uint32_t rid = 0; // 0: none
	bool hosting = false;
	bool listed = false;
	std::optional<int64_t> host_user_id;
	std::string server_name;
	std::string mission_name;
	int players = 0;
	int max_players = 0;
	// What it plays on (LobbyState's play columns): the listing at its PlaySetup's address
	// (0: none), that address ("" and 0 while it plays on nothing), and since when.
	uint32_t playing_rid = 0;
	std::string play_addr;
	int64_t playing_since_ms = 0;
	int64_t connected_ms = 0;
	int64_t last_seen_ms = 0;
};

// The session's address as the admin's list shows it ("a.b.c.d:port").
std::string session_addr(const SessionRow &row);

// Whether `a` and `b` agree in every column update_session writes (all but the id, the lobby and
// the two times): the lobby writes a row again only when it changed.
bool same_columns(const SessionRow &a, const SessionRow &b);

// Inserts `row` (its id ignored) and returns the new row's id.
int64_t insert_session(db::Database &db, const SessionRow &row);
// Writes every column of row `row.id` but its lobby and its times (connected_at stays the
// insert's; touch_sessions moves last_seen_at). A row deleted since writes nothing.
void update_session(db::Database &db, const SessionRow &row);
// Writes only the listing columns of row `id` (rid, hosting, listed, host_user_id, server_name,
// mission_name, players, max_players). persist_host calls it inside the listing row's own
// transaction, so a reader that sees a listing's row (its owner, its RID) sees the session row
// that says the same: the push routes answer from the session row right after a test, or an
// owner, reads the listing.
void update_session_listing(db::Database &db, int64_t id, uint32_t rid, bool hosting, bool listed,
		std::optional<int64_t> host_user_id, const std::string &server_name,
		const std::string &mission_name, int players, int max_players);
void delete_session(db::Database &db, int64_t id);
// Instance `lobby_id`'s session rows but those `keep` names: the lobby's sweep of the rows it
// holds no session for (the NW UDP listener's, every kSessionSeenWriteMs). A drop forgets its
// session before it deletes the row, so a delete that failed would leave a row that answers for
// a RID no session holds (a push to it queued, then dropped by the lobby). How many went; the
// commands addressed to them go with them. Another instance's rows stay; 0 sweeps none.
std::size_t delete_sessions_except(
		db::Database &db, int64_t lobby_id, const std::vector<int64_t> &keep);

// last_seen_at of each row named, in one statement.
struct SessionSeen {
	int64_t id = 0;
	int64_t last_seen_ms = 0;
};
void touch_sessions(db::Database &db, const std::vector<SessionSeen> &seen);

// Every session row, oldest first, of every instance.
std::vector<SessionRow> list_sessions(db::Database &db);

// The account whose live retail login set `session_tag` (a Cookie list's LOGINSESSIONTAG): its
// active_user_sessions row, while the account is still at the credentials_gen the row was made
// under, as the HTTP routes read a login (accounts/auth.h get_user_by_login_session_tag). nullopt
// for an empty or unknown tag. The lobby pins the session's account with it (LobbyState::user_id).
std::optional<int64_t> login_account(db::Database &db, const std::string &session_tag);

// --- lobby_goodbyes ---------------------------------------------------------

// The ServerGoodBye a lobby's clean stop leaves for one session it held (lobby_goodbyes'
// comment): the session's address ("a.b.c.d" and its port), the key its packets address (the SK
// the lobby minted) and the key the goodbye bears (the client's CK).
struct Goodbye {
	std::string peer_ip;
	int peer_port = 0;
	uint32_t server_sk = 0;
	uint32_t client_ck = 0;
};

// A stopping lobby's goodbyes (each address once), in one statement, each replacing any row at its
// address; first, the rows left more than `max_age_seconds` ago go.
void leave_goodbyes(
		db::Database &db, const std::vector<Goodbye> &goodbyes, int64_t max_age_seconds);
// The goodbyes left within the last `max_age_seconds`, by address.
std::vector<Goodbye> list_goodbyes(db::Database &db, int64_t max_age_seconds);

// --- lobby_commands ---------------------------------------------------------

// How long a command waits for its lobby: one the lobby takes later than this is dropped unrun
// (it was not taking commands then, and a push or an end that late would surprise whoever asked).
inline constexpr int64_t kCommandTtlSeconds = 30;

// What queue_server_command / queue_stop_hosting did with a statement.
enum class PushResult : uint8_t {
	Queued,     // inserted: the lobby holding the session sends it on its next drain
	UnknownRid, // no session holds the RID (none the owner's listing, with an owner)
	NotHosted,  // the session holding it is not hosting (it stopped, or awaits its re-host)
};
// "queued", "unknown_rid", "not_hosting": the routes' status and error codes.
const char *push_result_name(PushResult result);

// The service's statements to a listed server, over that server's own NovaWorld session (the
// ServerCommand admin channel; docs/net/novaworld-net-re.md "the service side"): the RID
// resolves to the session that holds it, hosting, and the statement is inserted for that
// session's lobby. `owner` scopes a push to the listing's owner (the owner's routes, the
// supervisor's stop; service policy): set, the RID resolves only to a session whose listing that
// account owns (lobby_sessions.host_user_id), and the lobby checks the owner again as the
// statement goes out, in its memory and by the live claim (hostdb::listing_owned_by). A session
// another account owns, or none, is then UnknownRid, the answer for a RID no session holds.
// A RID held by a hosting session and by one that is not (none, as the lobby keeps it) resolves
// to the hosting one.
// ServerCommand: one Cmd param carrying `cmd` verbatim (compose it with server_command_text).
PushResult queue_server_command(db::Database &db, uint32_t rid, const std::string &cmd,
                                std::optional<int64_t> owner = std::nullopt);
// ServerStopHosting: MsgCode / MsgParam1 / MsgParam2, the sysop punt from the host's side; once
// its record goes out the session also leaves the browser.
PushResult queue_stop_hosting(db::Database &db, uint32_t rid, int msg_code, int msg_param1 = 0,
                              int msg_param2 = 0, std::optional<int64_t> owner = std::nullopt);

// The admin's end of one session, named by its row's id (lobby_sessions.id: unique across every
// instance and never reused, where a connection id and an address are unique within one lobby
// alone). Inserted only for a session that does not host.
struct EndSession {
	enum class Result : uint8_t {
		Queued, // inserted: the lobby holding it ends it on its next drain
		NotFound, // no session row has the id (it ended)
		Hosting, // it hosts: its listing's stop ends it (`rid`)
	};
	Result result = Result::NotFound;
	SessionRow session; // the session found (Queued, Hosting)
};
const char *end_session_result_name(EndSession::Result result);
EndSession queue_end_session(db::Database &db, int64_t session_id);

// `user_id` lost its keys (an admin password reset, a status other than 'active', a delete, a
// reset by link): every lobby takes it off every session and parked reconnect that names it as
// its listing's owner, as its rows: one command per instance (active or draining), each bearing
// that instance's id. The keys' delete already stops every push of the account's (the routes and
// the lobby read the live claim); this keeps the lobbies' own view, and the admin's sessions view
// from it, in step.
void queue_forget_owner(db::Database &db, int64_t user_id);

enum class CommandKind : uint8_t { ServerCommand, StopHosting, EndSession, ForgetOwner };
// The kind as lobby_commands.kind names it ("server_command", ...).
const char *command_kind_name(CommandKind kind);

// One command as the lobby takes it.
struct Command {
	int64_t id = 0;
	CommandKind kind = CommandKind::ServerCommand;
	int64_t session_id = 0;          // 0 for ForgetOwner
	uint32_t rid = 0;                // the pushes'
	std::optional<int64_t> account;  // a push's owner scope; ForgetOwner's account
	std::string cmd;                 // ServerCommand's Cmd line
	int msg_code = 0;                // StopHosting's
	int msg_param1 = 0;
	int msg_param2 = 0;
	bool expired = false;            // past its expires_at: dropped unrun
};

// Takes the oldest `limit` commands addressed to instance `lobby_id`: deletes them and returns
// them in insert order (the lobby runs each once, best effort; one it cannot run is gone all the
// same). Another instance's are never taken, and a lobby with no instance (0) takes none.
std::vector<Command> take_commands(db::Database &db, int64_t lobby_id, std::size_t limit);

} // namespace lobbydb

} // namespace opennova
