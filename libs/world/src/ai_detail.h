#pragma once

// Internal to libs/world's AI TUs — not part of world/ai.h. Split out of ai.cpp
// (quality campaign W3-3); the bodies are unchanged.
//
// The handful of free helpers the AI TUs share: the body-anim slot pick, the seat
// anim lookup, and the BAM/distance/PRNG primitives the handlers and the waypoint
// and combat legs all reach for.

#include "world/ai.h"

#include <cmath>
#include <cstdint>

#include <io/bam.h>

namespace opennova::world {

class World;

namespace detail {

// Minimal port of Entity_UpdateInfantryAI @0x4b9910's anim selection: pick a body-anim slot
// from the brain state + movement so 3rd-person NPCs walk/idle instead of sliding at rest.
// Writes the world Entity's body_anim_slot (what the present snapshot reads). Full fidelity
// (randomized idle variants, jog/run thresholds, attack/death clips) is a grill follow-up.
inline void update_body_anim_slot(AiEntity &e, World &world) {
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;
    if (!ent->alive || ent->health <= 0) return; // dead: present pass hides it; leave the slot
    const AiBrain &b = e.brain;
    const bool moving = b.f[AiBrain::kOutSpeed] > 0;
    int32_t slot;
    if (moving) {
        slot = (b.f[AiBrain::kAlert] >= 2) ? kBodyAnimRunForward : kBodyAnimWalkForward;
    } else {
        slot = kBodyAnimIdle;
    }
    ent->body_anim_slot = slot;
}

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

    // The original derives this from the mounted seat bone name: sitexNN/ctrlxNN/drvrxNN
    // becomes anim_sit + NN. The dynamic sit_24 driver lean states need vehicle control fields
    // that are not modeled in this port yet, so this resolver intentionally stops at base sit_N.
    return anim_state::kSit + std::clamp<int>(seat.pose_index, 0, 30);
}

// radians -> 32-bit binary angle. [orig: dbl_7C19D8 = 0x41C45F306DC9C883.]
constexpr double kBamPerRadian = 683565275.5764316; // 2^32 / (2*pi)

// mission yaw degrees -> 32-bit binary angle (entity+16). [orig: AI_HandleCommand cmd 0x16
// @0x4659fa; matches promote.cpp's seed.] Used to mirror a posed mount transform into the brain.
constexpr int64_t kBamPerDegreeInt = 11930464; // trunc(2^32/360) — the original multiplier

// Saturation clamp on the death-velocity magnitude. [orig: flt_7C19E0 = 0x4EFFFE00.]
constexpr double kDeathSpeedClamp = 2147418112.0;

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

// Bearing to a point in 32-bit binary angle. [orig: AI_FindBestTargetB @0x4671a0 fpatan path —
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
