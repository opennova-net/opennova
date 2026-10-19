// The hosted servers' supervisor and what it stands on (apps/novaworld_common/
// hosted), on a migrated database with a FakeDriver whose machines' agents the
// test plays (hosted_fake_driver.h, hosted_fleet_rig.h) and a hand-stepped
// clock: the data sets (the host limits by title, what a start may pick from
// the rows with ready trees, a server whose data set has no data), the owner's
// settings and their checks against the data set's title's limits, its
// expansions with data and its trees' missions, the files and the arguments
// rendered from them for a slot (read back by the formats' own readers; /game
// per title), the trees a start pins and the sync carries, the service accounts (minted,
// refused by the website, hidden, reserved, their password set again at every
// start, deleted at the end), the start request's checks (the hosting
// entitlement, the global cap, the fleet's room under the planner), each
// server's lifecycle on the fleet (a shared machine launched and waited for,
// two servers on one machine's slots, an own machine of its own), the stop (the
// push, the listing's end, the 60 s timeout, any exit while stopping, a stop
// before any slot, a stop landing mid-placement, the kill switch), the crash
// restart and its limit (any exit, the OOM kill, a start that failed; a stop is
// never read from an exit code), a start whose data could not be fetched placed
// again after the backoff with no restart counted (the agent's line kept on its
// start, never in its event), a launch the cloud refused costing the server
// no restart, the restart window, the boot and listing timeouts, the health
// checks, a lost machine placing its server again with no restart counted (and
// holding its listing's clock), no lobby live (nothing placed, an exit costing
// no restart, the listing's clocks waiting), an owner's account banned
// or deleted, its comp grant withdrawn or its support tier canceled (also
// during a crash restart's backoff), the owners' limits (own machines cut moving
// their servers to shared machines while the fleet has room, stopping the rest,
// and the provisioning re-check) and their hold, the
// reconciliation at the service's start, and the admin links kept in step with
// the running servers. Compiles the server's accounts/auth.cpp, accounts/web_session.cpp and the
// hosted sources directly, so it runs on every build.

#include "hosted_fleet_rig.h"

#include "accounts/web_session.h"
#include "core/random_secret.h"
#include "hosted/admin_link.h"
#include "hosted/model/address_jobs.h"
#include "hosted/model/address_repository.h"

#include <base/gameprofile/gameprofile.h>
#include <base/io/cp1252.h>
#include <formats/admincfg/admin_cfg.h>
#include <formats/gamecfg/game_cfg.h>

#include "common/test_expect.h"
#include "common/test_parts.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace hosted_rig;
using opennova::novaworld_server::hosted::AddressState;

namespace {

// The file `name` of the container the server's agent was given.
std::string file_of(Rig &rig, int64_t id, const std::string &name) {
	const FakeAgent::Container *c = rig.running_container(id);
	if (!c || !c->desired.files) return {};
	const auto it = c->desired.files->find(name);
	return it == c->desired.files->end() ? std::string() : it->second;
}

// --- the settings and the renders ---------------------------------------------

// The offered data sets check_settings is run against here, as
// offered_data_sets lists them: Joint Operations (title jo) with Escalation
// (jox01), Delta Force Xtreme 2 (dfx2) and OpenNova's own data (jo), each with
// its tree's missions.
std::vector<hosted::OfferedDataSet> offered_for_rules() {
	std::vector<hosted::OfferedDataSet> out;
	hosted::OfferedDataSet jo;
	jo.id = 1;
	jo.display_name = "Joint Operations";
	jo.title = "jo";
	jo.game = "jop_2_consumer";
	jo.tree_id = 10;
	jo.version = "JO V1.7.5.3";
	jo.missions = { { "SERVETST.BMS", "Serve Test" }, { "second.bms", "Second" }, { "ASH_G11A.BMS", "Ash" },
		{ "x-y_1.bms", "XY" }, { "map.bms", "Map" } };
	hosted::OfferedExpansion jox;
	jox.id = 2;
	jox.code = "jox01";
	jox.display_name = "Escalation";
	jox.tree_id = 11;
	jox.missions = { { "escal.bms", "Escalation Map" } };
	jo.expansions.push_back(jox);
	out.push_back(jo);
	hosted::OfferedDataSet dfx2;
	dfx2.id = 3;
	dfx2.display_name = "Delta Force Xtreme 2";
	dfx2.title = "dfx2";
	dfx2.game = "dfx2_consumer";
	dfx2.tree_id = 12;
	dfx2.missions = { { "servetst.bms", "Serve Test" }, { "second.bms", "Second" } };
	out.push_back(dfx2);
	hosted::OfferedDataSet on;
	on.id = 4;
	on.display_name = "OpenNova";
	on.title = "jo";
	on.game = "opennova_0_consumer";
	on.tree_id = 13;
	on.missions = dfx2.missions;
	out.push_back(on);
	return out;
}

int test_settings_rules() {
	const std::vector<hosted::OfferedDataSet> offered = offered_for_rules();
	const auto check = [&offered](const hosted::HostedSettings &c) { return hosted::check_settings(c, offered); };
	hosted::HostedSettings s = valid_settings(1);
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
	// The expansion: one of the data set's with data.
	TEST_EXPECT(refused(with([](auto &c) { c.expansion_id = 99; }), "expansion_id"));
	TEST_EXPECT(refused(with([](auto &c) { c.expansion_id = 1; }), "expansion_id")); // the base itself
	TEST_EXPECT(!check(with([](auto &c) { c.expansion_id = 2; })));
	// The data set: one the service offers, whose title's limits the whole numbers take.
	TEST_EXPECT(refused(with([](auto &c) { c.data_set_id = 0; }), "data_set_id"));
	TEST_EXPECT(refused(with([](auto &c) { c.data_set_id = 77; }), "data_set_id"));
	TEST_EXPECT(!check(with([](auto &c) { c.data_set_id = 3; })));
	TEST_EXPECT(!check(with([](auto &c) { c.data_set_id = 4; })));
	// Delta Force Xtreme 2's host screen takes 100 players, Joint Operations' and
	// OpenNova's 64 (host_limits.h, by the base's title).
	TEST_EXPECT(!check(with([](auto &c) {
		c.data_set_id = 3;
		c.max_players = 100;
	})));
	TEST_EXPECT(refused(with([](auto &c) {
		c.data_set_id = 3;
		c.max_players = 101;
	}), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) {
		c.data_set_id = 4;
		c.max_players = 65;
	}), "max_players"));
	TEST_EXPECT(refused(with([](auto &c) {
		c.data_set_id = 3;
		c.expansion_id = 2;
	}), "expansion_id"));
	TEST_EXPECT(refused(with([](auto &c) {
		c.data_set_id = 4;
		c.expansion_id = 2;
	}), "expansion_id"));
	{
		// Only what is offered: a data set with no data is refused.
		const std::vector<hosted::OfferedDataSet> jo_only = { offered[0] };
		hosted::HostedSettings dfx2 = s;
		dfx2.data_set_id = 3;
		const auto r = hosted::check_settings(dfx2, jo_only);
		TEST_EXPECT(r && r->field == "data_set_id" && r->reason == "is no data set this service offers");
		TEST_EXPECT(!hosted::check_settings(s, jo_only));
		const hosted::HostLimits &jo = hosted::host_limits_for_title("jo");
		hosted::HostedSettings big = s;
		big.max_players = jo.max_players.max + 1;
		const auto too_many = hosted::check_settings(big, offered);
		TEST_EXPECT(too_many && too_many->field == "max_players" && too_many->reason == "must be 2 to 64");
		hosted::HostedSettings escalation = s;
		escalation.data_set_id = 3;
		escalation.expansion_id = 2;
		const auto no_exp = hosted::check_settings(escalation, offered);
		TEST_EXPECT(no_exp && no_exp->reason == "is no expansion of this data set with data");
	}
	// The rotation: 1..kMissionsMax names, each a .bms file name among the base's
	// missions plus the expansion's (by file, without case).
	TEST_EXPECT(hosted::kMissionsMax == 128);
	TEST_EXPECT(refused(with([](auto &c) { c.missions.clear(); }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions.assign(129, "map.bms"); }), "missions"));
	TEST_EXPECT(!check(with([](auto &c) { c.missions.assign(128, "map.bms"); })));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "../up.bms" }; }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "map.txt" }; }), "missions"));
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "two words.bms" }; }), "missions"));
	TEST_EXPECT(!check(with([](auto &c) { c.missions = { "ASH_G11A.BMS", "x-y_1.bms" }; })));
	TEST_EXPECT(!check(with([](auto &c) { c.missions = { "servetst.BMS", "Second.BMS" }; })));
	{
		const auto unknown = check(with([](auto &c) { c.missions = { "second.bms", "unknown.bms" }; }));
		TEST_EXPECT(unknown && unknown->field == "missions" &&
				unknown->reason == "names \"unknown.bms\", which is no mission of this data set");
	}
	// The expansion's missions count only with the expansion picked.
	TEST_EXPECT(refused(with([](auto &c) { c.missions = { "escal.bms" }; }), "missions"));
	TEST_EXPECT(!check(with([](auto &c) {
		c.expansion_id = 2;
		c.missions = { "second.bms", "ESCAL.BMS" };
	})));

	TEST_EXPECT(hosted::split_missions(hosted::join_missions(s.missions)) == s.missions);
	TEST_EXPECT(hosted::split_missions("").empty());
	return 0;
}

int test_renders() {
	hosted::HostedSettings s = valid_settings(1);
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

	const std::string admin_password = nws::random_secret(hosted::kAdminPasswordChars);
	const std::string admin_text = hosted::render_admin_cfg(plan, admin_password);
	const auto admin = opennova::admincfg::parse(admin_text.data(), admin_text.size());
	TEST_EXPECT(admin.users.size() == 1);
	TEST_EXPECT(admin.users[0].name == "svc" && admin.users[0].password == admin_password);
	TEST_EXPECT(admin.users[0].rights == 0x7Fu);
	TEST_EXPECT(admin.ip_restrictions == std::vector<std::string>{ "10.1.2.3" });

	const std::string creds = hosted::render_credentials("srv-12", "pw");
	TEST_EXPECT(credential(creds, "NOVAWORLD_USER") == "srv-12");
	TEST_EXPECT(credential(creds, "NOVAWORLD_PASS") == "pw");

	const std::string a = nws::random_secret(31);
	const std::string b = nws::random_secret(31);
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
	s.expansion_id = 2;
	const hosted::LaunchSpec spec = hosted::render_launch(s, "jo", "jox01", in);
	TEST_EXPECT(spec.server_id == 12 && spec.files.size() == 4);
	for (const auto &f : spec.files) {
		const bool secret = f.name != "game.cfg"; // the host file carries the game password
		TEST_EXPECT(f.secret == secret);
	}
	const std::vector<std::string> args = { "/HOST", "server.host", "--credentials",
		"credentials.txt", "--master-host", "203.0.113.5", "--master-gate-port", "7597",
		"--status-page", "off", "--stats-file", "tick-stats.json", "--exit-file", "exit.txt",
		"/game", "jo", "/exp", "jox01" };
	TEST_EXPECT(spec.serve_args == args);
	TEST_EXPECT(hosted::render_serve_args(s, "jo", "jox01", "203.0.113.5", 7597) == args);

	// Each data set speaks its title (its row's); OpenNova's data speaks Joint
	// Operations', and no expansion is no /exp.
	s.expansion_id.reset();
	const hosted::LaunchSpec dfx2 = hosted::render_launch(s, "dfx2", "", in);
	const std::vector<std::string> dfx2_args = { "/HOST", "server.host", "--credentials",
		"credentials.txt", "--master-host", "203.0.113.5", "--master-gate-port", "7597",
		"--status-page", "off", "--stats-file", "tick-stats.json", "--exit-file", "exit.txt",
		"/game", "dfx2" };
	TEST_EXPECT(dfx2.serve_args == dfx2_args);
	const hosted::LaunchSpec on = hosted::render_launch(s, "jo", "", in);
	TEST_EXPECT(on.serve_args.size() == 16 && on.serve_args[14] == "/game" &&
			on.serve_args[15] == "jo");
	return 0;
}

// --- the data sets ----------------------------------------------------------------

// A server whose data set has no data at its placement (its tree pruned since
// the request, no ready current tree, and no pins to keep) fails before any
// account or machine: no restart mends that. An expansion with no data, and a
// data set row gone, the same.
int test_no_data() {
	Rig rig("hosted_no_data");
	hosted::HostedSettings settings = valid_settings(rig.data.dfx2);
	settings.missions = { "dfx2_a.bms" };
	const int64_t id = rig.request("gone_owner", settings);
	TEST_EXPECT(id > 0);
	TEST_EXPECT(rig.server(id)->settings.data_set_id == rig.data.dfx2);
	// Its data goes: the tree pruned, the data set with no current tree.
	rig.db.exec("UPDATE data_sets SET current_tree_id = NULL WHERE id = $1;", { BindValue(rig.data.dfx2) });
	hosted::mark_tree_pruned(rig.db, rig.data.dfx2_tree);
	rig.tick(); // -> provisioning
	rig.tick(); // -> failed
	const auto row = rig.server(id);
	TEST_EXPECT(row && row->state == State::Failed);
	TEST_EXPECT(row && row->failure_reason == "its data set has no data");
	TEST_EXPECT(rig.fake.launches == 0 && row && !row->service_user_id && !row->machine_ref);

	// The expansion's data goes while the base's stays.
	hosted::HostedSettings escalation = rig.settings();
	escalation.expansion_id = rig.data.jox;
	const int64_t exp = rig.request("gone_exp_owner", escalation);
	TEST_EXPECT(exp > 0);
	rig.db.exec("UPDATE data_sets SET current_tree_id = NULL WHERE id = $1;", { BindValue(rig.data.jox) });
	rig.tick();
	rig.tick();
	TEST_EXPECT(rig.state(exp) == State::Failed && rig.server(exp)->failure_reason == "its expansion has no data");
	TEST_EXPECT(rig.fake.launches == 0);

	// The data set row gone (its id set NULL on the server's row): named as such.
	const int64_t on = seeded_data_set(rig.db, "OpenNova");
	TEST_EXPECT(seed_tree(rig.db, on, "ON V0.1", { "on.bms" }) != 0);
	hosted::HostedSettings on_settings = valid_settings(on);
	on_settings.missions = { "on.bms" };
	const int64_t orphan = rig.request("gone_row_owner", on_settings);
	TEST_EXPECT(orphan > 0);
	rig.db.exec("DELETE FROM data_sets WHERE id = $1;", { BindValue(on) });
	rig.tick();
	rig.tick();
	TEST_EXPECT(rig.state(orphan) == State::Failed && rig.server(orphan)->failure_reason == "its data set is gone");
	return 0;
}

// The host limits by title (host_limits.h) and what a start may pick
// (offered_data_sets: the bases with a ready current tree, each with its
// expansions that have one), over the rig's seeded rows.
int test_data_sets() {
	const hosted::HostLimits &jo = hosted::host_limits_for_title("jo");
	const hosted::HostLimits &dfx2 = hosted::host_limits_for_title("dfx2");
	TEST_EXPECT(jo.max_players.min == 2 && jo.max_players.max == 64 && jo.time_limit.max == 300 &&
			jo.kill_limit.max == 500 && jo.max_score.max == 500 && jo.respawn.min == 0 && jo.start_delay.max == 300);
	TEST_EXPECT(dfx2.max_players.max == 100 && dfx2.time_limit.max == 300 && dfx2.kill_limit.max == 500);
	// Every title this service hosts runs Joint Operations' rules but DFX2's cap.
	TEST_EXPECT(&hosted::host_limits_for_title("opennova") == &jo && &hosted::host_limits_for_title("") == &jo);

	Rig rig("hosted_data_sets");
	const std::vector<hosted::OfferedDataSet> offered = rig.offered();
	// Joint Operations with Escalation, then Delta Force Xtreme 2; OpenNova,
	// with no tree, is not offered.
	TEST_EXPECT(offered.size() == 2);
	if (offered.size() != 2) return 1;
	TEST_EXPECT(offered[0].id == rig.data.jo && offered[0].title == "jo" && offered[0].game == "jop_2_consumer" &&
			offered[0].display_name == "Joint Operations" && offered[0].tree_id == rig.data.jo_tree &&
			offered[0].version == "JO V1.7.5.3" && offered[0].missions.size() == 3 &&
			offered[0].missions[1].file == "SERVETST.BMS" && offered[0].missions[1].title == "Map SERVETST.BMS");
	TEST_EXPECT(offered[0].expansions.size() == 1 && offered[0].expansions[0].id == rig.data.jox &&
			offered[0].expansions[0].code == "jox01" && offered[0].expansions[0].display_name == "Escalation" &&
			offered[0].expansions[0].tree_id == rig.data.jox_tree && offered[0].expansions[0].missions.size() == 1);
	TEST_EXPECT(offered[1].id == rig.data.dfx2 && offered[1].title == "dfx2" && offered[1].expansions.empty() &&
			offered[1].missions.size() == 2);
	const int64_t on = seeded_data_set(rig.db, "OpenNova");
	TEST_EXPECT(hosted::find_offered(offered, rig.data.jo) != nullptr && hosted::find_offered(offered, on) == nullptr);
	TEST_EXPECT(hosted::find_offered_expansion(offered[0], rig.data.jox) != nullptr &&
			hosted::find_offered_expansion(offered[0], rig.data.jo) == nullptr);
	// Each title is a gameprofile code opennova-serve's /game takes.
	for (const hosted::OfferedDataSet &d : offered)
		TEST_EXPECT(opennova::gameprofile::gameprofile_by_code(d.title.c_str()) != nullptr);
	// A ready tree offers OpenNova's data, speaking Joint Operations' title.
	TEST_EXPECT(seed_tree(rig.db, on, "ON V0.1", { "on.bms" }) != 0);
	const std::vector<hosted::OfferedDataSet> now = rig.offered();
	TEST_EXPECT(now.size() == 3 && now[2].id == on && now[2].title == "jo" && now[2].version == "ON V0.1");
	return 0;
}

// The trees a server is pinned to (ADR 0064): its first start takes its data
// sets' current trees; a crash restart and a fleet-caused replace keep them
// while newer data arrives (the owner's view says an update waits); the
// owner's update places the server again on the current trees, a new
// generation, no restart counted; the admin's update-all does the same for
// every outdated server; a pinned tree pruned falls back to the current one.
int test_pins_and_updates() {
	Rig rig("hosted_pins");
	Database &db = rig.db;
	hosted::HostedSettings settings = rig.settings();
	settings.expansion_id = rig.data.jox;
	const int64_t id = rig.request("pins_owner", settings);
	TEST_EXPECT(id > 0 && !rig.to_running(id, 9301).empty());
	auto row = rig.server(id);
	TEST_EXPECT(row->base_tree_id == rig.data.jo_tree && row->expansion_tree_id == rig.data.jox_tree &&
			row->generation == 1 && !hosted::server_outdated(db, id));
	TEST_EXPECT(hosted::request_update(db, id) == hosted::UpdateOutcome::UpToDate);

	// Newer data for the base: the server is outdated, runs on, and a crash
	// restart keeps its pins (its generation moves on, its restart counted).
	const int64_t jo_tree2 = seed_tree(db, rig.data.jo, "JO V1.7.5.4", { "second.bms", "SERVETST.BMS" });
	TEST_EXPECT(jo_tree2 != 0 && hosted::server_outdated(db, id));
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Running && rig.server(id)->base_tree_id == rig.data.jo_tree);
	rig.agent_of(id)->exit_with(rig.container(id), 1);
	rig.sync_all();
	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == 1);
	TEST_EXPECT(!rig.back_to_running(id, 9302).empty());
	row = rig.server(id);
	TEST_EXPECT(row->generation == 2 && row->base_tree_id == rig.data.jo_tree &&
			row->expansion_tree_id == rig.data.jox_tree && hosted::server_outdated(db, id));
	{
		const FakeAgent::Container *c = rig.running_container(id);
		TEST_EXPECT(c && c->desired.base_tree == rig.data.jo_tree);
	}
	// A fleet-caused replace (its machine drained) keeps them too.
	const int64_t machine = rig.machine_of(id);
	TEST_EXPECT(hosted::transition_machine(db, machine, hosted::FleetMachineState::Ready,
			hosted::FleetMachineState::Draining, "test"));
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == 1);
	TEST_EXPECT(!rig.to_running(id, 9303).empty());
	row = rig.server(id);
	TEST_EXPECT(row->generation == 3 && rig.machine_of(id) != machine && row->base_tree_id == rig.data.jo_tree &&
			row->expansion_tree_id == rig.data.jox_tree);

	// The owner's update: placed again at once, no restart counted, the next
	// start on the current trees with a new generation; the flag cleared.
	TEST_EXPECT(hosted::request_update(db, id) == hosted::UpdateOutcome::Requested);
	TEST_EXPECT(rig.server(id)->update_requested);
	rig.step();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 1 &&
			contains(last_detail(db, id), "updated to its data set's current data"));
	TEST_EXPECT(!rig.to_running(id, 9304).empty());
	row = rig.server(id);
	TEST_EXPECT(row->generation == 4 && row->base_tree_id == jo_tree2 && row->expansion_tree_id == rig.data.jox_tree &&
			!row->update_requested && !hosted::server_outdated(db, id) && row->restarts == 1);
	// Its runs, each ended with why: a crash, its machine's drain, the owner's update;
	// the fourth on the new tree, open.
	{
		const auto starts = hosted::list_starts(db, id, 10);
		TEST_EXPECT(starts.size() == 4 && starts[0].generation == 4 && starts[0].ended_at.empty() &&
				starts[0].base_tree_id == jo_tree2 && starts[1].cause == hosted::StartCause::Update &&
				starts[2].cause == hosted::StartCause::Fleet && starts[3].cause == hosted::StartCause::Crash &&
				starts[3].exit_code == 1 && starts[2].machine_id == machine && starts[1].machine_id != machine);
	}
	{
		const FakeAgent::Container *c = rig.running_container(id);
		TEST_EXPECT(c && c->desired.base_tree == jo_tree2 && c->desired.expansion_tree == rig.data.jox_tree);
		const FakeAgent *agent = rig.agent_of(id);
		TEST_EXPECT(agent && agent->last.trees.count(jo_tree2) == 1 &&
				agent->last.trees.at(jo_tree2).version == "JO V1.7.5.4");
	}
	TEST_EXPECT(has_event(db, id, "with tree " + std::to_string(jo_tree2)));

	// The admin's update-all: every outdated server (a second one on Delta Force
	// Xtreme 2, up to date, is left alone), each placed again.
	hosted::HostedSettings dfx2 = valid_settings(rig.data.dfx2);
	dfx2.missions = { "dfx2_a.bms" };
	const int64_t other = rig.request("pins_other", dfx2);
	TEST_EXPECT(other > 0 && !rig.to_running(other, 9305).empty());
	const int64_t jox_tree2 = seed_tree(db, rig.data.jox, "JOX V1.1", { "escal.bms" }, { "jox01.pff", "jox01L.pff", "version.txt" });
	TEST_EXPECT(jox_tree2 != 0 && hosted::server_outdated(db, id) && !hosted::server_outdated(db, other));
	TEST_EXPECT(hosted::request_update_all(db, std::nullopt) == 1);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.state(other) == State::Running);
	TEST_EXPECT(!rig.to_running(id, 9306).empty());
	row = rig.server(id);
	TEST_EXPECT(row->generation == 5 && row->base_tree_id == jo_tree2 && row->expansion_tree_id == jox_tree2 &&
			row->restarts == 1);
	TEST_EXPECT(rig.server(other)->generation == 1 && rig.server(other)->base_tree_id == rig.data.dfx2_tree);

	// A pinned tree pruned: a crash restart falls back to the current tree.
	const int64_t dfx2_tree2 = seed_tree(db, rig.data.dfx2, "DFX2 V1.1", { "dfx2_a.bms", "dfx2_b.bms" });
	TEST_EXPECT(dfx2_tree2 != 0);
	hosted::mark_tree_pruned(db, rig.data.dfx2_tree);
	rig.agent_of(other)->exit_with(rig.container(other), 1);
	rig.sync_all();
	rig.tick();
	TEST_EXPECT(rig.state(other) == State::Provisioning);
	TEST_EXPECT(!rig.back_to_running(other, 9307).empty());
	TEST_EXPECT(rig.server(other)->base_tree_id == dfx2_tree2 && rig.server(other)->restarts == 1);
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
	TEST_EXPECT(nws::billing::comp_grants_of(db, made.id).servers == 0 && account->role == "player");
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
	TEST_EXPECT(!nws::billing::set_comp_grants(db, made.id, { 1, 0 }));
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
	// The comp grants: a player's alone, a 0 deleting its row.
	TEST_EXPECT(nws::billing::set_comp_grants(db, player, { 2, 1 }));
	auto grants = nws::billing::comp_grants_of(db, player);
	TEST_EXPECT(grants.servers == 2 && grants.own_machines == 1);
	TEST_EXPECT(!nws::billing::set_comp_grants(db, 999999, { 1, 0 }));
	TEST_EXPECT(nws::billing::set_comp_grants(db, player, { 3, 0 }));
	grants = nws::billing::comp_grants_of(db, player);
	TEST_EXPECT(grants.servers == 3 && grants.own_machines == 0);
	TEST_EXPECT(db.query("SELECT 1 FROM comp_grants WHERE user_id = $1;", { BindValue(player) }).size() == 1);
	TEST_EXPECT(nws::billing::comp_grants_by_account(db).at(player).servers == 3);
	return 0;
}

// --- the requests ---------------------------------------------------------------

int test_requests() {
	Rig rig("hosted_requests");
	Database &db = rig.db;
	const auto settings = rig.settings();
	const std::vector<hosted::OfferedDataSet> offered = rig.offered();
	const auto request = [&](int64_t user, const hosted::HostedSettings &with,
								 const std::vector<hosted::OfferedDataSet> &sets, int cap) {
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
	TEST_EXPECT(row->settings.data_set_id == rig.data.jo && !row->settings.expansion_id);
	TEST_EXPECT(!row->base_tree_id && !row->expansion_tree_id && !row->update_requested);
	TEST_EXPECT(!row->machine_ref && row->slot == -1 && row->generation == 0 && row->serve_image.empty());
	// A data set not offered is refused here too.
	hosted::HostedSettings dfx2 = settings;
	dfx2.data_set_id = rig.data.dfx2;
	dfx2.missions = { "dfx2_a.bms" };
	const std::vector<hosted::OfferedDataSet> jo_only = { offered[0] };
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
		return hosted::request_server(*db, owner, rig.settings(), rig.offered(), rig.config.cap, rig.wall,
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
	hosted::HostedSettings settings = rig.settings();
	settings.expansion_id = rig.data.jox;
	const int64_t id = rig.request("life_owner", settings);
	TEST_EXPECT(id > 0);

	rig.tick();
	TEST_EXPECT(rig.state(id) == State::Provisioning && rig.fake.launches == 0);
	// No machine yet: one is launched (its row and its launch job, which a
	// worker runs after the pass), and the next pass places the server on its
	// first slot (it waits for the machine's boot in booting).
	rig.tick();
	TEST_EXPECT(rig.fake.launches == 1 && rig.state(id) == State::Provisioning);
	auto machines = hosted::list_live_machines(rig.db);
	TEST_EXPECT(machines.size() == 1);
	const hosted::FleetMachineRow launched = machines.front();
	TEST_EXPECT(launched.kind == hosted::MachineKind::Shared && launched.slots == 4 &&
			launched.state == hosted::FleetMachineState::Booting && launched.provider == "fake" &&
			launched.cloud_id == rig.fake.last && launched.private_address == "10.0.1.1" &&
			launched.serve_image == kServeImage && launched.reserved_for == 0);
	TEST_EXPECT(rig.fake.machines.at(launched.cloud_id).token.size() == hosted::kMachineTokenChars);
	rig.tick();
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Booting && row->machine_ref == launched.id && row->slot == 0);
	TEST_EXPECT(row->game_port == 32768 && row->generation == 1 && row->serve_image == kServeImage);
	TEST_EXPECT(row->provider == "fake" && row->container == "srv-" + std::to_string(id) + "-g1");
	TEST_EXPECT(row->machine_address == "10.0.1.1");
	// Its first start pinned it to its data sets' current trees.
	TEST_EXPECT(row->base_tree_id == rig.data.jo_tree && row->expansion_tree_id == rig.data.jox_tree &&
			!row->update_requested);
	TEST_EXPECT(contains(last_detail(rig.db, id), "with tree " + std::to_string(rig.data.jo_tree) +
							 " and expansion tree " + std::to_string(rig.data.jox_tree)));
	// Its start: made with its slot, its image, trees and log stream written as it rendered.
	{
		const auto starts = hosted::list_starts(rig.db, id, 10);
		TEST_EXPECT(starts.size() == 1 && starts[0].generation == 1 && starts[0].machine_id == launched.id &&
				starts[0].slot == 0 && starts[0].serve_image == kServeImage &&
				starts[0].base_tree_id == rig.data.jo_tree && starts[0].expansion_tree_id == rig.data.jox_tree &&
				starts[0].log_stream == std::to_string(id) + "/g1" && starts[0].running_at.empty() &&
				starts[0].ended_at.empty());
	}
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
	TEST_EXPECT(c->desired.base_tree == rig.data.jo_tree && c->desired.expansion_tree == rig.data.jox_tree &&
			c->desired.expansion_code == "jox01" && c->desired.memory_mb == 512 && c->desired.pids == 128);
	// The reply carried both trees beside it: where to fetch each and its manifest.
	{
		const FakeAgent *agent = rig.agent_of(id);
		TEST_EXPECT(agent && agent->last.trees.size() == 2 && agent->last.trees.count(rig.data.jo_tree) == 1 &&
				agent->last.trees.count(rig.data.jox_tree) == 1);
		if (agent && agent->last.trees.count(rig.data.jox_tree) == 1) {
			const hosted::TreeSpec &jox = agent->last.trees.at(rig.data.jox_tree);
			TEST_EXPECT(jox.url == tree_url_of(rig.data.jox_tree) && jox.version == "JOX V1.0" &&
					jox.files.size() == 3 && jox.files[0].name == "jox01.pff" && jox.files[0].size == 1000);
		}
	}
	const std::vector<std::string> args = { "/HOST", "server.host", "--credentials",
		"credentials.txt", "--master-host", "203.0.113.5", "--master-gate-port", "7597",
		"--status-page", "off", "--stats-file", "tick-stats.json", "--exit-file", "exit.txt",
		"/game", "jo", "/exp", "jox01" };
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
	TEST_EXPECT(!hosted::list_starts(rig.db, id, 1)[0].running_at.empty());
	// The container names the stream its output goes to.
	TEST_EXPECT(rig.running_container(id) &&
			rig.running_container(id)->desired.log_stream == std::to_string(id) + "/g1");
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
	// The stop ended its start.
	{
		const auto starts = hosted::list_starts(rig.db, id, 10);
		TEST_EXPECT(starts.size() == 1 && !starts[0].ended_at.empty() &&
				starts[0].cause == hosted::StartCause::Stop && !starts[0].exit_code);
	}
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
	const int64_t own = rig.request_as(owner, rig.settings(), true);
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

// Stopped and started again at the fleet's maximum of own machines (1): the
// start is taken while the stopped server's machine still waits for its end
// (its place went with its server), another owner's own-machine start is
// still at_capacity, and the new server's machine launches once the old one
// has ended, never past the maximum (no launch refused).
int test_own_restart_at_cap() {
	Rig rig("hosted_own_restart");
	rig.config.cap = 10;
	rig.config.placement.max_own = 1;
	rig.make_supervisor();
	Database *db = &rig.db;
	const hosted::PlacementPolicy &fleet = rig.config.placement;
	const int64_t owner = make_player(*db, "restart_owner", 0);
	subscribe(*db, owner, 1, "active", "sub_restart");
	subscribe_machines(*db, owner, 1, "sub_restart_machine");
	const int64_t other = make_player(*db, "restart_other", 0);
	subscribe(*db, other, 1, "active", "sub_restart_other");
	subscribe_machines(*db, other, 1, "sub_restart_other_machine");
	const int64_t first = rig.request_as(owner, rig.settings(), true);
	TEST_EXPECT(first > 0 && !rig.to_running(first, 5211).empty());
	const int64_t old_machine = rig.machine_of(first);
	TEST_EXPECT(!hosted::fleet_has_room(*db, fleet, true));
	TEST_EXPECT(hosted::request_stop(*db, first, "stopped by its owner"));
	rig.step(); // the push
	hostdb::remove_host_by_rid(*db, 5211);
	rig.step(); // stopped
	TEST_EXPECT(rig.state(first) == State::Stopped);
	// Its machine takes servers until the next pass ends it; its place is free.
	TEST_EXPECT(rig.machine(old_machine)->state == hosted::FleetMachineState::Ready);
	TEST_EXPECT(hosted::fleet_has_room(*db, fleet, true));
	const int64_t again = rig.request_as(owner, rig.settings(), true);
	TEST_EXPECT(again > 0);
	// The one place is the new server's.
	TEST_EXPECT(rig.request_as(other, rig.settings(), true) == 0);
	TEST_EXPECT(!hosted::fleet_has_room(*db, fleet, true));
	TEST_EXPECT(!rig.to_running(again, 5212).empty());
	TEST_EXPECT(rig.machine(old_machine)->state == hosted::FleetMachineState::Ended);
	TEST_EXPECT(rig.machine_of(again) != old_machine && rig.fake.launches == 2);
	TEST_EXPECT(db->query("SELECT COUNT(*) FROM fleet_machines WHERE state = 'failed';").front().as_int(0) == 0);
	return 0;
}

// A static address (hosted/model/address_repository.h): its own machine launched to carry
// it, the server's container held back (booting, its agent told to start nothing)
// while the machine's row does not show the address, an address.associate asked for
// meanwhile, then the start once a describe brings the address; the subscription
// ended, the address lapses only after a clean reconcile with Stripe, its machine
// ends and the server is placed again without it (no restart counted), and the
// address goes releasing once no machine carries it.
int test_static_address() {
	namespace billing = opennova::novaworld_server::billing;
	Rig rig("hosted_address");
	Database *db = &rig.db;
	const int64_t owner = make_player(*db, "addr_owner", 0);
	subscribe(*db, owner, 1, "active", "sub_addr_tier");
	subscribe_machines(*db, owner, 1, "sub_addr_machine");
	billing::SubscriptionRow address_sub;
	address_sub.stripe_subscription_id = "sub_addr_address";
	address_sub.stripe_customer_id = "cus_addr";
	address_sub.user_id = owner;
	address_sub.status = "active";
	address_sub.item = billing::ItemKind::StaticAddress;
	address_sub.price_id = "price_address";
	address_sub.current_period_end = kWall + 30 * 24 * 60 * 60;
	billing::upsert_subscription(*db, address_sub);
	const auto named = hosted::name_subscription_address(*db, owner, "rig-place", rig.wall);
	TEST_EXPECT(named.outcome == hosted::NameOutcome::Created);
	const int64_t address = named.id;
	const std::string ip = "203.0.113.50";
	TEST_EXPECT(hosted::record_allocation(*db, address, "eipalloc-rig", ip) &&
			hosted::record_dns_upsert(*db, address, "rig-place", "c1"));
	const auto r = hosted::request_server(*db, owner, rig.settings(), rig.offered(), rig.config.cap, rig.wall, true,
			&rig.config.placement, address);
	TEST_EXPECT(r.outcome == hosted::RequestOutcome::Created);
	const int64_t id = r.id;

	// Its machine carries the address, and starts nothing while it shows another.
	for (int i = 0; i < 6; ++i) rig.step();
	const int64_t machine = rig.machine_of(id);
	TEST_EXPECT(machine > 0 && rig.machine(machine)->address_id == address &&
			rig.machine(machine)->state == hosted::FleetMachineState::Ready);
	TEST_EXPECT(rig.machine(machine)->public_address != ip && rig.state(id) == State::Booting);
	// The hold's rule, which fleet_health and the owner's phase read too.
	TEST_EXPECT(hosted::awaits_address(*db, *rig.machine(machine)));
	TEST_EXPECT(rig.agent_of(id) != nullptr && rig.agent_of(id)->last.desired.empty() &&
			rig.running_container(id) == nullptr);
	TEST_EXPECT(hosted::address_associate_pending(*db, machine));
	// The association's describe brings the address: the server starts at it.
	rig.fake.machines.at(rig.machine(machine)->cloud_id).public_address = ip;
	TEST_EXPECT(!rig.to_running(id, 5301).empty());
	TEST_EXPECT(rig.machine(machine)->public_address == ip);
	TEST_EXPECT(!hosted::awaits_address(*db, *rig.machine(machine)));

	// Its subscription ends: nothing lapses before a clean reconcile confirms it.
	address_sub.status = "canceled";
	billing::upsert_subscription(*db, address_sub);
	rig.step();
	rig.step();
	TEST_EXPECT(hosted::find_address(*db, address)->state == AddressState::Ready && rig.state(id) == State::Running);
	hosted::limits_hold::reconciled(*db);
	rig.step();
	// Lapsed: its machine ended, the server placed again without it, no restart counted.
	TEST_EXPECT(hosted::find_address(*db, address)->state != AddressState::Ready);
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Ended);
	const auto replaced = rig.server(id);
	TEST_EXPECT(replaced && !replaced->address_id && replaced->restarts == 0 && is_live(replaced->state) &&
			has_event(*db, id, "its static address rig-place lapsed"));
	rig.step();
	TEST_EXPECT(hosted::find_address(*db, address)->state == AddressState::Releasing);
	TEST_EXPECT(jobs::job_pending(*db, std::string(hosted::kJobAddressRelease) + ":" + std::to_string(address)));
	// It runs again on a machine of its own with no address.
	TEST_EXPECT(!rig.to_running(id, 5302).empty());
	TEST_EXPECT(rig.machine(rig.machine_of(id))->address_id == 0);
	return 0;
}

// The lapses' brake: more subscriptions' addresses lapsing in one pass than
// max(2, half of them) is the cache in doubt, and none lapses. Then, with the
// add-on on, the allocations run again: one whose job is gone, and a refusal
// that passes once its retry_at is reached.
int test_static_address_brake() {
	namespace billing = opennova::novaworld_server::billing;
	Rig rig("hosted_address_brake");
	Database *db = &rig.db;
	std::vector<int64_t> addresses;
	for (int i = 0; i < 3; ++i) {
		const std::string n = std::to_string(i);
		const int64_t owner = make_player(*db, "brake_owner" + n, 0);
		subscribe(*db, owner, 1, "active", "sub_brake_tier" + n);
		subscribe_machines(*db, owner, 1, "sub_brake_machine" + n);
		billing::SubscriptionRow sub;
		sub.stripe_subscription_id = "sub_brake_address" + n;
		sub.stripe_customer_id = "cus_brake" + n;
		sub.user_id = owner;
		sub.status = "active";
		sub.item = billing::ItemKind::StaticAddress;
		sub.price_id = "price_address";
		sub.current_period_end = kWall + 30 * 24 * 60 * 60;
		billing::upsert_subscription(*db, sub);
		const auto named = hosted::name_subscription_address(*db, owner, "brake-" + n, rig.wall);
		TEST_EXPECT(named.outcome == hosted::NameOutcome::Created);
		addresses.push_back(named.id);
		sub.status = "canceled";
		billing::upsert_subscription(*db, sub);
	}
	rig.step();
	hosted::limits_hold::reconciled(*db);
	rig.step();
	for (const int64_t a : addresses) TEST_EXPECT(hosted::find_address(*db, a)->state == AddressState::Allocating);
	// An admin's release of one goes through all the same.
	TEST_EXPECT(hosted::lapse_address(*db, addresses[0], hosted::kLapseAdmin, "released by an admin"));
	rig.step();
	TEST_EXPECT(hosted::find_address(*db, addresses[0])->state == AddressState::Releasing);
	// Two left of three: within the brake, they lapse.
	rig.step();
	for (size_t i = 1; i < addresses.size(); ++i)
		TEST_EXPECT(hosted::find_address(*db, addresses[i])->state != AddressState::Allocating);

	// With the add-on on (as the worker reads it): an allocation whose job is gone
	// runs again, and an exhausted quota's refusal is tried again once its retry_at
	// is reached, never before.
	rig.config.placement.max_addresses = 4;
	rig.make_supervisor();
	const int64_t owner = make_player(*db, "retry_owner", 0);
	subscribe(*db, owner, 1, "active", "sub_retry_tier");
	subscribe_machines(*db, owner, 1, "sub_retry_machine");
	billing::SubscriptionRow sub;
	sub.stripe_subscription_id = "sub_retry_address";
	sub.stripe_customer_id = "cus_retry";
	sub.user_id = owner;
	sub.status = "active";
	sub.item = billing::ItemKind::StaticAddress;
	sub.price_id = "price_address";
	sub.current_period_end = kWall + 30 * 24 * 60 * 60;
	billing::upsert_subscription(*db, sub);
	const auto named = hosted::name_subscription_address(*db, owner, "retry-me", rig.wall);
	TEST_EXPECT(named.outcome == hosted::NameOutcome::Created);
	const std::string key = std::string(hosted::kJobAddressAllocate) + ":" + std::to_string(named.id);
	TEST_EXPECT(!jobs::job_pending(*db, key));
	rig.step();
	TEST_EXPECT(jobs::job_pending(*db, key));
	db->exec("UPDATE jobs SET state = 'failed', finished_at = now() WHERE dedupe_key = $1;", { BindValue(key) });
	TEST_EXPECT(hosted::fail_address(*db, named.id, "AddressLimitExceeded", hosted::address_retry_delay_s(0)));
	rig.step();
	TEST_EXPECT(hosted::find_address(*db, named.id)->state == AddressState::Failed && !jobs::job_pending(*db, key));
	db->exec("UPDATE hosted_addresses SET retry_at = now() - interval '1 second' WHERE id = $1;",
			{ BindValue(named.id) });
	rig.step();
	const auto again = hosted::find_address(*db, named.id);
	TEST_EXPECT(again->state == AddressState::Allocating && again->retries == 1 && again->reason.empty() &&
			jobs::job_pending(*db, key));
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

	// A container that never started because its data could not be fetched (the
	// agent's reason): the data's cause, not the server's. No restart counted; its
	// event says what happened without the agent's line, which its start keeps (an
	// admin's to read); placed again only once the backoff has passed (its
	// machine's agent then syncs without its trees and fetches them afresh), as
	// often as it takes.
	const std::string unfetched_line = "its data could not be fetched: aws s3 sync exited 1: NoSuchKey";
	const int64_t unfetched = rig.request("fetch_fails");
	TEST_EXPECT(rig.walk(unfetched, State::Booting));
	for (int64_t generation = 1; generation <= 2; ++generation) {
		rig.agent_of(unfetched)->fail_start(rig.container(unfetched), unfetched_line);
		rig.sync_all();
		rig.tick();
		const auto row = rig.server(unfetched);
		TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && !row->machine_ref);
		TEST_EXPECT(last_detail(rig.db, unfetched) ==
				"its data could not be fetched; placed again (no restart counted)");
		const auto starts = hosted::list_starts(rig.db, unfetched, 10);
		TEST_EXPECT(starts.size() == static_cast<size_t>(generation) &&
				starts[0].generation == generation &&
				starts[0].cause == hosted::StartCause::DataUnavailable &&
				starts[0].exit_code == 125 && starts[0].reason == unfetched_line);
		if (generation == 2)
			break;
		rig.step(); // the backoff holds the placement
		TEST_EXPECT(rig.state(unfetched) == State::Provisioning);
		rig.advance(rig.config.restart_backoff_ms);
		rig.step();
		TEST_EXPECT(rig.state(unfetched) == State::Booting &&
				rig.server(unfetched)->generation == 2 && rig.server(unfetched)->restarts == 0);
	}
	TEST_EXPECT(hosted::request_stop(rig.db, unfetched, "stopped by its owner"));
	rig.tick();
	TEST_EXPECT(rig.state(unfetched) == State::Stopped);

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
	// Each run a start of its own, each ended by its crash: its code, its OOM flag.
	{
		const auto starts = hosted::list_starts(rig.db, id, 10);
		TEST_EXPECT(starts.size() == 2 && starts[0].generation == 2 && starts[0].cause == hosted::StartCause::Crash &&
				starts[0].exit_code == 137 && starts[0].oom && starts[1].generation == 1 &&
				starts[1].cause == hosted::StartCause::Crash && starts[1].exit_code == 3 && !starts[1].oom &&
				!starts[1].running_at.empty());
	}
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
	{
		const auto starts = hosted::list_starts(rig.db, id, 10);
		TEST_EXPECT(starts.size() == 3 && starts[0].cause == hosted::StartCause::Crash && starts[0].exit_code == 125 &&
				starts[0].running_at.empty());
	}
	return 0;
}

// The age of the supervisor's last good pass (the fleet leader's heartbeat's
// fleet_pass_age_s): none before it starts, each pass that runs whole moves it
// on, and a pass its guard skips (a fleet process that no longer leads) does
// not.
int test_last_good_pass() {
	Rig rig("hosted_last_good_pass");
	TEST_EXPECT(rig.sup->last_good_pass_ms() == 0);
	rig.tick();
	TEST_EXPECT(rig.sup->last_good_pass_ms() == rig.now);
	rig.advance(5000);
	rig.tick();
	TEST_EXPECT(rig.sup->last_good_pass_ms() == 6000);
	rig.sup->set_pass_guard([](opennova::db::Database &) { return false; });
	rig.advance(5000);
	rig.tick();
	TEST_EXPECT(rig.sup->last_good_pass_ms() == 6000);
	return 0;
}

// A machine launch the cloud refuses is the machine's cause, never the server's:
// no restart counted, however many fail. The driver's own text goes to the log,
// not to the event the owner reads (written once per wait); the machine's row
// fails (its launch job's), the next pass counts it for the heartbeat, and no
// launch of that kind follows within the launch backoff, which doubles with
// each refusal in a row to its cap (the rig's 5, 10, 20, 20 s); the server is
// placed on the next machine that launches, whose boot ends the backoff. The
// cloud's error code (an AWS CLI's text) names the machine's end and both events.
int test_launch_failure() {
	Rig rig("hosted_launch_failure");
	const int64_t id = rig.request("launch_refused");
	TEST_EXPECT(id > 0);
	rig.tick(); // -> provisioning
	rig.fake.fail_next_launch = true;
	rig.fake.launch_error = "aws ec2 run-instances exited with code 254: An error occurred "
							"(VcpuLimitExceeded) when calling the RunInstances operation: You have "
							"requested more vCPU capacity than your current vCPU limit of 8 allows";
	rig.tick(); // the launch, which the cloud refuses
	rig.tick(); // the refusal counted
	auto row = rig.server(id);
	TEST_EXPECT(rig.fake.launches == 1);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0 && !row->machine_ref);
	const std::string detail = last_detail(rig.db, id);
	TEST_EXPECT(contains(detail, "the launch of its machine failed (VcpuLimitExceeded); it waits for the next (no "
								 "restart counted)") &&
			!contains(detail, "vCPU limit of 8"));
	TEST_EXPECT(rig.sup->launch_failures() == 1);
	const auto machines = hosted::list_machines(rig.db, 10);
	TEST_EXPECT(machines.size() == 1 && machines[0].state == hosted::FleetMachineState::Failed &&
			machines[0].end_reason == "the launch failed (VcpuLimitExceeded)" &&
			machines[0].kind == hosted::MachineKind::Shared);
	TEST_EXPECT(!hosted::list_machine_events(rig.db, machines[0].id, 10).empty() &&
			hosted::list_machine_events(rig.db, machines[0].id, 10).back().detail ==
					"the launch failed (VcpuLimitExceeded)");
	rig.fake.launch_error = "the fake refused the launch";
	// Within the launch backoff (5 s) nothing launches.
	rig.advance(rig.config.launch_backoff_ms - 1);
	rig.tick();
	TEST_EXPECT(rig.fake.launches == 1 && rig.state(id) == State::Provisioning);
	rig.advance(1);
	// Refused again, and again: each backoff twice the one before, to the cap;
	// still no restart, the event once.
	for (const uint64_t wait : { 10000, 20000, 20000 }) {
		rig.fake.fail_next_launch = true;
		rig.tick(); // the launch, refused
		rig.tick(); // the refusal counted
		const int launches = rig.fake.launches;
		rig.advance(wait - 1);
		rig.tick();
		TEST_EXPECT(rig.fake.launches == launches); // held
		rig.advance(1);
	}
	row = rig.server(id);
	TEST_EXPECT(rig.fake.launches == 4 && rig.sup->launch_failures() == 4);
	TEST_EXPECT(hosted::launch_backoff(rig.db, hosted::MachineKind::Shared).failures == 4);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0);
	TEST_EXPECT(transitions(rig.db, id) ==
			(std::vector<std::string>{
					">requested", "requested>provisioning", "provisioning>provisioning" }));
	rig.tick(); // the launch
	rig.tick(); // the placement
	TEST_EXPECT(rig.fake.launches == 5 && rig.state(id) == State::Booting);
	TEST_EXPECT(rig.walk(id, State::Listing) && rig.server(id)->restarts == 0);
	// Its machine booted: the backoff is over.
	const hosted::LaunchBackoff after = hosted::launch_backoff(rig.db, hosted::MachineKind::Shared);
	TEST_EXPECT(after.failures == 0 && after.not_before_ms == 0);
	return 0;
}

// A refusal that lands while a pass runs (the launch job's worker fails the
// machine's row after the pass counted the refusals, before it places the
// server the machine was for): counted before the server launches again, so
// its backoff holds that launch too, never one more launch inside it.
int test_launch_refused_mid_pass() {
	Rig rig("hosted_refused_mid_pass");
	Database *db = &rig.db;
	const int64_t a = rig.request("mid_pass_a");
	const std::string container = rig.to_running(a, 7401);
	TEST_EXPECT(!container.empty());
	const int64_t owner = make_player(*db, "mid_pass_owner", 0);
	subscribe(*db, owner, 1, "active", "sub_mid_pass");
	subscribe_machines(*db, owner, 1, "sub_mid_pass_machine");
	const int64_t b = rig.request_as(owner, rig.settings(), true);
	TEST_EXPECT(b > a);
	// Passes alone (no job runs): b's own machine, its row and its launch job.
	rig.sup->tick(rig.now);
	rig.sup->tick(rig.now);
	int64_t own = 0;
	for (const auto &m : hosted::list_live_machines(*db)) {
		if (m.kind == hosted::MachineKind::Own) own = m.id;
	}
	TEST_EXPECT(own > 0 && rig.machine(own)->state == hosted::FleetMachineState::Launching);
	// a crashes; it is placed again once its restart's backoff is over.
	rig.agent_of(a)->exit_with(container, 0);
	rig.sync_all();
	rig.sup->tick(rig.now);
	TEST_EXPECT(rig.state(a) == State::Provisioning);
	rig.advance(rig.config.restart_backoff_ms);
	// The pass that places a again: as it does, the worker's refusal of b's
	// machine lands; b, placed after a, launches nothing.
	rig.fake.on_slot_plan = [db, own] { hosted::fail_refused_launch(*db, own, "the launch failed"); };
	rig.sup->tick(rig.now);
	rig.fake.on_slot_plan = nullptr;
	TEST_EXPECT(rig.state(a) == State::Booting);
	TEST_EXPECT(rig.sup->launch_failures() == 1);
	TEST_EXPECT(hosted::launch_not_before_ms(*db, hosted::MachineKind::Own) ==
			static_cast<int64_t>(rig.now + rig.config.launch_backoff_ms));
	int own_rows = 0;
	for (const auto &m : hosted::list_machines(*db, 10)) own_rows += m.kind == hosted::MachineKind::Own ? 1 : 0;
	TEST_EXPECT(own_rows == 1 && rig.state(b) == State::Provisioning);
	TEST_EXPECT(has_event(*db, b, "the launch of its machine failed; it waits for the next (no restart counted)"));
	// Past the backoff, its next machine.
	rig.advance(rig.config.launch_backoff_ms);
	TEST_EXPECT(rig.walk(b, State::Listing));
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
	return 0;
}

// A running server's listing gone, as a lobby's replacement takes its listings
// (the staging rehearsal of 2026-10-17 failed both servers listed on the old
// lobby 55 s after the swap). Its checks wait listing_lost_ms, twice the 240 s
// past which its lister's NWU session drops the dead lobby, which it then
// reconnects to alone; a check that finds the listing again starts the clock
// over, a lost machine holds it; gone that long, the server is placed again,
// no restart counted, even with none left, and runs again.
int test_listing_lost() {
	Rig rig("hosted_listing_gone");
	TEST_EXPECT(rig.config.listing_lost_ms == 2 * 240 * 1000);
	const int64_t id = rig.request("listing_gone");
	TEST_EXPECT(!rig.to_running(id, 8001).empty());
	// Its restarts used up first (two crashes), so a counted restart would fail it.
	for (int crash = 1; crash <= rig.config.restart_limit; ++crash) {
		rig.agent_of(id)->exit_with(rig.container(id), 1);
		rig.sync_all();
		rig.tick();
		TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == crash);
		hostdb::remove_host_by_rid(rig.db, 8000 + static_cast<uint32_t>(crash));
		TEST_EXPECT(!rig.back_to_running(id, 8001 + static_cast<uint32_t>(crash)).empty());
	}
	const uint32_t rid = 8001 + static_cast<uint32_t>(rig.config.restart_limit);
	const std::string container = rig.container(id);

	// Gone from the browser for less than the window: every check finds it
	// running, nothing counted.
	hostdb::remove_host_by_rid(rig.db, rid);
	const uint64_t ran_at = rig.now;
	while (rig.now + 60 * 1000 < ran_at + rig.config.listing_lost_ms) {
		rig.advance(60 * 1000);
		rig.step();
		TEST_EXPECT(rig.state(id) == State::Running && rig.container(id) == container);
	}
	TEST_EXPECT(rig.server(id)->restarts == rig.config.restart_limit);
	// Back in the browser (its key still claimed for the RID): the clock starts over.
	{
		Database *db = &rig.db;
		hostdb::HostRow row;
		row.rid = rid;
		row.gsid = "GSID-" + std::to_string(rid);
		row.game = "jop_2_consumer";
		row.server_name = "Back";
		row.host_ip = row.peer_ip = "198.51.100.1";
		row.host_port = row.peer_port = 32768;
		row.host_user_id = rig.server(id)->service_user_id;
		hostdb::upsert_host(*db, row);
	}
	rig.advance(rig.config.health_interval_ms);
	rig.step();
	hostdb::remove_host_by_rid(rig.db, rid);
	const uint64_t seen_at = rig.now;

	// Unlisted most of the window, then its machine lost for 2 min (short of
	// its end, lost_end_ms on) past the window's end: the listing's clock waits
	// while the machine is lost, and starts over once its agent reports again.
	rig.advance(rig.config.listing_lost_ms - 2 * 60 * 1000);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Running);
	const int64_t machine = rig.machine_of(id);
	rig.silence(machine);
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Lost);
	rig.advance(2 * 60 * 1000);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Running &&
			rig.machine(machine)->state == hosted::FleetMachineState::Lost);
	rig.muted.erase(machine);
	rig.step();
	rig.step();
	TEST_EXPECT(rig.machine(machine)->state == hosted::FleetMachineState::Ready);
	TEST_EXPECT(rig.now - seen_at >= rig.config.listing_lost_ms);
	TEST_EXPECT(rig.state(id) == State::Running);
	rig.advance(rig.config.listing_lost_ms - rig.config.health_interval_ms);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Running && rig.container(id) == container);

	// Gone the whole window: placed again with no restart counted, its
	// container no longer wanted; it starts again, lists and runs.
	rig.advance(rig.config.health_interval_ms);
	rig.step();
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == rig.config.restart_limit &&
			!row->machine_ref);
	TEST_EXPECT(contains(last_detail(rig.db, id),
			"no listing for 480 s (its NovaWorld session reconnects only to the lobby it listed "
			"on); placed again (no restart counted)"));
	TEST_EXPECT(rig.agents.at(machine).find(container) == nullptr);
	const std::string again = rig.to_running(id, rid + 1);
	row = rig.server(id);
	TEST_EXPECT(!again.empty() && again != container && row->state == State::Running &&
			row->restarts == rig.config.restart_limit);
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
	// The cloud says it is gone (a describe's answer, on its row): it ends at the
	// next pass, and the server is placed again.
	rig.fake.vanish(cloud);
	rig.step();
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

// No lobby live (a lobby outage; a deployment's first release, whose fleet program
// rolls before any lobby: prod's cutover of 2026-10-17 failed both restored
// servers, their restarts used up, when each opennova-serve gave up on a gate
// that handed it no lobby): nothing is placed and no machine launched, an exit
// counts no restart, the listing's clocks wait; once a lobby is live the
// waiting server starts, and an exit counts again.
int test_waits_for_lobby() {
	Rig rig("hosted_waits_for_lobby");
	rig.lobby_down();
	const int64_t id = rig.request("lobby_wait");
	for (int i = 0; i < 5; ++i) {
		rig.advance(1000);
		rig.step();
	}
	auto row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && !row->machine_ref && row->restarts == 0);
	TEST_EXPECT(rig.fake.launches == 0 && hosted::list_live_machines(rig.db).empty());
	// A lobby takes clients: the server starts.
	rig.lobby_up();
	TEST_EXPECT(!rig.to_running(id, 8201).empty());
	TEST_EXPECT(rig.fake.launches == 1 && rig.server(id)->restarts == 0);
	const int64_t machine = rig.machine_of(id);
	// Its restarts used up (two crashes while a lobby is live), so a counted
	// restart would fail it.
	for (int crash = 1; crash <= rig.config.restart_limit; ++crash) {
		rig.agent_of(id)->exit_with(rig.container(id), 1);
		rig.sync_all();
		rig.tick();
		TEST_EXPECT(rig.state(id) == State::Provisioning && rig.server(id)->restarts == crash);
		hostdb::remove_host_by_rid(rig.db, 8200 + static_cast<uint32_t>(crash));
		TEST_EXPECT(!rig.back_to_running(id, 8201 + static_cast<uint32_t>(crash)).empty());
	}
	uint32_t rid = 8201 + static_cast<uint32_t>(rig.config.restart_limit);

	// The lobby goes, its listings with it: the running server's listing clock
	// waits however long (its lister reconnects to the lobby address), and it
	// keeps its container.
	rig.lobby_down();
	hostdb::remove_host_by_rid(rig.db, rid);
	const std::string container = rig.container(id);
	const uint64_t down_at = rig.now;
	while (rig.now < down_at + 2 * rig.config.listing_lost_ms) {
		rig.advance(60 * 1000);
		rig.step();
		TEST_EXPECT(rig.state(id) == State::Running && rig.container(id) == container);
	}
	// Its exit: placed again with no restart counted (though none is left), and
	// it waits unplaced.
	rig.agent_of(id)->exit_with(container, 1);
	rig.sync_all();
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == rig.config.restart_limit &&
			!row->machine_ref);
	TEST_EXPECT(contains(last_detail(rig.db, id),
			"exited with code 1 while no lobby instance was live; placed again (no restart "
			"counted)"));
	for (int i = 0; i < 5; ++i) {
		rig.advance(rig.config.restart_backoff_ms);
		rig.step();
	}
	TEST_EXPECT(rig.state(id) == State::Provisioning && !rig.server(id)->machine_ref);

	// A lobby back: it starts on its machine. Gone again while the server waits
	// for its listing: the listing's clock waits, and its exit costs no restart.
	rig.lobby_up();
	TEST_EXPECT(rig.walk(id, State::Listing) && rig.machine_of(id) == machine);
	rig.lobby_down();
	rig.advance(2 * rig.config.listing_timeout_ms);
	rig.step();
	TEST_EXPECT(rig.state(id) == State::Listing);
	rig.agent_of(id)->exit_with(rig.container(id), 1);
	rig.sync_all();
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == rig.config.restart_limit);

	// Live again: it starts and runs, and an exit counts once more: with no
	// restart left, it fails.
	rig.lobby_up();
	TEST_EXPECT(!rig.to_running(id, ++rid).empty());
	TEST_EXPECT(rig.machine_of(id) == machine && rig.fake.launches == 1);
	rig.agent_of(id)->exit_with(rig.container(id), 1);
	rig.sync_all();
	rig.tick();
	row = rig.server(id);
	TEST_EXPECT(row->state == State::Failed && contains(row->failure_reason, "no restart left"));
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
	TEST_EXPECT(nws::billing::set_comp_grants(rig.db, *rig.server(withdrawn)->owner_user_id, { 0, 0 }));
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
	TEST_EXPECT(nws::billing::set_comp_grants(rig.db, *rig.server(crashed)->owner_user_id, { 0, 0 }));
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
	TEST_EXPECT(nws::billing::set_comp_grants(rig.db, owner, { 1, 0 }));
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
// (PastDue); the add-on lowered moves the newest on own machines to a shared
// machine; a canceled tier stops them all; a stopping server counts toward
// neither.
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
	TEST_EXPECT(!hosted::limits_hold::held(rig.db));

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
	TEST_EXPECT(hosted::request_server(*db, ben, rig.settings(), rig.offered(), rig.config.cap, rig.wall)
						.outcome == hosted::RequestOutcome::PastDue);
	// Canceled: they stop.
	subscribe(*db, ben, 2, "canceled", "sub_lim_ben");
	rig.step();
	TEST_EXPECT(stopped_with(*db, b1, "hosting entitlement allows 0 hosted servers"));
	TEST_EXPECT(stopped_with(*db, b2, "hosting entitlement allows 0 hosted servers"));

	// The add-on: two machines, then one; the newest on an own machine, still
	// waiting for its place, goes to a shared machine instead (the fleet has
	// room), and the others stay where they are.
	const int64_t cy = make_player(*db, "lim_cy", 0);
	subscribe(*db, cy, 3, "active", "sub_lim_cy");
	subscribe_machines(*db, cy, 2, "sub_lim_cy_machines");
	const auto own = [&](bool on_own) { return rig.request_as(cy, rig.settings(), on_own); };
	const int64_t c1 = own(true);
	const int64_t c2 = own(false);
	const int64_t c3 = own(true);
	TEST_EXPECT(c1 > 0 && c2 > c1 && c3 > c2);
	TEST_EXPECT(own(true) == 0); // a fourth: every server the tier allows is live
	rig.step();
	subscribe_machines(*db, cy, 1, "sub_lim_cy_machines");
	rig.step();
	TEST_EXPECT(has_event(*db, c3, "moved to a shared machine: its owner has 1 own machine now"));
	TEST_EXPECT(hosted::is_live(rig.state(c3)) && rig.state(c3) != State::Stopping && !rig.server(c3)->own_machine);
	TEST_EXPECT(hosted::is_live(rig.state(c1)) && rig.state(c1) != State::Stopping && rig.server(c1)->own_machine);
	TEST_EXPECT(hosted::is_live(rig.state(c2)) && rig.state(c2) != State::Stopping);
	TEST_EXPECT(rig.walk(c3, State::Listing));
	TEST_EXPECT(rig.machine(rig.machine_of(c3))->kind == hosted::MachineKind::Shared);
	return 0;
}

// Own machines cut while their servers run (an admin's grant of two, then
// one): the newest server on an own machine moves to a shared machine, which
// the fleet has room for (its match ends: its start ends `limits`, it is placed
// again with its own-machine choice cleared and no restart counted, its event
// saying why, its static address left to the account, and its own machine ends
// after it), and starts again there; the older one stays on its own machine,
// the shared one where it runs.
int test_own_machines_cut() {
	Rig rig("hosted_own_cut");
	rig.config.cap = 10;
	rig.make_supervisor();
	Database *db = &rig.db;
	const int64_t owner = make_player(*db, "own_cut_owner", 0);
	TEST_EXPECT(nws::billing::set_comp_grants(*db, owner, { 3, 2 }));
	// The newer one runs at an admin's comp address.
	const auto named = hosted::add_comp_address(*db, owner, "cut-place", rig.wall);
	TEST_EXPECT(named.outcome == hosted::NameOutcome::Created);
	const int64_t address = named.id;
	const std::string ip = "203.0.113.60";
	TEST_EXPECT(hosted::record_allocation(*db, address, "eipalloc-cut", ip) &&
			hosted::record_dns_upsert(*db, address, "cut-place", "c1"));
	const int64_t older = rig.request_as(owner, rig.settings(), true);
	const auto requested = hosted::request_server(*db, owner, rig.settings(), rig.offered(), rig.config.cap,
			rig.wall, true, &rig.config.placement, address);
	TEST_EXPECT(requested.outcome == hosted::RequestOutcome::Created);
	const int64_t newer = requested.id;
	const int64_t shared = rig.request_as(owner);
	TEST_EXPECT(older > 0 && newer > older && shared > newer);
	// Its own machine carries the address once a describe brings it.
	for (int i = 0; i < 6; ++i) rig.step();
	const int64_t newer_machine = rig.machine_of(newer);
	TEST_EXPECT(newer_machine > 0 && rig.machine(newer_machine)->address_id == address);
	const std::string newer_cloud = rig.machine(newer_machine)->cloud_id;
	rig.fake.machines.at(newer_cloud).public_address = ip;
	TEST_EXPECT(!rig.to_running(older, 6101).empty() && !rig.to_running(newer, 6102).empty() &&
			!rig.to_running(shared, 6103).empty());
	TEST_EXPECT(rig.fake.launches == 3);
	const int64_t older_machine = rig.machine_of(older);
	const int64_t shared_machine = rig.machine_of(shared);
	TEST_EXPECT(rig.machine(newer_machine)->kind == hosted::MachineKind::Own &&
			rig.machine(newer_machine)->public_address == ip &&
			rig.machine(shared_machine)->kind == hosted::MachineKind::Shared);
	const int64_t generation = rig.server(newer)->generation;

	TEST_EXPECT(nws::billing::set_comp_grants(*db, owner, { 3, 1 }));
	rig.step();
	auto row = rig.server(newer);
	TEST_EXPECT(row && !row->own_machine && row->restarts == 0 && hosted::is_live(row->state) &&
			row->state != State::Stopping);
	// Placed again in the same pass, on the shared machine's next slot.
	TEST_EXPECT(rig.machine_of(newer) == shared_machine && row->generation == generation + 1);
	TEST_EXPECT(has_event(*db, newer,
			"moved to a shared machine: its owner has 1 own machine now; placed again (no restart counted)"));
	TEST_EXPECT(!stopped_with(*db, newer, "own machine"));
	// Its address stays the account's, ready, and no live server's (only an own
	// machine carries one).
	TEST_EXPECT(!row->address_id && !hosted::server_of_address(*db, address) &&
			hosted::find_address(*db, address)->state == AddressState::Ready);
	// Its own machine ended after it.
	TEST_EXPECT(rig.machine(newer_machine)->state == hosted::FleetMachineState::Ended &&
			rig.machine(newer_machine)->end_reason == "its server moved to a shared machine");
	TEST_EXPECT(rig.fake.was_terminated(newer_cloud));
	// The others run on where they ran.
	TEST_EXPECT(rig.state(older) == State::Running && rig.machine_of(older) == older_machine &&
			rig.server(older)->own_machine);
	TEST_EXPECT(rig.state(shared) == State::Running && rig.machine_of(shared) == shared_machine);
	TEST_EXPECT(rig.machine(older_machine)->state == hosted::FleetMachineState::Ready);
	// Its old listing went with its match; it lists again from the shared machine.
	hostdb::remove_host_by_rid(*db, 6102);
	TEST_EXPECT(!rig.to_running(newer, 6104).empty());
	TEST_EXPECT(rig.machine_of(newer) == shared_machine && rig.server(newer)->restarts == 0);
	TEST_EXPECT(rig.fake.launches == 3); // no machine launched for the move
	{
		// A new start on the shared machine, the one before it on its own machine
		// ended for its owner's limits.
		const auto starts = hosted::list_starts(*db, newer, 10);
		TEST_EXPECT(starts.size() == 2 && starts[0].generation == generation + 1 &&
				starts[0].machine_id == shared_machine && starts[0].ended_at.empty() &&
				!starts[0].running_at.empty() && starts[1].machine_id == newer_machine &&
				!starts[1].ended_at.empty() && starts[1].cause == hosted::StartCause::Limits);
	}
	// Nothing more moves or stops.
	rig.step();
	TEST_EXPECT(rig.state(older) == State::Running && rig.state(newer) == State::Running &&
			rig.state(shared) == State::Running && !hosted::limits_hold::held(*db));
	return 0;
}

// Own machines cut with room for one of their servers alone: the newest moves
// to the shared machine's last slot, the older one, which the fleet then has no
// shared room for, stops as before, its event saying so and its start ending
// `limits`; the shared server runs on.
int test_own_machines_cut_no_room() {
	Rig rig("hosted_own_cut_full");
	rig.config.cap = 10;
	rig.config.placement.shared_slots = 2;
	rig.config.placement.max_shared = 1;
	rig.make_supervisor();
	Database *db = &rig.db;
	const int64_t owner = make_player(*db, "own_full_owner", 0);
	TEST_EXPECT(nws::billing::set_comp_grants(*db, owner, { 3, 2 }));
	const int64_t older = rig.request_as(owner, rig.settings(), true);
	const int64_t newer = rig.request_as(owner, rig.settings(), true);
	const int64_t shared = rig.request_as(owner);
	TEST_EXPECT(older > 0 && newer > older && shared > newer);
	TEST_EXPECT(!rig.to_running(older, 6201).empty() && !rig.to_running(newer, 6202).empty() &&
			!rig.to_running(shared, 6203).empty());
	const int64_t older_machine = rig.machine_of(older);
	const int64_t shared_machine = rig.machine_of(shared);
	TEST_EXPECT(hosted::fleet_has_room(*db, rig.config.placement, false)); // the shared machine's second slot

	TEST_EXPECT(nws::billing::set_comp_grants(*db, owner, { 3, 0 }));
	rig.step();
	TEST_EXPECT(!rig.server(newer)->own_machine && rig.machine_of(newer) == shared_machine);
	TEST_EXPECT(has_event(*db, newer, "moved to a shared machine: its owner has 0 own machines now"));
	TEST_EXPECT(stopped_with(*db, older, "its owner has 0 own machines now and the fleet has no shared room"));
	TEST_EXPECT(rig.server(older)->own_machine && rig.machine_of(older) == older_machine);
	TEST_EXPECT(hosted::list_starts(*db, older, 1)[0].cause == hosted::StartCause::Limits);
	TEST_EXPECT(rig.state(shared) == State::Running);
	TEST_EXPECT(!hosted::fleet_has_room(*db, rig.config.placement, false));
	// The stop ends as any (its listing gone), and its own machine with it.
	hostdb::remove_host_by_rid(*db, 6201);
	rig.step(); // the push
	rig.step(); // stopped
	TEST_EXPECT(rig.state(older) == State::Stopped);
	rig.step();
	TEST_EXPECT(rig.machine(older_machine)->state == hosted::FleetMachineState::Ended);
	TEST_EXPECT(rig.state(shared) == State::Running && hosted::is_live(rig.state(newer)));
	return 0;
}

// The provisioning re-check (on_provisioning): own machines cut after the pass's
// limits read them and before an own-machine server waiting for its place is
// placed (an admin's cut landing mid-pass, played here at the pass's launch for
// the owner's older shared server): the server goes to a shared machine instead
// of its own, the shared machine launched for the older one taking both.
int test_own_cut_at_provisioning() {
	Rig rig("hosted_own_cut_provisioning");
	Database *db = &rig.db;
	const int64_t owner = make_player(*db, "own_prov_owner", 0);
	TEST_EXPECT(nws::billing::set_comp_grants(*db, owner, { 2, 1 }));
	const int64_t older = rig.request_as(owner);
	const int64_t own = rig.request_as(owner, rig.settings(), true);
	TEST_EXPECT(older > 0 && own > older);
	rig.tick(); // both -> provisioning
	TEST_EXPECT(rig.state(older) == State::Provisioning && rig.state(own) == State::Provisioning);
	// The pass's next guard read past its first (the older server's launch, after
	// the limits ran) cuts the own machine.
	int guards = 0;
	bool cut = false;
	rig.sup->set_pass_guard([&](Database &d) {
		if (++guards == 2) cut = nws::billing::set_comp_grants(d, owner, { 2, 0 });
		return true;
	});
	rig.tick();
	TEST_EXPECT(cut);
	auto row = rig.server(own);
	TEST_EXPECT(row && row->state == State::Provisioning && !row->own_machine && !row->machine_ref);
	TEST_EXPECT(has_event(*db, own, "moved to a shared machine: its owner has 0 own machines now"));
	rig.sup->set_pass_guard(nullptr);
	TEST_EXPECT(rig.walk(own, State::Listing) && rig.walk(older, State::Listing));
	TEST_EXPECT(rig.machine_of(own) == rig.machine_of(older) &&
			rig.machine(rig.machine_of(own))->kind == hosted::MachineKind::Shared);
	TEST_EXPECT(rig.fake.launches == 1); // no own machine launched
	return 0;
}

// The owners' limits' hold, its row (hosted/model/limits_hold.h) as the tests read
// and move it.
struct HoldRow {
	Database &db;
	hosted::LimitsHoldView view() { return hosted::limits_hold::view(db); }
	bool held() { return hosted::limits_hold::held(db); }
	void reconciled() { hosted::limits_hold::reconciled(db); }
	void force_next_pass() { hosted::limits_hold::force_next_pass(db); }
	bool take_force() { return hosted::limits_hold::take_force(db); }
	bool clear() { return hosted::limits_hold::clear(db); }
};

// The brake on the brake: a pass that would stop the servers of more owners
// than max(2, half the owners) stops none, logs, and holds (no limit stops,
// whatever their count, until it releases); cleared while the cause stands, it
// holds again. A clean reconcile with Stripe (limits_hold::reconciled) confirms
// the cache: the next pass finds some of the same owners still past their
// entitlements, releases, and stops theirs alone.
int test_mass_stop_hold() {
	Rig rig("hosted_hold");
	HoldRow hold{ rig.db };
	Database *db = &rig.db;
	const int64_t a = rig.request("hold_a");
	const int64_t b = rig.request("hold_b");
	const int64_t c = rig.request("hold_c");
	TEST_EXPECT(a > 0 && b > 0 && c > 0);
	rig.tick();
	for (const int64_t id : { a, b, c })
		TEST_EXPECT(nws::billing::set_comp_grants(*db, *rig.server(id)->owner_user_id, { 0, 0 }));
	rig.tick();
	TEST_EXPECT(hold.held() && !hold.view().reason.empty());
	TEST_EXPECT(hold.view().owners == 3 && hold.view().servers == 3);
	for (const int64_t id : { a, b, c }) TEST_EXPECT(rig.state(id) != State::Stopping && hosted::is_live(rig.state(id)));
	// Cleared while all three still lapse: held again.
	TEST_EXPECT(hold.clear());
	rig.tick();
	TEST_EXPECT(hold.held());
	for (const int64_t id : { a, b, c }) TEST_EXPECT(rig.state(id) != State::Stopping);
	// Two restored: one owner past, below the brake, and still held (no
	// reconcile has confirmed the cache since).
	for (const int64_t id : { a, b })
		TEST_EXPECT(nws::billing::set_comp_grants(*db, *rig.server(id)->owner_user_id, { 1, 0 }));
	rig.tick();
	TEST_EXPECT(hold.held() && rig.state(c) != State::Stopping);
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
	HoldRow hold{ rig.db };
	Database *db = &rig.db;
	const auto lapse = [&](const std::vector<int64_t> &ids, int comp) {
		bool ok = true;
		for (const int64_t id : ids) ok = nws::billing::set_comp_grants(*db, *rig.server(id)->owner_user_id, { comp, 0 }) && ok;
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
	// The reconciliation's sweep (a job) finds the two machines gone; its first
	// pass ends them.
	rig.reconcile();
	rig.tick();

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

	// It is placed again at once (no backoff: no crash), on the machine that
	// pass launched for it.
	rig.step();
	TEST_EXPECT(rig.fake.launches == 4);
	TEST_EXPECT(rig.walk(lost, State::Booting));
	TEST_EXPECT(hosted::request_stop(rig.db, lost, "done"));

	// The resumed server's checks wait out its grace, unlisted and uncounted,
	// and its listing comes back within it; past the grace a check finds it.
	for (int i = 0; i < 2; ++i) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
	}
	TEST_EXPECT(rig.state(kept) == State::Running);
	TEST_EXPECT(list_as(rig.db, *rig.server(kept)->service_user_id, 9104));
	rig.advance(rig.config.relist_grace_ms);
	rig.step();
	TEST_EXPECT(rig.state(kept) == State::Running);
	TEST_EXPECT(rig.state(lost) == State::Stopped);
	return 0;
}

// The relist grace after the service's restart, on either driver: twice the
// 240 s of receive silence past which a host's lister drops its dead
// NOVAWORLDUDP session and re-hosts (the 2026-10-16 deploy's two servers
// listed again 217 s and 228 s after the restart). A running server its agent
// still reports, unlisted 300 s on, is not failed and lists again; one that
// never lists again is placed again once the grace is out, its listing gone
// listing_lost_ms since the restart (no restart counted).
int test_relist_grace() {
	Rig rig("hosted_relist_grace");
	TEST_EXPECT(rig.config.relist_grace_ms == 2 * 240 * 1000);
	const int64_t alive = rig.request("relist_alive");
	TEST_EXPECT(!rig.to_running(alive, 9201).empty());
	const int64_t dead = rig.request("relist_dead");
	TEST_EXPECT(!rig.to_running(dead, 9202).empty());
	// The service restarts: the boot cleared its previous run's listings.
	for (const uint32_t rid : { 9201u, 9202u })
		hostdb::remove_host_by_rid(rig.db, rid);
	rig.make_supervisor();
	rig.advance(1000);
	rig.sup->reconcile(rig.now);
	const uint64_t restarted = rig.now;

	// 300 s unlisted, their agent reporting both running every pass: no check
	// counts (the listing timeout, 20 s here, is not the grace).
	while (rig.now - restarted < 300 * 1000) {
		rig.advance(rig.config.health_interval_ms);
		rig.step();
	}
	TEST_EXPECT(rig.state(alive) == State::Running && rig.state(dead) == State::Running);
	TEST_EXPECT(rig.running_container(alive) != nullptr && rig.running_container(dead) != nullptr);
	TEST_EXPECT(rig.server(alive)->restarts == 0 && rig.server(dead)->restarts == 0);
	TEST_EXPECT(list_as(rig.db, *rig.server(alive)->service_user_id, 9203));
	// Up to the grace's end the checks still wait.
	rig.advance(restarted + rig.config.relist_grace_ms - rig.config.health_interval_ms - rig.now);
	rig.step();
	TEST_EXPECT(rig.state(alive) == State::Running && rig.state(dead) == State::Running);
	// Past it the checks run: the listed one passes them, and the one that
	// never listed again, unlisted listing_lost_ms since the restart, is placed
	// again with no restart counted.
	TEST_EXPECT(rig.config.listing_lost_ms <= rig.config.relist_grace_ms);
	rig.advance(rig.config.health_interval_ms);
	rig.step();
	TEST_EXPECT(rig.state(alive) == State::Running);
	const auto row = rig.server(dead);
	TEST_EXPECT(row->state == State::Provisioning && row->restarts == 0);
	TEST_EXPECT(contains(last_detail(rig.db, dead), "no listing for 480 s"));
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

// A row per case (tests/common/test_parts.h; service_tests.cmake lists them): each makes its own
// databases, so the cases run side by side.
int main(int argc, char **argv) {
	test_db::init(argc, argv);
	const int failures = test_parts::run(argc, argv, {
		{ "settings_rules", { test_settings_rules } },
		{ "renders", { test_renders } },
		{ "data_sets", { test_data_sets } },
		{ "no_data", { test_no_data } },
		{ "pins_and_updates", { test_pins_and_updates } },
		{ "service_accounts", { test_service_accounts } },
		{ "requests", { test_requests } },
		{ "fleet_room", { test_fleet_room } },
		{ "lifecycle_and_stop", { test_lifecycle_and_stop } },
		{ "two_servers_one_machine", { test_two_servers_one_machine } },
		{ "own_machine", { test_own_machine } },
		{ "own_restart_at_cap", { test_own_restart_at_cap } },
		{ "static_address", { test_static_address } },
		{ "static_address_brake", { test_static_address_brake } },
		{ "stop_paths", { test_stop_paths } },
		{ "exits_and_crash_restarts", { test_exits_and_crash_restarts } },
		{ "last_good_pass", { test_last_good_pass } },
		{ "launch_failure", { test_launch_failure } },
		{ "launch_refused_mid_pass", { test_launch_refused_mid_pass } },
		{ "restart_window", { test_restart_window } },
		{ "timeouts_and_health", { test_timeouts_and_health } },
		{ "listing_lost", { test_listing_lost } },
		{ "machine_lost_replaces", { test_machine_lost_replaces } },
		{ "listing_waits_while_lost", { test_listing_waits_while_lost } },
		{ "waits_for_lobby", { test_waits_for_lobby } },
		{ "owner_changes", { test_owner_changes } },
		{ "limit_at_provisioning", { test_limit_at_provisioning } },
		{ "owner_limits", { test_owner_limits } },
		{ "own_machines_cut", { test_own_machines_cut } },
		{ "own_machines_cut_no_room", { test_own_machines_cut_no_room } },
		{ "own_cut_at_provisioning", { test_own_cut_at_provisioning } },
		{ "mass_stop_hold", { test_mass_stop_hold } },
		{ "hold_releases", { test_hold_releases } },
		{ "reconciliation", { test_reconciliation } },
		{ "relist_grace", { test_relist_grace } },
		{ "console_ring", { test_console_ring } },
		{ "admin_links_follow_running", { test_admin_links_follow_running } },
	});
	if (failures != 0) return 1;
	std::printf("OK: hosted servers' supervisor\n");
	return 0;
}
