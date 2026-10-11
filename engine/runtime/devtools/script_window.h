// The Script window: the mission scripts' live state over the ScriptSnapshot
// the embedder pushes (mission::script_debug_report, by value) — the WAC
// program and VM (the first compile error, the retail script-state page's
// line, then the diagnostics, the clock and per-event fired state), the BMS
// events' latches, timers and each trigger's last truth, the variable banks
// with inline editing of the mission variables, and the runtime-gap census
// (the script commands the port meets that are not witnessed yet). The script
// pause, a variable edit and an event's Fire leave as the runtime_wac_paused /
// set_mission_variable / fire_event rows.
#pragma once

#include <runtime/devtools/control_board.h>
#include <runtime/devtools/control_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/mission/script_debug_report.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

struct ScriptSnapshot {
	bool valid = false;
	uint64_t logic_tick = 0;
	bool authority = false; // edits land (not a joiner)
	mission::ScriptDebugReport report;
};

class ScriptWindow : public Window {
public:
	static constexpr double kRefreshSeconds = 0.5;

	explicit ScriptWindow(ControlBoard &board) : board_(board) {}

	const char *title() const override { return "Script"; }
	MenuGroup menu_group() const override { return MenuGroup::Sim; }
	WindowSizeHint preferred_size() const override { return {720.0f, 640.0f}; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;
	void wanted_controls(std::vector<const char *> &out) const override;

	void set_snapshot(ScriptSnapshot snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }

	// A mission-variable edit (the set_mission_variable row).
	void request_set_mission_variable(int32_t index, int32_t value);
	// A BMS event's chain reads true at its next run (the fire_event row).
	void request_fire_event(int32_t index);
	bool take_request(ControlRequest &request);

	// The formatted readings, for tests.
	const std::string &summary_text() const { return summary_; }
	const std::string &first_error_text() const { return snapshot_.report.first_error; }
	int gap_count() const { return static_cast<int>(gap_rows_.size()); }
	const char *gap_text(int row) const;
	// One BMS event row's trigger truth as the window shows it: one mark per
	// trigger of its slice, T true, F false, - not evaluated by the last run;
	// "(no triggers)" for an empty chain; "" for a row outside the record.
	std::string bms_truth_text(int row) const;

private:
	void format();
	void draw_variables();

	ControlBoard &board_;
	ScriptSnapshot snapshot_{};
	std::string summary_;
	std::vector<std::string> gap_rows_;
	std::deque<ControlRequest> requests_;
	bool nonzero_only_ = true;
	bool show_globals_ = false;
	int editing_index_ = -1;
	int edit_value_ = 0;
};

}  // namespace opennova::devtools
