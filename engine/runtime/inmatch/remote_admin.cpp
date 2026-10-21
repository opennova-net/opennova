#include <runtime/inmatch/remote_admin.h>

#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_config.h>   // host_screen_from_block (the joiner's applied settings)
#include <runtime/inmatch/host_settings.h> // mission_session_title
#include <runtime/inmatch/joiner_role.h>   // JoinerRole::quit_to_menu (input action 3)
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_server_ctx.h> // session_uptime_ms (UP-TIME)
#include <runtime/world/epilog_cine.h> // kWorldMissionExitQuit
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// The settings globals GET GAMESETTINGS prints are zero in the process image; no writer runs
// before a mission start's apply (Game_ApplySessionSettingsToGlobals' four callers) or an admin
// SET. [orig: g_ScoreLimit @0x24D2134 .. dword_24D2284 and g_RulesFlags @0x24D1E34, zero in
// the image; Game_ApplySessionSettingsToGlobals @0x551500 called by Game_StartMission @0x524662,
// SinglePlayer_StartMission @0x561C28, CNapiGameSession_BuildAndCreateSession @0x5695B8,
// UI_JoinSelectedSession @0x569B07]
void zero_settings_globals(GameConfig &c) {
	c.auto_balance_enabled = 0;
	c.voting_enabled = 0;
	c.voting_percent = 0.0f;
	c.voting_min_players = 0;
	c.change_team_interval_seconds = 0;
	c.change_team_penalty_seconds = 0;
	c.capture_duration_seconds = 0;
	c.start_delay = 0;
	c.do_min_ping_check = 0;
	c.min_ping = 0;
	c.do_max_ping_check = 0;
	c.max_ping = 0;
	c.max_friendly_kills = 0;
	c.round_time_limit_minutes = 0;
	c.time_limit_minutes = 0;
	c.score_limit = 0;
	c.max_score = 0;
	c.fat_bullets = 0;
	c.one_shot_kill = 0;
	c.mp_attributes = 0;
}

} // namespace

RemoteAdminConsoles::RemoteAdminConsoles() {
	zero_settings_globals(session_less_.config);
}

uint16_t remote_admin_listen_port(const gamecfg::GameCfg &cfg) {
	// [orig: Game_InitSubsystems @0x4A72C7, the movzx of the word]
	return cfg.remote_admin_port;
}

std::string admin_log_file_text(std::string_view line) {
	// [orig: CAdminServer_Construct @0x402C84, the text-mode fopen]
	std::string text;
	text.reserve(line.size() + 1);
	for (const char c : line) {
		if (c == '\n') text += '\r';
		text += c;
	}
	return text;
}

std::optional<AdminScene> remote_admin_idle_scene(const RemoteAdminIdleFrame &frame) {
	// The PreMenu mode's join [orig: MultiPlayer_JoinSessionStateMachine @0x56A320].
	if (frame.join_pending) return std::nullopt;
	// A menu frame inside ConnectOrHost's play or host wait [orig: CNapiGameSession_ConnectOrHost
	//  @0x4D4F10 - @0x4D5180..0x4D519D, @0x4D54C0..0x4D54EE].
	if (frame.novaworld_wait) return std::nullopt;
	// The MainMenu mode's frame [orig: Menu_UpdateFrame @0x552901].
	if (!frame.mission) return AdminScene::MainMenu;
	// The Game Loop's frame under its in-game menu [orig: Input_HandleActionBinding @0x49B3B6;
	//  Game_ProcessMainFrame @0x5268F1].
	if (frame.session_state == State::Paused) return AdminScene::GameLoop;
	// The blocking load, the held start, the teardown [orig: GameLoop_Init @0x526377;
	//  PostMenu_RouteMissionExit @0x568460].
	return std::nullopt;
}

void input_action_exit(const RemoteAdminBinding &binding) {
	if (binding.host != nullptr)
		binding.host->mission_exit_reason = kMissionExitQuit;
	else if (binding.joiner != nullptr)
		binding.joiner->quit_to_menu();
	else if (binding.world != nullptr)
		binding.world->mission_exit_reason = world::kWorldMissionExitQuit;
}

bool remote_admin_idle_frame_exits(const RemoteAdminIdleFrame &frame) {
	// [orig: Game_ProcessMainFrame @0x52685E (`cmp eax, esi`), @0x526860 (zero reaches the pump)]
	return remote_admin_idle_scene(frame) == AdminScene::GameLoop && frame.mission_exit_reason != 0;
}

void RemoteAdminConsoles::session_destroyed() {
	// [orig: CNapiGameSession_FullDestroy @0x4C96A0 (ebp zeroed @0x4C96ED, g_GameType = 0
	//  @0x4C977A; g_MapFileName and g_SessionTitle untouched)]
	process_mission_.game_type = 0;
}

void RemoteAdminConsoles::route_mission_exit(int32_t reason) {
	// [orig: PostMenu_RouteMissionExit @0x568460 -> CNapiGameSession_FullDestroy @0x568683]
	if (inmatch::route_mission_exit(reason, /*novaworld=*/false).destroys_session) session_destroyed();
}

void RemoteAdminConsoles::bind(const RemoteAdminBinding &binding, AdminConsole::Seams seams) {
	console_.reset();
	// Single player's console is the session-less one over its world; its mission start applied
	// the block. [orig: SinglePlayer_StartMission @0x561C28 -> Game_ApplySessionSettingsToGlobals]
	const bool single_player = binding.host == nullptr && binding.client == nullptr && binding.world != nullptr;
	session_less_.world = single_player ? binding.world : nullptr;
	if (single_player && !(bound_ == binding) && seams.config_block != nullptr)
		session_less_.config = host_screen_from_block(*seams.config_block).config;
	bound_ = binding;
	NapiNPServerCtx *ctx = binding.host;
	if (ctx == nullptr && binding.client != nullptr) {
		// A joiner: connection mode 2, inside the joined session.
		// [orig: UI_EnumerateAndJoinSession @0x56A05B -> CGameSession_SetConnectionMode
		//  @0x4C4A0F (is_client 1, is_host 0)]
		joined_ = NapiNPServerCtx{};
		joined_.connection_mode = ConnectionMode::ClientOnly;
		joined_.is_authority = 0;
		joined_.is_mp_session_peer = 1;
		joined_.is_in_session = 1;
		// Its settings globals are what its own mission start copied from its process cfg
		// block (the copies run unconditionally, a joiner's start included); its slot table
		// is empty (the start's clear leaves it unallocated off the authority).
		// [orig: Game_ApplySessionSettingsToGlobals @0x551B6D..0x551DF1; Server_InitNewRoundState
		//  @0x51C939 (PlayerSlotTable_ClearEntryArray), @0x51C946 (the authority's allocation)]
		if (seams.config_block != nullptr) joined_.config = host_screen_from_block(*seams.config_block).config;
		ctx = &joined_;
	}
	seams.host_client = binding.client;
	// Input action 3 (input_action_exit) [orig: Input_HandleActionBinding case 3 @0x49AF26,
	//  @0x49AF38..0x49AF42].
	if (binding.host != nullptr || binding.joiner != nullptr || binding.world != nullptr)
		seams.quit_to_menu = [binding] { input_action_exit(binding); };
	seams.process_mission = &process_mission_;
	console_ = std::make_unique<AdminConsole>(ctx != nullptr ? *ctx : session_less_, std::move(seams));
}

void RemoteAdminConsoles::refresh_process_mission() {
	if (const NapiNPServerCtx *host = bound_.host) {
		process_mission_.session_title = host->session_title;
		process_mission_.map_file = host->config.mission_file;
		process_mission_.game_type =
				host->world != nullptr ? host->world->match.rules().game_type : host->config.game_type;
		// UP-TIME: the round-state init's stamp to now [orig: Server_BuildStatusReport
		//  @0x530B84..0x530B8C].
		process_mission_.session_elapsed_ms = session_uptime_ms(*host);
		if (host->world != nullptr) process_mission_.tod_fixed24 = host->world->weather.tod_fixed24;
		process_mission_.rules_word =
				host->world != nullptr ? host->world->rules.mpattrib : host->config.mp_attributes;
		process_mission_.round_ticks_remaining =
				host->world != nullptr ? host->world->match.remaining_ticks() : 0;
		// The session's settings globals stay the process's after it ends.
		session_less_.config = host->config;
	} else if (const ClientRuntime *client = bound_.client) {
		// [orig: Game_ApplySessionSettingsToGlobals @0x552145..0x55219E at the joiner's own
		//  mission start, over the final 0x64's copy @0x4324F5]
		process_mission_.session_title = mission_session_title(bound_.text_override, client->map_file());
		process_mission_.map_file = client->map_file();
		process_mission_.game_type = client->game_type();
		// g_SessionStatus from the joined session's S2C 0x58; g_EnvCurTimeFixed24 the joiner's
		// world clock, which the S2C 0x0A sets and the weather tick advances.
		// [orig: NapiNPClientMsg_SessionStatus @0x4228CA; NapiNPClientMsg_0x00A @0x4302AE;
		//  Environment_UpdateWeatherTick @0x57E9B0 (the read @0x57E9B8)]
		process_mission_.session_elapsed_ms = client->session_elapsed_ms();
		if (bound_.world != nullptr) process_mission_.tod_fixed24 = bound_.world->weather.tod_fixed24;
		// g_RulesFlags, the joined mission's S2C 0x64 word (the joiner's world keeps it), and
		// g_RoundTimeRemaining, the S2C 0x0A clock.
		// [orig: CAdminServer_HandleGet @0x403591..0x403A02 (g_RulesFlags), @0x4038C5
		//  (g_RoundTimeRemaining); NapiNPClientMsg_0x00A @0x430219..0x430235]
		if (bound_.world != nullptr) process_mission_.rules_word = bound_.world->rules.mpattrib;
		process_mission_.round_ticks_remaining = client->state().round_time_remaining_ticks;
		session_less_.config = joined_.config;
	} else if (const world::World *world = bound_.world) {
		// Single player: its world's live words (no host clock).
		process_mission_.session_elapsed_ms = 0;
		process_mission_.tod_fixed24 = world->weather.tod_fixed24;
		process_mission_.rules_word = world->rules.mpattrib;
		process_mission_.round_ticks_remaining = world->match.remaining_ticks();
	} else {
		// No session (the menus): every mission teardown cleared g_SessionStatus; the time of
		// day keeps the last mission's clock (no teardown writer).
		// [orig: Game_TeardownMission @0x5223FA (CPlayerStats_Init(&g_SessionStatus))]
		process_mission_.session_elapsed_ms = 0;
	}
}

// [orig: CAdminServer_DispatchCommand @0x406720]
bool RemoteAdminConsoles::dispatch(const AdminSession &session, std::string_view line,
		std::vector<std::string> &replies) {
	refresh_process_mission();
	return console_ != nullptr ? console_->dispatch(session, line, replies) : true;
}

std::string RemoteAdminConsoles::status_report() {
	refresh_process_mission();
	return console_ != nullptr ? console_->status_report() : std::string();
}

} // namespace opennova::inmatch
