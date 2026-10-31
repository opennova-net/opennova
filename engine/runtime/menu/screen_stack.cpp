#include "screen_stack.h"

namespace opennova::menu {

bool ScreenStack::open(MissionScreen screen) {
	// The open word refuses a second screen [orig: UI_OpenMenuScreen @0x54E52C]; the select
	// leaves the request clear [orig: @0x54E5A8].
	if (is_open() || screen == MissionScreen::None) return false;
	screen_ = screen;
	close_request_ = false;
	return true;
}

bool ScreenStack::close() {
	// Nothing while no screen is up [orig: UI_CloseMenuScreen @0x54E5E7]; the word and the
	// request clear together [orig: @0x54E60A, @0x54E614].
	if (!is_open()) return false;
	screen_ = MissionScreen::None;
	close_request_ = false;
	return true;
}

bool ScreenStack::close(MissionScreen screen) {
	return screen != MissionScreen::None && screen_ == screen && close();
}

const char *mission_screen_state_name(MissionScreen screen) {
	switch (screen) {
	case MissionScreen::Ingame: return "paused";
	case MissionScreen::Weapon: return "armory";
	case MissionScreen::CommandMap: return "command_map";
	case MissionScreen::Vehicle: return "vehicle";
	case MissionScreen::Stat: return "end_round";
	case MissionScreen::Death: return "deploy";
	case MissionScreen::None: break;
	}
	return nullptr;
}

} // namespace opennova::menu
