#pragma once

// The Mods list: the rows the game fills its expansion list with, what a pick of one shows and
// what ACCEPT takes of it [orig: Options_PopulateModList @ 0x559fb0 (on the OPTIONS screen's
// init, Options_InitScreenOnce @ 0x55d870); Options_OnModListSelect @ 0x55a530;
// Options_HandleAcceptOrBack @ 0x55a710, the list's leg @ 0x55acea..0x55ad5b]. The game finds the
// list and the description by name on its OPTIONS screen (AVAIL_LIST, MOD_DESC); the shell's name
// sets carry those names (menu_commands.h ModLists, ModDescriptions). Witness record:
// docs/mnu/menu-re.md "The Mods list" (D-MNU-31).

#include <base/vfs/vfs.h>
#include <runtime/menu/menu_runtime.h>

#include <string>
#include <vector>

namespace opennova::menu {

// The base game's row: the key the code looks up through the list's string table, its text where
// the table holds none [orig: Options_PopulateModList @ 0x559fe2, the literal JO's program carries].
inline constexpr const char *kModListBaseKey = "Joint Operations: Typhoon Rising";

class ModList {
public:
	// The read-only texts a pick describes the expansion in (the shell's ModDescriptions names).
	void set_descriptions(std::vector<std::string> names) { descriptions_ = std::move(names); }
	// The records the rows list: the game folder's scan (vfs_expansion_records), which the game makes
	// once, at boot [orig: Expansion_ScanAndRegister @ 0x4a43d0 from Game_InitSubsystems @ 0x4a6f32].
	void set_records(std::vector<ExpansionRecord> records) { records_ = std::move(records); }
	const std::vector<ExpansionRecord> &records() const { return records_; }

	// The list `id` filled anew: the base game's row first (kModListBaseKey through the list's
	// string table), selected; then a row per record, its name, in the scan's order; the record
	// whose directory names `current` (the expansion running, "" the base game; no case) has its
	// row selected instead and its description shown. With the base game running the
	// descriptions keep what they hold.
	void populate(MenuRuntime &menu, int id, const std::string &current) const;
	// A row picked (the list's select, not its double click): the descriptions show its record's
	// description, nothing for the base game's row.
	void select(MenuRuntime &menu, int id, int row) const;
	// What ACCEPT takes of the list: the directory of the selected row's record, "" for the base
	// game's row. With no row selected the game reads the value 0, the first record.
	std::string pick(const MenuRuntime &menu, int id) const;

private:
	void describe(MenuRuntime &menu, const std::string &text) const;
	std::vector<std::string> descriptions_;
	std::vector<ExpansionRecord> records_;
};

} // namespace opennova::menu
