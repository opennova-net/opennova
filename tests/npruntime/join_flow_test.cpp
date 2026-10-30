// The front end's join flow (inmatch/join_flow.h), headless over a scripted device: the browse's
// rows stand in for the enumerated sessions' 0x81s, the join screen's answers for the player's.
//   - a target with no flag word enumerates first (the process enumerator drained ahead of the
//     browse), joins the first discovered session, and takes its record, name, game type and
//     expansion; a word carrying any of 0x6038 asks the choice, TeamChoose alone dials;
//   - the choice's commit routes the shared password by the role and dials: the callsign, then the
//     expansion switch with its profile save and reload, then the joiner load; a failed switch
//     saves nothing and still dials;
//   - ABORT_CANCEL, BACK and a refusal's Cancel run state 9 in its order, a NovaWorld join's
//     destroy keeping its transport and its session going back to the NovaWorld menu once;
//   - the launch flags' sessions (the host, the endpoint join with its role, the browse search);
//   - the install refusal rule and the NovaWorld join's refusal note;
//   - the mission start's Host-list columns read the mission start's over the session's.
// GUT's spectator_join_prompt / lan_session / connection_error logic moved here (ADR 0077 R3).
#include <runtime/inmatch/join_flow.h>

#include <net/novaworld/lobby_vars.h>
#include <runtime/inmatch/host_listing_binding.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
namespace inmatch = opennova::inmatch;
using inmatch::DiscoveredSessionRow;
using inmatch::JoinFlowState;
using inmatch::JoinTargetRecord;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

struct Device final : inmatch::JoinFlowDevice {
	std::vector<std::string> log;
	bool browse_starts = true;
	bool choice_shows = true;
	bool refusal_shows = true;
	std::string switch_error;
	uint32_t serial = 0;
	bool browse_has_target = false;
	JoinTargetRecord browse_target;
	uint32_t choice_flags = 0;
	JoinTargetRecord joined;
	int joins = 0;
	inmatch::LaunchHost host;
	bool reenter = false;

	bool begin_browse(const JoinTargetRecord *target, uint32_t s) override {
		log.push_back(target != nullptr ? "browse" : "search");
		serial = s;
		browse_has_target = target != nullptr;
		if (target != nullptr)
			browse_target = *target;
		return browse_starts;
	}
	void adopt_browse() override { log.push_back("adopt"); }
	void end_browse() override { log.push_back("end_browse"); }
	void drain_browse() override { log.push_back("drain_browse"); }
	void drain_enumerator() override { log.push_back("drain_enumerator"); }
	bool show_enumerating() override {
		log.push_back("enumerating");
		return true;
	}
	bool show_choice(uint32_t flags) override {
		log.push_back("choice");
		choice_flags = flags;
		return choice_shows;
	}
	bool show_refusal(const std::string &text) override {
		log.push_back("refusal:" + text);
		return refusal_shows;
	}
	void close_join_screen() override { log.push_back("close"); }
	std::string callsign() override { return "Profile"; }
	std::string switch_expansion(const std::string &expansion) override {
		log.push_back("switch:" + expansion);
		return switch_error;
	}
	void save_profile() override { log.push_back("save_profile"); }
	void reload_profile() override { log.push_back("reload_profile"); }
	void save_config() override { log.push_back("save_config"); }
	void start_join(const JoinTargetRecord &target) override {
		log.push_back("start_join");
		joined = target;
		++joins;
	}
	void start_launch_host(const inmatch::LaunchHost &h) override {
		log.push_back("start_host");
		host = h;
	}
	void destroy_pre_dial_session(bool novaworld) override {
		log.push_back(novaworld ? "destroy:novaworld" : "destroy:lan");
	}
	void cancel_pre_game() override { log.push_back("cancel_pre_game"); }
	void show_main_menu() override { log.push_back("main_menu"); }
	void route_novaworld_session(bool r) override {
		log.push_back(r ? "route:reenter" : "route:stop");
		reenter = r;
	}

	bool logged(const std::string &what) const {
		for (const std::string &line : log)
			if (line == what)
				return true;
		return false;
	}
	// `steps` appear in `log` in this order (others may sit between them).
	bool in_order(const std::vector<std::string> &steps) const {
		size_t at = 0;
		for (const std::string &line : log)
			if (at < steps.size() && line == steps[at])
				++at;
		return at == steps.size();
	}
};

DiscoveredSessionRow row(int port, int64_t flags, const std::string &expansion = "") {
	DiscoveredSessionRow r;
	r.host_ip = "10.0.0.7";
	r.port = port;
	r.server_name = "Host " + std::to_string(port);
	r.game_type = 0x904;
	r.server_flags = flags;
	r.expansion = expansion;
	r.host_key = 0x1234;
	r.password_required = (flags & inmatch::server_flag::kServerPassword) != 0;
	r.client_index = 3;
	return r;
}

JoinTargetRecord endpoint(int port = 32768) {
	JoinTargetRecord t;
	t.host_ip = "10.0.0.7";
	t.port = port;
	return t;
}

// A target with no word enumerates; the first discovered session is the join's, and a plain team
// game (0x4) dials at once with the record, the session's name and type, and the switch to its
// expansion between the profile's save and its reload.
void test_enumerate_then_dial() {
	Device device;
	inmatch::JoinFlow flow(device);
	flow.join(endpoint());
	CHECK(flow.state() == JoinFlowState::Enumerating);
	CHECK(device.in_order({ "drain_enumerator", "browse", "enumerating" }));
	CHECK(device.browse_has_target && device.browse_target.port == 32768);
	// A stale browse's rows, and an empty row set, change nothing.
	flow.browse_rows(device.serial + 1, { row(32768, 0x4) });
	flow.browse_rows(device.serial, {});
	CHECK(flow.state() == JoinFlowState::Enumerating && device.joins == 0);
	// The first discovered session, whatever its port.
	flow.browse_rows(device.serial, { row(32770, 0x4, "jox01"), row(32768, 0x4) });
	CHECK(flow.state() == JoinFlowState::Idle && device.joins == 1);
	const JoinTargetRecord &j = device.joined;
	CHECK(j.port == 32770 && j.discovered_session && j.session_host_key == 0x1234 &&
			j.session_client_index == 3 && j.server_flags == 0x4);
	CHECK(j.server_name == "Host 32770" && j.game_type == 0x904);
	CHECK(j.expansion == "jox01" && j.expansion_known && j.player_name == "Profile");
	CHECK(device.in_order({ "adopt", "end_browse", "close", "switch:jox01", "save_profile",
			"reload_profile", "start_join" }));
	CHECK(!device.logged("choice"));
}

// A failed switch saves nothing and dials anyway (the post-auth reconcile fails it there).
void test_failed_switch_still_dials() {
	Device device;
	device.switch_error = "no such expansion";
	inmatch::JoinFlow flow(device);
	JoinTargetRecord t = inmatch::join_target_from_row(row(32768, 0x4, "nope"));
	t.player_name = "Given";
	flow.join(t);
	CHECK(device.joins == 1 && device.joined.player_name == "Given");
	CHECK(device.logged("switch:nope") && !device.logged("save_profile") &&
			!device.logged("reload_profile"));
}

// A word with any of 0x6038 asks the choice; the commit routes the one shared TEAM_PASSWORD text
// by the role and dials.
void test_choice_commit_routes_the_passwords() {
	using namespace inmatch::server_flag;
	{
		Device device;
		inmatch::JoinFlow flow(device);
		flow.join(inmatch::join_target_from_row(row(32768, kSpectators | kSideAPassword)));
		CHECK(flow.choice_pending() && device.choice_flags == (kSpectators | kSideAPassword));
		flow.commit_choice("", true, "watch");
		CHECK(!flow.choice_pending() && device.joins == 1);
		CHECK(device.joined.join_role == inmatch::JoinRole::Spectator &&
				device.joined.spectator_password == "watch" &&
				device.joined.join_password.empty() && device.joined.role_explicit);
		// A second commit does nothing.
		flow.commit_choice("x", false, "y");
		CHECK(device.joins == 1);
	}
	{
		Device device;
		inmatch::JoinFlow flow(device);
		flow.join(inmatch::join_target_from_row(row(32768, kServerPassword | kSideBPassword)));
		CHECK(flow.choice_pending());
		flow.commit_choice("secret", false, "red");
		CHECK(device.joined.server_password == "secret" && device.joined.join_password == "red" &&
				device.joined.join_role == inmatch::JoinRole::Player);
	}
	// An answered target that owes nothing dials without the choice.
	{
		Device device;
		inmatch::JoinFlow flow(device);
		JoinTargetRecord t = inmatch::join_target_from_row(row(32768, kSpectators));
		t.role_explicit = true;
		flow.join(t);
		CHECK(device.joins == 1 && !device.logged("choice"));
	}
}

// ABORT_CANCEL while enumerating: the browse's records drain, the pre-dial session goes (a LAN
// join's transport with it), the browse and its screen close, the game type resets, the main menu.
void test_enumerate_abort_runs_state_9() {
	Device device;
	inmatch::JoinFlow flow(device);
	flow.join(endpoint());
	device.log.clear();
	flow.cancel();
	CHECK(flow.state() == JoinFlowState::Idle);
	CHECK(device.in_order({ "drain_browse", "destroy:lan", "end_browse", "close", "cancel_pre_game",
			"main_menu", "route:stop" }));
	// A browse that cannot start runs the same tail.
	Device failing;
	failing.browse_starts = false;
	inmatch::JoinFlow flow2(failing);
	flow2.join(endpoint());
	CHECK(flow2.state() == JoinFlowState::Idle && failing.logged("destroy:lan") &&
			!failing.logged("enumerating") && failing.logged("main_menu"));
}

// The choice's BACK drains the process enumerator; a NovaWorld join's destroy keeps its transport
// and its session goes back to the NovaWorld menu, once (g_ReturnToNovaWorldMenu).
void test_novaworld_join_abandon_reenters_once() {
	Device device;
	inmatch::JoinFlow flow(device);
	JoinTargetRecord t = endpoint();
	t.network_type = inmatch::NetworkType::NovaWorld;
	t.player_name = "Char";
	flow.join_from_novaworld(t, 77);
	CHECK(device.in_order({ "save_profile", "save_config", "drain_enumerator", "browse" }));
	flow.browse_rows(device.serial, { row(32768, inmatch::server_flag::kServerPassword) });
	CHECK(flow.choice_pending() &&
			device.browse_target.network_type == inmatch::NetworkType::NovaWorld);
	device.log.clear();
	flow.cancel();
	CHECK(device.in_order({ "drain_enumerator", "destroy:novaworld", "cancel_pre_game", "main_menu",
			"route:reenter" }));
	CHECK(!flow.take_return_to_novaworld());
	// The abandoned join notes no refusal.
	inmatch::ConnectionErrorRecord pv2;
	pv2.connect_error = 7;
	CHECK(!flow.join_failed(&pv2));
}

// A refused LAN row: its text with Cancel only, whose Cancel is state 9 over the LAN screen's
// enumerator; a screen that cannot show it runs the tail at once.
void test_refusal() {
	Device device;
	inmatch::JoinFlow flow(device);
	flow.refuse("CVUNABLETOCREATE");
	CHECK(flow.state() == JoinFlowState::Refused && device.logged("refusal:CVUNABLETOCREATE"));
	flow.cancel();
	CHECK(flow.state() == JoinFlowState::Idle &&
			device.in_order({ "drain_enumerator", "destroy:lan", "main_menu" }));
	Device hidden;
	hidden.refusal_shows = false;
	inmatch::JoinFlow flow2(hidden);
	flow2.refuse("CVUNABLETOCREATE");
	CHECK(flow2.state() == JoinFlowState::Idle && hidden.logged("destroy:lan"));
}

// The launch flags' sessions.
void test_launch() {
	{
		Device device;
		inmatch::JoinFlow flow(device);
		CHECK(!flow.launch(parse_launch_flags({ "game.exe" })));
		CHECK(device.log.empty());
	}
	{
		Device device;
		inmatch::JoinFlow flow(device);
		CHECK(flow.launch(parse_launch_flags(
				{ "--lan-host", "a.bms", "--lan-rotation", "b.bms,c.bms", "--lan-max-players", "8",
						"--lan-gametype", "0x10020", "--lan-port", "40000" })));
		CHECK(device.host.mission == "a.bms" && device.host.rotation.size() == 3 &&
				device.host.rotation[0] == "a.bms" && device.host.rotation[2] == "c.bms");
		CHECK(device.host.max_players == 8 && device.host.game_type == 0x10020 &&
				device.host.port == 40000 && device.host.lan_mode == 0);
		CHECK(!inmatch::launch_host(parse_launch_flags({ "--lan-host", "a.bms" }))
						->rotation.size());
		CHECK(inmatch::launch_host(parse_launch_flags({ "--lan-host", "a.bms" }))->game_type == -1);
	}
	{
		// An endpoint join is enumerated first, with its role answered.
		Device device;
		inmatch::JoinFlow flow(device);
		CHECK(flow.launch(parse_launch_flags(
				{ "--lan-join", "10.0.0.9:40001", "--spectator", "--spectator-password", "pw" })));
		CHECK(flow.state() == JoinFlowState::Enumerating &&
				device.browse_target.host_ip == "10.0.0.9" && device.browse_target.port == 40001 &&
				device.browse_target.join_role == inmatch::JoinRole::Spectator &&
				device.browse_target.spectator_password == "pw" &&
				device.browse_target.role_explicit);
	}
	{
		// The browse search joins the first row on its port, as the LAN screen's join does.
		Device device;
		inmatch::JoinFlow flow(device);
		CHECK(flow.launch(parse_launch_flags({ "--lan-join", "browse:40002" })));
		CHECK(flow.state() == JoinFlowState::LaunchSearch && device.logged("search") &&
				!device.browse_has_target);
		const uint32_t search = device.serial;
		flow.browse_rows(search, { row(40001, 0x4) });
		CHECK(flow.state() == JoinFlowState::LaunchSearch && device.joins == 0);
		flow.browse_rows(search, { row(40001, 0x4), row(40002, 0x4) });
		CHECK(device.joins == 1 && device.joined.port == 40002 && device.joined.discovered_session);
		CHECK(device.in_order({ "search", "adopt", "end_browse", "start_join" }));
	}
	{
		// A search with no row on its port ends.
		Device device;
		inmatch::JoinFlow flow(device);
		flow.launch(parse_launch_flags({ "--lan-join", "browse" }));
		flow.browse_finished(device.serial + 1);
		CHECK(flow.state() == JoinFlowState::LaunchSearch);
		flow.browse_finished(device.serial);
		CHECK(flow.state() == JoinFlowState::Idle && device.joins == 0 &&
				!device.logged("main_menu"));
		CHECK(inmatch::launch_search_row({ row(1, 0), row(2, 0) }, 0) == 0);
		CHECK(inmatch::launch_search_row({ row(1, 0) }, 3) == -1);
	}
}

// NCC007 (PV2), GCC003 (a banned address, JFC 14 / JFP 3), the join build (GDC002..008) and the
// expansion (GDC047 / GDC048) refuse this install; a kick (GDC032) and no connection do not.
void test_refusal_rule_and_note() {
	inmatch::ConnectionErrorRecord r;
	r.connect_error = 7;
	CHECK(inmatch::refuses_this_install(&r));
	r = {};
	r.connect_error = 14;
	r.connect_param = 3;
	CHECK(inmatch::refuses_this_install(&r));
	r.connect_param = 4;
	CHECK(!inmatch::refuses_this_install(&r));
	for (uint32_t dpc : { 2u, 5u, 8u, 47u, 48u }) {
		r = {};
		r.disconnect_code = 2;
		r.disconnect_param = dpc;
		CHECK(inmatch::refuses_this_install(&r));
	}
	for (uint32_t dpc : { 1u, 9u, 32u, 46u, 49u }) {
		r = {};
		r.disconnect_code = 2;
		r.disconnect_param = dpc;
		CHECK(!inmatch::refuses_this_install(&r));
	}
	r = {};
	r.disconnect_code = 3;
	r.disconnect_param = 5;
	CHECK(!inmatch::refuses_this_install(&r));
	CHECK(!inmatch::refuses_this_install(nullptr));

	// The note: once per NovaWorld join, its listed server.
	Device device;
	inmatch::JoinFlow flow(device);
	JoinTargetRecord t = inmatch::join_target_from_row(row(32768, 0x4));
	t.network_type = inmatch::NetworkType::NovaWorld;
	flow.join_from_novaworld(t, 41);
	r = {};
	r.connect_error = 7;
	CHECK(flow.join_failed(&r) == std::optional<uint32_t>(41));
	CHECK(!flow.join_failed(&r));
	flow.join(inmatch::join_target_from_row(row(32768, 0x4)));
	CHECK(!flow.join_failed(&r));
	// A normal exit from the NovaWorld join's match returns to the NovaWorld menu once.
	CHECK(flow.take_return_to_novaworld() && !flow.take_return_to_novaworld());
	flow.host_from_novaworld();
	flow.release_novaworld();
	CHECK(!flow.take_return_to_novaworld());
}

// The mission start's columns over the session's: a map change's context still holding the last
// map's mission text never outreads the mission start's title, clock and TimeOfDay.
void test_mission_start_columns() {
	inmatch::NapiNPServerCtx ctx;
	ctx.config.server_name = "My Server";
	ctx.mission_text_title = "Last Map";
	inmatch::MissionStartListing mission;
	mission.mission_text_title = "Next Map";
	mission.header_name = "NEXT";
	mission.climate = 2;
	mission.last_round_ticks = 1234;
	HostRegistration r;
	r.time_of_day = 3;
	inmatch::read_mission_start_columns(r, ctx, {}, mission);
	CHECK(r.server_name == "My Server" && r.mission_name == "Next Map" && r.region_index == 2 &&
			r.time_of_day == 0 && r.round_time_remaining_ticks == 1234);
	mission.mission_text_title.clear();
	inmatch::read_mission_start_columns(r, ctx, {}, mission);
	CHECK(r.mission_name == "NEXT");
}

} // namespace

int main() {
	test_enumerate_then_dial();
	test_failed_switch_still_dials();
	test_choice_commit_routes_the_passwords();
	test_enumerate_abort_runs_state_9();
	test_novaworld_join_abandon_reenters_once();
	test_refusal();
	test_launch();
	test_refusal_rule_and_note();
	test_mission_start_columns();
	if (failures == 0)
		std::printf("inmatch_join_flow: ok\n");
	return failures == 0 ? 0 : 1;
}
