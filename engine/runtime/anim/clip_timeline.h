#pragma once

#include <cstdint>
#include <vector>

namespace opennova::anim {

// Retail's float channel clock, addressed by elapsed simulation ticks.
// Sparse checkpoints let shared clips answer independent entity playheads and
// seeks without replacing repeated float addition with a different rounding law.
// [orig: AnimChannel_InitFromData @0x410560; AnimChannel_AdvancePlayback @0x40b140]
class ClipTimeline {
public:
	ClipTimeline() = default;
	ClipTimeline(uint32_t fps, uint32_t frames, bool loop);
	bool matches(uint32_t fps, uint32_t frames, bool loop) const {
		return fps_ == fps && frames_ == frames && loop_ == loop;
	}
	float normalized_at(int32_t ticks) const;
	// The armed-wrap park: with the end-notify armed (flag 0x40000) a loop wraps
	// and is re-parked at 0.99999 in the SAME tick, latching 0x20000 but never
	// the 0x10000 stop, so the boundary tick samples the clip end
	// (rec[frames-1]..rec[frames], trigger[frames-1]) and the consumer promotes
	// its deferred state on the next tick. `armed_boundary` is the tick
	// boundary_after() reported; -1 = unarmed. One-shots park on their own end
	// regardless. [orig: AnimChannel_AdvancePlayback @0x40B193 (0x40000 test),
	// wrap @0x40B199, re-park @0x40B1A2..0x40B1B1 (0x20000 | flt_7C327C)]
	float normalized_at(int32_t ticks, int32_t armed_boundary) const;
	double frame_at(int32_t ticks) const;
	double seconds_at(int32_t ticks) const;
	bool stopped_at(int32_t ticks) const;
	int32_t length_ticks() const;
	int32_t boundary_after(int32_t ticks) const;
	static constexpr float kPark = 0.99999f;

private:
	bool step(float &time) const;
	void extend_to(int32_t ticks) const;
	uint32_t fps_ = 0;
	uint32_t frames_ = 0;
	bool loop_ = false;
	float delta_ = 0.0f;
	static constexpr int32_t kCheckpointTicks = 128;
	mutable std::vector<float> checkpoints_{0.0f};
	mutable int32_t furthest_tick_ = 0;
	mutable float furthest_time_ = 0.0f;
	mutable int32_t first_end_tick_ = -1;
};
}
