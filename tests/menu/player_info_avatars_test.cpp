#include <base/crt/crt_rng.h>
#include <runtime/menu/player_info_avatars.h>
#include <runtime/menu/menu_runtime.h>

#include <cstring>
#include <iostream>

using namespace opennova;
using namespace opennova::menu;
using Change = PlayerInfoAvatars::Change;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << __func__ << ':' << __LINE__ << ": " #c "\n"; ++failures; } } while (false)

mnu::Document document(std::initializer_list<const char *> names) {
	mnu::Document doc;
	mnu::Screen screen;
	screen.name = "PLAYER_INFO";
	mnu::Window root;
	root.name = "ROOT";
	for (const char *name : names) {
		mnu::Window w;
		w.name = name;
		w.type = mnu::WindowType::Combo;
		root.children.push_back(w);
	}
	screen.roots.push_back(root);
	doc.screens.push_back(screen);
	return doc;
}
struct Avatars {
	avatars::AvatarNationality nations[3]{};
	avatars::AvatarDivision divisions[2]{};
	avatars::AvatarCombo combos[2]{};
	avatars::AvatarsFile file{};
	Avatars() {
		file.nationalities = nations; file.nationalities_count = 3;
		nations[0].alignment = 1;
		std::strcpy(nations[0].name_key, "N_EVIL");
		std::strcpy(nations[1].name_key, "N_GOOD");
		std::strcpy(nations[2].name_key, "N_EMPTY");
		nations[1].divisions = divisions; nations[1].divisions_count = 2;
		std::strcpy(divisions[0].name_key, "D_ZERO");
		std::strcpy(divisions[1].name_key, "D_ONE");
		divisions[0].combos = combos; divisions[0].combos_count = 2;
		divisions[1].combos = combos + 1; divisions[1].combos_count = 1;
		std::strcpy(combos[0].head.display_name, "HEAD_M");
		std::strcpy(combos[0].body.display_name, "BODY");
		std::strcpy(combos[1].head.display_name, "HEAD_F");
		std::strcpy(combos[1].body.display_name, "BODY_F");
		combos[0].head.voice = 4;
		combos[1].head.sex = 1; combos[1].head.voice = 8;
	}
};
const hud::GameTextLookup gameui = [](const char *section, const char *key, const char *fallback) {
	if (std::string(section) != "Avatars") return std::string(fallback);
	const std::string k(key);
	if (k == "N_GOOD") return std::string("Good");
	if (k == "HEAD_M") return std::string("Head");
	if (k == "BODY") return std::string("Body");
	if (k == "DEFAULT_VOICE") return std::string("UI default");
	if (k == "CHARVOICE_1") return std::string("UI one");
	return std::string(fallback);
};
const hud::GameTextLookup menutxt = [](const char *section, const char *key, const char *fallback) {
	return std::string(section) == "Menu" && std::string(key) == "CHARVOICE_1"
			? std::string("Menu one") : std::string(fallback);
};

void test_cascade_and_voice() {
	Avatars data;
	const auto doc = document({"NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE"});
	MenuRuntime menu; menu.open_document(&doc, "player.mnu", "PLAYER_INFO");
	PlayerInfoAvatars state;
	int events = 0;
	menu.set_sink([&](const MenuEvent &event) { if (event.kind == MenuEvent::Kind::ValueChanged) ++events; });
	CHECK(state.update(menu, data.file, Change::Team, 0, 0, 8, gameui, menutxt) == 0);
	CHECK(state.nationalities == (std::vector<int32_t>{1, 2}));
	CHECK(state.nationality == 1 && state.division == 0 && state.preview_changed);
	CHECK(menu.get_widget_items(menu.widget_id("NATIONALITY")) == (std::vector<std::string>{"Good", "N_EMPTY"}));
	CHECK(menu.get_widget_items(menu.widget_id("DIVISION")) == (std::vector<std::string>{"D_ZERO", "D_ONE"}));
	const int combo = menu.widget_id("COMBO_LIST"), voice = menu.widget_id("PLAYERVOICE");
	CHECK(menu.get_widget_items(combo) == (std::vector<std::string>{"Head - Body", "HEAD_F - BODY_F"}));
	CHECK(state.voices == (std::vector<int32_t>{0, 1, 2, 3, 4, 5, 6, 10}));
	CHECK(menu.item_text(voice, 0) == "UI default" && menu.item_text(voice, 1) == "Menu one");
	CHECK(menu.item_text(voice, 2) == "Voice 2" && menu.selected_row(voice) == 0);
	CHECK(state.voice_preview_trigger(menu, data.file, 0) == "VOICE_4");
	CHECK(state.voice_preview_trigger(menu, data.file, 99) == "VOICE_99");
	menu.select_row(combo, 1, false);
	CHECK(state.update(menu, data.file, Change::Combo, 0, 0, 8, gameui, menutxt) == 8);
	CHECK(state.voices == (std::vector<int32_t>{0, 7, 8, 11}));
	CHECK(menu.selected_row(voice) == 2 && state.preview_changed);
	CHECK(state.voice_preview_trigger(menu, data.file, 0) == "VOICE_8");
	CHECK(state.update(menu, data.file, Change::Voice, 1, 0, 8, gameui, menutxt) == 7);
	CHECK(!state.preview_changed && menu.selected_row(voice) == 2); // handler stores only; no refill/reselect
	CHECK(state.update(menu, data.file, Change::Voice, 9, 0, 8, gameui, menutxt) == 0);
	CHECK(state.update(menu, data.file, Change::Division, 1, 0, 7, gameui, menutxt) == 7);
	CHECK(menu.item_count(combo) == 1 && menu.selected_row(combo) == 0);
	CHECK(state.selected_combo(menu, data.file) == data.combos + 1);
	CHECK(state.update(menu, data.file, Change::Nationality, 0, 0, 7, gameui, menutxt) == 0);
	CHECK(state.division == 0 && menu.item_count(combo) == 2); // nationality resets division
	menu.select_row(combo, -1, false);
	CHECK(state.selected_combo(menu, data.file) == data.combos);
	CHECK(state.voice_preview_trigger(menu, data.file, 0) == "VOICE_4"); // voice fallback reads first combo
	CHECK(events == 0);
}

void test_missing_stages_and_invalid_selections() {
	Avatars data;
	const auto complete = document({"NATIONALITY", "DIVISION", "COMBO_LIST", "PLAYERVOICE"});
	const auto no_div = document({"NATIONALITY", "COMBO_LIST", "PLAYERVOICE"});
	const auto no_combo = document({"NATIONALITY", "DIVISION", "PLAYERVOICE"});
	const auto no_voice = document({"NATIONALITY", "DIVISION", "COMBO_LIST"});
	const auto no_nat = document({"DIVISION", "COMBO_LIST", "PLAYERVOICE"});
	MenuRuntime menu; menu.open_document(&complete, "player.mnu", "PLAYER_INFO");
	PlayerInfoAvatars state;
	state.update(menu, data.file, Change::Team, 0, 0, 3, gameui, menutxt);
	CHECK(state.update(menu, data.file, Change::Nationality, 1, 0, 3, gameui, menutxt) == 3);
	CHECK(state.nationality == 2 && state.division == -1 && state.preview_changed);
	CHECK(menu.item_count(menu.widget_id("COMBO_LIST")) == 0);
	CHECK(state.voice_preview_trigger(menu, data.file, 0).empty());
	state.update(menu, data.file, Change::Nationality, 99, 0, 3, gameui, menutxt);
	CHECK(state.nationality == -1 && state.division == -1);
	state.update(menu, data.file, Change::Team, 0, -2, 3, gameui, menutxt);
	CHECK(state.nationalities == (std::vector<int32_t>{0})); // nonzero team, not just team == 1
	state.update(menu, data.file, Change::Team, 0, 0, 3, gameui, menutxt);
	state.update(menu, data.file, Change::Division, 999, 0, 3, gameui, menutxt);
	CHECK(state.division == 999 && state.selected_combo(menu, data.file) == nullptr);
	// A list the screen lacks leaves the record's byte, and the next stage fills from it
	// [orig: PlayerInfo_PopulateAllControls @0x560802 / @0x560865; @0x55eab2 / @0x55eb15 (dfx2.exe)]:
	// no nationality list, the record's nationality's divisions (the list's first row picked).
	menu.open_document(&no_nat, "partial.mnu", "PLAYER_INFO");
	state.seed_record(1, 1);
	CHECK(state.update(menu, data.file, Change::Team, 0, 1, 7, gameui, menutxt) == 0);
	CHECK(state.nationalities.empty() && state.nationality == 1 && state.division == 0);
	CHECK(menu.get_widget_items(menu.widget_id("DIVISION")) == (std::vector<std::string>{"D_ZERO", "D_ONE"}));
	CHECK(menu.item_count(menu.widget_id("COMBO_LIST")) == 2 && state.preview_changed);
	// No division list: the record's division's combos (a female head: her voices keep 7).
	menu.open_document(&no_div, "partial.mnu", "PLAYER_INFO");
	state.seed_record(-1, 1);
	CHECK(state.update(menu, data.file, Change::Team, 0, 0, 7, gameui, menutxt) == 7);
	CHECK(state.nationality == 1 && state.division == 1 && state.preview_changed);
	CHECK(menu.get_widget_items(menu.widget_id("COMBO_LIST")) == (std::vector<std::string>{"HEAD_F - BODY_F"}));
	state.seed_record(-1, 999);
	CHECK(state.update(menu, data.file, Change::Team, 0, 1, 7, gameui, menutxt) == 0);
	CHECK(state.nationality == 0 && state.division == 999 && state.preview_changed);
	CHECK(menu.item_count(menu.widget_id("COMBO_LIST")) == 0);
	menu.open_document(&no_combo, "partial.mnu", "PLAYER_INFO");
	CHECK(state.update(menu, data.file, Change::Team, 0, 0, 7, gameui, menutxt) == 7);
	CHECK(state.division == 0 && !state.preview_changed);
	menu.open_document(&no_voice, "partial.mnu", "PLAYER_INFO");
	CHECK(state.update(menu, data.file, Change::Team, 0, 0, 7, gameui, menutxt) == 7);
	CHECK(state.preview_changed); // absent voice control retains the saved override
	menu.open_document(&complete, "player.mnu", "PLAYER_INFO");
	CHECK(state.update(menu, {}, Change::Team, 0, 0, 7, gameui, menutxt) == 0);
	CHECK(state.nationality == -1 && state.division == -1 && state.preview_changed);
	CHECK(menu.item_count(menu.widget_id("NATIONALITY")) == 0);
}

// dfx2.exe's PLAYER_INFO: a COMBO_LIST spin list and neither NATIONALITY nor DIVISION. The
// combos are the record's nationality and division's [orig: PlayerInfo_PopulateAllControls
// @0x55e9a0 - PlayerInfo_PopulateAvatarComboList @0x55eb15 (dfx2.exe)].
void test_record_fills_a_lone_combo_spin_list() {
	Avatars data;
	mnu::Document doc = document({"COMBO_LIST"});
	doc.screens[0].roots[0].children[0].type = mnu::WindowType::SpinList;
	MenuRuntime menu; menu.open_document(&doc, "player.mnu", "PLAYER_INFO");
	PlayerInfoAvatars state;
	state.seed_record(1, 0);
	state.update(menu, data.file, Change::Team, 0, 0, 0, gameui, menutxt);
	const int combo = menu.widget_id("COMBO_LIST");
	CHECK(state.nationalities.empty() && state.nationality == 1 && state.division == 0);
	CHECK(menu.get_widget_items(combo) == (std::vector<std::string>{"Head - Body", "HEAD_F - BODY_F"}));
	CHECK(menu.selected_row(combo) == 0 && state.preview_changed);
	menu.select_row(combo, 1, false);
	CHECK(state.selected_combo(menu, data.file) == data.combos + 1);
	state.seed_record(1, 1);
	state.update(menu, data.file, Change::Team, 0, 0, 0, gameui, menutxt);
	CHECK(menu.get_widget_items(combo) == (std::vector<std::string>{"HEAD_F - BODY_F"}));
	// No record seeded: nothing to list.
	PlayerInfoAvatars fresh;
	fresh.update(menu, data.file, Change::Team, 0, 0, 0, gameui, menutxt);
	CHECK(menu.item_count(combo) == 0 && fresh.selected_combo(menu, data.file) == nullptr);
}
// The preview's initial facing: the reseed, then rand() % 180 whole degrees at
// 0xB60B60 BAM a degree, the draw stepping the CRT stream once
// [orig: PlayerInfo_InitPreviewModel @0x560128, @0x5601a2..0x5601b7].
void test_preview_initial_yaw() {
	// srand(1) draws 41 first (base/io/crt_rand.h).
	CHECK(player_info_preview_initial_yaw_bam(1) == 41 * 0xB60B60);
	CHECK(opennova::crt::crt_rand_state() == 1u * 214013u + 2531011u);
	// srand(2) draws 45; the next draw from the same seed repeats it.
	CHECK(player_info_preview_initial_yaw_bam(2) == 45 * 0xB60B60);
	CHECK(player_info_preview_initial_yaw_bam(2) == 45 * 0xB60B60);
	// Every draw lands in [0, 180) whole degrees.
	for (uint32_t seed = 0; seed < 2000; ++seed) {
		const int32_t bam = player_info_preview_initial_yaw_bam(seed);
		CHECK(bam >= 0 && bam % 0xB60B60 == 0 && bam / 0xB60B60 < 180);
	}
}
} // namespace
// The portrait's motion, one menu update at a time (D-PLAYERINFO-37): the blend
// 5% of the way to the hover target, the yaw 0x800000 an update at rest, hovered 5%
// of its error toward the sway about 0x58000000 back of it, in 32-bit words.
// [orig: PlayerInfo_UpdatePlayerPreviewAnimation @0x55dba0]
void test_preview_motion_steps_per_update() {
	float blend = 0.0f;
	blend = player_info_preview_blend_step(blend, true);
	CHECK(blend == 0.05f);
	blend = player_info_preview_blend_step(blend, true);
	CHECK(blend == 0.05f * 0.95f + 0.05f);
	CHECK(player_info_preview_blend_step(1.0f, false) == 0.95f);
	CHECK(player_info_preview_yaw_step(0, false, 1234) == 0x800000);
	CHECK(player_info_preview_yaw_step(0x7FC00000, false, 0) == static_cast<int32_t>(0x80400000u));
	// Ticks 0: no sway, so the yaw closes on -0x58000000: from 0 the error is
	// 0x58000000 and the move ftol(0x58000000 * -0.05).
	const int32_t move = static_cast<int32_t>(static_cast<float>(0x58000000) * -0.05f);
	CHECK(player_info_preview_yaw_step(0, true, 0) == move);
	// At the facing it holds.
	CHECK(player_info_preview_yaw_step(-0x58000000, true, 0) == -0x58000000);
	// Many hovered updates settle on the facing.
	int32_t yaw = 0x12345678;
	for (int i = 0; i < 400; ++i) yaw = player_info_preview_yaw_step(yaw, true, 0);
	CHECK(yaw > -0x58000000 - 64 && yaw < -0x58000000 + 64);
}

int main() {
	test_cascade_and_voice();
	test_missing_stages_and_invalid_selections();
	test_record_fills_a_lone_combo_spin_list();
	test_preview_initial_yaw();
	test_preview_motion_steps_per_update();
	std::cout << "player_info_avatars: " << failures << " failures\n";
	return failures != 0;
}
