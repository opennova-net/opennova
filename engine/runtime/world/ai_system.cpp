#include "world/ai.h"

// Split out of ai.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// The AI event queue and the AiSystem core: registration, per-entity rows, and the
// tick that drives every handler above.

#include "terrain_query/height_field.h"
#include "world/angle.h"
#include "world/body_anim.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/vehicle_part_anim.h"
#include "world/vehicle_sound.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "ai_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

namespace {

// Apply only the carrier-owned body frame. This is deliberately separate from
// pose_if_mounted's input, gunner-look, animation, and wire-state work so the
// authority can repeat the pose after its later pool-1 vehicle motor without
// advancing any of those once-per-body-tick behaviors twice.
int32_t apply_resolved_mounted_seat_frame(AiEntity &e, World &world,
                                          Entity &occupant, Entity &vehicle,
                                          const Seat &seat) {
    pose_mounted_occupant(world, occupant, vehicle, seat);
    // Capture the resolved seat orientation before an independent LOOK mirror
    // overwrites registry yaw. Keep the witnessed integer yaw conversion here:
    // the generic degree helper rounds differently at non-cardinal headings.
    const int16_t seat_yaw = occupant.yaw;
    const int16_t seat_pitch = occupant.pitch;
    const int16_t seat_roll = occupant.roll;
    const int32_t resolved_heading = static_cast<int32_t>(
            static_cast<int64_t>(90 - seat_yaw) * kBamPerDegreeInt);
    if (e.inf.active) {
        // Mirror both the direct seat-frame writes and the carried-infantry leg
        // chase snap so render and per-section collision consume one coherent
        // body frame. [orig: seat carry @0x4b654e-0x4b6575; carried body/leg
        // snap Flags & 0x100060]
        e.inf.body_heading = resolved_heading;
        e.inf.leg_yaw[0] = resolved_heading;
        e.inf.leg_yaw[1] = resolved_heading;
        e.inf.leg_target[0] = resolved_heading;
        e.inf.leg_target[1] = resolved_heading;
        e.body_pitch = bam_from_degrees_wrapped(static_cast<double>(seat_pitch));
        e.roll = bam_from_degrees_wrapped(static_cast<double>(seat_roll));
    }
    // Organics present from AiEntity.pos, not Entity.position.
    e.pos[0] = to_fixed(occupant.position.x);
    e.pos[1] = to_fixed(occupant.position.y);
    e.pos[2] = to_fixed(occupant.position.z);
    return resolved_heading;
}

// A read-applied remote player in either vehicle-control seat has the same
// split pose as the local driver: LOOK remains player/wire-owned because the
// authority vehicle motor consumes Entity::yaw, while the carried body frame
// remains seat-owned. Keep NPC drivers on the existing seat-owned path.
bool remote_player_controls_vehicle(const AiEntity &e, const Entity &occupant,
                                    const Seat &seat) {
    return e.inf.active && e.net_is_remote_peer &&
           occupant.handle.pool() == 0 && occupant.player_class != 0 &&
           is_vehicle_control_seat(seat.type);
}

} // namespace

bool AiSystem::refresh_mounted_pose(AiEntity &e, World &world) {
    Entity *occupant = world.registry.get(e.handle);
    if (occupant == nullptr || !occupant->mounted || occupant->health <= 0) return false;
    Entity *vehicle = world.registry.get(occupant->mount_target);
    if (vehicle == nullptr || occupant->mount_seat < 0 ||
        occupant->mount_seat >= static_cast<int>(vehicle->seats.size()))
        return false;
    const Seat &seat = vehicle->seats[occupant->mount_seat];
    const int32_t saved_look_heading = e.heading;
    const int32_t saved_look_pitch = e.pitch;
    const int32_t seat_heading = apply_resolved_mounted_seat_frame(
            e, world, *occupant, *vehicle, seat);

    if (e.inf.active &&
        (e.inf.is_local_player || seat.type == SeatType::Gunner ||
         remote_player_controls_vehicle(e, *occupant, seat))) {
        // Independent LOOK was already promoted/chased in pose_if_mounted. Restore
        // that exact value without consulting input latches or advancing aim again.
        e.heading = saved_look_heading;
        e.pitch = saved_look_pitch;
        occupant->yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(saved_look_heading))));
    } else {
        e.heading = seat_heading;
    }
    return true;
}

// ----------------------------------------------------------------------------
// AiEventQueue. [orig: AIEvent_QueueEntry @0x455da0 / AIEvent_ProcessTimedEntries @0x455df0.]
// ----------------------------------------------------------------------------
void AiEventQueue::queue(const AiEventEntry &e) {
    if (count_ < kMax) buf_[count_++] = e;
}

void AiEventQueue::process_timed(AiSystem &sys, World &world) {
    int index = 0;
    while (index < count_) {
        AiEventEntry &entry = buf_[index];
        float remaining = entry.timer() - kFrameDt;
        entry.set_timer(remaining);
        if (remaining <= 0.0f) {
            AiEntity *e = sys.at(entry.entity_index());
            if (e && e->brain.f[AiBrain::kOwner]) {
                AiThinkCtx ctx{&sys, e, &world, &entry};
                sys.row(e->brain.f[AiBrain::kCurState]).event(ctx); // off_815244
                int32_t pend = e->brain.f[AiBrain::kPendState];
                if (pend) {
                    int32_t cur = e->brain.f[AiBrain::kCurState];
                    if (pend != cur) {
                        sys.row(cur).exit(ctx);   // off_815240
                        sys.row(pend).enter(ctx); // off_815238
                        // Re-read: the evade enter re-routes by rewriting the pending
                        // state and tail-calling the routed enter [orig: @0x467400 —
                        // the original reads the brain field, not a saved copy].
                        e->brain.f[AiBrain::kCurState] = e->brain.f[AiBrain::kPendState];
                    }
                }
            }
            // Compact: swap-with-last (faithful to the orig array compaction).
            buf_[index] = buf_[count_ - 1];
            --count_;
            --index;
        }
        ++index;
    }
}

// ----------------------------------------------------------------------------
// AiSystem.
// ----------------------------------------------------------------------------
AiSystem::AiSystem() {
    clear_handle_index();
}

void AiSystem::clear_handle_index() {
    handle_to_ai_index_.assign(65536, -1);
}

void AiSystem::rebuild_handle_index() {
    clear_handle_index();
    for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
        const EntityHandle h = entities_[i].handle;
        if (h.valid()) handle_to_ai_index_[h.packed] = i;
    }
}

int AiSystem::attach(EntityHandle h) {
    AiEntity e;
    e.handle = h;
    e.brain.f[AiBrain::kOwner] = 1; // nonzero = live slot
    entities_.push_back(e);
    const int index = static_cast<int>(entities_.size()) - 1;
    if (h.valid()) handle_to_ai_index_[h.packed] = index;
    return index;
}

int AiSystem::attach_dismemberment_piece(
        EntityHandle h, const AiEntity &source, const int32_t impulse_q16[3]) {
    AiEntity piece = source;
    piece.handle = h;
    piece.brain = AiBrain{};
    piece.brain.f[AiBrain::kOwner] = 1;
    piece.slot = AiSlot{};
    piece.health = 0;
    piece.net_id = 0;
    piece.relmat_id = 0;
    piece.net_is_remote_peer = false;
    piece.net_interp_progress = 0;
    piece.net_interp_steps = 0;
    piece.muzzle_valid = false;
    piece.inf.is_local_player = false;
    piece.inf.player_moving = false;
    piece.inf.move_mode = 0;
    piece.inf.target_dist = 0;
    piece.inf.combat_target = EntityHandle{};
    piece.inf.last_attacker = EntityHandle{};
    piece.inf.was_hit = false;
    piece.inf.vel[0] += impulse_q16 != nullptr ? impulse_q16[0] : 0;
    piece.inf.vel[1] += impulse_q16 != nullptr ? impulse_q16[1] : 0;
    piece.inf.vel[2] += impulse_q16 != nullptr ? impulse_q16[2] : 0;
    entities_.push_back(std::move(piece));
    const int index = static_cast<int>(entities_.size()) - 1;
    if (h.valid()) handle_to_ai_index_[h.packed] = index;
    return index;
}

AiEntity *AiSystem::at(int ai_index) {
    if (ai_index < 0 || ai_index >= static_cast<int>(entities_.size())) return nullptr;
    return &entities_[ai_index];
}

AiEntity *AiSystem::for_handle(EntityHandle h) {
    if (!h.valid()) return nullptr;
    if (handle_to_ai_index_.size() != 65536) return nullptr;
    const int index = handle_to_ai_index_[h.packed];
    if (index < 0 || index >= static_cast<int>(entities_.size())) return nullptr;
    AiEntity &e = entities_[index];
    return e.handle == h ? &e : nullptr;
}

void AiSystem::set_entity_muzzle(EntityHandle h, const int32_t pos[3], uint32_t logic_tick) {
    AiEntity *e = for_handle(h);
    if (e == nullptr || pos == nullptr) return;
    e->muzzle_world[0] = pos[0];
    e->muzzle_world[1] = pos[1];
    e->muzzle_world[2] = pos[2];
    e->muzzle_tick = logic_tick;
    e->muzzle_valid = true;
}

// [orig: AI_BeginUpdate @0x457b40] copy working fields, then the shared-budget gate.
bool AiSystem::begin_update(AiEntity &e) {
    AiBrain &b = e.brain;
    b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA]; // [128]=[49]
    b.f[138] = e.profile.field220;
    b.f[134] = 0;
    b.f[133] = 0;
    b.f[131] = b.f[51] + e.profile.field216;
    b.f[132] = e.heading;
    int32_t budget_used = scheduler.budget;
    if (budget_used > AiScheduler::kBudgetCap) {
        if ((e.profile.flags100 & 2) == 0)
            b.f[AiBrain::kPendState] = 8;
        else
            b.f[AiBrain::kPendState] = b.f[AiBrain::kFallback];
        return false;
    }
    scheduler.budget = budget_used + b.f[AiBrain::kStep];
    return true;
}

// [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0] event 0=update,1=spawn,4=death.
void AiSystem::process_infantry_state_machine(AiEntity &e, World &world, int event) {
    AiBrain &b = e.brain;

    // "no target" idle gate.
    if ((b.f[53] == 0 && b.f[54] == 0) || (!e.profile.has_src148 && !e.profile.has_src180)) {
        if (b.f[AiBrain::kGuard] == 0)
            b.f[AiBrain::kNoTargetIdle] = 1;
    }

    int32_t alert = b.f[AiBrain::kAlert];
    if (is_authority && e.has_physics && (e.physics_flags & 0x100) == 0 &&
        b.f[AiBrain::kPrevAlert] != alert) {
        alert = 2;
        if ((e.profile.flags96 & 2) == 0 && b.f[AiBrain::kCurState] != 14)
            b.f[AiBrain::kPendState] = 10;
    }
    b.f[AiBrain::kPrevAlert] = alert;

    AiThinkCtx ctx{this, &e, &world, nullptr};
    const int ai_index = static_cast<int>(&e - at(0));

    // LABEL_27/28/31: authority applies the pending transition; clients apply only a
    // restricted subset (pending in {7} or 12<pending<=15).
    auto finish = [&]() {
        if (is_authority) {
            apply_transition(e, world);
        } else {
            int32_t pend = b.f[AiBrain::kPendState];
            if (pend == 7 || (pend > 12 && pend <= 15))
                apply_transition(e, world);
        }
    };

    if (event == 0) {
        if (is_authority || b.f[AiBrain::kCurState] == 13 || b.f[AiBrain::kCurState] == 15)
            row(b.f[AiBrain::kCurState]).tick(ctx);
        ++b.f[AiBrain::kTick];
        update_body_anim_slot(e, world); // pick walk/idle from state+movement for the present pass
        finish();
        return;
    }
    if (event == 1) { // spawn
        AiEventEntry ev{};
        ev.f[0] = 1;
        ev.f[1] = 9 | (ai_index << 16); // channel 9 | entity index
        ev.set_timer(0.0f);
        // [orig: ev.f[3] = sub_4E7000()[17] @0x458326] spawn payload from an unmodeled accessor;
        // left 0 (TODO: model sub_4E7000). If a spawn event later reaches a ground combat-event
        // handler (cur_state in {16,17,18}), h_combat_event reads f[3] into brain[39] (kDamageInfo).
        events.queue(ev);
        finish();
        return;
    }
    if (event == 4) { // death
        if (is_authority) {
            apply_transition(e, world);
            return;
        }
        if (is_in_session) {
            e.health = 0; // [orig: *(int16*)(entity+286) = 0 @0x45827f, before the death tick] so the
                          // dispatched tick takes its death path (not the alive path) on a death event
            row(b.f[AiBrain::kCurState]).tick(ctx);
            AiEventEntry ev{};
            ev.f[0] = 20;
            ev.f[1] = 9 | (ai_index << 16);
            ev.set_timer(0.0f);
            events.queue(ev);
            finish();
            return;
        }
        int32_t pend = b.f[AiBrain::kPendState];
        if (pend == 7 || (pend > 12 && pend <= 15))
            apply_transition(e, world);
        return;
    }
}

void AiSystem::apply_transition(AiEntity &e, World &world) {
    AiBrain &b = e.brain;
    int32_t cur = b.f[AiBrain::kCurState];
    if (b.f[AiBrain::kPendState] != cur) {
        AiThinkCtx ctx{this, &e, &world, nullptr};
        row(cur).exit(ctx);                       // off_815240
        row(b.f[AiBrain::kPendState]).enter(ctx); // off_815238
        b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState];
    }
}

void AiSystem::tick(World &world, const TickContext &ctx) {
    // AI does not run during the BMS pre-mission script pass: that invocation only
    // settles initial scripted state (EventFlags PreMission), it does not step brains.
    if (ctx.phase != TickPhase::Gameplay) return;
    is_authority = ctx.is_authority;
    scheduler.budget = 0; // per-frame budget reset (the staggering accumulator)
    // Drain the round sim's processed hits into the AI reaction stamps BEFORE any brain
    // updates: infantry get wasHit/lastAttacker (consumed by the §17.1 scan + §17.3 hit
    // reactions), SM brains get the type-1 damage AIEvent (h_combat_event -> evade).
    // [orig: the damage chain writes the victim entity + queues the event inline —
    // Projectile_ProcessDamageOnTarget @0x4e7fb0; our sim/AI split drains a record.]
    for (const RoundHit &hit : world.round_sim.hits) {
        AiEntity *victim = for_handle(hit.victim);
        if (victim == nullptr) continue;
        if (victim->inf.active) {
            // Every ordinary damage callback alerts an NPC and its trigger group,
            // before lethal/nonlethal handling. The player flag skips this AI leg.
            // [orig: Entity_HandleDamageTrigger @0x4073c8..0x4073ea]
            const Entity *victim_entity = world.registry.get(victim->handle);
            if (victim_entity != nullptr &&
                (victim_entity->engine_flags & kEntityFlagPlayer) == 0) {
                victim->slot.bytes()[AiSlot::kAlertByte] = 2;
                world.relations.group(victim_entity->group_id).alert =
                        TriggerRelations::kAlertRed;
            }
            // Self-damage does not stamp a reaction or attacker. Retail tests the
            // timer against 25, then adds 10 without clamping (24 becomes 34).
            // [orig: Entity_OnDamageReceived @0x4af859..0x4af878]
            if (hit.shooter != victim->handle) {
                victim->inf.was_hit = true;
                if (victim->inf.damage_timer < 25) victim->inf.damage_timer += 10;
                victim->inf.last_attacker = hit.shooter;
            }
        } else {
            AiEventEntry ev{};
            ev.f[0] = 1; // damage
            ev.f[1] = (index_of(*victim) << 16);
            ev.f[3] = hit.damage; // -> brain[39] kDamageInfo via h_combat_event
            ev.set_timer(0.0f);
            events.queue(ev);
        }
    }
    world.round_sim.hits.clear();
    // Rebuild the collision proximity tables once per tick, before any entity update.
    // [orig: Entity_UpdateAllEntities @0x4c2100 -> Entity_BuildAllProximityLists
    // @0x4c20f0 (pool-2 statics + pool-0/1 snapshots) + Entity_BuildProximityListsFromPools
    // @0x4b8eb0 (per-entity candidate slices)]
    // Pool-0 person publication does not depend on any entity having a 3DI
    // collision instance.  RoundSim still queries this snapshot on missions
    // containing only organic entities, so always rebuild the pool tables when
    // a CollisionWorld is installed.  Model-backed blink work remains gated on
    // instance_count below.
    const bool collision_active = collision != nullptr && collision->instance_count() != 0;
    if (collision != nullptr) {
        collision->local_player = world.cached.local_player; // blink accumulation target
        collision->build_tick_tables(world);
    }
    // The loop runs on a JOINER (client, !is_authority) too: tick_infantry's §5.38
    // entity==g_local_player branch (line below, no authority guard) motor-sims the
    // joiner's own player from input, while NPC think/select stays authority-gated. A
    // header-only join keeps remote organics in ClientState rather than this AI array;
    // any native non-authority rows from an explicit complete-BMS/debug join just hold
    // idle. REMOTE presentation reads the host's S2C 0x0A ClientState, so that idle tick
    // cannot overwrite wire pose. [orig: the client also runs the
    // per-entity AI tick; Entity_UpdateInfantryAI @0x4b9910 simulate-when entity==local.]
    for (int i = 0; i < count(); ++i) {
        AiEntity &e = *at(i);
        if (e.inf.active) {
            // Joiners retain seat-follow presentation for wire-owned peers. The
            // authority continues into the remote org2 animation/collision tail:
            // mounted contact callbacks remain live while model push is suppressed.
            if (e.net_is_remote_peer && pose_if_mounted(e, world)) {
                advance_part_anim(e);
                if (!is_authority) continue;
            }
            // A client-only wire peer has no authority collision tail, so its
            // blink/indoors presentation state comes from the position-only refresh.
            // [orig: remote persons
            // refresh via the net position/create handlers — NapiNPClientMsg_0x00F
            // @0x42e442, NetPacket_HandleEntityCreate @0x42f227; the @0x4c229c per-tick
            // walk is pool-2 statics on an 8-per-tick stagger, not persons]
            if (collision_active && e.net_is_remote_peer && !is_authority) {
                if (Entity *ent = world.registry.get(e.handle))
                    collision->refresh_blink(world, *ent);
            }
            // org1-class soldier: the infantry motor replaces the vehicle SM + kinematic
            // locomotion for this entity. [orig: g_EntityClassPhysicsTable row "org1" ->
            // Entity_UpdateInfantryAI @0x4b9910]
            tick_infantry(e, world, ctx.logic_tick);
            continue;
        }
        // Non-infantry mounted controllers retain the seat-follow shortcut.
        if (pose_if_mounted(e, world)) {
            advance_part_anim(e);
            continue;
        }
        if (begin_update(e)) {
            process_infantry_state_machine(e, world, 0);
            // Family-motor vehicles retire the generic SM kinematic mover. Ground
            // families integrate through selector-gated tick_vehicle_motor below;
            // direct-air families own their CHel/cpln callback regardless of selector.
            // The SM stays their decision layer (waypoints,
            // visited bits, states) but the kinematic locomotion model retires for them
            // [orig: one entity update — the SM never integrates ground vehicles, the
            // physics does; Entity_DispatchPhysics_cveh @0x48efc0].
            const Entity *ent = world.registry.get(e.handle);
            const VehicleTraits *vt =
                    ent != nullptr ? world.vehicle_traits.get(ent->item_id) : nullptr;
            const bool motor_driven = vt != nullptr &&
                    (vt->physics != 0 ||
                     vehicle_family_uses_direct_air_mover(vt->family));
            if (locomotion_enabled && !motor_driven) {
                apply_locomotion(e);   // horizontal: advance pos[0]/pos[1] toward the node
                apply_ground_clamp(e, &world); // vertical: snap pos[2] onto ground (no-op if unwired)
            }
        }
        advance_part_anim(e); // part-anim channels integrate independent of the AI budget gate
    }
    // Vehicle motor pass: every pool-1 entity with vehicle traits (items.def
    // `physics` selector non-zero) runs its family's drive core — ground/bike
    // through the cveh core, watercraft through the cbot mover — consuming a
    // mounted ctrl/drvr player's replicated input on the authority. AUTHORITY-ONLY
    // here: a joiner's local copies are wire-posed (the vehicle compact record
    // read side), and the driver's client-side prediction leg is the retail
    // client's concern, not this host loop's.
    // [orig: the per-class tick from Entity_UpdateAllEntities ->
    // Entity_DispatchPhysics_cveh @0x48efc0 -> Entity_UpdateVehiclePhysics
    // @0x48af00 / _cbot @0x48EFA3 -> Entity_UpdateWatercraftPhysics @0x48D480;
    // authority drive gates @0x48b0ff / @0x48DF8C]
    if (is_authority && !world.vehicle_traits.empty()) {
        vehicle_pass_handles_.clear();
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != 1) return;
            const VehicleTraits *traits = world.vehicle_traits.get(e.item_id);
            // Ground/water rows are selector-gated. Direct CHel/cpln rows are
            // admitted regardless of the selector — they branch to the shared
            // aircraft mover below, never through tick_vehicle_motor.
            if (traits == nullptr) return;
            if (traits->physics == 0 &&
                !vehicle_family_uses_direct_air_mover(traits->family)) return;
            vehicle_pass_handles_.push_back(e.handle);
        });
        for (const EntityHandle h : vehicle_pass_handles_) {
            Entity *veh = world.registry.get(h);
            if (veh == nullptr) continue;
            const VehicleTraits *traits = world.vehicle_traits.get(veh->item_id);
            if (traits == nullptr) continue;
            // Mover-entry savedLivePose [orig: the +0x80..+0x94 prologue
            // stamps every mover carries; rider deltas read (current - saved)].
            stamp_saved_live_pose(*veh);
            // Direct CHel/cpln rows never reach the ground cmd/motor leg: the
            // class table routes them to the shared aircraft mover, whose AI
            // brain leg and physics live in one function. A live PLAYER pilot
            // drives through the predicted path instead. [orig: the class table
            // dispatch -> Entity_UpdateAircraftPhysics @0x490310, never the
            // ground core @0x48af00]
            if (vehicle_family_uses_direct_air_mover(traits->family)) {
                if (!veh->veh.net_predicted) {
                    Entity *actrl = resolve_vehicle_controller(world, *veh);
                    const bool actrl_alive = actrl != nullptr && actrl->alive &&
                                             actrl->health > 0;
                    const bool aplayer = actrl_alive && actrl->handle.pool() == 0 &&
                                         actrl->player_class != 0;
                    if (aplayer) {
                        // A PLAYER pilot still runs the shared mover: retail has
                        // ONE aircraft function, and its occupant-input block
                        // (our stage_air_vehicle_input) stages the same
                        // fwd/lat/steer/altitude registers the AI leg fills.
                        // Skipping the mover here left a player in the pilot
                        // seat with no physics at all - the aircraft simply did
                        // not respond.
                        // [orig: Entity_UpdateAircraftPhysics @0x490310 — the
                        //  input gate is `(occ->Flags & 0x100) && (occ ==
                        //  g_local_player_entity || is_authority)`, not a
                        //  separate mover]
                        aircraft_client_tick(world, *veh, *traits);
                    } else {
                        chel_ai_drive(world, *veh, actrl_alive ? actrl : nullptr,
                                      *traits);
                        aircraft_client_tick(world, *veh, *traits);
                    }
                }
                else {
                    // A predicted row skips the mover, so the mover's tail call
                    // never runs for it. Retail's client has no such skip — it
                    // runs the aircraft function (and therefore the tail) for
                    // every vehicle it is not driving, seeding the drive
                    // command from the wire — so advancing the accumulator here
                    // restores that, it does not add a new one.
                    // [orig: the HELO twin @0x48FA70 called from the aircraft
                    //  mover's tail @0x4905A6; the not-driven client leg is
                    //  @0x48B7F0]
                    vehicle_part_anim_tick(world, *veh, *traits);
                }
                if (AiEntity *ve = for_handle(h)) {
                    ve->pos[0] = to_fixed(veh->position.x);
                    ve->pos[1] = to_fixed(veh->position.y);
                    ve->pos[2] = to_fixed(veh->position.z);
                    ve->heading = veh->veh.yaw_seeded
                            ? veh->veh.yaw_bam
                            : bam_heading_from_mission_yaw_deg(
                                      static_cast<double>(veh->yaw));
                }
                continue;
            }
            // Stage the drive input class the motor will consume: a live PLAYER controller
            // keeps the occupant leg; an AI controller (or none) routes through the brain
            // (state stamps + the witnessed steer/speed leg). [orig: the occupant class
            // switch inside Entity_UpdateVehiclePhysics @0x48b949-0x48c034]
            VehicleDriveCmd cmd;
            if (traits->player_control) {
                Entity *ctrl = resolve_vehicle_controller(world, *veh);
                // A DEAD controller parks the vehicle. The infantry death edge detaches
                // first; this guard preserves the same result if the vehicle pass happens
                // to observe the controller earlier in the frame.
                // [orig: infantry death detach @0x4b9c57..0x4b9c60]
                const bool ctrl_alive =
                        ctrl != nullptr && ctrl->alive && ctrl->health > 0;
                const bool player_ctrl = ctrl_alive && ctrl->handle.pool() == 0 &&
                                         ctrl->player_class != 0;
                if (player_ctrl) {
                    // A player drive freezes the SM mover exactly like the parked leg —
                    // the route never advances under a human driver [orig: the player
                    // leg forces SM state 22 too @0x48b993 / the boat leg @0x48DFF5].
                    if (AiEntity *ve = for_handle(h)) {
                        ve->brain.f[AiBrain::kCurState] = 22;
                        ve->brain.f[AiBrain::kPendState] = 22;
                    }
                } else if (traits->family == VehicleFamily::Watercraft) {
                    watercraft_ai_drive(world, *veh, ctrl_alive ? ctrl : nullptr,
                                        *traits, cmd);
                } else {
                    vehicle_ai_drive(world, *veh, ctrl_alive ? ctrl : nullptr, *traits,
                                     cmd);
                }
            }
            // Per-family motor dispatch, the class-table split [orig:
            // Entity_DispatchPhysics_cbot @0x48EFA3 -> the cbot mover @0x48D480
            // vs _cveh @0x48efc0 -> the ground core @0x48af00].
            if (traits->family == VehicleFamily::Watercraft) {
                tick_watercraft_motor(world, *veh, *traits, &cmd);
            } else {
                tick_vehicle_motor(world, *veh, *traits, &cmd);
            }
            // Mirror the integrated transform back into the brain entity — one struct in
            // the original; the SM mover and the present snapshot read pos[]/heading.
            if (AiEntity *ve = for_handle(h)) {
                ve->pos[0] = to_fixed(veh->position.x);
                ve->pos[1] = to_fixed(veh->position.y);
                ve->pos[2] = to_fixed(veh->position.z);
                ve->heading = veh->veh.yaw_seeded
                        ? veh->veh.yaw_bam
                        : bam_heading_from_mission_yaw_deg(static_cast<double>(veh->yaw));
            }
        }
        // Pool-0 bodies were seat-posed in the entity loop above, before these
        // pool-1 motors advanced their carriers. Recompose only their carrier-
        // owned frame now so the authority snapshot writes a stable seat-local
        // offset against the vehicle's final same-tick pose. Retail's compact
        // writer consumes that final pair; leaving the earlier body pose here
        // makes every remote rider trail by one vehicle motor step.
        for (int i = 0; i < count(); ++i)
            refresh_mounted_pose(*at(i), world);
    }
    // A joiner does not integrate its replicated pool-1 vehicle copies here, but
    // retail still executes the per-entity ground callback's presentation leg on
    // clients. Evaluate sound from the current wire/local state after the authority
    // motor pass, leaving position, heading, and motor accumulators untouched.
    // Collision contact is authority-physics state and therefore unavailable on
    // this path; an explicit replicated collision bit can replace `false` later.
    // [orig: Entity_UpdateVehiclePhysics @0x48af00; movement-sound call
    // @0x48d181..0x48d1c4]
    if (!is_authority && !world.vehicle_traits.empty()) {
        vehicle_pass_handles_.clear();
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != 1) return;
            const VehicleTraits *traits = world.vehicle_traits.get(e.item_id);
            if (traits == nullptr) return;
            // Ground/water/bike rows retain their selector gate. CHel/cpln
            // dispatch directly and therefore remain eligible at physics=0.
            if (traits->physics == 0 &&
                !vehicle_family_uses_direct_air_mover(traits->family)) return;
            vehicle_pass_handles_.push_back(e.handle);
        });
        for (const EntityHandle h : vehicle_pass_handles_) {
            Entity *veh = world.registry.get(h);
            if (veh == nullptr) continue;
            const VehicleTraits *traits = world.vehicle_traits.get(veh->item_id);
            if (traits == nullptr) continue;
            // Mover-entry savedLivePose, stamped BEFORE the prediction gates
            // so a frozen/parked hull reads as zero rider delta — retail
            // stamps in every mover prologue regardless of the later bails
            // [orig: the +0x80..+0x94 prologue stamps; the deck-ride reads
            // @0x4b530b../@0x4ba47f..].
            stamp_saved_live_pose(*veh);
            // The joiner-side family prediction (net-re §5.38e B-facet, all
            // four families landed): each mover chases the staged wire target
            // and predicts between records from the mirrored speed/steer
            // registers — the client-executed subset of its family mover
            // [orig: cbot @0x48D480; CHel/cpln via the @0x45D6F0 thunk;
            // ground @0x48af00 core]. The embedding sim clears net_predicted
            // for wire-frozen rows (bit0 / dead-pose / carried), so a wreck
            // never keeps driving (D-NET-66).
            if (veh->veh.net_predicted && veh->health > 0 &&
                    traits->family == VehicleFamily::Watercraft) {
                watercraft_client_tick(world, *veh, *traits);
            } else if (veh->veh.net_predicted && veh->health > 0 &&
                    (traits->family == VehicleFamily::Helicopter ||
                     traits->family == VehicleFamily::Plane)) {
                aircraft_client_tick(world, *veh, *traits);
            } else if (veh->veh.net_predicted && veh->health > 0 &&
                    (traits->family == VehicleFamily::Ground ||
                     traits->family == VehicleFamily::Bike ||
                     traits->family == VehicleFamily::Tank)) {
                // Runs the motor core, whose tail already ticks the movement
                // sound — skip the separate sound call below for this row.
                // Bikes and tanks ride the same entry; the core branches on
                // the family tag for the witnessed cbik deltas (gravity 250,
                // vZ up-cap, contact-gated integration, always-applied yaw)
                // and the ctan deltas (gravity 250, contact-gated integration
                // with the ±2·decel reversal clamps, full-basis velocity,
                // parked-gated yaw with the airborne quarter-rate)
                // [orig: @0x483FE0 / @0x488AB0 vs @0x48AF00].
                ground_client_tick(world, *veh, *traits);
                continue;
            }
            // The shared aircraft mover has no movement-sound call. In retail,
            // Entity_ProcessMovementSoundEffects @0x5294A0 is reached from the
            // ground/bike/water paths, but neither CHel @0x490310 nor cpln's
            // thunk calls it. Physicsless air rows are newly eligible above, so
            // keep them out of the ground-sound presentation tail in every
            // prediction/death state.
            if (vehicle_family_uses_direct_air_mover(traits->family)) continue;
            update_ground_vehicle_sound(world, *veh, *traits,
                                        /*wrecked=*/veh->health <= 0,
                                        /*collided=*/false);
        }
    }
    events.process_timed(*this, world);
}

void AiSystem::pump_mounted_weapon_slots(World &world, uint32_t logic_tick) {
    mounted_weapon_handles_.clear();
    world.registry.for_each([&](const Entity &entity) {
        if (entity.handle.pool() == 1 && entity.primary_weapon_owner.valid())
            mounted_weapon_handles_.push_back(entity.handle);
    });

    for (const EntityHandle mount_handle : mounted_weapon_handles_) {
        Entity *mount = world.registry.get(mount_handle);
        if (mount == nullptr) continue;
        Entity *owner = world.registry.get(mount->primary_weapon_owner);
        if (owner == nullptr || owner->health <= 0 || !owner->mounted ||
            owner->mount_type != SeatType::Gunner ||
            owner->mount_target != mount_handle) {
            mount->primary_weapon_owner = EntityHandle{};
            continue;
        }
        // L's borrowed parent slot is pumped by Simulation with the live
        // trigger/reload/scope inputs and first-person event sink. Advancing it
        // here as well would run one slot twice per frame. Remote players and
        // NPC gunners remain owned by this global world pump.
        // [orig: one WeaponAction_ProcessAllEntities walk @0x542690]
        if (world.external_local_mounted_weapon_pump &&
            owner->handle == world.cached.local_player)
            continue;
        AiEntity *gunner = for_handle(owner->handle);
        if (gunner == nullptr || mount->primary_weapon_slot_adm == 0xFF) continue;
        const WeaponTableEntry *weapon =
                world.weapons.by_index(mount->primary_weapon_slot_adm);
        if (weapon == nullptr || weapon->ammo_index < 0) continue;

        WeaponFsmInputs inputs;
        inputs.is_local = owner->handle == world.cached.local_player;
        inputs.is_authority = is_authority;
        inputs.auto_reload = true;
        // The heat window is derived from the tick, so the pump needs it. AI gunners
        // sit on the emplaced guns that actually author heat, so this is the path
        // that overheats in practice. [orig: current_tick @ 0x24C1968]
        inputs.current_tick = static_cast<int32_t>(logic_tick);
        WeaponFsmEvents weapon_events;
        weapon_fsm_tick(weapon->action_fsm, mount->primary_weapon_slot,
                        inputs, weapon_events);
        if (!weapon_events.fired || !is_authority) continue;

        // The slot owner is the gunner, while its def/ammo live on the parent.
        // Use the live chased look and the freshest posed muzzle available: the
        // MOUNT's stamp first, then the gunner's own seam origin (stamp or the
        // chest/seat fallback).
        // [orig: slot owner path in WeaponAction_Fire @0x542b10;
        //  Entity_CalcWeaponFirePosition parentSlot 3]
        int32_t origin[3];
        if (mount->posed_muzzle_valid &&
            logic_tick - mount->posed_muzzle_tick <= AiSystem::kMuzzleFreshTicks) {
            origin[0] = mount->posed_muzzle_world[0];
            origin[1] = mount->posed_muzzle_world[1];
            origin[2] = mount->posed_muzzle_world[2];
        } else {
            AiSystem::weapon_fire_origin(*gunner, logic_tick, origin);
        }
        if (fire_ai_round(world, *gunner, origin, gunner->heading,
                          io::bam_add(gunner->pitch, gunner->inf.recoil_pitch),
                          weapon->ammo_index))
            gunner->inf.aim_ref0 = gunner->inf.combat_target;
    }
}

bool AiSystem::pose_if_mounted(AiEntity &e, World &world) {
    Entity *occ = world.registry.get(e.handle);
    // A dead occupant cannot enter the live mounted-pose path: it must reach the
    // infantry death edge, detach, then consume its staged death animation.
    // [orig: Entity_UpdateInfantryAI @0x4b9960..0x4b9983]
    if (occ == nullptr || !occ->mounted || occ->health <= 0) return false;
    Entity *veh = world.registry.get(occ->mount_target);
    if (veh == nullptr) {              // vehicle gone -> auto-dismount, resume normal AI
        entity_detach_from_vehicle(world, e.handle);
        return false;
    }
    if (occ->mount_seat < 0 || occ->mount_seat >= static_cast<int>(veh->seats.size())) return false;
    const Seat &seat = veh->seats[occ->mount_seat];
    // Local input owns LOOK before retail evaluates the parent UseGun bone. Our
    // split AiEntity keeps that input in the infantry latch until the mounted
    // branch, so expose it before the host asks for the live parent pose.
    if (e.inf.active && e.inf.is_local_player) {
        e.heading = e.inf.target_heading;
        e.pitch = e.inf.look_pitch;
    }
    const int32_t saved_look_heading = e.heading;
    const int32_t saved_look_pitch = e.pitch;
    const int32_t seat_heading = apply_resolved_mounted_seat_frame(
            e, world, *occ, *veh, seat);
    if (e.inf.active && e.inf.is_local_player) {
        // The mounted LOCAL player keeps the LOOK as its entity yaw: the witnessed mounted
        // carry writes bodyHeading/headLook from the seat bone but leaves entity->Yaw
        // player-owned — the drive motor reads it as the mouse-steer target
        // [orig: Entity_UpdateInfantryPlayerBody mounted leg (bodyHeading only) +
        //  Entity_UpdateVehiclePhysics steer source @0x48ba59/v61->Yaw].
        occ->yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(e.inf.target_heading))));
        // Mirror the live inputs into the wire fields the motor consumes — tick_infantry's
        // per-tick mirror is skipped while mounted (one entity struct in the original; the
        // split is ours) [orig: the MoveOrder packer @0x4df68f-0x4df741].
        occ->net_move_input = static_cast<uint8_t>(
                (e.inf.player_move_dir_index & Entity::kMoveOrderDirMask) |
                (e.inf.player_moving ? Entity::kMoveOrderMoving : 0) |
                (e.inf.lean_left ? Entity::kMoveOrderLeanLeft : 0) |
                (e.inf.lean_right ? Entity::kMoveOrderLeanRight : 0));
        occ->net_stance_bits = 0; // seated stance stays cleared [orig: @0x435c42]
        // Drop any pending jump: the input latch is set-only (its consumer is
        // tick_infantry's jump block, skipped for the whole ride) and the witnessed
        // mounted carry scrubs the move flags every tick — a seated player cannot
        // bank a jump for dismount [orig: the mounted-leg flag scrub &= 0xFF8F57DF].
        e.inf.jump_requested = false;
    }
    // Occupants present their carried body in the captured seat frame. Independent
    // player/gunner LOOK restored below must not rotate that body/collision pose.
    e.heading = seat_heading;
    if (e.inf.active && e.inf.is_local_player) {
        // The seated LOOK stays mouse-instant at FULL precision: retail drives entity
        // Yaw/Pitch straight from input regardless of mount (the mounted body leg
        // writes bodyHeading/headLook from the bone, never the look) — the camera
        // reads these mirrors, and the whole-degree occ->yaw roundtrip above (the
        // wire/motor mirror) must not quantize or freeze it. tick_infantry's own
        // mirrors (its lines `e.heading = inf.target_heading` / `e.pitch =
        // inf.look_pitch`) are skipped for the whole ride.
        // [orig: Input_HandleActionBinding_0 @0x4e1330 writes entity+0x10/+0x14;
        //  §23.5 — the entity Yaw is the LOOK, player-owned while seated]
        e.heading = e.inf.target_heading;
        e.pitch = e.inf.look_pitch;
    } else if (remote_player_controls_vehicle(e, *occ, seat)) {
        // The host read-applies this player's LOOK from C2S before the body tick.
        // Restore it after seat carry so the later authority vehicle pass consumes
        // the owner's mouse-steer target, not the carrier yaw.
        e.heading = saved_look_heading;
        e.pitch = saved_look_pitch;
        occ->yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(saved_look_heading))));
    } else if (e.inf.active && seat.type == SeatType::Gunner) {
        // Attachment writes the seat/base pose but restores the child's independent
        // live look. The look then chases the desired solution instead of snapping:
        // yaw quarter-step clamped to +/-0x02000000, pitch eighth-step. Most mount
        // configs also constrain look to +/-90 degrees around the attached base.
        // [orig: save/restore @0x546416..0x546664; chase @0x4bef57..0x4bef97;
        //  base-relative clamp @0x4bef9a..0x4beff0]
        e.heading = saved_look_heading;
        e.pitch = saved_look_pitch;
        if (e.inf.aim_valid) {
            int32_t yaw_step = io::bam_sar(
                    io::bam_add(io::bam_sub(e.inf.aim_heading, e.heading), 2), 2);
            yaw_step = std::clamp(yaw_step, -0x02000000, 0x02000000);
            e.heading = io::bam_add(e.heading, yaw_step);
            const int32_t pitch_step = io::bam_sar(
                    io::bam_add(io::bam_sub(e.inf.aim_pitch, e.pitch), 4), 3);
            e.pitch = io::bam_add(e.pitch, pitch_step);
        }
        const int cfg = veh->emplaced_config;
        const bool wide_mount = veh->emplaced_config_valid &&
                (cfg == 3 || cfg == 4 || cfg == 5 || cfg == 7);
        if (!wide_mount) {
            const int32_t rel = io::bam_sub(e.heading, seat_heading);
            if (rel > 0x40000000)
                e.heading = io::bam_add(seat_heading, 0x40000000);
            else if (rel < -0x40000000)
                e.heading = io::bam_sub(seat_heading, 0x40000000);
        }
        // The chase above changes the semantic EWEAP controls consumed by a
        // model-aware provider. Resolve once more so the NPC root/body and the
        // parent gun presented after this tick use the same Hn+1 control phase.
        // Keep the first frame as the existing clamp base and preserve LOOK as
        // the child's independent heading/pitch after the second seat resolve.
        if (e.heading != saved_look_heading || e.pitch != saved_look_pitch) {
            const int32_t chased_look_heading = e.heading;
            const int32_t chased_look_pitch = e.pitch;
            apply_resolved_mounted_seat_frame(e, world, *occ, *veh, seat);
            e.heading = chased_look_heading;
            e.pitch = chased_look_pitch;
        }
        occ->yaw = static_cast<int16_t>(std::lround(normalize_mission_yaw_deg(
                mission_yaw_deg_from_bam_heading(e.heading))));
    }
    if (e.inf.active) {
        const int mounted_state = mounted_anim_state_for_seat(*veh, seat, e.inf, root_motion);
        if (e.inf.anim_state != mounted_state) {
            e.inf.begin_body_transition(mounted_state);
        }
        e.inf.anim_pending = 0;
        e.inf.move_mode = 0;
        e.inf.target_dist = 0;
        occ->body_anim_slot = body_anim_slot_from_state(e.inf.anim_state);
        mirror_wire_anim(e, world); // seat-state anim bytes reach the 0x0A player record too
    }
    return true;
}

static int32_t part_anim_wrapped_add(int32_t lhs, int32_t rhs) {
    const uint32_t bits = static_cast<uint32_t>(lhs) +
                          static_cast<uint32_t>(rhs);
    int32_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

static int32_t part_anim_wrapped_sub(int32_t lhs, int32_t rhs) {
    const uint32_t bits = static_cast<uint32_t>(lhs) -
                          static_cast<uint32_t>(rhs);
    int32_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool part_anim_step(int32_t &phase, int32_t dir, int32_t rate) {
    if (dir == 0) return false; // play_type 0 (stop) freezes the sweep
    if (dir == 1) {
        phase = part_anim_wrapped_add(phase, rate);
        // The original clears direction only after a strict upper overshoot.
        // Landing exactly on 0x10000 remains active.
        // [orig: Entity_UpdateSuspensionBounce @0x456740..0x456764]
        if (phase > 0x10000) {
            phase = 0x10000;
            return true;
        }
        return false;
    }
    // Every nonzero direction other than +1 takes the subtraction branch;
    // only a negative result clamps and stops.
    // [orig: Entity_UpdateSuspensionBounce @0x456756..0x456764]
    phase = part_anim_wrapped_sub(phase, rate);
    if (phase < 0) {
        phase = 0;
        return true;
    }
    return false;
}

void AiSystem::advance_part_anim(AiEntity &e) {
    AiBrain &b = e.brain;
    for (int slot = 0; slot < 2; ++slot) {
        const int32_t dir = b.f[AiBrain::kPartAnimDir0 + slot];
        const int32_t rate = b.f[AiBrain::kPartAnimRate0 + slot];
        if (dir == 0) continue;
        if (part_anim_step(b.f[AiBrain::kPartAnimPhase0 + slot], dir, rate))
            b.f[AiBrain::kPartAnimDir0 + slot] = 0;
    }
}

// [flt_7C3310=1/65536, flt_7C3B40=0.016, flt_7C32BC=65536.] The original
// computes `base` unconditionally, so ANIMTIME==0 -> base 0.0 -> 0.016/0.0 =
// +inf, and the x87 ftol of infinity is the integer-indefinite 0x80000000
// (INT_MIN) — nonzero, so the min-1 guard does NOT fire. The updater then
// applies ordinary wrapping ADD/SUB: from phase zero, zero-time forward
// alternates INT_MIN/zero without stopping; zero-time reverse clamps back to
// zero and stops on its first tick. [orig: Entity_ApplyCommand
// @0x43B1A9..0x43B1F9]
int32_t part_anim_rate_from_seconds(double seconds) {
    const double rate_f = (0.016 / seconds) * 65536.0; // +inf when seconds==0
    int32_t rate;
    if (rate_f != rate_f || rate_f >= 2147483648.0 || rate_f < -2147483648.0) {
        rate = static_cast<int32_t>(0x80000000); // ftol integer-indefinite
    } else {
        rate = static_cast<int32_t>(rate_f);     // truncate toward zero
    }
    if (rate == 0) rate = 1; // min-1 guard (does NOT fire for INT_MIN)
    return rate;
}

// [orig: Entity_ApplyCommand @0x43ab60] See the header. Only case 0x22 (PLAYPARTANIM) is ported.
void ai_apply_command(AiBrain &comp, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    switch (sub_type) {
        case 0x22: { // PLAYPARTANIM: p2=ANIMNUM(channel), p3=ANIMPLAYTYPE, p4=ANIMTIME(16.16 s)
            const int channel = p2;
            if (channel != 1 && channel != 2) return;              // only channels 1,2 act
            const int play_type = p3;
            if (static_cast<unsigned>(play_type + 1) > 2u) return; // play_type in {-1,0,1}
            const int slot = channel - 1;
            // rate = (0.016 / seconds) * 65536 phase-units/tick, min 1 —
            // shared with the editor-preview binding (see
            // part_anim_rate_from_seconds below for the witnessed FPU shape).
            const double seconds = static_cast<double>(p4) / 65536.0; // base; p4==0 -> 0.0
            comp.f[AiBrain::kPartAnimDir0 + slot] = play_type;  // comp+436+4*slot (direction)
            comp.f[AiBrain::kPartAnimRate0 + slot] =            // comp+444+4*slot (rate)
                    part_anim_rate_from_seconds(seconds);
            break;
        }
        case 29:   // COMBATSPEED -> kSpeedA (brain +196)
        case 30: { // PATROLSPEED -> kSpeedB (brain +200)
            // [orig: Entity_ApplyCommand @0x43ab60 cases 0x1D/0x1E queue AIEvent types
            // 10/11 -> AI_HandleCommand @0x465770 cases 0xA/0xB — km/h to 16.16 u/tick:
            // fild(value) (+2^32 when negative = the unsigned reinterpret) * 1000
            // * (1/225000) * 65536 = x65536/225 (the exact 62.5 Hz conversion; the
            // items.def parse's x293 is its integer approximation).]
            double v = static_cast<double>(p2);
            if (v < 0.0) v += 4294967296.0; // flt_7C3288 add on negative [orig: @0x465974]
            const int32_t scaled =
                    static_cast<int32_t>(v * 1000.0 * 4.444444584805751e-06 * 65536.0);
            comp.f[sub_type == 29 ? AiBrain::kSpeedA : AiBrain::kSpeedB] = scaled;
            break;
        }
        default:
            // Tracked-TODO: accuracy(8), AISETSTATE(0x1C), etc. (the D-AI
            // rows in docs/world/world-wac-ai-re.md; the alert subs 5/6/0x16
            // are ported — controller byte + queued brain event at the
            // EntityCommands seam). No-op so an unported sub-type
            // can't corrupt the AI component.
            break;
    }
}

void AiSystem::capture_spawn_baseline() {
    spawn_baseline_ = entities_;
    baseline_captured_ = true;
}

// Per-mission re-init seam: World::restore() calls load_systems() on an editor Play->Stop,
// which reaches this. Rewind every brain (position/heading/state/timers) to the captured
// spawn baseline and drop the transient queues, so a simulate/stop cycle leaves the authored
// mission clean. The nav table is read-only path data and is left intact.
void AiSystem::on_load(World &) {
    if (baseline_captured_) entities_ = spawn_baseline_;
    rebuild_handle_index();
    events.clear();
    scheduler.budget = 0;
    relmat_calls.clear();
    rel_ops.clear();
    target_set_calls.clear();
    scan_candidates_.clear();
    unported_calls = 0;
    find_target_calls = 0;
    fire_shot_seq = 0;
}

// [orig: Entity_CalcAverageGroundHeight @0x457230] 5-tap weighted ground height (16.16 fixed).
// The taps sample the bilinear terrain column at center + N/S/E/W at sample_radius; the original
// per-tap sampler is the hi-res down-raycast @0x60e710 (its near-vertical result == this column
// height — tracked deviation). Engine ground plane is (X,Y) = (pos[0],pos[1]); the renderer-loaded
// atlas is sampled in Godot coords (x, -y), and the N/S/E/W taps are symmetric so the Y sign only
// renames which tap is "north". Height comes back in world units; ground_16.16 = units * 65536.
int32_t calc_average_ground_height(const terrain::TerrainHeightField &field, const int32_t pos[3],
                                   int32_t sample_radius, const GroundClearance &clearance) {
    if (!field.valid()) return INT32_MIN; // [orig: terrain assumed present; null -> no ground]

    auto sample = [&](int32_t x16, int32_t y16) -> int32_t {
        const float wx = static_cast<float>(x16) / 65536.0f;
        const float wz = -static_cast<float>(y16) / 65536.0f; // engine Y -> Godot z
        const float h = terrain::height_field_height_world_bilinear(field, wx, wz);
        return static_cast<int32_t>(h * 65536.0f);
    };

    int32_t result;
    int32_t center;
    if (sample_radius) {
        int32_t maxHeight = 0;                                       // [orig: maxHeight = 0]
        const int32_t north = sample(pos[0], pos[1] + sample_radius); // Entity_RaycastGroundHeight(e,0,+r) @0x4142c0
        if (north > 0) maxHeight = north;                           // [orig: if(north>0) max=north]
        const int32_t south = sample(pos[0], pos[1] - sample_radius); // Entity_RaycastGroundHeight(e,0,-r)
        if (south > maxHeight) maxHeight = south;
        const int32_t east = sample(pos[0] + sample_radius, pos[1]);  // sub_414320(e,+r,0)
        if (east > maxHeight) maxHeight = east;
        const int32_t west = sample(pos[0] - sample_radius, pos[1]);  // Entity_RaycastGroundHeight(e,-r,0)
        if (west > maxHeight) maxHeight = west;
        center = sample(pos[0], pos[1]);                             // sub_414320(e,0,0)
        if (center > maxHeight) maxHeight = center;
        result = (north + south + east + west + 2 * (center + 2 * maxHeight)) / 10;
        if (result < center) result = center;                        // [orig: clamp >= center]
    } else {
        result = sample(pos[0], pos[1]);                             // [orig: radius 0 -> center only]
    }

    // [orig: if (*(entity+368) && (int)worldY > result) result = worldY] water-surface clamp.
    if (clearance.has_physics && field.has_water && field.water_y > result) {
        result = field.water_y;
    }
    // [orig: def offset: dead path uses def+0x30, else def+0x2C; gated by entityDef != 0.]
    result += clearance.use_dead ? clearance.dead_offset : clearance.alive_offset;
    return result;
}

// Drive the vertical off the terrain sampler. See the header. Snap model for the un-reversed
// vertical driver: SET kWorkPosZ + the entity's pos[2] to ground + ground_stand_offset.
void AiSystem::apply_ground_clamp(AiEntity &e, World *world) {
    if (terrain == nullptr) return;
    GroundClearance clearance = ground_clearance;
    clearance.has_physics = e.has_physics;                    // [orig: entity+368 gate]
    clearance.use_dead = (e.health <= 0);                     // [orig: health<=0 dead path]
    constexpr int32_t kSampleRadius = 0x50000;
    int32_t ground;
    if (world != nullptr && collision != nullptr && collision->instance_count() != 0) {
        // The witnessed 5-tap average with MODEL-AWARE rays: each tap is the
        // ray from the tap column + 1.0u lift, 48u drop, clipped by terrain and
        // by candidate models, so a brain standing on a building deck grounds
        // on the deck. Weights/order/clamps are the same as the terrain-only
        // path below (they are the same function in retail).
        // [orig: Entity_CalcAverageGroundHeight @0x457230 — the four
        //  Entity_RaycastGroundHeight(AndObject)(entity, dx, dy, 0x10000,
        //  0x300000) taps + the doubled centre/max fold]
        const auto tap = [&](int32_t dx, int32_t dy) {
            return collision->raycast_ground(*world, e.handle, e.pos, dx, dy,
                                             0x10000, 0x300000, nullptr);
        };
        int32_t max_h = 0;                       // [orig: maxHeight = 0]
        const int32_t north = tap(0, kSampleRadius);
        if (north > 0) max_h = north;            // [orig: if (north > 0) max = north]
        const int32_t south = tap(0, -kSampleRadius);
        if (south > max_h) max_h = south;
        const int32_t east = tap(kSampleRadius, 0);
        if (east > max_h) max_h = east;
        const int32_t west = tap(-kSampleRadius, 0);
        if (west > max_h) max_h = west;
        const int32_t centre = tap(0, 0);
        if (centre > max_h) max_h = centre;
        ground = (north + south + east + west + 2 * (centre + 2 * max_h)) / 10;
        if (ground < centre) ground = centre;    // [orig: clamp >= centre]
        if (clearance.has_physics && terrain->has_water && terrain->water_y > ground)
            ground = terrain->water_y;           // [orig: the occupant water clamp]
        ground += clearance.use_dead ? clearance.dead_offset : clearance.alive_offset;
    } else {
        ground = calc_average_ground_height(*terrain, e.pos, kSampleRadius, clearance);
    }
    if (ground == INT32_MIN) return;                          // no terrain coverage -> leave Z
    const int32_t z = ground + ground_stand_offset;           // [orig: brain[131] = ground + 0x50000]
    e.brain.f[AiBrain::kWorkPosZ] = z;                        // mover output field stays faithful
    e.pos[2] = z;                                             // snap the entity onto the ground
}

// Kinematic locomotion over the mover output. The brain decides a target (kWorkPos*), a heading
// (kWorkHeading, BAM) and a speed (kOutSpeed); here we turn to that heading and advance the entity
// toward the target by kOutSpeed * loco_scale, clamped so we never overshoot. INTERIM MODEL being
// replaced: the original routes organics through the infantry motor [orig: Entity_UpdateInfantryAI
// @ 0x4b9910] (anim-driven root motion; ground vehicles consume this SM's output instead) — see
// docs/world/world-wac-ai-re.md §3 for the full spec. Out-speed 0 leaves the entity put.
void AiSystem::apply_locomotion(AiEntity &e) {
    AiBrain &b = e.brain;
    int32_t speed = b.f[AiBrain::kOutSpeed]; // brain[128]
    if (speed <= 0) return;
    e.heading = b.f[AiBrain::kWorkHeading];  // brain[132] (snap; turn-rate physics deferred)
    int64_t stepd = static_cast<int64_t>(speed) * loco_scale; // AI units -> 16.16 world delta
    int64_t dx = static_cast<int64_t>(b.f[AiBrain::kWorkPosX]) - e.pos[0]; // brain[129]
    int64_t dy = static_cast<int64_t>(b.f[AiBrain::kWorkPosY]) - e.pos[1]; // brain[130]
    double dist = std::sqrt(static_cast<double>(dx) * static_cast<double>(dx) +
                            static_cast<double>(dy) * static_cast<double>(dy));
    if (dist == 0.0 || dist <= static_cast<double>(stepd)) {
        e.pos[0] = b.f[AiBrain::kWorkPosX]; // arrive (clamp, no overshoot)
        e.pos[1] = b.f[AiBrain::kWorkPosY];
    } else {
        double f = static_cast<double>(stepd) / dist;
        e.pos[0] += static_cast<int32_t>(static_cast<double>(dx) * f);
        e.pos[1] += static_cast<int32_t>(static_cast<double>(dy) * f);
    }
}

// ----------------------------------------------------------------------------

} // namespace opennova::world
