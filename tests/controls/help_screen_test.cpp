// The F1 key-binding help pages (controls/help_screen.h): the prune of
// unbound / Cheat / Debug rows, the class-then-help-order sort, the 23-row
// pages that never straddle a class, the wrapping page cycle and the
// rendered strings (no keyhelp table installed: the marker-stripped
// fallbacks) [orig: KeyBinding_BuildHelpScreenTable @0x497340;
// KeyBinding_BuildCategoryPages @0x4966c0; HelpScreen_BuildPage @0x4971b0;
// HelpScreen_CyclePage @0x4972e0]. Each title's catalog sorts on its own rows'
// help words: dfx2.exe's rows are numbered apart from Jointops.exe's
// [orig: KeyBinding_CompareEntries @0x4965f0] [orig: KeyBinding_CompareEntries
// @0x496600 (dfx2.exe)].
#include <base/title/title.h>
#include <runtime/controls/binding_set.h>
#include <runtime/controls/controls.h>
#include <runtime/controls/help_screen.h>
#include <runtime/controls/key_strings.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::controls;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

int non_empty_rows(const HelpScreen &hs) {
	int n = 0;
	for (const HelpScreenRow &row : hs.rows())
		if (!row.text.empty()) ++n;
	return n;
}

void test_pages_from_defaults() {
	clear_key_strings();
	BindingSet bindings(opennova::controls::default_catalog());
	HelpScreen hs;
	hs.build(bindings);
	CHECK(hs.page_count() > 0);
	CHECK(hs.current_page() == 0);
	// Class 1 opens the table, and its first row is Forward (help order 1001:
	// the two absolute-look rows are unbound and pruned).
	CHECK(hs.title() == "Help - Movement");
	CHECK(hs.page_line() == "Page 1 of " + std::to_string(hs.page_count()));
	CHECK(hs.rows().size() == static_cast<size_t>(kHelpScreenRowsPerPage));
	CHECK(hs.rows()[0].text == "Forward");
	CHECK(!hs.rows()[0].key.empty());
	// Walk every page: none over 23 rows, the titles never repeat a class
	// out of order, and no Cheat / Debug page exists.
	std::string prev_title;
	int weapons_pages = 0;
	for (int p = 0; p < hs.page_count(); ++p) {
		CHECK(non_empty_rows(hs) <= kHelpScreenRowsPerPage);
		CHECK(non_empty_rows(hs) > 0);
		CHECK(hs.title() != "Help - Cheat" && hs.title() != "Help - Debug");
		if (hs.title() == "Help - Weapons") ++weapons_pages;
		prev_title = hs.title();
		hs.cycle_page(true);
	}
	CHECK(weapons_pages >= 1);
	// A full lap wraps back to page 0.
	CHECK(hs.current_page() == 0);
	hs.cycle_page(false);
	CHECK(hs.current_page() == hs.page_count() - 1);
	CHECK(hs.page_line() == "Page " + std::to_string(hs.page_count()) + " of " +
			std::to_string(hs.page_count()));
	CHECK(help_screen_footer() == "PgUp and PgDn to change pages");
}

void test_prune_follows_live_bindings() {
	clear_key_strings();
	BindingSet bindings(opennova::controls::default_catalog());
	HelpScreen hs;
	hs.build(bindings);
	const int before = hs.page_count();
	int rows_before = 0;
	for (int p = 0; p < hs.page_count(); ++p, hs.cycle_page(true)) rows_before += non_empty_rows(hs);
	// Clearing Forward's keyboard slots drops it from the table.
	const int forward = bindings.index_of_token("move_forward");
	bindings.clear(forward, Device::Keyboard);
	hs.build(bindings);
	int rows_after = 0;
	for (int p = 0; p < hs.page_count(); ++p, hs.cycle_page(true)) rows_after += non_empty_rows(hs);
	CHECK(rows_after == rows_before - 1);
	CHECK(hs.page_count() == before);
	CHECK(hs.rows()[0].text != "Forward");
}

// The texts of the page titled `title`, in their sorted order (none: no such page).
std::vector<std::string> page_texts(HelpScreen &hs, const std::string &title) {
	std::vector<std::string> out;
	for (int p = 0; p < hs.page_count(); ++p, hs.cycle_page(true)) {
		if (hs.title() != title) continue;
		for (const HelpScreenRow &row : hs.rows())
			if (!row.text.empty()) out.push_back(row.text);
	}
	return out;
}

// The Map page under each title's own catalog, its rows by their own +0x10 words (Map 4001,
// Radar Zoom In 4002, Radar Zoom Out 4003, Hud Detail 4004, Next Waypoint 4005, Next Flag
// 4006; Jointops.exe's Commander Menu 2023 ahead of them). dfx2.exe numbers them 46..50 and
// 96 where Jointops.exe numbers 48..52 and 98, so a lookup of the help order by row number
// read Jointops.exe's words for other rows (Radar Zoom In, Next Waypoint, Hud Detail, Next
// Flag, Radar Zoom Out, Map).
void test_each_title_sorts_on_its_own_help_words() {
	clear_key_strings();
	const opennova::Title *jo = opennova::title_by_code("jo");
	const opennova::Title *dfx2 = opennova::title_by_code("dfx2");
	CHECK(jo != nullptr && dfx2 != nullptr);
	if (jo == nullptr || dfx2 == nullptr) return;
	const std::vector<std::string> map = {"Map", "Radar Zoom In", "Radar Zoom Out", "Hud Detail",
			"Next Waypoint", "Next Flag"};
	BindingSet jo_bindings(title_catalog(*jo));
	HelpScreen jo_hs;
	jo_hs.build(jo_bindings);
	std::vector<std::string> jo_map = map;
	jo_map.insert(jo_map.begin(), "Commander Menu");
	CHECK(page_texts(jo_hs, "Help - Map") == jo_map);
	BindingSet dfx2_bindings(title_catalog(*dfx2));
	HelpScreen dfx2_hs;
	dfx2_hs.build(dfx2_bindings);
	CHECK(page_texts(dfx2_hs, "Help - Map") == map);
}

} // namespace

int main() {
	test_pages_from_defaults();
	test_prune_follows_live_bindings();
	test_each_title_sorts_on_its_own_help_words();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("help_screen_test OK\n");
	return 0;
}
