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
	screen.root_window.name = "ROOT";
	for (const char *name : names) {
		mnu::Window w;
		w.name = name;
		w.type = mnu::WindowType::Combo;
		screen.root_window.children.push_back(w);
	}
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
	menu.open_document(&no_nat, "partial.mnu", "PLAYER_INFO");
	CHECK(state.update(menu, data.file, Change::Team, 0, 1, 7, gameui, menutxt) == 7);
	CHECK(state.nationalities == (std::vector<int32_t>{1, 2}) && !state.preview_changed);
	menu.open_document(&no_div, "partial.mnu", "PLAYER_INFO");
	CHECK(state.update(menu, data.file, Change::Team, 0, 1, 7, gameui, menutxt) == 7);
	CHECK(state.nationality == 0 && state.division == 999 && !state.preview_changed);
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
} // namespace
int main() {
	test_cascade_and_voice();
	test_missing_stages_and_invalid_selections();
	std::cout << "player_info_avatars: " << failures << " failures\n";
	return failures != 0;
}
