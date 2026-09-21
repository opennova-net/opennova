#include <runtime/menu/player_info_avatars.h>

#include <runtime/menu/menu_runtime.h>
#include <runtime/menu/player_info_kit.h>

#include <algorithm>

namespace opennova::menu {
namespace {
const avatars::AvatarNationality *nationality_at(const avatars::AvatarsFile &file, int index) {
	return index >= 0 && static_cast<size_t>(index) < file.nationalities_count
			? file.nationalities + index : nullptr;
}
const avatars::AvatarDivision *division_at(const avatars::AvatarsFile &file, int nat, int div) {
	const auto *nationality = nationality_at(file, nat);
	return nationality && div >= 0 && static_cast<size_t>(div) < nationality->divisions_count
			? nationality->divisions + div : nullptr;
}

struct Fill {
	PlayerInfoAvatars &state;
	MenuRuntime &menu;
	const avatars::AvatarsFile &file;
	int team;
	int voice;
	const hud::GameTextLookup &gameui;
	const hud::GameTextLookup &menutxt;

	// Preserve the existing gameui/Avatars lookup and its raw-key fallback.
	// [orig: GameText_GetStringWithFallback @0x51eb90 / g_TextGameText @0xB4C2AC;
	//  TextResource_GetStringWithFallback(resource, "Avatars", nameKey)]
	std::string name(const char *key) const { return hud::game_text(gameui, "Avatars", key, key); }
	std::string voice_label(int value) const {
		const std::string key = value == kDefaultVoiceValue ? "DEFAULT_VOICE" : "CHARVOICE_" + std::to_string(value);
		const std::string fallback = value == kDefaultVoiceValue ? "Default" : "Voice " + std::to_string(value);
		const std::string ui = hud::game_text(gameui, "Avatars", key.c_str(), fallback.c_str());
		return hud::game_text(menutxt, "Menu", key.c_str(), ui.c_str());
	}
	void items(int id, const std::vector<std::string> &rows) {
		menu.set_widget_items(id, rows);
		if (!rows.empty()) menu.select_row(id, 0, false);
	}

	// [orig: PlayerInfo_PopulateNationalityList @0x55d8c0; team filter D-PLAYERINFO-5]
	void nationalities() {
		const int id = menu.widget_id("NATIONALITY");
		if (id < 0) return;
		state.nationalities.clear();
		std::vector<std::string> rows;
		for (size_t i = 0; i < file.nationalities_count; ++i) {
			const auto &nat = file.nationalities[i];
			if ((nat.alignment != 0) != (team != 0)) continue;
			state.nationalities.push_back(static_cast<int32_t>(i));
			rows.push_back(name(nat.name_key));
		}
		items(id, rows);
		state.nationality = state.nationalities.empty() ? -1 : state.nationalities.front();
		divisions();
	}

	// [orig: PlayerInfo_PopulateDivisionList @0x55da50]
	void divisions() {
		const int id = menu.widget_id("DIVISION");
		if (id < 0) return;
		std::vector<std::string> rows;
		if (const auto *nat = nationality_at(file, state.nationality)) {
			for (size_t i = 0; i < nat->divisions_count; ++i) rows.push_back(name(nat->divisions[i].name_key));
		}
		items(id, rows);
		state.division = rows.empty() ? -1 : 0;
		combos();
	}

	// [orig: populate_avatar_combo_list @0x560210] Head display precedes body.
	void combos() {
		const int id = menu.widget_id("COMBO_LIST");
		if (id < 0) return;
		std::vector<std::string> rows;
		if (const auto *div = division_at(file, state.nationality, state.division)) {
			for (size_t i = 0; i < div->combos_count; ++i) {
				const auto &combo = div->combos[i];
				rows.push_back(name(combo.head.display_name) + " - " + name(combo.body.display_name));
			}
		}
		items(id, rows);
		voices();
		state.preview_changed = true;
	}

	// [orig: populate_player_voice_combo @0x55dce0; sub_57AE90 @0x57ae90
	//  resolves combo+280, stored from the head sex @0x57aad2; miss @0x57aeb5
	//  gives male; invalid override resets profile[team+1532] before select-by-value]
	void voices() {
		const int id = menu.widget_id("PLAYERVOICE");
		if (id < 0) return;
		const auto *combo = state.selected_combo(menu, file, true);
		state.voices = player_info_voice_values(combo ? combo->head.sex : 0);
		std::vector<std::string> rows;
		for (int value : state.voices) rows.push_back(voice_label(value));
		voice = player_info_voice_selection(voice, state.voices);
		items(id, rows);
		const auto found = std::find(state.voices.begin(), state.voices.end(), voice);
		menu.select_row(id, found == state.voices.end() ? 0 : static_cast<int>(found - state.voices.begin()), false);
	}
};
} // namespace

const avatars::AvatarCombo *PlayerInfoAvatars::selected_combo(const MenuRuntime &menu,
		const avatars::AvatarsFile &file, bool default_first) const {
	const auto *div = division_at(file, nationality, division);
	if (!div) return nullptr;
	const int id = menu.widget_id("COMBO_LIST");
	int row = id >= 0 ? menu.selected_row(id) : (default_first ? 0 : -1);
	if ((id >= 0 || default_first) && row < 0) row = 0;
	return row >= 0 && static_cast<size_t>(row) < div->combos_count ? div->combos + row : nullptr;
}

int PlayerInfoAvatars::update(MenuRuntime &menu, const avatars::AvatarsFile &file,
		Change change, int value, int team, int saved_voice,
		const hud::GameTextLookup &gameui, const hud::GameTextLookup &menutxt) {
	preview_changed = false;
	Fill fill{*this, menu, file, team, saved_voice, gameui, menutxt};
	switch (change) {
		case Change::Team:
			// [orig: PlayerInfo_SaveAndRepopulate @0x5608f0 -> PopulateAllControls(team)]
			fill.nationalities();
			break;
		case Change::Nationality:
			// [orig: PlayerInfo_HandleNationalitySelect @0x560600]
			nationality = value >= 0 && static_cast<size_t>(value) < nationalities.size() ? nationalities[value] : -1;
			fill.divisions();
			break;
		case Change::Division:
			// [orig: PlayerInfo_HandleDivisionSelect @0x560690]
			division = value;
			fill.combos();
			break;
		case Change::Combo:
			// [orig: PlayerInfo_HandleVoiceSelect @0x55fe00, COMBO_LIST registration @0x5615a6]
			fill.voices();
			preview_changed = true;
			break;
		case Change::Voice:
			// [orig: sub_560030 @0x560030 -- notification VALUE byte -> profile[team+1532]]
			fill.voice = value >= 0 && static_cast<size_t>(value) < voices.size() ? voices[value] : kDefaultVoiceValue;
			break;
	}
	return fill.voice;
}

std::string PlayerInfoAvatars::voice_preview_trigger(const MenuRuntime &menu,
		const avatars::AvatarsFile &file, int saved_voice) const {
	// [orig: PlayerInfo_PreviewVoice @0x55ff70 -- nonzero override, else
	//  Avatars_ResolveSelectionIndex @0x57ae60 reads combo+284 (head voice @0x57aae3)]
	if (saved_voice == kDefaultVoiceValue) {
		const auto *combo = selected_combo(menu, file, true);
		saved_voice = combo ? combo->head.voice : -1;
	}
	return saved_voice < 0 ? std::string() : "VOICE_" + std::to_string(saved_voice);
}
} // namespace opennova::menu
