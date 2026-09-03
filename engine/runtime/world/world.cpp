#include <runtime/world/world.h>
#include <runtime/devtools/tick_profile.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_sound.h>

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <base/io/bam.h>

namespace opennova::world {

// Max distance (mission units) for mount_best's nearest-emplacement search — the proximity
// proxy for the occupant-model+144 vehicle link the original resolves through the entity
// hierarchy. A manned-gun soldier is placed on/next to its gun, so this is generous.

static void emit_vehicle_control(World &world, const char *kind, uint16_t target_net_id,
                                 int32_t target_bms_id, uint32_t target_spawn_origin,
                                 uint16_t target_wire_handle) {
    Effect effect;
    effect.kind = kind;
    effect.a = static_cast<int32_t>(target_net_id);
    effect.b = target_bms_id;
    effect.c = static_cast<int32_t>(target_spawn_origin);
    effect.d = static_cast<int32_t>(target_wire_handle);
    world.out.effects.push(std::move(effect));
}

void emit_vehicle_control_started(World &world, const Entity &vehicle) {
    emit_vehicle_control(world, "vehicle_control_started", vehicle.net_id, vehicle.bms_id,
                         vehicle.spawn_origin, vehicle.handle.packed);
}

void emit_vehicle_control_stopped(World &world, const Entity &vehicle) {
    emit_vehicle_control_stopped(world, vehicle.net_id, vehicle.bms_id, vehicle.spawn_origin,
                                 vehicle.handle.packed);
}

void emit_vehicle_control_stopped(World &world, uint16_t target_net_id,
                                  int32_t target_bms_id, uint32_t target_spawn_origin,
                                  uint16_t target_wire_handle) {
    emit_vehicle_control(world, "vehicle_control_stopped", target_net_id, target_bms_id,
                         target_spawn_origin, target_wire_handle);
}

bool vehicle_claim_primary_occupant(World &world, Entity &vehicle, EntityHandle occupant,
                                    SeatType seat) {
    // [orig: Entity_AttachToVehicleSlot @0x4946d0] ctrlx(2)/drvrx(5) claim +368 when it is
    // empty or already theirs (@0x4947b3..0x4947d2 / @0x4948b9..0x4948d8); UseGun(3) claims
    // only when empty (@0x494944..0x49495e); sitex passengers never touch +368.
    const bool was_empty = !vehicle.primary_occupant.valid();
    switch (seat) {
        case SeatType::Controller:
        case SeatType::Driver:
            if (!was_empty && vehicle.primary_occupant != occupant) return false;
            break;
        case SeatType::Gunner:
            if (!was_empty) return vehicle.primary_occupant == occupant;
            break;
        default:
            return false;
    }
    vehicle.primary_occupant = occupant;
    // The empty -> claimed edge is the retail engine-start edge (the per-tick spawner
    // fires once its latch sees +368 set) [orig: @0x48faad..0x48fb0c].
    if (was_empty) emit_vehicle_control_started(world, vehicle);
    return true;
}

bool vehicle_release_primary_occupant(World &world, Entity &vehicle, EntityHandle occupant) {
    // [orig: Entity_DetachFromVehicle @0x4355f0] the stop leg runs ONLY when the detaching
    // entity IS the claimant (@0x4356e9); anyone else leaving — including a second control
    // occupant — leaves the latch untouched.
    if (!vehicle.primary_occupant.valid() || vehicle.primary_occupant != occupant)
        return false;
    stop_ground_vehicle_sound(world, vehicle);
    vehicle.primary_occupant = EntityHandle{};
    emit_vehicle_control_stopped(world, vehicle);
    return true;
}

bool vehicle_prepare_weapon_slot(World &world, Entity &vehicle) {
    const int weapon_index = world.tables.weapons.index_of(vehicle.primary_weapon.c_str());
    if (weapon_index < 0 || weapon_index > 0xFF) return false;
    const uint8_t adm = static_cast<uint8_t>(weapon_index);
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(adm);
    if (weapon == nullptr) return false;
    if (vehicle.primary_weapon_slot_adm != adm) {
        vehicle.primary_weapon_slot = WeaponSlotState{};
        vehicle.primary_weapon_slot_adm = adm;
        if (weapon->clipsize < 0) {
            vehicle.primary_weapon_slot.clip = -1;
        } else {
            vehicle.primary_weapon_slot.clip = weapon->clipsize;
            vehicle.primary_weapon_slot.reserve = std::max<int32_t>(
                    0, static_cast<int32_t>(weapon->startrounds) - weapon->clipsize);
        }
    }
    return true;
}

bool vehicle_bind_use_gun_slot(World &world, Entity &occupant, Entity &vehicle) {
    if (!occupant.use_gun_slot_swapped) {
        occupant.pre_use_gun_equipped_adm_index = occupant.equipped_adm_index;
        occupant.use_gun_slot_swapped = true;
    }
    vehicle.primary_weapon_owner = occupant.handle;
    if (!vehicle_prepare_weapon_slot(world, vehicle)) {
        occupant.equipped_adm_index = 0xFF;
        return false;
    }
    occupant.equipped_adm_index = vehicle.primary_weapon_slot_adm;
    return true;
}

const WeaponSlotState *resolve_mounted_ammo_slot(
        const World &world, const Entity &mount) {
    // Retail proves the item definition and the EWeap attrib before resolving
    // ANY slot: the shared helper bails to NULL and the phase-8 writer emits
    // the zero-word form when either is missing [orig: shared helper @0x5460E0
    // (!itemDef -> 0; !(attrib & 0x20) -> 0); writer gate @0x4FFE0B]. A tool
    // world that installs authored seat specs before the item database
    // therefore resolves no slot until traits arrive.
    if (!mount.has_item_def ||
        (mount.item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    // The unredirected route is the entity's already-bound embedded MountSlot.
    // Only following the mutable route bit to another entity needs the
    // cross-entity relationship proof below.
    if (!mount.primary_weapon_slot.redirect_to_parent_slot)
        return &mount.primary_weapon_slot;
    // Stand-in note: the shared helper routes vehicles via the def+84 attrib
    // bit 0x40 [orig: Entity_GetWeaponSlots @ 0x5460E0] while the phase-8
    // writer keys def+92 type==1 [orig: @ 0x4FFE3F]; the shipped corpus stamps
    // both together on every EWeap vehicle, so type==1 serves both sites.
    if (mount.item_type == 1u)
        return &mount.primary_weapon_slot;

    // Retail follows entity+0x28 (groundEntity), not the addeweap metadata
    // pointer. Promotion/materialization capture the same relationship's live
    // generation so packed-handle reuse cannot redirect into an unrelated row.
    if (!mount.ground_target.valid() ||
        mount.emplacement_parent != mount.ground_target ||
        mount.emplacement_parent_spawn_id == 0)
        return nullptr;
    const Entity *parent = world.registry.get(mount.ground_target);
    if (parent == nullptr ||
        parent->registry_spawn_id != mount.emplacement_parent_spawn_id ||
        !parent->has_item_def || parent->item_type != 1u ||
        (parent->item_attrib & kItemAttribEweap) == 0u)
        return nullptr;
    return &parent->primary_weapon_slot;
}

WeaponSlotState *resolve_mounted_ammo_slot(World &world, Entity &mount) {
    return const_cast<WeaponSlotState *>(resolve_mounted_ammo_slot(
            static_cast<const World &>(world),
            static_cast<const Entity &>(mount)));
}

void vehicle_release_use_gun_slot(Entity &occupant, Entity *vehicle) {
    if (vehicle != nullptr && vehicle->primary_weapon_owner == occupant.handle)
        vehicle->primary_weapon_owner = EntityHandle{};
    if (!occupant.use_gun_slot_swapped) return;
    const bool is_player =
            ((occupant.flags | occupant.engine_flags) & 0x100u) != 0;
    occupant.equipped_adm_index =
            is_player ? occupant.pre_use_gun_equipped_adm_index : 0xFF;
    occupant.pre_use_gun_equipped_adm_index = 0xFF;
    occupant.use_gun_slot_swapped = false;
}

Vec3 entity_local_point_world(const Entity &vehicle, const Vec3 &local) {
    // Build the SAME frame collision serves (target_view): heading from the
    // stored mission yaw, pitch/roll BAM-wrapped from degrees, through
    // collision_matrix_from_euler [orig: @0x613f40]. Pure-yaw carriers keep the
    // pre-existing 2D rotate bit-for-bit (the euler matrix reduces to it, but
    // the trig paths differ in rounding; the fast path also skips the matrix).
    const Vec3 &L = local;
    if (vehicle.pitch == 0 && vehicle.roll == 0) {
        constexpr double kDeg2Rad = io::kRadiansPerDegree;
        const double a = static_cast<double>(-vehicle.yaw) * kDeg2Rad;
        const double ca = std::cos(a), sa = std::sin(a);
        Vec3 p;
        p.x = vehicle.position.x + static_cast<float>(L.x * ca - L.y * sa);
        p.y = vehicle.position.y + static_cast<float>(L.x * sa + L.y * ca);
        p.z = vehicle.position.z + L.z;
        return p;
    }
    const int32_t heading =
            bam_heading_from_mission_yaw_deg(static_cast<double>(vehicle.yaw));
    const int32_t origin[3] = {0, 0, 0};
    const CollisionMatrix m = collision_matrix_from_euler(
            heading,
            bam_from_degrees_wrapped(static_cast<double>(vehicle.pitch)),
            bam_from_degrees_wrapped(static_cast<double>(vehicle.roll)), origin);
    // seat_local is pre-swizzled ((-y, x, z) over the raw authored ints — a
    // baked-in Rz(90)), while the collision euler matrix with heading
    // bam(90 - yaw) expects RAW model coordinates: un-swizzle first, so the
    // flat case reduces bit-for-bit to the legacy -yaw rotate above.
    const int32_t lf[3] = {static_cast<int32_t>(L.y * 65536.0f),
                           static_cast<int32_t>(-L.x * 65536.0f),
                           static_cast<int32_t>(L.z * 65536.0f)};
    int32_t wf[3];
    m.rotate_point(lf, wf);
    Vec3 p;
    p.x = vehicle.position.x + static_cast<float>(wf[0]) / 65536.0f;
    p.y = vehicle.position.y + static_cast<float>(wf[1]) / 65536.0f;
    p.z = vehicle.position.z + static_cast<float>(wf[2]) / 65536.0f;
    return p;
}

static int16_t mounted_pose_yaw(const Entity &vehicle, const Seat &seat) {
    if (seat.attachment_frame)
        return static_cast<int16_t>(vehicle.yaw + seat.yaw_offset);
    if (seat.type == SeatType::Gunner)
        return static_cast<int16_t>(vehicle.yaw - seat.yaw_offset);
    return static_cast<int16_t>(vehicle.yaw + seat.yaw_offset);
}

void presnap_vehicle_attach_heading(World &world, Entity &occupant,
                                    const Entity &vehicle, const Seat &seat) {
    const int16_t seat_yaw = mounted_pose_yaw(vehicle, seat);
    occupant.yaw = seat_yaw;
    AiEntity *body = world.ai.for_handle(occupant.handle);
    if (body == nullptr) return;

    const int32_t seat_heading =
            bam_heading_from_mission_yaw_deg(static_cast<double>(seat_yaw));
    body->heading = seat_heading;
    // Retail has one entity Yaw. OpenNova separates the local input-owned look
    // target from the render heading, so both must receive the same attach snap.
    if (body->inf.is_local_player)
        body->inf.target_heading = seat_heading;
}

void pose_mounted_occupant(World &world, Entity &occ, const Entity &vehicle,
                           const Seat &seat) {
    MountedPose live;
    if (world.pose_provider != nullptr &&
        world.pose_provider->resolve_mounted_pose(world, vehicle, seat, live)) {
        occ.position = live.position;
        occ.yaw = live.yaw;
        occ.pitch = live.pitch;
        occ.roll = live.roll;
        return;
    }
    // The seat-local offset through the carrier's FULL orientation frame (yaw +
    // pitch + roll). Retail's seat bone path reads the one entity orientation
    // matrix, the same matrix the collision shell is posed with; a yaw-only
    // rotate here left every mounted body (and its dismount start) in an
    // unrolled frame while the collision volumes leaned with the vehicle.
    // [orig: Entity_GetBoneTransformAndOrientation @0x4b0c50 over
    //  Math_BuildFixedPointMatrixFromEulerAngles @0x613f40]
    occ.position = entity_local_point_world(vehicle, seat.seat_local);
    occ.yaw = mounted_pose_yaw(vehicle, seat);
    occ.pitch = vehicle.pitch;
    occ.roll = vehicle.roll;
}

// Attached emplacement children are allocated breadth-first after their carrier,
// so pool/slot iteration is parent-before-child even for turret-on-vehicle chains.
// Reuse the mounted-pose provider: a resolved USRP bone follows live PANM; bone
// zero takes pose_mounted_occupant's parent-root/local fallback.
static void pose_emplacement_attachments(World &world) {
    devtools::ProfileLap lap(world.profile);
    // Parent ownership ends when the carrier dies, even though ordinary item
    // destruction keeps that carrier resident as a husk. Peel orphan chains
    // without mutating registry slots during traversal.
    for (int depth = 0; depth < 8; ++depth) {
        std::vector<EntityHandle> orphans;
        world.registry.for_each([&](const Entity &candidate) {
            if (!candidate.emplacement_parent.valid()) return;
            const Entity *parent =
                    world.registry.get(candidate.emplacement_parent);
            if (parent == nullptr ||
                parent->registry_spawn_id !=
                        candidate.emplacement_parent_spawn_id ||
                !parent->alive || parent->health <= 0)
                orphans.push_back(candidate.handle);
        });
        if (orphans.empty()) break;
        for (EntityHandle orphan : orphans) {
            std::vector<EntityHandle> occupants;
            world.registry.for_each([&](const Entity &candidate) {
                if (candidate.mounted && candidate.mount_target == orphan)
                    occupants.push_back(candidate.handle);
            });
            for (EntityHandle occupant : occupants)
                entity_detach_from_vehicle(world, occupant);
            world.registry.despawn(orphan);
        }
    }
    lap.mark(devtools::Slot::SIM_ATTACHMENT_ORPHANS);
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.emplacement_parent.valid()) return;
        // A stock streamed child carries an exact absolute spawn pose, but its
        // parent/type pair can map to multiple authored addeweap slots. Only a
        // resolved attachment row may replace that wire pose with a userpoint
        // pose. Orphan ownership and mounted-rider refresh remain independent.
        if (!snapshot.emplacement_pose_metadata_resolved) return;
        Entity *child = world.registry.get(snapshot.handle);
        const Entity *parent =
                world.registry.get(snapshot.emplacement_parent);
        if (child == nullptr || parent == nullptr ||
            parent->registry_spawn_id !=
                    snapshot.emplacement_parent_spawn_id)
            return;
        Seat anchor;
        anchor.type = SeatType::Gunner;
        anchor.bone_index = child->emplacement_bone;
        anchor.seat_local = child->emplacement_local;
        anchor.yaw_offset = child->emplacement_yaw_offset;
        anchor.attachment_frame = true;
        pose_mounted_occupant(world, *child, *parent, anchor);
    });
    lap.mark(devtools::Slot::SIM_ATTACHMENT_CHILDREN);

    // A gunner riding an attached child was posed earlier in the AI system loop,
    // before the carrier moved. Refresh those occupants from the child's fresh pose.
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
            world.ai.pose_if_mounted(*body, world);
            return;
        }
        pose_mounted_occupant(
                world, *occupant, *target, target->seats[snapshot.mount_seat]);
    });
    lap.mark(devtools::Slot::SIM_ATTACHMENT_RIDERS);
}

// ----------------------------------------------------------------------------
// World
// ----------------------------------------------------------------------------

uint16_t World::next_prng16() noexcept {
    // [orig: PRNG_Next16 @0x6130a0 / @0x613140, both over
    // dword_31BFBB0] s = rol4(s + rol11(s)) ^ 1; return low word.
    const uint32_t rol11 = (prng16_state << 11) | (prng16_state >> 21);
    uint32_t next = prng16_state + rol11;
    next = ((next << 4) | (next >> 28)) ^ 1u;
    prng16_state = next;
    return static_cast<uint16_t>(next);
}

void World::add_system(ISystem *sys) {
    if (sys) systems_.push_back(sys);
}

void World::load_systems() {
    for (ISystem *s : systems_) s->on_load(*this);
}

void World::run_logic_tick(bool is_authority, TickPhase phase) {
    // The whole tick lands on SIM_SERVER_WORLD for every role (the server
    // tick, the joiner's local tick and the bare no-net tick alike); the
    // phases below lap onto the SIM_WORLD_* rows.
    const devtools::ProfileScope tick_scope(profile, devtools::Slot::SIM_SERVER_WORLD);
    devtools::ProfileLap lap(profile);
    // [orig: WacScript_AdvanceTick refreshes the per-tick local-player cache via
    // WacScript_CacheLocalPlayerState @0x4f5780 at the top of the tick, before the
    // script evaluators read it. Deferred: the mission sim has no local-player avatar
    // yet, so `cached` stays host-populated and the WAC near-* builtins read it as-is.]
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.phase = phase;
    const bool pre_mission = phase == TickPhase::PreMission;
    const bool gameplay = phase == TickPhase::Gameplay;
    rules.logic_authority = is_authority;
    // The pending fire-sound countdown, before this tick's spawns: retail
    // drains after the client network frame (whose receive seeds our embedder
    // also applies pre-tick) and before the server/entity updates that seed
    // the rest [orig: Sound_TickPendingSlots @ 0x526697 in
    // Game_ProcessMainFrame, between Client_ProcessNetworkFrame and
    // Server_TickUpdate / Entity_UpdateAllEntities].
    out.fire_sounds.tick();
    // The presenting-client identity for the spawn-time tracer style select — stamped
    // before the system loop so out.rounds spawned THIS tick (AI fire, local fire) select
    // against fresh values [orig: g_local_player_entity->Team read @ 0x4ec740].
    round_sim.local_player = cached.local_player;
    if (const Entity *lp = registry.get(cached.local_player))
        round_sim.local_team = static_cast<uint8_t>(lp->team);
    lap.mark(devtools::Slot::SIM_WORLD_SETUP);
    // The system loop runs on BOTH the authoritative host and a non-authority client
    // (the original client also runs a tick): each system self-gates on
    // ctx.is_authority. WacSystem / BmsEventSystem early-out on a client (scripting is
    // host-only; the in-match C2S drain is host-only too, owned by Server_TickUpdate, not an
    // ISystem); AiSystem on a client simulates ONLY the
    // local player (the §5.38 entity==local-player branch) and leaves every other
    // entity to the replicated wire state. [orig: the client tick still steps the
    // local player's infantry motor; Server_TickUpdate / Game_ProcessMainFrame.]
    if (phase != TickPhase::PreRound) {
        for (ISystem *s : systems_) {
            // The AI system's own phases lap onto the SIM_AI_* rows inside its
            // tick; every other registered system is an authored script.
            const devtools::ProfileScope system_scope(
                    profile, s == &ai ? devtools::Slot::SIM_WORLD_AI
                                     : devtools::Slot::SIM_WORLD_SCRIPTS);
            s->tick(*this, ctx);
        }
        lap.restart();
        pose_emplacement_attachments(*this);
        // Static attachment poses can change after AI collision queries. The
        // projectile/out.destruction half of the tick starts a fresh matrix-view
        // epoch so it never inherits a pre-attachment target transform.
        if (collision != nullptr) collision->reset_query_view_cache();
    }
    lap.mark(devtools::Slot::SIM_WORLD_ATTACHMENTS);
    // Entity_UpdateAllEntities walks pool 1 before the projectile pool. That
    // prevents a newly converted charge from losing an arm-delay tick and lets
    // claymore shrapnel fly later in its detonation frame [orig:
    // Entity_UpdatePool1Slot @0x4b8dd0 -> Weapon_UpdateAllProjectiles @0x4ec020].
    // These presentation events describe only the current authoritative tick.
    if (is_authority && gameplay) {
        throwables.events.clear();
        throwables.tick(*this, ai.collision, tables.terrain);
    }
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
    if (gameplay && cached.local_player.valid())
        weather.precipitation.fall_tick(weather.core.scalar_channels.rain_pct_fp,
                                        weather.precipitation_kind);
    lap.mark(devtools::Slot::SIM_WORLD_THROWABLES);
    // The global weapon-action pump follows the complete entity/system update and
    // precedes projectile stepping. This is where an AI UseGun nextAction write can
    // become a same-frame round.
    // [orig: Entity_UpdateAllEntities @0x52674b, then
    //  WeaponAction_ProcessAllEntities @0x526786]
    // WeaponAction_ProcessAllEntities is after the timer-gated entity update
    // and is itself ungated, so an already-queued action may advance during
    // PreRound even though its spawned projectile cannot move until gameplay.
    // PreMission remains outside the frame pump entirely.
    // [orig: Game_ProcessMainFrame @0x52672C..0x526786]
    if (phase != TickPhase::PreMission)
        ai.pump_mounted_weapon_slots(*this, logic_tick);
    lap.mark(devtools::Slot::SIM_WORLD_WEAPONS);
    // Live out.rounds step on the host and on an explicitly configured MP
    // non-authority client. The latter is the retail tag-2 visual re-sim path;
    // every decoded/predicted round carries VisualOnly through all consequence
    // sites, so only the host can mutate gameplay state. Do not infer a client
    // role from is_authority=false alone -- tests and pre-mission callers use it too.
    // [orig: Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    if (gameplay &&
        (is_authority || (rules.mp_session && !rules.projectile_authority)))
        round_sim.tick(*this, tables.terrain, ai.collision);
    lap.mark(devtools::Slot::SIM_WORLD_PROJECTILES);
    if (gameplay &&
        (is_authority || (rules.mp_session && !rules.projectile_authority))) {
        // The explosion-queue drain runs once per frame after the projectile
        // update [orig: Projectile_ProcessExplosionQueue @0x4ead80]; entries the
        // damage callbacks push (the kz death chain) land next tick, exactly like
        // the original's post-reset writes. Dead non-AI items then settle
        // [orig: the Entity_UpdateStaticDeathPhysics / _UpdateFallingDeathPhysics
        // update callbacks] and the death-piece pool advances
        // [orig: DeathPiece_TickAll @0x57b900].
        // Retail runs all three UNGATED on every peer — the shared per-frame
        // entity update calls them on clients too, which is how a joiner's
        // 0x13/0x26-triggered death chain detonates its kz blasts and flies its
        // pieces locally. The MP visual client (the round pool's predicate
        // above) therefore drains them as well; its authoritative state keeps
        // arriving over the wire regardless.
        // [orig: Entity_UpdateAllEntities @0x4c2100 — DeathPiece_TickAll
        //  @0x4c221c, Projectile_ProcessExplosionQueue @0x4c223f, and the
        //  pool-2/3 update-callback walk, all unconditional]
        // The water plane: env.water_z (16.16, the #265 sound-profile home) —
        // zero means "no water authored", the same read the wreck gates use
        // [orig: Env_WaterHeightFixed @0x26c6454].
        const float water_z =
                env.water_z != 0 ? static_cast<float>(env.water_z) / 65536.0f : -1.0e9f;
        explosions.process(*this, ai.collision, tables.terrain,
                           water_z, out.destruction);
        destruction_tick_dead_items(*this, tables.terrain, water_z, out.destruction);
        death_pieces.tick(*this, tables.terrain, water_z, out.destruction);
    }
    lap.mark(devtools::Slot::SIM_WORLD_DESTRUCTION);
    // The waypoint current-selection pass, from the local player's position (the
    // original runs it in the client frame beside the player update; our SP host
    // is that client — the pure-client view is D-HUD-16). Position converts to
    // the original's 16.16 fixed compare space. [orig: Player_UpdatePerFrame
    // @0x4de5f7]
    if (is_authority && gameplay && !script.waypoints.empty()) {
        if (const Entity *lp = registry.get(cached.local_player))
            script.waypoints.tick_advance(static_cast<int32_t>(lp->position.x * 65536.0f),
                                   static_cast<int32_t>(lp->position.y * 65536.0f));
    }
    if (is_authority) {
        if (pre_mission) {
            // The one-shot initial group recount, ordered right after the pre
            // pass [orig: Game_StartMission @ 0x525b86 -> @ 0x525b8b].
            recount_group_initials();
        } else if (--group_recount_timer_ <= 0) {
            // The 62-tick live rescan [orig: Server_TickUpdate timer
            // @ 0x51db6d, reload 0x3E @ 0x51db93 -> EntityPool_RecountLiveByGroup
            // @ 0x51dc02].
            group_recount_timer_ = 0x3E;
            recount_group_live();
        }
    }
    ++logic_tick; // [orig: current_tick @0x24c1968 advances once per frame tick]
    // Audio-less/headless hosts never drain presentation. Retire their bounded
    // latest-intent rows on the same logic clock so old entity lifetimes cannot
    // occupy mailbox admission indefinitely.
    out.sound_emitters.prune(logic_tick);
    lap.mark(devtools::Slot::SIM_WORLD_HOUSEKEEPING);
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
    // The SP tail [orig: @0x51691d..0x51698f]: stop the dialog audio channel
    // (DialogAudio_PlayNextChunkOrStop(0) @0x51694b) + Dialog_ResetAll + park the
    // mission music, then winner==1 -> the WIN epilog (Cine_InitPlayback @0x578390:
    // <mission>.cne if present, else a static camera; the flyaway + jo_Epil.tga
    // score screen) + end music track 1; anything else -> the LOSE cine
    // (Cine_StartPlayback @0x577840 letterbox/fade + the jo_Epil2.tga MISSION FAILED
    // screen) + end track 2 (MusicCtx_SelectEndTrack @0x672fd0). All host
    // presentation: the effect carries the winner, the host selects the flow.
    out.effects.push({"round_end", winning_team, 0, 0, 0, std::string()});
}

// Count alive members per commandGroup over the actor pools; group 0 is
// forced to zero [orig: EntityPool_RecountByType @ 0x40e7e0 /
// EntityPool_RecountLiveByGroup @ 0x40e8d0 — !(flags & 2) && health > 0].
static void count_groups(const EntityRegistry &registry,
                         int32_t (&counts)[TriggerRelations::kGroups]) {
    std::vector<EntityHandle> members;
    for (int g = 1; g < TriggerRelations::kGroups; ++g) {
        members.clear();
        registry.by_group(static_cast<uint8_t>(g), members);
        int32_t alive = 0;
        for (EntityHandle h : members) {
            const Entity *e = registry.get(h);
            if (e && e->alive && e->health > 0) ++alive;
        }
        counts[g] = alive;
    }
    counts[0] = 0;
}

void World::recount_group_initials() {
    int32_t counts[TriggerRelations::kGroups] = {};
    count_groups(registry, counts);
    for (int g = 0; g < TriggerRelations::kGroups; ++g) {
        script.relations.group(g).initial_count = counts[g];
        script.relations.group(g).live_count = counts[g];
    }
}

void World::recount_group_live() {
    int32_t counts[TriggerRelations::kGroups] = {};
    count_groups(registry, counts);
    for (int g = 0; g < TriggerRelations::kGroups; ++g)
        script.relations.group(g).live_count = counts[g];
}

World::Snapshot World::snapshot() const {
    Snapshot s;
    s.registry = registry;
    s.vars = script.vars;
    s.wac_values = script.wac_values;
    s.env = env;
    s.weather = weather;
    s.match = match;
    s.spawn_waves = spawn_waves;
    s.zone_capture_state = zone_capture_state;
    s.spawn_cycle_counter = spawn_cycle_counter;
    s.logic_tick = logic_tick;
    s.preround_delay_seconds = preround_delay_seconds;
    s.prng16_state = prng16_state;
    s.crt_rand_state = crt_rand.state;
    s.local_player = cached.local_player;
    return s;
}

void World::restore(const Snapshot &s) {
    registry.restore_from(s.registry);
    script.vars = s.vars;
    script.wac_values = s.wac_values;
    env = s.env;
    weather = s.weather;
    match = s.match;
    spawn_waves = s.spawn_waves;
    zone_capture_state = s.zone_capture_state;
    spawn_cycle_counter = s.spawn_cycle_counter;
    logic_tick = s.logic_tick;
    preround_delay_seconds = s.preround_delay_seconds;
    prng16_state = s.prng16_state;
    crt_rand.state = s.crt_rand_state;
    // Reset per-tick health/proximity counters, then restore only the stable
    // ownership identity captured with the registry. A post-snapshot player may
    // have reused a baseline actor's slot, while a listen baseline may already
    // contain its host player; copying the current cache or clearing ownership
    // unconditionally gets one of those cases wrong.
    cached = CachedFrameState{};
    cached.local_player = s.local_player;
    out.effects.clear();
    out.slot_sounds.clear();
    out.sound_emitters.clear();
    out.fire_sounds.clear();
    round_sim.reset();
    explosions.reset();
    throwables.reset();
    death_pieces.reset();
    destruction_rng.reset();
    out.scars.reset();
    out.terrain_scorches.reset();
    out.destruction = DestructionEvents{};
    // The baseline copy above restores the configured rules, roster, clock,
    // stats, and outcome together. This matters for SP-as-listen-server: its
    // host player and game type already exist when the play-start snapshot is
    // sealed, and reset must not reconstruct them through another seam.
    kill_stats = MissionKillStats{};
    load_systems(); // systems re-init their per-mission state
    if (collision != nullptr) collision->refresh_after_registry_change(*this);
    registry.for_each([&](const Entity &vehicle) {
        if (vehicle.primary_occupant.valid())
            emit_vehicle_control_started(*this, vehicle);
    });
}


void count_mission_units(World &world) {
    // Players (the +534 byte; our player_class != 0) count into the separate
    // player bucket the panel never draws; everything else with team >= 2 and
    // a non-zero items.def unit-class byte is one enemy unit. The original's
    // vehicle(3/4)/aircraft(9)/infantry split is fold-consumed as the total.
    // [orig: Score_ClassifyEntityForCounts @0x4fd070 — player @0x4fd074,
    //  team gate @0x4fd08d, def+0x196 gate @0x4fd09f, total @0x4fd0a8;
    //  driven over both pools by Score_CountMissionSubgoalsAndUnits @0x509e13..0x509e4a]
    int32_t total = 0;
    world.registry.for_each([&](const Entity &e) {
        if (e.player_class != 0) return;
        if (e.team < 2) return;
        if (e.item_unit_type == 0) return;
        ++total;
    });
    world.kill_stats.enemy_unit_total = total;
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
