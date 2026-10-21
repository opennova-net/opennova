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

namespace opennova::rtxt {
struct File;
}

namespace opennova::inmatch {

class JoinerRole;

// The admin server's log, by bare name in the working directory, truncated when the process
// starts. [orig: "admin_log.txt" @0x7C0860; CAdminServer_Construct @0x402C39, the fopen("w")
//  @0x402C84]
inline constexpr const char *kAdminLogFileName = "admin_log.txt";

// The listener's address: IPv4 alone, every interface (retail's socket is AF_INET bound to
// INADDR_ANY, so an IPv6 admin client is never accepted).
// [orig: CAdminServer_Listen @0x406E1F (socket(AF_INET, SOCK_STREAM)), @0x406E74 (sin_addr 0)]
inline constexpr const char *kAdminListenAddress = "0.0.0.0";

// The time of day the PLAYER_INFO screen's player preview stores on every draw (16:00 in the
// 24.8 fixed day), never restored: the menus' CURRENT TOD reads it once the preview has drawn.
// [orig: PlayerInfo_RenderPlayerPreview3D @0x5609C0 (the draw, event 1 @0x5609D7), the store
//  g_EnvCurTimeFixed24 = 0x10000000 @0x560AF5]
inline constexpr uint32_t kPlayerPreviewTodFixed24 = 0x10000000u;

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
	// The exit reason the running session's role reads (0: none).
	int32_t mission_exit_reason = 0;
	// The NovaWorld menu's ConnectOrHost leg awaiting its result (its play or host request): the
	// menu frame that runs it ends when the leg does.
	bool novaworld_wait = false;
};

// The scene such a frame pumps the admin server in, or none. Retail pumps it in two places only:
// the end of the Game Loop's main frame and the end of the MainMenu's menu frame, every one. Its
// PreMenu mode (a joiner's connect and verification, a host's session create) and Post Menu mode
// pump nothing, nor does GameLoop_Init's mission load, one blocking call: the admin's bytes wait
// in the socket until the next pumping frame. So a joiner's connect pumps nothing; with no mission
// the frame is the menus' (MainMenu, whatever world the process keeps); a paused single player is
// the Game Loop's frame with its in-game menu up (game.mnu opens inside the Game Loop, no mode
// change); any other state of a loaded mission (its load, a held start, its teardown) pumps
// nothing. Nor does a menu frame running the NovaWorld menu's play or host leg: each blocks inside
// the frame until its result or 60 s (the gate wait and the connect polls run frame by frame and
// pump). GET GAMESTATE's "Unknown" reply is therefore never sent.
// [orig: the two callers of CAdminServer_ProcessFrame, Game_ProcessMainFrame @0x5268F1 and
//  Menu_UpdateFrame @0x552901 (no early return ahead of it); the PreMenu mode's update
//  MultiPlayer_JoinSessionStateMachine @0x56A320 (its mode descriptor stru_83C9D0, pushed by
//  UI_HandleLANGameListEvent @0x556C35 and UI_HandleHostSessionStart @0x556EDC) and the Post
//  Menu's PostMenu_RouteMissionExit @0x568460 call neither; GameLoop_Init's Game_StartMission(0)
//  @0x526377; game.mnu opened by Input_HandleActionBinding @0x49B3B6; CAdminServer_HandleGet
//  @0x4033D0..0x40340B (the Menus / Game / Unknown compare); CNapiGameSession_ConnectOrHost
//  @0x4D4F10 - the host loop @0x4D5180..0x4D519D, the play loop @0x4D54C0..0x4D54EE, its one
//  caller UI_DispatchScreenEvent (the call @0x54F33B)]
std::optional<AdminScene> remote_admin_idle_scene(const RemoteAdminIdleFrame &frame);

// Whether such a frame leaves the mission instead of pumping: the Game Loop's frame under its
// in-game menu still runs its exit router, ahead of the pump, and a stored exit reason (an
// admin's GOTO MENUSTATE from the pause menu) pushes the router's mode and returns past the
// pump. The embedder routes the exit as a tick's terminal outcome does, with no tick run.
// [orig: Game_ProcessMainFrame @0x5267F8 (the g_MissionExitReason read), the router
//  @0x526803..0x526878 (a nonzero reason's Post Menu push @0x526867 and return), ahead of
//  the pump @0x5268F1 (CAdminServer_ProcessFrame @0x406F50)]
bool remote_admin_idle_frame_exits(const RemoteAdminIdleFrame &frame);

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
	// A joiner's role (its client is `client`): input action 3 runs its user leave.
	JoinerRole *joiner = nullptr;
	// The running mission's world (a single player's quit with no host context).
	world::World *world = nullptr;
	// The mount's text-override table (null: none): a joiner's session title.
	const rtxt::File *text_override = nullptr;
	uint64_t session = 0;

	bool operator==(const RemoteAdminBinding &o) const {
		return host == o.host && client == o.client && joiner == o.joiner && world == o.world &&
				text_override == o.text_override && session == o.session;
	}
};

// Where input action 3 runs in the frame. The admin's GOTO MENUSTATE runs in the admin pump,
// after the frame's exit test, so a listen host's own goodbye waits for the next frame's head
// receive and the server receive pump of that frame's tick takes it; the frame's input pass (the
// HUD quit dialog's yes, a console-queued code 3, the in-game CONFIRM_YES) runs after the frame
// head's receive, so the goodbye waits through the frame's tick for the mission teardown's pump.
// [orig: Game_ProcessMainFrame @0x5263F0 - the head's CNapiNetwork_PumpManagerReceive call
//  @0x526528, Server_TickUpdate's receive pump @0x51D895, the admin pump @0x5268F1;
//  Game_TeardownMission @0x522350 - the pump @0x522588] (D-NET-NEW-L8-21)
enum class ExitSite : uint8_t {
	Admin,
	Input,
};

// Input action 3, the exit (the admin's GOTO MENUSTATE, the in-game CONFIRM_YES, a console-queued
// code 3): exit reason 1 where the running role reads it, a host context's store, else the
// joiner's, whose user leave sends its CIDEMIS goodbye inside the action
// (JoinerRole::quit_to_menu), else the single-player world's. A host context with its own client
// (a listen host) disconnects that client as a user leave too (request_own_client_disconnect),
// its goodbye taken where `site` says.
// The in-game CONFIRM_YES and the HUD quit dialog's yes run it inline, so a joiner's goodbye
// leaves at the click (the game's quit legs call it ahead of their return to the menu).
// [orig: Input_HandleActionBinding case 3 @0x49AF26 (g_MissionExitReason = 1),
//  Sound_PlayInterfaceTriggerSet @0x49AF30 (the shell's tone), the disconnect
//  @0x49AF38..0x49AF42; UI_RegisterIngameCallbacks @0x555510 -> UI_IngameConfirmExitCommand
//  @0x555460 - Game_CloseInGameScreens @0x55546A, Input_HandleActionBinding(3) @0x555479;
//  the quit dialog's yes in Input_HandleSpecialKeys @0x49C5C0]
void input_action_exit(const RemoteAdminBinding &binding, ExitSite site);

// The admin server's command half for a process whose sessions come and go, as retail's
// dispatcher runs over the process's globals whatever the scene: the console over the binding's
// context with the embedder's seams, completed here with the process's own client
// (`host_client`) and GOTO MENUSTATE's input action 3 (`quit_to_menu`): exit reason 1 stored
// where the running role reads it, so the next frame's mission exit takes the router's reason-1
// arm (a host's teardown; a joiner's leave, whose CIDEMIS goodbye the action itself sends,
// JoinerRole::quit_to_menu). Rebinding drops the previous console.
// [orig: CAdminServer_DispatchCommand @0x406720 (no session test of its own);
//  CAdminServer_HandleStatus @0x402E30; CAdminServer_HandleGotoCommand @0x404A5D
//  (Input_HandleActionBinding(3)); Input_HandleActionBinding case 3 @0x49AF26
//  (g_MissionExitReason = 1), the disconnect @0x49AF38..0x49AF42]
class RemoteAdminConsoles final : public AdminCommandHandler {
public:
	// The process's settings globals start zeroed: no mission start has applied the block yet.
	RemoteAdminConsoles();

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

	// The session's destroy: g_GameType goes to 0; g_MapFileName and g_SessionTitle keep the
	// last session's. Retail destroys it on the Post Menu's destroy arm (route_mission_exit) and
	// on every pre-game exit to the MainMenu (the join or host machine's cancel and failures);
	// a rotation's next mission and the Post Menu's reset and error arms keep it.
	// [orig: CNapiGameSession_FullDestroy @0x4C96A0, its four callers PostMenu_RouteMissionExit
	//  @0x568683, UI_JoinSelectedSession @0x569F06, MultiPlayer_JoinSessionStateMachine @0x56A3E3
	//  (state 9, which PreMenu_OnAbortCancel @0x568CB4 and UI_PreGameMenuStateMachine's default
	//  arm @0x568B61 set) and @0x56A5A0; the rotation's PreMenu push @0x568632]
	void session_destroyed();
	// The Post Menu's router over the session's final exit reason, where the embedder routes it
	// to the menus: its destroy arm (inmatch::route_mission_exit's destroys_session) destroys the
	// session; the reset (2) and the error arms (5, 6, 7, 9..19, 20) keep g_GameType, so the
	// menus still print the last session's game type after a mapped disconnect, a NovaWorld
	// session's end or a CD / system / bad-mission exit.
	// [orig: PostMenu_RouteMissionExit @0x568460 (the error arms @0x5684A8..0x5685BD ->
	//  loc_5686DA..0x568715 skip CNapiGameSession_FullDestroy @0x568683)]
	void route_mission_exit(int32_t reason);
	// The PLAYER_INFO screen's player preview drew: the process time of day is its 16:00, which
	// the menus keep until a mission start sets the clock (a session's refresh reads its world).
	// [orig: PlayerInfo_RenderPlayerPreview3D @0x560AF5]
	void player_preview_drawn() { process_mission_.tod_fixed24 = kPlayerPreviewTodFixed24; }

	// The bound session's mission into the process words, before each line runs and on every
	// frame the embedder pumps: a host context's session title, mission file and game type; a
	// joiner's own mission start's session title over its connection's g_MapFileName, and its
	// g_GameType; the session clock, the time of day and GET GAMESETTINGS' live words; and the
	// session's settings globals into the menus' context. With no session (the menus) they keep
	// the last session's (a destroy zeroed the game type, session_destroyed; the teardown the
	// clock).
	void refresh_process_mission();

private:

	// The console's context with no host or joiner session: the menus', and single player's
	// (its world bound). Its config holds the process's settings globals: zero before the first
	// mission start, then the last session's.
	NapiNPServerCtx session_less_;
	// A joiner's context: the joined session's peer, not its authority.
	NapiNPServerCtx joined_;
	std::unique_ptr<AdminConsole> console_;
	RemoteAdminBinding bound_;
	AdminProcessMission process_mission_;
};

} // namespace opennova::inmatch
