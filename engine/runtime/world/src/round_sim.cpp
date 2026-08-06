// Round flight and presentation, with authoritative consequences carried explicitly
// per round. See round_sim.h and docs/net/novaworld-net-re.md §5.60.
#include "world/round_sim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <io/bam.h>

#include "world/ai.h"
#include "world/ammo_table.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/infantry.h"
#include "world/throwables.h"
#include "world/weapon_table.h"
#include "world/world.h"

namespace opennova::world {

namespace {

constexpr double kPi = 3.14159265358979323846;
// BAM32 -> radians (full turn = 2^32) [orig: engine-wide BAM convention, angle.h].
constexpr double kRadPerBam = (2.0 * kPi) / 4294967296.0;
constexpr int32_t kProjectileGravityQ16 = 167;
constexpr int32_t kDragTableSize = 1220;

constexpr uint32_t rotl32(uint32_t value, unsigned count) {
    return (value << count) | (value >> (32u - count));
}

constexpr uint32_t rotr32(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

uint32_t spread_hash(uint32_t seed) {
    const uint64_t square = static_cast<uint64_t>(seed) * seed;
    const uint32_t low = static_cast<uint32_t>(square) - seed;
    uint32_t high = static_cast<uint32_t>(square >> 32);
    high = rotr32((high ^ 0xCC1CDC1Du) + 0xC11ABB09u, 16);
    uint32_t hash = rotl32(low + high, 18);
    if ((hash & 0x80000000u) != 0) hash += 0x001ABB09u;
    return hash;
}

} // namespace

RandomSpreadOffset weapon_calc_random_spread_offset(
        int32_t spread_fp16, uint32_t seed, int32_t vertical_fp16,
        bool use_spread_two) {
    if (spread_fp16 == 0) return {};

    const uint32_t hash = spread_hash(seed);
    uint32_t mixed = rotl32(hash, 12);
    if ((mixed & 0x80000000u) != 0) mixed += 0x001ABB09u;
    const uint32_t a = mixed & 0xFFFFu;
    const uint32_t b = (hash >> 8) & 0xFFFFu;

    // These are the exact binary32 constants loaded by retail. Converting the
    // float values to double before the arithmetic mirrors the x87 PC53 path;
    // rounding every intermediate to float differs by several BAM units.
    // [orig: Weapon_CalcRandomSpreadOffset @0x4E4120]
    constexpr float kDegreeToBam16 = 182.04443359375f; // bits 0x43360B60
    constexpr float kQuarterTurnPhase = 2.3968430468812585e-05f; // 0x37C90FD0
    constexpr float kFullTurnPhase = 9.587372187525034e-05f; // 0x38C90FD0
    constexpr float kUnit16 = 1.0f / 65536.0f; // 0x37800000
    const double scale = static_cast<double>(kDegreeToBam16);
    const double quarter_phase = static_cast<double>(kQuarterTurnPhase);
    const double full_phase = static_cast<double>(kFullTurnPhase);
    const double unit16 = static_cast<double>(kUnit16);

    RandomSpreadOffset out;
    if (!use_spread_two) {
        const double radius = std::cos(static_cast<double>(a) * quarter_phase);
        out.yaw_bam = static_cast<int32_t>(
                static_cast<double>(spread_fp16) * scale * radius *
                std::sin(static_cast<double>(b) * full_phase));
        const int32_t pitch_spread =
                vertical_fp16 != 0 ? vertical_fp16 : spread_fp16;
        out.pitch_bam = static_cast<int32_t>(
                static_cast<double>(pitch_spread) * scale * radius *
                std::cos(static_cast<double>(b) * full_phase));
        return out;
    }

    if (vertical_fp16 == 0) {
        const double radius = static_cast<double>(spread_fp16) * scale *
                              static_cast<double>(b) * unit16;
        out.yaw_bam = static_cast<int32_t>(
                radius * std::cos(static_cast<double>(b) * full_phase));
        out.pitch_bam = static_cast<int32_t>(
                radius * std::sin(static_cast<double>(b) * full_phase));
        return out;
    }

    out.yaw_bam = static_cast<int32_t>(
            static_cast<double>(spread_fp16) * scale *
            static_cast<double>(b) * unit16 *
            std::cos(static_cast<double>(b) * full_phase));
    out.pitch_bam = static_cast<int32_t>(
            static_cast<double>(vertical_fp16) * scale *
            static_cast<double>(a) * unit16 *
            std::sin(static_cast<double>(a) * full_phase));
    return out;
}

RandomSpreadOffset weapon_calc_shotgun_spread_offset(
        int32_t pie_slice_bam, uint16_t radial_draw, uint16_t phase_draw) {
    // Retail loads these as binary32 and keeps the products/trig results in
    // x87 PC53 until each fistp truncation. [orig:
    // Weapon_SpawnProjectileBurstWithSpread @0x4EBC4B..0x4EBD21]
    constexpr float kQuarterTurnPhase = 2.3968430468812585e-05f; // 0x37C90FD0
    constexpr float kFullTurnPhase = 9.587372187525034e-05f; // 0x38C90FD0
    const double radius =
            static_cast<double>(pie_slice_bam) *
            std::cos(static_cast<double>(radial_draw) *
                     static_cast<double>(kQuarterTurnPhase));
    const int32_t radial_bam = static_cast<int32_t>(radius);
    const double phase = static_cast<double>(phase_draw) *
                         static_cast<double>(kFullTurnPhase);
    return RandomSpreadOffset{
            static_cast<int32_t>(static_cast<double>(radial_bam) *
                                 std::cos(phase)),
            static_cast<int32_t>(static_cast<double>(radial_bam) *
                                 std::sin(phase))};
}

// Queue an explosive round's kill zone at its stop. Knife/medic/bullet classes
// never take this projectile detonation path. Shared with the throwable
// motors [orig: WeaponEffect_PushExplosionQueueEntry @ 0x4e83c0].
void detonate_round(World &world, const LiveRound &round, const Vec3 &at,
                    const AmmoTableEntry &ammo) {
    if (ammo.kz_maxradius <= 0.0f || ammo.kz_damage == 0) return;
    if (ammo.kztype != ammo_kz::kStandard &&
        ammo.kztype != ammo_kz::kRadiusBlast &&
        ammo.kztype != ammo_kz::kC4 && ammo.kztype != ammo_kz::kSlash)
        return;
    ExplosionEntry explosion;
    explosion.pos = at;
    explosion.dir_bam = round.yaw_bam; // [orig: entry dir <- the round angles]
    explosion.type = ammo.kztype;
    explosion.ammo_index = round.ammo_index;
    explosion.owner = round.owner;
    explosion.hit_word = round.shot_seq;
    explosion.radius_override = 0.0f;
    world.explosions.queue_explosion(world, explosion);
}

namespace {

void push_round_debug(RoundSim &sim, const RoundDebugEvent &event) {
    sim.debug_trail[static_cast<size_t>(sim.debug_trail_next)] = event;
    sim.debug_trail_next =
        (sim.debug_trail_next + 1) % RoundSim::kDebugTrailCap;
    if (sim.debug_trail_count < RoundSim::kDebugTrailCap)
        ++sim.debug_trail_count;
}

struct DragBand {
    int32_t upper_fps;
    double coefficient;
    double exponent;
};

// Ballistic retardation bands used to build g_ProjectileDragTable.
// [orig: Projectile_InitDragTable, consumed by Entity_ApplyDragAndBounceForce
// @0x4E5EC0]. The source sweep is 0..3999 ft/s; repeated metric-bin writes are
// intentional, and bin 1219 remains the zero-initialized BSS edge entry.
constexpr std::array<DragBand, 40> kDragBands{{
    {65,   .000052062141073205883, 2.0},
    {100,  .000057926849570745460, 1.975},
    {250,  .000072252473275904129, 1.925},
    {550,  .000093379919571313886, 1.875},
    {600,  .000092265570919734271, 1.875},
    {640,  .000057111746887342401, 1.95},
    {700,  .000018390150958995790, 2.125},
    {750,  .000003569169672385163, 2.375},
    {780,  6.824429329105383e-7, 2.625},
    {810,  1.2911592008462161e-7, 2.875},
    {860,  1.0455132639662721e-8, 3.25},
    {905,  3.5661294709749512e-10, 3.75},
    {945,  1.1850970456898540e-11, 4.25},
    {980,  3.8550994248074508e-13, 4.75},
    {1025, 2.9169382641004953e-14, 5.125},
    {1060, 1.2285973707747459e-14, 5.25},
    {1100, 2.9382369548473309e-14, 5.125},
    {1150, 4.0701579611478821e-13, 4.75},
    {1185, 1.3797465900250881e-11, 4.25},
    {1220, 4.7454695371573713e-10, 3.75},
    {1280, 1.6579563210676119e-8, 3.25},
    {1315, 2.4194851918955649e-7, 2.875},
    {1360, 1.4569223287202981e-6, 2.625},
    {1420, 8.8477425816744163e-6, 2.375},
    {1520, 5.4318962664623510e-5, 2.125},
    {1595, 1.9544732100373980e-4, 1.95},
    {1730, 4.0861467973051169e-4, 1.85},
    {1810, 8.6099575924682592e-4, 1.75},
    {1890, .0015110010288919039, 1.675},
    {2015, .0022033295422978091, 1.625},
    {2225, .0032229422712623359, 1.575},
    {2460, .0039043682186912492, 1.55},
    {2680, .0032095597831298811, 1.575},
    {2830, .0021628872029303761, 1.625},
    {2960, .0014537215601872859, 1.675},
    {3130, .00097480736940786959, 1.725},
    {3295, .00065204218718926618, 1.775},
    {3450, .00043499051111156362, 1.825},
    {3680, .00028947510268197461, 1.875},
    {3999, .00019203392687556139, 1.925},
}};

const std::array<int32_t, kDragTableSize> &projectile_drag_table() {
    static const std::array<int32_t, kDragTableSize> table = [] {
        std::array<int32_t, kDragTableSize> values{};
        for (int32_t feet_per_second = 0; feet_per_second <= 3999; ++feet_per_second) {
            const DragBand *band = &kDragBands.back();
            for (const DragBand &candidate : kDragBands) {
                if (feet_per_second <= candidate.upper_fps) {
                    band = &candidate;
                    break;
                }
            }
            const int32_t destination = feet_per_second * 381 / 1250;
            const int64_t raw = static_cast<int64_t>(
                std::pow(static_cast<double>(feet_per_second), band->exponent) *
                band->coefficient * 19975.3728);
            if (destination >= 0 && destination < kDragTableSize)
                values[static_cast<size_t>(destination)] = static_cast<int32_t>(raw / 62);
        }
        return values;
    }();
    return table;
}

int32_t fixed_magnitude(const FixedVec3 &v) {
    const double magnitude = std::sqrt(static_cast<double>(v.x) * v.x +
                                       static_cast<double>(v.y) * v.y +
                                       static_cast<double>(v.z) * v.z);
    // Retail caps immediately below the signed ftol overflow boundary.
    constexpr double kFtolLimit = 2147418112.0; // float bits 0x4EFFFE00
    return static_cast<int32_t>(std::min(magnitude, kFtolLimit));
}

// Convert a modulo-2^32 result to the signed value represented by the same bits
// without relying on implementation-defined unsigned-to-signed conversion.
int32_t signed_from_u32(uint32_t value) {
    if (value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return static_cast<int32_t>(value);
    return -1 - static_cast<int32_t>(std::numeric_limits<uint32_t>::max() - value);
}

int32_t wrapped_signed_product(int32_t lhs, int32_t rhs) {
    return signed_from_u32(static_cast<uint32_t>(lhs) * static_cast<uint32_t>(rhs));
}

int32_t drag_speed_index(int32_t magnitude_q16) {
    const int64_t scaled = static_cast<int64_t>(magnitude_q16) * 62;
    if (scaled <= 0) return 0;
    const int64_t index = scaled >> 16;
    return static_cast<int32_t>(index > 1219 ? 1219 : index);
}

int32_t arithmetic_shift_right_16(int64_t value) {
    if (value >= 0) return static_cast<int32_t>(value >> 16);
    return -static_cast<int32_t>((-value + 0xffff) >> 16);
}

Vec3 vec_from_fixed(const FixedVec3 &v) {
    return Vec3{static_cast<float>(from_fixed(v.x)),
                static_cast<float>(from_fixed(v.y)),
                static_cast<float>(from_fixed(v.z))};
}

void apply_aerodynamic_drag(FixedVec3 &velocity, const AmmoTableEntry &ammo,
                            int32_t position_z_q16, int32_t water_z_q16) {
    if (ammo.drag_fp16 == 0) return;

    const int32_t old_magnitude = fixed_magnitude(velocity);
    if (old_magnitude == 0) return;
    const int32_t old_index = drag_speed_index(old_magnitude);
    const int32_t raw = projectile_drag_table()[static_cast<size_t>(old_index)];
    const int64_t first_division =
        (static_cast<int64_t>(raw) << 16) / ammo.drag_fp16;
    const int64_t scaled_drag = first_division / 62;
    const bool underwater = water_z_q16 != 0 && position_z_q16 <= water_z_q16;
    const int64_t drag_step = underwater ? scaled_drag * 25 : scaled_drag;

    // Retail normalizes against the un-truncated x87 magnitude (PC53 = double
    // semantics), multiplying each negated component by 65536/|v| and truncating
    // [orig: the fdivr flt_7C32BC leg @0x4e5fd6-0x4e602e]. The ftol'd
    // old_magnitude above feeds only the table index.
    const double float_magnitude =
        std::sqrt(static_cast<double>(velocity.x) * velocity.x +
                  static_cast<double>(velocity.y) * velocity.y +
                  static_cast<double>(velocity.z) * velocity.z);
    const double direction_scale = 65536.0 / float_magnitude;
    const int32_t direction[3] = {
        static_cast<int32_t>(-static_cast<double>(velocity.x) * direction_scale),
        static_cast<int32_t>(-static_cast<double>(velocity.y) * direction_scale),
        static_cast<int32_t>(-static_cast<double>(velocity.z) * direction_scale),
    };
    const int32_t delta[3] = {
        arithmetic_shift_right_16(drag_step * direction[0] + 0x8000),
        arithmetic_shift_right_16(drag_step * direction[1] + 0x8000),
        arithmetic_shift_right_16(drag_step * direction[2] + 0x8000),
    };

    velocity.x += delta[0];
    velocity.y += delta[1];
    velocity.z += delta[2];
    // The reversal test rounds each Q32 product term to Q16 BEFORE the 32-bit
    // wrapping sum [orig: the three (delta*vel + 0x8000) >> 16 legs
    // @0x4e61f8-0x4e624a], so tiny same-sign terms can round to zero where an
    // exact 64-bit dot would not.
    auto dot_term = [](int32_t d, int32_t v) {
        return static_cast<uint32_t>(
            static_cast<uint64_t>(static_cast<int64_t>(d) * v + 0x8000) >> 16);
    };
    const int32_t overshoot_dot = signed_from_u32(dot_term(delta[0], velocity.x) +
                                                  dot_term(delta[1], velocity.y) +
                                                  dot_term(delta[2], velocity.z));
    if (overshoot_dot > 0) velocity = FixedVec3{};

    const int32_t post_index = drag_speed_index(fixed_magnitude(velocity));
    if (post_index < ammo.min_stable_velocity) {
        velocity.x -= static_cast<int32_t>((static_cast<int64_t>(velocity.x) + 16) >> 5);
        velocity.y -= static_cast<int32_t>((static_cast<int64_t>(velocity.y) + 16) >> 5);
        if (velocity.z > 0)
            velocity.z -= static_cast<int32_t>((static_cast<int64_t>(velocity.z) + 16) >> 5);
        // This apparently inverted gate is retail behavior: the ordinary gravity
        // pass skipped NoGravity, then the below-stable branch adds the same step.
        if ((ammo.flags & kAmmoFlagNoGravity) != 0) velocity.z -= kProjectileGravityQ16;

        // A threshold crossing also applies tumble_error in a randomized local
        // frame. Its gate is characterized, but the PRNG/frame producer is not;
        // deterministic damping above is the exact non-random portion.
        (void)old_index;
    }
}

// The kinetic damage number [orig: Weapon_CalcImpactDamage @ 0x4EC920]. `vel` is
// units/tick; the original wraps 62 * |vel|_16.16 as signed 32-bit, shifts it by 16,
// applies only an upper clamp of 1219 (@0x4ecad6), then wraps the signed speed*weight
// product before /875 (@0x4ecb1a). It next applies the hit-zone
// multiplier (now fed by posed COBJ hit zones) and the shooter-class byte (0.9 / 1.1 —
// replicated from the loadout's per-ammo class table), floors at min_damage
// (@0x4ecb3a), and caps at max_damage when > 0 (@0x4ecb42). Multiplayer authority and
// OneShotKill are explicit inputs, including the non-authority zero return @0x4ec933.
int32_t calc_impact_damage(const FixedVec3 &velocity_q16, const AmmoTableEntry &ammo,
                           int32_t hit_zone, int32_t hit_bone, Entity &target,
                           const Entity *shooter, int32_t ammo_index,
                           const World &world) {
    if (world.mp_session) {
        if (!world.projectile_authority) return 0;
        if (world.one_shot_kill) return 2000;
    }
    int32_t speed_scaled = arithmetic_shift_right_16(
        wrapped_signed_product(fixed_magnitude(velocity_q16), 62));
    if (speed_scaled >= 1219) speed_scaled = 1219;
    int32_t damage = wrapped_signed_product(speed_scaled, ammo.weight_in_grains) / 875;
    if (target.item_type == 3) {
        double zone_scale = 1.0;
        if ((target.item_attrib & kItemAttribLandable) != 0) {
            // The attrib-0x200 seat branch reads the primary/reaction section
            // ray[31] (hitZoneData+124 @0x4ec977), NOT the damage zone ray[32]
            // the normal-infantry table below reads (@0x4ec9bf). The two differ
            // whenever the bone walk crosses more than one sphere.
            if (hit_bone == 2 || hit_bone == 3 || hit_bone == 6 || hit_bone == 7) {
                target.flags |= 0x800u;
                zone_scale = 6.0;
            }
        } else if (hit_zone >= 0 && hit_zone <= 4) {
            zone_scale = 1.25;
        } else if ((hit_zone >= 9 && hit_zone <= 12) ||
                   (hit_zone >= 15 && hit_zone <= 18)) {
            zone_scale = 0.5;
        } else if (hit_zone == 13 || hit_zone == 14) {
            target.flags |= 0x800u;
            zone_scale = 3.0;
        }
        damage = static_cast<int32_t>(static_cast<double>(damage) * zone_scale);

        uint8_t damage_class = 0;
        if (shooter != nullptr && ammo_index >= 0 &&
            static_cast<size_t>(ammo_index) < shooter->ammo_damage_class.size())
            damage_class = shooter->ammo_damage_class[static_cast<size_t>(ammo_index)];
        if (damage_class == 1)
            damage = static_cast<int32_t>(static_cast<double>(damage) *
                                          static_cast<double>(0.9f));
        else if (damage_class == 2)
            damage = static_cast<int32_t>(static_cast<double>(damage) *
                                          static_cast<double>(1.1f));
    }
    if (damage <= ammo.min_damage) damage = ammo.min_damage;
    if (ammo.max_damage > 0 && damage >= ammo.max_damage) damage = ammo.max_damage;
    return damage;
}

// [orig: Entity_CountMountedEntities @ 0x435970] The pool-0 walk counts a live
// candidate whose ATTACH parent (+40 — our mount_target) is the vehicle, or
// whose attach parent's groundEntity (+0x28 — our ground_target) is: a person
// seated on a deck-standing gun counts toward the carrier. Deck-standers with
// no attach do not count.
int vehicle_occupant_count(const World &world, EntityHandle vehicle) {
    int count = 0;
    world.registry.for_each([&](const Entity &candidate) {
        if (candidate.handle.pool() != 0) return;
        if (!candidate.has_item_def) return;
        if ((candidate.flags & 2u) != 0) return;
        if (!candidate.mounted) return;
        if (candidate.mount_target == vehicle) {
            ++count;
            return;
        }
        const Entity *carrier = world.registry.get(candidate.mount_target);
        if (carrier != nullptr && carrier->ground_target == vehicle) ++count;
    });
    return count;
}

int32_t apply_vehicle_occupant_scale(const World &world, const Entity &target,
                                     int32_t damage) {
    if (target.item_type != 1 || !target.has_item_def || damage <= 0) return damage;
    const int count = vehicle_occupant_count(world, target.handle);
    if (count <= 1) return damage;
    double factor = static_cast<double>(count) *
                    static_cast<double>(target.damage_reduc_pp);
    if (factor > static_cast<double>(target.damage_reduc_max))
        factor = static_cast<double>(target.damage_reduc_max);
    const int32_t reduction = static_cast<int32_t>(static_cast<double>(damage) * factor);
    return damage - reduction;
}

// Normalized flight direction for the impact descriptor
// [orig: Projectile_SpawnImpactEffect @ 0x4e9b80].
Vec3 flight_direction(const Vec3 &vel) {
    const float len = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    if (len <= 0.0f) return Vec3{0.0f, 0.0f, 1.0f};
    return Vec3{vel.x / len, vel.y / len, vel.z / len};
}

RoundSourceState resolve_round_source(World &world,
                                      const RoundSpawnParams &params) {
    if (params.source_state != nullptr) return *params.source_state;

    RoundSourceState source;
    Entity *entity = world.registry.get(params.owner);
    if (entity == nullptr) return source;
    const uint32_t flags = entity->flags | entity->engine_flags;
    source.person_with_item_def = entity->has_item_def && entity->item_type == 3;
    source.player = (flags & kEntityFlagPlayer) != 0;
    source.scope_raised = (flags & kEntityFlagScopeRaised) != 0;
    source.underwater = (flags & kEntityFlagDrowning) != 0;

    AiEntity *body = world.ai != nullptr ? world.ai->for_handle(params.owner) : nullptr;
    const int32_t source_z =
            body != nullptr ? body->pos[2] : to_fixed(entity->position.z);
    source.underwater = source.underwater ||
                        (world.env.water_z != 0 && source_z < world.env.water_z);
    if (body == nullptr) {
        source.stance_category = entity->mounted ? 1 : 2;
        return source;
    }

    source.recoil_pitch = &body->inf.recoil_pitch;
    source.weapon_weight_spread = &body->inf.weapon_weight_spread;
    source.scope_raised = source.scope_raised || body->inf.scope_raised;
    const bool forced_standing = body->inf.airborne || source.underwater ||
                                 (flags & kEntityFlagInAir) != 0;
    if (entity->mounted) {
        source.stance_category = 1;
    } else if (forced_standing) {
        source.stance_category = 2;
    } else if (body->inf.stance == InfantryState::Stance::kProne) {
        source.stance_category = 0;
    } else if (body->inf.stance == InfantryState::Stance::kCrouch) {
        source.stance_category = 1;
    } else {
        source.stance_category = 2;
    }
    return source;
}

void apply_round_recoil(const AmmoTableEntry &ammo,
                        const RoundSourceState &source) {
    if (!source.person_with_item_def || source.recoil_pitch == nullptr) return;
    const int category = std::min<int>(source.stance_category, 2);
    const int shift = source.underwater ? 20 : 18;
    int32_t impulse = static_cast<int32_t>(ammo.recoil[category]) << shift;
    if (source.scope_raised) impulse = impulse * 3 / 4;
    *source.recoil_pitch = io::bam_add(*source.recoil_pitch, impulse);
}

void record_round_fire(RoundSim &sim, const RoundSpawnParams &params) {
    FireEvent event;
    event.shooter = params.owner;
    event.shooter_handle = params.shooter_handle;
    event.ammo_index = params.ammo_index;
    event.origin = params.origin;
    // The ring/presentation descriptor is deliberately pre-spread. The final
    // randomized angles live only on the spawned round.
    // [orig: RoundData_AddRound @0x4FDB40 -> RoundData_SpawnRound @0x4EC0D0]
    event.yaw_bam = params.dir_yaw_bam;
    event.pitch_bam = params.dir_pitch_bam;
    event.wire_round_flags = params.wire_round_flags;
    event.adm_index = params.adm_index;
    sim.fired.push_back(event);
}

} // namespace

void RoundSim::reset() noexcept {
	rounds.assign(kCapacity, LiveRound{});
	active_count = 0;
	deaths.clear();
	impacts.clear();
	hits.clear();
	fired.clear();
	next_impact_order = 1;
	next_presentation_generation_ = 1;
	debug_trail = {};
	debug_trail_next = 0;
	debug_trail_count = 0;
	trails.reset(); // [orig: the pool memset in CEffectEmitterPool_ResetAndBuildStyles
	                //  @ 0x5db3b0, run from Game_StartMission]
	remote_visual_tracer_counters_.clear();
}

int RoundSim::spawn(World &world, const RoundSpawnParams &params,
                    RoundConsequenceMode mode) {
    const AmmoTableEntry *ammo = world.ammo.by_index(params.ammo_index);
    if (ammo == nullptr) return -1;
    const bool authoritative =
        mode == RoundConsequenceMode::Authoritative &&
        (!world.mp_session || world.projectile_authority);
    // The retail spawn dispatch order [orig: RoundData_SpawnRound @ 0x4ec1f3..
    // 0x4ec2a5]: instantkillzone -> Detonatesatchels -> designator -> claymore
    // fan -> shotgun -> the ballistic default.
    if ((ammo->flags & kAmmoFlagInstantKillZone) != 0) {
        // instantkillzone: the kill zone queues at the spawn point, no round
        // flies [orig: @ 0x4ec1f3 -> WeaponEffect_PushExplosionQueueEntry; the
        // kztype==1 knife raycast leaf stays with the FSM knife path].
        ExplosionEntry explosion;
        explosion.pos = params.origin;
        explosion.dir_bam = params.dir_yaw_bam;
        explosion.type = ammo->kztype;
        explosion.ammo_index = params.ammo_index;
        explosion.owner = params.owner;
        explosion.hit_word = params.shot_seq;
        if (authoritative) world.explosions.queue_explosion(world, explosion);
        if (impacts.size() < kMaxPendingImpacts) {
            // the detonation's obj-row effect [orig: AmmoDef_ProcessImpactEffect
            // tag 4 at the descriptor position in every think handler]
            RoundImpact imp;
            imp.position = params.origin;
            imp.direction = Vec3{0.0f, 0.0f, 1.0f};
            imp.ammo_index = params.ammo_index;
            imp.effect_tag = 4;
            imp.tick = world.logic_tick;
            imp.source_order = next_impact_order++;
            impacts.push_back(imp);
        }
        return -1;
    }
    if ((ammo->flags & kAmmoFlagDetonateSatchels) != 0) {
        // Detonatesatchels [orig: @ 0x4ec234 -> Entity_DetonateSatchelsByOwner]
        if (authoritative)
            world.throwables.detonate_satchels_by_owner(world, params.owner);
        return -1;
    }
    if ((ammo->flags & kAmmoFlagDesignateTarget) != 0) {
        // The designator dispatches to its tracker instead of allocating a
        // projectile. That tracker is outside RoundSim; critically, this return
        // precedes both shotgun/ordinary spawn and recoil.
        // [orig: RoundData_SpawnRound @0x4EC249]
        return -1;
    }
    if ((ammo->flags & kAmmoFlagClaymore) != 0) {
        // the claymore shrapnel fan [orig: @ 0x4ec288 -> Weapon_SpawnProjectileBurst]
        return spawn_burst(world, params, *ammo, mode);
    }
    const RoundSourceState source = resolve_round_source(world, params);
    const WeaponTableEntry *weapon = world.weapons.by_index(params.adm_index);
    if ((ammo->flags & kAmmoFlagShotgun) != 0) {
        // Shotgun is a separate pellet-fan leaf: it never reads weapon ERROR or
        // the rules gate, but the ordinary ammo recoil is applied after the fan.
        // [orig: RoundData_SpawnRound @0x4EC378]
        const int first = spawn_burst(
                world, params, *ammo, mode, /*shotgun_spread=*/true);
        record_round_fire(*this, params);
        apply_round_recoil(*ammo, source);
        return first;
    }
    // Null/non-ballistic ammo spawns nothing at this altitude: the Knife(1)/Medic(3)
    // kill zones are the immediate-raycast leaves [orig: kztype dispatch @0x4ec21f],
    // `hasitem` ammo places an item entity instead of flying.
    if (ammo->velocity <= 0) return -1;
    if (ammo->kztype == 1 || ammo->kztype == 3) return -1;
    if ((ammo->flags & kAmmoFlagHasItem) != 0) return -1;

    int slot = -1;
    for (int i = 0; i < kCapacity; ++i) {
        if (!rounds[static_cast<size_t>(i)].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1; // pool exhausted [orig: allocator scan @0xB7DFA0 flags]

    int32_t final_yaw = params.dir_yaw_bam;
    int32_t final_pitch = params.dir_pitch_bam;
    if (weapon != nullptr) {
        // Weapon ERROR is player-only and rule-gated. The current shot consumes
        // the PREVIOUS recoil accumulator; its own recoil is added after spawn.
        // Projectile row = verticalSpread ? 3 : stance category.
        // [orig: RoundData_SpawnRound @0x4EC0D0]
        if (source.player && weapon_spread_enabled) {
            const bool vertical_spread = (params.subtype & 0x80u) != 0;
            const int category = std::min<int>(source.stance_category, 2);
            const int row = vertical_spread ? 3 : category;
            int32_t spread = weapon->error_fp16[row];
            if (source.recoil_pitch != nullptr) {
                spread = io::bam_add(
                        spread, io::bam_sar(*source.recoil_pitch, 8));
            }
            if (source.weapon_weight_spread != nullptr) {
                spread = io::bam_add(
                        spread, io::bam_sar(*source.weapon_weight_spread, 7));
            }
            const int32_t vertical =
                    (vertical_spread || category == 0)
                            ? weapon->error_up_theta_fp16
                            : weapon->error_hip_theta_fp16;
            const RandomSpreadOffset offset =
                    weapon_calc_random_spread_offset(
                            spread, params.shot_seq, vertical,
                            (weapon->flags & weapon_flag::kUseSpreadTwo) != 0);
            final_yaw = io::bam_add(final_yaw, offset.yaw_bam);
            final_pitch = io::bam_add(final_pitch, offset.pitch_bam);
        }
    } else if (ammo->spread_error_fp16 != 0) {
        // A missing AdmDef falls back to ammo.def ERROR. This leaf is not a
        // weapon-rule check and always uses the ordinary helper.
        // [orig: RoundData_SpawnRound @0x4EC0D0]
        const RandomSpreadOffset offset = weapon_calc_random_spread_offset(
                ammo->spread_error_fp16, params.shot_seq, 0, false);
        final_yaw = io::bam_add(final_yaw, offset.yaw_bam);
        final_pitch = io::bam_add(final_pitch, offset.pitch_bam);
    }

    // Wire fire direction -> mission-frame unit vector. The 0x06 yaw BAM IS the mission
    // bearing directly — NOT the 0x0A euler_z heading frame (which is 90deg - mission
    // yaw): wire-validated on the v29 duel baselines (wire yaw -122.0/45.6 deg vs true
    // shooter->victim bearings -122.4/44.2 deg; the old 90-minus mapping missed by 26 deg
    // and only coincided on the 45-deg diagonal — the asymmetric-kill bug, D-NET-153).
    // The original spawner builds X=sinYaw*cosPitch, Y=cosYaw*cosPitch, Z=sinPitch in
    // engine axes [orig: RoundData_SpawnRound @0x4ec5e9 / Weapon_SpawnSingleProjectile
    // @0x4ebf51]; in this mission frame that lands as (cos yaw, sin yaw, sin pitch).
    const double bearing = double(final_yaw) * kRadPerBam;
    const double pitch = double(final_pitch) * kRadPerBam;
    const double cp = std::cos(pitch);
    double speed_per_tick = double(ammo->velocity) / 62.0; // [orig: speed/62 @0x4ec508]
    // The PowerThrow charge byte scales the launch speed for 1..254; 0 and 255
    // mean full [orig: (charge - 1) <= 0xFD gate @ 0x4ec5bb, x charge/256].
    if (static_cast<uint8_t>(params.charge - 1u) <= 0xFDu)
        speed_per_tick = speed_per_tick * double(params.charge) / 256.0;

    LiveRound &r = rounds[static_cast<size_t>(slot)];
    r = LiveRound{};
    r.active = true;
    r.consequence_mode = mode;
    r.owner = params.owner;
    r.shooter_handle = params.shooter_handle;
    r.shooter_carrier_handle = params.shooter_carrier_handle;
    r.ammo_index = params.ammo_index;
    r.adm_index = params.adm_index;
    r.shot_seq = params.shot_seq;
    r.presentation_generation = next_presentation_generation_++;
    r.pos = params.origin;
    r.vel.x = static_cast<float>(std::cos(bearing) * cp * speed_per_tick);
    r.vel.y = static_cast<float>(std::sin(bearing) * cp * speed_per_tick);
    r.vel.z = static_cast<float>(std::sin(pitch) * speed_per_tick);
    r.age_ticks = 0;
    // noage rounds never expire on time [orig: flag 0x4000]; everything else uses the
    // ammo max_age (already in 62 Hz ticks).
    r.max_age_ticks = ((ammo->flags & kAmmoFlagNoAge) != 0) ? INT32_MAX : ammo->max_age_ticks;

    // The tracer decision [orig: RoundData_SpawnRound @0x4ec184-0x4ec1e5]: every
    // tracer_rate-th round per shooter is a tracer (the counter lives on the weapon
    // slot +0x80 in the original — ours rides the shooter entity, one weapon per NPC
    // today); rate 0 = never; no shooter = every round; the FORCETRACER ammo flag
    // (0x8000) rides every round. Team = the shooter team byte [orig: round+0x162
    // copy @0x4ec705; the slot+4 & 0x200 0xFF override is unmodeled].
    bool tracer = true;                    // [orig: var init @0x4ec15a]
    Entity *owner_ent = world.registry.get(params.owner);
    if (ammo->tracer_rate == 0) {
        tracer = false;                    // [orig: @0x4ec18a]
    } else if (owner_ent != nullptr) {
        // [orig: @0x4ec199-0x4ec1bb: ++counter, wrap to 0 at >= rate, tracer on wrap]
        if (++owner_ent->tracer_shot_counter >= ammo->tracer_rate)
            owner_ent->tracer_shot_counter = 0;
        tracer = (owner_ent->tracer_shot_counter == 0);
    } else if (mode == RoundConsequenceMode::VisualOnly &&
               params.shooter_handle != 0xFFFF) {
        // The retail receiver resolves H to the remote shooter and advances
        // that shooter's slot cadence. Our visual World deliberately does not
        // clone remote entities, so retain the same field by wire identity.
        uint32_t &counter = remote_visual_tracer_counters_[params.shooter_handle];
        if (++counter >= static_cast<uint32_t>(ammo->tracer_rate)) counter = 0;
        tracer = counter == 0;
    }                                      // [orig: @0x4ec1cf no slot + rate != 0 -> stays true]
    if ((ammo->flags & kAmmoFlagForceTracer) != 0) tracer = true; // [orig: forcetracer @0x4ec1db]
    r.tracer = tracer;
    // A local owner supplies the team; a decoded remote round carries the resolved
    // wire shooter's team in params (retail resolves the wire shooter entity and
    // copies its team @0x4ec705). 0xFF (always the ENEMY style) is only the truly
    // unresolvable-source arm [orig: the !sourceEntity arm @ 0x4ec721; the
    // slot+4 & 0x200 0xFF override is unmodeled].
    r.team = owner_ent != nullptr ? static_cast<uint8_t>(owner_ent->team)
                                  : params.shooter_team;

    // The tracer VISUAL — a trail channel allocated at spawn, styled friendly/enemy
    // against the presenting client's team [orig: RoundData_SpawnRound @ 0x4ec740:
    // gate (tracer && !(NoTracers rules & 1)) || FORCETRACER; style id = ammo
    // tracer_type friendly (+232) when round team == local team or shooter == local
    // player, else enemy (+236); id 0 = no channel; -> round+0x2B4].
    r.trail_slot = -1;
    const bool forcetracer = (ammo->flags & kAmmoFlagForceTracer) != 0;
    const bool same_team = r.team == local_team;
    const bool friendly_tracer =
            (local_player.valid() && params.owner.valid() &&
             params.owner.packed == local_player.packed) ||
            same_team;
    if ((tracer && !no_tracers_rule) || forcetracer) {
        const int32_t style =
                friendly_tracer ? ammo->tracer_type_friendly : ammo->tracer_type_enemy;
        if (style != 0) r.trail_slot = trails.alloc(style);
    }

    // The TrcrID item model + class bind [orig: @ 0x4ec787..0x4ec7b7 —
    // team item selection with a missing-foe -> friendly fallback, independent
    // of the per-shot tracer cadence; global NoTracers still suppresses it].
    // The result becomes round ItemTypeIndex(+28); Entity_InitFromItemDef binds
    // the class motor
    // (+452) / think (+456) from the items.def ai_function/move_function tags,
    // and the init callback seeds 1 deg/tick spin @ 0x4435A0].
    r.yaw_bam = final_yaw;
    r.pitch_bam = final_pitch;
    const int32_t item_id = throwable_item_for_viewer(
            ammo->tracer_item_friendly, ammo->tracer_item_enemy, r.team, local_team);
    if (item_id != 0 && (!no_tracers_rule || forcetracer)) {
        r.item_type_id = item_id;
        if (const ThrowableClassRow *row = world.throwables.classes.get(item_id)) {
            r.motor = row->motor;
            r.think = row->think;
        }
        r.spin_yaw = 11930464;   // 1 deg/tick [orig: Entity_InitThrowableSpin_*]
        r.spin_pitch = 11930464;
        r.spin_roll = 11930464;
    }

    // Record the fire for the host present layer (sound + muzzle effect) — the
    // inline-presentation moment of the original [orig: WeaponSlot_FireAndSpawnEffects
    // @0x53f440 runs its presentation right after Entity_FireWeaponAndSendPacket].
    record_round_fire(*this, params);
    ++active_count;
    // Same-shot ERROR used the old accumulator above. Recoil becomes visible
    // immediately but affects only later shots.
    // [orig: RoundData_SpawnRound @0x4EC8A3]
    apply_round_recoil(*ammo, source);
    return slot;
}

// The two pellet fans: claymore uses Weapon_SpawnProjectileBurst @0x4EB900
// (rectangular yaw/pitch draws), while shotgun uses the radial
// Weapon_SpawnProjectileBurstWithSpread @0x4EBBB0 distribution. Both clamp
// spread_count to 1..32 and emit plain ballistic pellets; individual pellets
// carry no tracer, model, or fire event.
int RoundSim::spawn_burst(World &world, const RoundSpawnParams &params,
                          const AmmoTableEntry &ammo, RoundConsequenceMode mode,
                          bool shotgun_spread) {
    int count = ammo.spread_count;
    if (count > 32) count = 32;
    if (count <= 0) count = 1;
    Entity *owner_ent = world.registry.get(params.owner);
    const uint32_t base_yaw = static_cast<uint32_t>(params.dir_yaw_bam) & 0xFFFF0000u;
    const uint32_t base_pitch = static_cast<uint32_t>(params.dir_pitch_bam) & 0xFFFF0000u;
    int first_slot = -1;
    for (int n = 0; n < count; ++n) {
        int slot = -1;
        for (int i = 0; i < kCapacity; ++i) {
            if (!rounds[static_cast<size_t>(i)].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) break;
        const int64_t pieslice = ammo.kz_pieslice_bam;
        const uint16_t r1 = world.throwables.fan_prng();
        const uint16_t r2 = world.throwables.fan_prng();
        int32_t yaw;
        int32_t pitch;
        if (shotgun_spread) {
            const RandomSpreadOffset offset =
                    weapon_calc_shotgun_spread_offset(
                            ammo.kz_pieslice_bam, r1, r2);
            yaw = io::bam_add(static_cast<int32_t>(base_yaw), offset.yaw_bam);
            pitch = io::bam_add(
                    static_cast<int32_t>(base_pitch), offset.pitch_bam);
        } else {
            yaw = static_cast<int32_t>(
                    base_yaw + static_cast<uint32_t>(
                            ((2 * pieslice * r1 + 0x8000) >> 16) - pieslice));
            pitch = static_cast<int32_t>(
                    base_pitch + static_cast<uint32_t>(
                            (pieslice * r2 + 0x8000) >> 16));
        }
        const double bearing = double(yaw) * kRadPerBam;
        const double pitch_rad = double(pitch) * kRadPerBam;
        const double cp = std::cos(pitch_rad);
        const double speed = double(ammo.velocity) / 62.0;
        LiveRound &r = rounds[static_cast<size_t>(slot)];
        r = LiveRound{};
        r.active = true;
        r.consequence_mode = mode;
        r.owner = params.owner;
        r.shooter_handle = params.shooter_handle;
        r.shooter_carrier_handle = params.shooter_carrier_handle;
        r.ammo_index = params.ammo_index;
        r.adm_index = params.adm_index;
        r.shot_seq = params.shot_seq;
        r.presentation_generation = next_presentation_generation_++;
        r.pos = params.origin;
        r.vel.x = static_cast<float>(std::cos(bearing) * cp * speed);
        r.vel.y = static_cast<float>(std::sin(bearing) * cp * speed);
        r.vel.z = static_cast<float>(std::sin(pitch_rad) * speed);
        r.max_age_ticks = ammo.max_age_ticks;
        r.team = owner_ent != nullptr ? static_cast<uint8_t>(owner_ent->team)
                                      : params.shooter_team;
        r.tracer = false;
        r.trail_slot = -1;
        r.yaw_bam = yaw;
        r.pitch_bam = pitch;
        if (first_slot < 0) first_slot = slot;
        ++active_count;
    }
    return first_slot;
}

void RoundSim::tick(World &world, const terrain::TerrainHeightField *terrain,
                    CollisionWorld *collision) {
    // Keep the shared query seam synchronized even on an idle round tick; a
    // mission transition may clear or replace the terrain before another
    // collision consumer runs.
    CollisionWorld *queries = collision != nullptr ? collision : world.collision;
    if (queries != nullptr) queries->terrain = terrain;
    if (active_count <= 0) {
        trails.tick();
        return;
    }

    // Production worlds use the host-owned mission CollisionWorld. Headless
    // unit callers without one still route through the same query interface.
    CollisionWorld fallback_collision;
    if (queries == nullptr) {
        fallback_collision.terrain = terrain;
        // Standalone/headless World tests have no AiSystem to publish the
        // per-tick proximity snapshot. Build the same pool tables locally so
        // the person pass keeps its retail table semantics.
        fallback_collision.build_tick_tables(world);
        queries = &fallback_collision;
    }

    for (int i = 0; i < kCapacity; ++i) {
        LiveRound &r = rounds[static_cast<size_t>(i)];
        if (!r.active) continue;
        const bool authoritative =
            r.consequence_mode == RoundConsequenceMode::Authoritative &&
            (!world.mp_session || world.projectile_authority);

        // The lifetime/armed-fuse head runs before the motor. Advance the
        // stored age before dispatch; the custom motor compensates so its
        // elapsed/remaining values match retail's post-motor decrement
        // [orig: Projectile_UpdatePhysics @ 0x4e9da7..0x4e9f4e].
        if (r.det_at_expiry || r.age_ticks >= r.max_age_ticks) {
            // Only rounds the motor armed detonate at this head; an ordinary
            // ballistic lifetime expiry vanishes silently.
            const AmmoTableEntry *fuze_ammo = world.ammo.by_index(r.ammo_index);
            if (fuze_ammo != nullptr && r.det_at_expiry) {
                if (authoritative) detonate_round(world, r, r.pos, *fuze_ammo);
                if (impacts.size() < kMaxPendingImpacts) {
                    RoundImpact imp;
                    imp.position = r.pos;
                    imp.direction = Vec3{0.0f, 0.0f, 1.0f};
                    imp.ammo_index = r.ammo_index;
                    imp.effect_tag = 4; // the ammo obj row
                    imp.tick = world.logic_tick;
                    imp.source_order = next_impact_order++;
                    impacts.push_back(imp);
                }
            }
            if (r.trail_slot >= 0) {
                trails.append(r.trail_slot, r.pos);
                trails.request_kill(r.trail_slot);
            }
            RoundDebugEvent event;
            event.tick = world.logic_tick;
            event.kind = RoundDebugEvent::kExpired;
            event.shooter = r.owner.packed;
            event.ammo_index = r.ammo_index;
            event.p0 = r.pos;
            event.p1 = r.pos;
            event.hit = r.pos;
            push_round_debug(*this, event);
            r.active = false;
            --active_count;
            continue;
        }
        ++r.age_ticks;

        if (r.trail_slot >= 0) trails.append(r.trail_slot, r.pos);

        const AmmoTableEntry *ammo = world.ammo.by_index(r.ammo_index);
        const uint32_t ammo_flags = ammo != nullptr ? ammo->flags : 0;

        // `ignore` rounds only age.
        if ((ammo_flags & kAmmoFlagIgnore) != 0) continue;
        // `useownmove` rounds run ONLY their class motor — no stock ray,
        // gravity, or drag [orig: the +452 motor leg of Projectile_UpdatePhysics
        // @ 0x4e9f06; the motor may convert the round into a placed device or
        // detonate it, releasing the slot].
        if ((ammo_flags & kAmmoFlagUseOwnMove) != 0) {
            bool alive = true;
            if (ammo != nullptr)
                alive = throwable_motor_tick(
                    world, *this, r, *ammo, queries, terrain, authoritative);
            if (!alive) {
                if (r.trail_slot >= 0) {
                    trails.append(r.trail_slot, r.pos);
                    trails.request_kill(r.trail_slot);
                }
                r.active = false;
                --active_count;
            }
            continue;
        }

        const FixedVec3 position_q16{to_fixed(r.pos.x), to_fixed(r.pos.y), to_fixed(r.pos.z)};
        FixedVec3 velocity_q16{to_fixed(r.vel.x), to_fixed(r.vel.y), to_fixed(r.vel.z)};

        // Stock pre-ray water stall: a strictly submerged round below
        // 0.25 units/tick zeroes its lifetime BEFORE the sweep and still flies
        // this tick — retail falls through to the ray and releases the round at
        // the next tick's lifetime head check [orig: the +684/+28 zero
        // @0x4ea142-0x4ea148 with no early return]. Model that as one final
        // ordinary sweep followed by retirement at the end of this iteration.
        const bool submerged_stall =
            ammo != nullptr && world.env.water_z != 0 &&
            position_q16.z < world.env.water_z && fixed_magnitude(velocity_q16) < 0x4000;

        // Exact-zero is a distinct retail leaf: no sweep or position commit, one
        // gravity step even for NoGravity ammo, and no aerodynamic drag. A
        // stalled zero round dies at the next lifetime head check without
        // another sweep.
        if (velocity_q16.x == 0 && velocity_q16.y == 0 && velocity_q16.z == 0) {
            velocity_q16.z -= kProjectileGravityQ16;
            r.vel = vec_from_fixed(velocity_q16);
            if (submerged_stall) {
                if (r.trail_slot >= 0) trails.request_kill(r.trail_slot);
                r.active = false;
                --active_count;
            }
            continue;
        }

        const FixedVec3 end_q16{
            position_q16.x + velocity_q16.x,
            position_q16.y + velocity_q16.y,
            position_q16.z + velocity_q16.z,
        };

        ProjectileTrace trace;
        trace.start = position_q16;
        trace.end = end_q16;
        trace.owner = r.owner;
        trace.radius_q16 = ammo != nullptr ? ammo->bullet_radius_fp16 : 0;
        trace.ammo_flags = ammo_flags;
        // Only decoded remote presentation rounds may trace the client-state
        // proxy projections (person + dynamic). A default Authoritative round
        // in a non-authority MP world is consequence-gated above, but it must
        // not silently become a proxy/presentation round merely because this
        // process lacks authority.
        trace.include_wire_proxies =
            r.consequence_mode == RoundConsequenceMode::VisualOnly;
        trace.shooter_wire_handle = r.shooter_handle;
        trace.shooter_carrier_wire_handle = r.shooter_carrier_handle;
        const ProjectileHit collision = queries->trace_projectile(world, trace);
        if (!collision.hit()) {
            r.pos = vec_from_fixed(end_q16);
            if ((ammo_flags & kAmmoFlagNoGravity) == 0) velocity_q16.z -= kProjectileGravityQ16;
            if (ammo != nullptr)
                apply_aerodynamic_drag(velocity_q16, *ammo, end_q16.z, world.env.water_z);
            r.vel = vec_from_fixed(velocity_q16);
            if (submerged_stall) {
                if (r.trail_slot >= 0) trails.request_kill(r.trail_slot);
                r.active = false;
                --active_count;
            }
            continue;
        }

        const bool entity_hit = collision.hit_class == ProjectileHitClass::StaticEntity ||
                                collision.hit_class == ProjectileHitClass::DynamicEntity ||
                                collision.hit_class == ProjectileHitClass::Person;
        Entity *impact_target =
            entity_hit ? world.registry.get(collision.geometry_entity) : nullptr;

        FixedVec3 impact_q16 = collision.position_q16;
        if (collision.hit_class == ProjectileHitClass::Person &&
            collision.bone_index >= 0) {
            // ray[29] remains the collision/arbitration distance. Authored
            // person presentation backs the effect point another 0x800 Q16
            // along the flight direction; the torso fallback does not.
            const int32_t magnitude = fixed_magnitude(velocity_q16);
            if (magnitude > 0) {
                impact_q16.x -= static_cast<int32_t>(
                    (static_cast<int64_t>(velocity_q16.x) * 0x800) / magnitude);
                impact_q16.y -= static_cast<int32_t>(
                    (static_cast<int64_t>(velocity_q16.y) * 0x800) / magnitude);
                impact_q16.z -= static_cast<int32_t>(
                    (static_cast<int64_t>(velocity_q16.z) * 0x800) / magnitude);
            }
        }
        const Vec3 impact_position = vec_from_fixed(impact_q16);

        // Pre-arm entity impacts substitute the authored dud and apply no damage.
        // Retail resolves AmmoDef+241, copies the projectile's first 692 bytes, and
        // continues with that child.  LiveRound has no separately addressable pool-3
        // entity, so the same lifecycle is projected as an in-slot logical child:
        // owner/kinematics/elapsed age survive, the dud definition supplies the new
        // lifetime, and the trail slot at retail +0x2B4 (exactly byte 692) does not.
        const bool not_armed = entity_hit && ammo != nullptr &&
                               r.age_ticks < ammo->arm_age_ticks;
        int32_t impact_ammo_index = r.ammo_index;
        LiveRound dud_replacement;
        bool has_dud_replacement = false;
        if (not_armed && !ammo->notarmmed_ammo.empty()) {
            const int32_t dud = world.ammo.index_of(ammo->notarmmed_ammo.c_str());
            const AmmoTableEntry *dud_ammo = world.ammo.by_index(dud);
            if (dud_ammo != nullptr) {
                impact_ammo_index = dud;
                dud_replacement = r;
                dud_replacement.ammo_index = dud;
                dud_replacement.pos = impact_position;
                dud_replacement.max_age_ticks =
                    ((dud_ammo->flags & kAmmoFlagNoAge) != 0)
                        ? INT32_MAX
                        : dud_ammo->max_age_ticks;
                dud_replacement.trail_slot = -1;
                has_dud_replacement = true;
            }
        }

        // Damage can route exactly one carrier hop while impact presentation stays
        // on the geometry actually struck.
        EntityHandle damage_entity = collision.geometry_entity;
        Entity *target = impact_target;
        if (target != nullptr && target->item_type != 1 &&
            (target->item_attrib & kItemAttribEweap) != 0) {
            Entity *parent = world.registry.get(target->ground_target);
            if (parent != nullptr && parent->item_type == 1) {
                target = parent;
                damage_entity = parent->handle;
            }
        }

        // Entity Health/healthMax (+286 and its template mirror) and ItemDef armor
        // (+0x190/+0x192) are signed WORDs in retail. Entity intentionally exposes
        // int32_t carriers to the rest of OpenNova, so enforce the storage width at
        // this consequence boundary before any signed comparisons are made.
        if (authoritative && target != nullptr) {
            target->health = retail_signed_i16(target->health);
            target->health_max = retail_signed_i16(target->health_max);
            target->armor_impact = retail_signed_i16(target->armor_impact);
            // Runtime armor_kz is the compatibility alias for canonical
            // ItemDef +0x192 blast armor.
            target->armor_kz = retail_signed_i16(target->armor_kz);
        }

        const bool person_collision =
            collision.hit_class == ProjectileHitClass::Person;
        const bool organic_fallback =
            person_collision && collision.bone_index < 0;
        const int16_t primary_section = person_collision
            ? static_cast<int16_t>(organic_fallback ? 1 : collision.bone_index)
            : static_cast<int16_t>(-1);
        const int16_t secondary_section = person_collision
            ? static_cast<int16_t>(organic_fallback
                  ? 1
                  : (collision.hit_zone >= 0 ? collision.hit_zone
                                             : collision.bone_index))
            : static_cast<int16_t>(-1);

        // Projectile_ProcessDamageOnTarget returns before damage calculation when
        // target->ItemDef is null.  Geometry still consumed the round above, so
        // the physical impact remains observable even though no hit is recorded.
        if (authoritative && target != nullptr && target->has_item_def &&
            !not_armed && ammo != nullptr) {
            const Entity *shooter = world.registry.get(r.owner);
            int32_t damage = calc_impact_damage(velocity_q16, *ammo, collision.hit_zone,
                                                collision.bone_index, *target, shooter,
                                                r.ammo_index, world);
            if ((target->engine_flags & kEntityFlagIndestructible) != 0 ||
                target->armor_impact == -1 ||
                ammo->penetration_impact < target->armor_impact ||
                target->damage_state != 0)
                damage = 0;
            damage = apply_vehicle_occupant_scale(world, *target, damage);
            if (damage > target->health) damage = target->health;
            if ((target->item_attrib & kItemAttribNoDie) != 0 &&
                damage >= target->health)
                damage = target->health - 1;
            if (damage != 0) {
                if (shooter != nullptr) {
                    auto &rel = world.relations;
                    const int sg = shooter->group_id, ss = shooter->net_id;
                    const int vg = target->group_id, vs = target->net_id;
                    rel.set_group_group(TriggerRelations::kShot, sg, vg);
                    rel.set_single_group(TriggerRelations::kShot, ss, vg);
                    rel.set_group_single(TriggerRelations::kShot, sg, vs);
                    rel.set_single_single(TriggerRelations::kShot, ss, vs);
                }

                // The original writes the subtraction back through a signed 16-bit
                // entity+286 field. Preserve its modulo-2^16 wrap explicitly.
                target->health = retail_signed_i16(
                    static_cast<int64_t>(target->health) - static_cast<int64_t>(damage));
                hits.push_back(RoundHit{damage_entity, r.owner, damage,
                                        primary_section, secondary_section});
                const bool damage_target_is_person =
                    target->item_type == 3 ||
                    (target->item_type == 0 &&
                     target->kind == EntityKind::Organic);
                if (!damage_target_is_person) {
                    target->last_attacker = r.owner;
                    // deathCallback(entity, 1, 0): nonlethal item damage is
                    // observable, while lethal damage enters the husk chain.
                    destruction_notify_item_damage(world, *target, 1);
                }
                if (target->health <= 0) {
                    const int32_t heading_bam =
                        bam_heading_from_mission_yaw_deg(target->yaw);
                    const int quadrant =
                        death_quadrant_from_round(heading_bam, r.vel.x, r.vel.y);
                    if (damage_target_is_person) {
                        const int32_t death_section =
                            primary_section >= 0 ? primary_section : 1;
                        target->death_anim_state =
                            compute_death_anim_state(death_section, quadrant,
                                                     death_cause::kBullet);
                    }
                    world.relations.group(target->group_id).alert =
                        TriggerRelations::kAlertRed;

                    RoundDeath d;
                    d.victim = damage_entity;
                    d.killer = r.owner;
                    d.victim_handle = damage_entity.packed;
                    d.killer_handle = r.shooter_handle;
                    d.adm_index = r.adm_index;
                    deaths.push_back(d);
                }
            }
        }

        RoundImpact imp;
        imp.position = impact_position;
        imp.direction = flight_direction(r.vel);
        imp.ammo_index = impact_ammo_index;
        if (collision.hit_class == ProjectileHitClass::Terrain) {
            imp.effect_tag = 5; // no-charmap retail default: dirt
        } else if (collision.hit_class == ProjectileHitClass::Water) {
            imp.effect_tag = 11;
        } else if (person_collision) {
            // A decoded remote-player proxy intentionally has no registry
            // target, but it is still the retail person collision class.
            imp.effect_tag = 2;
        } else if (impact_target != nullptr &&
                   impact_target->kind == EntityKind::Building &&
                   collision.surface_type == 1) {
            imp.effect_tag = 23; // building material 1 uses the flesh bank
        } else if (collision.surface_type >= 0 &&
                   collision.surface_type + 4 < kImpactEffectTagCount) {
            imp.effect_tag = collision.surface_type + 4;
        } else {
            imp.effect_tag = 4; // generic object without material data
        }
        imp.tick = world.logic_tick;
        imp.source_order = next_impact_order++;
        if (impacts.size() < kMaxPendingImpacts) impacts.push_back(imp);

        // Every explosive round stop queues its authored kill zone. The queue
        // drains after the round simulation, preserving direct-hit-before-AoE
        // consequence ordering.
        if (authoritative && ammo != nullptr)
            detonate_round(world, r, impact_position, *ammo);

        RoundDebugEvent event;
        event.tick = world.logic_tick;
        if (person_collision) {
            event.kind = RoundDebugEvent::kOrganic;
            event.material = 19;
            event.section = primary_section;
            event.secondary_section = secondary_section;
            event.organic_fallback = organic_fallback;
        } else if (entity_hit) {
            event.kind = collision.face_index >= 0
                ? RoundDebugEvent::kItemFace
                : RoundDebugEvent::kItemSphere;
            if (collision.surface_type >= 0 && collision.surface_type <= 255)
                event.material = static_cast<uint8_t>(collision.surface_type);
            event.section = static_cast<int16_t>(collision.section_index);
            event.face = collision.face_index;
        } else {
            // The retained debug schema predates water as a separate kind; both
            // terrain and water are environmental stops, distinguished by tag.
            event.kind = RoundDebugEvent::kTerrain;
        }
        event.effect_tag = imp.effect_tag;
        if (collision.geometry_entity.valid())
            event.entity = collision.geometry_entity.packed;
        event.shooter = r.owner.packed;
        event.ammo_index = impact_ammo_index;
        event.husk = impact_target != nullptr &&
                     (impact_target->engine_flags & kEntityFlagHusk) != 0;
        event.t = static_cast<float>(collision.t_q16) / 65536.0f;
        event.p0 = r.pos;
        event.p1 = vec_from_fixed(end_q16);
        event.hit = impact_position;
        push_round_debug(*this, event);

        if (r.trail_slot >= 0) {
            trails.append(r.trail_slot, r.pos);
            trails.request_kill(r.trail_slot);
        }
        if (has_dud_replacement) {
            r = dud_replacement;
        } else {
            r.active = false;
            --active_count;
        }
    }

    trails.tick();
}

} // namespace opennova::world
