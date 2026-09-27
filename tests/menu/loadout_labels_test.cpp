// The loadout screens' shared text compositions (menu/loadout_labels.h),
// pinned where they used to live in the GDScript LoadoutLabels helper and the
// two companions (ADR 0040 ladder E4b): the WepDes weapon label with its raw
// id fallback, the "<rounds> - <round label>" ammo row with the null-def
// form, the armory's case-insensitive row order, and the weight readout with
// its encumbrance band tokens.
// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430; UI_UpdateWeaponWeightDisplay
//  @0x565640; ListWidget_SortRows cmp @0x6448a0]
#include <runtime/menu/loadout_labels.h>

#include <formats/def/def.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

using namespace opennova::def;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

static opennova::hud::GameTextLookup table_of(std::map<std::string, std::string> rows) {
	return [rows](const char *section, const char *key, const char *fallback) {
		const auto it = rows.find(std::string(section) + "/" + key);
		return it != rows.end() ? it->second : std::string(fallback);
	};
}

int main() {
	const opennova::hud::GameTextLookup gametext = table_of({
			{ "WepDes/M4_TEXT", "M4A1 Carbine" },
			{ "WepDes/RND_556", "5.56mm" },
	});
	const opennova::hud::GameTextLookup empty = table_of({});

	DefWeaponDef m4;
	std::memset(&m4, 0, sizeof(m4));
	std::strcpy(m4.weapon_name, "WPN_M4");
	std::strcpy(m4.loadout_menu_textid, "M4_TEXT");
	std::strcpy(m4.round_type, "RND_556");
	m4.clipsize = 30;
	DefWeaponDef knife;
	std::memset(&knife, 0, sizeof(knife));
	std::strcpy(knife.weapon_name, "WPN_KNIFE");
	knife.clipsize = 1;

	// The weapon label: the WepDes text id, else the raw id; a missing WepDes
	// row falls back to the raw id too.
	CHECK(weapon_label(m4, gametext) == "M4A1 Carbine");
	CHECK(weapon_label(m4, empty) == "WPN_M4");
	CHECK(weapon_label(knife, gametext) == "WPN_KNIFE");

	// The ammo row: rounds = clips x clipsize, the round label through WepDes
	// (its raw key when unresolved, nothing when the def has none); a null def
	// rows "<clips> - ".
	CHECK(ammo_row_label(&m4, 3, gametext) == "90 - 5.56mm");
	CHECK(ammo_row_label(&m4, 3, empty) == "90 - RND_556");
	CHECK(ammo_row_label(&knife, 2, gametext) == "2 - ");
	CHECK(ammo_row_label(nullptr, 4, gametext) == "4 - ");

	// The armory's row order: case-insensitive ascending by label.
	const std::vector<int> order = armory_slot_order({ "m4A1", "AK-47", "M16", "ak-74" });
	CHECK(order == (std::vector<int>{ 1, 3, 2, 0 }));
	CHECK(armory_slot_order({}).empty());

	// The weight readout with its band tokens and their fallbacks.
	const opennova::hud::GameTextLookup menu = table_of({
			{ "Menu/TOTAL_WEIGHT", "Gesamtgewicht" },
			{ "Menu/LBS", "kg" },
			{ "Menu/HEAVY_ENCUMBRANCE", "Schwer" },
	});
	CHECK(loadout_weight_line(12.34, empty, empty) == "Total Weight 12.3 lbs (Light)");
	CHECK(loadout_weight_line(33.3, empty, empty) == "Total Weight 33.3 lbs (Normal)");
	CHECK(loadout_weight_line(66.6, empty, empty) == "Total Weight 66.6 lbs (Heavy)");
	CHECK(loadout_weight_line(70.0, menu, empty) == "Gesamtgewicht 70.0 kg (Schwer)");
	CHECK(loadout_weight_line(0.0, menu, empty) == "Gesamtgewicht 0.0 kg (Light)");

	const auto overrides = table_of({{"Menu/TOTAL_WEIGHT", "Override"}, {"Menu/LBS", ""}});
	CHECK(loadout_weight_line(70.0, empty, menu) == "Gesamtgewicht 70.0 kg (Schwer)");
	CHECK(loadout_weight_line(70.0, overrides, menu) == "Override 70.0  (Schwer)");

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("loadout_labels_test OK\n");
	return 0;
}
