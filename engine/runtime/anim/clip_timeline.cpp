#include "clip_timeline.h"

#include <algorithm>
#include <limits>
#include <base/io/tick_rate.h>

namespace opennova::anim {

// [orig: AnimChannel_InitFromData @0x410560, delta store @0x4105BA]
ClipTimeline::ClipTimeline(uint32_t fps, uint32_t frames, bool loop)
	: fps_(fps), frames_(frames), loop_(loop),
	  delta_(frames ? static_cast<float>(double(fps) / io::kTicksPerSecondInt / frames)
	                : 0.0f) {}

// [orig: AnimChannel_AdvancePlayback @0x40B140, clock add @0x40B156, compare @0x40B15E]
bool ClipTimeline::step(float &time) const {
	// Compare the x87 sum before the float store rounds it. Loop subtraction
	// also precedes the store; a one-shot parks and retains its capsule.
	const double next = double(time) + delta_;
	if (next >= 1.0) {
		time = loop_ ? static_cast<float>(next - 1.0) : kPark;
		return true;
	}
	time = static_cast<float>(next);
	return false;
}

void ClipTimeline::extend_to(int32_t ticks) const {
	while (furthest_tick_ < ticks && (loop_ || first_end_tick_ < 0)) {
		const bool ended = step(furthest_time_);
		++furthest_tick_;
		if (ended && first_end_tick_ < 0) first_end_tick_ = furthest_tick_;
		if (furthest_tick_ % kCheckpointTicks == 0)
			checkpoints_.push_back(furthest_time_);
	}
}

float ClipTimeline::normalized_at(int32_t ticks) const {
	if (delta_ <= 0.0f || ticks <= 0) return 0.0f;
	extend_to(ticks);
	if (!loop_ && first_end_tick_ >= 0 && ticks >= first_end_tick_) return kPark;
	if (ticks == furthest_tick_) return furthest_time_;
	const int32_t block = ticks / kCheckpointTicks;
	float time = checkpoints_[static_cast<size_t>(block)];
	for (int32_t i = block * kCheckpointTicks; i < ticks; ++i) step(time);
	return time;
}

// [orig: AnimChannel_AdvancePlayback @0x40B193..0x40B1B1 -- the loop wrap
// overwritten by the 0.99999 park when the end-notify is armed]
float ClipTimeline::normalized_at(int32_t ticks, int32_t armed_boundary) const {
	if (loop_ && delta_ > 0.0f && armed_boundary >= 0 && ticks == armed_boundary) return kPark;
	return normalized_at(ticks);
}

bool ClipTimeline::wrapped_at(int32_t ticks) const {
	if (!loop_ || delta_ <= 0.0f || ticks <= 0) return false;
	float time = normalized_at(ticks - 1);
	return step(time);
}

double ClipTimeline::frame_at(int32_t ticks) const {
	return double(normalized_at(ticks)) * frames_;
}

double ClipTimeline::seconds_at(int32_t ticks) const {
	return fps_ ? frame_at(ticks) / fps_ : 0.0;
}

double ClipTimeline::seconds_at(int32_t ticks, int32_t armed_boundary) const {
	return fps_ ? double(normalized_at(ticks, armed_boundary)) * frames_ / fps_ : 0.0;
}

bool ClipTimeline::stopped_at(int32_t ticks) const {
	normalized_at(ticks);
	return !loop_ && first_end_tick_ >= 0 && ticks >= first_end_tick_;
}

int32_t ClipTimeline::length_ticks() const {
	if (delta_ <= 0.0f) return -1;
	while (first_end_tick_ < 0) {
		if (furthest_tick_ > std::numeric_limits<int32_t>::max() - kCheckpointTicks)
			return -1;
		extend_to(furthest_tick_ + kCheckpointTicks);
	}
	return first_end_tick_;
}

int32_t ClipTimeline::boundary_after(int32_t ticks) const {
	if (delta_ <= 0.0f) return -1;
	if (!loop_) return length_ticks();
	ticks = std::max(ticks, 0);
	float time = normalized_at(ticks);
	while (ticks < std::numeric_limits<int32_t>::max()) {
		++ticks;
		if (step(time)) return ticks;
	}
	return -1;
}

}
