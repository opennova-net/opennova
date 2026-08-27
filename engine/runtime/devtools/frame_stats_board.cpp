#include <runtime/devtools/frame_stats_board.h>

namespace opennova::devtools {

namespace {

constexpr int64_t kNoFrame = -1;

int slot_index(Slot slot) {
	const int index = static_cast<int>(slot);
	return (index >= 0 && index < kSlotCount) ? index : -1;
}

}  // namespace

FrameStatsBoard::FrameStatsBoard() {
	reset_window(0);
}

bool FrameStatsBoard::set_capture_active(bool active, uint64_t frame_index) {
	if (active == capture_active_) {
		return false;
	}
	capture_active_ = active;
	reset_window(frame_index);
	return true;
}

void FrameStatsBoard::add(Slot slot, int64_t amount, uint64_t frame_index) {
	if (!capture_active_) {
		return;
	}
	const int index = slot_index(slot);
	if (index < 0) {
		return;
	}
	const int64_t frame = static_cast<int64_t>(frame_index);
	sums_[index] += amount;
	if (last_frame_[index] != frame) {
		last_frame_[index] = frame;
		frame_totals_[index] = amount;
		sample_frames_[index] += 1;
	} else {
		frame_totals_[index] += amount;
	}
	if (frame_totals_[index] > peaks_[index]) {
		peaks_[index] = frame_totals_[index];
	}
}

CaptureWindow FrameStatsBoard::drain(uint64_t frame_index) {
	CaptureWindow window;
	window.frames = frame_index > window_start_frame_ ? frame_index - window_start_frame_ : 0;
	window.sums = sums_;
	window.peaks = peaks_;
	window.sample_frames = sample_frames_;
	reset_window(frame_index);
	return window;
}

void FrameStatsBoard::reset_window(uint64_t frame_index) {
	sums_.fill(0);
	peaks_.fill(0);
	frame_totals_.fill(0);
	last_frame_.fill(kNoFrame);
	sample_frames_.fill(0);
	window_start_frame_ = frame_index;
}

const char *slot_name(Slot slot) {
	static constexpr const char *kNames[] = {
#define OPENNOVA_FRAME_STATS_SLOT_NAME(name, description) #name,
		OPENNOVA_FRAME_STATS_SLOTS(OPENNOVA_FRAME_STATS_SLOT_NAME)
#undef OPENNOVA_FRAME_STATS_SLOT_NAME
	};
	const int index = slot_index(slot);
	return index < 0 ? nullptr : kNames[index];
}

const char *slot_description(Slot slot) {
	static constexpr const char *kDescriptions[] = {
#define OPENNOVA_FRAME_STATS_SLOT_DESCRIPTION(name, description) description,
		OPENNOVA_FRAME_STATS_SLOTS(OPENNOVA_FRAME_STATS_SLOT_DESCRIPTION)
#undef OPENNOVA_FRAME_STATS_SLOT_DESCRIPTION
	};
	const int index = slot_index(slot);
	return index < 0 ? "" : kDescriptions[index];
}

}  // namespace opennova::devtools
