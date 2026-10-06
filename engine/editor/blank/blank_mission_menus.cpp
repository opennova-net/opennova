#include "blank_makers.h"

#include <initializer_list>

#include <formats/mnu/mnu.h>

// The menus a mission opens by name, each authored from scratch as the least the game needs of it
// (docs/required-resources.md, "What a mission's menus must hold"). The game opens a screen by its
// file and NAME [orig: UI_OpenMenuScreen @ 0x54e520]: a file it cannot read opens nothing (the
// load's E_FAIL skips the select @ 0x54e584), but a file without the named screen still raises the
// menu-open flag dword_255110C @ 0x54e59e though the select failed [orig: CUIScene_SelectNodeByName
// @ 0x63b6b0, E_FAIL @ 0x63b746], and with that flag up the Esc menu, the armory key and the map
// key are all gated off [orig: Input_HandleActionBinding @ 0x49b3a5, @ 0x49b8c9, @ 0x49b909]: an
// input dead end. So every file holds its screen. Every control the code looks up by name on these
// screens is null-checked before use [orig: UI_FindScreenControl @ 0x63ae80, every call site on the
// CMAP, INGAME, WEAPON, VEHICLE, STAT and DEATH screens], and a callback registered on a control
// the screen lacks binds to nothing [orig: CUIScene_RegisterControlCallback @ 0x63c060 ->
// CUIScene_BindControlCallbacks @ 0x63af80], so a screen may leave any of them out; what it may not
// leave out is a way back to play, which only the code's close commands give [orig:
// Game_CloseInGameScreens @ 0x54b940, the close every leave command below calls: the in-game
// latches cleared and the menu's close requested @ 0x54b94a]. Each screen therefore holds the
// controls the player leaves it by, named as the code binds them, with literal labels (menutxt.bin
// is optional and absent from a new project), and the pointer the player aims them with.

namespace opennova::editor {

namespace {

// A virtual key (VK_ESCAPE) or a character key (V), as the HOTKEY element authors it.
struct Key {
	bool is_virtual;
	const char *key;
};

// A window action: SHOW or HIDE the named window [orig: CUIElement_ParseXMLDefinition @
// 0x648ee2..0x6490e9].
struct Show {
	const char *state;
	const char *target;
};

// A window's rectangle in its parent's frame; a button or a static gives no bottom (its font's).
struct Box {
	int left, top, right, bottom = -1;
};

std::string position(const Box &box) {
	std::string xml = "<POSITION><LEFT>" + std::to_string(box.left) + "</LEFT><TOP>" + std::to_string(box.top) +
	                  "</TOP><RIGHT>" + std::to_string(box.right) + "</RIGHT>";
	if (box.bottom >= 0) xml += "<BOTTOM>" + std::to_string(box.bottom) + "</BOTTOM>";
	return xml + "</POSITION>\n";
}

std::string button(const char *name, const char *label, const Box &box, std::initializer_list<Key> keys,
                   std::initializer_list<Show> actions = {}) {
	std::string xml = "<WINDOW type=\"button\" name=\"" + std::string(name) + "\">\n";
	for (const Key &key : keys)
		xml += std::string(key.is_virtual ? "<HOTKEY VIRTUAL>" : "<HOTKEY>") + key.key + "</HOTKEY>\n";
	for (const Show &action : actions)
		xml += "<ACTION type=\"window\" state=\"" + std::string(action.state) + "\">" + action.target + "</ACTION>\n";
	xml += "<APPEARANCE state=\"default\"></APPEARANCE>\n"
	       "<APPEARANCE state=\"mouseover\"></APPEARANCE>\n"
	       "<APPEARANCE state=\"selected\"></APPEARANCE>\n"
	       "<APPEARANCE state=\"disabled\"></APPEARANCE>\n" +
	       position(box) + "<STRING justify=\"CENTER\">" + mnu::escape_text(label) + "</STRING>\n</WINDOW>\n";
	return xml;
}

std::string text(const char *name, const char *label, const Box &box) {
	return "<WINDOW type=\"static\" name=\"" + std::string(name) + "\">\n<APPEARANCE state=\"default\"></APPEARANCE>\n" +
	       position(box) + "<STRING justify=\"CENTER\">" + mnu::escape_text(label) + "</STRING>\n</WINDOW>\n";
}

std::string panel(const char *name, const Box &box, bool hidden, const std::string &children) {
	return "<WINDOW type=\"window\" name=\"" + std::string(name) + "\"" + (hidden ? " HIDDEN" : "") + ">\n" +
	       position(box) + children + "</WINDOW>\n";
}

// The screen: its NAME, and a MAIN window over the whole 800x600 design frame carrying the font
// every window under it inherits and the pointer every window under it shows [orig:
// CWnd_GetInheritedCursorTexture @ 0x646AD0]: the startup screen's CURSOR (blank_menu_cursor, the
// file made with the menu where the project has none, blank_companion), as every shipped in-mission
// screen's MAIN names it. The game hides the system pointer for good as it starts [orig:
// Game_InitSubsystems @ 0x4a725a -> Game_HideCursorLoop @ 0x7612e0], and a screen's only pointer is
// the CURSOR texture its windows name, drawn last at the mouse [orig: CUIScene_DrawScreensAndCursor
// @ 0x63bf60 over scene_end_frame @ 0x63e600's pick, zeroed each frame @ 0x63e606]. The mouse still
// works without one, each message's own point hit-testing the buttons [orig: Game_WindowProc @
// 0x7624c0 -> Input_DispatchMouseEvent @ 0x761470 -> widget_process_mouse_event @ 0x647a00], so a
// screen naming none is clicked blind: its buttons light under a mouse the player cannot see.
std::string screen(const char *name, const std::string &children) {
	return "<SCREEN>\n<NAME>" + std::string(name) +
	       "</NAME>\n<WINDOW type=\"window\" name=\"MAIN\">\n"
	       "<APPEARANCE type=\"custom\" state=\"default\"></APPEARANCE>\n" +
	       position({0, 0, 800, 600}) + blank_menu_font() + blank_menu_cursor() + children +
	       "</WINDOW>\n</SCREEN>\n";
}

// The leave-the-mission idiom the in-game screens share: a hidden CONFIRM_EXIT panel asking
// first, its CONFIRM_YES the code's command that closes the screens and leaves the mission
// (action 3), CONFIRM_NO a way back; both put `wrapper` (the panel the screen's leave button hid)
// back and hide the question, the authored half of the pair the command completes.
std::string confirm_exit(const char *wrapper) {
	return panel("CONFIRM_EXIT", {250, 250, 550, 350}, true,
	             text("STATIC_CONFIRM", "Leave the mission?", {0, 10, 300}) +
	                     button("CONFIRM_YES", "Yes", {30, 60, 140}, {{true, "VK_RETURN"}},
	                            {{"SHOW", wrapper}, {"HIDE", "CONFIRM_EXIT"}}) +
	                     button("CONFIRM_NO", "No", {160, 60, 270}, {{true, "VK_ESCAPE"}},
	                            {{"SHOW", wrapper}, {"HIDE", "CONFIRM_EXIT"}}));
}

} // namespace

// cmap.mnu: the command map, CMAP, loaded as a mission starts [orig: Game_StartMission @ 0x526316,
// @ 0x526332] and opened by the commander key, action 221 (V) [orig: Input_HandleActionBinding @
// 0x49b91b]. With a menu up that key is gated off, so OK, its close [orig: CRenderManager_FreeLoadScreen
// @ 0x547980, registered @ 0x54adb9: Game_CloseInGameScreens and g_CmapScreenOpen = 0], takes Esc
// and the commander key both: the key that opened the map closes it.
bool make_blank_cmap_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return blank_menu_bytes(screen("CMAP", button("OK", "Close", {340, 540, 460}, {{true, "VK_ESCAPE"}, {false, "V"}})),
	                        request, out, error);
}

// game.mnu: the in-mission menu, INGAME, opened by Esc [orig: Input_HandleActionBinding @ 0x49b3b1].
// Its show pauses single player [orig: UI_OptionsScreenInit @ 0x554dd8] and only its back command
// resumes [orig: UI_IngameBackResumeCommand @ 0x555490, the pause cleared @ 0x55549e], registered on
// HIDDEN_BACK [orig: UI_RegisterIngameCallbacks @ 0x555583]: without it Esc pauses for good. Esc
// presses it; under the hidden MAIN_WRAPPER (the confirm up) Esc reaches CONFIRM_NO instead. ABORT
// arms the leave [orig: UI_IngameAbortArmConfirm @ 0x555450, registered @ 0x555547] and raises the
// question by its actions; CONFIRM_YES leaves [orig: UI_IngameConfirmExitCommand @ 0x555460,
// registered @ 0x555565] for the menu screen the mission was started from, which the menu's return
// selects again from its history [orig: Menu_InitShellResources @ 0x552682 ->
// UIScene_ReturnToHistoryScreen @ 0x63dfa0].
//
// The blank leaves out the shipped screen's OPTIONS (the whole options dialog) and RESTART. RESTART
// queues the respawn action before the restart [orig: UI_IngameRestartCommand @ 0x555410, action 12
// @ 0x555428], and the respawn resets the player through its body's animation slot, read unchecked
// [orig: Server_ProcessClientRequestRespawn @ 0x519ce3 -> Server_ProcessPlayerDeath @ 0x517899 ->
// Entity_ResetToSpawnState @ 0x4b9620, entity+0x188]: a project with no character yet plays the
// first items.def row (D-NET-348), which has none, and the game faults there (witnessed in strict
// retail 2026-10-05). A project whose player has a body adds it; the command stays bound by name.
bool make_blank_game_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string wrapper =
	        panel("MAIN_WRAPPER", {300, 250, 500, 350}, false,
	              button("HIDDEN_BACK", "Resume", {0, 10, 200}, {{true, "VK_ESCAPE"}}) +
	                      button("ABORT", "Leave Mission", {0, 50, 200}, {},
	                             {{"SHOW", "CONFIRM_EXIT"}, {"HIDE", "MAIN_WRAPPER"}}));
	return blank_menu_bytes(screen("INGAME", wrapper + confirm_exit("MAIN_WRAPPER")), request, out, error);
}

// weapon.mnu: the armory, WEAPON [orig: UI_OpenMenuScreen("weapon.mnu", "WEAPON") @
// Input_HandleActionBinding @ 0x49b8de, Input_HandleActionBinding_0 @ 0x4e0b44,
// UI_OpenWeaponScreenSinglePlayer @ 0x4ddcfe]. ACCEPT and CANCEL share one handler
// [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0, registered @ 0x567207 / @ 0x567225]: ACCEPT
// applies the kit the screen's slot controls hold, so with none it would strip the player's
// weapons; CANCEL (argument 1) skips the apply and closes [orig: @ 0x5662a9]. The blank holds
// CANCEL alone, on Esc.
bool make_blank_weapon_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return blank_menu_bytes(screen("WEAPON", button("CANCEL", "Cancel", {340, 540, 460}, {{true, "VK_ESCAPE"}})),
	                        request, out, error);
}

// vehicle.mnu: a vehicle's loadout, VEHICLE [orig: Input_HandleActionBinding @ 0x49b892,
// Input_HandleActionBinding_0 @ 0x4e0af8]. ACCEPT sends the weapon ITEM_LIST selects and CANCEL
// only closes [orig: the shared handler @ 0x563b60, registered @ 0x5641d9 / @ 0x5641f7, closing
// through Game_CloseInGameScreens @ 0x563be3]; with no list there is nothing to accept, so the
// blank holds CANCEL alone, on Esc.
bool make_blank_vehicle_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return blank_menu_bytes(screen("VEHICLE", button("CANCEL", "Cancel", {340, 540, 460}, {{true, "VK_ESCAPE"}})),
	                        request, out, error);
}

// stat.mnu: the end-of-round board, STAT [orig: UI_ProcessEndRoundScreenTransition @ 0x5b8636].
// Its CONFIRM_YES leaves the mission [orig: UI_StatConfirmExitCommand @ 0x562210, registered by
// HUD_CacheStatPanelValues @ 0x562813]; HIDDEN_BACK, on Esc, asks first.
bool make_blank_stat_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string stats =
	        panel("STATS", {300, 250, 500, 350}, false,
	              button("HIDDEN_BACK", "Leave Mission", {0, 30, 200}, {{true, "VK_ESCAPE"}},
	                     {{"SHOW", "CONFIRM_EXIT"}, {"HIDE", "STATS"}}));
	return blank_menu_bytes(screen("STAT", stats + confirm_exit("STATS")), request, out, error);
}

// death.mnu: the deploy screen, DEATH, opened when the player dies in a session or in a mission
// that respawns [orig: Render_ProcessMainSceneFrame @ 0x5cab7e]. A spawn picked on
// SPAWNPOINTS_LIST queues the respawn [orig: DeathScreen_OnSpawnListSelect @ 0x553630, registered by
// UI_RegisterDeathScreenCallbacks @ 0x5546a1], the game filling the list by name [orig:
// UI_UpdateDeathScreenContent @ 0x553a7d]; CONFIRM_YES leaves the mission [orig:
// DeathScreen_OnConfirmYes @ 0x553530, registered @ 0x554647]. The list and the leave button sit
// in DEATH_SHROUD, the window the game hides as the screen shows and reveals after the death
// [orig: DeathScreen_UpdateShroudReveal @ 0x554730].
bool make_blank_death_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string shroud =
	        panel("DEATH_SHROUD", {0, 0, 800, 600}, false,
	              text("STATIC_LIST_TITLE", "Deploy", {550, 50, 750}) +
	                      "<WINDOW type=\"list\" name=\"SPAWNPOINTS_LIST\">\n"
	                      "<APPEARANCE state=\"default\"></APPEARANCE>\n" +
	                      position({550, 80, 750, 350}) + "</WINDOW>\n" +
	                      button("HIDDEN_BACK", "Leave Mission", {550, 380, 750}, {{true, "VK_ESCAPE"}},
	                             {{"SHOW", "CONFIRM_EXIT"}, {"HIDE", "DEATH_SHROUD"}}));
	return blank_menu_bytes(screen("DEATH", shroud + confirm_exit("DEATH_SHROUD")), request, out, error);
}

// mp.mnu: the multiplayer screens. The game selects one by name, NW_MULTI_PLAYER, as the menu comes
// back from a NovaWorld session [orig: UI_EnterNovaWorldMenu @ 0x5588fa -> CUIScene_SelectNodeByName
// @ 0x558953; Menu_InitShellResources @ 0x5526a6]; the others are reached by authored actions. Its
// BACK pops the screen, an authored action [orig: CUIWidget_HandleScriptedAction @ 0x6497f0].
bool make_blank_mp_menu(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string back = "<WINDOW type=\"button\" name=\"BACK\">\n"
	                         "<ACTION type=\"POP_SCREEN\"></ACTION>\n"
	                         "<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>\n"
	                         "<APPEARANCE state=\"default\"></APPEARANCE>\n"
	                         "<APPEARANCE state=\"mouseover\"></APPEARANCE>\n"
	                         "<APPEARANCE state=\"selected\"></APPEARANCE>\n"
	                         "<APPEARANCE state=\"disabled\"></APPEARANCE>\n" +
	                         position({340, 540, 460}) + "<STRING justify=\"CENTER\">Back</STRING>\n</WINDOW>\n";
	return blank_menu_bytes(screen("NW_MULTI_PLAYER", back), request, out, error);
}

} // namespace opennova::editor
