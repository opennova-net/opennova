// The AI-change command family, Entity_ApplyCommand over the rows a single,
// group or area target names (split from entity_commands.cpp by responsibility).

#include <runtime/world/entity_commands.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cstdint>

#include <formats/mission/mission_params.h>
#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command, entity_dcb_id

namespace opennova::world {

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
            // The helper writes brain+148, so the arm needs the vehicle brain
            // (an organic has none). Its live pool-0 match returns before
            // storing; all other paths clear the old priority pointer.
            // [orig: Entity_ApplyCommand @0x43AB60 case 44, the +0x64 gate
            //  @0x43B136..0x43B13B -> Entity_FindByNetId @0x4655B0]
            if (ae.inf.active) break;
            if (p2 != 0) {
                bool preserve = false;
                for (size_t slot = 0; slot < world.registry.pool_capacity(0); ++slot) {
                    const Entity *target = world.registry.get(EntityHandle::make(0, static_cast<int>(slot)));
                    // The DcbId compare [orig: Entity_FindByNetId @0x4655B0 - @0x4655E0].
                    if (target == nullptr || entity_dcb_id(world, *target) != p2) continue;
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
        // The slot arms with no editor token: each gates on the AI slot (+0x68),
        // which every AiEntity carries, and writes one slot word or behavior bit.
        // The arithmetic is the signed wrapping int32 of the original.
        case 1: // slot+0x20 = p2 [orig: case 1 @0x43AB7F, store @0x43AB92]
            ae.slot.f[8] = p2;
            break;
        case 3: // [orig: case 3 @0x43ABC2 — bit 0x2000 @0x43ABD7/@0x43ABE4]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x2000u, p2 != 0);
            break;
        case 4: // [orig: case 4 @0x43ABF1 — bit 0x10000 @0x43AC06/@0x43AC13]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x10000u, p2 != 0);
            break;
        case 7: // slot+0x84 = (p2 << 16) / 360 [orig: case 7 @0x43AD5C, store @0x43AD85]
            ae.slot.f[33] = static_cast<int32_t>(static_cast<uint32_t>(p2) << 16) / 360;
            break;
        case 9: // slot+0x30 = (p2 << 16) / 100 [orig: case 9 @0x43ADCE, store @0x43ADF5]
            ae.slot.f[12] = static_cast<int32_t>(static_cast<uint32_t>(p2) << 16) / 100;
            break;
        case 10: // slot+0x48 = p2 * 62 [orig: case 10 @0x43ADFD, store @0x43AE19]
            ae.slot.f[18] = static_cast<int32_t>(static_cast<uint32_t>(p2) * 62u);
            break;
        case 11: // slot+0x4C = p2 * 62 [orig: case 11 @0x43AE21, store @0x43AE3D]
            ae.slot.f[19] = static_cast<int32_t>(static_cast<uint32_t>(p2) * 62u);
            break;
        case 12: // slot+0x54 = p2 * 62 [orig: case 12 @0x43AE45, store @0x43AE61]
            ae.slot.f[21] = static_cast<int32_t>(static_cast<uint32_t>(p2) * 62u);
            break;
        case 13: // slot+0x38 = (p2 << 8) / 360 [orig: case 13 @0x43AE69, store @0x43AE92]
            ae.slot.f[14] = static_cast<int32_t>(static_cast<uint32_t>(p2) << 8) / 360;
            break;
        case 14: // [orig: case 14 @0x43AE9A — bit 0x100 @0x43AEAF/@0x43AEBC]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x100u, p2 != 0);
            break;
        case 18: // [orig: case 18 @0x43AF50 — bit 0x800 @0x43AF65/@0x43AF72]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x800u, p2 != 0);
            break;
        case 19: // [orig: case 19 @0x43AF7F — bit 0x200000 @0x43AF94/@0x43AFA1]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x200000u, p2 != 0);
            break;
        case 20: // [orig: case 20 @0x43B03A — bit 0x8000 @0x43B04F/@0x43B05C]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x8000u, p2 != 0);
            break;
        case 24: // [orig: case 24 @0x43B00B — bit 0x80000 @0x43B020/@0x43B02D]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x80000u, p2 != 0);
            break;
        case 25: // [orig: case 25 @0x43AFDC — bit 0x100000 @0x43AFF1/@0x43AFFE]
            set_slot_mask(ae.slot.f[AiSlot::kBehaviorFlags], 0x100000u, p2 != 0);
            break;
        default: break; // cases 35..39 are the switch's default [orig: @0x43B336]
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
            // Both writes replace the current callback, including a death callback
            // and a drop's fall or ride: Entity_UpdateOrg0Movement, or the def's own.
            // [orig: Entity_ApplyCommand @0x43B22A..0x43B24E]
            entity.motor_suspended = p2 != 0;
            entity.death_motion = DeathMotionMode::None;
            entity.drop_motion = p2 != 0 ? DropMotion::None : def_update_drop_motion(entity);
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
    // Channel 0 for the command-queued events (the air machine's kill/damage
    // notification and both machines' death queue sites stamp 9; this site
    // stamps 0) [orig: event_source = 0 @0x43ac66/@0x43acd1/@0x43ad41/@0x43b30e].
    ev.f[1] = sys.index_of(ae) << 16;
    ev.set_timer(0.0f);
    ev.f[3] = argument;
    sys.events.queue(ev);
}

// The brain half of Entity_ApplyCommand: every queued-event arm and the direct
// brain arms (AIUSEWPZ/AICLEARWPZ/PLAYPARTANIM) first require the vehicle brain
// at entity+0x64. An organic carries only the AI slot, so its slot arms apply
// and these do not (TARGETSSN's arm carries the same gate). [orig:
//  Entity_ApplyCommand, the `mov eax,[reg+64h]` gate per arm: the alerts
//  @0x43AC34 (red, after the slot byte), @0x43AC94 (yellow), @0x43AD04 (green);
//  DRIVESKILL @0x43B0A0, AIMSKILL @0x43B0BE, AISETSTATE @0x43B0DC, COMBATSPEED
//  @0x43B0FA, PATROLSPEED @0x43B118, START_FIRING @0x43B2C4, FIRING_ANGLE
//  @0x43B2DB; AIUSEWPZ @0x43B158, AICLEARWPZ @0x43B177, PLAYPARTANIM @0x43B1C1]
void apply_ai_brain_arms(World &world, AiEntity &ae, int sub_type, int32_t p2, int32_t p3,
                         int32_t p4) {
    if (ae.inf.active) return;
    queue_ai_brain_event(world.ai, ae, sub_type, p2);
    ai_apply_command(ae.brain, sub_type, p2, p3, p4);
}

// One Entity_ApplyCommand over a resolved row: the entity arms first (no
// aiRuntime gate), then the controller, queued-event and brain halves when
// the row has an AI component. True when some arm took the command.
// [orig: Entity_ApplyCommand @0x43ab60]
bool apply_row_ai_command(World &world, EntityHandle handle, int sub_type, int32_t p2,
                          int32_t p3, int32_t p4) {
    Entity *entity = world.registry.get(handle);
    if (entity == nullptr) return false;
    if (apply_entity_ai_command(world, *entity, sub_type, p2)) return true;
    AiEntity *ae = world.ai.for_handle(handle);
    if (ae == nullptr) return false;
    apply_ai_controller_command(world, *entity, *ae, sub_type, p2, p3);
    apply_ai_brain_arms(world, *ae, sub_type, p2, p3, p4);
    return true;
}

} // namespace

bool EntityCommands::apply_ai_command_at(EntityHandle handle, int sub_type, int32_t p2,
                                         int32_t p3, int32_t p4) {
    return apply_row_ai_command(world_, handle, sub_type, p2, p3, p4);
}

bool EntityCommands::apply_ai_command(EntityTarget ssn, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    return apply_row_ai_command(world_, resolve_target(ssn), sub_type, p2, p3, p4);
}

int EntityCommands::apply_group_ai_command(int group, int sub_type, int32_t p2, int32_t p3, int32_t p4) {
    // Sub 0 and group 0 do nothing. Pool 2 rows take the command only with
    // an AI component; pool 0 and pool 1 rows take it unconditionally, so
    // the entity arms reach a brainless item but never a pool-3 marker.
    // [orig: Entity_HandleAlertCommand @0x43cf10 — sub 0 @0x43cf1e, group 0
    //  @0x43cf2c; pool 2 with the aiRuntime gate @0x43cf57/@0x43cf64, pool 0
    //  @0x43cf97, pool 1 @0x43cfcd]
    if (sub_type == 0 || mission::group_names_none(group)) return 0;
    int n = 0;
    for (int pool : {2, 0, 1}) {
        const size_t capacity = world_.registry.pool_capacity(pool);
        for (size_t slot = 0; slot < capacity; ++slot) {
            const EntityHandle handle = EntityHandle::make(pool, static_cast<int>(slot));
            const Entity *entity = world_.registry.get(handle);
            if (entity == nullptr || static_cast<int>(entity->group_id) != group) continue;
            if (pool == 2 && world_.ai.for_handle(handle) == nullptr) continue;
            if (apply_row_ai_command(world_, handle, sub_type, p2, p3, p4)) ++n;
        }
    }
    // After the fan, the alert-change subs stamp the per-group alert record
    // the cat-1 triggers read [orig: @0x43cfea..0x43d01a — sub 5 -> red, 6 ->
    // green, 22 -> yellow via TriggerGroup_SetAlertRed/Green/Yellow
    // @ 0x40d630/0x40d5f0/0x40d610].
    if (group > 0 && group < TriggerRelations::kGroups) {
        if (sub_type == 5) world_.script.relations.group(group).alert = TriggerRelations::kAlertRed;
        else if (sub_type == 6) world_.script.relations.group(group).alert = TriggerRelations::kAlertGreen;
        else if (sub_type == 22) world_.script.relations.group(group).alert = TriggerRelations::kAlertYellow;
    }
    return n;
}

int EntityCommands::apply_area_ai_command(int team, int sub_type, int32_t p1, int32_t p2,
                                          int32_t p3, int32_t p4, int32_t reserved1) {
    // Sub 0 does nothing. Pool 0 rows whose signed team byte matches take
    // the command when X lies in [p1, p3] and Y in [p4, reserved1], no Z
    // test. The zone resolver stored x_min, y_min, x_max, y_max in those
    // words, so the tested box pairs the zone's corners across axes, and
    // Entity_ApplyCommand reads y_min and x_max as its p3/p4.
    // [orig: Entity_KillTeamInBounds @0x43d030 — sub 0 @0x43d05b, the team
    //  byte @0x43d099, X @0x43d0a8..0x43d0b5, Y @0x43d0b7..0x43d0c4,
    //  Entity_ApplyCommand @0x43d0c8]
    if (sub_type == 0) return 0;
    int n = 0;
    const size_t capacity = world_.registry.pool_capacity(0);
    for (size_t slot = 0; slot < capacity; ++slot) {
        const EntityHandle handle = EntityHandle::make(0, static_cast<int>(slot));
        const Entity *entity = world_.registry.get(handle);
        if (entity == nullptr || static_cast<int8_t>(entity->team) != team) continue;
        const int32_t x = entity->position.x.q16;
        const int32_t y = entity->position.y.q16;
        if (x < p1 || x > p3 || y < p4 || y > reserved1) continue;
        if (apply_row_ai_command(world_, handle, sub_type, p2, p3, p4)) ++n;
    }
    return n;
}

} // namespace opennova::world
