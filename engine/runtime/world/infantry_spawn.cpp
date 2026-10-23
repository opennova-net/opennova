// Organic spawn reset and quota-driven corpse lifecycle.
// [orig: Entity_ResetToSpawnState @0x4B9610;
//        Entity_UpdateInfantryAI @0x4B9E4D..0x4BA071]
#include <runtime/world/entity_spawn.h>

#include <algorithm>
#include <array>
#include <cmath>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/local_player.h> // LocalPlayer::view / radar (the class init's local resets)
#include <runtime/world/player_spawn.h> // player_item_row (the bound row's class)
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
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

bool player_watches(World &world, AiSystem &ai, const Entity &entity,
                    const EntityPosition &position) {
    if (ai.is_in_session || world.rules.mp_session) return false;
    const Entity *player = world.registry.get(world.cached.local_player);
    if (player == nullptr) return false;
    // The ray runs from the body's Position words, or its spawn backup's
    // (+0x318..+0x320), to the local player's Position words, with no float
    // view (D-NET-507). [orig: Entity_UpdateInfantryAI - edi = Position
    //  @0x4B9A5C into the ray @0x4B9F74..0x4B9F77; `lea ecx, [esi+318h]`
    //  @0x4B9FDF into the ray @0x4B9FE8; the player's +4 @0x4B9F70 / @0x4B9FDB]
    const int32_t from[3] = {position.x.q16, position.y.q16, position.z.q16};
    const int32_t to[3] = {player->position.x.q16, player->position.y.q16,
                           player->position.z.q16};
    return ai.line_of_sight_clear(world, from, to, entity.handle, player->handle);
}

} // namespace

// See entity_spawn.h.
void release_corpse_effect(World &world, Entity &entity) {
    if (entity.death_effect_active[0] != 0)
        release_death_effect_bank(entity, 1, world.out.destruction);
}

namespace {

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

// Entity_WarmUpOrganicAnimation on the body's channels as they stand: only for
// a body holding its animation channel; the DcbId's (+0x7C) count of dual updates, the
// vertical root motion and capsule bottom added to an unparented body's z, then
// one ground solve that sets it down when it stands under one unit.
// [orig: Entity_WarmUpOrganicAnimation @0x4B8B20 - the channel test
//  @0x4B8B32..0x4B8B39, the loop @0x4B8B3F..0x4B8BA3, the parent test
//  @0x4B8BA5, the resolver @0x4B8BDF, the settle @0x4B8BE7..0x4B8BF5; the count's
//  `mov eax, [esi+7Ch]` @0x4B8B3F]
void warm_up_organic_body(World &world, AiSystem &ai, Entity &entity, AiEntity &body,
                          uint32_t dcb_id) {
    InfantryState &inf = body.inf;
    if (!infantry_has_anim_channel(inf, ai.root_motion)) return;
    RootMotionFrame frame{};
    const bool parented = entity.mount_target.valid();
    // The entity's own Position words (D-NET-507).
    EntityPosition &position = body.self->position;
    position.z.q16 = io::bam_add(position.z.q16,
            warm_up_channels(inf, dcb_id, parented, *ai.root_motion, ai.anim_rings, frame));
    if (parented) return;
    body.collide_state.skip_counter = 0;
    if (ai.collision != nullptr) {
        std::array<int32_t, 3> pos = position.words();
        // The resolver's eye point is the body's CameraOffset words as they stand: only the
        // mover writes them, so a body that never ran one (a fresh add, a mission's organic)
        // holds its spawn memset's 0, the eye at the origin; never the resolver's
        // derived capsule height, which only a row without the words takes (D-NET-503).
        // [orig: Entity_MovementCollisionResolver @0x4B2BD0 - CameraOffset +0x6C / +0x70 /
        //  +0x74 read @0x4B2EE2..0x4B2EFB; Entity_SpawnFromAnimSlotProperty - the memset
        //  @0x43C3FE]
        const int32_t eye_offset[3] = {inf.eye_offset_x, inf.eye_offset_y, inf.eye_offset_z};
        // The resolver takes the player bit from the body's own Flags (0x100): a player
        // body's warm-up tests the player-only volumes and the opening doors' sections that
        // an NPC's skips.
        // [orig: Entity_MovementCollisionResolver @0x4B2BD0 - (Flags >> 8) & 1 @0x4B2C10..0x4B2C13
        //  (stored @0x4B2C26), the query mask `lea ecx, [edx+ecx*2]` @0x4B2F79]
        const bool player_class = ((entity.flags | entity.engine_flags) & kEntityFlagPlayer) != 0;
        const int32_t clearance = ai.collision->resolve_entity(
                world, entity.handle, body.collide_state, pos.data(),
                inf.vel, inf.vel[2], frame.capsule_bottom, frame.capsule_top,
                body.heading, 0, player_class, ai.is_authority, world.logic_tick,
                inf.anim_state, infantry_anim_flags(inf.anim_state), body.health,
                nullptr, nullptr, eye_offset);
        position.set_q16({pos[0], pos[1], pos[2]});
        if (clearance < kOrganicWarmupSettleQ16)
            position.z.q16 = io::bam_sub(position.z.q16, clearance);
    }
    body.collide_state.skip_counter = 0;
}

} // namespace

// See entity_spawn.h.
void warm_up_player_body(World &world, Entity &entity) {
    AiEntity *body = world.ai.for_handle(entity.handle);
    if (body == nullptr || !body->inf.active) return;
    // The placement wrote the entity's words; the warm-up works on them. Its count reads the
    // DcbId (+0x7C): the plyr class init stamps 10000 + the slot byte, which ours keeps off
    // Entity::net_id (player bodies carry their net id there; world::body_dcb_id); the
    // organic init and an item row write none, so their body keeps the spawn memset's 0.
    // [orig: PlayerClass_InitEntity @0x4B1060 - @0x4B1149..0x4B1173; Entity_InitOrganicAI
    //  @0x4BFCC0; Entity_SpawnFromAnimSlotProperty @0x43C390 - the memset @0x43C3FE]
    const uint32_t dcb_id = player_item_row(world, entity).body_class == PlayerBodyClass::kPlayer
            ? 10000u + entity.player_slot_id
            : 0u;
    warm_up_organic_body(world, world.ai, entity, *body, dcb_id);
}

// See entity_spawn.h.
void player_class_init_animation(World &world, Entity &entity) {
    if (player_item_row(world, entity).body_class != PlayerBodyClass::kPlayer) return;
    AiSystem &ai = world.ai;
    AiEntity *body = ai.for_handle(entity.handle);
    if (body == nullptr || !body->inf.active) return;
    InfantryState &inf = body->inf;
    // The init's word run stores the hold and body state words (43, idle), the pending word
    // (0), the torso roll (+0x2DC) and the lean (+0xB0) ahead of its channel test, so a body
    // holding no channel takes them too. No respawn stores the roll or the lean
    // (InfantryState::reset_for_spawn); the joiner's own 0x18 rebuild takes all five here, its
    // memset stopping short of +0x2B8.
    // [orig: PlayerClass_InitEntity @0x4B1060 - +0x2C8 @0x4B115B, +0x2BC @0x4B1161, +0x2B8
    //  @0x4B1176, +0x2DC @0x4B11B2, +0xB0 @0x4B11B8, the channel test @0x4B116D / @0x4B11D0;
    //  NapiNPClientMsg_FullEntitySpawn @0x433780 - `push 2B4h` @0x43381A, the memset @0x433821,
    //  the lean @0x433E5C, the class init @0x433E82]
    inf.wpn_state = anim_state::kIdle;
    inf.anim_state = anim_state::kIdle;
    inf.anim_pending = 0;
    inf.torso_roll = 0;
    inf.lean_angle = 0;
    if (!infantry_has_anim_channel(inf, ai.root_motion)) return;
    // The channel starts its hold and body clips on those words (its first dual update plays
    // them). [orig: PlayerClass_InitEntity @0x4B115B / @0x4B1161 / @0x4B1176]
    begin_spawn_channels(inf, anim_state::kIdle, ai.root_motion, ai.anim_rings);
    // The count is the organic warm-up's arithmetic over +0x78, recomputed each pass from a
    // word nothing in the loop writes. [orig: @0x4B11D2..0x4B11E8, @0x4B1200..0x4B1225]
    RootMotionFrame frame{};
    const uint32_t steps = organic_warmup_updates(entity.owner_connection_id);
    for (uint32_t i = 0; i < steps; ++i)
        infantry_dual_update(inf, ai.root_motion, ai.anim_rings, frame);
    ai.mirror_wire_anim(*body, world);
}

// See entity_spawn.h. The init's model init (the call @0x4B10C8) restamps what the 0x18's own
// stamped from the same words, and its dialog call (Player_QueueDialogSound @0x5293C0, the call
// @0x4B1144, eventData -1) empties a queue nothing else fills (its one caller), so neither has a
// store here. The stores read nothing the others write: ours runs them in retail's order.
void player_class_init_words(World &world, Entity &entity) {
    if (player_item_row(world, entity).body_class != PlayerBodyClass::kPlayer) return;
    AiEntity *body = world.ai.for_handle(entity.handle);
    if (body == nullptr || !body->inf.active) return;
    InfantryState &inf = body->inf;
    // [orig: PlayerClass_InitEntity @0x4B1060 - `cmp g_LocalPlayerEntity, esi` @0x4B10D0,
    //  g_CameraShakeCounter @0x4B10D8, g_LockOnMeToneTicks @0x4B10DE]
    if (entity.handle == world.cached.local_player && world.local_player_state != nullptr) {
        world.local_player_state->view.shake.counter = 0;
        world.local_player_state->radar.lock_tone = 0;
    }
    // [orig: `mov word ptr [esi+160h], 3` @0x4B1105; AdmDef_FindIndexByName called @0x4B110E,
    //  `mov [esi+2B0h], al` @0x4B1116]
    constexpr int16_t kPlayerClassAmmoDefWord = 3;
    entity.ammo_def_word = kPlayerClassAmmoDefWord;
    entity.equipped_adm_index = player_spawn_equipped_default(world);
    // The raise is a signed word compare against the ceiling; Armor takes def+0x17E, the word
    // the traits sweep binds as mana_max.
    // [orig: Entity_RaiseHealthToMax @0x43C290 (called @0x4B112B) - `cmp [esi+11Eh], ax`
    //  @0x43C2A1, `jge` @0x43C2A8; the def word `mov cx, [eax+17Eh]` @0x4B1133, the store
    //  @0x4B113D]
    const int32_t ceiling = max_health_with_difficulty(world, entity);
    if (retail_signed_i16(entity.health) < ceiling) entity.health = ceiling;
    body->health = static_cast<int16_t>(entity.health);
    entity.mana = entity.mana_max;
    // The word run over the Yaw [orig: `mov eax, [esi+10h]` @0x4B1167].
    const int32_t yaw = entity.heading;
    inf.arms_dip_ticks = 0;    // +0x371 [orig: @0x4B117C]
    inf.reload_anim_ticks = 0; // +0x372 [orig: @0x4B1182]
    // [orig: +0x2E4 / +0x2E8 @0x4B118E / @0x4B1194, +0x2D4 / +0x2D8 @0x4B119A / @0x4B11A0]
    inf.leg_target[0] = inf.leg_target[1] = yaw;
    inf.leg_yaw[0] = inf.leg_yaw[1] = yaw;
    body->collide_state.skip_counter = 0; // +0x373 [orig: @0x4B11A6]
    // +0x8C, the body heading and savedLiveYaw [orig: @0x4B11AC]
    inf.body_heading = yaw;
    entity.saved_live_yaw = yaw;
    entity.saved_live_roll = 0; // +0x94 [orig: @0x4B11BE]
    entity.roll_bam = body->roll = 0; // [orig: Roll @0x4B11C4]
    // +0x90, savedLivePitch and the body pitch [orig: @0x4B11C7]
    entity.saved_live_pitch = 0;
    body->body_pitch = 0;
    // The Pitch, which the local body's input accumulates [orig: @0x4B11CD;
    //  Input_HandleActionBinding_0 @0x4E0420 - `add [eax+14h], edx` @0x4E0D39]
    entity.pitch_bam = body->pitch = 0;
    if (inf.is_local_player) inf.look_pitch = 0;
}

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
    // The warmup runs only for a body holding its animation channel.
    // [orig: Entity_WarmUpOrganicAnimation @0x4B8B32..0x4B8B39]
    if (infantry_has_anim_channel(inf, source)) {
        RootMotionFrame frame{};
        out.rise = warm_up_channels(inf, net_id, facts.parented, *source, rings, frame);
        out.capsule_bottom = frame.capsule_bottom;
        out.capsule_top = frame.capsule_top;
    }
    out.pose = infantry_body_pose(inf);
    out.channels = inf;
    return out;
}

namespace {

// The organic init's legs over a pool-0 body holding its motor (initialize_organic_ai's for a
// placed person, player_body_class_init's for a player body bound to an org row).
// [orig: Entity_InitOrganicAI @0x4BFCC0]
void organic_init_body(World &world, AiEntity *body, Entity &entity, uint32_t dcb_id) {
    AiSystem &ai = world.ai;
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
    entity.pitch_bam = entity.roll_bam = 0;
    // [orig: Entity_InitOrganicAI @0x4BFCC0 - +0x2DC @0x4BFD8D, +0xB0 @0x4BFD93 (ecx = 0
    //  @0x4BFD38)]
    inf.torso_roll = inf.lean_angle = 0;
    inf.arms_dip_ticks = inf.reload_anim_ticks = 0;
    inf.head_look_target = inf.last_look_target = inf.previous_look_target = {};
    entity.dragger = {};
    entity.dragger_spawn_id = 0;
    body->collide_state.skip_counter = 0;
    inf.goal_z = body->self->position.z.q16; // [orig: Entity_InitOrganicAI @0x4BFE07]
    inf.magazine = static_cast<int16_t>(body->profile.clip_size);

    OrganicSpawnFacts facts;
    // A body with no aidata reads no route here (a slot of zeros: a player body's AI slot is
    // ours' zeroed one): retail reads the null slot and faults (PERMANENT, ratified 2026-10-21,
    // Q8-1 (a): a deliberate fix of a retail crash, authored data only).
    // [orig: Entity_InitOrganicAI @0x4BFCC0 - @0x4BFFF9 / @0x4C000C, the null test only at
    //  @0x4BFFA0]
    facts.route = body->slot.f[35] != 0;
    facts.route_channel = body->slot.f[37];
    facts.flags = flags;
    facts.rotor_wash = world.rotor_wash.nearby_zone(body->self->position.words().data(), 983040) != 0;
    if (const Entity *parent = world.registry.get(entity.mount_target)) {
        entity.ground_target = parent->ground_target;
        facts.parented = true;
        facts.parent_phrase_set = parent->has_item_def ? parent->emplaced_config : 0;
    }
    begin_spawn_channels(inf, organic_spawn_state(facts, ai.root_motion, inf.adm_id), ai.root_motion,
                         ai.anim_rings);

    // Entity_WarmUpOrganicAnimation @0x4B8B20: secondary then primary,
    // net-ID permutation, vertical root motion only, one final ground solve.
    warm_up_organic_body(world, ai, entity, *body, dcb_id);
    // The source is savedLivePose, deliberately not the grounded position; the
    // offset is 3 u along the heading in the Q22 trig of retail's
    // radians-per-BAM double. [orig: Entity_InitOrganicAI @0x4C01CD..0x4C022C,
    //  `fmul dbl_7C3608` @0x4C01DC]
    std::copy_n(entity.saved_live_pos, 3, inf.aim_point);
    const double radians = double(body->heading) * io::kRadiansPerBamX87;
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

// The arty row's class init over a person body's words: it zeroes +0x2B8..+0x2C8, then stores
// the tick in +0x2BC (the primary channel's request), 10 shells in +0x2C4 (the secondary's
// deferred state), the +0x26C ammo row's +8 / +0xC words (max_age_ticks / arm_age_ticks) in
// +0x2C0 (the pending death anim) / +0x2C8 (the secondary's state), and the ARTYRND row's index
// (0 on a miss) in the byte +0x2B0 (the equipped weapon byte). On a person the +0x26C word is
// the motor's advanced-launch store, the spawn memset's 0. The deploy reset rewrites +0x2B8,
// +0x2BC and +0x2C8 and leaves the 10, which the barrage counts down (person_item_mover).
// [orig: ArtyClass_InitFromDef @0x443430 - the row @0x443441, the zeroes @0x443449..0x443461,
//  +0x2BC @0x44346D, +0x2C4 @0x443473, +0x2C0 @0x443480, +0x2C8 @0x44348E, AmmoDef_LookupByName
//  @0x409870 (0 on a miss @0x4098AC) called @0x443494, +0x2B0 @0x44349C;
//  Entity_ResetToSpawnState @0x4B9610 - +0x2B8 @0x4B96F1, +0x2BC @0x4B9708 / @0x4B9714, +0x2C8
//  @0x4B972A]
void arty_class_init(World &world, AiEntity &body, Entity &entity) {
    auto &inf = body.inf;
    const AmmoTableEntry *row = world.tables.ammo.by_index(inf.last_advanced_ammo);
    inf.anim_pending = 0;
    entity.net_anim_pending = 0;
    inf.anim_state = static_cast<int>(world.logic_tick);
    entity.death_anim_state = row != nullptr ? row->max_age_ticks : 0;
    inf.wpn_deferred = 10;
    inf.wpn_state = row != nullptr ? row->arm_age_ticks : 0;
    const int artyrnd = world.tables.ammo.index_of("ARTYRND");
    entity.equipped_adm_index = static_cast<uint8_t>(artyrnd < 0 ? 0 : artyrnd);
}

// The aflr and gflr rows' class inits over a person body's words. Each reads the +0x170 link
// first, which is null at a body's spawn (the spawn's memset, and no store before the init), so
// their linked arms (the link's motion or velocity into the body's) do not run there. aflr
// re-copies the def's heatsig into +0x1A4 (the model init's copy of the same word, so it holds),
// sets +0x1C to -1 and stores the terrain's bilinear height under the Position words in +0x88
// (savedLivePose's Z); gflr sets the velocity triple (+0x98 / +0x9C / +0xA0, the motor's) to
// (0, 0, 1336) and stores the same height in +0x88. The terrain sampler reads 0 without a grid.
// Ours does not write aflr's -1: our item_type_index is also the body's def row
// (player_item_row), where retail keeps the def pointer (+0x20) apart from the index word
// (D-NET-NEW-L4-14 (a), narrowed).
// [orig: AflrClass_InitFromDef @0x4434B0 - the link @0x4434B5..0x4434BD, +0x1A4 @0x4434EA, +0x1C
//  @0x4434F6, Terrain_SampleHeightBilinear @0x6067B0 called @0x4434FD, +0x88 @0x443505;
//  GflrClass_InitFromDef @0x443510 - the link @0x443515..0x44351F, the null arm
//  @0x443564..0x443570, the call @0x443582, +0x88 @0x44358A; Entity_SpawnFromAnimSlotProperty
//  @0x43C390 - the memset @0x43C3FE, the init @0x43C4FE]
void flare_class_init(World &world, AiEntity &body, Entity &entity, bool ground_flare) {
    if (ground_flare) {
        body.inf.vel[0] = 0;
        body.inf.vel[1] = 0;
        body.inf.vel[2] = 1336;
    } else {
        entity.heat_sig = player_item_row(world, entity).heat_sig;
    }
    entity.saved_live_pos[2] = world.tables.terrain != nullptr
            ? terrain::height_field_height_bilinear_fixed(*world.tables.terrain,
                      entity.position.x.q16, entity.position.y.q16)
            : 0;
}

} // namespace

// Initial org1 callback, after definition/ADM/collision resources are bound.
// Respawn uses the separate 12-step callback below. [orig: @0x4BFCC0]
void initialize_organic_ai(World &world, Entity &entity) {
    AiEntity *body = world.ai.for_handle(entity.handle);
    if (body == nullptr || !body->inf.active || entity.handle.pool() != 0 ||
            body->inf.is_local_player || body->net_is_remote_peer ||
            ((entity.flags | entity.engine_flags) & kEntityFlagPlayer) != 0) return;
    organic_init_body(world, body, entity, uint32_t(body->net_id));
}

// See entity_spawn.h.
void player_body_class_init(World &world, Entity &entity) {
    switch (player_item_row(world, entity).body_class) {
    case PlayerBodyClass::kPlayer:
        player_class_init_animation(world, entity);
        return;
    case PlayerBodyClass::kOrganic: {
        AiEntity *body = world.ai.for_handle(entity.handle);
        if (body == nullptr || !body->inf.active || entity.handle.pool() != 0) return;
        // The warm-up counts by the DcbId (+0x7C), which the organic init writes none of: a
        // player body's stays the spawn memset's 0 (the plyr init alone stamps it).
        // [orig: Entity_WarmUpOrganicAnimation @0x4B8B20 - `mov eax, [esi+7Ch]` @0x4B8B3F;
        //  Entity_SpawnFromAnimSlotProperty @0x43C390 - the memset @0x43C3FE]
        organic_init_body(world, body, entity, 0u);
        return;
    }
    case PlayerBodyClass::kItem: {
        // The item row's init over the body's words: the arty row's (D-NET-NEW-L4-23 (a)) and
        // the two flare rows' (D-NET-NEW-L4-14 (a)); the other rows' inits are not run here.
        // [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - `mov eax, [ecx+148h]` @0x43C4F3,
        //  the call @0x43C4FE]
        const ItemClassInit init = player_item_row(world, entity).class_init;
        if (init == ItemClassInit::kNone) return;
        AiEntity *body = world.ai.for_handle(entity.handle);
        if (body == nullptr || !body->inf.active || entity.handle.pool() != 0) return;
        if (init == ItemClassInit::kArty)
            arty_class_init(world, *body, entity);
        else
            flare_class_init(world, *body, entity, init == ItemClassInit::kGroundFlare);
        return;
    }
    }
}

void entity_reset_to_spawn_state(World &world, AiSystem &ai, Entity &entity) {
    // The reset's one raise is to the ceiling with the difficulty term (the local
    // player's out of a session), no def-word raise ahead of it [orig:
    // Entity_ResetToSpawnState @0x4B97BC -> Entity_RaiseHealthToMax @0x43C290 ->
    // Entity_GetMaxHealthWithDifficulty]; a row with no definition keeps the def word.
    entity_reset_to_spawn_state(world, ai, entity, entity.has_item_def
            ? max_health_with_difficulty(world, entity) : entity.health_max);
}

void entity_reset_to_spawn_state(World &world, AiSystem &ai, Entity &entity,
                                 int32_t health_ceiling) {
    // Port-side invariants, not retail words: Entity_ResetToSpawnState
    // @0x4B9610 writes no fade/destroy state. destroy_phases_q16 /
    // destroy_progress are derived values update_item_destroy_fade zeroes and
    // recomputes every call, and objective_death_scored is this port's
    // once-per-life scoring latch (retail Server_SendItemDeathState re-dispatches on the def
    // flag alone). The +0x1B0 destroy timer is NOT re-seeded here -- retail's
    // only re-seed is Entity_RespawnVehicle (VehicleSystem::respawn).
    entity.destroy_phases_q16.fill(0);
    entity.destroy_progress = 0;
    entity.objective_death_scored = false;
    // The reset reads the Position words as they stand (no float view): the
    // spawn backup (+0x318..+0x320) is a dword copy of them (D-NET-507); it
    // leaves savedLivePose's Position words (+0x80..+0x88) alone, its +0x8C /
    // +0x90 taking the Yaw word and zero in the base reset below.
    // [orig: Entity_ResetToSpawnState @0x4B9610 - the stores @0x4B9632 /
    //  @0x4B963B / @0x4B9644; +0x8C @0x4B968C, +0x90 @0x4B97DD]
    const EntityPosition origin = entity.position;
    const int32_t heading = entity.heading;
    const uint32_t flags = entity.flags | entity.engine_flags;
    AiEntity *body = ai.for_handle(entity.handle);
    if (body != nullptr) {
        // The resolver's idle-skip count (+0x373) starts over, so the resolve below runs whole
        // whatever count the body kept, rather than taking the skip band's return.
        // [orig: Entity_ResetToSpawnState @0x4B9610 - +0x373 = 0 @0x4B9656, ahead of the
        //  resolver call @0x4B9755; Entity_MovementCollisionResolver @0x4B2BD0 - the count's
        //  read @0x4B2CBC, the skip return @0x4B2CF5, its clear @0x4B2D09]
        body->collide_state.skip_counter = 0;
        for (int axis = 0; axis < 3; ++axis) body->inf.vel[axis] = 0;
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

        // Dead-flag callers retain their current body: no state request, no dual
        // update, no resolve and no spawn effect. A respawn that clears the dead bit
        // first takes the spawn-pose leg: the NPC respawn restores its saved live
        // flags, the player deploy clears the bit itself (Server_ReleasePlayerDeployment);
        // the joiner's record respawn arm calls the reset on its still-dead L, so it
        // takes none of it (JoinerRole::land_local_respawn_arm).
        // [orig: Entity_ResetToSpawnState @0x4B9610 - `test byte ptr [esi+24h], 2`
        //  @0x4B96ED, its jnz @0x4B96F7; Server_DeployPlayer @0x517740 - the clear
        //  @0x51787A ahead of the call @0x517899; NetPacket_SerializePlayerState
        //  @0x4C09C0 - the dead test @0x4C1109, the call @0x4C113C with no Flags store
        //  between]
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
                std::array<int32_t, 3> pos = body->self->position.words();
                // After the twelve dual updates the body resolves where it stands, over
                // the slice its last build left (the reset's own build is its tail, below),
                // and a clearance at or below zero lifts it by that much. The eye point is
                // the body's CameraOffset words as they stand; the reset writes none (as
                // the warm-up's, warm_up_organic_body).
                // [orig: Entity_ResetToSpawnState @0x4B9610 - the resolver call @0x4B9755,
                //  `cmp eax, ebx; jg` @0x4B975D..0x4B975F, `sub [esi+0Ch], eax` @0x4B9761;
                //  Entity_MovementCollisionResolver - the read @0x4B2EE2..0x4B2EFB]
                const int32_t eye_offset[3] = {
                        inf.eye_offset_x, inf.eye_offset_y, inf.eye_offset_z};
                const int32_t clearance = ai.collision->resolve_entity(
                        world, entity.handle, body->collide_state, pos.data(),
                        inf.vel, inf.vel[2], frame.capsule_bottom, frame.capsule_top,
                        heading, 0, (flags & kEntityFlagPlayer) != 0,
                        ai.is_authority, world.logic_tick, inf.anim_state,
                        infantry_anim_flags(inf.anim_state), body->health,
                        nullptr, nullptr, eye_offset);
                body->self->position.set_q16({pos[0], pos[1], pos[2]});
                if (clearance <= 0) body->self->position.z.q16 -= clearance;
            }
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

    // The reset's one raise, to the caller's ceiling [orig: Entity_ResetToSpawnState @0x4B97BC].
    entity_reset_to_spawn_state(entity, health_ceiling);
    // The backup precedes collision correction in the original reset.
    entity.spawn_position = origin;
    entity.spawn_heading = heading;
    entity.spawn_flags = flags & ~kEntityFlagDead;
    release_corpse_effect(world, entity);
    if (body != nullptr) {
        body->health = static_cast<int16_t>(entity.health);
        entity.net_anim_state = static_cast<uint8_t>(body->inf.anim_state);
        entity.net_anim_pending = body->inf.anim_pending;
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
    // The tail publishes no table: it builds every slice over the tables already published
    // (restarting the 17-tick phase), then the body's own list (D-COL-21, D-COL-23).
    // [orig: Entity_ResetToSpawnState @0x4B9610 - Entity_BuildProximityListsFromPools called
    //  @0x4B98EB, Entity_BuildProximityList called @0x4B98F1]
    if (ai.collision != nullptr) ai.collision->refresh_entity_proximity(world, entity);
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

// The 186-tick decay both corpse legs spawn into the corpse's +0x1CC slot: a group the
// corpse holds there is released first, then the def's decay effect spawns at the corpse's
// Position, oriented (0, 0, -0.5) and tagged with the corpse, so the group takes the section
// gate. The slot keeps the group for its next release; it stays where it spawned, as the
// victim-hit emitter in the same slot does (spawn_victim_hit_emitter): the pool-0 walk re-poses
// no +0x1CC emitter, only the pool-1 and pool-2 walks do (Entity_UpdateAllEntities @0x4C2100 -
// the pool-0 walk @0x4C2443..0x4C261F; CEffect_UpdateEmitterTransform called @0x4B8E76 /
// @0x4C22FA).
// [orig: org1 Entity_UpdateInfantryAI @0x4B9910 - the release @0x4B9EB7..0x4B9ED1, the tag
//  @0x4B9F0F, the orientation word 0xFFFF8000 @0x4B9F24, CEffectWorld_SpawnEmitterAtPosition
//  called @0x4B9F36, the slot store @0x4B9F3E; the org2 twin Entity_UpdateInfantryPlayerBody
//  @0x4B40E0 - the release @0x4B4DB9..0x4B4DD3, the tag @0x4B4E12, the orientation word
//  @0x4B4E27, the call @0x4B4E39, the store @0x4B4E41]
static void spawn_corpse_decay(World &world, Entity &entity, const std::string &effect_name) {
    release_corpse_effect(world, entity);
    DestructionEffectEvent effect;
    effect.effect = effect_name;
    effect.pos = entity.position;
    effect.dir = {0.0f, 0.0f, -0.5f};
    effect.attach_net_id = entity.net_id;
    effect.attach_bms_id = entity.bms_id;
    effect.attach_wire_handle = entity.handle.packed;
    effect.attach_spawn_origin = entity.spawn_origin;
    effect.family = 1;
    effect.section_tagged = true;
    effect.positioned = true;
    entity.death_effect_active[0] = 1;
    world.out.destruction.effects.push_back(std::move(effect));
}

// Corpse expiry is the shared destroy: the facial slot, scar entries and incoming references go
// with the row, not only the registry slot and the brain. The destroy notifies no peer: no S2C
// 0x12 rides it (the remove-and-notify is the script, vehicle and disconnect paths', never a
// corpse leg's); a joiner tracking the row is despawned by the priority list's freed-row test
// instead.
// [orig: Entity_UpdateInfantryAI @0x4B9F93 -> Entity_Destroy @0x43E810 (no send among its
//  callees); the updater then leaves through loc_4BFC89 @0x4B9F9B; Server_RemoveEntityAndNotify
//  @0x50A270 has no caller in Entity_UpdateInfantryAI; the freed-row despawn
//  Server_BuildEntityPriorityList @0x50E7F3; the org2 twin Entity_UpdateInfantryPlayerBody
//  @0x4B4E5F, its return @0x4B4E71]
static void destroy_corpse(World &world, AiSystem &ai, Entity &entity) {
    release_corpse_effect(world, entity);
    entity.hidden = true;
    const EntityHandle handle = entity.handle;
    world.commands.remove_ssn(handle);
    if (ai.collision != nullptr) ai.collision->refresh_after_registry_change(world);
}

NpcCorpseStep step_npc_corpse(World &world, AiSystem &ai, Entity &entity) {
    const bool silent = (entity.section_mask & 1u) != 0;
    if (entity.leave_corpse && !silent) return NpcCorpseStep::Kept;
    if (entity.corpse_timer != 0)
        entity.corpse_timer = static_cast<int32_t>(uint32_t(entity.corpse_timer) - 1u);
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    const bool decay_effect = traits != nullptr && !traits->particledeath.empty();
    // [orig: the 186-tick decay spawn @0x4B9E7D; respawn quota leg @0x4B9FA0]
    if (entity.corpse_timer == 186 && !silent && decay_effect)
        spawn_corpse_decay(world, entity, traits->particledeath);
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
        entity.heading = entity.spawn_heading;
        entity.flags = entity.engine_flags = entity.spawn_flags;
        entity_reset_to_spawn_state(world, ai, entity);
        world.recount_group_live();
        return NpcCorpseStep::Respawned;
    }
    if (!decay_effect && player_watches(world, ai, entity, entity.position)) {
        entity.corpse_timer = 62;
        return NpcCorpseStep::Kept;
    }
    destroy_corpse(world, ai, entity);
    return NpcCorpseStep::Removed;
}

// See entity_spawn.h. Unlike org1's, the step has no zero guard (it is an add of the -1 the
// update keeps in ebx), and a non-positive timer is stored back as 0 every tick.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the dead bit `test byte ptr [esi+24h], 2`
//  @0x4B4D30, LeaveCorpse `test dword ptr [edi+54h], 400000h` @0x4B4D63, section bit 0
//  @0x4B4D6C, `or ebx, 0FFFFFFFFh` @0x4B4BE6, `add [esi+148h], ebx` @0x4B4D79, `cmp ..., 0BAh`
//  @0x4B4D7F, section bit 0 @0x4B4D8F, def+0x412 @0x4B4D9C, the decay @0x4B4DA9..0x4B4E41,
//  `cmp [esi+148h], ebp; jg` @0x4B4E47..0x4B4E4D, section bit 0 @0x4B4E4F, the zero store
//  @0x4B4E56, Entity_Destroy @0x43E810 called @0x4B4E5F]
bool step_player_corpse(World &world, AiSystem &ai, Entity &entity) {
    if (((entity.flags | entity.engine_flags) & kEntityFlagDead) == 0) return false;
    const bool silent = (entity.section_mask & 1u) != 0;
    if (entity.leave_corpse && !silent) return false;
    entity.corpse_timer = static_cast<int32_t>(uint32_t(entity.corpse_timer) - 1u);
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (entity.corpse_timer == 186 && !silent && traits != nullptr &&
            !traits->particledeath.empty())
        spawn_corpse_decay(world, entity, traits->particledeath);
    if (entity.corpse_timer > 0) return false;
    entity.corpse_timer = 0;
    if (!silent) return false;
    destroy_corpse(world, ai, entity);
    return true;
}

} // namespace opennova::world
