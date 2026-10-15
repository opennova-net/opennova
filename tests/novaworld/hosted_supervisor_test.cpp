// The hosted servers' supervisor and what it stands on (apps/novaworld_server/
// hosted), on a migrated database with a FakeDriver whose machines' agents the
// test plays (hosted_fake_driver.h, hosted_fleet_rig.h) and a hand-stepped
// clock: the data sets (the catalog, the variable's read, what each machine
// driver offers, a server whose data set is no longer offered), the owner's
// settings and their checks against the data set's limits and expansions, the
// files and the arguments rendered from them for a slot (read back by the
// formats' own readers; /game per data set), the service accounts (minted,
// refused by the website, hidden, reserved, their password set again at every
// start, deleted at the end), the start request's checks (the hosting
// entitlement, the global cap, the fleet's room under the planner), each
// server's lifecycle on the fleet (a shared machine launched and waited for,
// two servers on one machine's slots, an own machine of its own), the stop (the
// push, the listing's end, the 60 s timeout, any exit while stopping, a stop
// before any slot, a stop landing mid-placement, the kill switch), the crash
// restart and its limit (any exit, the OOM kill, a start that failed; a stop is
// never read from an exit code), a launch the cloud refused costing the server
// no restart, the restart window, the boot and listing timeouts, the health
// checks, a lost machine placing its server again with no restart counted (and
// holding its listing's clock), an owner's account banned
// or deleted, its comp grant withdrawn or its support tier canceled (also
// during a crash restart's backoff), the owners' limits and their hold, the
// reconciliation at the service's start, and the admin links kept in step with
// the running servers. Compiles the server's auth.cpp, web_session.cpp and the
// hosted sources directly, so it runs on every build.

#include "hosted_fleet_rig.h"

#include "hosted/admin_link.h"
#include "web_session.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/cp1252.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>

#include "common/test_expect.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace hosted_rig;

namespace {

// The file `name` of the container the server's agent was given.
std::string file_of(Rig &rig, int64_t id, const std::string &name) {
	const FakeAgent::Container *c = rig.running_container(id);
	if (!c || !c->desired.files) return {};
	const auto it = c->desired.files->find(name);
	return it == c->desired.files->end() ? std::string() : it->second;
}

// --- the settings and the renders ---------------------------------------------

int test_settings_rules() {
	const std::vector<hosted::DataSet> &offered = hosted::known_data_sets();
	const auto check = [&offered](const hosted::HostedSettings &c) { return hosted::check_settings(c, offered); };
	hosted::HostedSettings s = valid_settings();
	TEST_EXPECT(!check(s));
	auto refused = [&check](hosted::HostedSettings bad, const char *field) {
		const auto r = check(bad);
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
	TEST_EXPECT(!check(with([](auto &c) { c.game_name = std::string(31, 'n'); })));
	// 31 characters of two UTF-8 bytes each are 31 cp1252 bytes.
	TEST_EXPECT(!check(with([](auto &c) {
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
	TEST_EXPECT(!check(with([](auto &c) { c.game_password = "Secret!16-chars."; })));
	TEST_EXPECT(refused(with([](auto &c) { c.max_players = 1; }), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) { c.max_players = 65; }), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) { c.time_limit = 0; }), "time_limit"));
	TEST_EXPECT(refused(with([](auto &c) { c.kill_limit = 501; }), "kill_limit"));
	TEST_EXPECT(refused(with([](auto &c) { c.max_score = 0; }), "max_score"));
	TEST_EXPECT(refused(with([](auto &c) { c.respawn = -1; }), "respawn"));
	TEST_EXPECT(refused(with([](auto &c) { c.start_delay = 301; }), "start_delay"));
	TEST_EXPECT(refused(with([](auto &c) { c.expansion = "jox02"; }), "expansion"));
	TEST_EXPECT(!check(with([](auto &c) { c.expansion = "jox01"; })));
	// The game: one the service offers, whose expansions and limits the rest take.
	TEST_EXPECT(refused(with([](auto &c) { c.game.clear(); }), "game"));
	TEST_EXPECT(refused(with([](auto &c) { c.game = "jop_3_consumer"; }), "game"));
	TEST_EXPECT(!check(with([](auto &c) { c.game = hosted::kDataSetDeltaForceXtreme2; })));
	TEST_EXPECT(!check(with([](auto &c) { c.game = hosted::kDataSetOpenNova; })));
	// Delta Force Xtreme 2's host screen takes 100 players, Joint Operations' and
	// OpenNova's 64.
	TEST_EXPECT(!check(with([](auto &c) {
		c.game = hosted::kDataSetDeltaForceXtreme2;
		c.max_players = 100;
	})));
	TEST_EXPECT(refused(with([](auto &c) {
		c.game = hosted::kDataSetDeltaForceXtreme2;
		c.max_players = 101;
	}), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) {
		c.game = hosted::kDataSetOpenNova;
		c.max_players = 65;
	}), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) {
		c.game = hosted::kDataSetDeltaForceXtreme2;
		c.expansion = "jox01";
	}), "expansion"));
	TEST_EXPECT(refused(with([](auto &c) {
		c.game = hosted::kDataSetOpenNova;
		c.expansion = "jox01";
	}), "expansion"));
	{
		// Only the deployment's: a known data set it does not offer is refused.
		std::vector<hosted::DataSet> jo_only = { *hosted::find_data_set(offered, hosted::kDataSetJointOperations) };
		hosted::HostedSettings dfx2 = s;
		dfx2.game = hosted::kDataSetDeltaForceXtreme2;
		const auto r = hosted::check_settings(dfx2, jo_only);
		TEST_EXPECT(r && r->field == "game");
		TEST_EXPECT(!hosted::check_settings(s, jo_only));
		// A data set's own limits: a narrower row refuses what Joint Operations' takes.
		jo_only[0].limits.max_players = { 2, 16 };
		hosted::HostedSettings big = s;
		big.max_players = 32;
		const auto too_many = hosted::check_settings(big, jo_only);
		TEST_EXPECT(too_many && too_many->field == "max_players" && too_many->reason == "must be 2 to 16");
		TEST_EXPECT(hosted::check_settings(big, offered) == std::nullopt);
		hosted::HostedSettings escalation = s;
		escalation.game = hosted::kDataSetDeltaForceXtreme2;
		escalation.expansion = "jox01";
		const auto no_exp = hosted::check_settings(escalation, offered);
		TEST_EXPECT(no_exp && no_exp->reason == "is \"\" (the base game)");
		escalation.game = hosted::kDataSetJointOperations;
		escalation.expansion = "jox02";
		const auto two = hosted::check_settings(escalation, offered);
		TEST_EXPECT(two && two->reason == "is \"\" (the base game) or \"jox01\"");
	}
	TEST_EXPECT(refused(with([](auto &c) { c.missions.clear(); }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions.assign(33, "map.bms"); }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "../up.bms" }; }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "map.txt" }; }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "two words.bms" }; }), "missions"));
	TEST_EXPECT(!check(with([](auto &c) { c.missions = { "ASH_G11A.BMS", "x-y_1.bms" }; })));

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

	// Slot 3 of a machine: its own game port and admin port.
	hosted::SlotPlan plan;
	plan.game_port = hosted::slot_game_port(3);
	plan.admin_port = hosted::slot_admin_port(3);
	plan.admin_allowed_ip = "10.1.2.3";
	TEST_EXPECT(plan.game_port == 32771 && plan.admin_port == 32803);
	const std::string cfg_text = hosted::render_game_cfg(plan);
	const auto cfg = opennova::gamecfg::load(cfg_text.data(), cfg_text.size(), {});
	TEST_EXPECT(!cfg.version_reset);
	TEST_EXPECT(cfg.cfg.networkconnecttype == 1);
	TEST_EXPECT(cfg.cfg.mp_novaworld_port_min == 32771 && cfg.cfg.mp_novaworld_port_max == 32771);
	TEST_EXPECT(cfg.cfg.remote_admin_port == 32803);
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
	const auto &known = hosted::known_data_sets();
	const hosted::LaunchSpec spec =
			hosted::render_launch(s, *hosted::find_data_set(known, hosted::kDataSetJointOperations), in);
	TEST_EXPECT(spec.server_id == 12 && spec.files.size() == 4);
	TEST_EXPECT(spec.data_set == "jop_2_consumer");
	for (const auto &f : spec.files) {
		const bool secret = f.name != "game.cfg"; // the host file carries the game password
		TEST_EXPECT(f.secret == secret);
	}
	const std::vector<std::string> args = { "/HOST", "server.host", "--credentials", "credentials.txt",
		"--master-host", "203.0.113.5", "--master-gate-port", "7597", "/game", "jo", "/exp", "jox01" };
	TEST_EXPECT(spec.serve_args == args);
	TEST_EXPECT(hosted::render_serve_args(s, *hosted::find_data_set(known, hosted::kDataSetJointOperations),
						"203.0.113.5", 7597) == args);

	// Each data set speaks its own title; OpenNova's data speaks Joint Operations'.
	s.expansion.clear();
	s.game = hosted::kDataSetDeltaForceXtreme2;
	const hosted::LaunchSpec dfx2 =
			hosted::render_launch(s, *hosted::find_data_set(known, hosted::kDataSetDeltaForceXtreme2), in);
	TEST_EXPECT(dfx2.data_set == "dfx2_consumer");
	const std::vector<std::string> dfx2_args = { "/HOST", "server.host", "--credentials", "credentials.txt",
		"--master-host", "203.0.113.5", "--master-gate-port", "7597", "/game", "dfx2" };
	TEST_EXPECT(dfx2.serve_args == dfx2_args);
	s.game = hosted::kDataSetOpenNova;
	const hosted::LaunchSpec on = hosted::render_launch(s, *hosted::find_data_set(known, hosted::kDataSetOpenNova), in);
	TEST_EXPECT(on.data_set == "opennova_0_consumer");
	TEST_EXPECT(on.serve_args.size() == 10 && on.serve_args[8] == "/game" && on.serve_args[9] == "jo");
	return 0;
}

// --- the data sets ----------------------------------------------------------------

// A server whose data set the deployment stopped offering after its request
// fails at its placement, before any account or machine: no restart mends that.
int test_game_no_longer_offered() {
	Rig rig("hosted_game_gone");
	hosted::HostedSettings settings = valid_settings();
	settings.game = hosted::kDataSetDeltaForceXtreme2;
	const int64_t id = rig.request("gone_owner", settings);
	TEST_EXPECT(id > 0);
	TEST_EXPECT(rig.server(id)->settings.game == "dfx2_consumer");
	rig.config.data_sets = { *hosted::find_data_set(hosted::known_data_sets(), hosted::kDataSetJointOperations) };
	rig.make_supervisor();
	rig.tick(); // -> provisioning
	rig.tick(); // -> failed
	const auto row = rig.server(id);
	TEST_EXPECT(row && row->state == State::Failed);
	TEST_EXPECT(row && row->failure_reason == "its game (dfx2_consumer) is no longer hosted here");
	TEST_EXPECT(rig.fake.launches == 0 && row && !row->service_user_id && !row->machine_ref);
	return 0;
}

int test_data_sets() {
	// The catalog: Joint Operations first, each title a gameprofile code
	// opennova-serve's /game takes, the base game first among its expansions.
	const auto &known = hosted::known_data_sets();
	TEST_EXPECT(known.size() == 3);
	TEST_EXPECT(known[0].game == "jop_2_consumer" && known[1].game == "dfx2_consumer" &&
			known[2].game == "opennova_0_consumer");
	for (const hosted::DataSet &d : known) {
		TEST_EXPECT(opennova::gameprofile::gameprofile_by_code(d.title.c_str()) != nullptr);
		TEST_EXPECT(!d.display_name.empty() && !d.expansions.empty() && d.expansions[0].code.empty());
		TEST_EXPECT(d.limits.max_players.min >= 2 && d.limits.max_players.min <= d.limits.max_players.max);
		TEST_EXPECT(d.missions.empty() && d.resource_dir.empty());
	}
	TEST_EXPECT(known[0].title == "jo" && known[1].title == "dfx2" && known[2].title == "jo");
	TEST_EXPECT(known[0].expansions.size() == 2 && known[0].expansions[1].code == "jox01");
	TEST_EXPECT(known[1].expansions.size() == 1 && known[2].expansions.size() == 1);
	TEST_EXPECT(hosted::find_expansion(known[0], "jox01") && !hosted::find_expansion(known[1], "jox01"));
	TEST_EXPECT(!hosted::find_data_set(known, "jop_2") && !hosted::find_data_set(known, ""));

	// ONNET_HOSTED_DATA_SETS, by data set.
	std::vector<hosted::DataSetConfig> configs;
	std::string error;
	TEST_EXPECT(hosted::parse_data_set_configs(
			"{\"jop_2_consumer\":{\"missions\":[\"a.bms\",\"B-2.BMS\"],\"resource_dir\":\"C:/JO\",\"loose_root\":false},"
			"\"dfx2_consumer\":{\"resource_dir\":\"/data/dfx2\",\"loose_root\":true},"
			"\"opennova_0_consumer\":{}}",
			configs, error));
	TEST_EXPECT(error.empty() && configs.size() == 3);
	const auto config_of = [&configs](const char *game) -> const hosted::DataSetConfig * {
		for (const auto &c : configs) {
			if (c.game == game) return &c;
		}
		return nullptr;
	};
	const auto *jo = config_of("jop_2_consumer");
	TEST_EXPECT(jo && jo->missions == std::vector<std::string>({ "a.bms", "B-2.BMS" }) && jo->resource_dir == "C:/JO" &&
			!jo->loose_root);
	const auto *dfx2 = config_of("dfx2_consumer");
	TEST_EXPECT(dfx2 && dfx2->missions.empty() && dfx2->loose_root);
	const auto *on = config_of("opennova_0_consumer");
	TEST_EXPECT(on && on->resource_dir.empty() && on->missions.empty());
	// Unset, it is an empty object.
	TEST_EXPECT(hosted::parse_data_set_configs(" ", configs, error) && configs.empty());
	// Anything else is refused, naming what.
	const auto refused = [&](const char *sets, const char *names) {
		std::vector<hosted::DataSetConfig> out;
		std::string why;
		const bool ok = hosted::parse_data_set_configs(sets, out, why);
		if (ok || why.find(names) == std::string::npos)
			std::fprintf(stderr, "  want a refusal naming '%s', got '%s'\n", names, why.c_str());
		return !ok && why.find(names) != std::string::npos;
	};
	TEST_EXPECT(refused("not json", "ONNET_HOSTED_DATA_SETS is no JSON"));
	TEST_EXPECT(refused("[]", "ONNET_HOSTED_DATA_SETS is no JSON object"));
	TEST_EXPECT(refused("{\"jop_3\":{}}", "no data set"));
	TEST_EXPECT(refused("{\"jop_2_consumer\":[]}", "jop_2_consumer is no JSON object"));
	TEST_EXPECT(refused("{\"jop_2_consumer\":{\"missions\":\"a.bms\"}}", "missions is no list"));
	TEST_EXPECT(refused("{\"jop_2_consumer\":{\"missions\":[\"../a.bms\"]}}", "no .bms file name"));
	TEST_EXPECT(refused("{\"jop_2_consumer\":{\"resource_dir\":1}}", "resource_dir is no string"));
	TEST_EXPECT(refused("{\"jop_2_consumer\":{\"loose_root\":\"1\"}}", "loose_root is no true or false"));
	TEST_EXPECT(refused("{\"jop_2_consumer\":{\"mission\":[]}}", "'mission'"));

	// What each driver offers, in the catalog's order, with the config's values:
	// the local driver the data sets with a folder, the ec2 driver every one named
	// (each cloud machine mounts them all).
	TEST_EXPECT(hosted::parse_data_set_configs(
			"{\"dfx2_consumer\":{\"resource_dir\":\"/data/dfx2\",\"loose_root\":true,\"missions\":[\"d.bms\"]},"
			"\"jop_2_consumer\":{\"missions\":[\"a.bms\"],\"resource_dir\":\"C:/JO\"},"
			"\"opennova_0_consumer\":{\"missions\":[\"o.bms\"]}}",
			configs, error));
	const auto local = hosted::offered_data_sets("local", configs);
	TEST_EXPECT(local.size() == 2 && local[0].game == "jop_2_consumer" && local[1].game == "dfx2_consumer");
	TEST_EXPECT(local[0].resource_dir == "C:/JO" && local[0].missions == std::vector<std::string>{ "a.bms" });
	TEST_EXPECT(local[1].loose_root && local[1].title == "dfx2" && local[1].limits.max_players.max == 100);
	const auto ec2 = hosted::offered_data_sets("ec2", configs);
	TEST_EXPECT(ec2.size() == 3 && ec2[0].game == "jop_2_consumer" && ec2[2].game == "opennova_0_consumer" &&
			ec2[2].missions == std::vector<std::string>{ "o.bms" });
	TEST_EXPECT(hosted::offered_data_sets("none", configs).empty());
	TEST_EXPECT(hosted::offered_data_sets("fake", configs).empty());
	return 0;
}

// --- the service accounts -----------------------------------------------------

int test_service_accounts() {
	test_db::TestDatabase scratch;
	ConnectionPool pool(scratch.options());
	auto lease = pool.acquire();
	Database &db = *lease;
	const int64_t player = make_player(db, "acct_player", 0);
	TEST_EXPECT(player > 0);

	const auto made = nws::upsert_service_account(db, "srv-7", "first-password");
	TEST_EXPECT(made.ok && made.id > 0);
	auto account = nws::get_user_by_id(db, made.id);
	TEST_EXPECT(account && account->kind == "service" && account->nwhandle == "srv-7");
	TEST_EXPECT(account->comp_servers == 0 && account->role == "player");
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
	TEST_EXPECT(nws::set_comp_servers(db, made.id, 1).error_code == "not_found");
	TEST_EXPECT(nws::bootstrap_admin(db, "srv-7").outcome == nws::BootstrapAdminResult::Outcome::NoSuchAccount);
	// A session row planted by hand resolves to nothing.
	db.exec("INSERT INTO web_sessions (token_hash, user_id, expires_at) "
			"VALUES ($1, $2, (now() + interval '+1 day'));",
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
	TEST_EXPECT(db.query("SELECT 1 FROM host_keys WHERE user_id = $1;", { BindValue(made.id) }).empty());

	// A player holding the name (a database written by hand) keeps it.
	db.exec("INSERT INTO players (username, password_hash, pcid, nwh, nwhandle) "
			"VALUES ('srv-8', 'x', '0e0e0e08', '1', 'HandMade');");
	TEST_EXPECT(nws::upsert_service_account(db, "srv-8", "pw").error_code == "username_exists");
	TEST_EXPECT(nws::upsert_service_account(db, "player-name", "pw").error_code == "invalid_field");

	TEST_EXPECT(nws::delete_service_account(db, player).error_code == "not_found");
	TEST_EXPECT(nws::delete_service_account(db, made.id).ok);
	TEST_EXPECT(!nws::get_user_by_id(db, made.id));
	// The comp grant: 0..16.
	TEST_EXPECT(nws::set_comp_servers(db, player, 2).ok && nws::get_user_by_id(db, player)->comp_servers == 2);
	TEST_EXPECT(nws::set_comp_servers(db, player, 17).error_code == "invalid_field");
	TEST_EXPECT(nws::set_comp_servers(db, player, -1).error_code == "invalid_field");
	TEST_EXPECT(nws::set_comp_servers(db, 999999, 1).error_code == "not_found");
	TEST_EXPECT(nws::get_user_by_id(db, player)->comp_servers == 2);
	return 0;
}

// --- the requests ---------------------------------------------------------------

int test_requests() {
	Rig rig("hosted_requests");
	Database &db = rig.db;
	const auto settings = valid_settings();
	const std::vector<hosted::DataSet> &offered = hosted::known_data_sets();
	const auto request = [&](int64_t user, const hosted::HostedSettings &with,
								 const std::vector<hosted::DataSet> &sets, int cap) {
		return hosted::request_server(db, user, with, sets, cap, kWall);
	};
	const int64_t unentitled = make_player(db, "req_unentitled", 0);
	TEST_EXPECT(request(unentitled, settings, offered, 3).outcome == hosted::RequestOutcome::NotEntitled);
	TEST_EXPECT(request(999999, settings, offered, 3).outcome == hosted::RequestOutcome::NoSuchUser);
	const auto service = nws::upsert_service_account(db, "srv-500", "pw");
	TEST_EXPECT(request(service.id, settings, offered, 3).outcome == hosted::RequestOutcome::NoSuchUser);
	const int64_t banned = make_player(db, "req_banned", 1);
	nws::UpdateUserParams ban;
	ban.account_status = "banned";
	TEST_EXPECT(nws::update_user(db, banned, ban).ok);
	TEST_EXPECT(request(banned, settings, offered, 3).outcome == hosted::RequestOutcome::NotEntitled);
	// A support tier entitles; past due it starts nothing, canceled it entitles nothing.
	const int64_t subscriber = make_player(db, "req_subscriber", 0);
	subscribe(db, subscriber, 1, "past_due", "sub_req");
	TEST_EXPECT(request(subscriber, settings, offered, 3).outcome == hosted::RequestOutcome::PastDue);
	subscribe(db, subscriber, 1, "canceled", "sub_req");
	TEST_EXPECT(request(subscriber, settings, offered, 3).outcome == hosted::RequestOutcome::NotEntitled);
	// Its period ran out (and the 48 h net with it): nothing either.
	subscribe(db, subscriber, 1, "active", "sub_req");
	TEST_EXPECT(hosted::request_server(db, subscriber, settings, offered, 3, kWall + 33 * 24 * 60 * 60).outcome ==
			hosted::RequestOutcome::NotEntitled);

	// As many live servers as the entitlement grants, a stopping one counted.
	const int64_t a = make_player(db, "req_a", 1);
	hosted::HostedSettings unchecked = settings;
	unchecked.missions.clear();
	TEST_EXPECT(request(a, unchecked, offered, 3).outcome == hosted::RequestOutcome::InvalidSettings);
	const auto first = request(a, settings, offered, 3);
	TEST_EXPECT(first.outcome == hosted::RequestOutcome::Created && first.id > 0);
	const auto second = request(a, settings, offered, 3);
	TEST_EXPECT(second.outcome == hosted::RequestOutcome::AtLimit && second.max_servers == 1 && second.used == 1);
	TEST_EXPECT(transitions(db, first.id) == std::vector<std::string>{ ">requested" });
	TEST_EXPECT(hosted::count_live_of(db, a) == 1 && hosted::count_own_live_of(db, a) == 0);
	const auto row = hosted::find_server(db, first.id);
	TEST_EXPECT(row && row->state == State::Requested && row->owner_user_id == a);
	TEST_EXPECT(row->settings.missions == settings.missions && row->settings.game_name == settings.game_name);
	TEST_EXPECT(row->settings.game == "jop_2_consumer");
	TEST_EXPECT(!row->machine_ref && row->slot == -1 && row->generation == 0 && row->serve_image.empty());
	// A game the deployment does not offer is refused here too.
	hosted::HostedSettings dfx2 = settings;
	dfx2.game = hosted::kDataSetDeltaForceXtreme2;
	const std::vector<hosted::DataSet> jo_only = { offered[0] };
	const int64_t e = make_player(db, "req_e", 1);
	TEST_EXPECT(request(e, dfx2, jo_only, 3).outcome == hosted::RequestOutcome::InvalidSettings);
	TEST_EXPECT(hosted::list_servers_of(db, e).empty());

	// The cap counts every live server.
	const int64_t b = make_player(db, "req_b", 1);
	const int64_t c = make_player(db, "req_c", 1);
	const int64_t d = make_player(db, "req_d", 1);
	TEST_EXPECT(request(b, settings, offered, 3).outcome == hosted::RequestOutcome::Created);
	const auto third = request(c, settings, offered, 3);
	TEST_EXPECT(third.outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::count_live(db) == 3);
	TEST_EXPECT(request(d, settings, offered, 3).outcome == hosted::RequestOutcome::AtCapacity);
	// An ended server frees its place, and its owner may start again.
	TEST_EXPECT(hosted::transition(db, third.id, State::Requested, State::Failed, "test"));
	TEST_EXPECT(request(d, settings, offered, 3).outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(request(c, settings, offered, 4).outcome == hosted::RequestOutcome::Created);
	const auto of_c = hosted::list_servers_of(db, c);
	TEST_EXPECT(of_c.size() == 2 && of_c[0].state == State::Requested && of_c[1].id == third.id);
	TEST_EXPECT(hosted::list_servers(db, 10).size() == 5);
	// An owner's server by id, and another's not.
	TEST_EXPECT(hosted::owned_server(db, first.id, a) && !hosted::owned_server(db, first.id, c));

	// A grant of two: two live, the third at the limit; the own-machine add-on
	// counted apart, a tier needed for it (a comp grant buys no machine).
	const int64_t g = make_player(db, "req_g", 2);
	const int big_cap = 100;
	TEST_EXPECT(request(g, settings, offered, big_cap).outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::request_server(db, g, settings, offered, big_cap, kWall, /*own_machine=*/true).outcome ==
			hosted::RequestOutcome::NoOwnMachine);
	TEST_EXPECT(request(g, settings, offered, big_cap).outcome == hosted::RequestOutcome::Created);
	const auto at_limit = request(g, settings, offered, big_cap);
	TEST_EXPECT(at_limit.outcome == hosted::RequestOutcome::AtLimit && at_limit.max_servers == 2 &&
			at_limit.used == 2);
	// A stopping server still counts.
	const auto g_live = hosted::live_servers_of(db, g);
	TEST_EXPECT(g_live.size() == 2 && g_live[0].id < g_live[1].id);
	TEST_EXPECT(hosted::request_stop(db, g_live[0].id, "test"));
	TEST_EXPECT(request(g, settings, offered, big_cap).outcome == hosted::RequestOutcome::AtLimit);
	// A tier with one own machine: one server on it, the second refused.
	const int64_t m = make_player(db, "req_m", 0);
	subscribe(db, m, 2, "active", "sub_req_m");
	subscribe_machines(db, m, 1, "sub_req_m_machine");
	const auto own = hosted::request_server(db, m, settings, offered, big_cap, kWall, true);
	TEST_EXPECT(own.outcome == hosted::RequestOutcome::Created && hosted::find_server(db, own.id)->own_machine);
	TEST_EXPECT(hosted::request_server(db, m, settings, offered, big_cap, kWall, true).outcome ==
			hosted::RequestOutcome::NoOwnMachine);
	TEST_EXPECT(request(m, settings, offered, big_cap).outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(hosted::count_live_of(db, m) == 2 && hosted::count_own_live_of(db, m) == 1);
	for (const int64_t owner : { g, m }) {
		for (const auto &s : hosted::live_servers_of(db, owner)) hosted::request_stop(db, s.id, "test");
		db.exec("UPDATE hosted_servers SET state = 'stopped' WHERE owner_user_id = $1 AND state = 'stopping';",
				{ BindValue(owner) });
	}

	// The kill switch asks every live server's stop, once.
	TEST_EXPECT(hosted::request_stop_all(db, "kill switch") == 4);
	TEST_EXPECT(hosted::request_stop_all(db, "kill switch") == 0);
	TEST_EXPECT(!hosted::request_stop(db, first.id, "again"));
	TEST_EXPECT(!hosted::request_stop(db, third.id, "ended"));
	return 0;
}

// The fleet's room at the request (the supervisor's planner): one shared machine
// of two slots at most and one own machine, so two shared servers and one own one
// fit, waiting ones counted before the request; a third shared one, or a second
// own one, is at_capacity, and a place frees once a server ends.
int test_fleet_room() {
	Rig rig("hosted_fleet_room");
	rig.config.cap = 50;
	rig.config.placement.shared_slots = 2;
	rig.config.placement.max_shared = 1;
	rig.config.placement.max_own = 1;
	rig.make_supervisor();
	Database *db = &rig.db;
	const hosted::PlacementPolicy &fleet = rig.config.placement;
	TEST_EXPECT(hosted::fleet_has_room(*db, fleet, false) && hosted::fleet_has_room(*db, fleet, true));
	// A comp grant of ten servers (the fleet, not the entitlement, is the limit
	// here) and a tier, which the own-machine add-on needs.
	const int64_t owner = make_player(*db, "room_owner", 10);
	subscribe(*db, owner, 1, "active", "sub_room");
	subscribe_machines(*db, owner, 2, "sub_room_machines");
	const auto start = [&](bool own) {
		return hosted::request_server(*db, owner, valid_settings(), rig.config.data_sets, rig.config.cap, rig.wall,
				own, &fleet);
	};
	const auto a = start(false);
	const auto b = start(false);
	TEST_EXPECT(a.outcome == hosted::RequestOutcome::Created && b.outcome == hosted::RequestOutcome::Created);
	// Both wait for the one shared machine the fleet may launch: it is full.
	TEST_EXPECT(start(false).outcome == hosted::RequestOutcome::AtCapacity);
	TEST_EXPECT(!hosted::fleet_has_room(*db, fleet, false) && hosted::fleet_has_room(*db, fleet, true));
	const auto own = start(true);
	TEST_EXPECT(own.outcome == hosted::RequestOutcome::Created);
	TEST_EXPECT(start(true).outcome == hosted::RequestOutcome::AtCapacity);
	// Placed, they hold the slots the planner counted: still full.
	TEST_EXPECT(rig.walk(a.id, State::Listing) && rig.walk(b.id, State::Listing) && rig.walk(own.id, State::Listing));
	TEST_EXPECT(rig.fake.launches == 2);
	TEST_EXPECT(rig.machine_of(a.id) == rig.machine_of(b.id) && rig.machine_of(own.id) != rig.machine_of(a.id));
	TEST_EXPECT(!hosted::fleet_has_room(*db, fleet, false) && !hosted::fleet_has_room(*db, fleet, true));
	// One ends: its slot is free again.
	TEST_EXPECT(hosted::request_stop(*db, b.id, "test"));
	rig.step();
	rig.step();
	TEST_EXPECT(rig.state(b.id) == State::Stopped);
	TEST_EXPECT(hosted::fleet_has_room(*db, fleet, false) && !hosted::fleet_has_room(*db, fleet, true));
	TEST_EXPECT(start(false).outcome == hosted::RequestOutcome::Created);
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
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.fake.launches == 0);
	// No machine yet: one is launched, and the server placed on its first slot
	// at once (it waits for the machine's boot in booting).
	rig.tick();
	TEST_EXPECT(rig.fake.launches == 1);
	auto machines = hosted::list_live_machines(rig.db);
	TEST_EXPECT(machines.size() == 1);
	const hosted::FleetMachineRow launched = machines.front();
	TEST_EXPECT(launched.kind == hosted::MachineKind::Shared && launched.slots == 4 &&
			launched.state == hosted::FleetMachineState::Booting && launched.provider == "fake" &&
			launched.cloud_id == rig.fake.last && launched.private_address == "10.0.1.1" &&
			launched.serve_image == kServeImage && launched.reserved_for == 0);
	TEST_EXPECT(rig.fake.machines.at(launched.cloud_id).token.size() == hosted::kMachineTokenChars);
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Booting && row->machine_ref == launched.id && row->slot == 0);
	TEST_EXPECT(row->game_port == 32768 && row->generation == 1 && row->serve_image == kServeImage);
	TEST_EXPECT(row->provider == "fake" && row->container == "srv-" + std::to_string(id) + "-g1");
	TEST_EXPECT(row->machine_address == "10.0.1.1");
	const auto login = hosted::admin_login_of(rig.db, id);
	TEST_EXPECT(login && login->user == "svc" && login->port == 32800 && row->admin_port == login->port);
	TEST_EXPECT(login->password.size() == hosted::kAdminPasswordChars);
	TEST_EXPECT(row->service_user_id.has_value());
	const int64_t service = *row->service_user_id;

	// Its agent's first sync: the machine is ready, and the container is wanted,
	// with its files, on slot 0.
	rig.sync_all();
	TEST_EXPECT(rig.machine(launched.id)->state == hosted::FleetMachineState::Ready);
	const FakeAgent::Container *c = rig.running_container(id);
	TEST_EXPECT(c != nullptr);
	if (!c) return 1;
	TEST_EXPECT(c->desired.slot == 0 && c->desired.uid == 10001 && c->desired.image == kServeImage);
	TEST_EXPECT(c->desired.data_set == "jop_2_consumer" && c->desired.memory_mb == 512 && c->desired.pids == 128);
	const std::vector<std::string> args = { "/HOST", "server.host", "--credentials", "credentials.txt",
		"--master-host", "203.0.113.5", "--master-gate-port", "7597", "/game", "jo", "/exp", "jox01" };
	TEST_EXPECT(c->desired.args == args);
	{
		Database *db = &rig.db;
		const auto account = nws::get_user_by_id(*db, service);
		TEST_EXPECT(account && account->kind == "service" && account->username == "srv-" + std::to_string(id));
		// The credentials the container got log the account in.
		const std::string creds = file_of(rig, id, "credentials.txt");
		TEST_EXPECT(credential(creds, "NOVAWORLD_USER") == account->username);
		const std::string password = credential(creds, "NOVAWORLD_PASS");
		TEST_EXPECT(password.size() == hosted::kServicePasswordChars);
		TEST_EXPECT(nws::authenticate_user(*db, account->username, password));
		const std::string admin = file_of(rig, id, "admin.cfg");
		TEST_EXPECT(contains(admin, login->password) && contains(admin, "ip_restrict = 10.0.0.1"));
		TEST_EXPECT(contains(file_of(rig, id, "server.host"), "GameName \"Hosted Test\""));
		const std::string cfg_text = file_of(rig, id, "game.cfg");
		const auto cfg = opennova::gamecfg::load(cfg_text.data(), cfg_text.size(), {});
		TEST_EXPECT(cfg.cfg.mp_novaworld_port_min == 32768 && cfg.cfg.remote_admin_port == 32800);
	}
	// The next sync reports it, and its files are no longer sent.
	rig.tick(); // the report it read predates the container
	TEST_EXPECT(rig.state(id) == State::Booting);
	rig.sync_all();
	FakeAgent *agent = rig.agent_of(id);
	TEST_EXPECT(agent && agent->last.desired.size() == 1 && !agent->last.desired[0].files);
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Listing);
	rig.step(); // no listing yet
	TEST_EXPECT(rig.state(id) == State::Listing);
	TEST_EXPECT(list_as(rig.db, service, 5001));
	rig.step();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Running && !row->running_at.empty());
	TEST_EXPECT(contains(last_detail(rig.db, id), "RID 5001"));
	rig.advance(rig.config.health_interval_ms);
	rig.step(); // a health check that finds the listing
	TEST_EXPECT(rig.state(id) == State::Running);

	// The owner's stop: the push to the listing, then the end once it has left.
	TEST_EXPECT(hosted::request_stop(rig.db, id, "stopped by its owner"));
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Stopping);
	TEST_EXPECT(rig.pushes.size() == 1 && rig.pushes[0].first == 5001 && rig.pushes[0].second == service);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Stopping && rig.pushes.size() == 1);
	TEST_EXPECT(rig.running_container(id) != nullptr); // still wanted while it deregisters
	hostdb::remove_host_by_rid(rig.db, 5001);
	rig.step();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Stopped && !row->ended_at.empty());
	TEST_EXPECT(row->container.empty() && !row->service_user_id.has_value());
	// The row keeps where it last ran; its container is no longer wanted.
	TEST_EXPECT(row->machine_ref == launched.id && row->slot == 0);
	TEST_EXPECT(agent->containers.empty() && agent->last.desired.empty());
	TEST_EXPECT(!nws::get_user_by_id(rig.db, service));
	TEST_EXPECT(transitions(rig.db, id) ==
			(std::vector<std::string>{ ">requested", "requested>provisioning", "provisioning>booting",
					"booting>listing", "listing>running", "running>stopping", "stopping>stopped" }));
	rig.step(); // nothing live: nothing happens, and the machine stays (idle) for now
	TEST_EXPECT(rig.fake.launches == 1 && !rig.fake.was_terminated(launched.cloud_id));
	return 0;
}

// Two owners' servers share one shared machine: one launch, slots 0 and 1, each
// on its own game and admin ports (a stock client lists and joins each at the
// machine's address and its slot's port), each its own container and user.
int test_two_servers_one_machine() {
	Rig rig("hosted_two_on_one");
	const int64_t a = rig.request("pair_a");
	const int64_t b = rig.request("pair_b");
	TEST_EXPECT(a > 0 && b > 0);
	TEST_EXPECT(!rig.to_running(a, 5101).empty());
	TEST_EXPECT(!rig.to_running(b, 5102).empty());
	TEST_EXPECT(rig.fake.launches == 1);
	const auto ra = rig.server(a);
	const auto rb = rig.server(b);
	TEST_EXPECT(ra->machine_ref == rb->machine_ref && ra->slot == 0 && rb->slot == 1);
	TEST_EXPECT(ra->game_port == 32768 && rb->game_port == 32769);
	TEST_EXPECT(ra->machine_address == rb->machine_address);
	TEST_EXPECT(hosted::admin_login_of(rig.db, a)->port == 32800 &&
			hosted::admin_login_of(rig.db, b)->port == 32801);
	FakeAgent *agent = rig.agent_of(a);
	TEST_EXPECT(agent && agent->containers.size() == 2);
	const auto *cb = rig.running_container(b);
	TEST_EXPECT(cb && cb->desired.uid == 10002 && cb->desired.slot == 1);
	const auto holders = hosted::slot_holders(rig.db, *ra->machine_ref);
	TEST_EXPECT(holders.size() == 2 && holders[0].server_id == a && holders[1].server_id == b);
	return 0;
}

// The own-machine add-on: the server gets a machine of its own (one slot,
// reserved for it, its own kind's template), never a shared one's slot, while a
// shared server of the same owner goes to a shared machine; the own machine
// ends when its server stops.
int test_own_machine() {
	Rig rig("hosted_own");
	Database *db = &rig.db;
	const int64_t owner = make_player(*db, "own_owner", 0);
	subscribe(*db, owner, 2, "active", "sub_own");
	subscribe_machines(*db, owner, 1, "sub_own_machine");
	const int64_t own = rig.request_as(owner, valid_settings(), true);
	const int64_t shared = rig.request_as(owner);
	TEST_EXPECT(own > 0 && shared > 0);
	TEST_EXPECT(!rig.to_running(own, 5201).empty() && !rig.to_running(shared, 5202).empty());
	TEST_EXPECT(rig.fake.launches == 2);
	const auto own_machine = rig.machine(rig.machine_of(own));
	const auto shared_machine = rig.machine(rig.machine_of(shared));
	TEST_EXPECT(own_machine && own_machine->kind == hosted::MachineKind::Own && own_machine->slots == 1 &&
			own_machine->reserved_for == own);
	TEST_EXPECT(shared_machine && shared_machine->kind == hosted::MachineKind::Shared);
	TEST_EXPECT(rig.fake.machines.at(own_machine->cloud_id).kind == hosted::MachineKind::Own);
	TEST_EXPECT(rig.server(own)->slot == 0 && rig.server(own)->game_port == 32768);
	// Its stop ends the machine with it (the shared one stays).
	TEST_EXPECT(hosted::request_stop(*db, own, "stopped by its owner"));
	rig.step(); // the push
	hostdb::remove_host_by_rid(*db, 5201);
	rig.step(); // stopped
	TEST_EXPECT(rig.state(own) == State::Stopped);
	rig.step(); // its machine ends
	TEST_EXPECT(rig.fake.was_terminated(own_machine->cloud_id) && !rig.fake.was_terminated(shared_machine->cloud_id));
	TEST_EXPECT(rig.machine(own_machine->id)->state == hosted::FleetMachineState::Ended);
	TEST_EXPECT(rig.machine(own_machine->id)->end_reason == "its server ended");
	TEST_EXPECT(rig.state(shared) == State::Running);
	return 0;
}

int test_stop_paths() {
	Rig rig("hosted_stops");
	// A stop before any slot: stopped with no launch.
	const int64_t early = rig.request("stop_early");
	TEST_EXPECT(hosted::request_stop(rig.db, early, "stopped by its owner"));
	rig.tick();
	TEST_EXPECT(rig.state(early) == State::Stopped && rig.fake.launches == 0);
	TEST_EXPECT(transitions(rig.db, early) ==
			(std::vector<std::string>{ ">requested", "requested>stopping", "stopping>stopped" }));

	// Still listed 60 s after the push: its container is let go anyway.
	const int64_t stuck = rig.request("stop_stuck");
	const std::string container = rig.to_running(stuck, 6001);
	TEST_EXPECT(!container.empty());
	TEST_EXPECT(hosted::request_stop(rig.db, stuck, "stopped by an admin"));
	rig.step();
	TEST_EXPECT(rig.pushes.size() == 1);
	rig.advance(rig.config.stop_timeout_ms - 1);
	rig.step();
	TEST_EXPECT(rig.state(stuck) == State::Stopping);
	rig.advance(1);
	rig.step();
	TEST_EXPECT(rig.state(stuck) == State::Stopped);
	TEST_EXPECT(contains(last_detail(rig.db, stuck), "after the stop"));
	TEST_EXPECT(rig.agents.begin()->second.find(container) == nullptr);
	hostdb::remove_host_by_rid(rig.db, 6001);

	// The process exits on its own during the stop, with the code a
	// ServerStopHosting push ends opennova-serve with (3): the stop's end, never a
	// restart.
	const int64_t quick = rig.request("stop_quick");
	const std::string c2 = rig.to_running(quick, 6002);
	TEST_EXPECT(hosted::request_stop(rig.db, quick, "stopped by its owner"));
	rig.step();
	const int launches_before = rig.fake.launches;
	rig.agent_of(quick)->exit_with(c2, 3);
	rig.sync_all();
	rig.tick();
	TEST_EXPECT(rig.state(quick) == State::Stopped);
	TEST_EXPECT(rig.server(quick)->restarts == 0);
	TEST_EXPECT(contains(last_detail(rig.db, quick), "code 3"));
	rig.advance(rig.config.restart_backoff_ms);
	rig.step();
	TEST_EXPECT(rig.state(quick) == State::Stopped && rig.fake.launches == launches_before);
	hostdb::remove_host_by_rid(rig.db, 6002);

	// A stop that lands while a pass places the server: it lets the slot go, its
	// container never starts, and the stop ends with nothing to stop.
	const int64_t racing = rig.request("stop_racing");
	rig.tick(); // -> provisioning
	rig.fake.on_slot_plan = [&rig, racing] {
		hosted::request_stop(rig.db, racing, "stopped mid-placement");
	};
	rig.tick(); // the placement loses to the stop
	rig.fake.on_slot_plan = nullptr;
	auto row = rig.server(racing);
	TEST_EXPECT(row->state == State::Stopping && !row->machine_ref && row->container.empty());
	rig.sync_all();
	for (const auto &entry : rig.agents) TEST_EXPECT(entry.second.last.desired.empty());
	rig.tick();
	TEST_EXPECT(rig.state(racing) == State::Stopped);
	TEST_EXPECT(contains(last_detail(rig.db, racing), "stopped before it ran"));
	return 0;
}

int test_exits_and_crash_restarts() {
	Rig rig("hosted_restarts");
	// opennova-serve exiting 0 while it should run (its rotation's end, the
	// admin's GOTO MENUSTATE) is no stop: only the supervisor stops a server, so
	// it is a crash, restarted and counted.
	const int64_t zero = rig.request("exit_zero");
	const std::string c0 = rig.to_running(zero, 7001);
	TEST_EXPECT(!c0.empty());
	rig.agent_of(zero)->exit_with(c0, 0);
	rig.sync_all();
	rig.tick();
	auto zero_row = rig.server(zero);
	TEST_EXPECT(zero_row->state == State::Provisioning && zero_row->restarts == 1 && !zero_row->machine_ref);
	TEST_EXPECT(contains(last_detail(rig.db, zero), "exited with code 0; restart 1 of 2"));
	hostdb::remove_host_by_rid(rig.db, 7001);
	TEST_EXPECT(hosted::request_stop(rig.db, zero, "stopped by its owner"));
	rig.tick(); // no container to stop
	TEST_EXPECT(rig.state(zero) == State::Stopped);

	// A crash (here the ServerStopHosting exit, 3, while the server should run)
	// restarts, a new generation with a new password, until the limit (2) is used up.
	const int64_t id = rig.request("exit_crash");
	const std::string c1 = rig.to_running(id, 7002);
	TEST_EXPECT(!c1.empty());
	const int64_t service = *rig.server(id)->service_user_id;
	const std::string first_password = credential(file_of(rig, id, "credentials.txt"), "NOVAWORLD_PASS");
	const std::string first_admin = hosted::admin_login_of(rig.db, id)->password;
	const int64_t machine = rig.machine_of(id);
	rig.agent_of(id)->exit_with(c1, 3);
	rig.sync_all();
	rig.tick();
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 1 && row->container.empty() &&
			!row->machine_ref);
	TEST_EXPECT(contains(last_detail(rig.db, id), "exited with code 3; restart 1 of 2"));
	hostdb::remove_host_by_rid(rig.db, 7002); // its NWU session timed out
	rig.step(); // the backoff holds the placement; the exited container is let go
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.agents.at(machine).containers.empty());
	rig.advance(rig.config.restart_backoff_ms);
	rig.step();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Booting && row->machine_ref == machine && row->generation == 2);
	const std::string c2 = row->container;
	TEST_EXPECT(c2 == "srv-" + std::to_string(id) + "-g2" && row->service_user_id == service);
	TEST_EXPECT(rig.fake.launches == 1); // the same machine, no new one
	const std::string second_password = credential(file_of(rig, id, "credentials.txt"), "NOVAWORLD_PASS");
	TEST_EXPECT(!second_password.empty() && second_password != first_password);
	TEST_EXPECT(hosted::admin_login_of(rig.db, id)->password != first_admin);
	{
		Database *db = &rig.db;
		const std::string name = "srv-" + std::to_string(id);
		TEST_EXPECT(!nws::authenticate_user(*db, name, first_password));
		TEST_EXPECT(nws::authenticate_user(*db, name, second_password));
	}
	// The kernel ends it out of memory: the second restart.
	TEST_EXPECT(rig.walk(id, State::Listing));
	rig.agent_of(id)->exit_with(c2, 137, /*oom=*/true);
	rig.sync_all();
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 2);
	TEST_EXPECT(contains(last_detail(rig.db, id), "ran out of memory; restart 2 of 2"));
	// A start that fails (docker's 125): the limit is used up, it fails.
	rig.advance(rig.config.restart_backoff_ms);
	rig.step(); // placed, booting, its container started
	const std::string c3 = rig.container(id);
	TEST_EXPECT(rig.state(id) == State::Booting && rig.running_container(id) != nullptr);
	rig.agent_of(id)->fail_start(c3);
	rig.sync_all();
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed && row->restarts == 2);
	TEST_EXPECT(contains(row->failure_reason, "its container did not start") &&
			contains(row->failure_reason, "no restart left"));
	TEST_EXPECT(!row->service_user_id && !nws::get_user_by_id(rig.db, service));
	rig.sync_all();
	TEST_EXPECT(rig.agents.at(machine).containers.empty());
	return 0;
}

// A machine launch the cloud refuses is the machine's cause, never the server's:
// no restart counted, however many fail. The driver's own text goes to the log,
// not to the event the owner reads (written once per wait); the machine's row
// fails, the heartbeat counts it, and no launch of that kind follows within the
// launch backoff, after which the server is placed on the next one.
int test_launch_failure() {
	Rig rig("hosted_launch_failure");
	const int64_t id = rig.request("launch_refused");
	TEST_EXPECT(id > 0);
	rig.tick(); // -> provisioning
	rig.fake.fail_next_launch = true;
	rig.tick();
	auto row = rig.server(id);
	TEST_EXPECT(rig.fake.launches == 1);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && !row->machine_ref);
	const std::string detail = last_detail(rig.db, id);
	TEST_EXPECT(contains(detail, "the launch of its machine failed; it waits for the next (no restart counted)") &&
			!contains(detail, "fake"));
	TEST_EXPECT(rig.sup->launch_failures() == 1);
	const auto machines = hosted::list_machines(rig.db, 10);
	TEST_EXPECT(machines.size() == 1 && machines[0].state == hosted::FleetMachineState::Failed &&
			machines[0].end_reason == "the launch failed");
	// Within the launch backoff (5 s) nothing launches.
	rig.advance(rig.config.launch_backoff_ms - 1);
	rig.tick();
	TEST_EXPECT(rig.fake.launches == 1 && rig.state(id) == State::Provisioning);
	// Refused again, and again: still no restart, the event once.
	for (int i = 0; i < 3; ++i) {
		rig.advance(rig.config.launch_backoff_ms);
		rig.fake.fail_next_launch = true;
		rig.tick();
	}
	row = rig.server(id);
	TEST_EXPECT(rig.fake.launches == 4 && rig.sup->launch_failures() == 4);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0);
	TEST_EXPECT(transitions(rig.db, id) ==
			(std::vector<std::string>{ ">requested", "requested>provisioning", "provisioning>provisioning" }));
	rig.advance(rig.config.launch_backoff_ms);
	rig.tick();
	TEST_EXPECT(rig.fake.launches == 5 && rig.state(id) == State::Booting);
	TEST_EXPECT(rig.walk(id, State::Listing) && rig.server(id)->restarts == 0);
	return 0;
}

// The restart window: restart_limit crash restarts at most, but a run of
// restart_reset_ms in running forgives the ones before it, so an always-on
// server that crashes now and then is restarted every time; crashes in quick
// succession still use the limit up.
int test_restart_window() {
	Rig rig("hosted_restart_window");
	const int64_t id = rig.request("window_owner");
	std::string container = rig.to_running(id, 7101);
	TEST_EXPECT(!container.empty());
	const auto crash = [&rig, id](const std::string &name) {
		rig.agent_of(id)->exit_with(name, 3);
		rig.sync_all();
		rig.tick();
	};
	for (int n = 1; n <= 2; ++n) {
		crash(container);
		TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == n);
		container = rig.back_to_running(id, 7101 + static_cast<uint32_t>(n));
		TEST_EXPECT(!container.empty());
	}
	rig.advance(rig.config.restart_reset_ms - 1);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Running && rig.server(id)->restarts == 2);
	rig.advance(1);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Running && rig.server(id)->restarts == 0);
	// So the next crash is restarted, not the end.
	crash(container);
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 1);
	TEST_EXPECT(contains(last_detail(rig.db, id), "restart 1 of 2"));
	// Within the window the limit holds.
	container = rig.back_to_running(id, 7110);
	TEST_EXPECT(!container.empty());
	crash(container);
	TEST_EXPECT(rig.server(id)->restarts == 2);
	container = rig.back_to_running(id, 7111);
	TEST_EXPECT(!container.empty());
	crash(container);
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed && contains(row->failure_reason, "no restart left"));
	return 0;
}

int test_timeouts_and_health() {
	Rig rig("hosted_health");
	// The container never starts (it stays starting): the clock counts from the
	// machine's ready.
	rig.start_running = false;
	const int64_t slow = rig.request("health_slow");
	TEST_EXPECT(rig.walk(slow, State::Booting));
	rig.sync_all();
	const FakeAgent::Container *starting = rig.running_container(slow);
	TEST_EXPECT(starting && starting->report.state == hosted::ContainerState::Starting);
	rig.advance(rig.config.boot_timeout_ms - 1);
	rig.step();
	TEST_EXPECT(rig.state(slow) == State::Booting);
	rig.advance(1);
	rig.step();
	TEST_EXPECT(rig.state(slow) == State::Provisioning);
	TEST_EXPECT(contains(last_detail(rig.db, slow), "its container did not start in 10 s"));
	TEST_EXPECT(hosted::request_stop(rig.db, slow, "done"));
	rig.step();
	rig.start_running = true;
	for (auto &entry : rig.agents) entry.second.start_running = true;

	// It runs, but never lists.
	const int64_t quiet = rig.request("health_quiet");
	TEST_EXPECT(rig.walk(quiet, State::Listing));
	rig.advance(rig.config.listing_timeout_ms);
	rig.step();
	TEST_EXPECT(rig.state(quiet) == State::Provisioning);
	TEST_EXPECT(contains(last_detail(rig.db, quiet), "no listing in 20 s"));
	TEST_EXPECT(hosted::request_stop(rig.db, quiet, "done"));
	rig.step();

	// Listed, then gone from the browser: three missed checks in a row fail it,
	// and a check that finds the listing again starts the count over.
	const int64_t id = rig.request("health_lost");
	const std::string container = rig.to_running(id, 8001);
	TEST_EXPECT(!container.empty());
	hostdb::remove_host_by_rid(rig.db, 8001);
	rig.step(); // before the interval: no check
	for (int miss = 1; miss <= 2; ++miss) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
		TEST_EXPECT(rig.state(id) == State::Running);
	}
	// Back in the browser (its key still claimed for the RID): the count starts over.
	{
		Database *db = &rig.db;
		hostdb::HostRow row;
		row.rid = 8001;
		row.gsid = "GSID-8001";
		row.game = "jop_2_consumer";
		row.server_name = "Back";
		row.host_ip = row.peer_ip = "198.51.100.1";
		row.host_port = row.peer_port = 32768;
		row.host_user_id = rig.server(id)->service_user_id;
		hostdb::upsert_host(*db, row);
	}
	rig.advance(rig.config.health_interval_ms);
	rig.step();
	hostdb::remove_host_by_rid(rig.db, 8001);
	for (int miss = 1; miss <= 2; ++miss) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
		TEST_EXPECT(rig.state(id) == State::Running);
	}
	rig.advance(rig.config.health_interval_ms);
	rig.step();
	const auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed);
	TEST_EXPECT(contains(row->failure_reason, "no listing at 3 health checks"));
	TEST_EXPECT(row->restarts == 0);
	TEST_EXPECT(rig.agents.begin()->second.find(container) == nullptr);
	return 0;
}

// A machine lost under a running server (its agent silent, then the cloud says
// it is gone): the fleet's cause, so the server is placed again with no restart
// counted, on a new machine, and runs again.
int test_machine_lost_replaces() {
	Rig rig("hosted_machine_lost");
	const int64_t id = rig.request("lost_owner");
	TEST_EXPECT(!rig.to_running(id, 8101).empty());
	const int64_t machine = rig.machine_of(id);
	const std::string cloud = rig.machine(machine)->cloud_id;
	rig.silence(machine);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Lost);
	// While lost, the health checks wait (its listing gone too).
	hostdb::remove_host_by_rid(rig.db, 8101);
	for (int i = 0; i < 4; ++i) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
	}
	TEST_EXPECT(rig.state(id) == State::Running);
	// The cloud says it is gone: it ends, and the server is placed again.
	rig.fake.vanish(cloud);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Ended);
	auto row = rig.server(id);
	TEST_EXPECT(row->restarts == 0 && row->machine_ref != machine);
	TEST_EXPECT(has_event(rig.db, id, "its machine is gone; placed again (no restart counted)"));
	TEST_EXPECT(!rig.to_running(id, 8102).empty());
	row = rig.server(id);
	TEST_EXPECT(row->restarts == 0 && row->machine_ref != machine && rig.fake.launches == 2);
	return 0;
}

// A server waiting for its listing whose machine goes lost: the listing's clock
// waits while the machine is lost (its end, not a missed listing, decides), and
// counts again from the machine's return.
int test_listing_waits_while_lost() {
	Rig rig("hosted_listing_lost");
	const int64_t id = rig.request("listing_lost_owner");
	TEST_EXPECT(rig.walk(id, State::Listing));
	const int64_t machine = rig.machine_of(id);
	rig.silence(machine);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Lost);
	rig.advance(rig.config.listing_timeout_ms * 2);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Listing && rig.server(id)->restarts == 0);
	// Its agent reports again: the clock starts over.
	rig.muted.erase(machine);
	rig.step();
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Ready);
	rig.advance(rig.config.listing_timeout_ms - 1000);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Listing);
	rig.advance(1000);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == 1);
	TEST_EXPECT(contains(last_detail(rig.db, id), "no listing in 20 s"));
	return 0;
}

int test_owner_changes() {
	Rig rig("hosted_owners");
	// A banned owner's running server is stopped.
	const int64_t banned = rig.request("owner_banned");
	const std::string c1 = rig.to_running(banned, 9001);
	TEST_EXPECT(!c1.empty());
	{
		Database *db = &rig.db;
		nws::UpdateUserParams ban;
		ban.account_status = "banned";
		TEST_EXPECT(nws::update_user(*db, *rig.server(banned)->owner_user_id, ban).ok);
	}
	rig.step();
	TEST_EXPECT(rig.state(banned) == State::Stopping);
	TEST_EXPECT(contains(last_detail(rig.db, banned), "owner's account is banned"));

	// A deleted owner's server is stopped (the row keeps no owner).
	const int64_t deleted = rig.request("owner_deleted");
	const std::string c2 = rig.to_running(deleted, 9002);
	TEST_EXPECT(!c2.empty());
	const int64_t owner = *rig.server(deleted)->owner_user_id;
	TEST_EXPECT(nws::delete_user(rig.db, owner).ok);
	TEST_EXPECT(!rig.server(deleted)->owner_user_id);
	rig.step();
	TEST_EXPECT(rig.state(deleted) == State::Stopping);
	TEST_EXPECT(contains(last_detail(rig.db, deleted), "owner's account was deleted"));

	// The hosting entitlement is the brake: the comp grant withdrawn (0), the
	// server stops, as for a ban. Before the pickup nothing is placed.
	const int64_t withdrawn = rig.request("owner_withdrawn");
	TEST_EXPECT(nws::set_comp_servers(rig.db, *rig.server(withdrawn)->owner_user_id, 0).ok);
	rig.step();
	TEST_EXPECT(stopped_with(rig.db, withdrawn, "hosting entitlement allows 0 hosted servers"));
	rig.step();
	TEST_EXPECT(rig.state(withdrawn) == State::Stopped && !rig.server(withdrawn)->machine_ref);

	// While it runs, on a support tier Stripe cancels; past due, before that, it
	// keeps running.
	const int64_t subscriber = make_player(rig.db, "owner_subscriber", 0);
	subscribe(rig.db, subscriber, 1, "active", "sub_owner");
	for (const uint32_t rid : { 9001u, 9002u }) hostdb::remove_host_by_rid(rig.db, rid);
	rig.step(); // the banned and deleted owners' stops end (their listings gone)
	TEST_EXPECT(rig.state(banned) == State::Stopped && rig.state(deleted) == State::Stopped);
	const int64_t revoked = rig.request_as(subscriber);
	const std::string c3 = rig.to_running(revoked, 9003);
	TEST_EXPECT(!c3.empty());
	subscribe(rig.db, subscriber, 1, "past_due", "sub_owner");
	rig.step();
	TEST_EXPECT(rig.state(revoked) == State::Running);
	subscribe(rig.db, subscriber, 1, "canceled", "sub_owner");
	rig.step();
	TEST_EXPECT(rig.state(revoked) == State::Stopping);
	TEST_EXPECT(contains(last_detail(rig.db, revoked), "hosting entitlement allows 0 hosted servers"));
	hostdb::remove_host_by_rid(rig.db, 9003);
	rig.step(); // the push (nothing is listed)
	rig.step(); // no listing left: the stop finishes
	TEST_EXPECT(rig.state(revoked) == State::Stopped);

	// During a crash restart's backoff: no new credentials, no placement.
	const int64_t crashed = rig.request("owner_crashed");
	const std::string c4 = rig.to_running(crashed, 9004);
	TEST_EXPECT(!c4.empty());
	rig.agent_of(crashed)->exit_with(c4, 3);
	rig.sync_all();
	rig.tick();
	TEST_EXPECT(rig.state(crashed) == State::Provisioning);
	const int64_t service = *rig.server(crashed)->service_user_id;
	const int64_t generation = nws::get_user_by_id(rig.db, service)->credentials_gen;
	TEST_EXPECT(nws::set_comp_servers(rig.db, *rig.server(crashed)->owner_user_id, 0).ok);
	rig.advance(rig.config.restart_backoff_ms);
	rig.step();
	TEST_EXPECT(stopped_with(rig.db, crashed, "hosting entitlement allows 0 hosted servers") &&
			!rig.server(crashed)->machine_ref);
	// No new credentials: the account is untouched, or gone with the stop's end.
	const auto account = nws::get_user_by_id(rig.db, service);
	TEST_EXPECT(!account || account->credentials_gen == generation);
	rig.step();
	TEST_EXPECT(rig.state(crashed) == State::Stopped);
	return 0;
}

// A server past its owner's entitlement when it is about to be placed is
// stopped rather than started: a grant of two, two requests picked up, the grant
// cut to one before the placements; the older goes on, the newer (past the
// grant in id order) stops before any slot.
int test_limit_at_provisioning() {
	Rig rig("hosted_limit_provisioning");
	const int64_t owner = make_player(rig.db, "limit_owner", 2);
	TEST_EXPECT(owner > 0);
	const int64_t older = rig.request_as(owner);
	const int64_t newer = rig.request_as(owner);
	TEST_EXPECT(older > 0 && newer > older);
	rig.tick(); // both -> provisioning
	TEST_EXPECT(rig.state(older) == State::Provisioning && rig.state(newer) == State::Provisioning);
	TEST_EXPECT(nws::set_comp_servers(rig.db, owner, 1).ok);
	rig.tick();
	TEST_EXPECT(stopped_with(rig.db, newer, "hosting entitlement allows 1 hosted server"));
	TEST_EXPECT(rig.fake.launches == 1); // the older one's machine
	TEST_EXPECT(rig.walk(older, State::Listing));
	TEST_EXPECT(rig.state(newer) == State::Stopped && !rig.server(newer)->machine_ref);
	return 0;
}

// The owners' limits (the supervisor's enforce_limits, each pass's first act):
// a tier lowered stops the newest servers past it, the oldest kept, the event
// naming the tier; a past-due tier keeps every server while a start is refused
// (PastDue); the add-on lowered stops the newest on own machines; a canceled
// tier stops them all; a stopping server counts toward neither.
int test_owner_limits() {
	Rig rig("hosted_limits");
	rig.config.cap = 20;
	rig.make_supervisor();
	Database *db = &rig.db;
	// Tier 3: three servers.
	const int64_t ana = make_player(*db, "lim_ana", 0);
	subscribe(*db, ana, 3, "active", "sub_lim_ana");
	const int64_t a1 = rig.request_as(ana);
	const int64_t a2 = rig.request_as(ana);
	const int64_t a3 = rig.request_as(ana);
	TEST_EXPECT(a1 > 0 && a2 > a1 && a3 > a2);
	rig.tick(); // all three picked up
	TEST_EXPECT(rig.state(a1) == State::Provisioning && rig.state(a3) == State::Provisioning);
	// Tier 1 now: the two newest stop, the oldest goes on.
	subscribe(*db, ana, 1, "active", "sub_lim_ana");
	rig.tick();
	TEST_EXPECT(stopped_with(*db, a3, "its owner's support tier allows 1 hosted server") &&
			stopped_with(*db, a2, "its owner's support tier allows 1 hosted server") &&
			!stopped_with(*db, a3, "hosted servers"));
	TEST_EXPECT(hosted::is_live(rig.state(a1)) && rig.state(a1) != State::Stopping);
	// The stopping ones count toward nothing: a pass stops no more.
	rig.step();
	TEST_EXPECT(hosted::is_live(rig.state(a1)) && rig.state(a1) != State::Stopping);
	TEST_EXPECT(!rig.sup->limits_held());

	// Past due: the servers stay, a start is refused.
	const int64_t ben = make_player(*db, "lim_ben", 0);
	subscribe(*db, ben, 2, "active", "sub_lim_ben");
	const int64_t b1 = rig.request_as(ben);
	const int64_t b2 = rig.request_as(ben);
	TEST_EXPECT(b1 > 0 && b2 > b1);
	rig.step();
	subscribe(*db, ben, 2, "past_due", "sub_lim_ben");
	rig.step();
	TEST_EXPECT(hosted::is_live(rig.state(b1)) && rig.state(b1) != State::Stopping);
	TEST_EXPECT(hosted::is_live(rig.state(b2)) && rig.state(b2) != State::Stopping);
	TEST_EXPECT(hosted::request_server(*db, ben, valid_settings(), rig.config.data_sets, rig.config.cap, rig.wall)
						.outcome == hosted::RequestOutcome::PastDue);
	// Canceled: they stop.
	subscribe(*db, ben, 2, "canceled", "sub_lim_ben");
	rig.step();
	TEST_EXPECT(stopped_with(*db, b1, "hosting entitlement allows 0 hosted servers"));
	TEST_EXPECT(stopped_with(*db, b2, "hosting entitlement allows 0 hosted servers"));

	// The add-on: two machines, then one; the newest on an own machine stops,
	// the shared one stays.
	const int64_t cy = make_player(*db, "lim_cy", 0);
	subscribe(*db, cy, 3, "active", "sub_lim_cy");
	subscribe_machines(*db, cy, 2, "sub_lim_cy_machines");
	const auto own = [&](bool on_own) { return rig.request_as(cy, valid_settings(), on_own); };
	const int64_t c1 = own(true);
	const int64_t c2 = own(false);
	const int64_t c3 = own(true);
	TEST_EXPECT(c1 > 0 && c2 > c1 && c3 > c2);
	TEST_EXPECT(own(true) == 0); // a fourth: every server the tier allows is live
	rig.step();
	subscribe_machines(*db, cy, 1, "sub_lim_cy_machines");
	rig.step();
	TEST_EXPECT(stopped_with(*db, c3, "own-machine add-on covers 1 machine"));
	TEST_EXPECT(hosted::is_live(rig.state(c1)) && rig.state(c1) != State::Stopping);
	TEST_EXPECT(hosted::is_live(rig.state(c2)) && rig.state(c2) != State::Stopping);
	return 0;
}

// The brake on the brake: a pass that would stop the servers of more owners
// than max(2, half the owners) stops none, logs, and holds (no limit stops,
// whatever their count, until it releases); cleared while the cause stands, it
// holds again. A clean reconcile with Stripe (LimitsHold::reconciled) confirms
// the cache: the next pass finds some of the same owners still past their
// entitlements, releases, and stops theirs alone.
int test_mass_stop_hold() {
	Rig rig("hosted_hold");
	hosted::LimitsHold hold;
	rig.sup->set_limits_hold(&hold);
	Database *db = &rig.db;
	const int64_t a = rig.request("hold_a");
	const int64_t b = rig.request("hold_b");
	const int64_t c = rig.request("hold_c");
	TEST_EXPECT(a > 0 && b > 0 && c > 0);
	rig.tick();
	for (const int64_t id : { a, b, c })
		TEST_EXPECT(nws::set_comp_servers(*db, *rig.server(id)->owner_user_id, 0).ok);
	rig.tick();
	TEST_EXPECT(rig.sup->limits_held() && !rig.sup->limits_hold_reason().empty());
	TEST_EXPECT(hold.view().owners == 3 && hold.view().servers == 3);
	for (const int64_t id : { a, b, c }) TEST_EXPECT(rig.state(id) != State::Stopping && hosted::is_live(rig.state(id)));
	// Cleared while all three still lapse: held again.
	rig.sup->clear_limits_hold();
	rig.tick();
	TEST_EXPECT(rig.sup->limits_held());
	for (const int64_t id : { a, b, c }) TEST_EXPECT(rig.state(id) != State::Stopping);
	// Two restored: one owner past, below the brake, and still held (no
	// reconcile has confirmed the cache since).
	for (const int64_t id : { a, b })
		TEST_EXPECT(nws::set_comp_servers(*db, *rig.server(id)->owner_user_id, 1).ok);
	rig.tick();
	TEST_EXPECT(rig.sup->limits_held() && rig.state(c) != State::Stopping);
	TEST_EXPECT(hold.view().owners == 1 && hold.view().servers == 1);
	// A clean reconcile: the next pass releases, and the one owner still past
	// (one of those held) is stopped.
	hold.reconciled();
	rig.tick();
	TEST_EXPECT(!hold.held());
	TEST_EXPECT(stopped_with(*db, c, "hosting entitlement allows 0 hosted servers"));
	TEST_EXPECT(rig.state(a) != State::Stopping && rig.state(b) != State::Stopping);
	TEST_EXPECT(hold.view().owners == 0 && hold.view().reason.empty());
	return 0;
}

// The hold's releases: after a clean reconcile the same owners still past
// their entitlements are stopped, every one, past the brake; owners past
// theirs that the hold did not count (after or before a reconcile) widen it,
// and it waits for the next reconcile; a reconcile that finds no owner past
// releases it with nothing stopped; an admin's override applies the stops at
// the next pass, once.
int test_hold_releases() {
	Rig rig("hosted_hold_releases");
	rig.config.cap = 20;
	rig.config.placement.max_shared = 3; // room for the nine
	rig.make_supervisor();
	hosted::LimitsHold hold;
	rig.sup->set_limits_hold(&hold);
	auto db = rig.pool.acquire();
	const auto lapse = [&](const std::vector<int64_t> &ids, int comp) {
		bool ok = true;
		for (const int64_t id : ids) ok = nws::set_comp_servers(*db, *rig.server(id)->owner_user_id, comp).ok && ok;
		return ok;
	};
	const auto none_stopping = [&](const std::vector<int64_t> &ids) {
		for (const int64_t id : ids) {
			if (rig.state(id) == State::Stopping || !hosted::is_live(rig.state(id))) return false;
		}
		return true;
	};
	std::vector<int64_t> s;
	for (const char *name : { "rel_a", "rel_b", "rel_c", "rel_d", "rel_e", "rel_f", "rel_g", "rel_h", "rel_i" })
		s.push_back(rig.request(name));
	for (const int64_t id : s) TEST_EXPECT(id > 0);
	rig.tick();

	// Five of nine owners lapse (the brake: max(2, 4) = 4): held, pass after
	// pass; a clean reconcile, and the next pass stops all five.
	TEST_EXPECT(lapse({ s[0], s[1], s[2], s[3], s[4] }, 0));
	rig.tick();
	TEST_EXPECT(hold.held() && hold.view().owners == 5 && none_stopping(s));
	rig.tick();
	TEST_EXPECT(hold.held() && none_stopping(s));
	hold.reconciled();
	rig.tick();
	TEST_EXPECT(!hold.held());
	for (int i = 0; i < 5; ++i) TEST_EXPECT(stopped_with(*db, s[i], "hosting entitlement allows 0 hosted servers"));
	TEST_EXPECT(none_stopping({ s[5], s[6], s[7], s[8] }));

	// Four owners live now (the brake: max(2, 2) = 2). Three lapse: held. A
	// reconcile lands, then a fourth lapses before the next pass: the hold
	// widens and waits for the next reconcile.
	const std::vector<int64_t> rest = { s[5], s[6], s[7], s[8] };
	TEST_EXPECT(lapse({ s[5], s[6], s[7] }, 0));
	rig.tick();
	TEST_EXPECT(hold.held() && hold.view().owners == 3);
	hold.reconciled();
	TEST_EXPECT(lapse({ s[8] }, 0));
	rig.tick();
	TEST_EXPECT(hold.held() && hold.view().owners == 4 && none_stopping(rest));
	// Restored, all four, while held: still held (unconfirmed); the next
	// reconcile finds none past: released, nothing stopped.
	TEST_EXPECT(lapse(rest, 1));
	rig.tick();
	TEST_EXPECT(hold.held() && none_stopping(rest));
	hold.reconciled();
	rig.tick();
	TEST_EXPECT(!hold.held() && none_stopping(rest));

	// The admin's override: held again, then the next pass stops all four past
	// the brake, and the override is spent.
	TEST_EXPECT(lapse(rest, 0));
	rig.tick();
	TEST_EXPECT(hold.held() && none_stopping(rest));
	hold.force_next_pass();
	rig.tick();
	TEST_EXPECT(!hold.held() && !hold.take_force());
	for (const int64_t id : rest) TEST_EXPECT(stopped_with(*db, id, "hosting entitlement allows 0 hosted servers"));
	return 0;
}

// The service starts again over the same rows and the same fleet: a running
// server resumes on its machine (its health checks wait out its grace, its
// container answered for without files), a stopping one whose machine is gone
// ends, a running one whose machine is gone is placed again with no restart
// counted, and one placed but never rendered is placed again.
int test_reconciliation() {
	Rig rig("hosted_reconcile");
	rig.config.placement.shared_slots = 1; // a machine per server
	rig.config.placement.max_shared = 4;
	rig.config.cap = 10;
	rig.make_supervisor();
	const int64_t kept = rig.request("rec_kept");
	TEST_EXPECT(!rig.to_running(kept, 9101).empty());
	const int64_t lost = rig.request("rec_lost");
	TEST_EXPECT(!rig.to_running(lost, 9102).empty());
	const int64_t stopping = rig.request("rec_stopping");
	TEST_EXPECT(!rig.to_running(stopping, 9103).empty());
	TEST_EXPECT(hosted::request_stop(rig.db, stopping, "stopped by its owner"));
	const std::string kept_container = rig.container(kept);
	const int64_t kept_machine = rig.machine_of(kept);
	// The service goes down: its previous run's listings are gone (main()
	// clears active_hosts at boot), two machines vanished.
	for (const uint32_t rid : { 9101u, 9102u, 9103u }) hostdb::remove_host_by_rid(rig.db, rid);
	rig.fake.vanish(rig.machine(rig.machine_of(lost))->cloud_id);
	rig.fake.vanish(rig.machine(rig.machine_of(stopping))->cloud_id);
	rig.make_supervisor();
	rig.advance(1000);
	rig.sup->reconcile(rig.now);

	TEST_EXPECT(rig.state(kept) == State::Running && rig.machine_of(kept) == kept_machine);
	auto row = rig.server(lost);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && row->container.empty() && !row->machine_ref);
	TEST_EXPECT(contains(last_detail(rig.db, lost), "its machine is gone; placed again (no restart counted)"));
	TEST_EXPECT(rig.state(stopping) == State::Stopped);
	// The resumed server's container is answered for without files.
	rig.sync_all();
	FakeAgent *agent = rig.agent_of(kept);
	TEST_EXPECT(agent && agent->last.desired.size() == 1 && agent->last.desired[0].name == kept_container &&
			!agent->last.desired[0].files && agent->find(kept_container) != nullptr);

	// The next pass places the lost one again at once (no backoff: no crash).
	rig.step();
	TEST_EXPECT(rig.fake.launches == 4);
	TEST_EXPECT(rig.walk(lost, State::Booting));
	TEST_EXPECT(hosted::request_stop(rig.db, lost, "done"));

	// The resumed server's checks wait out its grace, unlisted and uncounted,
	// and its listing comes back within it.
	for (int i = 0; i < 2; ++i) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
	}
	TEST_EXPECT(rig.state(kept) == State::Running);
	TEST_EXPECT(list_as(rig.db, *rig.server(kept)->service_user_id, 9104));
	rig.advance(rig.config.listing_timeout_ms);
	rig.step();
	TEST_EXPECT(rig.state(kept) == State::Running);
	TEST_EXPECT(rig.state(lost) == State::Stopped);
	return 0;
}

// The console ring (hosted_server_console): an append keeps a
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
	Database *db = &rig.db;
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
	} catch (const opennova::db::Error &) {
		refused = true;
	}
	TEST_EXPECT(refused && texts(a, 0, 100).size() == 5);
	return 0;
}

// The admin links follow the rows (Supervisor::set_admin_links): none before
// running; one once running, at the machine's private address and the slot's
// admin port with the login the start rendered; none from a crash's move back
// to provisioning, and a new one with the restart's new login once it runs
// again; none from stopping on; and the health check counts the link.
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
	TEST_EXPECT(rig.walk(id, State::Listing));
	TEST_EXPECT(links.find(id) == nullptr);
	TEST_EXPECT(list_as(rig.db, *rig.server(id)->service_user_id, 9201));
	rig.step(); // -> running
	TEST_EXPECT(rig.state(id) == State::Running);
	const auto link = links.find(id);
	const auto login = hosted::admin_login_of(rig.db, id);
	TEST_EXPECT(link != nullptr && login.has_value());
	if (link && login) {
		TEST_EXPECT(link->target().host == "10.0.1.1");
		TEST_EXPECT(link->target().port == login->port && login->port == 32800);
		TEST_EXPECT(link->target().user == "svc" && link->target().password == login->password);
	}
	rig.step();
	TEST_EXPECT(links.find(id) == link); // the same target keeps its link

	// A crash: the link goes with the move out of running.
	rig.agent_of(id)->exit_with(rig.container(id), 1);
	rig.sync_all();
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Provisioning && links.find(id) == nullptr);
	TEST_EXPECT(!rig.back_to_running(id, 9202).empty());
	const auto relinked = links.find(id);
	TEST_EXPECT(relinked != nullptr && relinked != link);
	if (relinked && link) TEST_EXPECT(relinked->target().password != link->target().password);

	// The stop: no link from stopping on.
	TEST_EXPECT(hosted::request_stop(rig.db, id, "stopped by its owner"));
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Stopping && links.find(id) == nullptr);

	// The health check counts the link (ADR 0053 d6): a running server whose link never
	// answers (no server behind it) fails after health_misses checks, listed all along.
	const int64_t quiet = rig.request("links_quiet");
	TEST_EXPECT(quiet != 0 && !rig.to_running(quiet, 9203).empty());
	TEST_EXPECT(links.find(quiet) != nullptr && !links.answering(quiet));
	TEST_EXPECT(rig.server(quiet)->slot == 1); // the stopping server still holds slot 0
	for (int i = 0; i < rig.config.health_misses; ++i) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
	}
	const auto failed = rig.server(quiet);
	TEST_EXPECT(failed && failed->state == State::Failed &&
			contains(failed->failure_reason, "no answer on its admin link"));
	TEST_EXPECT(links.find(quiet) == nullptr);
	links.stop_all();
	TEST_EXPECT(link == nullptr || link->finished());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "settings_rules", test_settings_rules },
		{ "renders", test_renders },
		{ "data_sets", test_data_sets },
		{ "game_no_longer_offered", test_game_no_longer_offered },
		{ "service_accounts", test_service_accounts },
		{ "requests", test_requests },
		{ "fleet_room", test_fleet_room },
		{ "lifecycle_and_stop", test_lifecycle_and_stop },
		{ "two_servers_one_machine", test_two_servers_one_machine },
		{ "own_machine", test_own_machine },
		{ "stop_paths", test_stop_paths },
		{ "exits_and_crash_restarts", test_exits_and_crash_restarts },
		{ "launch_failure", test_launch_failure },
		{ "restart_window", test_restart_window },
		{ "timeouts_and_health", test_timeouts_and_health },
		{ "machine_lost_replaces", test_machine_lost_replaces },
		{ "listing_waits_while_lost", test_listing_waits_while_lost },
		{ "owner_changes", test_owner_changes },
		{ "limit_at_provisioning", test_limit_at_provisioning },
		{ "owner_limits", test_owner_limits },
		{ "mass_stop_hold", test_mass_stop_hold },
		{ "hold_releases", test_hold_releases },
		{ "reconciliation", test_reconciliation },
		{ "console_ring", test_console_ring },
		{ "admin_links_follow_running", test_admin_links_follow_running },
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
