// The menu's screen history (menu/screen_history.h) and the menu mode's leave and
// re-entry over it (MenuRuntime::leave_menu_mode / return_to_menu_mode): leaving a
// mission returns to the screen the mission was started from (D-MNU-28).
// [orig: UIScene_PushScreenHistory @0x63b350; UIScene_PopScreenHistory @0x63c410;
//  UIScene_MarkScreenHistory @0x63c3d0; UIScene_ReturnToHistoryScreen @0x63dfa0;
//  Menu_TeardownShellAndCloseBinkVideos @0x54e514; Menu_InitShellResources @0x552682]
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/screen_history.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

bool row_is(const ScreenHistoryRow &row, const char *file, const char *screen) {
	return !row.mark && row.file == file && row.screen == screen;
}

// The rows bottom first, a mark spelled "|".
std::string spell(const ScreenHistory &history) {
	std::string out;
	for (const ScreenHistoryRow &row : history.rows()) {
		if (!out.empty()) out += ' ';
		out += row.mark ? std::string("|") : row.file + ":" + row.screen;
	}
	return out;
}

void test_push_and_pop() {
	ScreenHistory h;
	ScreenHistoryRow row;
	CHECK(!h.pop(&row)); // an empty history pops nothing
	h.push("main.mnu", "STARTUP");
	h.push("sp.mnu", "SINGLE_PLAYER");
	h.push("sp.mnu", "SINGLE_PLAYER"); // the same screen pushes again, as the select does
	h.push("main.mnu", "");            // no screen was current: nothing
	CHECK(h.size() == 3);
	CHECK(h.pop(&row) && row_is(row, "sp.mnu", "SINGLE_PLAYER"));
	CHECK(h.pop(nullptr));
	CHECK(h.pop(&row) && row_is(row, "main.mnu", "STARTUP"));
	CHECK(h.empty() && !h.pop(&row));
}

void test_mark_and_return() {
	ScreenHistory h;
	ScreenHistoryRow row;
	h.push("main.mnu", "STARTUP");
	// The menu mode's leave: the current screen, then the mark on it.
	h.mark("sp.mnu", "SINGLE_PLAYER");
	CHECK(spell(h) == "main.mnu:STARTUP sp.mnu:SINGLE_PLAYER |");
	// A mark on top pops nothing (the in-game back finds no screen).
	CHECK(!h.pop(&row) && h.size() == 3);
	// The in-game screens push above the mark; an in-game close drops them, the mark kept.
	h.push("sp.mnu", "SINGLE_PLAYER");
	h.push("game.mnu", "INGAME");
	CHECK(!h.return_to_mark(false, &row));
	CHECK(spell(h) == "main.mnu:STARTUP sp.mnu:SINGLE_PLAYER |");
	// The re-entry: the rows above the mark and the mark go, the screen under it is popped.
	h.push("game.mnu", "INGAME");
	row = ScreenHistoryRow();
	CHECK(h.return_to_mark(true, &row) && row_is(row, "sp.mnu", "SINGLE_PLAYER"));
	CHECK(spell(h) == "main.mnu:STARTUP");
	// No mark: every row is dropped and nothing is selected.
	CHECK(!h.return_to_mark(true, &row) && h.empty());
	// No current screen at the leave: neither the screen nor the mark is laid.
	h.push("main.mnu", "STARTUP");
	h.mark("", "");
	CHECK(spell(h) == "main.mnu:STARTUP");
	// Each return takes the newest mark only.
	h.clear();
	h.mark("main.mnu", "STARTUP");
	h.mark("sp.mnu", "SINGLE_PLAYER");
	CHECK(h.return_to_mark(true, &row) && row_is(row, "sp.mnu", "SINGLE_PLAYER"));
	CHECK(spell(h) == "main.mnu:STARTUP |");
	// The trim with the mark on top changes nothing.
	CHECK(!h.return_to_mark(false, &row) && spell(h) == "main.mnu:STARTUP |");
	CHECK(h.return_to_mark(true, &row) && row_is(row, "main.mnu", "STARTUP") && h.empty());
}

mnu::Document document(const std::vector<const char *> &screens) {
	mnu::Document doc;
	for (const char *name : screens) {
		mnu::Screen screen;
		screen.name = name;
		screen.root_window.name = std::string(name) + "_ROOT";
		screen.root_window.type = mnu::WindowType::Window;
		doc.screens.push_back(screen);
	}
	return doc;
}

// The witnessed flow: STARTUP -> SINGLE_PLAYER (another file) -> a mission -> the
// in-game menu -> LEAVE MISSION -> back on SINGLE_PLAYER, STARTUP under it.
void test_mission_leave_returns_to_the_starting_screen() {
	const mnu::Document main_doc = document({ "STARTUP" });
	const mnu::Document sp_doc = document({ "SINGLE_PLAYER" });
	const mnu::Document game_doc = document({ "INGAME" });
	MenuRuntime rt;
	CHECK(rt.open_document(&main_doc, "main.mnu", ""));
	// The shell's cross-file jump pushes the screen it leaves.
	rt.screen_history().push(rt.menu_file(), rt.current_screen());
	CHECK(rt.open_document(&sp_doc, "sp.mnu", "SINGLE_PLAYER"));
	rt.leave_menu_mode();
	CHECK(spell(rt.screen_history()) == "main.mnu:STARTUP sp.mnu:SINGLE_PLAYER |");
	// The in-game menu is another document; the history is the runtime's, not the file's.
	CHECK(rt.open_document(&game_doc, "game.mnu", "INGAME"));
	ScreenHistoryRow row;
	CHECK(!rt.screen_history().pop(&row)); // the in-game back pops nothing past the mark
	rt.screen_history().push("game.mnu", "INGAME");
	rt.trim_screen_history();
	CHECK(spell(rt.screen_history()) == "main.mnu:STARTUP sp.mnu:SINGLE_PLAYER |");
	// The re-entry.
	CHECK(rt.return_to_menu_mode(&row) && row_is(row, "sp.mnu", "SINGLE_PLAYER"));
	CHECK(spell(rt.screen_history()) == "main.mnu:STARTUP");
	// A second leave with the menu shown on STARTUP returns there.
	CHECK(rt.open_document(&main_doc, "main.mnu", ""));
	rt.leave_menu_mode();
	CHECK(rt.return_to_menu_mode(&row) && row_is(row, "main.mnu", "STARTUP"));
	CHECK(spell(rt.screen_history()) == "main.mnu:STARTUP");
}

// The file's own back stack is part of the one history: a mission started from a
// screen reached inside its file comes back to it with that file's earlier screens
// under it, in order (mp.mnu: LAN_MULTI_PLAYER -> MULTI_PLAYER_HOST).
void test_leave_moves_the_file_stack_into_the_history() {
	const mnu::Document mp_doc = document({ "LAN_MULTI_PLAYER", "MULTI_PLAYER_HOST", "X" });
	MenuRuntime rt;
	rt.screen_history().push("main.mnu", "STARTUP");
	CHECK(rt.open_document(&mp_doc, "mp.mnu", "LAN_MULTI_PLAYER"));
	CHECK(rt.navigate_to_screen("X"));
	CHECK(rt.navigate_to_screen("MULTI_PLAYER_HOST"));
	rt.leave_menu_mode();
	CHECK(spell(rt.screen_history()) ==
			"main.mnu:STARTUP mp.mnu:LAN_MULTI_PLAYER mp.mnu:X mp.mnu:MULTI_PLAYER_HOST |");
	// The file's own stack moved: a back inside the file is now the history's.
	CHECK(!rt.pop_screen());
	ScreenHistoryRow row;
	CHECK(rt.return_to_menu_mode(&row) && row_is(row, "mp.mnu", "MULTI_PLAYER_HOST"));
	CHECK(rt.screen_history().pop(&row) && row_is(row, "mp.mnu", "X"));
	CHECK(rt.screen_history().pop(&row) && row_is(row, "mp.mnu", "LAN_MULTI_PLAYER"));
	CHECK(rt.screen_history().pop(&row) && row_is(row, "main.mnu", "STARTUP"));
	CHECK(rt.screen_history().empty());
	// No document: nothing is current, so nothing is marked.
	MenuRuntime bare;
	bare.leave_menu_mode();
	CHECK(bare.screen_history().empty() && !bare.return_to_menu_mode(&row));
}

} // namespace

int main() {
	test_push_and_pop();
	test_mark_and_return();
	test_mission_leave_returns_to_the_starting_screen();
	test_leave_moves_the_file_stack_into_the_history();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("screen_history_test OK\n");
	return 0;
}
