#include <runtime/world/weapon_scope_zero.h>

#include <algorithm>
#include <cmath>
#include <base/io/bam.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/round_sim.h>

namespace opennova::world {

// [orig: WeaponSlot_CalcElevationTable @0x545100]
void weapon_scope_zero_bake(WeaponScopeZero &zero, const AmmoTableEntry &ammo) {
    zero.elevation.fill(0);
    const int32_t speed = static_cast<int32_t>(uint32_t(ammo.velocity) << 16) / 62;
    if (speed <= 0) return;
    int32_t target = 0;
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
            do {
                const int32_t next = io::bam_add(range, velocity.x);
                if (next > range) range = next;
                height = io::bam_add(height, velocity.z);
                velocity.z = io::bam_sub(velocity.z, 167);
                projectile_apply_drag(velocity, ammo, 100 << 16, 0);
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
}

int16_t weapon_scope_zero_initial(const WeaponScopeZero &zero) {
    return zero.step_metres != 0 ? static_cast<int16_t>(zero.default_metres / zero.step_metres) : 0;
}

int16_t weapon_scope_zero_adjust(const WeaponScopeZero &zero, int16_t current,
    int delta, bool in_session, bool team_game) {
    if (zero.max_steps == 0) return current;
    int32_t next = std::min(io::bam_add(current, delta), zero.max_steps);
    next = std::max(next, !in_session || team_game ? -1 : 0);
    if (zero.min_steps != 0) next = std::max(next, zero.min_steps);
    return static_cast<int16_t>(next);
}

int32_t weapon_scope_zero_pitch(const WeaponScopeZero &zero, int16_t step) {
    return step >= 0 && step < int(zero.elevation.size()) ? zero.elevation[step] : 0;
}
}
