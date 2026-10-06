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
#include <runtime/inmatch/rotation_admin.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::gamecfg {
struct GameCfg;
}

namespace opennova::inmatch {

struct NapiNPServerCtx;
class ClientRuntime;
class CharacterRegistry;

// The main loop's current scene, as the console's state tests read it.
// [orig: sub_52BC90() against g_GameModeMainMenu @0x83B400 and g_GameModeGameLoop @0x82F340]
enum class AdminScene : uint8_t {
	MainMenu,
	GameLoop,
	Other, // the pre and post menus, a load
};

// DispatchCommand's split: `isspace` separates, at most 25 tokens, and the split stops
// writing terminators at the cap, so the 25th token runs to the end of the line.
// [orig: CAdminServer_DispatchCommand @0x40676B..0x4067EB, the cap @0x4067A3]
std::vector<std::string> admin_split_command(std::string_view line);

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
		// reason-1 arm).
		// [orig: CAdminServer_HandleGotoCommand @0x404A4E (Game_CloseInGameScreens),
		//  @0x404A5D (Input_HandleActionBinding(3)); PostMenu_RouteMissionExit @0x5684AB ->
		//  CNapiGameSession_FullDestroy @0x568683]
		std::function<void()> quit_to_menu;
		// The map rotation and its catalog (ADR 0051 PR3; HostRotationAdmin over the host's
		// own); null reads as no rotation and an empty catalog.
		RotationAdmin *rotation = nullptr;
		// A listen host's chat window display lines, oldest first (CHAT GET's walk of slots
		// 40..1): its HUD's word-wrapped ring. A host with no client reads the context's CHAT
		// ring instead (`console_chat`, server_console.h; D-NET-372).
		std::function<std::vector<std::string>()> chat_window;
		// A listen host's flood refusal echoed into its HUD window (Chat_AddMessageChannel1). A
		// host with no client sends through Server_SendConsoleChat, which posts the sent line,
		// and echoes a refusal, into the context's CHAT ring.
		std::function<void(const std::string &text)> chat_echo;
		// The listen host's own client: a session peer's CHAT SEND and CMD leave on its connection.
		ClientRuntime *host_client = nullptr;
		// A gametext string (GameText_GetString(section, key)); "" when absent.
		std::function<std::string(std::string_view section, std::string_view key)> game_text;
		// The avatar registry PETERRABBIT SEXCHANGE picks its two combos from.
		const CharacterRegistry *characters = nullptr;
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
	std::string player_table() const;
	// The rotation's lines in MISSION LIST's format, or with QUERY's empty fillers.
	std::string rotation_lines(bool query_fillers) const;
	void cycle_tail(std::vector<std::string> &replies);
	void chat_send(std::string text);
	void console_command(const std::string &line);
	bool in_game() const { return scene_ == AdminScene::GameLoop; }

	NapiNPServerCtx &ctx_;
	Seams seams_;
	AdminScene scene_ = AdminScene::GameLoop;
	uint32_t main_frame_ = 0;
};

} // namespace opennova::inmatch
