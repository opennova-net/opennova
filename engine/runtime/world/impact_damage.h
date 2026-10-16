// The impact damage a striking round deals and the energy it loses on the way
// in: the material cost of a face it passes, the armor deceleration, and the
// kinetic damage number with its section scales and the ammo's clamp. Split
// from round_sim.cpp by responsibility; the round sim (flight, spawn and the
// damage consequences) calls it. [orig: Weapon_CalcImpactDamage @ 0x4EC920,
// Projectile_ApplyDragDeceleration @0x4e5cd0, the material-energy leg of
// Projectile_UpdatePhysics @0x4E9D70]
#pragma once

#include <cstdint>
#include <limits>

#include <runtime/world/ammo_table.h>
#include <runtime/world/geom.h>

namespace opennova::world {

struct Entity;
class World;

// Convert a modulo-2^32 result to the signed value represented by the same bits
// without relying on implementation-defined unsigned-to-signed conversion.
inline int32_t signed_from_u32(uint32_t value) {
    if (value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return static_cast<int32_t>(value);
    return -1 - static_cast<int32_t>(std::numeric_limits<uint32_t>::max() - value);
}

inline int32_t wrapped_signed_product(int32_t lhs, int32_t rhs) {
    return signed_from_u32(static_cast<uint32_t>(lhs) * static_cast<uint32_t>(rhs));
}

inline int32_t arithmetic_shift_right_16(int64_t value) {
    if (value >= 0) return static_cast<int32_t>(value >> 16);
    return -static_cast<int32_t>((-value + 0xffff) >> 16);
}

// The witnessed person hit-zone -> damage-multiplier table (ray[32] damage
// zones; item_type 3 targets) and the Landable (attrib 0x200) seat-branch bone
// leg (which reads the ray[31] primary/reaction section instead). The critical
// predicates name the rows that also stamp the 0x800 entity flag at damage
// time. [orig: Weapon_CalcImpactDamage @ 0x4EC920 — the zone table @ 0x4ec9bf
//  (0..4 x1.25, 9..12/15..18 x0.5, 13/14 head x3.0), the seat leg @ 0x4ec977
//  (bones 2/3/6/7 x6.0)]
constexpr bool hit_zone_is_critical(int32_t hit_zone) {
    return hit_zone == 13 || hit_zone == 14;
}
constexpr double hit_zone_damage_multiplier(int32_t hit_zone) {
    if (hit_zone >= 0 && hit_zone <= 4) return 1.25;
    if ((hit_zone >= 9 && hit_zone <= 12) || (hit_zone >= 15 && hit_zone <= 18))
        return 0.5;
    if (hit_zone_is_critical(hit_zone)) return 3.0;
    return 1.0;
}
// dfx2.exe's table: every section 1.0 but the head's 13 and 14 (3.0, with the
// 0x800 latch as Jointops.exe's); its seat leg is Jointops.exe's 6.0
// (docs/titles/dfx2-vs-jo-re.md DFX2-16).
// [orig: Weapon_CalcImpactDamage @0x4e9a50 - the zone switch @0x4e9ae2..0x4e9b17,
//  the seat leg @0x4e9aab..0x4e9acf (dfx2.exe)]
constexpr double dfx2_hit_zone_damage_multiplier(int32_t hit_zone) {
    return hit_zone_is_critical(hit_zone) ? 3.0 : 1.0;
}
// The ammo's floor and ceiling (Def[47], Def[48]): at least the minimum, at
// most the maximum when that is positive. Jointops.exe takes them after the
// section and class scales, dfx2.exe before them (DFX2-16).
// [orig: Weapon_CalcImpactDamage — the floor @0x4ecb77..0x4ecb81, the cap
//  @0x4ecb83..0x4ecb91] [orig: @0x4e9c4d..0x4e9c63
//  (dfx2.exe)]
inline int32_t clamp_impact_damage(int32_t damage, const AmmoTableEntry &ammo) {
    if (damage <= ammo.min_damage) damage = ammo.min_damage;
    if (ammo.max_damage > 0 && damage >= ammo.max_damage) damage = ammo.max_damage;
    return damage;
}
constexpr bool seat_hit_bone_is_critical(int32_t hit_bone) {
    return hit_bone == 2 || hit_bone == 3 || hit_bone == 6 || hit_bone == 7;
}
constexpr double seat_hit_bone_damage_multiplier(int32_t hit_bone) {
    return seat_hit_bone_is_critical(hit_bone) ? 6.0 : 1.0;
}

bool impact_is_critical(const Entity &target, int32_t hit_zone, int32_t hit_bone);
bool subtract_material_energy(FixedVec3 &velocity, int32_t mass, int32_t cost);
int32_t calc_impact_damage(FixedVec3 &velocity_q16, const AmmoTableEntry &ammo,
                           int32_t hit_zone, int32_t hit_bone, const Entity &target,
                           const Entity *shooter, int32_t ammo_index,
                           const World &world);

} // namespace opennova::world
