#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/ai.h>
#include <runtime/devtools/tick_profile.h>
#include <base/io/crt_ftol.h>
#include <base/io/fixed.h>         // kQ22One (the barrage's Q22 table reads)
#include <base/io/rotating_prng.h> // the barrage's ROL4 steps

// The AI event queue and the AiSystem core: registration, per-entity rows, and the
// tick that drives every handler above.

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/body_anim.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_spawn.h>
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

// The mounted body's heading refresh after the seat frame is applied: every
// organic rider keeps its full-precision LOOK heading/pitch after the seat
// transform. The organic movers reach the UseGun attach for seat type 3 alone,
// whose tail restores both look words saved at its entry; their other seats'
// carry writes no look word (D-NET-NEW-L6-3). A non-organic brain keeps the
// words the seat pose gave it: retail runs no seat mover for one (the seat bone's
// readers are the organic movers, the request, the rope renderer and the suspension
// bounce; its only non-organic mount is the WAC's) [orig:
// Entity_GetBoneTransformAndOrientation @0x4B0C50, its callers @0x436540 / @0x4561F6 /
// @0x4567F7 / @0x4B6510 / @0x4BED72; WacScript_TryMountEntityToVehicle @0x4F70F0].
// [orig: Entity_UpdateInfantryPlayerBody `cmp [esi+168h], 3` @0x4B63A5..0x4B63BF,
//  the carry @0x4B6510..0x4B6575; Entity_UpdateInfantryAI @0x4BEC0F..0x4BEC1B;
//  Entity_AttachToBoneAndUpdateTransform @0x5463D0 - the saves @0x546416..0x54642C,
//  the seat Yaw into +0x2D4 / +0x2D8 / +0x2E4 / +0x2E8 @0x546635..0x54665B, the
//  restores @0x546661 (Yaw) / @0x546664 (Pitch); org1 mounted @0x4BEF57..0x4BEF97;
//  world-wac-ai-re.md §22.4 / §5.38]
bool AiSystem::refresh_mounted_pose(AiEntity &e, World &world) {
    Entity *occupant = world.registry.get(e.handle);
    if (occupant == nullptr || occupant->motor_suspended || !occupant->mounted || occupant->health <= 0) return false;
    Entity *vehicle = world.registry.get(occupant->mount_target);
    Seat seat;
    if (vehicle == nullptr || !world.vehicles.rider_seat(*occupant, *vehicle, seat))
        return false;
    const int32_t saved_look_heading = e.heading;
    const int32_t saved_look_pitch = e.pitch;
    const int32_t seat_heading =
            apply_resolved_mounted_seat_frame(e, world, *occupant, *vehicle, seat);
    if (!e.inf.active) {
        e.heading = seat_heading;
        return true;
    }
    // Independent LOOK was already promoted/chased in pose_if_mounted. Restore
    // that exact value without consulting input latches or advancing aim again;
    // the Yaw and Pitch words both take the look back.
    e.heading = saved_look_heading;
    e.pitch = saved_look_pitch;
    occupant->heading = saved_look_heading;
    occupant->pitch_bam = saved_look_pitch;
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
    bind_self(e);
    e.brain.f[AiBrain::kOwner] = 1; // nonzero = live slot
    if (h.valid()) handle_to_ai_index_[h.packed] = index;
    return index;
}

void AiSystem::bind_self(AiEntity &e) {
    e.self = registry_ != nullptr ? registry_->row_storage(e.handle) : nullptr;
}

void AiSystem::release(EntityHandle h) {
    const int index = index_for_handle(h);
    if (index < 0) return;
    entities_[index] = AiEntity{}; // owner word 0 = free slot [orig: memset @0x43e995]
    handle_to_ai_index_[h.packed] = -1;
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
    if (defer_piece_brains_) {
        deferred_piece_brains_.emplace_back(h, std::move(piece));
        return -1;
    }
    return place_dismemberment_piece(h, std::move(piece));
}

int AiSystem::place_dismemberment_piece(EntityHandle h, AiEntity &&piece) {
    release(h);
    bind_self(piece);
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
// [orig: EntityAI_ProcessAirStateMachine @0x4581b0 — alert @0x458239..0x45823b,
//  tick gate @0x458340..0x458348, commit gate @0x458375..0x458382, notification word 9
//  @0x458312]
constexpr StateMachineGates kAirClassGates{14, 10, 13, 15, 7, 12, 15, 9};
// [orig: EntityAI_ProcessGroundStateMachine @0x4583c0 — alert @0x458442..0x458448,
//  tick gate @0x458545..0x45854d, commit gate @0x458579..0x458586, notification word bx (=0,
//  xor ebx,ebx @0x4583cd) @0x45851a]
constexpr StateMachineGates kVehicleClassGates{22, 18, 21, 23, 16, 20, 23, 0};

void process_class_state_machine(
        AiSystem &sys, AiEntity &e, World &world, int event, const StateMachineGates &g) {
    AiBrain &b = e.brain;

    // The no-target idle latch: a brain with both ammo counts spent, or whose
    // profile resolved no ammo byte in either weapon block, latches idle unless
    // it carries gunner attachments [orig: EntityAI_ProcessAirStateMachine
    // @0x4581C4..0x4581F4; EntityAI_ProcessGroundStateMachine @0x4583D7..0x458402:
    // brain+0xD4/+0xD8, profile bytes +0x94/+0xB4, guard brain+0x240].
    if ((b.f[AiBrain::kAmmoA] == 0 && b.f[AiBrain::kAmmoB] == 0) ||
            (e.profile.fire_a.ammo_byte() == 0 && e.profile.fire_b.ammo_byte() == 0)) {
        if (b.f[AiBrain::kGuard] == 0)
            b.f[AiBrain::kNoTargetIdle] = 1;
    }

    // The alert edge needs an occupant (entity+0x170, the vehicle's first
    // claimant) that is not a Player; an empty hull or a player-driven one only
    // records the alert [orig: EntityAI_ProcessAirStateMachine
    // @0x45820D..0x45823B; EntityAI_ProcessGroundStateMachine @0x45841A..0x458448].
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
    // only the class's restricted subset [orig: EntityAI_ProcessAirStateMachine
    // @0x458369..0x4583B0; EntityAI_ProcessGroundStateMachine @0x45856E..0x4585B4].
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
        // later [orig: EntityAI_ProcessGroundStateMachine
        // @0x458561..0x458568; EntityAI_ProcessAirStateMachine @0x45835c..0x458363].
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
        // The notification payload is the hit record's owner word, as a script
        // kill left it (a KillSingle item row keeps it) [orig:
        // EntityAI_ProcessAirStateMachine `mov ecx,[eax+44h]` @0x45831E,
        // stored into the event @0x458326; EntityAI_ProcessGroundStateMachine
        // @0x458524]. If the notification reaches a ground combat-event handler
        // (cur_state in {16,17,18}), h_combat_event reads f[3] into brain[39]
        // (kDamageInfo).
        const EntityHandle owner = world.round_sim.hit_record.owner;
        ev.f[3] = owner.valid() ? int32_t(owner.packed) + 1 : 0;
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
    // [orig: EntityAI_ProcessGroundStateMachine @0x45846D; air @0x458261]
    finish();
}
} // namespace

// [orig: EntityAI_ProcessAirStateMachine @0x4581b0] event 0=update, 1=kill/damage
// notification, 4=death.
void AiSystem::process_air_state_machine(AiEntity &e, World &world, int event) {
    process_class_state_machine(*this, e, world, event, kAirClassGates);
}

// [orig: EntityAI_ProcessGroundStateMachine @0x4583c0] event 0=update, 1=kill/damage
// notification, 4=death.
void AiSystem::process_ground_state_machine(AiEntity &e, World &world, int event) {
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
        // machines share this commit arm [orig: EntityAI_ProcessGroundStateMachine
        // @0x4585ae..0x4585b4; EntityAI_ProcessAirStateMachine @0x4583b0].
        // The enter handler may have destroyed the entity: re-resolve it.
        if (b.f[AiBrain::kOwner] != 0)
            if (Entity *ent = world.registry.get(e.handle))
                ent->spawn_phase = 0;
    }
}

void AiSystem::apply_round_hits(World &world) {
    // Infantry get wasHit/lastAttacker (consumed by the §17.1 scan + §17.3 hit
    // reactions), SM brains get the type-1 damage AIEvent (h_combat_event -> evade),
    // which the next tick's timed-event pass dispatches.
    // [orig: the damage chain writes the victim entity + queues the event inline —
    // Projectile_ProcessDamageOnTarget @0x4e7fb0; our sim/AI split drains a record.]
    for (const RoundHit &hit : world.round_sim.hits) {
        AiEntity *victim = for_handle(hit.victim);
        if (victim == nullptr) continue;
        if (victim->inf.active) {
            // Every ordinary damage callback alerts an NPC and its trigger group,
            // before lethal/nonlethal handling. The player flag skips this AI leg.
            // [orig: OrganicClass_HandleEvent @0x4073c8..0x4073ea]
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
}

void AiSystem::think_brain(AiEntity &e, World &world) {
    const Entity *ent = world.registry.get(e.handle);
    const VehicleTraits *vt =
            ent != nullptr ? world.vehicles.traits.get(ent->item_id) : nullptr;
    if (vt == nullptr || vt->brain_class == VehicleBrainClass::Unset) return;
    // The machine only decides; nothing here moves the body. The row's own
    // +0x1C4 physics callback, resolved from its items.def move_function, is
    // its one mover: the vehicle movers integrate the brain's outputs.
    // [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53 (the +0x1C4 call);
    //  g_EntityClassPhysicsTable @0x82ABC8]
    if (vt->brain_class == VehicleBrainClass::Ground)
        process_ground_state_machine(e, world, 0);
    else
        process_air_state_machine(e, world, 0);
}

// A person body bound to a vehicle, aircraft or genx row runs that row's mover over its own
// words; true when `row` is one of them (the body ran its mover). A vehicle or aircraft mover
// loads the brain word (+0x64) at its head, which a person's class init never fills, stamps the
// savedLivePose words from the body's Position and Yaw / Pitch / Roll words, and on the null brain
// jumps to its matrix-build tail (the matrix of +4 into +0xB4, Flags |= 0x20000): no ground probe,
// carrier ride, health cadence, input, motion or sound. Every mover a row's dispatcher selects
// takes that path, whatever the def's +0x8DC selector or the afloat bit picks.
// [orig: Entity_DispatchPhysics_cveh @0x48EFC0 (the +0x8DC select @0x48EFC7) and
//  Entity_DispatchPhysics_ctrn @0x48F060 -> Entity_UpdateVehiclePhysics @0x48AF00 (`mov edi,
//  [esi+64h]` @0x48AF1A, the stamp @0x48AF74..0x48AFA0, `jz` @0x48AFA6 -> the tail
//  @0x48D43E..0x48D451) or Vehicle_ProcessSelectorZeroGround @0x46E100 (@0x46E119, the stamp
//  @0x46E12D..0x46E15F, `jz` @0x46E165 -> @0x46F9B2..0x46F9C5); Entity_DispatchPhysicsUpdate
//  @0x48F010 (catv: the afloat bit @0x48F014, else the same pair @0x48F041);
//  Entity_DispatchPhysics_ctank @0x48F000 -> Entity_UpdateTankVehiclePhysics @0x488AB0
//  (@0x488ACA, the stamp @0x488B24..0x488B50, `jz` @0x488B56 -> @0x48AEB6..0x48AEC9);
//  Entity_DispatchPhysics_cbike @0x48EFF0 -> Entity_UpdateLightVehiclePhysics @0x483FE0
//  (@0x483FFA, the stamp @0x484054..0x484080, `jz` @0x484086 -> @0x486A04..0x486A17);
//  Entity_DispatchPhysics_cbot @0x48EF90 -> Entity_UpdateWatercraftPhysics @0x48D480
//  (@0x48D499, the stamp @0x48D4AC..0x48D4E9, `jz` @0x48D4EF -> @0x48EF50..0x48EF63) or
//  Vehicle_ProcessSelectorZeroBoat @0x46FA00 (@0x46FA19, the stamp @0x46FA2C..0x46FA5E, `jz`
//  @0x46FA64 -> @0x471670..0x471683); cpln j_Entity_UpdateAircraftPhysics @0x45D6F0 and CHel
//  -> Entity_UpdateAircraftPhysics @0x490310 (`mov ebx, [esi+64h]` @0x49032C, the stamp
//  @0x490337..0x49036B, `jz` @0x490371 -> @0x492753..0x492766)]
// The genx row's mover is the parent follow over the body's own words: the spawn stored it as
// the body's +0x1C4, which the drop motion models (a re-fall replaces it), so the body runs its
// installed drop motion, the ride on its +0x28 ground entity (update_dropped_object).
// [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - the +0x1C4 store @0x43C453;
//  Entity_UpdateParentTransform @0x4A88B0 - the def+0x8DC gate @0x4A88BB, the stamp
//  @0x4A88D5..0x4A8902, the +0x28 read @0x4A88F1]
// The rokt, hlfr and jvln movers write the body's +0x2B8 pending dword, which the motor's
// pending anim (and its wire mirror, the dword the records test whole) is: rokt sets the high
// word (+0x2BA) to 1 while it is 0; hlfr and jvln set the low word's bit 0 when the body has no
// +0x170 link (Entity::primary_occupant), then return while that bit is set.
// [orig: Entity_UpdateRocketFlag @0x443DD0 - the test @0x443DD4, the store @0x443DDE;
//  Entity_UpdateGuidedMissile @0x446690 - the +0x170 test @0x4466B3..0x4466C9, `or word ptr
//  [esi+2B8h], 1` @0x4466CB, the bit test @0x4466DA..0x4466DC to the return @0x446B82;
//  Missile_UpdateGuidance @0x446BA0 - @0x446BBD..0x446BD7, @0x446BD9, @0x446BE8..0x446BEA to
//  the return @0x44712F]
// The envs, door and squib movers write no word of a person body's: envs registers its def's
// loop emitter, door rebinds its def model's bone callbacks, and squib returns at its head on
// the zero death tick (+0x1AC) a live body's update would have written; nor does upfx while
// the body's 0x4 bit is clear (its def+0x8DC return, or envs's emitter). [orig:
// Entity_UpdateEnvSoundEmitter @0x4A8080; Entity_SetDefaultBoneCallbacks @0x4A91D0 (the def
// model @0x4A91D8, the slots @0x4A91F0..0x4A922D); Entity_ProcessProjectileTravel @0x448D50 -
// `cmp dword ptr [ebp+1ACh], 0` @0x448D5E, `jz` @0x448D65 to the return @0x4497EE;
// Entity_UpdateWaterPhysicsAndEffects @0x4A92E0 - @0x4A92E8, `test byte ptr [esi+24h], 4`
// @0x4A92F5, the envs leg @0x4A93D6]
// The envs row's mover IS the loop emitter, so the body registers its def's regional loop on
// every pool-0 visit, with no gate; upfx's tail registers it on the batch's last tick for a def
// with no physics selector (def+0x8DC) and a body with neither Flags 0x2 nor 0x4. Ahead of that
// tail, a body whose 0x4 bit is set (on a person the NVG bit, the view refresh's) runs the
// husk leg: the swap fade, the authority's one S2C 0x26 in a session (sub-type 0x20), and a
// building def's two blasts (0x80). [orig: Entity_UpdateWaterPhysicsAndEffects @0x4A92E0 - the
// bit @0x4A92F5, Entity_PublishSwapFadePhases called @0x4A9304, the 0x26 @0x4A9310..0x4A9336,
// the +0x34 test @0x4A933D, the blasts @0x4A934E..0x4A93BD]
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-0 `call eax` @0x4C2472 (the envs row's
//  entry Entity_UpdateEnvSoundEmitter @0x4A8080); Entity_UpdateWaterPhysicsAndEffects @0x4A92E0
//  - the def+0x8DC test @0x4A92E8, `test byte ptr [esi+24h], 6` @0x4A93C5, dword_24E0E80
//  @0x4A93CC, the call @0x4A93D6]
// The psec row's mover is the physics step over the body's words, its velocity triple (+0x98 /
// +0x9C / +0xA0) the motor's (InfantryState::vel) and its +0x270 word Entity::palm_state, zero
// on a person (the spawn's memset; the class inits and the spawn reset write neither).
// [orig: Entity_UpdatePhysicsStep @0x53BE10 - the stamp @0x53BE1E..0x53BE4D, the +0xA0 read
//  @0x53BE58, the +0x270 test @0x53BE81; Entity_ResetToSpawnState @0x4B9610 - the zeroed
//  +0x98..+0xA8 @0x4B9668..0x4B9686; PlayerClass_InitEntity @0x4B1060]
// The ele0 row's mover (update_elevator) runs over a person's words: its lift the +0x1A8 word
// (InfantryState::target_heading, the spawn reset's Yaw word), its vertical step +0xA0 the
// motor's, its state +0x29C and previous state +0x2A0 the spawn memset's 0 on a player body
// (Entity::veh.speed, Entity::script_next_ssn); the body's +0x28 ground entity the resolver's.
// [orig: Entity_UpdateFallAndLandState @0x4A20E0 - +0x29C @0x4A2116, +0x1A8 @0x4A2124, +0xA0
//  @0x4A2133, +0x2A0 @0x4A2187; Entity_ResetToSpawnState @0x4B9610 - +0x1A8 @0x4B9692]
// The arty row's mover zeroes the velocity triple (the motor's), then on the session's
// authority sends S2C 0x44 group 6 (Entity_SendEffectPacket, 0x136, msgClass 1): the +0x170
// link's handle (0xFFFF with none), the +0x78 word, then the def+0x164 serializer's body, which
// only the guided class rows' serializer writes for group 6, the attach triple +0x2E4 / +0x2E8 /
// +0x2EC (the legs' re-plant pair and the aim heading on a person; the spawn reset's Yaw words).
// The person rows' serializers and the vehicles' return -1 off group 11 and every other row has
// none, so no packet leaves. Ahead of the send its barrage leg runs over the words the arty class
// init stored at the spawn (player_body_class_init; D-NET-NEW-L4-23).
// [orig: ArtyClass_UpdateBarrage @0x447140 - the zeroes @0x44714F..0x447163, the gate
//  @0x447148 / @0x447185 / @0x447199 / @0x4471AE, the send @0x4472C9..0x4472D9;
//  Entity_SendEffectPacket @0x445C40 - the serialize @0x445C6C, the -1 test @0x445C76, the send
//  @0x445CA2; NetPacket_SerializeEntityForServer @0x4D6240 - the authority and def+0x164 test
//  @0x4D62B1, the +0x170 handle @0x4D62B9..0x4D62F3, +0x78 @0x4D62FC, the group @0x4D632C;
//  Entity_SerializeGuidedMissileState @0x447C50 - the group switch @0x447C80, group 6's +0x2E4 /
//  +0x2E8 / +0x2EC writes @0x447DF6..0x447E44; NetPacket_SerializePlayerState
//  @0x4C09C0 - `cmp [esi+1Ch], 0Bh` @0x4C09F6, the -1 @0x4C20D1; NetPacket_SerializeInfantryEntityState
//  @0x4C0320 - @0x4C06E9, the -1 @0x4C06D8; Entity_SerializeVehicleState @0x460560 - @0x460B47,
//  the -1 @0x460B3C; Entity_ResetToSpawnState @0x4B9610 - +0x2EC / +0x2E4 / +0x2E8
//  @0x4B9698..0x4B96A4; ArtyClass_InitFromDef @0x443430 - +0x2C4 = 10 @0x443473]
// The ewep row's mover reads a person's +0x2D4 word, the spawn reset's Yaw, as its weapon def:
// on the batch's last tick, while the +0x30F byte (the top byte of the aimPoint X word, which
// the organic init's anchor writes) is nonzero, it loads [Yaw+0x294] and, counting that byte
// down, [Yaw+0x298]. Those two garbage-pointer reads are a deliberate fix of a retail bug: ours
// never runs them over a person (PERMANENT, ratified by the maintainer, Q16-1 (a), 2026-10-22).
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - `mov esi, [ebp+2D4h]` @0x440E8E, the
//  byte @0x4413A6, @0x4413B0, @0x441417; Entity_ResetToSpawnState @0x4B9610 - +0x2D4
//  @0x4B96AA; Entity_InitOrganicAI @0x4BFCC0 - +0x30C @0x4C01E8 / @0x4C0238]
// The rest of the ewep mover runs over the person's words (person_ewep_mover), save the legs
// that need a word ours does not yet home on a person (D-NET-NEW-L4-14 (a), narrowed): the
// anchor byte's refill from the ground def's slot table, the joiner's C2S 0x0F pair and
// destroy, the anchored bone attach, and the +0x170 occupant's clamp or detach.
// The other item rows' movers over a person's words are not run (D-NET-NEW-L4-14 (a),
// narrowed): towr, nade, rock, schl, clym, arti, aflr, gflr, stng,
// squib past a nonzero death tick, hlfr / jvln's guidance past a +0x170 link and an even
// pending word, and door's model callbacks.
static void set_anim_pending(AiEntity &e, Entity &body, int32_t word) {
    e.inf.anim_pending = word;
    body.net_anim_pending = word;
}

// The ewep row's mover over a person body's words. The ground entity (+0x28) is the carrier: a
// dead PlayerControl ground hides the body (Flags bit 0) and ends the update. The +0x2D4 word
// (the right leg's chase yaw, the spawn reset's Yaw) stands for the slot def: zero ends the
// update when there is a ground. A vehicle ground (def type 1) clears the body's Flags bits
// 0..2 (on a person the hide, dead and NVG bits), copies its 16-bit Health word and its blink
// quad; its def+0x144 CTRL writer writes only the global bus, which ours composes with the
// carrier's pose. Unanchored (the +0x319 byte, the spawn backup X's second byte, outside
// 1..the ground model's userpoint count), a ground whose matrix bit is clear gives the body its
// root pose: the Position, Yaw, Pitch and Roll words and savedLivePose as they are. The body's
// matrix bit is set either way. Then, with the +0x2D4 word nonzero, the spin tail over the
// spawn backup's bytes: while the aim anchor X's top byte (+0x30F) is nonzero, +0x31B (the
// backup X's top byte) takes 60 and +0x30F counts down; +0x31B coasts down a step a tick,
// adding 32 times what is left to the 16-bit word +0x320 (the backup Z's low half).
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the ground @0x440CBF, the dead
//  PlayerControl hide @0x440CCA..0x440CE1, +0x2D4 @0x440E8E, the gate @0x440EA0, def type 1
//  @0x440EA6..0x440EB1, `and [ebp+24h], 0FFFFFFF8h` @0x440EB3, Health @0x440EB7..0x440EBE,
//  the quad @0x440EC5..0x440EEF, def+0x144 @0x440EF5..0x440EFF, the anchor test
//  @0x440F04..0x440F34, the matrix-bit test @0x4410EA..0x4410F1, the root copy
//  @0x441151..0x4411BC, `or [ebp+24h], 20000h` @0x4411C2, the gate @0x4411C9, the countdown
//  @0x4413FE..0x44140E, the coast @0x44142B..0x441447; Entity_ResetToSpawnState @0x4B9610 -
//  the backup @0x4B9632..0x4B9644, +0x2D4 @0x4B96AA]
static void person_ewep_mover(World &world, AiEntity &e, Entity &body) {
    Entity *ground = world.registry.get(body.ground_target);
    if (emplaced_carrier_is_dead_hull(ground)) {
        body.flags |= kEntityFlagCarried;
        return;
    }
    const int32_t def_word = e.inf.leg_yaw[0];
    if (ground != nullptr) {
        if (def_word == 0) return;
        if (ground->has_item_def && ground->item_type == 1) {
            constexpr uint32_t kLowBits = kEntityFlagCarried | kEntityFlagDead | kEntityFlagNVGWorn;
            body.flags &= ~kLowBits;
            body.engine_flags &= ~kLowBits;
            body.alive = true;
            const int16_t health = static_cast<int16_t>(ground->health);
            body.health = health;
            e.health = health;
            for (int i = 0; i < 4; ++i) body.blink_hits[i] = ground->blink_hits[i];
        }
        const int8_t anchor = static_cast<int8_t>(
                static_cast<uint32_t>(body.spawn_position.x.q16) >> 8);
        const bool anchored = ground->has_graphic_model && ground->graphic_model_userpoints > 0 &&
                anchor > 0 && static_cast<uint32_t>(anchor) <= ground->graphic_model_userpoints;
        if (!anchored && ((ground->flags | ground->engine_flags) & kEntityFlagMatrixBuilt) == 0) {
            body.position = ground->position;
            const int32_t yaw = ground->veh.yaw_seeded ? ground->veh.yaw_bam : ground->heading;
            const int32_t pitch = ground->veh.yaw_seeded ? ground->veh.air_pitch_bam
                                                         : ground->pitch_bam;
            const int32_t roll = ground->veh.yaw_seeded ? ground->veh.air_roll_bam
                                                        : ground->roll_bam;
            body.heading = e.heading = yaw;
            body.pitch_bam = e.pitch = pitch;
            body.roll_bam = e.roll = roll;
            std::copy_n(ground->saved_live_pos, 3, body.saved_live_pos);
            body.saved_live_yaw = ground->saved_live_yaw;
            body.saved_live_pitch = ground->saved_live_pitch;
            body.saved_live_roll = ground->saved_live_roll;
            body.saved_live_valid = ground->saved_live_valid;
        }
        body.engine_flags |= kEntityFlagMatrixBuilt;
    }
    if (def_word == 0) return;
    uint32_t anchor_x = static_cast<uint32_t>(e.inf.aim_point[0]);
    uint32_t backup_x = static_cast<uint32_t>(body.spawn_position.x.q16);
    if ((anchor_x >> 24) != 0) {
        backup_x = (backup_x & 0x00FFFFFFu) | (60u << 24);
        anchor_x -= 1u << 24;
        e.inf.aim_point[0] = static_cast<int32_t>(anchor_x);
    }
    uint8_t coast = static_cast<uint8_t>(backup_x >> 24);
    if (coast != 0) {
        --coast;
        backup_x = (backup_x & 0x00FFFFFFu) | (static_cast<uint32_t>(coast) << 24);
        if (coast != 0) {
            // `movsx dx, al; shl dx, 5; add [ebp+320h], dx`: a 16-bit add.
            const uint32_t z = static_cast<uint32_t>(body.spawn_position.z.q16);
            const int step = static_cast<int8_t>(coast) * 32;
            const uint16_t word = static_cast<uint16_t>(static_cast<uint32_t>(z) +
                    static_cast<uint32_t>(step));
            body.spawn_position.z.q16 = static_cast<int32_t>((z & 0xFFFF0000u) | word);
        }
    }
    body.spawn_position.x.q16 = static_cast<int32_t>(backup_x);
}

static bool person_item_mover(World &world, PhysicsClass row, AiEntity &e, Entity &body) {
    switch (row) {
    case PhysicsClass::Ewep:
        person_ewep_mover(world, e, body);
        return true;
    case PhysicsClass::Genx:
        update_dropped_object(world, body);
        return true;
    case PhysicsClass::Rokt:
        if ((static_cast<uint32_t>(e.inf.anim_pending) >> 16) == 0)
            set_anim_pending(e, body, (e.inf.anim_pending & 0xFFFF) | 0x10000);
        return true;
    case PhysicsClass::Hlfr:
    case PhysicsClass::Jvln:
        if (!body.primary_occupant.valid()) set_anim_pending(e, body, e.inf.anim_pending | 1);
        return true;
    case PhysicsClass::Psec:
        update_physics_step(world, body, e.inf.vel[0], e.inf.vel[1], e.inf.vel[2]);
        return true;
    case PhysicsClass::Envs:
        update_item_ambient_sound(world, body);
        return true;
    case PhysicsClass::Upfx: {
        const ItemDeathTraits *traits = world.tables.item_death_traits.get(body.item_id);
        if (traits == nullptr || traits->physics != 0) return true;
        const uint32_t flags = body.flags | body.engine_flags;
        if ((flags & kEntityFlagNVGWorn) != 0) {
            publish_swap_fade_phases(world, body);
            upfx_husk_leg(world, body, *traits, body.destroy_progress);
        }
        if ((flags & (kEntityFlagDead | kEntityFlagHusk)) == 0 && world.rules.last_tick_of_batch)
            update_item_ambient_sound(world, body);
        return true;
    }
    case PhysicsClass::Door:
    case PhysicsClass::Squib:
        return true;
    case PhysicsClass::Ele0:
        update_elevator(world, body, e.inf.target_heading, e.inf.vel[2], body.veh.speed,
                body.script_next_ssn);
        return true;
    case PhysicsClass::Arty: {
        // The barrage over the person's words (D-NET-NEW-L4-23 (b), (c)): +0x2BC the start tick,
        // +0x2C4 the shells left, +0x2C8 the delay, the Roll word's third byte the phase, the
        // Pitch word the ROL4 stream. Each 32nd tick past the delay it steps the stream twice,
        // fires the round of the +0x2B0 index from the Position pushed out along the Yaw word
        // less the second step shifted up 16, by 50 units times the first step's low 17 bits
        // over 2^16, at the mip chain's ground plus 700 units, pitch word 0xC0000040, with the
        // +0x170 link as shooter, and counts a shell down.
        // [orig: ArtyClass_UpdateBarrage @0x447140 - +0x2BC @0x447148, the gate @0x44717D /
        //  @0x447185 / @0x447192..0x44719F / @0x4471A5..0x4471AE, the steps @0x4471B4..0x4471E9,
        //  the table index @0x447203..0x44721E, the radius @0x44723D..0x447260, X / Y
        //  @0x447264..0x447288, Foliage_SampleDensityBilinear @0x606200 called @0x447290,
        //  +0x2BC0000 @0x44729B, the block's pitch @0x4472B2, WeaponSlot_FireAndSpawnEffects @0x53F440 called
        //  @0x4472BA (+0x170 @0x447295, +0x2B0 @0x4472A4), the count @0x4472C2]
        const int32_t start = e.inf.anim_state;
        for (int axis = 0; axis < 3; ++axis) e.inf.vel[axis] = 0;
        const uint32_t elapsed = world.logic_tick - static_cast<uint32_t>(start);
        if (start != 0 && e.inf.wpn_deferred != 0 &&
                static_cast<int32_t>(elapsed) > e.inf.wpn_state &&
                ((elapsed ^ ((static_cast<uint32_t>(e.roll) >> 16) & 0xFFu)) & 0x1Fu) == 0) {
            uint32_t state = static_cast<uint32_t>(e.pitch);
            io::rotating_prng_next16(state);
            const uint32_t step = state;
            io::rotating_prng_next16(state);
            e.pitch = static_cast<int32_t>(state);
            body.pitch_bam = e.pitch;
            const int index = io::bam_table_index(
                    static_cast<uint32_t>(e.heading) - (state << 16) + 0x200000u);
            const int64_t cos_q22 = static_cast<int64_t>(io::bam_table_cos(index) * io::kQ22One);
            const int64_t sin_q22 = static_cast<int64_t>(io::bam_table_sin(index) * io::kQ22One);
            const int32_t radius = static_cast<int32_t>(
                    (int64_t{0x320000} * static_cast<int32_t>(step & 0x1FFFFu) + 0x8000) >> 16);
            FixedVec3 origin = body.position.q16();
            origin.x = io::bam_add(origin.x, static_cast<int32_t>((radius * cos_q22) >> 22));
            origin.y = io::bam_add(origin.y, static_cast<int32_t>((radius * sin_q22) >> 22));
            const int32_t ground = world.tables.terrain != nullptr
                    ? terrain::height_field_mip_max_bilinear_fixed(
                              *world.tables.terrain, origin.x, origin.y)
                    : 0;
            origin.z = io::bam_add(ground, 0x2BC0000);
            world.round_sim.fire_npc_ammo(world, body.primary_occupant, origin, e.heading,
                    static_cast<int32_t>(0xC0000040u), body.equipped_adm_index);
            --e.inf.wpn_deferred;
        }
        if (world.rules.mp_session && world.rules.logic_authority &&
                player_item_row(world, body).guided_record) {
            GuidedUpdateEvent send;
            send.shooter = body.primary_occupant.valid() ? body.primary_occupant.packed : 0xFFFF;
            send.net_id = static_cast<uint16_t>(body.owner_connection_id);
            send.group = 6;
            send.state.saved[0] = e.inf.leg_target[0];
            send.state.saved[1] = e.inf.leg_target[1];
            send.state.saved[2] = e.inf.aim_heading;
            world.out.entity_events.push_back(send);
        }
        return true;
    }
    case PhysicsClass::Cveh:
    case PhysicsClass::Ctrn:
    case PhysicsClass::Catv:
    case PhysicsClass::Ctank:
    case PhysicsClass::Cbike:
    case PhysicsClass::Cbot:
    case PhysicsClass::Cpln:
    case PhysicsClass::Chel:
        stamp_saved_live_pose(body);
        body.engine_flags |= kEntityFlagMatrixBuilt;
        return true;
    default:
        return false;
    }
}

void AiSystem::update_organic(AiEntity &e, World &world, uint32_t logic_tick) {
    const Entity *motor_entity = world.registry.get(e.handle);
    const bool motor_suspended = motor_entity != nullptr && motor_entity->motor_suspended;
    const devtools::ProfileScope entity_scope(world.profile, devtools::Slot::SIM_AI_INFANTRY);
    if (motor_suspended) return;
    // The body's +0x1C4 is its bound row's move function: org1 the AI update, org2 the
    // player-body update. The null row's stub is skipped, org0's and chld's are bare
    // returns, and a body bound to an item row runs that row's mover over its own words:
    // a vehicle, aircraft or genx row's, the rokt / hlfr / jvln rows' pending-word writes,
    // psec's physics step, envs's and upfx's loop emitter, arty's barrage and session
    // send, ewep's ground and spin legs (person_item_mover, the pool-0 walk's: a pool-1 slot's
    // visit runs a row's vehicle
    // mover and drop motion in its own legs); the other item rows' movers are not run here
    // (D-NET-NEW-L4-14 (a), narrowed).
    // [orig: Entity_UpdateAllEntities @0x4C2466..0x4C2472; Entity_UpdatePool1Slot
    //  @0x4B8E41..0x4B8E53; Entity_UpdateOrg0Movement @0x4AFF60; the chld row's nullsub_82
    //  @0x45D540]
    const PhysicsClass move_row = body_move_row(world, e, motor_entity);
    if (Entity *body = world.registry.get(e.handle); body != nullptr &&
            e.handle.pool() == 0 && person_item_mover(world, move_row, e, *body))
        return;
    if (move_row != PhysicsClass::Org1 && move_row != PhysicsClass::Org2) return;
    // Joiners retain seat-follow presentation for wire-owned peers. The
    // authority continues into the remote org2 animation/collision tail:
    // mounted contact callbacks remain live while model push is suppressed.
    if (e.net_is_remote_peer && !is_authority && pose_if_mounted(e, world))
        return;
    // A client-only wire peer has no authority collision tail, so its
    // blink/indoors presentation state comes from the position-only refresh.
    // [orig: remote persons
    // refresh via the net position/create handlers — NapiNPClientMsg_0x00F
    // @0x42e442, NetPacket_HandleEntityCreate @0x42f227; the @0x4c229c per-tick
    // walk is pool-2 statics on an 8-per-tick stagger, not persons]
    if (collision != nullptr && collision->instance_count() != 0 &&
            e.net_is_remote_peer && !is_authority) {
        if (Entity *ent = world.registry.get(e.handle))
            collision->refresh_blink(world, *ent);
    }
    // The walk runs on a JOINER (client, !is_authority) too: tick_infantry's
    // §5.38 entity==g_local_player branch (no authority guard) motor-sims the
    // joiner's own player from input, while NPC think/select stays
    // authority-gated. A header-only join keeps remote organics in ClientState
    // rather than this AI array; any native non-authority rows from an explicit
    // complete-BMS/debug join just hold idle. REMOTE presentation reads the
    // host's S2C 0x0A ClientState, so that idle tick cannot overwrite wire pose.
    // [orig: the client also runs the per-entity AI tick;
    //  Entity_UpdateInfantryAI @0x4b9910 simulate-when entity==local.]
    // A piece its death edge cuts off gets its brain once the update returns.
    defer_piece_brains_ = true;
    tick_infantry(e, world, logic_tick);
    defer_piece_brains_ = false;
    for (auto &deferred : deferred_piece_brains_)
        place_dismemberment_piece(deferred.first, std::move(deferred.second));
    deferred_piece_brains_.clear();
}

// The seats that borrow the mount's persistent slot: UseGun, and the ctrlx
// seat of an EWeap carrier. [orig: Entity_AttachToUseGunSlot @0x546B80;
// Entity_AttachToVehicleSlot @0x49480B..0x494883]
static bool seat_borrows_mount_slot(const Entity &owner, const Entity &mount) {
    return owner.mount_type == SeatType::Gunner ||
           (owner.mount_type == SeatType::Controller &&
            (mount.item_attrib & kItemAttribEweap) != 0);
}

// A borrower's link holds while it lives, stays seated in a borrowing seat and
// still names the mount that names it.
static bool gunner_holds_mount(const Entity &owner, const Entity &mount) {
    return owner.health > 0 && owner.mounted && seat_borrows_mount_slot(owner, mount) &&
           owner.mount_target == mount.handle;
}

void AiSystem::release_stale_gunner_link(World &world, Entity &mount) {
    if (!mount.primary_weapon_owner.valid()) return;
    const Entity *owner = world.registry.get(mount.primary_weapon_owner);
    if (owner == nullptr || !gunner_holds_mount(*owner, mount))
        mount.primary_weapon_owner = EntityHandle{};
}

// A remote player's own slot, the row of its table its EquippedSlot names, pumped
// with no input: the authority's arm of each accepted C2S 0x06 runs its FIRE, then
// the walk runs RECOIL and, an empty clip with rounds left, RELOAD, whose request
// relays as S2C 0x49 while nothing refills the host's copy but the player's own
// C2S 0x25 (D-NET-336). The pools and the loaded-round bucket it reads are the
// player's slot tables. [orig: WeaponAction_ProcessAllEntities @0x5426AD..0x5426C1
// -> WeaponAction_ProcessFrame @0x540E60 (the def test @0x540E74);
// Server_ClientFiredRound @0x50C27F..0x50C28B (the row it names);
// Entity_GetScoreValueBySlotType @0x5406E0; WeaponSlot_GetSharedLoadedRounds
// @0x5405F0]
void AiSystem::pump_personal_slot(World &world, Entity &owner, uint32_t logic_tick) {
    const auto row = owner.weapon_slots.find(owner.equipped_slot.combo);
    if (row == owner.weapon_slots.end()) return;
    const WeaponTableEntry *weapon = world.tables.weapons.by_index(row->second.adm_index);
    if (weapon == nullptr) return;
    PersonalWeaponSlot &slot = row->second;
    const WeaponAmmoSeat seat = entity_ammo_seat(world, owner);
    int32_t pool = 0;
    int32_t loaded = 0;
    if (world.remote_weapon_tables != nullptr)
        (void)world.remote_weapon_tables->remote_ammo(world, owner.handle,
                weapon->ammo_class_id, weapon->ammo_bucket, pool, loaded);
    slot.reserve = weapon_seat_pool(seat, weapon->ammo_class_id, pool);
    // A bucket weapon's loaded rounds are the bucket's; the row's own word stays.
    const int32_t own_clip = slot.clip;
    if (weapon->ammo_bucket != 0) slot.clip = weapon_seat_bucket(seat, loaded);
    WeaponFsmInputs inputs;
    inputs.is_local = false;
    inputs.is_authority = is_authority;
    inputs.reload_refills = reload_echo_refills(world, owner);
    inputs.auto_reload = world.rules.auto_reload; // [orig: g_AutoReloadEnabled @ 0x24D2118]
    inputs.current_tick = static_cast<int32_t>(logic_tick);
    weapon_fire_environment_inputs(world, owner, inputs);
    WeaponFsmEvents events;
    weapon_fsm_tick(weapon->action_fsm, slot, inputs, events);
    if (weapon->ammo_bucket != 0) slot.clip = own_clip;
    if (events.reload_requested && is_authority) queue_walk_reload(world, owner, *weapon);
}

// Every pool-0 row's EquippedSlot is pumped, whatever its seat: a borrower's is
// the carrier slot it names, seated or, after a refused UseGun attach, on foot
// (D-NET-420); a remote player's own is a row of its table (D-NET-336).
// [orig: WeaponAction_ProcessAllEntities @0x5426AD..0x5426C1]
void AiSystem::pump_gunner_slot(World &world, Entity &owner, uint32_t logic_tick) {
    if (owner.equipped_slot.kind == WeaponSlotRef::kPersonal) {
        pump_personal_slot(world, owner, logic_tick);
        return;
    }
    if (owner.equipped_slot.kind != WeaponSlotRef::kMount) return;
    Entity *mount = world.registry.get(owner.equipped_slot.mount);
    if (mount == nullptr) return;
    // The seated borrower's mount->gunner link is the port's (a stale one drops),
    // and only it fires: no fire request reaches a slot held on foot (the
    // infantry request writes FIRE from a seat alone, and a remote player's
    // accepted fire re-points to its personal slot first, Server_ClientFiredRound
    // @0x50C1D4..0x50C28D).
    bool seated = owner.mounted && owner.mount_target == mount->handle &&
            mount->primary_weapon_owner == owner.handle;
    if (seated && !gunner_holds_mount(owner, *mount)) {
        mount->primary_weapon_owner = EntityHandle{};
        seated = false;
    }
    {
        AiEntity *gunner = for_handle(owner.handle);
        if (gunner == nullptr || mount->primary_weapon_slot_adm == 0xFF) return;
        const WeaponTableEntry *weapon =
                world.tables.weapons.by_index(mount->primary_weapon_slot_adm);
        if (weapon == nullptr || weapon->ammo_index < 0) return;

        // The reload addresses the gun from a UseGun seat, else the row itself.
        // [orig: WeaponAction_Reload @0x5430EA..0x543103]
        const Entity *addressed = owner.mount_type == SeatType::Gunner && owner.mounted
                ? world.registry.get(owner.mount_target) : nullptr;
        if (addressed == nullptr) addressed = &owner;
        WeaponFsmInputs inputs;
        inputs.is_local = owner.handle == world.cached.local_player;
        inputs.is_authority = is_authority;
        inputs.reload_refills = reload_echo_refills(world, *addressed);
        inputs.auto_reload = world.rules.auto_reload; // [orig: g_AutoReloadEnabled @ 0x24D2118]
        // The heat window is derived from the tick, so the pump needs it. AI gunners
        // sit on the emplaced guns that actually author heat, so this is the path
        // that overheats in practice. [orig: current_tick @ 0x24C1968]
        inputs.current_tick = static_cast<int32_t>(logic_tick);
        weapon_fire_environment_inputs(world, owner, inputs);
        WeaponFsmEvents weapon_events;
        weapon_fsm_tick(weapon->action_fsm, mount->primary_weapon_slot,
                        inputs, weapon_events);
        weapon_sound_publish(world, owner, weapon->action_fsm, weapon_events);
        if (weapon_events.reload_requested && is_authority)
            queue_walk_reload(world, *addressed, *weapon);
        if (!weapon_events.fired || !is_authority || !seated) return;

        // The slot owner is the gunner, while its def/ammo live on the parent.
        // A UseGun shot leaves through the gunner branch every mounted shooter
        // shares: the EMPLACEMENT's posed fire userpoint for the barrel the
        // clip selected before this shot spent its round (a gfx3 def's launch
        // point instead), along that bone's euler; byte 0 or no model copies
        // the parent's raw position/euler.
        // [orig: WeaponAction_Fire @0x542B10 (the Entity_CalcWeaponFirePosition
        //  call @0x542bf7) -> Entity_CalcWeaponFirePosition @0x4dc750 parentSlot 3
        //  (the Entity_ComputeUserpointWorldTransform call @0x4dc7f6; barrel =
        //  slot[+0x10] & 3 @0x545D40..0x545D4B, read before the Weapon_ConsumeAmmo
        //  call @0x542C75); the fire command copies out[0..2] and out[3]/out[4]
        //  @0x42be84..0x42bef2]. The point is the
        //  slot's FIRE field (b): the host's own re-derivation names field 0
        //  outright [orig: Server_ClientFiredRound @0x50c1f4]; the m/c fields
        //  anchor the effect legs, not the round.
        int32_t fire[6];
        // The ctrlx branch poses the EWeap carrier itself.
        // [orig: Entity_CalcWeaponFirePosition @0x4DC803..0x4DC846]
        if (owner.mount_type == SeatType::Controller)
            controller_fire_pose(world, *mount, weapon,
                    weapon_events.fired_clip_before_consume, fire);
        else
            usegun_fire_pose(world, *mount, weapon,
                    weapon_events.fired_clip_before_consume, fire);
		// The fire tail rocks the tank along the gun's point for the slot as
		// the FSM leaves it: next = RECOIL (the mflash column) and the clip
		// already spent. [orig: WeaponAction_Fire tail @0x542D1F..0x542D5B]
		world.vehicles.weapon_recoil(owner,
				weapon->action_fsm.actions[weapon_action::kFire].action_value, weapon,
				mount->primary_weapon_slot.clip, 1);
		// The slot's fire action leaves through the adm-indexed arm with its mode
		// byte from the barrel before the spend [orig: WeaponAction_Fire @0x542C11,
		//  @0x542C1C..0x542C2F].
		const int32_t fire_mode =
				((weapon_events.fired_clip_before_consume & 3) << 4) | 2;
		if (fire_ai_round(world, *gunner, fire, fire[3], fire[4], weapon->ammo_index,
				fire_mode, mount->primary_weapon_slot_adm))
			gunner->inf.aim_ref0 = gunner->inf.combat_target;
    }
}

// A seated person rides its parent's blink quad every seated tick: groundEntity
// takes parentEntity, then the parent's entity+0x1D0..0x1DC is copied, or the
// quad is zeroed once no parent is left (the authority's failed-seat detach).
// [orig: Entity_UpdateInfantryPlayerBody @0x4b662f..0x4b667e;
//  Entity_UpdateInfantryAI @0x4beee7..0x4bef30 — reached from every seated
//  path and after Entity_DetachFromVehicleIfServer @0x4b6627 / @0x4beedf]
static void ride_parent_blink_quad(const World &world, Entity &occ) {
    const Entity *parent = occ.mounted ? world.registry.get(occ.mount_target) : nullptr;
    for (int i = 0; i < 4; ++i) occ.blink_hits[i] = parent != nullptr ? parent->blink_hits[i] : 0u;
}

bool AiSystem::pose_if_mounted(AiEntity &e, World &world) {
    Entity *occ = world.registry.get(e.handle);
    // The seat site keys on the parent alone. Org1's mounted-live local adds a
    // positive Health word, which its caller tests; the player body's selector reads
    // the parent and its def with no Health test, so a client's dead body (its death
    // edge detaches only on the authority) keeps the seat until the record's detach.
    // [orig: Entity_UpdateInfantryAI @0x4B9960..0x4B9985; Entity_UpdateInfantryPlayerBody
    //  @0x4B637C..0x4B6392]
    if (occ == nullptr || !occ->mounted) return false;
    Entity *veh = world.registry.get(occ->mount_target);
    if (veh == nullptr) {              // vehicle gone -> auto-dismount, resume normal AI
        world.vehicles.detach(e.handle);
        return false;
    }
    Seat seat;
    if (!world.vehicles.rider_seat(*occ, *veh, seat)) return false;
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
    // An NPC's ordinary seat resolves through the carrier's seat bone. When that
    // lookup fails, the authority kills a rider boarding-ordered at the carrier
    // or at the carrier's ground link (crediting the carrier's last attacker) and
    // detaches every such rider; a client keeps it seated, unposed.
    // [orig: Entity_UpdateInfantryAI Entity_GetBoneTransformAndOrientation call
    //  @0x4BED72, test @0x4BED7A..0x4BED7C; the fail arm @0x4BEE93..0x4BEED8;
    //  Entity_DetachFromVehicleIfServer @0x4BEEDF; +0x28 = +0x16C @0x4BEEE7..0x4BEEEF]
    if (npc_mounted_body(e, *occ) && veh->has_item_def && seat.type != SeatType::Gunner &&
            world.pose_provider != nullptr &&
            !world.pose_provider->resolve_seat_bone(world, *veh, seat.bone_index)) {
        if (is_authority) {
            const int32_t order = e.slot.f[37];
            const int32_t target = e.slot.f[38];
            const Entity *ground = world.registry.get(veh->ground_target);
            if ((order == kCommandAttachPassengerOnly || order == kCommandAttachSkipController ||
                        order == kCommandAttachAnySeat) &&
                    (target == veh->net_id || (ground != nullptr && target == ground->net_id))) {
                occ->health = e.health = 0;
                occ->last_attacker = veh->last_attacker;
            }
            world.vehicles.detach(e.handle);
            occ->ground_target = {};
        }
        ride_parent_blink_quad(world, *occ);
        // The seat block's tail still runs for the rider it left unposed: the
        // legs and their targets snap to the body heading and the look takes the
        // mounted chase. A client keeps the rider, so the +-90 degree look clamp
        // about the body follows here unless the parent's config widens it; a
        // detached rider takes that clamp in the motor.
        // [orig: legs @0x4BEF36..0x4BEF51; look @0x4BEF57..0x4BEF97; clamp
        //  @0x4BEF9A..0x4BEFED]
        e.inf.leg_yaw[0] = e.inf.leg_yaw[1] = e.inf.body_heading;
        e.inf.leg_target[0] = e.inf.leg_target[1] = e.inf.body_heading;
        infantry_look_tick(e.inf, e.heading, e.pitch, true);
        if (!occ->mounted) return false;
        const int cfg = veh->emplaced_config;
        if (!(veh->emplaced_config_valid && (cfg == 3 || cfg == 4 || cfg == 5 || cfg == 7))) {
            const int32_t twist = io::bam_sub(e.heading, e.inf.body_heading);
            if (twist > 0x40000000) e.heading = io::bam_add(e.inf.body_heading, 0x40000000);
            else if (twist < -0x40000000) e.heading = io::bam_sub(e.inf.body_heading, 0x40000000);
        }
        return true;
    }
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
    // A player body's ordinary seat resolves through the carrier's seat bone too. When that
    // lookup fails the body takes no pose and no seat animation; the authority detaches it
    // (the player body's arm kills no one), a client keeps it seated. The seat block's tail
    // still runs (groundEntity from parentEntity, the blink quad, the legs and their targets
    // to the body heading) and the pass ends at the mounted site.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the UseGun test @0x4B63A5..0x4B63BF,
    //  the bone call @0x4B6510, its test @0x4B6518..0x4B651A, Entity_DetachFromVehicleIfServer
    //  @0x4359D0 called @0x4B6627, the tail @0x4B662F..0x4B66A3, the site's returns @0x4B6907
    //  / @0x4B6977]
    const bool player_seat_bone_lost = e.inf.active && !npc_mounted_body(e, *occ) &&
            veh->has_item_def && seat.type != SeatType::Gunner && world.pose_provider != nullptr &&
            !world.pose_provider->resolve_seat_bone(world, *veh, seat.bone_index);
    int32_t seat_heading = e.inf.body_heading;
    if (player_seat_bone_lost) {
        if (is_authority) {
            world.vehicles.detach(e.handle);
            occ->ground_target = {};
        }
        e.inf.leg_yaw[0] = e.inf.leg_yaw[1] = e.inf.body_heading;
        e.inf.leg_target[0] = e.inf.leg_target[1] = e.inf.body_heading;
    } else {
        seat_heading = apply_resolved_mounted_seat_frame(e, world, *occ, *veh, seat);
    }
    ride_parent_blink_quad(world, *occ);
    if (e.inf.active && e.inf.is_local_player) {
        // The mounted LOCAL player keeps the LOOK as its entity yaw: the witnessed mounted
        // carry writes bodyHeading/headLook from the seat bone but leaves entity->Yaw
        // player-owned — the drive motor reads it as the mouse-steer target
        // [orig: Entity_UpdateInfantryPlayerBody mounted leg (bodyHeading only) +
        //  Entity_UpdateVehiclePhysics steer source @0x48ba59/v61->Yaw].
        occ->heading = e.inf.target_heading;
        // So is its Pitch word the look, not the seat's (D-NET-NEW-L6-3)
        // [orig: Input_HandleActionBinding_0 @0x4E0420
        //  (the Pitch stores @0x4E0D39..0x4E0D5C, @0x4E0F50, @0x4E111C) writes +0x14].
        occ->pitch_bam = e.inf.look_pitch;
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
    // Occupants present their carried body in the captured seat frame (the body
    // heading and the legs). An organic rider's Yaw word stays its look: the seat
    // carry writes no +0x10, so a remote passenger keeps the yaw its chase set; the
    // arms below restore or chase the look for the other riders. A non-organic
    // brain takes the seat yaw: retail runs no seat mover for one (see
    // refresh_mounted_pose's head).
    // [orig: Entity_UpdateInfantryPlayerBody - the seat carry @0x4B6510..0x4B6575,
    //  the remote chase @0x4B4361; Entity_UpdateInfantryAI @0x4BED72..0x4BEDDA]
    if (!e.inf.active) e.heading = seat_heading;
    if (e.inf.active && e.inf.is_local_player) {
        // The seated LOOK stays mouse-instant at FULL precision: retail drives entity
        // Yaw/Pitch straight from input regardless of mount (the mounted body leg
        // writes bodyHeading/headLook from the bone, never the look) — the camera
        // reads these, and the occ->heading store above (the wire/motor Yaw
        // word) must not freeze it. tick_infantry's own
        // mirrors (its lines `e.heading = inf.target_heading` / `e.pitch =
        // inf.look_pitch`) are skipped for the whole ride.
        // [orig: Input_HandleActionBinding_0 @0x4E0420
        //  (the Pitch stores @0x4E0D39..0x4E0D5C, @0x4E0F50, @0x4E111C) writes entity+0x10/+0x14;
        //  §23.5 — the entity Yaw is the LOOK, player-owned while seated]
        e.heading = e.inf.target_heading;
        e.pitch = e.inf.look_pitch;
    } else if (remote_player_controls_vehicle(e, *occ, seat)) {
        // The host read-applies this player's LOOK from C2S before the body tick.
        // Restore it after seat carry so the later authority vehicle pass consumes
        // the owner's mouse-steer target, not the carrier yaw.
        e.heading = saved_look_heading;
        e.pitch = saved_look_pitch;
        occ->heading = saved_look_heading;
        occ->pitch_bam = saved_look_pitch; // the look's Pitch word (D-NET-NEW-L6-3)
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
        // The look chase writes the Yaw and Pitch words; the UseGun attach
        // restores both (D-NET-NEW-L6-3). [orig: Entity_UpdateInfantryAI
        //  @0x4BEF84 / @0x4BEF97; Entity_AttachToBoneAndUpdateTransform
        //  @0x546661 / @0x546664]
        occ->heading = e.heading;
        occ->pitch_bam = e.pitch;
    }
    if (e.inf.active) {
        // The seat animation (+0x2BC, the deferred +0x2B8 cleared) is the posed arm's.
        // [orig: Entity_UpdateInfantryPlayerBody @0x4B65A8..0x4B65C3]
        if (!player_seat_bone_lost) {
            const int mounted_state = mounted_anim_state_for_seat(*veh, seat, e.inf, root_motion);
            if (e.inf.anim_state != mounted_state) {
                e.inf.request_body_animation(mounted_state);
            }
            e.inf.anim_pending = 0;
        }
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
    int32_t rate = io::retail_ftol_sse2(rate_f); // [orig: _ftol2_sse @0x76BC00]
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
            // (ported as AiSystem::update_movement_target).
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
    spawn_baseline_rings_ = anim_rings;
    baseline_captured_ = true;
}

// Per-mission re-init seam: World::restore() calls load_systems() on an editor Play->Stop,
// which reaches this. Rewind every brain (position/heading/state/timers) to the captured
// spawn baseline and drop the transient queues, so a simulate/stop cycle leaves the authored
// mission clean. The nav table is read-only path data and is left intact.
void AiSystem::on_load(World &) {
    if (baseline_captured_) {
        entities_ = spawn_baseline_;
        anim_rings = spawn_baseline_rings_;
    }
    // The restore replaced the registry's rows; each slot rebinds to its row,
    // whose words the registry baseline restored.
    for (AiEntity &e : entities_) bind_self(e);
    rebuild_handle_index();
    events.clear();
    relmat_calls.clear();
    rel_ops.clear();
    target_set_calls.clear();
    scan_candidates_.clear();
    find_target_calls = 0;
}

// [orig: Entity_CalcAverageGroundHeight @0x457230] 5-tap weighted ground height (16.16 fixed).
// The taps sample the bilinear terrain column at center + N/S/E/W at sample_radius; the original
// per-tap sampler is the hi-res down-raycast @0x60e710 (its near-vertical result == this column
// height — tracked deviation). The taps sample the mission plane (X,Y) = (pos[0],pos[1]). Height
// comes back in world units; ground_16.16 = units * 65536.
int32_t calc_average_ground_height(const terrain::TerrainHeightField &field, const int32_t pos[3],
                                   int32_t sample_radius, const GroundClearance &clearance) {
    if (!field.valid()) return INT32_MIN; // [orig: terrain assumed present; null -> no ground]

    auto sample = [&](int32_t x16, int32_t y16) -> int32_t {
        return terrain::height_field_height_bilinear_fixed(field, x16, y16); // the words (D-NET-507)
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

    // The water-surface clamp, on the raw word (0 on a map without water: no dry
    // sentinel). [orig: Entity_CalcAverageGroundHeight `if (*(entity+368) &&
    //  (int)g_EnvWaterHeightFixed > result) result = g_EnvWaterHeightFixed`
    //  @0x45732F..0x457331; the word's default Environment_InitDefaults
    //  @0x57C011..0x57C01E]
    if (clearance.has_occupant && field.water_y > result) {
        result = field.water_y;
    }
    // [orig: the caller's brain+0x30 when dead (@0x45734D), else brain+0x2C (@0x457367),
    //  both skipped for a brainless caller (@0x457333).]
    result += clearance.use_dead ? clearance.dead_offset : clearance.alive_offset;
    return result;
}

// See the header.
int32_t brain_ground_offset(const World &world, const Entity &entity) {
    const AiEntity *ai = world.ai.for_handle(entity.handle);
    if (ai == nullptr || ai->inf.active) return 0; // no vehicle brain at +0x64
    const bool dead = ((entity.flags | entity.engine_flags) & kEntityFlagDead) != 0 ||
                      entity.health <= 0;
    if (dead) {
        const ItemDeathTraits *traits = world.tables.item_death_traits.get(entity.item_id);
        if (traits != nullptr && traits->primary_husk_loaded)
            return ai->brain.f[AiBrain::kHuskFloor];
    }
    return ai->brain.f[AiBrain::kModelFloor];
}

// ----------------------------------------------------------------------------

} // namespace opennova::world
