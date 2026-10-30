#pragma once

// The multiplayer end of round's STAT screen (stat.mnu's STAT, net-re §5.68): what the code does
// when the end-round transition opens it, six seconds after the S2C 0x1D announcement once the
// S2C 0x56 board has reassembled (hud/end_round_overlay.h EndRoundTransition carries that gate),
// and with the controls it registers on it [orig: UI_ProcessEndRoundScreenTransition @0x5b8600
// (every HUD frame while g_RoundOver && is_in_session, from HUD_DrawOverlayPanels @0x5c0072): the
// first pass Game_CloseInGameScreens + Game_InitRespawnState + Overlay_ComputeStatFieldColumnLayout(40,
// 984); every pass UI_ProcessMenuScreenFrame @0x5b8674 (the menu-screen pass, which closes the
// screen) then HUD_DrawEndRoundStatsOverlay @0x5b7cd0; UI_OpenMenuScreen("stat.mnu", "STAT")
// @0x5b863b once]. It lives in inmatch because it fills the table from the npwire board's feed
// (stat_screen_feed.h).

#include <string>
#include <vector>

#include <runtime/inmatch/stat_screen_feed.h>
#include <runtime/menu/menu_screen_controller.h>

namespace opennova::inmatch {

// What the STAT screen reads of the session (the role's folded end-of-round state) and asks of it.
class EndRoundScreenHost {
public:
	virtual ~EndRoundScreenHost() = default;
	// The session game type's team arm (g_GameType & 0x10000; base/gameprofile/game_type.h).
	virtual bool team_mode() = 0;
	// The RESULTLIST's columns for a table `table_width` design units wide, their headers resolved
	// through the gametext Overlays table; every row; the local player entity's slot id (+0x154).
	virtual std::vector<StatScreenColumn> columns(int table_width) = 0;
	virtual std::vector<StatScreenRow> rows() = 0;
	virtual int local_slot() = 0;
	// The screen closes now (the exit's YES).
	virtual void close_screen() = 0;
	// CONFIRM_YES, after the close: the mission exits (the same close-screens + action-3 pair as
	// the pause menu's CONFIRM_YES; docs/mnu/menu-re.md "The in-game exit confirmation").
	virtual void exit_to_menu() = 0;
};

class EndRoundScreen final : public menu::MenuScreenController {
public:
	static constexpr const char *kFile = "stat.mnu";
	static constexpr const char *kScreen = "STAT";
	// The RESULTLIST's authored width (jo_stat.mnu: the STATS window spans 20..770), what the width
	// split reads when the frame has not laid the table out.
	static constexpr int kResultListDefaultWidth = 750;

	void set_host(EndRoundScreenHost *host) { host_ = host; }
	// The tab the stat filter shows last (0 all, 1 team 2, 2 team 1), -1 before any.
	int tab() const { return tab_; }

	// MenuScreenController: the STAT show callback (the table filled, the tabs by the game type);
	// the tab radios and CONFIRM_YES.
	void open(menu::MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) override;

private:
	EndRoundScreenHost *host_ = nullptr;
	int tab_ = -1;
};

} // namespace opennova::inmatch
