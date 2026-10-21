#include <runtime/inmatch/remote_admin.h>

#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_settings.h> // mission_session_title
#include <runtime/inmatch/joiner_role.h>   // JoinerRole::quit_to_menu (input action 3)
#include <runtime/inmatch/mission_exit.h>
#include <runtime/world/epilog_cine.h> // kWorldMissionExitQuit
#include <runtime/world/world.h>

namespace opennova::inmatch {

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
	// The MainMenu mode's frame [orig: Menu_UpdateFrame @0x552901].
	if (!frame.mission) return AdminScene::MainMenu;
	// The Game Loop's frame under its in-game menu [orig: Input_HandleActionBinding @0x49B3B6;
	//  Game_ProcessMainFrame @0x5268F1].
	if (frame.session_state == State::Paused) return AdminScene::GameLoop;
	// The blocking load, the held start, the teardown [orig: GameLoop_Init @0x526377;
	//  PostMenu_RouteMissionExit @0x568460].
	return std::nullopt;
}

bool remote_admin_idle_frame_exits(const RemoteAdminIdleFrame &frame) {
	// [orig: Game_ProcessMainFrame @0x52685E (`cmp eax, esi`), @0x526860 (zero reaches the pump)]
	return remote_admin_idle_scene(frame) == AdminScene::GameLoop && frame.mission_exit_reason != 0;
}

void RemoteAdminConsoles::bind(const RemoteAdminBinding &binding, AdminConsole::Seams seams) {
	console_.reset();
	// The session's end into the menus (a mission's exit, single player's included, or a failed
	// join) destroys the session, which zeroes g_GameType; g_MapFileName and g_SessionTitle keep
	// the last session's. A rotation's next mission skips the destroy.
	// [orig: PostMenu_RouteMissionExit @0x568683 -> CNapiGameSession_FullDestroy @0x4C96A0
	//  (ebp zeroed @0x4C96ED, g_GameType = 0 @0x4C977A); the failed join's
	//  UI_JoinSelectedSession @0x569F06, MultiPlayer_JoinSessionStateMachine @0x56A3E3 / @0x56A5A0;
	//  the rotation's PreMenu push @0x568632]
	const bool had_session = bound_.host != nullptr || bound_.client != nullptr || bound_.world != nullptr;
	const bool has_session = binding.host != nullptr || binding.client != nullptr || binding.world != nullptr;
	if (had_session && !has_session) process_mission_.game_type = 0;
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
		ctx = &joined_;
	}
	seams.host_client = binding.client;
	// Input action 3: exit reason 1 where the running role reads it (a host context's store, else
	// the joiner's, whose user leave sends its goodbye inside the action, else the single-player
	// world). [orig: Input_HandleActionBinding case 3 @0x49AF26, @0x49AF38..0x49AF42]
	if (binding.host != nullptr) {
		NapiNPServerCtx *host = binding.host;
		seams.quit_to_menu = [host] { host->mission_exit_reason = kMissionExitQuit; };
	} else if (binding.joiner != nullptr) {
		JoinerRole *joiner = binding.joiner;
		seams.quit_to_menu = [joiner] { joiner->quit_to_menu(); };
	} else if (binding.world != nullptr) {
		world::World *world = binding.world;
		seams.quit_to_menu = [world] { world->mission_exit_reason = world::kWorldMissionExitQuit; };
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
		process_mission_.session_elapsed_ms = host->np_protocol.host_run_duration_ms;
		if (host->world != nullptr) process_mission_.tod_fixed24 = host->world->weather.tod_fixed24;
	} else if (const ClientRuntime *client = bound_.client) {
		// [orig: Game_ApplySessionSettingsToGlobals @0x552145..0x55219E at the joiner's own
		//  mission start, over the final 0x64's copy @0x4324F5]
		process_mission_.session_title = mission_session_title(bound_.text_override, client->map_file());
		process_mission_.map_file = client->map_file();
		process_mission_.game_type = client->game_type();
		// g_SessionStatus from the joined session's S2C 0x58; g_EnvCurTimeFixed24 the joiner's
		// world clock, which the S2C 0x0A sets and the weather tick advances.
		// [orig: NapiNPClientMsg_SessionStatus @0x4228CA; NapiNPClientMsg_0x00A @0x4302AE;
		//  Environment_UpdateWeatherTick @0x57E9B8]
		process_mission_.session_elapsed_ms = client->session_elapsed_ms();
		if (bound_.world != nullptr) process_mission_.tod_fixed24 = bound_.world->weather.tod_fixed24;
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
