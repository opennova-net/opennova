#include <runtime/world/nvg_laser.h>

#include <formats/def/def.h>
#include <runtime/world/collision.h>
#include <runtime/world/world.h>

namespace opennova::world {

// [orig: Entity_RenderNVGLaserBeam @ 0x5c609a (attach bone +0x157),
//  @ 0x5c60a7 (+0x298), @ 0x5c60b5..0x5c60c2 (def+8 & 0x40000000),
//  @ 0x5c60c8 (g_NVGActive), @ 0x5c60d4 (g_camera_mode), @ 0x5c60e0
//  (g_local_player_entity)]
bool nvg_laser_beam_drawn(const NvgLaserGate &gate) {
	if (gate.attach_bone != 0 || !gate.has_weapon_def) return false;
	if ((static_cast<uint32_t>(gate.weapon_flags) & def::DEF_WEAPON_FLAG_LASERBEAM) == 0)
		return false;
	return gate.nvg_active && gate.camera_mode == 0 && !gate.local_player;
}

namespace {

// The ray end: the origin plus eight directions, each (dir << 5) >> 2 on the
// 32-bit word [orig: @ 0x5c6110..0x5c6136].
int32_t ray_end_axis(int32_t origin, int32_t direction) {
	const int32_t eight = static_cast<int32_t>(static_cast<uint32_t>(direction) << 5) >> 2;
	return static_cast<int32_t>(static_cast<uint32_t>(origin) + static_cast<uint32_t>(eight));
}

// (dir * clip + 0x8000) >> 16 on the 64-bit product, the low word kept, plus
// the origin [orig: imul / add 8000h / adc / shrd @ 0x5c629d..0x5c62bc].
int32_t clip_axis(int32_t origin, int32_t direction, int32_t clip) {
	const int64_t product = static_cast<int64_t>(direction) * clip + 0x8000;
	const int32_t offset = static_cast<int32_t>(static_cast<uint64_t>(product) >> 16);
	return static_cast<int32_t>(static_cast<uint32_t>(offset) + static_cast<uint32_t>(origin));
}

} // namespace

// [orig: Entity_RenderNVGLaserBeam @ 0x5c613d..0x5c622f — the ray state
//  (memset 0x88, start = the action point, end, the owner +0x44 = the person,
//  range 0x80000), Projectile_RaycastProximitySlots slot 2 then slot 1]
int32_t nvg_laser_clip_distance(const CollisionWorld &collision, const World &world,
		EntityHandle person, const int32_t origin_q16[3], const int32_t direction_q16[3]) {
	ProjectileTrace trace;
	trace.start = FixedVec3{origin_q16[0], origin_q16[1], origin_q16[2]};
	trace.end = FixedVec3{ray_end_axis(origin_q16[0], direction_q16[0]),
			ray_end_axis(origin_q16[1], direction_q16[1]),
			ray_end_axis(origin_q16[2], direction_q16[2])};
	trace.owner = person;
	trace.radius_q16 = 0;
	// The two slot walks are pools 2 and 1 alone: terrain, water and persons
	// never clip the beam. A client walks the vehicles it built from the wire
	// the way the host walks its own (the minefield query's precedent).
	trace.walk_terrain = false;
	trace.walk_water = false;
	trace.walk_persons = false;
	trace.include_wire_proxies = world.rules.mp_session && !world.rules.projectile_authority;
	const ProjectileHit hit = collision.trace_projectile(world, trace);
	int32_t clip = kNvgLaserRangeQ16;
	if (hit.hit() && hit.distance_q16 < clip) clip = hit.distance_q16;
	return clip;
}

// [orig: Entity_RenderNVGLaserBeam @ 0x5c6233..0x5c637e]
int nvg_laser_beam_points(const int32_t origin_q16[3], const int32_t direction_q16[3],
		int32_t clip_q16, float out_points[kNvgLaserMaxPoints * 4]) {
	constexpr float kInvQ16 = 1.0f / 65536.0f;
	int count = 0;
	uint32_t along[3] = {0, 0, 0};
	for (; count < kNvgLaserMaxSamples; ++count) {
		if (static_cast<int32_t>(static_cast<uint32_t>(count) << 14) >= clip_q16) break;
		float *point = out_points + count * 4;
		for (int axis = 0; axis < 3; ++axis) {
			const int32_t offset = static_cast<int32_t>(along[axis]) >> 2;
			point[axis] = static_cast<float>(static_cast<int32_t>(
					static_cast<uint32_t>(origin_q16[axis]) + static_cast<uint32_t>(offset))) *
					kInvQ16;
			along[axis] += static_cast<uint32_t>(direction_q16[axis]);
		}
		point[3] = 1.0f;
	}
	if (count == kNvgLaserMaxSamples) return count;
	// The samples stopped short: the clip point, twice.
	for (int repeat = 0; repeat < 2; ++repeat, ++count) {
		float *point = out_points + count * 4;
		for (int axis = 0; axis < 3; ++axis)
			point[axis] = static_cast<float>(clip_axis(origin_q16[axis], direction_q16[axis],
					clip_q16)) * kInvQ16;
		point[3] = 1.0f;
	}
	return count;
}

} // namespace opennova::world
