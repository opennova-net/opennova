#pragma once

// The admin's user routes (README "Website sessions and roles", "Hosted
// servers"): GET and POST /api/admin/users, PUT and DELETE
// /api/admin/users/<id>, PUT /api/admin/users/<id>/comp-servers (an admin's
// grant of hosted servers) and PUT /api/admin/users/<id>/game-access. Every
// handler opens with WebAccess::require_admin: the Bearer ADMIN_API_TOKEN, or
// an admin-role website session (with the CSRF header on a state change). The
// HttpListener owns one beside its other route families.

#include "auth.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <functional>

namespace opennova::novaworld_server {

// A user row as JSON (no password_hash), with its admin comp grant and its
// hosting entitlement's server count (billing/entitlement.h, read now). Shared
// by the admin user routes and the public /api/register reply.
crow::json::wvalue user_to_json(db::Database &db, const UserRecord &u);

class AdminUserRoutes {
public:
	// The in-memory half of an account's revocation (HttpListener's
	// forget_revoked_account): a delete, and an update that ends its logins.
	using ForgetAccount = std::function<void(int64_t user_id)>;

	AdminUserRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	AdminUserRoutes(const AdminUserRoutes &) = delete;
	AdminUserRoutes &operator=(const AdminUserRoutes &) = delete;

	void register_routes(WebApp &app, ForgetAccount forget);

private:
	WebAccess &access_;
	db::ConnectionPool &pool_;
	ForgetAccount forget_;
};

} // namespace opennova::novaworld_server
