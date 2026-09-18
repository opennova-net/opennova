// Round flight and presentation, with authoritative consequences carried explicitly
// per round. See round_sim.h and docs/net/novaworld-net-re.md §5.60.
#include <runtime/world/round_sim.h>
#include <base/io/tick_rate.h>

#include <runtime/world/fire_sound.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <base/io/bam.h>

#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/infantry.h>
#include <runtime/world/collision_force.h>
#include <runtime/world/player_view.h>
#include <runtime/world/round_move_effect.h>
#include <runtime/world/throwables.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

constexpr double kPi = io::kPi;
// BAM32 -> radians (full turn = 2^32) [orig: engine-wide BAM convention, angle.h].
constexpr double kRadPerBam = io::kRadiansPerBam;
constexpr int32_t kProjectileGravityQ16 = 167;
constexpr int32_t kDragTableSize = 1220;
// The glass-section break sound: row 29 of the 84-row {char name[32]; int *handle}
// trigger-set resolver table, whose slot is the global the break helper plays
// [orig: table row @0x82F9A4 -> dword_24E0920, read @0x439d1c; DialogSystem_Init
// @ 0x527687 resolves every row at load].
constexpr const char *kGlassSmashSound = "GLASS_SMASH";

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

// The hit-time body roll: EVERY projectile hit on a torso-stack bone (bone < 5)
// of a not-yet-dead body tips it ~8 deg toward the shot — front quadrant
// positive, rear negative — lethal or not, whatever the damage number, on both
// peers (the class callback runs after the authority-gated subtraction with
// damage 0 on a joiner). The store is bodyRoll (entity+0x94), our
// AiEntity::roll — the field the slope pass then chases toward the ground
// slope (infantry_slope_pass conform leg) and the torso roll chases in turn
// (fp_roll = torso_roll + lean/4), so a living body visibly flinches and
// settles back while a corpse tips onto the terrain.
// [orig: Entity_HandleDamageTrigger gate @0x40755e; +0x05B05B00 @0x407564;
//  -0x05B05B00 @0x407575; Entity_HandleDamageAndTriggerZones gate @0x4078c6;
//  @0x4078cc / @0x407918]
void apply_hit_body_roll(AiEntity *body, int32_t bone, int quadrant) {
    if (body == nullptr || bone >= 5) return;
    if (quadrant == 0) {
        body->roll = 0x05B05B00;
    } else if (quadrant == 2) {
        body->roll = -0x05B05B00;
    }
}

Entity make_dismemberment_piece_seed(const Entity &victim, uint32_t cut_mask,
                                     int32_t death_anim_state) {
    // The original clones by memcpy of the first 0x2B4 entity bytes plus the
    // 0xAC AI block (person -> pool 0), which INHERITS NetId/Ssn/Team and even
    // the anim-slot pointer (the halves share one skeletal evaluation), while
    // the tail past +0x2B4 keeps recycled-slot data. It then severs exactly:
    // sectionMask (complement swap @0x407691), Health = 0 (@0x407697), the
    // burn-emitter link +0x1CC (@0x40769e), shadowSlot0 +0x1B6 (@0x4076a4),
    // and DcbId +0x7C (@0x4076ab). [orig: Entity_CloneFromTemplateByType
    // @0x4398a0 person leg @0x4398c1-0x439924; the sever set above]
    // Our registry/AI split cannot memcpy an entity row, so this seed keeps
    // the authored/model traits and clears every runtime relationship a copied
    // struct would otherwise alias (weapon slots, mounts, emplacement poses) —
    // a superset of the original's sever list, same observable corpse.
    // Deliberate divergence (D-AI-9): net_id and the names are CLEARED, where
    // the original's memcpy duplicates the victim's — our forward-scanning
    // slot allocator could put the clone BELOW the victim, and the ascending
    // first-match find_by_net_id/find_by_name would then misroute scripted
    // kill/target refs to the clone. The 0x0C/0x18 wire records carry 0/""
    // for the clone instead of the duplicate.
    Entity piece = victim;
    piece.net_id = 0;
    piece.bms_id = 0;
    piece.handle = EntityHandle{};
    piece.registry_spawn_id = 0;
    piece.owner_connection_id = 0;
    piece.minimap_net_id = 0;
    piece.player_class = 0;
    piece.display_name.clear();
    piece.name.clear();
    piece.group_id = 0;
    piece.waypoint_id = 0;
    piece.wp_number = 0;
    piece.ai_state = 0;
    piece.ai_target = -1;
    piece.ai_target_refcount = 0;
    piece.ammo_damage_class.clear();

    piece.health = 0;
    piece.alive = false;
    piece.damage_state = -1;
    piece.death_anim_state = death_anim_state;
    piece.corpse_timer = 0;
    piece.flags = 0;
    piece.engine_flags = 0;
    piece.net_move_input = 0;
    piece.net_stance_bits = 0;
    piece.net_analog_x = 0;
    piece.net_analog_y = 0;
    piece.net_analog_z = 0;
	piece.analog_throttle = 0;
	piece.equipped_adm_index = kAdmSlotNone;
	piece.pre_use_gun_equipped_adm_index = kAdmSlotNone;
    piece.use_gun_slot_swapped = false;
    piece.hidden = false;
    piece.held = false;
    piece.disabled = false;
    piece.last_attacker = EntityHandle{};
    piece.death_blast_center = Vec3{};
    piece.death_tick = 0;
    // Equivalent to the original's ~newBoneBits, since ~(~old & cut) =
    // old | ~cut. [orig: clone sectionMask store @0x407691]
    piece.section_mask = victim.section_mask | ~cut_mask;
    piece.dismemberment_piece = true;
    piece.spawned_piece_mask = 0;
    piece.death_motion = DeathMotionMode::None;
    piece.spawn_origin = kSpawnOriginNone;

    piece.seats.clear();
    piece.armory_points.clear();
    piece.primary_weapon.clear();
    piece.emplacement_parent = EntityHandle{};
    piece.emplacement_parent_spawn_id = 0;
    piece.emplacement_local = Vec3{};
    piece.emplacement_yaw_offset = 0;
    piece.emplacement_bone = 0;
    piece.emplacement_kind = 0;
    piece.emplacement_slot = 0;
    piece.emplacement_attachment_flags = 0;
    piece.emplacement_angle_count = 0;
    piece.emplacement_down_limit_bam = 0;
    piece.emplacement_up_limit_bam = 0;
    piece.emplacement_right_limit_bam = 0;
    piece.emplacement_left_limit_bam = 0;
    piece.emplacement_pose_metadata_resolved = false;
    piece.primary_weapon_slot = WeaponSlotState{};
    piece.primary_weapon_slot_adm = kAdmSlotNone;
    piece.primary_weapon_owner = EntityHandle{};
    piece.primary_occupant = EntityHandle{};
    piece.mount_target = EntityHandle{};
    piece.mount_target_net_id = 0;
    piece.mount_target_bms_id = 0;
    piece.mount_target_spawn_origin = 0;
    piece.mount_seat = -1;
    piece.mount_type = SeatType::None;
    piece.mounted = false;
    piece.mounted_config_valid = false;
    piece.mounted_config = 0;
    piece.mount_bone = 0;
    piece.ground_target = EntityHandle{};
    piece.mounted_child = EntityHandle{};
    piece.last_fire_target = EntityHandle{};
    piece.spawn_position = piece.position;
    return piece;
}

void try_spawn_dismemberment_piece(World &world, Entity &victim,
                                   const FixedVec3 &round_velocity_q16,
                                   int32_t bone, int32_t death_anim_state,
                                   bool was_alive) {
    const uint32_t cut_mask = dismemberment_mask_for_bone(bone);
    if (!was_alive || cut_mask == 0 || victim.handle.pool() != 0 || victim.dismemberment_piece ||
        (victim.flags & kEntityFlagPlayer) != 0 ||
        (victim.engine_flags & kEntityFlagPlayer) != 0 ||
        victim.player_class != 0 ||
        (victim.item_attrib & kItemAttribNoDismember) != 0 ||
        victim.health > 0 || victim.health > (victim.health_max >> 1))
        return;

    const AiEntity *source = world.ai.for_handle(victim.handle);
    if (source == nullptr) return;

    Entity piece = make_dismemberment_piece_seed(
        victim, cut_mask, death_anim_state);
    const EntityHandle piece_handle = world.registry.spawn(0, piece);
    if (!piece_handle.valid()) return;

    // The horizontal kick only: the original adds roundVel X/Y >> 8 to the
    // clone's velocity pair and leaves the vertical component alone.
    // [orig: +152 add @0x4076b7; +156 add @0x4076c9]
    const int32_t impulse_q16[3] = {
        round_velocity_q16.x >> 8,
        round_velocity_q16.y >> 8,
        0,
    };
    world.ai.attach_dismemberment_piece(piece_handle, *source, impulse_q16);

    // Deliberate divergence (docs/divergence-ledger.md D-AI-9): the original
    // commits the victim mask BEFORE the clone call (sectionMask |= newBoneBits
    // @0x407684) and never null-checks the allocation (@0x407691 writes through
    // the Entity_CloneFromTemplateByType return, so actor-pool exhaustion would
    // fault). We commit after a successful allocation + AI attachment instead,
    // so a full pool leaves the original body intact. The |= is equivalent to
    // the original's ~old & boneMask two-step. [orig: @0x407675-0x407684]
    victim.section_mask |= cut_mask;
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
                values[static_cast<size_t>(destination)] = static_cast<int32_t>(raw / io::kTicksPerSecondInt);
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
    const int64_t scaled = static_cast<int64_t>(magnitude_q16) * io::kTicksPerSecondInt;
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

// `surface_normal` is Entity_ApplyDragAndBounceForce's third argument: 0 for
// the flight call, a positive multiplier for the person-hit call.
void apply_aerodynamic_drag(FixedVec3 &velocity, const AmmoTableEntry &ammo,
                            int32_t position_z_q16, int32_t water_z_q16,
                            int32_t surface_normal) {
    if (ammo.drag_fp16 == 0) return;

    const int32_t old_magnitude = fixed_magnitude(velocity);
    if (old_magnitude == 0) return;
    const int32_t old_index = drag_speed_index(old_magnitude);
    const int32_t raw = projectile_drag_table()[static_cast<size_t>(old_index)];
    const int64_t first_division =
        (static_cast<int64_t>(raw) << 16) / ammo.drag_fp16;
    const int64_t scaled_drag = first_division / io::kTicksPerSecondInt;
    // A positive surface multiplier scales the step directly and skips the
    // water/air legs [orig: the surfaceNormal > 0 arm @0x4e604e..0x4e6097
    // ahead of the water-height test @0x4e60a9].
    const bool underwater = water_z_q16 != 0 && position_z_q16 <= water_z_q16;
    const int64_t drag_step = surface_normal > 0 ? scaled_drag * surface_normal
                              : underwater      ? scaled_drag * 25
                                                : scaled_drag;

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

bool impact_is_critical(const Entity &target, int32_t hit_zone,
                        int32_t hit_bone) {
    if (target.item_type != 3) return false;
    return (target.item_attrib & kItemAttribLandable) != 0
        ? seat_hit_bone_is_critical(hit_bone)
        : hit_zone_is_critical(hit_zone);
}

// Armor removes kinetic energy, independently of atmospheric drag.
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

// The surface multiplier every type-3 (person) hit hands the drag/bounce
// force after the armor arms [orig: Weapon_CalcImpactDamage `push 23h`
// @0x4ecc38].
constexpr int32_t kPersonHitDragSurface = 35;

// The kinetic damage number [orig: Weapon_CalcImpactDamage @ 0x4EC920]. `vel` is
// units/tick; the original wraps 62 * |vel|_16.16 as signed 32-bit, shifts it by 16,
// applies only an upper clamp of 1219 (@0x4ecad6), then wraps the signed speed*weight
// product before /875 (@0x4ecb1a). It next applies the hit-zone
// multiplier (now fed by posed COBJ hit zones) and the shooter-class byte (0.9 / 1.1 —
// replicated from the loadout's per-ammo class table), floors at min_damage
// (@0x4ecb3a), and caps at max_damage when > 0 (@0x4ecb42). Multiplayer authority and
// OneShotKill are explicit inputs, including the non-authority zero return @0x4ec933.
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
    if (target.item_type == 3) {
        // The zone/bone -> multiplier tables live in round_sim.h (the one home
        // the debug views read through the binding too).
        double zone_scale = 1.0;
        if ((target.item_attrib & kItemAttribLandable) != 0) {
            // The attrib-0x200 seat branch reads the primary/reaction section
            // ray[31] (hitZoneData+124 @0x4ec977), NOT the damage zone ray[32]
            // the normal-infantry table below reads (@0x4ec9bf). The two differ
            // whenever the bone walk crosses more than one sphere.
            zone_scale = seat_hit_bone_damage_multiplier(hit_bone);
        } else {
            zone_scale = hit_zone_damage_multiplier(hit_zone);
        }
        damage = static_cast<int32_t>(static_cast<double>(damage) * zone_scale);

        if (damage_class == 1)
            damage = static_cast<int32_t>(static_cast<double>(damage) *
                                          static_cast<double>(0.9f));
        else if (damage_class == 2)
            damage = static_cast<int32_t>(static_cast<double>(damage) *
                                          static_cast<double>(1.1f));
    }
    if (damage <= ammo.min_damage) damage = ammo.min_damage;
    if (ammo.max_damage > 0 && damage >= ammo.max_damage) damage = ammo.max_damage;
    if (target.item_type != 3) return damage;
    if (body_armor)
        apply_armor_deceleration(velocity_q16, ammo, ammo.armor_density[armor_class]);
    // Every person hit then bleeds the round through the drag/bounce force
    // with the fixed surface multiplier 35, armored or not: the three armor
    // arms and the no-armor path all fall into the same call.
    // [orig: Weapon_CalcImpactDamage — the type-3 gate @0x4ecb96, the
    //  `push 23h; push 1; push esi; call Entity_ApplyDragAndBounceForce`
    //  leg @0x4ecc38..0x4ecc42]
    apply_aerodynamic_drag(velocity_q16, ammo, 0, 0, kPersonHitDragSurface);
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

// The fire descriptor's engine-frame BAM pair projected into mission axes.
// Knife consumes this before the ballistic spread/recoil branch, while an
// ordinary round uses the same mapping after its final angle is selected.
// [orig: RoundData_SpawnRound @0x4ec5e9;
// Weapon_RaycastAndSpawnImpact @0x4e8460]
Vec3 fire_direction(int32_t yaw_bam, int32_t pitch_bam) {
    const double bearing = double(yaw_bam) * kRadPerBam;
    const double pitch = double(pitch_bam) * kRadPerBam;
    const double cp = std::cos(pitch);
    return Vec3{
        static_cast<float>(std::cos(bearing) * cp),
        static_cast<float>(std::sin(bearing) * cp),
        static_cast<float>(std::sin(pitch)),
    };
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

    AiEntity *body = world.ai.for_handle(params.owner);
    const int32_t source_z =
            body != nullptr ? body->pos[2] : to_fixed(entity->position.z);
    // The below-water classifier projects the per-tick EYE height (the shared
    // entity_eye_below_water witness) wherever the drowning bit is clear;
    // retail's stance leg folds the ladder/parachute bits with it (Flags &
    // 0x108020 [orig: @0x4ec2d5]). eye_offset_z is the ported +0x74 channel
    // (0 when never stamped, the retail spawn value).
    source.underwater = source.underwater ||
                        entity_eye_below_water(world, source_z,
                                               entity->eye_offset_z);
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

void record_round_fire(World &world, RoundSim &sim,
                       const RoundSpawnParams &params) {
    if (params.launch_presented) return;
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
	// Received ADM fire executes the action row on every peer, including its
	// tank recoil. The local FSM owns its own action execution. [orig: @0x4020A0]
	if (params.owner != world.cached.local_player &&
			(params.wire_round_flags &
					(round_event_flag::kAltFire | round_event_flag::kAdmIndexed)) ==
					round_event_flag::kAdmIndexed) {
		const auto *weapon = world.tables.weapons.by_index(params.adm_index);
		if (Entity *shooter = world.registry.get(params.owner);
				shooter != nullptr && weapon != nullptr)
			world.vehicles.weapon_recoil(*shooter,
					weapon->action_fsm.actions[weapon_action::kFire].action_value,
					params.dir_yaw_bam, params.dir_pitch_bam);
	}
	// The sound legs run on the same logic-tick moment (world/fire_sound.h).
	fire_sound_on_spawn(world, params);
}

} // namespace

bool entity_eye_below_water(const World &world, int32_t body_z_q16,
                            int32_t eye_offset_z) {
    return world.env.water_z != 0 &&
           body_z_q16 + eye_offset_z < world.env.water_z;
}

void projectile_apply_drag(FixedVec3 &velocity, const AmmoTableEntry &ammo,
    int32_t position_z_q16, int32_t water_z_q16) {
    apply_aerodynamic_drag(velocity, ammo, position_z_q16, water_z_q16, /*surface_normal=*/0);
}

void projectile_apply_person_hit_drag(FixedVec3 &velocity, const AmmoTableEntry &ammo) {
    apply_aerodynamic_drag(velocity, ammo, 0, 0, kPersonHitDragSurface);
}

void RoundSim::reset() noexcept {
	rounds.assign(kCapacity, LiveRound{});
	active_count = 0;
	deaths.clear();
	impacts.clear();
	hits.clear();
	fired.clear();
	guided_updates.clear();
	next_impact_order = 1;
	next_presentation_generation_ = 1;
	debug_trail = {};
	debug_trail_next = 0;
	debug_trail_count = 0;
	trails.reset(); // [orig: the pool memset in CEffectEmitterPool_ResetAndBuildStyles
	                //  @ 0x5db3b0, run from Game_StartMission]
	remote_visual_tracer_counters_.clear();
}

void RoundSim::present_fire(World &world, const RoundSpawnParams &params) {
    record_round_fire(world, *this, params);
}

int RoundSim::spawn(World &world, const RoundSpawnParams &params,
                    RoundConsequenceMode mode) {
    const AmmoTableEntry *ammo = world.tables.ammo.by_index(params.ammo_index);
    if (ammo == nullptr) return -1;
    const bool authoritative =
        mode == RoundConsequenceMode::Authoritative &&
        (!world.rules.mp_session || world.rules.projectile_authority);
    // The retail spawn dispatch order [orig: RoundData_SpawnRound @ 0x4ec1f3..
    // 0x4ec2a5]: instantkillzone -> Detonatesatchels -> designator -> claymore
    // fan -> shotgun -> the ballistic default.
    if ((ammo->flags & kAmmoFlagInstantKillZone) != 0) {
        // instantkillzone: the kill zone queues at the spawn point and no round
        // flies [orig: @0x4ec1f3 -> WeaponEffect_PushExplosionQueueEntry].
        ExplosionEntry explosion;
        explosion.pos = params.origin;
        explosion.dir_bam = params.dir_yaw_bam;
        explosion.type = ammo->kztype;
        explosion.ammo_index = params.ammo_index;
        explosion.owner = params.owner;
        explosion.hit_word = params.shot_seq;
        if (authoritative) world.explosions.queue_explosion(world, explosion);

        if (ammo->kztype == ammo_kz::kKnife) {
            // Knife alone adds the bounded, effects-only ray. It is not a
            // damage path: authority remains in the queued kill zone above.
            // Every entity table uses CFAC, including persons; ordinary bullet
            // bone spheres are intentionally excluded.
            // [orig: Weapon_RaycastAndSpawnImpact @0x4e8460]
            if (world.collision != nullptr && ammo->kz_maxradius > 0.0f &&
                impacts.size() < kMaxPendingImpacts) {
                const Vec3 direction = fire_direction(
                    params.dir_yaw_bam, params.dir_pitch_bam);
                ProjectileTrace trace;
                trace.start = FixedVec3{to_fixed(params.origin.x),
                                        to_fixed(params.origin.y),
                                        to_fixed(params.origin.z)};
                trace.end = FixedVec3{
                    trace.start.x + to_fixed(direction.x * ammo->kz_maxradius),
                    trace.start.y + to_fixed(direction.y * ammo->kz_maxradius),
                    trace.start.z + to_fixed(direction.z * ammo->kz_maxradius),
                };
                trace.owner = params.owner;
                trace.ammo_flags = ammo->flags;
                trace.include_wire_proxies =
                    mode == RoundConsequenceMode::VisualOnly;
                trace.shooter_wire_handle = params.shooter_handle;
                trace.shooter_carrier_wire_handle =
                    params.shooter_carrier_handle;
                const ProjectileHit hit =
                    world.collision->trace_knife_impact(world, trace);
                if (hit.hit()) {
                    RoundImpact imp;
                    imp.position = vec_from_fixed(hit.position_q16);
                    imp.direction = direction;
                    imp.ammo_index = params.ammo_index;
                    if (hit.hit_class == ProjectileHitClass::Terrain) {
                        const int32_t surface = terrain::surface_type_at_fixed(
                            world.tables.surface_map, hit.position_q16.x,
                            hit.position_q16.y);
                        imp.effect_tag =
                            (surface >= 0 && surface + 4 < kImpactEffectTagCount)
                                ? surface + 4
                                : 5;
                    } else if (hit.hit_class == ProjectileHitClass::Water) {
                        imp.effect_tag = 11;
                    } else if (hit.hit_class == ProjectileHitClass::Person &&
                               hit.surface_type == 1) {
                        // The PERSON leg (hit type 3 = the default slot-type
                        // walk of Projectile_RaycastProximitySlots) remaps CFAC
                        // material 1 to the flesh row; buildings (hit type 1)
                        // and items (hit type 2) are plain material + 4.
                        // [orig: Weapon_RaycastAndSpawnImpact @0x4e8880..0x4e8888
                        //  vs @0x4e8867]
                        imp.effect_tag = 23;
                    } else if (hit.surface_type >= 0 &&
                               hit.surface_type + 4 < kImpactEffectTagCount) {
                        imp.effect_tag = hit.surface_type + 4;
                    } else {
                        imp.effect_tag = 4;
                    }
                    imp.tick = world.logic_tick;
                    imp.source_order = next_impact_order++;
                    impacts.push_back(imp);
                    // The knife leaf feeds the same impact-effect processor,
                    // so a stab into an entity leaves the same scar by the
                    // ammo's `scar_type` [orig: Weapon_RaycastAndSpawnImpact
                    // @0x4e8460 -> AmmoDef_ProcessImpactEffect @0x40a170 ->
                    // Impact_SpawnGlassEffectsOrScar @0x5cf1b0].
                    if (const Entity *struck = world.registry.get(hit.geometry_entity))
                        scar_add_entry(world, hit, *struck, ammo->scar_type);
                }
            }
        } else if (impacts.size() < kMaxPendingImpacts) {
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
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(params.adm_index);
    if ((ammo->flags & kAmmoFlagShotgun) != 0) {
        // Shotgun is a separate pellet-fan leaf: it never reads weapon ERROR or
        // the rules gate, but the ordinary ammo recoil is applied after the fan.
        // [orig: RoundData_SpawnRound @0x4EC378]
        const int first = spawn_burst(
                world, params, *ammo, mode, /*shotgun_spread=*/true);
        record_round_fire(world, *this, params);
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
    double speed_per_tick = double(ammo->velocity) / io::kTicksPerSecondInt; // [orig: speed/62 @0x4ec508]
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
    // tracer_rate-th round FROM THE FIRING WEAPON SLOT is a tracer — the counter is
    // the slot's +0x80 byte, so each weapon keeps its own phase across switches.
    // Producers with per-slot state pass that byte (params.tracer_counter); the
    // owner-entity byte stands in for producers without one (slot-less organic
    // fire, weaponSlot 0 at WeaponSlot_FireAndSpawnEffects @0x53F477 — the
    // D-AI-8 (a) residual). Rate 0 = never; no shooter = every round; the
    // FORCETRACER ammo flag (0x8000) rides every round. Team = the shooter team
    // byte [orig: round+0x162 copy @0x4ec705; the slot+4 & 0x200 0xFF override is
    // unmodeled].
    bool tracer = true;                    // [orig: var init @0x4ec15a]
    Entity *owner_ent = world.registry.get(params.owner);
    if (ammo->tracer_rate == 0) {
        tracer = false;                    // [orig: @0x4ec18a]
    } else if (params.tracer_counter != nullptr) {
        // [orig: @0x4ec199-0x4ec1bb: ++slot->0x80, wrap to 0 at >= rate, tracer on
        //  wrap]
        if (++(*params.tracer_counter) >= ammo->tracer_rate)
            *params.tracer_counter = 0;
        tracer = (*params.tracer_counter == 0);
    } else if (owner_ent != nullptr) {
        // The slot-less producer stand-in (same recurrence on the entity byte).
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

    init_guided(world, r, *ammo);

    // Record the fire for the host present layer (sound + muzzle effect) — the
    // inline-presentation moment of the original [orig: WeaponSlot_FireAndSpawnEffects
    // @0x53f440 runs its presentation right after Entity_FireWeaponAndSendPacket].
    record_round_fire(world, *this, params);
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
        const double speed = double(ammo.velocity) / io::kTicksPerSecondInt;
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


// Shared direct-hit consequence. Squib rays call the same helper as bullets.
// [orig: Projectile_ProcessDamageOnTarget @0x4E7FB0]
void RoundSim::process_damage_hit(World &world, LiveRound &r,
        const ProjectileHit &collision, FixedVec3 &velocity_q16) {
    const bool authoritative = r.consequence_mode == RoundConsequenceMode::Authoritative &&
            (!world.rules.mp_session || world.rules.projectile_authority);
    const bool not_armed = false;
    const auto *ammo = world.tables.ammo.by_index(r.ammo_index);
    EntityHandle damage_entity = collision.geometry_entity;
    Entity *target = world.registry.get(damage_entity);
    if (target && target->item_type != 1 && (target->item_attrib & kItemAttribEweap)) {
        Entity *parent = world.registry.get(target->ground_target);
        if (parent && parent->item_type == 1) { target = parent; damage_entity = parent->handle; }
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
        const bool peer_item_hit = !world.rules.logic_authority &&
                r.consequence_mode == RoundConsequenceMode::VisualOnly && target &&
                target->kind != EntityKind::Organic && !target->is_ai_capable;
        const bool damage_target_is_person = target != nullptr &&
            (target->item_type == 3 ||
             (target->item_type == 0 && target->kind == EntityKind::Organic));
        // A non-authority in-session peer runs the SAME hit path with
        // Weapon_CalcImpactDamage's forced zero [orig: @0x4ec933..0x4ec93a]:
        // no health, hits, or deaths, but the person class callback still
        // fires (@0x4e820e) and stages the death anim / tips the body on the
        // joiner too. Only a person with a World entity reaches this leg on a
        // joiner (the wire person proxies never resolve to geometry_entity,
        // collision_trace.cpp), i.e. the joiner's own body.
        const bool peer_person_hit = world.rules.mp_session &&
                !world.rules.projectile_authority && damage_target_is_person;
        if ((authoritative || peer_item_hit || peer_person_hit) && target != nullptr &&
            target->has_item_def && !not_armed && ammo != nullptr) {
            const Entity *shooter = world.registry.get(r.owner);
            // Weapon_CalcImpactDamage latches the critical/headshot cause bit on
            // the ENTITY (+44 bit 0x800) for every authoritative hit, lethal or
            // not. OneShotKill returns before the zone branch (no bit even when
            // the ray crossed a critical section), and the in-session
            // non-authority return precedes it too, so a joiner never latches.
            // [orig: Weapon_CalcImpactDamage @0x4ec933 / @0x4ec942; the latches
            //  @0x4ec994 (seat leg) / @0x4ec9c6 (zone table);
            //  GameEvent_PlayerDeath @0x516DD0 reads entity+44 bit 0x800]
            const bool critical_hit =
                !(world.rules.mp_session && world.rules.one_shot_kill) &&
                impact_is_critical(*target, collision.hit_zone,
                                   collision.bone_index);
            int32_t damage = calc_impact_damage(velocity_q16, *ammo, collision.hit_zone,
                                                collision.bone_index, *target, shooter,
                                                r.ammo_index, world);
            if (authoritative && critical_hit) target->cause_flags |= 0x800u;
            r.vel = vec_from_fixed(velocity_q16);
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
            const bool target_was_alive =
                target->health > 0 && target->alive &&
                (target->flags & kEntityFlagDead) == 0 &&
                (target->engine_flags & kEntityFlagDead) == 0;
            // The class callbacks return first on a body already flagged dead;
            // the killing hit itself passes (the edge latches the bit later).
            // [orig: Entity_HandleDamageAndTriggerZones @0x40772f]
            const bool target_not_dead =
                ((target->flags | target->engine_flags) & kEntityFlagDead) == 0;
            // The local player's damage feedback (red vignette + camera shake) arms
            // on every hit that beats the 5-point floor, after the class damage
            // callback and ahead of the kill routing; retail tests the value it
            // just computed, not the applied health delta
            // [orig: Projectile_ProcessDamageOnTarget @0x4e8213..0x4e822b ->
            //  Player_OnDamageReceived @0x4dd880].
            if (target_not_dead && target->handle == world.cached.local_player && damage > 5)
                player_on_damage_received(world);
            if (!authoritative && !peer_person_hit) {
                // The hit callback executes on both peers; only the health
                // subtraction and gameplay kill fan below require authority.
                // [orig: Projectile_ProcessDamageOnTarget @ 0x4E7FB0]
                destruction_notify_item_damage(world, *target, 1,
                        {collision.section_index, damage, r.yaw_bam, r.pitch_bam, r.roll_bam});
            } else {
                if (authoritative && damage != 0) {
                    if (shooter != nullptr) {
                        auto &rel = world.script.relations;
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
                    if (damage_target_is_person) {
                        // [orig: Entity_HandleDamageTrigger @0x4074BA; twin @0x407822]
                        apply_collision_force(world, *target, ammo->secondary_anim, ammo->kz_physics, r.pos, r.owner);
                    }
                    if (!damage_target_is_person) {
                        target->last_attacker = r.owner;
                        // deathCallback(entity, 1, 0): nonlethal item damage is
                        // observable, while lethal damage enters the husk chain.
                        destruction_notify_item_damage(world, *target, 1,
                                {collision.section_index, damage, r.yaw_bam, r.pitch_bam, r.roll_bam});
                    }
                }
                if (damage_target_is_person) {
                    // Every hit stamps lastAttacker (+0x178) / the ammo def on
                    // the victim before the callback, on both peers, unless the
                    // def carries attrib 0x20 [orig: Projectile_ProcessDamageOnTarget
                    // @0x4e81e7..0x4e81f9]. A never-hit body keeps an empty
                    // slot, which the death edge's fallback also restores.
                    if ((target->item_attrib & kItemAttribEweap) == 0)
                        target->last_attacker = r.owner;
                    // The person class callback, event 1 with a projectile:
                    // EVERY hit (lethal or not, damage 0 included) selects the
                    // death anim from the hit bone + attack quadrant into +0x2C0
                    // and tips the body on a torso-stack bone; a later kill
                    // that stamps nothing (script/WAC) plays this hit's clip.
                    // The roll, the mask switch, and the anim selector all
                    // consume the SAME hit-record bone (hitRecord[14]);
                    // death_section is our preserved copy of that record field.
                    // [orig: Entity_HandleDamageTrigger @0x407478 quadrant,
                    //  @0x407483 select, @0x40755e / @0x4075f6 gates;
                    //  Entity_HandleDamageAndTriggerZones @0x4077e0 / @0x4077eb
                    //  / @0x4078c6]
                    const int32_t heading_bam =
                        bam_heading_from_mission_yaw_deg(target->yaw);
                    const int quadrant =
                        death_quadrant_from_round(heading_bam, r.vel.x, r.vel.y);
                    const int32_t death_section =
                        primary_section >= 0 ? primary_section : 1;
                    if (target_not_dead) {
                        target->death_anim_state =
                            compute_death_anim_state(death_section, quadrant,
                                                     death_cause::kBullet);
                        AiEntity *victim_body = world.ai.for_handle(target->handle);
                        apply_hit_body_roll(victim_body, death_section, quadrant);
                        // The plyr callback re-arms the player body's 64-tick
                        // think cadence on every event it handles.
                        // [orig: Entity_HandleDamageAndTriggerZones @0x407b5e / @0x407c71]
                        if (((target->flags | target->engine_flags) & kEntityFlagPlayer) != 0)
                            target->spawn_phase = 64;
                    }
                    if (authoritative && damage != 0 && target->health <= 0) {
                        // A lethal player-flag hit counts on the ROUND; past the
                        // first kill the victim takes the same-projectile cause
                        // bit 0x100 before the death is reported.
                        // [orig: Projectile_ProcessDamageOnTarget @0x4e8112 lethal
                        //  gate, @0x4e8159 Flags&0x100, @0x4e8169..0x4e816b]
                        if (target_was_alive &&
                            ((target->flags | target->engine_flags) & kEntityFlagPlayer) != 0 &&
                            ++r.player_kills > 1)
                            target->cause_flags |= 0x100u;
                        try_spawn_dismemberment_piece(
                            world, *target, velocity_q16,
                            death_section, target->death_anim_state,
                            target_was_alive);
                        world.script.relations.group(target->group_id).alert =
                            TriggerRelations::kAlertRed;

                        RoundDeath d;
                        d.victim = damage_entity;
                        d.killer = r.owner;
                        d.victim_handle = damage_entity.packed;
                        d.killer_handle = r.shooter_handle;
                        d.adm_index = r.adm_index;
                        d.ammo_index = r.ammo_index;
                        // GameEvent_PlayerDeath reads the victim's entity+44
                        // cause bits at the death edge [orig: @0x516f4d /
                        // @0x517188..0x517206]; the sim snapshots them here.
                        d.event_flags = target->cause_flags & 0xF00u;
                        deaths.push_back(d);
                    }
                } else if (authoritative && damage != 0) {
                    if (target->health <= 0) {
                        world.script.relations.group(target->group_id).alert =
                            TriggerRelations::kAlertRed;

                        RoundDeath d;
                        d.victim = damage_entity;
                        d.killer = r.owner;
                        d.victim_handle = damage_entity.packed;
                        d.killer_handle = r.shooter_handle;
                        d.adm_index = r.adm_index;
                        d.ammo_index = r.ammo_index;
                        d.event_flags = target->cause_flags & 0xF00u;
                        deaths.push_back(d);
                    }
                } else if (authoritative && target->kind != EntityKind::Organic &&
                           !target->is_ai_capable) {
                    destruction_notify_item_damage(world, *target, 1,
                            {collision.section_index, 0, r.yaw_bam, r.pitch_bam, r.roll_bam});
                }
            }
        }

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
            (!world.rules.mp_session || world.rules.projectile_authority);

        // The lifetime/armed-fuse head runs before the motor. Advance the
        // stored age before dispatch; the custom motor compensates so its
        // elapsed/remaining values match retail's post-motor decrement
        // [orig: Projectile_UpdatePhysics @ 0x4e9da7..0x4e9f4e].
        if (r.det_at_expiry || (r.guided_family != GuidedFamily::None && (r.guided.flags & 1)) || r.age_ticks >= r.max_age_ticks) {
            // Only rounds the motor armed detonate at this head; an ordinary
            // ballistic lifetime expiry vanishes silently.
            const AmmoTableEntry *fuze_ammo = world.tables.ammo.by_index(r.ammo_index);
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

        const AmmoTableEntry *ammo = world.tables.ammo.by_index(r.ammo_index);
        const uint32_t ammo_flags = ammo != nullptr ? ammo->flags : 0;

        if (ammo && r.guided_family != GuidedFamily::None) tick_guided(world, r, *ammo, authoritative);

        // `ignore` rounds skip the stock sweep after their class motor.
        if ((ammo_flags & kAmmoFlagIgnore) != 0) continue;

        // The `move`-row emitter's lifecycle, tested BEFORE the move — every
        // test reads the pre-move position, since the function's only
        // Position.Z store sits after them [orig: Projectile_UpdatePhysics
        // @0x4E9D70]. The two legs differ: the `useownmove` (flag 0x2000)
        // leg runs the custom motor @0x4e9f06, the aging decrement @0x4e9f41,
        // then the spawn test @0x4e9f58..0x4e9f8e (an authored effect, no
        // live handle, life left, not at/below the water plane under
        // ClipWaterFx) and the handle branch @0x4ea019..0x4ea03e (`z > water`
        // re-poses, else ClipWaterFx releases the emitter, unlatched, so it
        // may respawn once above water). The BALLISTIC leg branches past all
        // of that @0x4e9f0c -> 0x4ea06a: its spawn test @0x4ea8ae..0x4ea8d3
        // is effect && no handle && life != 0 with NO water term — read
        // before that leg's own decrement @0x4eaa7f — and its handle branch
        // @0x4ea963 only re-poses, so a ballistic plume is never released by
        // the plane. The shell spawns/retires the emitter as this flag flips.
        {
            const bool has_move_effect =
                ammo != nullptr && !ammo->impact_effects[1].effect.empty();
            if ((ammo_flags & kAmmoFlagUseOwnMove) != 0) {
                const int32_t life_ticks =
                    r.max_age_ticks == INT32_MAX ? INT32_MAX
                                                 : r.max_age_ticks - r.age_ticks;
                const bool clipped = round_effect_should_release_for_water(
                    ammo_flags, to_fixed(r.pos.z), world.env.water_z);
                if (clipped) {
                    r.move_effect_live = false;
                } else if (round_effect_should_spawn(has_move_effect, r.move_effect_live,
                                                     life_ticks, clipped)) {
                    r.move_effect_live = true;
                }
            } else {
                // The life the ballistic test reads is this tick's
                // pre-decrement value (age was stepped above).
                const int32_t life_before =
                    r.max_age_ticks == INT32_MAX ? INT32_MAX
                                                 : r.max_age_ticks - (r.age_ticks - 1);
                if (round_effect_should_spawn(has_move_effect, r.move_effect_live,
                                              life_before, /*clipped_by_water=*/false)) {
                    r.move_effect_live = true;
                }
            }
        }

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
        // Non-building projectiles refresh their own BB membership; the
        // shooter can be in a different room [orig: Entity_BuildProximityList
        // @0x4B3DC0; Projectile_UpdatePhysics terrain gate @0x4EA2F0].
        const int32_t blink_position[3] = {position_q16.x, position_q16.y, position_q16.z};
        BlinkAccum blink;
        queries->query_blink_boxes_at_point(world, blink_position, blink);
        trace.walk_terrain = (blink.flags & kBlinkIndoorsBit) == 0;
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
            if (r.guided_family == GuidedFamily::None && (ammo_flags & kAmmoFlagNoGravity) == 0) velocity_q16.z -= kProjectileGravityQ16;
            if (ammo != nullptr && r.guided_family == GuidedFamily::None)
                apply_aerodynamic_drag(velocity_q16, *ammo, end_q16.z, world.env.water_z,
                                       /*surface_normal=*/0);
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
            const int32_t dud = world.tables.ammo.index_of(ammo->notarmmed_ammo.c_str());
            const AmmoTableEntry *dud_ammo = world.tables.ammo.by_index(dud);
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

        // The entity-impact handler's two FACE-MATERIAL legs, in retail order:
        // the dead-victim effect suppression, then the material-15 section
        // break. Both read the struck face's CFAC material byte (ray[22],
        // `collision.surface_type`), cached once by retail into the frame slot
        // the auto-namer calls `weaponType` — it is the material, not a weapon
        // type (world-wac-ai-re §15.8 "ray[22] = face MATERIAL byte"; the same
        // slot feeds the material + 4 effect row @0x4e982b).
        // Only hit types 1/2 (pool-2 statics, pool-1 dynamics) reach this
        // handler; the person pass has its own handler, ported below.
        // [orig: Projectile_HandleEntityImpact @ 0x4E9390 — `mov ecx, [edi+58h]`
        //  @0x4e95c7; the sole caller arm @0x4ea73a]
        const bool entity_impact_handler_leg =
            collision.hit_class == ProjectileHitClass::StaticEntity ||
            collision.hit_class == ProjectileHitClass::DynamicEntity;
        // A victim already flagged dead drops the whole impact-effect
        // presentation for two face classes: a person body (itemDef+92 == 3)
        // and the flesh material 19 (19 + 4 = the `flesh` effect row). Either
        // clears retail's `shouldProcessEffect`, which gates BOTH the
        // material + 4 spawn and the slot-2 local-player feedback, so a corpse
        // on the item pass sprays nothing.
        // [orig: the dead test `test byte ptr [esi+24h], 2` @0x4e95c3,
        //  `cmp dword ptr [eax+5Ch], 3` @0x4e95d0 storing 0 @0x4e95d6,
        //  `cmp ecx, 13h` @0x4e95db storing 0 @0x4e95e0; consumed @0x4e9817]
        bool entity_effect_suppressed = false;
        if (entity_impact_handler_leg && target != nullptr &&
            (((target->flags & kEntityFlagDead) != 0) ||
             ((target->engine_flags & kEntityFlagDead) != 0)) &&
            (target->item_type == 3 || collision.surface_type == 19))
            entity_effect_suppressed = true;
        // Face material 15 is GLASS: a round through a live BUILDING's glass
        // breaks that section outright. The section bit is set on the victim
        // unconditionally (even section 0), while the helper that fronts it
        // plays the break sound only for a not-yet-broken NON-ZERO section of
        // an entity that carries an item def. That helper is an auto-namer
        // misnomer: its transformed min/max bone points and the extent product
        // it computes are dead locals, and its one observable effect is the
        // full-volume positional play of the GLASS_SMASH trigger set at the hit
        // point. NOTHING recomputes collision bounds, in retail or here: the bit
        // removes the section from the DRAW (`item_hidden_sections` ->
        // inmatch/present_rows.cpp) and from the person bone-sphere walks
        // (collision_query.cpp `collision_raycast_person_sections`,
        // collision_trace.cpp), while the item CFAC face walk consults only the
        // matrix-disabled bit, so a shot-out pane still stops ordinary rounds.
        // The rocket family is what passes through it, by the report below.
        // [orig: the face walk's only per-section gate is `(boneMatrix+60) & 3`
        //  @0x4e4f12 in Physics_RaycastAgainstBoneCollision @ 0x4E4CB0]
        // [orig: `cmp ecx, 0Fh` @0x4e964f, the husk gate `test byte ptr
        //  [esi+24h], 4` @0x4e9654, `cmp dword ptr [ecx+5Ch], 5` @0x4e965d, the
        //  `or [esi+134h], edx` @0x4e9684; the helper
        //  Entity_PlaySectionBreakSound @ 0x439C00 — gate @0x439c08 /
        //  @0x439c1e / @0x439c2a, play @0x439d25 ->
        //  Entity_PlaySound3D_FullVolume @ 0x528E20 -> Sound_Play3DPositional
        //  @ 0x527CB0 at volume 255; the bank handle dword_24E0920 is row 29
        //  `GLASS_SMASH` of the 36-B {name[32], slot*} resolver table @0x82F9A4
        //  that DialogSystem_Init @ 0x527687 fills]
        if (entity_impact_handler_leg && collision.surface_type == 15 &&
            target != nullptr && (target->engine_flags & kEntityFlagHusk) == 0 &&
            target->item_type == 5 && collision.section_index >= 0) {
            // Material 15 can only arrive from a CFAC face walk, so retail's
            // hit record always carries that face's section here; the bound
            // keeps the shift defined for the face-less sphere stand-in.
            const int32_t section = collision.section_index;
            const uint32_t bit = 1u << (static_cast<uint32_t>(section) & 31u);
            if (section != 0 && (target->section_mask & bit) == 0 &&
                target->has_item_def)
                world.out.fire_sounds.play_immediate(
                    kGlassSmashSound, impact_position, target->bms_id,
                    target->handle.packed);
            target->section_mask |= bit;
            // UNPORTED: the pass-through report. A `lawr|fgrenade` round
            // (ammo flags 0x18000000, the round's +0x114 copy) that breaks a
            // section reports "continue" to the flight loop and keeps flying
            // through the hole. RoundSim has no round-continues output — an
            // entity stop always consumes the round below — so the rocket
            // stops at the glass instead of passing it.
            // [orig: `test dword ptr [ebx+114h], 18000000h` @0x4e968a ->
            //  `mov dword ptr [eax], 1` @0x4e969a; DEF_AMMO_FLAG_LAWR 0x08000000
            //  | DEF_AMMO_FLAG_FGRENADE 0x10000000, the flag table @0x813500]
        }

		if (!not_armed && target != nullptr && ammo != nullptr) {
			// The impact helper receives the ray's incoming direction, not
			// the struck face normal. [orig: Projectile_UpdatePhysics @0x4E9D70,
			// normalized ray @0x4EA20A, final argument @0x4EA73A]
			const int32_t magnitude = fixed_magnitude(velocity_q16);
			const int32_t inverse = magnitude != 0 ? int32_t(0x100000000LL / magnitude) : 0;
			const int32_t incoming[3] = { retail_q16_mul_rhu(inverse, velocity_q16.x),
				retail_q16_mul_rhu(inverse, velocity_q16.y),
				retail_q16_mul_rhu(inverse, velocity_q16.z) };
			const int32_t hit[3] = { impact_q16.x, impact_q16.y, impact_q16.z };
			world.vehicles.projectile_impact(*target, ammo->weight_in_grains, incoming, hit);
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

        if (!not_armed) process_damage_hit(world, r, collision, velocity_q16);

        RoundImpact imp;
        imp.position = impact_position;
        // The orientation the physical handlers hand the effect spawner. The
        // entity and person handlers pass the ray record as the descriptor's
        // orientation source, and its +24..+32 is the round's normalized flight
        // direction [orig: Projectile_HandleEntityImpact @0x4e97c5 and
        // Projectile_HandleTerrainImpact_0 @0x4e9a57 -> AmmoDef_ProcessImpactEffect
        // @0x40a240 -> CEffectWorld_SpawnEmitterAtPosition @0x5f6e44; the record
        // is laid out in Projectile_UpdatePhysics @0x4ea241..0x4ea25f]. The
        // terrain and water handlers build their descriptor with a NULL record
        // [orig: Projectile_HandleTerrainImpact @0x4e92c8;
        // Projectile_SpawnImpactEffect @0x4e9d1c], which the spawner turns into
        // a zero orientation [orig: @0x5f6e52..0x5f6e5c]: every EMITVECTOR
        // member keeps a zero emission axis [orig:
        // CEffectEmitter_SetOrientationFromDirection @0x5e5d51] and the
        // direction helper emits it around world +Y, so a dirt puff rises out
        // of the ground instead of following the round into it.
        imp.direction = flight_direction(r.vel);
        if (collision.hit_class == ProjectileHitClass::Terrain ||
            collision.hit_class == ProjectileHitClass::Water) {
            imp.direction = Vec3{0.0f, 0.0f, 0.0f};
        }
        imp.ammo_index = impact_ammo_index;
        if (collision.hit_class == ProjectileHitClass::Terrain) {
            // Permanent terrain-cache scorch, before the ordinary impact
            // presenter. Water and entity stops never enter this registry.
            // [orig: Projectile_HandleTerrainImpact @0x4E9314 reads ammo word
            // +0x74 and calls the standard scorch router @0x6060D0]
            const AmmoTableEntry *scorch_ammo =
                    world.tables.ammo.by_index(impact_ammo_index);
            if (scorch_ammo != nullptr) {
                world.out.terrain_scorches.emit_standard(
                        impact_q16.x, impact_q16.y,
                        scorch_ammo->scorch_id, world.logic_tick);
            }
            // Terrain hits sample the charmap surface type at the impact point,
            // shifted into the impact-effect table (no charmap -> 1 -> 5 dirt;
            // unmapped sector -> 7 -> 11 water). [orig:
            // Terrain_GetSurfaceTypeAtPosition @ 0x606510 result + 4; the
            // terrain leg of the @ 0x4ea6a7 hit switch in
            // Projectile_UpdatePhysics @ 0x4e9d70]
            const int32_t surface =
                terrain::surface_type_at_fixed(world.tables.surface_map, impact_q16.x, impact_q16.y);
            imp.effect_tag =
                (surface >= 0 && surface + 4 < kImpactEffectTagCount) ? surface + 4 : 5;
        } else if (collision.hit_class == ProjectileHitClass::Water) {
            imp.effect_tag = 11;
        } else if (person_collision) {
            // The person leg. Bullets reach a person ONLY through the bone-section
            // pass — Projectile_RaycastProximitySlots walks pool 2 (statics) for
            // slotType 2 and pool 1 (items) for slotType 1, never pool 0, so the
            // two proximity-slot dispatch cases cannot produce a person hit; pool 0
            // is reached by Physics_RaycastAgainstProximityList, whose sole narrow
            // phase is Physics_RaycastAgainstBoneSections. That is the pass we model
            // here, and its impact handler owns the rules below.
            // [orig: Entity_BuildProximityLists_Pool01 @0x4b9340 fills g_DynProx*
            //  from pool 1 @0x4b9389 and g_PersonProx* from pool 0 @0x4b93eb;
            //  Projectile_RaycastProximitySlots @0x4e5340; the bullet dispatch
            //  Projectile_UpdatePhysics @0x4e9d70 calls slotType 2 @0x4ea4f5 and
            //  slotType 1 @0x4ea535 only, then Physics_RaycastAgainstProximityList
            //  @0x4ea5bf -> Physics_RaycastAgainstBoneSections @0x4e4670 (its only
            //  caller, @0x4e4c59) -> Projectile_HandleTerrainImpact_0 @0x4e98f0]
            //
            // A victim already flagged dead presents NO impact effect at all; the
            // two tag legs below are mutually exclusive, not additive.
            // [orig: shouldPlayEffect = (hitEntity+36 & 2) == 0 @0x4e9920/@0x4e994f;
            //  the local/non-local split @0x4e9a55..0x4e9a73]
            const bool victim_dead =
                impact_target != nullptr &&
                (((impact_target->flags & kEntityFlagDead) != 0) ||
                 ((impact_target->engine_flags & kEntityFlagDead) != 0));
            const bool victim_is_local_player =
                impact_target != nullptr && local_player.valid() &&
                impact_target->handle == local_player;
            if (victim_dead) {
                imp.present_effect = false;
                imp.present_sound = false;
                imp.effect_tag = 2;
            } else if (victim_is_local_player) {
                // The local player takes the 'player' row.
                // [orig: push 2 @0x4e9aa1 -> the shared call @0x4e9ada]
                imp.effect_tag = 2;
            } else {
                // Everyone else takes the 'flesh' row — but retail SUPPRESSES it
                // for a healthy squad-mate: the effect is spawned only when the
                // victim's group differs from the local player's, OR the victim is
                // already below half of its items.def hp. A same-group victim at or
                // above half health shows nothing.
                // [orig: group WORDs compared @0x4e9aac/@0x4e9ab3 (entity+0x11C);
                //  healthMax WORD itemDef+0x17C halved by `sar dx,1` @0x4e9abf..
                //  @0x4e9ac6; signed Health WORD entity+0x11E compared @0x4e9ac9
                //  with `jge` skipping the spawn @0x4e9ad0; push 17h @0x4e9ad7]
                imp.effect_tag = 23;
                const Entity *local = world.registry.get(local_player);
                // Retail dereferences the victim's ItemDef unconditionally, so a
                // live person always has a real healthMax there. Ours carries 0 to
                // mean UNRESOLVED (entity.h) — which would halve to 0 and suppress
                // every same-group hit — so an unresolved max declines to suppress
                // rather than inventing a threshold.
                const int32_t victim_health_max =
                    impact_target != nullptr
                        ? retail_signed_i16(impact_target->health_max)
                        : 0;
                if (impact_target != nullptr && local != nullptr &&
                    victim_health_max > 0 &&
                    local->group_id == impact_target->group_id &&
                    retail_signed_i16(impact_target->health) >=
                        static_cast<int32_t>(victim_health_max >> 1)) {
                    imp.present_effect = false;
                    imp.present_sound = false;
                }
            }
        } else if (collision.surface_type >= 0 &&
                   collision.surface_type + 4 < kImpactEffectTagCount) {
            imp.effect_tag = collision.surface_type + 4;
        } else {
            imp.effect_tag = 4; // generic object without material data
        }
        // The dead-victim gate resolved above with the other face-material legs
        // [orig: `shouldProcessEffect` @0x4e95d6/@0x4e95e0, consumed @0x4e9817].
        if (entity_effect_suppressed) {
            imp.present_effect = false;
            imp.present_sound = false;
        }
        imp.tick = world.logic_tick;
        // Armor is an additional row before the ordinary person effect. Its
        // gate is independent of the dead/local/squad suppression below.
        // [orig: Projectile_HandleTerrainImpact_0 @0x4e99f8..0x4e9a38]
        if (person_collision && !submerged_stall && impact_target != nullptr &&
                collision.hit_zone >= 0 && collision.hit_zone <= 4 &&
                (impact_target->carry_flags & 8u) != 0) {
            RoundImpact armor = imp;
            armor.effect_tag = 24;
            armor.present_effect = true;
            armor.present_sound = true;
            armor.source_order = next_impact_order++;
            if (impacts.size() < kMaxPendingImpacts) impacts.push_back(armor);
        }
        imp.source_order = next_impact_order++;
        if (impacts.size() < kMaxPendingImpacts) impacts.push_back(imp);

        // The impact scar: a round stop on an ENTITY (item, vehicle, building
        // or person — terrain and water never scar) writes one ring slot per
        // the ammo's `scar_type` [orig: the impact-effect processor
        // AmmoDef_ProcessImpactEffect @0x40a24e..0x40a264 passes the entity,
        // the hit record and the kind (ammo word +0x76) to
        // Impact_SpawnGlassEffectsOrScar @0x5CF1B0 -> Scar_AddEntry @0x5CC830;
        // the GLASS userpoint leg's residual is recorded in world/impact_scar.h].
        if (impact_target != nullptr) {
            const AmmoTableEntry *scar_ammo = world.tables.ammo.by_index(impact_ammo_index);
            scar_add_entry(world, collision, *impact_target,
                           scar_ammo != nullptr ? scar_ammo->scar_type : 0);
        }

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
