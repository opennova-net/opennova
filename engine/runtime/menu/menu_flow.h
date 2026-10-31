#pragma once

#include "menu_runtime.h"
#include <runtime/hud/game_text_lookup.h>
#include <runtime/mission/campaign.h>
#include <runtime/mission/mission_catalog.h>

namespace opennova {
struct Title;
}

namespace opennova::menu {

// Catalog data after the embedder resolves the display label and briefing.
struct MissionChoice {
	std::string file, text, briefing;
	uint32_t game_type = 0;
	mission_catalog::CampaignWords campaign; // the table entry's (mission_catalog.h)
	std::string title; // the entry's title alone ("" none): the campaign mission list's text
};

// A catalog row's choice: its file, its list text (mission_catalog::display_text), its briefing,
// its session game type (mission_catalog::game_type_of) and its campaign words.
MissionChoice mission_choice(const mission_catalog::Row &row);

// The SP list's model and deferred expansion request survive screen switches
// on the shell's one driver; document swaps invalidate only the row mappings.
class MenuFlow {
public:
	void set_mission_controls(std::vector<std::string> lists,
			std::vector<std::string> briefings, std::vector<std::string> accepts);
	void clear_rows() { mission_rows_.clear(); }
	void seed_missions(MenuRuntime &menu, int id, const std::vector<MissionChoice> &rows);
	void select_mission(MenuRuntime &menu, int id, int row, const std::string &fallback);
	void activate_mission(int id, int row);
	const std::string &selected_mission() const { return selected_mission_; }
	// The picked mission's campaign words: SinglePlayer_StartMission records its campaign in the
	// profile (+60) and the round end writes its completion byte from both [orig:
	// SinglePlayer_StartMission @0x561b93..0x561b99; PlayerProfile_SaveRoundEndProgress @0x54d6c6..
	// 0x54d712]. A fallback pick, none or a list without a row has none (-1).
	const mission_catalog::CampaignWords &selected_campaign() const { return selected_campaign_; }
	void clear_selected_mission() {
		selected_mission_.clear();
		selected_campaign_ = {};
	}

	// The executable whose single-player screen this is (null: Jointops.exe's):
	// Title::sp_campaign_tabs selects dfx2.exe's campaign and instant-action tabs.
	void set_title(const Title *title) { title_ = title; }
	// The campaign reader's inputs (DFX2-60): campaign.def's bytes (empty: the mount has none),
	// the menu text table (menutxt.bin, g_TextMenuUi; borrowed, null none) and the current
	// profile's completion bytes. Read at each populate.
	void set_campaign_source(std::string def_text, const rtxt::File *menu_text,
			const campaign::CompletionTable &progress);
	// A CA_LIST pick under the campaign tabs, `row` the list's row: the campaign's briefing,
	// mission list and overlay, ACCEPT off; false for any other list or title.
	// [orig: sub_55FAD0 @0x55fad0, registered on CA_LIST @0x560f25 (dfx2.exe)]
	bool select_campaign(MenuRuntime &menu, int id, int row);
	// A tab radio's activation under the campaign tabs: ACCEPT on when its tab's list
	// (RADIO_TAB_CAMPAIGN's CA_MISSION_LIST, RADIO_TAB_INSTANTACTION's IA_LIST) has a row
	// selected, else off; false for any other control or title.
	// [orig: SinglePlayer_RefreshAcceptOnActivate @0x560710, registered on the two radios with
	//  0 / 1 @0x560ee9 / @0x560f07 (dfx2.exe); Jointops.exe's twin @0x561a20 has no caller]
	bool activate_tab(MenuRuntime &menu, const std::string &name);
	// The campaign list as the last populate read it (DFX2-60).
	const campaign::CampaignList &campaigns() const { return campaigns_; }
	// The image the campaign tab's CAMPAIGN_IMAGE last named: the shown campaign's image, or a
	// picked mission's <name>.tga ("" none).
	const std::string &campaign_image() const { return campaign_image_; }

	// ACCEPT's pick of the Mods list (ModList::pick): `name` the expansion, "" the base game,
	// against `current`, the one running ("" the base game). The same name, without case, takes
	// nothing and drops a request raised before; another is queued for the next update.
	enum class ExpansionPick { Ignored, Queued, NeedsPackedRoot };
	ExpansionPick request_expansion(const std::string &name, const std::string &current,
			bool packed_root);
	bool has_pending_expansion() const { return expansion_pending_; }
	// The request's name ("" the base game), the request dropped; "" with none.
	std::string take_expansion_reload();

private:
	bool is_sp_list(const std::string &name) const;
	bool campaign_tabs() const;
	const MissionChoice *mission_row(int id, int row) const;
	void apply_sp_selection(MenuRuntime &menu, const std::vector<std::string> &briefings,
			const std::string &briefing, bool enabled);
	void set_accept(MenuRuntime &menu, bool enabled);
	void seed_campaign_tabs(MenuRuntime &menu, int id, const std::vector<MissionChoice> &rows);
	void read_campaigns(MenuRuntime &menu);
	void show_campaign(MenuRuntime &menu, size_t campaign);
	void show_overlay(MenuRuntime &menu, bool complete);
	std::vector<std::string> sp_lists_, briefings_, accepts_;
	std::unordered_map<int, std::vector<MissionChoice>> mission_rows_;
	std::string selected_mission_, expansion_request_;
	mission_catalog::CampaignWords selected_campaign_;
	bool expansion_pending_ = false;
	const Title *title_ = nullptr;
	// The campaign tabs' state (DFX2-60): the reader's inputs; the mission table as the populate
	// was handed it, its campaign words kept across populates of the same table (retail's
	// entries keep theirs until the table is built again); the list the reader made; the
	// CA_LIST rows' campaigns and its picked row (the list's own selection, kept as the widget
	// keeps it); the image CAMPAIGN_IMAGE names.
	std::string campaign_def_;
	const rtxt::File *menu_text_ = nullptr;
	campaign::CompletionTable progress_{};
	std::vector<MissionChoice> catalog_;
	campaign::CampaignList campaigns_;
	std::vector<size_t> campaign_rows_;
	int campaign_pick_ = -1;
	std::string campaign_image_;
};

// The armory's PLAYER_CLASS spin: its rows 0..4 each enabled by the host's class mask, bits 5..9 in
// turn, whatever the session [orig: UI_InitTeamClassSelection @ 0x56739e..0x5673f4, ahead of its
// select by value @ 0x567402].
void enable_class_rows(MenuRuntime &menu, int spin, uint32_t class_allow_mask);

// Host-dialog mission rotation. The networking option-value readback stays
// in inmatch/host_settings; this model consumes catalog values and menu state.
class HostDialog {
public:
	void seed(MenuRuntime &menu, const std::vector<MissionChoice> &rows);
	void filter(MenuRuntime &menu);
	void add_selected(MenuRuntime &menu, const hud::GameTextLookup &text);
	void remove_selected(MenuRuntime &menu);
	// START is live while some class word is set and a mission is selected: each gate tests
	// the ten class words first (all 0 disables it), then the SELECTED_MISSIONS rows.
	// [orig: HostDialog_AddRemoveSelectedMissions @0x557c10 - the class words @0x557F24..
	//  0x557F43, the row count @0x557F4E..0x557F5F; HostDialog_OnClassAvailabilityListClick
	//  @0x5577d0; HostDialog_SetAllClassAvailability @0x557a10]
	bool can_start() const { return any_class_ && !selected_.empty(); }
	// The block's ten class words as the gate reads them, the gate run again.
	void set_class_words(MenuRuntime &menu, const int32_t *words, size_t count);
	std::vector<std::string> selected_missions() const;
	// The Switch cell (column 2) of a SELECTED_MISSIONS row: a click toggles
	// the row's launch option, only on a team, non-objective row, and the
	// cell redraws "1" / "0". The option is the catalog row's word, which
	// START hands the rotation with the files (selected_launch_options, one
	// per selected row). [orig: HostDialog_SelectedMissionsTableEvent
	// @0x557FB0 -- the column test @0x557FEF, the eligibility and the toggle
	// @0x558061..0x558086, CTableWnd_SetCellText @0x5580AB]
	void toggle_switch(MenuRuntime &menu, int row);
	std::vector<int32_t> selected_launch_options() const;
	static void select_location(MenuRuntime &menu, int id, const std::string &country);

private:
	void sync_start(MenuRuntime &menu) const;
	std::vector<MissionChoice> pool_;
	std::vector<int> visible_, selected_;
	std::vector<int32_t> launch_options_; // one per selected row
	bool any_class_ = true; // some class word set (the block's ten, set_class_words)

};

} // namespace opennova::menu
