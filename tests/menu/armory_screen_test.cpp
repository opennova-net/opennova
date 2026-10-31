// The in-game armory's WEAPON screen driven headless (menu/armory_screen.h): the controller over
// the runtime's own store and an authored WEAPON document, its host faked. weapon.def text is
// authored here and read through the engine's parser; the shipped-value pins read the authored
// fixtures/game/weapon.mnu and the shipped weapon.def values the loadout catalog carries
// (WPN_M4AUTO weaponweight 5.5, maxclips 10, clipweight 1.5, clipsize 30, round AMMO_CAR15_556MM;
// WPN_SATCHEL_CHARGE maxclips 2, clipweight 17.6; WPN_GRENADEFB round GRENADEFB; prove once, pin).
//
// Pinned: the INIT's class spin (Game.bin's CHARCLASS_* strings, the "??key??" miss) and the
// on-show's order (the class resolve and the select by value, the rows the host's mask enables,
// the class pair shown in a session alone, the stage under the team gate, the populate and its
// weight and icons); the category lists over the class's page under Game.bin's NONE; a class flip
// saving the page it leaves (D-MNU-9); the count and weapon picks and the readout; a banned
// grenade keeping its table-order control; ACCEPT and CANCEL past the press latch (the input reset,
// the page, the session or the local apply and its viewmodel, the close), nothing while the latch
// holds; the empty and the missing table; a DFX2 title's DEATH screen picker (DFX2-41: the
// activation's fill under the title's lists, a pick, the close's submit past the latch).
// [orig: UI_InitWeaponClassSelection @0x567250; UI_InitTeamClassSelection @0x567370;
//  UI_PopulateThreeCategoryLists @0x566db0; UI_HandleTeamClassSelection @0x566f60;
//  UI_UpdateWeaponWeightDisplay @0x565640; WeaponLoadout_ApplyFromBuffer @0x565cd0]
// [orig: sub_550690 @0x550690; UI_RegisterDeathScreenCallbacks @0x5512a0; sub_550BA0 @0x550ba0
//  (dfx2.exe)]

#include <formats/def/def.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/armory_screen.h>
#include <runtime/menu/menu_runtime.h>

#include "common/test_paths.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::menu;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

std::string wnd(const char *type, const char *name, int top) {
	char out[320];
	std::snprintf(out, sizeof(out),
			"<WINDOW type=\"%s\" name=\"%s\"><POSITION><LEFT>10</LEFT><TOP>%d</TOP><RIGHT>250</RIGHT>"
			"<BOTTOM>%d</BOTTOM></POSITION></WINDOW>",
			type, name, top, top + 20);
	return out;
}

// A WEAPON screen of the controls the code registers (the GUT seam's synthetic screen).
mnu::Document weapon_screen(bool with_class_label) {
	std::string body = wnd("spinlist", "PLAYER_CLASS", 10);
	if (with_class_label) body += wnd("static", "STATIC_PLAYER_CLASS", 10);
	int y = 40;
	for (const char *name : {"PRIMARY", "SECONDARY", "ACCESSORY", "PRIMARY_AMMO1", "SECONDARY_AMMO1",
				 "ACCESSORY_AMMO1", "GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"}) {
		body += wnd("combobox", name, y);
		y += 24;
	}
	for (const char *name : {"PRIMARY_ICON", "SECONDARY_ICON", "ACCESSORY_ICON"}) {
		body += wnd("window", name, y);
		y += 24;
	}
	body += wnd("button", "ACCEPT", y) + wnd("button", "CANCEL", y + 24) +
			wnd("static", "STATIC_TOTAL_WEIGHT", y + 48);
	const std::string xml = "<SCREEN><NAME>WEAPON</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION>"
	                        "<LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" +
	                        body + "</WINDOW></SCREEN>";
	mnu::Document doc;
	std::string error;
	CHECK(mnu::parse(xml, doc, error));
	return doc;
}

// One weapon.def block (CRLF, as the shipped file).
std::string weapon(const char *name, int category, int rank, const char *slot, int clip, int maxclips,
		double weight, double clipweight, const char *team, const char *classes, const char *round,
		const char *icon = nullptr, int subclasses = 0) {
	char out[1200];
	std::snprintf(out, sizeof(out),
			"weapon \"%s\"\r\n category %d\r\n rank %d\r\n clipsize %d\r\n maxclips %d\r\n startrounds %d\r\n"
			" ammoclass CLASS_FIXTURE 1\r\n weapon_class %s\r\n loadout_selectable 1\r\n loadout_subclasses %d\r\n"
			" weaponweight %g\r\n clipweight %g\r\n round_type %s\r\n%s%s%s%s",
			name, category, rank, clip, maxclips, clip * maxclips, slot, subclasses, weight, clipweight, round,
			team, classes, icon ? " loadout_menu_icon " : "", icon ? icon : "");
	return std::string(out) + (icon ? "\r\n" : "") + "end\r\n";
}

const char *kBlue = " teamfilter blue\r\n";
const char *kRed = " teamfilter red\r\n";
const char *kBoth = " teamfilter blue\r\n teamfilter red\r\n";
const char *kRifle = " charfilter rifleman\r\n charfilter engineer\r\n";

// The armory catalog: blue and red primaries (the red one a rifleman/engineer AK), a blue
// sniper-only rifle, a secondary, an accessory and three grenades.
std::string catalog() {
	return std::string("ammoclass_max_carry CLASS_FIXTURE 1000\r\n") +
	       weapon("WPN_M4AUTO", 3, 0, "primary", 30, 10, 5.5, 1.5, kBlue, kRifle, "AMMO_CAR15_556MM",
	               "M_4.tga\r\n loadout_menu_textid WEAP_SHORT_M4") +
	       weapon("WPN_AK47AUTO", 3, 2, "primary", 30, 7, 4, 1.5, kRed, kRifle, "FIXTURE_ROUND") +
	       weapon("WPN_SNIPE", 3, 3, "primary", 5, 7, 9, 1, kBlue, " charfilter sniper\r\n", "FIXTURE_ROUND") +
	       weapon("WPN_M9Beretta", 2, 0, "secondary", 15, 7, 2, 0.5, kBoth, kRifle, "FIXTURE_ROUND") +
	       weapon("WPN_SATCHEL_CHARGE", 7, 0, "accessory", 1, 2, 0, 17.6, kBoth, kRifle, "AMMO_FX_CHARGE") +
	       weapon("WPN_GRENADEFB", 4, 0, "grenade", 1, 2, 0, 1.2, kBoth, kRifle, "GRENADEFB") +
	       weapon("WPN_GRENADEHE", 5, 0, "grenade", 1, 2, 0, 1, kBoth, kRifle, "FIXTURE_ROUND") +
	       weapon("WPN_GRENADESM", 6, 0, "grenade", 1, 3, 0, 0.7, kBoth, kRifle, "FIXTURE_ROUND");
}

struct Weapons {
	def::DefWeaponsFile file{};
	explicit Weapons(const std::string &text) {
		CHECK(def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &file) == 0);
	}
	~Weapons() { def::def_free_weapons(&file); }
	int row(const char *name) const { return def::def_weapon_index_by_name(file.entries, file.count, name); }
};

hud::GameTextLookup table(std::map<std::string, std::string> rows) {
	return [rows](const char *section, const char *key, const char *fallback) {
		const auto it = rows.find(std::string(section) + "/" + key);
		return it != rows.end() ? it->second : std::string(fallback);
	};
}

struct Host : ArmoryScreenHost {
	const def::DefWeaponsFile *table_ = nullptr;
	std::map<std::string, std::string> menu_ui_rows;
	std::map<std::string, std::string> gametext_rows;
	int team = 1;
	int cls = 0;
	uint32_t mask = 0x3FF;
	bool selection = true;
	bool session = false;
	bool latched = false;
	bool applies = true;
	playersav::Record record;
	bool has_record = false;
	playersav::KitPage spawn;
	WeaponAvailability allowed;
	int resets = 0, applied = 0, presented = 0, closes = 0, icon_calls = 0;
	playersav::KitPage last_kit;
	int last_class = -1;
	ArmoryIcons last_icons;

	const def::DefWeaponsFile *weapons() override { return table_; }
	hud::GameTextLookup gametext() override { return table(gametext_rows); }
	hud::GameTextLookup menu_ui() override { return table(menu_ui_rows); }
	hud::GameTextLookup menutxt() override { return {}; }
	int entity_team() override { return team; }
	int player_class() override { return cls; }
	uint32_t class_allow_mask() override { return mask; }
	bool class_selection() override { return selection; }
	const playersav::Record *profile_record() override { return has_record ? &record : nullptr; }
	playersav::KitPage spawn_page() override { return spawn; }
	WeaponAvailability availability() override { return allowed; }
	bool press_latched() override { return latched; }
	void reset_all_input_state() override { ++resets; }
	bool apply_loadout(const playersav::KitPage &kit, int player_class) override {
		++applied;
		last_kit = kit;
		last_class = player_class;
		return applies;
	}
	bool in_session() override { return session; }
	void present_equipped() override { ++presented; }
	void close_screen() override { ++closes; }
	void icons_changed(const ArmoryIcons &icons) override {
		++icon_calls;
		last_icons = icons;
	}
};

playersav::KitEntry entry(const char *name, int32_t primary = -1) {
	playersav::KitEntry e;
	e.name = name;
	e.ammo_primary = primary;
	return e;
}

// The host's record with a blue class page for `cls` holding `entries`.
void page_record(Host &host, int cls, std::vector<playersav::KitEntry> entries) {
	host.has_record = true;
	host.record.blue.pages[cls - 5].entries = std::move(entries);
}

// The runtime over `doc`, its events into `screen` (the shell's claim forwards them).
struct Rig {
	mnu::Document doc;
	MenuRuntime menu;
	ArmoryScreen screen;
	Host host;
	explicit Rig(mnu::Document document) : doc(std::move(document)) {
		screen.set_host(&host);
		menu.set_sink([this](const MenuEvent &event) {
			if (screen.is_open()) screen.on_event(menu, event);
		});
		CHECK(menu.open_document(&doc, kArmoryMenuFile, kArmoryScreenName));
	}
	void open() { screen.open(menu, kArmoryMenuFile, kArmoryScreenName); }
	int id(const char *name) const { return menu.widget_id(name); }
	std::vector<std::string> items(const char *name) const { return menu.get_widget_items(id(name)); }
	bool has(const char *list, const char *text) const {
		for (const std::string &row : items(list))
			if (row == text) return true;
		return false;
	}
	std::string weight() const { return menu.get_widget_text(id("STATIC_TOTAL_WEIGHT")); }
};

void test_init_and_unclassed_show() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.menu_ui_rows = {{"Menu/CHARCLASS_MEDIC", "Medic"}, {"Menu/NONE", "None"}};
	rig.host.selection = false; // retail's leg: no session
	rig.open();
	const int spin = rig.id("PLAYER_CLASS");
	// The five classes, Game.bin's strings, a miss its "??key??" [orig: @0x56729b..0x567351;
	// TextResource_GetStringWithFallback @0x562f16].
	CHECK(rig.items("PLAYER_CLASS") == (std::vector<std::string>{"Medic", "??CHARCLASS_SNIPER??",
			"??CHARCLASS_GUNNER??", "??CHARCLASS_RIFLEMAN??", "??CHARCLASS_ENGINEER??"}));
	// Unclassed: the resolve keeps class 0, the spin shows row 0, every class's weapons list.
	CHECK(rig.screen.selected_class() == 0 && rig.menu.selected_row(spin) == 0);
	// Out of a session the class pair hides [orig: @0x567467 / @0x567470].
	CHECK(!rig.menu.is_widget_shown(spin) && !rig.menu.is_widget_shown(rig.id("STATIC_PLAYER_CLASS")));
	CHECK(rig.items("PRIMARY") == (std::vector<std::string>{"None", "WPN_M4AUTO", "WPN_SNIPE"}));
	CHECK(rig.menu.selected_row(rig.id("PRIMARY")) == 0);
	// The INIT fills the spin once: a later show leaves its rows.
	rig.host.menu_ui_rows.clear();
	rig.open();
	CHECK(rig.items("PLAYER_CLASS")[0] == "Medic" && rig.items("PRIMARY")[0] == "??NONE??");
	// In a session (or offline under D-MNU-10) the pair shows; with no label, neither is touched.
	rig.host.selection = true;
	rig.open();
	CHECK(rig.menu.is_widget_shown(spin) && rig.menu.is_widget_shown(rig.id("STATIC_PLAYER_CLASS")));
	Rig bare(weapon_screen(false));
	bare.host.table_ = &weapons.file;
	bare.host.selection = false;
	bare.open();
	CHECK(bare.menu.is_widget_shown(bare.id("PLAYER_CLASS")));
}

void test_classed_show_page_and_counts() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	page_record(rig.host, 8, {entry("WPN_M4AUTO")});
	rig.open();
	CHECK(rig.menu.selected_row(rig.id("PLAYER_CLASS")) == 3); // rifleman by value
	CHECK(rig.screen.selected_class() == 8);
	// Rifleman, blue: NONE, then the M4 (the sniper rifle and the red AK filtered out).
	CHECK(rig.items("PRIMARY").size() == 2 && rig.has("PRIMARY", "WPN_M4AUTO") && !rig.has("PRIMARY", "WPN_SNIPE"));
	// A row's label is its loadout_menu_textid in gametext's WepDes, else the raw id
	// [orig: WeaponDef_ParseProperty @0x54d730 - GameText_GetString("WepDes", textid)].
	rig.host.gametext_rows = {{"WepDes/WEAP_SHORT_M4", "Authored rifle label"}};
	rig.open();
	CHECK(rig.has("PRIMARY", "Authored rifle label") && !rig.has("PRIMARY", "WPN_M4AUTO"));
	rig.host.gametext_rows.clear();
	rig.open();
	CHECK(rig.screen.selected_weapon("PRIMARY") == weapons.row("WPN_M4AUTO"));
	// AMMO1: one row per clip, an untouched count the last [orig: @0x564c7d..0x564ce4].
	const int ammo = rig.id("PRIMARY_AMMO1");
	CHECK(rig.menu.item_count(ammo) == 10 && rig.menu.selected_row(ammo) == 9);
	CHECK(rig.menu.item_text(ammo, 0) == "30 - AMMO_CAR15_556MM");
	// The readout: the M4 (5.5 + 10 * 1.5) and the three untouched grenades at their maxclips
	// (2 * 1.2 + 2 * 1 + 3 * 0.7): 27.0, Light; the M4's icon on PRIMARY_ICON.
	CHECK(rig.weight() == "Total Weight 27.0 lbs (Light)");
	CHECK(rig.screen.icons().slot[0] == "M_4.tga" && rig.screen.icons().slot[1].empty());
	CHECK(rig.host.last_icons == rig.screen.icons());
	// A count pick: row 0 is one clip, the readout follows [orig: UI_OnPrimaryAmmoCountSelected
	// @0x566620].
	rig.menu.select_row(ammo, 0, true);
	CHECK(rig.screen.selected_clips("PRIMARY") == 1);
	CHECK(rig.weight() == "Total Weight 13.5 lbs (Light)");
	// A weapon pick: NONE hides its ammo, clears its icon and re-weighs [orig: @0x5662d0, the
	// readout @0x5665f4].
	rig.menu.select_row(rig.id("PRIMARY"), 0, true);
	CHECK(rig.screen.selected_weapon("PRIMARY") == -1 && rig.screen.icons().slot[0].empty());
	CHECK(!rig.menu.is_widget_shown(ammo));
	CHECK(rig.weight() == "Total Weight 6.5 lbs (Light)");
}

void test_class_flip_and_page_memory() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	page_record(rig.host, 8, {entry("WPN_M4AUTO", 2)});
	rig.open();
	const int spin = rig.id("PLAYER_CLASS");
	const int ammo = rig.id("PRIMARY_AMMO1");
	CHECK(rig.menu.selected_row(ammo) == 1); // the page's two clips
	rig.menu.select_row(ammo, 0, true); // one clip
	// The sniper's flip re-filters (the M4 out, the sniper rifle in) [orig: @0x566f60].
	rig.menu.select_row(spin, 1, true);
	CHECK(rig.screen.selected_class() == 6 && !rig.has("PRIMARY", "WPN_M4AUTO") && rig.has("PRIMARY", "WPN_SNIPE"));
	// The engineer's empty page; the flip saved the rifleman page with its picked count.
	rig.menu.select_row(spin, 4, true);
	CHECK(rig.menu.selected_row(rig.id("PRIMARY")) == 0);
	int m4 = -2;
	for (const playersav::KitEntry &e : rig.screen.loadout().buffers.page(8).entries)
		if (e.name == "WPN_M4AUTO") m4 = e.ammo_primary;
	CHECK(m4 == 1);
	rig.menu.select_row(spin, 3, true); // the rifleman again: its saved page
	CHECK(rig.screen.selected_weapon("PRIMARY") == weapons.row("WPN_M4AUTO") && rig.menu.selected_row(ammo) == 0);
	// A show under the same team mask keeps the pages; another mask stages the profile again.
	rig.open();
	CHECK(rig.menu.selected_row(ammo) == 0);
	rig.host.team = 2;
	rig.open();
	rig.host.team = 1;
	rig.open();
	CHECK(rig.menu.selected_row(ammo) == 1);
	// The mission start's stage takes the profile page again too: a flip saves the picked count,
	// the stage puts the profile's back.
	rig.menu.select_row(ammo, 0, true);
	rig.menu.select_row(spin, 4, true);
	rig.screen.stage_mission_start();
	rig.open();
	CHECK(rig.screen.selected_class() == 8 && rig.menu.selected_row(ammo) == 1);
}

void test_class_mask_rows() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	rig.host.mask = (1u << 5) | (1u << 8); // medic and rifleman
	rig.open();
	const int spin = rig.id("PLAYER_CLASS");
	CHECK(rig.menu.is_item_enabled(spin, 0) && rig.menu.is_item_enabled(spin, 3));
	for (int row : {1, 2, 4}) CHECK(!rig.menu.is_item_enabled(spin, row));
	CHECK(rig.menu.selected_row(spin) == 3);
	// A class the mask refuses resolves to the next allowed one [orig: @0x5642f0].
	rig.host.cls = 6;
	rig.open();
	CHECK(rig.screen.selected_class() == 8 && rig.menu.selected_row(spin) == 3);
}

void test_banned_grenade_keeps_its_control() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	rig.host.allowed = [](const std::string &name) { return name == "WPN_GRENADEFB" ? 0 : 1; };
	rig.open();
	CHECK(rig.menu.item_count(rig.id("GRENADE_AMMO1")) == 1); // only its zero row
	CHECK(rig.menu.item_count(rig.id("GRENADE_AMMO2")) == 3 && rig.menu.item_count(rig.id("GRENADE_AMMO3")) == 4);
	rig.menu.select_row(rig.id("GRENADE_AMMO2"), 1, true);
	rig.screen.accept();
	std::vector<std::string> names;
	std::vector<int> counts;
	for (const playersav::KitEntry &e : rig.host.last_kit.entries)
		if (e.name.compare(0, 11, "WPN_GRENADE") == 0) {
			names.push_back(e.name);
			counts.push_back(e.ammo_primary);
		}
	CHECK(names == (std::vector<std::string>{"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM"}));
	CHECK(counts == (std::vector<int>{-1, 1, -1}));
}

void test_accept_and_cancel() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	page_record(rig.host, 8, {entry("WPN_M4AUTO")});
	rig.open();
	// The press latch holds: ACCEPT and CANCEL do nothing at all, the page not serialized
	// [orig: @0x565cf8 ahead of Input_ResetAllState @0x565d01 and the serialize @0x565d22].
	rig.host.latched = true;
	rig.menu.select_row(rig.id("PRIMARY_AMMO1"), 0, true);
	rig.menu.activate(rig.id("ACCEPT"));
	rig.screen.cancel();
	CHECK(rig.host.resets == 0 && rig.host.applied == 0 && rig.host.closes == 0);
	CHECK(rig.screen.loadout().buffers.page(8).entries.size() == 1);
	rig.host.latched = false;
	// ACCEPT (its click): the reset, the page (the blue knife first, the M4 with its one clip) applied
	// with the class, out of a session the viewmodel, then the close.
	rig.menu.activate(rig.id("ACCEPT"));
	CHECK(rig.host.resets == 1 && rig.host.applied == 1 && rig.host.presented == 1 && rig.host.closes == 1);
	CHECK(rig.host.last_class == 8 && rig.host.last_kit.entries.size() > 1);
	CHECK(rig.host.last_kit.entries[0].name == "WPN_KNIFE");
	CHECK(rig.host.last_kit.entries[1].name == "WPN_M4AUTO" && rig.host.last_kit.entries[1].ammo_primary == 1);
	CHECK(rig.screen.loadout().buffers.page(8).entries.size() == rig.host.last_kit.entries.size());
	CHECK(rig.screen.last_accepted().entries.size() == rig.host.last_kit.entries.size());
	// In a session the apply is its submit alone: no viewmodel; a refused apply presents nothing.
	rig.host.session = true;
	rig.screen.accept();
	CHECK(rig.host.applied == 2 && rig.host.presented == 1 && rig.host.closes == 2);
	rig.host.session = false;
	rig.host.applies = false;
	rig.screen.accept();
	CHECK(rig.host.applied == 3 && rig.host.presented == 1 && rig.host.closes == 3);
	// CANCEL (its click): the reset and the close, nothing applied.
	rig.menu.activate(rig.id("CANCEL"));
	CHECK(rig.host.resets == 4 && rig.host.applied == 3 && rig.host.closes == 4);
	// A closed screen takes neither.
	rig.screen.close(rig.menu);
	rig.screen.accept();
	CHECK(rig.host.resets == 4 && rig.host.closes == 4);
}

void test_empty_and_missing_tables() {
	// A weapon.def with no rows is a loaded, empty table: each list holds only NONE (D-MNU-27).
	Weapons empty(std::string("// Weapon definitions\r\n\r\n"));
	Rig rig(weapon_screen(true));
	rig.host.table_ = &empty.file;
	rig.open();
	for (const char *slot : {"PRIMARY", "SECONDARY", "ACCESSORY"}) CHECK(rig.items(slot).size() == 1);
	// No table at all: the lists stay empty, the screen still opens and closes.
	Rig none(weapon_screen(true));
	none.open();
	CHECK(none.menu.item_count(none.id("PRIMARY")) == 0 && none.menu.item_count(none.id("PLAYER_CLASS")) == 5);
	none.screen.cancel();
	CHECK(none.host.closes == 1);
}

// The authored WEAPON screen and the shipped weapon.def values: the M4 (5.5 + 10 * 1.5) with two
// satchels (2 * 17.6) and the grenades at zero is 55.7 lbs (Normal); one satchel 38.1 lbs.
void test_authored_weapon_mnu() {
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/game/weapon.mnu";
	mnu::Document doc;
	std::string error;
	CHECK(mnu::parse_file(path, doc, error));
	Weapons weapons(catalog());
	Rig rig(std::move(doc));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	page_record(rig.host, 8, {entry("WPN_M4AUTO"), entry("WPN_SATCHEL_CHARGE"), entry("WPN_GRENADEFB", 0),
			entry("WPN_GRENADEHE", 0), entry("WPN_GRENADESM", 0)});
	rig.open();
	CHECK(rig.menu.item_count(rig.id("PLAYER_CLASS")) == 5 && rig.menu.item_count(rig.id("PRIMARY")) > 1);
	CHECK(rig.weight() == "Total Weight 55.7 lbs (Normal)");
	const int satchel = rig.id("ACCESSORY_AMMO1");
	CHECK(rig.menu.selected_row(satchel) == 1);
	rig.menu.select_row(satchel, 0, true);
	CHECK(rig.weight() == "Total Weight 38.1 lbs (Normal)");
	CHECK(rig.menu.item_text(rig.id("GRENADE_AMMO1"), 0) == "0 - GRENADEFB");
	// The shipped WepDes round labels (gametext.bin WepDes/AMMO_CAR15_556MM, WepDes/GRENADEFB).
	rig.host.gametext_rows = {{"WepDes/AMMO_CAR15_556MM", "5.56x45"}, {"WepDes/GRENADEFB", "Flashbang"}};
	rig.open();
	CHECK(rig.menu.item_text(rig.id("PRIMARY_AMMO1"), 0) == "30 - 5.56x45");
	CHECK(rig.menu.item_text(rig.id("GRENADE_AMMO1"), 0) == "0 - Flashbang");
	CHECK(rig.menu.item_text(rig.id("GRENADE_AMMO1"), 1) == "1 - Flashbang");
}

// A DEATH screen of the three category lists and their icons (dfx2.exe's death.mnu authors no
// ammo, grenade or weight control).
mnu::Document death_screen() {
	std::string body;
	int y = 40;
	for (const char *name : {"PRIMARY", "SECONDARY", "ACCESSORY"}) {
		body += wnd("combobox", name, y);
		y += 24;
	}
	for (const char *name : {"PRIMARY_ICON", "SECONDARY_ICON", "ACCESSORY_ICON"}) {
		body += wnd("window", name, y);
		y += 24;
	}
	const std::string xml = "<SCREEN><NAME>DEATH</NAME><WINDOW type=\"window\" name=\"MAIN\"><POSITION>"
	                        "<LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" +
	                        body + "</WINDOW></SCREEN>";
	mnu::Document doc;
	std::string error;
	CHECK(mnu::parse(xml, doc, error));
	return doc;
}

// DFX2-41: the DEATH screen's picker runs the armory controller over that screen's own runtime
// (the WEAPON screen's claim untouched): the activation's fill resolves the class and lists the
// class's page under the title's lists (dfx2.exe's open with no NONE row, DFX2-73), a pick there
// takes the WEAPON screen's handler, and the close's submit past the press latch resets the input
// and applies the page the screen shows, closing nothing; with no table nothing fills and the
// submit applies an empty page.
void test_death_screen_picker() {
	Weapons weapons(catalog());
	Rig rig(weapon_screen(true));
	rig.host.table_ = &weapons.file;
	rig.host.cls = 8;
	page_record(rig.host, 8, {entry("WPN_M4AUTO"), entry("WPN_M9Beretta")});
	CHECK(!rig.screen.death_screen_loadout());
	Title dfx2;
	dfx2.death_screen_loadout = true;
	dfx2.armory_lists_none_row = false;
	rig.screen.set_title(&dfx2);
	CHECK(rig.screen.death_screen_loadout());
	const mnu::Document doc = death_screen();
	MenuRuntime death;
	CHECK(death.open_document(&doc, "death.mnu", "DEATH"));
	rig.screen.stage_mission_start();
	CHECK(rig.screen.populate_screen(death));
	CHECK(rig.screen.selected_class() == 8);
	CHECK(death.get_widget_items(death.widget_id("PRIMARY")) == (std::vector<std::string>{"WPN_M4AUTO"}));
	CHECK(rig.screen.selected_weapon_on(death, "PRIMARY") == weapons.row("WPN_M4AUTO"));
	CHECK(rig.screen.selected_weapon_on(death, "SECONDARY") == weapons.row("WPN_M9Beretta"));
	CHECK(!rig.screen.is_open() && rig.host.icon_calls == 0);
	CHECK(rig.screen.pick_screen(death, "SECONDARY"));
	// The close's submit: the latch holds it; past it the reset and the page, the class's own.
	rig.host.latched = true;
	CHECK(!rig.screen.submit_screen(death));
	CHECK(rig.host.resets == 0 && rig.host.applied == 0);
	rig.host.latched = false;
	CHECK(rig.screen.submit_screen(death));
	CHECK(rig.host.resets == 1 && rig.host.applied == 1 && rig.host.closes == 0);
	CHECK(rig.host.last_class == 8 && rig.host.last_kit.entries.size() > 2);
	CHECK(rig.host.last_kit.entries[0].name == "WPN_KNIFE" && rig.host.last_kit.entries[1].name == "WPN_M4AUTO");
	CHECK(rig.screen.loadout().buffers.page(8).entries.size() == rig.host.last_kit.entries.size());
	// No table: no fill, no pick; the submit applies an empty page.
	rig.host.table_ = nullptr;
	CHECK(!rig.screen.populate_screen(death) && !rig.screen.pick_screen(death, "PRIMARY"));
	CHECK(rig.screen.submit_screen(death));
	CHECK(rig.host.applied == 2 && rig.host.last_kit.entries.empty());
}

} // namespace

int main() {
	test_init_and_unclassed_show();
	test_classed_show_page_and_counts();
	test_class_flip_and_page_memory();
	test_class_mask_rows();
	test_banned_grenade_keeps_its_control();
	test_accept_and_cancel();
	test_empty_and_missing_tables();
	test_authored_weapon_mnu();
	test_death_screen_picker();
	if (failures) {
		std::printf("armory_screen: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("armory_screen OK\n");
	return 0;
}
