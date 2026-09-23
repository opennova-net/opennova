#pragma once

// Internal to engine/runtime/world's AI TUs — not part of world/ai.h.
//
// The handful of free helpers the AI TUs share: the seat anim lookup, the live
// hull reads, and the BAM/distance/PRNG primitives the handlers and the waypoint
// and combat legs all reach for.

#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <base/io/bam.h>

namespace opennova::world {

class World;

namespace detail {

inline int mounted_anim_state_for_seat(const Entity &target, const Seat &seat, const InfantryState &inf,
                                const IRootMotionSource *root_motion) {
    if (seat.type == SeatType::Gunner) {
        const int variant = target.emplaced_config_valid
                                ? std::clamp<int>(target.emplaced_config, 0, 8)
                                : 0;
        if (variant > 0) {
            const int candidate = anim_state::kEmplaced + variant;
            if (root_motion != nullptr && root_motion->has_clip(inf.adm_id, candidate)) {
                return candidate;
            }
        }
        return anim_state::kEmplaced;
    }

	// Seat names select sit_N; sit_24 then follows hull roll and signed speed.
	// The assignments are ordered: reverse overrides lean, rest overrides both.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0;
	// Entity_UpdateInfantryAI @0x4B9910]
	int state = anim_state::kSit + std::clamp<int>(seat.pose_index, 0, 30);
	if (state == 100) {
		const int32_t roll = target.veh.yaw_seeded ? target.veh.air_roll_bam
												   : int32_t(int64_t(target.roll) * 11930464);
		if (roll < -71582784)
			state = 110;
		if (roll > 71582784)
			state = 109;
		const int32_t speed = target.veh.speed;
		if (speed < 0)
			state = 108;
		const int32_t magnitude = speed < 0 ? int32_t(0u - uint32_t(speed)) : speed;
		if (magnitude < 200)
			state = 107;
	}
	return state;
}

// radians -> 32-bit binary angle. [orig: dbl_7C19D8 = 0x41C45F306DC9C883.]
constexpr double kBamPerRadian = 683565275.5764316; // 2^32 / (2*pi)

// mission yaw degrees -> 32-bit binary angle (entity+16). [orig: AI_HandleCommand cmd 0x16
// @0x4659fa; matches promote.cpp's seed.] Used to mirror a posed mount transform into the brain.
constexpr int64_t kBamPerDegreeInt = 11930464; // trunc(2^32/360) — the original multiplier

// Saturation clamp on the death-velocity magnitude. [orig: flt_7C19E0 = 0x4EFFFE00.]
constexpr double kDeathSpeedClamp = 2147418112.0;

// The ground rows read the hull's live words off the entity record, not the
// AiEntity mirrors (only the spawn and bury paths refresh those for a hull):
// the health word +0x11E and the velocity pair +0x98/+0x9C. A brain without a
// registry row (a bare AiSystem fixture) keeps its mirrors.
// [orig: AI_HandleEvent_HelicopterCombatD @0x467747 / @0x467954..0x46795A;
//  AI_UpdatePatrolBehavior @0x457D87 / @0x457E23..0x457E29;
//  AI_EnterState_GroundEvade @0x4674A5 / @0x4674B2..0x4674B8;
//  AI_TransitionToDeath_GroundVehicle @0x467C09..0x467C0F;
//  AI_TickState_VehicleDying @0x467CED..0x467CF3]
inline int32_t hull_health(const World *world, const AiEntity &e) {
    if (world != nullptr)
        if (const Entity *entity = world->registry.get(e.handle)) return entity->health;
    return e.health;
}

// The death legs' horizontal hull speed: fsqrt(vx*vx + vy*vy) over the same
// pair, the flt_7C19E0 min-clamp, then _ftol2_sse's chop.
inline int32_t hull_death_speed(const World *world, const AiEntity &e) {
    int32_t vx = e.vel_x;
    int32_t vy = e.vel_z;
    if (world != nullptr) {
        if (const Entity *entity = world->registry.get(e.handle)) {
            vx = entity->veh.vel_x;
            vy = entity->veh.vel_y;
        }
    }
    double sp = std::sqrt(static_cast<double>(vx) * vx + static_cast<double>(vy) * vy);
    if (sp > kDeathSpeedClamp) sp = kDeathSpeedClamp;
    return static_cast<int32_t>(sp);
}

// Byte-exact integer abs (cdq/xor/sub idiom; INT_MIN -> INT_MIN like the orig).
inline int32_t iabs32(int32_t v) {
    int32_t s = v >> 31;
    return (v ^ s) - s;
}

// Shared distance + bearing math for both waypoint types. [orig: AIWaypoint_UpdateTarget
// @0x457380.] Approx 3D distance = larger-axis + ((5*(other-axis + |dy|)) >> 16), all in
// wrapping 32-bit; bearing = trunc(atan2(dz, dx) * 2^32/2pi). dx/dz/dy follow the decomp
// naming (dx<-pos+4, dz<-pos+8, dy<-pos+12).
inline void wp_dist_bearing(int32_t dx, int32_t dz, int32_t dy, int32_t &dist, int32_t &bearing) {
    int32_t adx = iabs32(dx), adz = iabs32(dz), ady = iabs32(dy);
    int32_t base, cross;
    if (adx <= adz) { base = adz; cross = static_cast<int32_t>(static_cast<uint32_t>(adx) + static_cast<uint32_t>(ady)); }
    else            { base = adx; cross = static_cast<int32_t>(static_cast<uint32_t>(adz) + static_cast<uint32_t>(ady)); }
    int32_t term = static_cast<int32_t>(static_cast<uint32_t>(cross) * 5u) >> 16; // x5 wrap, arithmetic >>16
    dist = static_cast<int32_t>(static_cast<uint32_t>(base) + static_cast<uint32_t>(term));
    bearing = static_cast<int32_t>(static_cast<int64_t>(
        std::atan2(static_cast<double>(dz), static_cast<double>(dx)) * kBamPerRadian)); // chop toward zero
}

// 32-bit rotate-left. [orig: __ROL4__.]
inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

// One step of the shared rotate-LCG. [orig: __ROL4__(s + __ROL4__(s,11), 4) ^ 1.] Returns the
// new state; the low 16 bits are PRNG_Next16's value and the engagement jitter source.
inline uint32_t prng_step(uint32_t &s) {
    uint32_t r = rotl32(s + rotl32(s, 11), 4);
    s = r ^ 1u;
    return s;
}

// Bearing to a point in 32-bit binary angle. [orig: AI_FindBestTargetB @0x46719D fpatan path —
// atan2(candidate.Y - self.Y, candidate.X - self.X), x87 chop toward zero.] dY/dX follow the
// mover's convention (atan2(dz, dx)).
inline int32_t bearing_bam(int32_t dY, int32_t dX) {
    return static_cast<int32_t>(static_cast<int64_t>(
        std::atan2(static_cast<double>(dY), static_cast<double>(dX)) * kBamPerRadian));
}

// 3D distance in world units. [orig: AI_FindBestTargetB @0x4671e8 sqrt of dx^2+dy^2+dz^2,
// flt_7C19E0 min-clamp, ftol2_sse (chop), then >> 16.]
inline int32_t dist3d_units(const int32_t a[3], const int32_t b[3]) {
    double dx = static_cast<double>(a[0] - b[0]);
    double dy = static_cast<double>(a[1] - b[1]);
    double dz = static_cast<double>(a[2] - b[2]);
    double m = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (m > kDeathSpeedClamp) m = kDeathSpeedClamp; // flt_7C19E0 min-clamp idiom
    return static_cast<int32_t>(m) >> 16;           // ftol2 chop, then >>16 to world units
}

} // namespace detail
} // namespace opennova::world
