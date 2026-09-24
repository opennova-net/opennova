// The AI window (ADR 0042 d6): the AI system's debug surface — the system
// counters, the selected brain's deep pane, the TriggerRelations group table
// and the nav-channel table — over the AiDebugSnapshot the embedder pushes.
// There is no second entity list: the window reads the Entities window's
// selection (the EntityPropertiesWindow pattern) and its deep pane rides the
// same EntityDetailSnapshot push, so a world pick lands here too.
//
// The window holds only pushed value records — it never reaches into a live
// World or into Godot. Visibility-armed: while hidden it drops its records
// and the embedder (gated on GameDevTools::needs_ai_debug) stops building new
// ones — unless one of its Game-view layers (ai_overlay.h: labels, routes,
// targets, perception rings) is on, which keeps the record flowing every
// logic tick. Rows are formatted once per push; a frame between pushes only
// re-emits cached strings.
#pragma once

#include <runtime/devtools/ai_debug_snapshot.h>
#include <runtime/devtools/ai_overlay.h>
#include <runtime/devtools/entity_detail_snapshot.h>
#include <runtime/devtools/imgui_pass.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::devtools {

class EntitiesWindow;

class AiWindow : public Window {
public:
	// Seconds per pushed snapshot: counters/groups/routes read fine at the
	// Stats/Entities cadence.
	static constexpr double kRefreshSeconds = 0.5;

	explicit AiWindow(EntitiesWindow &entities);

	const char *title() const override { return "AI"; }
	MenuGroup menu_group() const override { return MenuGroup::World; }
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
	// Every brain of the report, one row each ("name | state | alert |
	// target | route").
	int brain_count() const { return static_cast<int>(brain_rows_.size()); }
	const char *brain_text(int row) const;

	// The Game-view layers (labels, routes, targets, rings), registered on
	// the pass by the composer.
	static constexpr int kLayerCount = 4;
	AiOverlayLayer &layer(int index) { return *layers_[static_cast<size_t>(index)]; }
	const AiOverlayLayer &layer(int index) const { return *layers_[static_cast<size_t>(index)]; }
	bool any_layer_enabled() const;
	const AiDebugSnapshot &snapshot() const { return snapshot_; }

private:
	void format_snapshot();
	void format_detail();
	void draw_detail_pane();
	void draw_tables();

	EntitiesWindow &entities_;
	AiDebugSnapshot snapshot_{};
	EntityDetailSnapshot detail_{};
	bool shown_ = false;

	std::array<std::unique_ptr<AiOverlayLayer>, kLayerCount> layers_;
	std::string counters_;
	std::vector<std::string> brain_rows_;
	std::vector<int> brain_alerts_;
	std::vector<std::string> group_rows_;
	std::vector<int> group_alerts_; // per group row, for the colored draw
	std::vector<std::string> channel_rows_;
	std::vector<std::string> detail_lines_;
};

}  // namespace opennova::devtools
