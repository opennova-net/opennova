// The `.bad` trigger word's known bits (runtime/anim/anim_event_bits.h kAnimEventKnownMask: the table's
// masks together, the feet 0x1/0x2, the fire bits 0x4/0x8/0x10, the six foley bits 0x20 << i) and the
// end-pose record whose trigger no channel reads (runtime/anim/clip_timeline.h clip_record_is_end_pose:
// record frames of a clip of frames frames [orig: AnimChannel_InterpolateKeyframe @ 0x40B230]).
#include <runtime/anim/anim_event_bits.h>
#include <runtime/anim/clip_timeline.h>

#include <cstdint>
#include <cstdio>

#include "common/test_expect.h"

namespace anim = opennova::anim;

// Shared clips serve bodies at unrelated ages, repeatedly sampling channels and
// triggers. Seeking/eviction must preserve every rounded float, wrap and park.
// [orig: AnimChannel_AdvancePlayback @ 0x40B140, add/wrap/park @ 0x40B156..0x40B1B1]
int test_shared_clock_seeks() {
    for (bool loop : {false, true}) {
        for (uint32_t frames : {3u, 60u, 511u}) {
            const anim::ClipTimeline clock(30, frames, loop);
            std::vector<float> expected(4096, 0.0f);
            std::vector<bool> wraps(4096, false);
            int32_t first_end = -1;
            for (int32_t tick = 1; tick < 4096; ++tick) {
                const double next = double(expected[tick - 1]) + clock.delta();
                if (!loop && first_end >= 0) expected[tick] = anim::ClipTimeline::kPark;
                else if (next >= 1.0) {
                    if (first_end < 0) first_end = tick;
                    wraps[tick] = loop;
                    expected[tick] = loop ? static_cast<float>(next - 1.0) : anim::ClipTimeline::kPark;
                } else expected[tick] = static_cast<float>(next);
            }
            TEST_EXPECT(clock.normalized_at(4095) == expected[4095]);
            // Interleave six distant playheads, exceeding the bounded cache,
            // with six channel reads at each age. Cover every checkpoint edge.
            for (int32_t age = 0; age < 640; ++age) {
                for (int32_t body = 0; body < 6; ++body) {
                    const int32_t tick = body * 640 + age;
                    for (int channel = 0; channel < 6; ++channel)
                        TEST_EXPECT(clock.normalized_at(tick) == expected[tick]);
                    TEST_EXPECT(clock.wrapped_at(tick) == wraps[tick]);
                    TEST_EXPECT(clock.stopped_at(tick) == (!loop && tick >= first_end));
                    TEST_EXPECT(clock.frame_index_at(tick) == static_cast<int32_t>(double(expected[tick]) * frames));
                }
            }
            TEST_EXPECT(clock.normalized_at(first_end, first_end) == anim::ClipTimeline::kPark);
            TEST_EXPECT(clock.normalized_at(first_end + 1, first_end) == expected[first_end + 1]);
            TEST_EXPECT(clock.normalized_at(-1) == 0.0f);
        }
    }
    return 0;
}

int main() {
    TEST_EXPECT(test_shared_clock_seeks() == 0);
	// Feet, fire and the six foley bits; nothing above 0x400.
	static_assert(anim::kAnimEventKnownMask == 0x7FFu, "the eleven bits the readers test");
	uint32_t mask = 0;
	for (int i = 0; i < anim::kAnimEventBitCount; ++i) mask |= anim::kAnimEventBits[i].mask;
	TEST_EXPECT(mask == anim::kAnimEventKnownMask);
	TEST_EXPECT((anim::kAnimEventKnownMask & (anim::kAnimEventFoley1 << (anim::kAnimEventFoleyCount - 1))) != 0);
	TEST_EXPECT((anim::kAnimEventKnownMask & 0x800u) == 0);
	// The fire block's three bits.
	static_assert(anim::kAnimEventFireMask == 0x1Cu, "fire primary, secondary and marker3");

	// A clip of three frames has four records: 0..2 play, 3 is the end pose; a clip of no frame has none.
	TEST_EXPECT(!anim::clip_record_is_end_pose(3, 0) && !anim::clip_record_is_end_pose(3, 2));
	TEST_EXPECT(anim::clip_record_is_end_pose(3, 3));
	TEST_EXPECT(!anim::clip_record_is_end_pose(0, 0));
	// The clock never reads it: a one-shot's frames run 0 to frames - 1.
	const anim::ClipTimeline once(30, 3, false);
	for (int32_t tick = 0; tick < 200; ++tick)
		TEST_EXPECT(!anim::clip_record_is_end_pose(3, static_cast<uint32_t>(once.frame_index_at(tick))));
	std::printf("anim_event_bits: known mask 0x%X, the end pose unread\n", anim::kAnimEventKnownMask);
	return 0;
}
