// The Entities window (ADR 0042 d6): the entity directory as a filterable,
// selectable table over the EntityDirectorySnapshot the embedder pushes, with
// the selected row's debug actions (set health, set position, teleport the
// local player here) leaving as typed DebugRequests the embedder drains into
// the engine-backed debug delegates.
//
// The window holds only the pushed value record — it never reaches into a
// live World or into Godot. Visibility-armed: while hidden it drops its
// snapshot and the embedder (gated on GameDevTools::needs_entity_directory)
// stops building new ones, so a closed window costs the producers nothing.
// Rows are formatted once per push (the embedder pushes on the StatsWindow
// 0.5 s cadence, kRefreshSeconds); a frame between pushes only re-emits
// cached strings.
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
	// (pass open && window open): the embedder skips building snapshots
	// nobody shows.
	bool wants_directory() const { return shown_; }

	// The typed request queue the embedder drains (the GameWindowRequest
	// pattern). enqueue_request is the one path the drawn actions feed — and
	// the headless test seam, since clicking a button needs a real backend.
	void enqueue_request(const DebugRequest &request);
	bool take_request(DebugRequest &request);

	// The filtered, formatted table, for tests and probes (the StatsWindow
	// row-text seam): row indices address the rows the table would draw.
	int row_count() const;
	const char *row_name(int row) const;
	const char *row_ai(int row) const;
	const char *row_net_id(int row) const;
	const char *row_team(int row) const;
	const char *row_health(int row) const;
	const char *row_alive(int row) const;
	const char *row_pos(int row) const;
	bool snapshot_valid() const { return snapshot_.valid; }
	uint64_t snapshot_logic_tick() const { return snapshot_.logic_tick; }
	// The name filter (case-insensitive substring over name and SSN); the
	// drawn filter box edits the same buffer.
	void set_filter(const char *text);

private:
	struct RowText {
		std::string name;
		std::array<char, 12> ai{};
		std::array<char, 12> net_id{};
		std::array<char, 12> team{};
		std::array<char, 12> health{};
		std::array<char, 6> alive{};
		std::array<char, 48> pos{};
	};

	void format_rows();
	void apply_filter();
	void select_row(int snapshot_index);
	const world::inspect::EntityRow *selected_row() const;
	void draw_selected_actions();

	EntityDirectorySnapshot snapshot_{};
	std::vector<RowText> texts_;   // one per snapshot row
	std::vector<int> filtered_;    // snapshot-row indices the table draws
	std::array<char, 64> filter_{};
	int selected_ = -1;            // snapshot-row index; -1 = none
	int32_t health_edit_ = 0;
	float pos_edit_[3] = {0.0f, 0.0f, 0.0f};
	float yaw_edit_ = 0.0f;
	float pitch_edit_ = 0.0f;
	bool shown_ = false;
	std::deque<DebugRequest> requests_;
};

}  // namespace opennova::devtools
