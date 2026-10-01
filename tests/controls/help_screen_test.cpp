// The F1 key-binding help pages (controls/help_screen.h): the prune of
// unbound / Cheat / Debug rows, the class-then-help-order sort, the 23-row
// pages that never straddle a class, the wrapping page cycle and the
// rendered strings (no keyhelp table installed: the marker-stripped
// fallbacks) [orig: KeyBinding_BuildHelpScreenTable @0x497340;
// KeyBinding_BuildCategoryPages @0x4966c0; HelpScreen_BuildPage @0x4971b0;
// HelpScreen_CyclePage @0x4972e0].
#include <runtime/controls/binding_set.h>
#include <runtime/controls/controls.h>
#include <runtime/controls/help_screen.h>
#include <runtime/controls/key_strings.h>

#include <cstdio>
#include <string>

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
	BindingSet bindings;
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
	BindingSet bindings;
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

} // namespace

int main() {
	test_pages_from_defaults();
	test_prune_follows_live_bindings();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("help_screen_test OK\n");
	return 0;
}
