// The loadout dialogs' list and state model: the category list fill (the slot
// filter, the WEAPON screen's availability term, the row order under NONE), the
// reselect by weapon.def row, the WEAPON screen's clip count and per-class loadout
// memory (runtime/menu/armory_loadout.h), and the PLAYER_INFO panel's state (runtime/menu/
// player_info_loadout.h): the kit restore's catalog rows, the handlers' picks
// (row + 1, the authored type byte, the grenade row), the selected clips and
// the kit page's selection.
// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430; UI_PopulateThreeCategoryLists
//  @0x566db0; PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55e8b0; @0x55f730;
//  @0x55f760; @0x55fb70; WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0]

#include <runtime/menu/armory_loadout.h>
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
	// An absent control fills nothing.
	CHECK(loadout_fill_slot(menu, weapons.file, "ACCESSORY", 0, 8, 2, {}, "None", text).empty());
	CHECK(loadout_selected_weapon(menu, "ACCESSORY", rows) == -1);
}

// The WEAPON screen's AMMO1 read seam: row + 1, -1 with no control or no selection.
void test_armory_selected_clips() {
	Weapons weapons;
	const auto doc = document({"PRIMARY", "PRIMARY_AMMO1"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "test.mnu", "LOADOUT"));
	const int ammo = menu.widget_id("PRIMARY_AMMO1");
	menu.select_row(ammo, 1, false);
	CHECK(armory_selected_clips(menu, "PRIMARY") == 2);
	CHECK(armory_selected_clips(menu, "SECONDARY") == -1);
	menu.select_row(ammo, -1, false);
	CHECK(armory_selected_clips(menu, "PRIMARY") == -1);
}

std::string names_of(const playersav::KitPage &page) {
	std::string out;
	for (const auto &e : page.entries) out += e.name + ":" + std::to_string(e.ammo_primary) + "," +
			std::to_string(e.ammo_secondary) + "," + std::to_string(e.flags) + " ";
	return out;
}

playersav::KitEntry entry(const char *name, int32_t primary, int32_t secondary, int32_t flags) {
	playersav::KitEntry e;
	e.name = name;
	e.ammo_primary = primary;
	e.ammo_secondary = secondary;
	e.flags = flags;
	return e;
}

// The per-class loadout memory (D-MNU-9): the stage of the record's side pages from class 5
// on and of the spawn buffer into class 1, under the on-show's team gate; the populate over a
// class's page (the walk's selects, counts and type bytes, then the fills); and the page the
// screen writes back, the team's knife first, the medic's medpack, the categories with their
// counts and bytes, the shown grenade positions with theirs
// [orig: Armory_StageLoadoutBuffers @0x564290; UI_InitTeamClassSelection @0x56740c..0x567419;
//  UI_PopulateAmmoTypeComboBoxes @0x564930; WeaponLoadout_SerializeSelectionsToBuffer
//  @0x5658b0].
void test_armory_page_memory() {
	Weapons weapons;
	playersav::KitPage blue;
	blue.entries = {entry("WPN_KNIFE", -1, -1, -1), entry("WPN_MIKE", 2, -1, 1),
			entry("WPN_PISTOL", 1, -1, -1), entry("WPN_SMOKE", 2, -1, -1)};
	playersav::KitPage red;
	red.entries = {entry("wpn_alpha", 3, -1, 0)};
	playersav::KitPage spawn;
	spawn.entries = {entry("WPN_Zulu", -1, -1, -1)};
	playersav::Record record;
	record.blue.pages[3] = blue; // class 8
	record.red.pages[3] = red;
	ArmoryLoadoutBuffers buffers;
	CHECK(buffers.stage_on_show(record, 1, spawn));
	CHECK(names_of(buffers.page(8)) == names_of(blue));
	CHECK(names_of(buffers.page(1)) == names_of(spawn));
	CHECK(buffers.page(9).entries.empty() && buffers.page(12).entries.empty());
	CHECK(!buffers.stage_on_show(record, 3, spawn)); // the same mask: no stage
	buffers.store(8, red);
	CHECK(names_of(buffers.page(8)) == names_of(red));
	CHECK(buffers.stage_on_show(record, 2, spawn)); // another mask stages the red side
	CHECK(names_of(buffers.page(8)) == names_of(red));
	buffers.stage(record, 1, spawn); // the mission start's stage keeps the gate's mask
	CHECK(names_of(buffers.page(8)) == names_of(blue));
	CHECK(!buffers.stage_on_show(record, 4, spawn));
	buffers.store(16, blue);
	CHECK(buffers.page(16).entries.empty() && buffers.page(-1).entries.empty());

	const auto doc = document({"PRIMARY", "SECONDARY", "PRIMARY_AMMO1", "SECONDARY_AMMO1",
			"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "test.mnu", "LOADOUT"));
	LoadoutSlotRows rows[3];
	rows[0] = loadout_fill_slot(menu, weapons.file, "PRIMARY", 1, 8, 2, {}, "NONE", text);
	rows[1] = loadout_fill_slot(menu, weapons.file, "SECONDARY", 2, 8, 2, {}, "NONE", text);
	ArmoryScreenPairs pairs;
	armory_populate_page(menu, weapons.file, rows, blue, pairs, text);
	CHECK(menu.selected_row(menu.widget_id("PRIMARY")) == 2); // WPN_MIKE
	CHECK(menu.selected_row(menu.widget_id("SECONDARY")) == 1); // WPN_PISTOL
	CHECK(menu.selected_row(menu.widget_id("PRIMARY_AMMO1")) == 1);
	CHECK(menu.selected_row(menu.widget_id("SECONDARY_AMMO1")) == 0);
	CHECK(pairs.type[0] == 1 && pairs.type[1] == -1);
	const std::vector<int> grenades =
			armory_fill_grenades(menu, weapons.file, 8, 2, pairs, {}, text);
	CHECK(grenades == (std::vector<int>{5, 6, 7}));
	CHECK(menu.selected_row(menu.widget_id("GRENADE_AMMO1")) == 3); // untouched FRAG: maxclips
	CHECK(menu.selected_row(menu.widget_id("GRENADE_AMMO2")) == 2); // SMOKE's page count
	CHECK(names_of(armory_serialize_page(menu, weapons.file, rows, grenades, pairs, 1, 8)) ==
			"WPN_KNIFE:-1,-1,-1 WPN_MIKE:2,-1,1 WPN_PISTOL:1,-1,-1 WPN_FRAG:-1,-1,-1 "
			"WPN_SMOKE:2,-1,-1 WPN_FLASH:-1,-1,-1 ");
	// A pick moves its count; a red medic's page leads with the second knife and the medpack;
	// a hidden grenade position and a NONE category write nothing.
	pairs.set_clips(3, 3);
	menu.set_widget_shown(menu.widget_id("GRENADE_AMMO3"), false);
	menu.select_row(menu.widget_id("SECONDARY"), 0, false);
	CHECK(names_of(armory_serialize_page(menu, weapons.file, rows, grenades, pairs, 2, 5)) ==
			"WPN_KNIFE2:-1,-1,-1 WPN_MEDPACK:-1,-1,-1 WPN_MIKE:3,-1,1 WPN_FRAG:-1,-1,-1 "
			"WPN_SMOKE:2,-1,-1 ");
	// A page naming a weapon its list lacks leaves the list with no selection; the counts
	// reset to the page's, and a byte no entry carries outlives the populate.
	pairs.type[1] = 2;
	playersav::KitPage foreign;
	foreign.entries = {entry("WPN_RED", 1, -1, 0)};
	armory_populate_page(menu, weapons.file, rows, foreign, pairs, text);
	CHECK(menu.selected_row(menu.widget_id("PRIMARY")) == -1);
	CHECK(menu.selected_row(menu.widget_id("SECONDARY")) == 0);
	CHECK(pairs.type[0] == 0 && pairs.type[1] == 2);
	CHECK(pairs.clips_of(4) == 1 && pairs.clips_of(3) == -1);
}

// The WEAPON screen's state over the class pages (runtime/menu/armory_loadout.h): the
// populate over the selected class's staged page, the handlers' writes (an AMMO1 pick
// row + 1, a TYPE pick its row, a grenade pick its row), the remembered count a weapon's
// reselect shows, the class flip's save of the page it leaves and the ACCEPT's
// [orig: UI_PopulateThreeCategoryLists @0x566db0; WeaponDef_RegisterUICallbacks @0x567020;
//  sub_566620 @0x566620; sub_566650 @0x566650; WeaponDef_UISlotSelectCallback @0x566d70;
//  UI_OnPrimaryWeaponTypeChanged @0x5662d0; UI_HandleTeamClassSelection @0x566f60;
//  WeaponLoadout_ApplyFromBuffer @0x565cd0].
void test_armory_loadout_state() {
	Weapons weapons;
	const auto doc = document({"PRIMARY", "SECONDARY", "ACCESSORY", "PRIMARY_AMMO1",
			"PRIMARY_AMMO1_TYPE", "GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"});
	MenuRuntime menu;
	CHECK(menu.open_document(&doc, "test.mnu", "LOADOUT"));
	const int primary = menu.widget_id("PRIMARY"), ammo1 = menu.widget_id("PRIMARY_AMMO1");
	const int type = menu.widget_id("PRIMARY_AMMO1_TYPE");
	playersav::Record record;
	record.blue.pages[3].entries = {entry("WPN_KNIFE", -1, -1, -1), entry("WPN_MIKE", 2, -1, 1),
			entry("WPN_SMOKE", 1, -1, -1)};
	ArmoryLoadout state;
	CHECK(state.buffers.stage_on_show(record, 1, {}));
	state.populate(menu, weapons.file, 1, 8, {}, "NONE", text);
	CHECK(state.slot_rows[0] == (LoadoutSlotRows{-1, 1, 2, 0}));
	CHECK(state.selected_weapon(menu, "PRIMARY") == 2 && menu.selected_row(primary) == 2);
	CHECK(menu.selected_row(ammo1) == 1 && menu.selected_row(type) == 1);
	CHECK(state.grenade_rows == (std::vector<int>{5, 6, 7}));
	CHECK(menu.selected_row(menu.widget_id("GRENADE_AMMO2")) == 1);
	menu.select_row(ammo1, 0, false);
	CHECK(state.pick_count(menu, "PRIMARY_AMMO1", 0) && state.pairs.clips_of(3) == 1);
	menu.select_row(type, 2, false);
	CHECK(state.pick_count(menu, "primary_ammo1_type", 2) && state.pairs.type[0] == 2);
	menu.select_row(menu.widget_id("GRENADE_AMMO3"), 0, false);
	CHECK(state.pick_count(menu, "GRENADE_AMMO3", 0) && state.pairs.clips_of(8) == 0);
	CHECK(!state.pick_count(menu, "ACCESSORY_AMMO1_TYPE", 0));
	CHECK(!state.pick_count(menu, "PRIMARY", 0));
	// MIKE 4 + its one clip 0.5, FRAG's untouched three 1.5, SMOKE's one 0.5, FLASH's none.
	CHECK(state.weight(menu, weapons.file) == 6.5);
	// Another weapon shows its own count (untouched: its maxclips); MIKE's comes back.
	menu.select_row(primary, 1, false);
	state.pick_weapon(menu, weapons.file, "PRIMARY", text);
	CHECK(menu.selected_row(ammo1) == 2);
	menu.select_row(primary, 2, false);
	state.pick_weapon(menu, weapons.file, "PRIMARY", text);
	CHECK(menu.selected_row(ammo1) == 0 && menu.selected_row(type) == 2);
	// The flip saves the page it leaves; the medic's page lists nothing for these weapons.
	state.flip_class(menu, weapons.file, 1, 8, 5, {}, "NONE", text);
	CHECK(names_of(state.buffers.page(8)) ==
			"WPN_KNIFE:-1,-1,-1 WPN_MIKE:1,-1,2 WPN_FRAG:-1,-1,-1 WPN_SMOKE:1,-1,-1 "
			"WPN_FLASH:0,-1,-1 ");
	CHECK(menu.item_count(primary) == 1 && state.grenade_rows.empty());
	CHECK(!menu.is_widget_shown(menu.widget_id("GRENADE_AMMO1")));
	state.flip_class(menu, weapons.file, 1, 5, 8, {}, "NONE", text);
	CHECK(names_of(state.buffers.page(5)) == "WPN_KNIFE:-1,-1,-1 WPN_MEDPACK:-1,-1,-1 ");
	CHECK(menu.selected_row(primary) == 2 && menu.selected_row(ammo1) == 0);
	CHECK(menu.selected_row(menu.widget_id("GRENADE_AMMO3")) == 0);
	// The ACCEPT serializes into the class's page, a red entity's knife first.
	CHECK(names_of(state.accept(menu, weapons.file, 2, 8)) ==
			"WPN_KNIFE2:-1,-1,-1 WPN_MIKE:1,-1,2 WPN_FRAG:-1,-1,-1 WPN_SMOKE:1,-1,-1 "
			"WPN_FLASH:0,-1,-1 ");
	CHECK(names_of(state.buffers.page(8)).rfind("WPN_KNIFE2:", 0) == 0);
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
	test_armory_selected_clips();
	test_armory_page_memory();
	test_armory_loadout_state();
	test_player_info_state();
	std::cout << "loadout_dialog: " << failures << " failures\n";
	return failures != 0;
}
