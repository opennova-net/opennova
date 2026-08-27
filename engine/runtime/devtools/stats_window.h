// The Stats window (ADR 0039): one row per measured runtime system with
// window-averaged per-frame milliseconds (mean + worst frame in the window)
// and live counters beside them. The numbers come from the FrameStatsBoard the
// shell and the engine feed; this window only opens/closes the capture window
// and formats what accumulated.
//
// Capture is edge-gated: the board captures only while this window is visible
// inside an open pass, so closed tools cost the producers nothing. Rows read
// from the fixed table in stats_window_rows.h; every read/format runs at the
// refresh cadence (0.5 s windows), never per frame — a frame between refreshes
// only re-emits cached strings.
#pragma once

#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/frame_stats_board.h>

#include <array>
#include <cstdint>
#include <vector>

namespace opennova::devtools {

class StatsWindow : public Window {
public:
	// Seconds per reading: wide enough that two consecutive readings of a
	// steady scene agree (the retired page's 0.25 s timer divided by two).
	static constexpr double kRefreshSeconds = 0.5;

	StatsWindow();

	const char *title() const override { return "Stats"; }
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The board this window drains (owned by the embedder; may be null).
	void set_board(FrameStatsBoard *board);

	// A reading supplied from outside: a probe that drains the board itself
	// (per frame, for its own accounting) re-accumulates its drains and feeds
	// the window this way; the window then never drains the board.
	void feed_external(const CaptureWindow &window);

	// The last reading's row texts, for tests and probes.
	int row_count() const;
	const char *row_id(int row) const;
	const char *row_average(int row) const;
	const char *row_peak(int row) const;
	const char *row_info(int row) const;
	uint64_t reading_frames() const { return reading_.frames; }

private:
	struct RowText {
		std::array<char, 24> average{};
		std::array<char, 24> peak{};
		std::array<char, 96> info{};
	};

	void apply_reading(const CaptureWindow &window);
	void format_rows();
	void format_info();
	void set_info(const char *row_id, const char *text);
	int draw_rows(int index, int depth);

	FrameStatsBoard *board_ = nullptr;
	CaptureWindow reading_;
	std::vector<RowText> texts_;
	std::array<float, 120> frame_history_{};
	int frame_history_next_ = 0;
	int frame_history_count_ = 0;
	double last_refresh_time_ = -1.0;
	uint64_t last_frame_index_ = 0;
	bool external_ = false;
	bool shown_ = false;
};

}  // namespace opennova::devtools
