// The Physics window (ADR 0042 d6): the collision debug view's control
// surface over the PhysicsSnapshot the embedder pushes — the "Show collision"
// overlay toggle (wireframe boxes for what movement actually resolves
// against), the contact capture that flashes those boxes on hits, per-kind
// counts (held in ring / lifetime total) with draw-filter checkboxes, and
// Clear — leaving typed PhysicsRequests the embedder drains (mask/clear/
// capture into the Simulation contact-debug seam, the view toggle out to the
// shell, which owns building the 3D view).
//
// The window holds only the pushed value record — it never reaches into a
// live World. Visibility-armed: while hidden it drops its snapshot and the
// embedder (gated on GameDevTools::needs_physics_snapshot) stops building
// new ones.
#pragma once

#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/physics_request.h>
#include <runtime/devtools/physics_snapshot.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>

namespace opennova::devtools {

class PhysicsWindow : public Window {
public:
	// Seconds per pushed snapshot (the Rays window's cadence).
	static constexpr double kRefreshSeconds = 0.25;

	const char *title() const override { return "Physics"; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::Right;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed record, by value; an invalid snapshot clears the page.
	void set_snapshot(const PhysicsSnapshot &snapshot);
	// (pass open && window open): the embedder skips building snapshots
	// nobody shows.
	bool wants_snapshot() const { return shown_; }

	// The typed request queue the embedder drains. enqueue_request is the one
	// path the drawn controls feed — and the headless test seam.
	void enqueue_request(const PhysicsRequest &request);
	bool take_request(PhysicsRequest &request);

	// The formatted kind rows, for tests and probes (the RaysWindow row-text
	// seam): "Name: held N / total M".
	int row_count() const;
	const char *row_text(int row) const;
	bool snapshot_valid() const { return snapshot_.valid; }

private:
	void format_rows();

	PhysicsSnapshot snapshot_{};
	std::array<std::string, kContactKindCount> rows_{};
	bool shown_ = false;
	std::deque<PhysicsRequest> requests_;
	// Edit state mirrored from every push (these controls display authoritative
	// state; a click flips locally + queues the request, the next push confirms).
	bool view_edit_ = false;
	bool capture_edit_ = false;
	uint32_t mask_edit_ = 0x3F;
};

}  // namespace opennova::devtools
