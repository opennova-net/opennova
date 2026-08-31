// The Rays window (ADR 0042 d6): the engine ray-debug capture's control
// surface over the RaysSnapshot the embedder pushes — recording state,
// per-category counts (held in ring / lifetime total) with draw-filter
// checkboxes, the fade TTL, and Clear — leaving typed RaysRequests the
// embedder drains (filter/TTL/clear into the Simulation's ray-debug seam,
// the view toggle out to the shell, which owns building the 3D view).
//
// The window holds only the pushed value record — it never reaches into a
// live World. Visibility-armed: while hidden it drops its snapshot and the
// embedder (gated on GameDevTools::needs_rays_snapshot) stops building new
// ones.
#pragma once

#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/rays_request.h>
#include <runtime/devtools/rays_snapshot.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>

namespace opennova::devtools {

class RaysWindow : public Window {
public:
	// Seconds per pushed snapshot — counts move every tick, the Environment
	// window's cadence reads comfortably.
	static constexpr double kRefreshSeconds = 0.25;

	const char *title() const override { return "Rays"; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::Right;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed record, by value; an invalid snapshot clears the page.
	void set_snapshot(const RaysSnapshot &snapshot);
	// (pass open && window open): the embedder skips building snapshots
	// nobody shows.
	bool wants_snapshot() const { return shown_; }

	// The typed request queue the embedder drains. enqueue_request is the one
	// path the drawn controls feed — and the headless test seam.
	void enqueue_request(const RaysRequest &request);
	bool take_request(RaysRequest &request);

	// The formatted category rows, for tests and probes (the StatsWindow
	// row-text seam): "Name: held N / total M".
	int row_count() const;
	const char *row_text(int row) const;
	bool snapshot_valid() const { return snapshot_.valid; }

private:
	void format_rows();

	RaysSnapshot snapshot_{};
	std::array<std::string, kRayCategoryCount> rows_{};
	bool shown_ = false;
	std::deque<RaysRequest> requests_;
	// Edit state mirrored from every push (these controls display authoritative
	// state; a click flips locally + queues the request, the next push confirms).
	bool view_edit_ = false;
	uint32_t mask_edit_ = 0x7FFF;
	int32_t ttl_edit_ = 93;
};

}  // namespace opennova::devtools
