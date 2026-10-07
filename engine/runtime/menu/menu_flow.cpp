#include "menu_flow.h"

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace opennova::menu {

void MenuFlow::set_mission_controls(std::vector<std::string> lists,
		std::vector<std::string> briefings, std::vector<std::string> accepts) {
	sp_lists_ = std::move(lists);
	briefings_ = std::move(briefings);
	accepts_ = std::move(accepts);
}

// The SP mission-select names (IA_LIST / CA_MISSION_LIST) filter to Co-op,
// show catalog titles and drive BRIEFING and ACCEPT.
// [orig: SinglePlayer_PopulateMissionList @0x561840;
// SinglePlayer_MissionListEventHandler @0x561ed0]
bool MenuFlow::is_sp_list(const std::string &name) const {
	for (const auto &list : sp_lists_)
		if (strutil::iequals(name, list)) return true;
	return false;
}

const MissionChoice *MenuFlow::mission_row(int id, int row) const {
	const auto found = mission_rows_.find(id);
	if (found == mission_rows_.end() || row < 0 || row >= static_cast<int>(found->second.size()))
		return nullptr;
	return &found->second[static_cast<size_t>(row)];
}

// [orig: the ACCEPT SetInteractiveRecursive pair @0x56198d / @0x561f6a]
void MenuFlow::apply_sp_selection(MenuRuntime &menu, const std::string &briefing, bool enabled) {
	for (const auto &name : briefings_) {
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_text(id, briefing);
	}
	for (const auto &name : accepts_) {
		const int id = menu.widget_id(name);
		if (id >= 0) menu.set_widget_disabled(id, !enabled);
	}
}

// Populate leaves NO selection: the frame's row-0 preselect is a device
// artifact. The activate refresh enables ACCEPT only when a selection exists.
// [orig: SinglePlayer_PopulateMissionList @0x561840;
// SinglePlayer_RefreshAcceptOnActivate @0x561a20;
// UIList_CountSelectedItems @0x6445c0]
void MenuFlow::seed_missions(MenuRuntime &menu, int id, const std::vector<MissionChoice> &rows) {
	const bool sp = is_sp_list(menu.widget_name_of(id));
	auto &saved = mission_rows_[id];
	saved.clear();
	std::vector<std::string> texts;
	for (const auto &row : rows) {
		if (sp && !game_type::is_waypoint_family(row.game_type)) continue;
		saved.push_back(row);
		texts.push_back(row.text);
	}
	menu.set_widget_items(id, texts);
	if (sp) {
		menu.select_row(id, -1, false);
		apply_sp_selection(menu, "", false);
	}
}

// [orig: SinglePlayer_MissionListEventHandler @0x561ed0: BRIEFING SetText
// from the entry's briefing pointer, then ACCEPT re-enable]
void MenuFlow::select_mission(MenuRuntime &menu, int id, int row, const std::string &fallback) {
	const auto *choice = mission_row(id, row);
	selected_mission_ = choice ? choice->file : fallback;
	if (choice && is_sp_list(menu.widget_name_of(id)))
		apply_sp_selection(menu, choice->briefing, true);
}

// Double-click launches the FILE, never the title painted in the list.
// [orig: SinglePlayer_MissionListEventHandler, event 0x5000002 @0x561f8d]
void MenuFlow::activate_mission(int id, int row) {
	if (const auto *choice = mission_row(id, row)) selected_mission_ = choice->file;
}

// Choosing only raises the request; remounting inside the callback would
// replace the resources while the menu pump is still using them. The pick is
// compared without case with the name running: the same takes nothing and
// lowers the flag; another, the base game's "" included, is copied into the
// name and raises it. Nothing keeps the pick past the run: no configuration
// key, no restart (D-MNU-31). A loose authoring mount cannot layer expansion
// PFFs (ADR 0025). The reload this request leads to tears the menu down and
// boots it anew, where the join's switch reloads under the menu it keeps.
// [orig: Options_HandleAcceptOrBack @0x55a710 — _stricmp(pick, g_ExpansionName)
// @0x55ad29; the profile saved @0x55ad35, the pick copied @0x55ad43, the flag
// dword_252DD90 @0x252DD90 raised @0x55ad4f / lowered @0x55ad5b; Menu_UpdateFrame
// @0x5528a0 calls Game_ReloadExpansionAndMods @0x552710 on the following update;
// the join's Expansion_ReloadAllAssets @0x568370 keeps g_GameMenu, restyling it
// @0x5683c5..0x5683eb]
MenuFlow::ExpansionPick MenuFlow::request_expansion(const std::string &name,
		const std::string &current, bool packed_root) {
	if (strutil::iequals(name, current)) {
		expansion_request_.clear();
		expansion_pending_ = false;
		return ExpansionPick::Ignored;
	}
	if (!packed_root) return ExpansionPick::NeedsPackedRoot;
	expansion_request_ = name;
	expansion_pending_ = true;
	return ExpansionPick::Queued;
}

// Menu_UpdateFrame tail: if (request && !video_mode_state) reload, then
// clear. OpenNova has no video-mode transition machine, so only the request
// gates this port. Consuming before the device call prevents a re-entry from
// repeating it. [orig: @0x552906..0x55291d; UI_ApplyVideoModeChange @0x55a590;
// sub_555710 seeds the video-mode state to zero @0x555734]
std::string MenuFlow::take_expansion_reload() {
	expansion_pending_ = false;
	return std::exchange(expansion_request_, {});
}

// The host population excludes stock Co-op and resets the rotation.
// [orig: UI_InitHostSettingsDialog @0x558960, skip @0x558a70;
// GAME_TYPE ALL=255 @0x558aee]
void HostDialog::seed(MenuRuntime &menu, const std::vector<MissionChoice> &rows) {
	pool_.clear();
	selected_.clear();
	launch_options_.clear();
	for (const auto &row : rows)
		if (game_type::host_list_visible(row.game_type)) pool_.push_back(row);
	const int table = menu.widget_id("SELECTED_MISSIONS");
	if (table >= 0) menu.table_clear_rows(table);
	filter(menu);
	sync_start(menu);
}

// [orig: HostDialog_FilterMissionListByGameType @0x556fe0, walk @0x557072;
// UI_InitHostSettingsDialog title/filename row @0x558a48]
void HostDialog::filter(MenuRuntime &menu) {
	const int list = menu.widget_id("MISSION_LIST");
	if (list < 0) return;
	const int spin = menu.widget_id("GAME_TYPE");
	const auto value = menu.item_value(spin, menu.selected_row(spin));
	char *end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	const int category = !value.empty() && end == value.c_str() + value.size()
			? static_cast<int>(parsed) : game_type::kHostFilterAll;
	visible_.clear();
	std::vector<std::string> texts;
	for (int i = 0; i < static_cast<int>(pool_.size()); ++i) {
		if (std::find(selected_.begin(), selected_.end(), i) != selected_.end()) continue;
		if (category != game_type::kHostFilterAll &&
				game_type::host_filter_category(pool_[i].game_type) != category)
			continue;
		visible_.push_back(i);
		texts.push_back(pool_[i].text);
	}
	menu.set_widget_items(list, texts);
	menu.select_row(list, -1, false);
}

// Resolve every selected visible row before rebuilding its mapping. Each
// table row stores the mission, localized GateTypeAbbrev and rotation bit.
// [orig: HostDialog_AddRemoveSelectedMissions @0x557c10,
// add branch @0x557e27..0x557ef1; GameType_GetAbbreviation @0x520fd0]
void HostDialog::add_selected(MenuRuntime &menu, const hud::GameTextLookup &text) {
	const int list = menu.widget_id("MISSION_LIST");
	const int table = menu.widget_id("SELECTED_MISSIONS");
	if (list < 0 || table < 0) return;
	for (int index : menu.selected_rows(list)) {
		if (index < 0 || index >= static_cast<int>(visible_.size())) continue;
		const int picked = visible_[index];
		if (std::find(selected_.begin(), selected_.end(), picked) != selected_.end()) continue;
		const auto &row = pool_[picked];
		const char *key = game_type::host_abbreviation_key(row.game_type);
		// The ADD sets the row's launch option to its default: a team mode and
		// not an objective mode [orig: @0x557E72..0x557E94].
		const int32_t option = game_type::host_rotation_default(row.game_type) ? 1 : 0;
		menu.table_add_row(table, {row.text, text ? text("GateTypeAbbrev", key, key) : key,
				option != 0 ? "1" : "0"});
		selected_.push_back(picked);
		launch_options_.push_back(option);
	}
	filter(menu);
	sync_start(menu);
}

// Removing high indices first preserves all lower row identities.
// [orig: HostDialog_AddRemoveSelectedMissions backward walk @0x557c84]
void HostDialog::remove_selected(MenuRuntime &menu) {
	const int table = menu.widget_id("SELECTED_MISSIONS");
	if (table < 0) return;
	auto rows = menu.table_selected_rows(table);
	std::sort(rows.rbegin(), rows.rend());
	for (int row : rows) {
		menu.table_remove_row(table, row);
		if (row >= 0 && row < static_cast<int>(selected_.size())) {
			selected_.erase(selected_.begin() + row);
			launch_options_.erase(launch_options_.begin() + row);
		}
	}
	filter(menu);
	sync_start(menu);
}

// [orig: START_GAME interactive gate @0x557f09, init disable @0x5589f2]
void HostDialog::sync_start(MenuRuntime &menu) const {
	const int start = menu.widget_id("START_GAME");
	if (start >= 0) menu.set_widget_disabled(start, !can_start());
}

// Table cells carry display text; START resolves their underlying file names.
// [orig: UI_HandleHostSessionStart mission-index walk @0x556dae]
std::vector<std::string> HostDialog::selected_missions() const {
	std::vector<std::string> files;
	for (int row : selected_) files.push_back(pool_[row].file);
	return files;
}

void HostDialog::toggle_switch(MenuRuntime &menu, int row) {
	const int table = menu.widget_id("SELECTED_MISSIONS");
	if (table < 0 || row < 0 || row >= static_cast<int>(selected_.size())) return;
	int32_t &option = launch_options_[static_cast<size_t>(row)];
	if (game_type::host_rotation_default(pool_[static_cast<size_t>(selected_[row])].game_type))
		option = option == 0 ? 1 : 0;
	menu.table_set_cell_text(table, row, 2, option != 0 ? "1" : "0");
}

std::vector<int32_t> HostDialog::selected_launch_options() const { return launch_options_; }

// Country matches the first three characters, case-insensitively, leaving
// the authored row when there is no match.
// [orig: UI_PopulateHostSettingsFromConfig @0x555fe0]
void HostDialog::select_location(MenuRuntime &menu, int id, const std::string &country) {
	for (int row = 0; row < menu.item_count(id); ++row) {
		if (strutil::iequals(menu.item_text(id, row).substr(0, 3), country.substr(0, 3))) {
			menu.select_row(id, row, false);
			return;
		}
	}
}

} // namespace opennova::menu
