#pragma once

// The menu screen a running mission has up (the screens UI_OpenMenuScreen opens by name,
// menu_commands.h mission_menu_screens) and the main frame's input law under it. A screen is
// up while the menu-screen word holds; the frame then runs the screen pass instead of its own
// input frame, and the screen pass runs the input frame only under the menu's pass-through
// word, which only DEATH and STAT raise (DEATH's update and STAT's show) and drop (their
// deactivations, DEATH's HIDDEN_BACK and CONFIRM_NO). So under INGAME, WEAPON, CMAP and
// VEHICLE no input frame runs (controls::PlayerActionPoll::menu_open), and under DEATH and
// STAT the whole frame runs while the word holds.
// [orig: UI_IsMenuScreenOpen @0x54B970 (dword_255110C); Game_ProcessMainFrame @0x5263F0 - the
//  UI_IsMenuScreenOpen call @0x52660D, the Input_ProcessFrame call @0x52661D;
//  UI_ProcessMenuScreenFrame @0x54E650 - g_MenuInputPassThrough tested @0x54E686, the
//  Input_ProcessFrame call @0x54E695; UI_SetMenuInputPassThrough @0x54B980, its callers
//  DeathScreen_UpdateUI @0x553150 (@0x55316E), DeathScreen_OnDeactivate @0x553510 (@0x553512),
//  DeathScreen_OnHiddenBack @0x553660 (@0x55366C), DeathScreen_OnConfirmNo @0x553680
//  (@0x55368C), StatScreen_OnDeactivate @0x562120 (@0x562122), StatScreen_ShowCallback
//  @0x562840 (@0x562877 / @0x5628D6)]

#include <cstdint>

namespace opennova::menu {

enum class MissionScreen : uint8_t {
	None,
	Ingame,     // game.mnu INGAME (the pause)
	Weapon,     // weapon.mnu WEAPON (the armory)
	CommandMap, // cmap.mnu CMAP
	Vehicle,    // vehicle.mnu VEHICLE
	Stat,       // stat.mnu STAT (the end of round)
	Death,      // death.mnu DEATH (the deploy screen)
};

// Whether a menu screen is up (UI_IsMenuScreenOpen).
constexpr bool mission_screen_open(MissionScreen screen) {
	return screen != MissionScreen::None;
}

// Whether the screen up runs the input frame: the pass-through word holds, which only DEATH and
// STAT raise.
constexpr bool mission_screen_passes_input(MissionScreen screen, bool pass_through_word) {
	return pass_through_word && (screen == MissionScreen::Death || screen == MissionScreen::Stat);
}

// Whether the screen up holds the input frame (controls::PlayerActionPoll::menu_open): one is up
// and its input does not pass through.
constexpr bool mission_screen_holds_input(MissionScreen screen, bool pass_through_word) {
	return mission_screen_open(screen) && !mission_screen_passes_input(screen, pass_through_word);
}

} // namespace opennova::menu
