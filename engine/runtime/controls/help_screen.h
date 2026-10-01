#pragma once

// The F1 key-binding help screen's page table: the live binding records
// sorted by class then by each row's help order, the unbound / Cheat / Debug
// rows dropped, split into pages of at most 23 rows that never straddle a
// class; one page is current and holds its rendered rows (the key text and
// the action's help text).
// [orig: KeyBinding_BuildHelpScreenTable @0x497340 (built at every mission
//  start by the profile apply sub_563620 @0x5636c4) -> BMS_PruneEmptyEntities
//  @0x496630 (the prune; its IDB name is a misnomer), qsort with
//  KeyBinding_CompareEntries @0x4965f0, KeyBinding_BuildCategoryPages
//  @0x4966c0, HelpScreen_BuildPage @0x4971b0; the page keys
//  HelpScreen_CyclePage @0x4972e0]

#include <runtime/controls/binding_set.h>

#include <string>
#include <vector>

namespace opennova::controls {

// Rows per page [orig: `++entries_in_page > 23` @0x4966c0; the 23-row build
// loop in HelpScreen_BuildPage].
inline constexpr int kHelpScreenRowsPerPage = 23;

struct HelpScreenRow {
	std::string key;  // the formatted binding [orig: row + 0]
	std::string text; // the action's help text [orig: row + 128]
};

class HelpScreen {
public:
	// Rebuild the table from the live records and render page 0.
	void build(const BindingSet &bindings);
	// PgDn (forward) / PgUp, wrapping, then re-render the page.
	void cycle_page(bool forward);

	int page_count() const { return static_cast<int>(pages_.size()); }
	int current_page() const { return current_; }
	// "Help - <class>" [orig: KeyHelp Text/HELPTITLE, "!Help - %s"].
	const std::string &title() const { return title_; }
	// "Page <n> of <count>" [orig: KeyHelp Text/PAGE, "!Page %i of %i"].
	const std::string &page_line() const { return page_line_; }
	// The 23 rows; a row past the page's last entry stays empty.
	const std::vector<HelpScreenRow> &rows() const { return rows_; }

private:
	struct Page {
		int category = 0;
		std::string name;
		int first = 0;
		int last = 0;
	};
	void build_page();

	std::vector<int> sorted_; // catalog indices in the sorted order, pruned rows dropped
	std::vector<BindingRecord> records_;
	std::vector<Page> pages_;
	int current_ = 0;
	std::string title_;
	std::string page_line_;
	std::vector<HelpScreenRow> rows_;
};

// The footer under the rows [orig: KeyHelp Text/CHANGE_SCREEN,
// "!PgUp and PgDn to change pages" @0x4978dd].
std::string help_screen_footer();

} // namespace opennova::controls
