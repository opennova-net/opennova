// The retail host and join flow's routes (HttpListener::register_legacy_host_join_routes):
// the per-game GSB browser blobs, /NWJoin.dll's two-phase relay and /NWHost.dll's host-key
// mint. Split out of http_listener.cpp whole.

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
#include <string>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server {

// ----- Phase E.2: GSB (Game Server Browser) ---------------------------
// Build a GSB binary blob from the listed hosts (active_hosts: every
// connection that issued ClientHostRequest in the lobby session). Wire
// format from engine/net/novaworld/gsb.h is byte-exact with
// onnet's onnw/gsb.py, so retail's IB3 browser parser accepts it.
//
// Each games row's list, at its gsb_path (/jop_2.gsb, /dfx2_0.gsb,
// /acidgambit_0.gsb, an account's own game's), served through the catch-all
// route (register_static_routes) from the catalogue as it stands
// (games_now), listing the hosts registered under its LobbyName; a column a
// host reported nothing for shows its game's VER1 / Expbits.
crow::response HttpListener::gsb_reply(const catalog::GameRow &game) {
	// Phase I.2: backed by active_hosts, filtered to the requested game
	// (onnet serves each *.gsb from a per-game query — without the filter
	// a DFX2 host would leak into the JO browser and vice versa).
	std::vector<opennova::GsbServerEntry> entries;
	try {
		auto db_conn = db_pool_.acquire();
		// One snapshot for the rows and their rosters: a row's player count
		// and the names in its tail come from the same commit. Two queries
		// however many hosts are listed: the rows, then every roster.
		db::ReadSnapshot snapshot(*db_conn);
		auto rows = hostdb::list_hosts_by_game(*db_conn, game.lobby_name);
		std::vector<uint32_t> rids;
		rids.reserve(rows.size());
		for (const auto &h : rows)
			rids.push_back(h.rid);
		const auto rosters = hostdb::list_rosters(*db_conn, rids);
		const std::vector<HostRosterSlot> no_roster;
		entries.reserve(rows.size());
		for (const auto &h : rows) {
			// Every FLDS column carries the host-reported value and the row
			// tail carries the host's roster names (hostdb::gsb_entry_from_host).
			const auto roster = rosters.find(h.rid);
			entries.push_back(hostdb::gsb_entry_from_host(h,
					roster == rosters.end() ? no_roster : roster->second,
					{ game.ver1, game.exp_bits }));
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GSB list failed: %s\n", e.what());
	}
	const auto blob = opennova::gsb_build_response(entries);
	std::printf("[http] GET %s (%s) -> %zu hosts, %zu bytes\n", game.gsb_route().c_str(),
			game.slug.c_str(), entries.size(), blob.size());
	crow::response res(200);
	res.set_header("Content-Type", "application/octet-stream");
	res.body.assign(blob.begin(), blob.end());
	return res;
}

// Retail host/join flow: the /NWJoin.dll two-phase relay and the /NWHost.dll
// host-key mint (the per-game GSB binary browser blobs are gsb_reply's,
// through the catch-all). Join and host register both lowercase and
// Title-case spellings for the same reason as the login chain: Crow routes
// are case-sensitive.
void HttpListener::register_legacy_host_join_routes(
		const std::string &templates_dir) {
	auto &app = impl_->app;

	// ----- Phase E.3: /NWJoin.dll two-phase relay -------------------------
	// Mirror onnet's onnw/controllers/nova_world/join.py.
	//   First call (no NWJOINSESSIONTAG cookie):  store params -> render
	//                                             relay template, set
	//                                             session cookie.
	//   Second call (with cookie):                look up session, find
	//                                             hosted entry by RID, encode
	//                                             NK/CK, render .joi
	//                                             success template.
	auto handle_join = [this, templates_dir](const crow::request &req) {
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		// The relay page's bare refresh carries the tag in the NWJOINSESSIONTAG cookie.
		std::string tag_from_request;
		if (auto tag_it = cookies.find("NWJOINSESSIONTAG"); tag_it != cookies.end())
			tag_from_request = tag_it->second;

		// The page link's query (rid=NNN and the page fields) is the first call
		// even when the client's persistent jar still carries the tag a previous
		// join or host left: a stock client sends every jar cookie on every GET
		// (net/novaworld/relay_request.h). Only the relay page's bare refresh
		// resolves a tag.
		const bool first_call =
				classify_relay_request(has_relay_first_call_query(req), !tag_from_request.empty()) ==
				RelayLeg::First;

		if (first_call) {
			const char *pfid_param = req.url_params.get("pfid");
			const char *success_param = req.url_params.get("success");
			const auto games = games_now();
			const catalog::GameRow &game = client_game(*games, req, cookies,
					pfid_param ? pfid_param : "", success_param ? success_param : "");
			JoinSession s;
			s.session_tag = sessions_.generate_tag("NWJoin.dll");
			s.success     = success_param                    ? success_param                    : game.page("join.joi");
			s.failure     = req.url_params.get("failure")    ? req.url_params.get("failure")    : game.page("main.htm");
			const std::string relay_template =
					req.url_params.get("relay") ? req.url_params.get("relay") : game.page("relay.htm");
			s.msgbase     = req.url_params.get("msgbase")    ? req.url_params.get("msgbase")    : game.page("msg.htm");
			s.needexpkey  = req.url_params.get("needexpkey") ? req.url_params.get("needexpkey") : "";
			s.pfid        = req.url_params.get("pfid")       ? req.url_params.get("pfid")       : "";
			s.rid         = req.url_params.get("rid")        ? req.url_params.get("rid")        : "";
			s.gsid        = req.url_params.get("gsid")       ? req.url_params.get("gsid")       : "";
			s.lan         = req.url_params.get("lan")        ? req.url_params.get("lan")        : "";
			const std::string tag = s.session_tag;
			sessions_.put_join(tag, s);

			std::printf(
					"[http] /NWJoin.dll (first call) rid=%s success=%s -> tag %s cookies=[%s]\n",
					req.url_params.get("rid") ? loggable(req.url_params.get("rid")).c_str()
											  : "(none)",
					req.url_params.get("success") ? loggable(req.url_params.get("success")).c_str()
												  : "(default)",
					loggable_secret(tag).c_str(),
					cookie_summary(request_cookie_header(req)).c_str());

			// Bare "NWJoin.dll" refresh — matches onnet and the genuine server;
			// the NWJOINSESSIONTAG cookie carries the tag (see the /NWLogin.dll
			// relay note above re: dropping the divergent `?tag=` workaround).
			TemplateVars vars{
				{"MESSAGE",          "Contacting game server...."},
				{"REFRESH_ENDPOINT", "NWJoin.dll"},
				{"HOST_URL",         host_url(game)},
				{"GSB_SERVER",       gsb_url(game)},
				{"JOINLAN_URL",      game.join_lan_url},
			};
			crow::response res(200);
			res.body = render_template_file(templates_dir, relay_template, vars);
			res.set_header("Content-Type", "text/html");
			add_standard_headers(res);
			add_cookie(res, "YOURIP",            req.remote_ip_address);
			add_cookie(res, "NWJOINSESSIONTAG",  tag);
			return res;
		}

		// Second call — the tag came from the NWJOINSESSIONTAG cookie.
		// Diagnostic (join-failure triage): the full inbound cookie set —
		// LOGINSESSIONTAG / PERSISTENTEXPRESSLOGINDATA decide the joiner identity
		// that PUBPCID is encoded from.
		std::printf("[http] /NWJoin.dll (second call) tag=%s cookies=[%s]\n",
				loggable_secret(tag_from_request).c_str(),
				cookie_summary(request_cookie_header(req)).c_str());
		const auto session = sessions_.get_join(tag_from_request);
		if (!session) {
			std::printf("[http] /NWJoin.dll (second call) WARN unknown tag %s\n",
					loggable_secret(tag_from_request).c_str());
			crow::response res(400);
			res.body = "unknown NWJOINSESSIONTAG";
			return res;
		}

		// The joiner's game: the row its page link names.
		const auto games = games_now();
		const catalog::GameRow &game =
				client_game(*games, req, cookies, session->pfid, session->success);

		// Resolve RID — query param wins, falls back to stored value. A LAN-list
		// join's link (the main page's @JOINLAN_URL@, the games row's join_lan_url)
		// names the server by its GSID instead: the 0x81 SUS1 the client's LAN
		// discovery found it by, which is the GSID this service's ServerHostResult
		// handed that host (service choice: the page answers the link as a join of
		// that listing).
		// [orig: UI_DispatchScreenEvent @0x54B920 (dfx2.exe) case 12 - the GSID stored
		//  @0x54B9AA, @JOINLAN_URL@ read @0x54B9BD, @GSID@ replaced from it
		//  @0x54B9E1..0x54B9F0, UIScene_OpenUrl @0x54BA76]
		// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4D5BC0 (dfx2.exe) - "GSID"
		//  @0x4D5DB3 -> the protocol's server_user_string1 @0x4D5DFF]
		// [orig: CNapiNetwork_OnSessionDiscovered @0x4C8580 (dfx2.exe) - the 0x81's SUS1
		//  into the record @0x4C86CD, the row @0x4C87D4]
		const std::string rid_str = req.url_params.get("rid") ? req.url_params.get("rid") : session->rid;
		const std::string gsid = req.url_params.get("gsid") ? req.url_params.get("gsid") : session->gsid;
		if (rid_str.empty() && gsid.empty()) {
			crow::response res(400);
			res.body = "missing RID";
			return res;
		}
		std::optional<uint32_t> rid_value;
		if (!rid_str.empty()) {
			const auto rid_parsed = opennova::strutil::parse_ulong(rid_str);
			if (!rid_parsed) {
				crow::response res(400);
				res.body = "bad RID";
				return res;
			}
			rid_value = static_cast<uint32_t>(*rid_parsed);
		}

		// Look up the hosted entry from active_hosts (Phase I.2 — DB-backed).
		auto db_conn = db_pool_.acquire();
		auto host_row = rid_value ? hostdb::find_host_by_rid(*db_conn, *rid_value)
		                          : hostdb::find_host_by_gsid(*db_conn, gsid);
		if (!host_row && !rid_value) {
			// A GSID no listing holds (the host stopped hosting since the LAN
			// discovery found it): the msgbase page, which a stock client shows, its
			// title raising no join.
			// [orig: UI_ProcessWebResponseContent @0x6CA6C0 (dfx2.exe) - the title test
			//  @0x6CA835..0x6CA874, event 7 only for "[NK=" / "[HOSTKEY=" (the raise
			//  @0x6CA86A)]
			std::fprintf(stderr, "[http] /NWJoin.dll GSID %s not in active_hosts\n",
			             loggable(gsid).c_str());
			return render_legacy_message(templates_dir, kLanGameGoneMessage,
					session->failure.empty() ? game.page("main.htm") : session->failure,
					session->msgbase.empty() ? game.page("msg.htm") : session->msgbase,
					req.remote_ip_address, host_url(game), gsb_url(game));
		}
		if (!host_row) {
			std::fprintf(stderr, "[http] /NWJoin.dll RID %u not in active_hosts\n",
			             static_cast<unsigned>(*rid_value));
			crow::response res(404);
			res.body = "host not found";
			return res;
		}
		const auto &host = *host_row;
		// The LAN-list join's .joi: LN nonzero, so the client dials the address its
		// LAN discovery found (not NK's), and no NI / NP / BK, so no proxy rides the
		// LAN dial; NK stays, the parse refusing a .joi without it (service choice to
		// the client's contract).
		// [orig: URL_ParseConnectionQueryString @0x54B230 (dfx2.exe) - NK= @0x54B3BC,
		//  LN= @0x54B56A, its atol @0x54B5BE]
		// [orig: CNapiGameSession_InitTransportConnection @0x4C9F80 (dfx2.exe) - the LN
		//  dial @0x4C9FD5..0x4C9FED, the proxy only with NI, NP and BK all set: the tests
		//  @0x4CA192..0x4CA1B0]
		const auto lan_number = opennova::strutil::parse_int(
				req.url_params.get("lan") ? std::string(req.url_params.get("lan")) : session->lan);
		const bool lan_join = lan_number && *lan_number != 0;

		// _encode_token (from base.py:113-116):
		//   chr(ord(value[i]) + ord(key[i]) - 48)
		// NOVAKEY_KEY/CK_KEY are 21 chars; encoded values must fit.
		static constexpr const char *NOVAKEY_KEY = "diheijefhgcdjcgcjcfbd";
		static constexpr const char *CK_KEY      = "cfhdcegjigecjehcgjdhe";
		static constexpr const char *BK_VALUE    = "986119";
		auto encode_token = [](const std::string &value, const char *key) -> std::string {
			std::string out;
			const size_t key_len = std::strlen(key);
			out.reserve(value.size());
			for (size_t i = 0; i < value.size() && i < key_len; ++i) {
				out.push_back(static_cast<char>(value[i] + key[i] - 48));
			}
			return out;
		};

		const std::string nk_plain = host.host_ip + ":" + std::to_string(host.host_port);
		const std::string ck_plain = host.app_id;
		const std::string nk_token = encode_token(nk_plain, NOVAKEY_KEY);
		const std::string ck_token = encode_token(ck_plain, CK_KEY);

		std::printf("[http] /NWJoin.dll (second call) rid=%u host=%s:%d nk=%s ck=%s%s\n",
				static_cast<unsigned>(host.rid), loggable(host.host_ip).c_str(), host.host_port,
				loggable(nk_token).c_str(), loggable(ck_token).c_str(), lan_join ? " lan" : "");

		// The listing's game: the row its LobbyName names (the joiner's for a
		// listing with none; null for a LobbyName no row has, which no game's
		// access covers).
		const catalog::GameRow *host_game =
				host.game.empty() ? &game : games->by_lobby_name(host.game);
		TemplateVars vars{
			{"NK",          nk_token},
			{"CK",          ck_token},
			{"NI",          lan_join ? std::string() : host.host_ip},
			{"NP",          lan_join ? std::string() : std::to_string(host.host_port)},
			{"BK",          lan_join ? std::string() : std::string(BK_VALUE)},
			{"LAN",         lan_join ? "LN=1&" : ""},
			{"SERVER_NAME", host.server_name.empty() ? std::string("OpenNova Server") : host.server_name},
			{"HOST_URL",    host_url(game)},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", game.join_lan_url},
		};
		const std::string success_template = session->success.empty() ? game.page("join.joi") : session->success;
		crow::response res(200);
		res.body = render_template_file(templates_dir, success_template, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",           req.remote_ip_address);
		add_cookie(res, "NWJOINSESSIONTAG", tag_from_request);

		// PUBcrypto-encoded join cookies (Phase E.4). Retail host-side join
		// reads CD/<localaddr>{PCID,NAMEINFO,SQUADINFO}; SQUADINFO supplies
		// the remote player's display/short names before validation.
		// The joiner is the request's own retail login (retail_login_user: its
		// LOGINSESSIONTAG, else the PERSISTENTEXPRESSLOGINDATA pin its login
		// made, G.8), never the NWHANDLE cookie, which any client can set to
		// any account's handle. An account that is not active is refused with
		// the failure page a missing expansion gets, carrying the game login's
		// NWEC11 / NWEC12 message.
		std::string pub_pcid;
		std::string pub_nameinfo;
		std::string pub_squadinfo;
		std::string pub_join_ticket;
		const std::string host_pcid_key = host.pcid_key;
		std::string joiner_pcid;
		std::string joiner_nwhandle;
		int64_t     joiner_user_id = 0;
		int64_t     joiner_credentials_gen = 0; // the generation its retail login was read under
		std::string joiner_label;  // for logging
		{
			const std::string msg_tpl =
					session->msgbase.empty() ? game.page("msg.htm") : session->msgbase;
			const std::string fail_tpl =
					session->failure.empty() ? game.page("main.htm") : session->failure;
			std::optional<UserRecord> joiner;
			const char *how = "";
			try {
				joiner = retail_login_user(cookies, *db_conn, how);
			} catch (const db::Error &e) {
				std::fprintf(
						stderr, "[http] /NWJoin.dll WARN joiner lookup failed: %s\n", e.what());
			}
			if (joiner && joiner->account_status != "active") {
				std::printf("[http] /NWJoin.dll rejected: joiner=%lld status=%s\n",
						static_cast<long long>(joiner->id),
						loggable(joiner->account_status).c_str());
				return render_legacy_message(templates_dir,
						joiner->account_status == "banned"
								? "This NovaWorld account has been banned. NWEC11"
								: "This NovaWorld account is restricted. NWEC12",
						fail_tpl, msg_tpl, req.remote_ip_address, host_url(game), gsb_url(game));
			}
			if (joiner) {
				joiner_user_id = joiner->id;
				joiner_credentials_gen = joiner->credentials_gen;
				joiner_pcid = joiner->pcid;
				joiner_nwhandle = joiner->nwhandle;
				joiner_label = std::string(how) + "=" + joiner->nwhandle;
			}
			if (joiner_user_id != 0) {
				const std::string host_slug = host_game ? host_game->slug : host.game;
				const std::string required_exp_bits = !host.exp_bits.empty() ? host.exp_bits
					: host_game ? host_game->exp_bits : std::string();
				const auto access = get_game_access(*db_conn, joiner_user_id, host_slug);
				const bool access_ok = access && access->status == "active" &&
					expansion_bits_compatible(access->exp_bits, required_exp_bits);
				if (!access_ok) {
					std::printf("[http] /NWJoin.dll rejected: joiner=%lld game=%s owned_exp=%s "
								"required_exp=%s status=%s\n",
							static_cast<long long>(joiner_user_id), loggable(host_slug).c_str(),
							access ? loggable(access->exp_bits).c_str() : "(none)",
							loggable(required_exp_bits).c_str(),
							access ? loggable(access->status).c_str() : "(none)");
					return render_legacy_message(templates_dir,
							"This NovaWorld account does not have the required expansion key.",
							session->needexpkey.empty() ? fail_tpl : session->needexpkey, msg_tpl,
							req.remote_ip_address, host_url(game), gsb_url(game));
				}
			}
			if (host_pcid_key.empty()) {
				std::fprintf(stderr, "[http] /NWJoin.dll WARN host pcid_key missing - PUB* cookies will be empty (joiner cookies=[%s])\n",
				             cookie_summary(request_cookie_header(req)).c_str());
			} else if (joiner_pcid.empty()) {
				std::fprintf(stderr, "[http] /NWJoin.dll WARN no joiner identity - PUB* cookies will be empty (joiner cookies=[%s])\n",
				             cookie_summary(request_cookie_header(req)).c_str());
			} else {
				const auto payloads =
					opennova::build_pub_join_identity_plaintexts(joiner_pcid, joiner_nwhandle);
				// A refused encode (only an empty key is refused, ruled out above)
				// leaves every PUB* cookie empty, as a missing host key does.
				const bool encoded =
					opennova::encode_pub_value(payloads.pcid, host_pcid_key, pub_pcid) &&
					(payloads.name_info.empty() ||
					 opennova::encode_pub_value(payloads.name_info, host_pcid_key, pub_nameinfo)) &&
					(payloads.squad_info.empty() ||
					 opennova::encode_pub_value(payloads.squad_info, host_pcid_key, pub_squadinfo));
				if (!encoded) {
					pub_pcid.clear();
					pub_nameinfo.clear();
					pub_squadinfo.clear();
					std::fprintf(stderr, "[http] /NWJoin.dll PUB* encode refused (host_key=%zuB)\n",
					             host_pcid_key.size());
				} else {
					if (payloads.name_info.empty() || payloads.squad_info.empty()) {
						std::fprintf(stderr,
								"[http] /NWJoin.dll WARN no joiner display handle - "
								"PUBNAMEINFO/PUBSQUADINFO will be empty (joiner=%s cookies=[%s])\n",
								loggable(joiner_label).c_str(),
								cookie_summary(request_cookie_header(req)).c_str());
					}
					std::printf("[http] /NWJoin.dll PUB* encoded for joiner=%s pcid=%s nwhandle=%s host_key=%zuB name=%zuB squad=%zuB\n",
					            loggable(joiner_label).c_str(), loggable(joiner_pcid).c_str(),
					            loggable(joiner_nwhandle).c_str(), host_pcid_key.size(),
					            payloads.name_info.size(), payloads.squad_info.size());
				}
			}
			// Phase I.3: persist the join in host_players, the listing's roster,
			// keyed by the joiner's account. Its lobby session, signed in as the
			// same account, takes it off when it stops playing there, plays
			// elsewhere or ends (hostdb::remove_player); it cascades when the
			// host row goes away (host disconnect).
			if (joiner_user_id != 0) {
				try {
					hostdb::PlayerRow p;
					p.host_rid  = host.rid;
					p.user_id   = joiner_user_id;
					p.nwhandle  = joiner_nwhandle.empty()
					                ? joiner_label
					                : joiner_nwhandle;
					p.peer_ip   = req.remote_ip_address;
					hostdb::add_player(*db_conn, p);
					pub_join_ticket = hostdb::mint_join_ticket(
							*db_conn, host.rid, p.user_id, joiner_credentials_gen, p.peer_ip);
				} catch (const std::exception &e) {
					std::fprintf(stderr, "[http] /NWJoin.dll WARN join rows: %s\n", e.what());
				}
			}
		}

		add_cookie(res, "PUBPCID",       pub_pcid);
		add_cookie(res, "PUBNAMEINFO",   pub_nameinfo);
		add_cookie(res, "PUBSQUADINFO",  pub_squadinfo);
		// The joiner's ticket (hostdb::mint_join_ticket), which its host forwards to the lobby.
		add_cookie(res, "PUBJOINTICKET", pub_join_ticket);

		// NW* and AFF* go out empty, as onnet sent them: a host reads a joiner's identity and
		// ticket under its own prefix alone, "PUB" [orig: g_LocalNetAddressStr @0x7CA298, read
		// only by CNapiNetwork_GetLocalAddress @0x4C4F60].
		for (const char *name : { "NWPCID", "NWNAMEINFO", "NWSQUADINFO", "NWTI", "NWTV",
					 "NWJOINTICKET", "AFFPCID", "AFFNAMEINFO", "AFFSQUADINFO", "AFFJOINTICKET" }) {
			add_cookie(res, name, "");
		}
		add_cookie(res, "NWPF",  session->pfid);
		add_cookie(res, "NWPF2", "0");
		return res;
	};
	CROW_ROUTE(app, "/NWJoin.dll").methods("GET"_method)(handle_join);
	CROW_ROUTE(app, "/nwjoin.dll").methods("GET"_method)(handle_join);

	// ----- Phase F.2: /NWHost.dll two-phase relay -------------------------
	// Mirrors onnet's onnw/controllers/nova_world/host.py.
	//   First call (the page link's query, or no NWJOINSESSIONTAG cookie):
	//     - generate session + 48-char A-P host_key (24 random bytes nibble-encoded)
	//     - record the key's hash with the account the request's retail login
	//       names, an active account (retail_login_user -> hostdb::record_host_key), if any
	//     - store HostSession with the client's game's default templates (JO's
	//       jop_2_host2.htm etc.) where the page link names none
	//     - render relay template with REFRESH_ENDPOINT="NWHost.dll" (bare; the
	//       NWJOINSESSIONTAG cookie carries the tag — matches onnet/genuine)
	//   Second call (the bare refresh, its tag in the cookie):
	//     - render success template (jop_2_host2.htm) with {{HOSTKEY}} substituted
	//       so retail's IB3 parser extracts <TITLE>[HOSTKEY=...&]</TITLE>
	//       and uses it for subsequent UDP ClientHostRequest.
	auto handle_host = [this, templates_dir](const crow::request &req) {
		auto looks_encrypted = [](const std::string &v) {
			if (v.size() < 16) return false;
			for (char c : v) if (c < 'A' || c > 'P') return false;
			return true;
		};
		auto field_or = [&](const char *name, const std::string &fallback) -> std::string {
			const char *v = req.url_params.get(name);
			if (!v || !*v) return fallback;
			std::string s(v);
			return looks_encrypted(s) ? fallback : s;
		};

		// The tag rides the NWJOINSESSIONTAG cookie.
		const auto cookies = parse_cookie_header(request_cookie_header(req));
		std::string tag;
		if (auto it = cookies.find("NWJOINSESSIONTAG"); it != cookies.end()) tag = it->second;

		// The host page link's query is the first call even with a stale tag
		// cookie in the client's jar (net/novaworld/relay_request.h).
		const bool host_first_call =
				classify_relay_request(has_relay_first_call_query(req), !tag.empty()) ==
				RelayLeg::First;
		if (host_first_call) {
			// First call — generate session.
			HostSession s;
			s.session_tag = sessions_.generate_tag("NWHost.dll");
			// 48-char A-P encoded host_key from 24 OS CSPRNG bytes (base/os_random).
			std::array<uint8_t, 24> raw{};
			os_random_bytes(raw.data(), raw.size());
			std::string hk;
			hk.reserve(48);
			for (const uint8_t b : raw) {
				hk.push_back(static_cast<char>('A' + ((b >> 4) & 0x0F)));
				hk.push_back(static_cast<char>('A' + (b & 0x0F)));
			}
			s.host_key   = std::move(hk);
			// The host's game (its gate tag, else the page link's pfid and
			// success page): the default pages and the NWPF are its.
			const std::string pfid_param = field_or("pfid", std::string());
			const std::string success_param = field_or("success", std::string());
			const auto games = games_now();
			const catalog::GameRow &game =
					client_game(*games, req, cookies, pfid_param, success_param);
			s.success    = success_param.empty() ? game.page("host2.htm") : success_param;
			const std::string relay_tpl = field_or("relay", game.page("relay.htm"));
			s.pfid       = pfid_param.empty() ? game.pfid : pfid_param;

			// The listing's owner (service policy, unwitnessed in retail): the
			// account this request's own retail login names gets the key, kept as
			// its hash (host_keys), so the host that presents it on its NovaWorld
			// session lists its server as that account's. With no such login
			// nothing is recorded, and that host lists anonymously (or not at all
			// under ONNET_REQUIRE_HOST_KEY). Either way the pages are the same bytes.
			std::string owner_label = "none";
			try {
				auto db_conn = db_pool_.acquire();
				const char *how = "";
				const auto owner = retail_login_user(cookies, *db_conn, how);
				// Recorded only while the account is still active and at the
				// generation the login read, tested by the insert: a reset that
				// commits after the lookup leaves the key unrecorded.
				if (owner && owner->account_status == "active" &&
						hostdb::record_host_key(
								*db_conn, s.host_key, owner->id, owner->credentials_gen)) {
					owner_label = std::to_string(owner->id) + " (" + how + ")";
				}
			} catch (const std::exception &e) {
				owner_label = "none (db error)";
				std::fprintf(stderr, "[http] /NWHost.dll WARN host key record: %s\n", e.what());
			}

			const std::string new_tag    = s.session_tag;
			const std::string key_head   = s.host_key.substr(0, 6);
			sessions_.put_host(new_tag, s);

			std::printf("[http] /NWHost.dll (first call) -> tag %s host_key=%s... owner=%s\n",
					loggable_secret(new_tag).c_str(), key_head.c_str(), owner_label.c_str());

			TemplateVars vars{
				{"MESSAGE",          "Contacting NovaWorld...."},
				{"REFRESH_ENDPOINT", "NWHost.dll"},
				{"HOST_URL",         host_url(game)},
				{"GSB_SERVER",       gsb_url(game)},
				{"JOINLAN_URL",      game.join_lan_url},
			};
			crow::response res(200);
			res.body = render_template_file(templates_dir, relay_tpl, vars);
			res.set_header("Content-Type", "text/html");
			add_standard_headers(res);
			add_cookie(res, "YOURIP",           req.remote_ip_address);
			add_cookie(res, "NWJOINSESSIONTAG", new_tag);
			return res;
		}

		// Second call — take the session (served once: a stock client follows
		// the relay page's refresh once and reads the key from that page's
		// TITLE, so the tag never serves the key again), render success with
		// HOSTKEY.
		const auto session = sessions_.take_host(tag);
		if (!session) {
			std::printf("[http] /NWHost.dll (second call) -> 400 (unknown tag %s)\n",
					loggable_secret(tag).c_str());
			crow::response res(400);
			res.body = "unknown NWJOINSESSIONTAG (host)";
			return res;
		}

		std::printf("[http] /NWHost.dll (second call) tag=%s -> %s with HOSTKEY=%.6s...\n",
				loggable_secret(tag).c_str(), loggable(session->success).c_str(),
				session->host_key.c_str());

		const auto games = games_now();
		const catalog::GameRow &game =
				client_game(*games, req, cookies, session->pfid, session->success);
		TemplateVars vars{
			{"HOSTKEY",     session->host_key},
			{"HOST_URL",    host_url(game)},
			{"GSB_SERVER",  gsb_url(game)},
			{"JOINLAN_URL", game.join_lan_url},
		};
		crow::response res(200);
		res.body = render_template_file(templates_dir, session->success, vars);
		res.set_header("Content-Type", "text/html");
		add_standard_headers(res);
		add_cookie(res, "YOURIP",           req.remote_ip_address);
		add_cookie(res, "NWJOINSESSIONTAG", tag);
		// PUBcrypto-encoded fields stubbed empty for first pass.
		for (const char *name : {"PUBPCID","PUBNAMEINFO","PUBSQUADINFO",
		                          "NWPCID","NWNAMEINFO","NWSQUADINFO","NWTI","NWTV",
		                          "AFFPCID","AFFNAMEINFO","AFFSQUADINFO"}) {
			add_cookie(res, name, "");
		}
		add_cookie(res, "NWPF",  session->pfid);
		add_cookie(res, "NWPF2", "0");
		return res;
	};
	CROW_ROUTE(app, "/NWHost.dll").methods("GET"_method)(handle_host);
	CROW_ROUTE(app, "/nwhost.dll").methods("GET"_method)(handle_host);
}

} // namespace opennova::novaworld_server
