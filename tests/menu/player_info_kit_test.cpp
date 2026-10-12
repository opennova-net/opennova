// The PLAYER_INFO kit model (menu/player_info_kit.h), pinned where it used to
// live in the GDScript companion (ADR 0040 ladder E4): the voice list per sex
// with the disabled row skipped and DEFAULT_VOICE first, the persisted
// override reset, and the weapon.sav kit page order — the side's knife, the
// medic's medpack, the three categories with their flags, the fixed three
// grenade slots, NONE and an empty grenade slot writing the catalog's "None";
// and the populate's restore of a saved page.
// [orig: PlayerInfo_PopulatePlayerVoiceCombo @ 0x55dce0; PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0]
#include <runtime/menu/player_info_kit.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

// A weapon.def row for the restore: weapon_class_slot 0 accessory, 1 primary,
// 2 secondary, 3 grenade.
static void row(opennova::def::DefWeaponDef &w, const char *name, int slot, int selectable,
		const char *round, int subclasses = 0) {
	std::strcpy(w.weapon_name, name);
	w.weapon_class_slot = slot;
	w.loadout_selectable = selectable;
	std::strcpy(w.round_type, round);
	w.loadout_subclasses = subclasses;
}

static opennova::playersav::KitEntry entry(const char *name, int pri, int sec, int flags) {
	opennova::playersav::KitEntry e;
	e.name = name;
	e.ammo_primary = pri;
	e.ammo_secondary = sec;
	e.flags = flags;
	return e;
}

// The populate's walk of a saved class page [orig: PlayerInfo_PopulateWeaponAccessoryAmmoUI
// @ 0x55e8b0]: catalog rows are weapon.def's plus one.
static void test_restore() {
	opennova::def::DefWeaponDef rows[16]{};
	row(rows[0], "WPN_RIFLE", 1, 1, "R1", 1);
	row(rows[1], "WPN_RIFLE_GL", 1, 0, "GL"); // the rifle's subclass: another round
	row(rows[2], "WPN_PISTOL", 2, 1, "P");
	row(rows[3], "WPN_MINE", 0, 1, "M");
	row(rows[4], "WPN_FRAG", 3, 1, "F");
	row(rows[5], "WPN_SMOKE", 3, 1, "S");
	row(rows[6], "WPN_FLASH", 3, 1, "B");
	row(rows[7], "WPN_KNIFE", 0, 0, "K");
	row(rows[8], "WPN_HIDDEN", 1, 0, "H");
	row(rows[9], "WPN_STUN", 3, 1, "T");
	row(rows[10], "WPN_SAME", 1, 1, "Q", 1);
	row(rows[11], "WPN_SAME_ALT", 1, 0, "Q");  // its one subclass row: the same round
	row(rows[12], "WPN_DEEP", 1, 0, "D", 2);
	row(rows[13], "WPN_DEEP_ALT", 1, 0, "D");  // the same round, then
	row(rows[14], "WPN_DEEP_GL", 1, 0, "E");   // another inside the window
	row(rows[15], "WPN_OPEN", 1, 1, "O");      // the last entry, no `end`: no row
	rows[15].unclosed = 1;
	opennova::def::DefWeaponsFile weapons{};
	weapons.entries = rows;
	weapons.count = 16;

	opennova::playersav::KitPage page;
	page.entries = {
		entry("WPN_KNIFE", -1, -1, -1),  // counts stored, never selected
		entry("wpn_rifle", 3, 2, 1),     // case-insensitive; the GL row differs: both counts
		entry("WPN_PISTOL", 4, 7, 200),  // no subclass: no secondary count; flags a signed byte
		entry("None", 5, 5, 5),          // row 0: one string on, then "5" x3 name nothing
		entry("WPN_GHOST", 9, 9, 9),     // unknown: skipped the same way
		entry("WPN_HIDDEN", 6, -1, 2),   // not selectable: its count, no select, no type
		entry("WPN_MINE", 2, -1, -1),
		entry("WPN_FRAG", 1, -1, -1),
		entry("WPN_SMOKE", 0, -1, -1),
		entry("WPN_FLASH", -1, -1, -1),
		entry("WPN_STUN", 2, -1, -1),    // a fourth grenade: the "None" row's primary count
		entry("WPN_SAME", 8, 8, 0),      // its subclass row shares the round: no secondary
		entry("WPN_DEEP", 4, 6, -1),     // not selectable; the second subclass row differs
		entry("WPN_OPEN", 4, 4, 4),      // no row: nothing
	};
	PlayerInfoKitRestore r = player_info_restore_kit(page, weapons);
	CHECK(r.ammo_primary.size() == kPlayerInfoCatalogRows && r.ammo_secondary.size() == kPlayerInfoCatalogRows);
	CHECK(r.ammo_primary[8] == -1);                                  // the knife's -1
	CHECK(r.ammo_primary[1] == 3 && r.ammo_secondary[1] == 2);       // the rifle
	CHECK(r.ammo_primary[3] == 4 && r.ammo_secondary[3] == -1);      // the pistol
	CHECK(r.ammo_primary[9] == 6);                                   // the hidden primary
	CHECK(r.ammo_primary[4] == 2 && r.ammo_primary[5] == 1 && r.ammo_primary[6] == 0);
	CHECK(r.ammo_primary[11] == 8 && r.ammo_secondary[11] == -1);    // WPN_SAME
	CHECK(r.ammo_primary[13] == 4 && r.ammo_secondary[13] == 6);     // WPN_DEEP
	CHECK(r.ammo_primary[16] == -1);                                 // WPN_OPEN never named
	CHECK(r.select[0] == 11 && r.select[1] == 3 && r.select[2] == 4); // the last select wins
	CHECK(r.has_type[0] && r.type[0] == 0);                          // WPN_SAME's flags
	CHECK(r.has_type[1] && r.type[1] == -56);                        // 200 as a signed byte
	CHECK(r.grenade_slots[0] == 5 && r.grenade_slots[1] == 6 && r.grenade_slots[2] == 7);
	CHECK(r.ammo_primary[0] == 10);  // the fourth slot is the "None" row's primary count
	CHECK(r.ammo_secondary[0] == -1);
	CHECK(r.ammo_primary[10] == 2);  // the stun grenade's own count

	// A page naming no category: no select, no type, zeroed slots, every pair -1.
	opennova::playersav::KitPage empty;
	empty.entries = {entry("WPN_KNIFE", -1, -1, -1)};
	r = player_info_restore_kit(empty, weapons);
	CHECK(r.select[0] == -1 && r.select[1] == -1 && r.select[2] == -1);
	CHECK(!r.has_type[0] && !r.has_type[1]);
	CHECK(r.grenade_slots[0] == 0 && r.grenade_slots[1] == 0 && r.grenade_slots[2] == 0);
	for (int32_t v : r.ammo_primary) CHECK(v == -1);
	for (int32_t v : r.ammo_secondary) CHECK(v == -1);
}

int main() {
	// The voice list: males get rows 1..6 and 10 (9 is disabled), females 7, 8, 11.
	const std::vector<int32_t> male = player_info_voice_values(0);
	CHECK(male == (std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 10}));
	const std::vector<int32_t> female = player_info_voice_values(1);
	CHECK(female == (std::vector<int32_t>{0, 7, 8, 11}));
	// A persisted override the list carries is kept; one it does not resets.
	CHECK(player_info_voice_selection(10, male) == 10);
	CHECK(player_info_voice_selection(10, female) == 0);
	CHECK(player_info_voice_selection(9, male) == 0);
	CHECK(player_info_voice_selection(0, female) == 0);

	// The lookup names weapon.def rows; a pick names the catalog row, one past
	// its file row, with the seeded "None" at 0.
	const WeaponNameLookup names = [](int32_t row) -> std::string {
		switch (row) {
			case 0: return "WPN_ROW0";
			case 3: return "WPN_M4";
			case 6: return "WPN_PISTOL";
			case 11: return "WPN_FRAG";
			default: return std::string();
		}
	};
	CHECK(catalog_index_of_row(-1) == 0 && catalog_index_of_row(0) == 1 &&
	      catalog_index_of_row(3) == 4);
	PlayerInfoKitSelection sel;
	sel.team_mask = 2; // blue
	sel.player_class = 5; // medic
	sel.primary = KitSlotPick{4, 6, -1, 1};
	sel.secondary = KitSlotPick{7, -1, -1, 0};
	sel.accessory = KitSlotPick{0, -1, -1, -1}; // NONE serializes the catalog's "None"
	sel.grenades[0] = KitSlotPick{12, 2, -1, -1};
	std::vector<opennova::playersav::KitEntry> page = player_info_kit_entries(sel, names);
	// knife + medpack + PRIMARY/SECONDARY/ACCESSORY + the fixed three grenades.
	CHECK(page.size() == 8);
	CHECK(page[0].name == "WPN_KNIFE" && page[0].ammo_primary == -1 && page[0].flags == -1);
	CHECK(page[1].name == "WPN_MEDPACK" && page[1].ammo_secondary == -1);
	CHECK(page[2].name == "WPN_M4" && page[2].ammo_primary == 6 && page[2].flags == 1);
	CHECK(page[3].name == "WPN_PISTOL" && page[3].ammo_primary == -1 && page[3].flags == 0);
	CHECK(page[4].name == "None" && page[4].flags == -1);
	CHECK(page[5].name == "WPN_FRAG" && page[5].ammo_primary == 2 && page[5].flags == -1);
	CHECK(page[6].name == "None" && page[6].ammo_primary == -1);
	CHECK(page[7].name == "None" && page[7].ammo_secondary == -1);
	// The red side takes the other knife, a non-medic no medpack, and the
	// defensive mask-zero leg the blue knife.
	sel.team_mask = 1;
	sel.player_class = 8;
	page = player_info_kit_entries(sel, names);
	CHECK(page.size() == 7 && page[0].name == "WPN_KNIFE2" && page[1].name == "WPN_M4");
	sel.team_mask = 0;
	CHECK(player_info_kit_entries(sel, names)[0].name == "WPN_KNIFE");
	// No name lookup: nameless weapon entries; the "None" row needs none.
	CHECK(player_info_kit_entries(sel, WeaponNameLookup{})[1].name.empty());
	CHECK(player_info_kit_entries(sel, WeaponNameLookup{})[3].name == "None");

	test_restore();

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("player_info_kit_test OK\n");
	return 0;
}
