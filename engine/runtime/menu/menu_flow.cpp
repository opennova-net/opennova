#include "menu_flow.h"

#include <base/gameprofile/game_type.h>
#include <base/io/strutil.h>
#include <base/title/title.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace opennova::menu {

MissionChoice mission_choice(const mission_catalog::Row &row) {
	return { row.file, mission_catalog::display_text(row), row.briefing, mission_catalog::game_type_of(row),
		row.campaign, row.title, row.mission_type };
}

namespace {

// dfx2.exe's SINGLE_PLAYER controls, each named where its code finds it (DFX2-60).
// [orig: sub_560190 @0x56024e (CA_LIST), @0x560317 (BRIEFING), @0x56034a (CA_MISSION_LIST),
//  @0x560479 (CAMPAIGN_IMAGE_OVERLAY); sub_560510 @0x56053a (IA_LIST), @0x5606a6 (IA_BRIEFING);
//  sub_560ED0 @0x560ee9 / @0x560f07 (the radios) (dfx2.exe)]
constexpr const char *kCampaignList = "CA_LIST";
constexpr const char *kCampaignMissionList = "CA_MISSION_LIST";
constexpr const char *kCampaignBriefing = "BRIEFING";
constexpr const char *kCampaignImageOverlay = "CAMPAIGN_IMAGE_OVERLAY";
constexpr const char *kCampaignImage = "CAMPAIGN_IMAGE";
// The appearance state the campaign images take: the default's, mask 1 [orig:
//  CWnd_SetAppearanceTexture(.., 1, 0x300651) @0x560464, @0x5604a4, @0x560cac, @0x560cfc
//  (dfx2.exe)].
constexpr uint32_t kCampaignImageStates = 1u;
constexpr const char *kInstantActionList = "IA_LIST";
constexpr const char *kInstantActionBriefing = "IA_BRIEFING";
constexpr const char *kTabCampaign = "RADIO_TAB_CAMPAIGN";
constexpr const char *kTabInstantAction = "RADIO_TAB_INSTANTACTION";

// The zeroed slot a campaign index past the counted ones reads: no briefing, no missions.
const campaign::Campaign kEmptyCampaign{};

void set_text(MenuRuntime &menu, const std::string &name, const std::string &text) {
	const int id = menu.widget_id(name);
	if (id >= 0) menu.set_widget_text(id, text);
}

} // namespace

bool MenuFlow::campaign_tabs() const {
	return require_title(title_, "MenuFlow::campaign_tabs").sp_campaign_tabs;
}

void MenuFlow::set_campaign_source(std::string def_text, const rtxt::File *menu_text,
		const campaign::CompletionTable &progress) {
	campaign_def_ = std::move(def_text);
	menu_text_ = menu_text;
	progress_ = progress;
}

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
void MenuFlow::apply_sp_selection(MenuRuntime &menu, const std::vector<std::string> &briefings,
		const std::string &briefing, bool enabled) {
	for (const auto &name : briefings) set_text(menu, name, briefing);
	set_accept(menu, enabled);
}

void MenuFlow::set_accept(MenuRuntime &menu, bool enabled) {
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
	if (sp && campaign_tabs()) {
		seed_campaign_tabs(menu, id, rows);
		return;
	}
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
		apply_sp_selection(menu, briefings_, "", false);
	}
}

// dfx2.exe's populate: the campaign reader first, then IA_LIST with the plain Co-op missions (the
// objective bit no longer forgiven) that no campaign holds or that are complete, each row "*" for a
// loose file and its title else its name; ACCEPT off and IA_BRIEFING cleared. CA_MISSION_LIST is the
// reader's, so its own seed fills nothing.
// [orig: sub_560510 @0x560510 - sub_560190 @0x560525; the row test @0x560576..0x560596 ((code &
//  0xFFFDFFFF) == 0x10020 && !(code & 0x20000)), @0x56062e..0x56063a (!(+0x10C & 2) || (+0x10C &
//  4)); ACCEPT off @0x560691; IA_BRIEFING "" @0x5606bd (dfx2.exe)]
void MenuFlow::seed_campaign_tabs(MenuRuntime &menu, int id, const std::vector<MissionChoice> &rows) {
	if (!strutil::iequals(menu.widget_name_of(id), kInstantActionList)) return;
	// The table keeps its entries' campaign words until it is built again: the same table keeps
	// the words the last reads left.
	bool same = catalog_.size() == rows.size();
	for (size_t i = 0; same && i < rows.size(); ++i) same = catalog_[i].file == rows[i].file;
	std::vector<MissionChoice> kept = std::move(catalog_);
	catalog_ = rows;
	if (same)
		for (size_t i = 0; i < rows.size(); ++i) catalog_[i].campaign = kept[i].campaign;
	read_campaigns(menu);
	auto &saved = mission_rows_[id];
	saved.clear();
	std::vector<std::string> texts;
	for (const auto &row : catalog_) {
		if (row.game_type != game_type::kCoop) continue;
		const uint32_t flags = row.campaign.flags;
		if ((flags & mission_catalog::kRowInCampaign) != 0 && (flags & mission_catalog::kRowComplete) == 0)
			continue;
		saved.push_back(row);
		texts.push_back(row.text);
	}
	menu.set_widget_items(id, texts);
	menu.select_row(id, -1, false);
	apply_sp_selection(menu, {kInstantActionBriefing}, "", false);
}

// The reader: campaign.def parsed and the profile's progress applied over the table's words, then,
// with a CA_LIST, its rows rebuilt (a campaign with no title has none; one whose depends are not all
// complete is locked), its picked row picked again, and the campaign that row holds shown (row 0's
// value, campaign 0, when none was picked); ACCEPT off.
// [orig: sub_560190 @0x560190 - the parse and progress @0x5601a8..0x56023c; CA_LIST
//  @0x56024e..0x5602ed (UIList_GetSelectedRow @0x560264, the rows' clear @0x56026e, the title test
//  @0x560282, UIList_AddRow @0x5602cb, CListWnd_SetRowAvailable @0x5602d9); CListWnd_SetRowVisibility
//  @0x5602f8; CListWnd_GetRowValue @0x560315 (0 for a row out of range); ACCEPT off @0x5604cf
//  (dfx2.exe)]
void MenuFlow::read_campaigns(MenuRuntime &menu) {
	std::vector<mission_catalog::CampaignWords> words;
	std::vector<std::string> files;
	for (const auto &row : catalog_) {
		words.push_back(row.campaign);
		files.push_back(row.file);
	}
	campaigns_ = campaign::parse(campaign_def_.data(), campaign_def_.size(), words, files, menu_text_);
	campaign::apply_progress(campaigns_, progress_, words);
	for (size_t i = 0; i < catalog_.size(); ++i) catalog_[i].campaign = words[i];
	const int list = menu.widget_id(kCampaignList);
	if (list < 0) return;
	const int previous = campaign_pick_;
	campaign_rows_.clear();
	std::vector<std::string> texts;
	for (size_t c = 0; c < campaigns_.campaigns.size(); ++c) {
		if (campaigns_.campaigns[c].title.empty()) continue;
		campaign_rows_.push_back(c);
		texts.push_back(campaigns_.campaigns[c].title);
	}
	menu.set_widget_items(list, texts);
	for (size_t r = 0; r < campaign_rows_.size(); ++r)
		if (!campaign::available(campaigns_, campaign_rows_[r]))
			menu.set_item_enabled(list, static_cast<int>(r), false);
	const bool in_range = previous >= 0 && previous < static_cast<int>(campaign_rows_.size());
	// A locked row is not picked again [orig: CListWnd_SetRowVisibility @0x645190 (Jointops.exe),
	//  the locked test @0x6451fa].
	campaign_pick_ = in_range && menu.is_item_enabled(list, previous) ? previous : -1;
	menu.select_row(list, campaign_pick_, false);
	show_campaign(menu, in_range ? campaign_rows_[static_cast<size_t>(previous)] : 0);
	set_accept(menu, false);
}

// A campaign shown: its briefing into BRIEFING, then, with a CA_MISSION_LIST, its missions each
// listed whose depends are complete (its title, else its file name, no loose mark), and the overlay
// shown when the campaign is complete.
// [orig: sub_560190 @0x560317..0x5604a4; sub_55FAD0 @0x55fb19..0x55fcd4, the row text
//  @0x55fba7..0x55fbda, UIList_AddRow @0x55fc25 (dfx2.exe)]
void MenuFlow::show_campaign(MenuRuntime &menu, size_t index) {
	const campaign::Campaign &shown =
			index < campaigns_.campaigns.size() ? campaigns_.campaigns[index] : kEmptyCampaign;
	set_text(menu, kCampaignBriefing, shown.has_briefing ? shown.briefing : std::string());
	const int list = menu.widget_id(kCampaignMissionList);
	if (list < 0) return;
	std::vector<mission_catalog::CampaignWords> words;
	for (const auto &row : catalog_) words.push_back(row.campaign);
	auto &saved = mission_rows_[list];
	saved.clear();
	std::vector<std::string> texts;
	for (const campaign::Mission &mission : shown.missions) {
		if (mission.row >= catalog_.size() || !campaign::listed(mission, words)) continue;
		const MissionChoice &row = catalog_[mission.row];
		saved.push_back(row);
		texts.push_back(row.title.empty() ? row.file : row.title);
	}
	menu.set_widget_items(list, texts);
	menu.select_row(list, -1, false);
	// [orig: CAMPAIGN_IMAGE's image CWnd_SetAppearanceTexture(+0x144) @0x560464; the overlay
	//  @0x560479..0x5604ad (dfx2.exe)]
	campaign_image_ = shown.image;
	set_campaign_image(menu);
	show_overlay(menu, shown.complete);
}

// CAMPAIGN_IMAGE's default appearance takes the named image, loaded at once
// [orig: UI_FindScreenControl(CAMPAIGN_IMAGE) @0x56044b, CWnd_SetAppearanceTexture @0x560464;
//  sub_560BC0 @0x560c5d, @0x560cac (dfx2.exe)].
void MenuFlow::set_campaign_image(MenuRuntime &menu) {
	const int image = menu.widget_id(kCampaignImage);
	if (image >= 0) menu.set_widget_image(image, kCampaignImageStates, campaign_image_);
}

// [orig: CWnd_SetShown(CAMPAIGN_IMAGE_OVERLAY, 1) with the complete_overlay image @0x560491 /
//  @0x5604a4, else CWnd_SetShown 0 @0x5604ad (dfx2.exe)]
void MenuFlow::show_overlay(MenuRuntime &menu, bool complete) {
	const int overlay = menu.widget_id(kCampaignImageOverlay);
	if (overlay < 0) return;
	menu.set_widget_shown(overlay, complete);
	// Shown, it takes campaign.def's complete_overlay image [orig:
	//  CWnd_SetAppearanceTexture(byte_25DB320) @0x5604a4, @0x560cfc (dfx2.exe)].
	if (complete)
		menu.set_widget_image(overlay, kCampaignImageStates, campaigns_.complete_overlay);
}

// [orig: sub_55FAD0 @0x55fad0 - events 0x5000001 / 0x5000002 @0x55fb03, the row's value (the
//  campaign) @0x55fb3b; ACCEPT off @0x55fcf8 (dfx2.exe)]
bool MenuFlow::select_campaign(MenuRuntime &menu, int id, int row) {
	if (!campaign_tabs() || !strutil::iequals(menu.widget_name_of(id), kCampaignList)) return false;
	if (row < 0 || row >= static_cast<int>(campaign_rows_.size())) return true;
	campaign_pick_ = row;
	show_campaign(menu, campaign_rows_[static_cast<size_t>(row)]);
	set_accept(menu, false);
	return true;
}

bool MenuFlow::activate_tab(MenuRuntime &menu, const std::string &name) {
	if (!campaign_tabs()) return false;
	const char *list_name = strutil::iequals(name, kTabCampaign) ? kCampaignMissionList
			: strutil::iequals(name, kTabInstantAction) ? kInstantActionList
			: nullptr;
	if (list_name == nullptr) return false;
	// [orig: UIList_CountSelectedItems > 0 @0x56076e / @0x5607bb; nothing without the list
	//  @0x56074b / @0x56079c (dfx2.exe)]
	const int list = menu.widget_id(list_name);
	if (list >= 0) set_accept(menu, !menu.selected_rows(list).empty());
	return true;
}

// [orig: SinglePlayer_MissionListEventHandler @0x561ed0: BRIEFING SetText
// from the entry's briefing pointer, then ACCEPT re-enable]. dfx2.exe's twin writes IA_BRIEFING
// [orig: SinglePlayer_MissionListEventHandler @0x560d60 (@0x560dbd) (dfx2.exe)]; its campaign
// mission list's pick writes BRIEFING and names the mission's own image, the overlay shown for a
// complete mission [orig: sub_560BC0 @0x560bc0 - the entry @0x560c1d, BRIEFING @0x560c4b, the image
// Path_ReplaceOrAppendExtension(name, "tga") @0x560c96, the overlay @0x560ce5, ACCEPT on @0x560d28
// (dfx2.exe)].
void MenuFlow::select_mission(MenuRuntime &menu, int id, int row, const std::string &fallback) {
	const auto *choice = mission_row(id, row);
	selected_mission_ = choice ? choice->file : fallback;
	selected_campaign_ = choice ? choice->campaign : mission_catalog::CampaignWords{};
	const std::string name = menu.widget_name_of(id);
	if (choice == nullptr || !is_sp_list(name)) return;
	if (!campaign_tabs()) {
		apply_sp_selection(menu, briefings_, choice->briefing, true);
	} else if (strutil::iequals(name, kCampaignMissionList)) {
		const size_t dot = choice->file.find('.');
		campaign_image_ = (dot == std::string::npos ? choice->file : choice->file.substr(0, dot)) + ".tga";
		set_campaign_image(menu);
		show_overlay(menu, (choice->campaign.flags & mission_catalog::kRowComplete) != 0);
		apply_sp_selection(menu, {kCampaignBriefing}, choice->briefing, true);
	} else {
		apply_sp_selection(menu, {kInstantActionBriefing}, choice->briefing, true);
	}
}

// Double-click launches the FILE, never the title painted in the list.
// [orig: SinglePlayer_MissionListEventHandler, event 0x5000002 @0x561f8d]
void MenuFlow::activate_mission(int id, int row) {
	if (const auto *choice = mission_row(id, row)) {
		selected_mission_ = choice->file;
		selected_campaign_ = choice->campaign;
	}
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
// UI_InitIngameScreenOnce seeds the video-mode state to zero @0x555734]
std::string MenuFlow::take_expansion_reload() {
	expansion_pending_ = false;
	return std::exchange(expansion_request_, {});
}

void enable_class_rows(MenuRuntime &menu, int spin, uint32_t class_allow_mask) {
	if (spin < 0) return;
	for (int row = 0; row < 5; ++row)
		menu.set_item_enabled(spin, row, ((class_allow_mask >> (5 + row)) & 1u) != 0);
}

// The host population excludes stock Co-op and resets the rotation.
// [orig: UI_InitHostSettingsDialog @0x558960, skip @0x558a70;
// GAME_TYPE ALL=255 @0x558aee]
void HostDialog::seed(MenuRuntime &menu, const std::vector<MissionChoice> &rows, const Title &title) {
	pool_.clear();
	selected_.clear();
	launch_options_.clear();
	vehicle_missions_ = title.host_vehicle_missions;
	switch_starts_off_ = title.host_switch_starts_off;
	for (const auto &row : rows)
		if (game_type::host_list_visible(row.game_type)) pool_.push_back(row);
	shown_.assign(pool_.size(), false);
	const int table = menu.widget_id("SELECTED_MISSIONS");
	if (table >= 0) menu.table_clear_rows(table);
	// Each GAME_TYPE row disabled, then enabled when it is ALL or some mission of the whole list
	// (the stock Co-op rows the list leaves out among them) maps to its value; then ALL selected
	// [orig: UI_InitHostSettingsDialog @ 0x558ac3..0x558c0e, CSpinListWnd_SetItemEnabled
	// @ 0x558adc / 0x558c02, the 13-way map @ 0x558b16 (game_type::host_filter_category);
	// SpinList_SelectItemByValue @ 0x558c1b].
	const int spin = menu.widget_id("GAME_TYPE");
	if (spin >= 0) {
		const int count = menu.item_count(spin);
		for (int i = 0; i < count; ++i) {
			menu.set_item_enabled(spin, i, false);
			// The row's VALUE as its parse read it [orig: CUISpinList_ParseXMLDefinition @ 0x64c144, wcstol].
			const int item = static_cast<int>(std::strtol(menu.item_value(spin, i).c_str(), nullptr, 10));
			bool used = item == game_type::kHostFilterAll;
			// DFX2-72: the combat-vehicle value is enabled by any combat-vehicle mission of the
			// whole list [orig: UI_InitHostSettingsDialog @0x556b30 - the 254 test @0x556cd5, the
			// walk over entry+0x1134 == 2 @0x556cf5..0x556d11 (dfx2.exe)].
			if (vehicle_missions_ && item == game_type::kHostFilterVehicle) {
				for (size_t m = 0; !used && m < rows.size(); ++m)
					used = rows[m].mission_type == game_type::kHostVehicleMissionType;
				if (used) menu.set_item_enabled(spin, i, true);
				continue;
			}
			for (size_t m = 0; !used && m < rows.size(); ++m)
				used = game_type::host_filter_category(rows[m].game_type) == item;
			if (used) menu.set_item_enabled(spin, i, true);
		}
		menu.select_row_by_value(spin, std::to_string(game_type::kHostFilterAll), false);
	}
	filter(menu);
	sync_start(menu);
}

// Whether `row` lists under GAME_TYPE `value`: ALL, or its own category; under the title's
// combat-vehicle filter (DFX2-72) a combat-vehicle row lists under 254 too and, but for a
// remove's re-show, not under its own category.
// [orig: HostDialog_FilterMissionListByGameType @0x556fe0 - (category == value || value == 255)
//  @0x55717c; HostDialog_AddRemoveSelectedMissions @0x557c10 - the remove's re-show, the same
//  test @0x557da1..0x557db7; HostDialog_FilterMissionListByGameType @0x555180 - (category == value
//  && entry+0x1134 != 2) || value == 255 || (entry+0x1134 == 2 && value == 254)
//  @0x555300..0x555334; the remove's re-show in HostDialog_SelectedMissionsTableEvent's chunk
//  @0x555f87..0x555faf: category == value || value == 255 || (entry+0x1134 == 2 && value == 254)
//  (dfx2.exe)]
bool HostDialog::listed_under_(const MissionChoice &row, int value, bool removed) const {
	if (value == game_type::kHostFilterAll) return true;
	const bool vehicle = vehicle_missions_ && row.mission_type == game_type::kHostVehicleMissionType;
	if (vehicle && value == game_type::kHostFilterVehicle) return true;
	return game_type::host_filter_category(row.game_type) == value && (removed || !vehicle);
}

void HostDialog::publish_(MenuRuntime &menu) {
	const int list = menu.widget_id("MISSION_LIST");
	if (list < 0) return;
	visible_.clear();
	std::vector<std::string> texts;
	for (int i = 0; i < static_cast<int>(pool_.size()); ++i) {
		if (!shown_[static_cast<size_t>(i)]) continue;
		visible_.push_back(i);
		texts.push_back(pool_[i].text);
	}
	menu.set_widget_items(list, texts);
	menu.select_row(list, -1, false);
}

// Every row disabled, then each not selected that lists under the value enabled.
// [orig: HostDialog_FilterMissionListByGameType @0x556fe0, the value @0x557044, walk @0x557072;
// UI_InitHostSettingsDialog title/filename row @0x558a48]
void HostDialog::filter(MenuRuntime &menu) {
	const int list = menu.widget_id("MISSION_LIST");
	if (list < 0) return;
	const int spin = menu.widget_id("GAME_TYPE");
	const auto value = menu.item_value(spin, menu.selected_row(spin));
	char *end = nullptr;
	const long parsed = std::strtol(value.c_str(), &end, 10);
	filter_value_ = !value.empty() && end == value.c_str() + value.size()
			? static_cast<int>(parsed) : game_type::kHostFilterAll;
	for (int i = 0; i < static_cast<int>(pool_.size()); ++i)
		shown_[static_cast<size_t>(i)] =
				std::find(selected_.begin(), selected_.end(), i) == selected_.end() &&
				listed_under_(pool_[static_cast<size_t>(i)], filter_value_, false);
	publish_(menu);
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
		// not an objective mode [orig: @0x557E72..0x557E94]; under the title's
		// DFX2-72 default, 0 for every row [orig: HostDialog_SelectedMissionsTableEvent's chunk -
		// `mov [esi+eax+1140h], 0` @0x556076 (dfx2.exe)].
		const int32_t option =
				!switch_starts_off_ && game_type::host_rotation_default(row.game_type) ? 1 : 0;
		menu.table_add_row(table, {row.text, text ? text("GateTypeAbbrev", key, key) : key,
				option != 0 ? "1" : "0"});
		selected_.push_back(picked);
		launch_options_.push_back(option);
		// Its list row disabled [orig: CListWnd_SetRowEnabled(row, 0) @0x557EF1; @0x5560ce
		// (dfx2.exe)].
		shown_[static_cast<size_t>(picked)] = false;
	}
	publish_(menu);
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
			// Its list row enabled again when it lists under the last filtered value, by the
			// remove's own test [orig: @0x557cce..0x557dc7; HostDialog_SelectedMissionsTableEvent's
			// chunk @0x555f87..0x555fbf (dfx2.exe)].
			const int picked = selected_[static_cast<size_t>(row)];
			shown_[static_cast<size_t>(picked)] =
					listed_under_(pool_[static_cast<size_t>(picked)], filter_value_, true);
			selected_.erase(selected_.begin() + row);
			launch_options_.erase(launch_options_.begin() + row);
		}
	}
	publish_(menu);
	sync_start(menu);
}

void HostDialog::set_class_words(MenuRuntime &menu, const int32_t *words, size_t count) {
	any_class_ = std::any_of(words, words + count, [](int32_t word) { return word != 0; });
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
