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
// `wire_key` is the corpse's wire handle (the low 16 bits every peer decodes).
// The kick's seed mixes it with the clip id, the two values every machine
// holds alike for the corpse (corpse_kick_seed), never a per-machine count
// such as a respawn revision a late joiner never saw.
CorpseDeath corpse_death(int anim_state, uint32_t wire_key);
uint32_t corpse_kick_seed(uint32_t wire_key, int anim_state);

// Whether a corpse whose hips stand at `hips_height` (presentation frame, world
// units) may start a ragdoll: a body that dies in the water keeps retail's
// float, its clip, so a ragdoll starts only above the water plane; a world with
// no water starts one anywhere (ADR 0060 d6).
bool corpse_ragdoll_may_start(const World &world, float hips_height);

// The ragdoll set's key for the local avatar, the one subject with no row of
// its own: no wire handle reaches bit 31.
inline constexpr uint32_t kLocalAvatarRagdollKey = 0x80000000u;

// The local avatar's life: one more at every dead -> alive edge of the local
// player's Dead flag, counted as the pool rows count theirs (inmatch
// present_rows' respawn revision), so its ragdoll ends at once on a respawn
// (ADR 0060 d6). Another entity taking the local player's place is a new life
// as well.
class LocalAvatarLife {
public:
	int32_t observe(const World &world);

private:
	uint64_t spawn_id_ = 0;
	bool dead_known_ = false;
	bool dead_ = false;
	int32_t life_ = 0;
};

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
