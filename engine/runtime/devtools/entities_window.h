// The Entities window (ADR 0042 d6): the entity directory as a filterable,
// selectable table over the EntityDirectorySnapshot the embedder pushes, and
// the selection every other entity surface keys on. The selected row's card,
// its debug actions and its items.def attrib toggles live in the separate,
// separately dockable Entity Properties window (entity_properties_window.h),
// which reads this window's selection and the snapshot's authority fact.
//
// The window holds only the pushed value record — it never reaches into a
// live World or into Godot. The shell's world pick reaches it as a selection
// request carrying only the engine handle (select_handle): the window opens,
// asks for focus, selects the row when the pushed directory has it, and
// otherwise keeps the handle pending for the next push. Visibility-armed:
// while hidden it drops its snapshot (the selection survives as a pending
// handle so reopening re-selects the same entity), and the embedder (gated on
// GameDevTools::needs_entity_directory, which the Properties window keeps
// armed too) stops building new ones once no entity window shows. Rows are
// formatted once per push (the embedder pushes on the StatsWindow 0.5 s
// cadence, kRefreshSeconds); a frame between pushes only re-emits cached
// strings.
#pragma once

#include <runtime/devtools/debug_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/entity_directory_snapshot.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

class EntitiesWindow : public Window {
public:
	// Seconds per pushed snapshot: the same reading cadence as the Stats
	// window, held by the feeder (the window formats only when a push lands).
	static constexpr double kRefreshSeconds = 0.5;

	const char *title() const override { return "Entities"; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::Right;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed directory record, by value; an invalid snapshot clears the
	// table (the world unloaded).
	void set_directory(EntityDirectorySnapshot snapshot);
	// The pushed authority facts (false until a valid push): this peer owns
	// the world; a wire session is live under it.
	bool authority() const { return snapshot_.valid && snapshot_.authority; }
	bool session_live() const { return snapshot_.valid && snapshot_.session_live; }

	// The selection seam. select_handle is what a world pick lands as: the
	// window opens and asks for focus; when the pushed directory has the row
	// the filter clears (a hidden row cannot be scrolled to) and the row
	// selects and scrolls into view, else the handle stays pending until a
	// push carries it (a push without it drops it: the entity is gone).
	// selected_handle reads the selected row's handle, else the pending one,
	// else kInvalid.
	void select_handle(uint16_t handle);
	void clear_selection();
	uint16_t selected_handle() const;
	// The selected row of the held snapshot; null while none or pending.
	const world::inspect::EntityRow *selected_row() const;
	uint16_t pending_select_handle() const { return pending_select_handle_; }
	bool wants_scroll_to_selected() const { return scroll_to_selected_; }

	// The typed request queue the embedder drains (the GameWindowRequest
	// pattern); the Entity Properties window queues its actions here too, so
	// one drain serves both. enqueue_request is the one path the drawn
	// actions feed — and the headless test seam, since clicking a button
	// needs a real backend.
	void enqueue_request(const DebugRequest &request);
	bool take_request(DebugRequest &request);

	// The filtered, formatted table, for tests and probes (the StatsWindow
	// row-text seam): row indices address the rows the table would draw.
	int row_count() const;
	const char *row_name(int row) const;
	const char *row_item(int row) const;
	const char *row_ai(int row) const;
	const char *row_net_id(int row) const;
	const char *row_team(int row) const;
	const char *row_health(int row) const;
	const char *row_alive(int row) const;
	const char *row_pos(int row) const;
	bool snapshot_valid() const { return snapshot_.valid; }
	uint64_t snapshot_logic_tick() const { return snapshot_.logic_tick; }
	// The name filter (case-insensitive substring over name, item name and
	// SSN); the drawn filter box edits the same buffer.
	void set_filter(const char *text);
	const char *filter() const { return filter_.data(); }

private:
	struct RowText {
		std::string name;
		std::string item;
		std::array<char, 12> ai{};
		std::array<char, 12> net_id{};
		std::array<char, 12> team{};
		std::array<char, 12> health{};
		std::array<char, 6> alive{};
		std::array<char, 48> pos{};
	};

	// Format the held snapshot's rows and re-key the selection onto
	// `keep_handle` (kInvalid = no selection; a handle the snapshot lacks
	// leaves none, the entity is gone).
	void format_rows(uint16_t keep_handle);
	void apply_filter();
	void apply_pending_selection();
	void select_row(int snapshot_index);
	int row_index_for_handle(uint16_t handle) const;

	EntityDirectorySnapshot snapshot_{};
	std::vector<RowText> texts_;   // one per snapshot row
	std::vector<int> filtered_;    // snapshot-row indices the table draws
	std::array<char, 64> filter_{};
	int selected_ = -1;            // snapshot-row index; -1 = none
	uint16_t pending_select_handle_ = world::EntityHandle::kInvalid;
	bool scroll_to_selected_ = false;
	bool shown_ = false;
	std::deque<DebugRequest> requests_;
};

}  // namespace opennova::devtools
