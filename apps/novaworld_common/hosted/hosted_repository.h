#pragma once

// The hosted servers' rows (hosted_servers, hosted_server_events; the
// baseline, and 0003_fleet.sql's place in the fleet): the requests the site
// makes, the state each server is in, every transition with what caused it,
// and which listing is a hosted server's. The supervisor (supervisor.h) and the
// routes share it; every write that spans statements is one db::Transaction,
// and a transition is a compare-and-set on the state, so the supervisor and a
// route that both move a server never both win. The slot a server holds is
// fleet_repository.h's (assign_slot, release_slot). Crow-free. Each function
// throws db::Error on a database failure.

#include "hosted_settings.h"
#include "placement.h"

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// requested -> provisioning -> booting -> listing -> running -> stopping ->
// stopped | failed; a crash restart goes back to provisioning, and a stop
// request takes any live state to stopping.
enum class State : uint8_t {
	Requested,    // the owner asked; the supervisor has not picked it up
	Provisioning, // placed on a slot of a machine (or waiting for one), its files rendered
	Booting,      // its container is wanted on its slot; the machine's agent starts it
	Listing,      // opennova-serve runs; its listing has not appeared yet
	Running,      // listed on NovaWorld by its service account
	Stopping,     // a stop was asked for; its container is being stopped
	Stopped,      // ended by a stop (or a clean exit)
	Failed,       // ended by a fault (failure_reason)
};
const char *state_name(State state);
std::optional<State> state_from_name(std::string_view name);
// Any state but Stopped and Failed: the server holds (or is about to hold) a
// machine, and counts against the cap and against its owner's hosting
// entitlement (a stopping one included, until it has stopped).
bool is_live(State state);
// is_live as SQL on hosted_servers' state, for every query of the table
// (here and fleet_repository.h's).
inline constexpr const char *kLiveServerSql = "state NOT IN ('stopped', 'failed')";
// The crash restarts a server has within its restart window (the supervisor's
// restart_limit, and the owner's "restart n of 3").
inline constexpr int kRestartLimit = 3;

// A hosted server's row. It carries no admin credential: that is AdminLogin's,
// read by admin_login_of alone, so nothing that serializes a HostedServer can
// emit one.
struct HostedServer {
	int64_t id = 0;
	std::optional<int64_t> owner_user_id;
	std::optional<int64_t> service_user_id;
	State state = State::Requested;
	HostedSettings settings;
	// The machine driver that ran its last start, its container's name
	// (srv-<id>-g<generation>) and the machine's private address (the admin
	// link's target); empty until a start renders them, and cleared when it
	// leaves its slot.
	std::string provider;
	std::string container;
	std::string machine_address;
	int admin_port = 0;
	int restarts = 0;
	std::string failure_reason;
	// It runs on a machine of its own (the own-machine add-on), not a shared one.
	bool own_machine = false;
	// The static address its owner asked it to run at (address_repository.h;
	// an own-machine server's), kept through its restarts and moves; unset for
	// none, and once the address lapses.
	std::optional<int64_t> address_id;
	// The trees it is pinned to (data_set_repository.h, ADR 0064): the base data
	// set's and, with an expansion, the expansion's, taken from the data sets'
	// current trees at its first start and kept through crash restarts and moves
	// until its owner asks for an update (update_requested: the supervisor starts
	// it again on the current trees, then clears the flag). Unset before its
	// first start on this schema.
	std::optional<int64_t> base_tree_id;
	std::optional<int64_t> expansion_tree_id;
	bool update_requested = false;
	// Its place in the fleet (0003_fleet.sql): the fleet_machines row and the
	// slot it holds while live (an ended row keeps where it last ran), the game
	// port its files were rendered with, how many starts it has had (its
	// container's generation) and the serve image its start pinned.
	std::optional<int64_t> machine_ref;
	int slot = -1;
	int game_port = 0;
	int64_t generation = 0;
	std::string serve_image;
	// UTC 'YYYY-MM-DD HH:MM:SS'; running_at / ended_at empty until set.
	std::string created_at;
	std::string updated_at;
	std::string running_at;
	std::string ended_at;
};

struct HostedEvent {
	int64_t id = 0;
	std::string from_state; // "" for the request itself
	std::string to_state;
	std::string detail;
	std::string at;
};

// The remote-admin login the rendered admin.cfg grants the service (K's admin
// link logs in with it). Never part of a reply.
struct AdminLogin {
	int port = 0;
	std::string user;
	std::string password;
};
// The server's admin login; nullopt for no such server or one not yet rendered.
std::optional<AdminLogin> admin_login_of(db::Database &db, int64_t id);

// What a start request came to.
enum class RequestOutcome : uint8_t {
	Created,
	InvalidSettings, // check_settings refused them (the routes check first)
	NoSuchUser,   // no player account by that id
	NotEntitled,  // the account is not active, or its hosting entitlement is 0 servers
	PastDue,      // its support tier's payment is past due: its servers run on, none starts
	AtLimit,      // `used` of its `max_servers` are live already
	NoOwnMachine, // an own machine was asked for, and each the add-on pays for runs one
	AtCapacity,   // `cap` servers are live, or the fleet has no place for it (fleet_has_room)
	AddressUnavailable, // the address asked for is not the owner's, not ready, or another live server's
};
struct RequestResult {
	RequestOutcome outcome = RequestOutcome::Created;
	int64_t id = 0;
	// AtLimit: the entitlement's servers, and how many of the account's are live.
	int max_servers = 0;
	int used = 0;
};

// A start the owner asks for: the settings checked again against the offered
// data sets (check_settings over offered_data_sets, as the routes do first), then in one transaction
// (which holds the hosted-start lock, database.h kHostedStartLock, so no other
// start lands between a count and the row) the account (a player's, active),
// its hosting entitlement at `now` (Unix seconds; billing/entitlement.h): some
// servers, none past due, fewer of its servers live than it grants (a stopping
// one counts), and with `own_machine` fewer of them on own machines than the
// add-on pays for; then the global cap, and with `fleet` the fleet's room under
// that placement policy (the supervisor's planner, fleet_repository.h
// fleet_has_room); with `address_id` (an own-machine server's), that address
// the owner's, ready and no other live server's; and the row written (state
// requested, its event beside it).
RequestResult request_server(db::Database &db, int64_t owner_user_id, const HostedSettings &settings,
		const std::vector<OfferedDataSet> &offered, int cap, int64_t now, bool own_machine = false,
		const PlacementPolicy *fleet = nullptr, std::optional<int64_t> address_id = std::nullopt);
// The live server's address let go (it lapsed: the supervisor places the
// server again without it); false when it held none.
bool clear_server_address(db::Database &db, int64_t id);

std::optional<HostedServer> find_server(db::Database &db, int64_t id);
// Server `id` when `owner_user_id` owns it, in any state; nullopt for no such
// server and for another account's (the owner's routes answer both 404).
std::optional<HostedServer> owned_server(db::Database &db, int64_t id, int64_t owner_user_id);
// The owner's live servers by id, oldest first.
std::vector<HostedServer> live_servers_of(db::Database &db, int64_t owner_user_id);
// The owner's live servers by id, oldest first, then its newest
// `ended_limit` ended ones, newest first.
std::vector<HostedServer> list_servers_of(db::Database &db, int64_t owner_user_id, size_t ended_limit = 10);
// How many of the owner's servers are live, and how many of those run on own
// machines (a stopping one counts in both).
int count_live_of(db::Database &db, int64_t owner_user_id);
int count_own_live_of(db::Database &db, int64_t owner_user_id);
std::vector<HostedServer> list_live(db::Database &db);
// The newest `limit` servers, live ones first (the admin's fleet view).
std::vector<HostedServer> list_servers(db::Database &db, size_t limit);
// Which servers the admin's fleet view lists.
enum class ServerFilter : uint8_t { Live, Ended, All };
// The newest `limit` servers `filter` takes, live ones first, of `owner_user_id`
// alone when it is set.
std::vector<HostedServer> list_fleet_servers(
		db::Database &db, std::optional<int64_t> owner_user_id, ServerFilter filter, size_t limit);
// How many servers are live, and how many of those run on own machines (a
// stopping one counts in both).
int count_live(db::Database &db);
int count_own_live(db::Database &db);
// The servers that ended failed in the last `seconds` (the heartbeat's
// hosted_failed).
int count_failed_since(db::Database &db, int64_t seconds);
// The newest `limit` transitions of a server, oldest first.
std::vector<HostedEvent> list_events(db::Database &db, int64_t id, size_t limit);

// The supervisor's move of a server from `from` to `to`, with its event
// (`detail`), when the row is still in `from`; false (nothing written)
// otherwise. Running sets running_at; Stopped and Failed set ended_at, and
// Failed the failure reason (`detail`). `count_restart` adds one to restarts.
// `clear_machine` empties the machine's id and address (a crash restart's
// relaunch, an end). `end_service_account` deletes the server's service
// account in the same transaction (an end: its sessions and keys cascade, and
// the row's service_user_id goes NULL), so no ended row keeps an account.
struct TransitionOptions {
	bool count_restart = false;
	bool clear_machine = false;
	bool end_service_account = false;
};
bool transition(db::Database &db, int64_t id, State from, State to, const std::string &detail,
		TransitionOptions options = {});
// A stop an owner or an admin asks for: a live server not already stopping
// goes to stopping, with its event. False when there was nothing to stop.
bool request_stop(db::Database &db, int64_t id, const std::string &detail);
// Every live server's stop (the admin's kill switch); how many it asked.
size_t request_stop_all(db::Database &db, const std::string &detail);
// A running server's restarts back to 0 (its healthy run outlasted the
// supervisor's restart window); false when it was not running or had none.
bool reset_restarts(db::Database &db, int64_t id);

// The fields a start writes beside the state.
void set_service_account(db::Database &db, int64_t id, std::optional<int64_t> user_id);
void set_machine(db::Database &db, int64_t id, const std::string &provider,
		const std::string &container, const std::string &address);
void set_serve_image(db::Database &db, int64_t id, const std::string &image);
void set_admin_login(db::Database &db, int64_t id, int admin_port, const std::string &user,
		const std::string &password);

// --- A server's starts (hosted_server_starts, 0016_fleet_observability.sql) ---
//
// A row is one owner's request; a generation is one of its runs: made with its
// slot (fleet_repository.h assign_slot), its image, trees and log stream written
// when it renders, running once its agent reports it so, and ended with its
// cause. The restarts the stats count are its crash and timeout ends.

// Why a start ended.
enum class StartCause : uint8_t {
	Crash, // the server exited, or never came up (its own cause)
	Fleet, // its machine's cause, or NovaWorld's: a move, no restart counted
	Stop, // an owner's, an admin's or the service's stop
	Update, // the owner's update to its data sets' current data
	Limits, // its owner's entitlement no longer covers it
	NoData, // its data set has none
	Timeout, // it did not start, or did not list, in time
	LinkLost, // its admin link stopped answering
};
const char *start_cause_text(StartCause cause); // "crash", "fleet", ... "link_lost"
std::optional<StartCause> parse_start_cause(std::string_view text);

struct ServerStart {
	int64_t server_id = 0;
	int64_t generation = 0;
	int64_t machine_id = 0;
	int slot = 0;
	std::string serve_image;
	std::optional<int64_t> base_tree_id;
	std::optional<int64_t> expansion_tree_id;
	std::string log_stream;
	// UTC 'YYYY-MM-DD HH:MM:SS'; running_at / ended_at empty until set.
	std::string placed_at;
	std::string running_at;
	std::string ended_at;
	std::optional<int64_t> exit_code;
	bool oom = false;
	std::string reason; // the server's exit line, or why it never started ("" for none)
	std::optional<StartCause> cause; // set at its end (limits: when its stop was asked)
};

// How a start's container ended, as its agent reported it.
struct StartExit {
	std::optional<int64_t> exit_code;
	bool oom = false;
	std::string reason;
};

// What start `generation` of server `server_id` runs, written when it renders.
void set_start_files(db::Database &db, int64_t server_id, int64_t generation,
		const std::string &serve_image, int64_t base_tree_id,
		std::optional<int64_t> expansion_tree_id, const std::string &log_stream);
// Its agent reports it running: running_at, once.
void mark_start_running(db::Database &db, int64_t server_id, int64_t generation);
// The start ended with `cause` (one already set, a stop's limits, stands) and
// how its container ended; false when it had ended already.
bool end_start(db::Database &db, int64_t server_id, int64_t generation, StartCause cause,
		const StartExit &exit = {});
// The cause server `server_id`'s open start will end with, set now unless one is
// (a stop for its owner's limits, asked before the stop ends it).
void set_start_cause(db::Database &db, int64_t server_id, StartCause cause);
// The server's newest `limit` starts, newest first.
std::vector<ServerStart> list_starts(db::Database &db, int64_t server_id, size_t limit);

// --- The remote-admin console (hosted_server_console) ---
//
// One line of a hosted server's console: a command the service's admin link
// (admin_link.h) sent on the server's retail remote-admin port, a reply packet
// the server answered with, or one of the link's own status lines. A capped
// ring per server: every append keeps the server's newest kConsoleKeep lines.

inline constexpr size_t kConsoleKeep = 500;
struct ConsoleLine {
	int64_t id = 0;
	std::string kind;  // "command", "reply" or "status"
	std::string actor; // a command's sender: "owner", "admin" or "service"; "" for the rest
	std::string text;  // UTF-8
	std::string at;    // UTC 'YYYY-MM-DD HH:MM:SS'
};
// Appends `lines` (their ids unread) in one transaction and trims the server's
// ring to its newest `keep`.
void append_console(db::Database &db, int64_t server_id, const std::vector<ConsoleLine> &lines,
		size_t keep = kConsoleKeep);
// The server's lines with an id past `after_id`, the newest `limit` of them,
// oldest first.
std::vector<ConsoleLine> list_console(db::Database &db, int64_t server_id, int64_t after_id, size_t limit);

// The RID of the listing the account `account_user_id` holds now: listed, with
// one of its HostKeys claimed for the RID (hostdb::listing_owned_by's rule).
// The newest when it holds several; nullopt for none.
std::optional<uint32_t> listing_rid_of(db::Database &db, int64_t account_user_id);

// --- A hosted listing's owner: the one rule (ADR 0053 d4, proposed) ---
//
// A listing belongs to the account whose HostKey it holds now
// (hostdb::listing_owned_by), except that a hosted server's service account
// stands for that hosted server's owner while the server is live. Every
// owner-facing view and push resolves a listing through the three functions
// below (host_routes.cpp: GET /api/me/hosts, POST /api/me/hosts/<rid>/command
// and /stop, GET /api/admin/hosts and its stop), so the rule lives here alone.
// Service policy, like the HostKey claim it extends. Reads only: safe inside a
// db::ReadSnapshot.

// The hosted server whose service account `account_user_id` is, in any state
// (a stopped or failed server's account is deleted with the transition that
// ends it, so an ended one shows only on a database written by hand).
struct HostedAccount {
	int64_t server_id = 0;
	std::optional<int64_t> owner_user_id; // unset once the owner's account is gone
	State state = State::Requested;
};
std::optional<HostedAccount> hosted_server_of_account(db::Database &db, int64_t account_user_id);
// The accounts whose listings are `user_id`'s: itself first, then the service
// accounts of its live hosted servers (is_live; a stopping one included).
std::vector<int64_t> listing_accounts_of(db::Database &db, int64_t user_id);
// The account through which `user_id` owns the listing under `rid` now: itself
// or one of its live hosted servers' service accounts, whichever holds the
// listing's claim; nullopt when the listing is not `user_id`'s (or not listed).
std::optional<int64_t> owned_listing_account(db::Database &db, uint32_t rid, int64_t user_id);

} // namespace opennova::novaworld_server::hosted
