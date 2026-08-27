// The reserved-command legs of the infantry navigation think — slot+148 values
// 123..127 are ORDERS, not path ids: 123/124/125 "Goto SSN and board" (seat
// filter per command), 126 goto-group hold, 127 follow the local player. Split
// from infantry.cpp by leg per the size ratchet (precedent: infantry_ladder.cpp).
//
// [orig: Entity_UpdateInfantryAI @0x4b9910 — the command dispatch on aiComp+148
//  inside the 16-tick think; board target resolve = pool 0..3 scans by entity
//  net id (+124) against aiComp+152, cached in aiComp+144; walk to the seat
//  approach point; arrival -> Entity_FindBestSeatSlot @0x4351f0 ->
//  Entity_RequestVehicleAttach @0x4364a0.]
//
// This port retires the D-INF-2 early-return. Deliberate residuals, ledgered on
// D-INF-2: the E1..E8 entry-point claim/stagger (boneWalkSlot), the 64-tick
// mounted seat re-upgrade, the 11000/12000/12001 scripted escort offsets, the
// can't-enter fallbacks (incl. the far-from-spawn self-kill @0x4b9910), and
// Entity_CanEnterVehicle's INTERNAL gates (@0x435480 — the Flags&2/itemDef/model
// preamble, the groundEntity path, Entity_IsBoneInProximity, and the Flags&0x2000
// + 16-unit savedLivePose arm) — the modeled equivalents are cited inline where
// each stands in. NO LONGER A RESIDUAL: the itemDef attrib 0x40 (PlayerControl)
// gate that guards the Entity_CanEnterVehicle consult is ported at the ARRIVED
// branch below.

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace opennova::world {

namespace {

constexpr double kBamPerRadianBoard = 2147483648.0 / 3.14159265358979323846;

// atan2 -> engine BAM (same formula as infantry.cpp's file-local bearing_to).
int32_t board_bearing_to(int32_t dx, int32_t dy) {
    return static_cast<int32_t>(std::atan2(static_cast<double>(dy),
                                           static_cast<double>(dx)) *
                                kBamPerRadianBoard);
}

int32_t board_to_fixed(float v) { return static_cast<int32_t>(v * 65536.0f); }

// 3D separation with the witnessed 1.0u vertical slack — every command leg
// measures distance this way. [orig: dz = max(0, |dz| - 0x10000) before the
// fsqrt, @0x4b9910 the 127 leg and the board leg alike]
int32_t board_dist(const int32_t pos[3], const int32_t tgt[3]) {
    const double dx = static_cast<double>(tgt[0]) - pos[0];
    const double dy = static_cast<double>(tgt[1]) - pos[1];
    int32_t dzi = tgt[2] - pos[2];
    dzi = (dzi < 0 ? -dzi : dzi) - 0x10000;
    if (dzi < 0) dzi = 0;
    const double dz = static_cast<double>(dzi);
    return static_cast<int32_t>(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// The chosen seat's world position through the carrier's FULL orientation
// frame — the same provider-less frame pose_mounted_occupant uses. This is the
// modeled seat APPROACH point.
// [orig: the seat-point builder Entity_GetBoneWorldPosition_0 @0x434df0 feeding rayEnd, frame per
//  Entity_GetBoneTransformAndOrientation @0x4b0c50]
void seat_world_position(const Entity &vehicle, const Seat &seat, int32_t out[3]) {
    const Vec3 p = entity_local_point_world(vehicle, seat.seat_local);
    out[0] = board_to_fixed(p.x);
    out[1] = board_to_fixed(p.y);
    out[2] = board_to_fixed(p.z);
}

// [orig: Entity_FindBestSeatSlot @0x4351f0 — the aiComp+148 admit term:
//  (cmd != 124 || type != ctrl) && (cmd != 123 || type == sitex)]
SeatSelectionMode seat_mode_for_command(int32_t command) {
    switch (command) {
        case 123: return SeatSelectionMode::PassengerOnly;
        case 124: return SeatSelectionMode::RejectController;
        default: return SeatSelectionMode::Any;
    }
}

} // namespace

void AiSystem::infantry_command_think(AiEntity &e, World &world) {
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;
    const int32_t command = slot.f[37];

    if (command == 126) {
        // GOTO GROUP -> a stationary guard. Retail parks the shared gait select
        // at moveMode 3 with targetDist == arrivalRadius == 10.0u, which resolves
        // to no motion; our think contract encodes that outcome as the move_mode 0
        // the caller's per-think reset already left in place.
        // [orig: the ==126 leg @0x4baabd..0x4baacf — moveMode=3, dist=radius=0xA0000]
        return;
    }

    if (command == 127) {
        // FOLLOW THE LOCAL PLAYER. Arrive at 4.0u; when the AI's combat focus IS
        // the player (a guard order), the ring widens to max(slot[16], 4.0u);
        // inside the ring the move clears. [orig: the ==127 leg @0x4baad4.. —
        // target = g_local_player position @0x4baae5, radius 0x40000
        // @0x4bab60, focus compare aiComp[3] == g_local_player, radius
        // max(aiComp[16], 0x40000) @0x4bab77..0x4bab83]
        const Entity *player = world.registry.get(world.cached.local_player);
        if (player == nullptr) return;
        int32_t tgt[3] = {board_to_fixed(player->position.x),
                          board_to_fixed(player->position.y),
                          board_to_fixed(player->position.z)};
        const int32_t dist = board_dist(e.pos, tgt);
        int32_t radius = 0x40000;
        if (inf.combat_target.valid() &&
            inf.combat_target == world.cached.local_player)
            radius = std::max(slot.f[16], radius);
        if (dist < radius) return;
        inf.move_mode = 3;
        inf.target_dist = dist;
        inf.arrival_radius = radius;
        inf.move_target[0] = tgt[0];
        inf.move_target[1] = tgt[1];
        inf.move_target[2] = tgt[2];
        inf.target_heading = board_bearing_to(tgt[0] - e.pos[0], tgt[1] - e.pos[1]);
        return;
    }

    infantry_board_think(e, world, command);
}

void AiSystem::infantry_board_think(AiEntity &e, World &world, int32_t command) {
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;

    // Resolve the board target by SSN — slot+152 carries the authored wp_number.
    // Retail caches the raw entity pointer in aiComp+144 and rescans pools 0..3
    // by net id (+124) when the cache goes stale; our registry find performs the
    // same scan and the cache keeps the handle (+1 so 0 stays "none").
    // [orig: the board-leg pool scans @0x4bee93..0x4beec6 (authority gate
    //  @0x4bee93, cmd 123/124/125 @0x4beea5..0x4beeaf, aiComp[38] vs entity+0x7C
    //  @0x4beeba); stale test aiComp[36]->+124 != aiComp[38]]
    const uint16_t target_ssn = static_cast<uint16_t>(slot.f[38] & 0xFFFF);
    const EntityHandle th = world.registry.find_by_net_id(target_ssn);
    Entity *target = world.registry.get(th);
    if (target == nullptr) {
        // Unresolved target: stand and retry next think. (The witnessed
        // dead-target fallbacks — hold at spawn / the >8u-from-spawn self-kill —
        // are D-INF-2 residuals.) [orig: @0x4b9910 the CanEnterVehicle==0 legs]
        slot.f[36] = 0;
        inf.board_blocked = false;
        inf.board_progress_valid = false;
        return;
    }
    slot.f[36] = static_cast<int32_t>(th.packed) + 1;

    const Entity *self = world.registry.get(e.handle);
    if (self == nullptr) return;
    if (self->mounted) {
        // Seated. The 64-tick better-seat re-upgrade stays a D-INF-2 residual.
        // [orig: the (tickKey & 0x3F) == 0 upgrade block @0x4ba9d8..0x4baa41 —
        //  mounted (+0x16C) @0x4ba9e1, Entity_FindBestSeatSlot @0x4ba9fb,
        //  Entity_GetBoneSlotType != +0x168 @0x4baa1d, Entity_RequestVehicleAttach
        //  @0x4baa2c when the best slot differs]
        return;
    }

    // Walk goal: the filtered best seat's approach point when one resolves (the
    // modeled at Entity_GetBoneWorldPosition_0 @0x434df0), else the target origin. Arrive at 2.0u for a seat
    // point [orig: 0x20000 @0x4bb32e]; the no-seat fallback stands off at 4.0u —
    // a stand-in for retail's bound-radius + 1.0u (entity+0 is unmodeled here).
    // [orig: the ring pick @0x4bb325..0x4bb34a: +0x369 clear -> 0x20000, set ->
    //  target->+0 (bound radius) + 0x10000]
    int32_t goal[3];
    int32_t radius = 0x20000;
    const int seat_idx =
            world.commands.find_best_seat(*target, e.handle,
                                          seat_mode_for_command(command));
    if (seat_idx >= 0 &&
        seat_idx < static_cast<int>(target->seats.size())) {
        seat_world_position(*target, target->seats[seat_idx], goal);
    } else {
        goal[0] = board_to_fixed(target->position.x);
        goal[1] = board_to_fixed(target->position.y);
        goal[2] = board_to_fixed(target->position.z);
        radius = 0x40000;
    }

    // The blocked latch: retail arms pad_368[1] from the collision
    // push-response (the hull pressing back against the walker) and, while
    // latched, widens the arrival ring from the 2.0u seat ring to the target's
    // bound radius + 1.0u — pressed against the fuselage counts as arrived, so
    // interior seat points (helo cabins, boat wells) stay boardable. Our motor
    // has no push signal; the latch arms when an ORDERED board walk makes under
    // half a walk-step of progress across a think, and the widened ring is 4.0u
    // (the bound radius is unmodeled — a D-INF-2 stand-in).
    // [orig: pad_368[1] set @0x4b9910 push block (displacement >= 768);
    //  ring pick `pad_368[1] ? *target + 0x10000 : 0x20000` in the board leg]
    if (inf.board_progress_valid) { // a walk was ordered last think
        const int32_t pdx = e.pos[0] - inf.board_progress_pos[0];
        const int32_t pdy = e.pos[1] - inf.board_progress_pos[1];
        const int64_t moved2 = static_cast<int64_t>(pdx) * pdx +
                               static_cast<int64_t>(pdy) * pdy;
        constexpr int64_t kStallStep = 0x8000; // half a 16-tick walk stride
        if (moved2 < kStallStep * kStallStep) inf.board_blocked = true;
    }
    inf.board_progress_valid = false;
    if (inf.board_blocked) radius = std::max(radius, 0x40000);

    const int32_t dist = board_dist(e.pos, goal);
    if (dist > radius) {
        inf.move_mode = 3;
        inf.target_dist = dist;
        inf.arrival_radius = radius;
        inf.move_target[0] = goal[0];
        inf.move_target[1] = goal[1];
        inf.move_target[2] = goal[2];
        // The board walk is a final approach — retail raises the one-shot flag
        // so the shared gait select takes the slow-in ramp.
        // [orig: waypointLooping = 1 on the board walk @0x4b9910 (the common
        //  move tail @0x4bbe11 every command leg jumps to)]
        inf.at_final_oneshot = true;
        inf.target_heading = board_bearing_to(goal[0] - e.pos[0], goal[1] - e.pos[1]);
        inf.board_progress_pos[0] = e.pos[0];
        inf.board_progress_pos[1] = e.pos[1];
        inf.board_progress_valid = true;
        return;
    }
    inf.board_blocked = false;

    // ARRIVED -> board. A full or filtered-out vehicle leaves the soldier
    // standing at the goal (no seat -> no attach). The modeled admit gate is
    // "the target owns seats"; the per-command seat filter reruns inside
    // mount_boarding_command. [orig: the arrived gate @0x4bbda6..0x4bbe07 —
    //  !parentEntity (+0x16C) @0x4bbda6, radius < 0x640000 @0x4bbdaf, itemDef
    //  attrib & 0x60 @0x4bbdc4 -> Entity_FindBestSeatSlot @0x4351f0 (call
    //  @0x4bbdd4) -> Entity_RequestVehicleAttach @0x4364a0 (call @0x4bbdf2)]
    // THE PLAYERCONTROL ADMIT GATE. Retail only reaches its board/attach path
    // when the TARGET's itemDef carries attrib bit 0x40 (items.def PlayerControl):
    //
    //     type = v158->itemDef->type;
    //     if (type != ItemType_Vehicle && type != ItemType_Powerup) goto the move tail;
    //     if ((v158->itemDef->attrib & 0x40) != 0) {
    //         CanEnterVehicle = Entity_CanEnterVehicle(entity, v158);
    //         ...
    //     }
    //     the move tail @0x4bbe11: walk toward the target (moveMode 3)
    //
    // so a target WITHOUT the bit is still walked to and simply never boarded.
    // [orig: Entity_UpdateInfantryAI @0x4b9910 — the attrib test guarding the
    //  Entity_CanEnterVehicle @0x435480 consult, and the fall-through to the
    //  move tail @0x4bbe11.]
    //
    // The type test is NOT reproduced because it cannot discriminate here: the
    // engine stores powerup and object as the SAME value 6
    // [orig: ItemDef_ParseProperty @0x49eb00, mirrored in def.h DefItemType], so
    // every object-type target passes it. The attrib bit is the operative gate.
    //
    // This replaces the previous stand-in admit gate ("the target owns seats"),
    // which boarded anything with a seat. 00TRg orders three soldiers onto 1902
    // "50cal on 180 tripod" emplacements (attrib EWeap 0x20, no PlayerControl)
    // standing at their own spawns; without this gate they mount an emplacement
    // retail never lets them mount, and are pinned there for the whole mission.
    //
    // STILL A RESIDUAL: Entity_CanEnterVehicle's own internal gates (the Flags&2 /
    // itemDef / model preamble, the groundEntity path, Entity_IsBoneInProximity,
    // and the Flags&0x2000 + 16-unit savedLivePose arm) remain unported — only
    // the attrib gate that guards the CALL is ported here.
    // RETRACTED 2026-08-22: this branch previously required
    // `target->item_attrib & kItemAttribPlayerControl` before mounting. The
    // attrib 0x40 test IS witnessed, but it guards the Entity_CanEnterVehicle
    // CONSULT, not the attach itself:
    //
    //     if ((v158->itemDef->attrib & 0x40) != 0) { CanEnterVehicle = ...; }
    //     the move tail @0x4bbe11: walk toward the target
    //
    // Gating the MOUNT on it was an over-application of the witness, and the wire
    // refutes it: retail emplaces SEVEN AI at ~100% of their rows, and three of
    // them are handles 42/43/51 -- exactly the soldiers 00TRg orders onto the
    // 1902 tripods (attrib EWeap, no PlayerControl). Retail mans those tripods;
    // the gate stopped us doing so. [orig: Entity_UpdateInfantryAI @0x4b9910.]
    //
    // The attach path retail takes for a non-PlayerControl target is NOT yet
    // witnessed (the LABEL_363 leg is unread), so no replacement gate is invented
    // here: the admit condition returns to "the target owns a seat we selected".
    if (seat_idx >= 0 && !target->seats.empty())
        world.commands.mount_boarding_command(self->net_id, target_ssn,
                                              static_cast<uint8_t>(command));
}

} // namespace opennova::world
