// The per-system frame-stats accumulator behind the dev tools' Stats window
// (ADR 0039). Producers feed fixed integer slots (microseconds, or plain
// counts for the VALUE slots) every frame while capture is active; the Stats
// window drains a multi-frame window at its refresh cadence and shows
// mean-per-frame + worst-frame numbers.
//
// Cost contract: while capture is inactive every feed site guards on it — no
// clock reads, no writes. While active, a feed is one add() into fixed arrays:
// no allocation anywhere on the hot path. Window math (means, copies) runs only
// at the window's refresh cadence.
//
// The board is a plain value: one per owner (the shell's FrameStats binding,
// a test), never a process-wide object. The engine has no frame counter, so
// the render-frame index every call keys on is a parameter.
#pragma once

#include <runtime/devtools/frame_stats_slots.h>

#include <array>
#include <cstdint>

namespace opennova::devtools {

// One drained window: the sums/peaks/sample counts of every slot plus the
// number of render frames the window spanned.
struct CaptureWindow {
	uint64_t frames = 0;
	std::array<int64_t, kSlotCount> sums{};
	std::array<int64_t, kSlotCount> peaks{};
	std::array<int32_t, kSlotCount> sample_frames{};
};

class FrameStatsBoard {
public:
	FrameStatsBoard();

	// Open or close the one capture window. Returns true when the state
	// changed (the edge observers arm/disarm their auxiliary diagnostics on).
	bool set_capture_active(bool active, uint64_t frame_index);
	bool is_capture_active() const { return capture_active_; }

	// Hot-path feed: amount is microseconds (or a count for the VALUE slots).
	// Callers still guard on is_capture_active() before taking timestamps;
	// this defensive gate prevents a stale producer from contaminating a
	// closed window. Multiple writes to one slot in one render frame are
	// summed before the peak is compared, so "peak" really means the worst
	// render frame.
	void add(Slot slot, int64_t amount, uint64_t frame_index);

	// Atomically copy and reset the current capture window. The copies happen
	// only at the reader's refresh cadence, never on producer paths.
	CaptureWindow drain(uint64_t frame_index);

private:
	void reset_window(uint64_t frame_index);

	std::array<int64_t, kSlotCount> sums_{};
	std::array<int64_t, kSlotCount> peaks_{};
	std::array<int64_t, kSlotCount> frame_totals_{};
	std::array<int64_t, kSlotCount> last_frame_{};
	std::array<int32_t, kSlotCount> sample_frames_{};
	uint64_t window_start_frame_ = 0;
	bool capture_active_ = false;
};

}  // namespace opennova::devtools
