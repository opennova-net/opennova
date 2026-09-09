#include <runtime/world/entity_commands.h>
#include <runtime/world/world.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <base/io/bam.h>

namespace opennova::world {
namespace {
FixedVec3 script_position(const World &world, const Entity &entity) {
    // The motor owns the exact Q16 pose where an organic component exists.
    if (const AiEntity *body = world.ai.for_handle(entity.handle))
        return {body->pos[0], body->pos[1], body->pos[2]};
    return {to_fixed(entity.position.x), to_fixed(entity.position.y), to_fixed(entity.position.z)};
}
} // namespace

// The target commands walk pool 3 in slot order, test ItemDef and its numeric
// id, then compare WP_NUMBER. Health, alive state and team do not participate.
// [orig: WacCmd_SoundToTarget @0x4F7F60 / ammo2tgt @0x4F8100]
const Entity *EntityCommands::script_target(int32_t number) const {
    for (size_t slot = 0; slot < world_.registry.pool_capacity(3); ++slot) {
        const Entity *marker = world_.registry.get(EntityHandle::make(3, int(slot)));
        if (marker && marker->has_item_def && marker->item_id == kParticleEffectMarkerTypeId &&
                marker->wp_number == number)
            return marker;
    }
    return nullptr;
}

// [orig: WAC ammo2tgt handler @0x4F8100]
int EntityCommands::fire_ammo_at_target(int32_t ammo, int32_t target) {
    const Entity *marker = script_target(target);
    if (!marker || ammo == 0) return 1;
    world_.round_sim.fire_source(world_, nullptr, script_position(world_, *marker),
            bam_heading_from_mission_yaw_deg(marker->yaw),
            bam_from_degrees_wrapped(marker->pitch), uint8_t(ammo));
    return 0;
}

// [orig: WAC ammoarea handler @0x4EE240]
int EntityCommands::fire_ammo_in_area(int32_t ammo, int32_t area_id) {
    if (ammo == 0) return 1;
    const int area_index = world_.registry.area_index_by_zone_id(area_id);
    if (area_index < 0 || area_index >= 128) return 1;
    const Area &area = *world_.registry.area(area_index);
    const auto random_offset = [&](int32_t range) -> int32_t {
        // This is the same 31BFBB8 stream as AI engagement jitter, NOT the
        // WAC VM stream used by ammorain/fxrain. Signed range * low-u16.
        const uint16_t random = uint16_t(world_.ai.prng_step_a());
        return int32_t(uint32_t((int64_t(range) * random + 0x8000) >> 16));
    };
    const int32_t x = to_fixed(area.script_bounds.min.x);
    const int32_t y = to_fixed(area.script_bounds.min.y);
    FixedVec3 p;
    p.x = io::bam_add(x, random_offset(io::bam_sub(to_fixed(area.script_bounds.max.x), x)));
    p.y = io::bam_add(y, random_offset(io::bam_sub(to_fixed(area.script_bounds.max.y), y)));
    p.z = io::bam_add(to_fixed(area.script_bounds.min.z), 0x1900000); // area bottom + 400u
    const int32_t rise = random_offset(0x320000); // 0..50u, third draw
    // This is the existing CFAC/terrain support query, despite the old
    // decompiler's unrelated name compute_clamped_displacement.
    CollisionWorld fallback;
    fallback.terrain = world_.ai.terrain;
    const CollisionWorld &collision = world_.collision ? *world_.collision : fallback;
    p.z = io::bam_add(collision.minefield_ground(world_, EntityHandle{}, p, false),
                     io::bam_add(0xC80000, rise)); // surface + 200u + random rise
    world_.round_sim.fire_source(world_, nullptr, p, 0, int32_t(0xC0000040u), uint8_t(ammo));
    return 0;
}

// [orig: WAC ammorain handler @0x4EE1A0] The VM draws exactly once, after
// testing the full ammo word for zero, and supplies the resulting WAC PRNG word.
int EntityCommands::rain_ammo_near_player(int32_t ammo, uint32_t random) {
    const Entity *player = world_.registry.get(world_.cached.local_player);
    if (!player || ammo == 0) return 1; // guard retail's missing-player dereference
    FixedVec3 p = script_position(world_, *player);
    p.x = io::bam_add(p.x, int32_t(uint32_t(int32_t(random << 8) >> 15) * 25u));
    p.y = io::bam_add(p.y, int32_t(uint32_t(int32_t(int16_t(random))) * 50u));
    p.z = io::bam_add(p.z, int32_t(((random & 0xFFFFFF00u) + 0x30000u) * 10u));
    world_.round_sim.fire_npc_ammo(world_, EntityHandle{}, p, 0, int32_t(0xC0000040u), uint8_t(ammo));
    return 0;
}

// [orig: WacScript_EntityFireAtTarget (ammo2ssn) @0x4F24E0]
int EntityCommands::fire_ammo_from_ssn(int32_t ammo, EntityTarget source, EntityTarget target) {
    Entity *shooter = world_.registry.get(resolve_target(source));
    if (shooter == nullptr || !shooter->has_item_def || ammo == 0 ||
        static_cast<uint16_t>(shooter->health) == 0 || world_.collision == nullptr ||
        world_.collision->entity_model_id(shooter->handle) < 0 || world_.pose_provider == nullptr)
        return 0;
    // The caller alternates MODEL userpoints 0/1, independent of the weapon's
    // resolved muzzle bytes. The callee poses their parts and returns the bone
    // Euler triple; its optional direction output is null, so the point's
    // authored direction and the command target do not redirect the shot.
    const int point = (shooter->script_fire_userpoint_counter++ & 1u) + 1;
    int32_t transform[6] = {};
    if (!world_.pose_provider->resolve_userpoint_transform(
            world_, shooter->handle, point, transform))
        return 0; // guard the retail call's unchecked missing userpoint/model data
    const Entity *resolved_target = world_.registry.get(resolve_target(target));
    shooter->last_attacker = resolved_target != nullptr ? resolved_target->handle : EntityHandle{};
    const EntityHandle occupant = shooter->primary_occupant;
    if (!occupant.valid()) shooter->primary_occupant = shooter->handle;
    world_.round_sim.fire_source(world_, shooter,
            {transform[0], transform[1], transform[2]}, transform[3], transform[4], uint8_t(ammo));
    shooter->primary_occupant = occupant;
    return 1;
}

} // namespace opennova::world
