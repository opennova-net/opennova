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

int main() {
	// Feet, fire and the six foley bits; nothing above 0x400.
	static_assert(anim::kAnimEventKnownMask == 0x7FFu, "the eleven bits the readers test");
	uint32_t mask = 0;
	for (int i = 0; i < anim::kAnimEventBitCount; ++i) mask |= anim::kAnimEventBits[i].mask;
	TEST_EXPECT(mask == anim::kAnimEventKnownMask);
	TEST_EXPECT((anim::kAnimEventKnownMask & (anim::kAnimEventFoley1 << (anim::kAnimEventFoleyCount - 1))) != 0);
	TEST_EXPECT((anim::kAnimEventKnownMask & 0x800u) == 0);

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
