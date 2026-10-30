// A flying round's `move` emitter (round +0x1CC): the ammo's effects_table
// tag-1 particle the round holds while it flies (world/round_move_effect.h:
// LiveRound::move_effect_live), spawned, re-posed and released in the world's
// scene where the round simulation's tick ends. The handle is a scene slot by
// the round's presentation key, so a same-slot replacement never inherits the
// outgoing round's group.
#pragma once

#include <runtime/particle/effect_scene.h>

#include <cstdint>
#include <vector>

namespace opennova::world {

class World;

inline constexpr uint64_t kRoundMoveSlotDomain = 0x0800000000000000ull;
inline particle::EffectSlotToken round_move_slot(int64_t presentation_key) {
	return {kRoundMoveSlotDomain | (uint64_t(presentation_key) & (kRoundMoveSlotDomain - 1))};
}

class RoundEmitters {
public:
	// Every live round whose emitter is live and holds none spawns one at its position, along its
	// velocity, tagged with the round; one that holds one re-poses it as the +0x1CC re-pose does
	// (its model's first masked particlefx point, else its position with the aim kept); a round
	// whose emitter the water released, or that left the pool, releases its group (stops its
	// emission; the live particles drain).
	// [orig: Projectile_UpdatePhysics @ 0x4E9D70 - the useownmove spawn @ 0x4E9F94..0x4EA011
	//  (orientation the velocity +0x98..+0xA0 @ 0x4E9FB5..0x4EA002, the round the tag
	//  @ 0x4E9FF4), the water release Entity_ReleaseEffectEmitter @ 0x4EA031, the re-pose
	//  CEffect_UpdateEmitterTransform @ 0x4EA039 / @ 0x4EA968; Projectile_ReleaseEffects
	//  @ 0x4E8280 -> Entity_ReleaseEffectEmitter @ 0x5F75D0]
	void sync(World &world);
	void reset() { held_.clear(); }
	std::size_t held() const { return held_.size(); }

private:
	std::vector<int64_t> held_;
};

} // namespace opennova::world
