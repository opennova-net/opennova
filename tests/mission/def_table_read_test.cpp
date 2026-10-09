// The def tables as the mission load reads them through an embedder's files
// (runtime/mission/runtime_boot.h): weapon.def through the game's parser with the
// mount's SIGHTS file test (a row whose texture the mount lacks is no row [orig:
// FileSystem_FileExists @0x544AE2]), a zero-length weapon.def an empty table;
// ammo.def into the dense table, untouched unless it reads; a missing file. And
// items.def's per-id index (collision_resolve.h item_defs_by_id): each id's first
// row, find_item_def's rule [orig: ItemList_FindIndexByTypeId @0x49E100].
#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/runtime_boot.h>

#include <formats/def/def.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;

namespace {

// An in-memory file set, names compared as given.
mission::BootFileSource files_of(const std::map<std::string, std::string> &files) {
	mission::BootFileSource source;
	source.has_file = [files](const std::string &name) { return files.count(name) != 0; };
	source.read_file = [files](const std::string &name, std::vector<uint8_t> &out) {
		const auto found = files.find(name);
		if (found == files.end()) return false;
		out.assign(found->second.begin(), found->second.end());
		return true;
	};
	return source;
}

const char *const kWeaponDef =
		"weapon WPN_CARD\r\n"
		" sights card.tga 0 0 64 64\r\n"
		"end\r\n"
		"weapon WPN_NOCARD\r\n"
		" sights missing.tga 0 0 64 64\r\n"
		"end\r\n";

const char *const kAmmoDef =
		"ammo AT_NULL\r\n"
		"end\r\n"
		"ammo AMMO_A\r\n"
		" velocity 100\r\n"
		"end\r\n";

int test_weapons() {
	const mission::BootFileSource files =
			files_of({{"weapon.def", kWeaponDef}, {"card.tga", "x"}, {"empty.def", ""}});
	def::DefWeaponsFile weapons{};
	TEST_EXPECT(mission::read_weapon_defs(files, "weapon.def", weapons) == mission::DefTableRead::Read);
	TEST_EXPECT(weapons.count == 2);
	TEST_EXPECT(std::strcmp(weapons.entries[0].weapon_name, "WPN_CARD") == 0 && weapons.entries[0].sights_count == 1);
	TEST_EXPECT(std::strcmp(weapons.entries[1].weapon_name, "WPN_NOCARD") == 0 && weapons.entries[1].sights_count == 0);
	def::def_free_weapons(&weapons);
	// A zero-length file is an empty table, not a missing one.
	TEST_EXPECT(mission::read_weapon_defs(files, "empty.def", weapons) == mission::DefTableRead::Read);
	TEST_EXPECT(weapons.count == 0);
	def::def_free_weapons(&weapons);
	TEST_EXPECT(mission::read_weapon_defs(files, "nosuch.def", weapons) == mission::DefTableRead::Missing);
	TEST_EXPECT(mission::read_weapon_defs(mission::BootFileSource{}, "weapon.def", weapons) ==
	            mission::DefTableRead::Missing);
	std::printf("weapons: the SIGHTS row kept where the mount has its card, an empty table, a missing file\n");
	return 0;
}

int test_ammo() {
	const mission::BootFileSource files = files_of({{"ammo.def", kAmmoDef}, {"empty.def", ""}});
	world::AmmoTable table;
	TEST_EXPECT(mission::read_ammo_table(files, "ammo.def", table) == mission::DefTableRead::Read);
	TEST_EXPECT(table.entries.size() == 2 && table.entries[0].name == "AT_NULL" && table.entries[1].name == "AMMO_A");
	TEST_EXPECT(table.index_of("AMMO_A") == 1);
	// A file that does not read leaves the table as it was.
	TEST_EXPECT(mission::read_ammo_table(files, "nosuch.def", table) == mission::DefTableRead::Missing);
	TEST_EXPECT(mission::read_ammo_table(files, "empty.def", table) == mission::DefTableRead::Unreadable);
	TEST_EXPECT(table.entries.size() == 2);
	std::printf("ammo: the dense table, untouched by a missing or unreadable file\n");
	return 0;
}

int test_item_index() {
	std::vector<def::DefItemDef> rows(4);
	std::memset(rows.data(), 0, rows.size() * sizeof(def::DefItemDef));
	rows[0].id = 7;
	rows[1].id = 9;
	rows[2].id = 7; // a later duplicate: never reached
	rows[3].id = 11;
	def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	const auto index = mission::item_defs_by_id(items);
	TEST_EXPECT(index.size() == 3);
	for (const int id : {7, 9, 11}) TEST_EXPECT(index.at(id) == mission::find_item_def(items, id));
	TEST_EXPECT(index.at(7) == &rows[0]);
	TEST_EXPECT(index.count(8) == 0 && mission::find_item_def(items, 8) == nullptr);
	std::printf("items: each id's first row, as find_item_def finds it\n");
	return 0;
}

} // namespace

int main() {
	if (test_weapons() != 0) return 1;
	if (test_ammo() != 0) return 1;
	if (test_item_index() != 0) return 1;
	return 0;
}
