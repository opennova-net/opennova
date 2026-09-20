// The root-motion double the death-state and projectile-combat rigs tick a
// player body over: the clips its org2 anim path asks for, the idle pair plus
// the whole death family, one phase step per tick. Header-only test
// infrastructure (no retail counterpart to cite).
#pragma once

#include <runtime/world/infantry.h>

#include <cstdint>

namespace test_world {

struct DeathClipSource final : opennova::world::IRootMotionSource {
	bool has_clip(int, int state_id) const override {
		namespace anim_state = opennova::world::anim_state;
		return state_id == anim_state::kIdle || state_id == anim_state::kIdle2 ||
		       (state_id >= anim_state::kDeathFire &&
		        state_id <= anim_state::kDeathBulletBase + 59);
	}
	int32_t clip_length_ticks(int, int, int) const override { return -1; }
	bool advance(int, int state_id, int32_t &phase,
			opennova::world::RootMotionFrame &out) override {
		if (!has_clip(0, state_id)) return false;
		++phase;
		out = opennova::world::RootMotionFrame{};
		return true;
	}
};

} // namespace test_world
