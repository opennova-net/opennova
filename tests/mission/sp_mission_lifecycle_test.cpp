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
#include <runtime/inmatch/special_key_chain.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/epilog_cine.h>
#include <runtime/world/local_player.h>
#include <runtime/world/objectives_feed.h>
#include <base/gameprofile/game_type.h>

#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <algorithm>
#include <functional>
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
	// Run inside the boot's bring-up, past the role's (the round init it installed stands).
	std::function<void()> after_bringup;

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
		options.bringup_net_session = [this, host] {
			host->bring_up_singleplayer();
			if (after_bringup) after_bringup();
		};
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
// [orig: CineEventEpilogCounter_Step @0x573390 — the wait @0x5733ab, the step
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

// D-NET-NEW-L7-39: the single player's session create adds its own slot with no body (the own
// arm stores g_LocalPlayerEntity, which every teardown zeroes), and its mission start's round
// init spawns the body, hidden, past the PreMission pass; Player_InitPlayer and the start's
// local-player block leave it hidden until the own join, which clears it. So a first start's
// PreMission pass meets no local player: its ShowWinSubgoal shows the objective and plays no
// NEW_GOAL (the null test). A restart's start (Game_StartMission(1)) runs the same pass and
// round init, but retail's restart destroys the entities without clearing g_LocalPlayerEntity
// (only Player_InitPlayer's memset does, past the pass), so its pass reads the destroyed row and
// plays NEW_GOAL at its Position; ours has no body there. The restart leg pins ours, a
// deliberate fix of that dangling read (D-NET-NEW-L7-40, PERMANENT, ratified by the
// maintainer 2026-10-23, Q-L7-39-1 (a)).
// [orig: Server_PlayerAdd's own arm @0x51CC33..0x51CC60; Game_TeardownMission - the memset of
//  g_LocalPlayerEntity @0x522774..0x522780; Game_StartMission @0x524360 - the
//  EventTrigger_UpdateAllWithFlag2 call @0x525B86, the Server_InitAllPlayerEntitiesForRound call
//  @0x525BAF, the Player_InitPlayer call @0x525BBC; EventAction_Dispatch case 35 - the
//  g_LocalPlayerEntity test @0x4546E7..0x4546F1; Entity_SpawnFromAnimSlotProperty - Flags |=
//  0x101 @0x43C508; Server_OnPlayerJoin - the clear @0x51A7DA; Game_RestartRoundSP - the
//  Game_StartMission call @0x5263DB; Game_DestroyAllEntitiesAndReset @0x5235C0 (Entity_Destroy
//  called @0x5235FA, no store to g_LocalPlayerEntity); Player_InitPlayer - the memset called
//  @0x4E1602; EventAction_Dispatch case 35 - Sound_PlayAtPosition called @0x45470C]
void test_the_local_body_is_born_at_the_round_init() {
	const std::map<std::string, std::string> files;
	for (const bool restart : {false, true}) {
		auto m = std::make_unique<SpMission>();
		m->kernel->world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
		bool body_ahead_of_round_init = true;
		ms::MissionKernel *kernel = m->kernel.get();
		m->after_bringup = [kernel, &body_ahead_of_round_init] {
			const std::function<void()> round_init = kernel->round_init;
			CHECK(static_cast<bool>(round_init));
			kernel->round_init = [kernel, &body_ahead_of_round_init, round_init] {
				body_ahead_of_round_init = kernel->local.has_local_player();
				if (round_init) round_init();
			};
		};
		CHECK(m->boot(files, restart));
		w::World &world = m->world();
		// Absent through the PreMission pass: the pass ran its ShowWinSubgoal (the objective
		// bit) and played no NEW_GOAL.
		CHECK(!body_ahead_of_round_init);
		CHECK((world.script.subgoals.show_win & (1u << 1)) != 0);
		int new_goals = 0;
		for (const w::ReadyFireSound &ready : world.out.fire_sounds.drain())
			if (ready.set_name == "NEW_GOAL") ++new_goals;
		CHECK(new_goals == 0);
		// The boot spawned none; the round init followed the pass.
		const std::vector<std::string> &trace = m->kernel->boot_trace;
		const auto at = [&trace](const char *name) {
			return static_cast<size_t>(std::find(trace.begin(), trace.end(), name) - trace.begin());
		};
		CHECK(at("spawn_local_player") == trace.size());
		CHECK(at("premission") < at("round_init") && at("round_init") < trace.size());
		// Present and hidden past the round init: the own slot's body, under the round
		// init's 620-tick protection, the only one.
		const im::NapiNPConnection *own = nullptr;
		for (const im::NapiNPConnection &conn : m->role->state.host_owner.ctx.np_protocol.connection_list)
			if (conn.type == im::NapiNPConnection::kTypeClientSide) own = &conn;
		CHECK(own != nullptr);
		const w::Entity *body = world.registry.get(world.cached.local_player);
		CHECK(body != nullptr);
		if (own == nullptr || body == nullptr) continue;
		CHECK(own->link.owned_entity == world.cached.local_player);
		CHECK((body->flags & w::kEntityFlagCarried) != 0);
		CHECK(body->damage_state == 620);
		int bodies = 0;
		world.registry.for_each_in_pool(0, [&](const w::Entity &e) {
			if (e.owner_connection_id == own->connection_id) ++bodies;
		});
		CHECK(bodies == 1);
		// The own join (the start's own 0x0B, the first frame's receive pump) clears the bit,
		// out of a session for good.
		m->frames(2);
		body = world.registry.get(world.cached.local_player);
		CHECK(body != nullptr && (body->flags & w::kEntityFlagCarried) == 0);
	}
}

} // namespace

int main() {
	const std::map<std::string, std::string> files;

	// --- the mission start's cine legs -----------------------------------------
	// A first start flags the cine running over an empty timeline (the
	// <mission>.cin intro leg; no shipped mission has one); the first render
	// pass stops it without drawing, so no cine frame has been drawn yet.
	// [orig: Game_StartMission @0x525DA8..0x525DD6; Cine_StartScriptPlayback @0x578296;
	//  Cine_RenderOrEndPlayback @0x575A6B]
	{
		SpMission m;
		CHECK(m.boot(files, /*restart=*/false));
		const w::EpilogCine &cine = m.world().epilog;
		CHECK(cine.active);
		CHECK(cine.frame == 0);
		CHECK(cine.mode == w::EpilogCineMode::None);
		CHECK(cine.events.empty());
		CHECK(!cine.screen_active);
		CHECK(!cine.world_sound_parked);
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
		// The SP tail stops the playing dialog line ahead of its Dialog_ResetAll
		// [orig: DialogAudio_PlayNextChunkOrStop(0) @0x51694B]; the WIN epilog's
		// start shuts no audio channel down (only the LOSE cine's does).
		CHECK(world.out.dialog_line_stop);
		CHECK(!world.out.audio_channels_shutdown);
		// The SP tail parks the world-sound volume option until the next mission
		// start's cine release [orig: sub_5280B0 called @0x516958].
		CHECK(cine.world_sound_parked);
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
		// frames; a line fades in over its fade_in frames [orig: Cine_AdvanceTimelineFrame;
		// CCineEventImageFade_Step; CCineEventFade_StepAlpha @0x5712D0; CCineEventLetterbox_Finish; CCineEventTextFade_Step].
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
		// @0x5735d0, its step CineNode_ColorFadeUpdate @0x5736fe].
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
		// Parked through the whole end screen; the mission start's cine release puts
		// the option back [orig: Game_StartMission @0x525DA8 -> Cine_ReleaseAll
		// @0x577940 -> sub_5280D0].
		CHECK(cine.world_sound_parked);
		cine.mission_start(false, false, world);
		CHECK(!cine.world_sound_parked);
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
		// The playing line stops ahead of that reset [orig: @0x51694B], and the
		// world-sound volume option parks [orig: sub_5280B0 called @0x516958].
		CHECK(world.out.dialog_line_stop);
		CHECK(cine.world_sound_parked);
		CHECK(world.out.effects.count("round_end") == 1);
		// Cine_StartPlayback: the 100-frame letterbox and the edit fade.
		CHECK(cine.mode == w::EpilogCineMode::Lose && cine.lose_state == 0);
		CHECK(cine.events.size() == 2);
		// Cine_StartPlayback shuts the audio channels down for the embedder
		// [orig: Cine_StartPlayback @0x577840 - the call @0x577853].
		CHECK(world.out.audio_channels_shutdown);
		world.out.audio_channels_shutdown = false; // the embedder took it
		CHECK(find_event(cine, w::CineEventKind::EditFade) != nullptr);
		// No cine frame has rendered in this process yet: the first dispatch
		// enters state 1 and its render draws one; the second builds.
		(void)m.frame();
		CHECK(cine.lose_state == 1 && !cine.screen_active && cine.frame_drawn);
		CHECK(!world.out.audio_channels_shutdown);
		CHECK(world.script_may_advance());
		// The build's Dialog_ResetAll clears the table again [orig:
		// Cinematic_EpilogUpdate @0x5747DA].
		world.script.dialog.enqueue(audio::dialog_name_of(6), {});
		(void)m.frame();
		CHECK(cine.screen_active && cine.lose_state == 2);
		// The build shuts the audio channels down again [orig: Cinematic_EpilogUpdate
		// @0x5747E6].
		CHECK(world.out.audio_channels_shutdown);
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
		CHECK(im::round_over_key(view, *m.role, 'X', 'R') == (k::kArm | k::kConsumed));
		CHECK(world.mission_exit_reason == 0);
		CHECK(im::round_over_key(view, *m.role, 'R', 'R') ==
				(k::kArm | k::kConsumed | k::kRestart));
		CHECK(!cine.screen_active);
		CHECK(world.mission_exit_reason == 4);
		CHECK(im::main_frame_exit(world.mission_exit_reason, false, true) ==
				im::MainFrameExit::RestartRoundSP);
		// The session reports the exit on its next frame.
		const im::FrameOutcome out = m.frame();
		CHECK(out.status == im::FrameStatus::SessionLost);
	}

	// --- the lose cine's edit fade scales the bank's smoothing history ---------
	{
		// The fade (start 0, one frame) scales the main loop's frame-time
		// smoothing history by 1.66f at its start (timeline frame 0) and by
		// 0.60240966f at its finish (frame 1), while the cine runs; the session
		// applies the scales after the tick and clears the list (world::
		// TickAccumulator::scale_smoothing_history carries the witness).
		// [orig: Cine_AdvanceTimelineFrame @0x56FFC4 (vtable+4 on the start frame)
		//  / @0x56FFE5 (vtable+0x10 on the last); CCineEventEditFade_Start @0x5726C9;
		//  CCineEventEditFade_Finish @0x572775]
		SpMission m;
		CHECK(m.boot(files, false));
		m.frames(4);
		w::World &world = m.world();
		world.process_round_end(2);
		CHECK(world.out.frame_time_scales.empty());
		world.epilog.update(world);
		CHECK(world.out.frame_time_scales.size() == 1 &&
				world.out.frame_time_scales[0] == w::kEpilogEditFadeStartScale);
		world.epilog.update(world);
		CHECK(world.out.frame_time_scales.size() == 2 &&
				world.out.frame_time_scales[1] == w::kEpilogEditFadeFinishScale);
		world.epilog.update(world);
		CHECK(world.out.frame_time_scales.size() == 2);
		(void)m.frame();
		CHECK(world.out.frame_time_scales.empty());
	}

	// --- the round init's dialog reset -----------------------------------------
	{
		// The local player's (re)deploy runs round init, which clears the
		// world's dialog table [orig: Server_DeployPlayer @0x5178aa ->
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
		CHECK(im::round_over_key(view, *m.role, 27, 'R') == 0);
		m.world().process_round_end(2);
		CHECK(im::round_over_key(view, *m.role, 27, 'R') == (k::kArm | k::kConsumed | k::kExit));
		CHECK(m.world().mission_exit_reason == 1);
		// The pure rule: a session's board arm takes PgUp / PgDn alone and lets
		// every other key, ADVANCED's 'A' included, go on to the chain's tail; the
		// co-op game type takes every key [orig: Input_HandleSpecialKeys
		// @0x49c7d5..0x49c8e2].
		hud::HudRoundOverKeyInput in;
		in.vk = 'R';
		in.in_session = true;
		in.game_type = 0x10000; // TDM
		CHECK(hud::hud_round_over_key(in) == k::kArm);
		in.vk = 0x21; // VK_PRIOR [orig: @0x49c802]
		CHECK(hud::hud_round_over_key(in) == (k::kArm | k::kConsumed));
		in.vk = 0x22; // VK_NEXT [orig: @0x49c82c]
		CHECK(hud::hud_round_over_key(in) == (k::kArm | k::kConsumed));
		in.vk = 'A'; // ADVANCED [orig: @0x49c859 -> the tail @0x49c864]
		CHECK(hud::hud_round_over_key(in) == k::kArm);
		in.vk = 27; // ESC: the binding scan's [orig: @0x49c859 -> the tail]
		CHECK(hud::hud_round_over_key(in) == k::kArm);
		in.vk = 'R';
		in.game_type = 0x10020; // co-op
		CHECK(hud::hud_round_over_key(in) == (k::kArm | k::kConsumed | k::kRestart));
		in.vk = 0x21; // co-op: the page keys are taken with every other key [orig: @0x49c8d5]
		CHECK(hud::hud_round_over_key(in) == (k::kArm | k::kConsumed));
		in.vk = 'R';
		in.game_type = 0x30020; // co-op with bit 0x20000: the board arm
		CHECK(hud::hud_round_over_key(in) == k::kArm);
		// In a session the leg's exits are the role's: a co-op session's ESC and
		// RESTART store 1 and 4 on the role's g_MissionExitReason (the listen
		// host's world here), and the board arm stores nothing [orig:
		// Input_HandleSpecialKeys @0x49c8e2 / @0x49c8ad; the session test
		// @0x49c7d5..0x49c7fd].
		{
			SpMission c;
			CHECK(c.boot(files, false));
			c.world().process_round_end(2);
			c.world().rules.mp_session = true;
			c.world().match.live_rules().game_type = 0x10000; // TDM: the board arm
			CHECK(im::round_over_key(c.view(), *c.role, 27, 'R') == k::kArm);
			CHECK(im::round_over_key(c.view(), *c.role, 0x22, 'R') == (k::kArm | k::kConsumed));
			CHECK(c.world().mission_exit_reason == 0);
			c.world().match.live_rules().game_type = 0x10020; // co-op
			CHECK(im::round_over_key(c.view(), *c.role, 'X', 'R') == (k::kArm | k::kConsumed));
			CHECK(c.world().mission_exit_reason == 0);
			CHECK(im::round_over_key(c.view(), *c.role, 27, 'R') ==
					(k::kArm | k::kConsumed | k::kExit));
			CHECK(c.world().mission_exit_reason == 1);
			CHECK(im::round_over_key(c.view(), *c.role, 'R', 'R') ==
					(k::kArm | k::kConsumed | k::kRestart));
			CHECK(c.world().mission_exit_reason == 4);
		}
		// The main frame's router [orig: @0x526806..0x526867].
		CHECK(im::main_frame_exit(0, false, true) == im::MainFrameExit::None);
		CHECK(im::main_frame_exit(8, false, true) == im::MainFrameExit::GameLoop);
		CHECK(im::main_frame_exit(4, true, false) == im::MainFrameExit::GameLoop);
		CHECK(im::main_frame_exit(4, true, true) == im::MainFrameExit::PostMenu);
		CHECK(im::main_frame_exit(12, true, false) == im::MainFrameExit::PostMenu);
	}

	// --- the special-key chain: one key's walk over the role -------------------
	{
		// One else-if chain (inmatch::special_key_chain): the open quit dialog leads (its YES
		// runs Exit Mission, its RESTART out of a session queues event 12 for the default
		// spawn, a key it leaves goes to the tail); an open menu takes a digit, closing the
		// menu, and sends any other key past the round-over arm to the tail; once the round is
		// over every key is the round-over arm's out of a session, RESTART re-running the
		// splash and queueing event 12; in a session the Tab board takes the page keys and the
		// status page its keys; the tail's deploy keys queue their picks and the help and
		// briefing pages take PgUp / PgDn.
		// [orig: Input_HandleSpecialKeys @0x49c5c0 - the dialog @0x49c5e6..0x49c6d1
		//  (Input_QueueEvent(12, 0) @0x49c658), the Emotes arm @0x49c737..0x49c77c, the
		//  round-over arm @0x49c7cf (Input_QueueEvent(12, 0) @0x49c8a3), the board
		//  @0x49c910..0x49c95f, the status page @0x49c960..0x49c9c7, the deploy tail
		//  @0x49c9c9..0x49ca97, the help and briefing pages @0x49ca9e..0x49cb53]
		const w::SpawnZoneRegistry zones{};
		controls::PlayerActions actions;
		hud::HudToggleState toggles;
		const auto key = [&](SpMission &mission, int vk) {
			im::SpecialKeyInput in;
			in.vk = vk;
			in.deploy_vk = vk;
			return im::special_key_chain(mission.view(), *mission.role, toggles, actions, zones, in);
		};
		SpMission m;
		CHECK(m.boot(files, false));
		// Nothing open, the round live: no leg takes a letter or a page key.
		im::SpecialKeyOutcome out = key(m, 'Q');
		CHECK(!out.taken && out.pick < 0 && !out.digit_pick && !out.quit_confirmed);
		CHECK(!key(m, 0x22).taken);
		// The quit dialog's YES, then its RESTART out of a session.
		toggles.quit_dialog_open = true;
		out = key(m, 'Y');
		CHECK(out.taken && out.quit_confirmed && out.pick < 0);
		out = key(m, 'R');
		CHECK(out.taken && out.pick == 0 && !out.quit_confirmed && !out.restart_splash);
		// A key the open dialog leaves reaches the tail: the open help's PgDn.
		toggles.help_open = true;
		out = key(m, 0x22);
		CHECK(out.taken && out.page == im::SpecialKeyPage::Help && out.page_forward);
		toggles.help_open = false;
		toggles.briefing_mode = hud::kBriefingModeOpen;
		out = key(m, 0x21);
		CHECK(out.taken && out.page == im::SpecialKeyPage::Briefing && !out.page_forward);
		toggles.briefing_mode = 0;
		// The dialog's NO closes it.
		CHECK(key(m, 'N').taken && !toggles.quit_dialog_open);
		// The Emotes menu's digit is its pick (key 0 picks 10) and closes it.
		toggles.emotes_menu_open = true;
		out = key(m, '0');
		CHECK(out.taken && out.digit_pick && out.digit_pick->action == controls::PlayerAction::EmotePick &&
				out.digit_pick->value == 10);
		CHECK(!toggles.emotes_menu_open);
		// Once the round is over the open menu still sends a non-digit past the round-over
		// arm: ESC under it stores no exit.
		m.world().process_round_end(2);
		toggles.emotes_menu_open = true;
		out = key(m, 27);
		CHECK(!out.taken && m.world().mission_exit_reason == 0);
		toggles.emotes_menu_open = false;
		// With the menu shut the round-over arm takes every key out of a session; RESTART
		// re-runs the splash, queues event 12 and stores the restart's exit.
		out = key(m, 'Q');
		CHECK(out.taken && out.pick < 0 && !out.restart_splash);
		out = key(m, 'R');
		CHECK(out.taken && out.pick == 0 && out.restart_splash);
		CHECK(m.world().mission_exit_reason == 4);

		// In a session: the Tab board's page keys, then the status page's on the authority's
		// status view, then the tail's deploy keys for a dead player.
		SpMission c;
		CHECK(c.boot(files, false));
		c.world().rules.mp_session = true;
		toggles.scoreboard_open = true;
		out = key(c, 0x21);
		CHECK(out.taken && out.page == im::SpecialKeyPage::Scoreboard && !out.page_forward);
		toggles.scoreboard_open = false;
		CHECK(!key(c, 0x0D).taken);
		toggles.server_status_view = true;
		out = key(c, 0x0D);
		CHECK(out.taken && out.page == im::SpecialKeyPage::None && out.pick < 0);
		toggles.server_status_view = false;
		w::Entity *local = c.world().registry.get(c.world().cached.local_player);
		CHECK(local != nullptr);
		if (local != nullptr) local->flags |= w::kEntityFlagDead;
		out = key(c, 'X');
		CHECK(out.taken && out.pick == 0);
		out = key(c, ' ');
		CHECK(out.taken && out.pick == hud::kHudDeployKeyAutoTeam);
		CHECK(!key(c, 'Q').taken);
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
		CHECK(im::round_over_key(played->view(), *played->role, 'R', 'R') ==
				(hud::hud_round_over::kArm | hud::hud_round_over::kConsumed |
						hud::hud_round_over::kRestart));
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
			CHECK(w::mission_yaw_whole_deg(again->heading) == w::mission_yaw_whole_deg(fresh_player->heading));
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
	test_the_local_body_is_born_at_the_round_init();
	if (failures == 0) std::printf("sp_mission_lifecycle: OK\n");
	return failures == 0 ? 0 : 1;
}
