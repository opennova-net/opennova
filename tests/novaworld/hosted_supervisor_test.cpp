// The hosted servers' supervisor and what it stands on (apps/novaworld_server/
// hosted, migration 0011), on a migrated SQLite file with a FakeProvider
// (hosted_fake_provider.h) and a hand-stepped clock: the owner's settings and
// their range checks, the files rendered from them (read back by the formats'
// own readers), the service accounts (minted, refused by the website, hidden,
// reserved, their password set again at every start, deleted at the end), the
// start request's checks (approval, one live server per account, the global
// cap), every transition of the lifecycle with its event, the stop (the push,
// the listing's end, the 60 s timeout, any exit while stopping, a stop before
// any machine, a stop landing mid-launch, the kill switch), the crash restart
// and its limit (any exit while the server should run, code 0 and the
// ServerStopHosting push's 3 included: a stop is never read from an exit code),
// the restart window (an hour of running forgives the crashes before it), the
// boot and listing timeouts, the health checks, an owner's account banned or
// deleted or its approval withdrawn (also during a crash restart's backoff),
// the reconciliation at the service's start (a provisioning row whose machine
// is gone among it) and the periodic orphan sweep, and the admin links kept in
// step with the running servers.
// Then LocalProcessProvider against this test binary itself as the
// opennova-serve it runs (`--resource-dir ...` makes it the fake server): the
// working directory rendered from scratch with its secret files owner-only and
// deleted at terminate, an environment that keeps a planted secret out, the
// argv, the exit code, terminate and the tags; and its children outliving the
// supervisor's thread to get an orderly stop from the provider. Compiles the
// server's auth.cpp, web_session.cpp and the hosted sources directly, so it
// runs on every build.

#include "auth.h"
#include "child_process.h"
#include "hosted/admin_link.h"
#include "hosted/hosted_repository.h"
#include "hosted/hosted_settings.h"
#include "hosted/local_process_provider.h"
#include "hosted/supervisor.h"
#include "hosted_fake_provider.h"
#include "web_session.h"

#include <base/io/cp1252.h>
#include <base/io/file_io.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/host_repository.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace fs = std::filesystem;
namespace nws = opennova::novaworld_server;
namespace hosted = opennova::novaworld_server::hosted;
namespace hostdb = opennova::hostdb;
using hosted::State;
using hosted_test::FakeProvider;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

std::string g_self;

// --- fixtures -----------------------------------------------------------------

hosted::HostedSettings valid_settings() {
	hosted::HostedSettings s;
	s.game_name = "Hosted Test";
	s.missions = { "servetst.bms", "second.bms" };
	return s;
}

int g_pcid = 0;

// A player, approved to host when `can_host`.
int64_t make_player(Database &db, const std::string &name, bool can_host) {
	nws::CreateUserParams p;
	p.username = name;
	p.password = "pw-" + name;
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", static_cast<unsigned>(0x0d000000 + ++g_pcid));
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = name + "H";
	const auto created = nws::create_user(db, p);
	if (!created.ok) {
		std::fprintf(stderr, "  create_user %s: %s\n", name.c_str(), created.error_code.c_str());
		return 0;
	}
	if (can_host && !nws::set_can_host(db, created.id, true).ok) return 0;
	return created.id;
}

// A 48-letter A..P HostKey, distinct per `n`.
std::string key_for(uint32_t n) {
	std::string key(opennova::hostdb::kHostKeyChars, 'A');
	for (size_t i = 0; i < 8; ++i) key[i] = static_cast<char>('A' + ((n >> (i * 4)) & 0xF));
	return key;
}

// What the lobby does when `account`'s server hosts and presents a key its
// /NWHost.dll recorded: the row listed under `rid`, owned by the account.
bool list_as(Database &db, int64_t account, uint32_t rid) {
	const auto user = nws::get_user_by_id(db, account);
	if (!user) return false;
	const std::string key = key_for(rid);
	if (!hostdb::record_host_key(db, key, account, user->credentials_gen)) return false;
	hostdb::HostRow row;
	row.rid = rid;
	row.gsid = "GSID-" + std::to_string(rid);
	row.game = "jop_2_consumer";
	row.server_name = "Hosted Listing";
	row.host_ip = "127.0.0.1";
	row.host_port = 40000;
	row.peer_ip = "127.0.0.1";
	row.peer_port = 40000;
	row.host_user_id = hostdb::claim_host_key(db, key, rid);
	hostdb::upsert_host(db, row);
	return row.host_user_id == account;
}

std::vector<std::string> transitions(Database &db, int64_t id) {
	std::vector<std::string> out;
	for (const auto &e : hosted::list_events(db, id, 100)) out.push_back(e.from_state + ">" + e.to_state);
	return out;
}

std::string last_detail(Database &db, int64_t id) {
	const auto events = hosted::list_events(db, id, 1);
	return events.empty() ? std::string() : events.back().detail;
}

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

// The value of `key=` in the credentials file's text.
std::string credential(const std::string &text, const std::string &key) {
	const std::string prefix = key + "=";
	const auto at = text.find(prefix);
	if (at == std::string::npos) return {};
	const auto start = at + prefix.size();
	return text.substr(start, text.find('\n', start) - start);
}

// One supervisor over a fresh database and a FakeProvider, its clock in hand.
struct Rig {
	test_temp::TempDir temp;
	ConnectionPool pool;
	FakeProvider fake;
	hosted::SupervisorConfig config;
	std::unique_ptr<hosted::Supervisor> sup;
	std::vector<std::pair<uint32_t, int64_t>> pushes;
	uint64_t now = 1000;

	explicit Rig(const char *name) : temp(name), pool(temp.path / "hosted.db") {
		auto boot = pool.acquire();
		opennova::db::run_migrations(*boot, fs::path(OPENNOVA_SOURCE_DIR) / "backend" / "migrations");
		config.cap = 3;
		config.master_host = "127.0.0.1";
		config.master_gate_port = 7597;
		config.boot_timeout_ms = 10000;
		config.listing_timeout_ms = 20000;
		config.health_interval_ms = 5000;
		config.health_misses = 3;
		config.restart_limit = 2;
		config.restart_backoff_ms = 3000;
		config.stop_timeout_ms = 60000;
		make_supervisor();
	}
	void make_supervisor() {
		sup = std::make_unique<hosted::Supervisor>(pool, fake, config);
		sup->set_stop_listing([this](uint32_t rid, int64_t account) {
			pushes.emplace_back(rid, account);
			return true;
		});
	}
	void tick() { sup->tick(now); }
	void advance(uint64_t ms) { now += ms; }
	std::optional<hosted::HostedServer> server(int64_t id) { return hosted::find_server(*pool.acquire(), id); }
	State state(int64_t id) {
		const auto s = server(id);
		return s ? s->state : State::Failed;
	}
	// An approved owner's request.
	int64_t request(const std::string &owner, hosted::HostedSettings settings = valid_settings()) {
		auto db = pool.acquire();
		const int64_t user = make_player(*db, owner, true);
		if (user == 0) return 0;
		const auto r = hosted::request_server(*db, user, settings, config.cap);
		return r.outcome == hosted::RequestOutcome::Created ? r.id : 0;
	}
	// A request walked to running, listed under `rid`: its machine's id.
	std::string to_running(int64_t id, uint32_t rid) {
		tick(); // requested -> provisioning
		tick(); // the launch: -> booting
		const auto s = server(id);
		if (!s || s->state != State::Booting) return {};
		const std::string machine = s->machine_id;
		fake.run(machine);
		tick(); // -> listing
		if (!list_as(*pool.acquire(), *server(id)->service_user_id, rid)) return {};
		tick(); // -> running
		return state(id) == State::Running ? machine : std::string();
	}
	// A crashed server (provisioning, its backoff pending) relaunched and walked
	// back to running, listed under `rid` with a key of its new credentials: its
	// machine's id.
	std::string back_to_running(int64_t id, uint32_t rid) {
		advance(config.restart_backoff_ms);
		tick(); // the relaunch: -> booting
		const auto s = server(id);
		if (!s || s->state != State::Booting) return {};
		const std::string machine = s->machine_id;
		fake.run(machine);
		tick(); // -> listing
		if (!list_as(*pool.acquire(), *server(id)->service_user_id, rid)) return {};
		tick(); // -> running
		return state(id) == State::Running ? machine : std::string();
	}
};

// --- the settings and the renders ---------------------------------------------

int test_settings_rules() {
	hosted::HostedSettings s = valid_settings();
	TEST_EXPECT(!hosted::check_settings(s));
	auto refused = [](hosted::HostedSettings bad, const char *field) {
		const auto r = hosted::check_settings(bad);
		if (!r || r->field != field)
			std::fprintf(stderr, "  want a refusal of %s, got %s\n", field, r ? r->field.c_str() : "none");
		return r && r->field == field;
	};
	auto with = [&s](auto change) {
		hosted::HostedSettings c = s;
		change(c);
		return c;
	};
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = ""; }), "game_name"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = "   "; }), "game_name"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = std::string(32, 'n'); }), "game_name"));
	TEST_EXPECT(!hosted::check_settings(with([](auto &c) { c.game_name = std::string(31, 'n'); })));
	// 31 characters of two UTF-8 bytes each are 31 cp1252 bytes.
	TEST_EXPECT(!hosted::check_settings(with([](auto &c) {
		c.game_name.clear();
		for (int i = 0; i < 31; ++i) c.game_name += "\xC3\xA9";
	})));
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = "say \"hi\""; }), "game_name"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = "line\nbreak"; }), "game_name"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = "c1 \xC2\x85"; }), "game_name"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_name = "emoji \xF0\x9F\x98\x80"; }), "game_name"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_password = std::string(17, 'p'); }), "game_password"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_password = "has space"; }), "game_password"));
	TEST_EXPECT(refused(with([](auto &c) { c.game_password = "q\"uote"; }), "game_password"));
	TEST_EXPECT(!hosted::check_settings(with([](auto &c) { c.game_password = "Secret!16-chars."; })));
	TEST_EXPECT(refused(with([](auto &c) { c.max_players = 1; }), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) { c.max_players = 65; }), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) { c.time_limit = 0; }), "time_limit"));
	TEST_EXPECT(refused(with([](auto &c) { c.kill_limit = 501; }), "kill_limit"));
	TEST_EXPECT(refused(with([](auto &c) { c.max_score = 0; }), "max_score"));
	TEST_EXPECT(refused(with([](auto &c) { c.respawn = -1; }), "respawn"));
	TEST_EXPECT(refused(with([](auto &c) { c.start_delay = 301; }), "start_delay"));
	TEST_EXPECT(refused(with([](auto &c) { c.expansion = "jox02"; }), "expansion"));
	TEST_EXPECT(!hosted::check_settings(with([](auto &c) { c.expansion = "jox01"; })));
	TEST_EXPECT(refused(with([](auto &c) { c.missions.clear(); }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions.assign(33, "map.bms"); }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "../up.bms" }; }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "map.txt" }; }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "two words.bms" }; }), "missions"));
	TEST_EXPECT(!hosted::check_settings(with([](auto &c) { c.missions = { "ASH_G11A.BMS", "x-y_1.bms" }; })));

	TEST_EXPECT(hosted::split_missions(hosted::join_missions(s.missions)) == s.missions);
	TEST_EXPECT(hosted::split_missions("").empty());
	return 0;
}

int test_renders() {
	hosted::HostedSettings s = valid_settings();
	s.game_name = "Caf\xC3\xA9 Night"; // "Café Night": é is cp1252 0xE9
	s.game_password = "letmein";
	s.max_players = 24;
	s.time_limit = 45;
	s.kill_limit = 100;
	s.max_score = 7;
	s.respawn = 10;
	s.start_delay = 15;
	s.team_ff = true;
	s.missions = { "first.bms", "second.bms", "third.bms" };
	const std::string host = hosted::render_host_file(s, 12);
	const char *const want[] = {
		"GameName \"Caf\xE9 Night\"\r\n", "MaxPlayers 24\r\n", "MPHostGamePassword \"letmein\"\r\n",
		"Time_Limit 45\r\n", "KillLimit 100\r\n", "MaxScore 7\r\n", "Respawn 10\r\n", "Delay 15\r\n",
		"TeamFF 1\r\n", "Replay 1\r\n",
		// The first map's line last: the last Mission line is the starting map.
		"Mission second.bms 0\r\nMission third.bms 0\r\nMission first.bms 0\r\n",
	};
	for (const char *line : want) {
		if (!contains(host, line)) std::fprintf(stderr, "  the host file lacks %s", line);
		TEST_EXPECT(contains(host, line));
	}
	TEST_EXPECT(host.size() >= 2 && host.compare(host.size() - 2, 2, "\r\n") == 0);

	hosted::MachinePlan plan;
	plan.nw_port = 32768;
	plan.admin_port = 32800;
	plan.admin_allowed_ip = "10.1.2.3";
	const std::string cfg_text = hosted::render_game_cfg(plan);
	const auto cfg = opennova::gamecfg::load(cfg_text.data(), cfg_text.size(), {});
	TEST_EXPECT(!cfg.version_reset);
	TEST_EXPECT(cfg.cfg.networkconnecttype == 1);
	TEST_EXPECT(cfg.cfg.mp_novaworld_port_min == 32768 && cfg.cfg.mp_novaworld_port_max == 32768);
	TEST_EXPECT(cfg.cfg.remote_admin_port == 32800);
	TEST_EXPECT(cfg.cfg.mp_novaworld_host_lan_only == 0);

	const std::string admin_password = hosted::random_secret(hosted::kAdminPasswordChars);
	const std::string admin_text = hosted::render_admin_cfg(plan, admin_password);
	const auto admin = opennova::admincfg::parse(admin_text.data(), admin_text.size());
	TEST_EXPECT(admin.users.size() == 1);
	TEST_EXPECT(admin.users[0].name == "svc" && admin.users[0].password == admin_password);
	TEST_EXPECT(admin.users[0].rights == 0x7Fu);
	TEST_EXPECT(admin.ip_restrictions == std::vector<std::string>{ "10.1.2.3" });

	const std::string creds = hosted::render_credentials("srv-12", "pw");
	TEST_EXPECT(credential(creds, "NOVAWORLD_USER") == "srv-12");
	TEST_EXPECT(credential(creds, "NOVAWORLD_PASS") == "pw");

	const std::string a = hosted::random_secret(31);
	const std::string b = hosted::random_secret(31);
	TEST_EXPECT(a.size() == 31 && a != b);
	TEST_EXPECT(std::all_of(a.begin(), a.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
	}));

	hosted::RenderInputs in;
	in.server_id = 12;
	in.plan = plan;
	in.service_username = "srv-12";
	in.service_password = "nwpass";
	in.admin_password = admin_password;
	in.master_host = "203.0.113.5";
	in.master_gate_port = 7597;
	s.expansion = "jox01";
	const hosted::LaunchSpec spec = hosted::render_launch(s, in);
	TEST_EXPECT(spec.server_id == 12 && spec.files.size() == 4);
	for (const auto &f : spec.files) {
		const bool secret = f.name != "game.cfg"; // the host file carries the game password
		TEST_EXPECT(f.secret == secret);
	}
	const std::vector<std::string> args = { "/HOST", "server.host", "--credentials", "credentials.txt",
		"--master-host", "203.0.113.5", "--master-gate-port", "7597", "/exp", "jox01" };
	TEST_EXPECT(spec.serve_args == args);
	return 0;
}

// --- the service accounts -----------------------------------------------------

int test_service_accounts() {
	test_temp::TempDir temp("hosted_accounts");
	ConnectionPool pool(temp.path / "accounts.db");
	auto lease = pool.acquire();
	Database &db = *lease;
	opennova::db::run_migrations(db, fs::path(OPENNOVA_SOURCE_DIR) / "backend" / "migrations");
	const int64_t player = make_player(db, "acct_player", false);
	TEST_EXPECT(player > 0);

	const auto made = nws::upsert_service_account(db, "srv-7", "first-password");
	TEST_EXPECT(made.ok && made.id > 0);
	auto account = nws::get_user_by_id(db, made.id);
	TEST_EXPECT(account && account->kind == "service" && account->nwhandle == "srv-7");
	TEST_EXPECT(!account->can_host && account->role == "player");
	TEST_EXPECT(nws::get_user_by_id(db, player)->kind == "player");
	const auto authed = nws::authenticate_user(db, "srv-7", "first-password");
	TEST_EXPECT(authed && authed->kind == "service");

	// The site's side refuses it everywhere.
	for (const auto &u : nws::list_users(db)) TEST_EXPECT(u.kind == "player" && u.id != made.id);
	TEST_EXPECT(!nws::create_web_session(db, made.id, account->credentials_gen, "127.0.0.1", "test"));
	nws::UpdateUserParams rename;
	rename.nwhandle = "Renamed";
	TEST_EXPECT(nws::update_user(db, made.id, rename).error_code == "not_found");
	TEST_EXPECT(nws::delete_user(db, made.id).error_code == "not_found");
	TEST_EXPECT(nws::set_can_host(db, made.id, true).error_code == "not_found");
	TEST_EXPECT(nws::bootstrap_admin(db, "srv-7").outcome == nws::BootstrapAdminResult::Outcome::NoSuchAccount);
	// A session row planted by hand resolves to nothing.
	db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
			"VALUES (?, ?, datetime('now', '+1 day'));",
			{ BindValue(nws::web_session_token_hash(std::string(64, 'b'))), BindValue(made.id) });
	TEST_EXPECT(!nws::resolve_web_session(db, std::string(64, 'b')));

	// The prefix is reserved for people, as a username, a handle and a rename.
	nws::CreateUserParams p;
	p.username = "srv-9";
	p.password = "pw";
	p.pcid = "0e0e0e09";
	p.nwhandle = "Fine";
	TEST_EXPECT(nws::create_user(db, p).error_code == "invalid_field");
	p.username = "SRV-9";
	TEST_EXPECT(nws::create_user(db, p).error_code == "invalid_field");
	p.username = "fine_name";
	p.nwhandle = "Srv-handle";
	TEST_EXPECT(nws::create_user(db, p).error_code == "invalid_field");
	nws::UpdateUserParams to_reserved;
	to_reserved.username = "srv-10";
	TEST_EXPECT(nws::update_user(db, player, to_reserved).error_code == "invalid_field");
	TEST_EXPECT(nws::is_service_username("srv-1") && nws::is_service_username("SRV-x"));
	TEST_EXPECT(!nws::is_service_username("server") && nws::service_username(42) == "srv-42");

	// A second upsert sets a new password: the old one stops working, the
	// generation moves and the account's keys go.
	const std::string key = key_for(77);
	TEST_EXPECT(hostdb::record_host_key(db, key, made.id, account->credentials_gen));
	const auto again = nws::upsert_service_account(db, "srv-7", "second-password");
	TEST_EXPECT(again.ok && again.id == made.id);
	TEST_EXPECT(!nws::authenticate_user(db, "srv-7", "first-password"));
	TEST_EXPECT(nws::authenticate_user(db, "srv-7", "second-password"));
	TEST_EXPECT(nws::get_user_by_id(db, made.id)->credentials_gen == account->credentials_gen + 1);
	TEST_EXPECT(db.query("SELECT 1 FROM host_keys WHERE user_id = ?;", { BindValue(made.id) }).empty());

	// A player holding the name (a database written by hand) keeps it.
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
			"VALUES ('srv-8', 'x', '0e0e0e08', '1', 'HandMade');");
	TEST_EXPECT(nws::upsert_service_account(db, "srv-8", "pw").error_code == "username_exists");
	TEST_EXPECT(nws::upsert_service_account(db, "player-name", "pw").error_code == "invalid_field");

	TEST_EXPECT(nws::delete_service_account(db, player).error_code == "not_found");
	TEST_EXPECT(nws::delete_service_account(db, made.id).ok);
	TEST_EXPECT(!nws::get_user_by_id(db, made.id));
	TEST_EXPECT(nws::set_can_host(db, player, true).ok && nws::get_user_by_id(db, player)->can_host);
	return 0;
}

// --- the requests ---------------------------------------------------------------

int test_requests() {
	Rig rig("hosted_requests");
	auto lease = rig.pool.acquire();
	Database &db = *lease;
	const auto settings = valid_settings();
	const int64_t unapproved = make_player(db, "req_unapproved", false);
	TEST_EXPECT(hosted::request_server(db, unapproved, settings, 3).outcome ==
			hosted::RequestOutcome::NotApproved);
	TEST_EXPECT(hosted::request_server(db, 999999, settings, 3).outcome == hosted::RequestOutcome::NoSuchUser);
	const auto service = nws::upsert_service_account(db, "srv-500", "pw");
	TEST_EXPECT(hosted::request_server(db, service.id, settings, 3).outcome ==
			hosted::RequestOutcome::NoSuchUser);
	const int64_t banned = make_player(db, "req_banned", true);
	nws::UpdateUserParams ban;
	ban.account_status = "banned";
	TEST_EXPECT(nws::update_user(db, banned, ban).ok);
	TEST_EXPECT(hosted::request_server(db, banned, settings, 3).outcome == hosted::RequestOutcome::NotApproved);

	// One live server per account.
	const int64_t a = make_player(db, "req_a", true);
	hosted::HostedSettings unchecked = settings;
	unchecked.missions.clear();
	TEST_EXPECT(hosted::request_server(db, a, unchecked, 3).outcome == hosted::RequestOutcome::InvalidSettings);
	const auto first = hosted::request_server(db, a, settings, 3);
	TEST_EXPECT(first.outcome == hosted::RequestOutcome::Created && first.id > 0);
	const auto second = hosted::request_server(db, a, settings, 3);
	TEST_EXPECT(second.outcome == hosted::RequestOutcome::AlreadyLive && second.id == first.id);
	// The index holds it too, whatever writes the row.
	bool refused = false;
	try {
		db.exec("INSERT INTO hosted_servers (owner_user_id, game_name, max_players, time_limit, "
				"kill_limit, max_score, respawn, start_delay, missions) "
				"VALUES (?, 'x', 8, 1, 1, 1, 0, 0, 'm.bms');",
				{ BindValue(a) });
	} catch (const opennova::db::SqliteError &) {
		refused = true;
	}
	TEST_EXPECT(refused);
	TEST_EXPECT(transitions(db, first.id) == std::vector<std::string>{ ">requested" });
	const auto row = hosted::find_server(db, first.id);
	TEST_EXPECT(row && row->state == State::Requested && row->owner_user_id == a);
	TEST_EXPECT(row->settings.missions == settings.missions && row->settings.game_name == settings.game_name);

	// The cap counts every live server.
	const int64_t b = make_player(db, "req_b", true);
	const int64_t c = make_player(db, "req_c", true);
	const int64_t d = make_player(db, "req_d", true);
	TEST_EXPECT(hosted::request_server(db, b, settings, 3).outcome == hosted::RequestOutcome::Created);
	const auto third = hosted::request_server(db, c, settings, 3);
	TEST_EXPECT(third.outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::count_live(db) == 3);
	TEST_EXPECT(hosted::request_server(db, d, settings, 3).outcome == hosted::RequestOutcome::AtCapacity);
	// An ended server frees its place, and its owner may start again.
	TEST_EXPECT(hosted::transition(db, third.id, State::Requested, State::Failed, "test"));
	TEST_EXPECT(hosted::request_server(db, d, settings, 3).outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::request_server(db, c, settings, 4).outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::latest_server_of(db, c)->state == State::Requested);
	TEST_EXPECT(hosted::list_servers(db, 10).size() == 5);

	// The kill switch asks every live server's stop, once.
	TEST_EXPECT(hosted::request_stop_all(db, "kill switch") == 4);
	TEST_EXPECT(hosted::request_stop_all(db, "kill switch") == 0);
	TEST_EXPECT(!hosted::request_stop(db, first.id, "again"));
	TEST_EXPECT(!hosted::request_stop(db, third.id, "ended"));
	return 0;
}

// --- the lifecycle -------------------------------------------------------------

int test_lifecycle_and_stop() {
	Rig rig("hosted_lifecycle");
	hosted::HostedSettings settings = valid_settings();
	settings.expansion = "jox01";
	const int64_t id = rig.request("life_owner", settings);
	TEST_EXPECT(id > 0);

	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Provisioning);
	TEST_EXPECT(rig.fake.launches == 0);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Booting);
	TEST_EXPECT(rig.fake.launches == 1);
	auto row = rig.server(id);
	const std::string machine = row->machine_id;
	TEST_EXPECT(machine == rig.fake.last_machine && row->provider == "fake");
	TEST_EXPECT(row->machine_address == "10.0.0.2");
	const auto login = hosted::admin_login_of(*rig.pool.acquire(), id);
	TEST_EXPECT(login && login->user == "svc" && login->port == 41000 + id && row->admin_port == login->port);
	TEST_EXPECT(login->password.size() == hosted::kAdminPasswordChars);
	TEST_EXPECT(row->service_user_id.has_value());
	const int64_t service = *row->service_user_id;
	{
		auto db = rig.pool.acquire();
		const auto account = nws::get_user_by_id(*db, service);
		TEST_EXPECT(account && account->kind == "service" && account->username == "srv-" + std::to_string(id));
		// The credentials the machine got log the account in.
		const auto *creds = rig.fake.file(machine, "credentials.txt");
		TEST_EXPECT(creds && creds->secret);
		TEST_EXPECT(credential(creds->content, "NOVAWORLD_USER") == account->username);
		const std::string password = credential(creds->content, "NOVAWORLD_PASS");
		TEST_EXPECT(password.size() == hosted::kServicePasswordChars);
		TEST_EXPECT(nws::authenticate_user(*db, account->username, password));
		const auto *admin = rig.fake.file(machine, "admin.cfg");
		TEST_EXPECT(admin && admin->secret && contains(admin->content, login->password));
		TEST_EXPECT(contains(admin->content, "ip_restrict = 10.0.0.1"));
		const auto *host = rig.fake.file(machine, "server.host");
		TEST_EXPECT(host && host->secret && contains(host->content, "GameName \"Hosted Test\""));
		const auto *cfg = rig.fake.file(machine, "game.cfg");
		TEST_EXPECT(cfg && contains(cfg->content, std::to_string(40000 + id)));
		const std::vector<std::string> args = { "/HOST", "server.host", "--credentials",
			"credentials.txt", "--master-host", "127.0.0.1", "--master-gate-port", "7597", "/exp",
			"jox01" };
		TEST_EXPECT(rig.fake.machines.at(machine).spec.serve_args == args);
	}

	rig.tick(); // still Pending
	TEST_EXPECT(rig.state(id) == State::Booting);
	rig.fake.run(machine);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Listing);
	rig.tick(); // no listing yet
	TEST_EXPECT(rig.state(id) == State::Listing);
	TEST_EXPECT(list_as(*rig.pool.acquire(), service, 5001));
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Running && !row->running_at.empty());
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), id), "RID 5001"));
	rig.advance(rig.config.health_interval_ms);
	rig.tick(); // a health check that finds the listing
	TEST_EXPECT(rig.state(id) == State::Running);

	// The owner's stop: the push to the listing, then the end once it has left.
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), id, "stopped by its owner"));
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Stopping);
	TEST_EXPECT(rig.pushes.size() == 1 && rig.pushes[0].first == 5001 && rig.pushes[0].second == service);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Stopping && rig.pushes.size() == 1);
	TEST_EXPECT(!rig.fake.was_terminated(machine));
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 5001);
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Stopped && !row->ended_at.empty());
	TEST_EXPECT(rig.fake.was_terminated(machine));
	TEST_EXPECT(row->machine_id.empty() && !row->service_user_id.has_value());
	TEST_EXPECT(!nws::get_user_by_id(*rig.pool.acquire(), service));
	TEST_EXPECT(transitions(*rig.pool.acquire(), id) ==
			(std::vector<std::string>{ ">requested", "requested>provisioning", "provisioning>booting",
					"booting>listing", "listing>running", "running>stopping", "stopping>stopped" }));
	rig.tick(); // nothing live: nothing happens
	TEST_EXPECT(rig.fake.launches == 1);
	return 0;
}

int test_stop_paths() {
	Rig rig("hosted_stops");
	// A stop before any machine: stopped with no launch.
	const int64_t early = rig.request("stop_early");
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), early, "stopped by its owner"));
	rig.tick();
	TEST_EXPECT(rig.state(early) == State::Stopped && rig.fake.launches == 0);
	TEST_EXPECT(transitions(*rig.pool.acquire(), early) ==
			(std::vector<std::string>{ ">requested", "requested>stopping", "stopping>stopped" }));

	// Still listed 60 s after the push: the machine is ended anyway.
	const int64_t stuck = rig.request("stop_stuck");
	const std::string machine = rig.to_running(stuck, 6001);
	TEST_EXPECT(!machine.empty());
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), stuck, "stopped by an admin"));
	rig.tick();
	TEST_EXPECT(rig.pushes.size() == 1);
	rig.advance(rig.config.stop_timeout_ms - 1);
	rig.tick();
	TEST_EXPECT(rig.state(stuck) == State::Stopping);
	rig.advance(1);
	rig.tick();
	TEST_EXPECT(rig.state(stuck) == State::Stopped && rig.fake.was_terminated(machine));
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), stuck), "after the stop"));
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 6001);

	// The process exits on its own during the stop, with the code a
	// ServerStopHosting push ends opennova-serve with (3): the stop's end, never a
	// restart.
	const int64_t quick = rig.request("stop_quick");
	const std::string m2 = rig.to_running(quick, 6002);
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), quick, "stopped by its owner"));
	rig.tick();
	const int launches_before = rig.fake.launches;
	rig.fake.exit_with(m2, 3);
	rig.tick();
	TEST_EXPECT(rig.state(quick) == State::Stopped);
	TEST_EXPECT(rig.server(quick)->restarts == 0);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), quick), "code 3"));
	rig.advance(rig.config.restart_backoff_ms);
	rig.tick();
	TEST_EXPECT(rig.state(quick) == State::Stopped && rig.fake.launches == launches_before);
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 6002);

	// A stop that lands while the launch runs: the launched machine is ended.
	const int64_t racing = rig.request("stop_racing");
	rig.tick(); // -> provisioning
	rig.fake.on_launch = [&rig, racing] {
		hosted::request_stop(*rig.pool.acquire(), racing, "stopped mid-launch");
	};
	rig.tick(); // the launch; the move to booting loses to the stop
	rig.fake.on_launch = nullptr;
	auto row = rig.server(racing);
	TEST_EXPECT(row->state == State::Stopping && !row->machine_id.empty());
	const std::string launched = row->machine_id;
	rig.tick(); // the push (none: nothing listed)
	rig.tick();
	TEST_EXPECT(rig.state(racing) == State::Stopped && rig.fake.was_terminated(launched));
	return 0;
}

int test_exits_and_crash_restarts() {
	Rig rig("hosted_restarts");
	// opennova-serve exiting 0 while it should run (its rotation's end, the
	// admin's GOTO MENUSTATE) is no stop: only the supervisor stops a server, so
	// it is a crash, restarted and counted.
	const int64_t zero = rig.request("exit_zero");
	const std::string m0 = rig.to_running(zero, 7001);
	TEST_EXPECT(!m0.empty());
	rig.fake.exit_with(m0, 0);
	rig.tick();
	auto zero_row = rig.server(zero);
	TEST_EXPECT(zero_row->state == State::Provisioning && zero_row->restarts == 1);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), zero), "exited with code 0; restart 1 of 2"));
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 7001);
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), zero, "stopped by its owner"));
	rig.tick(); // no machine to end
	TEST_EXPECT(rig.state(zero) == State::Stopped);

	// A crash (here the ServerStopHosting exit, 3, while the server should run)
	// restarts, with a new password, until the limit (2) is used up.
	const int64_t id = rig.request("exit_crash");
	const std::string m1 = rig.to_running(id, 7002);
	TEST_EXPECT(!m1.empty());
	const int64_t service = *rig.server(id)->service_user_id;
	const std::string first_password = credential(rig.fake.file(m1, "credentials.txt")->content, "NOVAWORLD_PASS");
	const std::string first_admin = hosted::admin_login_of(*rig.pool.acquire(), id)->password;
	rig.fake.exit_with(m1, 3);
	rig.tick();
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 1 && row->machine_id.empty());
	TEST_EXPECT(rig.fake.was_terminated(m1));
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), id), "exited with code 3; restart 1 of 2"));
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 7002); // its NWU session timed out
	rig.tick(); // the backoff holds the relaunch
	TEST_EXPECT(rig.fake.launches == 2);
	rig.advance(rig.config.restart_backoff_ms);
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Booting && rig.fake.launches == 3);
	const std::string m2 = row->machine_id;
	TEST_EXPECT(m2 != m1 && row->service_user_id == service);
	const std::string second_password = credential(rig.fake.file(m2, "credentials.txt")->content, "NOVAWORLD_PASS");
	TEST_EXPECT(second_password != first_password);
	TEST_EXPECT(hosted::admin_login_of(*rig.pool.acquire(), id)->password != first_admin);
	{
		auto db = rig.pool.acquire();
		const std::string name = "srv-" + std::to_string(id);
		TEST_EXPECT(!nws::authenticate_user(*db, name, first_password));
		TEST_EXPECT(nws::authenticate_user(*db, name, second_password));
	}
	// Its machine goes away: the second restart.
	rig.fake.vanish(m2);
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 2);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), id), "its machine is gone; restart 2 of 2"));
	rig.advance(rig.config.restart_backoff_ms);
	rig.tick();
	const std::string m3 = rig.server(id)->machine_id;
	rig.fake.run(m3);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Listing);
	rig.fake.exit_with(m3, 1);
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed && row->restarts == 2);
	TEST_EXPECT(contains(row->failure_reason, "exited with code 1") &&
			contains(row->failure_reason, "no restart left"));
	TEST_EXPECT(!row->service_user_id && !nws::get_user_by_id(*rig.pool.acquire(), service));
	TEST_EXPECT(rig.fake.was_terminated(m3));

	// A launch that fails counts as an attempt; the provider's own text goes to the
	// log, not to the event the owner reads.
	const int64_t refused = rig.request("exit_refused");
	rig.tick();
	rig.fake.fail_next_launch = true;
	rig.tick();
	row = rig.server(refused);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 1);
	const std::string detail = last_detail(*rig.pool.acquire(), refused);
	TEST_EXPECT(contains(detail, "the launch failed; restart 1 of 2") && !contains(detail, "fake"));
	return 0;
}

// The restart window: restart_limit crash restarts at most, but a run of
// restart_reset_ms in running forgives the ones before it, so an always-on
// server that crashes now and then is restarted every time; crashes in quick
// succession still use the limit up.
int test_restart_window() {
	Rig rig("hosted_restart_window");
	const int64_t id = rig.request("window_owner");
	std::string machine = rig.to_running(id, 7101);
	TEST_EXPECT(!machine.empty());
	for (int crash = 1; crash <= 2; ++crash) {
		rig.fake.exit_with(machine, 3);
		rig.tick();
		TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == crash);
		machine = rig.back_to_running(id, 7101 + static_cast<uint32_t>(crash));
		TEST_EXPECT(!machine.empty());
	}
	rig.advance(rig.config.restart_reset_ms - 1);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Running && rig.server(id)->restarts == 2);
	rig.advance(1);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Running && rig.server(id)->restarts == 0);
	// So the next crash is restarted, not the end.
	rig.fake.exit_with(machine, 3);
	rig.tick();
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 1);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), id), "restart 1 of 2"));
	// Within the window the limit holds.
	machine = rig.back_to_running(id, 7110);
	TEST_EXPECT(!machine.empty());
	rig.fake.exit_with(machine, 3);
	rig.tick();
	TEST_EXPECT(rig.server(id)->restarts == 2);
	machine = rig.back_to_running(id, 7111);
	TEST_EXPECT(!machine.empty());
	rig.fake.exit_with(machine, 3);
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed && contains(row->failure_reason, "no restart left"));
	return 0;
}

// The periodic sweep: a machine the provider runs that no live row names (a
// launch whose row write failed) is ended between the boots too, every
// orphan_sweep_interval_ms; the server's own machine is kept.
int test_orphan_sweep() {
	Rig rig("hosted_sweep");
	const int64_t id = rig.request("sweep_owner");
	const std::string machine = rig.to_running(id, 9201);
	TEST_EXPECT(!machine.empty());
	const std::string stray = rig.fake.adopt(id);
	rig.tick();
	TEST_EXPECT(!rig.fake.was_terminated(stray));
	rig.advance(rig.config.orphan_sweep_interval_ms);
	rig.tick();
	TEST_EXPECT(rig.fake.was_terminated(stray) && !rig.fake.was_terminated(machine));
	TEST_EXPECT(rig.state(id) == State::Running);
	return 0;
}

int test_timeouts_and_health() {
	Rig rig("hosted_health");
	// The machine never runs opennova-serve.
	const int64_t slow = rig.request("health_slow");
	rig.tick();
	rig.tick();
	const std::string pending = rig.server(slow)->machine_id;
	rig.advance(rig.config.boot_timeout_ms - 1);
	rig.tick();
	TEST_EXPECT(rig.state(slow) == State::Booting);
	rig.advance(1);
	rig.tick();
	TEST_EXPECT(rig.state(slow) == State::Provisioning && rig.fake.was_terminated(pending));
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), slow), "did not start in 10 s"));
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), slow, "done"));
	rig.tick();

	// It runs, but never lists.
	const int64_t quiet = rig.request("health_quiet");
	rig.tick();
	rig.tick();
	const std::string unlisted = rig.server(quiet)->machine_id;
	rig.fake.run(unlisted);
	rig.tick();
	TEST_EXPECT(rig.state(quiet) == State::Listing);
	rig.advance(rig.config.listing_timeout_ms);
	rig.tick();
	TEST_EXPECT(rig.state(quiet) == State::Provisioning && rig.fake.was_terminated(unlisted));
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), quiet), "no listing in 20 s"));
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), quiet, "done"));
	rig.tick();

	// Listed, then gone from the browser: three missed checks in a row fail it,
	// and a check that finds the listing again starts the count over.
	const int64_t id = rig.request("health_lost");
	const std::string machine = rig.to_running(id, 8001);
	TEST_EXPECT(!machine.empty());
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 8001);
	rig.tick(); // before the interval: no check
	for (int miss = 1; miss <= 2; ++miss) {
		rig.advance(rig.config.health_interval_ms);
		rig.tick();
		TEST_EXPECT(rig.state(id) == State::Running);
	}
	// Back in the browser (its key still claimed for the RID): the count starts over.
	{
		auto db = rig.pool.acquire();
		hostdb::HostRow row;
		row.rid = 8001;
		row.gsid = "GSID-8001";
		row.game = "jop_2_consumer";
		row.server_name = "Back";
		row.host_ip = row.peer_ip = "127.0.0.1";
		row.host_port = row.peer_port = 40000;
		row.host_user_id = rig.server(id)->service_user_id;
		hostdb::upsert_host(*db, row);
	}
	rig.advance(rig.config.health_interval_ms);
	rig.tick();
	hostdb::remove_host_by_rid(*rig.pool.acquire(), 8001);
	for (int miss = 1; miss <= 2; ++miss) {
		rig.advance(rig.config.health_interval_ms);
		rig.tick();
		TEST_EXPECT(rig.state(id) == State::Running);
	}
	rig.advance(rig.config.health_interval_ms);
	rig.tick();
	const auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed && rig.fake.was_terminated(machine));
	TEST_EXPECT(contains(row->failure_reason, "no listing at 3 health checks"));
	TEST_EXPECT(row->restarts == 0);
	return 0;
}

int test_owner_changes() {
	Rig rig("hosted_owners");
	// A banned owner's running server is stopped.
	const int64_t banned = rig.request("owner_banned");
	const std::string m1 = rig.to_running(banned, 9001);
	TEST_EXPECT(!m1.empty());
	{
		auto db = rig.pool.acquire();
		nws::UpdateUserParams ban;
		ban.account_status = "banned";
		TEST_EXPECT(nws::update_user(*db, *rig.server(banned)->owner_user_id, ban).ok);
	}
	rig.tick();
	TEST_EXPECT(rig.state(banned) == State::Stopping);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), banned), "owner's account is banned"));

	// A deleted owner's server is stopped (the row keeps no owner).
	const int64_t deleted = rig.request("owner_deleted");
	const std::string m2 = rig.to_running(deleted, 9002);
	TEST_EXPECT(!m2.empty());
	const int64_t owner = *rig.server(deleted)->owner_user_id;
	TEST_EXPECT(nws::delete_user(*rig.pool.acquire(), owner).ok);
	TEST_EXPECT(!rig.server(deleted)->owner_user_id);
	rig.tick();
	TEST_EXPECT(rig.state(deleted) == State::Stopping);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), deleted), "owner's account was deleted"));

	// The approval (can_host) is the brake: withdrawn, the server stops, as for a
	// ban. Before the pickup nothing launches.
	const int64_t withdrawn = rig.request("owner_withdrawn");
	TEST_EXPECT(nws::set_can_host(*rig.pool.acquire(), *rig.server(withdrawn)->owner_user_id, false).ok);
	rig.tick();
	TEST_EXPECT(rig.state(withdrawn) == State::Stopping);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), withdrawn), "approval to host was withdrawn"));
	rig.tick();
	TEST_EXPECT(rig.state(withdrawn) == State::Stopped && rig.fake.launches == 2);

	// While it runs.
	const int64_t revoked = rig.request("owner_revoked");
	const std::string m3 = rig.to_running(revoked, 9003);
	TEST_EXPECT(!m3.empty());
	TEST_EXPECT(nws::set_can_host(*rig.pool.acquire(), *rig.server(revoked)->owner_user_id, false).ok);
	rig.tick();
	TEST_EXPECT(rig.state(revoked) == State::Stopping);
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), revoked), "approval to host was withdrawn"));

	// The three stopping servers still count against the rig's cap (3): their
	// listings end (as the stop's push takes them down), the stops finish, and
	// their places are free again.
	for (const uint32_t rid : { 9001u, 9002u, 9003u }) hostdb::remove_host_by_rid(*rig.pool.acquire(), rid);
	rig.tick(); // the revoked server's first stopping pass (the push; nothing is listed)
	rig.tick(); // no listing left: each stop finishes
	for (const int64_t ended : { banned, deleted, revoked }) TEST_EXPECT(rig.state(ended) == State::Stopped);

	// During a crash restart's backoff: no new credentials, no relaunch.
	const int64_t crashed = rig.request("owner_crashed");
	const std::string m4 = rig.to_running(crashed, 9004);
	TEST_EXPECT(!m4.empty());
	rig.fake.exit_with(m4, 3);
	rig.tick();
	TEST_EXPECT(rig.state(crashed) == State::Provisioning);
	const int launches = rig.fake.launches;
	const int64_t service = *rig.server(crashed)->service_user_id;
	const int64_t generation = nws::get_user_by_id(*rig.pool.acquire(), service)->credentials_gen;
	TEST_EXPECT(nws::set_can_host(*rig.pool.acquire(), *rig.server(crashed)->owner_user_id, false).ok);
	rig.advance(rig.config.restart_backoff_ms);
	rig.tick();
	TEST_EXPECT(rig.state(crashed) == State::Stopping && rig.fake.launches == launches);
	TEST_EXPECT(nws::get_user_by_id(*rig.pool.acquire(), service)->credentials_gen == generation);
	rig.tick();
	TEST_EXPECT(rig.state(crashed) == State::Stopped);
	return 0;
}

int test_reconciliation() {
	Rig rig("hosted_reconcile");
	const int64_t kept = rig.request("rec_kept");
	const std::string kept_machine = rig.to_running(kept, 9101);
	const int64_t lost = rig.request("rec_lost");
	const std::string lost_machine = rig.to_running(lost, 9102);
	const int64_t stopping = rig.request("rec_stopping");
	const std::string stopping_machine = rig.to_running(stopping, 9103);
	TEST_EXPECT(!kept_machine.empty() && !lost_machine.empty() && !stopping_machine.empty());
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), stopping, "stopped by its owner"));
	// The service goes down: its previous run's listings are gone (main()
	// clears active_hosts at boot), one machine vanished, and two machines run
	// that no live row names.
	for (const uint32_t rid : { 9101u, 9102u, 9103u }) hostdb::remove_host_by_rid(*rig.pool.acquire(), rid);
	rig.fake.vanish(lost_machine);
	rig.fake.vanish(stopping_machine);
	const std::string stray = rig.fake.adopt(4242);
	const std::string impostor = rig.fake.adopt(kept); // the kept server's id, another machine
	rig.make_supervisor();
	rig.advance(1000);
	rig.sup->reconcile(rig.now);

	TEST_EXPECT(rig.fake.was_terminated(stray) && rig.fake.was_terminated(impostor));
	TEST_EXPECT(!rig.fake.was_terminated(kept_machine));
	TEST_EXPECT(rig.state(kept) == State::Running);
	auto row = rig.server(lost);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && row->machine_id.empty());
	TEST_EXPECT(contains(last_detail(*rig.pool.acquire(), lost), "gone at the service's start"));
	TEST_EXPECT(rig.state(stopping) == State::Stopped);

	// The next pass launches the lost one again at once (no backoff: no crash).
	const int launches = rig.fake.launches;
	rig.advance(rig.config.health_interval_ms);
	rig.tick();
	TEST_EXPECT(rig.state(lost) == State::Booting && rig.fake.launches == launches + 1);
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), lost, "done"));

	// The resumed server's checks wait out its grace, unlisted and uncounted,
	// and its listing comes back within it.
	for (int i = 0; i < 2; ++i) {
		rig.advance(rig.config.health_interval_ms);
		rig.tick();
	}
	TEST_EXPECT(rig.state(kept) == State::Running);
	TEST_EXPECT(list_as(*rig.pool.acquire(), *rig.server(kept)->service_user_id, 9104));
	rig.advance(rig.config.listing_timeout_ms);
	rig.tick();
	TEST_EXPECT(rig.state(kept) == State::Running);
	TEST_EXPECT(rig.state(lost) == State::Stopped);
	return 0;
}

// A provider that knows a machine's address only once it boots (a cloud
// machine): the launch's describe names none, and the next describe writes it,
// so the running server's admin link has its target.
int test_address_after_launch() {
	Rig rig("hosted_address_late");
	rig.fake.hide_address_describes = 1;
	hosted::AdminLinkConfig link_config;
	link_config.io_timeout_ms = 200;
	link_config.reconnect_min_ms = 50;
	link_config.log = [](const std::string &) {};
	hosted::AdminLinks links(nullptr, link_config);
	rig.sup->set_admin_links(&links);
	const int64_t id = rig.request("late_address");
	TEST_EXPECT(id != 0);
	rig.tick(); // requested -> provisioning
	rig.tick(); // the launch: its describe names no address
	TEST_EXPECT(rig.state(id) == State::Booting && rig.server(id)->machine_address.empty());
	rig.fake.run(rig.fake.last_machine);
	rig.tick(); // -> listing, the address written
	TEST_EXPECT(rig.state(id) == State::Listing && rig.server(id)->machine_address == "10.0.0.2");
	TEST_EXPECT(list_as(*rig.pool.acquire(), *rig.server(id)->service_user_id, 9301));
	rig.tick(); // -> running
	const auto link = links.find(id);
	TEST_EXPECT(rig.state(id) == State::Running && link && link->target().host == "10.0.0.2");
	links.stop_all();
	return 0;
}

// The console ring (hosted_server_console, migration 0012): an append keeps a
// server's newest `keep` lines and leaves another server's alone; the reads go
// oldest first, past an id, the newest `limit`.
int test_console_ring() {
	Rig rig("hosted_console_ring");
	const int64_t a = rig.request("console_a");
	const int64_t b = rig.request("console_b");
	TEST_EXPECT(a != 0 && b != 0);
	auto line = [](const std::string &text) {
		hosted::ConsoleLine l;
		l.kind = "reply";
		l.text = text;
		l.at = "2026-10-12 10:00:00";
		return l;
	};
	auto db = rig.pool.acquire();
	hosted::append_console(*db, b, { line("b0") }, 5);
	for (int i = 0; i < 4; ++i) hosted::append_console(*db, a, { line("a" + std::to_string(i)) }, 5);
	hosted::append_console(*db, a, { line("a4"), line("a5"), line("a6") }, 5);
	auto texts = [&](int64_t id, int64_t after, size_t limit) {
		std::vector<std::string> out;
		for (const auto &l : hosted::list_console(*db, id, after, limit)) out.push_back(l.text);
		return out;
	};
	TEST_EXPECT(texts(a, 0, 100) == (std::vector<std::string>{ "a2", "a3", "a4", "a5", "a6" }));
	TEST_EXPECT(texts(b, 0, 100) == std::vector<std::string>{ "b0" });
	TEST_EXPECT(texts(a, 0, 2) == (std::vector<std::string>{ "a5", "a6" }));
	const auto all = hosted::list_console(*db, a, 0, 100);
	TEST_EXPECT(texts(a, all[2].id, 100) == (std::vector<std::string>{ "a5", "a6" }));
	TEST_EXPECT(all[0].kind == "reply" && all[0].at == "2026-10-12 10:00:00");
	// The schema's checks: a kind or actor outside the sets is refused.
	hosted::ConsoleLine bad = line("x");
	bad.actor = "player";
	bool refused = false;
	try {
		hosted::append_console(*db, a, { bad });
	} catch (const opennova::db::SqliteError &) {
		refused = true;
	}
	TEST_EXPECT(refused && texts(a, 0, 100).size() == 5);
	return 0;
}

// A row in provisioning whose machine is gone at the service's start (launched
// just before the service went, its move to booting never written): its
// machine is cleared with no event and no restart counted, and the next pass
// launches it again.
int test_reconcile_provisioning_gone() {
	Rig rig("hosted_reconcile_provisioning");
	const int64_t id = rig.request("rec_provisioning");
	TEST_EXPECT(id != 0);
	rig.tick(); // requested -> provisioning
	TEST_EXPECT(rig.state(id) == State::Provisioning);
	hosted::set_machine(*rig.pool.acquire(), id, "fake", "fake-ghost", "10.0.0.2");
	const std::vector<std::string> before = transitions(*rig.pool.acquire(), id);
	rig.make_supervisor();
	rig.advance(1000);
	rig.sup->reconcile(rig.now);

	const auto row = rig.server(id);
	TEST_EXPECT(row && row->state == State::Provisioning && row->machine_id.empty() &&
			row->machine_address.empty() && row->restarts == 0);
	TEST_EXPECT(transitions(*rig.pool.acquire(), id) == before);
	const int launches = rig.fake.launches;
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Booting && rig.fake.launches == launches + 1);
	TEST_EXPECT(rig.server(id)->machine_id != "fake-ghost" && !rig.server(id)->machine_id.empty());
	return 0;
}

// The admin links follow the rows (Supervisor::set_admin_links): none before
// running; one once running, at the machine's address with the admin login the
// start rendered; none from a crash's move back to provisioning, and a new one
// with the restart's new login once it runs again; none from stopping on.
int test_admin_links_follow_running() {
	Rig rig("hosted_admin_links");
	hosted::AdminLinkConfig link_config;
	link_config.io_timeout_ms = 200;
	link_config.reconnect_min_ms = 50;
	link_config.reconnect_max_ms = 200;
	link_config.log = [](const std::string &) {};
	hosted::AdminLinks links(nullptr, link_config);
	rig.sup->set_admin_links(&links);
	const int64_t id = rig.request("links_owner");
	TEST_EXPECT(id != 0);
	rig.tick();
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Booting && links.find(id) == nullptr);
	const std::string machine = rig.fake.last_machine;
	rig.fake.run(machine);
	rig.tick(); // -> listing
	TEST_EXPECT(links.find(id) == nullptr);
	TEST_EXPECT(list_as(*rig.pool.acquire(), *rig.server(id)->service_user_id, 9201));
	rig.tick(); // -> running
	TEST_EXPECT(rig.state(id) == State::Running);
	const auto link = links.find(id);
	const auto login = hosted::admin_login_of(*rig.pool.acquire(), id);
	TEST_EXPECT(link != nullptr && login.has_value());
	if (link && login) {
		TEST_EXPECT(link->target().host == "10.0.0.2");
		TEST_EXPECT(link->target().port == login->port && login->port == 41000 + id);
		TEST_EXPECT(link->target().user == "svc" && link->target().password == login->password);
		TEST_EXPECT(login->password.size() == hosted::kAdminPasswordChars);
	}
	rig.tick();
	TEST_EXPECT(links.find(id) == link); // the same target keeps its link

	// A crash: the link goes with the move out of running.
	rig.fake.exit_with(machine, 1);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Provisioning && links.find(id) == nullptr);
	TEST_EXPECT(!rig.back_to_running(id, 9202).empty());
	const auto relinked = links.find(id);
	TEST_EXPECT(relinked != nullptr && relinked != link);
	if (relinked && link) TEST_EXPECT(relinked->target().password != link->target().password);

	// The stop: no link from stopping on.
	TEST_EXPECT(hosted::request_stop(*rig.pool.acquire(), id, "stopped by its owner"));
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Stopping && links.find(id) == nullptr);

	// The health check counts the link (ADR 0053 d6): a running server whose link never
	// answers (no server behind it) fails after health_misses checks, listed all along.
	const int64_t quiet = rig.request("links_quiet");
	TEST_EXPECT(quiet != 0 && !rig.to_running(quiet, 9203).empty());
	TEST_EXPECT(links.find(quiet) != nullptr && !links.answering(quiet));
	for (int i = 0; i < rig.config.health_misses; ++i) {
		rig.advance(rig.config.health_interval_ms);
		rig.tick();
	}
	const auto failed = rig.server(quiet);
	TEST_EXPECT(failed && failed->state == State::Failed &&
			contains(failed->failure_reason, "no answer on its admin link"));
	TEST_EXPECT(links.find(quiet) == nullptr);
	links.stop_all();
	TEST_EXPECT(link == nullptr || link->finished());
	return 0;
}

// --- LocalProcessProvider over this binary as the server ----------------------

// The variable the test plants in its own environment, which no server may see.
constexpr const char *kPlantedSecret = "OPENNOVA_HOSTED_PLANTED_SECRET";

volatile std::sig_atomic_t g_fake_serve_terminated = 0;
void on_fake_serve_term(int) { g_fake_serve_terminated = 1; }

// The fake opennova-serve: prints its arguments, the files it found and the
// names in its environment, then waits for exit.txt (its code) or a stop. On
// POSIX a SIGTERM (opennova-serve's orderly stop) writes terminated.txt first.
int fake_serve_main(int argc, char **argv) {
#ifndef _WIN32
	std::signal(SIGTERM, on_fake_serve_term);
#endif
	for (int i = 1; i < argc; ++i) std::printf("arg %s\n", argv[i]);
	for (const char *name : { "server.host", "credentials.txt" }) {
		std::printf("file %s %s\n", name, fs::exists(name) ? "present" : "missing");
	}
	for (const std::string &entry : opennova::proc::current_environment())
		std::printf("env %s\n", entry.substr(0, entry.find('=')).c_str());
	std::fflush(stdout);
	for (int i = 0; i < 6000; ++i) {
		if (g_fake_serve_terminated) {
			std::ofstream marker("terminated.txt");
			marker << "SIGTERM";
			return 0;
		}
		std::ifstream code_file("exit.txt");
		int code = 0;
		if (code_file >> code) return code;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return 99;
}

// Plants kPlantedSecret in this process's environment.
void plant_secret() {
#ifdef _WIN32
	_putenv_s(kPlantedSecret, "hunter2");
#else
	setenv(kPlantedSecret, "hunter2", 1);
#endif
}

std::string upper_ascii(std::string text) {
	for (char &c : text) {
		if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
	}
	return text;
}

bool wait_state(hosted::MachineProvider &p, const std::string &id, hosted::MachineState want) {
	for (int i = 0; i < 1000; ++i) {
		if (p.describe(id).state == want) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

int test_local_provider() {
	test_temp::TempDir temp("hosted_local");
	const fs::path data = temp.path / "data dir";
	const fs::path work = temp.path / "work";
	fs::create_directories(data);
	hosted::LocalProcessConfig config;
	config.serve_binary = g_self;
	config.resource_dir = data.u8string();
	config.loose_root = true;
	config.work_dir = work.u8string();
	hosted::LocalProcessProvider provider(config);
	TEST_EXPECT(std::string(provider.name()) == "local");
	const hosted::MachinePlan plan = provider.plan(7);
	TEST_EXPECT(plan.nw_port != 0 && plan.admin_port != 0 && plan.admin_allowed_ip == "127.0.0.1");

	hosted::LaunchSpec spec;
	spec.server_id = 7;
	spec.files = { { "server.host", "GameName \"x\"\r\n", false }, { "credentials.txt", "NOVAWORLD_PASS=s\n", true } };
	spec.serve_args = { "/HOST", "server.host", "--marker", "two words" };
	// A file a previous start left is gone: the directory is rendered from scratch.
	fs::create_directories(provider.server_dir(7));
	{
		std::ofstream stale(provider.server_dir(7) / "stale.txt");
		stale << "old";
	}
	std::string error;
	const auto id = provider.launch(spec, error);
	if (!id) std::fprintf(stderr, "  launch: %s\n", error.c_str());
	TEST_EXPECT(id.has_value());
	const fs::path dir = provider.server_dir(7);
	TEST_EXPECT(!fs::exists(dir / "stale.txt"));
	TEST_EXPECT(fs::exists(dir / "server.host") && fs::exists(dir / "credentials.txt"));
#ifndef _WIN32
	struct stat st {};
	TEST_EXPECT(::stat((dir / "credentials.txt").c_str(), &st) == 0 && (st.st_mode & 077) == 0);
	TEST_EXPECT(::stat(dir.c_str(), &st) == 0 && (st.st_mode & 077) == 0);
#endif
	TEST_EXPECT(provider.describe(*id).state == hosted::MachineState::Running);
	TEST_EXPECT(provider.describe(*id).address == "127.0.0.1");
	const auto tagged = provider.list_tagged();
	TEST_EXPECT(tagged.size() == 1 && tagged[0].machine_id == *id && tagged[0].server_id == 7);

	{
		std::ofstream code(dir / "exit.txt");
		code << "5";
	}
	TEST_EXPECT(wait_state(provider, *id, hosted::MachineState::Exited));
	TEST_EXPECT(provider.describe(*id).exit_code == 5);
	std::string console;
	std::string read_error;
	TEST_EXPECT(opennova::io::read_file_text((dir / "console.log").u8string(), console, read_error));
	const std::string want_args[] = { "arg --resource-dir\n", "arg " + fs::absolute(data).u8string() + "\n",
		"arg --loose-root\n", "arg /HOST\n", "arg server.host\n", "arg --marker\n", "arg two words\n",
		"file server.host present\n", "file credentials.txt present\n" };
	std::string normalized;
	for (const char c : console) {
		if (c != '\r') normalized.push_back(c);
	}
	for (const std::string &line : want_args) {
		if (!contains(normalized, line)) std::fprintf(stderr, "  console.log lacks %s", line.c_str());
		TEST_EXPECT(contains(normalized, line));
	}
	// Its environment is the allowlist's: the service's own variables (here a
	// planted one) never reach it; PATH does.
	TEST_EXPECT(!contains(normalized, std::string("env ") + kPlantedSecret));
	TEST_EXPECT(contains(upper_ascii(normalized), "ENV PATH\n"));
	provider.terminate(*id);
	TEST_EXPECT(provider.describe(*id).state == hosted::MachineState::Gone);
	TEST_EXPECT(provider.list_tagged().empty());
	// Its secret files went with it; the console log and the rest stay.
	TEST_EXPECT(!fs::exists(dir / "credentials.txt"));
	TEST_EXPECT(fs::exists(dir / "console.log") && fs::exists(dir / "server.host"));
	provider.terminate(*id); // gone already: no error

	// A running one ends at terminate, and the destructor ends the rest.
	const auto running = provider.launch(spec, error);
	TEST_EXPECT(running.has_value() && *running != *id);
	TEST_EXPECT(!fs::exists(dir / "exit.txt"));
	TEST_EXPECT(provider.describe(*running).state == hosted::MachineState::Running);
	provider.terminate(*running);
	TEST_EXPECT(provider.describe(*running).state == hosted::MachineState::Gone);
	spec.server_id = 8;
	TEST_EXPECT(provider.launch(spec, error).has_value());

	// Missing configuration and a bad file name refuse the launch (a server of
	// its own: server 8's directory is in use).
	hosted::LocalProcessProvider unset(hosted::LocalProcessConfig{});
	TEST_EXPECT(!unset.launch(spec, error) && contains(error, "ONNET_HOSTED_SERVE_BIN"));
	spec.server_id = 9;
	spec.files.push_back({ "../escape.txt", "x", false });
	TEST_EXPECT(!provider.launch(spec, error) && contains(error, "bare file name"));
	return 0;
}

// The local provider's children outlive the supervisor's thread: on Linux their
// parent-death signal follows the thread that started them, which is the
// provider's own spawner, so joining the supervisor (stop()) ends none of them,
// and the provider's end then stops each in order (SIGTERM, which the fake
// server answers by writing terminated.txt; TerminateProcess on Windows).
int test_children_outlive_the_supervisor_thread() {
	test_temp::TempDir temp("hosted_thread");
	ConnectionPool pool(temp.path / "thread.db");
	opennova::db::run_migrations(*pool.acquire(), fs::path(OPENNOVA_SOURCE_DIR) / "backend" / "migrations");
	const int64_t owner = make_player(*pool.acquire(), "thread_owner", true);
	TEST_EXPECT(owner > 0);
	const auto request = hosted::request_server(*pool.acquire(), owner, valid_settings(), 5);
	TEST_EXPECT(request.outcome == hosted::RequestOutcome::Created);
	const fs::path data = temp.path / "data";
	fs::create_directories(data);
	hosted::LocalProcessConfig config;
	config.serve_binary = g_self;
	config.resource_dir = data.u8string();
	config.loose_root = true;
	config.work_dir = (temp.path / "work").u8string();
	fs::path marker;
	{
		hosted::LocalProcessProvider provider(config);
		hosted::SupervisorConfig supervision;
		supervision.tick_interval_ms = 20;
		supervision.listing_timeout_ms = 10 * 60 * 1000;
		{
			hosted::Supervisor supervisor(pool, provider, supervision);
			supervisor.start();
			bool launched = false;
			for (int i = 0; i < 1500 && !launched; ++i) {
				const auto row = hosted::find_server(*pool.acquire(), request.id);
				launched = row && row->state == State::Listing;
				if (!launched) std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			TEST_EXPECT(launched);
			supervisor.stop();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		const auto tagged = provider.list_tagged();
		TEST_EXPECT(tagged.size() == 1);
		TEST_EXPECT(provider.describe(tagged[0].machine_id).state == hosted::MachineState::Running);
		marker = provider.server_dir(request.id) / "terminated.txt";
	}
#ifndef _WIN32
	TEST_EXPECT(fs::exists(marker));
#endif
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--resource-dir") == 0) return fake_serve_main(argc, argv);
	if (argc < 2) {
		std::fprintf(stderr, "usage: hosted_supervisor_test <this binary's path>\n");
		return 2;
	}
	g_self = argv[1];
	plant_secret();
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "settings_rules", test_settings_rules },
		{ "renders", test_renders },
		{ "service_accounts", test_service_accounts },
		{ "requests", test_requests },
		{ "lifecycle_and_stop", test_lifecycle_and_stop },
		{ "stop_paths", test_stop_paths },
		{ "exits_and_crash_restarts", test_exits_and_crash_restarts },
		{ "restart_window", test_restart_window },
		{ "timeouts_and_health", test_timeouts_and_health },
		{ "owner_changes", test_owner_changes },
		{ "reconciliation", test_reconciliation },
		{ "reconcile_provisioning_gone", test_reconcile_provisioning_gone },
		{ "console_ring", test_console_ring },
		{ "admin_links_follow_running", test_admin_links_follow_running },
		{ "address_after_launch", test_address_after_launch },
		{ "orphan_sweep", test_orphan_sweep },
		{ "local_provider", test_local_provider },
		{ "children_outlive_the_supervisor_thread", test_children_outlive_the_supervisor_thread },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: hosted servers' supervisor\n");
	return 0;
}
