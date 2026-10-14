#pragma once

// The CD keys' routes (cd_keys.h, migration 0015). The admin's /api routes,
// each behind WebAccess::require_admin (and ONNET_API_PROXY_ONLY like every
// /api route):
//
//   GET  /api/admin/cd-keys                   the keys, newest first: ?title=,
//                                             ?state= (unassigned, assigned,
//                                             revoked), ?q= (a part of the serial,
//                                             the wire key or the username),
//                                             ?limit= (1..kMaxListed), ?offset=
//   POST /api/admin/cd-keys                   {"title", "count"}: new unassigned keys
//   POST /api/admin/cd-keys/<id>/assign       {"username"} or {"user_id"}
//   POST /api/admin/cd-keys/<id>/revoke       the key activates no machine again
//   GET  /api/admin/cd-keys/<id>/activations  the machines it activated
//   POST /api/admin/cd-keys/<id>/activations  {"nwpssk"}: the NWACT for that
//                                             machine (a manual activation), recorded
//   GET  /api/admin/users/<id>/cd-keys        an account's keys and activations
//
// and the retail login's hook (http_listener.cpp): the NWACT a completed DFX2
// login hands an unactivated client for its machine, as the NWACT cookie the
// client's NovaWorld screen reads (cd_keys.h, cdkey.h). Every reply is an
// error code with our own message, never database text. The HttpListener owns
// one CdKeyRoutes beside its HostedRoutes.

#include "catalog_repository.h"
#include "cd_keys.h"
#include "web_access.h"

#include <net/novaworld/db/sqlite.h>

#include <crow.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace opennova::novaworld_server {

struct ServerConfig;

class CdKeyRoutes {
public:
	// The most keys one generate request makes, and one list reply holds.
	static constexpr size_t kMaxGenerate = 100;
	static constexpr size_t kMaxListed = 500;

	CdKeyRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}

	CdKeyRoutes(const CdKeyRoutes &) = delete;
	CdKeyRoutes &operator=(const CdKeyRoutes &) = delete;

	// Takes ONNET_CD_KEY_AUTO_MINT (cd_key_auto_mint) and logs it. Before
	// register_routes().
	void configure(const ServerConfig &config);
	void register_routes(WebApp &app);

	// The NWACT a retail login's completion hands its client, "" for none: for
	// a client of a title that activates (`game`) whose NWCDKIID cookie is no
	// wire key (needs_activation), the activation of the machine its NWPSSK
	// cookie names under account `user_id`'s key (activate_account, minting the
	// account's first key when auto-minting is on). Logged either way.
	std::string login_activation(int64_t user_id, const catalog::GameRow &game,
			const std::map<std::string, std::string> &cookies);

private:
	crow::response list(const crow::request &req);
	crow::response generate(const crow::request &req);
	crow::response assign(const crow::request &req, uint64_t id);
	crow::response revoke(const crow::request &req, uint64_t id);
	crow::response key_activations(const crow::request &req, uint64_t id);
	crow::response activate(const crow::request &req, uint64_t id);
	crow::response account(const crow::request &req, int64_t user_id);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	bool auto_mint_ = true;
};

} // namespace opennova::novaworld_server
