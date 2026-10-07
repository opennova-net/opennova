#include <runtime/menu/mod_list.h>

#include <base/io/strutil.h>

namespace opennova::menu {

namespace {

// A row's value: the record it lists, -1 the base game's row [orig: UIList_AddRow's value,
// -1 @ 0x55a00d and the record's index @ 0x55a03a].
int row_value(int row) { return row - 1; }

} // namespace

void ModList::describe(MenuRuntime &menu, const std::string &text) const {
	for (const std::string &name : descriptions_) {
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_text(id, text);
	}
}

// [orig: Options_PopulateModList @ 0x559fb0 — every row removed @ 0x559fd8; the base row's text
//  UIStringTable_LookupAndDup(CWnd_GetInheritedTextRsrc(list), "Joint Operations: Typhoon Rising")
//  @ 0x559fe2..0x55a000, added with the value -1 @ 0x55a012 and selected @ 0x55a01c; a row per
//  record @ 0x55a036..0x55a077, its display name @ 0x55a03b, selected where its directory
//  _stricmps g_ExpansionName @ 0x55a04e; MOD_DESC's SetText of that record's description
//  @ 0x55a084..0x55a0b3, only when one was]
void ModList::populate(MenuRuntime &menu, int id, const std::string &current) const {
	std::vector<std::string> rows;
	rows.push_back(menu.widget_string(id, kModListBaseKey));
	for (const ExpansionRecord &record : records_) rows.push_back(record.info.name);
	menu.set_widget_items(id, rows);
	menu.select_row(id, 0, false);
	int selected = -1;
	for (int i = 0; i < static_cast<int>(records_.size()); ++i) {
		if (!strutil::iequals(current, records_[static_cast<size_t>(i)].directory)) continue;
		menu.select_row(id, i + 1, false);
		selected = i;
	}
	if (selected >= 0) describe(menu, records_[static_cast<size_t>(selected)].info.description);
}

// [orig: Options_OnModListSelect @ 0x55a530 — the 0x5000001 event alone @ 0x55a538; the row's
//  value -1 sets MOD_DESC to "" @ 0x55a580, another the record's description @ 0x55a575]
void ModList::select(MenuRuntime &menu, int id, int row) const {
	if (row < 0 || row >= menu.item_count(id)) return;
	const int value = row_value(row);
	if (value < 0) {
		describe(menu, std::string());
		return;
	}
	if (value < static_cast<int>(records_.size())) describe(menu, records_[static_cast<size_t>(value)].info.description);
}

// [orig: Options_HandleAcceptOrBack @ 0x55ad05 — UIList_GetSelectedValue @ 0x644660 (the first
//  selected row's value, 0 with none); -1 the empty name @ 0x55ad0f, else the record's directory
//  @ 0x55ad16]
std::string ModList::pick(const MenuRuntime &menu, int id) const {
	const int row = menu.selected_row(id);
	const int value = row >= 0 ? row_value(row) : 0;
	if (value < 0 || value >= static_cast<int>(records_.size())) return std::string();
	return records_[static_cast<size_t>(value)].directory;
}

} // namespace opennova::menu
