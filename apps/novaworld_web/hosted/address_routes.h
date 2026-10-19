#pragma once

// The static addresses' /api routes (ADR 0066; apps/novaworld_common/hosted/README.md
// "Static addresses"): the owner's, GET /api/me/addresses and POST /api/me/addresses
// (a name for an address subscription the account bought; require_user, the CSRF
// header on the POST; its cancel is the Portal's, POST /api/billing/portal
// {"cancel_subscription"}), and the admin's, GET /api/admin/addresses, PUT
// /api/admin/addresses/<id> (a rename), POST /api/admin/addresses/<id>/release (with
// the address's name typed back) and POST /api/admin/users/<id>/addresses (a comp
// address; require_admin). They write rows and enqueue the worker's address jobs in
// the same transaction (hosted/model/address_jobs.h); the fleet's pass and the workers do
// the rest. Every reply is an error code with our own message, never database text.
// The HttpListener owns one.

#include "hosted/model/address_repository.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

// An address as JSON: {id, name, hostname (<name>.<zone>), state, public_ip
// and port (where its servers list: null until allocated), source,
// stripe_subscription_id (null for a comp one), server ({id, game_name,
// state} of the live server using it, or null), reason (null for none),
// created_at, ready_at, lapsed_at, released_at}, and with `admin` its owner
// ({id, username} or null), allocation_id, dns_name and dns_state and its
// lapse_cause.
crow::json::wvalue address_json(db::Database &db, const HostedAddress &a, const std::string &zone, bool admin);

class AddressRoutes {
public:
	AddressRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	AddressRoutes(const AddressRoutes &) = delete;
	AddressRoutes &operator=(const AddressRoutes &) = delete;

	// Whether the static addresses are on (hosted/model/hosted_policy.h
	// HostedPolicy::addresses_on),
	// the play zone they are named in and the addresses the fleet sells. Before
	// register_routes(); off, a name or a comp address answers 503
	// addresses_disabled.
	void configure(bool on, std::string zone, int sellable);
	void register_routes(WebApp &app);

private:
	crow::response me_addresses(const crow::request &req);
	crow::response me_name(const crow::request &req);
	crow::response admin_addresses(const crow::request &req);
	crow::response admin_rename(const crow::request &req, uint64_t id);
	crow::response admin_release(const crow::request &req, uint64_t id);
	crow::response admin_comp(const crow::request &req, uint64_t user_id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool on_ = false;
	std::string zone_;
	int sellable_ = 0;
};

} // namespace opennova::novaworld_server::hosted
