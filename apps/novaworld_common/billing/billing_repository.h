#pragma once

// The billing tables (backend/migrations/0002_billing.sql): the service's
// instance id, each account's Stripe customer, the subscriptions as Stripe
// last answered them, and the webhook events still to sync. The sync worker
// (billing_sync.h) writes them from what it reads back from Stripe, the
// routes read them, and the hosting entitlement (entitlement.h) is computed
// from them, whether or not billing is configured. Every write that spans
// statements is one db::Transaction, and no Stripe call ever runs inside one.
// Times in and out of the rows' period ends are Unix seconds (UTC). Crow-free.
// Each function throws db::Error on a database failure. Service policy, not a
// port.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::billing {

// This database's instance id (service_meta.instance_id): 32 lower-case hex
// digits, minted from the OS CSPRNG at the first call on a database and the
// same ever after (two services racing at a first boot agree on the first
// row written).
std::string instance_id(db::Database &db);

// An account's Stripe customer.
struct CustomerRow {
	int64_t user_id = 0;
	std::string stripe_customer_id;
	bool livemode = false;
	std::string open_checkout_session; // "" for none
	std::string created_at;            // UTC 'YYYY-MM-DD HH:MM:SS'
	std::string synced_at;             // "" until the worker's first full read
};

std::optional<CustomerRow> customer_of(db::Database &db, int64_t user_id);
std::optional<CustomerRow> customer_by_id(db::Database &db, const std::string &stripe_customer_id);
std::optional<int64_t> user_of_customer(db::Database &db, const std::string &stripe_customer_id);
// The account's customer row; false (nothing written) when the account or the
// customer already has one.
bool insert_customer(db::Database &db, int64_t user_id, const std::string &stripe_customer_id, bool livemode);
// Every customer row, by account (the admin's overview).
std::vector<CustomerRow> list_customers(db::Database &db);
void set_open_checkout_session(db::Database &db, int64_t user_id, const std::string &session_id);
void mark_customer_synced(db::Database &db, const std::string &stripe_customer_id);
// The customer's row deleted (a customer Stripe deleted: the next checkout
// makes a new one); its subscriptions' rows stay.
void delete_customer(db::Database &db, const std::string &stripe_customer_id);

// What a subscription's price sells.
enum class ItemKind : uint8_t { Tier, OwnMachine, StaticAddress, Unknown };
const char *item_kind_name(ItemKind kind);          // "tier", "own_machine", "static_address", "unknown"
ItemKind item_kind_from_name(std::string_view name); // Unknown for any other text

struct SubscriptionRow {
	std::string stripe_subscription_id;
	std::string stripe_customer_id;
	std::optional<int64_t> user_id;
	bool livemode = false;
	std::string status; // Stripe's
	ItemKind item = ItemKind::Unknown;
	std::string price_id;
	int tier = 0; // 1..3 for a tier; 0 otherwise
	int64_t quantity = 1;
	std::optional<int64_t> current_period_end; // Unix seconds
	bool cancel_at_period_end = false;
	std::string status_since; // UTC 'YYYY-MM-DD HH:MM:SS' (read only)
	std::string updated_at;   // likewise
};

// Writes the row whole (insert or replace); status_since moves to now only
// when the status changes (a new row's is now).
void upsert_subscription(db::Database &db, const SubscriptionRow &row);
std::vector<SubscriptionRow> subscriptions_of(db::Database &db, int64_t user_id);
std::vector<SubscriptionRow> subscriptions_of_customer(db::Database &db, const std::string &stripe_customer_id);
std::optional<SubscriptionRow> find_subscription(db::Database &db, const std::string &stripe_subscription_id);
// Every subscription row, newest status first (the admin's overview).
std::vector<SubscriptionRow> list_subscriptions(db::Database &db);
// Every subscription row with an account, by account, each account's in
// subscriptions_of's order (the admin's users list, in one read).
std::map<int64_t, std::vector<SubscriptionRow>> subscriptions_by_account(db::Database &db);
// Every subscription of the customer still not ended (any status but canceled
// and incomplete_expired) whose id is not in `keep` goes to `status` (a
// customer Stripe deleted: canceled), each with its status_since moved; how
// many.
size_t end_subscriptions_of_customer(db::Database &db, const std::string &stripe_customer_id,
		const std::vector<std::string> &keep, const std::string &status);

// --- The webhook events ---

// Records a delivered event; false when its id is already recorded (a
// redelivery). An event naming no customer (customer_id "") is recorded
// processed, with `outcome` "ignored": the worker has nothing to sync for it.
bool record_event(db::Database &db, const std::string &event_id, const std::string &type, bool livemode,
		const std::string &customer_id, std::optional<int64_t> stripe_created);
// The customers with an unprocessed event, the longest-waiting first, at most
// `limit`.
std::vector<std::string> pending_customers(db::Database &db, size_t limit);
// How many events are unprocessed, and the age in seconds of the oldest (0
// with none).
size_t pending_event_count(db::Database &db);
int64_t oldest_pending_age_s(db::Database &db);
// Every unprocessed event of the customer marked processed with `outcome`
// (with `received_by`, a database_clock() reading, only those received by
// then: a sync marks the events it started after, so one delivered while it
// read Stripe waits for the next); how many.
size_t mark_processed(db::Database &db, const std::string &customer_id, const std::string &outcome,
		const std::optional<std::string> &received_by = std::nullopt);
// The database's clock now, as its exact timestamptz text (for mark_processed).
std::string database_clock(db::Database &db);
// Processed events older than `days` deleted; how many.
size_t prune_events(db::Database &db, int days = 30);

struct EventRow {
	std::string event_id;
	std::string type;
	std::string customer_id;
	std::string received_at; // UTC 'YYYY-MM-DD HH:MM:SS'
	std::string outcome;     // "" while unprocessed
	bool processed = false;
};
// The newest `limit` events, newest first (the admin's log).
std::vector<EventRow> recent_events(db::Database &db, size_t limit);

// Whether a subscription of the account grants at `now` (entitlement.h
// subscription_grants: a tier, a dedicated machine or a static address, active, trialing or past_due,
// inside its period's net). The admin's account delete refuses such an
// account.
bool has_granting_subscription(db::Database &db, int64_t user_id, int64_t now);

// --- An admin's comp grants (comp_grants, 0017_capacity_addresses.sql) ---

// What an admin grants an account outside any subscription (each 0..16,
// tiers.h kMaxCompServers and kMaxCompOwnMachines): hosted servers on shared
// machines, and dedicated machines, each one more server on a machine of its
// own (entitlement.h).
struct CompGrants {
	int servers = 0;      // shared servers
	int own_machines = 0; // dedicated machines
};
// The account's grants (none: zeros).
CompGrants comp_grants_of(db::Database &db, int64_t user_id);
// Every account's grants that are not zeros, by account (the admin's users
// list and the sold capacity, in one read).
std::map<int64_t, CompGrants> comp_grants_by_account(db::Database &db);
// The account's grants set to `grants` (a 0 deletes its row), in one
// transaction; false (nothing written) when no player account has `user_id`
// (a service account included). The caller checks the ranges.
bool set_comp_grants(db::Database &db, int64_t user_id, const CompGrants &grants);
// An admin's comp addresses that count toward an account's entitlement
// (hosted_addresses' comp rows allocating or ready;
// hosted/model/address_repository.h): the account's, and every account's that has
// some.
int comp_addresses_of(db::Database &db, int64_t user_id);
std::map<int64_t, int> comp_addresses_by_account(db::Database &db);
// The static-address subscriptions an address was named for that holds one
// (hosted_addresses' subscription rows allocating or ready): the account's,
// and every account's that has some (entitlement.h addresses_to_end ends
// these last).
std::set<std::string> named_address_subscriptions(db::Database &db, int64_t user_id);
std::map<int64_t, std::set<std::string>> named_address_subscriptions_by_account(db::Database &db);

// --- A checkout's hold on what it sells (billing_checkout_reservations) ---
//
// A checkout reserves the item it sells before Stripe makes its session, in
// the transaction that judged it under the hosting-start lock (database.h
// kHostedStartLock), so two checkouts at once never both take the last of
// the fleet's room (hosted/model/sold_capacity.h). The reservation counts as a
// granting subscription of its item until it is dropped: by the sync that
// writes the subscription its session made (the subscription's
// opennova_reservation metadata, in the same transaction), by the session's
// checkout.session.expired event, by the account's next checkout (its
// previous session is settled first), or at its expires_at.

// The metadata key a checkout stamps its reservation's id under (on the
// session and on the subscription it makes).
inline constexpr const char *kMetaReservation = "opennova_reservation";

struct CheckoutReservation {
	int64_t id = 0;
	int64_t user_id = 0;
	ItemKind item = ItemKind::Unknown; // Tier, OwnMachine, StaticAddress
	int tier = 0; // 1..3 for a tier
	std::string checkout_session; // "" until its session is made
};

// A reservation of `item` (`tier` for a tier) for the account that lapses
// `life_s` from now, in the caller's transaction; its id. Expired ones are
// deleted first.
int64_t reserve_checkout(db::Database &db, int64_t user_id, ItemKind item, int tier, int64_t life_s);
// The checkout session made for reservation `id`.
void stamp_reservation(db::Database &db, int64_t id, const std::string &checkout_session);
// Reservation `id` of account `user_id` dropped (another account's stays).
void drop_reservation(db::Database &db, int64_t id, int64_t user_id);
// The reservation of checkout session `checkout_session` dropped (none for
// ""); how many.
size_t drop_session_reservation(db::Database &db, const std::string &checkout_session);
// The reservations that have not lapsed, by account.
std::map<int64_t, std::vector<CheckoutReservation>> live_reservations_by_account(db::Database &db);
std::vector<CheckoutReservation> live_reservations_of(db::Database &db, int64_t user_id);
// The reservation as the granting subscription it counts as at `now`
// (status active, its period a day on).
SubscriptionRow reservation_row(const CheckoutReservation &reservation, int64_t now);

} // namespace opennova::novaworld_server::billing
