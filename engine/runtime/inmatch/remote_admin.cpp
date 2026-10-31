#include <runtime/inmatch/remote_admin.h>

#include <formats/gamecfg/game_cfg.h>
#include <formats/rtxt/rtxt.h>
#include <net/admin/admin_protocol.h> // admin_log_redacted
#include <runtime/hud/game_text_lookup.h> // kGameTextTable
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_config.h>   // host_screen_from_block (the joiner's applied settings)
#include <runtime/inmatch/host_settings.h> // mission_session_title
#include <runtime/inmatch/joiner_role.h>   // JoinerRole::quit_to_menu (input action 3)
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/napi_np_server_ctx.h> // session_uptime_ms (UP-TIME)
#include <runtime/inmatch/napi_np_protocol.h> // request_own_client_disconnect (input action 3)
#include <runtime/mission/runtime_boot.h> // BootFileSource (process_game_text)
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
	const std::string redacted = admin_log_redacted(line); // D-NET-1472
	std::string text;
	text.reserve(redacted.size() + 1);
	for (const char c : redacted) {
		if (c == '\n') text += '\r';
		text += c;
	}
	return text;
}

std::function<std::string(std::string_view section, std::string_view key)> process_game_text(
		const mission::BootFileSource &files, std::shared_ptr<const rtxt::File> override_table) {
	// [orig: Game_InitSubsystems @0x4A6FE3..0x4A6FED]
	auto gametext = std::make_shared<rtxt::File>();
	std::vector<uint8_t> bytes;
	std::string parse_error;
	if (!files.valid() || !files.read_file(hud::kGameTextTable, bytes) ||
			!rtxt::parse(bytes.data(), bytes.size(), *gametext, parse_error))
		return {};
	// [orig: GameText_GetString @0x51EBD0 -> TextResource_FindEntryBySectionAndKey @0x75D250]
	return [gametext, override_table](std::string_view section, std::string_view key) {
		const rtxt::Entry *entry = rtxt::find_with_override(
				override_table.get(), *gametext, std::string(section), std::string(key));
		return entry != nullptr ? entry->text : std::string();
	};
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

void input_action_exit(const RemoteAdminBinding &binding, ExitSite site) {
	// A menu screen's confirmation and the admin's GOTO close the in-game screens ahead of the
	// action, dropping the DEATH screen's open latch with no close leg (ClientState::
	// close_in_game_screens); the input pass's own action 3 closes none.
	// [orig: DeathScreen_OnConfirmYes @0x55353A, UI_IngameConfirmExitCommand @0x55546A,
	//  CAdminServer_HandleGotoCommand @0x404A4E, each ahead of its action 3]
	if (site != ExitSite::Input && binding.client != nullptr)
		binding.client->view().state().close_in_game_screens();
	if (binding.host != nullptr) {
		binding.host->mission_exit_reason = kMissionExitQuit;
		// [orig: CNapiNetwork_DisconnectActiveConnection(&g_NapiNPCtx, "I.C:CIDEMIS") @0x49AF42
		//  on ctx+0xE60, the own client a listen host's create made (@0x4C9B9C)]
		// The queued action dispatches where a main input pass drains the queue after the exit:
		// the input pass's own, or the next frame's after the admin's with no menu screen up
		// (ExitSite carries the witness).
		const bool admin = site == ExitSite::Admin || site == ExitSite::AdminOverMenuScreen;
		request_own_client_disconnect(*binding.host,
				admin ? OwnGoodbye::NextReceivePump : OwnGoodbye::TeardownPump,
				/*queued_action_dispatches=*/site == ExitSite::Admin || site == ExitSite::Input);
	} else if (binding.joiner != nullptr)
		binding.joiner->quit_to_menu();
	else if (binding.world != nullptr)
		binding.world->mission_exit_reason = world::kWorldMissionExitQuit;
}

bool ingame_restart(world::World &world, ClientRuntime *client) {
	// [orig: UI_IngameRestartCommand @0x555410; the declaration carries the legs]
	if (world.rules.mp_session) return false;
	if (client != nullptr) client->view().state().close_in_game_screens();
	return world.ingame_restart_command();
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
	// [orig: PostMenu_RouteMissionExit @0x568460 -> CNapiGameSession_FullDestroy @0x4C96A0 (the
	//  call @0x568683)]
	if (inmatch::route_mission_exit(reason, /*novaworld=*/false).destroys_session) session_destroyed();
}

void RemoteAdminConsoles::bind(const RemoteAdminBinding &binding, AdminConsole::Seams seams) {
	console_.reset();
	// Single player's console is the session-less one over its world; its mission start applied
	// the block. [orig: SinglePlayer_StartMission @0x561C28 -> Game_ApplySessionSettingsToGlobals]
	const bool single_player = binding.host == nullptr && binding.client == nullptr && binding.world != nullptr;
	session_less_.world = single_player ? binding.world : nullptr;
	if (single_player && !(bound_ == binding) && seams.config_block != nullptr)
		session_less_.config = host_screen_from_block(*seams.config_block, *seams.config_title).config;
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
		if (seams.config_block != nullptr)
			joined_.config = host_screen_from_block(*seams.config_block, *seams.config_title).config;
		ctx = &joined_;
	}
	seams.host_client = binding.client;
	seams.joiner_world =
			binding.host == nullptr && binding.client != nullptr ? binding.world : nullptr;
	// Input action 3 (input_action_exit) after the in-game screens' close, whose request a menu
	// screen up at the GOTO takes at the next frame's screen pass (ExitSite::AdminOverMenuScreen)
	// [orig: CAdminServer_HandleGotoCommand @0x404A20 - Game_CloseInGameScreens @0x404A4E,
	//  Input_HandleActionBinding(3) @0x404A5D; Input_HandleActionBinding case 3 @0x49AF26,
	//  @0x49AF38..0x49AF42].
	if (binding.host != nullptr || binding.joiner != nullptr || binding.world != nullptr) {
		std::function<bool()> menu_screen_open = seams.menu_screen_open;
		seams.quit_to_menu = [binding, menu_screen_open] {
			input_action_exit(binding, menu_screen_open && menu_screen_open()
					? ExitSite::AdminOverMenuScreen : ExitSite::Admin);
		};
	}
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
		// [orig: NapiNPClientMsg_SessionStatus @0x4228C0 (@0x4228CA); NapiNPClientMsg_0x00A
		//  @0x42FEC0 (@0x4302AE);
		//  Environment_UpdateWeatherTick @0x57E9B0 (the read @0x57E9B8)]
		process_mission_.session_elapsed_ms = client->session_elapsed_ms();
		if (bound_.world != nullptr) process_mission_.tod_fixed24 = bound_.world->weather.tod_fixed24;
		// g_RulesFlags, the joined mission's S2C 0x64 word (the joiner's world keeps it), and
		// g_RoundTimeRemaining, the S2C 0x0A clock.
		// [orig: CAdminServer_HandleGet @0x403591..0x403A02 (g_RulesFlags), @0x40388D
		//  (g_RoundTimeRemaining); NapiNPClientMsg_0x00A @0x42FEC0 (@0x430219..0x430235)]
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
		// [orig: Game_TeardownMission @0x522350 (@0x5223FA, CPlayerStats_Init(&g_SessionStatus))]
		process_mission_.session_elapsed_ms = 0;
		// g_RoundTimeRemaining is the process's, as the last session's close handed it back
		// (D-NET-1266), or as a SET in the menus left it.
		// [orig: g_RoundTimeRemaining @0x24C1958; CAdminServer_HandleGet @0x40388D]
		if (process_ != nullptr)
			process_mission_.round_ticks_remaining = process_->round_time_remaining_ticks;
	}
}

void RemoteAdminConsoles::session_exit_frame(const RemoteAdminBinding &binding, AdminConsole::Seams seams) {
	// [orig: Game_ProcessMainFrame @0x52685E..0x526867 (the exit's Post Menu push, past
	//  CAdminServer_ProcessFrame @0x5268F1): the globals the exit frame wrote, whatever pumped]
	if (!bound_to(binding)) bind(binding, std::move(seams));
	refresh_process_mission();
}

// [orig: CAdminServer_DispatchCommand @0x406720]
bool RemoteAdminConsoles::dispatch(const AdminSession &session, std::string_view line,
		std::vector<std::string> &replies) {
	refresh_process_mission();
	const bool kept = console_ != nullptr ? console_->dispatch(session, line, replies) : true;
	// A joiner's SET writes the globals its own session reads, as retail's writes them in
	// place: g_RulesFlags, the joiner world's word (the mission data's copy, which the next
	// copy replaces, JoinerRole::stamp_mission_rules), and g_RoundTimeRemaining, its client's
	// S2C 0x0A clock (the next 0x0A's replaces it). A host context's SET wrote its world.
	// [orig: CAdminServer_HandleSetCommand @0x405A60 - the rule bits @0x405C80..0x405F86,
	//  GameTime @0x405E80..0x405E86; NapiNPClientMsg_HandleMissionDataChunk @0x432410 (the
	//  memcpy @0x432470); NapiNPClientMsg_0x00A @0x42FEC0 (@0x430219..0x430235)]
	if (bound_.host == nullptr && bound_.client != nullptr) {
		if (bound_.world != nullptr && bound_.world->rules.mpattrib != process_mission_.rules_word)
			apply_rules_word(*bound_.world, process_mission_.rules_word);
		bound_.client->state().round_time_remaining_ticks = process_mission_.round_ticks_remaining;
	}
	// The menus' SET GameTime writes the process's clock, which the next session's first
	// mission-start update reads (D-NET-1269).
	// [orig: CAdminServer_HandleSetCommand @0x405A60 - the store @0x405E86;
	//  Lobby_UpdateServerInfo's read @0x4FED57, called by Game_StartMission @0x5248F5]
	if (bound_.host == nullptr && bound_.client == nullptr && bound_.world == nullptr &&
			process_ != nullptr)
		process_->round_time_remaining_ticks = process_mission_.round_ticks_remaining;
	return kept;
}

std::string RemoteAdminConsoles::status_report() {
	refresh_process_mission();
	return console_ != nullptr ? console_->status_report() : std::string();
}

} // namespace opennova::inmatch
