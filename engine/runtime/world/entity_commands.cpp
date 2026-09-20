// EntityCommands — the shared Entity_* primitive layer (the BMS/WAC command
// targets: SSN/group/area commands, teleports, alerts, the AI-change family).
// Split out of world.cpp by leg (the infantry_ladder.cpp precedent) so the
// World tick TU stays under the size ratchet; the two share world.h only.
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/vehicle_attach.h>

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <base/io/bam.h>

namespace opennova::world {

// The mount_best proximity proxy's search radius (a tracked stand-in; the
// witnessed seat-offer walk is the mount system's).
static constexpr double kMountRadius = 20.0;

// ----------------------------------------------------------------------------
// EntityCommands — the shared Entity_* primitive layer.
// Commands mutate the Entity model directly and locally. The one outbound seam
// is the WAC VM's: a registry row whose flags carry 0x18 is serialized as the
// S2C 0x23 script remote command (world/script_remote_command.h) before, or
// instead of, its local call [orig: WacScript_ExecuteBytecode @0x4F58B0 ->
// NapiNPServer_SendFiltered(..., 0x23, ...) @0x4f5e74 / @0x4f5ed1].
// ----------------------------------------------------------------------------

// Script SSN -> entity handle. Mission scripts (WAC SSN* commands + BMS Single
// triggers/actions) written under the dfx2med authoring convention address the
// local player as SSN 10000 (10000 + player slot). The kLocalPlayerSsn alias
// below is a port seam that honours that convention; it is not a retail
// lookup. Retail JO has no alias: its SSN leg is atol(token) ->
// EntityPool_FindByNetId @0x4f0a20, which keys on GamePlayerEntity+0x7C
// (DcbId, low 16 bits; pools 0..3, no netId gate), and the JO player spawn
// [orig: Entity_SpawnFromAnimSlotProperty @0x43c390] memsets the record and
// writes only +0x78 (ownerConnectionId) and +0x15c (NetId, the minimap slot
// id), never DcbId@0x7C or Ssn@0x2e. A retail lookup of 10000 therefore
// misses every player and the compiler reports "Unknown SSN" unless an
// authored entity carries that DcbId (docs/net/novaworld-net-re.md, the
// field-identity grill). Our player entities carry net_id 0 (the wire is
// handle-based), and this seam resolves the authoring convention in their
// place; MP joiner SSNs (10001+) wait on the net track.
// [orig: EntityPool_FindByNetId @0x4f0a20; Entity_SpawnFromAnimSlotProperty @0x43c390]
EntityHandle EntityCommands::resolve_ssn(uint16_t ssn) const {
    if (ssn == kLocalPlayerSsn && world_.cached.local_player.valid())
        return world_.cached.local_player;
    // No SSN-0 guard and pools 0..3, like the retail lookup this mirrors;
    // resolve_ssn_in_pools012 below is the OTHER retail walk (dcb != 0 gate,
    // pools 0..2) and the two differ on purpose. [orig: EntityPool_FindByNetId
    // @0x4f0a20 — mask 0xF, no netId gate]
    return world_.registry.find_by_net_id(ssn);
}

EntityHandle EntityCommands::resolve_target(EntityTarget target) const {
    return target.bound() ? target.handle() : resolve_ssn(target.ssn());
}

bool EntityCommands::set_ssn_target_selector(int32_t ssn, AiTargetSelector field, int32_t value) {
    // [orig: 0x43DA50/+334, 0x43DAC0/+332, 0x43D970/+338, 0x43D9E0/+336]
    if (ssn == 0) return false;
    for (int pool = 0; pool <= 1; ++pool) {
        for (size_t slot = 0; slot < world_.registry.pool_capacity(pool); ++slot) {
            Entity *entity = world_.registry.get(EntityHandle::make(pool, static_cast<int>(slot)));
            if (entity == nullptr || entity->net_id != ssn) continue;
            entity->target_selectors.set(field, value);
            return true;
        }
    }
    return false;
}

int EntityCommands::set_group_target_selector(int32_t group, AiTargetSelector field, int32_t value) {
    // [orig: 0x43D870/+334, 0x43D8F0/+332, 0x43D770/+338, 0x43D7F0/+336]
    if (group == 0) return 0;
    int changed = 0;
    for (int pool = 0; pool <= 1; ++pool) {
        for (size_t slot = 0; slot < world_.registry.pool_capacity(pool); ++slot) {
            Entity *entity = world_.registry.get(EntityHandle::make(pool, static_cast<int>(slot)));
            if (entity == nullptr || entity->group_id != group) continue;
            entity->target_selectors.set(field, value);
            ++changed;
        }
    }
    return changed;
}

bool EntityCommands::set_ssn_name(EntityTarget ssn, const std::string &name) {
    // [orig: WacCmd_SsnName @0x4F7230] Includes dead items; empty names are ignored.
    Entity *entity = world_.registry.get(resolve_target(ssn));
    if (entity == nullptr || entity->item_id == 0 || name.empty()) return false;
    entity->display_name = name.substr(0, 31);
    return true;
}

bool EntityCommands::ssn_critical(EntityTarget ssn) const {
    // [orig: WacCmd_SsnCritical @0x4F1BF0] Signed words, no Flags/alive predicate.
    const Entity *entity = world_.registry.get(resolve_target(ssn));
    if (entity == nullptr || entity->item_id == 0) return false;
    const int32_t health = retail_signed_i16(entity->health);
    return health > 0 && health <= retail_signed_i16(entity->critical_hp);
}

// Retail's entity+0x28 (groundEntity) is ONE carrier link that covers
// seated-in, emplacement-child-of and standing-on alike: both infantry movers
// copy parentEntity (+0x16C) into it every tick while seated [orig:
// Entity_UpdateInfantryPlayerBody @0x4B41A2..0x4B41B4; Entity_UpdateInfantryAI
// @0x4B9960..0x4B9A11], an emplacement child's word is READ as the hull by
// Entity_UpdateChildAttachment @0x4409A0 (@0x4409c6 / @0x4409d6; the
// host-side attach-time writer is unwitnessed, a joiner takes the word from
// the full-spawn record [orig: NapiNPClientMsg_FullEntitySpawn @0x433cdd]),
// and the ground probe stores a deck-stander's carrier [orig: @0x414370]. Our
// model splits those into
// mount_target / emplacement_parent / ground_target, so every +0x28 hop
// re-folds them in that order.
static const Entity *carrier_of(const EntityRegistry &registry, const Entity &e) {
    if (e.mounted && e.mount_target.valid())
        return registry.get(e.mount_target);
    if (e.emplacement_parent.valid())
        return registry.get(e.emplacement_parent);
    return registry.get(e.ground_target);
}

bool EntityCommands::ssn_has_rider(EntityTarget target_ssn) const {
    // [orig: WacCmd_SsnRide @0x4F7000] Pool 0 only; a rider is any non-dead
    // row whose +0x28 carrier chain reaches the target within three hops
    // (@0x4f7067 / @0x4f707a / @0x4f7085; the Entity_IsOnTopOfChain @0x4f19a0
    // shape). No health or item-definition predicate on the rider.
    const EntityHandle target = resolve_target(target_ssn);
    if (!world_.registry.get(target)) return false;
    bool found = false;
    world_.registry.for_each_in_pool(0, [&](const Entity &rider) {
        if (found || ((rider.flags | rider.engine_flags) & kEntityFlagDead) != 0) return;
        const Entity *hop = carrier_of(world_.registry, rider);
        for (int depth = 0; depth < 3 && hop != nullptr; ++depth) {
            if (hop->handle == target) { found = true; return; }
            hop = carrier_of(world_.registry, *hop);
        }
    });
    return found;
}

bool EntityCommands::order_boarding(EntityTarget source_ssn, EntityTarget target_ssn) {
    // [orig: WacCmd_SsnToSsn @0x4F7330] This schedules the existing entry walk.
    const EntityHandle source = resolve_target(source_ssn), target = resolve_target(target_ssn);
    Entity *entity = world_.registry.get(source);
    const Entity *carrier = world_.registry.get(target);
    AiEntity *ai = world_.ai.for_handle(source);
    if (entity == nullptr || carrier == nullptr || entity->item_id == 0 ||
            carrier->item_id == 0 || ai == nullptr) return false;
    if (entity->mounted) world_.vehicles.detach(source);
    ai->slot.f[37] = 125;
    ai->slot.f[38] = carrier->net_id;
    ai->slot.f[36] = int32_t(target.packed) + 1; // rebased nullable pointer
    ai->inf.wait_cooldown = 0;
    return true;
}

bool EntityCommands::kill_ssn(EntityTarget ssn) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    e->health = 0;
    if (e->kind != EntityKind::Organic && !e->is_ai_capable) {
        // Pool 3 uses phase 4; the other pools use phase 1.
        // [orig: Entity_KillByNetId @0x43DBD0]
        destruction_notify_item_damage(world_, *e, e->handle.pool() == 3 ? 4 : 1, {0, 0});
    } else {
        e->alive = false;
    }
    return true;
}

bool EntityCommands::remove_ssn(EntityTarget ssn) {
    EntityHandle h = resolve_target(ssn);
    Entity *entity = world_.registry.get(h);
    if (!entity) return false;
    // The shared destroy primitive, including script and teammate removals.
    // [orig: Entity_Destroy @0x43E810; reference walk @0x465670]
    if (entity->item_type == 3) world_.match.drop_carried_object(world_, h);
    world_.facials.release(*entity);
    world_.out.scars.clear_entity(h);
    world_.rotor_wash.release(*entity);
    for (uint8_t family = 1; family <= 3; ++family)
        if (entity->death_effect_active[family - 1] != 0)
            release_death_effect_bank(*entity, family, world_.out.destruction);
    world_.ai.clear_entity_references(world_, h);
    if (Entity *carrier = world_.registry.get(entity->primary_occupant)) {
        if (entity->item_type != 3) world_.vehicles.detach(carrier->handle);
        if (carrier->mounted_child == h) carrier->mounted_child = {};
    }
    if (entity->mount_target.valid()) world_.vehicles.detach(h);
    // Detach mutates the retained seat entries; iterate a copied handle list.
    std::vector<EntityHandle> occupants;
    for (const Seat &seat : entity->seats)
        if (seat.occupant.valid()) occupants.push_back(seat.occupant);
    for (EntityHandle occupant : occupants) world_.vehicles.detach(occupant);
    world_.ai.release(h);
    if (world_.collision) world_.collision->remove_entity_instance(h);
    world_.registry.despawn(h);
    return true;
}

bool EntityCommands::set_entity_health(EntityHandle h, int32_t hp) {
    Entity *e = world_.registry.get(h);
    if (!e) return false;
    e->health = hp;
    e->alive = hp > 0;
    // The AI motor's entity+286 mirror follows, or the next infantry tick
    // hydrates the registry row back [orig: the WAC SETHP op writes entity+286].
    if (AiEntity *a = world_.ai.for_handle(h)) a->health = static_cast<int16_t>(hp);
    return true;
}

bool EntityCommands::set_entity_position(EntityHandle h, const Vec3 &mission_pos) {
    Entity *e = world_.registry.get(h);
    if (!e) return false;
    e->position = mission_pos;
    if (AiEntity *a = world_.ai.for_handle(h)) {
        a->pos[0] = static_cast<int32_t>(mission_pos.x * 65536.0f);
        a->pos[1] = static_cast<int32_t>(mission_pos.y * 65536.0f);
        a->pos[2] = static_cast<int32_t>(mission_pos.z * 65536.0f);
    }
    return true;
}

// --- the WAC weather handlers (the observable EnvState mirror + the weather
// home; every command bumps the mirror's generation the way the VM did) ------

void EntityCommands::set_fog_type(int32_t type) {
    world_.env.fog_type = type;
    ++world_.env.generation;
    world_.weather.command_fog_type(type);
}

void EntityCommands::set_fog_distance(int32_t metres) {
    set_fog_distance_q16(static_cast<int32_t>(static_cast<uint32_t>(metres) << 16));
}

void EntityCommands::move_fog(int32_t metres, int32_t seconds) {
    move_fog_q16(static_cast<int32_t>(static_cast<uint32_t>(metres) << 16), seconds);
}

void EntityCommands::set_fog_distance_q16(int32_t distance_q16) {
    world_.env.fog_dist = distance_q16 / 65536; // legacy whole-metre inspection mirror
    ++world_.env.generation;
    world_.weather.command_fog_distance_q16(distance_q16);
}

void EntityCommands::move_fog_q16(int32_t distance_q16, int32_t seconds) {
    world_.env.fog_dist = distance_q16 / 65536;
    ++world_.env.generation;
    world_.weather.command_move_fog_q16(distance_q16, seconds);
}

void EntityCommands::set_rain(int32_t percent, int32_t seconds) {
    world_.env.rain = percent;
    ++world_.env.generation;
    world_.weather.command_rain(percent, seconds);
}

void EntityCommands::set_snow(int32_t percent, int32_t seconds) {
    world_.env.snow = percent;
    ++world_.env.generation;
    world_.weather.command_snow(percent, seconds);
}

void EntityCommands::set_overcast(int32_t percent, int32_t seconds) {
    world_.env.overcast = percent;
    ++world_.env.generation;
    world_.weather.command_overcast(percent, seconds);
}

void EntityCommands::set_sky_speed(int32_t rate) {
    world_.env.sky_speed = rate;
    ++world_.env.generation;
    world_.weather.command_sky_speed(rate);
}

void EntityCommands::set_fov(int32_t degrees) {
    world_.weather.command_fov(degrees);
}

void EntityCommands::set_sky_height(int32_t height_raw) {
    world_.weather.command_sky_height(height_raw);
}

void EntityCommands::quake(int32_t seconds) {
    world_.weather.command_quake(seconds);
}

void EntityCommands::set_time_of_day_minutes(int32_t minute_of_day) {
    world_.env.time_of_day = minute_of_day;
    ++world_.env.generation;
    world_.weather.command_time_of_day_minutes(minute_of_day);
}

void EntityCommands::debug_set_time_of_day_minutes(double minute_of_day) {
    world_.env.time_of_day = static_cast<int32_t>(minute_of_day);
    ++world_.env.generation;
    world_.weather.debug_set_time_of_day_minutes(minute_of_day);
}

void EntityCommands::sun_fade(int32_t percent, int32_t seconds) {
    world_.weather.command_sun_fade(percent, seconds);
}

void EntityCommands::set_color_fade(int32_t seconds) {
    world_.weather.command_color_fade(seconds);
}

void EntityCommands::set_lightning_color(uint32_t rgb) {
    world_.weather.command_lightning_color(rgb);
}

void EntityCommands::lightning_flash() {
    world_.weather.command_flash();
}

void EntityCommands::lightning_far_flash() {
    world_.weather.command_far_flash();
}

void EntityCommands::set_weather_color(WeatherColorTarget target, uint32_t rgb) {
    switch (target) {
        case WeatherColorTarget::Sun: world_.env.sun_rgb = rgb; ++world_.env.generation; break;
        case WeatherColorTarget::Sky: world_.env.sky_rgb = rgb; ++world_.env.generation; break;
        case WeatherColorTarget::Fog: world_.env.fog_rgb = rgb; ++world_.env.generation; break;
        default: break;
    }
    world_.weather.command_block_color(target, rgb);
}

void EntityCommands::set_wind_scale(int32_t value) {
    world_.weather.set_wind_scale(value);
}

bool EntityCommands::kill_player(EntityHandle victim, EntityHandle killer) {
    Entity *e = world_.registry.get(victim);
    if (e == nullptr || (e->flags & kEntityFlagPlayer) == 0) return false;
    e->health = 0;
    RoundDeath d;
    d.victim = victim;
    d.victim_handle = victim.packed;
    // An unstamped killer falls back to the victim's +0x178 lastAttacker, the
    // field GameEvent_PlayerDeath reads; the infantry death edge has already
    // emptied it for a body nothing ever hit [orig: @0x516f6b].
    d.killer = killer.valid() ? killer : e->last_attacker;
    d.killer_handle = killer.valid() ? killer.packed : 0xFFFFu;
    world_.round_sim.deaths.push_back(d);
    return true;
}

bool EntityCommands::set_entity_weapon_ammo(EntityHandle h, int32_t clip, int32_t reserve) {
    Entity *e = world_.registry.get(h);
    if (e == nullptr) return false;
    e->primary_weapon_slot.clip = retail_signed_i16(clip);
    e->primary_weapon_slot.reserve = retail_signed_i16(reserve);
    return true;
}

bool EntityCommands::set_entity_item_attrib(EntityHandle h, uint32_t attrib, uint32_t attrib2) {
    Entity *e = world_.registry.get(h);
    if (e == nullptr) return false;
    stamp_item_attrib(*e, attrib, attrib2);
    return true;
}

bool EntityCommands::set_ssn_respawns(EntityTarget ssn, int32_t count) {
    // [orig: WacCmd_SsnSpawn @0x4F7A80] No item, health or brain gate.
    Entity *entity = world_.registry.get(resolve_target(ssn));
    if (entity == nullptr) return false;
    entity->npc_respawns = static_cast<int16_t>(static_cast<uint16_t>(count));
    return true;
}

void EntityCommands::set_group_respawns(int32_t group, int32_t count) {
    // [orig: WacScript_SetEntityWaypoint @0x4F7AE0] GroupSpawn's actual body.
    world_.registry.for_each_in_pool(0, [&](const Entity &row) {
        Entity &entity = *world_.registry.get(row.handle);
        if (static_cast<int16_t>(entity.group_id) == group)
            entity.npc_respawns = static_cast<int16_t>(static_cast<uint16_t>(count));
    });
}

bool EntityCommands::set_ssn_hp(EntityTarget ssn, int32_t hp) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    e->health = hp;
    e->alive = hp > 0;
    return true;
}

bool EntityCommands::add_ssn_hp(EntityTarget ssn, int32_t delta) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    e->health += delta;
    if (e->health < 0) e->health = 0;
    e->alive = e->health > 0;
    return true;
}

bool EntityCommands::set_ssn_accuracy(EntityTarget ssn, int32_t primary,
                                      int32_t secondary) {
    AiEntity *ae = world_.ai.for_handle(resolve_target(ssn));
    if (ae == nullptr) return false;
    // The two authored values land in reverse slot order.
    // [orig: WacCmd_SetAccuracy @0x4F2070]
    ae->slot.f[AiSlot::kAimErrorSecondary] = std::max(0, 100 - primary);
    ae->slot.f[AiSlot::kAimErrorPrimary] = std::max(0, 100 - secondary);
    return true;
}

bool EntityCommands::set_ssn_guard(EntityTarget ssn, bool guard) {
    Entity *entity = world_.registry.get(resolve_target(ssn));
    if (entity == nullptr) return false;
    // [orig: WacCmd_SsnGuard @0x4F71C0] Retail writes the one Flags dword;
    // 0x40 is legacy-mirrored, so both views stay coherent here (the
    // vehicle_attach precedent — engine_flags is what the 0x10 static record
    // streams).
    if (guard) {
        entity->flags |= kEntityFlagMounted;
        entity->engine_flags |= kEntityFlagMounted;
    } else {
        entity->flags &= ~kEntityFlagMounted;
        entity->engine_flags &= ~kEntityFlagMounted;
    }
    return true;
}

namespace {

// The per-entity leg of a waypoint REDIRECT [orig: Entity_SetWaypointByTeam @0x43cdb4]:
// a mounted NON-player auto-detaches [orig: Entity_DetachFromVehicleIfServer @0x4359d0],
// the entity route fields update, and the brain (when the entity carries one) takes the
// mode/list/node order + the turn-budget seed.
void apply_waypoint_order(World &world, Entity &e, int32_t list, int32_t node) {
    const bool is_player = e.handle.pool() == 0 && e.player_class != 0;
    if (e.mounted && !is_player) world.commands.dismount(e.handle);
    e.waypoint_id = static_cast<uint8_t>(list);
    // Retail keeps the authored node, resolving only the -1 sentinel to nearest.
    // [orig: Entity_SetWaypointByTeam @0x43cdb4 ->
    // Entity_FindNearestTriggerByType @0x407ea0]
    e.wp_number = node >= 0 ? node : 0;
    if (AiEntity *ae = world.ai.for_handle(e.handle)) {
        world.ai.apply_route_order(*ae, list, node);
        // Mirror the resolved nearest/clamped node into the registry entity,
        // which is the script/debug-facing route state.
        if (ae->brain.f[AiBrain::kWpType] == 1 &&
            ae->brain.f[AiBrain::kWpChannel] == list)
            e.wp_number = ae->brain.f[AiBrain::kWpNode];
    }
}

} // namespace

bool EntityCommands::set_ssn_waypoint(EntityTarget ssn, int32_t wp, int32_t node) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    apply_waypoint_order(world_, *e, wp, node);
    return true;
}

namespace {

bool set_entity_range(World &world, EntityHandle handle, int slot, int32_t distance_q16) {
    // No item/health gate. A valid entity without a controller still succeeds.
    if (world.registry.get(handle) == nullptr) return false;
    if (AiEntity *body = world.ai.for_handle(handle)) body->slot.f[slot] = distance_q16;
    return true;
}

int set_group_range(World &world, int32_t group, int slot, int32_t distance_q16) {
    // [orig: GroupMin/Max/Att @0x4F7C50/@0x4F7CA0/@0x4F7CF0]
    // All used pool-0 rows, including group 0 and itemless/dead rows.
    world.registry.for_each_in_pool(0, [&](const Entity &entity) {
        if (static_cast<int16_t>(entity.group_id) == group)
            set_entity_range(world, entity.handle, slot, distance_q16);
    });
    return 1;
}

} // namespace

bool EntityCommands::set_ssn_engage_min(EntityTarget ssn, int32_t distance_q16) {
    // [orig: WacCmd_SsnMin @0x4F2010] Controller+64.
    return set_entity_range(world_, resolve_target(ssn), AiSlot::kEngageMin, distance_q16);
}

bool EntityCommands::set_ssn_engage_max(EntityTarget ssn, int32_t distance_q16) {
    // [orig: WacCmd_SsnMax @0x4F2210] Controller+68.
    return set_entity_range(world_, resolve_target(ssn), AiSlot::kSightRange, distance_q16);
}

bool EntityCommands::set_ssn_attack_max(EntityTarget ssn, int32_t distance_q16) {
    // [orig: WacCmd_SsnAtt @0x4F2270] Controller+60.
    return set_entity_range(world_, resolve_target(ssn), AiSlot::kAttackRange, distance_q16);
}

namespace {
void store_script_body_animation(World &world, Entity &entity, int32_t state) {
    if (AiEntity *body = world.ai.for_handle(entity.handle))
        body->inf.store_body_animation(state);
    entity.net_anim_state = static_cast<uint8_t>(state);
    entity.body_anim_slot = body_anim_slot_from_state(state);
}
} // namespace

bool EntityCommands::set_ssn_anim(EntityTarget ssn, int32_t anim_state) {
    // The +0x1C type index gate does not test health or the +0x20 ItemDef pointer.
    // [orig: WacCmd_SsnAnim @0x4F7630]
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (e == nullptr || e->item_id == 0) return false;
    store_script_body_animation(world_, *e, anim_state);
    return true;
}

bool EntityCommands::set_local_anim(int32_t anim_state) {
    // [orig: WacCmd_Anim @0x4ED5B0] Only the local entity pointer is required.
    Entity *e = world_.registry.get(world_.cached.local_player);
    if (e == nullptr) return false;
    store_script_body_animation(world_, *e, anim_state);
    return true;
}

bool EntityCommands::raise_local_player() {
    // [orig: Cheat_RaiseLocalPlayerZ @0x4ED4E0] A single wrapped Z addition,
    // preserving velocity, saved pose and ground ownership. Guard the original's
    // unchecked null-player dereference for headless/script-only worlds.
    Entity *e = world_.registry.get(world_.cached.local_player);
    if (e == nullptr) return false;
    AiEntity *body = world_.ai.for_handle(e->handle);
    const int32_t z = body != nullptr ? body->pos[2] : to_fixed(e->position.z);
    const int32_t raised = static_cast<int32_t>(uint32_t(z) + 0x1F40000u);
    if (body != nullptr) body->pos[2] = raised;
    e->position.z = static_cast<float>(from_fixed(raised));
    return true;
}

bool EntityCommands::set_ssn_turn(EntityTarget ssn, int32_t heading_degrees) {
    // [orig: WacCmd_SsnTurn @0x4F72B0] The heading argument is converted with
    // two wrapped shifts and signed division, not a full-precision BAM divide.
    Entity *entity = world_.registry.get(resolve_target(ssn));
    if (entity == nullptr || entity->item_id == 0) return false;
    const int32_t numerator = static_cast<int32_t>((90u - uint32_t(heading_degrees)) << 16);
    const int32_t target = static_cast<int32_t>(uint32_t(numerator / 360) << 16);
    if (AiEntity *body = world_.ai.for_handle(entity->handle)) {
        // Entity+0x1A8 is the org1 turn target but the org2 jump cooldown.
        // [orig: NPC body chase @0x4BE8FD; player cooldown @0x4B7DE0]
        const bool player = body->inf.is_local_player || body->net_is_remote_peer ||
            ((entity->flags | entity->engine_flags) & kEntityFlagPlayer) != 0;
        if (player) body->inf.jump_cooldown = target;
        else body->inf.target_heading = target;
    }
    return true;
}

bool EntityCommands::teleport_local_to_ssn(EntityTarget ssn) {
    // [orig: WacCmd_Tele @0x4F22D0] Allocation is the only target/local gate.
    // Health and position stores do not revive flags, rotate, detach or stop
    // the motor. The saved-position triple changes; saved attitude does not.
    const Entity *target = world_.registry.get(resolve_target(ssn));
    Entity *player = world_.registry.get(world_.cached.local_player);
    if (target == nullptr || player == nullptr) return false;
    const AiEntity *target_body = world_.ai.for_handle(target->handle);
    const int32_t pos[3] = {
        target_body != nullptr ? target_body->pos[0] : to_fixed(target->position.x),
        target_body != nullptr ? target_body->pos[1] : to_fixed(target->position.y),
        target_body != nullptr ? target_body->pos[2] : to_fixed(target->position.z)};
    player->health = 20000;
    player->position = {float(from_fixed(pos[0])), float(from_fixed(pos[1])),
                        float(from_fixed(pos[2]))};
    std::copy_n(pos, 3, player->saved_live_pos);
    player->mount_toggle_fallback = target->handle;
    player->ground_target = target->handle;
    if (AiEntity *body = world_.ai.for_handle(player->handle)) {
        body->health = 20000;
        std::copy_n(pos, 3, body->pos);
        std::copy_n(pos, 3, body->net_saved_live_pose);
    }
    if (world_.collision != nullptr)
        world_.collision->refresh_entity_proximity(world_, *player);
    return true;
}

bool EntityCommands::set_ssn_hidden(EntityTarget ssn, bool hidden) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    e->hidden = hidden;
    return true;
}

bool EntityCommands::set_ssn_held(EntityTarget ssn, bool held) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    e->held = held;
    return true;
}

bool EntityCommands::set_ssn_disabled(EntityTarget ssn, bool disabled) {
    Entity *e = world_.registry.get(resolve_target(ssn));
    if (!e) return false;
    e->disabled = disabled;
    return true;
}

bool EntityCommands::ssn_exists(EntityTarget ssn) const {
    return world_.registry.get(resolve_target(ssn)) != nullptr;
}

bool EntityCommands::ssn_alive(EntityTarget ssn) const {
    const Entity *e = world_.registry.get(resolve_target(ssn));
    return e != nullptr && e->alive;
}

bool EntityCommands::ssn_dead(EntityTarget ssn) const {
    const Entity *e = world_.registry.get(resolve_target(ssn));
    return e != nullptr && !e->alive;
}

bool EntityCommands::ssn_wounded(EntityTarget ssn) const {
    const Entity *entity = world_.registry.get(resolve_target(ssn));
    if (entity == nullptr || entity->item_id == 0) return false;
    // [orig: WacCmd_SsnWounded @0x4F1B80] Health is read unsigned,
    // while healthMax is arithmetically halved as signed i16 and then compared
    // in the same unsigned 16-bit domain.
    const uint16_t health = static_cast<uint16_t>(entity->health);
    const int16_t health_max = static_cast<int16_t>(entity->health_max);
    const uint16_t half = static_cast<uint16_t>(
            static_cast<int16_t>(health_max >> 1));
    return health <= half;

}
bool EntityCommands::ssn_in_area(int32_t ssn, int area_id) const {
    // No death/item gate and no early exit on an out-of-bounds duplicate.
    // [orig: Entity_IsBmsRefInTriggerBounds @ 0x43e510]
    const Area *area = world_.registry.area(area_id);
    if (ssn == 0 || area == nullptr) return false;
    for (int pool = 0; pool <= 1; ++pool) {
        for (size_t slot = 0; slot < world_.registry.pool_capacity(pool); ++slot) {
            const EntityHandle handle = EntityHandle::make(pool, static_cast<int>(slot));
            const Entity *entity = world_.registry.get(handle);
            if (entity == nullptr) continue;
            // The SP listen-host model retains its existing script-player
            // alias: its socketless local body carries net_id 0 (D-NET-112).
            const bool local_alias = !world_.rules.mp_session && ssn == kLocalPlayerSsn &&
                    handle == world_.cached.local_player && entity->net_id == 0;
            if ((entity->net_id == ssn || local_alias) && area->bounds.contains(entity->position))
                return true;
        }
    }
    return false;
}

bool EntityCommands::group_in_area(int32_t group, int area_id) const {
    // Only pool 0 checks Flags bit 0; a destroyed pool-1 item still counts.
    // [orig: Entity_IsTeamInTriggerBounds @ 0x43c730]
    const Area *area = world_.registry.area(area_id);
    if (group == 0 || area == nullptr) return false;
    for (int pool = 0; pool <= 1; ++pool) {
        for (size_t slot = 0; slot < world_.registry.pool_capacity(pool); ++slot) {
            const Entity *entity = world_.registry.get(EntityHandle::make(pool, static_cast<int>(slot)));
            if (entity == nullptr || static_cast<int16_t>(entity->group_id) != group) continue;
            if (pool == 0 && ((entity->flags | entity->engine_flags) & 1u) != 0) continue;
            if (area->bounds.contains(entity->position)) return true;
        }
    }
    return false;
}

bool EntityCommands::ssn_in_script_area(EntityTarget target, int32_t zone_id,
                                         bool three_dimensional) const {
    // [orig: WacCmd_SsnArea @0x4F1020; WacCmd_SsnArea3D @0x4F0F60]
    const Entity *entity = world_.registry.get(resolve_target(target));
    if (entity == nullptr || entity->item_id == 0 ||
            ((entity->flags | entity->engine_flags) & 1u) != 0 || zone_id == 0)
        return false;
    const int index = world_.registry.area_index_by_zone_id(zone_id);
    const Area *area = index < 128 ? world_.registry.area(index) : nullptr;
    if (area == nullptr) return false;
    const Aabb &b = area->script_bounds;
    const Vec3 &p = entity->position;
    return p.x >= b.min.x && p.x <= b.max.x &&
           p.y >= b.min.y && p.y <= b.max.y &&
           (!three_dimensional || (p.z >= b.min.z && p.z <= b.max.z));
}

bool EntityCommands::ssn_at_location(EntityTarget target, int32_t location) const {
    // [orig: WacCmd_SsnLoc @0x4F0E90] A blink hit overrides the box even with ID 0.
    const Entity *entity = world_.registry.get(resolve_target(target));
    if (entity == nullptr || entity->item_id == 0 ||
            ((entity->flags | entity->engine_flags) & 1u) != 0) return false;
    int32_t value = world_.registry.location_at(entity->position);
    if (entity->blink_hits[0] != 0) {
        const Entity *building = world_.registry.get(
                EntityHandle::make(2, int(entity->blink_hits[0] >> 20)));
        value = building != nullptr ? building->music_location : 0;
    }
    return value == location;
}

void EntityCommands::update_local_location(EntityHandle player) {
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B6004..0x4B6359]
    // The local cache instead falls back to the last box when indoor music is 0.
    const Entity *entity = world_.registry.get(player);
    if (entity == nullptr || player != world_.cached.local_player || entity->health <= 0)
        return;
    const Entity *building = entity->blink_hits[0] != 0 ? world_.registry.get(
            EntityHandle::make(2, int(entity->blink_hits[0] >> 20))) : nullptr;
    world_.script.wac_values.local_location = building != nullptr && building->music_location != 0
            ? building->music_location : world_.registry.location_at(entity->position);
}

bool EntityCommands::local_player_out_of_bounds() const {
    // [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40] At least one area-trigger
    // record with the Active flag (bit0), and the local player's X/Y inside NONE
    // of the active zones' X/Y AABBs — the Z axis is ignored. No local player
    // (a serve-only host) reads as in-bounds.
    const Entity *p = world_.registry.get(world_.cached.local_player);
    if (!p) return false;
    bool any_active = false;
    for (int id = 0;; ++id) {
        const Area *a = world_.registry.area(id);
        if (!a) break;
        if (!a->active) continue;
        any_active = true;
        if (p->position.x >= a->bounds.min.x && p->position.x <= a->bounds.max.x &&
            p->position.y >= a->bounds.min.y && p->position.y <= a->bounds.max.y)
            return false;
    }
    return any_active;
}

// --- the cat-2 single-state trigger queries (bms-event-runtime-re §3b) -------

namespace {

// Retail's per-helper pool breadth: the alert/health scanners walk pools 0-1,
// the holding walks pool 0 only, the 42-45 family resolves through
// EntityPool_FindByNetId (pools 0-3). Our resolve is registry-wide, so each
// query re-applies its helper's pool gate from the handle's pool bits.
bool in_pools_01(EntityHandle h) { return h.valid() && h.pool() <= 1; }

} // namespace

bool EntityCommands::ssn_at_alert(uint16_t ssn, int level) const {
    // [orig: Entity_IsSsnAtAlertLevel @0x43e780 — SSN 0 -> 0 @0x43e787;
    // pools 0-1; aiRuntime (entity+0x68) null -> 0; byte +0x88 == level]
    if (ssn == 0) return false;
    EntityHandle h = resolve_target(ssn);
    if (!in_pools_01(h)) return false;
    AiEntity *ae = world_.ai.for_handle(h);
    if (!ae) return false;
    return ae->slot.bytes()[AiSlot::kAlertByte] == level;
}

bool EntityCommands::ssn_damage_taken_at_least(uint16_t ssn, int32_t points) const {
    // [orig: Entity_HasDamageCapacity @0x43e3d0 — SSN 0 -> 0 (the
    // per-helper head guard; the pool finder itself has none); pools 0-1;
    // signed health(+0x11E) <= healthMax(def+0x17C) - points; no null-def
    // guard, no alive gate]
    if (ssn == 0) return false;
    EntityHandle h = resolve_target(ssn);
    if (!in_pools_01(h)) return false;
    const Entity *e = world_.registry.get(h);
    if (!e) return false;
    return e->health <= e->health_max - points;
}

bool EntityCommands::ssn_full_health(uint16_t ssn) const {
    // [orig: Entity_HasFullHealth @0x43e470 — SSN 0 -> 0; pools 0-1; null
    // itemDef -> 0 (our health_max == 0 unresolved marker); health >= healthMax]
    if (ssn == 0) return false;
    EntityHandle h = resolve_target(ssn);
    if (!in_pools_01(h)) return false;
    const Entity *e = world_.registry.get(h);
    if (!e || e->health_max == 0) return false;
    return e->health >= e->health_max;
}

bool EntityCommands::ssn_health_at_least(uint16_t ssn, int32_t threshold) const {
    // [orig: Entity_HasHealthAboveThreshold @0x43e350 — SSN 0 -> 0;
    // pools 0-1; health >= threshold, def-free]
    if (ssn == 0) return false;
    EntityHandle h = resolve_target(ssn);
    if (!in_pools_01(h)) return false;
    const Entity *e = world_.registry.get(h);
    if (!e) return false;
    return e->health >= threshold;
}

bool EntityCommands::ssn_holding_group(uint16_t ssn, int group) const {
    // [orig: Entity_IsSsnHoldingItemGroup @0x43e2f0 — SSN 0 -> 0
    // @0x43e2f7; pool 0 only; mountedChild(+0x268) null -> 0;
    // held->commandGroup(+0x11C) == group]
    if (ssn == 0) return false;
    EntityHandle h = resolve_target(ssn);
    if (!h.valid() || h.pool() != 0) return false;
    const Entity *e = world_.registry.get(h);
    if (!e) return false;
    const Entity *held = world_.registry.get(e->mounted_child);
    if (!held) return false;
    return held->group_id == group;
}

bool EntityCommands::group_holding_group(int holder_group, int held_group) const {
    // [orig: TriggerGroup_AnyMemberHoldingItemGroup @0x43c870 — walk pool 0,
    // gate ItemTypeIndex(+0x1C) != 0, first member of holder_group whose
    // mountedChild's commandGroup == held_group]
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(holder_group), members);
    for (EntityHandle h : members) {
        if (h.pool() != 0) continue;
        const Entity *e = world_.registry.get(h);
        if (!e || e->item_id == 0) continue;
        const Entity *held = world_.registry.get(e->mounted_child);
        if (held && held->group_id == held_group) return true;
    }
    return false;
}

bool EntityCommands::ssn_on_chain_of(EntityTarget ssn, EntityTarget target_ssn) const {
    // [orig: Entity_IsOnTopOfChain @0x4f19a0 — both resolved + ItemTypeIndex
    // gates; A's groundEntity(+0x28) chain, up to 3 hops, == B]
    // The +0x28 carrier fold is carrier_of above — without it, "the player
    // rides the Stryker" (the 05TRcoop convoy root trigger, Single/sub42
    // p1=10000) never evaluated true for a seated player and the chain stayed
    // dead.
    const Entity *a = world_.registry.get(resolve_target(ssn));
    const Entity *b_probe = world_.registry.get(resolve_target(target_ssn));
    if (!a || !b_probe || a->item_id == 0 || b_probe->item_id == 0) return false;
    const Entity *hop = carrier_of(world_.registry, *a);
    for (int i = 0; i < 3 && hop != nullptr; ++i) {
        if (hop == b_probe) return true;
        hop = carrier_of(world_.registry, *hop);
    }
    return false;
}

namespace {

// [orig: Entity_CompareDistancesToTarget @0x4F13F2..0x4F14A6]
// Subtraction wraps before conversion to float. Each length clamps to the
// float constant 2147418112, then truncates to a signed Q16 word.
int32_t script_position_distance(const int32_t a[3], const int32_t b[3]) {
    double squared = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double delta = static_cast<int32_t>(
                static_cast<uint32_t>(a[axis]) - static_cast<uint32_t>(b[axis]));
        squared += delta * delta;
    }
    return static_cast<int32_t>(std::min(std::sqrt(squared), 2147418112.0));
}

int32_t script_entity_distance(const Entity &a, const Entity &b) {
    const int32_t pa[3] = {to_fixed(a.position.x), to_fixed(a.position.y), to_fixed(a.position.z)};
    const int32_t pb[3] = {to_fixed(b.position.x), to_fixed(b.position.y), to_fixed(b.position.z)};
    return script_position_distance(pa, pb);
}

// The same item-index gate applies to proximity, LOS and lead comparisons.
// [orig: Entity_CheckProximity @0x4F14C0]
bool trigger_pair_distance(const World &w, const EntityCommands &cmds,
                           EntityTarget ssn_a, EntityTarget ssn_b,
                           const Entity *&a, const Entity *&b, int32_t &dist) {
    a = w.registry.get(cmds.resolve_target(ssn_a));
    b = w.registry.get(cmds.resolve_target(ssn_b));
    if (a == nullptr || b == nullptr || a->item_id == 0 || b->item_id == 0)
        return false;
    dist = script_entity_distance(*a, *b);
    return true;
}

} // namespace

bool EntityCommands::ssn_within_distance(EntityTarget ssn, EntityTarget target_ssn,
                                         int32_t distance_q16) const {
    // [orig: Entity_CheckProximity @0x4f14c0 — dist <= p3, RAW positive]
    const Entity *a = nullptr;
    const Entity *b = nullptr;
    int32_t dist = 0;
    if (!trigger_pair_distance(world_, *this, ssn, target_ssn, a, b, dist))
        return false;
    return dist <= distance_q16;
}

namespace {

// The +0x1FC LOS endpoint: position plus the host-stamped model bbox center,
// added RAW (unrotated) — an unstamped center leaves the raw position, like
// retail's zeroed pool memory. [orig: rayStart = entity[1..3] +
// entity[127..129] @0x4f1880..0x4f18c5 (sub 45) / @0x4f1728..0x4f176f
// (sub 44); the center writer Entity_InitFromModel @0x40df1e..0x40e018]
void los_offset_point(const Entity &e, int32_t out[3]) {
    out[0] = static_cast<int32_t>(uint32_t(to_fixed(e.position.x)) + uint32_t(to_fixed(e.bbox_center.x)));
    out[1] = static_cast<int32_t>(uint32_t(to_fixed(e.position.y)) + uint32_t(to_fixed(e.bbox_center.y)));
    out[2] = static_cast<int32_t>(uint32_t(to_fixed(e.position.z)) + uint32_t(to_fixed(e.bbox_center.z)));
}

} // namespace

bool EntityCommands::ssn_leads_target(EntityTarget first, EntityTarget second,
                                      EntityTarget target, int32_t lead_q16) const {
    // [orig: Entity_CompareDistancesToTarget @0x4F12E0] Strictly greater.
    const Entity *a = world_.registry.get(resolve_target(first));
    const Entity *b = world_.registry.get(resolve_target(second));
    const Entity *goal = world_.registry.get(resolve_target(target));
    if (a == nullptr || b == nullptr || goal == nullptr ||
            a->item_id == 0 || b->item_id == 0 || goal->item_id == 0)
        return false;
    return script_entity_distance(*b, *goal) - script_entity_distance(*a, *goal) > lead_q16;
}

bool EntityCommands::ssn_los_clear_within(EntityTarget ssn, EntityTarget target_ssn,
                                          int32_t distance_q16) const {
    // [orig: Entity_CheckLineOfSightInRange @0x4f15e0 — center distance gate,
    // then a radius-0 ray between the +0x1FC bbox-center offset points;
    // <= 20 u uses the entity-aware walker @0x53b130, above it
    // terrain/sectors @0x539910. Our port rays through the one modeled LOS
    // seam (that walker split stays a tracked stand-in, §3b).]
    const Entity *a = nullptr;
    const Entity *b = nullptr;
    int32_t dist = 0;
    if (!trigger_pair_distance(world_, *this, ssn, target_ssn, a, b, dist))
        return false;
    if (dist > distance_q16) return false;
    int32_t pa[3];
    los_offset_point(*a, pa);
    int32_t pb[3];
    los_offset_point(*b, pb);
    const CollisionWorld::RayDebugScope ray_scope(
            world_.collision, CollisionWorld::RayDebugCategory::kScriptLos);
    return world_.ai.line_of_sight_clear(world_, pa, pb,
                                          resolve_target(ssn), resolve_target(target_ssn));
}

bool EntityCommands::ssn_sees_within(EntityTarget ssn, EntityTarget target_ssn,
                                     int32_t distance_q16) const {
    // [orig: Entity_CheckLineOfSight @0x4f17c0 — range, ray, AND bearing all
    // computed over the +0x1FC bbox-center offset points (deltas
    // @0x4f18cd..0x4f18dd), then the facing cone:
    // |wrap32(-yaw(+0x10) - int(atan2(dy, dx) * -(2^31/pi)))| <= 0x15555540
    // (30.0 deg), int32 wrap = shortest arc]
    const Entity *a = world_.registry.get(resolve_target(ssn));
    const Entity *b_ent = world_.registry.get(resolve_target(target_ssn));
    if (a == nullptr || b_ent == nullptr ||
        a->item_id == 0 || b_ent->item_id == 0)
        return false;
    int32_t pa[3];
    los_offset_point(*a, pa);
    int32_t pb[3];
    los_offset_point(*b_ent, pb);
    if (script_position_distance(pa, pb) > distance_q16) return false;
    const double fdx = static_cast<int32_t>(uint32_t(pb[0]) - uint32_t(pa[0]));
    const double fdy = static_cast<int32_t>(uint32_t(pb[1]) - uint32_t(pa[1]));
    const CollisionWorld::RayDebugScope ray_scope(
            world_.collision, CollisionWorld::RayDebugCategory::kScriptLos);
    if (!world_.ai.line_of_sight_clear(world_, pa, pb, resolve_target(ssn),
                                        resolve_target(target_ssn)))
        return false;
    // Retail truncates toward zero (_ftol2_sse) over the NEGATED scale
    // -(2^31/pi); the sign folds out under the cdq-abs below, but the
    // truncation is load-bearing (llround here would drift 1 BAM32 LSB on
    // half of all bearings). [orig: fpatan -> fmul dbl_7C57B8
    // (-683565275.5764316) -> _ftol2_sse @0x4f195f-0x4f196f]
    const int32_t bearing_neg = static_cast<int32_t>(
            std::atan2(fdy, fdx) * -683565275.5764316);
    const int32_t heading = a->veh.yaw_seeded
            ? a->veh.yaw_bam
            : bam_heading_from_mission_yaw_deg(static_cast<double>(a->yaw));
    // diff = wrap32(-yaw - trunc(atan2 * -(2^31/pi))) [orig: neg ecx; sub
    // ecx, eax @0x4f1977-0x4f1979] == wrap32(bearing - yaw); |.| equalizes.
    const int32_t diff = static_cast<int32_t>(
            0u - static_cast<uint32_t>(heading) - static_cast<uint32_t>(bearing_neg));
    // Retail's cdq/xor/sub abs: INT_MIN stays negative, so a target EXACTLY
    // 180.0 deg astern satisfies the signed <= — a witnessed quirk, carried.
    const uint32_t mask = static_cast<uint32_t>(diff >> 31);
    const int32_t adiff =
            static_cast<int32_t>((static_cast<uint32_t>(diff) ^ mask) - mask);
    return adiff <= 0x15555540; // 30.0000 deg in BAM32
}

// A scripted group kill has to reach the WIRE, not just zero the health. Retail
// never fans deaths from the damage pass: every motor's per-entity update carries
// the edge `Health <= 0 && (Flags & 2) == 0` and calls Entity_CheckAndProcessDeath
// there, so ANY writer of zero health — a bullet, or this action — is noticed and
// notified. The killer rides on the VICTIM (entity+704, read by
// BuildDeathNotifyPayload), which is why retail's own baseline capture shows its
// scripted kills as `killerSource=0`: the script never stamps that field. Ours
// leaves the killer handle unset for the same reason, and the burst matches.
//
// SHAPE NOTE: raising the death here rather than from a health<=0 sweep in the
// motor is narrower than the original — a future health-zeroing path would have
// to remember to do the same. Converging on the sweep is worth doing when the
// death path is next opened up; it needs the killer moved onto the entity first.
// [orig: the edge @0x4bfxxx (org1) / @0x4b73xx (org2) -> Entity_CheckAndProcessDeath
//  @0x51b550 -> BuildDeathNotifyPayload @0x5036e0, send_mask 0x90]
static void raise_scripted_death(World &world, Entity &e, EntityHandle h) {
    RoundDeath d;
    d.victim = h;
    d.victim_handle = h.packed;
    // The victim's +0x178 lastAttacker is the killer GameEvent_PlayerDeath
    // reads [orig: @0x516f6b]; the edge's fallback has already emptied it for
    // a body nothing ever hit. killer_handle stays at its default: retail's
    // unstamped entity+704.
    d.killer = e.last_attacker;
    d.killer_handle = 0;
    world.round_sim.deaths.push_back(d);
    e.alive = false;
    e.health = 0;
}

int EntityCommands::kill_group(int group) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (!e) continue;
        // Only the LIVING cross the edge — retail's `(Flags & 2) == 0` half. A
        // group killed twice must not notify twice.
        if (e->health > 0 && (e->flags & kEntityFlagDead) == 0)
            raise_scripted_death(world_, *e, h);
        else { e->alive = false; e->health = 0; }
        ++n;
    }
    return n;
}

int EntityCommands::group_to_waypoint(int group, int32_t wp, int32_t node) {
    // [orig: Entity_SetWaypointByTeam @0x43cdb4 — commandGroup match over pools 0..1]
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (e) { apply_waypoint_order(world_, *e, wp, node); ++n; }
    }
    return n;
}

int EntityCommands::set_group_hp(int group, int32_t hp) {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *e = world_.registry.get(h);
        if (!e) continue;
        // Setting a group to zero health is a kill by another name, and retail's
        // motor edge cannot tell the two apart — it only sees the zero. Same
        // notify, same unstamped killer.
        if (hp <= 0 && e->health > 0 && (e->flags & kEntityFlagDead) == 0) {
            raise_scripted_death(world_, *e, h);
            ++n;
            continue;
        }
        e->health = hp;
        e->alive = hp > 0;
        ++n;
    }
    return n;
}

int EntityCommands::set_group_engage_min(int group, int32_t distance_q16) {
    return set_group_range(world_, group, AiSlot::kEngageMin, distance_q16);
}

int EntityCommands::set_group_engage_max(int group, int32_t distance_q16) {
    return set_group_range(world_, group, AiSlot::kSightRange, distance_q16);
}

int EntityCommands::set_group_attack_max(int group, int32_t distance_q16) {
    return set_group_range(world_, group, AiSlot::kAttackRange, distance_q16);
}

namespace {

// The single-target SSN walk the team/group/teleport commands share: first
// matching row in pool order 0,1,2; SSN 0 never matches [orig: the dcb gate
// + pools-0,1,2 walks — Entity_FindByDCBAndSetFlag @0x43db30,
// Entity_SetNetIdByParentRef @0x43d6c0, EventAction_TeleportEntityToSpawn
// @0x43e005/0x43e0bb/0x43e161]. Our local-player rows deliberately carry
// net_id 0 (the wire is handle-based), so the resolve_ssn sentinel mapping
// runs first.
EntityHandle resolve_ssn_in_pools012(const World &world, uint16_t ssn) {
    // The dcb != 0 gate and the 0..2 pool set are this walk's own; the
    // net-id lookup EntityCommands::resolve_ssn wraps has neither.
    if (ssn == 0) return EntityHandle{};
    if (ssn == EntityCommands::kLocalPlayerSsn &&
        world.cached.local_player.valid())
        return world.cached.local_player;
    for (int pool : {0, 1, 2}) {
        const size_t capacity = world.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            const EntityHandle handle =
                    EntityHandle::make(pool, static_cast<int>(slot));
            const Entity *entity = world.registry.get(handle);
            if (entity != nullptr && entity->net_id == ssn) return handle;
        }
    }
    return EntityHandle{};
}

// Marker lookup: pool 3, def type 6088, WP_NUMBER match
// [orig: the pool-3 scans @0x43dfe0..0x43e003 / @0x43d3b0..0x43d3cf].
const Entity *find_teleport_marker(const World &world, int32_t wp_number) {
    const size_t capacity = world.registry.pool_capacity(3);
    for (size_t slot = 0; slot < capacity; ++slot) {
        const Entity *marker =
                world.registry.get(EntityHandle::make(3, static_cast<int>(slot)));
        if (marker != nullptr && marker->item_id == kParticleEffectMarkerTypeId &&
            marker->wp_number == wp_number)
            return marker;
    }
    return nullptr;
}

// The split-pose translation of retail's teleport stores: retail writes the
// ONE entity pose the motor reads (Position/Yaw + savedLivePose + the body
// pose [orig: @0x43e05f..0x43e096]); our AI row carries its own fixed-point
// mirror of that pose, so a teleport must land there too or the next motor
// tick snaps back. net_saved_live_pose is retail's savedLivePose.
void sync_teleported_ai(World &world, const Entity &entity) {
    AiEntity *ae = world.ai.for_handle(entity.handle);
    if (ae == nullptr) return;
    ae->pos[0] = to_fixed(entity.position.x);
    ae->pos[1] = to_fixed(entity.position.y);
    ae->pos[2] = to_fixed(entity.position.z);
    ae->heading = bam_heading_from_mission_yaw_deg(
            static_cast<double>(entity.yaw));
    ae->pitch = bam_from_degrees_wrapped(static_cast<double>(entity.pitch));
    ae->roll = bam_from_degrees_wrapped(static_cast<double>(entity.roll));
    ae->net_saved_live_pose[0] = ae->pos[0];
    ae->net_saved_live_pose[1] = ae->pos[1];
    ae->net_saved_live_pose[2] = ae->pos[2];
}

// The per-member pose copy the two teleport actions share. Flag tree per
// retail: the single action clears 0x20000 on every pool and copies the
// marker's chute bit onto a pool-0 target [orig: @0x43e08c/@0x43e0a0
// (pool 0), @0x43e13c/@0x43e1df (pools 1/2)]; the group action clears
// 0x20000 on pools 1/2 only [orig: @0x43d47f/@0x43d4e3 — the pool-0 arm
// @0x43d404..0x43d426 goes straight to the spawn reset]. Our split homes the
// bits: Building (0x20000) lives on engine_flags alone — it is outside the
// organic low byte `flags` mirrors — while the chute bit is legacy-mirrored,
// so that one is written on both views and read merged.
void copy_marker_pose(World &world, Entity &entity, const Entity &marker,
                      bool single_action) {
    entity.position = marker.position;
    entity.yaw = marker.yaw;
    entity.pitch = marker.pitch;
    entity.roll = marker.roll;
    if (single_action || entity.handle.pool() != 0)
        entity.engine_flags &= ~kEntityFlagBuilding;
    if (single_action && entity.handle.pool() == 0 &&
        ((marker.flags | marker.engine_flags) & kEntityFlagParachute) != 0) {
        entity.flags |= kEntityFlagParachute;
        entity.engine_flags |= kEntityFlagParachute;
    }
    if (entity.handle.pool() == 0)
        entity_reset_to_spawn_state(world, entity);
    sync_teleported_ai(world, entity);
}

} // namespace

int EntityCommands::remove_group(int group) {
    // [orig: Entity_TeleportAllByNetId @0x43D5D0] Despite the shipped
    // symbol name, action 4 removes every matching row in pools 2,0,1,3.
    static constexpr int pools[] = {2, 0, 1, 3};
    if (group == 0) return 0;
    int removed = 0;
    for (int pool : pools) {
        const size_t capacity = world_.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            const EntityHandle handle =
                    EntityHandle::make(pool, static_cast<int>(slot));
            const Entity *entity = world_.registry.get(handle);
            if (entity == nullptr || static_cast<int>(entity->group_id) != group)
                continue;
            world_.ai.release(handle); // the row's brain goes with it [orig: Entity_Destroy @0x43e810]
            world_.registry.despawn(handle);
            ++removed;
        }
    }
    if (removed != 0) {
        world_.recount_group_live();
        if (world_.collision != nullptr)
            world_.collision->refresh_after_registry_change(world_);
    }
    return removed;
}

int EntityCommands::set_group_accuracy(int group, int32_t primary,
                                       int32_t secondary) {
    // [orig: WacCmd_GroupSetAccuracy @0x4F7BE0] Pool 0 only.
    int changed = 0;
    const size_t capacity = world_.registry.pool_capacity(0);
    for (size_t slot = 0; slot < capacity; ++slot) {
        const EntityHandle handle =
                EntityHandle::make(0, static_cast<int>(slot));
        const Entity *entity = world_.registry.get(handle);
        if (entity == nullptr || entity->item_id == 0 ||
            static_cast<int>(entity->group_id) != group)
            continue;
        AiEntity *ae = world_.ai.for_handle(handle);
        if (ae == nullptr) continue;
        ae->slot.f[AiSlot::kAimErrorSecondary] =
                std::max(0, 100 - primary);
        ae->slot.f[AiSlot::kAimErrorPrimary] =
                std::max(0, 100 - secondary);
        ++changed;
    }
    return changed;
}

bool EntityCommands::set_group_move_speed_kph(int group, int32_t kph) {
    if (group < 0 || group >= TriggerRelations::kGroups) return false;
    // [orig: Entity_SetMoveSpeedKPH @0x43A960] The two integer divisions
    // preserve retail's authored km/h -> 16.16 units/SECOND truncation
    // (kph x 1000/3600; the store @0x43a9a5).
    int64_t scaled = (static_cast<int64_t>(256000) * kph) / 60;
    scaled = (scaled * 256) / 60;
    // Declared residual: the group-record consumer (the AI motor's group
    // speed override) is unported, so this store has no reader yet.
    world_.script.relations.group(group).move_speed_q16_per_sec =
            static_cast<int32_t>(scaled);
    return true;
}

int EntityCommands::set_group_team(int group, int32_t team) {
    // [orig: Entity_SetTeamByNetId @0x43C680] Pools 2,0,1.
    static constexpr int pools[] = {2, 0, 1};
    int changed = 0;
    for (int pool : pools) {
        const size_t capacity = world_.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            const EntityHandle handle =
                    EntityHandle::make(pool, static_cast<int>(slot));
            Entity *entity = world_.registry.get(handle);
            if (entity == nullptr || entity->item_id == 0 ||
                static_cast<int>(entity->group_id) != group)
                continue;
            entity->team = static_cast<uint8_t>(team);
            if (AiEntity *ae = world_.ai.for_handle(handle))
                ae->team = static_cast<uint8_t>(team);
            ++changed;
        }
    }
    return changed;
}

int EntityCommands::change_group(int old_group, int new_group) {
    // [orig: Entity_UpdateNetIdReferences @0x43C5B0] Pool 0 skips dead rows;
    // pools 2 and 1 update all resolved rows, then live counts are rebuilt.
    static constexpr int pools[] = {2, 0, 1};
    int changed = 0;
    for (int pool : pools) {
        const size_t capacity = world_.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            const EntityHandle handle =
                    EntityHandle::make(pool, static_cast<int>(slot));
            Entity *entity = world_.registry.get(handle);
            if (entity == nullptr || entity->item_id == 0 ||
                static_cast<int>(entity->group_id) != old_group)
                continue;
            if (pool == 0 && (entity->flags & kEntityFlagDead) != 0)
                continue;
            entity->group_id = static_cast<uint8_t>(new_group);
            if (AiEntity *ae = world_.ai.for_handle(handle))
                ae->relmat_id = static_cast<uint16_t>(new_group);
            ++changed;
        }
    }
    world_.recount_group_live();
    return changed;
}

int EntityCommands::teleport_group_to_marker(int group,
                                             int32_t marker_wp_number) {
    // [orig: Entity_TeleportTeamToSpawn @0x43D390] The name says team,
    // but the member filter is commandGroup and the marker key is WP_NUMBER.
    const Entity *marker = find_teleport_marker(world_, marker_wp_number);
    if (marker == nullptr) return 0;
    const Entity marker_copy = *marker;
    int changed = 0;
    static constexpr int pools[] = {0, 1, 2};
    for (int pool : pools) {
        const size_t capacity = world_.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            Entity *entity = world_.registry.get(
                    EntityHandle::make(pool, static_cast<int>(slot)));
            if (entity == nullptr || entity->item_id == 0 ||
                static_cast<int>(entity->group_id) != group)
                continue;
            copy_marker_pose(world_, *entity, marker_copy, false);
            ++changed;
        }
    }
    if (changed != 0 && world_.collision != nullptr)
        world_.collision->refresh_after_registry_change(world_);
    return changed;
}

bool EntityCommands::wac_teleport_ssn(EntityTarget source, int32_t marker_wp_number) {
    // [orig: WacCmd_TeleSsn @0x4F7E00] Retail validates source, then loses
    // its pointer at 0x4F7E47. The selected MARKER self-copies its pose. Keep
    // this shipped behavior separate from BMS's actual entity teleport.
    if (world_.registry.get(resolve_target(source)) == nullptr) return false;
    const Entity *found = find_teleport_marker(world_, marker_wp_number);
    if (found == nullptr) return false;
    Entity &marker = *world_.registry.get(found->handle);
    sync_teleported_ai(world_, marker);
    if (AiEntity *body = world_.ai.for_handle(marker.handle)) {
        body->inf.body_heading = body->heading;
        body->body_pitch = body->pitch;
    }
    if (marker.item_type == 3) {
        entity_reset_to_spawn_state(world_, marker);
    } else {
        marker.flags &= ~kEntityFlagBuilding;
        marker.engine_flags &= ~kEntityFlagBuilding;
        if (world_.collision != nullptr)
            world_.collision->refresh_after_registry_change(world_);
    }
    return true;
}

bool EntityCommands::set_ssn_team(uint16_t ssn, int32_t team) {
    // [orig: Entity_FindByDCBAndSetFlag @0x43DB30] First DcbId match walking
    // pools 0,1,2 in order (no item gate; the team byte is entity+354
    // @0x43db63). The AI-row team mirror is our split-structure copy of the
    // field retail's AI reads off the entity.
    const EntityHandle handle = resolve_ssn_in_pools012(world_, ssn);
    Entity *entity = world_.registry.get(handle);
    if (entity == nullptr) return false;
    entity->team = static_cast<uint8_t>(team);
    if (AiEntity *ae = world_.ai.for_handle(handle))
        ae->team = static_cast<uint8_t>(team);
    return true;
}

bool EntityCommands::set_ssn_group(uint16_t ssn, int32_t group) {
    // [orig: Entity_SetNetIdByParentRef @0x43D6C0] First DcbId match walking
    // pools 0,1,2 in order (no item gate; the commandGroup word is entity+284
    // @0x43d6f4). The relmat mirror + recount are our derived-cache upkeep.
    const EntityHandle handle = resolve_ssn_in_pools012(world_, ssn);
    Entity *entity = world_.registry.get(handle);
    if (entity == nullptr) return false;
    entity->group_id = static_cast<uint8_t>(group);
    if (AiEntity *ae = world_.ai.for_handle(handle))
        ae->relmat_id = static_cast<uint16_t>(group);
    world_.recount_group_live();
    return true;
}

bool EntityCommands::teleport_ssn_to_marker(uint16_t ssn,
                                            int32_t marker_wp_number) {
    // [orig: EventAction_TeleportEntityToSpawn @0x43DFC0] Marker lookup is
    // pool 3/type 6088/WP_NUMBER; the target is the first SSN row walking
    // pools 0,1,2 in order (this walk keeps the item gate @0x43e036).
    const Entity *marker = find_teleport_marker(world_, marker_wp_number);
    if (marker == nullptr) return false;
    const Entity marker_copy = *marker;
    const EntityHandle handle = resolve_ssn_in_pools012(world_, ssn);
    Entity *entity = world_.registry.get(handle);
    if (entity == nullptr || entity->item_id == 0) return false;
    copy_marker_pose(world_, *entity, marker_copy, true);
    if (world_.collision != nullptr)
        world_.collision->refresh_after_registry_change(world_);
    return true;
}
bool EntityCommands::group_alive(int group) const {
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    for (EntityHandle h : members) {
        const Entity *e = world_.registry.get(h);
        if (e && e->alive) return true;
    }
    return false;
}

bool EntityCommands::group_dead(int group) const {
    return !group_alive(group);
}

// --- mount / emplacement (AttachToEmplaced) ---

bool EntityCommands::mount(EntityTarget occupant_ssn, EntityTarget target_ssn, SeatSelectionMode mode) {
    // [orig: WacScript_TryMountEntityToVehicle @0x4f70f0] resolve both; reject already-mounted /
    // seatless; pick the best seat; write both sides; pose now.
    EntityHandle oh = resolve_target(occupant_ssn);
    EntityHandle th = resolve_target(target_ssn);
    Entity *occ = world_.registry.get(oh);
    Entity *tgt = world_.registry.get(th);
    if (!occ || !tgt || occ->mounted) return false;
    VehicleSeatSelection selection;
    if (!find_best_vehicle_seat(world_, th, oh, selection, mode)) return false;
    return world_.vehicles.attach_to_seat(oh, selection);
}

bool EntityCommands::mount_boarding_command(EntityTarget occupant_ssn, EntityTarget target_ssn,
                                            uint8_t command_id) {
    SeatSelectionMode mode = SeatSelectionMode::Any;
    switch (command_id) {
        case 123:
            mode = SeatSelectionMode::PassengerOnly;
            break;
        case 124:
            mode = SeatSelectionMode::RejectController;
            break;
        case 125:
            break;
        default:
            return false;
    }
    return mount(occupant_ssn, target_ssn, mode);
}

// WAC `ssnrelease` -- the RELEASE half of the AI boarding order, and the reason a
// transported squad ever gets out again. [orig: WacCmd_SsnRelease @0x4f7420]
//
//   if ( !v3 || !v3->ItemTypeIndex || !v3->parentEntity ) return 0;
//   Entity_DetachFromVehicleIfServer(v3);
//   if ( v3->aiRuntime ) { aiRuntime[37] = 0; aiRuntime[35] = 0; }
//
// It is the exact twin of the `ssn2ssn` setter (WacCmd_SsnToSsn @0x4f7330, which arms
// aiRuntime[37]=125 + [38]=target + [36]=carrier and zeroes thinkCooldown). Retail
// has NO arrival-driven unload anywhere -- all 20 Entity_DetachFromVehicleIfServer
// call sites are death/damage, a waypoint redirect, destroy, or spawn reset -- so
// THIS script command is how a mission disembarks a transported AI. Without it the
// occupant rides to the destination and then sits at command 125 forever, which is
// exactly what our 00TRg probe showed: 12 permanently-mounted AI, every one at
// wp=125, seven of them having driven ~700 u and then stopped dead.
//
// Clearing [35] (the has-route flag) as well as [37] is witnessed and load-bearing:
// leaving the route flag set would keep the stale board route live after the detach.
bool EntityCommands::release_boarding_command(EntityTarget occupant_ssn) {
    const EntityHandle oh = resolve_target(occupant_ssn);
    Entity *occ = world_.registry.get(oh);
    // [orig: the !ItemTypeIndex and !parentEntity rejects] -- a release only applies
    // to a real item entity that is actually riding something.
    if (!occ || occ->item_type == 0 || !occ->mounted) return false;
    dismount(occupant_ssn); // [orig: Entity_DetachFromVehicleIfServer]
    if (AiEntity *ae = world_.ai.for_handle(oh)) {
        ae->slot.f[37] = 0; // [orig: aiRuntime[37] = 0 — clear the board command]
        ae->slot.f[35] = 0; // [orig: aiRuntime[35] = 0 — clear the has-route flag]
    }
    return true;
}

bool EntityCommands::use_boarding_target(EntityTarget occupant) {
    // [orig: WacScript_TryMountEntityToVehicle @0x4F70F0]
    const EntityHandle handle = resolve_target(occupant);
    Entity *entity = world_.registry.get(handle);
    AiEntity *ai = world_.ai.for_handle(handle);
    if (entity == nullptr || entity->item_id == 0 || ai == nullptr ||
            ai->slot.f[36] == 0 || entity->mount_target.valid()) return false;
    const EntityHandle target{uint16_t(ai->slot.f[36] - 1)};
    SeatSelectionMode mode = SeatSelectionMode::Any;
    if (ai->slot.f[37] == 123) mode = SeatSelectionMode::PassengerOnly;
    else if (ai->slot.f[37] == 124) mode = SeatSelectionMode::RejectController;
    VehicleSeatSelection selected;
    const bool available = find_best_vehicle_seat(world_, target, handle, selected, mode);
    ai->slot.f[36] = available ? int32_t(selected.vehicle.packed) + 1 : 0;
    if (available) world_.vehicles.attach_to_seat(handle, selected);
    if (entity->mount_target.valid()) return true;
    entity->flags &= ~kEntityFlagMounted;
    entity->engine_flags &= ~kEntityFlagMounted;
    entity->mount_type = SeatType::None;
    return false;
}

bool EntityCommands::mount_best(uint16_t occupant_ssn) {
    // [orig: EventAction_Dispatch case 0x25 @0x4542e0 -> the vehicle is occupant-model+144.]
    // Proximity proxy: the nearest entity offering a free seat within kMountRadius.
    EntityHandle oh = resolve_target(occupant_ssn);
    const Entity *occ = world_.registry.get(oh);
    if (!occ || occ->mounted) return false;
    const Vec3 p = occ->position;
    EntityHandle best;
    double best_d2 = kMountRadius * kMountRadius + 1.0;
    world_.registry.for_each([&](const Entity &e) {
        if (e.handle == oh || e.seats.empty()) return;
        VehicleSeatSelection selection;
        if (!find_best_vehicle_seat(world_, e.handle, oh, selection)) return;
        const double dx = e.position.x - p.x, dy = e.position.y - p.y, dz = e.position.z - p.z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= kMountRadius * kMountRadius && d2 < best_d2) { best_d2 = d2; best = e.handle; }
    });
    const Entity *tgt = world_.registry.get(best);
    if (!tgt) return false;
    return mount(occupant_ssn, tgt->handle);
}

bool EntityCommands::dismount(EntityTarget occupant_ssn) {
    return world_.vehicles.detach(resolve_target(occupant_ssn));
}

uint16_t EntityCommands::find_mounted_on(uint16_t target_ssn) const {
    // [orig: Vehicle_HasEnemyOccupant @0x4359f0] first occupant riding target_ssn, else 0.
    EntityHandle th = resolve_target(target_ssn);
    if (!th.valid()) return 0;
    uint16_t result = 0;
    world_.registry.for_each([&](const Entity &e) {
        if (result == 0 && e.mounted && e.mount_target == th) result = e.net_id;
    });
    return result;
}

namespace {

// The shared frame of the four Player mount triggers: resolve the SSN entity + a live
// local player. [orig: the common head of @0x4f10d0/0x4f1260/0x4f1150/0x4f11e0 — handle
// resolve, ItemTypeIndex != 0, local player set, !(Flags & 2)]
const Entity *mount_trigger_ssn(const World &w, const EntityCommands &cmds, EntityTarget ssn,
                                const Entity **local_out) {
    const Entity *local = w.registry.get(w.cached.local_player);
    if (local == nullptr || ((local->flags | local->engine_flags) & 2u) != 0)
        return nullptr;
    const Entity *target = w.registry.get(cmds.resolve_target(ssn));
    if (target == nullptr || target->item_id == 0) return nullptr;
    *local_out = local;
    return target;
}

// entity == candidate OR entity's standing-carrier == candidate (one link deep).
// [orig: `vehicle == entity || vehicle->groundEntity == entity` @0x4f113d]
bool is_or_carried_by(const World &w, EntityHandle chain_head, const Entity &candidate) {
    const Entity *head = w.registry.get(chain_head);
    if (head == nullptr) return false;
    if (head->handle == candidate.handle) return true;
    return head->ground_target == candidate.handle;
}

} // namespace

bool EntityCommands::local_player_attached_to_ssn(EntityTarget ssn) const {
    // [orig: Entity_IsLocalPlayerSeatedOnSsn @0x4f10d0 — parentEntity chain, any seat]
    const Entity *local = nullptr;
    const Entity *target = mount_trigger_ssn(world_, *this, ssn, &local);
    if (target == nullptr) return false;
    return local->mounted && is_or_carried_by(world_, local->mount_target, *target);
}

bool EntityCommands::local_player_standing_on_ssn(EntityTarget ssn) const {
    // [orig: Entity_IsLocalPlayerStandingOnSsn @0x4f1260 — groundEntity chain]
    const Entity *local = nullptr;
    const Entity *target = mount_trigger_ssn(world_, *this, ssn, &local);
    if (target == nullptr) return false;
    return is_or_carried_by(world_, local->ground_target, *target);
}

bool EntityCommands::local_player_driving_ssn(EntityTarget ssn) const {
    // [orig: Entity_IsLocalPlayerDrivingSsn @0x4f1150 — the seat chain + parentSlot 2/5]
    if (!local_player_attached_to_ssn(ssn)) return false;
    const Entity *local = world_.registry.get(world_.cached.local_player);
    return local != nullptr && is_vehicle_control_seat(local->mount_type);
}

bool EntityCommands::local_player_on_gun_of_ssn(EntityTarget ssn) const {
    // [orig: Entity_IsLocalPlayerOnGunOfSsn @0x4f11e0 — the seat chain + parentSlot 3]
    if (!local_player_attached_to_ssn(ssn)) return false;
    const Entity *local = world_.registry.get(world_.cached.local_player);
    return local != nullptr && local->mount_type == SeatType::Gunner;
}

// --- AI command (the AI-change action family) ---
// [orig: Entity_ApplyCommand @0x43ab60.] Resolve the target's brain through World::ai and
// apply the sub-type command in-engine. No AI system / no brain -> no-op.

namespace {

// Entity_ApplyCommand has a synchronous controller/entity half and a queued
// brain half. Single, group, and area targets converge here so both halves see
// the same member set. [orig: Entity_ApplyCommand @0x43ab60]
void set_mask(uint32_t &word, uint32_t mask, bool enabled) {
    if (enabled) word |= mask;
    else word &= ~mask;
}

void set_slot_mask(int32_t &word, uint32_t mask, bool enabled) {
    uint32_t bits = static_cast<uint32_t>(word);
    set_mask(bits, mask, enabled);
    word = static_cast<int32_t>(bits);
}

// The synchronous controller/entity arms of Entity_ApplyCommand.
// [orig: Entity_ApplyCommand @0x43ab60]
void apply_ai_controller_command(World &world, Entity &entity, AiEntity &ae, int sub_type,
                                 int32_t p2, int32_t p3) {
    switch (sub_type) {
        case EntityCommands::kHudItem:
        case EntityCommands::kTmateStatus:
            // No switch arms in this binary. [orig: Entity_ApplyCommand @0x43AB60]
            break;
        case EntityCommands::kTargetSsn: {
            // The helper writes brain+148. Its live pool-0 match returns
            // before storing; all other paths clear the old priority pointer.
            // [orig: Entity_ApplyCommand @0x43AB60 -> Entity_FindByNetId @0x4655B0]
            if (p2 != 0) {
                bool preserve = false;
                for (size_t slot = 0; slot < world.registry.pool_capacity(0); ++slot) {
                    const Entity *target = world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
                    if (target == nullptr || target->net_id != p2) continue;
                    preserve = ((target->flags | target->engine_flags) & 2) == 0;
                    break;
                }
                if (preserve) break;
            }
            ae.brain.f[AiBrain::kPriorityTarget] = 0;
            break;
        }
        case EntityCommands::kGuardBit: // [orig: case 2 @0x43ab9a — Flags 0x40 @0x43abae/0x43abb8]
            // Retail writes the one Flags dword; 0x40 is legacy-mirrored,
            // so both views stay coherent (the vehicle_attach precedent).
            set_mask(entity.flags, kEntityFlagMounted, p2 != 0);
            set_mask(entity.engine_flags, kEntityFlagMounted, p2 != 0);
            break;
        case EntityCommands::kRedAlert: // [orig: case 5 @0x43ac24 — ai+136 = 2 @0x43ac2d]
            ae.slot.bytes()[AiSlot::kAlertByte] = 2; break;
        case EntityCommands::kGreenAlert: // [orig: case 6 @0x43acf4 — ai+136 = 0 @0x43acfd]
            ae.slot.bytes()[AiSlot::kAlertByte] = 0; break;
        case EntityCommands::kAccuracy100:
            // ACCURACY_100: p2 == 0 is a no-op, the store clamps at zero
            // [orig: case 8 @0x43ad94 — gate @0x43ada4, 100-p2 @0x43adb1,
            //  clamp @0x43adbb].
            if (p2 != 0)
                ae.slot.f[AiSlot::kAimErrorPrimary] = std::max(0, 100 - p2);
            break;
        case EntityCommands::kBlindBit: // [orig: case 0xF @0x43aecd — bit 0x1 @0x43aede/0x43aee8]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x1u, p2 != 0);
            break;
        case EntityCommands::kBerserkBit: // [orig: case 0x10 @0x43aef6 — bit 0x200 @0x43af07/0x43af14]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x200u, p2 != 0);
            break;
        case EntityCommands::kClimberBit: // [orig: case 0x11 @0x43af25 — bit 0x400 @0x43af36/0x43af43]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], AiSlot::kClimber, p2 != 0);
            break;
        case EntityCommands::kCowardBit:
            // COWARD_BIT: 0x20000 always clears first
            // [orig: case 0x15 @0x43b06d — clear @0x43b078, bit 0x8
            //  @0x43b088/0x43b092].
            ae.slot.f[AiSlot::kBehaviorFlags] =
                    static_cast<int32_t>(
                            static_cast<uint32_t>(ae.slot.f[AiSlot::kBehaviorFlags]) &
                            ~0x20000u);
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x8u, p2 != 0);
            break;
        case EntityCommands::kYellowAlert: // [orig: case 0x16 @0x43ac80 — ai+136 = 1 @0x43ac8d]
            ae.slot.bytes()[AiSlot::kAlertByte] = 1; break;
        case EntityCommands::kClimbChase:
            // The org1 climb-chase mode flag (entity.h kEntityFlagAiClimb;
            // no dfx2med token — runtime-only sub). Legacy-mirrored like 0x40.
            // [orig: case 0x17 @0x43afae — Flags 0x80 @0x43afc2/0x43afcf]
            set_mask(entity.flags, kEntityFlagAiClimb, p2 != 0);
            set_mask(entity.engine_flags, kEntityFlagAiClimb, p2 != 0);
            break;
        case EntityCommands::kAttackDistanceValue: // [orig: case 0x29 @0x43b263 — ai+60 = p2<<16]
            ae.slot.f[AiSlot::kAttackRange] =
                    static_cast<int32_t>(static_cast<uint32_t>(p2) << 16);
            break;
        case EntityCommands::kEngageDistance:
            // ENGAGEDISTANCE MIN/MAX [orig: case 0x2A — ai+64 = p2<<16
            //  @0x43b27c, ai+68 = p3<<16 @0x43b289].
            ae.slot.f[AiSlot::kEngageMin] =
                    static_cast<int32_t>(static_cast<uint32_t>(p2) << 16);
            ae.slot.f[AiSlot::kSightRange] =
                    static_cast<int32_t>(static_cast<uint32_t>(p3) << 16);
            break;
        default: break;
    }
}

// Entity/callback arms with NO aiRuntime gate: retail writes the entity Flags dword
// for any pool-0/1 row the alert walk hands it, brain or not
// [orig: case 0x2B @0x43b20a — Flags 0x4000000 @0x43b210/0x43b21d;
//  Entity_HandleAlertCommand @0x43cf10 walks pools 0/1 without the +104
//  gate]. Every consumer (destruction, collision_resolve, round_sim) reads
// engine_flags — the retail Flags dword home. Returns true when the sub was
// this arm (the brain halves have nothing to do).
bool apply_entity_ai_command(World &world, Entity &entity, int sub_type, int32_t p2) {
    switch (sub_type) {
        case EntityCommands::kIndestructableBit:
            set_mask(entity.engine_flags, kEntityFlagIndestructible, p2 != 0);
            return true;
        case EntityCommands::kAiNodePath:
            // Both writes replace the current callback, including a death callback.
            // [orig: Entity_ApplyCommand @0x43B22A..0x43B24E]
            entity.motor_suspended = p2 != 0;
            entity.death_motion = DeathMotionMode::None;
            return true;
        case EntityCommands::kFindAndUse: {
            const AiEntity *ai = world.ai.for_handle(entity.handle);
            if (!entity.motor_suspended &&
                    (ai == nullptr || !ai->inf.active || ai->inf.is_local_player || ai->net_is_remote_peer))
                return true;
            // The zero command clears only +0x184, retaining the byte index.
            // First pool-1 raw ID match wins, even when it has no attach point.
            // [orig: Entity_FindAttachBone @0x4B9580]
            if (p2 == 0) { entity.attach_parent = {}; return true; }
            for (size_t slot = 0; slot < world.registry.pool_capacity(1); ++slot) {
                const Entity *parent = world.registry.get(EntityHandle::make(1, static_cast<int>(slot)));
                if (parent == nullptr || parent->net_id != p2) continue;
                const int index = world.pose_provider
                        ? world.pose_provider->last_named_userpoint(world, parent->handle, "attach") : 0;
                if (index > 0) {
                    entity.attach_bone = static_cast<uint8_t>(index);
                    entity.attach_parent = parent->handle;
                }
                break;
            }
            return true;
        }
        default: return false;
    }
}

// Queue-backed brain arms. Alert commands remap to event 6 levels 2/0/1;
// authored state, skill, speed, weapons-free, and elevation preserve p2 as the
// event argument (the shared tail @0x43b2ec..0x43b326).
// [orig: Entity_ApplyCommand @0x43ab60 -> AIEvent_QueueEntry @0x455da0 ->
//  AI_HandleCommand @0x465770]
void queue_ai_brain_event(AiSystem &sys, AiEntity &ae, int sub_type, int32_t p2) {
    int event_type = -1;
    int32_t argument = p2;
    switch (sub_type) {
        case EntityCommands::kRedAlert: event_type = 6; argument = 2; break;  // [orig: @0x43ac59..0x43ac77]
        case EntityCommands::kGreenAlert: event_type = 6; argument = 0; break;  // [orig: @0x43ad34..0x43ad4e]
        case EntityCommands::kYellowAlert: event_type = 6; argument = 1; break; // [orig: @0x43acc4..0x43ace2]
        case EntityCommands::kDriveSkill: event_type = 9; break;  // [orig: case 0x1A @0x43b0ad]
        case EntityCommands::kAimSkill: event_type = 8; break;  // [orig: case 0x1B @0x43b0cb]
        case EntityCommands::kAiSetState: event_type = 7; break;  // [orig: case 0x1C @0x43b0e9]
        case EntityCommands::kCombatSpeed: event_type = 10; break; // [orig: case 0x1D @0x43b107]
        case EntityCommands::kPatrolSpeed: event_type = 11; break; // [orig: case 0x1E @0x43b125]
        case EntityCommands::kAiStartFiring: event_type = 21; break; // [orig: case 0x2D @0x43b2cd]
        case EntityCommands::kAiFiringAngle: event_type = 22; break; // [orig: case 0x2E @0x43b2e4]
        default: return;
    }
    AiEventEntry ev{};
    ev.f[0] = event_type;
    // Channel 0 for the command-queued events (the combat spawn/death queue
    // sites stamp 9; this site stamps 0) [orig: event_source = 0 @0x43ac66/
    // @0x43acd1/@0x43ad41/@0x43b30e].
    ev.f[1] = sys.index_of(ae) << 16;
    ev.set_timer(0.0f);
    ev.f[3] = argument;
    sys.events.queue(ev);
}

} // namespace

bool EntityCommands::apply_ai_command(EntityTarget ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    const EntityHandle handle = resolve_target(ssn);
    Entity *entity = world_.registry.get(handle);
    if (entity == nullptr) return false;
    if (apply_entity_ai_command(world_, *entity, sub_type, p2)) return true;
    AiEntity *ae = world_.ai.for_handle(handle);
    if (ae == nullptr) return false;
    apply_ai_controller_command(world_, *entity, *ae, sub_type, p2, p3);
    queue_ai_brain_event(world_.ai, *ae, sub_type, p2);
    ai_apply_command(ae->brain, sub_type, p2, p3, p4);
    return true;
}

int EntityCommands::apply_group_ai_command(int group, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    // The alert-change subs also stamp the per-group alert record the cat-1
    // triggers read, independent of any AI brains [orig: Entity_HandleAlertCommand
    // @ 0x43cff7 maps sub 5 -> red, 6 -> green, 22 -> yellow via
    // TriggerGroup_SetAlertRed/Green/Yellow @ 0x40d630/0x40d5f0/0x40d610].
    if (group > 0 && group < TriggerRelations::kGroups) {
        if (sub_type == 5) world_.script.relations.group(group).alert = TriggerRelations::kAlertRed;
        else if (sub_type == 6) world_.script.relations.group(group).alert = TriggerRelations::kAlertGreen;
        else if (sub_type == 22) world_.script.relations.group(group).alert = TriggerRelations::kAlertYellow;
    }
    std::vector<EntityHandle> members;
    world_.registry.by_group(static_cast<uint8_t>(group), members);
    int n = 0;
    for (EntityHandle h : members) {
        Entity *entity = world_.registry.get(h);
        if (entity == nullptr) continue;
        if (apply_entity_ai_command(world_, *entity, sub_type, p2)) { ++n; continue; }
        AiEntity *ae = world_.ai.for_handle(h);
        if (ae != nullptr) {
                    apply_ai_controller_command(world_, *entity, *ae, sub_type, p2, p3);
            queue_ai_brain_event(world_.ai, *ae, sub_type, p2);
            ai_apply_command(ae->brain, sub_type, p2, p3, p4);
            ++n;
        }
    }
    return n;
}

int EntityCommands::apply_area_ai_command(int zone_area_id, int team, int sub_type,
                                          int32_t p2, int32_t p3, int32_t p4) {
    // AREA_AI_RED/BLUE: apply to the team's units inside a zone. [target = zone area id,
    // team filter: blue=1/red=2; the exact BMS zone->area mapping is grill-gated (P5).]
    const Area *a = world_.registry.area(zone_area_id);
    if (!a) return 0;
    std::vector<EntityHandle> in;
    world_.registry.in_area(a->bounds, in);
    int n = 0;
    for (EntityHandle h : in) {
        Entity *entity = world_.registry.get(h);
        if (entity == nullptr || entity->team != static_cast<uint8_t>(team)) continue;
        if (apply_entity_ai_command(world_, *entity, sub_type, p2)) { ++n; continue; }
        AiEntity *ae = world_.ai.for_handle(h);
        if (ae != nullptr) {
                    apply_ai_controller_command(world_, *entity, *ae, sub_type, p2, p3);
            queue_ai_brain_event(world_.ai, *ae, sub_type, p2);
            ai_apply_command(ae->brain, sub_type, p2, p3, p4);
            ++n;
        }
    }
    return n;
}

} // namespace opennova::world
