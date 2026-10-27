#include <runtime/menu/loadout_screen.h>
#include <runtime/menu/menu_runtime.h>

#include <cstring>
#include <iostream>

using namespace opennova;
using namespace opennova::menu;

namespace {

const hud::GameTextLookup text = [](const std::string &, const std::string &, const std::string &fallback) { return fallback; };

int failures = 0;
#define CHECK(c) do { if (!(c)) { \
	std::cerr << __func__ << ':' << __LINE__ << ": " #c "\n"; ++failures; \
} } while (false)

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
		for (const char *value : {"2", "0", "1"}) {
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

struct Weapons {
	def::DefWeaponDef rows[8]{};
	def::DefWeaponsFile file{};
	Weapons() {
		file.entries = rows;
		file.count = 8;
		for (int i = 0; i < 8; ++i) {
			std::strcpy(rows[i].weapon_name, ("WPN_" + std::to_string(i)).c_str());
			std::strcpy(rows[i].round_type, "ROUND");
			rows[i].loadout_selectable = 1;
			rows[i].charfilter_mask = 8;
			rows[i].teamfilter_mask = 2;
			rows[i].weapon_class_slot = i < 3 ? 1 : 3;
			rows[i].maxclips = 3;
			rows[i].clipsize = 1;
			rows[i].weaponweight = 100.0f;
			rows[i].clipweight = 2.0f;
		}
		rows[0].clipsize = 10;
		rows[0].weaponweight = 2.0f;
		rows[0].clipweight = 0.5f;
		rows[0].loadout_subclasses = 2;
		std::strcpy(rows[1].round_type, "round"); // same-round subclass is skipped
		rows[1].weaponweight = 4.0f;
		rows[1].clipweight = 0.25f;
		std::strcpy(rows[2].round_type, "SUB");
		rows[2].maxclips = 2;
		rows[3].maxclips = 4;
		rows[3].clipweight = 1.5f;
		rows[5].teamfilter_mask = 1; // excluded before assigning grenade positions
		rows[6].maxclips = 2;
	}
};

void test_player_info_ammo() {
	Weapons weapons;
	const auto doc = document({"PRIMARY_AMMO1", "PRIMARY_AMMO1_TYPE", "PRIMARY_AMMO2"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "test.mnu", "LOADOUT"));
	int events = 0;
	std::vector<int> shown;
	menu.set_sink([&](const MenuEvent &event) {
		if (event.kind == MenuEvent::Kind::ValueChanged) ++events;
		if (event.kind == MenuEvent::Kind::ShownChanged) shown.push_back(event.id);
	});
	const int first = menu.widget_id("PRIMARY_AMMO1");
	const int type = menu.widget_id("PRIMARY_AMMO1_TYPE");
	const int second = menu.widget_id("PRIMARY_AMMO2");
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, 1, 1, text) == 1);
	CHECK(shown == (std::vector<int>{first, type, second}));
	CHECK(menu.get_widget_items(first) == (std::vector<std::string>{"10 - ROUND", "20 - ROUND", "30 - ROUND"}));
	CHECK(menu.selected_row(first) == 2);
	CHECK(menu.get_widget_items(second) == (std::vector<std::string>{"1 - SUB", "2 - SUB"}));
	CHECK(menu.selected_row(second) == 0);
	// The byte names the row, whatever value the row authors (this fixture authors
	// 2/0/1) [orig: CListWnd_SetRowVisibility @0x55e0af].
	CHECK(menu.selected_row(type) == 1);
	CHECK(!menu.is_widget_disabled(type));
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, 2, -1, 7, text) == 7);
	CHECK(menu.selected_row(first) == 1 && menu.selected_row(second) == 1);
	CHECK(menu.selected_row(type) == 1); // a row the list lacks keeps the selection
	// A saved -1 (a page's filler) selects row 0 and stays -1
	// [orig: PlayerInfo_PopulateAmmoComboBoxes @0x55e08a..0x55e091].
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, -1, -1, text) == -1);
	CHECK(menu.selected_row(type) == 0);
	// NOAMMOTYPES locks the combo on the byte's row, then resets the byte.
	weapons.rows[0].flags2 = def::DEF_WEAPON_FLAG2_NOAMMOTYPES;
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, 0, 0, 2, text) == 0);
	CHECK(menu.is_widget_disabled(type) && menu.selected_row(type) == 2);
	CHECK(menu.item_count(type) == 3 && menu.item_value(type, 0) == "2");
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", -1, -1, -1, 2, text) == 2);
	CHECK(!menu.is_widget_shown(first) && !menu.is_widget_shown(type) && !menu.is_widget_shown(second));
	CHECK(menu.item_count(first) == 3 && menu.item_count(second) == 2); // hide, retain rows
	weapons.rows[0].clipsize = 0;
	player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, -1, 2, text);
	CHECK(!menu.is_widget_shown(second)); // live subclass needs a live parent ammo leg
	CHECK(events == 0);
	const auto partial = document({"PRIMARY_AMMO1", "ACCESSORY_AMMO1", "ACCESSORY_AMMO1_TYPE"});
	menu.open_document(&partial, "partial.mnu", "LOADOUT");
	weapons.rows[0].clipsize = 10;
	// An absent TYPE still takes the byte's reset; the ACCESSORY has no byte and no combo.
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, -1, 2, text) == 0);
	CHECK(menu.selected_row(menu.widget_id("PRIMARY_AMMO1")) == 2);
	const int accessory_type = menu.widget_id("ACCESSORY_AMMO1_TYPE");
	CHECK(player_info_fill_ammo(menu, weapons.file, "ACCESSORY", 0, -1, -1, 2, text) == 2);
	CHECK(!menu.is_widget_disabled(accessory_type));
	// A screen without AMMO1 touches neither the combo nor the byte [orig: @0x55e040].
	const auto bare = document({"PRIMARY_AMMO1_TYPE"});
	menu.open_document(&bare, "bare.mnu", "LOADOUT");
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, -1, 2, text) == 2);
	CHECK(!menu.is_widget_disabled(menu.widget_id("PRIMARY_AMMO1_TYPE")));
}

void test_grenade_position_and_visibility() {
	Weapons weapons;
	const auto doc = document({"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"});
	MenuRuntime menu;
	menu.open_document(&doc, "test.mnu", "LOADOUT");
	const int first = menu.widget_id("GRENADE_AMMO1"), second = menu.widget_id("GRENADE_AMMO2");
	const int third = menu.widget_id("GRENADE_AMMO3");
	CHECK(player_info_fill_grenades(menu, weapons.file, 8, 2, {{3, 0}, {4, 8}}, text) == (std::vector<int>{3, 4, 6}));
	CHECK(menu.item_count(first) == 5 && menu.selected_row(first) == 0);
	CHECK(menu.item_count(second) == 4 && menu.selected_row(second) == 3);
	CHECK(menu.item_count(third) == 3 && menu.selected_row(third) == 2);
	CHECK(menu.item_text(first, 0) == "0 - ROUND");
	CHECK(player_info_fill_grenades(menu, weapons.file, 1, 2, {}, text).empty());
	CHECK(!menu.is_widget_shown(first) && !menu.is_widget_shown(third));
	CHECK(menu.item_count(first) == 5); // leftover controls hide without clearing
	// The WEAPON screen's positions [orig: UI_PopulateVehicleAmmoSlotList @0x5643c0]: each
	// shown, its rows 0..maxclips (the zero row alone when banned), the row its count
	// names (the def's maxclips untouched, none past its rows); the rest hidden.
	ArmoryScreenPairs pairs;
	pairs.set_clips(5, 2); // weapon.def row 4's catalog row
	const WeaponAvailability availability = [](const std::string &name) { return name == "WPN_3" ? 0 : 3; };
	CHECK(armory_fill_grenades(menu, weapons.file, 8, 2, pairs, availability, text) ==
			(std::vector<int>{3, 4, 6}));
	CHECK(menu.is_widget_shown(first) && menu.is_widget_shown(second));
	CHECK(menu.is_widget_shown(third));
	CHECK(menu.item_count(first) == 1 && menu.selected_row(first) == -1); // banned, untouched: none
	CHECK(menu.item_count(second) == 4 && menu.selected_row(second) == 2);
	CHECK(menu.item_count(third) == 3 && menu.selected_row(third) == 2); // untouched: maxclips
	pairs.set_clips(7, 5); // past row 6's rows
	armory_fill_grenades(menu, weapons.file, 8, 2, pairs, availability, text);
	CHECK(menu.selected_row(third) == -1);
	CHECK(armory_fill_grenades(menu, weapons.file, 1, 2, pairs, {}, text).empty());
	CHECK(!menu.is_widget_shown(first) && !menu.is_widget_shown(third));
	CHECK(menu.item_count(first) == 1); // hidden, the rows kept
	const auto partial = document({"GRENADE_AMMO1", "GRENADE_AMMO3"});
	menu.open_document(&partial, "partial.mnu", "LOADOUT");
	CHECK(player_info_fill_grenades(menu, weapons.file, 8, 2, {}, text) == (std::vector<int>{3, 4, 6}));
	CHECK(menu.item_count(menu.widget_id("GRENADE_AMMO3")) == 3); // missing second control is not compacted
}

// The WEAPON screen's ammo controls over the selected weapon and the screen's counts
// [orig: UI_PopulateAmmoTypeComboBoxes @0x564930 - the PRIMARY leg @0x564b86..0x564e7f]:
// AMMO1's row by its count (maxclips untouched, none for a count off its rows), the TYPE at
// its byte's row and locked by NOAMMOTYPES, which zeroes the byte, AMMO2 over the
// other-round subclass by the second count; no weapon or no clip hides them, rows kept,
// AMMO2 still answering a subclass with a clip.
void test_armory_ammo() {
	Weapons weapons;
	const auto doc = document({"PRIMARY_AMMO1", "PRIMARY_AMMO1_TYPE", "PRIMARY_AMMO2"});
	MenuRuntime menu;
	menu.open_document(&doc, "test.mnu", "LOADOUT");
	const int first = menu.widget_id("PRIMARY_AMMO1");
	const int type = menu.widget_id("PRIMARY_AMMO1_TYPE");
	const int second = menu.widget_id("PRIMARY_AMMO2");
	ArmoryScreenPairs pairs;
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, pairs, text);
	CHECK(menu.is_widget_shown(first) && menu.is_widget_shown(type));
	CHECK(menu.is_widget_shown(second));
	CHECK(menu.get_widget_items(first) ==
			(std::vector<std::string>{"10 - ROUND", "20 - ROUND", "30 - ROUND"}));
	CHECK(menu.selected_row(first) == 2);
	CHECK(menu.get_widget_items(second) == (std::vector<std::string>{"1 - SUB", "2 - SUB"}));
	CHECK(menu.selected_row(second) == 1);
	CHECK(menu.selected_row(type) == 0 && !menu.is_widget_disabled(type));
	pairs.set_clips(1, 2);
	pairs.set_secondary(1, 1);
	pairs.type[0] = 2;
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, pairs, text);
	CHECK(menu.selected_row(first) == 1 && menu.selected_row(second) == 0);
	CHECK(menu.selected_row(type) == 2);
	pairs.set_clips(1, 0);
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, pairs, text);
	CHECK(menu.selected_row(first) == -1); // a count no row carries selects none
	pairs.type[0] = 7;
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, pairs, text);
	CHECK(menu.selected_row(type) == 2); // a byte past the TYPE's rows keeps its row
	weapons.rows[0].flags2 = def::DEF_WEAPON_FLAG2_NOAMMOTYPES;
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, pairs, text);
	CHECK(menu.selected_row(type) == 2 && menu.is_widget_disabled(type) && pairs.type[0] == 0);
	armory_fill_ammo(menu, weapons.file, "PRIMARY", -1, pairs, text);
	CHECK(!menu.is_widget_shown(first) && !menu.is_widget_shown(type));
	CHECK(!menu.is_widget_shown(second));
	CHECK(menu.item_count(first) == 3);
	weapons.rows[0].clipsize = 0;
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, pairs, text);
	CHECK(!menu.is_widget_shown(first) && !menu.is_widget_shown(type));
	CHECK(menu.is_widget_shown(second));
}

void test_weight_gates_and_count_asymmetry() {
	Weapons weapons;
	const auto doc = document({"PRIMARY_AMMO1", "PRIMARY_AMMO2", "GRENADE_AMMO1", "GRENADE_AMMO2"});
	MenuRuntime menu;
	menu.open_document(&doc, "test.mnu", "LOADOUT");
	const int first = menu.widget_id("GRENADE_AMMO1"), second = menu.widget_id("GRENADE_AMMO2");
	menu.set_widget_shown(menu.widget_id("PRIMARY_AMMO2"), false);
	menu.set_widget_shown(second, false);
	const LoadoutParentIndices parents{0, 1, -1};
	CHECK(player_info_screen_weight(menu, weapons.file, parents, {3, 4, 6}, {}, {}) == 18.25);
	// Hidden AMMO2 still contributes (its control's presence gates it); hidden/absent
	// grenade controls do not [orig: PlayerInfo_CalculateLoadoutWeight @0x55f1f0 - the
	// AMMO2 lookup @0x55f2f9..0x55f301, CWnd_IsShown @0x55f40e].
	CHECK(player_info_screen_weight(menu, weapons.file, parents, {3, 4, 6}, {{0, 0}, {3, 0}}, {{0, 0}}) == 12.25);
	menu.set_widget_shown(first, false);
	CHECK(player_info_screen_weight(menu, weapons.file, parents, {3, 4}, {}, {}) == 12.25);
	CHECK(player_info_screen_weight(menu, weapons.file, {-1, 99, -2}, {-1}, {}, {}) == 0.0);
}

// The WEAPON screen's weight [orig: UI_CalculateEquippedWeaponsWeight @0x565490]: a listed
// category's weapon weight, its AMMO1 and AMMO2 while shown and selected, (row + 1) times
// the weapon's clip weight, and each grenade position while shown and selected, its row
// times its clip weight; no selection, a hidden control or an absent category list adds
// nothing.
void test_armory_weight() {
	Weapons weapons;
	const auto doc = document({"PRIMARY", "PRIMARY_AMMO1", "PRIMARY_AMMO2", "GRENADE_AMMO1",
			"GRENADE_AMMO2"});
	MenuRuntime menu;
	menu.open_document(&doc, "test.mnu", "LOADOUT");
	const int ammo1 = menu.widget_id("PRIMARY_AMMO1"), ammo2 = menu.widget_id("PRIMARY_AMMO2");
	const int g1 = menu.widget_id("GRENADE_AMMO1"), g2 = menu.widget_id("GRENADE_AMMO2");
	menu.select_row(ammo1, 1, false);
	menu.select_row(ammo2, 0, false);
	menu.select_row(g1, 2, false);
	menu.select_row(g2, 1, false);
	const LoadoutParentIndices parents{0, -1, -1};
	// 2 + 2 * 0.5 + 1 * 0.5 + 2 * 1.5 + 1 * 2
	CHECK(armory_screen_weight(menu, weapons.file, parents, {3, 4}) == 8.5);
	menu.set_widget_shown(ammo2, false);
	menu.set_widget_shown(g2, false);
	CHECK(armory_screen_weight(menu, weapons.file, parents, {3, 4}) == 6.0);
	menu.select_row(ammo1, -1, false);
	CHECK(armory_screen_weight(menu, weapons.file, parents, {3, 4}) == 5.0);
	CHECK(armory_screen_weight(menu, weapons.file, {-1, -1, -1}, {3, 4}) == 3.0);
	const auto bare = document({"PRIMARY_AMMO1"});
	menu.open_document(&bare, "bare.mnu", "LOADOUT");
	menu.select_row(menu.widget_id("PRIMARY_AMMO1"), 0, false);
	CHECK(armory_screen_weight(menu, weapons.file, parents, {}) == 0.0); // no PRIMARY list
}

} // namespace

int main() {
	test_player_info_ammo();
	test_grenade_position_and_visibility();
	test_armory_ammo();
	test_weight_gates_and_count_asymmetry();
	test_armory_weight();
	std::cout << "loadout_screen: " << failures << " failures\n";
	return failures != 0;
}
