#include <runtime/menu/menu_commands.h>

#include <base/io/strutil.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_runtime.h>

namespace opennova::menu {

namespace {

struct CommandRow {
	MenuCommand command;
	const char *token;
	const char *words;
};
constexpr CommandRow kCommands[] = {
	{ MenuCommand::None, "", "" },
	{ MenuCommand::StartMission, "start_mission", "start the selected mission" },
	{ MenuCommand::ApplyExpansion, "apply_expansion", "switch to the game picked in the Mods list for this run, reloading everything" },
	{ MenuCommand::Exit, "exit", "quit to the desktop" },
	{ MenuCommand::Back, "back", "go back to the screen before, else resume the mission or quit" },
	{ MenuCommand::ReturnToMenu, "return_to_menu", "leave the mission for the menu screen it was started from" },
	{ MenuCommand::Restart, "restart", "start the single-player mission again" },
	{ MenuCommand::NovaWorld, "novaworld", "open the NovaWorld login (online multiplayer)" },
	{ MenuCommand::LanSearch, "lan_search", "search the LAN for games" },
	{ MenuCommand::LanJoin, "lan_join", "join the highlighted LAN game" },
	{ MenuCommand::HostGame, "host_game", "host a LAN game with the screen's settings" },
	{ MenuCommand::SaveProfile, "save_profile", "save the player's profile from the screen" },
	{ MenuCommand::CloseMap, "close_map", "close the command map, back to the mission" },
	{ MenuCommand::ApplyLoadout, "apply_loadout", "apply the loadout the screen holds, back to the mission" },
	{ MenuCommand::CancelLoadout, "cancel_loadout", "close the loadout screen with nothing applied" },
	{ MenuCommand::Respawn, "respawn", "respawn the player at the point picked" },
};

const CommandRow &command_row(MenuCommand command) {
	for (const CommandRow &row : kCommands)
		if (row.command == command) return row;
	return kCommands[0];
}

// The OpenNova game shell's name sets (godot/game/menu_shell.gd's exported defaults; a game whose
// menus name its controls otherwise points the shell at its own).
const char *const kSetTokens[] = { "start", "exit", "return", "restart", "back", "novaworld", "mission_lists",
	"sp_lists", "briefings", "sp_accepts", "mod_lists", "mod_descriptions" };
static_assert(sizeof(kSetTokens) / sizeof(kSetTokens[0]) == size_t(MenuNameSet::kCount), "a token per set");

const std::vector<std::string> kSets[] = {
	// The launch controls on a play screen.
	{ "START_GAME", "ACCEPT", "LAUNCH", "GO", "HOST_GAME", "LAN_HOSTGAME" },
	{ "EXIT", "QUIT", "QUIT_GAME", "QUIT_TO_DESKTOP" },
	// The mission exit is CONFIRM_YES's, not ABORT's: ABORT's own rows raise the question.
	{ "CONFIRM_YES" },
	{ "RESTART" },
	// The in-game screen's ESC-hotkeyed resume [orig: UI_IngameBackResumeCommand @ 0x555490].
	{ "HIDDEN_BACK" },
	{ "NW_MULTI_PLAYER", "NOVAWORLD", "NOVAWORLD_LOGIN", "INTERNET_GAME" },
	{ "MISSION_LIST", "MISSIONLIST", "MISSIONS", "IA_LIST", "CA_MISSION_LIST", "MAP_LIST" },
	// [orig: SinglePlayer_PopulateMissionList @ 0x561840 fills IA_LIST]
	{ "IA_LIST", "CA_MISSION_LIST" },
	{ "BRIEFING" },
	{ "ACCEPT" },
	{ "AVAIL_LIST", "MOD_LIST", "MODLIST", "EXPANSION_LIST" },
	{ "MOD_DESC", "MOD_DESCRIPTION" },
};
static_assert(sizeof(kSets) / sizeof(kSets[0]) == size_t(MenuNameSet::kCount), "a list per set");

// The screens a mission opens by name [orig: UI_OpenMenuScreen @ 0x54e520] (docs/required-resources.md
// "What a mission's menus must hold").
const std::vector<MissionMenuScreen> kMissionScreens = {
	{ "cmap.mnu", "CMAP", "UI_OpenMenuScreen(\"cmap.mnu\", \"CMAP\") @ 0x49b920" },
	{ "game.mnu", "INGAME", "UI_OpenMenuScreen @ 0x49b3b1 (Esc)" },
	{ "weapon.mnu", "WEAPON", "UI_OpenMenuScreen @ 0x49b8de" },
	{ "vehicle.mnu", "VEHICLE", "UI_OpenMenuScreen @ 0x49b892" },
	{ "stat.mnu", "STAT", "UI_OpenMenuScreen @ 0x5b8636" },
	{ "death.mnu", "DEATH", "UI_OpenMenuScreen @ 0x5cab7e" },
};

// The controls the code registers on a screen by name, and what each does.
struct ScreenRow {
	const char *screen;
	const char *control;
	MenuCommand command;
	bool in_mission; // registered as a mission opens the screen (else the front end's)
};
constexpr ScreenRow kScreenRows[] = {
	// [orig: UI_InitSinglePlayerScreen @ 0x562080, run on any screen named SINGLE_PLAYER by
	//  UI_DispatchScreenEvent @ 0x54e6a0; SinglePlayer_HandleStartEvent @ 0x561fb0, registered @ 0x5620c0]
	{ "SINGLE_PLAYER", "ACCEPT", MenuCommand::StartMission, false },
	// [orig: CRenderManager_FreeLoadScreen @ 0x547980 (a misnomer: the map's close), registered @ 0x54adb9]
	{ "CMAP", "OK", MenuCommand::CloseMap, true },
	// [orig: UI_RegisterIngameCallbacks @ 0x555510 — HIDDEN_BACK UI_IngameBackResumeCommand @ 0x555490
	//  (@ 0x555583), CONFIRM_YES UI_IngameConfirmExitCommand @ 0x555460, RESTART UI_IngameRestartCommand
	//  @ 0x555410 (@ 0x555529); ABORT UI_IngameAbortArmConfirm @ 0x555450 arms the question alone]
	{ "INGAME", "HIDDEN_BACK", MenuCommand::Back, true },
	{ "INGAME", "CONFIRM_YES", MenuCommand::ReturnToMenu, true },
	{ "INGAME", "RESTART", MenuCommand::Restart, true },
	// [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0, registered @ 0x567207 / @ 0x567225]
	{ "WEAPON", "ACCEPT", MenuCommand::ApplyLoadout, true },
	{ "WEAPON", "CANCEL", MenuCommand::CancelLoadout, true },
	// [orig: @ 0x563b60, registered @ 0x5641d9 / @ 0x5641f7]
	{ "VEHICLE", "ACCEPT", MenuCommand::ApplyLoadout, true },
	{ "VEHICLE", "CANCEL", MenuCommand::CancelLoadout, true },
	// [orig: UI_StatConfirmExitCommand @ 0x562210, registered @ 0x562813]
	{ "STAT", "CONFIRM_YES", MenuCommand::ReturnToMenu, true },
	// [orig: UI_RegisterDeathScreenCallbacks @ 0x554610 — DeathScreen_OnConfirmYes @ 0x553530 (@ 0x554647),
	//  DeathScreen_OnSpawnListSelect @ 0x553630 (@ 0x5546a1)]
	{ "DEATH", "CONFIRM_YES", MenuCommand::ReturnToMenu, true },
	{ "DEATH", "SPAWNPOINTS_LIST", MenuCommand::Respawn, true },
};

bool is_list_kind(int kind) {
	return kind == int(mnu::WindowType::List) || kind == int(mnu::WindowType::LanList);
}

} // namespace

const char *menu_command_token(MenuCommand command) {
	return command_row(command).token;
}

const char *menu_command_words(MenuCommand command) {
	return command_row(command).words;
}

const std::vector<std::string> &menu_name_set(MenuNameSet set) {
	static const std::vector<std::string> kNone;
	return set < MenuNameSet::kCount ? kSets[size_t(set)] : kNone;
}

const char *menu_name_set_token(MenuNameSet set) {
	return set < MenuNameSet::kCount ? kSetTokens[size_t(set)] : "";
}

bool menu_name_set_from_token(const std::string &token, MenuNameSet &out) {
	for (size_t i = 0; i < size_t(MenuNameSet::kCount); ++i)
		if (token == kSetTokens[i]) {
			out = MenuNameSet(i);
			return true;
		}
	return false;
}

const std::vector<MissionMenuScreen> &mission_menu_screens() {
	return kMissionScreens;
}

bool is_mission_menu_file(const std::string &file) {
	for (const MissionMenuScreen &screen : kMissionScreens)
		if (strutil::iequals(file, screen.file)) return true;
	return false;
}

void MenuCommands::bind_(const std::string &screen, const std::string &name, MenuCommand command) {
	const std::string upper = strutil::to_upper(name);
	for (const Bound &bound : bound_)
		if (bound.name == upper && bound.screen == screen) return; // the first binding stays
	bound_.push_back({ screen, upper, command });
}

void MenuCommands::wire(const MenuRuntime &menu, bool in_mission) {
	bound_.clear();
	mission_lists_.clear();
	mod_lists_.clear();
	owner_.clear();
	// The code's registrations on the screens it knows by name, each on its own screen.
	for (const ScreenRow &row : kScreenRows) {
		if (row.in_mission != in_mission) continue;
		if (menu.find_screen_control(row.screen, row.control) >= 0)
			bind_(strutil::to_upper(row.screen), row.control, row.command);
	}
	const auto has = [&menu](const char *name) { return menu.widget_id(name) >= 0; };
	const auto bind_set = [&](MenuNameSet set, MenuCommand command) {
		for (const std::string &name : menu_name_set(set))
			if (menu.widget_id(name) >= 0) bind_(std::string(), name, command);
	};
	// A delegate owns a whole document by the controls it holds, and binds its own alone: the LAN
	// multiplayer screens [orig: UI_RegisterLANMultiplayerCallbacks @ 0x558d20; the host start
	// UI_HandleHostSessionStart @ 0x556d00], PLAYER_INFO [orig: PlayerInfo_SaveFromDialog @ 0x55ee10]
	// (godot/game/mp_menu_companion.gd, player_info_menu_companion.gd).
	if (has("LAN_GAME_LIST") || has("SELECTED_MISSIONS")) {
		owner_ = "lan";
		bind_(std::string(), "LAN_SEARCH", MenuCommand::LanSearch);
		bind_(std::string(), "LAN_JOINGAME", MenuCommand::LanJoin);
		bind_(std::string(), "START_GAME", MenuCommand::HostGame);
		return;
	}
	if (has("NATIONALITY") && has("COMBO_LIST")) {
		owner_ = "player_info";
		bind_(std::string(), "ACCEPT", MenuCommand::SaveProfile);
		return;
	}
	for (const std::string &name : menu_name_set(MenuNameSet::MissionLists)) {
		const int id = menu.widget_id(name);
		if (id >= 0 && is_list_kind(menu.widget_kind_of(id))) mission_lists_.push_back(id);
	}
	for (const std::string &name : menu_name_set(MenuNameSet::ModLists)) {
		const int id = menu.widget_id(name);
		if (id >= 0 && is_list_kind(menu.widget_kind_of(id))) mod_lists_.push_back(id);
	}
	// The launch controls are a play screen's: a document holding a mission list starts its mission, one
	// holding a mod list switches to the game picked in it (mod_list.h), any other leaves them to the
	// menu's own rows (an OK on the options screen is no launch).
	if (!mission_lists_.empty()) bind_set(MenuNameSet::Start, MenuCommand::StartMission);
	else if (!mod_lists_.empty()) bind_set(MenuNameSet::Start, MenuCommand::ApplyExpansion);
	bind_set(MenuNameSet::Exit, MenuCommand::Exit);
	// The mission's own commands answer only while a mission runs.
	if (in_mission) {
		bind_set(MenuNameSet::Return, MenuCommand::ReturnToMenu);
		bind_set(MenuNameSet::Restart, MenuCommand::Restart);
	}
	bind_set(MenuNameSet::NovaWorld, MenuCommand::NovaWorld);
	bind_set(MenuNameSet::Back, MenuCommand::Back);
}

MenuCommand MenuCommands::command_of(const std::string &screen, const std::string &name) const {
	const std::string upper = strutil::to_upper(name);
	const std::string on = strutil::to_upper(screen);
	// A screen's own registration first, then the document-wide one.
	for (const Bound &bound : bound_)
		if (bound.name == upper && !bound.screen.empty() && bound.screen == on) return bound.command;
	for (const Bound &bound : bound_)
		if (bound.name == upper && bound.screen.empty()) return bound.command;
	return MenuCommand::None;
}

} // namespace opennova::menu
