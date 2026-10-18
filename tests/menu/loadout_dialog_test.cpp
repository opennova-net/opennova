// The loadout dialogs' list and state model: the category list fill (the slot
// filter, the WEAPON screen's availability term, the row order under NONE), the
// two reselects (by weapon.def row, by name), the WEAPON screen's clip count and
// accept pick, and the PLAYER_INFO panel's state (runtime/menu/
// player_info_loadout.h): the kit restore's catalog rows, the handlers' picks
// (row + 1, the authored type byte, the grenade row), the selected clips and
// the kit page's selection.
// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430; UI_PopulateThreeCategoryLists
//  @0x566db0; PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55e8b0; @0x55f730;
//  @0x55f760; @0x55fb70; WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0]

#include <runtime/menu/loadout_screen.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/player_info_loadout.h>

#include <cstring>
#include <iostream>

using namespace opennova;
using namespace opennova::menu;

namespace {

const hud::GameTextLookup text = [](const std::string &, const std::string &, const std::string &fallback) {
	return fallback;
};

int failures = 0;
#define CHECK(c) do { if (!(c)) { \
	std::cerr << __func__ << ':' << __LINE__ << ": " #c "\n"; ++failures; \
} } while (false)

// A screen of combos; a *_AMMO1_TYPE combo authors FMJ/AP/SP as values 0/1/2.
mnu::Document document(std::initializer_list<const char *> names) {
	mnu::Document doc;
	mnu::Screen screen;
	screen.name = "LOADOUT";
	mnu::Window root;
	root.name = "ROOT";
	root.type = mnu::WindowType::Window;
	for (const char *name : names) {
		mnu::Window w;
		w.name = name;
		w.type = mnu::WindowType::Combo;
		mnu::Window &list = w.list_box.author(mnu::WindowType::List);
		list.items.present = true;
		for (const char *value : {"0", "1", "2"}) {
			mnu::Item row;
			row.text = std::string("type ") + value;
			row.value = value;
			list.items.items.push_back(row);
		}
		root.children.push_back(w);
	}
	screen.roots.push_back(root);
	doc.screens.push_back(screen);
	return doc;
}

// Eight rows: 0..3 primaries (row 3 red only), 4 a secondary, 5..7 grenades.
struct Weapons {
	def::DefWeaponDef rows[8]{};
	def::DefWeaponsFile file{};
	Weapons() {
		file.entries = rows;
		file.count = 8;
		const char *names[8] = {"WPN_Zulu", "wpn_alpha", "WPN_MIKE", "WPN_RED", "WPN_PISTOL",
				"WPN_FRAG", "WPN_SMOKE", "WPN_FLASH"};
		for (int i = 0; i < 8; ++i) {
			std::strcpy(rows[i].weapon_name, names[i]);
			std::strcpy(rows[i].round_type, "ROUND");
			rows[i].loadout_selectable = 1;
			rows[i].charfilter_mask = 8;
			rows[i].teamfilter_mask = 2;
			rows[i].weapon_class_slot = i < 4 ? 1 : i == 4 ? 2 : 3;
			rows[i].maxclips = 3;
			rows[i].clipsize = 10;
			rows[i].weaponweight = 4.0f;
			rows[i].clipweight = 0.5f;
		}
		rows[3].teamfilter_mask = 1;
	}
};

void test_fill_slot_and_reselects() {
	Weapons weapons;
	const auto doc = document({"PRIMARY", "SECONDARY", "PRIMARY_AMMO1"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "test.mnu", "LOADOUT"));
	const int primary = menu.widget_id("PRIMARY");
	// The slot filter (class 8, blue team), the case-insensitive label order and NONE at row 0.
	const LoadoutSlotRows rows = loadout_fill_slot(menu, weapons.file, "PRIMARY", 1, 8, 2,
			WeaponAvailability(), "None", text);
	CHECK(rows == (LoadoutSlotRows{-1, 1, 2, 0})); // alpha, MIKE, Zulu
	CHECK(menu.get_widget_items(primary) == (std::vector<std::string>{"None", "wpn_alpha", "WPN_MIKE", "WPN_Zulu"}));
	CHECK(menu.selected_row(primary) == 0);
	CHECK(loadout_selected_weapon(menu, "PRIMARY", rows) == -1);
	// The map availability term: a 0 drops the row; any other value lists.
	const WeaponAvailability availability = [](const std::string &name) { return name == "WPN_MIKE" ? 0 : 2; };
	const LoadoutSlotRows armory = loadout_fill_slot(menu, weapons.file, "PRIMARY", 1, 8, 2, availability,
			"NONE", text);
	CHECK(armory == (LoadoutSlotRows{-1, 1, 0}));
	CHECK(menu.item_text(primary, 0) == "NONE");
	// By weapon.def row (PLAYER_INFO): the row carrying it, else none at all.
	loadout_select_weapon_row(menu, "PRIMARY", rows, 0);
	CHECK(menu.selected_row(primary) == 3 && loadout_selected_weapon(menu, "PRIMARY", rows) == 0);
	loadout_select_weapon_row(menu, "PRIMARY", rows, 3);
	CHECK(menu.selected_row(primary) == -1 && loadout_selected_weapon(menu, "PRIMARY", rows) == -1);
	loadout_select_weapon_row(menu, "PRIMARY", rows, -1);
	CHECK(menu.selected_row(primary) == -1); // the NONE row never carries a weapon
	// By name (WEAPON): case-insensitive; an empty name or a miss keeps the selection.
	menu.select_row(primary, 0, false);
	loadout_select_weapon_name(menu, weapons.file, "PRIMARY", rows, "WPN_ALPHA");
	CHECK(menu.selected_row(primary) == 1);
	loadout_select_weapon_name(menu, weapons.file, "PRIMARY", rows, "WPN_NOPE");
	CHECK(menu.selected_row(primary) == 1);
	loadout_select_weapon_name(menu, weapons.file, "PRIMARY", rows, "");
	CHECK(menu.selected_row(primary) == 1);
	// An absent control fills nothing.
	CHECK(loadout_fill_slot(menu, weapons.file, "ACCESSORY", 0, 8, 2, {}, "None", text).empty());
	CHECK(loadout_selected_weapon(menu, "ACCESSORY", rows) == -1);
}

void test_armory_clips_and_pick() {
	Weapons weapons;
	const auto doc = document({"PRIMARY", "SECONDARY", "PRIMARY_AMMO1", "GRENADE_AMMO1", "GRENADE_AMMO2"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "test.mnu", "LOADOUT"));
	LoadoutSlotRows rows[3];
	rows[0] = loadout_fill_slot(menu, weapons.file, "PRIMARY", 1, 8, 2, {}, "NONE", text);
	rows[1] = loadout_fill_slot(menu, weapons.file, "SECONDARY", 2, 8, 2, {}, "NONE", text);
	CHECK(rows[1] == (LoadoutSlotRows{-1, 4}));
	menu.select_row(menu.widget_id("PRIMARY"), 2, false); // WPN_MIKE
	menu.select_row(menu.widget_id("SECONDARY"), 1, false);
	const int ammo = menu.widget_id("PRIMARY_AMMO1");
	menu.select_row(ammo, 1, false);
	// AMMO1 row + 1; no AMMO1 control or no selection reads -1.
	CHECK(armory_selected_clips(menu, "PRIMARY") == 2);
	CHECK(armory_selected_clips(menu, "SECONDARY") == -1);
	menu.select_row(ammo, -1, false);
	CHECK(armory_selected_clips(menu, "PRIMARY") == -1);
	menu.select_row(ammo, 0, false);
	// The grenade positions: rows past 0 carry {name, row, -1, -1}; row 0 nothing.
	menu.select_row(menu.widget_id("GRENADE_AMMO1"), 0, false);
	menu.select_row(menu.widget_id("GRENADE_AMMO2"), 2, false);
	const ArmoryLoadoutPick pick = armory_loadout_pick(menu, weapons.file, rows, {5, 6, 7});
	CHECK(pick.names[0] == "WPN_MIKE" && pick.clips[0] == 1);
	CHECK(pick.names[1] == "WPN_PISTOL" && pick.clips[1] == -1);
	CHECK(pick.names[2].empty() && pick.clips[2] == -1);
	CHECK(pick.grenades.size() == 1);
	CHECK(pick.grenades.size() == 1 && pick.grenades[0].name == "WPN_SMOKE" &&
			pick.grenades[0].ammo_primary == 2 && pick.grenades[0].ammo_secondary == -1 &&
			pick.grenades[0].flags == -1);
}

void test_player_info_state() {
	Weapons weapons;
	const auto doc = document({"PRIMARY", "SECONDARY", "ACCESSORY", "PRIMARY_AMMO1", "PRIMARY_AMMO1_TYPE",
			"SECONDARY_AMMO1", "SECONDARY_AMMO1_TYPE", "GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "player.mnu", "LOADOUT"));
	PlayerInfoLoadout state;
	state.fill_slots(menu, weapons.file, 8, 2, "None", text);
	CHECK(state.slot_rows[0] == (LoadoutSlotRows{-1, 1, 2, 0}));
	CHECK(state.slot_rows[1] == (LoadoutSlotRows{-1, 4}));
	CHECK(state.slot_rows[2] == (LoadoutSlotRows{-1})); // no accessory row
	// The restore: catalog rows (weapon.def row + 1), the None row's pair at -1.
	PlayerInfoKitRestore page;
	page.ammo_primary.assign(kPlayerInfoCatalogRows, -1);
	page.ammo_secondary.assign(kPlayerInfoCatalogRows, -1);
	page.ammo_primary[0] = 4;        // the "None" row's pair
	page.ammo_primary[1] = 2;        // WPN_Zulu
	page.ammo_secondary[1] = 1;
	page.ammo_primary[7] = 3;        // WPN_SMOKE
	page.select[0] = 1;              // WPN_Zulu
	page.select[1] = 9;              // a weapon the list lacks
	page.has_type[0] = true;
	page.type[0] = 2;
	page.grenade_slots[0] = 6;       // WPN_FRAG
	page.grenade_slots[1] = 0;       // "None"
	page.grenade_slots[2] = 8;       // WPN_FLASH
	state.ammo_type[1][1] = 1;       // the red team's SECONDARY byte survives
	state.restore(menu, page, 7, 0);
	CHECK(state.page_class == 7);
	CHECK(state.ammo_primary == (LoadoutAmmoCounts{{-1, 4}, {0, 2}, {6, 3}}));
	CHECK(state.ammo_secondary == (LoadoutAmmoCounts{{0, 1}}));
	CHECK(menu.selected_row(menu.widget_id("PRIMARY")) == 3 && state.selected_weapon(menu, "PRIMARY") == 0);
	CHECK(menu.selected_row(menu.widget_id("SECONDARY")) == -1 && state.selected_weapon(menu, "SECONDARY") == -1);
	CHECK(state.selected_ammo_type("PRIMARY", 0) == 2 && state.selected_ammo_type("SECONDARY", 1) == 1);
	CHECK(state.selected_ammo_type("SECONDARY", 0) == 0);
	CHECK(state.restored_grenades == (std::array<int32_t, 3>{5, -1, 7}));
	CHECK(state.selected_clips(menu, "PRIMARY") == 2);
	CHECK(state.selected_clips(menu, "SECONDARY") == -1); // NONE
	// The ammo fill reads the pair and the team's byte.
	state.fill_ammo(menu, weapons.file, "PRIMARY", 0, text);
	CHECK(menu.selected_row(menu.widget_id("PRIMARY_AMMO1")) == 1); // the saved 2 clips
	CHECK(menu.selected_row(menu.widget_id("PRIMARY_AMMO1_TYPE")) == 2);
	// The handlers: AMMO1 row + 1 for the selected weapon, nothing for NONE.
	state.pick_clips(menu, "PRIMARY", false, 0);
	CHECK(state.selected_clips(menu, "PRIMARY") == 1);
	state.pick_clips(menu, "PRIMARY", true, 2);
	CHECK(state.ammo_secondary.at(0) == 3);
	state.pick_clips(menu, "SECONDARY", false, 1);
	CHECK(state.ammo_primary.count(4) == 0);
	// The type pick stores the row's authored value for the team.
	state.pick_ammo_type(menu, "PRIMARY", 1, 1);
	CHECK(state.selected_ammo_type("PRIMARY", 1) == 1 && state.selected_ammo_type("PRIMARY", 0) == 2);
	state.pick_ammo_type(menu, "PRIMARY", 0, -1);
	CHECK(state.selected_ammo_type("PRIMARY", 0) == 0); // no row: the byte reads 0
	// The grenade fill and pick: the row is the count.
	state.fill_grenades(menu, weapons.file, 8, 2, text);
	CHECK(state.grenade_rows == (std::vector<int>{5, 6, 7}));
	CHECK(menu.selected_row(menu.widget_id("GRENADE_AMMO2")) == 3); // the saved 3
	state.pick_grenade(0, 2);
	CHECK(state.ammo_primary.at(5) == 2);
	state.pick_grenade(3, 1); // past the positions: nothing
	CHECK(state.ammo_primary.size() == 4);
	// The kit page's selection: categories with their pairs and type flags, then the grenade slots.
	state.pick_ammo_type(menu, "PRIMARY", 0, 2);
	const PlayerInfoKitSelection sel = state.kit_selection(menu, 0, 7);
	CHECK(sel.team_mask == 2 && sel.player_class == 7);
	CHECK(sel.primary.weapon_index == 1 && sel.primary.ammo_primary == 1 && sel.primary.ammo_secondary == 3 &&
			sel.primary.flags == 2);
	CHECK(sel.secondary.weapon_index == 0 && sel.secondary.ammo_primary == 4 &&
			sel.secondary.ammo_secondary == -1 && sel.secondary.flags == 0);
	CHECK(sel.accessory.weapon_index == 0 && sel.accessory.flags == kKitFiller);
	CHECK(sel.grenades[0].weapon_index == 6 && sel.grenades[0].ammo_primary == 2);
	CHECK(sel.grenades[1].weapon_index == 7 && sel.grenades[1].ammo_primary == 3);
	CHECK(sel.grenades[2].weapon_index == 8 && sel.grenades[2].flags == kKitFiller);
	// Past the fill's positions the slots are the restore's.
	state.grenade_rows = {5};
	const PlayerInfoKitSelection partial = state.kit_selection(menu, 0, 7);
	CHECK(partial.grenades[1].weapon_index == 0 && partial.grenades[2].weapon_index == 8);
	// The weight reads the selections and the pairs (player_info_screen_weight).
	CHECK(state.weight(menu, weapons.file) ==
			player_info_screen_weight(menu, weapons.file, {0, -1, -1}, {5}, state.ammo_primary, state.ammo_secondary));
	state.clear_counts();
	CHECK(state.ammo_primary.empty() && state.ammo_secondary.empty() && state.selected_ammo_type("PRIMARY", 0) == 2);
}

} // namespace

int main() {
	test_fill_slot_and_reselects();
	test_armory_clips_and_pick();
	test_player_info_state();
	std::cout << "loadout_dialog: " << failures << " failures\n";
	return failures != 0;
}
