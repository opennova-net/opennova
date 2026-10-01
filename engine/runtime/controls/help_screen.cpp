#include <runtime/controls/help_screen.h>

#include <runtime/controls/controls.h>
#include <runtime/controls/key_strings.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace opennova::controls {
namespace {

// Each catalog record's help order, the dword at +0x10 the sort reads
// second (0 sorts last as 999999) [orig: KeyBinding_CompareEntries @0x4965f0;
// the static table word_8159A8, 108-byte records].
constexpr int kHelpOrder[] = {
	0, 0, 1001, 1003, 1005, 1007, 1009, 1011, 1013, 1015,                     // 0-9
	1017, 1019, 1021, 1022, 1023, 1024, 1025, 2031, 2032, 2033,               // 10-19
	2034, 2035, 2036, 2037, 2038, 2039, 2040, 0, 2001, 2002,                  // 20-29
	2003, 2004, 2005, 2006, 2007, 2008, 2009, 2025, 2021, 2017,               // 30-39
	2016, 0, 0, 2013, 2031, 5014, 5015, 2012, 4003, 4002,                     // 40-49
	4004, 4005, 4006, 2023, 5000, 5001, 5003, 5004, 5005, 5006,               // 50-59
	5007, 5008, 5009, 5012, 5010, 0, 0, 10005, 5013, 10010,                   // 60-69
	10000, 10001, 10002, 10004, 10011, 10012, 10013, 10014, 10007, 10015,     // 70-79
	10006, 10016, 10017, 0, 6012, 6002, 6003, 6001, 6006, 6007,               // 80-89
	6008, 6009, 6010, 6005, 6011, 6013, 6014, 0, 4001, 10009,                 // 90-99
	5011, 5013, 5014, 2019, 2020, 2015, 10003, 3001, 3002, 3003,              // 100-109
	80001, 80002, 80003,                                                      // 110-112
};

int help_order(int id) {
	const int order = id >= 0 && id < static_cast<int>(std::size(kHelpOrder)) ? kHelpOrder[id] : 0;
	return order != 0 ? order : 999999; // [orig: @0x4965f0 — 0 -> 999999]
}

// The page title key per class [orig: KeyBinding_BuildCategoryPages
// @0x4966c0 — KeyHelp_GetStringWithFallback("Text", key, "!Name") per case;
// case 4's key is the string at off_7C7874].
void class_title(int category, const char **key, const char **fallback) {
	switch (category) {
		case 0: *key = "NULL"; *fallback = "!Null"; return;
		case 1: *key = "MOVEMENT"; *fallback = "!Movement"; return;
		case 2: *key = "WEAPONS"; *fallback = "!Weapons"; return;
		case 3: *key = "CAMERA"; *fallback = "!Camera"; return;
		case 4: *key = "MAP"; *fallback = "!Map"; return;
		case 5: *key = "COMMUNICATIONS"; *fallback = "!Communications"; return;
		case 6: *key = "SERVER"; *fallback = "!Server"; return;
		case 7: *key = "NOVALOGIC"; *fallback = "!NovaLogic"; return;
		case 9: *key = "CHEAT"; *fallback = "!Cheat"; return;
		case 10: *key = "SYSTEM"; *fallback = "!System"; return;
		case 11: *key = "DEBUG"; *fallback = "!Debug"; return;
		case 12: *key = "teammatemenu"; *fallback = "!teammatemenu"; return;
		case 13: *key = "SPECTATOR"; *fallback = "!Spectator"; return;
		default: *key = "UNKNOWN"; *fallback = "!Unknown Type"; return;
	}
}

std::string format_printf(const std::string &format, const std::string &arg) {
	char buf[256];
	std::snprintf(buf, sizeof(buf), format.c_str(), arg.c_str());
	return buf;
}

} // namespace

void HelpScreen::build(const BindingSet &bindings) {
	std::size_t count = 0;
	const ActionDef *cat = catalog(&count);
	// The prune: a record with no key, no second key and no mouse button, or
	// of class Cheat (9) or Debug (11), leaves the table (its class byte
	// zeroed, so it sorts ahead of every page) [orig: BMS_PruneEmptyEntities
	// @0x496630 — the +20/+22/+24 words, the class tests].
	records_.assign(count, BindingRecord{});
	sorted_.clear();
	for (std::size_t i = 0; i < count; ++i) {
		const BindingRecord *rec = bindings.record(static_cast<int>(i));
		if (rec != nullptr) records_[i] = *rec;
		const int category = static_cast<int>(cat[i].cls);
		const BindingRecord &r = records_[i];
		if (category == 0 || category == 9 || category == 11) continue;
		if (r.primary == 0 && r.secondary == 0 && r.mouse_mask == 0) continue;
		sorted_.push_back(static_cast<int>(i));
	}
	// Class, then help order. Retail's qsort leaves equal keys in an
	// implementation order; the stable sort keeps them in catalog order
	// [orig: qsort(.., 0x300, 0x6C, KeyBinding_CompareEntries) @0x497364].
	std::stable_sort(sorted_.begin(), sorted_.end(), [cat](int a, int b) {
		const int ca = static_cast<int>(cat[a].cls);
		const int cb = static_cast<int>(cat[b].cls);
		if (ca != cb) return ca < cb;
		return help_order(cat[a].id) < help_order(cat[b].id);
	});
	// The pages: a new one opens past 23 rows or on a class change, the
	// previous one closing on the row before; the last closes on the table's
	// end [orig: KeyBinding_BuildCategoryPages @0x4966c0; the 767 close].
	pages_.clear();
	int prev_category = 0;
	int in_page = 0;
	for (int i = 0; i < static_cast<int>(sorted_.size()); ++i) {
		const int category = static_cast<int>(cat[sorted_[static_cast<std::size_t>(i)]].cls);
		const bool changed = category != prev_category;
		if (++in_page > kHelpScreenRowsPerPage || changed) {
			if (!pages_.empty()) pages_.back().last = i - 1;
			Page page;
			page.category = category;
			const char *key = nullptr;
			const char *fallback = nullptr;
			class_title(category, &key, &fallback);
			page.name = key_help_string("Text", key, fallback);
			page.first = i;
			pages_.push_back(page);
			in_page = 1;
		}
		prev_category = category;
	}
	if (!pages_.empty()) pages_.back().last = static_cast<int>(sorted_.size()) - 1;
	// The table build clears the current page with the page table
	// [orig: memset(&byte_B220D8[2240], 0, 0xB88) @0x4966c0].
	current_ = 0;
	build_page();
}

void HelpScreen::cycle_page(bool forward) {
	// [orig: HelpScreen_CyclePage @0x4972e0 — forward wraps past the count to
	// 0, back wraps below 0 to the last page; then the page rebuild]
	const int count = page_count();
	if (forward) {
		current_ = current_ + 1 >= count ? 0 : current_ + 1;
	} else {
		current_ = current_ <= 0 ? count - 1 : current_ - 1;
	}
	build_page();
}

void HelpScreen::build_page() {
	// [orig: HelpScreen_BuildPage @0x4971b0]
	rows_.assign(kHelpScreenRowsPerPage, HelpScreenRow{});
	title_.clear();
	page_line_.clear();
	if (current_ < 0 || current_ >= page_count()) return;
	const Page &page = pages_[static_cast<std::size_t>(current_)];
	title_ = format_printf(key_help_string("Text", "HELPTITLE", "!Help - %s"), page.name);
	char line[128];
	std::snprintf(line, sizeof(line), key_help_string("Text", "PAGE", "!Page %i of %i").c_str(),
			current_ + 1, page_count());
	page_line_ = line;
	std::size_t count = 0;
	const ActionDef *cat = catalog(&count);
	for (int row = 0; row < kHelpScreenRowsPerPage; ++row) {
		const int index = page.first + row;
		if (index > page.last) break;
		const int id = sorted_[static_cast<std::size_t>(index)];
		const ActionDef &def = cat[id];
		// The key text through the display formatter (the flag word's 0x200
		// marks " *"); the help text is the Text table's token entry, else the
		// record's own "!"-marked name [orig: KeyBinding_FormatDisplayString
		// @0x496bd0 over the record; KeyHelp_GetStringWithFallback("Text",
		// token, record + 0x23)].
		HelpScreenRow &out = rows_[static_cast<std::size_t>(row)];
		out.key = format_display_string(records_[static_cast<std::size_t>(id)],
				(def.flags & 0x200u) != 0);
		const std::string marked = std::string("!") + def.name;
		out.text = key_help_string("Text", def.token, marked.c_str());
	}
}

std::string help_screen_footer() {
	return key_help_string("Text", "CHANGE_SCREEN", "!PgUp and PgDn to change pages");
}

} // namespace opennova::controls
