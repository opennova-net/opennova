// The Weapon window: a DCC-style editor over the equipped weapon's twelve
// ACTION slots (ADR 0042 d6 — pushed records in, typed requests out).
//
// Two stacked panes over one shared tick axis:
//   * the DOPE SHEET, where each action is a strip [delaystart | delayend] you
//     retime by dragging its arbiter divider or its right edge, with the
//     resolved anim clip as a ghost bar and the sound/particle legs as markers;
//   * the TRACE, the FSM as it actually ran, recorded at the 62.5 Hz pump tick
//     so a 1-tick SWITCHTO or RECOIL's zero-length tail cannot fall between two
//     display frames.
//
// Edits are LIVE ONLY: they patch the running weapon and never reach disk. The
// window holds only the pushed value records and never reaches into a live
// World or into Godot. While hidden it drops its live record and the embedder
// (gated on GameDevTools::needs_weapon_snapshot) stops building one; the
// scrollback stays, and the engine's 512-tick ring keeps recording as long as
// REC is on, so closing F3 to shoot and reopening shows the burst.
#pragma once

#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/weapon_action_snapshot.h>
#include <runtime/devtools/weapon_request.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace opennova::devtools {

class WeaponWindow : public Window {
public:
	// Trace samples the window keeps for scrollback: ~16 s at 62.5 Hz, twice
	// the engine ring, so a paused reader can page back past what the ring
	// itself still holds.
	static constexpr size_t kTraceCapacity = 1024;

	const char *title() const override { return "Weapon"; }
	// Deliberately undocked: the Right dock is 30% of the viewport and a
	// timeline wants width. It floats at a size the user then owns.
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::None;
	}
	WindowSizeHint preferred_size() const override { return WindowSizeHint{1040.0f, 760.0f}; }
	bool is_scrollable() const override { return false; }  // the panes scroll themselves
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// --- the pushed records ---
	// An invalid snapshot clears the window (no weapon, or the world unloaded).
	void set_snapshot(WeaponActionSnapshot snapshot);
	void set_catalog(WeaponCatalog catalog);
	// (pass open && window open): the embedder skips building records nobody shows.
	bool wants_snapshot() const { return shown_; }
	uint64_t catalog_serial() const { return catalog_.serial; }

	// --- the typed request queue the embedder drains ---
	// enqueue_request is the one path the drawn controls feed, and the headless
	// test seam, since clicking a button needs a real backend.
	void enqueue_request(const WeaponRequest &request);
	bool take_request(WeaponRequest &request);

	// --- read seams for tests and probes ---
	bool snapshot_valid() const { return snapshot_.valid; }
	const char *weapon_name() const { return snapshot_.weapon_name.c_str(); }
	int selected_action() const { return selected_; }
	void select_action(int action_id);
	// "0 / 5" for explicit delays, "0 / auto(31)" where the row authored `auto`.
	const char *action_timing(int action_id) const;
	// "FIRE", "SCOPEUP (not authored)", ...
	const char *action_label(int action_id) const;
	int trace_count() const { return static_cast<int>(trace_.size()); }
	// "8803 RECOIL ACTIVE c3 fired"
	const char *trace_row(int index) const;
	bool is_recording() const { return recording_; }
	// The REC switch as the checkbox sets it (the test seam for the hide policy).
	void set_recording_for_test(bool recording) {
		rec_touched_ = true;
		set_recording(recording);
	}

private:
	struct RowText {
		std::string label;
		std::string timing;
	};

	// The shared tick axis. `pixels_per_tick` is the zoom; `origin_tick` the
	// leftmost tick drawn.
	struct Axis {
		float pixels_per_tick = 6.0f;
		float origin_tick = 0.0f;
	};

	void format_rows();
	void draw_header();
	void draw_transport();
	void draw_dope_sheet(float height);
	void draw_trace(float height);
	void draw_properties();
	// The shared two-line ruler: absolute ticks over ms measured from
	// `ms_zero_tick` (0 for the definition, the newest tick for the trace).
	void draw_ruler(float x0, float x1, float y, const Axis &axis, double ms_zero_tick);
	// Wheel zoom about the cursor / middle- or right-drag pan over a pane's
	// rect; true when the user moved the axis this frame.
	bool handle_axis_input(float x0, float x1, Axis &axis, bool clamp_to_zero);
	// View All / View Selected: fit every strip (and clip ghost), or frame one.
	void fit_dope_sheet(float width);
	void frame_action(float width, int action_id);
	void fit_trace(float width);

	void queue_delays(int action_id, int32_t delay_start, int32_t delay_end);
	void queue_text(int action_id, WeaponRequest::TextField field, const char *text);
	void queue_trigger(WeaponRequest::Trigger trigger);
	void set_recording(bool recording);

	WeaponActionSnapshot snapshot_{};
	WeaponCatalog catalog_{};
	std::array<RowText, world::weapon_action::kCount> texts_{};
	std::deque<world::WeaponTraceSample> trace_;
	std::deque<WeaponRequest> requests_;

	Axis def_axis_{};
	Axis trace_axis_{};
	bool fit_pending_ = true;        // fit the dope sheet on the next layout
	bool fit_trace_pending_ = false; // fit the whole scrollback on the next layout
	bool trace_follow_ = true;       // the trace pane rides the newest tick
	bool recording_ = false;
	bool rec_touched_ = false;  // REC set by hand: showing no longer re-arms it
	bool shown_ = false;
	int selected_ = world::weapon_action::kFire;
	// The live drag: which handle on which action, and the value it started at.
	int drag_action_ = -1;
	int drag_handle_ = 0;        // 1 = arbiter divider, 2 = right edge
	int32_t drag_start_value_ = 0;
	int32_t drag_start_other_ = 0;
	float drag_accum_ = 0.0f;
	// The trace ruler's measure drag, in absolute ticks.
	double measure_from_ = 0.0;
	double measure_to_ = 0.0;
	bool measuring_ = false;
	bool measure_valid_ = false;
	// Scratch edit buffers for the properties panel's five name fields, in
	// WeaponRequest::TextField order. Reseeded from the snapshot on every frame
	// the field is not the active ImGui item, so an external change shows up
	// without stealing what the user is mid-way through typing.
	std::array<std::array<char, 128>, 5> field_buf_{};
	// Whether each field owned the keyboard on the previous frame (the public
	// IsItemActive answer, which only exists after the widget is submitted).
	std::array<bool, 5> field_active_{};
	mutable std::string scratch_;
};

}  // namespace opennova::devtools
