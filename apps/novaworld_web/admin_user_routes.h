#pragma once

// The admin's user routes (README "Website sessions and roles";
// apps/novaworld_common/hosted/README.md): GET and POST /api/admin/users, PUT and DELETE
// /api/admin/users/<id>, PUT /api/admin/users/<id>/comp (an admin's
// grants of hosted servers and own machines) and PUT /api/admin/users/<id>/game-access. Every
// handler opens with WebAccess::require_admin: the Bearer ADMIN_API_TOKEN, or
// an admin-role website session (with the CSRF header on a state change). The
// HttpListener owns one beside its other route families.

#include "auth.h"
#include "hosted/placement.h"
#include "hosted/sold_capacity.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace opennova::novaworld_server {

// A user row as JSON (no password_hash), with its admin comp grants and its
// hosting entitlement's server count (billing/entitlement.h, read now). Shared
// by the admin user routes and the public /api/register reply.
crow::json::wvalue user_to_json(db::Database &db, const UserRecord &u);

class AdminUserRoutes {
public:
	// The rest of an account's revocation (HttpListener's
	// forget_revoked_account), on the handler's connection: a delete, and an
	// update that ends its logins.
	using ForgetAccount = std::function<void(db::Database &db, int64_t user_id)>;

	AdminUserRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	AdminUserRoutes(const AdminUserRoutes &) = delete;
	AdminUserRoutes &operator=(const AdminUserRoutes &) = delete;

	// What the fleet sells (hosted/sold_capacity.h), which a user page's
	// capacity is read against; before register_routes(). Unset, placement.h's
	// defaults.
	void set_fleet(const hosted::PlacementPolicy &policy) { sellable_ = hosted::sellable_of(policy); }
	// The play zone the static addresses are named in (a user page's addresses).
	void set_address_zone(std::string zone) { address_zone_ = std::move(zone); }
	void register_routes(WebApp &app, ForgetAccount forget);

private:
	// The sold capacity against what the fleet sells (capacity_json).
	crow::json::wvalue capacity_json(db::Database &db) const;

	WebAccess &access_;
	db::ConnectionPool &pool_;
	ForgetAccount forget_;
	hosted::Capacity sellable_ = hosted::sellable_of(hosted::PlacementPolicy{});
	std::string address_zone_;
};

} // namespace opennova::novaworld_server
