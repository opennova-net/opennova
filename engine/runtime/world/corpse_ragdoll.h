#pragma once

// The world's side of the opt-in ragdoll corpses (ADR 0060): what a death
// clip says about the death it plays, and the world as a ragdoll collides with
// it. Both are reads: a ragdoll is drawn, never simulated, so nothing here
// writes the world.

#include <runtime/ragdoll/ragdoll_collider.h>
#include <runtime/ragdoll/ragdoll_solver.h>

#include <cstdint>

namespace opennova::world {

class World;

// A death clip's reading: whether a state is a death clip at all, whether the
// ragdoll replaces it, and the push it gives. The clip id is the death
// selector's (compute_death_anim_state), which every machine receives on the
// wire, so the push decodes the same everywhere: bullet ids carry the struck
// bone's group and the quadrant the shot came from, grenade ids the quadrant
// of the blast. Fire (173) and drowning (175) keep their clips: those clips
// are the death's look.
struct CorpseDeath {
	bool dying = false;
	bool ragdolls = false;
	ragdoll::Kick kick;
};
CorpseDeath corpse_death(int anim_state, uint32_t seed);

// The world a ragdoll collides with: the terrain's bilinear height and
// surface normal, and static geometry through the aim trace's walk (no
// persons, terrain or water, and nothing recorded). Presentation frame in,
// presentation frame out.
class WorldRagdollCollider final : public ragdoll::Collider {
public:
	explicit WorldRagdollCollider(const World &world) : world_(world) {}
	bool ground(float x, float z, float &r_height, ragdoll::Vec3 &r_normal) const override;
	bool sweep(const ragdoll::Vec3 &a, const ragdoll::Vec3 &b, ragdoll::Vec3 &r_point,
			ragdoll::Vec3 &r_normal) const override;

private:
	const World &world_;
};

} // namespace opennova::world
