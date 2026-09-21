// The PLAYER_INFO kit model (menu/player_info_kit.h), pinned where it used to
// live in the GDScript companion (ADR 0040 ladder E4): the voice list per sex
// with the disabled row skipped and DEFAULT_VOICE first, the persisted
// override reset, and the weapon.sav kit page order — the side's knife, the
// medic's medpack, the three categories with their flags, the fixed three
// grenade slots with the entry-0 quirk.
// [orig: populate_player_voice_combo @ 0x55dce0; serialize_weapon_loadout @ 0x55e4b0]
#include <runtime/menu/player_info_kit.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

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

	const WeaponNameLookup names = [](int32_t index) -> std::string {
		switch (index) {
			case 0: return "WPN_ENTRY0";
			case 4: return "WPN_M4";
			case 7: return "WPN_PISTOL";
			case 12: return "WPN_FRAG";
			default: return std::string();
		}
	};
	PlayerInfoKitSelection sel;
	sel.team_mask = 2; // blue
	sel.player_class = 5; // medic
	sel.primary = KitSlotPick{4, 6, -1, 1};
	sel.secondary = KitSlotPick{7, -1, -1, 0};
	sel.accessory = KitSlotPick{0, -1, -1, -1}; // NONE serializes entry 0
	sel.grenades[0] = KitSlotPick{12, 2, -1, -1};
	std::vector<opennova::playersav::KitEntry> page = player_info_kit_entries(sel, names);
	// knife + medpack + PRIMARY/SECONDARY/ACCESSORY + the fixed three grenades.
	CHECK(page.size() == 8);
	CHECK(page[0].name == "WPN_KNIFE" && page[0].ammo_primary == -1 && page[0].flags == -1);
	CHECK(page[1].name == "WPN_MEDPACK" && page[1].ammo_secondary == -1);
	CHECK(page[2].name == "WPN_M4" && page[2].ammo_primary == 6 && page[2].flags == 1);
	CHECK(page[3].name == "WPN_PISTOL" && page[3].ammo_primary == -1 && page[3].flags == 0);
	CHECK(page[4].name == "WPN_ENTRY0" && page[4].flags == -1);
	CHECK(page[5].name == "WPN_FRAG" && page[5].ammo_primary == 2 && page[5].flags == -1);
	CHECK(page[6].name == "WPN_ENTRY0" && page[6].ammo_primary == -1);
	CHECK(page[7].name == "WPN_ENTRY0" && page[7].ammo_secondary == -1);
	// The red side takes the other knife, a non-medic no medpack, and the
	// defensive mask-zero leg the blue knife.
	sel.team_mask = 1;
	sel.player_class = 8;
	page = player_info_kit_entries(sel, names);
	CHECK(page.size() == 7 && page[0].name == "WPN_KNIFE2" && page[1].name == "WPN_M4");
	sel.team_mask = 0;
	CHECK(player_info_kit_entries(sel, names)[0].name == "WPN_KNIFE");
	// No name lookup: nameless entries.
	CHECK(player_info_kit_entries(sel, WeaponNameLookup{})[1].name.empty());

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("player_info_kit_test OK\n");
	return 0;
}
