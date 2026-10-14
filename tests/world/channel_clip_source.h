// The root-motion double for a body that holds its animation channel and plays
// nothing: a table whose RESET row (slot 0) is authored, so the org1 update runs
// for it (infantry_has_anim_channel), and no state whose clip advances, so the
// motor head finds no clip and the body stands. For the rigs that test the
// update's other legs (the think, the heading chase, the vertical block, the
// death edge) on a body with no clips of its own. Header-only test
// infrastructure (no retail counterpart to cite).
#pragma once

#include <runtime/world/infantry.h>

#include <cstdint>

namespace test_world {

struct ChannelClipSource final : opennova::world::IRootMotionSource {
	bool has_clip(int, int state_id) const override {
		return state_id == opennova::world::anim_state::kReset;
	}
	int32_t clip_length_ticks(int, int, int) const override { return -1; }
	bool advance(int, int, int32_t &, opennova::world::RootMotionFrame &) override { return false; }
};

} // namespace test_world
