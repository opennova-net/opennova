#include <runtime/world/vehicle_system.h>
#include <runtime/world/world.h>
#include <base/io/rotating_prng.h>
#include <runtime/devtools/tick_profile.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/vehicle_attach.h>

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <runtime/world/local_player.h>
#include <runtime/world/weapon_fire_gate.h>
#include <base/io/bam.h>

namespace opennova::world {

// Attached emplacement children are allocated breadth-first after their carrier,
// so pool/slot iteration is parent-before-child even for turret-on-vehicle chains.
// Reuse the mounted-pose provider: a resolved USRP bone follows live PANM; bone
// zero takes pose_mounted_occupant's parent-root/local fallback.
static void pose_emplacement_attachments(World &world) {
    devtools::ProfileLap lap(world.profile);
    // A carrier's death leaves its attachments in place: the ewep class update
    // hides a dead PlayerControl hull's children and its vehicle block clears
    // that hide when the hull respawns (tick_emplaced_weapon_class_update); a
    // dead carrier of any other kind keeps them riding its wreck. Destroying
    // the carrier's row destroys every child whose def carries EWeap, as the
    // child shares the carrier's refNum (EntityCommands::remove_ssn). A child
    // without EWeap keeps its pointer to the freed row: it takes that zeroed
    // row's pose (the class update's root copy) and rides whatever entity is
    // next allocated there. The pass below poses against the row's current
    // occupant.
    // [orig: Entity_UpdateTransformAndTurret @0x440CBF..0x440CE1 (the hide),
    //  @0x440EB3 (the clear), the root copy @0x4410EA..0x4411BC; Entity_Destroy
    //  @0x43E810 (the refNum walk @0x43E9CD -> EntityReference_DestroyEWeapGroup @0x546F30,
    //  memset @0x43EA70)]
    lap.mark(devtools::Slot::SIM_ATTACHMENT_ORPHANS);
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.emplacement_parent.valid()) return;
        // A stock streamed child carries an exact absolute spawn pose. Only a
        // resolved attachment row (its subType's slot on the carrier's def) may
        // replace that wire pose with a userpoint pose. The mounted-rider
        // refresh below stays independent.
        if (!snapshot.emplacement_pose_metadata_resolved) return;
        Entity *child = world.registry.get(snapshot.handle);
        const Entity *parent =
                world.registry.get(snapshot.emplacement_parent);
        if (child == nullptr || parent == nullptr) return;
        Seat anchor;
        anchor.type = SeatType::Gunner;
        anchor.bone_index = child->emplacement_bone;
        anchor.seat_local = child->emplacement_local;
        anchor.yaw_offset = child->emplacement_yaw_offset;
        anchor.attachment_frame = true;
        world.vehicles.pose_mounted_occupant(*child, *parent, anchor);
    });
    lap.mark(devtools::Slot::SIM_ATTACHMENT_CHILDREN);

    // A gunner riding an attached child was posed earlier in the AI system loop,
    // before the carrier moved. Refresh only the seat frame: the gun channel and
    // gunner look already advanced once during that body's update.
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.mounted) return;
        Entity *occupant = world.registry.get(snapshot.handle);
        const Entity *target = world.registry.get(snapshot.mount_target);
        if (occupant == nullptr || target == nullptr ||
            !target->emplacement_parent.valid() ||
            snapshot.mount_seat < 0 ||
            snapshot.mount_seat >= static_cast<int>(target->seats.size()))
            return;
        if (AiEntity *body = world.ai.for_handle(snapshot.handle)) {
            world.ai.refresh_mounted_pose(*body, world);
            return;
        }
        world.vehicles.pose_mounted_occupant(*occupant, *target, target->seats[snapshot.mount_seat]);
    });
    lap.mark(devtools::Slot::SIM_ATTACHMENT_RIDERS);
}

// ----------------------------------------------------------------------------
// World
// ----------------------------------------------------------------------------

// Precipitation_Reset precedes mission entities and consumes the shared B stream.
// [orig: Game_StartMission @ 0x524360, seed @ 0x52460B / reset @ 0x5249D4]
World::World() : commands(*this), vehicles(*this), rotor_wash(*this), zones(*this) {
	weather.precipitation.reset(&io::rotating_prng_callback, &prng16_b_state);
}

uint16_t World::next_prng16() noexcept {
    // [orig: PRNG_Next16 @0x6130a0 / @0x613140, both over
    // dword_31BFBB0] s = rol4(s + rol11(s)) ^ 1; return low word.
    return io::rotating_prng_next16(prng16_state);
}

// [orig: PRNG_Next16_B @ 0x6130F0]
uint16_t World::next_prng16_b() noexcept {
    return io::rotating_prng_next16(prng16_b_state);
}

uint16_t World::next_prng16_c() noexcept {
	return io::rotating_prng_next16(prng16_c_state);
}

void World::add_system(ISystem *sys) {
    if (sys) systems_.push_back(sys);
}

// The every-32 legs ride the same admission as the WAC tick and the quarter
// pass (the pre-round phase skips every system): no human with a live clock,
// or the SP epilog screen, holds them too. The spawn markers run first, then
// the host's player idle timers.
// [orig: Server_TickUpdate — the admission @0x51D89F..0x51D8BD, `test
//  tick,1Fh` @0x51D8C4, the Spawn_AssignOverlaySpawnPoints call @0x51D8D2, the
//  Server_UpdatePlayerBreathTimers call @0x51D8D7]
void ServerIdleLegs::tick(World &world, const TickContext &ctx) {
    if (!ctx.is_authority || ctx.phase != TickPhase::Gameplay) return;
    const bool admitted = ctx.script_admitted.has_value()
            ? *ctx.script_admitted : world.script_may_advance();
    if (!admitted || (ctx.logic_tick & 0x1Fu) != 0u) return;
    world.vehicles.tick_spawn_markers();
    if (world.entity_idle_timers != nullptr)
        world.entity_idle_timers->update_entity_idle_timers(world);
}

void World::load_systems() {
    diagnostics.clear();
    for (ISystem *s : systems_) s->on_load(*this);
    ai.on_load(*this);
}

// The pool-0 walk's tail after a live body's update: the first vehicle
// (def type 1) on the body's ground-entity chain takes the body's team and
// its berserk bit when that vehicle is player-controllable, no spawn point,
// neither busted (0x1000) nor dead, and the body is seated or nobody holds the
// vehicle (a same-team body skips the hold scan outside a session or in a
// co-op game type). Every fourth tick the vehicle's contact solve is woken.
// [orig: Entity_UpdateAllEntities -- the dead skip @0x4C2477, the
//  Entity_FindChildByDefType(body, 1, 1) call @0x4C2484 (the +0x28 walk,
//  19 hops @0x43BEC0..0x43BEDF), the attrib/Flags gates @0x4C2494..0x4C24C1,
//  the seated test @0x4C24C7, the same-team skip @0x4C24DC..0x4C2501, the
//  ten seat words @0x4C2507..0x4C251F, the refNum peer scan
//  @0x4C2521..0x4C257E, the copy @0x4C258E..0x4C25C7, `test tick,3`
//  @0x4C25CE and Entity_WakeContactSolve @0x459290 (`or [e+24h],40h; mov [e+3B8h],tick`)]
static void claim_standing_vehicle(World &world, const Entity &body) {
    if (((body.flags | body.engine_flags) & kEntityFlagDead) != 0u) return;
    Entity *vehicle = nullptr;
    Entity *node = world.registry.get(body.ground_target);
    for (int depth = 1; node != nullptr && node->has_item_def && depth < 20; ++depth) {
        if (node->item_type == 1) {
            vehicle = node;
            break;
        }
        node = world.registry.get(node->ground_target);
    }
    if (vehicle == nullptr) return;
    const uint32_t vehicle_flags = vehicle->flags | vehicle->engine_flags;
    if ((vehicle->item_attrib & kItemAttribPlayerControl) != 0 &&
            (vehicle->item_attrib & kItemAttribSpawnPoint) == 0 &&
            (vehicle_flags & 0x1000u) == 0 && (vehicle_flags & kEntityFlagDead) == 0) {
        const bool seated = body.mount_target.valid();
        bool held = false;
        if (!seated) {
            const bool skip_scan = vehicle->team == body.team &&
                    (!world.rules.mp_session ||
                     (world.match.rules().game_type & 0x10000u) != 0);
            if (!skip_scan) {
                for (const Seat &seat : vehicle->seats)
                    if (seat.occupant.valid()) held = true;
                if (!held && vehicle->ref_num != 0) {
                    const EntityHandle vh = vehicle->handle;
                    const uint8_t ref = vehicle->ref_num;
                    world.registry.for_each([&](const Entity &peer) {
                        if (held || peer.handle == vh || peer.ref_num != ref) return;
                        if (((peer.flags | peer.engine_flags) & kEntityFlagCarried) != 0) return;
                        if (peer.ground_target != vh || !peer.has_item_def ||
                                (peer.item_attrib & kItemAttribEweap) == 0)
                            return;
                        if (peer.primary_occupant.valid()) held = true;
                    });
                }
            }
        }
        if (seated || !held) {
            vehicle->team = body.team;
            AiEntity *body_brain = world.ai.for_handle(body.handle);
            AiEntity *vehicle_brain = world.ai.for_handle(vehicle->handle);
            if (body_brain != nullptr && vehicle_brain != nullptr) {
                constexpr int32_t kBerserk = 0x200;
                int32_t &flags = vehicle_brain->slot.f[AiSlot::kBehaviorFlags];
                flags = (body_brain->slot.f[AiSlot::kBehaviorFlags] & kBerserk) != 0
                        ? flags | kBerserk : flags & ~kBerserk;
            }
        }
    }
    if ((world.logic_tick & 3u) == 0) {
        vehicle->flags |= 0x40u;
        vehicle->engine_flags |= 0x40u;
        vehicle->veh.contact_wake_tick = world.logic_tick;
    }
}

// The entity update's shared tail: the pool-0/1 proximity tables, the pool-0
// walk, then the update's count -- unless the epilog screen is up, whose tail
// runs the epilog cine instead (re-read here, after the walk).
// [orig: Entity_UpdateAllEntities -- the walk @0x4C2426..0x4C2474, `cmp
//  g_EpilogScreenActive,0` @0x4C2624 (the Cinematic_EpilogUpdate tail
//  @0x4C2634), `add g_EntityUpdateCounter,esi` @0x4C2639]
static void finish_entity_update(World &world, const TickContext &ctx, devtools::ProfileLap &lap) {
    // Rebuild the pool-0/1 proximity tables once per tick, ahead of the pool-0
    // walk (the pool-2 statics table rebuilds only on its registry/instance
    // edges). Pool-0 person publication does not depend on any entity having a
    // 3DI collision instance: RoundSim still queries this snapshot on missions
    // containing only organic entities, so always rebuild the pool tables when
    // a CollisionWorld is installed.
    // [orig: Entity_UpdateAllEntities @0x4c2100 -> Entity_BuildProximityLists_Pool01
    // @0x4b9340 every tick (@0x4c240a) + Entity_BuildProximityListsFromPools
    // @0x4b8eb0 (per-entity candidate slices, every 17th tick @0x4c2416); the statics
    // table is Entity_BuildAllProximityLists @0x4c20f0 at mission start/teleport]
    if (world.ai.collision != nullptr) world.ai.collision->build_tick_tables(world);
    lap.mark(devtools::Slot::SIM_UPDATE_PROXIMITY);
    // The pool-0 walk in slot order: each organic row's +0x1C4 body update.
    const size_t pool0 = world.registry.pool_capacity(0);
    for (size_t slot = 0; slot < pool0; ++slot) {
        const Entity *row = world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
        if (row == nullptr) continue;
        AiEntity *body = world.ai.for_handle(row->handle);
        if (body == nullptr || body->brain.f[AiBrain::kOwner] == 0 || !body->inf.active)
            continue;
        world.ai.update_organic(*body, world, ctx.logic_tick);
        if (const Entity *live =
                    world.registry.get(EntityHandle::make(0, static_cast<int>(slot))))
            claim_standing_vehicle(world, *live);
    }
    lap.mark(devtools::Slot::SIM_UPDATE_WALKS);
    // The flag and bay touches the bodies' movement resolves recorded run
    // inline in retail's resolver, whose handler returns at once off the
    // authority; the authority consumes them before the pass ends.
    // [orig: Entity_ProcessWaypointInteraction @0x4AD820 (the is_authority
    //  test @0x4AD823), its caller @0x4B2FF5]
    if (ctx.is_authority) world.match.process_movement_contacts(world);
    // The powerup touches run on every peer whose resolver produced them: the
    // callback has no authority gate before the pickup [orig: the branch
    //  @0x4B2FB8..0x4B2FE5 -> PowerupAction_Pickup @0x4428A0].
    powerup_process_contacts(world, ctx);
    if (!world.epilog_screen_active()) ++world.entity_update_counter;
}

void World::update_pool1_slot(Entity &row, const TickContext &ctx) {
    row.pool1_visited = true; // [orig: Entity_UpdatePool1Slot @0x4B8DE1]
    const EntityHandle handle = row.handle;
    const uint64_t lifetime = row.registry_spawn_id;
    // Every leg may destroy the row (and a spawn may reuse its slot), so each
    // leg re-reads the same registry lifetime.
    auto live = [this, handle, lifetime]() -> Entity * {
        Entity *e = registry.get(handle);
        return e != nullptr && e->registry_spawn_id == lifetime ? e : nullptr;
    };
    AiEntity *body = ai.for_handle(handle);
    if (body != nullptr && body->brain.f[AiBrain::kOwner] == 0) body = nullptr;
    AiEntity *brain = body != nullptr && !body->inf.active ? body : nullptr;
    // A placed device's row carries its own clock, think and parent-follow
    // motor (throwables.cpp); the item update callback still follows it.
    if (ctx.is_authority && body == nullptr) {
        if (PlacedDevice *device = throwables.device_for(row)) {
            throwables.update_device(*this, *device, ai.collision, tables.terrain);
            if (Entity *e = live()) {
                update_item_destroy_fade(*this, *e);
                if (e->squib.motor) {
                    tick_squib(*this, *e);
                } else {
                    const float water_z = env.water_z != 0
                            ? static_cast<float>(env.water_z) / 65536.0f : -1.0e9f;
                    tick_item_death_motion(*this, *e, tables.terrain, water_z, out.destruction);
                }
            }
            return;
        }
    }
    // The row's class callback and its +0x2AC clock: the brain machine
    // (Entity::spawn_phase), a minefield (its age) or an item damage callback
    // (Entity::class_think_ticks). A brain row never takes the item callback.
    enum class ThinkKind { None, Brain, Minefield, Item };
    ThinkKind kind = ThinkKind::None;
    if (brain != nullptr) kind = ThinkKind::Brain;
    else if (row.minefield.think) kind = row.hidden ? ThinkKind::None : ThinkKind::Minefield;
    else if (body == nullptr && !row.is_ai_capable) kind = ThinkKind::Item;
    // A mounted non-organic brain takes the seat-follow shortcut: no think and
    // no clock step this visit (its row still runs the motor legs).
    if (brain != nullptr && !row.motor_suspended && ai.pose_if_mounted(*brain, *this))
        kind = ThinkKind::None;
    auto clock_of = [kind](Entity &e) -> int32_t * {
        switch (kind) {
        case ThinkKind::Brain: return &e.spawn_phase;
        case ThinkKind::Minefield: return &e.minefield.age;
        case ThinkKind::Item: return &e.class_think_ticks;
        default: return nullptr;
        }
    };
    const int32_t *clock = clock_of(row);
    // The think visit: the row's own blink/indoors refresh, then the class
    // callback cb(entity, 0, 0) -- on the PRE-decrement clock.
    // [orig: Entity_UpdatePool1Slot `cmp [esi+2ACh],0; jg` @0x4B8E1B..0x4B8E22,
    //  the Entity_BuildProximityList call @0x4B8E25 (CollisionWorld::refresh_blink),
    //  `call eax` @0x4B8E3C]
    if (clock != nullptr && *clock <= 0) {
        if (ai.collision != nullptr) ai.collision->refresh_blink(*this, row);
        switch (kind) {
        case ThinkKind::Brain:
            ai.think_brain(*brain, *this);
            break;
        case ThinkKind::Minefield:
            // [orig: Entity_LandmineThink @ 0x441A40]
            minefields.think(*this, row);
            break;
        case ThinkKind::Item:
            if (tables.item_death_traits.get(row.item_id) != nullptr)
                destruction_notify_item_damage(*this, row, 0);
            break;
        default:
            break;
        }
    }
    // The +0x1C4 motor legs: the vehicle mover (a joiner's prediction leg),
    // the ewep class update, an organic row's body, then the item update
    // callback (the destroy fade, the squib or the death motion).
    // [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53]
    Entity *e = live();
    if (e == nullptr) return;
    vehicles.update_motor(*e, ctx.is_authority);
    if ((e = live()) == nullptr) return;
    tick_emplaced_weapon_class_update(*this, *e);
    if (body != nullptr && body->inf.active && body->brain.f[AiBrain::kOwner] != 0)
        ai.update_organic(*body, *this, ctx.logic_tick);
    if ((e = live()) == nullptr) return;
    update_item_destroy_fade(*this, *e);
    if (e->squib.motor) {
        tick_squib(*this, *e);
    } else {
        const float water_z =
                env.water_z != 0 ? static_cast<float>(env.water_z) / 65536.0f : -1.0e9f;
        tick_item_death_motion(*this, *e, tables.terrain, water_z, out.destruction);
    }
    if ((e = live()) == nullptr) return;
    // A dropped carried object's installed fall or ride (Match owns the
    // carry objects).
    if (e->drop_motion != DropMotion::None) update_dropped_object(*this, *e);
    if ((e = live()) == nullptr) return;
    // A mounted non-organic brain re-poses on its seat after the mover ran.
    if (brain != nullptr && ctx.is_authority && brain->brain.f[AiBrain::kOwner] != 0)
        ai.refresh_mounted_pose(*brain, *this);
    // The trailing decrement, every visit whether or not the row thought.
    // [orig: `add [esi+2ACh],-1` @0x4B8EA0]
    if ((e = live()) == nullptr) return;
    if (int32_t *step = clock_of(*e); step != nullptr)
        *step = static_cast<int32_t>(static_cast<uint32_t>(*step) - 1u);
}

void World::update_all_entities(const TickContext &ctx) {
    const devtools::ProfileScope pass_scope(profile, devtools::Slot::SIM_UPDATE_ENTITIES);
    devtools::ProfileLap lap(profile);
    const bool is_authority = ctx.is_authority;
    ai.is_authority = is_authority;
    // The retail is_in_session fact is set for a listen host, a dedicated host
    // and a joiner, never for single player: the SP launch leaves it clear
    // while it runs the in-process listen server, so rules.mp_session carries
    // it.
    // [orig: g_NapiNPCtx.is_in_session -- SinglePlayer_StartMission
    //  @0x561AF0 (read back @0x561E73); the death-event arm
    //  EntityAI_ProcessAirStateMachine @0x458273]
    ai.is_in_session = rules.mp_session;
    if (ai.collision != nullptr)
        ai.collision->local_player = cached.local_player; // blink accumulation target
    // These presentation events describe only the current authoritative tick.
    if (is_authority) throwables.events.clear();

    // The SP epilog screen runs a reduced pass: every pool-1 row's pose is
    // copied into its saved pose and only a row a player drives (its occupant
    // carries Flags 0x100) is visited, with no visited clear and no parent
    // chain; HeliLift through the pool-3 walk is skipped, and the shared tail
    // follows. [orig: Entity_UpdateAllEntities -- `cmp
    //  g_EpilogScreenActive,0` @0x4C211D (the jnz @0x4C2128), the walk
    //  @0x4C239A..0x4C2408 (the Entity_UpdatePool1Slot call @0x4C2400)]
    if (epilog_screen_active()) {
        const size_t rows = registry.pool_capacity(1);
        for (size_t slot = 0; slot < rows; ++slot) {
            Entity *row = registry.get(EntityHandle::make(1, static_cast<int>(slot)));
            if (row == nullptr) continue;
            stamp_saved_live_pose(*row);
            const Entity *driver = registry.get(row->primary_occupant);
            if (driver != nullptr &&
                    ((driver->flags | driver->engine_flags) & kEntityFlagPlayer) != 0)
                update_pool1_slot(*row, ctx);
        }
        lap.mark(devtools::Slot::SIM_UPDATE_WALKS);
        finish_entity_update(*this, ctx, lap);
        return;
    }

    // The pool-1 walk: the visited byte cleared on every live row, then each
    // unvisited row in slot order, its ground-entity chain (+0x28, up to three
    // ancestors) visited first -- a deck rider moves with the carrier's
    // CURRENT motor delta. The chain is re-read after each visit.
    // [orig: Entity_UpdateAllEntities -- the clear @0x4C212E..0x4C2156, the
    //  walk @0x4C2158..0x4C21F1 (the Entity_UpdatePool1Slot calls @0x4C21B9 /
    //  @0x4C21D0 / @0x4C21E0 / @0x4C21E9)]
    const size_t pool1 = registry.pool_capacity(1);
    for (size_t slot = 0; slot < pool1; ++slot)
        if (Entity *e = registry.get(EntityHandle::make(1, static_cast<int>(slot))))
            e->pool1_visited = false;
    // The powerup rows' +0x1C4 update callback is the respawn countdown
    // [orig: Entity_TickFireTimer @0x442850, installed by sub_442D00 @0x442D20].
    powerup_tick(*this, ctx);
    auto unvisited_parent = [this](const Entity *e) -> Entity * {
        if (e == nullptr) return nullptr;
        Entity *p = registry.get(e->ground_target);
        return p != nullptr && !p->pool1_visited ? p : nullptr;
    };
    for (size_t slot = 0; slot < pool1; ++slot) {
        const EntityHandle handle = EntityHandle::make(1, static_cast<int>(slot));
        Entity *row = registry.get(handle);
        if (row == nullptr || row->pool1_visited) continue;
        if (Entity *p1 = unvisited_parent(row)) {
            if (Entity *p2 = unvisited_parent(p1)) {
                if (Entity *p3 = unvisited_parent(p2)) update_pool1_slot(*p3, ctx);
                if (Entity *again = registry.get(handle))
                    if (Entity *p1_now = registry.get(again->ground_target))
                        if (Entity *p2_now = registry.get(p1_now->ground_target))
                            update_pool1_slot(*p2_now, ctx);
            }
            if (Entity *again = registry.get(handle))
                if (Entity *p1_now = registry.get(again->ground_target))
                    update_pool1_slot(*p1_now, ctx);
        }
        if ((row = registry.get(handle)) != nullptr) update_pool1_slot(*row, ctx);
    }
    throwables.compact();
    lap.mark(devtools::Slot::SIM_UPDATE_WALKS);
    pose_emplacement_attachments(*this);
    // Static attachment poses can change after AI collision queries. The
    // projectile/out.destruction half of the tick starts a fresh matrix-view
    // epoch so it never inherits a pre-attachment target transform.
    if (collision != nullptr) collision->reset_query_view_cache();
    lap.mark(devtools::Slot::SIM_UPDATE_ATTACHMENTS);

    // [orig: Entity_UpdateAllEntities @0x4C21F6 (HeliLift_UpdateAll), then the
    //  facial interpolation @0x4C21FB]
    teammates.tick(*this);
    facials.tick(*this);
    lap.mark(devtools::Slot::SIM_UPDATE_HELILIFT_FACES);
    // The precipitation fall: while it rains every drop slot lowers by the
    // kind's per-tick amount, once per ENTITY update — retail runs it inside
    // Entity_UpdateAllEntities after the pool-1 walk and before
    // DeathPiece_TickAll, so it rides the entity update's frame gate (never
    // the 255-tick weather settle, never the pre-mission pass) and every peer
    // falls its own drops from the rain current the previous weather tick left
    // Entity_UpdateAllEntities itself returns before it without a local
    // player entity (@ 0x4c2110); its epilog-screen path skips it as well.
    // [orig: Precipitation_FallTick @ 0x5de8f0 from Entity_UpdateAllEntities
    //  @ 0x4c2214].
    if (cached.local_player.valid())
        weather.precipitation.fall_tick(weather.core.scalar_channels.rain_pct_fp,
                                        weather.precipitation_kind);
    lap.mark(devtools::Slot::SIM_UPDATE_PRECIPITATION);
    // Live out.rounds, their explosions and the death pieces step on the host
    // and on an explicitly configured MP non-authority client. The latter is
    // the retail tag-2 visual re-sim path; every decoded/predicted round
    // carries VisualOnly through all consequence sites, so only the host can
    // mutate gameplay state. Do not infer a client role from is_authority=false
    // alone -- tests and pre-mission callers use it too. Retail runs these
    // UNGATED on every peer — the shared per-frame entity update calls them on
    // clients too, which is how a joiner's 0x13/0x26-triggered death chain
    // detonates its kz blasts and flies its pieces locally.
    // [orig: Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    const bool round_host =
            is_authority || (rules.mp_session && !rules.projectile_authority);
    // The water plane: env.water_z (16.16, the #265 sound-profile home) —
    // zero means "no water authored", the same read the wreck gates use
    // [orig: g_EnvWaterHeightFixed @0x26c6454].
    const float water_z =
            env.water_z != 0 ? static_cast<float>(env.water_z) / 65536.0f : -1.0e9f;
    // [orig: DeathPiece_TickAll @0x57b900, the call @0x4c221c]
    if (round_host) death_pieces.tick(*this, tables.terrain, water_z, out.destruction);
    // The timed AI events, before this tick's projectiles: a damage event the
    // reactions queue below dispatches on the next tick.
    // [orig: Entity_UpdateAllEntities @0x4C2226 (j_AIEvent_ProcessTimedEntries)]
    ai.events.process_timed(ai, *this);
    lap.mark(devtools::Slot::SIM_UPDATE_PIECES_EVENTS);
    // The weather particles and emitters [orig: @0x4C222B / @0x4C2235].
    rotor_wash.tick();
    // The projectiles, then the explosion queue once per frame [orig: the
    // Weapon_UpdateAllProjectiles call @0x4C223A, the
    // Projectile_ProcessExplosionQueue call @0x4c223f; the queue drain
    // Projectile_ProcessExplosionQueue @0x4ead80]: entries the damage
    // callbacks push (the kz death chain) land next tick, exactly like the
    // original's post-reset writes.
    if (round_host) round_sim.tick(*this, tables.terrain, ai.collision);
    lap.mark(devtools::Slot::SIM_UPDATE_PROJECTILES);
    if (round_host)
        explosions.process(*this, ai.collision, tables.terrain, water_z, out.destruction);
    ai.apply_round_hits(*this);
    // The pool-2 cohort walk, the doors, the pool-3 cohort walk.
    // [orig: Entity_UpdateAllEntities @0x4C2244..0x4C2302; FadeEffect_UpdateAll
    //  @0x4C2307; @0x4C230C..0x4C2398]
    tick_item_event_pool(*this, 2);
    doors.tick(*this);
    tick_item_event_pool(*this, 3);
    lap.mark(devtools::Slot::SIM_UPDATE_EXPLOSIONS);
    finish_entity_update(*this, ctx, lap);
}

// The whole frame: the embedders without a server tick (the bare local
// role, a joiner) and the tests run it in one call. The pending fire-sound
// countdown opens it: retail drains the slots after the client network frame
// (a joiner's receive has already run) and before the server tick's receive,
// so a slot this frame's C2S queues starts counting on the next frame; the
// host's frame runs it ahead of its session pump instead.
// [orig: Game_ProcessMainFrame @0x5263F0 (the Sound_TickPendingSlots call
//  @0x526697), between the Client_ProcessNetworkFrame call @0x526692 and the
//  Server_TickUpdate call @0x5266B6 (its receive pump @0x51D895)]
void World::run_logic_tick(bool is_authority, TickPhase phase) {
    tick_pending_sound_slots();
    const TickContext ctx = begin_tick(is_authority, phase);
    run_script_pass(ctx);
    run_entity_pass(ctx);
}

void World::tick_pending_sound_slots() {
    out.fire_sounds.tick();
    if (local_player_state != nullptr) radar_tick_lock_tone(local_player_state->radar);
}

TickContext World::begin_tick(bool is_authority, TickPhase phase) {
    // Every part of the tick lands on SIM_SERVER_WORLD for every role (the
    // server tick, the joiner's local tick and the bare no-net tick alike);
    // the phases below lap onto the SIM_WORLD_* / SIM_UPDATE_* rows.
    const devtools::ProfileScope tick_scope(profile, devtools::Slot::SIM_SERVER_WORLD);
    devtools::ProfileLap lap(profile);
    // The WAC player cache refreshes at bytecode entry, not at this tick
    // boundary. [orig: WacScript_ExecuteBytecode @0x4F58F4]
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.phase = phase;
    for (ISystem *s : systems_) s->prepare_tick(*this);
    // Retail admits WAC and the BMS quarter pass through one outer condition.
    // Writes to ticks/humans during WAC take effect on the next admission.
    // [orig: Server_TickUpdate @0x51D8BD..0x51D8F4]
    ctx.script_admitted = script_may_advance();
    rules.logic_authority = is_authority;
    // The presenting-client identity for the spawn-time tracer style select — stamped
    // before the system loop so out.rounds spawned THIS tick (AI fire, local fire) select
    // against fresh values [orig: g_LocalPlayerEntity->Team read @ 0x4ec740].
    round_sim.local_player = cached.local_player;
    if (const Entity *lp = registry.get(cached.local_player))
        round_sim.local_team = static_cast<uint8_t>(lp->team);
    lap.mark(devtools::Slot::SIM_WORLD_SETUP);
    return ctx;
}

// The script systems (WAC, the every-32 legs, BMS) run on BOTH the
// authoritative host and a non-authority client; each self-gates on
// ctx.is_authority (scripting is host-only; the in-match C2S drain is
// host-only too, owned by Server_TickUpdate, not an ISystem).
void World::run_script_pass(const TickContext &ctx) {
    const devtools::ProfileScope tick_scope(profile, devtools::Slot::SIM_SERVER_WORLD);
    if (ctx.phase == TickPhase::PreRound) return;
    for (ISystem *s : systems_) {
        const devtools::ProfileScope system_scope(
                profile, devtools::Slot::SIM_WORLD_SCRIPTS);
        s->tick(*this, ctx);
    }
}

void World::run_entity_pass(const TickContext &ctx) {
    const devtools::ProfileScope tick_scope(profile, devtools::Slot::SIM_SERVER_WORLD);
    devtools::ProfileLap lap(profile);
    const bool pre_mission = ctx.phase == TickPhase::PreMission;
    const bool gameplay = ctx.phase == TickPhase::Gameplay;
    // The entity update, on every peer, behind the frame's admission gate.
    // [orig: Game_ProcessMainFrame @0x5263F0 (the Entity_UpdateAllEntities
    //  call @0x52674B)]
    if (gameplay && entity_update_admitted(ctx.is_authority)) update_all_entities(ctx);
    lap.restart();
    item_emitters.sync_owners(*this);
    // The waypoint current-selection pass, from the local player's position (the
    // original runs it in the client frame beside the player update; our SP host
    // is that client — the pure-client view is D-HUD-16). Position converts to
    // the original's 16.16 fixed compare space. [orig: Player_UpdatePerFrame
    // @0x4de5f7]
    if (ctx.is_authority && gameplay && !script.waypoints.empty()) {
        if (const Entity *lp = registry.get(cached.local_player))
            script.waypoints.tick_advance(static_cast<int32_t>(lp->position.x * 65536.0f),
                                   static_cast<int32_t>(lp->position.y * 65536.0f));
    }
    // The one-shot initial group recount, ordered right after the pre pass
    // [orig: Game_StartMission @ 0x525b86 -> @ 0x525b8b]. The live rescan is
    // the server tick's periodic second (Server_TickUpdate @ 0x51db6d, reload
    // 0x3E @ 0x51db93 -> the EntityPool_RecountLiveByGroup call @ 0x51dc02).
    if (ctx.is_authority && pre_mission) recount_group_initials();
	++logic_tick; // [orig: g_CurrentTick @0x24c1968 advances once per frame tick]
	// Audio-less/headless hosts never drain presentation. Retire their bounded
    // latest-intent rows on the same logic clock so old entity lifetimes cannot
    // occupy mailbox admission indefinitely.
    out.sound_emitters.prune(logic_tick);
    script.voice.refresh(*this);
    lap.mark(devtools::Slot::SIM_WORLD_HOUSEKEEPING);
}

// An unoccupied EWEAP row whose MountSlot is still hot keeps its own pump
// until the heat window closes: the window, the kick and the action clocks
// run on, but with no owner the fire handler returns before any round and the
// owner-keyed cues stay silent.
// [orig: WeaponAction_ProcessAllEntities @0x5426E9..0x542716 ->
//  WeaponAction_ProcessFrame @0x540E60 (the def test @0x540E74, the heat
//  window @0x540FED..0x541262); WeaponAction_Fire's owner test @0x542B24]
static void pump_unoccupied_weapon_slot(World &world, Entity &row, uint32_t frame_tick) {
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(row.primary_weapon_slot_adm);
    if (weapon == nullptr) return;
    WeaponFsmInputs inputs;
    inputs.owner_present = false;
    inputs.is_local = false;
    inputs.is_authority = world.ai.is_authority;
    inputs.auto_reload = true;
    inputs.current_tick = static_cast<int32_t>(frame_tick);
    weapon_fire_environment_inputs(world, row, inputs);
    WeaponFsmEvents events;
    weapon_fsm_tick(weapon->action_fsm, row.primary_weapon_slot, inputs, events);
    weapon_sound_publish(world, row, weapon->action_fsm, events);
}

void World::pump_weapon_actions() {
    // A joiner's borrowers are its replica rows: it walks its own replica slots
    // (tick_replica_weapon_slots) and leaves their links alone here.
    if (rules.mp_session && !rules.projectile_authority) return;
    const devtools::ProfileScope scope(profile, devtools::Slot::SIM_WEAPON_WALK);
    // Every WeaponAction_ProcessFrame reads the frame's `tick`, which the
    // entity pass's tail has already stepped past.
    // [orig: current_tick @0x24C1968, advanced at the head of the frame @0x5265B4]
    const uint32_t frame_tick = logic_tick - 1u;
    // Pool 0 in slot order, each live row's weapon slot (+0x118; a UseGun
    // gunner's is the parent's MountSlot it borrowed): the local player's
    // through the installed LocalPlayer's pump, every other through the AI.
    // [orig: WeaponAction_ProcessAllEntities @0x5426A6..0x5426C9 -- the slot
    //  read @0x5426AD, the live test @0x5426B9, the WeaponAction_ProcessFrame
    //  call @0x5426C1]
    const EntityHandle local = cached.local_player;
    const size_t pool0 = registry.pool_capacity(0);
    for (size_t slot = 0; slot < pool0; ++slot) {
        const EntityHandle handle = EntityHandle::make(0, static_cast<int>(slot));
        if (local_player_state != nullptr && handle == local) {
            local_player_state->pump_local_weapon();
            continue;
        }
        if (Entity *row = registry.get(handle)) ai.pump_gunner_slot(*this, *row, frame_tick);
    }
    // Then pool 1 in slot order: a row with no occupant (+0x170), an EWEAP
    // definition and a slot whose heat window (+0x14) is still open.
    // [orig: WeaponAction_ProcessAllEntities @0x5426CB..0x54271E -- the
    //  occupant test @0x5426E9, the live test @0x5426F2, the EWEAP bit
    //  @0x5426FF, the slot @0x542704, the window @0x54270E, the call
    //  @0x542716]
    const size_t pool1 = registry.pool_capacity(1);
    for (size_t slot = 0; slot < pool1; ++slot) {
        Entity *row = registry.get(EntityHandle::make(1, static_cast<int>(slot)));
        if (row == nullptr) continue;
        ai.release_stale_gunner_link(*this, *row);
        if (row->primary_occupant.valid() || !row->has_item_def ||
                (row->item_attrib & kItemAttribEweap) == 0 ||
                row->primary_weapon_slot.heat_window_end_tick == 0)
            continue;
        pump_unoccupied_weapon_slot(*this, *row, frame_tick);
    }
}

// Shared semantic half of Server_ProcessRoundEnd @0x5164f0. The authority
// transport emits the per-recipient wire transaction from this frozen result.
void World::process_round_end(int32_t winning_team) {
    if (!match.finish(winning_team, *this)) return; // double-run guard + frozen board [orig: @0x516502/@0x516528]
    // Match::finish applies the winner marker and builds the immutable board in
    // the original pre-send order [orig: GameEvent_ProcessScoring @0x52f550;
    // Server_BuildEndOfRoundScoreboard @0x508f30]. The authority net tail sends,
    // per active slot in state 6: S2C 0x61 round-end marker (4 zero bytes)
    // @0x516790, S2C 0x1D
    // scoreboard header [u8 winner][s16 score0][s16 score1][u8 draw][s8 myEntryIndex]
    // (EndRoundScoreboard_SerializeHeader @0x505280) @0x516839, CNetPlayer_SetGameState(11)
    // @0x516846, slot state 6->7 @0x51685e; then the per-team round-win counters for
    // game types 0x10000/65537/65540 @0x5168a0 and the MP-only 2790-tick linger
    // @0x5166c4 (drained by Server_TickUpdate -> exit reason 3 / the client frame ->
    // reason 4; SP never drains it — the epilog owns the SP exit).
    // Match::finish sets the sole outcome latch before the network/presentation
    // tails, matching retail's double-run guard without copying its global.
    // The tick the round ended on: the SP lose cine that the tail starts halts
    // the script from the following frames on (World::epilog_screen_active).
    round_end_tick = logic_tick;
    // The SP tail [orig: @0x51691d..0x51698f]: stop the dialog audio channel
    // (DialogAudio_PlayNextChunkOrStop(0) @0x51694b) + Dialog_ResetAll + park the
    // mission music, then winner==1 -> the WIN epilog (Cine_InitPlayback @0x578390:
    // <mission>.cne if present, else a static camera; the flyaway + jo_Epil.tga
    // score screen) + MusicCtx_SelectEndTrack(1) @0x51696b; anything else -> the
    // LOSE cine (Cine_StartPlayback @0x577840 letterbox/fade + the jo_Epil2.tga
    // MISSION FAILED screen) + MusicCtx_SelectEndTrack(2) @0x51698f. The end
    // track is the value the gamemus MessageHandler receives through the VM's
    // restart frame (mus_vm_signal: sub_672E50 @0x672e95; retail gamemus.bin
    // dispatches 1 -> Missionwin, 2 -> Missionlose); the gamemus context is
    // open in SP (Game_StartMission @0x525581 opens it on the is_client bit,
    // which mode 3 single player carries). All host presentation: the effect
    // carries the winner (a) and the end track (b); the shell selects the flow
    // and, in SP only, signals the track after the cine starts.
    const int32_t end_track = winning_team == 1 ? 1 : 2;
    out.effects.push({"round_end", winning_team, end_track, 0, 0, std::string()});
}

void World::show_objective_notification(int32_t slot, int32_t is_win, int32_t is_active,
                                        uint8_t flag) {
    // An inactive notice does nothing [orig: HUD_ShowObjectiveNotification
    //  @0x5ba2f3].
    if (is_active == 0) return;
    // The directive keys off the header text id of the slot: WinConditions for
    // a win notice, LoseConditions otherwise (the header tables cover slots
    // 1..8; the byte read past them is not modeled).
    // [orig: byte_A7628B[slot] @0x5ba30e / byte_A76293[slot] @0x5ba343]
    const bool in_table = slot >= 1 && slot <= 8;
    const int32_t text_id = !in_table ? 0
            : is_win != 0 ? script.subgoals.win_text_ids[slot]
                          : script.subgoals.lose_text_ids[slot];
    // The two chat lines post on a client or outside a session: the
    // presentation composes gametext Misc/STRMISC_NEWOBJECTIVE and the
    // directive (hud_game_text.h objective_directive).
    // [orig: the gate @0x5ba382/@0x5ba38b; Chat_AddMessageChannel1 @0x5ba3ae
    //  (STRMISC_NEWOBJECTIVE) and @0x5ba3c2 (the directive)]
    if (rules.mp_session_peer || !rules.mp_session)
        out.effects.push({"objective", slot, is_win != 0 ? 1 : 0, text_id, 0, std::string()});
    // The authority relays it: S2C 0x3F kind 0.
    // [orig: @0x5ba3ca..0x5ba3df -> Server_BroadcastEntityActionPacket @0x5080d0]
    if (rules.logic_authority) {
        HudRelay relay;
        relay.kind = 0;
        relay.slot = slot;
        relay.is_win = is_win;
        relay.is_active = is_active;
        relay.flag = flag;
        out.hud_relays.push_back(std::move(relay));
    }
}

void World::relay_mission_text_chat(int32_t team, const std::string &key) {
    // [orig: GameMsg_AddChatLineAndRelay @0x5ba170 — is_authority @0x5ba19f,
    //  is_in_session @0x5ba1a8, the Server_BroadcastEntityActionPacket(1, key,
    //  team) call @0x5ba1c3]
    if (!rules.logic_authority || !rules.mp_session) return;
    HudRelay relay;
    relay.kind = 1;
    relay.team = team;
    relay.key = key;
    out.hud_relays.push_back(std::move(relay));
}

// The two group recounts walk pools 2, 0 and 1 in that order (the three
// pools both retail tallies visit); pools 3 and 4 never count.
static constexpr int kGroupRecountPools[] = {2, 0, 1};

// Tally every populated row of the three pools by its command group, with
// NO flags/health test, force group 0 to zero, then copy live = initial for
// every group [orig: EntityPool_RecountByType @0x40E7E0: the zero loop
// @0x40e7f0/@0x40e7f7, the tallies @0x40e834 (pool 2) / @0x40e867 (pool 0) /
// @0x40e89a (pool 1), initial[0]=0 @0x40e8a7, the live=initial copy
// @0x40e8b1..0x40e8c4].
void World::recount_group_initials() {
    int32_t counts[TriggerRelations::kGroups] = {};
    for (int pool : kGroupRecountPools) {
        registry.for_each_in_pool(pool, [&](const Entity &e) {
            if (e.group_id < TriggerRelations::kGroups) ++counts[e.group_id];
        });
    }
    counts[0] = 0;
    for (int g = 0; g < TriggerRelations::kGroups; ++g) {
        script.relations.group(g).initial_count = counts[g];
        script.relations.group(g).live_count = counts[g];
    }
}

// The live rescan: only rows that are not dead (Flags & 2 clear) and hold
// health > 0 count; group 0 forced to zero. Every death writer sets the dead
// flag on engine_flags, so the flag word is the predicate, not the `alive`
// latch. [orig: EntityPool_RecountLiveByGroup @0x40E8D0: the predicate
// @0x40e926 (pool 2) / @0x40e96c (pool 0) / @0x40e9b6 (pool 1), live[0]=0
// @0x40e9d5]
void World::recount_group_live() {
    int32_t counts[TriggerRelations::kGroups] = {};
    for (int pool : kGroupRecountPools) {
        registry.for_each_in_pool(pool, [&](const Entity &e) {
            if (e.group_id >= TriggerRelations::kGroups) return;
            if (((e.flags | e.engine_flags) & kEntityFlagDead) != 0) return;
            if (e.health <= 0) return;
            ++counts[e.group_id];
        });
    }
    counts[0] = 0;
    for (int g = 0; g < TriggerRelations::kGroups; ++g)
        script.relations.group(g).live_count = counts[g];
}

World::Snapshot World::snapshot() const {
    Snapshot s;
    s.registry = registry;
    s.vars = script.vars;
    s.wac_values = script.wac_values;
    s.squad_events = script.squad_events;
    s.weapon_input = script.weapon_input;
    s.voice = script.voice.snapshot();
    s.initial_script_effects = out.script_effects;
    s.initial_script_sounds = out.script_sounds;
    s.initial_slot_sounds = out.slot_sounds;
    s.next_script_effect_order = out.next_script_effect_order;
    s.forced_animation = script.forced_animation;
    s.diagnostics = diagnostics;
    s.reverb = reverb;
    s.env = env;
    s.weather = weather;
    s.doors = doors;
    s.facials = facials;
    s.teammates = teammates;
    s.vehicle_ai_spawn_phase = vehicle_ai_spawn_phase;
    s.match = match;
    s.kill_stats = kill_stats;
    s.subgoals = script.subgoals;
    s.spawn_waves = zones.spawn_waves;
    s.zone_capture_state = zones.capture;
    s.spawn_cycle_counter = zones.spawn_cycle_counter;
    s.logic_tick = logic_tick;
    s.preround_delay_seconds = preround_delay_seconds;
    s.prng16_state = prng16_state;
    s.prng16_b_state = prng16_b_state;
	s.prng16_c_state = prng16_c_state;
	s.cease_fire = rules.cease_fire;
	s.crt_rand_state = crt_rand.state;
    s.local_player = cached.local_player;
    return s;
}

void World::restore(const Snapshot &s) {
    registry.restore_from(s.registry);
    script.vars = s.vars;
    script.wac_values = s.wac_values;
    reverb = s.reverb;
    env = s.env;
    weather = s.weather;
    doors = s.doors;
    facials = s.facials;
    match = s.match;
    zones.spawn_waves = s.spawn_waves;
    zones.capture = s.zone_capture_state;
    zones.spawn_cycle_counter = s.spawn_cycle_counter;
    logic_tick = s.logic_tick;
    preround_delay_seconds = s.preround_delay_seconds;
    prng16_state = s.prng16_state;
    prng16_b_state = s.prng16_b_state;
	prng16_c_state = s.prng16_c_state;
	rotor_wash.clear();
	registry.for_each([&](const Entity &e) { registry.get(e.handle)->veh.rotor_wash_handle = 0; });
	rules.cease_fire = s.cease_fire;
	minefields.remote_actors.clear();
    crt_rand.state = s.crt_rand_state;
    // Reset per-tick health/proximity counters, then restore only the stable
    // ownership identity captured with the registry. A post-snapshot player may
    // have reused a baseline actor's slot, while a listen baseline may already
    // contain its host player; copying the current cache or clearing ownership
    // unconditionally gets one of those cases wrong.
    cached = CachedFrameState{};
    cached.local_player = s.local_player;
    out.effects.clear();
    out.script_effects = s.initial_script_effects;
    out.script_sounds = s.initial_script_sounds;
    out.next_script_effect_order = s.next_script_effect_order;
	out.vehicle_effects.clear();
	out.slot_sounds = s.initial_slot_sounds;
	out.sound_emitters.clear();
    out.fire_sounds.clear();
	out.source_fires.clear();
    out.rounds.clear();
	round_sim.reset();
	explosions.reset();
    throwables.reset();
    death_pieces.reset();
    destruction_rng.reset();
    item_emitters.reset();
    out.scars.reset();
    out.terrain_scorches.reset();
    out.destruction = DestructionEvents{};
    out.entity_events.clear();
    out.hud_relays.clear();
    out.powerup_grants.clear();
    out.powerup_weapon_grants.clear();
    // The baseline copy above restores the configured rules, roster, clock,
    // stats, and outcome together. This matters for SP-as-listen-server: its
    // host player and game type already exist when the play-start snapshot is
    // sealed, and reset must not reconstruct them through another seam.
    // The SP score block and the subgoal masks come back at their play-start
    // values: the block zeroed after the PreMission pass and then censused,
    // the masks as the PreMission pass left them — the state retail's restart
    // rebuilds by re-running Game_StartMission.
    // [orig: Game_RestartRoundSP @0x5263a0 — EventSystem_FreeAll's mask clears
    //  @0x453356..0x453368, Server_ResetRoundCounters @0x516c5e, the census
    //  @0x525d5d]
    kill_stats = s.kill_stats;
    script.subgoals = s.subgoals;
    load_systems(); // systems re-init their per-mission state
    teammates = s.teammates;
    vehicle_ai_spawn_phase = s.vehicle_ai_spawn_phase;
    script.heli_lift_active_count = int32_t(teammates.count());
    // on_load clears the WAC input arrays. A sealed play-start baseline may
    // already contain controls authored by the initial script execution.
    script.weapon_input = s.weapon_input;
    script.voice.restore(s.voice);
    script.squad_events = s.squad_events;
    script.forced_animation = s.forced_animation;
    diagnostics = s.diagnostics; // retain boot gaps, discard findings from the previous playthrough
    if (collision != nullptr) collision->refresh_after_registry_change(*this);
    registry.for_each([&](const Entity &vehicle) {
        if (vehicle.primary_occupant.valid())
            vehicles.emit_control_started(vehicle);
    });
}


void count_mission_units(World &world) {
    // Players (the +534 byte; our player_class != 0) count into the separate
    // player bucket the panels never draw; everything else with team >= 2 and
    // a non-zero items.def unit-class byte is one enemy unit, split per class
    // (the by-player kill tally grows a class past its count). Pools 0 then 1
    // only: the pool-2/3/4 rows are never counted.
    // [orig: Score_ClassifyEntityForCounts @0x4fd070 — player @0x4fd074,
    //  team gate @0x4fd08d, def+0x196 gate @0x4fd09f, total @0x4fd0a8, the
    //  class split @0x4fd0c7..0x4fd0dc; Score_CountMissionSubgoalsAndUnits
    //  zeroes the four @0x509dfb..0x509e0d and walks pool 0 @0x509e13, then
    //  pool 1 @0x509e40]
    MissionKillStats &ks = world.kill_stats;
    ks.enemy_unit_total = 0;
    ks.enemy_vehicle_total = 0;
    ks.enemy_infantry_total = 0;
    ks.enemy_aircraft_total = 0;
    const auto classify = [&](const Entity &e) {
        if (e.player_class != 0) return;
        if (e.team < 2) return;
        if (static_cast<uint8_t>(e.item_unit_type) == 0) return;
        ++ks.enemy_unit_total;
        switch (score_unit_class(e.item_unit_type)) {
            case ScoreUnitClass::Vehicle: ++ks.enemy_vehicle_total; break;
            case ScoreUnitClass::Aircraft: ++ks.enemy_aircraft_total; break;
            case ScoreUnitClass::Infantry: ++ks.enemy_infantry_total; break;
        }
    };
    world.registry.for_each_in_pool(0, classify);
    world.registry.for_each_in_pool(1, classify);
}

int32_t count_defined_subgoals(const World &world) {
    // The leading run of authored win conditions before the first 0 or 0xFF
    // entry, at most eight. [orig: Score_CountMissionSubgoalsAndUnits @0x509dc2..0x509dd1 scanning
    //  the header win-condition ids; the count lands in dword_C8468C]
    int32_t count = 0;
    for (int slot = 1; slot <= 8; ++slot) {
        const uint8_t id = world.script.subgoals.win_text_ids[slot];
        if (id == 0 || id == 0xFF) break;
        ++count;
    }
    return count;
}

} // namespace opennova::world
