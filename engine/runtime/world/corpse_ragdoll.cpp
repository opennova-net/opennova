#include "corpse_ragdoll.h"

#include <runtime/ragdoll/ragdoll_rig.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/infantry.h>
#include <runtime/world/world.h>

#include <cmath>

#include <base/io/fixed.h>

namespace opennova::world {

namespace {

// The bullet death families' bone groups (infantry_tables.cpp's selector,
// 0 hip .. 14 left foot) back to a bone of the group, the body the kick lands on.
constexpr int kGroupBone[15] = {
	ragdoll::bone::kHips,
	ragdoll::bone::kUpperSpine,
	ragdoll::bone::kHead,
	ragdoll::bone::kRUpperArm,
	ragdoll::bone::kLUpperArm,
	ragdoll::bone::kRForearm,
	ragdoll::bone::kLForearm,
	ragdoll::bone::kRHand,
	ragdoll::bone::kLHand,
	ragdoll::bone::kRThigh,
	ragdoll::bone::kLThigh,
	ragdoll::bone::kRCalf,
	ragdoll::bone::kLCalf,
	ragdoll::bone::kRFoot,
	ragdoll::bone::kLFoot,
};

constexpr int kDeathLast = anim_state::kDeathBulletBase + 4 * 15 - 1;

// presentation (x, y, z) -> mission 16.16 (x, -z, y), and back.
FixedVec3 mission_q16(const ragdoll::Vec3 &p) {
	return FixedVec3{ io::q16_from_float(p.x), io::q16_from_float(-p.z), io::q16_from_float(p.y) };
}
ragdoll::Vec3 presentation(const FixedVec3 &m) {
	constexpr float k = 1.0f / 65536.0f;
	return { static_cast<float>(m.x) * k, static_cast<float>(m.z) * k,
		-static_cast<float>(m.y) * k };
}

} // namespace

uint32_t corpse_kick_seed(uint32_t wire_key, int anim_state) {
	return wire_key * 0x9E3779B1u ^ static_cast<uint32_t>(anim_state) * 0x85EBCA77u;
}

CorpseDeath corpse_death(int anim_state, uint32_t wire_key) {
	CorpseDeath d;
	if (anim_state < anim_state::kDeathFire || anim_state > kDeathLast)
		return d;
	d.dying = true;
	d.kick.seed = corpse_kick_seed(wire_key, anim_state);
	if (anim_state == anim_state::kDeathFire || anim_state == anim_state::kDeathDrown)
		return d;
	d.ragdolls = true;
	if (anim_state == anim_state::kDeathPungi) {
		d.kick.kind = ragdoll::Kick::Kind::Collapse;
	} else if (anim_state < anim_state::kDeathBulletBase) {
		d.kick.kind = ragdoll::Kick::Kind::Blast;
		d.kick.quadrant = anim_state - anim_state::kDeathGrenadeBase;
	} else {
		const int off = anim_state - anim_state::kDeathBulletBase;
		d.kick.kind = ragdoll::Kick::Kind::Bullet;
		d.kick.quadrant = off % 4;
		d.kick.bone = kGroupBone[off / 4];
	}
	return d;
}

bool corpse_ragdoll_may_start(const World &world, float hips_height) {
	return world.env.water_z == 0 ||
			hips_height > static_cast<float>(world.env.water_z) * io::kInvFp16One;
}

int32_t LocalAvatarLife::observe(const World &world) {
	const Entity *self = world.registry.get(world.cached.local_player);
	if (self == nullptr)
		return life_;
	if (self->registry_spawn_id != spawn_id_) {
		spawn_id_ = self->registry_spawn_id;
		dead_known_ = false;
	}
	const bool dead = (self->flags & kEntityFlagDead) != 0;
	if (dead_known_ && dead_ && !dead)
		++life_;
	dead_known_ = true;
	dead_ = dead;
	return life_;
}

bool WorldRagdollCollider::ground(
		float x, float z, float &r_height, ragdoll::Vec3 &r_normal) const {
	const terrain::TerrainHeightField *field = world_.tables.terrain;
	if (field == nullptr || !field->valid())
		return false;
	// The height field takes the presentation frame's x and z (mission x, -y).
	r_height = terrain::height_field_height_world_bilinear(*field, x, z);
	constexpr float kStep = 0.5f;
	const float hx0 = terrain::height_field_height_world_bilinear(*field, x - kStep, z);
	const float hx1 = terrain::height_field_height_world_bilinear(*field, x + kStep, z);
	const float hz0 = terrain::height_field_height_world_bilinear(*field, x, z - kStep);
	const float hz1 = terrain::height_field_height_world_bilinear(*field, x, z + kStep);
	r_normal = ragdoll::normalized(
			ragdoll::Vec3{ (hx0 - hx1) / (2.0f * kStep), 1.0f, (hz0 - hz1) / (2.0f * kStep) });
	return std::isfinite(r_height);
}

bool WorldRagdollCollider::sweep(const ragdoll::Vec3 &a, const ragdoll::Vec3 &b,
		ragdoll::Vec3 &r_point, ragdoll::Vec3 &r_normal) const {
	using namespace ragdoll;
	if (world_.collision == nullptr)
		return false;
	ProjectileTrace trace;
	trace.start = mission_q16(a);
	trace.end = mission_q16(b);
	trace.walk_terrain = false;
	trace.walk_water = false;
	trace.walk_persons = false;
	const ProjectileHit hit = world_.collision->trace_aim(world_, trace);
	if (!hit.hit())
		return false;
	r_point = presentation(hit.position_q16);
	ragdoll::Vec3 n = ragdoll::normalized(presentation(hit.normal_q16));
	if (ragdoll::dot(n, b - a) > 0.0f)
		n = -n;
	r_normal = n;
	return true;
}

} // namespace opennova::world
