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

double ClipTimeline::frame_of(float time) const {
	return std::clamp(double(time) * frames_, 0.0, double(frames_));
}

double ClipTimeline::frame_at(int32_t ticks, int32_t armed_boundary) const {
	return frame_of(normalized_at(ticks, armed_boundary));
}

int32_t ClipTimeline::frame_index_at(int32_t ticks, int32_t armed_boundary) const {
	return static_cast<int32_t>(frame_at(ticks, armed_boundary));
}

std::vector<int32_t> ClipTimeline::first_ticks() const {
	std::vector<int32_t> out(frames_, -1);
	if (frames_ == 0) return out;
	// A zero step holds frame 0 without stopping.
	if (delta_ <= 0.0f) {
		out[0] = 0;
		return out;
	}
	// The clock stepped from tick 0 as normalized_at steps it, up to the tick it stops
	// (a one-shot) or wraps (a loop): the first pass, where the frame only rises.
	float time = 0.0f;
	for (int32_t tick = 0; tick < std::numeric_limits<int32_t>::max(); ++tick) {
		const auto at = static_cast<uint32_t>(frame_of(time));
		if (at < frames_ && out[at] < 0) out[at] = tick;
		if (step(time)) break;
	}
	return out;
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

uint32_t clip_trigger_at(const ClipTimeline &clock, const std::vector<uint32_t> &triggers, int32_t ticks,
                         int32_t armed_boundary) {
	if (clock.stopped_at(ticks)) return 0;
	const int32_t frame = clock.frame_index_at(ticks, armed_boundary);
	return frame >= 0 && static_cast<size_t>(frame) < triggers.size() ? triggers[static_cast<size_t>(frame)] : 0u;
}

}
