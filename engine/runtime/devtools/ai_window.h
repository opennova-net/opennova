// The AI window (ADR 0042 d6): the AI system's debug surface — the overlay
// toggle strip driving the shell's world-parented AI debug view, the
// system counters, the selected brain's deep pane, the TriggerRelations
// group table and the nav-channel table — over the AiDebugSnapshot the
// embedder pushes. There is no second entity list: the window reads the
// Entities window's selection (the EntityPropertiesWindow pattern) and its
// deep pane rides the same EntityDetailSnapshot push, so a world pick lands
// here too.
//
// The window holds only pushed value records — it never reaches into a live
// World or into Godot. Toggles are requests-out/pushed-truth: a click queues
// one AiViewRequest and the checkbox follows the overlay state of the next
// snapshot (the embedder re-pushes immediately after draining), so the strip
// stays consistent when MCP flips the same debug-control rows. Visibility-
// armed: while hidden it drops its records and the embedder (gated on
// GameDevTools::needs_ai_debug) stops building new ones. Rows are formatted
// once per push; a frame between pushes only re-emits cached strings.
#pragma once

#include <runtime/devtools/ai_debug_snapshot.h>
#include <runtime/devtools/ai_view_request.h>
#include <runtime/devtools/entity_detail_snapshot.h>
#include <runtime/devtools/imgui_pass.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

class EntitiesWindow;

class AiWindow : public Window {
public:
	// Seconds per pushed snapshot: counters/groups/routes read fine at the
	// Stats/Entities cadence (the world overlay refreshes per frame on the
	// shell side, not through this record).
	static constexpr double kRefreshSeconds = 0.5;

	explicit AiWindow(EntitiesWindow &entities) : entities_(entities) {}

	const char *title() const override { return "AI"; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::RightBottom;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed record, by value; an invalid snapshot clears the window.
	void set_snapshot(AiDebugSnapshot snapshot);
	bool snapshot_valid() const { return snapshot_.valid; }

	// The pushed detail record for the Entities selection (the same push the
	// Entity Properties window receives): accepted only when its card names
	// the selected handle; an invalid card clears the pane.
	void set_detail(EntityDetailSnapshot detail);
	void clear_detail();
	bool detail_valid() const;

	// The overlay toggle seam. toggle_element is what a drawn checkbox calls —
	// and the headless test seam (clicking needs a real backend): it queues
	// one typed request and changes no local state. element_enabled reads the
	// pushed truth.
	void toggle_element(AiViewRequest::Element element, bool enabled);
	bool take_request(AiViewRequest &request);
	bool overlay_available() const { return snapshot_.valid && snapshot_.overlay.available; }
	bool element_enabled(AiViewRequest::Element element) const;

	// The formatted readings, for tests and probes (the StatsWindow row-text
	// seam).
	const char *counters_text() const { return counters_.c_str(); }
	int group_count() const { return static_cast<int>(group_rows_.size()); }
	const char *group_text(int row) const;
	int channel_count() const { return static_cast<int>(channel_rows_.size()); }
	const char *channel_text(int row) const;
	// The deep pane: one line per formatted fact; a "[Section]" line renders
	// as a separator header.
	int detail_line_count() const { return static_cast<int>(detail_lines_.size()); }
	const char *detail_line(int row) const;

private:
	void format_snapshot();
	void format_detail();
	void draw_overlay_strip();
	void draw_detail_pane();
	void draw_tables();

	EntitiesWindow &entities_;
	AiDebugSnapshot snapshot_{};
	EntityDetailSnapshot detail_{};
	bool shown_ = false;
	std::deque<AiViewRequest> requests_;

	std::string counters_;
	std::vector<std::string> group_rows_;
	std::vector<int> group_alerts_; // per group row, for the colored draw
	std::vector<std::string> channel_rows_;
	std::vector<std::string> detail_lines_;
};

}  // namespace opennova::devtools
