#pragma once

// The process's remote-admin console, the half both embedders share: retail's game opens it in
// every Jointops.exe process (the listen host, a joiner, single player, the menus) when game.cfg's
// remote_admin_port is nonzero, and pumps it at the end of every main frame and every menu frame;
// opennova-serve opens it the same way. The wire is net/admin/admin_server.h's, the verbs
// admin_console.h's; the sockets stay the embedder's (apps/common AdminTcpServer, the game's
// AdminTcpPump). The witness record is docs/net/novaworld-net-re.md §6.9.
// [orig: Game_InitSubsystems @0x4A72B8..0x4A72D9 (admin.cfg, the port, CAdminServer_Listen);
//  Game_ProcessMainFrame @0x5268F1 and Menu_UpdateFrame @0x552901 (CAdminServer_ProcessFrame)]

#include <net/admin/admin_server.h>
#include <runtime/inmatch/admin_console.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/session.h> // State (the frame's session state)

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::world {
struct World;
}

namespace opennova::inmatch {

// The admin server's log, by bare name in the working directory, truncated when the process
// starts. [orig: "admin_log.txt" @0x7C0860; CAdminServer_Construct @0x402C39, the fopen("w")
//  @0x402C84]
inline constexpr const char *kAdminLogFileName = "admin_log.txt";

// The listener's address: IPv4 alone, every interface (retail's socket is AF_INET bound to
// INADDR_ANY, so an IPv6 admin client is never accepted).
// [orig: CAdminServer_Listen @0x406E1F (socket(AF_INET, SOCK_STREAM)), @0x406E74 (sin_addr 0)]
inline constexpr const char *kAdminListenAddress = "0.0.0.0";

// The listener's port: game.cfg's remote_admin_port as the 16-bit word it is stored as; 0 opens
// no listener. [orig: Game_InitSubsystems @0x4A72C7 (movzx of remoteAdminPort_A18),
//  @0x4A72CE..0x4A72D1 (the nonzero test), CAdminServer_Listen @0x4A72D9]
uint16_t remote_admin_listen_port(const gamecfg::GameCfg &cfg);

// One admin_log.txt line as retail's text-mode stream writes it: each "\n" reaches the file as
// CR LF. [orig: CAdminServer_Construct @0x402C84, fopen(.., "w"), a text-mode stream]
std::string admin_log_file_text(std::string_view line);

// A frame of the game process outside the running mission's main frame (whose frame leg pumps
// in the Game Loop scene after the session's tick), as the pump reads it.
struct RemoteAdminIdleFrame {
	// A mission's session exists (a loaded mission runtime), and its state.
	bool mission = false;
	State session_state = State::Unloaded;
	// A joiner's connect ahead of its world load (the preload).
	bool join_pending = false;
};

// The scene such a frame pumps the admin server in, or none. Retail pumps it in two places only:
// the end of the Game Loop's main frame and the end of the MainMenu's menu frame, every one. Its
// PreMenu mode (a joiner's connect and verification, a host's session create) and Post Menu mode
// pump nothing, nor does GameLoop_Init's mission load, one blocking call: the admin's bytes wait
// in the socket until the next pumping frame. So a joiner's connect pumps nothing; with no mission
// the frame is the menus' (MainMenu, whatever world the process keeps); a paused single player is
// the Game Loop's frame with its in-game menu up (game.mnu opens inside the Game Loop, no mode
// change); any other state of a loaded mission (its load, a held start, its teardown) pumps
// nothing. GET GAMESTATE's "Unknown" reply is therefore never sent.
// [orig: the two callers of CAdminServer_ProcessFrame, Game_ProcessMainFrame @0x5268F1 and
//  Menu_UpdateFrame @0x552901 (no early return ahead of it); the PreMenu mode's update
//  MultiPlayer_JoinSessionStateMachine @0x56A320 (its mode descriptor stru_83C9D0, pushed by
//  UI_HandleLANGameListEvent @0x556C35 and UI_HandleHostSessionStart @0x556EDC) and the Post
//  Menu's PostMenu_RouteMissionExit @0x568460 call neither; GameLoop_Init's Game_StartMission(0)
//  @0x526377; game.mnu opened by Input_HandleActionBinding @0x49B3B6; CAdminServer_HandleGet
//  @0x4033D0..0x40340B (the Menus / Game / Unknown compare)]
std::optional<AdminScene> remote_admin_idle_scene(const RemoteAdminIdleFrame &frame);

// What the process's console runs over: the running session's host context (a listen or Serve
// Only host, single player's in-process host), else a joiner's own client, whose console reads as
// the joined session's peer (retail's joiner sets connection mode 2: not the authority, a session
// peer, so CHAT SEND and CMD queue their C2S on its connection), else nothing (the menus). The
// identity is the session's: any change rebinds.
// [orig: CGameSession_SetConnectionMode @0x4C49F0 (mode 2: is_authority 0, is_mp_session_peer 1);
//  UI_EnumerateAndJoinSession @0x56A05B (the joiner's mode 2)]
struct RemoteAdminBinding {
	NapiNPServerCtx *host = nullptr;
	// The process's own client: the listen host's (its loopback) or the joiner's.
	ClientRuntime *client = nullptr;
	// The running mission's world (a single player's quit with no host context).
	world::World *world = nullptr;
	uint64_t session = 0;

	bool operator==(const RemoteAdminBinding &o) const {
		return host == o.host && client == o.client && world == o.world && session == o.session;
	}
};

// The admin server's command half for a process whose sessions come and go, as retail's
// dispatcher runs over the process's globals whatever the scene: the console over the binding's
// context with the embedder's seams, completed here with the process's own client
// (`host_client`) and GOTO MENUSTATE's input action 3 (`quit_to_menu`): exit reason 1 stored
// where the running role reads it, so the next frame's mission exit takes the router's reason-1
// arm (a host's teardown, a joiner's leave with its CIDEMIS goodbye). Rebinding drops the
// previous console.
// [orig: CAdminServer_DispatchCommand @0x406720 (no session test of its own);
//  CAdminServer_HandleStatus @0x402E30; CAdminServer_HandleGotoCommand @0x404A5D
//  (Input_HandleActionBinding(3)); Input_HandleActionBinding case 3 @0x49AF26
//  (g_MissionExitReason = 1), the disconnect @0x49AF38..0x49AF42]
class RemoteAdminConsoles final : public AdminCommandHandler {
public:
	void bind(const RemoteAdminBinding &binding, AdminConsole::Seams seams);
	// Whether the console is the one over `binding`.
	bool bound_to(const RemoteAdminBinding &binding) const {
		return console_ != nullptr && bound_ == binding;
	}
	AdminConsole *console() { return console_.get(); }

	bool dispatch(const AdminSession &session, std::string_view line,
			std::vector<std::string> &replies) override;
	std::string status_report() override;

	// The process's mission globals as the status report last read them.
	const AdminProcessMission &process_mission() const { return process_mission_; }

private:
	// The bound session's mission into the process words before a line runs: a host
	// context's mission file and game type, a joiner's connection's g_MapFileName and
	// g_GameType; with no session (the menus) they keep the last session's.
	void refresh_process_mission();

	NapiNPServerCtx session_less_;
	// A joiner's context: the joined session's peer, not its authority.
	NapiNPServerCtx joined_;
	std::unique_ptr<AdminConsole> console_;
	RemoteAdminBinding bound_;
	AdminProcessMission process_mission_;
};

} // namespace opennova::inmatch
