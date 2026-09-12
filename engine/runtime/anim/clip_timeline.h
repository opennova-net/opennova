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
