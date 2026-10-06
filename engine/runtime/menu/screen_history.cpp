#include <runtime/menu/screen_history.h>

namespace opennova::menu {

// [orig: UIScene_PushScreenHistory @0x63b350, called only past the select's
//  `current && push` test @0x63b75c..0x63b792]
void ScreenHistory::push(const std::string &file, const std::string &screen) {
	if (screen.empty()) return;
	ScreenHistoryRow row;
	row.file = file;
	row.screen = screen;
	rows_.push_back(std::move(row));
}

// [orig: UIScene_PopScreenHistory @0x63c410: `node && node->name` @0x63c418]
bool ScreenHistory::pop(ScreenHistoryRow *out) {
	if (rows_.empty() || rows_.back().mark) return false;
	if (out != nullptr) *out = rows_.back();
	rows_.pop_back();
	return true;
}

// [orig: UIScene_MarkScreenHistory @0x63c3d0: no current screen @0x63c3d8 lays
//  nothing; the name @0x63c3dd, then the row with no name @0x63c3ea..0x63c3fe]
void ScreenHistory::mark(const std::string &file, const std::string &screen) {
	if (screen.empty()) return;
	push(file, screen);
	ScreenHistoryRow row;
	row.mark = true;
	rows_.push_back(std::move(row));
}

// [orig: UIScene_ReturnToHistoryScreen @0x63dfa0]
bool ScreenHistory::return_to_mark(bool pop_extra, ScreenHistoryRow *out) {
	// The named rows above the newest mark (@0x63dfa4..0x63dfdd: the walk stops at
	// the first row with no name).
	while (!rows_.empty() && !rows_.back().mark) rows_.pop_back();
	// `extra_node && pop_extra` @0x63dff7: the mark goes (@0x63dffc..0x63e015), and the
	// row under it is popped and selected with no push when it names a screen
	// (@0x63e024..0x63e036).
	if (rows_.empty() || !pop_extra) return false;
	rows_.pop_back();
	return pop(out);
}

} // namespace opennova::menu
