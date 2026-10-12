#pragma once

// The hosted servers' rows (hosted_servers, hosted_server_events; migration
// 0011): the requests the site makes, the state each server is in, every
// transition with what caused it, and which listing is a hosted server's. The
// supervisor (supervisor.h) and the routes share it; every write that spans
// statements is one db::Transaction, and a transition is a compare-and-set on
// the state, so the supervisor and a route that both move a server never both
// win. Crow-free. Each function throws db::SqliteError on a database failure.

#include "hosted_settings.h"

#include <net/novaworld/db/sqlite.h>

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
	Provisioning, // the service account and the files are made, the machine launched
	Booting,      // the machine is starting opennova-serve
	Listing,      // opennova-serve runs; its listing has not appeared yet
	Running,      // listed on NovaWorld by its service account
	Stopping,     // a stop was asked for; the machine is being ended
	Stopped,      // ended by a stop (or a clean exit)
	Failed,       // ended by a fault (failure_reason)
};
const char *state_name(State state);
std::optional<State> state_from_name(std::string_view name);
// Any state but Stopped and Failed: the server holds (or is about to hold) a
// machine, counts against the cap and is its owner's one live server.
bool is_live(State state);

struct HostedServer {
	int64_t id = 0;
	std::optional<int64_t> owner_user_id;
	std::optional<int64_t> service_user_id;
	State state = State::Requested;
	HostedSettings settings;
	std::string provider;
	std::string machine_id;
	std::string machine_address;
	int admin_port = 0;
	std::string admin_user;
	std::string admin_password;
	int restarts = 0;
	std::string failure_reason;
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

// What a start request came to.
enum class RequestOutcome : uint8_t {
	Created,
	NoSuchUser,  // no player account by that id
	NotApproved, // the account is not approved (can_host) or not active
	AlreadyLive, // the account has a live server (`id` names it)
	AtCapacity,  // `cap` servers are live
};
struct RequestResult {
	RequestOutcome outcome = RequestOutcome::Created;
	int64_t id = 0;
};

// A start the owner asks for, with settings check_settings accepted: in one
// transaction the account's approval, its one live server and the global cap
// are checked and the row is written (state requested, its event beside it).
RequestResult request_server(db::Database &db, int64_t owner_user_id,
		const HostedSettings &settings, int cap);

std::optional<HostedServer> find_server(db::Database &db, int64_t id);
// The owner's live server, else the newest of its ended ones.
std::optional<HostedServer> latest_server_of(db::Database &db, int64_t owner_user_id);
std::vector<HostedServer> list_live(db::Database &db);
// The newest `limit` servers, live ones first (the admin's fleet view).
std::vector<HostedServer> list_servers(db::Database &db, size_t limit);
int count_live(db::Database &db);
// The newest `limit` transitions of a server, oldest first.
std::vector<HostedEvent> list_events(db::Database &db, int64_t id, size_t limit);

// The supervisor's move of a server from `from` to `to`, with its event
// (`detail`), when the row is still in `from`; false (nothing written)
// otherwise. Running sets running_at; Stopped and Failed set ended_at, and
// Failed the failure reason (`detail`). `count_restart` adds one to restarts.
// `clear_machine` empties the machine's id and address (a crash restart's
// relaunch, an end).
struct TransitionOptions {
	bool count_restart = false;
	bool clear_machine = false;
};
bool transition(db::Database &db, int64_t id, State from, State to, const std::string &detail,
		TransitionOptions options = {});
// A stop an owner or an admin asks for: a live server not already stopping
// goes to stopping, with its event. False when there was nothing to stop.
bool request_stop(db::Database &db, int64_t id, const std::string &detail);
// Every live server's stop (the admin's kill switch); how many it asked.
size_t request_stop_all(db::Database &db, const std::string &detail);

// The fields a start writes beside the state.
void set_service_account(db::Database &db, int64_t id, std::optional<int64_t> user_id);
void set_machine(db::Database &db, int64_t id, const std::string &provider,
		const std::string &machine_id, const std::string &address);
void set_admin_login(db::Database &db, int64_t id, int admin_port, const std::string &user,
		const std::string &password);

// The RID of the listing the account `account_user_id` holds now: listed, with
// one of its HostKeys claimed for the RID (hostdb::listing_owned_by's rule).
// The newest when it holds several; nullopt for none.
std::optional<uint32_t> listing_rid_of(db::Database &db, int64_t account_user_id);

// --- A hosted listing's owner: the one rule (ADR 0053 d4, proposed) ---
//
// A listing belongs to the account whose HostKey it holds now
// (hostdb::listing_owned_by), except that a hosted server's service account
// stands for that hosted server's owner. Every owner-facing view and push
// resolves a listing through the four functions below (host_routes.cpp: GET
// /api/me/hosts, POST /api/me/hosts/<rid>/command and /stop, GET
// /api/admin/hosts and its stop), so the rule lives here alone. Service
// policy, like the HostKey claim it extends. Reads only: safe inside a
// db::ReadSnapshot.

// The hosted server whose service account `account_user_id` is.
struct HostedAccount {
	int64_t server_id = 0;
	std::optional<int64_t> owner_user_id; // unset once the owner's account is gone
};
std::optional<HostedAccount> hosted_server_of_account(db::Database &db, int64_t account_user_id);
// The site account that owns what `account_user_id` lists: a hosted server's
// owner for its service account (nullopt once that owner is gone), else the
// account itself.
std::optional<int64_t> listing_owner_of(db::Database &db, int64_t account_user_id);
// The accounts whose listings are `user_id`'s: itself first, then the service
// accounts of its hosted servers.
std::vector<int64_t> listing_accounts_of(db::Database &db, int64_t user_id);
// The account through which `user_id` owns the listing under `rid` now: itself
// or one of its hosted servers' service accounts, whichever holds the listing's
// claim; nullopt when the listing is not `user_id`'s (or not listed).
std::optional<int64_t> owned_listing_account(db::Database &db, uint32_t rid, int64_t user_id);

} // namespace opennova::novaworld_server::hosted
