#include <runtime/world/weapon_scope_zero.h>

#include <algorithm>
#include <cmath>
#include <base/io/bam.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/round_sim.h>

namespace opennova::world {

namespace {
// The bake's per-tick speed index: (62 * ftol(min(|v|, flt_7C19E0))) >> 16
// as the 32-bit `shl 5; sub; add` product then `sar 16`, capped at 1219 --
// the AmmoDef+0xB0 comparand [orig: @0x5452a2..0x5452f8, the product
// @0x5452e4..0x5452eb, `sar ecx,10h` @0x5452ed].
int32_t scope_zero_speed_index(const FixedVec3 &velocity) {
    const double magnitude = std::sqrt(double(velocity.x) * velocity.x +
        double(velocity.y) * velocity.y + double(velocity.z) * velocity.z);
    const int32_t truncated = static_cast<int32_t>(std::min(magnitude, 2147418112.0));
    const int32_t product = static_cast<int32_t>(uint32_t(truncated) * 62u);
    const int32_t index = product >> 16;
    return index >= 1219 ? 1219 : index;
}
} // namespace

// [orig: WeaponSlot_CalcElevationTable @0x545100]
void weapon_scope_zero_bake(WeaponScopeZero &zero, const AmmoTableEntry &ammo) {
    zero.elevation.fill(0);
    zero.max_range_q16 = 0;
    const int32_t speed = static_cast<int32_t>(uint32_t(ammo.velocity) << 16) / 62;
    if (speed <= 0) return;
    int32_t target = 0;
    // The +0xF0 max-range output: the range at the first tick the post-drag
    // speed drops under AmmoDef+0xB0, else at the tick the ammo's lifetime
    // (AmmoDef+8) expires, kept as a running maximum across every solve;
    // neither ends the flight [orig: the lifetime seed @0x545246; the latch
    // and the countdown @0x54530f..0x545338].
    int32_t max_range = 0;
    for (int i = 0; i <= std::min(39, zero.max_steps); ++i) {
        int32_t angle = 190887424, bisect = angle, previous_angle = angle;
        int32_t previous_range = 0, range = 0;
        for (;;) {
            // Retail seeds a zeroed synthetic projectile 100 units above water,
            // with Q22 trig and gravity regardless of the ammo's runtime flags.
            const double radians = double(angle) * (2.0 * io::kPi / 4294967296.0);
            const int32_t cosine = static_cast<int32_t>(std::cos(radians) * 4194304.0);
            const int32_t sine = static_cast<int32_t>(std::sin(radians) * 4194304.0);
            FixedVec3 velocity{static_cast<int32_t>((int64_t(speed) * cosine) >> 22),
                0, static_cast<int32_t>((int64_t(speed) * sine) >> 22)};
            range = 0;
            int32_t height = 0;
            int32_t lifetime = ammo.max_age_ticks;
            bool below_stable = false;
            do {
                const int32_t next = io::bam_add(range, velocity.x);
                if (next > range) range = next;
                height = io::bam_add(height, velocity.z);
                velocity.z = io::bam_sub(velocity.z, 167);
                projectile_apply_drag(velocity, ammo, 100 << 16, 0);
                const int32_t speed_index = scope_zero_speed_index(velocity);
                if (ammo.min_stable_velocity > speed_index && !below_stable) {
                    below_stable = true;
                    if (range > max_range) max_range = range;
                }
                if (--lifetime == 0 && !below_stable && range > max_range) max_range = range;
            } while (height >= 0);
            if (bisect == 0) break;
            previous_angle = angle;
            bisect >>= 1;
            angle = range <= target ? io::bam_add(angle, bisect) : io::bam_sub(angle, bisect);
            previous_range = range;
        }
        zero.elevation[i] = std::abs(int64_t(previous_range) - target) <
                std::abs(int64_t(range) - target) ? previous_angle : angle;
        target = io::bam_add(target, static_cast<int32_t>(uint32_t(zero.step_metres) << 16));
    }
    zero.max_range_q16 = max_range; // [orig: @0x5453f8]
}

int16_t weapon_scope_zero_initial(const WeaponScopeZero &zero) {
    return zero.step_metres != 0 ? static_cast<int16_t>(zero.default_metres / zero.step_metres) : 0;
}

int16_t weapon_scope_zero_adjust(const WeaponScopeZero &zero, int16_t current,
    int delta, bool in_session, bool auto_scope_zero) {
    if (zero.max_steps == 0) return current;
    int32_t next = std::min(io::bam_add(current, delta), zero.max_steps);
    next = std::max(next, !in_session || auto_scope_zero ? -1 : 0);
    if (zero.min_steps != 0) next = std::max(next, zero.min_steps);
    return static_cast<int16_t>(next);
}

int32_t weapon_scope_zero_pitch(const WeaponScopeZero &zero, int16_t step) {
    return step >= 0 && step < int(zero.elevation.size()) ? zero.elevation[step] : 0;
}

// [orig: WeaponSlot_InitFromDef @0x53ef4f..0x53ef8b; Player_AdjustWeaponZoomLevel
//  @0x4dbd91..0x4dbdd5 -- fpatan(+0x8C, distance) * dbl_7C19D8 (BAM per
//  radian), ftol; the 100 m floor @0x4dbdb6..0x4dbdb8]
int32_t weapon_scope_zero_yaw(const WeaponScopeZero &zero, int16_t step) {
    if (zero.paralax_distance_q16 == 0) return 0;
    int32_t distance = static_cast<int32_t>(
        (uint32_t(zero.step_metres) * uint32_t(int32_t(step))) << 16);
    if (distance < 100 << 16) distance = 100 << 16;
    return static_cast<int32_t>(
        std::atan2(double(zero.paralax_distance_q16), double(distance)) * io::kBamPerRadian);
}
}
