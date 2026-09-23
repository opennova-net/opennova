#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/ai.h>
#include <runtime/devtools/tick_profile.h>

// The AI event queue and the AiSystem core: registration, per-entity rows, and the
// tick that drives every handler above.

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/body_anim.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/vehicle_part_anim.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "ai_detail.h"

#include <runtime/world/world.h>

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

namespace {

// Apply only the carrier-owned body frame. This is deliberately separate from
// pose_if_mounted's input, gunner-look, animation, and wire-state work so the
// non-organic controllers can refresh after the pool-1 motor without advancing
// those behaviors twice. Organic bodies run after vehicles and pose only once.
int32_t apply_resolved_mounted_seat_frame(AiEntity &e, World &world,
                                          Entity &occupant, Entity &vehicle,
                                          const Seat &seat) {
    const MountedPose pose = world.vehicles.pose_mounted_occupant(occupant, vehicle, seat);
    // Capture the full body frame before independent LOOK restores registry yaw.
    // [orig: Entity_AttachToBoneAndUpdateTransform @0x546620..0x546664]
    const int32_t resolved_heading = pose.heading;
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
        e.body_pitch = pose.pitch;
        e.roll = pose.roll;
    }
    // Organics present from AiEntity.pos, not Entity.position.
    e.pos[0] = to_fixed(occupant.position.x);
    e.pos[1] = to_fixed(occupant.position.y);
    e.pos[2] = to_fixed(occupant.position.z);
    return resolved_heading;
}

bool npc_mounted_body(const AiEntity &e, const Entity &occupant) {
    return e.inf.active && !e.inf.is_local_player && !e.net_is_remote_peer &&
            ((occupant.flags | occupant.engine_flags) & kEntityFlagPlayer) == 0;
}

// A read-applied remote player in either vehicle-control seat has the same
// split pose as the local driver: LOOK remains player/wire-owned because the
// authority vehicle motor consumes Entity::yaw, while the carried body frame
// remains seat-owned. NPC drivers instead run the org1 independent look chase.
bool remote_player_controls_vehicle(const AiEntity &e, const Entity &occupant,
                                    const Seat &seat) {
    return e.inf.active && e.net_is_remote_peer &&
           occupant.handle.pool() == 0 && occupant.player_class != 0 &&
           is_vehicle_control_seat(seat.type);
}

} // namespace

// The mounted body's heading refresh after the seat frame is applied: the local
// player, every NPC seat, a gunner and a remote peer driving the vehicle keep
// their full-precision LOOK heading/pitch after the seat transform. Other wire
// riders adopt the seat heading. [orig: org1 mounted @0x4BEF57..0x4BEF97;
// Entity_AttachToBoneAndUpdateTransform @0x5463D0, the look
// restore @0x546661 / @0x546664; world-wac-ai-re.md §22.4 / §5.38]
bool AiSystem::refresh_mounted_pose(AiEntity &e, World &world) {
    Entity *occupant = world.registry.get(e.handle);
    if (occupant == nullptr || occupant->motor_suspended || !occupant->mounted || occupant->health <= 0) return false;
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
        (e.inf.is_local_player || npc_mounted_body(e, *occupant) ||
         seat.type == SeatType::Gunner || remote_player_controls_vehicle(e, *occupant, seat))) {
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

// The AI component allocator: the lowest slot whose owner word is zero, else a
// new one (retail's fixed 812-byte array unk_AED380 is walked from index 0 until
// [0] == 0 and the slot is memset @0x460246). A brain already bound to h is freed
// first so one handle never carries two live brains (the array is walked directly
// here, whereas retail reaches brains through the entity pointer).
// [orig: Entity_InitVehicleAI @0x460204..0x460257]
int AiSystem::attach(EntityHandle h) {
    // One brain per row is the engine's own invariant (retail keeps the single
    // +0x64 brain pointer per entity and frees it with the row, Entity_Destroy
    // @0x43e810); a despawn releases it (release()). attach() itself does not
    // evict a live brain for the same handle: test rigs attach several brains
    // under one placeholder handle, and evicting them here aliases the callers'
    // pointers. Freed slots (owner word 0) are reused lowest-first.
    int index = -1;
    for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
        if (entities_[i].brain.f[AiBrain::kOwner] == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        entities_.emplace_back();
        index = static_cast<int>(entities_.size()) - 1;
    }
    AiEntity &e = entities_[index];
    e = AiEntity{};
    e.handle = h;
    e.brain.f[AiBrain::kOwner] = 1; // nonzero = live slot
    if (h.valid()) handle_to_ai_index_[h.packed] = index;
    return index;
}

void AiSystem::release(EntityHandle h) {
    const int index = index_for_handle(h);
    if (index < 0) return;
    entities_[index] = AiEntity{}; // owner word 0 = free slot [orig: memset @0x43e995]
    handle_to_ai_index_[h.packed] = -1;
}

int AiSystem::attach_dismemberment_piece(
        EntityHandle h, const AiEntity &source, const int32_t impulse_q16[3]) {
    release(h);
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
    int index = -1;
    for (int i = 0; i < static_cast<int>(entities_.size()); ++i) {
        if (entities_[i].brain.f[AiBrain::kOwner] == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        entities_.push_back(std::move(piece));
        index = static_cast<int>(entities_.size()) - 1;
    } else {
        entities_[index] = std::move(piece);
    }
    if (h.valid()) handle_to_ai_index_[h.packed] = index;
    return index;
}

AiEntity *AiSystem::at(int ai_index) {
    if (ai_index < 0 || ai_index >= static_cast<int>(entities_.size())) return nullptr;
    return &entities_[ai_index];
}

const AiEntity *AiSystem::at(int ai_index) const {
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

const AiEntity *AiSystem::for_handle(EntityHandle h) const {
    return const_cast<AiSystem *>(this)->for_handle(h);
}

int AiSystem::index_for_handle(EntityHandle h) const {
    if (!h.valid()) return -1;
    if (handle_to_ai_index_.size() != 65536) return -1;
    const int index = handle_to_ai_index_[h.packed];
    if (index < 0 || index >= static_cast<int>(entities_.size())) return -1;
    return entities_[index].handle == h ? index : -1;
}

// Per-entity movement controller row 4, not a world scheduling budget.
// [orig: AI_BeginUpdate @0x457b40] See world-wac-ai-re.md (D-AI-14).
bool AiSystem::begin_update(AiEntity &e) {
    AiBrain &b = e.brain;
    b.f[AiBrain::kOutSpeed] = b.f[AiBrain::kSpeedA]; // [128]=[49]
    b.f[138] = e.profile.field220;
    b.f[134] = 0;
    b.f[133] = 0;
    b.f[131] = io::bam_add(b.f[51], e.profile.field216);
    b.f[132] = e.heading;
    const int32_t phase = e.aircraft_phase;
    if (phase > 496) { // controller+16, strict greater-than [orig: @0x457b9f]
        if ((e.profile.flags100 & 2) == 0)
            b.f[AiBrain::kPendState] = 8;
        else
            b.f[AiBrain::kPendState] = b.f[AiBrain::kFallback];
        return false;
    }
    e.aircraft_phase = io::bam_add(phase, b.f[AiBrain::kStep]);
    return true;
}

namespace {
// The per-class constants that separate the two brain machines. Everything else
// in the two functions is instruction-for-instruction identical.
struct StateMachineGates {
    int32_t alert_hold;    // the alert leg leaves pending alone when cur == this
    int32_t alert_pend;    // ... and otherwise pends this evade state
    int32_t client_tick_a; // a client (!is_authority) ticks only these two states
    int32_t client_tick_b;
    int32_t commit_single; // a client commits pending == this ...
    int32_t commit_lo;     // ... or commit_lo < pending <= commit_hi
    int32_t commit_hi;
    int32_t notify_channel; // the kill/damage notification AIEvent's channel word
};
// [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0 — alert @0x458239..0x45823b,
//  tick gate @0x458340..0x458348, commit gate @0x458375..0x458382, notification word 9
//  @0x458312]
constexpr StateMachineGates kAirClassGates{14, 10, 13, 15, 7, 12, 15, 9};
// [orig: EntityAI_ProcessVehicleStateMachine @0x4583c0 — alert @0x458442..0x458448,
//  tick gate @0x458545..0x45854d, commit gate @0x458579..0x458586, notification word bx (=0,
//  xor ebx,ebx @0x4583cd) @0x45851a]
constexpr StateMachineGates kVehicleClassGates{22, 18, 21, 23, 16, 20, 23, 0};

void process_class_state_machine(
        AiSystem &sys, AiEntity &e, World &world, int event, const StateMachineGates &g) {
    AiBrain &b = e.brain;

    // The no-target idle latch: a brain with both ammo counts spent, or whose
    // profile resolved no ammo byte in either weapon block, latches idle unless
    // it carries gunner attachments [orig: EntityAI_ProcessInfantryStateMachine
    // @0x4581C4..0x4581F4; EntityAI_ProcessVehicleStateMachine @0x4583D7..0x458402:
    // brain+0xD4/+0xD8, profile bytes +0x94/+0xB4, guard brain+0x240].
    if ((b.f[AiBrain::kAmmoA] == 0 && b.f[AiBrain::kAmmoB] == 0) ||
            (e.profile.fire_a.ammo_byte() == 0 && e.profile.fire_b.ammo_byte() == 0)) {
        if (b.f[AiBrain::kGuard] == 0)
            b.f[AiBrain::kNoTargetIdle] = 1;
    }

    // The alert edge needs an occupant (entity+0x170, the vehicle's first
    // claimant) that is not a Player; an empty hull or a player-driven one only
    // records the alert [orig: EntityAI_ProcessInfantryStateMachine
    // @0x45820D..0x45823B; EntityAI_ProcessVehicleStateMachine @0x45841A..0x458448].
    const Entity *self = world.registry.get(e.handle);
    const Entity *occupant = self != nullptr ? world.registry.get(self->primary_occupant) : nullptr;
    int32_t alert = b.f[AiBrain::kAlert];
    if (sys.is_authority && occupant != nullptr &&
        ((occupant->flags | occupant->engine_flags) & kEntityFlagPlayer) == 0 &&
        b.f[AiBrain::kPrevAlert] != alert) {
        alert = 2;
        if ((e.profile.flags96 & 2) == 0 && b.f[AiBrain::kCurState] != g.alert_hold)
            b.f[AiBrain::kPendState] = g.alert_pend;
    }
    b.f[AiBrain::kPrevAlert] = alert;

    AiThinkCtx ctx{&sys, &e, &world, nullptr};
    const int ai_index = sys.index_of(e);

    // The transition tail: authority applies the pending transition; clients apply
    // only the class's restricted subset [orig: EntityAI_ProcessInfantryStateMachine
    // @0x458369..0x4583B0; EntityAI_ProcessVehicleStateMachine @0x45856E..0x4585B4].
    const auto client_commits = [&](int32_t pend) {
        return pend == g.commit_single || (pend > g.commit_lo && pend <= g.commit_hi);
    };
    auto finish = [&]() {
        if (sys.is_authority) {
            sys.apply_transition(e, world);
        } else if (client_commits(b.f[AiBrain::kPendState])) {
            sys.apply_transition(e, world);
        }
    };

    if (event == 0) {
        if (sys.is_authority || b.f[AiBrain::kCurState] == g.client_tick_a ||
                b.f[AiBrain::kCurState] == g.client_tick_b)
            sys.row(b.f[AiBrain::kCurState]).tick(ctx);
        // A dead-state tick may destroy the entity and free this brain (the retail
        // Server_RemoveEntityAndNotify inside AI_TickState_VehicleDead @0x467ede);
        // the zeroed slot then has nothing to count or commit.
        if (b.f[AiBrain::kOwner] == 0)
            return;
        ++b.f[AiBrain::kTick];
        // The brain-step mirror: the pool-1 visit's think countdown (entity+684)
        // is re-armed from brain[7] after every event-0 update, on clients too
        // (the tick-table gate above is separate); the visit's own trailing
        // decrement follows, so the next think lands brain[7] pool-1 visits
        // later [orig: EntityAI_ProcessVehicleStateMachine
        // @0x458561..0x458568; EntityAI_ProcessInfantryStateMachine @0x45835c..0x458363].
        // Neither machine touches a body-anim selection.
        if (Entity *ent = world.registry.get(e.handle))
            ent->spawn_phase = b.f[AiBrain::kStep];
        finish();
        return;
    }
    if (event == 1) { // the kill/damage notification
        AiEventEntry ev{};
        ev.f[0] = 1;
        ev.f[1] = g.notify_channel | (ai_index << 16); // channel word | entity index
        ev.set_timer(0.0f);
        // [orig: ev.f[3] = Projectile_GetHitRecord()[17] @0x458326] the notification payload read from
        // the current hit record (Projectile_GetHitRecord @0x4e7000 returns the hitRecord global; its
        // field [17] is not modeled here, so f[3] is left 0). If the notification reaches a ground
        // combat-event handler (cur_state in {16,17,18}), h_combat_event reads f[3] into brain[39]
        // (kDamageInfo).
        sys.events.queue(ev);
        finish();
        return;
    }
    if (event == 4) { // death
        if (sys.is_authority) {
            sys.apply_transition(e, world);
            return;
        }
        if (sys.is_in_session) {
            // The entity record's health word is zeroed before the death tick so the
            // dispatched tick takes its death path on a death event; the mirror
            // follows [orig: `mov [edi+11Eh],ax` @0x45827F; ground @0x45848B].
            if (Entity *ent = world.registry.get(e.handle)) ent->health = 0;
            e.health = 0;
            sys.row(b.f[AiBrain::kCurState]).tick(ctx);
            if (b.f[AiBrain::kOwner] == 0)
                return;
            AiEventEntry ev{};
            ev.f[0] = 20;
            ev.f[1] = 9 | (ai_index << 16); // both classes write 9 here [orig: @0x4582c8/@0x4584d4]
            ev.set_timer(0.0f);
            sys.events.queue(ev);
            finish();
            return;
        }
        if (client_commits(b.f[AiBrain::kPendState]))
            sys.apply_transition(e, world);
        return;
    }
    // Every other event still reaches the class-specific transition tail.
    // [orig: EntityAI_ProcessVehicleStateMachine @0x45846D; air @0x458261]
    finish();
}
} // namespace

// [orig: EntityAI_ProcessInfantryStateMachine @0x4581b0] event 0=update, 1=kill/damage
// notification, 4=death.
void AiSystem::process_infantry_state_machine(AiEntity &e, World &world, int event) {
    process_class_state_machine(*this, e, world, event, kAirClassGates);
}

// [orig: EntityAI_ProcessVehicleStateMachine @0x4583c0] event 0=update, 1=kill/damage
// notification, 4=death.
void AiSystem::process_vehicle_state_machine(AiEntity &e, World &world, int event) {
    process_class_state_machine(*this, e, world, event, kVehicleClassGates);
}

void AiSystem::apply_transition(AiEntity &e, World &world) {
    AiBrain &b = e.brain;
    int32_t cur = b.f[AiBrain::kCurState];
    if (b.f[AiBrain::kPendState] != cur) {
        AiThinkCtx ctx{this, &e, &world, nullptr};
        row(cur).exit(ctx);                       // off_815240
        row(b.f[AiBrain::kPendState]).enter(ctx); // off_815238
        b.f[AiBrain::kCurState] = b.f[AiBrain::kPendState];
        // A committed transition zeroes the pool-1 think countdown (entity+684),
        // so the brain thinks again on the very next pool-1 visit — both class
        // machines share this commit arm [orig: EntityAI_ProcessVehicleStateMachine
        // @0x4585ae..0x4585b4; EntityAI_ProcessInfantryStateMachine @0x4583b0].
        // The enter handler may have destroyed the entity: re-resolve it.
        if (b.f[AiBrain::kOwner] != 0)
            if (Entity *ent = world.registry.get(e.handle))
                ent->spawn_phase = 0;
    }
}

void AiSystem::tick(World &world, const TickContext &ctx) {
    // AI does not run during the BMS pre-mission script pass: that invocation only
    // settles initial scripted state (EventFlags PreMission), it does not step brains.
    if (ctx.phase != TickPhase::Gameplay) return;
    // The phases lap onto the SIM_AI_* rows of the world's profile (ADR 0043
    // d5); an inactive profile reads no clock.
    devtools::ProfileLap lap(world.profile);
    is_authority = ctx.is_authority;
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
                world.script.relations.group(victim_entity->group_id).alert =
                        TriggerRelations::kAlertRed;
            }
            // Self-damage does not stamp a reaction or attacker. Retail tests the
            // timer against 25, then adds 10 without clamping (24 becomes 34).
            // This is Entity_OnDamageReceived's TAIL; its HEAD -- the local
            // player's white hit flash floor and the explosive near-miss camera
            // shake, both keyed on the ammo record -- is ported as
            // world::entity_on_damage_received (collision_force.cpp), called
            // from the explosion sweep where the ammo row is still in hand. The
            // RoundHit drained here carries no ammo, so the split is deliberate.
            // [orig: Entity_OnDamageReceived @0x4af859..0x4af878; the head
            //  @0x4af812..0x4af84b]
            if (hit.shooter != victim->handle) {
                victim->inf.was_hit = true;
                if (victim->inf.damage_timer < 25) victim->inf.damage_timer += 10;
                victim->inf.last_attacker = hit.shooter;
            }
        } else {
            AiEventEntry ev{};
            ev.f[0] = 1; // damage
            ev.f[1] = (index_of(*victim) << 16);
			// HitRecord[17] is the projectile owner, not the damage amount.
			// [orig: Projectile_CopyEntityToHitRecord @0x4E7010]
			ev.f[3] = hit.shooter.valid() ? int32_t(hit.shooter.packed) + 1 : 0;
			ev.set_timer(0.0f);
			events.queue(ev);
        }
    }
    world.round_sim.hits.clear();
    // [orig: Entity_UpdateAllEntities @0x4C21F6, immediately before faces]
    world.teammates.tick(world);
    // Facial interpolation precedes the pool-0 infantry callback walk.
    // [orig: Entity_UpdateAllEntities @0x4C21FB]
    world.facials.tick(world);
    lap.mark(devtools::Slot::SIM_AI_REACTIONS);
    // Rebuild the pool-0/1 proximity tables once per tick, before any entity update
    // (the pool-2 statics table rebuilds only on its registry/instance edges).
    // [orig: Entity_UpdateAllEntities @0x4c2100 -> Entity_BuildProximityLists_Pool01
    // @0x4b9340 every tick (@0x4c240a) + Entity_BuildProximityListsFromPools
    // @0x4b8eb0 (per-entity candidate slices, every 17th tick @0x4c2416); the statics
    // table is Entity_BuildAllProximityLists @0x4c20f0 at mission start/teleport]
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
    lap.mark(devtools::Slot::SIM_AI_COLLISION);
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
        // A freed AI component (owner word 0) is not an entity's brain any more:
        // retail reaches brains only through live entities' +100 pointer.
        if (e.brain.f[AiBrain::kOwner] == 0 || e.inf.active) continue;
        const devtools::ProfileScope entity_scope(
                world.profile, devtools::Slot::SIM_AI_OTHER_ENTITIES);
        const Entity *motor_entity = world.registry.get(e.handle);
        const bool motor_suspended = motor_entity != nullptr && motor_entity->motor_suspended;
        // Non-infantry mounted controllers retain the seat-follow shortcut.
        if (!motor_suspended && pose_if_mounted(e, world))
            continue;
        // The pool-1 visit's think gate: the class event callback (the brain
        // machine) runs only on the visits where the PRE-decrement entity+684
        // countdown (Entity::spawn_phase) is <= 0, and the countdown drops by
        // one on every visit (after the machine's re-arm), so a brain thinks
        // once every brain[7] visits (16 with the class init's step 16), on the
        // visit after any committed transition (apply_transition zeroes it),
        // spread by the class init's 0..15 spawn stagger. The +0x1C4 motor runs
        // every visit regardless
        // (world.vehicles.tick_motors below). A brain outside pool 1 has no
        // pool-1 visit and keeps the every-tick think.
        // [orig: Entity_UpdatePool1Slot @0x4B8DD0 gate @0x4B8E1B..0x4B8E22, the
        //  +0x1C8 call @0x4B8E3C, the decrement @0x4B8EA0; the pool-1 walk
        //  Entity_UpdateAllEntities @0x4C2158..0x4C21F1; the seed
        //  Entity_InitVehicleAIFromDef @0x468915..0x468945 /
        //  Entity_InitHelicopterAIFromDef @0x468645..0x468669]
        // A think visit first refreshes the thinking entity's own blink/indoors
        // state -- Entity_BuildProximityList @0x4B3DC0 is CollisionWorld::refresh_blink
        // (the call @0x4B8E25 on the +684 gate alone, ahead of the +0x1C8
        // callback). The per-source candidate slice is the separate 17-tick
        // Entity_BuildProximityListsFromPools @0x4B8EB0 rebuild
        // (collision->build_tick_tables above).
        Entity *countdown_entity = e.handle.pool() == 1 ? world.registry.get(e.handle) : nullptr;
        const uint64_t countdown_lifetime =
                countdown_entity != nullptr ? countdown_entity->registry_spawn_id : 0;
        const bool think = countdown_entity == nullptr || countdown_entity->spawn_phase <= 0;
        if (think && countdown_entity != nullptr && collision != nullptr)
            collision->refresh_blink(world, *countdown_entity);
        // The class callback has no AI_BeginUpdate admission gate. That leaf
        // belongs to movement controller row 4 and its per-entity phase.
        // [orig: Entity_UpdatePool1Slot @0x4B8E1B..0x4B8E53]
        if (think) {
            const Entity *ent = world.registry.get(e.handle);
            const VehicleTraits *vt =
                    ent != nullptr ? world.vehicles.traits.get(ent->item_id) : nullptr;
            // The brain machine is the item's class event callback fn1, keyed by
            // items.def ai_function: CHel/cpln -> the air machine, cveh/cbot/ctrn ->
            // the vehicle machine [orig: g_EntityClassEventCallbackTable @0x813000
            // rows @0x8132a0/@0x8133a8 vs @0x813378/@0x813390/@0x8133c0, resolved
            // by EntityDef_InitAllCallbacks @0x4a5aae]. A traits row built without
            // its def (test rigs) falls back to its mover family; a brain with no
            // traits row keeps the air machine it always ran.
            bool vehicle_class = false;
            if (vt != nullptr) {
                if (vt->brain_class == VehicleBrainClass::Unset)
                    vehicle_class = !vehicle_family_uses_direct_air_mover(vt->family);
                else
                    vehicle_class = vt->brain_class == VehicleBrainClass::Ground;
            }
            if (vehicle_class)
                process_vehicle_state_machine(e, world, 0);
            else
                process_infantry_state_machine(e, world, 0);
            // The dead-state tick can destroy the entity and free this brain.
            if (e.brain.f[AiBrain::kOwner] == 0) continue;
            // The machine only decides; nothing here moves the body. The row's
            // own +0x1C4 physics callback, resolved from its items.def
            // move_function, is its one mover: the vehicle movers
            // (world.vehicles.tick_motors) integrate the brain's outputs, and
            // every other physics-table row leaves them unintegrated, so a
            // brain without a vehicle traits row stays where it is.
            // [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53 (the +0x1C4
            //  call); g_EntityClassPhysicsTable @0x82ABC8]
		}
        // The visit's trailing decrement, every pool-1 visit whether or not the
        // brain thought — the think may have destroyed and re-used the slot, so
        // only the same registry lifetime counts down [orig: `add [esi+2ACh],-1`
        // @0x4B8EA0 after the motor/emitter/light legs].
        if (countdown_entity != nullptr) {
            Entity *live = world.registry.get(e.handle);
            if (live != nullptr && live->registry_spawn_id == countdown_lifetime)
                --live->spawn_phase;
        }
    }
    lap.mark(devtools::Slot::SIM_AI_ENTITIES);
    world.vehicles.tick_motors(is_authority, lap);
    // The ewep class update runs every tick, occupied or not.
    // [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53]
    world.registry.for_each_in_pool(1, [&](const Entity &entity) {
        tick_emplaced_weapon_class_update(world, *world.registry.get(entity.handle));
    });
    // Pool 1 precedes pool 0: deck riders consume the carrier's CURRENT
    // motor delta, then resolve contacts against that same pose.
    // [orig: Entity_UpdateAllEntities @0x4C2158..0x4C21F1 before the
    // pool-0 callback walk @0x4C2426..0x4C2474]
    for (int i = 0; i < count(); ++i) {
        AiEntity &e = *at(i);
        if (e.brain.f[AiBrain::kOwner] == 0 || !e.inf.active) continue;
        const Entity *motor_entity = world.registry.get(e.handle);
        const bool motor_suspended = motor_entity != nullptr && motor_entity->motor_suspended;
        const devtools::ProfileScope entity_scope(world.profile, devtools::Slot::SIM_AI_INFANTRY);
        if (motor_suspended) continue;
        // Joiners retain seat-follow presentation for wire-owned peers. The
        // authority continues into the remote org2 animation/collision tail:
        // mounted contact callbacks remain live while model push is suppressed.
        if (e.net_is_remote_peer && pose_if_mounted(e, world) && !is_authority)
            continue;
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
    }
    lap.mark(devtools::Slot::SIM_AI_ENTITIES);
    events.process_timed(*this, world);
    lap.mark(devtools::Slot::SIM_AI_EVENTS);
}

void AiSystem::pump_mounted_weapon_slots(World &world, uint32_t logic_tick) {
    // The joiner's canonical borrowers are pumped by its replica pass.
    if (world.rules.mp_session && !world.rules.projectile_authority) return;
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
        if (world.rules.external_local_mounted_weapon_pump &&
            owner->handle == world.cached.local_player)
            continue;
        AiEntity *gunner = for_handle(owner->handle);
        if (gunner == nullptr || mount->primary_weapon_slot_adm == 0xFF) continue;
        const WeaponTableEntry *weapon =
                world.tables.weapons.by_index(mount->primary_weapon_slot_adm);
        if (weapon == nullptr || weapon->ammo_index < 0) continue;

        WeaponFsmInputs inputs;
        inputs.is_local = owner->handle == world.cached.local_player;
        inputs.is_authority = is_authority;
        inputs.auto_reload = true;
        // The heat window is derived from the tick, so the pump needs it. AI gunners
        // sit on the emplaced guns that actually author heat, so this is the path
        // that overheats in practice. [orig: current_tick @ 0x24C1968]
        inputs.current_tick = static_cast<int32_t>(logic_tick);
        weapon_fire_environment_inputs(world, *owner, inputs);
        WeaponFsmEvents weapon_events;
        weapon_fsm_tick(weapon->action_fsm, mount->primary_weapon_slot,
                        inputs, weapon_events);
        weapon_sound_publish(world, *owner, weapon->action_fsm, weapon_events);
        if (!weapon_events.fired || !is_authority) continue;

        // The slot owner is the gunner, while its def/ammo live on the parent.
        // A UseGun shot leaves through the gunner branch every mounted shooter
        // shares: the EMPLACEMENT's posed fire userpoint for the barrel the
        // clip selected before this shot spent its round (a gfx3 def's launch
        // point instead), along that bone's euler; byte 0 or no model copies
        // the parent's raw position/euler.
        // [orig: WeaponAction_Fire @0x542B10 (the Entity_CalcWeaponFirePosition
        //  call @0x542bf7) -> Entity_CalcWeaponFirePosition @0x4dc750 parentSlot 3
        //  (the Entity_ComputeUserpointWorldTransform call @0x4dc7f6; barrel =
        //  slot[+0x10] & 3 @0x545D40..0x545D4B, read before the consume_weapon_ammo
        //  call @0x542C75); the fire command copies out[0..2] and out[3]/out[4]
        //  @0x42be84..0x42bef2]. The point is the
        //  slot's FIRE field (b): the host's own re-derivation names field 0
        //  outright [orig: Server_ClientFiredRound @0x50c1f4]; the m/c fields
        //  anchor the effect legs, not the round.
        int32_t fire[6];
        usegun_fire_pose(world, *mount, weapon, weapon_events.fired_clip_before_consume, fire);
		// The fire tail rocks the tank along the gun's point for the slot as
		// the FSM leaves it: next = RECOIL (the mflash column) and the clip
		// already spent. [orig: WeaponAction_Fire tail @0x542D1F..0x542D5B]
		world.vehicles.weapon_recoil(*owner,
				weapon->action_fsm.actions[weapon_action::kFire].action_value, weapon,
				mount->primary_weapon_slot.clip, 1);
		if (fire_ai_round(world, *gunner, fire, fire[3], fire[4], weapon->ammo_index))
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
        world.vehicles.detach(e.handle);
        return false;
    }
    if (occ->mount_seat < 0 || occ->mount_seat >= static_cast<int>(veh->seats.size())) return false;
    // An NPC rider of a parent with an item def scrubs the chute/in-air/afloat/
    // ladder/dive/armory bits and stops its vertical velocity every tick (the
    // in-air bit's motor mirror goes with it). [orig: Entity_UpdateInfantryAI
    //  parent def gate @0x4BEBFA..0x4BEBFD, `and dword ptr [esi+24h],0FF8F57DFh`
    //  @0x4BEC03, `mov [esi+0A0h],ebx` @0x4BEC15]
    if (npc_mounted_body(e, *occ) && veh->has_item_def) {
        occ->flags &= 0xFF8F57DFu;
        occ->engine_flags &= 0xFF8F57DFu;
        e.inf.vel[2] = 0;
        e.inf.airborne = false;
    }
    // Both organic movers refresh groundEntity from parentEntity before the
    // mounted branch. PLYRONSSN follows this link through a turret to its hull
    // (07TR's boarding gate); retaining the pre-boarding contact stalls it.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0;
    //  Entity_UpdateInfantryAI @0x4B9910; PLYRONSSN @0x4F1260]
    occ->ground_target = veh->handle;
    const Seat &seat = veh->seats[occ->mount_seat];
    // Local input owns LOOK before retail evaluates the parent UseGun bone. Our
    // split AiEntity keeps that input in the infantry latch until the mounted
    // branch, so expose it before the host asks for the live parent pose.
    if (e.inf.active && e.inf.is_local_player) {
        e.heading = e.inf.target_heading;
        e.pitch = e.inf.look_pitch;
    }
    // The gun's own update precedes this body's: retail walks pool 1 before
    // the pool-0 organics, and the ewep pair refreshes the gun's stored
    // yaw/pitch words from the look this tick starts with (an IsTurret gun
    // slews them and tethers the gunner's own yaw), then pins the words to
    // the seat/weapon window and STORES the pinned look into the occupant.
    // Every look mirror below therefore carries the tethered/pinned look.
    // [orig: Entity_UpdateAllEntities @0x4c2100 (the pool-1 walk before the
    //  organic loop); Entity_UpdatePool1Slot @0x4b8dd0 ai-fn @0x4b8e3c then
    //  class update @0x4b8e53 — Entity_UpdateChildAttachment @0x4409A0 +
    //  Entity_UpdateTransformAndTurret @0x440ca0, gated on the UseGun
    //  claimant [esi+170h] whose parent is this gun @0x4411e4]
    if (seat.type == SeatType::Gunner && veh->primary_weapon_owner == occ->handle)
        tick_emplaced_weapon_channel(world, *veh, *occ, e);
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
    } else if (e.inf.active && (seat.type == SeatType::Gunner || npc_mounted_body(e, *occ))) {
        // Attachment writes the seat/base pose but restores the child's independent
        // live look. The look then chases the desired solution instead of snapping:
        // yaw quarter-step clamped to +/-0x02000000, pitch eighth-step. Most mount
        // configs also constrain look to +/-90 degrees around the attached base.
        // [orig: save/restore @0x546416..0x546664; chase @0x4bef57..0x4bef97;
        //  base-relative clamp @0x4bef9a..0x4beff0]
        e.heading = saved_look_heading;
        e.pitch = saved_look_pitch;
        if (npc_mounted_body(e, *occ) || e.inf.aim_valid)
            infantry_look_tick(e.inf, e.heading, e.pitch, true);
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
        // The chase above moves the look a model-aware provider may read for
        // the NPC root/body (the gun's own EWEAP words are the stored pair the
        // channel tick at the head already refreshed, and do not move with the
        // chase). Resolve once more so the root/body presented after this tick
        // sits on the Hn+1 look. Keep the first frame as the existing clamp
        // base and preserve LOOK as the child's independent heading/pitch
        // after the second seat resolve.
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
            e.inf.request_body_animation(mounted_state);
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

// [flt_7C3310=1/65536, flt_7C3B40=0.016f, flt_7C32BC=65536.] The dividend is
// the SINGLE-precision 0.016f (0x3C83126F, 0.01600000075995922) widened by the
// `fdivr` load, not the double 0.016 — ANIMTIME = 1 (1/65536 s) gives 68719480,
// where the double gives 68719476. The original computes `base`
// unconditionally, so ANIMTIME==0 -> base 0.0 -> 0.016f/0.0 = +inf, and the x87
// ftol of infinity is the integer-indefinite 0x80000000 (INT_MIN) — nonzero,
// so the min-1 guard does NOT fire. The sweep step then applies ordinary wrapping
// ADD/SUB: from phase zero, zero-time forward alternates INT_MIN/zero without
// stopping; zero-time reverse clamps back to zero and stops on its first tick.
// [orig: Entity_ApplyCommand @0x43B1A9..0x43B1F9, `fdivr ds:flt_7C3B40` @0x43B1D8]
int32_t part_anim_rate_from_seconds(double seconds) {
    const double rate_f = (static_cast<double>(0.016f) / seconds) * 65536.0; // +inf when seconds==0
    int32_t rate;
    if (rate_f != rate_f || rate_f >= 2147483648.0 || rate_f < -2147483648.0) {
        rate = static_cast<int32_t>(0x80000000); // ftol integer-indefinite
    } else {
        rate = static_cast<int32_t>(rate_f);     // truncate toward zero
    }
    if (rate == 0) rate = 1; // min-1 guard (does NOT fire for INT_MIN)
    return rate;
}

// [orig: Entity_ApplyCommand @0x43ab60] See the header. These are the arms
// which mutate the AI brain synchronously; queued command events are consumed
// by AiSystem::ai_handle_command.
void ai_apply_command(AiBrain &comp, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    switch (sub_type) {
        case 0x20: // AIUSEWPZ [orig: Entity_ApplyCommand case 32 @0x43B154,
                   //  `mov [eax+1B0h],1` @0x43B164]
            // [orig: AI_UpdateMovementTarget reads brain+432 @0x460FC7]
            // (ported as AiSystem::update_aircraft_waypoint_movement).
            comp.f[AiBrain::kUseWaypointZones] = 1;
            break;
        case 0x21: // AICLEARWPZ [orig: case 33 @0x43B173, the store @0x43B183]
            comp.f[AiBrain::kUseWaypointZones] = 0;
            break;
        case 0x22: { // PLAYPARTANIM: p2=ANIMNUM(channel), p3=ANIMPLAYTYPE, p4=ANIMTIME(16.16 s)
            const int channel = p2;
            if (channel != 1 && channel != 2) return;              // only channels 1,2 act
            const int play_type = p3;
            if (static_cast<unsigned>(play_type + 1) > 2u) return; // play_type in {-1,0,1}
            const int slot = channel - 1;
            // rate = (0.016f / seconds) * 65536 phase-units/tick, min 1 —
            // shared with the editor-preview binding (see
            // part_anim_rate_from_seconds below for the witnessed FPU shape).
            const double seconds = static_cast<double>(p4) / 65536.0; // base; p4==0 -> 0.0
            comp.f[AiBrain::kPartAnimDir0 + slot] = play_type;  // comp+436+4*slot (direction)
            comp.f[AiBrain::kPartAnimRate0 + slot] =            // comp+444+4*slot (rate)
                    part_anim_rate_from_seconds(seconds);
            break;
        }
        default:
            // Controller/entity arms are applied at EntityCommands; queued
            // state, skill, speed, alert, and fire commands are consumed by
            // AiSystem::ai_handle_command. Unknown sub-types remain no-ops.
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
    relmat_calls.clear();
    rel_ops.clear();
    target_set_calls.clear();
    scan_candidates_.clear();
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
    // [orig: the caller's brain+0x30 when dead (@0x45734D), else brain+0x2C (@0x457367),
    //  both skipped for a brainless caller (@0x457333).]
    result += clearance.use_dead ? clearance.dead_offset : clearance.alive_offset;
    return result;
}

// ----------------------------------------------------------------------------

} // namespace opennova::world
