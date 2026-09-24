// The dev tools' inspection windows added after the ADR 0039 core (the Log
// window and the per-domain windows) against a null ImGui backend: each
// window formats its pushed record (or its drained source), its controls
// leave as the debug-control rows they name, and a layout pass draws it.
// The window registry itself is pinned in devtools_test.cpp.
#include "devtools_test_support.h"

#include <base/io/log_ring.h>
#include <runtime/devtools/ai_window.h>
#include <runtime/devtools/audio_window.h>
#include <runtime/devtools/entity_properties_window.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/debug_control_ids.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/log_window.h>
#include <runtime/devtools/net_window.h>
#include <runtime/devtools/particles_window.h>
#include <runtime/devtools/player_window.h>
#include <runtime/devtools/render_window.h>
#include <runtime/devtools/script_window.h>

#include <cstring>
#include <string>

using namespace devtools_test;
using opennova::devtools::ControlRequest;
using opennova::devtools::ControlResult;
using opennova::devtools::GameDevTools;
using opennova::devtools::LogWindow;
namespace control_id = opennova::devtools::control_id;

namespace {

// The Log window drains its ring past the cursor, marks a wrap, carries the
// F3 command verdicts, filters by level and text, and clears its rows while
// keeping the cursor.
void test_log_window_drains_filters_and_marks_gaps() {
	using opennova::io::LogLevel;
	opennova::io::LogRing ring;
	LogWindow window;
	window.set_ring(&ring);
	window.poll();
	CHECK(window.row_count() == 0, "an empty ring drains nothing");

	ring.record(LogLevel::kInfo, "mission loaded");
	ring.record(LogLevel::kWarn, "clamped a fog distance");
	ring.record(LogLevel::kError, "texture missing: foo.pcx");
	window.poll();
	CHECK(window.row_count() == 3, "three entries drain");
	CHECK(std::strcmp(window.row_text(0), "[info] mission loaded") == 0, "oldest first, level-tagged");
	CHECK(std::strcmp(window.row_text(2), "[error] texture missing: foo.pcx") == 0, "the error row");
	window.poll();
	CHECK(window.row_count() == 3, "a second poll with nothing new adds nothing");

	// Overrun the ring's capacity between two polls: the window says how many
	// it missed.
	for (size_t i = 0; i < opennova::io::LogRing::kCapacity + 10; ++i) {
		ring.record(LogLevel::kDebug, "spam");
	}
	window.poll();
	bool gap = false;
	for (int i = 0; i < window.row_count(); ++i) {
		gap = gap || std::strstr(window.row_text(i), "the ring wrapped: 10 message(s) missed") != nullptr;
	}
	CHECK(gap, "a wrapped ring is marked with the missed count");

	ControlResult refused;
	refused.id = "set_entity_health";
	refused.message = "Unauthorized";
	window.add_command_result(refused);
	ControlResult read;
	read.id = "environment_weather_snapshot";
	read.ok = true;
	read.detail = "{\"tod_hhmm\": 1230}";
	window.add_command_result(read);

	window.set_level_mask(1u << LogWindow::kCommand);
	CHECK(window.row_count() == 2, "the cmd level shows only the F3 verdicts");
	CHECK(std::strcmp(window.row_text(0), "[cmd] set_entity_health: failed (Unauthorized)") == 0,
			"a refusal row");
	CHECK(std::strcmp(window.row_text(1),
				  "[cmd] environment_weather_snapshot: ok = {\"tod_hhmm\": 1230}") == 0,
			"a read carries its full payload");

	window.set_level_mask((1u << LogWindow::kLevelCount) - 1u);
	window.set_text_filter("TEXTURE");
	CHECK(window.row_count() == 1, "the text filter is case-insensitive");
	window.set_text_filter("");

	window.clear();
	CHECK(window.total_rows() == 0, "clear drops the rows");
	window.poll();
	CHECK(window.total_rows() == 0, "and keeps the cursor: nothing old comes back");
	ring.record(LogLevel::kInfo, "after the clear");
	window.poll();
	CHECK(window.total_rows() == 1, "new entries still arrive");
}

// The tools route every command verdict to the Log window too, and the
// window draws in a layout pass.
void test_log_window_in_the_tools() {
	NullBackend backend;
	GameDevTools tools;
	opennova::io::LogRing ring;
	tools.set_log_ring(&ring);
	ControlResult ok;
	ok.id = "hide_foliage";
	ok.ok = true;
	tools.report_control_result(ok);
	CHECK(tools.log_window().total_rows() == 1, "the verdict lands in the Log window");
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	tools.log_window().open = true;
	ring.record(opennova::io::LogLevel::kInfo, "hello");
	CHECK(draw_once(tools, 1), "the Log window draws");
	CHECK(tools.log_window().total_rows() == 2, "the draw drained the ring");
}

// Every per-domain window is gated on its own visibility, formats its pushed
// record, draws in a layout pass, and drains its requests through the one
// control-request channel.
void test_domain_windows_gate_and_draw() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	CHECK(!tools.needs_script_snapshot() && !tools.needs_player_snapshot() && !tools.needs_render_snapshot() &&
					!tools.needs_particle_snapshot() && !tools.needs_audio_snapshot() && !tools.needs_net_snapshot(),
			"closed windows want nothing");
	tools.script_window().open = true;
	tools.player_window().open = true;
	tools.render_window().open = true;
	tools.particles_window().open = true;
	tools.audio_window().open = true;
	tools.net_window().open = true;
	CHECK(tools.needs_script_snapshot() && tools.needs_player_snapshot() && tools.needs_render_snapshot() &&
					tools.needs_particle_snapshot() && tools.needs_audio_snapshot() && tools.needs_net_snapshot(),
			"open windows want their records");

	std::vector<const char *> wanted;
	tools.wanted_control_ids(wanted);
	const auto wants = [&](const char *id) {
		for (const char *w : wanted) {
			if (std::strcmp(w, id) == 0) return true;
		}
		return false;
	};
	CHECK(wants(control_id::kTerrainDrawMode) && wants(control_id::kViewportDebugDraw),
			"the Render window reads its knobs through the board");
	CHECK(wants(control_id::kSetMissionVariable) && wants(control_id::kForceFpArms) &&
					wants(control_id::kHideParticles) && wants(control_id::kNetJoinerDiagnostics),
			"every window declares its rows");
	CHECK(draw_once(tools, 1), "every window draws with no record");
}

void test_script_window() {
	opennova::devtools::ControlBoard board;
	opennova::devtools::ScriptWindow window(board);
	opennova::devtools::ScriptSnapshot snapshot;
	snapshot.valid = true;
	snapshot.authority = true;
	snapshot.report.wac_loaded = true;
	snapshot.report.code_words = 120;
	snapshot.report.runs = 7;
	snapshot.report.time = 9;
	snapshot.report.divider = 31;
	snapshot.report.rng_seed = 0x12333333u;
	snapshot.report.first_error = "mission.wac (12) unknown command";
	snapshot.report.mission_vars[3] = 5;
	opennova::world::RuntimeGap gap;
	gap.origin.kind = opennova::world::RuntimeGapKind::WacCommand;
	gap.origin.code = 88;
	gap.count = 4;
	snapshot.report.runtime_gaps.push_back(gap);
	window.set_snapshot(snapshot);
	CHECK(window.summary_text().find("runs 7") != std::string::npos &&
					window.summary_text().find("divider 31/62") != std::string::npos,
			"the WAC summary line");
	CHECK(window.first_error_text() == "mission.wac (12) unknown command", "the retail page's error line");
	CHECK(window.gap_count() == 1 && std::strstr(window.gap_text(0), "WAC command 88/0") != nullptr,
			"the runtime gaps list their site");
	window.request_set_mission_variable(3, 42);
	ControlRequest request;
	CHECK(window.take_request(request) && is_control(request, control_id::kSetMissionVariable) &&
					request.args.size() == 2 && request.args[0].i == 3 && request.args[1].i == 42,
			"a variable edit is the set_mission_variable row");
	window.on_visibility(false);
	CHECK(!window.snapshot_valid(), "hiding drops the record");
}

void test_player_window() {
	opennova::devtools::ControlBoard board;
	opennova::devtools::PlayerWindow window(board);
	opennova::devtools::PlayerSnapshot snapshot;
	snapshot.valid = true;
	snapshot.report.valid = true;
	snapshot.report.name = "Player1";
	snapshot.report.handle = 0x0002;
	snapshot.report.team = 1;
	snapshot.report.health = 80;
	snapshot.report.health_max = 100;
	snapshot.report.stance = 1;
	snapshot.report.yaw_deg = 90;
	window.set_snapshot(snapshot);
	CHECK(window.body_text() == "Player1  handle 0x0002  team 1  hp 80/100 | crouched | yaw 90 pitch 0 roll 0",
			"the body line");
	window.request_look(32.0f, 0.0f);
	window.request_viewmodel_weapon("M4");
	window.request_deploy_pick(2);
	window.request_crew_local_player(1234);
	ControlRequest request;
	CHECK(window.take_request(request) && is_control(request, control_id::kLocalPlayerLook) &&
					request.args[0].f == 32.0,
			"the look nudge");
	CHECK(window.take_request(request) && is_control(request, control_id::kSetViewmodelWeapon) &&
					request.args[0].text == "M4",
			"the viewmodel override");
	CHECK(window.take_request(request) && is_control(request, control_id::kDeployPick) && request.args[0].i == 2,
			"the deploy pick");
	CHECK(window.take_request(request) && is_control(request, control_id::kCrewLocalPlayer) &&
					request.args[0].i == 1234,
			"crewing a vehicle by SSN");
}

void test_render_particles_audio_net_windows() {
	opennova::devtools::ControlBoard board;
	opennova::devtools::RenderWindow render(board);
	opennova::devtools::RenderSnapshot rs;
	rs.valid = true;
	rs.device.viewport_width = 1280;
	rs.device.viewport_height = 720;
	rs.device.visible_draw_calls = 900;
	rs.terrain_valid = true;
	rs.terrain.emitted_patches = 120;
	rs.terrain.traversal.budget_drops = 3;
	render.set_snapshot(rs);
	CHECK(render.device_text().find("1280x720 | visible 900 draws") == 0, "the device line");
	CHECK(render.terrain_text().find("120 emitted") != std::string::npos &&
					render.terrain_text().find("budget drops 3") != std::string::npos,
			"the terrain traversal line");

	opennova::devtools::ParticlesWindow particles(board);
	opennova::devtools::ParticleSnapshot ps;
	ps.valid = true;
	ps.active_entries = 40;
	opennova::particle::EffectGroupDebugSnapshot group;
	group.effect_name = "smoke";
	opennova::particle::EffectEmitterDebugSnapshot e1;
	e1.definition_name = "puff";
	e1.alive_particle_count = 30;
	opennova::particle::EffectEmitterDebugSnapshot e2 = e1;
	e2.definition_name = "embers";
	e2.alive_particle_count = 10;
	group.emitters = {e1, e2};
	ps.scene.groups = {group};
	particles.set_snapshot(ps);
	CHECK(particles.count_text() == "Current Particle Count:  40 / 40", "the retail page's count line");
	CHECK(particles.emitter_row_count() == 2 && std::strncmp(particles.emitter_row(0), "00   smoke", 10) == 0 &&
					std::strncmp(particles.emitter_row(1), "      embers", 12) == 0,
			"the emitter list: the group's first emitter numbered, the rest indented");
	ps.active_entries = 12;
	particles.set_snapshot(ps);
	CHECK(particles.peak() == 40, "the peak latches");
	ps.active_entries = 0;
	particles.set_snapshot(ps);
	CHECK(particles.peak() == 0, "and resets at zero");

	opennova::devtools::AudioWindow audio;
	opennova::devtools::AudioSnapshot as;
	as.valid = true;
	as.buses.push_back({"Master", -3.0f, false, false, false, -12.0f, -14.0f});
	as.mission_valid = true;
	as.markers_total = 10;
	as.markers_resolved = 9;
	as.channel_budget = 32;
	audio.set_snapshot(as);
	CHECK(audio.mission_text().find("markers 9/10 resolved") == 0, "the mission audio line");
	audio.request_volume("Master", -100.0f);
	audio.request_flag(control_id::kSetAudioBusMute, "Master", true);
	ControlRequest request;
	CHECK(audio.take_request(request) && is_control(request, control_id::kSetAudioBusVolume) &&
					request.args[0].text == "Master" &&
					request.args[1].f == opennova::devtools::AudioWindow::kVolumeMinDb,
			"a volume is clamped to the row's range");
	CHECK(audio.take_request(request) && is_control(request, control_id::kSetAudioBusMute) && request.args[1].b,
			"the mute row");

	opennova::devtools::NetWindow net(board);
	opennova::devtools::NetSnapshot ns;
	ns.valid = true;
	ns.wall_seconds = 1.0;
	ns.role = opennova::devtools::StatusRole::ListenServer;
	ns.state = opennova::devtools::StatusState::Running;
	ns.bank_policy = "wall clock";
	ns.fps = 60.0;
	ns.traffic_valid = true;
	ns.traffic.tx_packets = 100;
	ns.traffic.tx_bytes = 10000;
	net.set_snapshot(ns);
	CHECK(net.session_text().find("listen server, running | 60 fps") == 0, "the session line");
	CHECK(net.traffic_text().find("sent 100 pkt") == 0, "the traffic totals");
	ns.wall_seconds = 2.0;
	ns.traffic.tx_packets = 160;
	net.set_snapshot(ns);
	CHECK(net.traffic_text().find("now 60.0/0.0 pkt/s") != std::string::npos, "rates between pushes");
}

// The automation rows on their natural windows: the AI group table's Kill and
// the Entity Properties crewing leave as the kill_group / crew_vehicle rows.
void test_group_kill_and_crewing_requests() {
	GameDevTools tools;
	tools.ai_window().request_kill_group(5);
	tools.entity_properties_window().request_crew_vehicle(12, 34);
	ControlRequest request;
	CHECK(tools.take_control_request(request) && is_control(request, control_id::kKillGroup) &&
					request.args.size() == 1 && request.args[0].i == 5,
			"Kill is the kill_group row by group id");
	CHECK(tools.take_control_request(request) && is_control(request, control_id::kCrewVehicle) &&
					request.args.size() == 2 && request.args[0].i == 12 && request.args[1].i == 34,
			"crewing is the crew_vehicle row (occupant, vehicle)");
	CHECK(!tools.take_control_request(request), "drained once");
}

}  // namespace

int main() {
	test_log_window_drains_filters_and_marks_gaps();
	test_log_window_in_the_tools();
	test_domain_windows_gate_and_draw();
	test_script_window();
	test_player_window();
	test_render_particles_audio_net_windows();
	test_group_kill_and_crewing_requests();
	return report("devtools_windows_test");
}
