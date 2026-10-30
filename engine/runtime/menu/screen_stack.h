#pragma once

// THE IN-MISSION SCREEN STACK: the menu screen a running mission has up over it. Retail keeps one
// screen at a time on the menu scene: the open word holds while it is up, an open while one is up
// does nothing, the screen's select pushes the screen it leaves onto the screen history and the
// close returns the history to its mark (menu/screen_history.h, the stack the in-game screens
// grow above the menu's mark). The screens' own commands never close the screen at once: they
// latch the close request (Game_CloseInGameScreens, which the options dialog's modal hold
// refuses), and the next screen pass closes it before its frame (UI_ProcessMenuScreenFrame). A
// close runs the whole input reset after the history's return (Input_ResetAllState), so the
// embedder's close leg trims the history and resets the input when close() reports it ran. The
// pass-through word is the menu's one word for DEATH and STAT (menu/mission_screen.h); a close
// leaves it as it stands.
// [orig: UI_OpenMenuScreen @0x54E520 - the open word's test @0x54E526..0x54E52C (dword_255110C),
//  the select with its push CUIScene_SelectNodeByName(g_GameMenu, name, 1) @0x54E599, the word
//  @0x54E59E, the close request's clear @0x54E5A8; UI_CloseMenuScreen @0x54E5E0 - the word's
//  test @0x54E5E7, its clear @0x54E60A, the request's @0x54E614,
//  UIScene_ReturnToHistoryScreen(g_GameMenu, 0) @0x54E635, Input_ResetAllState @0x54E63F;
//  Game_CloseInGameScreens @0x54B940 - g_OptionsModalState's test @0x54B948, the request
//  g_MenuScreenCloseRequest = 1 @0x54B94A; UI_ProcessMenuScreenFrame @0x54E650 - the requested
//  close @0x54E657..0x54E659; UI_IsMenuScreenOpen @0x54B970]

#include <runtime/menu/mission_screen.h>

namespace opennova::menu {

class ScreenStack {
public:
	// UI_OpenMenuScreen: `screen` comes up, the close request cleared. False, nothing changed,
	// while a screen is up (the open word), or for MissionScreen::None.
	bool open(MissionScreen screen);

	// UI_CloseMenuScreen: the screen up goes, the open word and the close request cleared. True
	// when one was up: the caller's close leg then returns the screen history to its mark and
	// runs the whole input reset. False (nothing changed) while none is up.
	bool close();

	// Close `screen` when it is the one up (a screen's own close edge). True when it closed.
	bool close(MissionScreen screen);

	// Game_CloseInGameScreens: the close request, which the next screen pass runs. The options
	// dialog's modal hold (`options_modal`) refuses it.
	void request_close(bool options_modal = false) {
		if (!options_modal) close_request_ = true;
	}
	bool close_requested() const { return close_request_; }

	// UI_ProcessMenuScreenFrame's head: a requested close. True when it closed a screen (the
	// close leg is owed, as for close()).
	bool run_requested_close() { return close_request_ && close(); }

	// UI_SetMenuInputPassThrough: the menu's pass-through word (DEATH's and STAT's).
	void set_pass_through(bool word) { pass_through_ = word; }
	bool pass_through() const { return pass_through_; }

	MissionScreen current() const { return screen_; }
	// UI_IsMenuScreenOpen.
	bool is_open() const { return mission_screen_open(screen_); }
	// The screen up runs the input frame (its pass-through word holds: DEATH, STAT).
	bool passes_input() const { return mission_screen_passes_input(screen_, pass_through_); }
	// The screen up holds the input frame (controls::PlayerActionPoll::menu_open).
	bool holds_input() const { return mission_screen_holds_input(screen_, pass_through_); }

	// The process's start state: no screen, no request, the word down.
	void clear() { *this = ScreenStack{}; }

private:
	MissionScreen screen_ = MissionScreen::None;
	bool close_request_ = false; // g_MenuScreenCloseRequest
	bool pass_through_ = false;  // g_MenuInputPassThrough
};

// The tooling's name for the screen up ("paused" the INGAME screen, "armory" WEAPON, "deploy"
// DEATH, "end_round" STAT, "command_map" CMAP, "vehicle" VEHICLE); null for None.
const char *mission_screen_state_name(MissionScreen screen);

} // namespace opennova::menu
