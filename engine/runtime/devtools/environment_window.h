// The Environment window (ADR 0042 d6): the retail environment debug page's
// rows [orig: Debug_DrawEnvironmentValues @ 0x4ef000 — "Script & Env Values",
// two columns at x 10 / 200] over the EnvironmentSnapshot the embedder pushes,
// plus the live weather state the page does not print (the clock, the ramp
// targets, the lightning timers), and a control strip whose every action is
// one of the WAC weather commands or the exact clock/wind/sky-height rows,
// leaving as ControlRequests the embedder drains into the ONE debug-control
// table (ADR 0043 d12): its environment_* rows are the same rows MCP's
// game_debug invokes, and they reach the ONE command layer
// (world::EntityCommands) every WAC handler uses.
//
// The window holds only the pushed value record — it never reaches into a
// live World or into Godot. Visibility-armed: while hidden it drops its
// snapshot and the embedder (gated on GameDevTools::needs_environment_snapshot)
// stops building new ones. Rows are formatted once per push (the 0.25 s
// cadence below); a frame between pushes only re-emits cached strings.
#pragma once

#include <runtime/devtools/control_request.h>
#include <runtime/devtools/environment_snapshot.h>
#include <runtime/devtools/imgui_pass.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>

namespace opennova::devtools {

class EnvironmentWindow : public Window {
public:
	// Seconds per pushed snapshot: the springs move per tick, so the page
	// refreshes twice as often as the Stats/Entities readings.
	static constexpr double kRefreshSeconds = 0.25;
	// The retail page's row labels, in its two-column order.
	static constexpr int kRowCount = 28;
	// The live weather rows beyond the retail page, drawn under it.
	static constexpr int kExtraRowCount = 8;

	const char *title() const override { return "Environment"; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::Right;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed record, by value; an invalid snapshot clears the page.
	void set_snapshot(const EnvironmentSnapshot &snapshot);

	// The control-request queue the embedder drains into the debug-control
	// table. enqueue_request is the one path the drawn controls feed — and
	// the headless test seam.
	void enqueue_request(const ControlRequest &request);
	bool take_request(ControlRequest &request);

	// The formatted page, for tests and probes (the StatsWindow row-text
	// seam): row i is the i-th retail label, "Label: value".
	int row_count() const;
	const char *row_text(int row) const;
	// The live weather rows beyond the retail page ("Label: value").
	int extra_row_count() const;
	const char *extra_row_text(int row) const;
	bool snapshot_valid() const { return snapshot_.valid; }

	// The control strip's edit seeds (tests read what a click would send).
	int32_t rain_percent_edit() const { return rain_pct_edit_; }
	int32_t sun_fade_percent_edit() const { return sun_fade_pct_edit_; }
	int32_t quake_edit() const { return quake_seconds_edit_; }
	int32_t sky_speed_edit() const { return sky_speed_edit_; }
	int32_t sky_height_edit() const { return sky_height_edit_; }
	int32_t wind_percent_edit() const { return wind_pct_edit_; }

	// The strip's rows beyond the WAC buttons, by what they send: the sky
	// height in whole metres (the row takes the raw 16.16 target), the exact
	// mission-clock scrub by minute of day, the wind as a percent of the
	// retail 256 default, and the weather-home read (its result is the
	// command's reported result).
	void request_sky_height(int32_t metres);
	void request_clock_scrub(int32_t minute_of_day);
	void request_wind_strength(int32_t percent);
	void request_weather_snapshot();

private:
	void format_rows();
	void draw_rows();
	void draw_controls();

	EnvironmentSnapshot snapshot_{};
	std::array<std::string, kRowCount> rows_{};
	std::array<std::string, kExtraRowCount> extra_rows_{};
	std::deque<ControlRequest> requests_;
	// The picker buffers of the color rows: a row follows its snapshot swatch
	// until its picker opens, then the picker owns it until it closes.
	std::array<std::array<float, 3>, kRowCount> color_edit_{};
	// The control strip's edit state, seeded from the pushed record on the
	// first valid push so an untouched Apply is a no-op-shaped write.
	bool seeded_ = false;
	int32_t rain_pct_edit_ = 0;
	int32_t overcast_pct_edit_ = 0;
	int32_t seconds_edit_ = 5;
	int32_t fog_metres_edit_ = 1000;
	int32_t sky_speed_edit_ = 0;
	int32_t sky_height_edit_ = 0;
	int32_t quake_seconds_edit_ = 5;
	int32_t minute_edit_ = 720;
	int32_t fog_type_edit_ = 2;
	int32_t sun_fade_pct_edit_ = 0;
	int32_t color_fade_seconds_edit_ = 0;
	int32_t wind_edit_ = 256;
	int32_t wind_pct_edit_ = 100;
};

}  // namespace opennova::devtools
