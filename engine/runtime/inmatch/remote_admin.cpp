#include <runtime/inmatch/remote_admin.h>

#include <formats/gamecfg/game_cfg.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_settings.h> // mission_session_title
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

void RemoteAdminConsoles::bind(const RemoteAdminBinding &binding, AdminConsole::Seams seams) {
	console_.reset();
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
	// the joiner's client, else the single-player world); the leave's goodbye is the teardown's.
	// [orig: Input_HandleActionBinding case 3 @0x49AF26, @0x49AF38..0x49AF42]
	if (binding.host != nullptr) {
		NapiNPServerCtx *host = binding.host;
		seams.quit_to_menu = [host] { host->mission_exit_reason = kMissionExitQuit; };
	} else if (binding.client != nullptr) {
		ClientRuntime *client = binding.client;
		seams.quit_to_menu = [client] { client->set_mission_exit_reason(kMissionExitQuit); };
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
	} else if (const ClientRuntime *client = bound_.client) {
		// [orig: Game_ApplySessionSettingsToGlobals @0x552145..0x55219E at the joiner's own
		//  mission start, over the final 0x64's copy @0x4324F5]
		process_mission_.session_title = mission_session_title(bound_.text_override, client->map_file());
		process_mission_.map_file = client->map_file();
		process_mission_.game_type = client->game_type();
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
