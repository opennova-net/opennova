// The impact damage and the energy a striking round loses (impact_damage.h).
#include <runtime/world/impact_damage.h>

#include <algorithm>
#include <cmath>

#include <base/io/fixed.h>
#include <base/io/tick_rate.h>

#include <runtime/world/entity.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// `fild; fsqrt; fistp` under the default round-to-nearest control word, then
// `shl 8`. A negative operand is an invalid fsqrt whose integer-indefinite
// result shifts to 0. [orig: @0x4E9142..0x4E9150, @0x4E915B..0x4E9169]
int32_t fistp_sqrt_shl8(int32_t value) {
    if (value < 0) return 0;
    const int32_t root = static_cast<int32_t>(std::nearbyint(std::sqrt(double(value))));
    return signed_from_u32(uint32_t(root) << 8);
}

// Armor removes kinetic energy, independently of the face-material cost.
// [orig: Projectile_ApplyDragDeceleration @0x4e5cd0]
void apply_armor_deceleration(FixedVec3 &velocity, const AmmoTableEntry &ammo,
                             int32_t density) {
    int32_t speed = arithmetic_shift_right_16(
            wrapped_signed_product(fixed_magnitude(velocity), io::kTicksPerSecondInt));
    speed = std::min(speed, 1219);
    const double energy = double(wrapped_signed_product(speed, speed)) -
            double(wrapped_signed_product(density, 2)) * 1000000.0 / ammo.weight_in_grains;
    const int32_t new_speed = energy > 0.0
            ? wrapped_signed_product(static_cast<int32_t>(std::sqrt(energy)), 65536) /
                    io::kTicksPerSecondInt
            : 0;
    const double length = std::sqrt(double(velocity.x) * velocity.x +
            double(velocity.y) * velocity.y + double(velocity.z) * velocity.z);
    if (length == 0.0 || new_speed == 0) {
        velocity = {};
        return;
    }
    const double normalize = 65536.0 / length;
    velocity.x = retail_q16_mul_rhu(new_speed, static_cast<int32_t>(velocity.x * normalize));
    velocity.y = retail_q16_mul_rhu(new_speed, static_cast<int32_t>(velocity.y * normalize));
    velocity.z = retail_q16_mul_rhu(new_speed, static_cast<int32_t>(velocity.z * normalize));
}

} // namespace

bool impact_is_critical(const Entity &target, int32_t hit_zone,
                        int32_t hit_bone) {
    if (target.item_type != 3) return false;
    return (target.item_attrib & kItemAttribLandable) != 0
        ? seat_hit_bone_is_critical(hit_bone)
        : hit_zone_is_critical(hit_zone);
}

bool subtract_material_energy(FixedVec3 &velocity, int32_t mass, int32_t cost) {
    if (cost == 0) return true;
    const int32_t squared = signed_from_u32(
            uint32_t(retail_q16_mul_rhu(velocity.x, velocity.x)) +
            uint32_t(retail_q16_mul_rhu(velocity.y, velocity.y)) +
            uint32_t(retail_q16_mul_rhu(velocity.z, velocity.z)));
    const int32_t energy = retail_q16_mul_rhu(squared, mass);
    if (energy <= cost) return false; // [orig: signed `jle` @0x4E9116]
    const int32_t remaining_squared = static_cast<int32_t>(
            (int64_t(energy - cost) * io::kFp16OneInt) / mass);
    const int32_t speed = fistp_sqrt_shl8(squared);
    const int32_t remaining_speed = fistp_sqrt_shl8(remaining_squared);
    if (speed == 0) return false; // [orig: `test ecx, ecx; jz` @0x4E916C]
    const int32_t ratio = static_cast<int32_t>(
            (int64_t(remaining_speed) * io::kFp16OneInt) / speed);
    velocity.x = retail_q16_mul_rhu(velocity.x, ratio);
    velocity.y = retail_q16_mul_rhu(velocity.y, ratio);
    velocity.z = retail_q16_mul_rhu(velocity.z, ratio);
    return true;
}

// The kinetic damage number [orig: Weapon_CalcImpactDamage @ 0x4EC920]. `vel` is
// units/tick; the original wraps 62 * |vel|_16.16 as signed 32-bit, shifts it by 16,
// applies only an upper clamp of 1219 (@0x4ecad6), then wraps the signed speed*weight
// product before /875 (@0x4ecb1a). It next applies the hit-zone multiplier (now fed by posed
// COBJ hit zones) and the shooter-class byte (0.9 / 1.1 — replicated from the loadout's per-ammo
// class table), floors at min_damage (@0x4ecb3a), and caps at max_damage when > 0 (@0x4ecb42);
// dfx2.exe floors and caps first, over its own zone table [orig: @0x4e9a50 (dfx2.exe)]. MP
// authority and OneShotKill are explicit inputs, including the non-authority zero return @0x4ec933.
int32_t calc_impact_damage(FixedVec3 &velocity_q16, const AmmoTableEntry &ammo,
                           int32_t hit_zone, int32_t hit_bone, const Entity &target,
                           const Entity *shooter, int32_t ammo_index,
                           const World &world) {
    if (world.rules.mp_session) {
        if (!world.rules.projectile_authority) return 0;
        if (world.rules.one_shot_kill) return 2000;
    }
    uint8_t damage_class = 0;
    if (shooter != nullptr && ammo_index >= 0 &&
        static_cast<size_t>(ammo_index) < shooter->ammo_damage_class.size())
        damage_class = shooter->ammo_damage_class[static_cast<size_t>(ammo_index)];
    const bool body_armor = target.item_type == 3 &&
            (target.item_attrib & kItemAttribLandable) == 0 &&
            hit_zone >= 0 && hit_zone <= 4 && (target.carry_flags & 8u) != 0;
    const int armor_class = damage_class == 1 || damage_class == 2 ? damage_class : 0;
    if (body_armor)
        apply_armor_deceleration(velocity_q16, ammo, ammo.armor_density[armor_class]);
    int32_t speed_scaled = arithmetic_shift_right_16(
        wrapped_signed_product(fixed_magnitude(velocity_q16), io::kTicksPerSecondInt));
    if (speed_scaled >= 1219) speed_scaled = 1219;
    int32_t damage = wrapped_signed_product(speed_scaled, ammo.weight_in_grains) / 875;
    const bool dfx2 = world.rules.title == gameprofile::GAME_DFX2; // clamps first (DFX2-16)
    if (dfx2) damage = clamp_impact_damage(damage, ammo);
    if (target.item_type == 3) {
        // The zone/bone -> multiplier tables live in round_sim.h (the debug views read them too).
        double zone_scale = 1.0;
        if ((target.item_attrib & kItemAttribLandable) != 0) {
            // The attrib-0x200 seat branch reads the primary/reaction section
            // ray[31] (hitZoneData+124 @0x4ec977), NOT the damage zone ray[32]
            // the normal-infantry table below reads (@0x4ec9bf). The two differ
            // whenever the bone walk crosses more than one sphere.
            zone_scale = seat_hit_bone_damage_multiplier(hit_bone);
        } else {
            zone_scale = dfx2 ? dfx2_hit_zone_damage_multiplier(hit_zone)
                              : hit_zone_damage_multiplier(hit_zone);
        }
        damage = static_cast<int32_t>(static_cast<double>(damage) * zone_scale);

        if (damage_class == 1)
            damage = static_cast<int32_t>(static_cast<double>(damage) *
                                          static_cast<double>(0.9f));
        else if (damage_class == 2)
            damage = static_cast<int32_t>(static_cast<double>(damage) *
                                          static_cast<double>(1.1f));
    }
    if (!dfx2) damage = clamp_impact_damage(damage, ammo);
    if (target.item_type != 3) return damage;
    if (body_armor)
        apply_armor_deceleration(velocity_q16, ammo, ammo.armor_density[armor_class]);
    // Every person hit then bleeds the round through the drag/bounce force
    // with the fixed surface multiplier 35, armored or not: the three armor
    // arms and the no-armor path all fall into the same call.
    // [orig: Weapon_CalcImpactDamage — the type-3 gate @0x4ecb96, the
    //  `push 23h; push 1; push esi; call Entity_ApplyDragAndBounceForce`
    //  leg @0x4ecc38..0x4ecc42]
    projectile_apply_person_hit_drag(velocity_q16, ammo);
    return damage;
}

} // namespace opennova::world
