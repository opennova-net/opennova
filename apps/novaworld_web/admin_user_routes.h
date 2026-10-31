#pragma once

// The admin's user routes (README "Website sessions and roles";
// apps/novaworld_common/hosted/README.md): GET and POST /api/admin/users, PUT and DELETE
// /api/admin/users/<id>, PUT /api/admin/users/<id>/comp (an admin's
// grants of hosted servers and dedicated machines) and PUT /api/admin/users/<id>/game-access. Every
// handler opens with WebAccess::require_admin: the Bearer ADMIN_API_TOKEN, or
// an admin-role website session (with the CSRF header on a state change). The
// HttpListener owns one beside its other route families.

#include "accounts/auth.h"
#include "hosted/model/placement.h"
#include "hosted/model/service_settings.h"
#include "hosted/model/sold_capacity.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace opennova::novaworld_server {

class ObjectStore;

// A user row as JSON (no password_hash), with its admin comp grants, its
// hosting entitlement's server count (billing/entitlement.h, read now) and its
// created_at and last_login (ISO 8601 UTC, last_login null for none). Shared
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

	// The placement policy the fleet booted with, which the admin's settings
	// override live (hosted/model/service_settings.h): a user page's capacity
	// is read against what the fleet sells now; before register_routes().
	// Unset, placement.h's defaults.
	void set_fleet(const hosted::PlacementPolicy &policy) { fleet_ = policy; }
	// The play zone the static addresses are named in (a user page's addresses).
	void set_address_zone(std::string zone) { address_zone_ = std::move(zone); }
	// The serve image the release pins (a user page's servers' server build updates,
	// hosted_routes.h server_json).
	void set_serve_image(std::string image) { serve_image_ = std::move(image); }
	// The deployment's object store, null while hosting is off (an account's delete then queues
	// no purge for its projects: with no store there are no objects). Before register_routes().
	void set_store(ObjectStore *store) { store_ = store; }
	void register_routes(WebApp &app, ForgetAccount forget);

private:
	WebAccess &access_;
	db::ConnectionPool &pool_;
	ForgetAccount forget_;
	hosted::PlacementPolicy fleet_;
	hosted::Capacity sellable(db::Database &db) const { return hosted::sellable_of(hosted::live_placement(db, fleet_)); }
	std::string address_zone_;
	std::string serve_image_;
	ObjectStore *store_ = nullptr;
};

} // namespace opennova::novaworld_server
