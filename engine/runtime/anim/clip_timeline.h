#pragma once

#include <cstdint>
#include <vector>

namespace opennova::anim {

// A loop wrap that serves another entry of the slot's ring fades the served
// entry in over eight ticks from weight 0, a step of 1/8 a tick: the re-init
// the wrap callback makes. The body channels, the secondary and the FP
// channel all fade this way.
// [orig: AnimMap_AdvanceToNextAnim @0x40BDF0 -> AnimChannel_InitFromParams(ch,
//  clip, 8, 0, 0x1000) @0x40BE24: the blend half at t = 0 @0x41068A, weight 0
//  @0x410690, step 1/8 @0x410695]
inline constexpr int32_t kWrapFadeTicks = 8;
inline constexpr float kWrapFadeStep = 1.0f / kWrapFadeTicks; // 0.125 exactly

// A clip's records are its frame count and one more, the end pose. The channel
// samples a frame by (int)(frames * t) with t below 1 and reads that frame's
// trigger, so it plays frames 0 to frames - 1 (the end pose reached by the last
// frame's blend into it) and never reads the end pose's trigger: a one-shot
// holds just short of it, a loop wraps before it. True for that record of a
// clip of `frames` frames (none for a clip with no frame).
// [orig: AnimChannel_InterpolateKeyframe @ 0x40B230; AnimChannel_AdvancePlayback
//  @ 0x40B140]
inline constexpr bool clip_record_is_end_pose(uint32_t frames, uint32_t record) {
	return frames > 0 && record == frames;
}

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
	// Whether a looping channel wrapped on the step into `ticks`: its t crossed 1
	// and took one away, which is where the slot's ring serves its next variant.
	// [orig: AnimChannel_AdvancePlayback @0x40B165 (t >= 1), @0x40B199 (t -= 1)]
	bool wrapped_at(int32_t ticks) const;
	// The playhead in frames at `ticks` (armed_boundary as for normalized_at): the
	// keyframe below it plus the fraction toward the next, which the channel lerps
	// [orig: AnimChannel_InterpolateKeyframe @0x40B230, rec[i]..rec[i+1]].
	double frame_at(int32_t ticks, int32_t armed_boundary = -1) const;
	// The keyframe below the playhead: the frame whose record a channel reads unlerped
	// (the trigger word) at `ticks`.
	int32_t frame_index_at(int32_t ticks, int32_t armed_boundary = -1) const;
	// Per frame, the first tick the running channel is on it, from a start at tick 0, in
	// one pass over the clock: -1 where it never is (a step passes over the frame, or a
	// one-shot stops before it; its stopped tick samples the parked end but reads no
	// trigger). A loop answers for its first pass.
	std::vector<int32_t> first_ticks() const;
	double seconds_at(int32_t ticks) const;
	// seconds_at with the armed-wrap park: on the armed boundary tick a loop
	// samples 0.99999 (its last frame) [orig: AnimChannel_AdvancePlayback
	// @0x40B1A2..0x40B1B1]; -1 = unarmed.
	double seconds_at(int32_t ticks, int32_t armed_boundary) const;
	bool stopped_at(int32_t ticks) const;
	int32_t length_ticks() const;
	int32_t boundary_after(int32_t ticks) const;
	static constexpr float kPark = 0.99999f;

private:
	bool step(float &time) const;
	double frame_of(float time) const;
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

// The event word a channel on `clock` reads at `ticks`: the record of the keyframe below the
// playhead, unlerped [orig: AnimChannel_InterpolateKeyframe @0x40b32f, the trigger sampled from the
// floor keyframe], and none from a one-shot that has stopped, whose parked end samples its capsule
// but no trigger [orig: AnimChannel_AdvancePlayback @0x40b140]. `triggers` holds a word per record
// (the clip's frames, then its end pose); armed_boundary as for ClipTimeline::frame_at. The one rule
// the body's channel (AdmRootMotion's sample) and the editor's clip preview read the word by.
uint32_t clip_trigger_at(const ClipTimeline &clock, const std::vector<uint32_t> &triggers, int32_t ticks,
                         int32_t armed_boundary = -1);
}
