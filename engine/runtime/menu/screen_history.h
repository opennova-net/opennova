#pragma once

// THE MENU'S SCREEN HISTORY: the one back stack the menu keeps across every menu
// file it shows and across a mission. A row names the screen a select left. When
// the menu mode is left for a mission the screen it was on is pushed with a mark
// row on top of it, and the menu's re-entry drops back to that mark and selects
// the screen under it again, so leaving a mission returns to the screen the
// mission was started from (SINGLE_PLAYER), not to the first screen (D-MNU-28).
//
// Retail keeps the rows on the menu scene (CUIScene +0x48: a LIFO of copied
// screen names, a mark being a row with no name; the +0x4C push gate is set by the
// constructor and by every clear, so it is always open), one list for every screen
// the scene has loaded, and a popped name is selected over all of them. The shell
// holds one menu file at a time, so a row here also names the file its screen is
// in. Unbounded, as retail's list is.
// [orig: UIScene_PushScreenHistory @0x63b350 (CUIScene_SelectNodeByName @0x63b79f
//  pushes the screen it leaves); UIScene_PopScreenHistory @0x63c410 (the POP_SCREEN
//  action, CUIWidget_HandleScriptedAction @0x649b7e); UIScene_MarkScreenHistory
//  @0x63c3d0 (the menu mode's teardown, Menu_TeardownShellAndCloseBinkVideos
//  @0x54e514); UIScene_ReturnToHistoryScreen @0x63dfa0 (the menu mode's re-entry,
//  Menu_InitShellResources @0x552682, pop_extra 1; an in-game screen's close,
//  UI_CloseMenuScreen @0x54e635, pop_extra 0); UIScene_ClearScreenHistory @0x63b3d0
//  (CUIScene_DestroyAllContent; the push gate @0x63b40e, CGameMenu_ctor @0x63e0d5)]

#include <string>
#include <vector>

namespace opennova::menu {

struct ScreenHistoryRow {
	std::string file;   // the menu file the screen is in
	std::string screen; // the screen's NAME; empty on a mark
	bool mark = false;
};

class ScreenHistory {
public:
	// The screen a select leaves, on top. No screen (nothing was current) pushes
	// nothing: the select pushes only when a screen was selected.
	// [orig: UIScene_PushScreenHistory @0x63b350; CUIScene_SelectNodeByName
	//  @0x63b757..0x63b79f (`if (current && push)`)]
	void push(const std::string &file, const std::string &screen);

	// Pop the top row when it names a screen (true, the row in *out, to be selected
	// with no push). A mark on top, or an empty history, pops nothing.
	// [orig: UIScene_PopScreenHistory @0x63c410: `node && node->name` @0x63c418]
	bool pop(ScreenHistoryRow *out);

	// The menu mode's leave: the current screen, then a mark on it. With no current
	// screen neither is laid.
	// [orig: UIScene_MarkScreenHistory @0x63c3d0: the current screen's name pushed
	//  @0x63c3dd, then a row with no name @0x63c3ea..0x63c3fe]
	void mark(const std::string &file, const std::string &screen);

	// Back to the newest mark: every named row above it is dropped. With
	// `pop_extra` the mark goes too, and when the row under it names a screen that
	// row is popped into *out (true), to be selected with no push. False when
	// nothing is to be selected: no `pop_extra`, no mark (the whole history then
	// dropped), or a mark under the mark.
	// [orig: UIScene_ReturnToHistoryScreen @0x63dfa0: the named rows @0x63dfa4..0x63dfdd,
	//  the mark @0x63dff7..0x63e015, the row selected @0x63e024..0x63e036]
	bool return_to_mark(bool pop_extra, ScreenHistoryRow *out);

	// [orig: UIScene_ClearScreenHistory @0x63b3d0]
	void clear() { rows_.clear(); }

	// Bottom first; back() is the top.
	const std::vector<ScreenHistoryRow> &rows() const { return rows_; }
	int size() const { return static_cast<int>(rows_.size()); }
	bool empty() const { return rows_.empty(); }

private:
	std::vector<ScreenHistoryRow> rows_;
};

} // namespace opennova::menu
