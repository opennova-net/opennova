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
	CHECK(menu.selected_row(type) == 2); // authored value 1 is the third row
	CHECK(!menu.is_widget_disabled(type));
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, 2, -1, 7, text) == 7);
	CHECK(menu.selected_row(first) == 1 && menu.selected_row(second) == 1);
	CHECK(menu.selected_row(type) == 2); // missing value leaves selection alone
	weapons.rows[0].flags2 = def::DEF_WEAPON_FLAG2_NOAMMOTYPES;
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, 0, 0, 2, text) == 0);
	CHECK(menu.is_widget_disabled(type) && menu.selected_row(type) == 1);
	CHECK(menu.item_count(type) == 3 && menu.item_value(type, 0) == "2");
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", -1, -1, -1, 2, text) == 2);
	CHECK(!menu.is_widget_shown(first) && !menu.is_widget_shown(type) && !menu.is_widget_shown(second));
	CHECK(menu.item_count(first) == 3 && menu.item_count(second) == 2); // hide, retain rows
	weapons.rows[0].clipsize = 0;
	player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, -1, 2, text);
	CHECK(!menu.is_widget_shown(second)); // live subclass needs a live parent ammo leg
	CHECK(events == 0);
	const auto partial = document({"PRIMARY_AMMO1"});
	menu.open_document(&partial, "partial.mnu", "LOADOUT");
	weapons.rows[0].clipsize = 10;
	CHECK(player_info_fill_ammo(menu, weapons.file, "PRIMARY", 0, -1, -1, 2, text) == 2);
	CHECK(menu.selected_row(menu.widget_id("PRIMARY_AMMO1")) == 2); // absent TYPE does not reset
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
	std::vector<playersav::KitEntry> current(3);
	current[0].name = "wpn_3"; current[0].ammo_primary = -1;
	current[1].name = "WPN_4"; current[1].ammo_primary = 2;
	current[2].name = "WPN_4"; current[2].ammo_primary = 1; // first match wins
	const WeaponAvailability availability = [](const std::string &name) { return name == "WPN_3" ? 0 : 3; };
	CHECK(armory_fill_grenades(menu, weapons.file, 8, 2, current, availability, text) == (std::vector<int>{3, 4, 6}));
	CHECK(menu.item_count(first) == 1 && menu.selected_row(first) == 0); // banned still owns row 1
	CHECK(menu.item_count(second) == 4 && menu.selected_row(second) == 2);
	CHECK(menu.selected_row(third) == 0); // no current tuple chooses zero
	// Ours: the armory retains the authored/runtime visibility where retail shows
	// every assigned grenade control (@0x56481f, D-MNU-9).
	CHECK(!menu.is_widget_shown(first));
	CHECK(armory_fill_grenades(menu, weapons.file, 1, 2, {}, {}, text).empty());
	CHECK(menu.get_widget_items(first) == (std::vector<std::string>{"0 - "}));
	const auto partial = document({"GRENADE_AMMO1", "GRENADE_AMMO3"});
	menu.open_document(&partial, "partial.mnu", "LOADOUT");
	CHECK(player_info_fill_grenades(menu, weapons.file, 8, 2, {}, text) == (std::vector<int>{3, 4, 6}));
	CHECK(menu.item_count(menu.widget_id("GRENADE_AMMO3")) == 3); // missing second control is not compacted
}

void test_armory_ammo_reselection() {
	Weapons weapons;
	const auto doc = document({"PRIMARY_AMMO1"});
	MenuRuntime menu;
	menu.open_document(&doc, "test.mnu", "LOADOUT");
	const int id = menu.widget_id("PRIMARY_AMMO1");
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, "wpn_0", 2, text);
	CHECK(menu.item_count(id) == 3 && menu.selected_row(id) == 1);
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, "WPN_OTHER", 1, text);
	CHECK(menu.selected_row(id) == 2);
	armory_fill_ammo(menu, weapons.file, "PRIMARY", -1, "WPN_0", 1, text);
	CHECK(menu.item_count(id) == 0 && menu.selected_row(id) == -1);
	// Ours: the armory still lists maxclips where retail hides AMMO1 on clipsize
	// <= 0 as PLAYER_INFO does (@0x564be2, D-MNU-9).
	weapons.rows[0].clipsize = 0;
	armory_fill_ammo(menu, weapons.file, "PRIMARY", 0, "", -1, text);
	CHECK(menu.item_count(id) == 3 && menu.selected_row(id) == 2);
	CHECK(menu.item_text(id, 0) == "0 - ROUND");
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
	// Hidden AMMO2 still contributes; hidden/absent grenade controls do not
	// (ours: retail gates every extra-ammo arm on IsShown, D-MNU-9).
	CHECK(player_info_screen_weight(menu, weapons.file, parents, {3, 4, 6}, {{0, 0}, {3, 0}}, {{0, 0}}) == 12.25);
	menu.set_widget_shown(first, false);
	CHECK(player_info_screen_weight(menu, weapons.file, parents, {3, 4}, {}, {}) == 12.25);
	menu.select_row(menu.widget_id("PRIMARY_AMMO1"), 0, false);
	menu.select_row(first, 2, false);
	menu.select_row(second, 1, false);
	CHECK(armory_screen_weight(menu, weapons.file, parents, {3, 4, 6}) == 12.25);
	// Armory includes hidden grenade rows (ours, see armory_screen_weight) but
	// excludes every subclass term.
	menu.select_row(first, 0, false);
	CHECK(armory_screen_weight(menu, weapons.file, parents, {3, 4}) == 9.25);
	menu.set_widget_items(menu.widget_id("PRIMARY_AMMO1"), {});
	// An unselected AMMO1 falls to the maxclips term (ours: retail adds no term).
	CHECK(armory_screen_weight(menu, weapons.file, parents, {3, 4}) == 10.25);
	CHECK(player_info_screen_weight(menu, weapons.file, {-1, 99, -2}, {-1}, {}, {}) == 0.0);
	CHECK(armory_screen_weight(menu, {}, parents, {3, 4}) == 0.0);
}

} // namespace

int main() {
	test_player_info_ammo();
	test_grenade_position_and_visibility();
	test_armory_ammo_reselection();
	test_weight_gates_and_count_asymmetry();
	std::cout << "loadout_screen: " << failures << " failures\n";
	return failures != 0;
}
