// The PLAYER_INFO screen's controller (runtime/inmatch/player_info_screen.h) driven headless over
// the menu runtime, the way the menu shell hands it the document and its events: the minted
// fixtures/avatars/synth_avatars.def, a weapon.def written here, and a fresh profile store.
//
// Pinned: the populate (the cascade's lists, the voice list by the head's sex, the authored side
// radio), the side radios' alignment filter and their save of the side they leave, the persisted
// sides restored, the snapshot's packed ids, the ACCEPT's save and hand-off, the name rule (one
// refused last character cut, a {hot} marker cut per set, the no-name flag cleared), the voice
// override per side (a pick writes the record, a fill that drops it resets it, a restore keeps it,
// the preview prefers it), the loadout (no table, an empty table's None page, the saved page
// restored and written back unchanged, a side switch and a class pick saving the page they leave,
// a class byte with no page), and the ACCEPT leaving the side not shown.
#include <formats/avatars/avatars.h>
#include <formats/def/def.h>
#include <formats/mnu/mnu.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/player_info_screen.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/profile/player_profiles.h>
#include <runtime/profile/profile_controls.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_paths.h"

using namespace opennova;
using inmatch::PlayerInfoScreen;
using inmatch::PlayerInfoSnapshot;
using menu::MenuEvent;
using menu::MenuRuntime;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

std::string wnd(const char *type, const std::string &name, int top, const std::string &inner = "",
		const char *attrs = "") {
	char position[160];
	std::snprintf(position, sizeof(position),
			"<POSITION><LEFT>10</LEFT><TOP>%d</TOP><RIGHT>250</RIGHT><BOTTOM>%d</BOTTOM></"
			"POSITION>",
			top, top + 20);
	return std::string("<WINDOW type=\"") + type + "\" name=\"" + name + "\"" + attrs + ">" +
			position + inner + "</WINDOW>";
}

std::string screen_xml(const std::string &body) {
	return "<SCREEN><NAME>PLAYER_INFO</NAME><WINDOW type=\"window\" "
		   "name=\"MAIN\"><POSITION><LEFT>0</LEFT>"
		   "<TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" +
			body + "</WINDOW></SCREEN>";
}

// The cascade combos, the side radios (SIDE_BLUE authored CHECKED, one GROUP), the name edit,
// ACCEPT and the voice preview: the control names player.mnu authors.
std::string avatar_body(bool red_checked = false) {
	std::string body;
	int y = 10;
	for (const char *name : { "NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE" }) {
		body += wnd("combobox", name, y);
		y += 24;
	}
	body += wnd("radio", "SIDE_BLUE", y, "<GROUP>1</GROUP>", red_checked ? "" : " CHECKED");
	body += wnd("radio", "SIDE_RED", y + 24, "<GROUP>1</GROUP>", red_checked ? " CHECKED" : "");
	body += wnd("edit", "PLAYERNAME", y + 48);
	body += wnd("button", "ACCEPT", y + 72);
	body += wnd("button", "TESTPLAYERVOICE", y + 96);
	return body;
}

// The three weapon combos, their ammo combos, PLAYERCLASS's authored values 5..9, the TYPE statics
// (FMJ / AP / SP, values 0 / 1 / 2), the grenades and the weight readout.
std::string loadout_body(int top) {
	const std::string type_items =
			"<LIST_BOX><ITEMS><ITEM value=\"0\">FMJ</ITEM><ITEM value=\"1\">AP</ITEM>"
			"<ITEM value=\"2\">SP</ITEM></ITEMS></LIST_BOX>";
	std::string body;
	int y = top;
	for (const char *name : { "PRIMARY", "SECONDARY", "ACCESSORY" }) {
		body += wnd("combobox", name, y);
		body += wnd("combobox", std::string(name) + "_AMMO1", y + 24);
		body += wnd("combobox", std::string(name) + "_AMMO2", y + 48);
		body += wnd("window", std::string(name) + "_ICON", y + 72);
		y += 100;
	}
	for (const char *name : { "PRIMARY", "SECONDARY" }) {
		body += wnd("combobox", std::string(name) + "_AMMO1_TYPE", y, type_items);
		y += 24;
	}
	for (const char *name : { "GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3" }) {
		body += wnd("combobox", name, y);
		y += 24;
	}
	body += wnd("static", "STATIC_TOTAL_WEIGHT", y);
	std::string classes;
	for (int v = 5; v <= 9; ++v)
		classes +=
				"<ITEM value=\"" + std::to_string(v) + "\">class " + std::to_string(v) + "</ITEM>";
	body += wnd("combobox", "PLAYERCLASS", y + 24,
			"<LIST_BOX><ITEMS>" + classes + "</ITEMS></LIST_BOX>");
	return body;
}

const char *kKitWeaponDef = R"(
weapon "WPN_P1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	primary
	round_type	"AMMO_P1"
	clipsize	30
	maxclips	5
	clipweight	1
	weaponweight	5
end

weapon "WPN_P2"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	primary
	round_type	"AMMO_P2"
	clipsize	20
	maxclips	4
	clipweight	1
	weaponweight	6
end

weapon "WPN_S1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	secondary
	round_type	"AMMO_S1"
	clipsize	7
	maxclips	3
	clipweight	1
	weaponweight	2
end

weapon "WPN_S2"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	secondary
	round_type	"AMMO_S2"
	clipsize	8
	maxclips	3
	clipweight	1
	weaponweight	2
end

weapon "WPN_A1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	accessory
	round_type	"AMMO_A1"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	3
end

weapon "WPN_G1"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	grenade
	round_type	"G1"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end

weapon "WPN_G2"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	grenade
	round_type	"G2"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end

weapon "WPN_G3"
	loadout_selectable	1
	teamfilter	blue
	teamfilter	red
	charfilter	medic
	charfilter	rifleman
	weapon_class	grenade
	round_type	"G3"
	clipsize	1
	maxclips	2
	clipweight	1
	weaponweight	1
end
)";

playersav::KitEntry entry(const char *name, int primary = -1, int secondary = -1, int flags = -1) {
	playersav::KitEntry out;
	out.name = name;
	out.ammo_primary = primary;
	out.ammo_secondary = secondary;
	out.flags = flags;
	return out;
}

std::vector<playersav::KitEntry> blue_kit() {
	return { entry("WPN_KNIFE"), entry("WPN_MEDPACK"), entry("WPN_P1", 3, -1, 1),
		entry("WPN_S1", 2, -1, 2), entry("WPN_A1", 1), entry("WPN_G1", 0), entry("WPN_G2", 1),
		entry("WPN_G3") };
}

std::vector<playersav::KitEntry> red_kit() {
	return { entry("WPN_KNIFE2"), entry("WPN_MEDPACK"), entry("WPN_P2", 2, -1, 0),
		entry("WPN_S2", 1, -1, 0), entry("None"), entry("WPN_G1"), entry("WPN_G2"),
		entry("WPN_G3") };
}

// A kit as its tokens, the page's strings.
std::vector<std::string> tokens(const std::vector<playersav::KitEntry> &kit) {
	std::vector<std::string> out;
	for (const playersav::KitEntry &e : kit) {
		out.push_back(e.name);
		out.push_back(std::to_string(e.ammo_primary));
		out.push_back(std::to_string(e.ammo_secondary));
		out.push_back(std::to_string(e.flags));
	}
	return out;
}

struct Host : inmatch::PlayerInfoScreenHost {
	std::vector<PlayerInfoSnapshot> chosen_;
	std::vector<std::string> voices;
	int opening_ = 0, opened_ = 0, closed_ = 0, previews = 0, loadouts = 0, refused = 0;
	void opening() override { ++opening_; }
	void opened() override { ++opened_; }
	void closed() override { ++closed_; }
	void preview_changed() override { ++previews; }
	void loadout_changed() override { ++loadouts; }
	void play_voice(const std::string &bank, const std::string &trigger) override {
		voices.push_back(bank + ":" + trigger);
	}
	void chosen(const PlayerInfoSnapshot &snapshot) override { chosen_.push_back(snapshot); }
	void save_refused() override { ++refused; }
};

// The process's tables: the minted avatar table, the kit weapon.def and the profile store.
struct Tables {
	avatars::AvatarsFile avatars{};
	def::DefWeaponsFile weapons{};
	def::DefWeaponsFile empty_weapons{};
	profile::PlayerProfiles store;
	bool ok = false;

	Tables() {
		const std::string root = test_paths_repo_root(__FILE__);
		ok = avatars::avatars_parse(
					 (root + "/fixtures/avatars/synth_avatars.def").c_str(), &avatars) == 0;
		// weapon.def's lines end CRLF, as the shipped table's do (TestFs.crlf's form).
		std::string text;
		for (const char *c = kKitWeaponDef; *c != '\0'; ++c) {
			if (*c == '\n')
				text += '\r';
			text += *c;
		}
		ok = ok &&
				def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()),
						text.size(), &weapons) == 0 &&
				weapons.count == 8;
		reset();
	}
	~Tables() {
		avatars::avatars_free(&avatars);
		def::def_free_weapons(&weapons);
	}
	Tables(const Tables &) = delete;
	Tables &operator=(const Tables &) = delete;

	// A fresh record, both sides on the medic class with each side's first character (N00/D00 and
	// N04/D00, combo id 1), the voice pair on DEFAULT_VOICE and the medic pages the given kits.
	void reset(const std::vector<playersav::KitEntry> &blue = {},
			const std::vector<playersav::KitEntry> &red = {}) {
		profile::PlayerProfiles::LoadInput input;
		profile::ProfileDefaults defaults;
		defaults.bindings = profile::default_binding_table();
		store.load(input, defaults);
		for (int side = 0; side < 2; ++side) {
			profile::CharacterSelection selection;
			selection.team = side;
			selection.player_class = 5;
			selection.character = side == 0
					? profile::CharacterSelection::Character{ 0, 0, 0x0200 }
					: profile::CharacterSelection::Character{ 4, 0, 0x8204 };
			profile::CharacterSelection::Kit kit;
			kit.entries = side == 0 ? blue : red;
			selection.kit = kit;
			CHECK(store.apply_character_selection(selection));
		}
		profile::set_voice_override(store.current(), 0, 0);
		profile::set_voice_override(store.current(), 1, 0);
	}
	std::vector<std::string> page(int side, int player_class) const {
		const playersav::Side &s = store.current_weapons().side(
				side == 0 ? playersav::SideId::Blue : playersav::SideId::Red);
		const playersav::KitPage *p = s.page_for_class(static_cast<uint8_t>(player_class));
		return p != nullptr ? tokens(p->entries) : std::vector<std::string>();
	}
};

// One open of the screen over a document, the shell's forwarding of the runtime's events.
struct Open {
	mnu::Document doc;
	MenuRuntime menu;
	PlayerInfoScreen &screen;
	bool ok = false;

	Open(PlayerInfoScreen &s, const std::string &body) : screen(s) {
		std::string error;
		ok = mnu::parse(screen_xml(body), doc, error) &&
				menu.open_document(&doc, "player.mnu", "PLAYER_INFO");
		menu.set_sink([this](const MenuEvent &event) { screen.on_event(menu, event); });
		if (ok)
			screen.open(menu, "player.mnu", "PLAYER_INFO");
	}
	~Open() {
		menu.set_sink(nullptr);
		if (screen.is_open())
			screen.close(menu);
	}
	int id(const char *name) const { return menu.widget_id(name); }
	void pick(const char *name, int row) { menu.select_row(id(name), row, true); }
	void click(const char *name) { menu.activate(id(name)); }
	std::string selected_text(const char *name) const {
		const int row = menu.selected_row(id(name));
		return row >= 0 ? menu.item_text(id(name), row) : std::string();
	}
	void type(const char *name, const std::string &text) {
		MenuEvent event;
		event.kind = MenuEvent::Kind::ValueChanged;
		event.text = name;
		event.text2 = "edit";
		event.text3 = text;
		screen.on_event(menu, event);
	}
};

void wire(PlayerInfoScreen &screen, Tables &tables, Host &host, bool weapons = false) {
	screen.set_host(&host);
	screen.set_avatars(&tables.avatars);
	screen.set_profiles(&tables.store);
	screen.set_weapons(weapons ? &tables.weapons : nullptr, weapons);
}

void test_populate_and_the_side_filter() {
	Tables tables;
	CHECK(tables.ok);
	if (!tables.ok)
		return;
	const inmatch::CharacterRegistry registry =
			inmatch::CharacterRegistry::from_file(tables.avatars);
	PlayerInfoScreen screen;
	Host host;
	wire(screen, tables, host);
	{
		Open open(screen, avatar_body());
		CHECK(open.ok && host.opening_ == 1 && host.opened_ == 1 && host.previews > 0);
		CHECK(open.menu.item_count(open.id("NATIONALITY")) > 0);
		// The team-0 cascade selects the first good nationality (N00) and its first division (D00).
		CHECK(open.menu.item_count(open.id("DIVISION")) == 4);
		CHECK(open.menu.item_count(open.id("COMBO_LIST")) == 4);
		// DEFAULT_VOICE and the seven enabled male rows (1..6, 10): the disabled id-9 row is
		// skipped.
		const int voices = open.id("PLAYERVOICE");
		CHECK(open.menu.item_count(voices) == 8);
		CHECK(open.menu.item_text(voices, 0) == "Default" &&
				open.menu.item_text(voices, 7) == "Voice 10");
		// The female head's division swaps the set.
		open.pick("DIVISION", 3);
		open.pick("COMBO_LIST", 1);
		CHECK(open.menu.item_count(voices) == 4 && open.menu.item_text(voices, 1) == "Voice 7" &&
				open.menu.item_text(voices, 3) == "Voice 11");
		CHECK(screen.preview_combo() == 1);
		// The radios filter by alignment, the two sides partitioning the nationalities
		// (D-PLAYERINFO-5).
		const std::vector<int32_t> good = screen.nationality_rows();
		for (const int32_t n : good)
			CHECK(tables.avatars.nationalities[n].alignment == avatars::AVATAR_ALIGN_GOOD);
		open.click("SIDE_RED");
		CHECK(screen.team() == 1);
		for (const int32_t n : screen.nationality_rows())
			CHECK(tables.avatars.nationalities[n].alignment == avatars::AVATAR_ALIGN_EVIL);
		CHECK(good.size() + screen.nationality_rows().size() == tables.avatars.nationalities_count);
	}
	CHECK(host.closed_ == 1 && !screen.is_open());
	(void)registry;
	// The authored SIDE_RED radio picks the first side when the profile names none.
	PlayerInfoScreen red;
	Host red_host;
	wire(red, tables, red_host);
	Open red_open(red, avatar_body(true));
	CHECK(red.team() == 1);
	// Without the avatar table the lists stay empty.
	PlayerInfoScreen bare;
	Host bare_host;
	bare.set_host(&bare_host);
	bare.set_profiles(&tables.store);
	Open bare_open(bare, avatar_body());
	CHECK(bare_open.ok && bare_open.menu.item_count(bare_open.id("NATIONALITY")) == 0);
}

void test_snapshot_persisted_sides_and_accept() {
	Tables tables;
	if (!tables.ok)
		return;
	const inmatch::CharacterRegistry registry =
			inmatch::CharacterRegistry::from_file(tables.avatars);
	PlayerInfoScreen screen;
	Host host;
	wire(screen, tables, host);
	{
		Open open(screen, avatar_body());
		open.menu.set_widget_text(open.id("PLAYERNAME"), "Ghost");
		const PlayerInfoSnapshot snap = screen.snapshot();
		CHECK(snap.name == "Ghost" && snap.team == 0 && snap.nationality == 0);
		CHECK(snap.sides[0] && snap.sides[0]->avatar_packed == 0x0200);
		CHECK(snap.sides[1] && snap.sides[1]->avatar_packed == 0x8204);
		CHECK(!snap.has_loadout && snap.player_class == 8);
		// The ACCEPT saves the dialog into the record, then hands the selection on.
		open.menu.set_widget_text(open.id("PLAYERNAME"), "Sandman");
		open.click("ACCEPT");
		CHECK(host.chosen_.size() == 1 && host.chosen_[0].name == "Sandman" &&
				host.chosen_[0].nationality == 0);
		CHECK(tables.store.current().name == "Sandman");
		CHECK(tables.store.current_weapons().side(playersav::SideId::Blue).avatar_packed == 0x0200);
	}
	// The persisted sides restore each side's cascade; the edit shows the record's name.
	const inmatch::CharacterEntry *blue = registry.find_by_packed_id(0x0400, 0);
	const inmatch::CharacterEntry *red = registry.find_by_packed_id(0x8407, 1);
	CHECK(blue != nullptr && red != nullptr);
	if (blue == nullptr || red == nullptr)
		return;
	CHECK(tables.store.rename("Persistent"));
	std::optional<inmatch::PlayerInfoSideSelection> sides[2];
	sides[0] = inmatch::PlayerInfoSideSelection{ 0, blue->nationality_index, blue->division_index,
		blue->combo_index, 8, 0, 0, 0x0400 };
	sides[1] = inmatch::PlayerInfoSideSelection{ 1, red->nationality_index, red->division_index,
		red->combo_index, 8, 7, 0, 0x8407 };
	PlayerInfoScreen restored;
	Host restored_host;
	wire(restored, tables, restored_host);
	restored.set_persisted(0, std::nullopt, sides);
	Open open(restored, avatar_body());
	CHECK(open.menu.selected_row(open.id("COMBO_LIST")) == blue->combo_index);
	CHECK(open.menu.get_widget_text(open.id("PLAYERNAME")) == "Persistent");
	open.click("SIDE_RED");
	const PlayerInfoSnapshot snap = restored.snapshot();
	CHECK(snap.nationality == red->nationality_index && snap.division == red->division_index);
	CHECK(open.menu.selected_row(open.id("COMBO_LIST")) == red->combo_index);
	CHECK(snap.sides[0] && snap.sides[0]->avatar_packed == 0x0400);
	CHECK(snap.sides[1] && snap.sides[1]->avatar_packed == 0x8407);
}

// The populate's set of PLAYERNAME and its change notice: one refused last character comes off
// the edit and the record keeps its name; each set cuts one {hot} marker; a kept name, or none,
// clears the no-name flag [orig: PlayerInfo_HandlePlayerNameChange @0x55fbb0].
void test_the_name_rule() {
	Tables tables;
	if (!tables.ok)
		return;
	PlayerInfoScreen screen;
	Host host;
	wire(screen, tables, host);
	tables.store.commit_name("Bob!?");
	{
		Open open(screen, avatar_body());
		CHECK(open.menu.get_widget_text(open.id("PLAYERNAME")) == "Bob!");
		CHECK(tables.store.current().name == "Bob!?");
		// A side radio saves the edit's text through the ACCEPT's leg, then populates again.
		open.menu.set_widget_text(open.id("PLAYERNAME"), "Ann#");
		open.click("SIDE_RED");
		CHECK(open.menu.get_widget_text(open.id("PLAYERNAME")) == "Ann");
		CHECK(tables.store.current().name == "Ann#");
	}
	tables.store.commit_name("A{hot}B{hot}C");
	{
		Open open(screen, avatar_body());
		CHECK(open.menu.get_widget_text(open.id("PLAYERNAME")) == "AB{hot}C");
		CHECK(tables.store.current().name == "AB{hot}C");
		open.type("PLAYERNAME", "AB{hot}C!");
		CHECK(open.menu.get_widget_text(open.id("PLAYERNAME")) == "ABC");
		CHECK(tables.store.current().name == "AB{hot}C");
	}
	tables.store.commit_name(" Ghost");
	CHECK((tables.store.current().flags & playersav::kRecordFlagNoName) != 0);
	{
		Open open(screen, avatar_body());
		CHECK(open.menu.get_widget_text(open.id("PLAYERNAME")).empty());
		CHECK((tables.store.current().flags & playersav::kRecordFlagNoName) == 0);
	}
}

// The voice override: a pick writes the shown side's byte at once, a fill whose rows drop it resets
// it, each side keeps its own, a side's restore keeps a voice the first rows lack, a persisted one
// selects its row by value, and the preview prefers it over the head's own.
void test_the_voice_override() {
	Tables tables;
	if (!tables.ok)
		return;
	PlayerInfoScreen screen;
	Host host;
	wire(screen, tables, host);
	{
		Open open(screen, avatar_body());
		const int voices = open.id("PLAYERVOICE");
		CHECK(screen.selected_voice() == 0);
		open.pick("PLAYERVOICE", 3);
		CHECK(screen.selected_voice() == 3 &&
				profile::voice_override(tables.store.current(), 0) == 3);
		open.pick("DIVISION", 3);
		open.pick("COMBO_LIST", 1);
		CHECK(screen.selected_voice() == 0 && open.menu.selected_row(voices) == 0);
		// Each side its own byte.
		open.click("SIDE_RED");
		CHECK(open.menu.item_text(voices, 1) == "Voice 7");
		open.pick("PLAYERVOICE", 1);
		CHECK(profile::voice_override(tables.store.current(), 1) == 7 &&
				profile::voice_override(tables.store.current(), 0) == 0);
		open.click("SIDE_BLUE");
		open.pick("DIVISION", 3);
		open.pick("COMBO_LIST", 1);
		open.pick("PLAYERVOICE", 1);
		CHECK(profile::voice_override(tables.store.current(), 0) == 7);
		// A side round trip keeps blue's female voice although blue's first head is male.
		open.click("SIDE_RED");
		open.click("SIDE_BLUE");
		CHECK(profile::voice_override(tables.store.current(), 0) == 7);
		CHECK(open.selected_text("PLAYERVOICE") == "Voice 7");
	}
	{
		// A fresh build keeps it too.
		Open open(screen, avatar_body());
		CHECK(open.menu.selected_row(open.id("COMBO_LIST")) == 1 &&
				open.selected_text("PLAYERVOICE") == "Voice 7");
	}
	tables.reset();
	profile::set_voice_override(tables.store.current(), 0, 10);
	PlayerInfoScreen fresh;
	Host fresh_host;
	wire(fresh, tables, fresh_host);
	Open open(fresh, avatar_body());
	CHECK(fresh.selected_voice() == 10 && open.selected_text("PLAYERVOICE") == "Voice 10");
	profile::set_voice_override(tables.store.current(), 0, 6);
	open.click("TESTPLAYERVOICE");
	open.pick("PLAYERVOICE", 0);
	open.click("TESTPLAYERVOICE");
	CHECK(fresh_host.voices ==
			(std::vector<std::string>{ "menu.lwf:VOICE_6", "menu.lwf:VOICE_1" }));
}

void test_the_loadout() {
	Tables tables;
	if (!tables.ok)
		return;
	const std::string body = avatar_body() + loadout_body(260);
	// No table: the class still snapshots, no loadout key.
	{
		PlayerInfoScreen screen;
		Host host;
		wire(screen, tables, host);
		Open open(screen, loadout_body(10));
		const PlayerInfoSnapshot snap = screen.snapshot();
		CHECK(snap.player_class == 5 && !snap.has_loadout && snap.kit.empty());
	}
	// An empty table (a missing weapon.def is a loaded, empty one, D-MNU-27): each list shows only
	// NONE and the medic's page names the catalog's seeded "None" row for every NONE category and
	// empty grenade slot, so the profile takes it [orig: WeaponDef_LoadAll @0x54dd10 seeds "None"
	// (@0x54dd45) and File_ParseASCIIFile @0x53d823 returns silently;
	// PlayerInfo_PopulateWeaponSlotLists @0x560571..0x5605ab; PlayerInfo_SerializeWeaponLoadout
	// names g_WeaponDefTable + 192 * row @0x55e6f0 / @0x55e7ee].
	{
		PlayerInfoScreen screen;
		Host host;
		wire(screen, tables, host);
		screen.set_weapons(&tables.empty_weapons, true);
		Open open(screen, loadout_body(10));
		for (const char *slot : { "PRIMARY", "SECONDARY", "ACCESSORY" })
			CHECK(open.menu.item_count(open.id(slot)) == 1);
		std::vector<std::string> names;
		for (const playersav::KitEntry &e : screen.snapshot().kit)
			names.push_back(e.name);
		CHECK(names ==
				(std::vector<std::string>{ "WPN_KNIFE", "WPN_MEDPACK", "None", "None", "None",
						"None", "None", "None" }));
	}
	// The saved page restores the lists, the counts, the type bytes and the grenades, and the
	// ACCEPT writes it back unchanged.
	tables.reset(blue_kit(), red_kit());
	const std::vector<std::string> saved = tables.page(0, 5);
	CHECK(saved == tokens(blue_kit()));
	{
		PlayerInfoScreen screen;
		Host host;
		wire(screen, tables, host, true);
		Open open(screen, body);
		CHECK(open.selected_text("PRIMARY") == "WPN_P1" &&
				open.selected_text("SECONDARY") == "WPN_S1" &&
				open.selected_text("ACCESSORY") == "WPN_A1");
		CHECK(open.menu.selected_row(open.id("PRIMARY_AMMO1")) == 2 &&
				open.menu.selected_row(open.id("SECONDARY_AMMO1")) == 1 &&
				open.menu.selected_row(open.id("ACCESSORY_AMMO1")) == 0);
		CHECK(screen.selected_ammo_type("PRIMARY") == 1 &&
				screen.selected_ammo_type("SECONDARY") == 2);
		CHECK(open.menu.selected_row(open.id("GRENADE_AMMO1")) == 0 &&
				open.menu.selected_row(open.id("GRENADE_AMMO2")) == 1 &&
				open.menu.selected_row(open.id("GRENADE_AMMO3")) == 2);
		CHECK(!open.menu.get_widget_text(open.id("STATIC_TOTAL_WEIGHT")).empty() &&
				host.loadouts > 0);
		open.menu.set_widget_text(open.id("PLAYERNAME"), "Ghost");
		open.click("ACCEPT");
		CHECK(host.chosen_.size() == 1 && tokens(host.chosen_[0].kit) == saved);
		CHECK(tables.store.current().name == "Ghost" && tables.page(0, 5) == saved);
	}
	// A side radio saves the side it leaves (the shown side's click too), then restores the
	// other's.
	{
		PlayerInfoScreen screen;
		Host host;
		wire(screen, tables, host, true);
		Open open(screen, body);
		open.pick("SECONDARY", 2); // WPN_S2
		open.pick("SECONDARY_AMMO1", 0); // one clip
		open.click("SIDE_BLUE");
		const std::vector<std::string> blue = tables.page(0, 5);
		CHECK(blue.size() >= 16 &&
				std::vector<std::string>(blue.begin() + 12, blue.begin() + 16) ==
						(std::vector<std::string>{ "WPN_S2", "1", "-1", "2" }));
		CHECK(open.selected_text("SECONDARY") == "WPN_S2");
		open.click("SIDE_RED");
		CHECK(screen.team() == 1 && open.selected_text("PRIMARY") == "WPN_P2" &&
				open.selected_text("ACCESSORY") == "None");
		CHECK(open.menu.selected_row(open.id("PRIMARY_AMMO1")) == 1);
		open.pick("PRIMARY", 1); // WPN_P1 on red
		open.click("SIDE_BLUE");
		const std::vector<std::string> red = tables.page(1, 5);
		CHECK(red.size() >= 12 &&
				std::vector<std::string>(red.begin() + 8, red.begin() + 12) ==
						(std::vector<std::string>{ "WPN_P1", "-1", "-1", "0" }));
		CHECK(open.selected_text("SECONDARY") == "WPN_S2" &&
				open.selected_text("PRIMARY") == "WPN_P1");
	}
	// A class pick saves the page of the class it leaves (its medpack follows the new class byte,
	// so it drops), then restores the picked class's page; picking it back restores the edit.
	tables.reset(blue_kit(), red_kit());
	{
		PlayerInfoScreen screen;
		Host host;
		wire(screen, tables, host, true);
		Open open(screen, body);
		open.pick("PRIMARY", 2); // WPN_P2
		open.pick("PLAYERCLASS", 3); // class 8
		const std::vector<std::string> medic = tables.page(0, 5);
		CHECK(medic.size() >= 8 &&
				std::vector<std::string>(medic.begin(), medic.begin() + 8) ==
						(std::vector<std::string>{
								"WPN_KNIFE", "-1", "-1", "-1", "WPN_P2", "-1", "-1", "1" }));
		CHECK(open.selected_text("PRIMARY") == "None");
		open.pick("PLAYERCLASS", 0);
		CHECK(open.selected_text("PRIMARY") == "WPN_P2" &&
				open.selected_text("SECONDARY") == "WPN_S1");
	}
	// A class byte that picks no page restores nothing, and the save still writes the class the
	// screen shows to both sides and the kit into that class's page (D-PLAYERINFO-26).
	tables.reset();
	tables.store.current_weapons().blue.player_class = 3;
	tables.store.current_weapons().red.player_class = 3;
	{
		PlayerInfoScreen screen;
		Host host;
		wire(screen, tables, host, true);
		Open open(screen, body);
		CHECK(open.selected_text("PLAYERCLASS") == "class 5" &&
				open.selected_text("PRIMARY") == "None");
		open.pick("PRIMARY", 1); // WPN_P1
		open.click("ACCEPT");
		CHECK(tables.store.current_weapons().blue.player_class == 5 &&
				tables.store.current_weapons().red.player_class == 5);
		const std::vector<std::string> page = tables.page(0, 5);
		CHECK(page.size() >= 12 &&
				std::vector<std::string>(page.begin(), page.begin() + 12) ==
						(std::vector<std::string>{ "WPN_KNIFE", "-1", "-1", "-1", "WPN_MEDPACK",
								"-1", "-1", "-1", "WPN_P1", "-1", "-1", "0" }));
	}
}

// The ACCEPT on blue writes blue's character and leaves red's bytes, the class to both sides.
void test_accept_leaves_the_side_not_shown() {
	Tables tables;
	if (!tables.ok)
		return;
	const uint16_t stale = 0x8000 | (63 << 9) | (15 << 5) | 30;
	profile::CharacterSelection selection;
	selection.team = 1;
	selection.player_class = 5;
	selection.character = profile::CharacterSelection::Character{ 30, 15, stale };
	CHECK(tables.store.apply_character_selection(selection));
	tables.store.current_weapons().red.player_class = 9;
	std::optional<inmatch::PlayerInfoSideSelection> none[2];
	PlayerInfoScreen screen;
	Host host;
	wire(screen, tables, host);
	// The profile the record gives names the medic class (the reset's) and no side the table
	// resolves.
	screen.set_persisted(std::nullopt, 5, none);
	Open open(screen, avatar_body());
	const PlayerInfoSnapshot snap = screen.snapshot();
	CHECK(snap.sides[1] && snap.sides[1]->avatar_packed == 0x8204);
	open.click("ACCEPT");
	const playersav::Record &record = tables.store.current_weapons();
	CHECK(record.red.avatar_a == 30 && record.red.avatar_b == 15 &&
			record.red.avatar_packed == stale);
	CHECK(snap.sides[0] && record.blue.avatar_packed == snap.sides[0]->avatar_packed);
	CHECK(record.blue.player_class == 5 && record.red.player_class == 5);
}

} // namespace

int main() {
	test_populate_and_the_side_filter();
	test_snapshot_persisted_sides_and_accept();
	test_the_name_rule();
	test_the_voice_override();
	test_the_loadout();
	test_accept_leaves_the_side_not_shown();
	if (failures == 0)
		std::printf("player_info_screen OK\n");
	return failures == 0 ? 0 : 1;
}
