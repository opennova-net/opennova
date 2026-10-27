// Item destruction — see world/destruction.h for the witness map.
#include <runtime/world/destruction.h>
#include <base/io/fixed.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <base/io/bam.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <base/crt/crt_rng.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/damage_pair.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/infantry.h>
#include <runtime/world/collision_force.h>
#include <runtime/world/player_view.h>
#include <runtime/world/vehicle_collision_damage.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/world.h>

using namespace opennova::crt;

namespace opennova::world {

int32_t entity_top_q16(const Entity &e) {
    return io::bam_add(e.position.z.q16, e.bound_radius.q16);
}

namespace {

// Authored impact slots use the entity pose; the fallback plays where the
// caller says (the routed water crossing's at the crossing point).
// [orig: Entity_UpdateFallingDeathPhysics @0x4940B2..0x494100; ground impact
// @0x49417E..0x4941B5; the DeathPiece_PhysicsUpdate twins @0x48F552..0x48F588
// and @0x48F726..0x48F759]
void wreck_impact_sound(World &world, const Entity &e, const ItemDeathTraits *traits, int slot,
		const char *fallback, Vec3 fallback_pos, DestructionEvents &events) {
	const auto *profile = world.tables.sound_profiles.find(
			traits && !traits->sound_profile.empty() ? traits->sound_profile.c_str() : "default");
	const std::string sound = profile ? profile->set_names[size_t(slot)] : std::string{};
	events.sounds.push_back(
			{ sound.empty() ? fallback : sound, sound.empty() ? fallback_pos : e.position });
}

// The water plane as the word retail compares against, 0 when none is authored
// (no dry sentinel). [orig: g_EnvWaterHeightFixed @0x26C6454]
int32_t world_water_q16(const World &world) {
    return world.env.water_z;
}

// The inline dword_31BFBB8 stream the blast's cause roll and the piece physics
// draw [orig: v = rol4(state + rol11(state)); state = v ^ 1, the low 16 bits the
// draw; the cause roll @0x4E6A6F..0x4E6A8E; DeathPiece_PhysicsUpdate
// @0x48F5B1..0x48F5F0]. The piece spawn draws World's three PRNG_Next16 streams
// instead (spawn_death_pieces), the wreck-fire crackle stream C.
uint16_t death_rand16(World &world) { return world.destruction_rng.next16(); }

// The BAM32/tick spin rate the aircraft death initializer stores directly, in
// the retail evaluation order: floor = ftol(min * 11930464.0f); r = PRNG % 100;
// t = r * 0.01f stays on the x87 stack in extended precision (0.01f is exactly
// 10737418 * 2^-30, so t * max_bam is exact in 64-bit integer arithmetic);
// max_bam = ftol(max * 11930464.0f); v = ftol(t * max_bam); max(v, floor).
// [orig: Death_RandomSpinRateBam @0x57B940 — fmul flt_7D76D0 (11930464.0)
//  @0x57b945, ftol @0x57b94b, PRNG_Next16 % 100 @0x57b952..0x57b95d, fild/fmul
//  flt_7C56A8 (0.01) @0x57b963..0x57b967, max ftol @0x57b977, fimul/ftol
//  @0x57b980..0x57b984, cmp/jge floor @0x57b989..0x57b98d]
int32_t death_random_spin_rate_bam(World &world, float min_deg, float max_deg) {
    const int32_t floor_bam = static_cast<int32_t>(static_cast<double>(min_deg) * 11930464.0);
    const int32_t roll = world.next_prng16() % 100; // stream A [orig: PRNG_Next16 @0x57B952]
    const int32_t max_bam = static_cast<int32_t>(static_cast<double>(max_deg) * 11930464.0);
    const int32_t v = static_cast<int32_t>(
            (static_cast<int64_t>(roll) * 10737418 * static_cast<int64_t>(max_bam)) >> 30);
    return v < floor_bam ? floor_bam : v;
}

// [orig: g_DeathPieceTypes @ 0x8404f0 — the 13 named rows, effect/sound slots
// resolved to their interning names ({name, slot} pair tables @ 0x849150 /
// @ 0x82F640). Field decode in world/destruction.h.]
const DeathPieceType kDeathPieceTypes[kDeathPieceTypeCount] = {
    // name        vel   launch spinMn spinMx prob  life bounce trail             bounce_fx             bounce_snd         splash_fx            splash_snd          final_fx          final_snd flags
    {"HULL",       1.0f, 0.0f,  0.0f,  0.0f,  1.0f,  1,  0.0f,  nullptr,          nullptr,              nullptr,           nullptr,             nullptr,            nullptr,          nullptr,  0},
    {"WHEEL",      0.3f, 0.0f,  3.5f,  14.0f, 0.5f,  48, 0.45f, "Effect_VexpM",   "Effect_DustBounceF", "IMP_DEBMED_LAND", "Effect_SmlSplash",  "IMP_DEBMED_WATER", "Effect_BurnScar", nullptr, 1},
    {"CHUNK_S",    0.5f, 0.75f, 2.5f,  16.0f, 1.0f,  12, 0.35f, "Effect_VexpS",   "Effect_DustBounceS", "IMP_DEBSML_LAND", "Effect_SmlSplash",  "IMP_DEBSML_WATER", nullptr,          nullptr,  0},
    {"CHUNK_M",    0.4f, 0.5f,  1.5f,  4.0f,  1.0f,  6,  0.25f, "Effect_VexpM",   "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
    {"CHUNK_L",    0.15f, 0.25f, 0.5f, 1.5f,  1.0f,  1,  0.1f,  "Effect_VexpL",   "Effect_DustBounceF", "IMP_DEBLRG_LAND", "Effect_LargeSplash", "IMP_DEBLRG_WATER", nullptr,         nullptr,  1},
    {"ROCK_S",     0.2f, 0.5f,  1.5f,  4.0f,  1.0f,  12, 0.2f,  "Effect_PDust_S", "Effect_DustBounce",  "IMP_DEBSML_LAND", "Effect_SmlSplash",  "IMP_DEBSML_WATER", nullptr,          nullptr,  0},
    {"ROCK_M",     0.1f, 0.25f, 1.5f,  3.0f,  1.0f,  6,  0.15f, "Effect_PDust_M", "Effect_DustBounce",  "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
    {"ROCK_L",     0.05f, 0.1f, 0.5f,  1.5f,  1.0f,  2,  0.1f,  nullptr,          "Effect_DustBounce",  "IMP_DEBLRG_LAND", "Effect_LargeSplash", "IMP_DEBLRG_WATER", nullptr,         nullptr,  1},
    {"CHUNKNP_S",  0.5f, 0.75f, 2.5f,  16.0f, 1.0f,  12, 0.35f, nullptr,          "Effect_DustBounceS", "IMP_DEBSML_LAND", "Effect_SmlSplash",  "IMP_DEBSML_WATER", nullptr,          nullptr,  0},
    {"CHUNKNP_M",  0.4f, 0.5f,  1.5f,  4.0f,  1.0f,  6,  0.25f, nullptr,          "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
    {"CHUNKNP_L",  0.15f, 0.25f, 0.5f, 1.5f,  1.0f,  1,  0.1f,  nullptr,          "Effect_DustBounceS", "IMP_DEBLRG_LAND", "Effect_LargeSplash", "IMP_DEBLRG_WATER", nullptr,         nullptr,  1},
    {"CACTUS_",    0.15f, 0.25f, 0.5f, 1.5f,  1.0f,  1,  0.1f,  nullptr,          "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  2},
    {"CHUNKSF_M",  0.4f, 0.5f,  1.5f,  4.0f,  1.0f,  6,  0.25f, "Effect_VexpSL",  "Effect_DustBounceS", "IMP_DEBMED_LAND", "Effect_MedSplash",  "IMP_DEBMED_WATER", nullptr,          nullptr,  0},
};

constexpr int32_t kPiecePhysicsGravityQ16 = 334;
constexpr int32_t kPiecePhysicsWaterFallFloorQ16 = -2048;
constexpr int32_t kPiecePhysicsProbeLiftQ16 = 0x4000;
constexpr int32_t kPiecePhysicsProbeDepthQ16 = 0x20000;
constexpr int32_t kPiecePhysicsProbeDistanceQ16 = 0x58000;
constexpr int32_t kPiecePhysicsLandingOffsetQ16 = 0x8000;
constexpr int32_t kPiecePhysicsYawStepBam = 0x02D82D82;
constexpr int32_t kPiecePitchSnapThresholdBam = 0x02D82D80;
constexpr int32_t kPiecePitchStepBam = 0x016C16C0;

void seed_piece_physics_angles(Entity &entity) {
    Entity::VehicleMotorState &motion = entity.veh;
    if (motion.yaw_seeded) return;
    motion.yaw_bam = entity.heading;
    motion.air_pitch_bam = entity.pitch_bam;
    motion.air_roll_bam = entity.roll_bam;
    motion.yaw_seeded = true;
}

void publish_piece_physics_angles(Entity &entity) {
    const Entity::VehicleMotorState &motion = entity.veh;
    entity.heading = motion.yaw_bam;
    entity.pitch_bam = motion.air_pitch_bam;
    entity.roll_bam = motion.air_roll_bam;
}

// Entity_RaycastGroundHeight @0x4142c0 casts a short vertical segment from
// entity Z + 0.25 down by 2.0 units. The portable terrain field supplies the
// terrain half of that query; the object-collision half remains D-ITEM-9.
int32_t piece_physics_ground_probe_q16(
        const terrain::TerrainHeightField *terrain, int32_t entity_x_q16,
        int32_t entity_y_q16, int32_t entity_z_q16, int32_t offset_x_q16,
        int32_t offset_y_q16) {
    const int32_t start_z =
            io::bam_add(entity_z_q16, kPiecePhysicsProbeLiftQ16);
    const int32_t end_z = io::bam_sub(start_z, kPiecePhysicsProbeDepthQ16);
    if (terrain == nullptr || !terrain->valid()) return end_z;
    const int32_t query_x = io::bam_add(entity_x_q16, offset_x_q16);
    const int32_t query_y = io::bam_add(entity_y_q16, offset_y_q16);
    const int32_t terrain_z =
            terrain::height_field_height_bilinear_fixed(*terrain, query_x, query_y);
    return terrain_z <= start_z && terrain_z >= end_z ? terrain_z : end_z;
}

struct PiecePhysicsSlope {
    int32_t forward_bam = 0;
    int32_t ground_q16 = 0;
};

// The Z-low rest correction's source model [orig: Entity_CalcSlopeForces
// @ 0x4b0c10..0x4b0c31]: `test byte ptr [entity+0x24], 4` picks the HUSK
// model (entity+0x34) when the entity is husked and the graphic model
// (entity+0x30) otherwise; a null pick leaves the offset at zero (no fallback
// to the other model); the value is the picked model's collision block
// (+0xB0) floor (+0x28 = the CMDL header bbox z-lo) x 240 >> 8. The port keeps
// the two halves of that pair where it stores them: the graphic floor is the
// vehicle probe box (VehicleTraits::box_z_lo, the CMDL z pair verbatim) and
// the husk floor is the husk-stage collision model's z-lo bound.
int32_t piece_physics_rest_floor_q16(const World &world, const Entity &entity) {
    if ((entity.engine_flags & kEntityFlagHusk) != 0) {
        const CollisionModel *husk = world.collision != nullptr
                ? world.collision->husk_model_for(world, entity.handle)
                : nullptr;
        return husk != nullptr ? husk->min[2] : 0;
    }
    const VehicleTraits *vehicle = world.vehicles.traits.get(entity.item_id);
    return vehicle != nullptr ? vehicle->box_z_lo : 0;
}

// Four table-quantized probes around the wreck, the clamped forward slope, then
// the picked model's Z-low rest correction. [orig: Entity_CalcSlopeForces
// @0x4B0B00 writes a clamped <<12 lateral slope @0x4B0BE0..0x4B0C0B that neither
// caller reads (DeathPiece_PhysicsUpdate @0x48F640, AI_UpdateFallingPhysics
// @0x457FEE take only its address); the lateral probes still feed the ground
// average @0x4B0C38..0x4B0C44]
PiecePhysicsSlope piece_physics_slope(
        const World &world, const Entity &entity,
        const terrain::TerrainHeightField *terrain, int32_t x_q16,
        int32_t y_q16, int32_t z_q16) {
    int32_t cos22 = 0;
    int32_t sin22 = 0;
    quantized_dir(entity.veh.yaw_bam, cos22, sin22);
    const int32_t forward_x = static_cast<int32_t>(
            (static_cast<int64_t>(kPiecePhysicsProbeDistanceQ16) * cos22) >> 22);
    const int32_t forward_y = static_cast<int32_t>(
            (static_cast<int64_t>(kPiecePhysicsProbeDistanceQ16) * sin22) >> 22);
    const int32_t height_forward = piece_physics_ground_probe_q16(
            terrain, x_q16, y_q16, z_q16, forward_x, forward_y);
    const int32_t height_backward = piece_physics_ground_probe_q16(
            terrain, x_q16, y_q16, z_q16, -forward_x, -forward_y);
    const int32_t lateral_x = io::bam_sar(forward_y, 2);
    const int32_t lateral_y = io::bam_sar(forward_x, 2);
    const int32_t height_left = piece_physics_ground_probe_q16(
            terrain, x_q16, y_q16, z_q16, -lateral_x, lateral_y);
    const int32_t height_right = piece_physics_ground_probe_q16(
            terrain, x_q16, y_q16, z_q16, lateral_x, -lateral_y);

    PiecePhysicsSlope out;
    const int32_t forward_delta = std::clamp(
            io::bam_sub(height_forward, height_backward), -655360, 655360);
    out.forward_bam = static_cast<int32_t>(
            static_cast<uint32_t>(forward_delta) << 10);

    int32_t sum = io::bam_add(height_backward, height_left);
    sum = io::bam_add(sum, height_right);
    sum = io::bam_add(sum, height_forward);
    sum = io::bam_add(sum, 2);
    // [orig: imul ecx, 0F0h; sar ecx, 8 @ 0x4b0c2b..0x4b0c31 on the picked
    // model's floor — a null pick keeps zero @ 0x4b0c0e/@ 0x4b0c20]
    const int32_t product = static_cast<int32_t>(
            static_cast<uint32_t>(piece_physics_rest_floor_q16(world, entity)) *
            240u);
    const int32_t speed_offset = io::bam_sar(product, 8);
    out.ground_q16 = io::bam_sub(io::bam_sar(sum, 2), speed_offset);
    return out;
}

void queue_named_landing_blast(World &world, const Entity &entity,
                               const char *ammo_name, float radius) {
    const int ammo_index = world.tables.ammo.index_of(ammo_name);
    if (ammo_index < 0) return;
    ExplosionEntry blast;
    if (const AmmoTableEntry *ammo = world.tables.ammo.by_index(ammo_index))
        blast.type = ammo->kztype;
    blast.ammo_index = ammo_index;
    blast.owner = entity.handle;
    blast.pos = entity.position;
    blast.radius_override = radius;
    // The gated entry [orig: WeaponEffect_QueueExplosion @0x4E8330, from
    //  DeathPiece_PhysicsUpdate @0x48F6FE / @0x48F71E, Entity_InitDeathState
    //  @0x48F864, Entity_UpdateFallingDeathPhysics @0x494209].
    world.explosions.queue_explosion_on_authority(world, blast);
}

// Retail uses the entity's complete placement matrix for model-local death
// anchors: Rz(90-yaw) * Ry(-pitch) * Rx(roll).  Reuse the same Q22 builder
// as collision so KZ points, death pieces, and the intact/husk shell cannot
// disagree about authored pitch/roll signs or multiplication order.
CollisionMatrix destruction_orientation(const Entity &target) {
    const int32_t origin[3] = {0, 0, 0};
    return collision_matrix_from_euler(
            target.heading,
            target.pitch_bam,
            target.roll_bam, origin);
}

Vec3 rotate_authored_point(const CollisionMatrix &orientation, const Vec3 &point) {
    const int32_t local[3] = {
            to_fixed(point.x), to_fixed(point.y), to_fixed(point.z)};
    int32_t rotated[3];
    orientation.rotate_point(local, rotated);
    return Vec3{rotated[0] * io::kInvFp16One, rotated[1] * io::kInvFp16One,
                rotated[2] * io::kInvFp16One};
}

// LOS between two word points: collision-world walk when available (terrain +
// pools), else the terrain leg alone; no data -> clear. The callers pass the
// words retail builds: pool 0 the victim's and the entry's Position words raw
// with a -0x4000 height offset (-0x2000 for the knife), pool 1 both Z words
// lifted 0x4000 with -0x4000 (D-NET-507). [orig: Projectile_ProcessExplosionQueue
// @0x4EAD80 - pool 0 @0x4EB0A8..0x4EB0D7, @0x4EB138, the call @0x4EB162; pool 1
// @0x4EB479..0x4EB4C3, the call @0x4EB4CA]
bool blast_los_clear(World &world, CollisionWorld *collision,
                     const terrain::TerrainHeightField *terrain,
                     const int32_t a[3], const int32_t b[3],
                     EntityHandle endpoint, int32_t height_offset, bool query_parent_cleared) {
    if (collision != nullptr) {
        const CollisionWorld::RayDebugScope ray_scope(
                collision, CollisionWorld::RayDebugCategory::kExplosionLos);
        // The blast caller passes entity B = null, radius -0.25 and
        // allTypes = 1 (`push 1` @0x4eb148 / @0x4eb49a): EVERY candidate kind
        // blocks a blast — vehicles, crates, emplacements, items — not just
        // buildings, and the flag-27 rows stay in. It uses the victim's
        // candidate slice, not the global pool ray, so a quantized mine point
        // just below terrain remains reachable. The pool-0 leg also nulls the
        // victim's parentEntity around the call (@0x4eb158/@0x4eb16c).
        // [orig: Projectile_ProcessExplosionQueue @ 0x4EAD80, calls @ 0x4EB162/0x4EB4CA]
        return collision->entity_los_clear(world, endpoint, {}, a, b, height_offset,
                                           /*all_types=*/true, query_parent_cleared);
    }
    if (terrain != nullptr && terrain->valid()) {
        // The same null-entity terrain leg when no collision device is bound:
        // the ray rides the height offset's negation above the points.
        // [orig: Physics_CheckTerrainLineOfSight @ 0x53B080]
        const auto buried = [&](const int32_t p[3]) {
            return terrain::height_field_height_bilinear_fixed(*terrain, p[0], p[1]) > p[2];
        };
        if (buried(a) || buried(b)) return true;
        const int32_t raised_a[3] = {a[0], a[1], io::bam_sub(a[2], height_offset)};
        const int32_t raised_b[3] = {b[0], b[1], io::bam_sub(b[2], height_offset)};
        return !los_terrain_blocked(*terrain, raised_a, raised_b);
    }
    return true;
}

// The dead-attacker kill-credit walk [orig: @ 0x4eae8c..0x4eaece — while the
// candidate is dead and not player-controlled (Flags & 0x100), follow its own
// lastAttacker; two hops witnessed]. A hop takes the link as stored, empty
// included: a dead source nobody damaged credits no one [orig: the stores
// @ 0x4eaeb3 / @ 0x4eaece run ahead of the null branch].
EntityHandle resolve_attacker_chain(World &world, EntityHandle owner) {
    EntityHandle resolved = owner;
    for (int hop = 0; hop < 2; ++hop) {
        const Entity *e = world.registry.get(resolved);
        if (e == nullptr) break;
        if (e->health > 0 || (e->engine_flags & kEntityFlagPlayer) != 0) break;
        resolved = e->last_attacker;
    }
    return resolved;
}

// Whether the victim's event callback is the tree one — the class whose death
// launches section debris from the recorded blast center [orig: the
// `cmp [edi+1C8h], offset Entity_HandleDestructibleDeathEvent` gate on the
// +0x80 store @0x4eb53f]. A row without a class runs that same body.
bool runs_tree_death_body(const World &world, const Entity &target) {
	if (runs_brain_class_row(world, target)) return false;
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
	return traits == nullptr || traits->death_class == ItemDeathClass::kTree ||
			traits->death_class == ItemDeathClass::kUnwitnessed;
}

// The cone gate [orig: @ 0x4eafe0..0x4eaffa — atan2(dy, dx) in BAM vs the entry
// direction, |delta| <= the ammo kz_pieslice half-angle]. The compare is
// signed: abs is cdq/xor/sub and the reject is `jg` [orig: pool 0
// @0x4EAFF1..0x4EAFFA, pool 1 @0x4EB528..0x4EB52F, pool 2 @0x4EB6EE..0x4EB6F7],
// so a victim at the exact antipode (delta 0x80000000, whose abs stays
// INT_MIN) passes even the narrowest cone, and a negative half-angle rejects
// every other bearing.
// The bearing takes the victim's word deltas, each fild exactly (D-NET-507)
// [orig: `fild var_A8; fild var_A4; fpatan; fmul dbl_7C19D8` @0x4EAFD9..0x4EAFE3].
bool cone_compare(const ExplosionEntry &e, int32_t cone_half_bam, int32_t dx, int32_t dy) {
    const int64_t scaled = static_cast<int64_t>(
            std::atan2(static_cast<double>(dy), static_cast<double>(dx)) * io::kBamPerRadian);
    const uint32_t ang = static_cast<uint32_t>(scaled);
    const int32_t diff = static_cast<int32_t>(ang - static_cast<uint32_t>(e.dir_bam));
    const int32_t sign = diff < 0 ? -1 : 0;
    const int32_t abs_diff = static_cast<int32_t>(
            (static_cast<uint32_t>(diff) ^ static_cast<uint32_t>(sign)) -
            static_cast<uint32_t>(sign));
    return !(abs_diff > cone_half_bam);
}

// The gate every type but the knife runs: a zero half-angle passes every
// bearing [orig: pool 0 `cmp [esp+0C0h+var_98], 0; jz` @0x4EAFD2..0x4EAFD7].
bool cone_gate(const ExplosionEntry &e, int32_t cone_half_bam, int32_t dx, int32_t dy) {
    if (cone_half_bam == 0) return true;
    return cone_compare(e, cone_half_bam, dx, dy);
}

// A victim's sweep words: its word deltas from the entry (victim less entry,
// wrapping), each made absolute (`cdq; xor; sub`) and tested against the reach
// word, then the x87 length saturated at flt_7C19E0 and truncated against it
// (D-NET-507). [orig: Projectile_ProcessExplosionQueue @0x4EAD80 - pool 0
//  @0x4EAF29..0x4EAFA2, pool 1 @0x4EB3CD..0x4EB44F, pool 2 @0x4EB604..0x4EB690]
struct SweepWords {
    int32_t dx = 0, dy = 0, dz = 0;
    int32_t length = 0;
};
int32_t fixed_distance(const EntityPosition &a, const EntityPosition &b);
int32_t x87_length(int32_t dx, int32_t dy, int32_t dz);
bool sweep_within(const Entity &t, const ExplosionEntry &e, int32_t reach, SweepWords &out) {
    out.dx = io::bam_sub(t.position.x.q16, e.pos.x.q16);
    out.dy = io::bam_sub(t.position.y.q16, e.pos.y.q16);
    out.dz = io::bam_sub(t.position.z.q16, e.pos.z.q16);
    if (io::bam_abs(out.dx) > reach || io::bam_abs(out.dy) > reach || io::bam_abs(out.dz) > reach)
        return false;
    out.length = fixed_distance(t.position, e.pos);
    return out.length <= reach;
}

// Pool 2's distance from the entry to the victim's model box, the graphic
// model's +0x18 (X/Y) and +0x1C (Z) half-extents about the Position words: per
// axis the entry's overshoot past the near face (wrapping words, 0 inside),
// each held to the radius word, then their saturated truncated x87 length,
// held to it too (the bound sphere is not this leg's surface). Both extents
// are 0 for every 3DI3 model (ItemDeathTraits::model_radius_xy_q16 / _z_q16),
// so the box is the Position point. [orig: Projectile_ProcessExplosionQueue
// @0x4EAD80 - the extents `mov edx, [ebp+1Ch]` @0x4EB6FD / `mov eax, [ebp+18h]`
// @0x4EB700, the box @0x4EB6FD..0x4EB7AE, the length @0x4EB7B4..0x4EB7EA]
bool pool2_box_distance(const Entity &t, const ExplosionEntry &e, const SweepWords &sw,
                        int32_t half_xy, int32_t half_z, int32_t radius, int32_t &out) {
    const auto axis = [&](int32_t victim, int32_t entry, int32_t victim_less_entry,
                          int32_t half) {
        int32_t d = 0;
        if (entry > io::bam_add(victim, half))
            d = io::bam_sub(io::bam_sub(entry, victim), half); // @0x4EB70E..0x4EB724
        if (entry < io::bam_sub(victim, half))
            d = io::bam_sub(victim_less_entry, half); // @0x4EB728..0x4EB734
        return d;
    };
    const int32_t dx = axis(t.position.x.q16, e.pos.x.q16, sw.dx, half_xy);
    if (dx > radius) return false; // @0x4EB738..0x4EB73C
    const int32_t dy = axis(t.position.y.q16, e.pos.y.q16, sw.dy, half_xy);
    if (dy > radius) return false; // @0x4EB76F..0x4EB773
    const int32_t dz = axis(t.position.z.q16, e.pos.z.q16, sw.dz, half_z);
    if (dz > radius) return false; // @0x4EB7AA..0x4EB7AE
    out = x87_length(dx, dy, dz);
    return out <= radius; // @0x4EB7E6..0x4EB7EA
}

// The victim's distance to the blast past its bound word, a signed subtract
// floored at 0 (`sub; jns`). [orig: pool 0 @0x4EB03C..0x4EB042, pool 1
// @0x4EB56F..0x4EB577]
int32_t sweep_surface(int32_t length, int32_t bound) {
    const int32_t surface = io::bam_sub(length, bound);
    return surface < 0 ? 0 : surface;
}

// The fixed-point length of a victim's offset: the fixed deltas' length,
// capped at 2147418112.0 and truncated [orig: pool 0 @0x4EAF66..0x4EAF97,
// flt_7C19E0, _ftol2_sse].
int32_t fixed_distance(const EntityPosition &a, const EntityPosition &b) {
    return x87_length(io::bam_sub(a.x.q16, b.x.q16), io::bam_sub(a.y.q16, b.y.q16),
                      io::bam_sub(a.z.q16, b.z.q16));
}

// The three words' x87 length, (dx^2 + dy^2) + dz^2, saturated at flt_7C19E0
// and truncated [orig: pool 0 @0x4EAF66..0x4EAF97; pool 2's box length
// @0x4EB7B4..0x4EB7DF].
int32_t x87_length(int32_t dx_word, int32_t dy_word, int32_t dz_word) {
    const double dx = double(dx_word);
    const double dy = double(dy_word);
    const double dz = double(dz_word);
    double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist > 2147418112.0) dist = 2147418112.0;
    return io::retail_ftol_sse2(dist);
}

// Shared health drain for a non-organic victim + the item death notify. It
// runs for a live victim whatever the damage, 0 included: an armor-blocked
// blast still stores the attacker and notifies the class callback.
// [orig: the Entity_ApplyWeaponDamage non-person tail @ 0x4e6f0d-0x4e6fc1 —
//  the only gates are Flags & 2 @0x4E6EFD and Health > 0 @0x4E6F0A]
void apply_item_blast_damage(World &world, Entity &target, int32_t damage,
                             EntityHandle attacker, int32_t ammo_index) {
    if ((target.engine_flags & kEntityFlagDead) != 0 || target.health <= 0) return;
    const int32_t before = target.health;
    target.last_attacker = attacker; // [orig: the +0x178 chain store @ 0x4e6f18]
    if (damage < before)
        target.health = before - damage;
    else
        target.health = 0;
    // deathCallback(entity, 2, 0) — the explosion-hit notify [orig: @ 0x4e6f93].
    const EntityHandle victim = target.handle; // a device's event may remove the item
    destruction_notify_item_damage(world, target, 2, {0, damage});
    if (target.health <= 0 && before > 0) {
        // The kill scores here, at once, after the callback, crediting +0x178 as the
        // callback left it; an item the callback removed scores nothing (its zeroed
        // words in retail). The record stages the death for the host's broadcasts.
        // [orig: Entity_ApplyWeaponDamage @0x4E6820 - Health <= 0 @0x4E6F98, before > 0
        //  @0x4E6FA2, +0x178 @0x4E6FA9, the Score_ProcessKillEvent call @0x4E6FB4]
        if (const Entity *still = world.registry.get(victim))
            world.match.process_kill_event(world, still->last_attacker, victim);
        RoundDeath d;
        d.victim = victim;
        d.killer = attacker;
        d.victim_handle = victim.packed;
        d.killer_handle = attacker.packed;
        d.ammo_index = ammo_index;
        world.round_sim.deaths.push_back(d);
    }
}

// The per-victim callback a queue entry's type installs [orig: the jumptable
// @ 0x4eadc6 targets — Entity_ApplyWeaponDamage, Entity_ApplyKnifeKillZoneDamage,
// GameEvent_HandleMedicInteraction, nullsub_91].
enum class KillZoneCallback : uint8_t { None, WeaponDamage, Melee, Medic };

// The kz-type-3 callback for one pool-0 victim: a person that is not the
// healer, a live healer, the same team; a DEAD victim not already being revived
// is revived, a live one below its max health healed. The max is the def hp:
// the difficulty scaling touches only the local player outside a network
// session, where GameEvent_HealPlayer never finds the second player slot it
// needs. The authority's host session runs each transaction here, in the
// queue, ahead of the pool-0 walk (a heal closes a death edge its walk would
// raise); a world it does not drive keeps the interaction for the host's drain.
// [orig: GameEvent_HandleMedicInteraction @0x4E6790 - the victim's def type 3
//  @0x4E679C, the healer (entry+0x20) @0x4E67A3, the gates @0x4E67A6..0x4E67BD,
//  the dead arm @0x4E67C2..0x4E67D8, the live arm @0x4E67E3..0x4E6805;
//  Entity_GetMaxHealthWithDifficulty @0x43B8A0, the session and local-player
//  tests @0x43B8AD..0x43B8C6]
void medic_interaction_callback(World &world, Entity &target, const ExplosionEntry &e) {
    if (target.item_type != 3) return;
    const Entity *healer = world.registry.get(e.owner);
    if (healer == nullptr || healer == &target || (healer->flags & kEntityFlagDead) != 0u ||
            healer->team != target.team)
        return;
    MedicInteraction interaction{target.handle, healer->handle, false};
    if ((target.flags & kEntityFlagDead) != 0u) {
        if (target.medic_reviving) return;
        interaction.revive = true;
    } else if (retail_signed_i16(target.health) >= retail_signed_i16(target.health_max)) {
        return;
    }
    // Both arms act on the authority alone: a peer's own kit entry (queued on
    // every peer) revives and heals nothing [orig: GameEvent_HandleMedicInteraction
    //  @0x4E6790 - `cmp g_NapiNPCtx.is_authority` @0x4E67CD / `jz` @0x4E67D4 (the
    //  dead arm), @0x4E67F7 / @0x4E67FE (the live arm)].
    if (!world.ai.is_authority) return;
    if (world.entity_update_transactions != nullptr)
        world.entity_update_transactions->medic_interaction(world, interaction);
    else
        world.round_sim.medic_interactions.push_back(interaction);
}

// The ammo's kill-zone damage as the drain and its callbacks read it: the
// kz_damage word off-session or on the session authority, 0 on a session
// peer [orig: AmmoDef_GetKzDamageIfAuthority @ 0x4E4010, which returns word
// +0x2E @0x4E4029].
int32_t kz_damage_if_authority(const World &world, const AmmoTableEntry &ammo) {
    if (world.rules.mp_session && !world.rules.logic_authority) return 0;
    return ammo.kz_damage;
}

// The kind-1 (knife) kill-zone callback for one victim [orig:
// Entity_ApplyKnifeKillZoneDamage @ 0x4E6620]. Only a live, non-immune
// person other than the source takes it; it deals the ammo's kz_damage in
// full (no falloff, armor, occupant or NoDie term), latches the knife cause
// bit and notifies the class callback with event 3.
void entity_apply_melee_damage(World &world, Entity &target, const ExplosionEntry &e) {
    const uint32_t flags = target.flags | target.engine_flags;
    // [orig: the person test @0x4E6633, Flags & 2 @0x4E6647, the source
    //  test @0x4E6657, Flags & 0x4000000 @0x4E665F]
    if (!collision_damage_applies(target.item_type, flags, target.handle == e.owner)) return;
    // The approach quadrant from the victim toward the entry point.
    // [orig: @0x4E666A..0x4E66B1 — the fpatan @0x4E668D; the shift @0x4E6725]
    const int quadrant = approach_quadrant(
            target.heading,
            io::bam_sub(e.pos.x.q16, target.position.x.q16),
            io::bam_sub(e.pos.y.q16, target.position.y.q16),
            kCollisionQuadrantBias);
    const int32_t before = target.health;
    if (before <= 0) return; // [orig: `test bp, bp; jle` @0x4E66B7..0x4E66BE]
    // The kill credit: the dead-source walk [orig: @0x4E66C4..0x4E6707 — the
    // first +0x178 store @0x4E66C6].
    target.last_attacker = resolve_attacker_chain(world, e.owner);
    const AmmoTableEntry *ammo = world.tables.ammo.by_index(e.ammo_index);
    const int32_t damage = ammo != nullptr ? kz_damage_if_authority(world, *ammo) : 0;
    // The signed-word subtraction, no clamp [orig: @0x4E6729 / @0x4E672E].
    target.health = retail_signed_i16(static_cast<int64_t>(before) - damage);
    target.death_anim_state = compute_death_anim_state(
            kCollisionDeathBone, quadrant, kCollisionDeathCause); // [orig: @0x4E674C]
    target.cause_flags |= kDamageFlagCollision; // [orig: @0x4E6740]
    // deathCallback(target, 3, 0) for every victim, no null test [orig: the event word
    // @0x4E673A, the call @0x4E6752]: a player body runs its bound row's callback
    // (player_body_row_event, D-NET-348): the plyr body keeps its cause bits (3 is in the
    // no-clear set), runs its health tail and re-arms its think before its waypoint tail [orig:
    // Entity_HandleDamageAndTriggerZones @0x407B3B..0x407B4F, @0x407B5E, the tail
    // @0x407B64..0x407C6B]; an NPC the organic class's, its think re-armed at 62
    // (person_class_event). The AI reaction rides the RoundHit drain, as for a blast.
    world.round_sim.hits.push_back(RoundHit{target.handle, target.last_attacker, damage});
    const EntityHandle victim = target.handle; // a device row's event may remove the body
    person_class_event(world, target, 3);
    if (collision_kill_fires(before, target.health)) {
        // The kill scores here, at once, after the event-3 callback, and credits the
        // entry's source itself, not the walk; a body the callback removed scores
        // nothing (its zeroed def word in retail)
        // [orig: Entity_ApplyKnifeKillZoneDamage @0x4E6620 - the Health re-read
        //  @0x4E6757, before > 0 @0x4E6761, `mov edx, [ebx+20h]` @0x4E676A, the
        //  Score_ProcessKillEvent call @0x4E6773].
        world.match.process_kill_event(world, e.owner, victim);
        RoundDeath d;
        d.victim = victim;
        d.killer = e.owner;
        d.victim_handle = victim.packed;
        d.killer_handle = e.owner.packed;
        d.ammo_index = e.ammo_index;
        d.event_flags = target.cause_flags & 0xF00u;
        world.round_sim.deaths.push_back(d);
    }
}

// The AoE damage applicator for one victim [orig: Entity_ApplyWeaponDamage
// @ 0x4e6820]. `d16` is the surface distance word (the center length less the
// victim's bound word, floored at 0 by the sweep), `blast16` the resolved
// radius word, the two dwords the sweep pushes (D-NET-507) [orig: `push var_AC;
// push var_B0` @0x4EB306..0x4EB30F].
void entity_apply_weapon_damage(World &world, CollisionWorld *collision, Entity &target, const ExplosionEntry &e,
                                int32_t d16, int32_t blast16) {
    if ((target.engine_flags & kEntityFlagDead) != 0) return; // [orig: Flags & 2 @ 0x4e682e]
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
    // In a session only the authority applies blast damage, and a Building
    // takes it only under the destroy-buildings rule. `World::mp_session` is
    // the retail session discriminator here: our socketless SP host still uses
    // loopback transport but must retain offline damage semantics.
    // [orig: g_NapiNPCtx.is_in_session && (!is_authority || ItemType_Building
    // && !g_DestroyBuildings) @0x4E682E..0x4E6860]
    if (world.rules.mp_session &&
        (!world.rules.logic_authority ||
         (target.kind == EntityKind::Building && !world.rules.destroy_buildings)))
        return;
    // The source entity itself (entry+0x20, not the resolved attacker) is the
    // side the same-team immunity compares, when the def authors attrib 0x8000
    // [orig: @ 0x4e686e..0x4e688d].
    const Entity *owner = world.registry.get(e.owner);
    if (owner != nullptr && traits != nullptr && traits->team_protect &&
        owner->team == target.team)
        return;
    // Indestructible / invulnerable armor word [orig: @ 0x4e68aa].
    if ((target.engine_flags & kEntityFlagIndestructible) != 0) return;
    if (traits != nullptr && traits->armor_blast == -1) return;
    // The store right after these gates is the victim's shot word (+0x1BA =
    // entry+0x28 @ 0x4e68b0), not an attacker: the attacker (+0x178) is
    // written only by the damage legs below, and by the drain afterwards when
    // the victim still has none [orig: @0x4EB319 / @0x4EB593 / @0x4EB86F].

    const AmmoTableEntry *ammo = world.tables.ammo.by_index(e.ammo_index);
    if (ammo == nullptr) return;
    // Authority-only base damage [orig: @ 0x4e68cf — non-authority reads 0].
    int32_t damage = ammo->kz_damage; // [orig: ammoDef word +46 @ 0x4e68d5]
    // Linear falloff from kz_minradius to the blast radius in 16.16: the fraction
    // is a truncating Q16 quotient and the multiply rounds at 0x8000; type 4
    // (radius blast / direct hit) skips it [orig: @ 0x4e6943-0x4e699c].
    if (e.type != ammo_kz::kRadiusBlast) {
        const int32_t min16 = to_fixed(ammo->kz_minradius);
        if (d16 > min16 && blast16 - min16 > 0) {
            const int32_t t16 = static_cast<int32_t>(
                    (static_cast<int64_t>(d16 - min16) << 16) / (blast16 - min16));
            damage = static_cast<int32_t>(
                    (static_cast<int64_t>(damage) * (0x10000 - t16) + 0x8000) >> 16);
        }
    }
    // Blast armor class gate [orig: @ 0x4e69b0 — ammo penetration_kz (+200)
    // must reach def+0x192].
    if (traits != nullptr && ammo->penetration_kz < traits->armor_blast) damage = 0;
    // The damage-disabled word (entity+0x124: a player's spawn protection, a
    // spectator's -1, a death piece's -1) zeroes the damage [orig: `cmp dword ptr
    // [esi+124h], 0` @ 0x4e69b8..0x4e69c3].
    if (target.damage_state != 0) damage = 0;
    // A vehicle's occupants reduce what reaches its hull; dfx2.exe's twin has
    // no such call, and keeps only the projectile path's (DFX2-16)
    // [orig: ItemDef type 1 @ 0x4e69c7 -> Entity_ApplyOccupantDamageScale
    //  @ 0x4e69d3] [orig: @0x4e3960, the test and the call gone after
    //  @0x4e3b07 (dfx2.exe)].
    if (world.rules.title != gameprofile::GAME_DFX2)
        damage = apply_vehicle_occupant_scale(world, target, damage);
    // NoDie clamps to health-1 [orig: @ 0x4e69e9].
    if (traits != nullptr && traits->no_die && damage >= target.health)
        damage = target.health - 1;
    // The kill credit this victim takes: the dead-source walk, run from the
    // entry's source when the leg stores it [orig: @ 0x4e6b0c..0x4e6b52 /
    // @ 0x4e6f13..0x4e6f59].
    const EntityHandle attacker = resolve_attacker_chain(world, e.owner);

    if (target.kind == EntityKind::Organic) {
        // The person path [orig: @ 0x4e6a01-0x4e6c5d]: the death-anim selection
        // runs for every person the callback reaches, damage 0 included —
        // bone hardcoded 1 (torso @ 0x4e6ac7), quadrant from the blast
        // direction vs the victim's heading, cause 2, or a ~25% roll for 3
        // taken ONLY when the surface distance lies in [4.0, 8.0) u (the PRNG
        // draw happens only in that band) [orig: @ 0x4e6a61-0x4e6aa0], 4 when
        // the source kz is Slash (type 7 @ 0x4e6abb). Only then does positive
        // damage on a live body take the leg [orig: `test edi, edi; jle` @
        // 0x4e6ad6, Flags & 2 @ 0x4e6ae1, Health > 0 @ 0x4e6aeb..0x4e6aee].
        // The quadrant reads the bearing from the victim toward the blast
        // point, biased by 0x1FFFFFFF, so a blast in front is quadrant 0
        // [orig: the entry-minus-target fpatan @ 0x4e6a07..0x4e6a2e,
        //  `add ecx, 1FFFFFFFh; shr ecx, 1Eh` @ 0x4e6a58..0x4e6a5e].
        const int32_t heading_bam = target.heading;
        const int quadrant = approach_quadrant(heading_bam,
                io::bam_sub(e.pos.x.q16, target.position.x.q16),
                io::bam_sub(e.pos.y.q16, target.position.y.q16), 0x1FFFFFFFu);
        int cause = death_cause::kExplosive;
        if (d16 >= 0x40000 && d16 < 0x80000)
            cause = (death_rand16(world) < 0x4000) ? death_cause::kFire : death_cause::kExplosive;
        if (e.type == ammo_kz::kSlash) cause = death_cause::kGeneric;
        const int32_t before = target.health;
        if (damage > 0 && (target.engine_flags & kEntityFlagDead) == 0 && before > 0) {
            if (damage < before)
                target.health = before - damage;
            else
                target.health = 0;
            target.last_attacker = attacker; // [orig: +0x178 @ 0x4e6b11]
            body_trace_blast(world, target, e.type, attacker, e.pos.words().data(), damage,
                    before); // the readout (body_trace.h)
            target.death_anim_state = compute_death_anim_state(1, quadrant, cause);
            // The processed hit feeds the AI reaction stamps, like a round hit
            // [orig: the deathCallback(2) notify @ 0x4e6b72].
            world.round_sim.hits.push_back(RoundHit{target.handle, attacker, damage});
            // That notify is the class callback with event 2, for every person it
            // reaches (person_class_event: a player body's bound row's,
            // player_body_row_event; an NPC the organic class's, its think
            // re-armed at 62): on a live plyr-row PLAYER
            // body the plyr callback runs its health tail, clears the kill-cause
            // bits 8..11 (a latched head-shot bit does not survive a blast, so
            // a blast kill routes as an ordinary death), re-arms the 64-tick
            // think and runs its waypoint tail
            // [orig: Entity_ApplyWeaponDamage @0x4E6820 (`mov eax, [esi+1C8h]`
            //  @0x4E6B63, the call @0x4E6B72);
            //  Entity_HandleDamageAndTriggerZones @0x40772f dead return;
            //  @0x407b4d..0x407b4f clear; @0x407b5e / @0x407c71 re-arm; the
            //  tail @0x407B64..0x407C6B].
            const EntityHandle victim = target.handle; // a device row's event may remove the body
            person_class_event(world, target, 2);
            // Right after that notify, a blast on the LOCAL player arms the red
            // damage vignette + the camera shake and the radar blip, unless the
            // record's kz type is 3 (the medic heal); the source is the entry's
            // entity (+0x20), the blip point the entry's position (+0x00)
            // [orig: @0x4e6b77..0x4e6b8a -> Player_OnDamageReceived @0x4dd880].
            if (target.handle == world.cached.local_player && e.type != ammo_kz::kMedic) {
                const int32_t blast[3] = {e.pos.x.q16, e.pos.y.q16, e.pos.z.q16};
                player_on_damage_received(world, radar_entity_source(world, e.owner), blast);
            }
            if (target.health <= 0 && before > 0) {
                world.script.relations.group(target.group_id).alert = TriggerRelations::kAlertRed;
                // The kill scores here, at once, after the class callback: the
                // dead-source walk runs again into +0x178 and the kill credits
                // it; a body the callback removed scores nothing (its zeroed
                // def word in retail). The death edge's scorer reads the cause
                // word as this blast's callback left it.
                // [orig: Entity_ApplyWeaponDamage @0x4E6820 - the callback
                //  @0x4E6B72, Health <= 0 @0x4E6B92, before > 0 @0x4E6B9C, the
                //  walk @0x4E6BA0..0x4E6BE1, the Score_ProcessKillEvent call
                //  @0x4E6BFE over +0x178 @0x4E6BF0]
                target.last_attacker = resolve_attacker_chain(world, e.owner);
                world.match.process_kill_event(world, target.last_attacker, victim);
                RoundDeath d;
                d.victim = victim;
                d.killer = target.last_attacker;
                d.victim_handle = victim.packed;
                d.killer_handle = target.last_attacker.packed;
                d.ammo_index = e.ammo_index;
                d.event_flags = target.cause_flags & 0xF00u;
                world.round_sim.deaths.push_back(d);
            }
        }
        // The person-blast impact effect (AmmoDef_ProcessImpactEffect tag 23
        // "flesh" @ 0x4e6c4b) rides the ammo impact rows the host already
        // presents; the world stays effect-name-free here.
        return;
    }

    // [orig: Entity_ApplyWeaponDamage @0x4E6C5E..0x4E6E6B]
    // The original transforms every COBJ AABB center through the callback's
    // first matrix. This is an inclusive box admission; the later vector
    // normalization supplies the break helper, not a spherical rejection.
    if (collision != nullptr) {
        const CollisionModel *model = collision->model(collision->entity_model_id(target.handle));
        CollisionMatrix matrix;
        if (model != nullptr && collision->entity_section_matrix(world, target.handle, 0, matrix)) {
            const int32_t center[3] = {e.pos.x.q16, e.pos.y.q16, e.pos.z.q16};
            const int32_t radius = blast16;
            for (size_t i = 0; i < model->sections.size(); ++i) {
                const CollisionSection &section = model->sections[i];
                if (!collision_section_breaks(section.flags)) continue;
                const int32_t local[3] = {
                    io::bam_add(section.min_x, section.max_x) >> 1,
                    io::bam_add(section.min_y, section.max_y) >> 1,
                    io::bam_add(section.min_z, section.max_z) >> 1};
                int32_t point[3];
                matrix.transform_point(local, point);
                bool inside = true;
                for (int axis = 0; axis < 3; ++axis)
                    if (int64_t(point[axis]) < int64_t(center[axis]) - radius ||
                            int64_t(point[axis]) > int64_t(center[axis]) + radius) inside = false;
                if (!inside) continue;
                const uint32_t bit = 1u << (static_cast<uint32_t>(i) & 31u);
                // The misleadingly named bounds helper only plays this sound;
                // its transformed bounds are dead locals [orig: @0x439C00].
                if (i != 0 && (target.section_mask & bit) == 0 && target.has_item_def)
                    world.out.fire_sounds.play_immediate("GLASS_SMASH",
                            {float(from_fixed(point[0])), float(from_fixed(point[1])), float(from_fixed(point[2]))},
                            target.bms_id, target.handle.packed);
                target.section_mask |= bit;
            }
        }
    }
    // The readout's item record, ahead of the drain (body_trace.h; the next tick record shows it).
    body_trace_blast(world, target, e.type, attacker, e.pos.words().data(), damage, target.health);
    apply_item_blast_damage(world, target, damage, attacker, e.ammo_index);
}

} // namespace

const DeathPieceType &death_piece_type(int index) {
    if (index < 0 || index >= kDeathPieceTypeCount) index = 0;
    return kDeathPieceTypes[index];
}

// ----------------------------------------------------------------------------
// ExplosionSim
// ----------------------------------------------------------------------------

void ExplosionSim::queue_explosion(World &world, const ExplosionEntry &e) {
    (void)world;
    // No gate: every peer that flies a round pushes its kill zone (FX-1); the
    // 64-entry cap drops silently [orig: WeaponEffect_PushExplosionQueueEntry
    // @0x4E83C0, `cmp edi, 40h; jge` @0x4E83C7..0x4E83CA].
    if (queue.size() >= static_cast<size_t>(kCapacity)) return;
    queue.push_back(e);
}

void ExplosionSim::queue_explosion_on_authority(World &world, const ExplosionEntry &e) {
    // [orig: WeaponEffect_QueueExplosion @0x4E8330 - `cmp g_NapiNPCtx.is_authority,
    //  edi; jz` @0x4E8333..0x4E8339, then the cap `cmp ebx, 40h; jge`
    //  @0x4E8342..0x4E8345]
    if (!world.rules.logic_authority) return;
    if (queue.size() >= static_cast<size_t>(kCapacity)) return;
    queue.push_back(e);
}

void ExplosionSim::process(World &world, CollisionWorld *collision,
                           const terrain::TerrainHeightField *terrain, DestructionEvents &events) {
    if (queue.empty()) return;
    // The drain consumes the whole queue and resets it [orig: the queue_index
    // loop @ 0x4ead97 + the count reset @ 0x4eb8a5]. Damage callbacks may push
    // NEW entries (the kz chain) — they land next tick, exactly like the
    // original's post-reset writes.
    std::vector<ExplosionEntry> batch;
    batch.swap(queue);
    for (const ExplosionEntry &e : batch) {
        ++events.explosions_processed;
        const AmmoTableEntry *ammo = world.tables.ammo.by_index(e.ammo_index);
        if (ammo == nullptr) continue;
        // The callback and radius each type takes [orig: the jumptable
        // @ 0x4eadc6]: 2/5/6/7 the weapon damage at kz_maxradius (@ 0x4eadcd);
        // 1 the knife callback at kz_maxradius, one unit wider when the
        // source's class carries KnifeBonus (@ 0x4eae0c..0x4eae34); 3 the medic
        // interaction at kz_maxradius when the source's class carries Medic
        // (AnimMap slot bit 8), else no callback at radius 0 (@0x4EADDD..0x4EAE0A);
        // 4 the weapon damage at the source's bound radius when there is a
        // source (@ 0x4eae3c..0x4eae52); any other type a no-op callback at
        // radius 0 (@ 0x4eae56). A nonzero override replaces the radius
        // (@ 0x4eae64..0x4eae80) and a zero radius drops the whole entry, the
        // kill switch for zero-radius bullet rows (@ 0x4eae84).
        // The radius is a word (var_AC): the ammo's kz_maxradius word, the
        // source's bound word, or the override float times 65536 truncated;
        // only a zero radius drops the entry (D-NET-507).
        const Entity *source = world.registry.get(e.owner);
        KillZoneCallback callback = KillZoneCallback::None;
        int32_t radius_q16 = 0;
        switch (e.type) {
            case ammo_kz::kKnife:
                callback = KillZoneCallback::Melee;
                radius_q16 = to_fixed(ammo->kz_maxradius); // [orig: E+28 -> +56 @ 0x4eae0c]
                if (source != nullptr &&
                        world.tables.class_has_attribute(
                                source->player_class, MissionTables::kCharAttrKnifeBonus))
                    radius_q16 = io::bam_add(radius_q16, 0x10000); // [orig: add edi, 10000h @ 0x4eae34]
                break;
            case ammo_kz::kMedic:
                // [orig: the charattr test @0x4EADEA..0x4EADF4, the radius
                //  @0x4EADF9, the callback @0x4EADFC]
                if (source != nullptr &&
                        world.tables.class_has_attribute(
                                source->player_class, MissionTables::kCharAttrMedic)) {
                    callback = KillZoneCallback::Medic;
                    radius_q16 = to_fixed(ammo->kz_maxradius);
                }
                break;
            case ammo_kz::kStandard:
            case ammo_kz::kC4:
            case ammo_kz::kBullets:
            case ammo_kz::kSlash:
                callback = KillZoneCallback::WeaponDamage;
                radius_q16 = to_fixed(ammo->kz_maxradius); // [orig: E+28 -> +56 @ 0x4eadcd]
                break;
            case ammo_kz::kRadiusBlast:
                callback = KillZoneCallback::WeaponDamage;
                radius_q16 = source != nullptr ? source->bound_radius.q16
                                               : to_fixed(ammo->kz_maxradius); // [orig: `mov edi, [eax]` @ 0x4eae52]
                break;
            default:
                break;
        }
        if (e.radius_override != 0.0f)
            radius_q16 = to_fixed(e.radius_override); // [orig: the E+0x2C float, fmul, _ftol2 @ 0x4eae64..0x4eae7e]
        if (radius_q16 == 0) continue;             // [orig: `cmp edi, ebx; jz` @ 0x4eae84]
        const int32_t cone_half = ammo->kz_pieslice_bam; // [orig: E+28 -> +60 @ 0x4eadad]
        const EntityHandle resolved = resolve_attacker_chain(world, e.owner);
        // The friendly-fire gate each pool leg asks of (victim, resolved
        // attacker) [orig: Projectile_DamagePairEligible @0x4E74F0, the calls
        // @0x4eb086 / @0x4eb45b / @0x4eb7f6].
        const Entity *resolved_entity = world.registry.get(resolved);
        const auto pair_protected = [&](const Entity &t) {
            return damage_pair_protected(world, t, resolved_entity);
        };
        // The in-session same-team skip pools 1 and 2 run in the sweep itself
        // for a victim whose def authors attrib 0x8000, against the entry's
        // source (entry+0x20), ahead of the cone: such a victim takes neither the
        // callback nor the attacker fill nor the blast-center store [orig: pool 1
        // @0x4EB4DA..0x4EB502 (the team bytes `mov cl, [edi+162h]` @0x4EB4F6), pool
        // 2 @0x4EB6A1..0x4EB6C9].
        const auto session_team_immune = [&](const Entity &t) {
            if (!world.rules.mp_session || source == nullptr) return false;
            const ItemDeathTraits *traits = world.tables.item_death_traits.get(t.item_id);
            return traits != nullptr && traits->team_protect && t.team == source->team;
        };
        // One victim's callback, then the drain's own attacker store when the
        // callback left the victim without one [orig: `call [esp+var_9C]`
        // @ 0x4eb312 / @ 0x4eb58c / @ 0x4eb868, the +0x178 fallbacks
        // @ 0x4eb319..0x4eb329 / @ 0x4eb593..0x4eb5a4 / @ 0x4eb86f..0x4eb880].
        // The leg stores the entry's +0x28 word into the victim's +0x1BA ahead
        // of the call (@0x4EB184 / @0x4EB585 / @0x4EB861); no reader of that
        // word is ported.
        const auto run_callback = [&](Entity &t, int32_t surface) {
            if (callback == KillZoneCallback::WeaponDamage)
                entity_apply_weapon_damage(world, collision, t, e, surface, radius_q16);
            else if (callback == KillZoneCallback::Melee)
                entity_apply_melee_damage(world, t, e);
            else if (callback == KillZoneCallback::Medic)
                medic_interaction_callback(world, t, e);
            if (!t.last_attacker.valid()) t.last_attacker = resolved;
        };
        const int32_t entry_words[3] = {e.pos.x.q16, e.pos.y.q16, e.pos.z.q16};

        // --- pool 0, organics [orig: @ 0x4eaeda-0x4eb32c] — skipped when the
        // ammo flags NoOItems [orig: the & 0x80000 gate @ 0x4eaece]. Every
        // type runs it, the medic kit included. ---
        if ((ammo->flags & kAmmoFlagNoOItems) == 0) {
            const size_t pool0 = world.registry.pool_capacity(0);
            for (size_t s = 0; s < pool0; ++s) {
                Entity *t = world.registry.get(EntityHandle::make(0, static_cast<int>(s)));
                // A row with no ItemTypeIndex, or Flags & 1, leaves the walk
                // [orig: @0x4EAF15..0x4EAF23].
                if (t == nullptr || t->item_type_index == 0 || (t->engine_flags & 0x1u) != 0)
                    continue;
                // The organic reaction band reaches the bound word plus twice
                // the radius word [orig: `mov ebx, [edi]; lea ebp, [ebx+eax*2]`
                // @0x4eaf29..0x4eaf2b].
                const int32_t bound = t->bound_radius.q16;
                const int32_t reach = static_cast<int32_t>(
                        static_cast<uint32_t>(bound) + 2u * static_cast<uint32_t>(radius_q16));
                SweepWords sw;
                if (!sweep_within(*t, e, reach, sw)) continue;
                // A victim whose damage-disabled word (entity+0x124: spawn
                // protection, a spectator's -1) is set skips straight to the
                // +0x1BA store and the player arm, whose own +0x124 test and the
                // kill event's null attacker leave nothing, then the callback's
                // +0x124 skip: nothing reaches it [orig: `cmp dword ptr [edi+124h],
                // 0; jnz loc_4EB178` @0x4EAFA8..0x4EAFAF; @0x4EB2C4, @0x4EB2FA..0x4EB300].
                if (t->damage_state != 0) continue;
                // The knife's zone (kztype 1, rounds_kz_Knife) skips the cone in
                // a session for a victim within its kz_minradius, and otherwise
                // runs the compare even at a zero half-angle; every other type
                // passes a zero half-angle unasked. Pools 1 and 2 have no knife
                // leg. [orig: `cmp ebp, 1` @0x4EAFB8, is_in_session @0x4EAFBD,
                // the fixed distance against [ammo+34h] (kz_minradius)
                // @0x4EAFC6..0x4EAFCE, `jmp loc_4EAFD9` @0x4EAFD0; kztype table
                // @0x8133E0]
                if (e.type == ammo_kz::kKnife) {
                    const bool within = world.rules.mp_session &&
                            sw.length <= to_fixed(ammo->kz_minradius);
                    if (!within && !cone_compare(e, cone_half, sw.dx, sw.dy)) continue;
                } else if (!cone_gate(e, cone_half, sw.dx, sw.dy)) {
                    continue;
                }
                // The slash (kztype 7) never reaches its source, and from a
                // source with an AI slot only the slot's target (+0xC)
                // [orig: @0x4EB000..0x4EB028].
                if (e.type == ammo_kz::kSlash && source != nullptr) {
                    if (t == source) continue;
                    if (const AiEntity *slot_owner = world.ai.for_handle(source->handle)) {
                        if (slot_owner->slot.f[3] == 0 ||
                                slot_owner->slot.f[3] != static_cast<int32_t>(t->handle.packed) + 1)
                            continue;
                    }
                }
                // The knife never reaches its own source [orig: @0x4EB02E..0x4EB036].
                if (e.type == ammo_kz::kKnife && t->handle == e.owner) continue;
                const int32_t surface = sweep_surface(sw.length, bound);
                // The OUTER-BAND flinch, ahead of the blast-radius cut: an
                // organic anywhere in the 2x reaction band takes the ammo
                // reaction notify, so an explosive near-miss that deals no
                // damage still floors the white flash and shakes the camera.
                // [orig: `if (victim->itemDef(+0x20)->type(+0x5C) == 3)
                //  Entity_OnDamageReceived(victim, entry.ammo, entry.owner)`
                //  @0x4eb04a..0x4eb05c]. Its AI half (wasHit / damageTimer /
                // lastAttacker) still rides the RoundHit drain, so a pure
                // near-miss stamps no AI reaction here yet.
                if (t->item_type == 3) entity_on_damage_received(world, *t, ammo);
                if (surface > radius_q16) continue; // [orig: @0x4eb064..0x4eb06c]
                // A medic entry and the attacker's own body pass the gate
                // unasked [orig: `cmp [esi+18h], 3` @0x4eb076, the self compare
                // @0x4eb07c, the call @0x4eb086].
                if (e.type != ammo_kz::kMedic && t->handle != resolved && pair_protected(*t))
                    continue;
                // An ammo whose kz_damage word is 0 skips the seat gate and the
                // LOS [orig: `cmp word ptr [eax+2Eh], 0; jz loc_4EB178`
                // @0x4EB09D..0x4EB0A2].
                if (ammo->kz_damage != 0) {
                    // The knife's LOS runs at -0x2000 and skips the seat gate;
                    // type 4 skips both [orig: `sub eax, 1; jz` @0x4EB0CF..0x4EB0DB
                    // -> `mov ecx, 0FFFFE000h` @0x4EB138; `sub eax, 3; jz`
                    // @0x4EB0DD..0x4EB0E0; `cmp ebp, 4; jz` @0x4EB13D..0x4EB140].
                    int32_t height_offset = -0x4000; // [orig: @0x4EB0D2]
                    if (e.type == ammo_kz::kKnife) {
                        height_offset = -0x2000;
                    } else if (e.type != ammo_kz::kRadiusBlast) {
                        // The mounted-occupant gate ahead of the LOS [orig: the
                        // parentSlot switch @0x4eb0e2..0x4eb136]: seats 1/2/5 with
                        // a parent that has no ItemDef, or a type-1 (vehicle)
                        // parent, leave the sweep — a crew takes its damage through
                        // the vehicle; seat 3 (a gun standing on something) leaves
                        // it when the parent's groundEntity is a vehicle. Every
                        // other seat state falls through to the LOS.
                        const SeatType seat = t->mount_type;
                        const Entity *parent = t->mount_target.valid()
                                ? world.registry.get(t->mount_target) : nullptr;
                        if (seat == SeatType::Passenger || seat == SeatType::Controller ||
                                seat == SeatType::Driver) {
                            if (parent != nullptr && (!parent->has_item_def || parent->item_type == 1))
                                continue;
                        } else if (seat == SeatType::Gunner) {
                            const Entity *ground = (parent != nullptr && parent->ground_target.valid())
                                    ? world.registry.get(parent->ground_target) : nullptr;
                            if (ground != nullptr && ground->has_item_def && ground->item_type == 1)
                                continue;
                        }
                    }
                    // The LOS gate from the victim's Position words to the
                    // entry's, unlifted [orig: @0x4EB0A8..0x4EB0CA, the call
                    // @0x4eb162]. The victim's parentEntity is NULLED around the
                    // call, so its own mount hull is a blocker here.
                    if (e.type != ammo_kz::kRadiusBlast) {
                        const int32_t victim_words[3] = {
                                t->position.x.q16, t->position.y.q16, t->position.z.q16};
                        if (!blast_los_clear(world, collision, terrain, victim_words, entry_words,
                                             t->handle, height_offset, /*query_parent_cleared=*/true))
                            continue;
                    }
                }
                // Burn reacts before the damage callback's armor/team gates.
                // [orig: Projectile_ProcessExplosionQueue @0x4EB1D2]
                if (t->item_type == 3)
                    apply_collision_force(world, *t, ammo->secondary_anim, ammo->kz_physics, e.pos.q16(), e.owner);
                // Then the victim's presentation, every pool-0 type: the
                // ammo's kz_sound full-volume at the victim, and its
                // secondary_effect into the victim's +0x1CC slot (the
                // witness sits on spawn_victim_hit_emitter).
                // [orig: @0x4EB1DA..0x4EB1EA (the +0x4C set ->
                //  Entity_PlaySound3D_FullVolume), @0x4EB1F2..0x4EB292 (the
                //  +0x48 effect)]
                if (!ammo->kz_sound.empty())
                    world.out.fire_sounds.play_immediate(ammo->kz_sound.c_str(),
                            t->position, t->bms_id, t->handle.packed);
                // What the slot holds goes first, whoever stored it (a script effect too).
                if (!ammo->secondary_effect.empty())
                    spawn_victim_hit_emitter(world, *t, ammo->secondary_effect, events);
                // An entry that deals no damage here stops after the burn:
                // a zero kz_damage, or any entry on a non-authority session
                // peer, where the damage read returns 0; the medic kit is
                // the exception [orig: AmmoDef_GetKzDamageIfAuthority @0x4E4010
                // called @ 0x4eb2a0, the type-3 test @ 0x4eb2ac..0x4eb2b0].
                if (kz_damage_if_authority(world, *ammo) == 0 && e.type != ammo_kz::kMedic)
                    continue;
                // A LIVE player body inside the blast arms the local damage
                // feedback a SECOND time, ahead of the damage callback: retail
                // gates on Flags & 0x100 (Player) && !(Flags & 2) (alive), the
                // victim's +0x124 word being zero, the victim being the local
                // player, and the entry's kz type not being 3. The kill event
                // the same leg calls next returns at its null attacker handle.
                // The source and point are the entry's +0x20 entity and +0x00
                // position, as in the damage callback's own arm.
                // [orig: @0x4eb2b6..0x4eb2df -> Player_OnDamageReceived @0x4dd880;
                //  Score_ProcessNetworkKillEvent @0x4eb2f2 -> @0x4fd49d]
                if (((t->flags | t->engine_flags) & kEntityFlagPlayer) != 0 &&
                        (t->engine_flags & kEntityFlagDead) == 0 &&
                        t->handle == world.cached.local_player &&
                        e.type != ammo_kz::kMedic) {
                    player_on_damage_received(world, radar_entity_source(world, e.owner), entry_words);
                }
                run_callback(*t, surface);
            }
        }

        // --- pool 1, movable items [orig: @ 0x4eb334-0x4eb5a8] — NoMItems
        // gate [orig: & 0x100000 @ 0x4eb378]. Types 1/3 never sweep items
        // [orig: @ 0x4eb337..0x4eb343], and nor does an entry that deals no
        // damage (a zero kz_damage, or a non-authority session peer)
        // [orig: @ 0x4eb349..0x4eb36c] — pool 2 included.
        if (e.type == ammo_kz::kKnife || e.type == ammo_kz::kMedic ||
                kz_damage_if_authority(world, *ammo) == 0)
            continue;
        if ((ammo->flags & kAmmoFlagNoMItems) == 0) {
            const size_t pool1 = world.registry.pool_capacity(1);
            for (size_t s = 0; s < pool1; ++s) {
                Entity *t = world.registry.get(EntityHandle::make(1, static_cast<int>(s)));
                if (t == nullptr || t->item_type_index == 0) continue; // [orig: @0x4EB3B5]
                if ((t->engine_flags & 0x1u) != 0 ||
                    (t->engine_flags & kEntityFlagIndestructible) != 0)
                    continue; // [orig: @ 0x4eb3bf..0x4eb3cb]
                // The reach is the bound word plus the radius word [orig:
                // `mov ebp, [edi]; add ebp, var_AC` @0x4eb3d2..0x4eb3d4].
                const int32_t bound = t->bound_radius.q16;
                SweepWords sw;
                if (!sweep_within(*t, e, io::bam_add(bound, radius_q16), sw)) continue;
                if (pair_protected(*t)) continue; // [orig: @0x4eb45b]
                // The LOS from the victim's Position words to the entry's, both
                // Z words lifted 0x4000, for every type when the ammo's
                // kz_damage word is nonzero; this leg leaves the item's
                // parentEntity in place [orig: `cmp word ptr [eax+2Eh], 0; jz`
                // @0x4EB472..0x4EB477, the points @0x4EB479..0x4EB4C3, the call
                // @0x4EB4CA].
                if (ammo->kz_damage != 0) {
                    const int32_t victim_words[3] = {t->position.x.q16, t->position.y.q16,
                            io::bam_add(t->position.z.q16, 0x4000)};
                    const int32_t lifted_entry[3] = {entry_words[0], entry_words[1],
                            io::bam_add(entry_words[2], 0x4000)};
                    if (!blast_los_clear(world, collision, terrain, victim_words, lifted_entry,
                                         t->handle, -0x4000, /*query_parent_cleared=*/false))
                        continue;
                }
                if (session_team_immune(*t)) continue;
                if (!cone_gate(e, cone_half, sw.dx, sw.dy)) continue;
                // An item with a zero bound word leaves the sweep [orig: `mov ecx,
                // [edi]; test ecx, ecx; jz` @0x4eb535..0x4eb539].
                if (bound == 0) continue;
                // A live row whose death callback is the destructible one takes
                // the entry's words into its savedLivePose, the section-debris
                // launch origin [orig: the deathCallback ==
                // Entity_HandleDestructibleDeathEvent check @0x4EB53F, Health > 0
                // @0x4EB54B..0x4EB553, the stores @0x4EB555..0x4EB569].
                if (t->health > 0 && runs_tree_death_body(world, *t)) {
                    std::copy_n(entry_words, 3, t->saved_live_pos);
                    t->saved_live_valid = true;
                }
                run_callback(*t, sweep_surface(sw.length, bound));
            }
        }

        // --- pool 2, static items/buildings [orig: @ 0x4eb5b8-0x4eb88c] —
        // NoDItems gate [orig: & 0x200000 @ 0x4eb5b8]. ---
        if ((ammo->flags & kAmmoFlagNoDItems) == 0) {
            const size_t pool2 = world.registry.pool_capacity(2);
            for (size_t s = 0; s < pool2; ++s) {
                Entity *t = world.registry.get(EntityHandle::make(2, static_cast<int>(s)));
                if (t == nullptr || t->item_type_index == 0 || (t->engine_flags & 0x1u) != 0)
                    continue; // [orig: @0x4EB5F4..0x4EB602]
                // The reach is the bound word plus the radius word [orig:
                // `mov ebp, [edi]; add ebp, var_AC` @0x4eb609..0x4eb60b].
                const int32_t bound = t->bound_radius.q16;
                SweepWords sw;
                if (!sweep_within(*t, e, io::bam_add(bound, radius_q16), sw)) continue;
                // A row with no graphic model (entity+0x30) leaves the walk
                // [orig: `mov ebp, [edi+30h]; test ebp, ebp; jz` @0x4EB696..0x4EB69B].
                if (!t->has_graphic_model) continue;
                if (session_team_immune(*t)) continue;
                if (!cone_gate(e, cone_half, sw.dx, sw.dy)) continue;
                const ItemDeathTraits *traits = world.tables.item_death_traits.get(t->item_id);
                int32_t distance = 0;
                if (!pool2_box_distance(*t, e, sw,
                                        traits != nullptr ? traits->model_radius_xy_q16 : 0,
                                        traits != nullptr ? traits->model_radius_z_q16 : 0,
                                        radius_q16, distance))
                    continue;
                if (pair_protected(*t)) continue; // [orig: @0x4eb7f6]
                // The four window calls (GLASS1..GLASS4) change nothing: every row
                // of retail's static model / GLASS table (35 rows, 192 bytes each)
                // carries the projected-decal word (+0xBC) set, and a set word
                // leaves the point before its scar and its four effect rolls, so
                // no scar, no effect, no PRNG draw and no CRT reseed follows a
                // blast's window point (the bullet leg owns window glass).
                // [orig: Projectile_ProcessExplosionQueue @0x4EAD80 - the calls
                //  @0x4EB814 / @0x4EB827 / @0x4EB83A / @0x4EB84D;
                //  Terrain_SpawnEffectsAtUserPoint @0x5CEE20 - `cmp [ecx+0BCh], 0;
                //  jnz` @0x5CF04D..0x5CF054 past Scar_AddEntry @0x5CF072 and the
                //  rolls @0x5CF0CA..0x5CF154 (their useAltProbability pushes from
                //  @0x5CF0B3; Effect_RollSurfaceEffectProbability @0x5CC1F0, its 100.0
                //  scale @0x5CC25A over g_SurfaceEffectProbTable @0x8418B8) to the next
                //  point @0x5CF180; Terrain_FindSurfaceMaterialEntry @0x5CC280 over the
                //  table @0x841980]
                // Pool 2 stores no blast center, and hands the callback the box
                // distance [orig: `push ebx` @0x4EB85E].
                run_callback(*t, distance);
            }
        }
    }
}

// ----------------------------------------------------------------------------
// The item death chain
// ----------------------------------------------------------------------------

namespace {

// The death sound + effect families + the kz blasts — the shared presentation
// tail every husked death runs [orig: Entity_InitDeathSounds @ 0x4939b0].
void emit_death_sounds_and_effects(World &world, Entity &target, bool silent) {
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
    DestructionEvents &ev = world.out.destruction;
    if (!silent && traits != nullptr && !traits->sound_death.empty())
        ev.sounds.push_back(DestructionSoundEvent{traits->sound_death, target.position});
    if (silent || traits == nullptr) return;
	// [orig: Entity_InitDeathSounds @0x4939B0 - @0x493A6E..0x493A73, `jge` @0x493A79]
	const bool submerged = entity_top_q16(target) < world_water_q16(world);
	spawn_death_effect_banks(target, *traits, submerged, ev);
	// The kz blasts: one kz_OrganicBlast r=5.0 per husk KZ user point, else one
	// at the entity with r = kz ?: bound radius [orig:
	// Entity_QueueKzBlastAtUserPoints(g_AmmoKzOrganicBlast, ..., "KZ", 1, ...)
	// @ 0x493b57; the fallback radius legs @ 0x4ead12-0x4ead68].
	const int kz_ammo = world.tables.ammo.index_of(kAmmoKzOrganicBlast);
    if (kz_ammo >= 0) {
        ExplosionEntry blast;
        blast.type = ammo_kz::kStandard; // kz_OrganicBlast kztype (word +44)
        if (const AmmoTableEntry *a = world.tables.ammo.by_index(kz_ammo))
            blast.type = a->kztype;
        blast.ammo_index = kz_ammo;
        blast.owner = target.handle;
        if (traits->kz_points.empty()) {
            blast.pos = target.position;
            // The def's kz, else the bound word in whole units, a signed divide
            // truncating toward zero (0 leaves the ammo's radius at the drain).
            // [orig: Entity_QueueKzBlastAtUserPoints @0x4EABF0 - the def +408
            //  test @0x4ead26, `mov eax, [esi]; cdq; and edx, 0FFFFh; add eax, edx;
            //  sar eax, 10h` @0x4EAD30..0x4EAD3B, fild @0x4ead42]
            blast.radius_override = traits->kz != 0.0f
                    ? traits->kz
                    : float(target.bound_radius.q16 / 65536);
            // The gated entry: a peer's death chain queues no blast (FOUND V1)
            // [orig: Entity_QueueKzBlastAtUserPoints -> WeaponEffect_QueueExplosion
            //  @0x4EAD68; @0x4E8333..0x4E8339].
            world.explosions.queue_explosion_on_authority(world, blast);
        } else {
            const CollisionMatrix orientation = destruction_orientation(target);
            for (const FixedVec3 &p : traits->kz_points) {
                // The user point's words through the entity's matrix onto its words
                // (D-NET-507) [orig: Entity_QueueKzBlastAtUserPoints @0x4EABF0 -
                //  the words @0x4EAC93..0x4EACAA, Math_FixedPointTransformPoint22
                //  @0x4EACB6].
                const int32_t local[3] = {p.x, p.y, p.z};
                int32_t offset[3];
                orientation.rotate_point(local, offset);
                blast.pos.set_q16({io::bam_add(target.position.x.q16, offset[0]),
                                   io::bam_add(target.position.y.q16, offset[1]),
                                   io::bam_add(target.position.z.q16, offset[2])});
                blast.radius_override = kKzPointBlastRadius; // [orig: the 5.0 at @ 0x4eace7]
                // [orig: -> WeaponEffect_QueueExplosion @0x4EACE7, gated
                //  @0x4E8333..0x4E8339]
                world.explosions.queue_explosion_on_authority(world, blast);
            }
        }
    }
}

} // namespace

void process_destructible_death(World &world, Entity &target) {
    // [orig: Entity_ProcessDestructibleDeath @ 0x43fbc0]
    DestructionEvents &ev = world.out.destruction;
    // Per-section debris burst — sample the intact model's collision faces
    // before the husk flag changes collision identity. [orig: the
    // Entity_SpawnSectionDebris loop @ 0x43fbd9].
    if (world.collision != nullptr) {
        // Resolve from the intact model before Flags|=Husk changes the target
        // view. The present pass receives fully positioned effect rows.
        world.collision->ensure_entity_instance(world, target.handle);
        const std::vector<SectionDebrisSample> samples =
                world.collision->sample_section_debris(
                        world, target.handle, target.saved_live_pos);
        // Material 17 (foliage) spawns with the ENTITY as the descriptor's
        // owner tag (+12), so its group takes the section gate; wood spawns
        // with tag 0 [orig: Entity_SpawnSectionDebris @ 0x43F580 — the entity
        // (ecx) kept in ebp @ 0x43F588; the material-17 branch @ 0x43f825
        // pushes it @ 0x43F838, the wood branch pushes eax, zeroed @ 0x43F80A,
        // @ 0x43F84C; the one submit @ 0x43F84D].
        for (const SectionDebrisSample &sample : samples) {
            DestructionEffectEvent debris{
                    sample.material == 17 ? kSectionDebrisFoliageEffect
                                          : kSectionDebrisWoodEffect,
                    sample.pos, sample.dir};
            debris.section_tagged = sample.material == 17;
            ev.effects.push_back(std::move(debris));
        }
        ev.debris_triangles += static_cast<int32_t>(samples.size());
    }
    // The dying entity's scar ring is cleared before the husk flag lands
    // [orig: Scar_ClearEntriesByEntity @ 0x5ccec0 (thunk @0x43a950), called
    //  from the death chain ahead of Flags |= 6 @ 0x43fbf6].
    world.out.scars.clear_entity(target.handle);
    target.engine_flags |= (kEntityFlagDead | kEntityFlagHusk); // [orig: Flags |= 6 @ 0x43fbf6]
    target.alive = false;
    target.health = 0;
    if (target.death_tick == 0) target.death_tick = world.logic_tick; // [orig: @ 0x43fbfd]
    ++ev.items_destroyed;
    ev.husk_swaps.push_back(husk_swap_event(target));
    // The item's footprint retires the terrain pages composed under it
    // [orig: Entity_ProcessDestructibleDeath @ 0x43fc12..0x43fc5e].
    // The box is the position words less and plus the bound word (twice on
    // x / y), word arithmetic [orig: @0x43FC12..0x43FC42].
    world.out.terrain_scorches.emit_page_invalidation(
            target.position.x.q16, target.position.y.q16, target.bound_radius.q16);
    // The S2C 0x26 entity-state broadcast (Server_SendEntityStatePacket
    // @ 0x509d70) is the net track's emit — staged with the other MP legs
    // (tracked §24).
    emit_death_sounds_and_effects(world, target, /*silent=*/false);
}

uint32_t spawn_death_pieces(World &world, Entity &target, bool silent) {
    // A refNum group's children are cleaned up before any gate: their held
    // gun words reset, their ammo re-splits, their gunners detach.
    // [orig: Entity_SpawnDeathPieces @0x493409..0x49344D ->
    //  Vehicle_ReleaseEWeapGroupOnDestruction @0x547040]
    if (target.ref_num != 0) world.vehicles.cleanup_destroyed_ref_group(target);
    // [orig: Entity_SpawnDeathPieces @ 0x493400] Gate: not already husked, a
    // husk model loaded, not fully underwater. The model test is the live
    // huskFinal ?: husk render object, not the items.def name: an authored
    // husk whose model did not load takes no pieces and keeps the def's
    // +0x1C4 mover, as a husk-less row does [orig: @0x4934AF..0x4934E5].
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
    if (traits == nullptr || !traits->husk_model_loaded) return 0;
    if ((target.engine_flags & kEntityFlagHusk) != 0) return 0;
    // [orig: Entity_SpawnDeathPieces @0x493400 - `mov ecx, [ebp+0Ch]; mov edx, [ebp+0];
    //  add edx, ecx; cmp edx, g_EnvWaterHeightFixed; jl` @0x4934D7..0x4934E5]
    if (entity_top_q16(target) < world_water_q16(world)) return 0;
    // The explosion glow flash [orig: the LightPool_SpawnGlowEffect
    // (@ 0x5a8d83) call inside Entity_SpawnDeathPieces @ 0x49351a — 2x the
    // piece model's bound radius, non-decorations only; the presenter's light
    // pool renders it (renderer/light_scene.h)].
    if (!traits->is_decoration && traits->piece_model.radius_q16 > 0) {
        world.out.destruction.death_lights.push_back(DeathLightEvent{
                target.position,
                2.0f * (traits->piece_model.radius_q16 * io::kInvFp16One)});
    }
    uint32_t mask = 0;
    // The loop bound is the HUSK MODEL's section count [orig: renderObj[8]+52
    // @ 0x49361a]; the items.def husk_sub_parts token is only the authored hint
    // (most retail defs author none).
    const int sections = traits->husk_section_count > 0
            ? traits->husk_section_count
            : static_cast<int>(traits->husk_sub_part_count);
    // The wreck's own motion folds into the spread base at 2x once it moves
    // faster than 0.1 u/tick; at rest the base is zero [orig: the velocity fold
    // @ 0x493589-0x4935ff — normalize2D(vel) * (|vel| * 2.0); eps 0.1
    // @ 0x7C69F4, scale flt 2.0 @ 0x7C3B90].
    const float wreck_vx = target.veh.vel_x / io::kFp16One;
    const float wreck_vy = target.veh.vel_y / io::kFp16One;
    const float wreck_speed = std::sqrt(wreck_vx * wreck_vx + wreck_vy * wreck_vy);
    float base_x = 0.0f, base_y = 0.0f;
    if (wreck_speed > 0.1f) {
        base_x = wreck_vx * 2.0f;
        base_y = wreck_vy * 2.0f;
    }
    // The launch_add multiplier is the AI row's normalized 3D motion direction
    // (zero for brainless items) [orig: aiRuntime+0x64..0x6C normalize
    // @ 0x49353c-0x49355d].
    float dir_ai_x = 0.0f, dir_ai_y = 0.0f;
    if (target.is_ai_capable) {
        const float az = target.veh.slide_z / io::kFp16One;
        const float alen =
                std::sqrt(wreck_vx * wreck_vx + wreck_vy * wreck_vy + az * az);
        if (alen > 1.0e-6f) {
            dir_ai_x = wreck_vx / alen;
            dir_ai_y = wreck_vy / alen;
        }
    }
    // Section centres ride the entity's orientation matrix entity+0xB4, which
    // the entry rebuilds from the live Euler triple and the entity scale
    // (+0x158 ?: def+0x1B8) unless the entity is a building
    // [orig: @ 0x493455..0x4934ac; Math_TransformPointFixedPoint22
    // (orientationMatrix) @ 0x4938e6].
    const CollisionMatrix orientation = entity_placement_matrix(target);
    // Every piece starts in the wreck's own pose, heading and pitch then spin
    // [orig: the entity+4..+0x18 copy @ 0x4936be..0x4936de].
    int32_t pose_bam[3];
    entity_live_euler_bam(target, pose_bam);
    for (int s = 1; s < sections; ++s) {
        // Every section spawns; only the TYPE lookup clamps at slot 16.
        const int type_idx = death_piece_type_index(traits->husk_sub_part_types, s);
        const DeathPieceType &tp = death_piece_type(type_idx);
        // Each draw takes retail's stream, in retail's order: the probability roll
        // C, the spread and the vertical B, the two spins and the bounce budget A;
        // the slide kick past the loop B.
        // [orig: PRNG_Next16_C @0x49365A; PRNG_Next16_B @0x49370E / @0x49372E /
        //  @0x493784 / @0x49396B; Death_RandomSpinRateBam @0x4937AB / @0x4937C0;
        //  PRNG_Next16 @0x49384F]
        // The roll runs unless the probability is exactly 1.0, and skips the piece only
        // when prob * 65536 falls below the draw, so a draw equal to it spawns
        // [orig: `fld1; fcomp; test ah,44h; jnp` @0x493649..0x493658; `fild; fld;
        //  fmul flt_7C32BC; fcompp; test ah,5; jnp` @0x493663..0x493677].
        if (tp.probability != 1.0f) {
            const uint16_t draw = world.next_prng16_c();
            if (static_cast<double>(tp.probability) * 65536.0 < static_cast<double>(draw))
                continue;
        }
        DeathPiece &p = world.death_pieces.alloc();
        p.active = true;
        p.item_id = target.item_id;
        p.section = static_cast<uint8_t>(s);
        p.type_index = static_cast<uint8_t>(type_idx);
        // def+0x1BC unless it is zero [orig: fcomp/jnp @ 0x4936f1..0x493708].
        p.render_scale = traits->debris_scale != 0.0f ? traits->debris_scale : 1.0f;
        p.pos = target.position;
        p.heading = pose_bam[0];
        p.pitch = pose_bam[1];
        p.roll = pose_bam[2];
        p.radius_q16 = traits->piece_model.radius_q16;
        // The piece collapses every other LOD-0 section of the piece model
        // [orig: @ 0x493889..0x4938b0 — shl wraps the count mod 32].
        for (int other = 0; other < sections; ++other)
            if (other != s) p.hidden_mask |= 1u << (other & 31);
        if (static_cast<size_t>(s) < traits->piece_model.section_origin_q16.size()) {
            // The piece starts at its section's COBJ centre through the
            // orientation matrix, not at the entity origin
            // [orig: the COBJ +0x38 read @ 0x4938b2..0x4938da, the add
            // @ 0x4938eb..0x493900].
            int32_t offset[3];
            orientation.rotate_point(
                    traits->piece_model.section_origin_q16[static_cast<size_t>(s)].data(),
                    offset);
            p.pos.set_q16({io::bam_add(p.pos.x.q16, offset[0]),
                           io::bam_add(p.pos.y.q16, offset[1]),
                           io::bam_add(p.pos.z.q16, offset[2])});
        }
        // Launch direction, the witnessed two-stage build [orig: @ 0x493718-
        // 0x49380e]: (1) 2D-normalize the +-0.5 random spread around the wreck
        // base; (2) add launch_add along the AI motion direction and 2D-normalize
        // again; the vertical is an INDEPENDENT rand [0,1) with the 1.25 lift
        // [orig: flt 1.25 @ 0x7C6F18] — so the horizontal launch speed is always
        // exactly vel_scale, only the direction varies.
        float hx = base_x + (static_cast<int32_t>(world.next_prng16_b()) - 0x8000) / io::kFp16One;
        float hy = base_y + (static_cast<int32_t>(world.next_prng16_b()) - 0x8000) / io::kFp16One;
        float hlen = std::sqrt(hx * hx + hy * hy);
        if (hlen > 1.0e-6f) {
            hx /= hlen;
            hy /= hlen;
        }
        hx += tp.launch_add * dir_ai_x;
        hy += tp.launch_add * dir_ai_y;
        hlen = std::sqrt(hx * hx + hy * hy);
        if (hlen > 1.0e-6f) {
            hx /= hlen;
            hy /= hlen;
        }
        const float vz = static_cast<int32_t>(world.next_prng16_b()) / io::kFp16One;
        // Spin rates: the BAM32 words per tick, max * (rand % 100) / 100 floored
        // at min [orig: the pair @0x4937AB / @0x4937C0 -> Death_RandomSpinRateBam
        // @0x57B940, stored @0x4937B0 / @0x4937C5], drawn after the vertical's
        // rand and ahead of the velocity words.
        p.spin_a = death_random_spin_rate_bam(world, tp.spin_min, tp.spin_max);
        p.spin_b = death_random_spin_rate_bam(world, tp.spin_min, tp.spin_max);
        // The velocity words: vel_scale times the unit direction times 65536,
        // truncated (_ftol2); the vertical lifts by 1.25 [orig: `fld [edi+10h];
        // fmul hx; fld flt_7C32BC; fmul st(1), st` @0x4937C8..0x4937DC -> +0x1C;
        // `fmul hy; fmul st, st(1)` @0x4937E4..0x4937ED -> +0x20; `fmul
        // flt_7C6F18; fmul vz; fmulp` @0x4937F5..0x493804 -> +0x24].
        const double scale = static_cast<double>(tp.vel_scale);
        p.vel[0] = io::retail_ftol_sse2(scale * hx * io::kFp16OneD);
        p.vel[1] = io::retail_ftol_sse2(scale * hy * io::kFp16OneD);
        p.vel[2] = io::retail_ftol_sse2(scale * 1.25 * static_cast<double>(vz) * io::kFp16OneD);
        // Bounce budget: rand % lifetime + 1, floored at lifetime/8
        // [orig: @ 0x49385b-0x493885].
        int bounces = tp.lifetime > 0 ? (world.next_prng16() % tp.lifetime) + 1 : 1;
        const int floor_b = tp.lifetime >> 3;
        if (bounces < floor_b) bounces = floor_b;
        if (bounces < 1) bounces = 1;
        p.bounces_left = bounces;
        p.flags = tp.flags;
        p.settled = false;
        // The per-piece trail effect/looped sound attach [orig: @ 0x493813] is
        // presented by the host from the type row (trail_fx), unless the
        // death was silent [orig: the (frameFlags & 1) test @ 0x493811].
        p.trail = !silent;
        mask |= (1u << (s & 31)); // x86 shl wraps the count mod 32 [orig: @ 0x493698]
    }
    target.spawned_piece_mask = mask; // [orig: entity+0x138 @ 0x493983]
    target.motor_suspended = false; // installing a callback replaces Entity_UpdateOrg0Movement
    // ... and the def's own, a genx item's parent follow included; a death
    // this gate refuses keeps it [orig: the +0x1C4 stores @0x493933 /
    //  @0x49393F / @0x49394B].
    target.drop_motion = DropMotion::None;
    target.death_motion = DeathMotionMode::Generic;
    if (traits->is_decoration)
        target.veh.slide_z -= 16182;
    else
        target.veh.slide_z += (world.next_prng16_b() >> 4) + 4096;
    // Successful entry installs the generic callback and applies the kick.
    // The unitType dispatch may replace it with a falling/static callback:
    // helicopters drop (-0.247), others pop (+0.0625 + rand/16)
    // [orig: @ 0x493969-0x493983 — def type 2 -> slideDecay -= 16182, else
    // += (rand16 >> 4) + 4096].
    return mask;
}

// [orig: Entity_InitDeathState @0x48F7C0]
void entity_init_aircraft_death(World &world, Entity &target, bool simulate) {
	stamp_saved_live_pose(target);
	// Death init clears the focal-wind slot's owner tag; the other clear is
	// entity destruction, which the rotor-wash tick's gone-owner sweep covers.
	// [orig: Terrain_ClearShadowTileSlot @0x5CB0D0 called @0x48F9F8]
	world.rotor_wash.release(target);
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
	if (world.rules.logic_authority) {
		const float radius = traits != nullptr && traits->kz != 0.0f
				? traits->kz
				: float(target.bound_radius.q16 / 65536);
		queue_named_landing_blast(world, target, kAmmoKzMItemBlast, radius);
	}
	world.out.scars.clear_entity(target.handle);
	const bool was_husked = ((target.flags | target.engine_flags) & kEntityFlagHusk) != 0;
	const uint32_t mask = spawn_death_pieces(world, target);
	emit_death_sounds_and_effects(world, target, false);
	if (simulate) {
        target.motor_suspended = false;
		target.death_motion = DeathMotionMode::PiecePhysics;
    }
	else {
		const bool water = target.position.z.q16 <= world.env.water_z;
		Vec3 pos = target.position;
		if (water)
			pos.z = float(from_fixed(world.env.water_z));
		world.out.destruction.effects.push_back(DestructionEffectEvent{
				water ? "Effect_MedSplash" : "Effect_HeloGroundHit", pos, Vec3{} });
		world.out.destruction.sounds.push_back(DestructionSoundEvent{
				water ? "EXPLO_HELO_WATER" : "EXPLO_HELO_LAND", target.position });
	}
	target.flags |= kEntityFlagDead | kEntityFlagHusk;
	target.engine_flags |= kEntityFlagDead | kEntityFlagHusk;
	target.alive = false;
	// Roll rate (+172) from 0.45..0.65 deg/tick, pitch rate (+168) the negated
	// 0.15..0.35 roll, in that PRNG order. [orig: Entity_InitDeathState
	// @0x48F98A / @0x48F9A4 -> Death_RandomSpinRateBam @0x57B940]
	target.veh.air_roll_rate = death_random_spin_rate_bam(world, 0.45f, 0.65f);
	target.veh.air_pitch_rate = -death_random_spin_rate_bam(world, 0.15f, 0.35f);
	if (target.veh.damage_fire_active) {
		DestructionEffectEvent release;
		release.family = 5;
		release.release = true;
		release.attach_net_id = target.net_id;
		release.attach_bms_id = target.bms_id;
		release.attach_spawn_origin = target.spawn_origin;
		release.attach_wire_handle = target.handle.packed;
		world.out.destruction.effects.push_back(std::move(release));
		target.veh.damage_fire_active = false;
	}
	if (!was_husked) {
		HuskSwapEvent husk = husk_swap_event(target);
		husk.spawned_piece_mask = mask;
		world.out.destruction.husk_swaps.push_back(husk);
		++world.out.destruction.items_destroyed;
	}
}

void entity_update_death_transforms(World &world, Entity &target, bool silent) {
    // [orig: Entity_UpdateDeathTransforms @ 0x494660 — pose snapshot, then the
    // unitType dispatch, then the death sounds.]
    // The refNum group cleanup leads. [orig: @0x494669..0x494673 ->
    // Vehicle_ReleaseEWeapGroupOnDestruction @0x547040]
    if (target.ref_num != 0) world.vehicles.cleanup_destroyed_ref_group(target);
    // The pose snapshot: savedLivePose (+0x80..+0x88) and the attitude triple
    // (+0x8C..+0x94) take Position and the Yaw / Pitch / Roll words, the one
    // store the dying vehicle's still-test reads (D-NET-507). [orig:
    // Entity_UpdateDeathTransforms @0x494660 - @0x49467B..0x4946B2, the +0x80
    // store @0x494684, +0x8C / +0x90 / +0x94 @0x4946A4 / @0x4946AB / @0x4946B2]
    stamp_saved_live_pose(target);
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
    const int unit_type = traits != nullptr ? traits->unit_type : 0;
    const bool matched_row = unit_type == 1 || unit_type == 2 || unit_type == 3 ||
                             unit_type == 5 || unit_type == 6 || unit_type == 7 ||
                             unit_type == 8 || unit_type == 10 || unit_type == 11 ||
                             unit_type == 12;
    const bool was_husked = (target.engine_flags & kEntityFlagHusk) != 0;
    if (!was_husked) target.death_motion = DeathMotionMode::None;
    uint32_t mask = 0;
    // The dispatch table @ 0x815410: every row spawns pieces and ORs Flags 6;
    // boats (5-8; the IDB's name for their callback says building) additionally play
    // the ship explosion and require a live husk [orig: Entity_ProcessBuildingDeath
    // @ 0x494420; the boat class Entity_GetVehicleClass @0x4f9e27]; bridges (11) add the
    // water shock at DEAD points (present-pass leg); the no-row default also
    // clears 0x20000 [orig: Flags & ~0x20006 | 6 @ 0x493f4b].
    switch (unit_type) {
    case 1: case 2: case 10: case 12:
        mask = spawn_death_pieces(world, target, silent);
        if (target.death_motion == DeathMotionMode::Generic)
            target.death_motion = DeathMotionMode::Falling;
        break;
    case 3:
        mask = spawn_death_pieces(world, target, silent);
        if (target.death_motion == DeathMotionMode::Generic)
            target.death_motion = DeathMotionMode::PiecePhysics;
        break;
    case 5: case 6: case 7: case 8: // unit_type_is_boat
        // The boat callback no-ops without a husk model, but the dispatch
        // still ORs the death flags after it [orig: the huskFinal||husk gate
        // @ 0x49442c wraps ONLY the callback body; Flags |= table flagBits
        // @ 0x493f63 runs regardless].
        if (traits != nullptr && traits->husk_model_loaded) {
            mask = spawn_death_pieces(world, target, silent);
            if (target.veh.slide_z > 0) target.veh.slide_z = 0;
            // The boat callback stores its +0x1C4 whether or not the piece
            // spawn took the death, so a drop's fall or ride ends here too
            // [orig: Entity_ProcessBuildingDeath @0x494420 - the store @0x49446F].
            target.motor_suspended = false;
            target.drop_motion = DropMotion::None;
            target.death_motion = DeathMotionMode::Static;
            world.out.destruction.sounds.push_back(
                    DestructionSoundEvent{kShipExplosionSound, target.position});
        }
        break;
    case kUnitTypeBridge:
        mask = spawn_death_pieces(world, target, silent);
        // The callback emits directly in world space: transform every FIRST-
        // husk DEAD point through the complete authored pose, retain x/y, and
        // force z to g_EnvWaterHeightFixed. Zero is a real raw plane here (not
        // the no-water sentinel used by submerged-death selection), and an
        // empty point bank has no entity-origin fallback. Every shock carries
        // the bridge as its descriptor tag, so it takes the section gate.
        // [orig: Entity_SpawnDeathEffectsAtBones @0x4944c0, the tag store
        //  @ 0x4945B4, the spawn @ 0x494635]
        if (!was_husked && traits != nullptr &&
            !traits->bridge_dead_points.empty()) {
            const CollisionMatrix orientation = destruction_orientation(target);
            const float water_z =
                    static_cast<float>(world.env.water_z) / io::kFp16One;
            for (const Vec3 &point : traits->bridge_dead_points) {
                const Vec3 offset = rotate_authored_point(orientation, point);
                DestructionEffectEvent shock{
                        kBridgeWaterShockEffect,
                        Vec3{target.position.x + offset.x,
                             target.position.y + offset.y, water_z},
                        Vec3{}};
                shock.section_tagged = true;
                world.out.destruction.effects.push_back(std::move(shock));
            }
        }
        break;
    default:
        mask = spawn_death_pieces(world, target, silent);
        break;
    }
    // The no-row arm's `(Flags & 0xFFFDFFFF) | 6` clears the matrix bit from
    // the one retail dword, so both halves of the port's split word drop it
    // (a joiner seeds both halves from the streamed dword).
    // [orig: Entity_DispatchDeathCallback @0x493F3C..0x493F4B]
    if (!matched_row) {
        target.engine_flags &= ~kEntityFlagMatrixBuilt;
        target.flags &= ~kEntityFlagMatrixBuilt;
    }
    // The second death entry clears the scar ring the same way
    // [orig: Scar_ClearEntriesByEntity @ 0x5ccec0 ahead of the Flags |= 6].
    if (!was_husked) world.out.scars.clear_entity(target.handle);
    // Retail keeps one Flags dword; the runtime `flags` half is the copy a
    // vehicle compact serializes, so the wreck streams the dead-pose form.
    // [orig: Entity_DispatchDeathCallback `or [edi+24h], edx` @0x493F63; its
    //  no-row arm @0x493F48]
    target.flags |= kEntityFlagDead | kEntityFlagHusk;
    target.engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    target.alive = false;
    if (target.death_tick == 0) target.death_tick = world.logic_tick;
    if (!was_husked) {
        HuskSwapEvent husk = husk_swap_event(target);
        husk.spawned_piece_mask = mask;
        world.out.destruction.husk_swaps.push_back(husk);
        ++world.out.destruction.items_destroyed;
    }
    // The successful spawn applied the death vertical kick [orig: @ 0x493969
    // — the key is the def TYPE word
    // (+0x5C) == 2 = DECORATION (the same field the glow-light skip tests
    // @ 0x4934ee), NOT the unitType dispatch word: decorations drop (-0.247),
    // everything else pops (+0.0625 + rand/16)].
    emit_death_sounds_and_effects(world, target, silent);
}

int32_t item_bullet_damage_gate(const World &world, const Entity &target,
                                int32_t damage, int32_t penetration_impact) {
    // [orig: Projectile_ProcessDamageOnTarget @ 0x4e7fb0 zeroing gates]
    if ((target.engine_flags & kEntityFlagIndestructible) != 0) return 0; // @ 0x4e7ff6
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
    if (traits != nullptr) {
        if (traits->armor_impact == -1) return 0;                 // @ 0x4e8019
        if (penetration_impact < traits->armor_impact) return 0;  // @ 0x4e802a
    }
    if (target.health <= 0) return 0;                             // @ 0x4e8032
    if (damage > target.health) damage = target.health;           // @ 0x4e8064
    if (traits != nullptr && traits->no_die && damage >= target.health)
        damage = target.health - 1;                               // @ 0x4e8074
    return damage;
}

// The settle transition [orig: Entity_TransitionToGroundDeath @ 0x493080]:
// play the authored `particlefinale` ground-impact effect once at the entity
// position (the def name +0x4E4, interned to the +0x4E2 handle at mission
// start; read @ 0x493088) and back the now-grounded pose into savedLivePose
// (@ 0x4930dd..0x493104). The adjacent periodic-sound clear
// (@ 0x4930aa -> 0x57b3e0) is a witnessed NO-OP in retail JO: the 256x20-B
// pool at 0x26B8050 has no producer (its allocator @ 0x57b380 and reset
// @ 0x57b360 are unreferenced), so clearing nothing is faithful and no pool
// is modeled. The four wreck emitter-handle releases stay with the D-ITEM-15
// bone-bank residual.
static void transition_to_ground_death(Entity &e, const ItemDeathTraits *traits,
                                       DestructionEvents &events) {
    if (traits != nullptr && !traits->particlefinale.empty())
        events.effects.push_back(DestructionEffectEvent{
                traits->particlefinale, e.position, Vec3{0.0f, 0.0f, 1.0f}});
	release_death_effect_bank(e, 2, events);
	stamp_saved_live_pose(e);
}

void tick_item_death_motion(World &world, Entity &entity,
        const terrain::TerrainHeightField *terrain, int32_t water_q16, DestructionEvents &events) {
    // The word as float units, for the wreck emitters' float compare and the splash places.
    const float water_height = static_cast<float>(water_q16) / io::kFp16One;
    // The update callback runs after this entity's class event on the same
    // pool visit: every tick in pool 1, the slot cohort (every 8 / 64 ticks)
    // in pools 2/3 [orig: Entity_UpdateAllEntities @0x4C22E7 / @0x4C2393].
    Entity *e = &entity;
    if (e->motor_suspended) return;
    if (tick_item_class_motion(world, entity, terrain)) return;
    if ((e->engine_flags & kEntityFlagHusk) == 0) return;
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(e->item_id);
    // The wreck-fire random crackle (S12b), one roll per masked fire-bone
    // slot per tick on World's stream C — the draw is consumed BEFORE
    // the water gate, retail's evaluation order. The sound rides the
    // fire-sound distance-delay queue at the entity position.
    // UnitType 3 calls it at the callback-specific sites below so its
    // angle draw and equal-pitch transition keep retail ordering.
    // [orig: Entity_UpdateDeadWreckEffects @ 0x493140 — the roll
    //  @ 0x4932bf, the effect @ 0x4932d1, the sound @ 0x4932e2]
    if (e->death_motion != DeathMotionMode::PiecePhysics &&
            e->death_motion != DeathMotionMode::PiecePitchSettle)
        update_dead_wreck_effects(
                world, *e, traits, water_height, events);
    if (e->death_motion == DeathMotionMode::None) return;
			if (e->death_motion == DeathMotionMode::Generic) {
				entity_process_falling_death(world, *e, terrain, water_q16);
				return;
			}

			// The post-contact callback installed by DeathPiece_PhysicsUpdate
			// [orig: DeathPiece_SettlePitch @0x48F0B0]. The forward slope target is kept
			// in the same +0xA8 register that previously held pitch rate.
			// It moves at most two degrees per tick, snaps inside four, and
			// performs the ground-death transition on the following tick.
			if (e->death_motion == DeathMotionMode::PiecePitchSettle) {
        seed_piece_physics_angles(*e);
        Entity::VehicleMotorState &motion = e->veh;
        const int32_t delta = io::bam_sub(
                motion.air_pitch_bam, motion.air_pitch_rate);
        if (delta == 0) {
            e->death_motion = DeathMotionMode::Generic;
            transition_to_ground_death(*e, traits, events);
        } else if (io::bam_abs(delta) < kPiecePitchSnapThresholdBam) {
            motion.air_pitch_bam = motion.air_pitch_rate;
            update_dead_wreck_effects(
                    world, *e, traits, water_height, events);
        } else {
            const int32_t step =
                    std::clamp(delta, -kPiecePitchStepBam,
                               kPiecePitchStepBam);
            motion.air_pitch_bam =
                    io::bam_sub(motion.air_pitch_bam, step);
            update_dead_wreck_effects(
                    world, *e, traits, water_height, events);
        }
        publish_piece_physics_angles(*e);
        return;
    }

    // The unitType-3 main-entity mover [orig:
    // DeathPiece_PhysicsUpdate @0x48F500]. This is intentionally its
    // own callback: water drag, angle integration, short slope probes,
    // landing pose, presentation, and blast routing are all distinct.
    if (e->death_motion == DeathMotionMode::PiecePhysics) {
        seed_piece_physics_angles(*e);
        Entity::VehicleMotorState &motion = e->veh;
        int32_t x_q16 = e->position.x.q16;
        int32_t y_q16 = e->position.y.q16;
        int32_t z_q16 = e->position.z.q16;
        if (z_q16 >= water_q16) {
            motion.slide_z = io::bam_sub(
                    motion.slide_z, kPiecePhysicsGravityQ16);
            if ((death_rand16(world) & 1u) != 0) {
                motion.air_pitch_bam = io::bam_add(
                        motion.air_pitch_bam, motion.air_pitch_rate);
                motion.air_roll_bam = io::bam_add(
                        motion.air_roll_bam, motion.air_roll_rate);
            } else {
                motion.yaw_bam = io::bam_add(
                        motion.yaw_bam, kPiecePhysicsYawStepBam);
            }
        } else {
            if (io::bam_sub(z_q16, motion.slide_z) >= water_q16) {
                motion.air_pitch_rate = 0;
                motion.air_roll_rate = 0;
                events.effects.push_back(DestructionEffectEvent{
                        "Effect_MedSplash",
                        Vec3{e->position.x, e->position.y, water_height},
                        Vec3{0.0f, 0.0f, 1.0f}});
                // The item's water-impact slot, else the helo-water
                // fallback, both at the piece [orig: itemDef +0x268 / the
                // +0x864 page's +0x9C @0x48F552..0x48F56E; fallback
                // dword_24E08FC @0x48F57C; the play @0x48F588 at entity+4].
                wreck_impact_sound(world, *e, traits, 39, "EXPLO_HELO_WATER", e->position, events);
            }
            motion.vel_x = io::bam_sar(motion.vel_x, 1);
            motion.vel_y = io::bam_sar(motion.vel_y, 1);
            if (motion.slide_z < kPiecePhysicsWaterFallFloorQ16)
                motion.slide_z = kPiecePhysicsWaterFallFloorQ16;
        }

        x_q16 = io::bam_add(x_q16, motion.vel_x);
        y_q16 = io::bam_add(y_q16, motion.vel_y);
        z_q16 = io::bam_add(z_q16, motion.slide_z);
        e->position.set_q16({x_q16, y_q16, z_q16});
        update_dead_wreck_effects(
                world, *e, traits, water_height, events);
        const PiecePhysicsSlope slope = piece_physics_slope(
                world, *e, terrain, x_q16, y_q16, z_q16);
        if (z_q16 > water_q16 && z_q16 < slope.ground_q16) {
            e->death_motion = DeathMotionMode::PiecePitchSettle;
            z_q16 = io::bam_sub(
                    slope.ground_q16, kPiecePhysicsLandingOffsetQ16);
            e->position.z.q16 = z_q16;
            motion.air_pitch_rate = slope.forward_bam;
            events.effects.push_back(DestructionEffectEvent{
                    "Effect_HeloGroundHit", e->position,
                    Vec3{0.0f, 0.0f, 1.0f}});
            world.out.terrain_scorches.emit_standard(
                    x_q16, y_q16, 7, world.logic_tick);
            if (world.rules.logic_authority) {
                // The def's kz, else the bound word in whole units, a signed
                // divide truncating toward zero. [orig: DeathPiece_PhysicsUpdate
                //  @0x48F500 - the def +408 test @0x48F6BA, `mov eax, [esi]; cdq;
                //  and edx, 0FFFFh; add eax, edx; sar eax, 10h` @0x48F6CF..0x48F6DA]
                const float radius =
                        traits != nullptr && traits->kz != 0.0f
                        ? traits->kz
                        : static_cast<float>(e->bound_radius.q16 / 65536);
                queue_named_landing_blast(
                        world, *e, kAmmoKzOrganicBlast, radius);
                queue_named_landing_blast(
                        world, *e, kAmmoKzMItemBlast, radius);
            }
            // The item's landing slot, else the large-vehicle fallback, at
            // the snapped pose the effect and both blasts use [orig: itemDef
            // +0x268 / the +0x864 page's +0x8C @0x48F726..0x48F745; fallback
            // dword_24E08F8 @0x48F750; the play @0x48F759 at edi, entity+4
            // @0x48F635].
            wreck_impact_sound(world, *e, traits, 35, "EXPLO_VEHCL_LG", e->position, events);
        }
        e->engine_flags &= ~kEntityFlagMatrixBuilt;
        publish_piece_physics_angles(*e);
        return;
    }
    // The routed falling and the static callbacks stamp savedLivePose (+0x80..+0x94: the
    // Position words and the Yaw / Pitch / Roll words) at their head, ahead of any move, so a
    // reader of the wreck's displacement (a body standing on it, its run-over push) takes this
    // tick's alone (D-NET-897: the dead 50cal gunner on the 03TR hull).
    // [orig: Entity_UpdateFallingDeathPhysics @0x493F70 - the stores @0x493FAA..0x493FD9;
    //  Entity_UpdateStaticDeathPhysics @0x494230 - the stores @0x494279..0x4942A0]
    if (e->death_motion == DeathMotionMode::Falling || e->death_motion == DeathMotionMode::Static)
        stamp_saved_live_pose(*e);
    if (e->death_motion != DeathMotionMode::Static &&
        e->veh.slide_z == 0 && e->veh.vel_x == 0 && e->veh.vel_y == 0)
        return;
    if (e->death_motion == DeathMotionMode::Generic &&
        traits != nullptr && traits->static_death) {
        e->veh.slide_z = 0;
        e->veh.vel_y = 0;
        e->veh.vel_x = 0;
        return;
    }
    const bool routed_falling = e->death_motion == DeathMotionMode::Falling;
    const bool static_motion = e->death_motion == DeathMotionMode::Static;
    if (routed_falling) e->engine_flags &= ~kEntityFlagMatrixBuilt;
    const EntityPosition old_position = e->position;
    if (static_motion) {
        // Entity_UpdateStaticDeathPhysics @ 0x494230 samples the current
        // ground at the Position words before movement and clears the
        // building bit; the water term, the landing line and the snap are
        // words (D-NET-507). No terrain samples as an empty cell, 0.
        // [orig: Terrain_SampleHeightBilinear @0x494250; water - ground
        //  against 655360 @0x4942B2 / 0 @0x4942C0, Z = ground @0x4942C3;
        //  the landing compare @0x4943CC / @0x4943D1, Z = ground @0x4943D7]
        const int32_t ground = terrain != nullptr && terrain->valid()
                ? terrain::height_field_height_bilinear_fixed(
                        *terrain, e->position.x.q16, e->position.y.q16)
                : 0;
        e->engine_flags &= ~kEntityFlagMatrixBuilt;
        const int32_t water_above_ground = io::bam_sub(water_q16, ground);
        if (water_above_ground <= 655360 && water_above_ground < 0) {
            e->position.z.q16 = ground;
            e->death_motion = DeathMotionMode::Generic;
            // [orig: Entity_UpdateStaticDeathPhysics call @ 0x4942c6]
            transition_to_ground_death(*e, traits, events);
        }
        const int32_t landing_line = water_above_ground > 655360
                ? io::bam_sub(ground, 1638400)
                : ground;

        // The callback then advances (the velocity words added to the
        // position words, D-NET-507), truncates the float 0.97 damp
        // toward zero, zeroes raw components under 8, and applies
        // half-gravity only once horizontal motion stops.
        // [orig: Entity_UpdateStaticDeathPhysics @0x494230 - the adds
        //  @0x4942D4 / @0x4942EA / @0x4942F9]
        e->position.x.q16 = io::bam_add(e->position.x.q16, e->veh.vel_x);
        e->position.y.q16 = io::bam_add(e->position.y.q16, e->veh.vel_y);
        e->position.z.q16 = io::bam_add(e->position.z.q16, e->veh.slide_z);
        e->veh.vel_x = static_cast<int32_t>(
                static_cast<float>(e->veh.vel_x) * 0.9700000286102295f);
        e->veh.vel_y = static_cast<int32_t>(
                static_cast<float>(e->veh.vel_y) * 0.9700000286102295f);
        if (e->veh.vel_x > -8 && e->veh.vel_x < 8) e->veh.vel_x = 0;
        if (e->veh.vel_y > -8 && e->veh.vel_y < 8) e->veh.vel_y = 0;
        if (e->veh.vel_x == 0 && e->veh.vel_y == 0) {
            e->veh.slide_z -= 167;
            if (e->position.z.q16 < landing_line) {
                e->position.z.q16 = ground;
                e->death_motion = DeathMotionMode::Generic;
                // [orig: Entity_UpdateStaticDeathPhysics call @ 0x4943da]
                transition_to_ground_death(*e, traits, events);
            }
        }
        return;
    }
    // Gravity [orig: -334/tick above water; below water the horizontal
    // halves per tick and the fall pins at -4096
    // @ 0x493fe5/@ 0x461ddf].
    // The top word against the water word, a signed compare.
    // [orig: Entity_UpdateFallingDeathPhysics @0x493F70 - `mov eax, [esi];
    //  add eax, [esi+0Ch]` @0x493FCB..0x493FCD, `cmp eax, g_EnvWaterHeightFixed;
    //  jge` @0x493FDF..0x493FE5]
    if (entity_top_q16(*e) >= water_q16) {
        e->veh.slide_z -= 334;
    } else {
        e->veh.vel_x >>= 1;
        e->veh.vel_y >>= 1;
        e->veh.slide_z = -4096;
    }
    // The ground is a word (D-NET-507); INT32_MIN stands for no ground.
    int32_t ground = INT32_MIN;
			e->ground_target = {};
			// The routed wreck queries posed models as well as terrain.
			// [orig: Entity_UpdateFallingDeathPhysics @0x493FF1..0x494001]
			if (world.ai.collision != nullptr) {
				const int32_t pos[3] = { e->position.x.q16, e->position.y.q16,
					e->position.z.q16 };
				const int32_t height = world.ai.collision->raycast_ground(
						world, e->handle, pos, 0, 0, 0x10000, 0x200000, &e->ground_target);
				ground = height;
			} else if (terrain != nullptr && terrain->valid())
				ground = terrain::height_field_height_bilinear_fixed(
						*terrain, e->position.x.q16, e->position.y.q16);
			// The wreck rests on its husk's collision section 0: the ground less
    // |z min| upright, plus |z max| inverted, by the sign of the head matrix's
    // up row (this leg never turns the wreck, so the live Euler words are the
    // head's). The ground above is the object-aware probe (terrain plus posed
    // models, retail's mask 0x200000 @ 0x461e1a).
    // [orig: Entity_UpdateFallingDeathPhysics @0x493F70 - the up row
    //  @0x493F8E..0x493F9D, its sign test @0x49402F..0x494034, +0x58
    //  @0x494036..0x494047, +0x54 @0x49404D..0x49405E]
    if (traits != nullptr && ground != INT32_MIN) {
        if (entity_placement_matrix(*e).m[10] >= 0)
            ground = io::bam_sub(ground, io::bam_abs(traits->husk_rest_z_lo_q16));
        else
            ground = io::bam_add(ground, io::bam_abs(traits->husk_rest_z_hi_q16));
    }
    const int32_t old_top_q16 = entity_top_q16(*e);
    // The pose advances by the velocity words, word arithmetic (D-NET-507).
    // [orig: Entity_UpdateFallingDeathPhysics @0x493F70 - @0x494077..0x49407D,
    //  the commit @0x49421C..0x494221; Entity_ProcessFallingDeathPhysics @0x461D30 -
    //  @0x461E5C / @0x461E64, the commit @0x461EAA..0x461EAF]
    const int32_t new_x_q16 = io::bam_add(e->position.x.q16, e->veh.vel_x);
    const int32_t new_y_q16 = io::bam_add(e->position.y.q16, e->veh.vel_y);
    const int32_t new_z_q16 = io::bam_add(e->position.z.q16, e->veh.slide_z);
    const float new_z = io::fp16_16_to_float(new_z_q16);
    const float new_x = io::fp16_16_to_float(new_x_q16);
    const float new_y = io::fp16_16_to_float(new_y_q16);
    if (!routed_falling) {
        e->position.x.q16 = new_x_q16;
        e->position.y.q16 = new_y_q16;
    }
			// No per-tick horizontal damp: the falling legs keep velocity until
			// water or ground [orig: 0x461d30/0x493f70 — the 0.97 damp belongs
			// to the separate static-death branch above @ 0x4942f7].
			// The water-crossing splash [orig: @ 0x49409f-0x494100 — the def
			// water-impact sound slot (+156), with the fallback when empty;
			// dword_2C25C64 resolves to Effect_MedSplash].
			// The new top below the water word and the old top above it, word
			// adds and signed compares. [orig: @0x494086..0x49409F - `lea edx,
			//  [eax+ebx]; cmp edx, ecx; jge`, `mov edx, [esi+0Ch]; add edx, eax;
			//  cmp edx, ecx; jle`]
			if (routed_falling && io::bam_add(new_z_q16, e->bound_radius.q16) < water_q16 &&
					old_top_q16 > water_q16) {
				events.effects.push_back(DestructionEffectEvent{
                "Effect_MedSplash", Vec3{new_x, new_y, water_height},
                Vec3{0.0f, 0.0f, 1.0f}});
				wreck_impact_sound(world, *e, traits, 39, "IMP_DEBLRG_WATER",
						Vec3{ new_x, new_y, water_height }, events);
			}
			if (ground != INT32_MIN && new_z_q16 <= ground) {
        // Ground contact [orig: Entity_TransitionToGroundDeath
        // @ 0x493080 + the landing legs @ 0x494113-0x494209]. The
        // generic leg restores the pre-move pose and clears vertical
        // motion. The routed leg transitions state here but its common
        // tail still commits the integrated pose and retains velocity.
        if (routed_falling) {
            e->position.z.q16 = ground;
            e->death_motion = DeathMotionMode::Generic;
            // [orig: Entity_UpdateFallingDeathPhysics call @ 0x494113]
            transition_to_ground_death(*e, traits, events);
					// A routed wreck marks bare terrain before its landing
					// sound and authority blast. Retail suppresses this call
					// when the ground trace returned another entity.
					// The router reads the entity's pre-tail x/y, not the
					// integrated pose committed at @0x49421C.
					// [orig: ground-entity test @0x49414E; scorch 7 call
					// @0x49416B..0x494179]
					if (!e->ground_target.valid())
						world.out.terrain_scorches.emit_standard(e->position.x.q16,
								e->position.y.q16, 7, world.logic_tick);
				} else {
					e->position = old_position;
            e->veh.slide_z = 0;
				}
				// The landing clunk + kz blast ride the unitType-routed falling
				// leg only [orig: 0x493f70 — the sound @ 0x4941af (the def
				// landing slot +140, with the fallback when empty —
				// world-wac-ai-re.md D-ITEM-10) and the authority kz
				// @ 0x4941be, r = def kz ?: boundRadius]; generic items
				// (0x461d30) land silently.
				if (routed_falling) {
					wreck_impact_sound(world, *e, traits, 35, "IMP_VCL_DROP", e->position, events);
					if (world.rules.logic_authority) {
						// The def's kz, else the bound word in whole units,
						// truncated toward zero. [orig: Entity_UpdateFallingDeathPhysics
						//  - the def +0x198 test @0x4941C7..0x4941D4, the word's
						//  `cdq` / `sar eax, 10h` @0x4941DE..0x4941E9]
						const float radius =
                        (traits != nullptr && traits->kz != 0.0f)
                        ? traits->kz
                        : float(e->bound_radius.q16 / 65536);
                queue_named_landing_blast(
                        world, *e, kAmmoKzOrganicBlast, radius);
					}
				}
    } else if (!routed_falling) {
        e->position.z.q16 = new_z_q16;
    }
    if (routed_falling)
        e->position.set_q16({new_x_q16, new_y_q16, new_z_q16});
}

// ----------------------------------------------------------------------------
// DeathPieceSim
// ----------------------------------------------------------------------------

DeathPiece &DeathPieceSim::alloc() {
    // [orig: DeathPiece_AllocSlot @ 0x57b4f0 — a plain ring; the next slot is
    // reused even if still live.] The reused slot's trail emitter is destroyed
    // outright, its particles with it, before the record is zeroed; the
    // presenter destroys the slot's trail group when it meets the new
    // generation [orig: the +0x8C test @0x57B51D..0x57B525,
    // Effect_DestroyEmitterIfLive @0x57B528, the memset @0x57B53A..0x57B541].
    DeathPiece &p = pieces[static_cast<size_t>(cursor)];
    cursor = (cursor + 1) % kCapacity;
    uint64_t generation = p.generation + 1;
    if (generation == 0) generation = 1; // reserve 0 for never allocated
    p = DeathPiece{};
    p.generation = generation;
    return p;
}

void DeathPieceSim::tick(World &world, const terrain::TerrainHeightField *terrain,
                         int32_t water, DestructionEvents &events) {
    // [orig: DeathPiece_TickAll @ 0x57b900 -> Entity_ProcessDeathPiecePhysics
    // @ 0x492dd0] The piece is a row on words (D-NET-507).
    for (DeathPiece &p : pieces) {
        if (!p.active || p.settled) continue;
        const DeathPieceType &tp = death_piece_type(p.type_index);
        // Gravity or the underwater sink, keyed on the PRE-move Z word: above
        // the water word vz += -334; below it the horizontal words halve
        // (`sar 1`) and the fall pins at -4096; then Position += velocity and
        // Yaw / Pitch += the spin words. [orig: Entity_ApplyGravitySimple
        // @0x492D80 - `cmp ecx, g_EnvWaterHeightFixed; jge` @0x492D84..0x492D8D,
        // @0x492D8F..0x492D95, `add [eax+24h], 0FFFFFEB2h` @0x492D9E, the adds
        // @0x492DA5..0x492DC2]
        if (p.pos.z.q16 < water) {
            p.vel[0] = io::bam_sar(p.vel[0], 1);
            p.vel[1] = io::bam_sar(p.vel[1], 1);
            p.vel[2] = -4096;
        } else {
            p.vel[2] = io::bam_add(p.vel[2], -334);
        }
        p.pos.set_q16({io::bam_add(p.pos.x.q16, p.vel[0]), io::bam_add(p.pos.y.q16, p.vel[1]),
                       io::bam_add(p.pos.z.q16, p.vel[2])});
        p.heading = io::bam_add(p.heading, p.spin_a);
        p.pitch = io::bam_add(p.pitch, p.spin_b);
        // The ground word under the moved X / Y words plus the 1024 rest offset
        // [orig: Terrain_SampleHeightBilinear @0x492DFA, `add edi, 400h`
        // @0x492E0D].
        // A world with no terrain bound (a bare rig) has no ground.
        const int32_t ground = terrain != nullptr && terrain->valid()
                ? io::bam_add(terrain::height_field_height_bilinear_fixed(
                          *terrain, p.pos.x.q16, p.pos.y.q16), 0x400)
                : INT32_MIN;
        const int32_t z = p.pos.z.q16;
        if (z < water) {
            // Water: a splash where the move crossed the plane (the pre-move Z
            // strictly above it), at the X / Y words and the water word; freed
            // at or under the ground word [orig: `sub eax, [esi+24h]; cmp eax,
            // ecx; jle` @0x492E1B..0x492E20, the point @0x492E45..0x492E58, the
            // free `cmp [esi+0Ch], edi; jg` @0x492E90..0x492E93].
            if (io::bam_sub(z, p.vel[2]) > water) {
                const EntityPosition at = EntityPosition::of_q16({p.pos.x.q16, p.pos.y.q16, water});
                if (tp.splash_fx != nullptr)
                    events.effects.push_back(
                            DestructionEffectEvent{tp.splash_fx, at, Vec3{}, 0, 0});
                if (tp.splash_snd != nullptr)
                    events.sounds.push_back(DestructionSoundEvent{tp.splash_snd, at});
            }
            if (z <= ground) p.active = false;
            continue;
        }
        if (z >= ground) continue; // still airborne [orig: `cmp eax, edi; jge` @0x492ECE]
        // Ground contact.
        if (p.bounces_left > 0) {
            // The spin words halve (`sar 1`), the horizontal words take 0.95
            // truncated, the vertical the type's bounce factor negated, the Z
            // word lands on the ground word; then dust, and the bounce sound when
            // the velocity words' saturated x87 length passes 0x5000 [orig:
            // @0x492ED6..0x492F22, `cmp eax, 5000h; jle` @0x492F87..0x492F8C].
            --p.bounces_left;
            p.spin_a = io::bam_sar(p.spin_a, 1);
            p.spin_b = io::bam_sar(p.spin_b, 1);
            constexpr double kBounceKeep = static_cast<double>(0.95f); // flt_7C6FB8
            p.vel[0] = io::retail_ftol_sse2(static_cast<double>(p.vel[0]) * kBounceKeep);
            p.vel[1] = io::retail_ftol_sse2(kBounceKeep * static_cast<double>(p.vel[1]));
            p.vel[2] = static_cast<int32_t>(0u - static_cast<uint32_t>(io::retail_ftol_sse2(
                    static_cast<double>(p.vel[2]) * static_cast<double>(tp.bounce))));
            p.pos.z.q16 = ground;
            if (tp.bounce_fx != nullptr)
                events.effects.push_back(
                        DestructionEffectEvent{tp.bounce_fx, p.pos, Vec3{}, 0, 0});
            // The length sums (vz^2 + vy^2) + vx^2 [orig: the filds and squares
            // @0x492F49..0x492F69].
            if (x87_length(p.vel[2], p.vel[1], p.vel[0]) > 0x5000 && tp.bounce_snd != nullptr)
                events.sounds.push_back(DestructionSoundEvent{tp.bounce_snd, p.pos});
        } else {
            // Exhausted [orig: the bounce count's `jle` @0x492EDB ->
            // @0x492FBC..0x493048]: release the trail, add a
            // permanent scorch at the X / Y words unless the debris row carries
            // bit 1 (CACTUS_), then the final sound and effect and persist (bit
            // 0: the spin and velocity words and the update callback zeroed) or
            // free. The Z word stays where the move left it (no landing store on
            // this arm). Scorch texture selection advances the shared CRT before
            // the presentation events below. [orig: flag test @0x492FD7; scorch 7
            // call @0x492FE7; the persist stores @0x493039..0x493048]
            if ((p.flags & 0x2u) == 0) {
                world.out.terrain_scorches.emit_standard(
                        p.pos.x.q16, p.pos.y.q16, 7, world.logic_tick);
            }
            if (tp.final_fx != nullptr)
                events.effects.push_back(
                        DestructionEffectEvent{tp.final_fx, p.pos, Vec3{}, 0, 0});
            if (tp.final_snd != nullptr)
                events.sounds.push_back(DestructionSoundEvent{tp.final_snd, p.pos});
            if ((p.flags & 0x1u) != 0) {
                p.settled = true; // stays visible where it landed
                p.vel[0] = p.vel[1] = p.vel[2] = 0;
                p.spin_a = p.spin_b = 0;
            } else {
                p.active = false; // [orig: the memset free @ 0x493029]
            }
        }
    }
}

void DeathPieceSim::reset() noexcept {
    // Clear retail state without reusing a presentation identity if the same
    // host presenter survives a world restart.
    for (DeathPiece &p : pieces) {
        const uint64_t generation = p.generation;
        p = DeathPiece{};
        p.generation = generation;
    }
    cursor = 0;
}

// The debris-type trail-effect column — the SAME kDeathPieceTypes rows the
// piece spawner reads (one table, one impl); nullptr rows read as ""
// [orig: g_DeathPieceTypes @ 0x8404f0 +0x2C].
const char *death_piece_trail_effect(uint8_t type_index) {
    if (type_index >= kDeathPieceTypeCount) return "";
    const char *trail = kDeathPieceTypes[type_index].trail_fx;
    return trail != nullptr ? trail : "";
}

} // namespace opennova::world
