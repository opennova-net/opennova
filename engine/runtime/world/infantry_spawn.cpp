// Organic spawn reset and quota-driven corpse lifecycle.
// [orig: Entity_ResetToSpawnState @0x4B9610;
//        Entity_UpdateInfantryAI @0x4B9E4D..0x4BA071]
#include <runtime/world/entity_spawn.h>

#include <algorithm>
#include <cmath>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>
#include <base/io/fixed.h>
#include <base/io/bam.h>

namespace opennova::world {

namespace {

void clear_target(World &world, AiSystem &ai, AiEntity &body) {
    ai.ai_set_target(world, body, {});
    body.slot.f[3] = 0;
    body.inf.combat_target = {};
}

bool spawn_zone_ready(const World &world, const Entity &entity) {
    if (!entity.npc_respawn_zone.valid()) return true;
    const Entity *zone = world.registry.get(entity.npc_respawn_zone);
    return zone != nullptr && zone->team == entity.team && zone->zone_control >= 0x10000;
}

bool player_watches(World &world, AiSystem &ai, const Entity &entity, const Vec3 &position) {
    if (ai.is_in_session || world.rules.mp_session) return false;
    const Entity *player = world.registry.get(world.cached.local_player);
    if (player == nullptr) return false;
    const int32_t from[3] = {to_fixed(position.x), to_fixed(position.y), to_fixed(position.z)};
    const int32_t to[3] = {to_fixed(player->position.x), to_fixed(player->position.y),
                           to_fixed(player->position.z)};
    // Retail uses the entity origins, including for the spawn-position watch.
    return ai.line_of_sight_clear(world, from, to, entity.handle, player->handle);
}

void release_corpse_effect(World &world, Entity &entity) {
    if (entity.death_effect_active[0] != 0)
        release_death_effect_bank(entity, 1, world.out.destruction);
}

} // namespace

// Initial org1 callback, after definition/ADM/collision resources are bound.
// Respawn uses the separate 12-step callback below. [orig: @0x4BFCC0]
void initialize_organic_ai(World &world, Entity &entity) {
    AiSystem &ai = world.ai;
    AiEntity *body = ai.for_handle(entity.handle);
    if (body == nullptr || !body->inf.active || entity.handle.pool() != 0 ||
            body->inf.is_local_player || body->net_is_remote_peer ||
            ((entity.flags | entity.engine_flags) & kEntityFlagPlayer) != 0) return;
    auto &inf = body->inf;
    const uint32_t flags = entity.flags | entity.engine_flags;
    entity.spawn_position = entity.position;
    entity.spawn_heading = body->heading;
    entity.spawn_flags = flags;
    entity.npc_respawns = static_cast<int16_t>(body->slot.f[18] / 62);
    inf.body_heading = inf.target_heading = inf.aim_heading = body->heading;
    inf.leg_yaw[0] = inf.leg_yaw[1] = body->heading;
    inf.leg_target[0] = inf.leg_target[1] = body->heading;
    entity.saved_live_yaw = body->heading;
    entity.saved_live_pitch = entity.saved_live_roll = 0;
    body->pitch = body->body_pitch = body->roll = 0;
    entity.pitch = entity.roll = 0;
    inf.torso_roll = inf.lean_angle = 0;
    inf.arms_dip_ticks = inf.reload_anim_ticks = 0;
    inf.head_look_target = inf.last_look_target = inf.previous_look_target = {};
    entity.dragger = {};
    entity.dragger_spawn_id = 0;
    body->collide_state.skip_counter = 0;
    inf.move_target[2] = body->pos[2];
    inf.magazine = static_cast<int16_t>(body->profile.clip_size);

    const auto available = [&](int state) {
        return ai.root_motion != nullptr && ai.root_motion->has_clip(inf.adm_id, state);
    };
    int state = body->slot.f[35] != 0 ? 1 : 43;
    if ((flags & 0x200u) != 0 && available(76)) state = 76;
    if ((flags & 0x40u) != 0 && available(140)) state = 140;
    if (body->slot.f[37] == 126) state = 44;
    if (body->slot.f[37] == 127) state = 43;
    if (world.rotor_wash.nearby_zone(body->pos, 983040) != 0) {
        if (state == 1 && available(28)) state = 28;
        if ((state == 148 || state == 149) && available(29)) state = 29;
        if ((state == 43 || state == 44) && available(27)) state = 27;
    }
    if (const Entity *parent = world.registry.get(entity.mount_target)) {
        entity.ground_target = parent->ground_target;
        state = 67;
        if (parent->has_item_def && parent->emplaced_config >= 1 &&
                parent->emplaced_config <= 8 && available(67 + parent->emplaced_config))
            state = 67 + parent->emplaced_config;
    }
    // Registration has already started both channels on the ADM reset clip.
    // The first update transitions from that clip with the normal blend.
    // [orig: AnimMap_RegisterEntity @0x40BB60 -> AnimMap_UpdateEntity @0x40B5F0]
    inf.reset_body_animation(0);
    inf.begin_body_transition(state);
    inf.reset_weapon_animation(0);
    inf.begin_weapon_transition(43, ai.root_motion != nullptr
            ? ai.root_motion->variant_count(inf.adm_id, 43) : 1);

    // Entity_WarmUpOrganicAnimation @0x4B8B20: secondary then primary,
    // net-ID permutation, vertical root motion only, one final ground solve.
    if (ai.root_motion != nullptr && inf.adm_id >= 0) {
        RootMotionFrame frame{};
        const auto advance = [&] {
            ai.infantry_weapon_channel_advance(*body);
            if (reset_capsule_bottom_state(inf.anim_state)) inf.prev_capsule_bottom = 0;
            if (advance_primary_channel(inf, *ai.root_motion, frame)) {
                if (inf.prev_capsule_bottom != 0)
                    frame.dz = io::bam_sub(frame.capsule_bottom, inf.prev_capsule_bottom);
                inf.prev_capsule_bottom = frame.capsule_bottom;
            }
        };
        const uint32_t id = uint32_t(body->net_id);
        const uint32_t steps = 8 * ((id & 12u) + 8 * ((id & 2u) + 4 * (id & 1u))) + 10;
        for (uint32_t i = 0; i < steps; ++i) {
            advance();
            if (!entity.mount_target.valid()) body->pos[2] = io::bam_add(body->pos[2], frame.dz);
        }
        if (!entity.mount_target.valid()) {
            body->pos[2] = io::bam_add(body->pos[2], frame.capsule_bottom);
            advance();
            body->pos[2] = io::bam_add(body->pos[2], frame.dz);
            body->collide_state.skip_counter = 0;
            if (ai.collision != nullptr) {
                const int32_t clearance = ai.collision->resolve_entity(
                        world, entity.handle, body->collide_state, body->pos,
                        inf.vel, inf.vel[2], frame.capsule_bottom, frame.capsule_top,
                        body->heading, 0, false, ai.is_authority, world.logic_tick,
                        inf.anim_state, infantry_anim_flags(inf.anim_state), body->health);
                if (clearance < 65536) body->pos[2] = io::bam_sub(body->pos[2], clearance);
            }
            body->collide_state.skip_counter = 0;
        }
    }
    entity.position = {body->pos[0] / 65536.0f, body->pos[1] / 65536.0f,
                         body->pos[2] / 65536.0f};
    // The source is savedLivePose, deliberately not the grounded position.
    std::copy_n(body->net_saved_live_pose, 3, inf.aim_point);
    const double radians = double(body->heading) * io::kRadiansPerBam;
    inf.aim_point[0] = io::bam_add(inf.aim_point[0],
            int32_t((int64_t(196608) * int32_t(std::cos(radians) * io::kQ22One)) >> 22));
    inf.aim_point[1] = io::bam_add(inf.aim_point[1],
            int32_t((int64_t(196608) * int32_t(std::sin(radians) * io::kQ22One)) >> 22));
    entity.npc_respawn_zone = {};
    // First matching row IN EACH pool; a pool-2 match replaces pool 1.
    for (int pool : {1, 2}) {
        EntityHandle found;
        world.registry.for_each_in_pool(pool, [&](const Entity &zone) {
            if (!found.valid() && zone.item_id != 0 && zone.has_item_def &&
                    (zone.item_attrib & kItemAttribSpawnPoint) != 0 &&
                    uint16_t(zone.group_id) != 0 &&
                    uint16_t(zone.group_id) == uint16_t(entity.group_id))
                found = zone.handle;
        });
        if (found.valid()) entity.npc_respawn_zone = found;
    }
    if (const Entity *zone = world.registry.get(entity.npc_respawn_zone)) {
        if (zone->team != entity.team) {
            entity.flags |= 1u;
            entity.engine_flags |= 1u;
            entity.hidden = true;
        }
    }
    ai.mirror_wire_anim(*body, world);
}

void entity_reset_to_spawn_state(World &world, AiSystem &ai, Entity &entity) {
    const Vec3 origin = entity.position;
    const int32_t heading = bam_heading_from_mission_yaw_deg(entity.yaw);
    const uint32_t flags = entity.flags | entity.engine_flags;
    AiEntity *body = ai.for_handle(entity.handle);
    if (body != nullptr) {
        for (int axis = 0; axis < 3; ++axis) {
            const float coordinate = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
            body->pos[axis] = to_fixed(coordinate);
            body->net_saved_live_pose[axis] = body->pos[axis];
            body->inf.vel[axis] = 0;
        }
        body->heading = heading;
        body->vel_x = body->vel_z = 0;
        body->body_pitch = body->roll = 0;
        auto &inf = body->inf;
        inf.body_heading = inf.target_heading = inf.aim_heading = heading;
        inf.leg_yaw[0] = inf.leg_yaw[1] = heading;
        inf.leg_target[0] = inf.leg_target[1] = heading;
        inf.aim_valid = false;
        inf.was_hit = false;
        inf.last_attacker = {};
        inf.ai_focus = {};
        inf.damage_timer = 0;
        body->slot.f[38] = ai.nearest_route_node(*body, uint32_t(body->slot.f[37]));
        inf.anim_pending = 0;

        // Dead-flag callers retain their current body. A respawn restores the
        // saved live flags BEFORE entering here, so it takes the spawn-pose leg.
        if ((flags & kEntityFlagDead) == 0) {
            const int state = ai.root_motion != nullptr &&
                    ai.root_motion->has_clip(inf.adm_id, 153) ? 153 : 44;
            inf.begin_body_transition(state);
            inf.begin_weapon_transition(state);
            RootMotionFrame frame{};
            for (int i = 0; i < 12; ++i) {
                ai.infantry_weapon_channel_advance(*body);
                if (ai.root_motion != nullptr)
                    advance_primary_channel(inf, *ai.root_motion, frame);
            }
            if (ai.collision != nullptr) {
                const int32_t clearance = ai.collision->resolve_entity(
                        world, entity.handle, body->collide_state, body->pos,
                        inf.vel, inf.vel[2], frame.capsule_bottom, frame.capsule_top,
                        heading, 0, (flags & kEntityFlagPlayer) != 0,
                        ai.is_authority, world.logic_tick, inf.anim_state,
                        infantry_anim_flags(inf.anim_state), body->health);
                if (clearance <= 0) body->pos[2] -= clearance;
            }
            entity.position = {body->pos[0] / 65536.0f, body->pos[1] / 65536.0f,
                                 body->pos[2] / 65536.0f};
            if (const auto *traits = world.tables.item_death_traits.get(entity.item_id)) {
                if (!traits->particlespawn.empty()) {
                    DestructionEffectEvent effect;
                    effect.effect = traits->particlespawn;
                    effect.pos = entity.position;
                    world.out.destruction.effects.push_back(std::move(effect));
                }
            }
        }
        inf.magazine = static_cast<int16_t>(body->profile.clip_size);
        clear_target(world, ai, *body);
    }

    entity_reset_to_spawn_state(entity);
    // The backup precedes collision correction in the original reset.
    entity.spawn_position = origin;
    entity.spawn_heading = heading;
    entity.spawn_flags = flags & ~kEntityFlagDead;
    release_corpse_effect(world, entity);
    if (body != nullptr) {
        body->health = static_cast<int16_t>(entity.health);
        entity.net_anim_state = static_cast<uint8_t>(body->inf.anim_state);
        entity.net_anim_pending = static_cast<uint8_t>(body->inf.anim_pending);
    }

    if (ai.is_authority) {
        if (entity.mount_target.valid()) world.vehicles.detach(entity.handle);
        const int32_t packed = int32_t(entity.handle.packed) + 1;
        for (int pool = 0; pool < 2; ++pool) {
            world.registry.for_each_in_pool(pool, [&](const Entity &row) {
                Entity &other = *world.registry.get(row.handle);
                if (other.item_id == 0 || other.handle == entity.handle) return;
                AiEntity *other_body = ai.for_handle(other.handle);
                if (other_body == nullptr) return;
                if (pool == 0) {
                    if (other.last_attacker == entity.handle) other.last_attacker = {};
                    if (other.primary_occupant == entity.handle) other.primary_occupant = {};
                    auto &inf = other_body->inf;
                    if (inf.last_attacker == entity.handle) inf.last_attacker = {};
                    if (inf.aim_ref0 == entity.handle) inf.aim_ref0 = {};
                    if (inf.head_look_target == entity.handle) inf.head_look_target = {};
                    if (inf.ai_focus == entity.handle) {
                        inf.ai_focus = {};
                        std::fill_n(inf.aim_point, 3, 0);
                    }
                }
                if (other_body->slot.f[3] == packed)
                    clear_target(world, ai, *other_body);
            });
        }
    }
    if (ai.collision != nullptr) ai.collision->refresh_after_registry_change(world);
}

void entity_reset_to_spawn_state(World &world, Entity &entity) {
    entity_reset_to_spawn_state(world, world.ai, entity);
}

bool npc_respawn_unhide(World &world, const AiSystem &ai, Entity &entity) {
    // [orig: org1 head @0x4B99DB] Only a linked, fully secured friendly zone
    // can lift the hidden flag here. No link does not undo a scripted hide.
    if (!ai.is_authority || !entity.npc_respawn_zone.valid() ||
            !spawn_zone_ready(world, entity)) return false;
    entity.flags &= ~1u;
    entity.engine_flags &= ~1u;
    entity.hidden = false;
    return true;
}

NpcCorpseStep step_npc_corpse(World &world, AiSystem &ai, Entity &entity) {
    const bool silent = (entity.section_mask & 1u) != 0;
    if (entity.leave_corpse && !silent) return NpcCorpseStep::Kept;
    if (entity.corpse_timer != 0)
        entity.corpse_timer = static_cast<int32_t>(uint32_t(entity.corpse_timer) - 1u);
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    const bool decay_effect = traits != nullptr && !traits->particledeath.empty();
    // [orig: the 186-tick decay spawn @0x4B9E7D; respawn quota leg @0x4B9FA0]
    if (entity.corpse_timer == 186 && !silent && decay_effect) {
        release_corpse_effect(world, entity);
        DestructionEffectEvent effect;
        effect.effect = traits->particledeath;
        effect.pos = entity.position;
        effect.attach_net_id = entity.net_id;
        effect.attach_bms_id = entity.bms_id;
        effect.attach_wire_handle = entity.handle.packed;
        effect.attach_spawn_origin = entity.spawn_origin;
        effect.family = 1;
        entity.death_effect_active[0] = 1;
        world.out.destruction.effects.push_back(std::move(effect));
    }
    if (entity.corpse_timer > 0) return NpcCorpseStep::Kept;
    if (entity.npc_respawns > 0) {
        if (!spawn_zone_ready(world, entity)) {
            entity.flags |= 1u;
            entity.engine_flags |= 1u;
            entity.hidden = true;
            return NpcCorpseStep::Kept;
        }
        if (player_watches(world, ai, entity, entity.spawn_position)) {
            entity.corpse_timer = 62;
            return NpcCorpseStep::Kept;
        }
        if (entity.npc_respawns < 100) --entity.npc_respawns;
        entity.position = entity.spawn_position;
        entity.yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(entity.spawn_heading))));
        entity.flags = entity.engine_flags = entity.spawn_flags;
        entity_reset_to_spawn_state(world, ai, entity);
        world.recount_group_live();
        return NpcCorpseStep::Respawned;
    }
    if (!decay_effect && player_watches(world, ai, entity, entity.position)) {
        entity.corpse_timer = 62;
        return NpcCorpseStep::Kept;
    }
    release_corpse_effect(world, entity);
    entity.hidden = true;
    const EntityHandle handle = entity.handle;
    world.out.entity_removals.push_back(handle.packed);
    world.registry.despawn(handle);
    ai.release(handle);
    if (ai.collision != nullptr) ai.collision->refresh_after_registry_change(world);
    return NpcCorpseStep::Removed;
}

} // namespace opennova::world
