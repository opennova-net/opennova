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
//   SINGLE_PLAYER screen, whatever file holds it (docs/required-resources.md "Reaching a mission
//   from the menu");
// - the shell's name sets (docs/mnu/menu-wiring.md "Shell wiring"), which the OpenNova game shell
//   binds on whatever document it opens (godot/game/menu_shell.gd's defaults read them through
//   MenuDriver.command_names), and the shell's delegates that own a whole document by the
//   controls it holds (the LAN multiplayer screens [orig: UI_RegisterLANMultiplayerCallbacks
//   @ 0x558d20], PLAYER_INFO [orig: PlayerInfo_InitProfileSelector @ 0x5611b0]).
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
	ApplyExpansion, // the highlighted expansion mounts over the base game
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
};
// "start_mission", "apply_expansion", "exit", "back", "return_to_menu", "restart", "novaworld",
// "lan_search", "lan_join", "host_game", "save_profile", "close_map", "apply_loadout",
// "cancel_loadout", "respawn" ("" for None).
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

// The commands the game binds on a document as the runtime holds it now (after each open), by
// control NAME: on a screen a mission opens (in a mission) or the SINGLE_PLAYER screen, the
// controls the code registers on that screen; then the shell's: a delegate that owns the document
// (the LAN screens, PLAYER_INFO) binds its own controls and nothing else, else the launch controls
// start the mission on a document holding a mission list (apply the highlighted expansion on one
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
