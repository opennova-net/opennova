#pragma once

#include "menu_runtime.h"
#include <runtime/hud/game_text_lookup.h>

namespace opennova::menu {

// Catalog data after the embedder resolves the display label and briefing.
struct MissionChoice {
	std::string file, text, briefing;
	uint32_t game_type = 0;
};

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
	void clear_selected_mission() { selected_mission_.clear(); }

	enum class ExpansionPick { Ignored, Queued, NeedsPackedRoot };
	ExpansionPick request_expansion(const std::string &name, const std::string &current,
			bool packed_root);
	bool has_pending_expansion() const { return !expansion_request_.empty(); }
	std::string take_expansion_reload();

private:
	bool is_sp_list(const std::string &name) const;
	const MissionChoice *mission_row(int id, int row) const;
	void apply_sp_selection(MenuRuntime &menu, const std::string &briefing, bool enabled);
	std::vector<std::string> sp_lists_, briefings_, accepts_;
	std::unordered_map<int, std::vector<MissionChoice>> mission_rows_;
	std::string selected_mission_, expansion_request_;
};

// Host-dialog mission rotation. The networking option-value readback stays
// in inmatch/host_settings; this model consumes catalog values and menu state.
class HostDialog {
public:
	void seed(MenuRuntime &menu, const std::vector<MissionChoice> &rows);
	void filter(MenuRuntime &menu);
	void add_selected(MenuRuntime &menu, const hud::GameTextLookup &text);
	void remove_selected(MenuRuntime &menu);
	bool can_start() const { return !selected_.empty(); }
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

};

} // namespace opennova::menu
