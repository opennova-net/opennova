#include "world/ai.h"

// Split out of ai.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// The AI event queue and the AiSystem core: registration, per-entity rows, and the
// tick that drives every handler above.

#include "terrain/height_field.h"
#include "world/angle.h"
#include "world/body_anim.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/vehicle_sound.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "ai_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

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
    if (ctx.pre_mission) return;
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
                victim->slot.bytes()[AiSlot::kMoveFlagByte] = 2;
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
    // joiner's own player from input, while NPC think/select stays authority-gated, so
    // local-promote copies of remote entities just hold (idle). The joiner renders every
    // REMOTE entity's pose from the host's S2C 0x0A (present reads ClientState, not these
    // local copies), so their idle ticking is harmless. [orig: the client also runs the
    // per-entity AI tick; Entity_UpdateInfantryAI @0x4b9910 simulate-when entity==local.]
    for (int i = 0; i < count(); ++i) {
        AiEntity &e = *at(i);
        if (e.inf.active) {
            // Net-snapped peers do not locally simulate their body, but retain the
            // existing seat-follow presentation phase. Simulated infantry enters the
            // full retail body tick; its mounted return suppresses locomotion only.
            if (e.net_is_remote_peer && pose_if_mounted(e, world)) {
                advance_part_anim(e);
                continue;
            }
            // Net-snapped remote peers skip the movement motor, so their blink/indoors
            // state comes from the position-only refresh instead. [orig: remote persons
            // refresh via the net position/create handlers — NapiNPClientMsg_0x00F
            // @0x42e442, NetPacket_HandleEntityCreate @0x42f227; the @0x4c229c per-tick
            // walk is pool-2 statics on an 8-per-tick stagger, not persons]
            if (collision_active && e.net_is_remote_peer) {
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
            // Motor-driven vehicles (items.def physics selector non-zero) integrate through
            // tick_vehicle_motor below — the SM stays their decision layer (waypoints,
            // visited bits, states) but the kinematic locomotion model retires for them
            // [orig: one entity update — the SM never integrates ground vehicles, the
            // physics does; Entity_DispatchPhysics_cveh @0x48efc0].
            const Entity *ent = world.registry.get(e.handle);
            const VehicleTraits *vt =
                    ent != nullptr ? world.vehicle_traits.get(ent->item_id) : nullptr;
            const bool motor_driven = vt != nullptr && vt->physics != 0;
            if (locomotion_enabled && !motor_driven) {
                apply_locomotion(e);   // horizontal: advance pos[0]/pos[1] toward the node
                apply_ground_clamp(e); // vertical: snap pos[2] onto the terrain (no-op if unwired)
            }
        }
        advance_part_anim(e); // part-anim channels integrate independent of the AI budget gate
    }
    // Ground-vehicle motor pass: every pool-1 entity with vehicle traits (items.def
    // `physics` selector non-zero) runs the drive core — consuming a mounted ctrl/drvr
    // player's replicated input on the authority. AUTHORITY-ONLY here: a joiner's local
    // copies are wire-posed (the vehicle compact record read side), and the driver's
    // client-side prediction leg is the retail client's concern, not this host loop's.
    // [orig: the per-class tick from Entity_UpdateAllEntities -> Entity_DispatchPhysics_cveh
    // @0x48efc0 -> Entity_UpdateVehiclePhysics @0x48af00; authority drive gate @0x48b0ff]
    if (is_authority && !world.vehicle_traits.empty()) {
        vehicle_pass_handles_.clear();
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != 1) return;
            if (world.vehicle_traits.get(e.item_id) == nullptr) return;
            vehicle_pass_handles_.push_back(e.handle);
        });
        for (const EntityHandle h : vehicle_pass_handles_) {
            Entity *veh = world.registry.get(h);
            if (veh == nullptr) continue;
            const VehicleTraits *traits = world.vehicle_traits.get(veh->item_id);
            if (traits == nullptr) continue;
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
                    // leg forces SM state 22 too @0x48b993].
                    if (AiEntity *ve = for_handle(h)) {
                        ve->brain.f[AiBrain::kCurState] = 22;
                        ve->brain.f[AiBrain::kPendState] = 22;
                    }
                } else {
                    vehicle_ai_drive(world, *veh, ctrl_alive ? ctrl : nullptr, *traits,
                                     cmd);
                }
            }
            tick_vehicle_motor(world, *veh, *traits, &cmd);
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
            if (traits == nullptr || traits->physics == 0) return;
            vehicle_pass_handles_.push_back(e.handle);
        });
        for (const EntityHandle h : vehicle_pass_handles_) {
            Entity *veh = world.registry.get(h);
            if (veh == nullptr) continue;
            const VehicleTraits *traits = world.vehicle_traits.get(veh->item_id);
            if (traits == nullptr) continue;
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
        // L's borrowed parent slot is pumped by NovaSimulation with the live
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
        // Use the live chased look and the freshest posed muzzle available; the
        // fallback is the mounted occupant's chest/seat origin.
        // [orig: slot owner path in WeaponAction_Fire @0x542b10;
        //  Entity_CalcWeaponFirePosition parentSlot 3]
        int32_t origin[3] = {gunner->pos[0], gunner->pos[1],
                             gunner->pos[2] + 0xE666};
        if (mount->posed_muzzle_valid &&
            logic_tick - mount->posed_muzzle_tick <= 4u) {
            origin[0] = mount->posed_muzzle_world[0];
            origin[1] = mount->posed_muzzle_world[1];
            origin[2] = mount->posed_muzzle_world[2];
        } else if (gunner->muzzle_valid &&
                   logic_tick - gunner->muzzle_tick <= 4u) {
            origin[0] = gunner->muzzle_world[0];
            origin[1] = gunner->muzzle_world[1];
            origin[2] = gunner->muzzle_world[2];
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
    const auto apply_resolved_seat_frame = [&]() {
        pose_mounted_occupant(world, *occ, *veh, seat);
        // Capture the resolved seat orientation before an independent LOOK mirror
        // overwrites registry yaw. Keep the witnessed integer yaw conversion here:
        // the generic degree helper rounds differently at non-cardinal headings.
        const int16_t seat_yaw = occ->yaw;
        const int16_t seat_pitch = occ->pitch;
        const int16_t seat_roll = occ->roll;
        const int32_t resolved_heading = static_cast<int32_t>(
                static_cast<int64_t>(90 - seat_yaw) * kBamPerDegreeInt);
        if (e.inf.active) {
            // Mirror both the direct seat-frame writes and the carried-infantry leg chase
            // snap so render and per-section collision consume one coherent body frame.
            // [orig: seat carry @0x4b654e-0x4b6575; carried body/leg snap Flags & 0x100060]
            e.inf.body_heading = resolved_heading;
            e.inf.leg_yaw[0] = resolved_heading;
            e.inf.leg_yaw[1] = resolved_heading;
            e.inf.leg_target[0] = resolved_heading;
            e.inf.leg_target[1] = resolved_heading;
            e.body_pitch = bam_from_degrees_wrapped(static_cast<double>(seat_pitch));
            e.roll = bam_from_degrees_wrapped(static_cast<double>(seat_roll));
        }
        // Organics present from AiEntity.pos, not Entity.position.
        e.pos[0] = to_fixed(occ->position.x);
        e.pos[1] = to_fixed(occ->position.y);
        e.pos[2] = to_fixed(occ->position.z);
        return resolved_heading;
    };
    const int32_t seat_heading = apply_resolved_seat_frame();
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
    // Remote occupants present in the captured seat frame. The local LOOK override
    // below remains player-owned and must not rotate the carried body/collision pose.
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
            apply_resolved_seat_frame();
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

void AiSystem::advance_part_anim(AiEntity &e) {
    AiBrain &b = e.brain;
    for (int slot = 0; slot < 2; ++slot) {
        const int32_t dir = b.f[AiBrain::kPartAnimDir0 + slot];
        const int32_t rate = b.f[AiBrain::kPartAnimRate0 + slot];
        if (dir == 0) continue; // play_type 0 (stop) freezes the sweep
        int32_t &phase = b.f[AiBrain::kPartAnimPhase0 + slot];
        if (dir == 1) {
            phase = part_anim_wrapped_add(phase, rate);
            // The original clears direction only after a strict upper
            // overshoot. Landing exactly on 0x10000 remains active.
            // [orig: Entity_UpdateSuspensionBounce @0x456740..0x456764]
            if (phase > 0x10000) {
                phase = 0x10000;
                b.f[AiBrain::kPartAnimDir0 + slot] = 0;
            }
        } else {
            phase = part_anim_wrapped_sub(phase, rate);
            // Every nonzero direction other than +1 takes the subtraction
            // branch; only a negative result clamps and stops.
            // [orig: Entity_UpdateSuspensionBounce @0x456756..0x456764]
            if (phase < 0) {
                phase = 0;
                b.f[AiBrain::kPartAnimDir0 + slot] = 0;
            }
        }
    }
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
            // rate = (0.016 / seconds) * 65536 phase-units/tick, min 1. [flt_7C3310=1/65536,
            // flt_7C3B40=0.016, flt_7C32BC=65536.] The original computes `base` unconditionally, so
            // ANIMTIME==0 -> base 0.0 -> 0.016/0.0 = +inf, and the x87 ftol of infinity is the
            // integer-indefinite 0x80000000 (INT_MIN) -- nonzero, so the min-1 guard does NOT fire.
            // The updater then applies ordinary wrapping ADD/SUB. From phase
            // zero, zero-time forward alternates INT_MIN/zero without stopping;
            // zero-time reverse clamps back to zero and stops on its first tick.
            const double seconds = static_cast<double>(p4) / 65536.0; // base; p4==0 -> 0.0
            const double rate_f = (0.016 / seconds) * 65536.0;        // +inf when p4==0
            int32_t rate;
            if (rate_f != rate_f || rate_f >= 2147483648.0 || rate_f < -2147483648.0) {
                rate = static_cast<int32_t>(0x80000000); // ftol integer-indefinite (inf/NaN/overflow)
            } else {
                rate = static_cast<int32_t>(rate_f);      // truncate toward zero
            }
            if (rate == 0) rate = 1;                      // min-1 guard (does NOT fire for INT_MIN)
            comp.f[AiBrain::kPartAnimDir0 + slot] = play_type;  // comp+436+4*slot (direction)
            comp.f[AiBrain::kPartAnimRate0 + slot] = rate;      // comp+444+4*slot (rate)
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
            // Tracked-TODO: alert(5/6/0x16), accuracy(8), AISETSTATE(0x1C),
            // etc. (notes/mission/anim-ai-grill-2026-06-07.md). No-op so an unported sub-type
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
        const int32_t north = sample(pos[0], pos[1] + sample_radius); // sub_4142C0(e,0,+r)
        if (north > 0) maxHeight = north;                           // [orig: if(north>0) max=north]
        const int32_t south = sample(pos[0], pos[1] - sample_radius); // sub_4142C0(e,0,-r)
        if (south > maxHeight) maxHeight = south;
        const int32_t east = sample(pos[0] + sample_radius, pos[1]);  // sub_414320(e,+r,0)
        if (east > maxHeight) maxHeight = east;
        const int32_t west = sample(pos[0] - sample_radius, pos[1]);  // sub_4142C0(e,-r,0)
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
void AiSystem::apply_ground_clamp(AiEntity &e) {
    if (terrain == nullptr) return;
    GroundClearance clearance = ground_clearance;
    clearance.has_physics = e.has_physics;                    // [orig: entity+368 gate]
    clearance.use_dead = (e.health <= 0);                     // [orig: health<=0 dead path]
    const int32_t ground = calc_average_ground_height(*terrain, e.pos, 0x50000, clearance);
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
