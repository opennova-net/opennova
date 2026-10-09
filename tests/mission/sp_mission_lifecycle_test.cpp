// The single-player mission lifecycle after the round ends, ungated, over a
// synthetic mission booted through the SP listen server (inmatch::HostRole's
// SP bring-up) and driven through inmatch::Session: the WIN epilog's stage
// sequence (the cached camera and the opening letterbox, the score screen
// 95 timeline frames in with the counters frozen at the build, the
// g_EpilogScreenActive script hold, the 18600-frame timeout to exit reason 1);
// the LOSE screen (the build on the first dispatch after a rendered cine
// frame, the script halted two frames after the round end); the round-over
// keys (every key taken out of a session, RESTART -> exit 4 with the screen
// down, ESC -> exit 1); the in-game RESTART command; the main frame's exit
// router; and the SP restart itself: the world the restart boots equals a
// fresh launch (the spawn, the fired-event boot set, the round and cine state)
// while the process-lifetime counters carry.
// [orig: Server_ProcessRoundEnd @0x5164F0 SP tail; Cinematic_EpilogUpdate
//  @0x577950; Cine_EpilogStateMachineUpdate @0x576240; Input_HandleSpecialKeys
//  @0x49C5C0 round-over arm; UI_IngameRestartCommand @0x555410;
//  Game_ProcessMainFrame @0x526806..0x526867; Game_RestartRoundSP @0x5263A0]
#include <runtime/audio/dialog_queue.h>
#include <runtime/hud/hud_toggles.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/epilog_cine.h>
#include <runtime/world/local_player.h>
#include <runtime/world/objectives_feed.h>
#include <base/gameprofile/game_type.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;
namespace im = opennova::inmatch;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

using test_boot::source_over;

// The SP mission: the two-entity mission plus a PreMission event (the boot
// set: it fires in the PreMission pass) and a trigger-less normal event (it
// fires on the first quarter pass of play, so it is NOT in the boot set).
bms::File lifecycle_mission() {
	bms::File m = test_mission::two_entity_mission();
	m.events.clear();
	bms::Event pre{};
	pre.flags = bms::EventFlags::PreMission;
	pre.action_index = 0;
	pre.action_count = 1;
	bms::Event play{};
	play.action_index = 1;
	play.action_count = 1;
	m.events = {pre, play};
	bms::Action show{};
	show.action_type = bms::ActionType::ShowWinSubgoal;
	show.param1 = 1;
	show.param2 = 1;
	bms::Action show2 = show;
	show2.param1 = 2;
	m.actions = {show, show2};
	return m;
}

// One SP mission: a kernel, its SP listen-server role and the session over it.
struct SpMission {
	std::unique_ptr<ms::MissionKernel> kernel = std::make_unique<ms::MissionKernel>();
	std::unique_ptr<im::HostRole> role = std::make_unique<im::HostRole>();
	std::unique_ptr<im::Session> session;

	bool boot(const std::map<std::string, std::string> &files, bool restart,
			ms::MissionKernel *previous = nullptr) {
		role->bind(*kernel);
		if (previous != nullptr) kernel->carry_across_load_from(*previous);
		kernel->open_document(lifecycle_mission(), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.game_type = game_type::for_mission_mode(bms::selected_game_mode(
				static_cast<bms::AttribFlags>(kernel->mission.header.attrib_flags)));
		options.restart = restart;
		im::HostRole *host = role.get();
		options.bringup_net_session = [host] { host->bring_up_singleplayer(); };
		std::string error;
		if (!kernel->boot(options, error)) {
			std::printf("boot failed: %s\n", error.c_str());
			return false;
		}
		session = std::make_unique<im::Session>(*role);
		return session->begin_load().applied() && session->complete_load().applied();
	}
	w::World &world() { return kernel->world; }
	im::FrameOutcome frame() { return session->drive_one(); }
	void frames(int n) {
		for (int i = 0; i < n; ++i) (void)frame();
	}
	im::RoleView view() {
		im::RoleView v;
		v.kernel = kernel.get();
		v.runtime = role->client_runtime();
		v.host = &role->state.host_owner.ctx;
		return v;
	}
};

std::vector<bool> fired_set(const ms::MissionKernel &kernel) {
	std::vector<bool> out;
	for (size_t i = 0; i < kernel.events.events().size(); ++i)
		out.push_back(kernel.events.event_fired(i));
	return out;
}

const w::CineEvent *find_event(const w::EpilogCine &cine, w::CineEventKind kind, int nth = 0) {
	for (const w::CineEvent &e : cine.events)
		if (e.kind == kind && nth-- == 0) return &e;
	return nullptr;
}

int count_sounds(const w::World &world, const char *name) {
	int n = 0;
	for (const w::ScriptSoundEvent &s : world.out.script_sounds)
		if (s.name == name) ++n;
	return n;
}

// D-SND-35: a counter's count-up column steps once every five dispatches and
// plays TEXT_END on the step that lands on its target; the live epilog's -1
// target and rate step it away from the target, so it never sounds.
// [orig: CineNode_CounterStep @0x573390 — the wait @0x5733ab, the step
//  @0x5733c0..0x5733fa, TEXT_END @0x573408, the re-arm @0x573416; the start
//  @0x573370]
void test_counter_step_plays_text_end_on_arrival() {
	struct Case {
		int32_t target, rate;
		int arrival_frame; // -1: never
		int32_t final_points;
	};
	// Steps land on frames 4, 9, 14, ... (the start's wait of four, then the
	// re-arm after each step).
	const Case cases[] = {
			{3, 1, 14, 3},     // up by one: 1, 2, 3
			{5, 2, 14, 5},     // up by two, clamped: 2, 4, 5
			{-3, 1, 14, -3},   // a negative target subtracts the rate
			{0, 1, -1, 0},     // a zero target never steps
			{-1, -1, -1, 20},  // the live epilog's: 0, 1, 2 .. away from -1
	};
	for (const Case &c : cases) {
		auto world = std::make_unique<w::World>();
		w::EpilogCine cine;
		cine.active = true;
		cine.frame = -1;
		w::CineEvent line;
		line.kind = w::CineEventKind::EpilogCounter;
		line.start = 0;
		line.duration = 200;
		line.points_target = c.target;
		line.points_rate = c.rate;
		cine.events.push_back(line);
		int arrival = -1;
		for (int f = 0; f <= 100; ++f) {
			const int before = count_sounds(*world, w::kEpilogCounterEndSoundset);
			cine.update(*world);
			if (count_sounds(*world, w::kEpilogCounterEndSoundset) != before) {
				CHECK(arrival < 0); // once
				arrival = f;
			}
		}
		CHECK(arrival == c.arrival_frame);
		CHECK(cine.events[0].points == c.final_points);
		for (const w::ScriptSoundEvent &s : world->out.script_sounds)
			CHECK(s.kind == w::ScriptSoundEvent::Kind::Interface);
	}
	CHECK(std::string(w::kEpilogCounterEndSoundset) == "TEXT_END");
}

} // namespace

int main() {
	const std::map<std::string, std::string> files;

	// --- the mission start's cine legs -----------------------------------------
	// A first start flags the cine running over an empty timeline (the
	// <mission>.cin intro leg; no shipped mission has one); the first render
	// pass stops it without drawing, so no cine frame has been drawn yet.
	// [orig: Game_StartMission @0x525DA8..0x525DD6; sub_578270 @0x578296;
	//  sub_575A50 @0x575A6B]
	{
		SpMission m;
		CHECK(m.boot(files, /*restart=*/false));
		const w::EpilogCine &cine = m.world().epilog;
		CHECK(cine.active);
		CHECK(cine.frame == 0);
		CHECK(cine.mode == w::EpilogCineMode::None);
		CHECK(cine.events.empty());
		CHECK(!cine.screen_active);
		m.frames(1);
		CHECK(!m.world().epilog.active);
		CHECK(!m.world().epilog.frame_drawn);
		CHECK(m.world().mission_exit_reason == 0);
	}

	// --- the WIN epilog --------------------------------------------------------
	{
		SpMission m;
		CHECK(m.boot(files, false));
		m.frames(4);
		w::World &world = m.world();
		const w::Entity *player = world.registry.get(world.cached.local_player);
		CHECK(player != nullptr);
		const w::Vec3 pose = player != nullptr ? player->position : w::Vec3{};
		// The won-subgoal tally the score screen reads.
		world.kill_stats.subgoals_won = 1;
		world.process_round_end(1);
		w::EpilogCine &cine = world.epilog;
		// Cine_InitPlayback with no <mission>.end: the cached camera at the
		// round-end pose for 300 frames, the 62-frame letterbox, state 0.
		// [orig: @0x578479..0x57861D]
		CHECK(cine.mode == w::EpilogCineMode::Win && cine.active && cine.frame == -1);
		CHECK(cine.win_state == 0 && cine.win_build_frame == 95);
		CHECK(cine.events.size() == 2);
		const w::CineEvent *camera = find_event(cine, w::CineEventKind::CacheCamera);
		CHECK(camera != nullptr && camera->start == 0 && camera->duration == 300);
		if (camera != nullptr) {
			CHECK(camera->camera_position.x == pose.x && camera->camera_position.y == pose.y &&
					camera->camera_position.z == pose.z);
		}
		const w::CineEvent *opening = find_event(cine, w::CineEventKind::Letterbox);
		CHECK(opening != nullptr && opening->start == 0 && opening->duration == 62 &&
				opening->letterbox_mode == 1);
		// The win holds no script until the score screen: the WAC and the BMS
		// quarter pass run through the cine's first 95 frames.
		CHECK(!world.epilog_screen_active());
		CHECK(world.script_may_advance());
		// Frame by frame: the timeline reaches 95 (state 0 -> 4), the next
		// dispatch builds the score screen, and that same update's tail
		// dispatches once more (the screen is up), entering the fade state.
		// [orig: @0x57626F..0x576283; @0x5764DB..0x5767D7; Entity_UpdateAllEntities
		//  @0x4C2624 -> @0x4C2634]
		const uint32_t counter_before = world.entity_update_counter;
		int built_at = -1;
		for (int i = 0; i < 200 && built_at < 0; ++i) {
			(void)m.frame();
			if (cine.screen_active) built_at = cine.frame;
		}
		CHECK(built_at == 97); // the build at frame 96, the tail's second dispatch at 97
		CHECK(cine.win_state == 5 && cine.win_age == 1);
		CHECK(world.entity_update_counter == counter_before + 96); // frames 0..95 counted
		CHECK(!world.script_may_advance());
		// The score screen: the fade pair from frame 97, jo_Epil.tga from 145,
		// the letterbox from 193, the four counters from 317 at 60 apart, the
		// key help 120 past the last [orig: @0x5764F6..0x5767D2].
		const w::CineEvent *fade1 = find_event(cine, w::CineEventKind::ImageFade, 0);
		const w::CineEvent *fade2 = find_event(cine, w::CineEventKind::ImageFade, 1);
		const w::CineEvent *backdrop = find_event(cine, w::CineEventKind::ImageFade, 2);
		CHECK(fade1 != nullptr && fade1->start == 97 && fade1->duration == 48 &&
				fade1->image.empty() && fade1->fade_mode == w::CineFadeMode::In &&
				fade1->fade_source == w::CineFadeSource::SolidColor && fade1->solid_color == 0xFFFFFFu);
		CHECK(fade2 != nullptr && fade2->start == 145 && fade2->fade_mode == w::CineFadeMode::Out);
		CHECK(backdrop != nullptr && backdrop->start == 145 && backdrop->duration == 223200 &&
				backdrop->image == "jo_Epil.tga" && backdrop->fade_mode == w::CineFadeMode::Hold);
		const w::CineEvent *bars = find_event(cine, w::CineEventKind::Letterbox, 1);
		CHECK(bars != nullptr && bars->start == 193 && bars->duration == 100 &&
				bars->letterbox_mode == 2);
		const char *labels[4] = {"STREPILOG_OBJECTIVEBONUS", "STREPILOG_ENEMYUNITS",
				"STREPILOG_TEAMUNITS", "STREPILOG_FRIENDLYUNITS"};
		const hud::EndRoundStatisticsInput in = w::end_round_statistics_input(world);
		for (int i = 0; i < 4; ++i) {
			const w::CineEvent *line = find_event(cine, w::CineEventKind::EpilogCounter, i);
			CHECK(line != nullptr);
			if (line == nullptr) continue;
			CHECK(line->start == 317 + 60 * i);
			CHECK(line->label_key == labels[i]);
			CHECK(line->row_y == 160 + 48 * i);
			CHECK(line->label_x == 192 && line->value_x == 692);
			CHECK(line->points_target == -1 && line->points_rate == -1);
		}
		const w::CineEvent *objective = find_event(cine, w::CineEventKind::EpilogCounter, 0);
		CHECK(objective != nullptr && objective->value == 1 &&
				objective->max == in.subgoals_defined);
		const w::CineEvent *team = find_event(cine, w::CineEventKind::EpilogCounter, 2);
		CHECK(team != nullptr && team->max == -1);
		const w::CineEvent *help = find_event(cine, w::CineEventKind::TextFade);
		CHECK(help != nullptr && help->start == 617 && help->text_key == "STREPILOG_KEYINFO" &&
				help->y == 700 && help->box_width == 1024);
		// The values are frozen at the build.
		world.kill_stats.subgoals_won = 3;
		CHECK(objective != nullptr && objective->value == 1);
		// The live state the timeline walk advances: the opening bars faded in
		// over 62 frames and are held; the white fade climbs 1/48 a frame from
		// its start; the screen's fade-out letterbox clears the bars over 100
		// frames; a line fades in over its fade_in frames [orig: sub_56FF40;
		// sub_571020; CCineEventFade_StepAlpha @0x5712D0; sub_571330; sub_572F90].
		auto near = [](float a, float b) { return a - b < 1e-3f && b - a < 1e-3f; };
		CHECK(!cine.bars_fading && cine.bars_held);
		const size_t fade1_index = static_cast<size_t>(fade1 - cine.events.data());
		while (cine.frame < 120) (void)m.frame();
		CHECK(cine.frame == 120);
		CHECK(near(cine.events[fade1_index].alpha, 24.0f / 48.0f));
		while (cine.frame < 243) (void)m.frame();
		CHECK(cine.bars_fading && !cine.bars_held && near(cine.bars_alpha, 1.0f - 51.0f / 100.0f));
		while (cine.frame < 300) (void)m.frame();
		CHECK(!cine.bars_fading && !cine.bars_held);
		const w::CineEvent *help_now = find_event(cine, w::CineEventKind::TextFade);
		while (cine.frame < 686) (void)m.frame();
		CHECK(help_now != nullptr && near(help_now->alpha, 70.0f / 140.0f));
		// D-SND-35: the live counters' count-up columns step away from their
		// -1 target, so neither TEXT_END nor HEADSHOTTONE plays (the counter
		// class whose step plays HEADSHOTTONE, CineEventEpilogCounterFont, is
		// never constructed) [orig: @0x5765F6..0x5765F8; the dead constructor
		// @0x5735d0, its step cinematic_node_color_fade_update @0x5736fe].
		const w::CineEvent *first_line = find_event(cine, w::CineEventKind::EpilogCounter, 0);
		CHECK(first_line != nullptr && first_line->points > 0);
		CHECK(count_sounds(world, "TEXT_END") == 0);
		CHECK(count_sounds(world, "HEADSHOTTONE") == 0);
		// The timeout: 18600 frames into the fade state the mission exits to
		// the Post Menu [orig: @0x576822 / @0x576824].
		im::FrameOutcome last;
		int frames = 0;
		while (frames < 20000) {
			last = m.frame();
			++frames;
			if (last.terminal()) break;
		}
		CHECK(last.status == im::FrameStatus::SessionLost);
		CHECK(world.mission_exit_reason == 1);
		CHECK(cine.win_age == 18601);
		CHECK(im::main_frame_exit(world.mission_exit_reason, false, true) ==
				im::MainFrameExit::PostMenu);
	}

	// --- the LOSE screen -------------------------------------------------------
	{
		SpMission m;
		CHECK(m.boot(files, false));
		m.frames(4);
		w::World &world = m.world();
		w::EpilogCine &cine = world.epilog;
		world.script.dialog.enqueue(audio::dialog_name_of(5), {});
		world.out.effects.clear();
		world.process_round_end(2);
		// The SP tail's Dialog_ResetAll clears the world's dialog table, its
		// waiting lines with it [orig: Server_ProcessRoundEnd @0x516953].
		CHECK(world.script.dialog.history().empty() && world.script.dialog.slots().empty());
		CHECK(world.out.effects.count("round_end") == 1);
		// Cine_StartPlayback: the 100-frame letterbox and the edit fade.
		CHECK(cine.mode == w::EpilogCineMode::Lose && cine.lose_state == 0);
		CHECK(cine.events.size() == 2);
		CHECK(find_event(cine, w::CineEventKind::EditFade) != nullptr);
		// No cine frame has rendered in this process yet: the first dispatch
		// enters state 1 and its render draws one; the second builds.
		(void)m.frame();
		CHECK(cine.lose_state == 1 && !cine.screen_active && cine.frame_drawn);
		CHECK(world.script_may_advance());
		// The build's Dialog_ResetAll clears the table again [orig:
		// Cinematic_EpilogUpdate @0x5747DA].
		world.script.dialog.enqueue(audio::dialog_name_of(6), {});
		(void)m.frame();
		CHECK(cine.screen_active && cine.lose_state == 2);
		CHECK(world.script.dialog.history().empty() && world.script.dialog.slots().empty());
		CHECK(cine.frame == 2 && cine.lose_age == 1);
		CHECK(!world.script_may_advance());
		// MISSION FAILED, the banner line and the key help: 249 frames past
		// the build frame, then the fade pair's 96 [orig: @0x574512..0x57476D].
		const w::CineEvent *backdrop = find_event(cine, w::CineEventKind::ImageFade, 2);
		CHECK(backdrop != nullptr && backdrop->image == "jo_Epil2.tga" && backdrop->start == 298);
		const w::CineEvent *failed = find_event(cine, w::CineEventKind::TextFade, 0);
		CHECK(failed != nullptr && failed->start == 346 && failed->text_section == "Overlays" &&
				failed->text_key == "STROVER_MISSION_FAILED" && failed->y == 120 &&
				failed->fade_in == 100);
		const w::CineEvent *banner = find_event(cine, w::CineEventKind::TextFade, 1);
		CHECK(banner != nullptr && banner->text_source == w::CineTextSource::Banner &&
				banner->y == 230);
		const w::CineEvent *help = find_event(cine, w::CineEventKind::TextFade, 2);
		CHECK(help != nullptr && help->start == 470 && help->y == 600);
		CHECK(cine.end_frame() == 470 + 223200);

		// --- the round-over keys (behind the gate the round end raised) ---
		const im::RoleView view = m.view();
		namespace k = hud::hud_round_over;
		CHECK(im::round_over_key(view, 'X', 'R') == k::kConsumed);
		CHECK(world.mission_exit_reason == 0);
		CHECK(im::round_over_key(view, 'R', 'R') == (k::kConsumed | k::kRestart));
		CHECK(!cine.screen_active);
		CHECK(world.mission_exit_reason == 4);
		CHECK(im::main_frame_exit(world.mission_exit_reason, false, true) ==
				im::MainFrameExit::RestartRoundSP);
		// The session reports the exit on its next frame.
		const im::FrameOutcome out = m.frame();
		CHECK(out.status == im::FrameStatus::SessionLost);
	}

	// --- the round init's dialog reset -----------------------------------------
	{
		// The local player's (re)deploy runs round init, which clears the
		// world's dialog table [orig: Server_ProcessPlayerDeath @0x5178aa ->
		// Game_InitNewRound @0x422741 / @0x4227ac -> Dialog_ResetAll @0x44dc90].
		SpMission m;
		CHECK(m.boot(files, false));
		w::World &world = m.world();
		CHECK(world.local_player_state != nullptr);
		world.script.dialog.enqueue(audio::dialog_name_of(5), {});
		CHECK(world.script.dialog.active(5));
		world.local_player_state->reset_for_new_round();
		CHECK(world.script.dialog.history().empty() && world.script.dialog.slots().empty());
	}

	// --- the round-over leg's gates and the ESC exit ---------------------------
	{
		SpMission m;
		CHECK(m.boot(files, false));
		const im::RoleView view = m.view();
		namespace k = hud::hud_round_over;
		// Before the round ends the leg is not reached.
		CHECK(im::round_over_key(view, 27, 'R') == 0);
		m.world().process_round_end(2);
		CHECK(im::round_over_key(view, 27, 'R') == (k::kConsumed | k::kExit));
		CHECK(m.world().mission_exit_reason == 1);
		// The pure rule: a session takes nothing, the co-op game type every key.
		hud::HudRoundOverKeyInput in;
		in.vk = 'R';
		in.in_session = true;
		in.game_type = 0x10000; // TDM
		CHECK(hud::hud_round_over_key(in) == 0);
		in.game_type = 0x10020; // co-op
		CHECK(hud::hud_round_over_key(in) == (k::kConsumed | k::kRestart));
		in.game_type = 0x30020; // co-op with bit 0x20000
		CHECK(hud::hud_round_over_key(in) == 0);
		// The main frame's router [orig: @0x526806..0x526867].
		CHECK(im::main_frame_exit(0, false, true) == im::MainFrameExit::None);
		CHECK(im::main_frame_exit(8, false, true) == im::MainFrameExit::GameLoop);
		CHECK(im::main_frame_exit(4, true, false) == im::MainFrameExit::GameLoop);
		CHECK(im::main_frame_exit(4, true, true) == im::MainFrameExit::PostMenu);
		CHECK(im::main_frame_exit(12, true, false) == im::MainFrameExit::PostMenu);
	}

	// --- the in-game RESTART command ------------------------------------------
	{
		SpMission m;
		CHECK(m.boot(files, false));
		CHECK(m.world().ingame_restart_command());
		CHECK(m.world().mission_exit_reason == 4);
		m.world().mission_exit_reason = 0;
		m.world().rules.mp_session = true; // in a session the command does nothing
		CHECK(!m.world().ingame_restart_command());
		CHECK(m.world().mission_exit_reason == 0);
	}

	// --- the SP restart is a fresh launch ---------------------------------------
	// Game_RestartRoundSP destroys every entity and starts the same mission
	// again (Game_StartMission(1)); the port swaps in a fresh kernel the way a
	// load does, the process-lifetime counters crossing over. The restarted
	// world equals a fresh launch: the spawn, the fired-event boot set, the
	// round and the cine idle, the score block zero.
	{
		SpMission fresh;
		CHECK(fresh.boot(files, false));
		const std::vector<bool> boot_set = fired_set(*fresh.kernel);
		CHECK(boot_set.size() == 2 && boot_set[0] && !boot_set[1]);
		const w::Entity *fresh_player = fresh.world().registry.get(fresh.world().cached.local_player);
		CHECK(fresh_player != nullptr);

		auto played = std::make_unique<SpMission>();
		CHECK(played->boot(files, false));
		played->frames(80);
		CHECK(fired_set(*played->kernel)[1]); // the play-time event fired
		w::Entity *player = played->world().registry.get(played->world().cached.local_player);
		CHECK(player != nullptr);
		if (player != nullptr) player->position.x += 40.0f;
		played->world().kill_stats.subgoals_won = 2;
		played->world().process_round_end(1);
		played->frames(120); // into the score screen
		CHECK(played->world().epilog_screen_active());
		CHECK(im::round_over_key(played->view(), 'R', 'R') ==
				(hud::hud_round_over::kConsumed | hud::hud_round_over::kRestart));
		CHECK(played->frame().status == im::FrameStatus::SessionLost);
		CHECK(im::main_frame_exit(played->world().mission_exit_reason, false, true) ==
				im::MainFrameExit::RestartRoundSP);
		const uint32_t counter_at_exit = played->world().entity_update_counter;
		(void)played->session->close();

		SpMission restarted;
		CHECK(restarted.boot(files, /*restart=*/true, played->kernel.get()));
		played.reset();
		w::World &rw = restarted.world();
		const w::Entity *again = rw.registry.get(rw.cached.local_player);
		CHECK(again != nullptr);
		if (again != nullptr && fresh_player != nullptr) {
			CHECK(again->position.x == fresh_player->position.x);
			CHECK(again->position.y == fresh_player->position.y);
			CHECK(again->position.z == fresh_player->position.z);
			CHECK(again->yaw == fresh_player->yaw);
		}
		CHECK(fired_set(*restarted.kernel) == boot_set);
		CHECK(!rw.match.outcome().ended);
		CHECK(rw.mission_exit_reason == 0);
		CHECK(rw.kill_stats.subgoals_won == 0);
		CHECK(rw.logic_tick == fresh.world().logic_tick);
		CHECK(rw.registry.count_humans() == fresh.world().registry.count_humans());
		// The restart's start skips the intro-cine leg: the cine stays idle
		// over an empty timeline, the end screen down.
		CHECK(!rw.epilog.active && rw.epilog.mode == w::EpilogCineMode::None);
		CHECK(rw.epilog.events.empty() && !rw.epilog.screen_active);
		// The process-lifetime words cross over: the entity-update counter and
		// the cine's drawn flag [orig: g_EntityUpdateCounter; dword_26970F4].
		CHECK(rw.entity_update_counter == counter_at_exit);
		CHECK(rw.epilog.frame_drawn);
		CHECK(!fresh.world().epilog.frame_drawn);
		// And it plays on: the round is live, the play-time event fires again.
		restarted.frames(80);
		CHECK(fired_set(*restarted.kernel)[1]);
		CHECK(rw.script_may_advance());
	}

	test_counter_step_plays_text_end_on_arrival();
	if (failures == 0) std::printf("sp_mission_lifecycle: OK\n");
	return failures == 0 ? 0 : 1;
}
