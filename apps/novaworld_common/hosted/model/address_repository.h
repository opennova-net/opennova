#pragma once

// The static addresses (ADR 0066; hosted_addresses, 0017_capacity_addresses.sql):
// an own machine's fixed public IPv4 (an Elastic IP on EC2, a fixed address on
// the local stack's network) with an A record in the deployment's play zone,
// which an owner holds for its own-machine servers, so a server lists at the
// same address:port on every start. Each is its own Stripe subscription
// (billing ItemKind::StaticAddress, quantity 1; its source 'subscription') or
// an admin's grant (source 'comp'), and lives as a row:
//
//   allocating  named (the owner after the purchase, or the admin); the
//               worker's address.allocate (address_jobs.h) finds or
//               allocates it and writes its A record
//   ready       held, its name in DNS: a start of the owner's on an own
//               machine may name it (hosted_servers.address_id; one live
//               server per address)
//   lapsed      its grant is gone (the fleet's pass, with the brake) or an
//               admin released it: its server is placed again without it,
//               and once no machine holds it the pass makes it releasing
//   releasing   the worker's address.release: its record deleted and given
//               its TTL to drain, then the address released
//   released    gone for good; its name stays taken until an admin renames
//               it, and its subscription, granting still, may name another
//   failed      the cloud refused it (reason: its error code); allocating
//               again once an admin retries it, or, for a refusal that
//               passes (address_refusal_passes), once the fleet's pass finds
//               its retry_at reached
//
// Every move is a compare-and-set on the state. Crow-free. Each function throws
// db::Error on a database failure. Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

enum class AddressState : uint8_t { Allocating, Ready, Lapsed, Releasing, Released, Failed };
const char *address_state_name(AddressState state);
std::optional<AddressState> address_state_from_name(std::string_view name);
// Whether the cloud may hold it now (allocating, ready, lapsed, releasing):
// the admin's "held" count, and what fleet.sweep keeps.
bool address_is_held(AddressState state);

// A failed allocation's automatic tries (the fleet's pass): the first
// kAddressRetryBaseS after the refusal, the wait doubling with each refusal
// after it, to kAddressRetryMaxS.
inline constexpr int64_t kAddressRetryBaseS = 5 * 60;
inline constexpr int64_t kAddressRetryMaxS = 6 * 60 * 60;
// Whether the cloud's refusal `code` passes on its own, so the address is
// tried again automatically: an exhausted Elastic IP quota (or the local
// stack's range), AddressLimitExceeded, which a raised quota or a released
// address ends. A throttle or an unanswered call fails no row: the drivers
// answer it Retry, the job's own backoff, and the fleet's pass runs an
// allocation whose job is gone again.
bool address_refusal_passes(std::string_view code);
// The wait before the automatic try that follows `retries` earlier ones.
int64_t address_retry_delay_s(int retries);

// A record a state moved by (hosted_addresses.lapse_cause).
inline constexpr const char *kLapseGrant = "grant"; // its subscription stopped granting
inline constexpr const char *kLapseAdmin = "admin"; // an admin released it

struct HostedAddress {
	int64_t id = 0;
	std::optional<int64_t> owner_user_id; // unset once the account is gone
	std::string name;
	std::string source; // "subscription" or "comp"
	std::string stripe_subscription_id; // a subscription's; "" for a comp one
	AddressState state = AddressState::Allocating;
	std::string allocation_id; // the cloud's (eipalloc-...); "" until allocated
	std::string public_ip;     // "" until allocated
	std::string dns_name;      // the name its A record carries now; "" for none
	std::string dns_change_id; // the zone's last change; "" for none
	std::string dns_state;     // "none", "pending" (a delete waits to be in sync), "insync"
	int64_t release_after = 0; // Unix seconds: its record's TTL drained (0: not yet)
	std::string lapse_cause;   // "", kLapseGrant or kLapseAdmin
	std::string reason;        // a failure's, a lapse's
	int retries = 0;           // the automatic tries since it was named or an admin retried it
	// UTC 'YYYY-MM-DD HH:MM:SS'; "" until set.
	std::string created_at;
	std::string ready_at;
	std::string lapsed_at;
	std::string released_at;
	std::string retry_at; // a failed one's next automatic try; "" for none
};

std::optional<HostedAddress> find_address(db::Database &db, int64_t id);
// The account's addresses: the held ones by id, then its released and failed
// ones, newest first.
std::vector<HostedAddress> addresses_of(db::Database &db, int64_t owner_user_id);
// Every address, the held ones first, newest first (the admin's view).
std::vector<HostedAddress> list_addresses(db::Database &db);
// The addresses in `state`, oldest first (the fleet's pass).
std::vector<HostedAddress> addresses_in(db::Database &db, AddressState state);
// How many addresses the cloud may hold now (address_is_held).
int count_held_addresses(db::Database &db);
// The addresses the cloud may hold now, by owner (an address whose account is
// gone under 0): those that hold a grant (allocating, ready) and those on
// their way out (lapsed, releasing), whose cloud address is held until it is
// released (the sold capacity, sold_capacity.h).
struct HeldAddresses {
	int64_t granted = 0;
	int64_t leaving = 0;
};
std::map<int64_t, HeldAddresses> held_addresses_by_owner(db::Database &db);

// The granting static-address subscriptions of the account that no address
// row names yet, a released one's aside (the owner names one of them),
// oldest first, at `now`.
std::vector<std::string> unnamed_address_subscriptions(db::Database &db, int64_t owner_user_id, int64_t now);

// What naming an address came to.
enum class NameOutcome : uint8_t {
	Created,
	InvalidName,   // address_names.h refuses it
	NameTaken,     // another address has it (any state)
	NoAddress,     // a subscription address: none of the account's is unnamed
	AddressLimit,  // a comp address: the account's addresses would pass its own machines
	NoSuchAccount, // no active player's account
};
struct NameResult {
	NameOutcome outcome = NameOutcome::Created;
	int64_t id = 0;
	std::string why; // InvalidName's
};
// A subscription address of the owner's named `name`: the oldest unnamed
// granting static-address subscription takes the row (allocating). In the
// caller's transaction (which enqueues its address.allocate beside it).
NameResult name_subscription_address(db::Database &db, int64_t owner_user_id, const std::string &name, int64_t now);
// An admin's comp address for the account named `name`, while its static
// addresses stay within its own machines (billing/entitlement.h). In the
// caller's transaction.
NameResult add_comp_address(db::Database &db, int64_t owner_user_id, const std::string &name, int64_t now);
// An admin's rename of address `id` (any state: a released one's frees its
// old name). InvalidName, NameTaken, or NoSuchAccount for no such address.
NameResult rename_address(db::Database &db, int64_t id, const std::string &name);

// --- the worker's writes (address_jobs.h) ---

// What the cloud allocated for allocating address `id`; false when it is no
// longer allocating.
bool record_allocation(db::Database &db, int64_t id, const std::string &allocation_id, const std::string &public_ip);
// Its A record names `dns_name` now (change `change_id`, in sync or not yet
// known: insync for an upsert), recorded whatever the row's state but
// released, so a release that took the row meanwhile deletes it;
// allocating -> ready on the first. True when the row is ready.
bool record_dns_upsert(db::Database &db, int64_t id, const std::string &dns_name, const std::string &change_id);
// The cloud refused it: allocating -> failed with `reason`. With
// `retry_in_s` above 0 (a refusal that passes) its next automatic try is
// that far on, and counted (retries); 0 leaves it to an admin's retry.
bool fail_address(db::Database &db, int64_t id, const std::string &reason, int64_t retry_in_s);
// An admin's retry: failed -> allocating, its automatic tries counted from 0
// again. The caller enqueues its address.allocate in the same transaction.
bool retry_address(db::Database &db, int64_t id);
// The fleet's pass: a failed address whose retry_at has passed -> allocating,
// its tries kept for the next backoff. The caller enqueues its
// address.allocate in the same transaction.
bool retry_due_address(db::Database &db, int64_t id);
// A releasing address's record deleted (change `change_id`, pending until in
// sync).
bool record_dns_delete(db::Database &db, int64_t id, const std::string &change_id);
// That delete in sync: no record now, and the address releasable once
// `release_after` (Unix seconds) has passed.
bool record_dns_gone(db::Database &db, int64_t id, int64_t release_after);
// releasing -> released.
bool mark_released(db::Database &db, int64_t id);
// A releasing address whose grant came back before it went: releasing ->
// ready (its record written again by the next allocate).
bool restore_address(db::Database &db, int64_t id);

// --- the fleet's pass (the supervisor) ---

// ready (or allocating, or failed) -> lapsed with `cause` and `reason` (no
// automatic try left).
bool lapse_address(db::Database &db, int64_t id, const std::string &cause, const std::string &reason);
// lapsed -> releasing.
bool begin_release(db::Database &db, int64_t id);

// The live server that holds address `id` now, or nullopt.
std::optional<int64_t> server_of_address(db::Database &db, int64_t id);

// --- what fleet.sweep found (hosted_address_orphans) ---

// An address the cloud holds for this deployment that no held row names.
struct AddressOrphan {
	std::string allocation_id;
	std::string public_ip;
	int64_t address_id = 0; // its tag's (0: none)
	std::string seen_at;
};
// The sweep's findings replace the last sweep's.
void replace_address_orphans(db::Database &db, const std::vector<AddressOrphan> &orphans);
std::vector<AddressOrphan> address_orphans(db::Database &db);

} // namespace opennova::novaworld_server::hosted
