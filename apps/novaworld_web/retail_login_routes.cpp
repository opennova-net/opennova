// The retail login chain's routes (HttpListener::register_legacy_login_routes): /nwprepare.dll,
// /NWStart.dll, the EPASK login POST and its relay GET (/NWLogin.dll), /NWLogout.dll and the
// character and account pages. Split out of http_listener.cpp whole.

#include "http_listener.h"

#include "accounts/auth.h"
#include "accounts/mailer.h"
#include "accounts/template_engine.h"
#include "admin_user_routes.h"
#include "billing/billing_routes.h"
#include "cd_key_routes.h"
#include "connection_routes.h"
#include "core/health.h"
#include "core/object_store.h"
#include "host_routes.h"
#include "hosted/address_routes.h"
#include "hosted/console_routes.h"
#include "hosted/fleet_routes.h"
#include "hosted/hosted_routes.h"
#include "hosted/model/net_cidr.h"
#include "hosted/stats_routes.h"
#include "http_cookies.h"
#include "http_listener_impl.h"
#include "http_listener_impl.h"
#include "infra/attention_routes.h"
#include "infra/cost_routes.h"
#include "infra/infra_routes.h"
#include "infra/log_routes.h"
#include "infra/usage_routes.h"
#include "jobs/job_routes.h"
#include "lobby_routes.h"
#include "password_reset_routes.h"
#include "products/product_routes.h"
#include "products/project_routes.h"
#include "products/release_routes.h"
#include "program/catalog_repository.h"
#include "program/server_config.h"
#include "redis_client.h"
#include "retail_pages.h"
#include "web_access.h"

#include <net/napi/session.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/novaworld/db/database.h>
#include <net/novaworld/gsb.h>
#include <net/novaworld/host_repository.h>
#include <net/novaworld/join_identity.h>
#include <net/novaworld/lobby_repository.h>
#include <net/novaworld/relay_request.h>
#include <net/novaworld/unknown_tracker.h>

#include <base/io/cp1252.h>
#include <base/io/file_io.h>
#include <base/io/strutil.h>
#include <base/os_random/os_random.h>

#include <crow.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server {

// Retail NW*.dll login/session chain: prepare, start, the EPASK login
// POST + relay GET, logout, and the character/account template pages.
void HttpListener::register_legacy_login_routes(const std::string &templates_dir) {
	auto &app = impl_->app;

	// ----- Phase E.1: Legacy NW*.dll login chain ---------------------------
	// Routes registered for both lowercase (retail capture) and Title-case
	// spellings since Crow routes are case-sensitive. Behavior mirrors
	// onnet's onnw/controllers/nova_world/{prepare,login}.py.

	// GET /nwprepare.dll: the gate's startupurl, whose `gt` is the client's
	// gate tag (the [GT] it substitutes), so the page and the GT cookie it sets
	// name the client's game; `url` defaults to that game's start page. DFX2's
	// startupurl also carries its pfid.
	auto handle_prepare = [this, templates_dir](const crow::request &req) {
		const std::string url_param = req.url_params.get("url") ? req.url_params.get("url") : "";
		const char *pfid_param = req.url_params.get("pfid");
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		const catalog::GameRow &game =
				client_game(impl_->games, req, cookies, pfid_param ? pfid_param : "", url_param);
		const std::string url   = url_param.empty() ? game.start_page : url_param;
		const std::string ver1  = req.url_params.get("ver1") ? req.url_params.get("ver1") : "";
		const std::string ver2  = req.url_params.get("ver2") ? req.url_params.get("ver2") : "";
		const std::string gt    = req.url_params.get("gt")   ? req.url_params.get("gt")   : "";
		const std::string cc    = req.url_params.get("cc")   ? req.url_params.get("cc")   : "";

		const std::string epask = opennova::epask_to_string(epask_params_);
		std::printf("[http] GET %s url=%s gt=%s game=%s ver=%s/%s cookies=[%s]\n",
				loggable(req.url).c_str(), loggable(url).c_str(), loggable(gt).c_str(),
				loggable(game.slug).c_str(), loggable(ver1).c_str(), loggable(ver2).c_str(),
				cookie_summary(request_cookie_header(req)).c_str());

		TemplateVars vars{
			{"HOST_URL",     host_url(game)},
			{"GSB_SERVER",   gsb_url(game)},
			{"JOINLAN_URL",  game.join_lan_url},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, url, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP", req.remote_ip_address);
		add_cookie(res, "VER1",   ver1);
		add_cookie(res, "VER2",   ver2);
		add_cookie(res, "GT",     gt);
		if (!cc.empty()) add_cookie(res, "CC", cc);
		// The EPASK bundle goes to a client that holds none: the live service's answer to a
		// re-entry, whose request carries the bundle the first entry set (and the login), sets
		// YOURIP, VER1, VER2, GT and CC alone (the LN S1 / S1b / S1d records, 2026-10-23: every
		// nwprepare request without an EPASK cookie is answered with one, every one with it gets
		// none).
		if (cookies.find("EPASK") == cookies.end()) add_cookie(res, "EPASK", epask);
		return res;
	};
	CROW_ROUTE(app, "/nwprepare.dll").methods("GET"_method)(handle_prepare);
	CROW_ROUTE(app, "/NWPrepare.dll").methods("GET"_method)(handle_prepare);

	// GET /nwstart.dll
	auto handle_start = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		const char *named_page = req.url_params.get("IN") ? req.url_params.get("IN")
		                         : req.url_params.get("OUT") ? req.url_params.get("OUT")
		                                                     : req.url_params.get("MSGBASE");
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {},
		                                           named_page ? named_page : "");
		const std::string in_p   = req.url_params.get("IN")      ? req.url_params.get("IN")      : game.page("main.htm");
		const std::string out_p  = req.url_params.get("OUT")     ? req.url_params.get("OUT")     : game.page("login.htm");
		const std::string msgbase = req.url_params.get("MSGBASE")? req.url_params.get("MSGBASE") : game.page("msg.htm");
		// A login the service remembers is answered with IN, the lobby page, else OUT, the login
		// page: the live service's NWStart answers a re-entry carrying the login it remembers
		// with the main page (11041 B, the size of the login's last poll answer) and a first entry
		// with the login page (4634 B), each with YOURIP and USEJUNCTION as its only cookies (the
		// LN S1 / S1b / S1d records, 2026-10-23). The remembered login is the request's own
		// (retail_login_user: its LOGINSESSIONTAG's active session, else its
		// PERSISTENTEXPRESSLOGINDATA pin), an active account only. Neither page is the start
		// page's ACT (DFX2's activation login): a DFX2 client whose NWCDKIID is no wire key is
		// handed its key silently at the login's completion instead
		// (CdKeyRoutes::login_activation; D-NET-725, the service's policy).
		std::optional<UserRecord> user;
		try {
			const char *how = "";
			user = retail_login_user(cookies, *db_pool_.acquire(), how);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] WARN retail login lookup failed: %s\n", e.what());
		}
		if (user && user->account_status != "active") user.reset();
		const std::string &page = user ? in_p : out_p;

		std::printf("[http] GET %s IN=%s OUT=%s MSGBASE=%s -> %s cookies=[%s]\n",
				loggable(req.url).c_str(), loggable(in_p).c_str(), loggable(out_p).c_str(),
				loggable(msgbase).c_str(), user ? "IN" : "OUT",
				cookie_summary(request_cookie_header(req)).c_str());

		// Hardcoded version-OK.
		TemplateVars vars{
			{"IN",          in_p},
			{"OUT",         out_p},
			{"MSGBASE",     msgbase},
			{"HOST_URL",    host_url(game)},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", game.join_lan_url},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, page, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",      req.remote_ip_address);
		add_cookie(res, "USEJUNCTION", "0");
		return res;
	};
	CROW_ROUTE(app, "/nwstart.dll").methods("GET"_method)(handle_start);
	CROW_ROUTE(app, "/NWStart.dll").methods("GET"_method)(handle_start);

	// POST /NWLogin.dll
	auto handle_login_post = [this, templates_dir](const crow::request &req) {
		auto db_conn = db_pool_.acquire();
		// The login rows idle past kRetailLoginIdleSeconds go with the periodic
		// active_user_sessions.prune job (accounts/account_jobs.h), never on a
		// login's thread; the login sessions themselves expire in Redis
		// (SessionStore::kTtl).

		const auto form = parse_form_body(req.body);
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		auto pick = [&](const char *name) {
			auto it = form.find(name);
			return it == form.end() ? std::string() : it->second;
		};

		// Real-auth path: when retail POSTs the form, the EPASK field
		// echoes the e:n:key bundle we sent on /nwprepare.dll, and the
		// other fields (NAME, PASSWORD, relay, msgbase, success, ...)
		// arrive encrypted under it. Decode them here. Falls back
		// silently when EPASK is absent — that lets curl walkthroughs
		// hit the same handler with raw values.
		std::optional<opennova::EpaskParams> epask_in;
		if (auto e_str = pick("EPASK"); !e_str.empty()) {
			opennova::EpaskParams parsed;
			std::string why;
			if (opennova::epask_from_string(e_str, parsed, &why)) {
				epask_in = parsed;
			} else {
				std::fprintf(
						stderr, "[http] WARN EPASK form field bad: %s\n", loggable(why).c_str());
			}
		}
		auto epask_decode = [&](const std::string &name) -> std::string {
			const auto v = pick(name.c_str());
			if (v.empty() || !epask_in) return v;
			std::string plain;
			std::string why;
			if (!opennova::epask_decrypt(v, *epask_in, plain, &why)) {
				std::fprintf(stderr, "[http] WARN EPASK decrypt(%s) failed: %s\n", name.c_str(),
						loggable(why).c_str());
				return std::string();
			}
			return plain;
		};

		// looks_encrypted is now only relevant for the no-EPASK fallback
		// path (curl walkthroughs that don't carry an EPASK form field).
		auto looks_encrypted = [](const std::string &v) {
			if (v.size() < 16) return false;
			for (char c : v) if (c < 'A' || c > 'P') return false;
			return true;
		};
		auto field_or = [&](const char *name, const std::string &fallback) {
			const auto v = epask_in ? epask_decode(name) : pick(name);
			return (v.empty() || (!epask_in && looks_encrypted(v))) ? fallback : v;
		};

		const std::string login_name     = epask_in ? epask_decode("NAME")     : std::string();
		const std::string login_password = epask_in ? epask_decode("PASSWORD") : std::string();
		// The login's game: the client's gate tag (the GT cookie), else the
		// form's pfid and success page. Its access row and expansion bits are
		// the ones this login checks and grants.
		const catalog::GameRow &game = client_game(impl_->games, req, cookies,
				field_or("pfid", std::string()), field_or("success", std::string()));
		const std::string &game_slug = game.slug;
		auto render_login_message = [&](const std::string &message) {
			const std::string fail_tpl = field_or("failure", game.page("main.htm"));
			const std::string msg_tpl  = field_or("msgbase", game.page("msg.htm"));
			return render_legacy_message(templates_dir, message, fail_tpl, msg_tpl,
			                             req.remote_ip_address, host_url(game), gsb_url(game));
		};

		const auto server_status = get_server_status(*db_conn);
		if (server_status.maintenance_enabled) {
			std::printf("[http] POST /NWLogin.dll rejected: maintenance\n");
			return render_login_message(server_status.message.empty()
				? std::string("NovaWorld is temporarily unavailable.")
				: server_status.message);
		}

		LoginSession s;
		s.session_tag  = sessions_.generate_tag("NWLogin.dll");

		// Three-tier identity resolution:
		//  1. EPASK-decrypted NAME + PASSWORD → authenticate_user.
		//     This is the real-auth path retail follows.
		//  2. PERSISTENTEXPRESSLOGINDATA pin (G.8) → DB lookup.
		//     Used when retail somehow skips the form (rare) or when
		//     the persistent cookie is the only thing identifying the
		//     process.
		//  3. Round-robin fallback over seeded `players`.
		//     Only kicks in for curl walkthroughs that don't carry an
		//     EPASK form field (real retail always does).
		const auto persist_it = cookies.find("PERSISTENTEXPRESSLOGINDATA");
		const std::string persist_cookie = persist_it == cookies.end() ? std::string() : persist_it->second;

		std::optional<UserRecord> user;
		const char *resolution = nullptr;
		if (!login_name.empty() && !login_password.empty()) {
			// The login brake POST /api/login runs (WebAccess) on its retail
			// buckets: the address's attempts, then one token from the
			// username's failure buckets, handed back when the password is
			// right. Keyed by the TCP peer: a stock client reaches this port
			// (the gate's startupurl names ONNET_WEB_HOST and the HTTP port)
			// through no proxy but a TCP load balancer that keeps the
			// client's address and the retail port's own proxy, whose
			// X-Real-IP (its TCP peer) AccessMiddleware put in its place
			// (ONNET_RETAIL_PROXY), so no header the client sends names it and
			// nothing the website's brake sees can brake this one; only the
			// per-username cap is shared, and it spares the account's known
			// addresses. Its refusal is this route's own failure reply, the
			// msgbase page with a message (as a bad password and maintenance
			// get), which a stock client shows; no retail throttling reply is
			// witnessed (service policy).
			const std::string &peer = req.remote_ip_address;
			LoginTicket ticket;
			if (impl_->access.take_login_address(LoginRoute::Retail, peer) != 0 ||
					impl_->access.begin_password_check(
							*db_conn, LoginRoute::Retail, login_name, peer, ticket) != 0) {
				std::fprintf(stderr,
						"[http] POST /NWLogin.dll rate-limited for user '%s' from %s\n",
						loggable(login_name).c_str(), loggable(peer).c_str());
				return render_login_message(kLoginThrottledMessage);
			}
			user = authenticate_user(*db_conn, login_name, login_password);
			if (user) {
				resolution = "epask-auth";
				impl_->access.password_accepted(*db_conn, ticket, user->id, user->credentials_gen);
			} else {
				// Bad credentials → render the failure template (jop_2_main.htm
				// per retail's form `failure` arg) with a "Invalid username or
				// password" message and bail before storing a session.
				std::fprintf(stderr, "[http] POST /NWLogin.dll auth failed for user '%s'\n",
				             loggable(login_name).c_str());
				return render_login_message("Invalid username or password");
			}
		}
		if (!user && !persist_cookie.empty()) {
			user = pinned_user(persist_cookie, *db_conn);
			if (user)
				resolution = "persist-pin";
		}
		if (!user) {
			// No EPASK-authenticated user and no persist-pin match → reject.
			// Login requires real authentication; we never invent a session
			// from a seeded/stub user. Retail always carries EPASK creds or a
			// persist cookie, so a legitimate client never lands here. (Same
			// behaviour in dev and prod — dev just seeds the test accounts.)
			std::fprintf(stderr,
			             "[http] POST /NWLogin.dll rejected: unauthenticated\n");
			return render_login_message("Invalid username or password");
		}
		std::string effective_exp_bits = game.exp_bits;
		if (user->id != 0) {
			const std::string &account_status = user->account_status;
			if (account_status == "banned") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld banned\n",
				            static_cast<long long>(user->id));
				return render_login_message("This NovaWorld account has been banned. NWEC11");
			}
			if (account_status != "active") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld status=%s\n",
						static_cast<long long>(user->id), loggable(account_status).c_str());
				return render_login_message("This NovaWorld account is restricted. NWEC12");
			}

			const auto access = get_game_access(*db_conn, user->id, game_slug);
			if (!access) {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld no access to %s\n",
				            static_cast<long long>(user->id), game_slug.c_str());
				return render_login_message("This NovaWorld account does not have access to this game.");
			}
			if (access->status == "banned") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld game=%s banned\n",
				            static_cast<long long>(user->id), game_slug.c_str());
				return render_login_message("This NovaWorld account has been banned for this game. NWEC11");
			}
			if (access->status != "active") {
				std::printf("[http] POST /NWLogin.dll rejected: user %lld game=%s status=%s\n",
						static_cast<long long>(user->id), game_slug.c_str(),
						loggable(access->status).c_str());
				return render_login_message("This NovaWorld account is restricted for this game. NWEC12");
			}
			if (!access->exp_bits.empty()) {
				effective_exp_bits = access->exp_bits;
			}
			// Re-login supersedes a prior session rather than being rejected.
			// Retail has no witnessed "already logged in" gate — the active-user
			// row is cleared on /NWLogout.dll, not used to block login
			// (docs/net/novaworld-net-re.md:55). A hard reject here stranded the
			// account whenever a prior session ended without a clean logout
			// (client crash / GOODBYE-less exit): the orphaned row blocked every
			// retry until the 2 h prune. register_active_user_session() below
			// is an upsert on the user_id PK, so the stale row is simply
			// overwritten by this fresh login.
		}
		// The login's session row and its pin are written only while the account
		// is active and still at the credentials generation its password check
		// (or pin) read, the row tested by its own insert, so a reset or a ban
		// that committed meanwhile leaves this login neither, and the reply is a
		// bad password's. A pin stored after such a commit still carries the old
		// generation, which pinned_user refuses.
		bool registered = false;
		try {
			registered = register_active_user_session(*db_conn, user->id, user->credentials_gen,
					user->username, s.session_tag, persist_cookie, req.remote_ip_address,
					req.get_header_value("User-Agent"));
		} catch (const std::exception &e) {
			std::fprintf(
					stderr, "[http] WARN active_user_sessions register failed: %s\n", e.what());
			return render_login_message("NovaWorld is temporarily unavailable.");
		}
		if (!registered) {
			std::fprintf(stderr,
					"[http] POST /NWLogin.dll rejected: user %lld changed during the check\n",
					static_cast<long long>(user->id));
			return render_login_message("Invalid username or password");
		}
		if (!persist_cookie.empty())
			pins_.put(persist_cookie, RetailPin{ user->id, user->credentials_gen });
		s.user_id  = user->id;
		s.username = user->username;
		s.pcid     = user->pcid;
		s.nwh      = user->nwh;
		s.nwhandle = user->nwhandle;
		s.exp_bits = effective_exp_bits;
		std::printf(
				"[http]   user resolved (%s): id=%lld username=%s pcid=%s nwhandle=%s persist=%s\n",
				resolution ? resolution : "?", static_cast<long long>(s.user_id),
				loggable(s.username).c_str(), loggable(s.pcid).c_str(),
				loggable(s.nwhandle).c_str(), loggable_secret(persist_cookie).c_str());

		// The relay template renders this response only; nothing reads it back
		// from the stored session.
		const std::string relay_template = field_or("relay", game.page("relay.htm"));
		s.msgbase      = field_or("msgbase",     game.page("msg.htm"));
		s.success      = field_or("success",     game.page("main.htm"));
		s.failure      = field_or("failure",     game.page("main.htm"));

		const std::string tag = s.session_tag;

		sessions_.put_login(tag, s);

		std::printf("[http] POST %s body=%zuB user=%s relay->%s success->%s -> tag %s\n",
				loggable(req.url).c_str(), req.body.size(), loggable(s.username).c_str(),
				loggable(relay_template).c_str(), loggable(s.success).c_str(),
				loggable_secret(tag).c_str());

		// Relay refresh target is the BARE "NWLogin.dll" — matching onnet AND
		// the genuine NovaLogic server (Wireshark capture, docs/net/
		// novaworld-net-re.md:1303-1352: retail polls plain `GET /NWLogin.dll`
		// and its LOGINSESSIONTAG cookie round-trips, then the server plants
		// NWHANDLE/PCID). The prior `?tag=` workaround (for a supposed HTTP/1.0
		// cookie drop the capture shows the real client does NOT have on the
		// bare URL) re-routed the completion GET so retail never stored the
		// identity cookies — they never reached /NWJoin.dll, leaving PUBPCID
		// empty ("login information is absent (GDC024)"). The handler reads the
		// tag from the LOGINSESSIONTAG cookie alone.
		TemplateVars vars{
			{"MESSAGE",          "Contacting login databases..."},
			{"REFRESH_ENDPOINT", "NWLogin.dll"},
			{"HOST_URL",         host_url(game)},
			{"GSB_SERVER",       gsb_url(game)},
			{"JOINLAN_URL",      game.join_lan_url},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, relay_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",         req.remote_ip_address);
		add_cookie(res, "LOGINSESSIONTAG", tag);
		// Empty placeholders the relay GET will populate after auth.
		for (const char *name : {"NWH","NWHANDLE","CHAR","NWI","NWV","NWD","STATSDISPLAYCHID","PCID"}) {
			add_cookie(res, name, "");
		}
		add_cookie(res, "EXPBITS", effective_exp_bits);
		return res;
	};
	CROW_ROUTE(app, "/NWLogin.dll").methods("POST"_method)(handle_login_post);
	CROW_ROUTE(app, "/nwlogin.dll").methods("POST"_method)(handle_login_post);

	// GET /NWLogin.dll  (relay refresh — completes auth)
	auto handle_login_get = [this, templates_dir](const crow::request &req) {
		// The relay page's bare refresh: the LOGINSESSIONTAG cookie carries the tag
		// (net/novaworld/relay_request.h).
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		std::string tag;
		if (auto tag_it = cookies.find("LOGINSESSIONTAG"); tag_it != cookies.end()) tag = tag_it->second;
		if (tag.empty()) {
			std::printf("[http] GET %s -> 400 (no LOGINSESSIONTAG cookie)\n",
					loggable(req.url).c_str());
			crow::response res(400);
			res.body = "missing LOGINSESSIONTAG cookie";
			return res;
		}
		auto session = sessions_.get_login(tag);
		if (!session) {
			// Retail can land here with a tag from a previous server run
			// (cached in PERSISTENTEXPRESSLOGINDATA / cookie jar). Rather
			// than 400 → blank screen, mint a fresh login session and
			// round-robin a dev user. Log it loudly so the cookie bridge
			// is visible.
			std::printf("[http] GET %s WARN stale/unknown tag '%s' — minting fresh login session\n",
					loggable(req.url).c_str(), loggable_secret(tag).c_str());
			// Mint a fresh, UNAUTHENTICATED session so retail doesn't hit a
			// blank screen on a cross-restart tag — but it carries no user.
			// The POST /NWLogin.dll path is the only auth gate; a stale tag
			// must re-authenticate there rather than be silently re-bound to
			// an account here. (user_id stays 0 → no active session, and any
			// authenticated action downstream still requires a real login.)
			const catalog::GameRow &fresh_game = client_game(impl_->games, req, cookies, {}, {});
			// It ends with this reply (the erase below), so it is never stored.
			LoginSession fresh;
			fresh.session_tag = sessions_.generate_tag("NWLogin.dll");
			fresh.success = fresh_game.page("main.htm");
			fresh.failure = fresh_game.page("main.htm");
			fresh.msgbase = fresh_game.page("msg.htm");
			tag = fresh.session_tag;
			session = std::move(fresh);
		}

		std::printf("[http] GET %s tag=%s -> auth ok (user=%s pcid=%s)\n",
				loggable(req.url).c_str(), loggable_secret(tag).c_str(),
				loggable(session->username).c_str(), loggable(session->pcid).c_str());
		if (session->user_id != 0) {
			try { touch_active_user_session(*db_pool_.acquire(), session->user_id); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions touch failed: %s\n",
				             e.what());
			}
		}

		// The main page this renders lists the client's game's servers.
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {}, session->success);
		TemplateVars vars{
			{"IN",           session->success},
			{"OUT",          session->failure},
			{"MSGBASE",      session->msgbase},
			{"HOST_URL",     host_url(game)},
			{"GSB_SERVER",   gsb_url(game)},
			{"JOINLAN_URL",  game.join_lan_url},
		};
		const std::string success_template = session->success.empty() ? game.page("main.htm") : session->success;
		crow::response res(200);
		res.body = render_template_file(templates_dir, success_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",          req.remote_ip_address);
		add_cookie(res, "LOGINSESSIONTAG", tag);
		add_cookie(res, "NWH",      session->nwh.empty()      ? "1"          : session->nwh);
		add_cookie(res, "NWHANDLE", session->nwhandle.empty() ? "DevUser"    : session->nwhandle);
		add_cookie(res, "CHAR",     session->nwhandle.empty() ? "DevUser"    : session->nwhandle);
		add_cookie(res, "PCID",     session->pcid.empty()     ? "00000001"   : session->pcid);
		add_cookie(res, "EXPBITS",  session->exp_bits.empty() ? "3" : session->exp_bits);
		// A DFX2 client whose NWCDKIID is no wire key (a keyless copy, as sold or byte-patched):
		// its machine's NWACT, which its NovaWorld screen writes to the registry CDKIID
		// (cd_key_routes.h), handed silently (D-NET-725).
		const std::string nwact = impl_->cd_keys.login_activation(session->user_id, game, cookies);
		if (!nwact.empty())
			add_cookie(res, "NWACT", nwact);

		// Echo the client's persistent/express-login cookies back so retail's
		// logged-in state stays valid and these survive to later requests —
		// notably the joiner's identity at /NWJoin.dll. Without this the joiner
		// fails with "Logging information is missing or invalid" (its PUB* join
		// cookies come out empty because no identity carried over). Echo only
		// what the client sent. Mirrors onnw/controllers/nova_world/login.py:159-171.
		{
			const auto req_cookies = parse_cookie_header(request_cookie_header(req));
			for (const char *name : {"PERSISTENTREMEMBERLOGINDATA",
			                         "PERSISTENTEXPRESSLOGINDATA",
			                         "NWI", "NWV", "NWD", "STATSDISPLAYCHID"}) {
				auto it = req_cookies.find(name);
				if (it != req_cookies.end()) add_cookie(res, name, it->second);
			}
		}

		sessions_.erase_login(tag);
		return res;
	};
	CROW_ROUTE(app, "/NWLogin.dll").methods("GET"_method)(handle_login_get);
	CROW_ROUTE(app, "/nwlogin.dll").methods("GET"_method)(handle_login_get);

	// ----- /NWLogout.dll: the two-call relay ---------------------------------
	// The live service's logout is a relay, as its login, join and host are (LN's S1 / S1b / S1d
	// records, 2026-10-23): the page link's call (success, failure, msgbase, relay, nodb) is
	// answered with the relay page and a LOGOUTSESSIONTAG (YOURIP and LOGOUTSESSIONTAG its only
	// cookies), and the page's bare refresh, the tag on its cookie, with the success page (the
	// login page) whose cookies are YOURIP and the cleared NWH, NWHANDLE, CHAR, NWI, NWV, NWD,
	// STATSDISPLAYCHID and PERSISTENTEXPRESSLOGINDATA. The relay's legs are told apart as NWJoin's
	// and NWHost's are (net/novaworld/relay_request.h). The refresh drops the HTTP-side login
	// session and the persistent-cookie pin, so the process resumes as no one; the UDP side keeps
	// running until its goodbye or the reap (the protocol's contract). The relay page's message is
	// this service's text (the live one is unrecorded).
	auto handle_logout = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		std::string logout_tag;
		if (auto it = cookies.find("LOGOUTSESSIONTAG"); it != cookies.end()) logout_tag = it->second;

		if (classify_relay_request(has_relay_first_call_query(req), !logout_tag.empty()) ==
				RelayLeg::First) {
			const char *success_param = req.url_params.get("success");
			const catalog::GameRow &game = client_game(impl_->games, req, cookies, {},
			                                           success_param ? success_param : "");
			LogoutSession s;
			s.session_tag = sessions_.generate_tag("NWLogout.dll");
			s.success = success_param ? success_param : game.page("login.htm");
			s.failure = req.url_params.get("failure") ? req.url_params.get("failure") : game.page("main.htm");
			s.msgbase = req.url_params.get("msgbase") ? req.url_params.get("msgbase") : game.page("msg.htm");
			const std::string relay_template =
					req.url_params.get("relay") ? req.url_params.get("relay") : game.page("relay.htm");
			sessions_.put_logout(s.session_tag, s);
			std::printf("[http] /NWLogout.dll relay -> tag %s success->%s\n",
					loggable_secret(s.session_tag).c_str(), loggable(s.success).c_str());

			TemplateVars vars{
				{"MESSAGE",          "Logging out..."},
				{"REFRESH_ENDPOINT", "NWLogout.dll"},
				{"HOST_URL",         host_url(game)},
				{"GSB_SERVER",       gsb_url(game)},
				{"JOINLAN_URL",      game.join_lan_url},
			};
			crow::response res(200);
			res.body = render_template_file(templates_dir, relay_template, vars);
			res.set_header("Content-Type", "text/html");
			add_standard_headers(res);
			add_cookie(res, "YOURIP", req.remote_ip_address);
			add_cookie(res, "LOGOUTSESSIONTAG", s.session_tag);
			return res;
		}

		// The relay's bare refresh: the logout itself, once per tag.
		const std::optional<LogoutSession> relay = sessions_.take_logout(logout_tag);
		std::string tag;
		if (auto it = cookies.find("LOGINSESSIONTAG"); it != cookies.end()) tag = it->second;
		if (!tag.empty()) {
			sessions_.erase_login(tag);
			try { clear_active_user_session_by_tag(*db_pool_.acquire(), tag); }
			catch (const std::exception &e) {
				std::fprintf(stderr, "[http] WARN active_user_sessions clear by tag failed: %s\n",
				             e.what());
			}
		}

		const auto persist_it = cookies.find("PERSISTENTEXPRESSLOGINDATA");
		if (persist_it != cookies.end() && !persist_it->second.empty()) {
			const auto pin = pins_.take(persist_it->second);
			const int64_t pinned_id = pin ? pin->user_id : 0;
			if (pinned_id != 0) {
				try { clear_active_user_session(*db_pool_.acquire(), pinned_id); }
				catch (const std::exception &e) {
					std::fprintf(stderr, "[http] WARN active_user_sessions clear failed: %s\n",
					             e.what());
				}
			}
		}

		const std::string named = relay ? relay->success : std::string();
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {}, named);
		const std::string success_tpl = named.empty() ? game.page("login.htm") : named;
		std::printf("[http] /NWLogout.dll tag=%s persist_dropped=%d -> %s\n",
				loggable_secret(tag).c_str(), persist_it != cookies.end() ? 1 : 0,
				loggable(success_tpl).c_str());

		TemplateVars vars{
			{"HOST_URL",    host_url(game)},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", game.join_lan_url},
		};
		crow::response res(200);
		if (template_file(templates_dir, success_tpl)) {
			res.body = render_template_file(templates_dir, success_tpl, vars);
			res.set_header("Content-Type", "text/html");
		} else {
			res.body = "Logged out.";
			res.set_header("Content-Type", "text/plain");
		}
		add_standard_headers(res);
		add_cookie(res, "YOURIP", req.remote_ip_address);
		for (const char *name : {"NWH", "NWHANDLE", "CHAR", "NWI", "NWV", "NWD", "STATSDISPLAYCHID",
		                          "PERSISTENTEXPRESSLOGINDATA"}) {
			add_cookie(res, name, "");
		}
		return res;
	};
	app.route_dynamic("/NWLogout.dll")(handle_logout);
	app.route_dynamic("/nwlogout.dll")(handle_logout);

	// Phase I.6: per-user template renderer for /NWCharacter.dll and
	// /NWAccount.dll. Both are retail's "tell me about my account/char"
	// queries; onnet doesn't even route them but retail's IB3 expects
	// them to return a template populated with the current user's
	// identity (so the in-game UI can show "Logged in as X").
	//
	// Identity is the request's own retail login (retail_login_user: its
	// LOGINSESSIONTAG, else its PERSISTENTEXPRESSLOGINDATA pin), an active
	// account only; never the NWHANDLE cookie, which names an account but
	// proves nothing.
	auto handle_generic = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		const char *success_param = req.url_params.get("success");
		const catalog::GameRow &game = client_game(impl_->games, req, cookies, {},
		                                           success_param ? success_param : "");
		const std::string tpl = success_param ? success_param : game.page("main.htm");
		const bool exists = template_file(templates_dir, tpl).has_value();

		// Resolve identity. Best-effort — if neither cookie resolves we
		// just render with empty identity vars (the template can still
		// be served for unauthenticated paths).
		auto db_conn = db_pool_.acquire();
		std::optional<UserRecord> user;
		try {
			const char *how = "";
			user = retail_login_user(cookies, *db_conn, how);
		} catch (const db::Error &e) {
			std::fprintf(stderr, "[http] WARN retail login lookup failed: %s\n", e.what());
		}
		if (user && user->account_status != "active")
			user.reset();
		std::printf("[http] %s -> %s%s user=%s\n", loggable(req.url).c_str(), loggable(tpl).c_str(),
				exists ? "" : " (MISSING — 404)",
				user ? loggable(user->username).c_str() : "(unknown)");
		if (!exists) {
			crow::response res(404);
			// The name is echoed only when it is one a template may have.
			res.body = "template not found: " + (is_bare_file_name(tpl) ? tpl : std::string());
			res.set_header("Content-Type", "text/plain");
			return res;
		}
		TemplateVars vars{
			{"HOST_URL",    host_url(game)},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", game.join_lan_url},
			{"NWHANDLE",    user ? user->nwhandle : std::string()},
			{"PCID",        user ? user->pcid     : std::string()},
			{"NWH",         user ? user->nwh      : std::string()},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, tpl, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP", req.remote_ip_address);
		// Refresh identity cookies so retail keeps consistent state.
		if (user) {
			add_cookie(res, "NWHANDLE", user->nwhandle);
			add_cookie(res, "CHAR",     user->nwhandle);
			add_cookie(res, "PCID",     user->pcid);
			add_cookie(res, "NWH",      user->nwh);
		}
		return res;
	};
	for (const char *route : {"/NWCharacter.dll","/nwcharacter.dll",
	                          "/NWAccount.dll","/nwaccount.dll"}) {
		app.route_dynamic(route)(handle_generic);
	}
}

} // namespace opennova::novaworld_server
