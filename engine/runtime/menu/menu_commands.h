#pragma once

// What the game's code does with a control it binds by NAME: a Command (ADR 0001), game
// behaviour the menu format cannot express (start a mission, quit, join a game), registered by
// the code on a control of a screen and never written in the .mnu [orig:
// CUIScene_RegisterControlCallback @ 0x63c060 -> CUIScene_BindControlCallbacks @ 0x63af80: rows
// (screen, control, mask, fn, ctx) bound to the control CWnd_FindChildByName finds; a control the
// screen lacks binds to nothing]. Two layers, as the game has them:
// - the screens a mission opens by name [orig: UI_OpenMenuScreen @ 0x54e520 and its callers], each
//   with the controls the code registers on it (docs/required-resources.md "What a mission's menus
//   must hold"; docs/mnu/menu-re.md "The in-game exit confirmation"), and the front end's
//   SINGLE_PLAYER screen (docs/required-resources.md "Reaching a mission from the menu") and the
//   updater's buttons on its STARTUP and OPTIONS screens, whatever file holds them;
// - the shell's name sets (docs/mnu/menu-wiring.md "Shell wiring"), which the OpenNova game shell
//   binds on whatever document it opens (godot/game/menu_shell.gd's defaults read them through
//   MenuDriver.command_names), and the shell's delegates that own a whole document by the
//   controls it holds (the LAN multiplayer screens [orig: UI_RegisterLANMultiplayerCallbacks
//   @ 0x558d20]) or by its screen's name (PLAYER_INFO, is_player_info_document).
// The editor's menu preview in Try mode (ADR 0046 DI-35) says from these what the game would do,
// where the game would leave the menu.

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {

class MenuRuntime;

enum class MenuCommand : uint8_t {
	None,
	StartMission, // the selected mission starts (the game leaves the menu)
	ApplyExpansion, // the game picked in the Mods list (an expansion, or the base game) loads for the run
	Exit, // the game quits to the desktop
	Back, // the screen history across files pops; with none, the mission resumes (in one), else Exit
	ReturnToMenu, // the mission ends, back to the menu screen it was started from
	Restart, // the single-player mission starts again
	NovaWorld, // the NovaWorld (online multiplayer) login opens
	LanSearch, // the LAN is searched for games
	LanJoin, // the highlighted LAN game is joined
	HostGame, // a LAN game is hosted with the host screen's settings
	SaveProfile, // the player's profile is saved from the screen
	CloseMap, // the command map closes, back to the mission
	ApplyLoadout, // the loadout the screen holds is applied, back to the mission
	CancelLoadout, // the loadout screen closes with nothing applied
	Respawn, // the player respawns at the point picked
	Update, // the updater starts; once it has, the game quits
};
// "start_mission", "apply_expansion", "exit", "back", "return_to_menu", "restart", "novaworld",
// "lan_search", "lan_join", "host_game", "save_profile", "close_map", "apply_loadout",
// "cancel_loadout", "respawn", "update" ("" for None).
const char *menu_command_token(MenuCommand command);
// What the game does, in words, as "the game would ..." ends ("start the selected mission").
const char *menu_command_words(MenuCommand command);

// The shell's name sets (menu_shell.gd's exported defaults, a game's own to replace).
enum class MenuNameSet : uint8_t {
	Start, // the launch controls: start the mission on a document holding a mission list
	Exit,
	Return, // CONFIRM_YES: in a mission only
	Restart, // in a mission only
	Back, // HIDDEN_BACK
	NovaWorld,
	MissionLists, // the lists the game fills with its missions
	SinglePlayerLists, // of those, the single-player screen's (its selection gate and briefing)
	Briefings, // the single-player briefing pane a pick fills
	SinglePlayerAccepts, // the single-player confirm the pick enables
	ModLists, // the lists the game fills with the expansions
	ModDescriptions, // the read-only text showing the highlighted expansion
	kCount,
};
const std::vector<std::string> &menu_name_set(MenuNameSet set);
// "start", "exit", "return", "restart", "back", "novaworld", "mission_lists", "sp_lists",
// "briefings", "sp_accepts", "mod_lists", "mod_descriptions"; and back (false for another).
const char *menu_name_set_token(MenuNameSet set);
bool menu_name_set_from_token(const std::string &token, MenuNameSet &out);

// A screen a mission opens by name, with its file [orig: UI_OpenMenuScreen @ 0x54e520]: CMAP,
// INGAME, WEAPON, VEHICLE, STAT, DEATH.
struct MissionMenuScreen {
	const char *file;
	const char *screen;
	const char *cite;
};
const std::vector<MissionMenuScreen> &mission_menu_screens();
// Whether the game opens `file` (by name, without case) in a mission.
bool is_mission_menu_file(const std::string &file);

// What the game does with an APPMSG row's message, on any screen: the row raises scene event 10
// with its text, and the game's handler starts the updater on "UPDATE" (without case, untrimmed)
// and does nothing with another [orig: CUIWidget_HandleScriptedAction @ 0x6497f0 case 13
// @ 0x649b88..0x649b99 (the row's text +0x1C) -> CUIScene_RaiseAppMessage @ 0x63b5c0 (CUIScene_RaiseEvent(10));
// UI_DispatchScreenEvent @ 0x54e6a0 case 10 - the _stricmp with "UPDATE" @ 0x54f3a4..0x54f3b4,
// UI_LaunchUpdateProcess @ 0x54f3c1]. No availability gates it: the launch runs update.exe and
// the game quits once the process started (UI_LaunchUpdateProcess @ 0x55b0b0, the CreateProcessA
// test @ 0x55b181, the quit @ 0x55b194..0x55b1a2).
MenuCommand app_message_command(const std::string &message);

// The character screen's document: one holding a screen named PLAYER_INFO (without case). Both
// executables know the screen by its name alone: their screen dispatch runs the screen's one-time
// registration on its init and its populate on its activation for the screen of that name, whatever
// controls it holds, each fill then finding its controls by name and skipping one the screen lacks
// [orig: UI_DispatchScreenEvent @ 0x54e6a0 - _stricmp(.., "PLAYER_INFO") @ 0x54e9d4 ->
// UI_InitPlayerInfoOnce @ 0x54e9e0 (PlayerInfo_RegisterAllControls @ 0x561470), @ 0x54eec1 ->
// PlayerInfo_InitProfileSelector @ 0x5611b0 (called @ 0x54eecd)] [orig: UI_DispatchScreenEvent
// @ 0x54b920 - @ 0x54bc2d -> UI_InitPlayerInfoOnce @ 0x54bc39 (PlayerInfo_RegisterAllControls
// @ 0x55f720), @ 0x54c11a -> PlayerInfo_InitProfileSelector @ 0x54c126 (dfx2.exe)]. dfx2.exe's
// player.mnu holds no NATIONALITY or DIVISION.
bool is_player_info_document(const MenuRuntime &menu);

// The commands the game binds on a document as the runtime holds it now (after each open), by
// control NAME: on a screen a mission opens (in a mission) or the SINGLE_PLAYER, STARTUP and
// OPTIONS screens (the front end), the controls the code registers on that screen; then the
// shell's: a delegate that owns the document (the LAN screens, PLAYER_INFO) binds its own
// controls and nothing else, else the launch controls
// start the mission on a document holding a mission list (switch to the Mods list's pick on one
// holding a mod list instead, else they are the menu's own), then exit, return and restart (in a
// mission), NovaWorld and back; a name keeps its first binding (menu_shell.gd _wire_named_controls).
class MenuCommands {
public:
	void wire(const MenuRuntime &menu, bool in_mission);
	// The command bound to the control `name` on the screen `screen` (the current one), None for none.
	MenuCommand command_of(const std::string &screen, const std::string &name) const;
	// The document's lists the game fills with its missions, and with its expansions (widget ids).
	const std::vector<int> &mission_lists() const { return mission_lists_; }
	const std::vector<int> &mod_lists() const { return mod_lists_; }
	// The delegate that owns the document ("" the shell's own wiring): "lan", "player_info".
	const std::string &owner() const { return owner_; }

private:
	struct Bound {
		std::string screen; // upper case; "" any screen of the document
		std::string name; // upper case
		MenuCommand command = MenuCommand::None;
	};
	void bind_(const std::string &screen, const std::string &name, MenuCommand command);
	std::vector<Bound> bound_;
	std::vector<int> mission_lists_;
	std::vector<int> mod_lists_;
	std::string owner_;
};

} // namespace opennova::menu
