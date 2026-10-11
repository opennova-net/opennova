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
#include <base/io/strutil.h>
#include <formats/mission/bms.h>

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

// Registration has already started both channels on the ADM reset clip; the
// init's two requests (the secondary 43, the primary `state`) re-init them on
// the first update with the normal blend, the secondary serving its ring entry
// before the primary's.
// [orig: AnimMap_RegisterEntity @0x40BB60 -> AnimMap_UpdateEntity @0x40B5F0;
//  the requests Entity_InitOrganicAI @0x4BFF8F..0x4BFF9A;
//  AnimMap_UpdateDualChannels @0x40B908 before @0x40B94E]
void begin_spawn_channels(InfantryState &inf, int state, const IRootMotionSource *source,
                          AnimVariantRings &rings) {
    inf.reset_weapon_animation(0);
    inf.begin_weapon_transition(anim_state::kIdle, rings.serve(source, inf.adm_id, anim_state::kIdle));
    inf.reset_body_animation(0);
    inf.begin_body_transition(state, -1, rings.serve(source, inf.adm_id, state));
}

// The warmup's dual updates and the height their vertical root motion adds to
// an unparented body: each update's vertical delta, then the last frame's
// capsule bottom and one more update's delta; a parented body only animates.
// `frame` holds the last update's root output (the ground solve's capsule).
// [orig: Entity_WarmUpOrganicAnimation @0x4B8B3F..0x4B8BA3 the loop and its
//  per-update `add [esi+0Ch]` @0x4B8B7E, the final bottom @0x4B8BB4, update
//  @0x4B8BC1 and delta @0x4B8BD2]
int32_t warm_up_channels(InfantryState &inf, uint32_t net_id, bool parented, IRootMotionSource &source,
                         AnimVariantRings &rings, RootMotionFrame &frame) {
    int32_t rise = 0;
    const uint32_t steps = organic_warmup_updates(net_id);
    for (uint32_t i = 0; i < steps; ++i) {
        infantry_dual_update(inf, &source, rings, frame);
        if (!parented) rise = io::bam_add(rise, frame.dz);
    }
    if (parented) return 0;
    rise = io::bam_add(rise, frame.capsule_bottom);
    infantry_dual_update(inf, &source, rings, frame);
    return io::bam_add(rise, frame.dz);
}

} // namespace

bool organic_init_class(const char *ai_function) {
    return ai_function != nullptr &&
           (strutil::iequals(ai_function, "org0") || strutil::iequals(ai_function, "org1"));
}

OrganicSpawnFacts organic_spawn_facts_from_record(bool ai_slot, int32_t waypoint_id,
                                                  uint32_t bmsi_attributes) {
    OrganicSpawnFacts facts;
    if (!ai_slot) return facts;
    facts.route = waypoint_id != 0;
    facts.route_channel = waypoint_id;
    if ((bmsi_attributes & static_cast<uint32_t>(bms::BmsiAttributeFlags::Guarding)) != 0)
        facts.flags |= kEntityFlagMounted;
    return facts;
}

int organic_spawn_state(const OrganicSpawnFacts &facts, const IRootMotionSource *source, int adm_id) {
    const auto available = [&](int state) {
        return source != nullptr && source->has_clip(adm_id, state);
    };
    // [orig: @0x4BFF8F..0x4BFFAB the idle request, a route's walk]
    int state = facts.route ? anim_state::kWalkForward : anim_state::kIdle;
    // [orig: @0x4BFFB5..0x4BFFD2 sit, @0x4BFFDC..0x4BFFEF guard]
    if ((facts.flags & 0x200u) != 0 && available(anim_state::kSit)) state = anim_state::kSit;
    if ((facts.flags & 0x40u) != 0 && available(anim_state::kGuard)) state = anim_state::kGuard;
    // [orig: @0x4BFFF9..0x4C0015 the reserved routes]
    if (facts.route_channel == 126) state = anim_state::kIdle2;
    if (facts.route_channel == 127) state = anim_state::kIdle;
    // [orig: @0x4C001B..0x4C0085 the rotor's wash]
    if (facts.rotor_wash) {
        if (state == anim_state::kWalkForward && available(anim_state::kWashWalk)) state = anim_state::kWashWalk;
        if ((state == anim_state::kJogForward || state == anim_state::kRunForward) &&
                available(anim_state::kWashRun))
            state = anim_state::kWashRun;
        if ((state == anim_state::kIdle || state == anim_state::kIdle2) && available(anim_state::kWashIdle))
            state = anim_state::kWashIdle;
    }
    // [orig: @0x4C008F..0x4C01BD a parent: emplaced 67, or its phrase_set's 68..75]
    if (facts.parented) {
        state = anim_state::kEmplaced;
        if (facts.parent_phrase_set >= 1 && facts.parent_phrase_set <= 8 &&
                available(anim_state::kEmplaced + facts.parent_phrase_set))
            state = anim_state::kEmplaced + facts.parent_phrase_set;
    }
    return state;
}

uint32_t organic_warmup_updates(uint32_t net_id) {
    // [orig: Entity_WarmUpOrganicAnimation @0x4B8B3F..0x4B8B56]
    return 8 * ((net_id & 12u) + 8 * ((net_id & 2u) + 4 * (net_id & 1u))) + 10;
}

OrganicSpawnBody organic_spawn_pose(const OrganicSpawnFacts &facts, uint32_t net_id, IRootMotionSource *source,
                                    AnimVariantRings &rings, int adm_id) {
    InfantryState inf;
    inf.active = true;
    inf.adm_id = adm_id;
    begin_spawn_channels(inf, organic_spawn_state(facts, source, adm_id), source, rings);
    OrganicSpawnBody out;
    if (source != nullptr && adm_id >= 0) {
        RootMotionFrame frame{};
        out.rise = warm_up_channels(inf, net_id, facts.parented, *source, rings, frame);
        out.capsule_bottom = frame.capsule_bottom;
        out.capsule_top = frame.capsule_top;
    }
    out.pose = infantry_body_pose(inf);
    out.channels = inf;
    return out;
}

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
    inf.goal_z = body->pos[2]; // [orig: Entity_InitOrganicAI @0x4BFE07]
    inf.magazine = static_cast<int16_t>(body->profile.clip_size);

    OrganicSpawnFacts facts;
    facts.route = body->slot.f[35] != 0;
    facts.route_channel = body->slot.f[37];
    facts.flags = flags;
    facts.rotor_wash = world.rotor_wash.nearby_zone(body->pos, 983040) != 0;
    if (const Entity *parent = world.registry.get(entity.mount_target)) {
        entity.ground_target = parent->ground_target;
        facts.parented = true;
        facts.parent_phrase_set = parent->has_item_def ? parent->emplaced_config : 0;
    }
    begin_spawn_channels(inf, organic_spawn_state(facts, ai.root_motion, inf.adm_id), ai.root_motion,
                         ai.anim_rings);

    // Entity_WarmUpOrganicAnimation @0x4B8B20: secondary then primary,
    // net-ID permutation, vertical root motion only, one final ground solve.
    if (ai.root_motion != nullptr && inf.adm_id >= 0) {
        RootMotionFrame frame{};
        const bool parented = entity.mount_target.valid();
        body->pos[2] = io::bam_add(body->pos[2],
                warm_up_channels(inf, uint32_t(body->net_id), parented, *ai.root_motion, ai.anim_rings, frame));
        if (!parented) {
            body->collide_state.skip_counter = 0;
            if (ai.collision != nullptr) {
                const int32_t clearance = ai.collision->resolve_entity(
                        world, entity.handle, body->collide_state, body->pos,
                        inf.vel, inf.vel[2], frame.capsule_bottom, frame.capsule_top,
                        body->heading, 0, false, ai.is_authority, world.logic_tick,
                        inf.anim_state, infantry_anim_flags(inf.anim_state), body->health);
                if (clearance < kOrganicWarmupSettleQ16)
                    body->pos[2] = io::bam_sub(body->pos[2], clearance);
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
    // Port-side invariants, not retail words: Entity_ResetToSpawnState
    // @0x4B9610 writes no fade/destroy state. destroy_phases_q16 /
    // destroy_progress are derived values update_item_destroy_fade zeroes and
    // recomputes every call, and objective_death_scored is this port's
    // once-per-life scoring latch (retail sub_50C840 re-dispatches on the def
    // flag alone). The +0x1B0 destroy timer is NOT re-seeded here -- retail's
    // only re-seed is Entity_RespawnVehicle (VehicleSystem::respawn).
    entity.destroy_phases_q16.fill(0);
    entity.destroy_progress = 0;
    entity.objective_death_scored = false;
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
        body->inf.z_quarter_step = 0; // [orig: @0x4B967A]
        body->heading = heading;
        body->vel_x = body->vel_y = 0;
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
            // Both requests take the respawn state; the first dual update
            // re-inits the secondary, then the primary, each serving its ring
            // entry. [orig: Entity_ResetToSpawnState +0x2BC @0x4B9708/@0x4B9714,
            //  +0x2C8 @0x4B972A, AnimMap_UpdateDualChannels @0x4B973D]
            const auto serve = [&](int played) {
                return ai.anim_rings.serve(ai.root_motion, inf.adm_id, played);
            };
            inf.begin_weapon_transition(
                    state, state != inf.weapon_clip_state() ? serve(state) : inf.wpn_variant);
            inf.begin_body_transition(
                    state, -1, state != inf.body_clip_state() ? serve(state) : inf.anim_variant);
            RootMotionFrame frame{};
            for (int i = 0; i < 12; ++i) {
                advance_weapon_channel(inf, ai.root_motion, ai.anim_rings);
                if (ai.root_motion != nullptr)
                    advance_primary_channel(inf, *ai.root_motion, ai.anim_rings, frame);
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

    // The reset's one raise is to the ceiling with the difficulty term (the local
    // player's out of a session), no def-word raise ahead of it [orig:
    // Entity_ResetToSpawnState @0x4B97BC -> Entity_RaiseHealthToMax @0x43C290 ->
    // Entity_GetMaxHealthWithDifficulty]; a row with no definition keeps the def word.
    entity_reset_to_spawn_state(entity, entity.has_item_def
            ? max_health_with_difficulty(world, entity) : entity.health_max);
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
        // The corpse is the descriptor's tag, so the decay group takes the
        // section gate [orig: Entity_UpdateInfantryAI, the store of esi
        // @ 0x4B9F0F ahead of the spawn @ 0x4B9F36].
        effect.section_tagged = true;
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
    // Corpse expiry is the shared destroy: the facial slot, scar entries and
    // incoming references go with the row, not only the registry slot and the
    // brain. [orig: Entity_UpdateInfantryAI @0x4B9F93 -> Entity_Destroy @0x43E810;
    //  the updater then leaves through loc_4BFC89 @0x4B9F9B]
    release_corpse_effect(world, entity);
    entity.hidden = true;
    const EntityHandle handle = entity.handle;
    world.out.entity_events.push_back(EntityRemoveEvent{handle.packed});
    world.commands.remove_ssn(handle);
    if (ai.collision != nullptr) ai.collision->refresh_after_registry_change(world);
    return NpcCorpseStep::Removed;
}

} // namespace opennova::world
