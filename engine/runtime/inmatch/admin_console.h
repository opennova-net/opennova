#pragma once

// The remote-admin console's command half: CAdminServer_DispatchCommand and its verb handlers
// over the authority's live state, and the QUERY status report. net/admin/admin_server.h owns
// the wire (the challenge, the login, the framing) and hands each authenticated line here;
// this returns the line's replies in order and applies its side effects to the server context
// and its world, or hands them to the embedder through the seams below. Every reply string is
// retail's. The witness record is docs/net/novaworld-net-re.md §6.9 ("The command set, the
// server view") and §5.70.8 (the MISSION verbs).
// [orig: CAdminServer_DispatchCommand @0x406720; CAdminServer_HandleStatus @0x402E30]

#include <net/admin/admin_server.h>
#include <runtime/inmatch/client_console.h> // ClientConsoleState (the console walk's debug words)
#include <runtime/inmatch/rotation_admin.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::def {
struct DefItemsFile;
}

namespace opennova::world {
class World;
}

namespace opennova::inmatch {

struct NapiNPServerCtx;
class ClientRuntime;
class CharacterRegistry;

// The main loop's current mode, as the console's state tests read it.
// [orig: UI_GetCurrentGameMode against g_GameModeMainMenu @0x83B400 and g_GameModeGameLoop
//  @0x82F340, CAdminServer_HandleGet @0x4033D0..0x40340B]
enum class AdminScene : uint8_t {
	MainMenu,
	GameLoop,
	// The PreMenu and Post Menu modes, whose frames never pump the admin server
	// (remote_admin_idle_scene, remote_admin.h): GET GAMESTATE's "Unknown" arm is never reached.
	Other,
};

// The process's mission globals the status report prints: the session title g_SessionTitle
// (SERVER), g_MapFileName (CURRENT MAP) and g_GameType (CURRENT GAME TYPE). The running session
// writes them (a host's mission start and SetServerName; a joiner's join, its S2C 0x64 / 0x2C /
// 0x08 and its own mission start) and no teardown resets them, so a joiner's report reads the
// joined session's and the menus' the last session's.
// [orig: CAdminServer_HandleStatus @0x402E83 (g_SessionTitle), @0x402FF6 (&g_MapFileName),
//  @0x403052 (GameType_GetAbbreviation(g_GameType)); the joiner's writers
//  NapiNPClientMsg_MissionMapNames @0x427E6C, NapiNPClientMsg_HandleMissionDataChunk @0x4324D8
//  (and g_SessionTitle @0x4324F5, which its mission start's apply then replaces),
//  NapiNPClientMsg_HandleSessionConfig @0x42836D, UI_EnumerateAndJoinSession @0x56A207]
struct AdminProcessMission {
	std::string session_title;
	std::string map_file;
	uint32_t game_type = 0;
	// CSessionTimer_GetElapsedMS(&g_SessionStatus) when the report is built.
	uint32_t session_elapsed_ms = 0;
	// g_EnvCurTimeFixed24.
	uint32_t tod_fixed24 = 0;
	// GET GAMESETTINGS' two live words: g_RulesFlags and g_RoundTimeRemaining (a host's world,
	// a joined mission's S2C 0x64 word and S2C 0x0A clock, single player's world; the last
	// session's in the menus, zero before any). SET writes them as retail's SET writes the
	// globals, whatever the scene.
	uint32_t rules_word = 0;
	int32_t round_ticks_remaining = 0;
};

// Server_SendValidatedChatToPlayer (a misnomer: the punt): on the authority the active slot
// `index`, when in range and not the local one, gets the disconnect description (reason `code`,
// extra info `detail`); a peer does nothing. The admin console's PUNT and BAN and the input
// action 34 ("Punt", reason 33, "ComIdPunt") call it.
// [orig: Server_SendValidatedChatToPlayer @0x50A210; Input_HandleActionBinding @0x49AD40 - case
//  34 @0x49B766..0x49B773]
void validated_punt(NapiNPServerCtx &ctx, int32_t index, uint32_t code, const char *detail);

// g_RulesFlags into a world: the word itself and the caches its readers take, so the word
// is live at once wherever it is written (a host's boot and admin SET, a joiner's mission-data
// copy and admin SET), as retail's one global is for every process that reads it.
// [orig: g_RulesFlags @0x24D1E34 - its readers Projectile_DamagePairEligible @0x4E74F0
//  (@0x4E74FD, 0x200), Player_AdjustWeaponZoomLevel @0x4DBCC0 (@0x4DBD15, 0x10000),
//  Entity_FindEnemyInCone @0x43CBA0 (@0x43CC2C) and Entity_FindEnemyVehicleInCone @0x43C9F0
//  (@0x43CA81) (0x8000), RoundData_SpawnRound @0x4EC0D0 (@0x4EC72F / @0x4EC79D, NoTracers,
//  bit 0); its writers CAdminServer_HandleSetCommand @0x405A60 (the rule
//  bits' stores @0x405C80..0x405F86) and the mission data's copy
//  NapiNPClientMsg_HandleMissionDataChunk @0x432410 (the memcpy @0x432470)]
void apply_rules_word(world::World &world, uint32_t word);

// The usage reply for a rights word: `USAGE - [QUIT`, then each listed verb by its usage bit
// (CHAT, ADMIN and BANLIST by 0x80, 0x100 and 0x200, though the dispatch opens them on 0x40),
// then `]`. [orig: CAdminServer_DispatchCommand @0x406A8D..0x406D58]
std::string admin_usage_reply(uint32_t rights);

class AdminConsole final : public AdminCommandHandler {
public:
	// What the console reaches outside the server context.
	struct Seams {
		// g_GameConfigState: the cfg block SET writes its shadows into and GET GAMESETTINGS reads
		// its cfg rows from (ArmoryTimer, ServerName and the three passwords). Required.
		gamecfg::GameCfg *config_block = nullptr;
		// Game_SaveConfig: every accepted SET saves the block at once (opennova-serve's
		// working-directory game.cfg). [orig: CAdminServer_HandleSetCommand @0x406185..0x4061AE]
		std::function<void()> save_config;
		// GOTO MENUSTATE out of the main menu: Game_CloseInGameScreens and input action 3, the
		// quit to the main menu (exit reason 1 and the active connection's disconnect, which a
		// Serve Only host does not have). No binding gate stops the action on any host
		// (handle_goto); the router then destroys the session (PostMenu_RouteMissionExit's
		// reason-1 arm). RemoteAdminConsoles::bind sets it from its binding.
		// [orig: CAdminServer_HandleGotoCommand @0x404A4E (Game_CloseInGameScreens),
		//  @0x404A5D (Input_HandleActionBinding(3)); PostMenu_RouteMissionExit @0x5684AB ->
		//  CNapiGameSession_FullDestroy @0x4C96A0 (the call @0x568683)]
		std::function<void()> quit_to_menu;
		// The map rotation and its catalog (ADR 0051 PR3; HostRotationAdmin over the host's
		// own); null reads as no rotation and an empty catalog.
		RotationAdmin *rotation = nullptr;
		// A session peer's (a listen host's, a joiner's) chat window display lines, oldest first
		// (CHAT GET's walk of slots 40..1): its HUD's word-wrapped ring. A host with no client
		// reads the context's own display buffer instead (`console_chat_display`,
		// server_console.h). [orig: CAdminServer_HandleChatCommand @0x404B0F..0x404BB3]
		std::function<std::vector<std::string>()> chat_window;
		// A session peer's flood refusal echoed into its HUD window in the global sender's flood
		// colour. A host with no client sends through Server_SendConsoleChat, which posts the
		// sent line, and echoes a refusal, into the context's CHAT ring.
		// [orig: Chat_SendGlobalMessage @0x49A900 (Chat_AddMessageChannel1(message, palette[3], 930)
		//  @0x49A953)]
		std::function<void(const std::string &text)> chat_echo;
		// The process's own client (a listen host's, a joiner's): a session peer's CHAT SEND and
		// CMD leave on its connection, CMD through its console sender. RemoteAdminConsoles::bind
		// sets it from its binding.
		ClientRuntime *host_client = nullptr;
		// A gametext string (GameText_GetString(section, key): the mount's override table first,
		// D-NET-675); "" when absent. [orig: GameType_GetAbbreviation @0x520FD0;
		//  GameText_GetString @0x51EBD0]
		std::function<std::string(std::string_view section, std::string_view key)> game_text;
		// The avatar registry PETERRABBIT SEXCHANGE picks its two combos from.
		const CharacterRegistry *characters = nullptr;
		// CMD VERSION's two texts, the binary's version and its title, each printed at its
		// start [orig: Game_ParseCommandLineAndInit @0x4A7310 (the title off_8154D8's sprintf
		//  @0x4A7D30..0x4A7D3C, "V%i.%i.%i.%i" into g_VersionText @0x4A7D75..0x4A7D90)].
		std::string version_text;
		std::string title_text;
		// The SYSTEM ring (Chat_AddMessageChannel2) of a host that is not a session peer, which
		// its CMD VERSION and its command walk post to; a session peer's lines are its own
		// client's rings (ClientRuntime::console_command). A host with no client shows no SYSTEM
		// ring (its status page reads the CHAT ring, CHAT GET the CHAT display), so no embedder
		// of that host sets it.
		std::function<void(const std::string &text, uint32_t argb)> system_message;
		// The mission's items.def rows, which CMD SAVENFO's dump names (null: none loaded).
		std::function<const def::DefItemsFile *()> items_table;
		// The local command's WAC overlay toggle over the running mission's script
		// (ClientLocalCommandSeams::toggle_overlay; wac::WacSystem::toggle_cheat). Unset: none.
		std::function<bool(const std::string &line)> toggle_overlay;
		// The process's mission globals the status report prints and GET GAMESETTINGS' live
		// words (RemoteAdminConsoles keeps them; SET writes the live words); null reads the
		// context's own mission (a console whose session is the process's).
		AdminProcessMission *process_mission = nullptr;
	};

	AdminConsole(NapiNPServerCtx &ctx, Seams seams);

	// The embedder's per-frame facts: the current scene and the per-main-frame counter
	// (g_MainFrameCounter, ex dword_A8705C; Game_TickHudFrameCounters @0x434C00) the chat flood
	// table keys on.
	void set_scene(AdminScene scene) { scene_ = scene; }
	void set_main_frame(uint32_t frame) { main_frame_ = frame; }

	bool dispatch(const AdminSession &session, std::string_view line,
			std::vector<std::string> &replies) override;
	std::string status_report() override;

	// CMD's local arms (Client_ProcessLocalCommand): true when one takes the line, which then
	// goes no further (the `pb_` lines, SAVEKEYS, SAVENFO, VERSION).
	bool local_command(const std::string &line);

private:
	using Args = std::vector<std::string>;

	void handle_get(const Args &args, std::vector<std::string> &replies);
	void handle_set(const Args &args, std::vector<std::string> &replies);
	void handle_mission(const Args &args, std::vector<std::string> &replies);
	void handle_mission_add(const Args &args, std::vector<std::string> &replies);
	void handle_player(const Args &args, std::vector<std::string> &replies);
	void handle_weapon(const Args &args, std::vector<std::string> &replies);
	void handle_cmd(const Args &args, std::vector<std::string> &replies);
	void handle_goto(const Args &args, std::vector<std::string> &replies);
	void handle_chat(const Args &args, std::vector<std::string> &replies);
	void handle_fun(const Args &args, std::vector<std::string> &replies);

	std::string game_settings_reply() const;
	// g_RulesFlags: the process words' when kept, else the context's live word.
	uint32_t rules_flags() const;
	void set_rules_flags(uint32_t word);
	std::string player_table() const;
	// The rotation's lines in MISSION LIST's format, or with QUERY's empty fillers.
	std::string rotation_lines(bool query_fillers) const;
	void cycle_tail(std::vector<std::string> &replies);
	void chat_send(std::string text);
	void console_command(const std::string &line);
	bool in_game() const { return scene_ == AdminScene::GameLoop; }

	NapiNPServerCtx &ctx_;
	Seams seams_;
	ClientConsoleState console_state_; // the process's console debug words (D-NET-710)
	AdminScene scene_ = AdminScene::GameLoop;
	uint32_t main_frame_ = 0;
};

} // namespace opennova::inmatch
