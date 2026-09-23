#include <runtime/world/vehicle_attach.h>
// Infantry motor: per-frame update for AI soldiers (entity class org1).
// [orig: Entity_UpdateInfantryAI @ 0x4b9910]. Spec: docs/world/world-wac-ai-re.md §3.
//
// Locomotion is anim-driven: the 16-tick think picks a movement order, the selector maps
// it to an anim state, and the playing clip's root-motion track (injected through
// IRootMotionSource) moves the entity. Without a source every state is unavailable and
// the soldier stands — exactly the original's relationship between motion and clip data.
//
// Open movement/presentation divergences are maintained in
// docs/world/world-wac-ai-re.md (D-INF records). The shared motor includes
// waypoint/boarding orders, obstacle detours, model collision and water state.
// Idle interest/greeting selection and the remaining escort/drag behaviors
// remain open; completed channel/boarding/water work is documented there.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <runtime/devtools/tick_profile.h>

#include <runtime/audio/footstep_slot.h>
#include <base/io/bam.h>
#include <runtime/terrain_query/height_field.h>

#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/infantry_ladder.h>
#include <runtime/world/infantry_sound.h>
#include <runtime/world/collision_force.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_view.h> // player_view_floor_eye_to_terrain (the on-foot local eye leg)
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h> // registry.get for the local-player AiEntity->Entity mirror
#include <base/io/fixed.h>

namespace opennova::world {

namespace {

// [orig: 0x4b9910 — body turn clamp ±69273360/tick (~5.8 deg)]
constexpr int32_t kBodyTurnClamp = 69273360;
// Leg-chain chase constants, per motor (D-INF-12 closure, byte-witnessed 2026-07-16).
// org1 (NPC) [orig: @0x4bea11-0x4beb12]: quarter-step (sixteenth when def+84&0x200),
// rate clamp ±0x5000000 (~7 deg), twist limit ±0x20000000 (45 deg) from the BODY.
// org2 (player) [orig: @0x4b49e9-0x4b4aa9]: quarter-step, rate clamp ±0x3000000
// (~4.2 deg), twist limit ±0x30000000 (67.5 deg) from the render YAW.
// Shared re-plant hysteresis [orig: @0x4be977-0x4be9cc / @0x4b4991-0x4b49e3]:
// |Δ| > 59652320 (~5 deg) and (|Δ| > 357913920 (~30 deg) or the leg's 64-tick
// window) — the LEFT leg's window runs 32 ticks behind the right's ((tick-32)&0x3F
// vs tick&0x3F [orig: @0x4be991/@0x4be9bb; org2 ebp = tick&0x3F @0x4b4680]).
constexpr int32_t kLegChaseClamp = 83886080;        // 0x5000000, org1
constexpr int32_t kLegTwistLimit = 0x20000000;      // org1, vs body
constexpr int32_t kLegChaseClampOrg2 = 0x3000000;   // org2 [orig: @0x4b49fb]
constexpr int32_t kLegTwistLimitOrg2 = 0x30000000;  // org2, vs yaw [orig: @0x4b4a23]
constexpr int32_t kLegReplantMin = 59652320;
constexpr int32_t kLegReplantSnap = 357913920;
// [orig: gravity, witnessed per tick with the ladder/drowning skip (Flags 0x108000,
// modeled at the tick sites) and terminal -32768. NPC org1: vel_z -= 416 (@0x4bf7bf)
// then pos.z += 2*vel (@0x4bf7ec). Player org2: vel_z -= 208 (@0x4b7acf, gate
// @0x4b7ac8) then pos.z += vel once, folded into the root-dz store (@0x4b7cef);
// clamp @0x4b7c77. D-INF-10 CLOSED for both legs 2026-07-16.]
constexpr int32_t kGravityStep = 416;
constexpr int32_t kGravityStepPlayer = 208;
constexpr int32_t kTerminalVelZ = -32768;
// [orig: jump launch vel_z impulse, Entity_UpdateInfantryPlayerBody @0x4b7ee5
// mov [esi+0A0h], 1600h; the in-air flag entity+0x24 |= 0x2000 the same block sets]
constexpr int32_t kJumpImpulseVelZ = 0x1600;
// The org2 jump gate's exact entity Flags mask: in-air (0x2000), dead (0x2),
// drowning/water (0x8000), and the terrain-gradient slide bit (0x10000).
// Carried (0x40) is tested separately immediately afterward. The reimpl keeps
// flags in two mirrors plus a typed mounted relation, so collapse those carriers
// at the one shared local/remote eligibility seam.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B7EA4..0x4B7EBD]
constexpr uint32_t kPlayerJumpBlockedFlags = 0x1A002u;
// [orig: turn-in-place gates; dump 2940-2952]
constexpr int32_t kTurnStopGate = 536870880;  // > 45 deg -> state 147 (stop)
constexpr int32_t kTurnWalkGate = 357913920;  // > 30 deg -> state 1 (walk turn)
// [orig: BAM bearing scale 683565275.5764316 = 2^31/pi (dbl_7C19D8)]
constexpr double kBamPerRadian = 683565275.5764316;
// [orig: degrees -> BAM32 = 2^32/360 = 11930464; same const as ai.cpp/promote.cpp.
// Used only to mirror the local player's engine heading back to the registry Entity's
// mission yaw — the precise (90 - deg) Q1 reconciliation lives with the present path.]
int32_t abs_bam(int32_t v) { return opennova::io::bam_abs(v); } // x86 neg: INT32_MIN stays put, never UB


int32_t damp_npc_slide(int32_t v) {
    const int32_t out = opennova::io::bam_sar(7 * v + 4, 3); // [orig: 0x4b9910 entity[38/39] decay]
    return abs_bam(out) <= 8 ? 0 : out;
}

int player_directional_state(int base, int move_dir_index) {
    static constexpr int kOffsetFromInputIndex[8] = {0, 7, 6, 5, 4, 3, 2, 1};
    return base + kOffsetFromInputIndex[move_dir_index & 7];
}

// The quantized direction table + accessor moved to world/dir_table.h (shared
// with the collision resolver); the generator/witness notes live there. (D-INF-4)

} // namespace

// Shared with infantry_combat.cpp (declared in infantry_internal.h).
int32_t bearing_to(int32_t dx, int32_t dy) {
    return static_cast<int32_t>(std::atan2(static_cast<double>(dy), static_cast<double>(dx)) *
                                kBamPerRadian);
}

// Shared with infantry_remote_anim.cpp (the size-gate split): these were
// file-local helpers; they keep their bodies verbatim and only gain external
// linkage so the extracted leg can call them. Declared in infantry_internal.h.

// The two halves of the org2 jump gate's Flags reads. The 0x1A002 word test
// (`test eax,1A002h` @0x4b7ea4: in-air, dead, the water pair, the terrain
// slide) fails INTO the landing else-leg @0x4b7f71; the carried test past it
// (`test al,40h; jnz 0x4b8020` @0x4b7ebb) skips the whole tail instead. The
// local block keys on the halves; the remote projection reads the union.
namespace {
bool player_jump_flags_blocked(const InfantryState &inf, const Entity *ent) {
    if (inf.airborne) return true;
    if (ent == nullptr) return false;
    return ((ent->flags | ent->engine_flags) & kPlayerJumpBlockedFlags) != 0;
}

bool player_jump_carried(const Entity *ent) {
    return ent != nullptr &&
           (ent->mounted || ((ent->flags | ent->engine_flags) & kEntityFlagMounted) != 0);
}
} // namespace

bool player_jump_world_state_blocked(const InfantryState &inf, const Entity *ent) {
    return player_jump_flags_blocked(inf, ent) || player_jump_carried(ent);
}

// ----------------------------------------------------------------------------
// Navigation think (every 16 ticks, authority). [orig: 0x4b9910 dump 1293-1540]
// ----------------------------------------------------------------------------
void AiSystem::infantry_think(AiEntity &e, World &world) {
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;

    // [orig: entity[74] decremented once per think; dump 1338]
    if (inf.wait_cooldown > 0) --inf.wait_cooldown;

    inf.move_mode = 0;
    inf.target_dist = 0;
    inf.at_final_oneshot = false;
	inf.board_anim = -1;

	const int32_t ch = slot.f[37]; // [orig: slot+148 = waypoint channel / command]
    // The board-target/carrier cache survives only under command 125 — every
    // other think clears it. [orig: @0x4b9910 think head — aiComp[36] = 0
    // unless aiComp[37] == 125]
    if (ch != 125) slot.f[36] = 0;
    // Reserved command range: 123/124/125 Goto-SSN-and-board, 126 goto-group
    // hold, 127 follow the local player — dispatched in infantry_board.cpp,
    // BEFORE the has-route gate like the original (retires the D-INF-2
    // early-return). [orig: the @0x4b9910 command dispatch on aiComp+148]
    if (ch >= 123 && ch <= 127) {
        infantry_command_think(e, world);
        return;
    }
    // [orig: dump 1409 — needs the has-route flag (slot+140) and no hold cooldown]
    if (ch == 0 || slot.f[35] == 0 || inf.wait_cooldown > 0) return;

    const NavChannel *nc = nav.channel(ch);
    if (nc == nullptr || nc->count == 0) { // [orig: dword_A71DD4[34*ch]==0 -> clear order]
        slot.f[35] = 0;
        return;
    }
    int32_t node = slot.f[38]; // [orig: slot+152 = node index (BMS wp_number at spawn)]
    if (node < 0 || node >= nc->count) node = 0; // container-rebase guard

    const NavEntry *mk = nav.entry(nav.entry_index(ch, node));
    if (mk == nullptr) { slot.f[35] = 0; return; }

    // Distance to the node: 3D with 1.0u vertical slack, target 0.25u above the marker.
    // [orig: dump 1421-1457 — targetZ = marker.z + 0x4000; dz = max(0, |dz| - 0x10000)]
    auto dist_to = [&](const NavEntry &n, int32_t out_target[3]) -> int32_t {
        out_target[0] = n.f[1];
        out_target[1] = n.f[2];
        out_target[2] = n.f[3] + 0x4000;
        const double dx = static_cast<double>(out_target[0]) - e.pos[0];
        const double dy = static_cast<double>(out_target[1]) - e.pos[1];
        int32_t dzi = out_target[2] - e.pos[2];
        dzi = abs_bam(dzi) - 0x10000;
        if (dzi < 0) dzi = 0;
        const double dz = static_cast<double>(dzi);
        // The shared sqrt-overflow guard: min(sqrt, flt_7C19E0 = 2147418112.0) before
        // the ftol [orig: @0x4bab4d, @0x4bac8b, @0x4bae51 `fld flt_7C19E0; fcom`].
        return static_cast<int32_t>(
                std::min(std::sqrt(dx * dx + dy * dy + dz * dz), 2147418112.0));
    };

    int32_t target[3];
    int32_t dist = dist_to(*mk, target);
    int32_t radius = mk->f[0]; // [orig: marker dword[0] = arrival radius]
    // [orig: dump 1462 — at the last node of a one-shot path (gait approach flag)]
    inf.at_final_oneshot = (node >= nc->count - 1) && ((nc->loopflag & 1) != 0);

    if (dist > radius) {
        // Walk toward the current node. [orig: moveMode = 3; dump 1461]
        inf.move_mode = 3;
        inf.target_dist = dist;
        inf.arrival_radius = radius;
        inf.move_target[0] = target[0];
        inf.move_target[1] = target[1];
        inf.move_target[2] = target[2];
        inf.target_heading = bearing_to(target[0] - e.pos[0], target[1] - e.pos[1]);
        return;
    }

    // Arrived. [orig: dump 1464-1532]
    mark_waypoint_visited(e, world, ch, node);

    if (mk->wait_ticks != 0) {
        // Face the marker's authored heading and hold. [orig: dump 1468-1477 —
        // entity[106] = marker+16; entity[74] = (wait + 8) >> 4 think-ticks]
        inf.target_heading = mk->f[4];
        inf.wait_cooldown = (mk->wait_ticks + 8) >> 4;
    }

    // Advance the node (wrap), honoring the one-shot end. [orig: dump 1479-1532]
    ++node;
    if (node >= nc->count) node = 0;
    if (node == 0 && (nc->loopflag & 1) != 0) {
        slot.f[38] = nc->count - 1;
        inf.wait_cooldown = 20; // [orig: entity[74] = 20 at the one-shot end]
        return;
    }
    slot.f[38] = node;
    // The next-node step is gated on the LIVE cooldown, not on the marker having
    // authored a wait: a wait of 1..7 ticks computes to 0 and retail already walks
    // toward the next node in the same think [orig: `if (!entity->thinkCooldown)`
    // @0x4badbf].
    if (inf.wait_cooldown != 0) return;

    const NavEntry *next = nav.entry(nav.entry_index(ch, node));
    if (next == nullptr) return;
    dist = dist_to(*next, target);
    inf.move_mode = 4; // [orig: moveMode = 4 after advancing; dump 1522]
    inf.target_dist = dist;
    inf.arrival_radius = next->f[0];
    inf.move_target[0] = target[0];
    inf.move_target[1] = target[1];
    inf.move_target[2] = target[2];
    inf.target_heading = bearing_to(target[0] - e.pos[0], target[1] - e.pos[1]);
}

// ----------------------------------------------------------------------------
// State selection + commit (every think). [orig: 0x4b9910 dump 2896-3110, 3693-3710]
// ----------------------------------------------------------------------------
int AiSystem::infantry_resolve_state(int adm_id, int state) const {
    if (root_motion == nullptr) return -1;
    auto has = [&](int s) { return root_motion->has_clip(adm_id, s); };
    if (has(state)) return state;
    // Cited fallback chains. [orig: availability = animMap[id] != animMap[0]; jog<->run
    // mutual fallback dump 3010-3025; wounded falls back to the base gait]
    switch (state) {
        case anim_state::kJogForward:
            if (has(anim_state::kRunForward)) return anim_state::kRunForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kRunForward:
            if (has(anim_state::kJogForward)) return anim_state::kJogForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        // (145/146 have no fallback: the wounded gate only substitutes them when the
        //  adm authors them [orig: @0x4bd70f..0x4bd73f], so the base gait survives.)
        case anim_state::kStop:
            // [orig: Entity_UpdateInfantryAI @0x4b9910 post-pass, kong 154811:
            //  animStateId == 147 && animMap[147] == animMap[0] -> 43]. Retail
            // converts AFTER the commit, at the end of the same think with no
            // observer in between; resolving before the commit is a declared
            // placement adaptation. Data-driven per body: 29 of the 219 retail
            // .adms author anim_stop and play 147; Eindo_R.adm does not.
            break;
        case anim_state::kIdle2:
        case anim_state::kIdle3:
            if (has(anim_state::kIdle)) return anim_state::kIdle;
            break;
        // Stance-clip fallbacks for a model lacking crouch/prone clips [orig: animMap[id] !=
        // animMap[0]]: crouch-walk -> stand walk; prone-walk -> crouch -> stand; stance idle -> idle.
        case anim_state::kWalkCrouchForward:
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kWalkProneForward:
            if (has(anim_state::kWalkCrouchForward)) return anim_state::kWalkCrouchForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kIdleCrouch:
        case anim_state::kIdleProne:
            if (has(anim_state::kIdle)) return anim_state::kIdle;
            break;
        default:
            break;
    }
    // [orig: targetAnimState seeds 43 with no availability test; an unauthored
    //  idle plays slot 0's node through the registration fill]
    return anim_state::kIdle;
}

// The flag-table arbitration both selectors share [orig: org2
// @0x4b7356..0x4b7396; org1 @0x4bd845..0x4bd874]: an uninterruptible current
// (bit 0x4) queues the target to pending; an exit-gated current (0x20) commits
// only a movement-flagged (bit 0) target; else commit now. Returns the state
// the channel commits (the current one when the target was queued).
static int arbitrate_body_state(InfantryState &inf, int resolved) {
    const uint32_t curf = infantry_anim_flags(inf.anim_state);
    if ((curf & 0x4u) != 0) {
        inf.anim_pending = resolved;
        return inf.anim_state;
    }
    if ((curf & 0x20u) == 0 || (infantry_anim_flags(resolved) & 0x1u) != 0) {
        inf.anim_pending = 0;
        return resolved;
    }
    inf.anim_pending = resolved;
    return inf.anim_state;
}

// Commit a resolved org1 target state. The two selectors differ by one test:
// org1 compares the target with the current state first and skips the whole
// arbitration on equality, retaining pending [orig: Entity_UpdateInfantryAI
// @0x4bd837..0x4bd843 cmp/jz loc_4BD87E].
// The channel retarget with the gait->stance transition insert [orig:
// AnimMap_UpdateEntity @0x40b662..0x40b737]: with no deferral armed, a forward
// gait committing to its crouch/prone walk plays the 169-172 transition clip
// first and re-arms the real target as pending for the clip-end promotion,
// gated on the adm actually carrying the clip. The replication replica channel runs
// the same insert through the shared pair map (D-NET-209 / D-INF-23).

void commit_body_state(InfantryState &inf, int resolved) {
    if (resolved < 0) return; // no clips at all: hold the current state
    if (resolved == inf.anim_state) return;
    const int committed = arbitrate_body_state(inf, resolved);
    if (committed != inf.anim_state)
        inf.request_body_animation(committed);
}

// Commit a resolved org2 (player-body) target state. The player arbitrates
// unconditionally against the old state captured before the selection, so a
// re-selection of the current state overwrites a queued pending with itself
// (locked / exit-gated legs) or clears it (the middle leg) [orig:
// Entity_UpdateInfantryPlayerBody old state @0x4b70e5, arbitration
// @0x4b7356..0x4b7396]. The rotor-wash substitution then rewrites the
// committed state [orig: @0x4b73a0..0x4b73e5].
void commit_player_body_state(InfantryState &inf, int resolved,
                              const IRootMotionSource *root_motion, bool wash) {
    if (resolved < 0) return; // no clips at all: hold the current state
    int committed = arbitrate_body_state(inf, resolved);
    if (wash && root_motion) {
        if (committed == anim_state::kWalkForward &&
                root_motion->has_clip(inf.adm_id, anim_state::kWashWalk))
            committed = anim_state::kWashWalk;
        if ((committed == anim_state::kIdle || committed == anim_state::kIdle2) &&
                root_motion->has_clip(inf.adm_id, anim_state::kWashIdle))
            committed = anim_state::kWashIdle;
    }
    // One final channel commit: intermediate land poses must not restart
    // an unchanged wash blend every fourth body tick.
    if (committed != inf.anim_state)
        inf.request_body_animation(committed);
}

void AiSystem::infantry_select(AiEntity &e, World &world, int selected_state) {
    InfantryState &inf = e.inf;
    const Entity *self = world.registry.get(e.handle);

    // EMPLACED. A mounted body in a GUNNER seat takes the emplaced state and
    // skips the gait/idle selection entirely:
    //
    //     v99 = entity->parentSlot == 3;
    //     if ( v99 ) {
    //         Entity_AttachToBoneAndUpdateTransform(entity, v455);
    //         entity->pendingAnimStateId = nullptr;
    //         entity->animStateId = 67;
    //         if ( itemDef->gap_86c == 1 && animMap[68] != *animMap ) = 68;
    //         if ( itemDef->gap_86c == 2 && animMap[69] != *animMap ) = 69;
    //     }
    //
    // [orig: Entity_UpdateInfantryAI @0x4b9910, the isInVehicle branch; the same
    //  block appears in the player body @0x4b40e0.] SeatType::Gunner IS retail's
    //  parentSlot 3, and `gap_86c` is the itemDef mount config our ItemSeatSpec
    //  carries as mount_config and promote stamps onto the occupant as
    //  mounted_config.
    //
    // Without this a seated gunner fell through to the not-moving branch and
    // idled: measured on the wire, retail spends 21.3% of all infantry rows in
    // state 67 and we emitted it 0.0% of the time, while idling 47% against
    // retail's 9.7%.
    //
    // DIVERGENCE, declared: retail tests `animMap[68] != *animMap`, i.e. the
    // variant row differs from the default entry. We ask the root-motion source
    // whether the clip exists, which is the same question our anim registry can
    // answer; the bone attach and pendingAnimStateId clear that retail does here
    // belong to the presentation/attach path and are not reproduced in the
    // selector.
    if (self != nullptr && self->mounted && self->mount_type == SeatType::Gunner) {
        int emplaced = anim_state::kEmplaced; // 67
        if (self->mounted_config_valid && root_motion != nullptr) {
            if (self->mounted_config == 1 && root_motion->has_clip(inf.adm_id, 68))
                emplaced = 68;
            if (self->mounted_config == 2 && root_motion->has_clip(inf.adm_id, 69))
                emplaced = 69;
        }
        commit_body_state(inf, infantry_resolve_state(inf.adm_id, emplaced));
        return;
    }
    // The WALK-vs-RUN gate, ported 1:1 from the move-state selection:
    //
    //     v99 = entity->damageTimer == 0;
    //     v327 = 1; targetAnimState = 1;                                  // WALK
    //     if ( !v99 || *((_BYTE *)playerSlotPtr + 136) || entity->wasHit )
    //         { v327 = 149; targetAnimState = 149; }                      // RUN
    //
    // [orig: Entity_UpdateInfantryAI @0x4b9910, the targetAnimState 1/149 block.]
    //
    // RETRACTS the previous reading ("entity[190] || slot+136 || combat-reaction
    // byte +875"). Two of those three terms were wrong, and the first was inert:
    // `alert_timer` (entity[190]) was read here and is WRITTEN NOWHERE in the
    // whole engine, so the term was always false. Retail's first term is damageTimer,
    // which IS live on both legs that raise it -- damage (+10, capped 25) and
    // SIGHT (+12, capped 15) -- so a soldier who merely sees an enemy runs. Ours
    // kept walking, which is why route followers covered a fraction of their
    // channel: node 7 of 30 in 430 s against retail finishing it.
    const bool alerted = inf.damage_timer != 0 ||
                         e.slot.bytes()[AiSlot::kAlertByte] != 0 || inf.was_hit;

    int target = selected_state > 0 ? selected_state : anim_state::kIdle;
    const bool moving = inf.move_mode != 0 && inf.target_dist > 0;
    const bool dragging = infantry_is_dragger(e, world);
    if (moving) {
        // A dragger searches and compares turn error while facing away from
        // its travel goal. Restore body heading after the gait choice below.
        // [orig: @0x4BD468, @0x4BD5B1]
        if (dragging) inf.body_heading = io::bam_add(inf.body_heading, INT32_MIN);
        infantry_detour(*this, e, world);
        target = alerted ? anim_state::kRunForward : anim_state::kWalkForward; // [dump 2898-2906]
        // Final-node approach gait. [orig: dump 2907-2924, ported literally incl. the skip]
        if ((inf.move_mode == 2 || inf.move_mode == 3) && inf.at_final_oneshot) {
            bool skip = false;
            if (inf.target_dist > 139264) {
                if (inf.target_dist > 270336) skip = true;
            } else if (inf.arrival_radius < 73728) {
                target = anim_state::kWalkForward;
            }
            if (!skip && inf.arrival_radius < 139264) target = anim_state::kJogForward;
        }
    }

    if (!moving) inf.path_state = 0; // [orig: zero targetDist -> 0x4BD2E9]

    // Turn-in-place overrides only in the moving branch. [orig: @0x4BD4AC]
    if (moving) {
        const int32_t err = abs_bam(opennova::io::bam_sub(inf.target_heading, inf.body_heading));
        if (err > kTurnStopGate) target = anim_state::kStop;
        else if (err > kTurnWalkGate) target = anim_state::kWalkForward;
        if (dragging) {
            inf.body_heading = io::bam_add(inf.body_heading, INT32_MIN);
            inf.target_heading = io::bam_add(inf.target_heading, INT32_MIN);
        }
    }

    auto has = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // Alerted idle: damageTimer or the slot alert byte (NOT wasHit) promotes 43 to the
    // armed idle 49 when the combat pass holds a target in slot[3], else 44.
    // [orig: LABEL_636 @0x4bd2f0..0x4bd31c]
    if ((inf.damage_timer != 0 || e.slot.bytes()[AiSlot::kAlertByte] != 0) &&
        target == anim_state::kIdle)
        target = e.slot.f[3] != 0 ? anim_state::kIdle3 : anim_state::kIdle2;

    // The previous-think move mode the pre_attack reaction reads [orig: @0x4bd356
    // `pad_368[2] = moveMode`, after the idle collapse].
    inf.prev_move_mode = inf.move_mode;

    // [orig: @0x4BD35C..0x4BD393] Drag pose wins after the ordinary gait
    // fallbacks, and suppresses idle gaze without changing the live look yaw.
    if (dragging) {
        inf.aim_override = true;
        inf.aim_pitch = 0;
        const bool walking = target == anim_state::kJogForward ||
                target == anim_state::kRunForward || target == anim_state::kWalkForward;
        const int drag_state = walking ? anim_state::kDraggerWalk : anim_state::kDraggerIdle;
        if (has(drag_state)) target = drag_state;
    }

    // Hit flinch: a body hit since the last think swaps its idle for cover_idle (163)
    // or its run/jog for cover_run (164) when the adm authors them, and wasHit is
    // consumed here, on the think cadence. [orig: LABEL_711 @0x4bd6a7..0x4bd6ee]
    if (inf.was_hit) {
        if ((target == anim_state::kIdle || target == anim_state::kIdle2) &&
            has(anim_state::kCoverIdle))
            target = anim_state::kCoverIdle;
        else if ((target == anim_state::kRunForward || target == anim_state::kJogForward) &&
                 has(anim_state::kCoverRun))
            target = anim_state::kCoverRun;
        inf.was_hit = false;
    }

    // Wounded gaits at half max-health, substituted only when the adm authors the
    // wounded clip: a jogger without wounded_run stays a jogger.
    // [orig: LABEL_722 @0x4bd6f5..0x4bd744 — def+380 >> 1; animMap[146]/[145] != *animMap]
    if (e.health <= static_cast<int16_t>(inf.max_health / 2)) {
        if ((target == anim_state::kRunForward || target == anim_state::kJogForward) &&
            has(anim_state::kWoundedRun))
            target = anim_state::kWoundedRun;
        else if (target == anim_state::kWalkForward && has(anim_state::kWoundedWalk))
            target = anim_state::kWoundedWalk;
    }

    // The NPC applies wash before animation arbitration. Unlike the player
    // path, its jog/run can select wash_run. [orig: @0x4BD78D..0x4BD7E6]
    if (world.rotor_wash.nearby_zone(e.pos, 983040)) {
        if (target == anim_state::kWalkForward && has(anim_state::kWashWalk))
            target = anim_state::kWashWalk;
        else if ((target == anim_state::kRunForward || target == anim_state::kJogForward) &&
                has(anim_state::kWashRun))
            target = anim_state::kWashRun;
        else if ((target == anim_state::kIdle || target == anim_state::kIdle2) &&
                has(anim_state::kWashIdle))
            target = anim_state::kWashIdle;
    }

    // A forced state stays numerically selected even if the ADM aliases that
    // slot to RESET. The ordinary gait fallbacks still apply to a later override.
    // [orig: raw forced store @0x4BD266, common arbitration @0x4B9910]
    const int resolved = world.script.forced_animation != 0 &&
            target == world.script.forced_animation ? target :
            infantry_resolve_state(inf.adm_id, target);
    commit_body_state(inf, resolved);
}

// The witnessed org2 player-body selection — see the ai.h declaration. One function
// for the local player AND the authority's remote-player path, exactly as the
// original runs the same @0x4b40e0 body for both. Inputs are the already-deposited
// entity+0x12C mirrors (player_moving / dir index / stance / lean bits) plus the
// per-tick weapon mirrors (scope_raised, wpn_run_anim, wpn_force_crouch).
// The local player's rain ambient [orig: Entity_UpdateInfantryPlayerBody
// @ 0x4b4747..0x4b490e — on the frame's last 16 ms quantum, while
// Env_RainPctCurrent != 0, the rain set handles are loaded (dword_24E0E80)
// and the kind is rain: the volume is the rain current (<= 0xFFFF by its max
// clamp), scaled by (lightTransfer x 0.5 + 0.5) when the first blink hit
// (entity+0x1D0) names a pool-2 building (ItemDef+0x218) — that hit alone
// gates it, no indoor-flag test (@ 0x4b4770..0x4b47a8) — and its low 16 bits
// are the 8.8 volume word (`mov word ptr [..], bx` @ 0x4b4845); the
// LPNV_RAIN_L set registers at (x + 2 m, y, z + eyeOffsetZ) on slot type 1
// and LPNV_RAIN_R at (x - 2 m, y, z + eyeOffsetZ) on slot type 2 (entity
// +0x74 added to Z @ 0x4b47b0 / @ 0x4b4865), lifetime 20, pitch 0x10000,
// through SoundEmitter_RegisterSetLayers @ 0x528340]. Our per-tick
// registration coalesces per (source, lane) in the mailbox exactly like the
// per-frame one.
void infantry_rain_ambient(World &world, const Entity &ent) {
    const WeatherState &weather = world.weather;
    if (weather.core.scalar_channels.rain_pct_fp == 0 ||
        weather.precipitation_kind != static_cast<uint32_t>(PrecipitationKind::Rain))
        return;
    int32_t volume = weather.core.scalar_channels.rain_pct_fp;
    // The first blink hit's owner carries the interior daylight transfer.
    if (ent.blink_hits[0] != 0) {
        const EntityHandle building = EntityHandle::make(2, static_cast<int>(ent.blink_hits[0] >> 20));
        if (const Entity *b = world.registry.get(building)) {
            volume = static_cast<int32_t>((b->light_transfer * 0.5f + 0.5f) *
                                          static_cast<float>(volume));
        }
    }
    // The word registers as is: the registrar tests the WHOLE word, so
    // 1..0xFF keeps a live level-0 slot and only 0 is the unregister
    // [orig: SoundEmitter_RegisterSetLayers @ 0x528377 `cmp [ecx+18h], bx`];
    // the mailbox's zero-volume clear is the same contract.
    const uint16_t volume_word = static_cast<uint16_t>(volume);
    constexpr uint16_t kLifetimeTicks = 20;
    constexpr int32_t kEarOffset = 2 << 16;
    const int32_t px = static_cast<int32_t>(ent.position.x * 65536.0f);
    // The emitter Z is the entity Z plus the eye-offset Z (entity +0x74).
    const float emitter_z = ent.position.z + static_cast<float>(ent.eye_offset_z) / 65536.0f;
    for (int side = 0; side < 2; ++side) {
        SoundEmitterEvent ev;
        ev.source_spawn_id = ent.registry_spawn_id;
        ev.source_handle = ent.handle.packed;
        ev.pos = ent.position;
        ev.pos.x = static_cast<float>(side == 0 ? px + kEarOffset : px - kEarOffset) / 65536.0f;
        ev.pos.z = emitter_z;
        ev.source_bms_id = ent.bms_id;
        ev.emitted_tick = world.logic_tick + 1;
        ev.lane = static_cast<uint8_t>(side == 0 ? 1 : 2);
        ev.lifetime_ticks = kLifetimeTicks;
        ev.pitch_q16 = 0x10000;
        ev.volume_q8_8 = volume_word;
        ev.set_name = side == 0 ? "LPNV_RAIN_L" : "LPNV_RAIN_R";
        world.out.sound_emitters.publish(std::move(ev));
    }
}

// [orig: Entity_UpdateInfantryPlayerBody @0x4b7183-0x4b7396]
void AiSystem::player_body_select(AiEntity &e, World &world, uint32_t entity_flags,
        uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    // Burn selection bypasses the ordinary movement/swim/lean selector, including
    // the pass that clears the timer. [orig: Entity_UpdateInfantryPlayerBody @0x4B70D9..0x4B717E -> @0x4B729D]
    if (inf.burn_state != 0) {
        const int target = select_infantry_burn(inf, root_motion, true, logic_tick);
        commit_player_body_state(inf, infantry_resolve_state(inf.adm_id, target), root_motion,
                world.rotor_wash.nearby_zone(e.pos, 983040) != 0);
        return;
    }
    auto has = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // No in-air branch here: the original SKIPS this selection while airborne
    // (@0x4b70b8) — the jump block stamps 30/31 and the fall edge stamps 31 (47
    // under a deployed chute, Flags 0x20) directly [orig: @0x4b7ef2/@0x4B7E61].
    int target;
    if (inf.player_moving) {
        inf.idle_counter = 0;                        // [orig: @0x4b719b]
        int base = anim_state::kWalkForward;         // [orig: @0x4b7196]
        if (inf.stance == InfantryState::Stance::kProne)
            base = anim_state::kWalkProneForward;    // [orig: @0x4b71ad]
        else if (inf.stance == InfantryState::Stance::kCrouch)
            base = anim_state::kWalkCrouchForward;   // [orig: @0x4b71bd]
        target = player_directional_state(base, inf.player_move_dir_index); // [orig: @0x4b71c7]
    } else if (inf.stance == InfantryState::Stance::kProne) {
        target = anim_state::kIdleProne;             // [orig: @0x4b722f]
    } else if (inf.stance == InfantryState::Stance::kCrouch) {
        target = anim_state::kIdleCrouch;            // [orig: @0x4b724b]
        // ForceCrouch (0x40000) weapons promote the crouch idle to idle_mortar when
        // the clip exists. [orig: @0x4b723f-0x4b7279 — Entity_CheckWeaponSeatFlags
        // (equipped, 0x40000) then animMap[46] != animMap[0]]
        if (inf.wpn_force_crouch && has(anim_state::kIdleMortar))
            target = anim_state::kIdleMortar;
    } else {
        // Standing idle: 43 until 62 selection passes have elapsed, then 44.
        // [orig: @0x4b727b-0x4b7293 state = 0x2B + (++entity[0x148] >= 0x3E)]
        ++inf.idle_counter;
        target = inf.idle_counter >= 62 ? anim_state::kIdle2 : anim_state::kIdle;
    }

    // Run promotion: pure-forward standing walk only, suppressed while scoped.
    // tier = weight band + run_anim. The band reads entity+0x37C = the LOADOUT
    // WEIGHT the S2C 0x5A apply sums (weaponweight + clips x clipweight over the
    // slots; Terrain_AccumulateSectorScores @0x425220, store @0x425310, called
    // @0x4296f9) — the "no writer" reading behind the old constant 2 (D-INF-16)
    // missed that store: > 0x430000 (67 u) or negative -> 0 (no run), >= 0x210000
    // (33 u) -> 1 (run_2), else 2 (run_3) [orig: @0x4b72aa..0x4b72cf]. Heavy kits
    // jog or walk. tier 1 -> run_2 if authored; tier >= 2 -> run_3 if authored,
    // ELSE the same run_2 test: the run_3-absent compare `jz short loc_4B730A`
    // @0x4b72f8 lands on the tier-1 arm's `animMap[9] != animMap[0]` test
    // @0x4b730a, so a body adm without run_3 runs at run_2 when it has one.
    // [orig: @0x4b729d-0x4b731b; scope Flags&0x10 test @0x4b72e2; run_3 test
    //  @0x4b72f3-0x4b72f8, store @0x4b72fa; run_2 test @0x4b730a, store @0x4b7311]
    if (target == anim_state::kWalkForward && !inf.scope_raised) {
        const int32_t w = inf.loadout_weight_fp16;
        const int band = (w > 0x430000 || w < 0) ? 0 : (w >= 0x210000 ? 1 : 2);
        const int tier = band + inf.wpn_run_anim;
        if (tier >= 2 && has(anim_state::kRun3))
            target = anim_state::kRun3;              // [orig: @0x4b72fa]
        else if (tier >= 1 && has(anim_state::kRun2))
            target = anim_state::kRun2;              // [orig: @0x4b7311]
    }

    // Prone lean rolls from the lean bits; right (bit 7) wins when both are held.
    // The original gates on !(Flags & 0x112002): dead 0x2 and in-air 0x2000 are
    // modeled (health/airborne); the 0x10000 leg is an unmodeled tail, and the
    // 0x100000 leg is subsumed by the climb block's same-tick state override.
    // [orig: @0x4b731b-0x4b7354]
    if (inf.stance == InfantryState::Stance::kProne && e.health > 0 && !inf.airborne) {
        if (inf.lean_left) target = anim_state::kRollLeft;   // [orig: @0x4b7335]
        if (inf.lean_right) target = anim_state::kRollRight; // [orig: @0x4b734c]
    }

    // SWIM. After the land/wash selection, a body on the float latch
    // (0x8000) that is not dead takes the
    // swim state STRAIGHT -- no availability test, no flag-table arbitration,
    // pending cleared: moving -> the direction index picks 37 forward / 38
    // left / 40 back / 39 right; idle -> 36. The same MoveOrder&7 index the
    // land gaits use, collapsed to four strokes.
    //
    // Retail computes the land state into the same plain field and then
    // OVERWRITES it here; only the final value reaches the anim layer. Our
    // crossfade channel restarts on every intermediate stamp (the same reason
    // the ladder override skips the selection), so the swim override REPLACES
    // the land commit instead of following it -- one commit per pass, the same
    // final value. Committing both thrashed the blend at the 4-tick cadence:
    // the weight never passed ~0.4, the pose looked frozen, and the never-
    // evicted blend SOURCE (the entry tick's jump_loop) kept feeding its
    // root-motion forward step -- the "auto swims forward" defect.
    // [orig: Entity_UpdateInfantryPlayerBody, kong 148422-148452:
    //  `(Flags & 0x8000) && !(Flags & 2)`; switch(MoveOrder & 7) case 0: 37;
    //  1,2: 38; 3,4,5: 40; 6,7: 39; else 36; pendingAnimStateId = 0]
    if ((entity_flags & kEntityFlagDrowning) != 0 && (entity_flags & kEntityFlagDead) == 0) {
        int swim = anim_state::kSwimIdle;
        if (inf.player_moving) {
            switch (inf.player_move_dir_index & 7) {
                case 0: swim = anim_state::kSwimForward; break;
                case 1: case 2: swim = anim_state::kSwimLeft; break;
                case 3: case 4: case 5: swim = anim_state::kSwimBack; break;
                case 6: case 7: swim = anim_state::kSwimRight; break;
                default: break;
            }
        }
        inf.request_body_animation(swim);
        inf.anim_pending = 0;
        return;
    }

    commit_player_body_state(inf, infantry_resolve_state(inf.adm_id, target), root_motion,
            world.rotor_wash.nearby_zone(e.pos, 983040) != 0);
}

// The lean-angle producer — see the ai.h declaration. Decay runs every body tick for
// every infantry body (the corpse keeps decaying, matching the original's placement
// before the weapon-channel block); the ramp needs a live, non-prone body.
// [orig: decay @0x4b5c97 lean -= (lean+8)>>4; ramp @0x4b7dbf/@0x4b7dd6]
void AiSystem::infantry_lean_tick(AiEntity &e, uint32_t entity_flags) {
    InfantryState &inf = e.inf;
    inf.lean_angle =
        io::bam_sub(inf.lean_angle, io::bam_sar(io::bam_add(inf.lean_angle, 8), 4));
    if (e.health <= 0) return;                      // [orig: the Flags&2 gate legs]
    // The ladder latch (hands on the rungs) and a deployed chute block the ramp.
    // [orig: (Flags & 0x100020) gate @0x4b7dad]
    if ((entity_flags & (kEntityFlagLadderContact | kEntityFlagParachute)) != 0)
        return;
    // The prone skip reads prone_local = the latch AND none of Flags 0x10A000
    // (in air / drowning / ladder) — afloat or airborne the ramp still runs
    // [orig: the prone_local derivation @0x4b416c..0x4b4183; the ramp gate
    //  @0x4b7da9].
    const bool prone_effective =
            inf.stance == InfantryState::Stance::kProne &&
            (entity_flags & (kEntityFlagInAir | kEntityFlagDrowning |
                             kEntityFlagLadderContact)) == 0;
    if (prone_effective) return; // [orig: the prone skip]
    if (inf.lean_left) inf.lean_angle = io::bam_add(inf.lean_angle, -0x3000000);
    if (inf.lean_right) inf.lean_angle = io::bam_add(inf.lean_angle, 0x3000000);
}

// The torso-roll producer -- see the ai.h declaration. Prone idle decays toward
// level; the combat rolls RAMP it +-0x4000000 (5.625 deg) per tick -- the FP
// barrel-roll view (the chase block skips 41/42; the ramp is a separate site in
// the same body pass); everything else chases the entity's slope roll a
// sixteenth-step per tick with the LAG clamped to roll +-0x0E38E380 (20 deg) --
// the clamp also snaps the wrapped post-roll value back once the clip ends.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5cff-0x4b5d6d (decay/skip/chase)
//  + @0x4b700c-0x4b7025 (the 41/42 ramp)]
void AiSystem::infantry_torso_roll_tick(AiEntity &e) {
    InfantryState &inf = e.inf;
    if (inf.anim_state == anim_state::kIdleProne) {  // [orig: cmp 0x30 @0x4b5d05]
        inf.torso_roll =
            io::bam_sub(inf.torso_roll, io::bam_sar(io::bam_add(inf.torso_roll, 8), 4));
        return;
    }
    if (inf.anim_state == anim_state::kRollLeft) {   // [orig: @0x4b700e]
        inf.torso_roll = io::bam_add(inf.torso_roll, -0x4000000);
        return;
    }
    if (inf.anim_state == anim_state::kRollRight) {  // [orig: @0x4b701d]
        inf.torso_roll = io::bam_add(inf.torso_roll, 0x4000000);
        return;
    }
    inf.torso_roll = io::bam_add(
        inf.torso_roll, io::bam_sar(io::bam_add(io::bam_sub(e.roll, inf.torso_roll), 8), 4));
    const int32_t delta = io::bam_sub(inf.torso_roll, e.roll);
    if (delta > 0x0E38E380) inf.torso_roll = io::bam_add(e.roll, 0x0E38E380);   // [orig: @0x4b5d4e]
    if (delta < -0x0E38E380) inf.torso_roll = io::bam_add(e.roll, -0x0E38E380); // [orig: @0x4b5d61]
}


// See the infantry.h contract: the motor store is the writer of the two-store pair, so a
// redeploy that only writes the registry Entity is undone by finish_infantry_tick.
void infantry_respawn_snap(AiEntity &e, const int32_t pos[3], int32_t heading,
                           int16_t health) {
    e.pos[0] = pos[0];
    e.pos[1] = pos[1];
    e.pos[2] = pos[2];
    e.heading = heading;
    e.pitch = 0;
    e.roll = 0;
    e.body_pitch = 0;
    e.health = health;
    e.vel_x = 0;
    e.vel_z = 0;
    // Nothing is in flight toward the old pose any more; a stale interpolation target
    // would drag a redeployed body back toward where it died.
    e.net_smooth_target[0] = pos[0];
    e.net_smooth_target[1] = pos[1];
    e.net_smooth_target[2] = pos[2];
    e.net_smooth_heading = heading;
    e.net_smooth_pitch = 0;
    e.net_interp_progress = 0;
    e.net_interp_steps = 0;
    e.collide_state = {};

    InfantryState &inf = e.inf;
    inf.player_moving = false;
    inf.player_move_dir_index = 0;
    inf.move_mode = 0;
    inf.target_dist = 0;
    // The death clip is a LOCKED anim family (flags 0x82), so the selection commit would
    // defer every later change to clip end and keep the corpse posed. Reseed the spawn
    // idle the way the original's respawn does [orig: @0x4b9714 — spawn body state 44].
    inf.reset_body_animation(anim_state::kIdle);
    inf.reload_anim_ticks = 0;
    inf.arms_dip_ticks = 0;
    inf.pitch_kick_accum = 0;
    inf.recoil_pitch = 0;
    inf.weapon_weight_spread = 0;
    inf.aimed_shot_available = false;
    inf.idle_counter = 0;
    inf.lean_left = false;
    inf.lean_right = false;
    inf.lean_angle = 0;
    inf.torso_roll = 0;
    inf.carrier_pitch_lag = 0;
    inf.body_heading = heading;
    inf.target_heading = heading;
    inf.leg_yaw[0] = inf.leg_yaw[1] = heading;
    inf.leg_target[0] = inf.leg_target[1] = heading;
    inf.vel[0] = inf.vel[1] = inf.vel[2] = 0;
    inf.z_quarter_step = 0; // [orig: Entity_ResetToSpawnState @0x4B967A]
    inf.stance = InfantryState::Stance::kStand;
    // `airborne` stays with the registry word, which no retail respawn writer
    // touches (see InfantryState::reset_for_spawn) [orig: Entity_ResetToSpawnState
    // @0x4b97b0 `Flags &= ~2`; Server_ProcessPlayerDeath @0x51787a].
    inf.jump_requested = false;
    inf.jump_cooldown = 0;
    // A restore armed by a pre-death ladder exit must not chase the fresh
    // spawn's view pitch (this snap is the host-respawn twin of
    // reset_for_spawn's clear).
    inf.pitch_restore_active = false;
    inf.pitch_restore_target = 0;
    inf.pitch_restore_prev = 0;
    inf.ground_cache_valid = false;
}

// See the infantry.h contract. [orig: Entity_HandleDamageAndTriggerZones
// @0x40772f return; @0x407b4d..0x407b4f clear; @0x407b5e / @0x407c71 re-arm]
void player_body_class_think(Entity &body) {
    if (((body.flags | body.engine_flags) & kEntityFlagDead) != 0) return;
    body.cause_flags &= ~0xF00u;
    body.spawn_phase = 64;
}

// See the infantry.h contract. [orig: BoneCallback_org0_Skin @0x4e3669..0x4e368e]
int32_t death_ctrl_register_value(bool dead, int32_t corpse_timer) {
    if (dead && corpse_timer < 248) {
        int32_t ramp = corpse_timer - 62;
        if (ramp < 0) ramp = 0;
        return (ramp << 16) / 186;
    }
    return 0xFFFF;
}

// ----------------------------------------------------------------------------
// The per-tick motor. [orig: Entity_UpdateInfantryAI @0x4b9910]
// ----------------------------------------------------------------------------
// The resolver's player predicate is the entity's wire Player class bit, for
// local and remote bodies alike; the resolver keys every physics leg on it and
// reserves `entity == g_local_player_entity` for the local side-writes.
// [orig: Entity_MovementCollisionResolver @0x4B2BD0 — Flags & 0x100 @0x4B2CD9 /
// @0x4B2F7C / @0x4B3271 / @0x4B33AA / @0x4B3C78]
static bool entity_is_player_class(const World &world, EntityHandle handle) {
    const Entity *ent = world.registry.get(handle);
    return ent != nullptr && ((ent->flags | ent->engine_flags) & kEntityFlagPlayer) != 0;
}


// The two organic motors share the gradient/gain kernel but apply its result
// on opposite sides of the body rotation. The NPC also arms the detour latch.
// [orig: Entity_UpdateInfantryAI @0x4BA896..0x4BA96E;
// Entity_UpdateInfantryPlayerBody @0x4B79DC..0x4B7AAE]
static bool infantry_terrain_motion(AiEntity &e, Entity *entity,
        const terrain::TerrainHeightField *field, bool npc, int32_t &dx, int32_t &dy) {
    const bool carried = entity != nullptr &&
            (entity->mounted || entity->ground_target.valid());
    // Org1's carrier branch bypasses the gradient without touching Flags
    // 0x10000 after the deck rotation. Org2 clears the bit for a carrier.
    // [orig: carrier @0x4BA85A; exit @0x4BA891]
    if (npc && carried) return false;
    const uint32_t flags = entity != nullptr ? entity->flags | entity->engine_flags : 0u;
    terrain::TerrainHeightGradient gradient;
    if (!carried && (flags & 0x90A000u) == 0 && field != nullptr)
        gradient = terrain::height_field_gradient_fixed(*field, e.pos[0], e.pos[1]);
    const int32_t magnitude = static_cast<int32_t>(std::min(
            std::sqrt(double(gradient.dx) * gradient.dx + double(gradient.dy) * gradient.dy),
            2147418112.0));
    if (magnitude < 768) {
        if (entity != nullptr) {
            entity->flags &= ~0x10000u;
            entity->engine_flags &= ~0x10000u;
        }
        return false;
    }
    // IMUL, ADD/ADC 0x8000, SHRD 16: retain the low signed dword.
    // The unsigned shift also pins the negative-product bits in C++17.
    const auto scale = [](int32_t value) {
        return static_cast<int32_t>(
                static_cast<uint64_t>(int64_t(value) * 419392 + 0x8000) >> 16);
    };
    dx = io::bam_sub(0, scale(gradient.dx));
    dy = scale(gradient.dy);
    e.inf.vel[2] = std::min(e.inf.vel[2], -167);
    if (entity != nullptr) {
        entity->flags |= 0x10000u;
        entity->engine_flags |= 0x10000u;
    }
    if (npc) e.inf.path_state = 1; // [orig: @0x4BA94E; boarding reads @0x4BB325]
    return true;
}

// The org1 heading chase, legs and look, for a body that is not riding a seat.
// Body: quarter-step toward the target heading, clamped +-69273360; the live
// look moves by the SAME step, preserving its offset from the body. A carried
// (Flags 0x40) body snaps both legs to the body; any other chases its legs.
// The look then chases the aim pair. The legs are consumed as the R/L leg-chain
// bone yaw by Entity_BuildBoneTransformMatrices @0x4b1290 (section 14).
// [orig: Entity_UpdateInfantryAI body @0x4BE8FD..0x4BE931, carried snap
//  @0x4BE934 -> @0x4BEB0C, legs @0x4BE944..0x4BEB0A, look @0x4BEB18..0x4BEBE7]
static void infantry_org1_heading_chase(AiEntity &e, const Entity *ent, uint32_t key) {
    InfantryState &inf = e.inf;
    const int32_t diff = io::bam_sub(inf.target_heading, inf.body_heading);
    int32_t step = io::bam_sar(io::bam_add(diff, 2), 2);
    if (step > kBodyTurnClamp) step = kBodyTurnClamp;
    if (step < -kBodyTurnClamp) step = -kBodyTurnClamp;
    inf.body_heading = io::bam_add(inf.body_heading, step);
    e.heading = io::bam_add(e.heading, step);

    if (ent != nullptr && ((ent->flags | ent->engine_flags) & kEntityFlagMounted) != 0) {
        inf.leg_yaw[0] = inf.body_heading;
        inf.leg_yaw[1] = inf.body_heading;
    } else {
        // A movement state or a def+84&0x200 body takes the WALK path: the right
        // foot half-snaps to the target, the left pulls a quarter of the
        // residual, both targets = target, the alternating shuffle while
        // walking/turning. [orig: selector @0x4be944-0x4be967; walk path
        // @0x4be9d4-0x4bea0b]
        if ((infantry_anim_flags(inf.anim_state) & 0x1u) != 0 ||
            (e.def_attrib & kItemAttribLandable) != 0) {
            const int32_t tgt = inf.target_heading;
            inf.leg_yaw[0] = io::bam_add(
                inf.leg_yaw[0], io::bam_sar(io::bam_sub(tgt, inf.leg_yaw[0]), 1));
            inf.leg_yaw[1] = io::bam_add(
                inf.leg_yaw[1], io::bam_sar(io::bam_sub(tgt, inf.leg_yaw[0]), 2));
            inf.leg_target[0] = tgt;
            inf.leg_target[1] = tgt;
        } else {
            // Idle: re-plant targets toward the MIDPOINT of (body, target),
            // measured vs the current TARGET, left window 32 ticks behind the
            // right. [orig: midpoint @0x4be969-0x4be975; L @0x4be977/@0x4be991-0x4be9a7;
            //  R @0x4be97f/@0x4be9bb-0x4be9cc]
            const int32_t mid = io::bam_add(
                inf.target_heading,
                io::bam_sar(io::bam_sub(inf.body_heading, inf.target_heading), 1));
            const int32_t dl = io::bam_sub(mid, inf.leg_target[1]);
            if (abs_bam(dl) > kLegReplantMin &&
                (abs_bam(dl) > kLegReplantSnap || ((key - 32) & 63u) == 0))
                inf.leg_target[1] = mid;
            const int32_t dr = io::bam_sub(mid, inf.leg_target[0]);
            if (abs_bam(dr) > kLegReplantMin &&
                (abs_bam(dr) > kLegReplantSnap || (key & 63u) == 0))
                inf.leg_target[0] = mid;
        }
        for (int leg = 0; leg < 2; ++leg) {
            // Quarter-step (sixteenth for def+84&0x200 bodies), clamp +-0x5000000
            // (~7 deg/tick), twist limit +-0x20000000 (45 deg) vs the BODY.
            // [orig: R @0x4bea11-0x4bea8d; L @0x4bea8d-0x4beb12; step pick @0x4bea1d]
            const int32_t ldiff = io::bam_sub(inf.leg_target[leg], inf.leg_yaw[leg]);
            int32_t lstep = (e.def_attrib & kItemAttribLandable) != 0
                                ? io::bam_sar(io::bam_add(ldiff, 8), 4)
                                : io::bam_sar(io::bam_add(ldiff, 2), 2);
            if (lstep > kLegChaseClamp) lstep = kLegChaseClamp;
            if (lstep < -kLegChaseClamp) lstep = -kLegChaseClamp;
            inf.leg_yaw[leg] = io::bam_add(inf.leg_yaw[leg], lstep);
            const int32_t twist = io::bam_sub(inf.leg_yaw[leg], inf.body_heading);
            if (twist > kLegTwistLimit)
                inf.leg_yaw[leg] = io::bam_add(inf.body_heading, kLegTwistLimit);
            else if (twist < -kLegTwistLimit)
                inf.leg_yaw[leg] = io::bam_sub(inf.body_heading, kLegTwistLimit);
        }
    }
    infantry_look_tick(inf, e.heading, e.pitch);
}

// The org2 local view/leg phase precedes the scoped additions, before later
// animation selection or ladder movement consumes aim.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B4945..0x4B4BC6;
// scoped additions @0x4B5C78..0x4B5C91]
// `tick` is the raw current_tick the org2 prologue banks [orig: @0x4B4147].
static void infantry_local_view_tick(AiEntity &e, const Entity *tick_entity, uint32_t tick) {
    InfantryState &inf = e.inf;
    // org2 on-foot [orig: Entity_UpdateInfantryPlayerBody @0x4b4945-0x4b4ac1].
    // There is NO body chase: the LEGS chase the mouse yaw (+0x10) directly and
    // the body heading is written as their midpoint — the legs lead, the body
    // follows, and the §14 torso twist is (yaw − leg midpoint). The parachute
    // (Flags 0x20) sixteenth-step body chase @0x4b494d runs first, below; the
    // seat-bone follow @0x4b654e is the mounted pose (ai_system.cpp).
    //
    // While latched on a ladder the view yaw is clamped to ±120° of the
    // body heading (infantry_ladder.cpp). [orig: gate @ 0x4b4978; clamp
    // @ 0x4b4b04-0x4b4b42]
    const uint32_t leg_flags = tick_entity != nullptr
            ? (tick_entity->flags | tick_entity->engine_flags)
            : 0u;
    if ((leg_flags & kEntityFlagParachute) != 0) {
        const int32_t delta = io::bam_add(io::bam_sub(inf.target_heading, inf.body_heading), 8) >> 4;
        inf.body_heading = io::bam_add(inf.body_heading, delta);
    }
    infantry_ladder_view_clamp(inf, leg_flags);
    e.heading = inf.target_heading; // mouse-instant render/aim yaw [orig:
                                    // Input_HandleActionBinding @0x49ad40 writes +0x10]
    const int32_t yaw = e.heading;
    if ((leg_flags & (kEntityFlagLadderContact | kEntityFlagMounted |
                      kEntityFlagParachute)) != 0) {
        // Latched on a ladder, carried, or under a chute: the whole
        // re-plant / chase / midpoint model is SKIPPED and both legs snap
        // to the body heading — the legs stay locked to the ladder while
        // the torso alone twists toward the view [orig: `test ecx,100060h;
        // jnz loc_4B4AC6` @0x4b4972..0x4b4978; the tail `mov eax,[esi+8Ch];
        // mov [esi+2D4h],eax; mov [esi+2D8h],eax` @0x4b4b5f..0x4b4b6b].
        inf.leg_target[0] = inf.body_heading;
        inf.leg_target[1] = inf.body_heading;
        inf.leg_yaw[0] = inf.body_heading;
        inf.leg_yaw[1] = inf.body_heading;
    } else if ((infantry_anim_flags(inf.anim_state) & 0x1u) != 0) {
        // A movement state re-plants both feet on the yaw every tick.
        // [orig: @0x4b4984 flag-table bit0 -> @0x4b49dd/@0x4b49e3]
        inf.leg_target[1] = yaw;
        inf.leg_target[0] = yaw;
    } else {
        // Idle: per-leg re-plant measured vs the CURRENT LEG YAW (org1 measures
        // vs the target), left window 32 ticks behind the right. Both windows
        // key on the RAW tick banked at @0x4B4147, unstaggered.
        // [orig: L @0x4b4993/@0x4b49ad-0x4b49bc ((tick-32)&0x3F);
        //  R @0x4b499b/@0x4b49d0-0x4b49e3 (ebp = tick&0x3F @0x4B467A..0x4B4683)]
        const int32_t dl = io::bam_sub(yaw, inf.leg_yaw[1]);
        if (abs_bam(dl) > kLegReplantMin &&
            (abs_bam(dl) > kLegReplantSnap || ((tick - 32) & 63u) == 0))
            inf.leg_target[1] = yaw;
        const int32_t dr = io::bam_sub(yaw, inf.leg_yaw[0]);
        if (abs_bam(dr) > kLegReplantMin &&
            (abs_bam(dr) > kLegReplantSnap || (tick & 63u) == 0))
            inf.leg_target[0] = yaw;
    }
    const bool leg_model_skipped =
            (leg_flags & (kEntityFlagLadderContact | kEntityFlagMounted |
                          kEntityFlagParachute)) != 0;
    for (int leg = 0; leg < 2 && !leg_model_skipped; ++leg) {
        // Quarter-step, rate clamp ±0x3000000 (~4.2 deg/tick — 3/5 the org1
        // rate), twist limit ±0x30000000 (67.5 deg) vs the YAW, not the body.
        // [orig: R @0x4b49e9-0x4b4a43; L @0x4b4a49-0x4b4aa9]
        const int32_t ldiff = io::bam_sub(inf.leg_target[leg], inf.leg_yaw[leg]);
        int32_t lstep = io::bam_sar(io::bam_add(ldiff, 2), 2);
        if (lstep > kLegChaseClampOrg2) lstep = kLegChaseClampOrg2;
        if (lstep < -kLegChaseClampOrg2) lstep = -kLegChaseClampOrg2;
        inf.leg_yaw[leg] = io::bam_add(inf.leg_yaw[leg], lstep);
        const int32_t twist = io::bam_sub(inf.leg_yaw[leg], yaw);
        if (twist > kLegTwistLimitOrg2)
            inf.leg_yaw[leg] = io::bam_add(yaw, kLegTwistLimitOrg2);
        else if (twist < -kLegTwistLimitOrg2)
            inf.leg_yaw[leg] = io::bam_sub(yaw, kLegTwistLimitOrg2);
    }
    // bodyHeading = legL + (legR - legL)/2. [orig: @0x4b4aa9-0x4b4abb]
    if (!leg_model_skipped)
        inf.body_heading = io::bam_add(
            inf.leg_yaw[1], io::bam_sar(io::bam_sub(inf.leg_yaw[0], inf.leg_yaw[1]), 1));
    // The motor-side look-pitch clamp RELATIVE TO THE BODY PITCH: ±80 deg,
    // ±40 deg while effectively prone, measured from the slope-conformed
    // body pitch (+0x90) rather than level — prone uphill shifts the window.
    // Skipped only for a seat in a vehicle parent flagged +0x2EC (mounted
    // bodies leave this block earlier). "Effectively prone" = the prone
    // latch with none of Flags 0x10A000 (in air / drowning / ladder).
    // [orig: @0x4b4b71..0x4b4bc6 — limit select @0x4b4b76/@0x4b4b7d, the
    //  two-sided clamp on +0x14 against +0x90 @0x4b4ba2..0x4b4bc6;
    //  prone_local gate @0x4b416c..0x4b4183]
    {
        const bool prone_effective =
                inf.stance == InfantryState::Stance::kProne &&
                (leg_flags & (kEntityFlagInAir | kEntityFlagDrowning |
                              kEntityFlagLadderContact)) == 0;
        // 0x1C71C700 (+40 deg) / 0x38E38E00 (+80 deg) — the same two BAM
        // constants player_look.h names for the input-side clamp.
        const int32_t limit = prone_effective ? 0x1C71C700 : 0x38E38E00;
        if (inf.look_pitch - e.body_pitch > limit) inf.look_pitch = e.body_pitch + limit;
        if (inf.look_pitch - e.body_pitch < -limit) inf.look_pitch = e.body_pitch - limit;
    }
    // The original clamp writes entity Pitch directly; publish our input
    // mirror before the following drift and later body consumers read it.
    e.pitch = inf.look_pitch;
}

void AiSystem::tick_infantry(AiEntity &e, World &world, uint32_t logic_tick) {
    // Recoil/dispersion live ahead of the network-snap motor exit [orig: the
    // Entity_UpdateInfantryAI flag test @0x4b9a03 exits past the sound block]. Received
    // shots are applied during the network pump, then decay in this frame's
    // body pass; locally generated shots happen later and first decay on the
    // following frame. The recoil PRNG draw is unconditional, including R=0.
    // [orig: Entity_UpdateInfantryPlayerBody / Entity_UpdateInfantryAI]
    Entity *tick_entity = world.registry.get(e.handle);
    if (tick_entity != nullptr && tick_entity->motor_suspended) return;
    // A newly spawned remote player is already org2 before its first pose
    // uplink sets net_is_remote_peer. NPC corpse ownership must follow the
    // entity's player bit, not whether a movement packet has arrived.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 vs org1 @0x4B9910]
    const bool npc_body = !e.inf.is_local_player && !e.net_is_remote_peer &&
            (tick_entity == nullptr ||
             ((tick_entity->flags | tick_entity->engine_flags) & kEntityFlagPlayer) == 0);
    if (tick_entity != nullptr && npc_body) {
        npc_respawn_unhide(world, *this, *tick_entity);
        if (((tick_entity->flags | tick_entity->engine_flags) & 1u) != 0) return;
    }
    // Org1 copies the primary state and its pending target into the secondary
    // channel at the motor head, before think or authority interpolation. The
    // two playheads and variant rings remain independent. No equipped ADM or
    // player hold-pose selection participates in this write.
    // [orig: Entity_UpdateInfantryAI @0x4B9A14..0x4B9A48; copy @0x4B9A28]
    if (npc_body) {
        const int state = e.inf.anim_state;
        e.inf.request_weapon_animation(state);
        e.inf.wpn_deferred = e.inf.anim_pending;
    }
    RootMotionFrame frame;
    // Org2 rotates root output before this tick's view/leg chase changes +0x8C.
    // [orig: @0x4B41E4..0x4B4255 precedes @0x4B4945..0x4B4ABB]
    const int32_t player_root_heading = e.inf.body_heading;
    bool have_clip = false;
    // Each body samples secondary then primary before any state producers.
    // A wire-owned org2 does the same in remote_player_body_anim below.
    // [orig: org1 @0x4B9A48; org2 @0x4B41DF; dual order @0x40B908/@0x40B94E]
    if (!e.net_is_remote_peer) {
        devtools::ProfileLap animation_lap(world.profile);
        infantry_weapon_channel_advance(e);
        if (reset_capsule_bottom_state(e.inf.anim_state)) e.inf.prev_capsule_bottom = 0;
        if (root_motion != nullptr)
            have_clip = advance_primary_channel(e.inf, *root_motion, frame);
        if (have_clip) {
            if (e.inf.prev_capsule_bottom != 0)
                frame.dz = frame.capsule_bottom - e.inf.prev_capsule_bottom;
            e.inf.prev_capsule_bottom = frame.capsule_bottom;
        }
        e.inf.last_events = have_clip ? frame.events : 0;
        animation_lap.mark(devtools::Slot::SIM_AI_INFANTRY_ANIMATION);
    }
    // Both bodies stamp the mover-entry savedLivePose (+0x80..+0x88) before any
    // motion, remote org2 peers included; a shooter's lead reads the target's
    // Position minus this stamp as its one-tick displacement.
    // [orig: org1 Entity_UpdateInfantryAI @0x4B9A53..0x4B9A6E (after the dual
    //  channel advance, before the authority/interpolation gate); org2
    //  Entity_UpdateInfantryPlayerBody @0x4B4187..0x4B419C]
    if (tick_entity != nullptr) stamp_saved_live_pose(*tick_entity);
    // The org2 queued USE survives a ground-probe clear through entity+0x180.
    // Clear the request after the attempt, including failed mounts. Ordinary
    // local input still enters through LocalPlayer::toggle_mount.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B424A..0x4B4272]
    if (!npc_body && tick_entity != nullptr && e.inf.adm_id > 0 &&
        ((tick_entity->flags | tick_entity->engine_flags) &
         (kEntityFlagQueuedMount | kEntityFlagCarried)) == kEntityFlagQueuedMount) {
        if (!tick_entity->ground_target.valid())
            tick_entity->ground_target = tick_entity->mount_toggle_fallback;
        world.vehicles.player_toggle_mount(e.handle); // the latch is still set: the 0x200 arm
        tick_entity->flags &= ~kEntityFlagQueuedMount;
        tick_entity->engine_flags &= ~kEntityFlagQueuedMount;
    }
    // The player bodies decay recoil and spread at their head; org1 runs the
    // same pair after its think (below). [orig: org1 Entity_UpdateInfantryAI
    //  @0x4BE7FD..0x4BE86F]
    if (!npc_body) infantry_recoil_tick(e.inf, e.heading, e.pitch, world.next_prng16());
    InfantryWeightSpreadInputs weight_inputs;
    const uint32_t tick_flags = tick_entity != nullptr
            ? (tick_entity->flags | tick_entity->engine_flags)
            : 0u;
    const bool mounted_for_spread = tick_entity != nullptr && tick_entity->mounted;
    const WeaponTableEntry *held = tick_entity != nullptr
            ? world.tables.weapons.by_index(tick_entity->equipped_adm_index)
            : nullptr;
    weight_inputs.produce = e.inf.is_local_player && e.inf.player_moving &&
                            !mounted_for_spread && held != nullptr;
    weight_inputs.aimed_shot_available = e.inf.aimed_shot_available;
    weight_inputs.prone = e.inf.stance == InfantryState::Stance::kProne;
    weight_inputs.crouched = e.inf.stance == InfantryState::Stance::kCrouch;
    weight_inputs.drowning = (tick_flags & kEntityFlagDrowning) != 0;
    weight_inputs.airborne_rising =
            (e.inf.airborne || (tick_flags & kEntityFlagInAir) != 0) &&
            e.inf.vel[2] > 0;
    if (held != nullptr) {
        weight_inputs.weaponweight_fp16 = held->weaponweight_fp16;
        weight_inputs.clipweight_fp16 = held->clipweight_fp16;
    }
    if (!npc_body) infantry_weapon_weight_spread_tick(e.inf, weight_inputs);
    // Keep the on-foot view clamp/leg phase before scoped drift. The mounted
    // branch still consumes the same already-drifted look when posing its seat.
    // This phase has no PRNG draws, so recoil/yaw/pitch sampling stays ordered.
    // [orig: view/legs @0x4B4945..0x4B4BC6 before drift @0x4B5966]
    const bool local_view_prepared = e.inf.is_local_player && !mounted_for_spread;
    // The org2 body reads the RAW current_tick for its leg re-plant windows:
    // the prologue stores it into the frame slot and the leg phase masks that
    // slot, with no per-entity stagger term (that term is org1's alone).
    // [orig: Entity_UpdateInfantryPlayerBody store @0x4B4147; read
    //  @0x4B467A..0x4B4683 (ebp = tick & 0x3F)]
    if (local_view_prepared) infantry_local_view_tick(e, tick_entity, logic_tick);
    // [orig: Entity_UpdateInfantryPlayerBody @ 0x4B40E0, local-only gate @0x4B5966]
    if (world.local_player_state != nullptr)
        world.local_player_state->apply_scoped_aim_drift(e, logic_tick);
    if (!npc_body && (tick_flags & 1u) == 0) {
        const Entity *parent = tick_entity != nullptr && tick_entity->mounted
                ? world.registry.get(tick_entity->mount_target) : nullptr;
        const uint8_t stance_bits = e.inf.stance == InfantryState::Stance::kProne ? 1 :
                e.inf.stance == InfantryState::Stance::kCrouch ? 2 : 0;
        emit_stance_change_sound(world, e.handle.packed, e.pos, e.inf.stance_sound_state,
                stance_bits, tick_flags, parent != nullptr && parent->has_item_def);
    }

    // The player body's think cadence, ahead of BOTH org2 death edges (the
    // local edge below and remote_player_body_anim's): the plyr class callback
    // fires as event 0 whenever entity+0x2AC is <= 0, and the counter decrements
    // every tick, corpse included (the callback then returns on Flags&2, so a
    // dead body neither clears its kill-cause bits nor re-arms). The callback
    // also runs a trigger-zone relation pass (@0x407b64..0x407c5f) that is not
    // ported here. [orig: Entity_UpdateInfantryPlayerBody @0x4b4bc9..0x4b4be9;
    //  the edge gate follows @0x4b4bf1]
    if (tick_entity != nullptr && !npc_body &&
        (tick_flags & kEntityFlagPlayer) != 0) {
        if (tick_entity->spawn_phase <= 0) player_body_class_think(*tick_entity);
        --tick_entity->spawn_phase;
    }

    // A remote player's locomotion source is its C2S pose snapshot, so do not
    // run the NPC/local-input movement core over it. The authority still runs
    // the org2 body animation and shared collision tail in
    // remote_player_body_anim; retail's entity+0x24 bit 0 is the conditional
    // hard-snap/freeze gate, not an all-remote-player classifier. [orig: org2
    // head @0x4B411B..0x4B4127; hard-snap test @0x4C207E..0x4C2091;
    // resolver call @0x4B7CE0..0x4B7CF4]
    if (e.net_is_remote_peer) {
        if (is_authority) {
            const devtools::ProfileScope remote_scope(
                    world.profile, devtools::Slot::SIM_AI_INFANTRY_REMOTE);
            remote_player_body_anim(e, world, logic_tick);
        }
        return;
    }

    // The registry Entity is the script/HUD/wire health store. Hydrate the
    // motor copy before death selection and damage so item-trait resolution,
    // round hits, and scripted health changes all feed this tick's one result.
    if (const Entity *ent = world.registry.get(e.handle)) {
        e.health = ent->health;
        if (ent->health_max > 0) {
            e.inf.max_health = static_cast<int16_t>(
                std::min(ent->health_max,
                         static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
        }
    }

    InfantryState &inf = e.inf;
    // Per-entity stagger key. [orig: Entity_UpdateInfantryAI @0x4b9910 head
    //  @0x4b9948..0x4b9953 — tickKey = current_tick + 36 * entity+0x7C (the net
    //  id), strength-reduced as lea [eax+eax*8] then lea [tick+edx*4]]
    const uint32_t key = logic_tick + 36u * static_cast<uint32_t>(e.net_id);
    // Tick-start pose Z: the resolver's ladder entry gate measures the CL anchor
    // against the pose at the motor head, not the integrated one.
    // [orig: savedLivePose captured @ 0x4b4190-0x4b419c; entry read @ 0x4b327d]
    const int32_t tick_start_z = e.pos[2];

    // 1. Death edge, once per life. Org1 keys it on the dead bit its own edge
    // latches; the player bodies still ride the dead bit the host's damage-time
    // death routing sets, so they key on the posed death clip (the 0x82 family
    // flag), as does a rowless test body with no flags word.
    // [orig: Entity_UpdateInfantryAI `cmp [esi+11Eh],bp; jg` @0x4B9C40,
    //  `test byte ptr [esi+24h],2; jnz` @0x4B9C4D]
    const bool org1_body = npc_body && tick_entity != nullptr;
    if (e.health <= 0) {
        const bool edge_open = org1_body
                ? ((tick_entity->flags | tick_entity->engine_flags) & kEntityFlagDead) == 0
                : infantry_anim_flags(inf.anim_state) != 0x82u;
        if (edge_open) infantry_death_edge(*this, e, world, tick_entity, org1_body, logic_tick);
    }
    // Org1's corpse leg, think and motion gates read that dead bit, not health:
    // a script that writes health back onto a corpse leaves it dead.
    // [orig: corpse leg `test al,2; jz` @0x4B9D55..0x4B9D5A; think gate
    //  `test byte ptr [esi+24h],2; jnz` @0x4BA98B..0x4BA98F]
    const auto org1_dead_now = [&] {
        return org1_body &&
               ((tick_entity->flags | tick_entity->engine_flags) & kEntityFlagDead) != 0;
    };
    // The per-tick corpse block: LeaveCorpse keeps the body forever, otherwise the
    // timer drains and the corpse despawns — held while the local player can see
    // it. [orig: Entity_UpdateInfantryAI @0x4b9e4d-0x4ba000 persistence]
    if (org1_dead_now()) {
        if (infantry_drag_corpse(e, world)) inf.request_body_animation(139);
        const NpcCorpseStep result = step_npc_corpse(world, *this, *tick_entity);
        if (result == NpcCorpseStep::Removed) return;
    }
    // A respawn inside the corpse leg has restored the live flags for the rest
    // of this pass. [orig: the respawn leg re-enters at @0x4BA066]
    const bool body_live = org1_body ? !org1_dead_now() : e.health > 0;

    // Org1's phase order ahead of its think: the slope pass (cadence inside),
    // the attach sample, then the ground-carrier ride and the gradient.
    // [orig: Entity_UpdateInfantryAI slope @0x4BA066, attach sample @0x4BA348,
    //  carrier @0x4BA45D..0x4BA891, gradient @0x4BA896, think @0x4BA970]
    if (npc_body) infantry_slope_pass(e, world, logic_tick, key);
    const InfantryAttachmentPose attachment = !inf.is_local_player && body_live
            ? infantry_attachment_pose(e, world) : InfantryAttachmentPose{};
    if (npc_body) infantry_follow_carrier(e, world, frame.capsule_bottom, false);
    // Org1 samples before the think so path_state=1 is visible to this tick's
    // boarding and detour selection. Its replacement motion overrides the
    // motor-head animation sample and still rotates by body heading. [orig: @0x4BA8CC,
    // @0x4BA917..0x4BA94E; root rotation @0x4BF001]
    const auto *gradient_field = world.tables.terrain ? world.tables.terrain : terrain;
    int32_t gradient_dx = 0, gradient_dy = 0;
    const bool terrain_slide = npc_body && infantry_terrain_motion(
            e, tick_entity, gradient_field, true, gradient_dx, gradient_dy);

    if (e.health > 0 && inf.is_local_player) {
        const Entity *ent = world.registry.get(e.handle);
        // The rain ambient registration rides the local body tick.
        if (ent != nullptr) {
            infantry_rain_ambient(world, *ent);
            int32_t building_reverb = 0;
            if (ent->blink_hits[0])
                if (const auto *building = world.registry.get(EntityHandle::make(2, ent->blink_hits[0] >> 20)))
                    building_reverb = building->reverb;
            world.reverb.update(e.pos, building_reverb);
        }
        if ((logic_tick & 15u) == 0) world.commands.update_local_location(e.handle);
    } else if (body_live && is_authority && (key & 15u) == 0) {
		// 2. Think + selection (every 16 ticks). [orig: gate (tick & 0xF) | !authority]
		// A cached board-any target can upgrade an already seated NPC once
		// per 64 staggered ticks. Compare slot TYPE, not the userpoint index.
		// [orig: Entity_UpdateInfantryAI @0x4BA9D8..0x4BAA41; cadence @0x4BA9E1,
		// cache @0x4BA9FB, seat-type compare @0x4BAA1D, attach @0x4BAA2C]
		if ((key & 63u) == 0 && tick_entity != nullptr && tick_entity->mounted &&
				e.slot.f[37] == 125 && e.slot.f[36] > 0) {
			VehicleSeatSelection selected;
			const EntityHandle target{ static_cast<uint16_t>(e.slot.f[36] - 1) };
			if (find_best_vehicle_seat(world, target, e.handle, selected) &&
					selected.type != tick_entity->mount_type)
				world.vehicles.attach_to_seat(e.handle, selected);
		}
		infantry_think(e, world);
        // Combat produces the movement goal and preferred animation before the
        // common detour/gait selector. The complete think is gated at 16 ticks.
        // [orig: Entity_UpdateInfantryAI @0x4BA970; combat @0x4BBE24]
        int combat_state;
        {
            const devtools::ProfileScope combat_scope(
                    world.profile, devtools::Slot::SIM_AI_INFANTRY_COMBAT);
            combat_state = infantry_combat_think(e, world, key);
        }
        if (inf.burn_state != 0) {
            // The live burn branch reaches the common arbiter directly.
            // [orig: Entity_UpdateInfantryAI @0x4BC047..0x4BC04E -> @0x4BD7FD]
            const int target = combat_state > 0 ? combat_state : anim_state::kIdle;
            commit_body_state(inf, infantry_resolve_state(inf.adm_id, target));
        } else {
            // The debug/script override is after combat and before attachment and
            // gait selection. It bypasses the current animation's lock, preserving
            // pending until the ordinary arbiter changes it. Org2 never reads it.
            // [orig: Entity_UpdateInfantryAI @0x4BD256..0x4BD271]
            if (npc_body && world.script.forced_animation != 0) {
                combat_state = world.script.forced_animation;
                inf.store_body_animation(combat_state);
                inf.move_mode = 0;
                inf.target_dist = 0;
            }
            // On a ladder the NPC's gait selection is suppressed — the org1
            // on-ladder block after the resolve owns states 32-35 (the same-tick
            // overwrite mapping as the player selection skip above; retail also
            // zeroes a speed local our selector has no carrier for).
            // [orig: @ 0x4bd18d — moveMode + the speed local zeroed on Flags 0x100000]
            if (tick_entity != nullptr &&
                ((tick_entity->flags | tick_entity->engine_flags) &
                 kEntityFlagLadderContact) != 0) {
                inf.move_mode = 0;
                inf.target_dist = 0;
                inf.path_state = 0;
            }
		else if (!attachment.parent.valid()) {
			infantry_select(e, world, combat_state);
			if (inf.board_anim >= 0 && inf.move_mode == 0 &&
					(tick_entity == nullptr || !tick_entity->mounted))
				inf.request_body_animation(inf.board_anim);
		}
        }
	}

    // 2c. The on-ladder override + player dismounts (org2; EVERY tick — the
    // 4th-tick gate above covers only the stance selection). Body in
    // infantry_ladder.cpp. [orig: @ 0x4b7484-0x4b76d8]
    if (!inf.is_local_player) infantry_ladder_override(e, tick_entity);

    if (!inf.is_local_player && is_authority && body_live && (key & 15u) == 0) {
        infantry_attachment_select(e, world, attachment);
        if (npc_body && (tick_flags & kEntityFlagDead) == 0)
            infantry_attention_think(*this, e, world, key);
    }

    // The org1 post-think block, every tick: the recoil kick (its PRNG draw
    // follows any think) and the spread decay. [orig: Entity_UpdateInfantryAI
    //  recoil @0x4BE7FD..0x4BE84B, spread @0x4BE84E..0x4BE869]
    if (npc_body) {
        infantry_recoil_tick(inf, e.heading, e.pitch, world.next_prng16());
        infantry_weapon_weight_spread_tick(inf, InfantryWeightSpreadInputs{});
    }

    // Mounted pose is a late phase, not an update bypass: death ran first and a
    // living NPC has already perceived, selected, and aimed. The mounted return
    // below suppresses only ordinary ground locomotion.
    // [orig: Entity_UpdateInfantryAI @0x4b9910; pose @0x4bec23..0x4bed3f;
    //  dedicated return @0x4bf5c6]
    const bool mounted = e.health > 0 && pose_if_mounted(e, world);
    const Entity *mounted_occ = mounted ? world.registry.get(e.handle) : nullptr;
    const bool mounted_gunner =
            mounted_occ != nullptr && mounted_occ->mount_type == SeatType::Gunner;
    // A live mounted org1 body takes the seat pose; any other chases heading,
    // legs and look here, ahead of its eye offset and its sound/fire passes.
    // [orig: Entity_UpdateInfantryAI mounted-live local @0x4B9960..0x4B9985,
    //  its test @0x4BE8F0]
    if (npc_body && !mounted) infantry_org1_heading_chase(e, tick_entity, key);

    // The lean angle decays every body tick (corpse included — the decay sits before
    // the weapon-channel block in the original) and ramps while a lean key is held;
    // the torso roll chases the slope roll in the same pass [orig: @0x4b5cff].
    // [orig: @0x4b5c97 / @0x4b7dbf; see infantry_lean_tick]
    if (inf.is_local_player) {
        infantry_lean_tick(e, tick_entity != nullptr
                                  ? (tick_entity->flags | tick_entity->engine_flags)
                                  : 0u);
        infantry_torso_roll_tick(e);
    }

    // Selection writes only next tick's secondary request. Both channels have
    // already advanced at the motor head. [orig: @0x4B41DF before @0x4B5CAB]
    if (inf.is_local_player) infantry_weapon_channel(e, world, logic_tick);

    if (!npc_body) infantry_follow_carrier(e, world, frame.capsule_bottom, true);

    // Restamp the camera offset from the current simulated head, after the
    // motor-head channel update and current view/seat/lean pose. The local
    // on-foot path terrain-floors the absolute eye; the mounted path does not.
    // The capsule fallback retains the org1/org2 extent and lean formulas.
    // [orig: carrier selector @0x4B6386; mounted local selector @0x4B66CD;
    //  org2 @0x4B6908..0x4B696C / @0x4B6BB3..0x4B6CC8;
    //  org1 @0x4BF078..0x4BF14C]
    if (have_clip) {
        int32_t head_world[3] = {};
        if (inf.is_local_player && world.pose_provider != nullptr &&
                world.pose_provider->resolve_skeletal_anchor(
                        world, e.handle, SkeletalAnchor::Head, head_world)) {
            float head[3] = {
                static_cast<float>(from_fixed(io::bam_sub(head_world[0], e.pos[0]))),
                static_cast<float>(from_fixed(io::bam_sub(head_world[1], e.pos[1]))),
                static_cast<float>(from_fixed(io::bam_sub(head_world[2], e.pos[2])))
            };
            const Entity *reg = world.registry.get(e.handle);
            // The terrain floor is an ABSOLUTE-space rule, so apply it to the
            // absolute eye and fold the correction back into the delta. It is
            // also on-foot only: the mounted leg is unfloored in retail
            // [orig: the 0x2000 floors @0x4b68e7/@0x4b6b98 belong to the
            //  capsule legs, and @0x4b6908 has no floor of any kind].
            const bool seated = reg != nullptr && reg->mounted;
            if (!seated) {
                float abs_eye[3] = {static_cast<float>(from_fixed(e.pos[0]) + head[0]),
                                    static_cast<float>(from_fixed(e.pos[1]) + head[1]),
                                    static_cast<float>(from_fixed(e.pos[2]) + head[2])};
                player_view_floor_eye_to_terrain(
                        terrain,
                        reg != nullptr &&
                                (reg->flags & kEntityFlagIndoors) != 0,
                        abs_eye);
                head[0] = abs_eye[0] - from_fixed(e.pos[0]);
                head[1] = abs_eye[1] - from_fixed(e.pos[1]);
                head[2] = abs_eye[2] - from_fixed(e.pos[2]);
            }
            inf.eye_offset_x = to_fixed(head[0]);
            inf.eye_offset_y = to_fixed(head[1]);
            inf.eye_offset_z = to_fixed(head[2]);
        } else {
            // Player extent cap: on foot @0x4B698C, mounted @0x4B66D9.
            // [orig: @0x4B698C / @0x4B66D9; floors @0x4B6B98 / @0x4B68E7]
            const int32_t extent = frame.capsule_top - frame.capsule_bottom;
            const int32_t delta = inf.is_local_player ? std::min(extent, 0xD000)
                                                      : std::max(extent, 0x9000);
            const double lean_rad = static_cast<double>(inf.lean_angle) *
                                    io::kRadiansPerBam;
            const int32_t lean_cos =
                static_cast<int32_t>(std::cos(lean_rad) * io::kQ22One);
            int32_t eye_z =
                static_cast<int32_t>((static_cast<int64_t>(delta) * lean_cos) >> 22);
            if (inf.is_local_player && eye_z < 0x2000) eye_z = 0x2000;
            inf.eye_offset_z = eye_z;
            if (inf.is_local_player) {
                // A lost sample must not leave a stale head-frame lateral pair
                // behind: the capsule fallback carries no lateral model (the
                // 3-angle tilt is the tracked residue), so the lanes reset.
                inf.eye_offset_x = 0;
                inf.eye_offset_y = 0;
            } else {
                const int32_t lean_sin =
                    static_cast<int32_t>(std::sin(lean_rad) * io::kQ22One);
                const int32_t lat_raw = static_cast<int32_t>(
                    (static_cast<int64_t>(delta) * lean_sin) >> 22);
                const int32_t lat = (lat_raw * 3) >> 2;
                const double yaw_rad = static_cast<double>(e.heading) *
                                       io::kRadiansPerBam;
                const int32_t yaw_sin =
                    static_cast<int32_t>(std::sin(yaw_rad) * io::kQ22One);
                const int32_t yaw_cos =
                    static_cast<int32_t>(std::cos(yaw_rad) * io::kQ22One);
                inf.eye_offset_x = static_cast<int32_t>(
                    (static_cast<int64_t>(lat) * yaw_sin) >> 22);
                inf.eye_offset_y = -static_cast<int32_t>(
                    (static_cast<int64_t>(lat) * yaw_cos) >> 22);
            }
        }
    }

    // Org2 selects its next gait only after the current head pose was built.
    // [orig: head @0x4B6BB3; fourth-tick selector @0x4B70CE; ladder @0x4B7484]
    if (e.health > 0 && inf.is_local_player) {
        // 2'. Local player: the player-body input is set from host input each frame
        // (world::apply_player_body_input), never by the org1 AI think path. The body
        // selection is the witnessed org2 selector, every 4th tick like the original
        // (idle 43->44 counts SELECTION passes, so the cadence is load-bearing). The
        // player takes the motor's simulate branch on host (is_authority) and on a
        // client (entity==local) alike. [orig: Entity_UpdateInfantryPlayerBody
        // @0x4b40e0; 4th-tick gate @0x4b70ce; net-re §5.38]
        // The 4th-tick selection is SKIPPED while airborne (and for dead/carried
        // bodies — dead is the branch above; carried rides the mount slice): the
        // jump/fall edges own the in-air clip, and the selection resumes on landing.
        // [orig: Entity_UpdateInfantryPlayerBody @0x4b70b8 `test Flags,2000h` /
        // @0x4b70c6 `test al,42h` / @0x4b70ce `test tick,3` — any set skips]
        // The jump itself runs AFTER the vertical resolve (step 9b), as the original
        // orders it (integrate -> resolver -> edges -> jump @0x4b7e8c).
        const Entity *ent = world.registry.get(e.handle);
        const bool carried = ent != nullptr && ent->mounted;
        if ((logic_tick & 3u) == 0 && !inf.airborne && !carried) {
            player_body_select(e, world,
                    ent != nullptr ? (ent->flags | ent->engine_flags) : 0u, logic_tick);
        }
    }
    if (inf.is_local_player) infantry_ladder_override(e, tick_entity);

    // 3a. The anim-event consumers, in the witnessed order: the sound block
    // (foley + footsteps) precedes the fire bits inside the same consume
    // [orig: @0x4bf169-0x4bf2b0 before the 0x4 test @0x4bf322]. The sound pass
    // runs for BOTH bodies (its tick-parity gate differs per body) and for
    // corpses — the landing/foley legs are not health-gated in the original.
    infantry_anim_sound_pass(e, world, logic_tick, frame.capsule_bottom);
    // The fire pass: consume the fresh trigger bits + the walking-fire latch into
    // authoritative rounds (odd ticks). [orig: the @0x4bf15c-0x4bf4b0 fire block runs
    // after the anim advance refreshed g_animEventTriggerBits; §17.4]
    if (!inf.is_local_player && is_authority && e.health > 0 && !mounted_gunner)
        infantry_fire_pass(e, world, logic_tick);
    if (!inf.is_local_player && is_authority && e.health > 0 && mounted_gunner)
        infantry_mounted_fire_pass(e, world, logic_tick, key);

    // Retail exits the ordinary mover immediately after the dedicated UseGun
    // request. Seat pose already supplied the transform; animation, wire state,
    // and part channels remain live.
    // [orig: mounted fire tail @0x4bf4bb..0x4bf5c6]
    if (mounted) {
        // The live mounted tail invokes the movement resolver on its exact
        // eight-tick entity phase. Contact/trigger work remains live while the
        // mounted source latch suppresses push-out, preserving the parent pose.
        // The ladder IO rides along — retail has ONE resolver, so a mounted
        // resolve runs the same gated CL block (the latch-only channel here
        // would latch a carried body on any CL touch with no entry gate).
        // [orig: phase8 gate/call @0x4bf5a5..0x4bf5c3]
        // The +0x74 CameraOffset the think produced above [orig: written
        // @0x4b9910 kong 155519-155521 before either resolver call].
        const int32_t eye_offset[3] = {inf.eye_offset_x, inf.eye_offset_y,
                                       inf.eye_offset_z};
        if ((key & 7u) == 0 && collision != nullptr) {
            const LadderResolveIO mounted_lio = make_ladder_resolve_io(e, tick_start_z);
            const devtools::ProfileScope collision_scope(
                    world.profile, devtools::Slot::SIM_AI_INFANTRY_COLLISION);
            collision->resolve_entity(
                    world, e.handle, e.collide_state, e.pos, inf.vel, inf.vel[2],
                    frame.capsule_bottom, frame.capsule_top, e.heading, e.pitch,
                    entity_is_player_class(world, e.handle), is_authority,
                    logic_tick, inf.anim_state,
                    infantry_anim_flags(inf.anim_state), e.health, nullptr,
                    &mounted_lio, eye_offset, world.profile);
        }
        finish_infantry_tick(e, world);
        return;
    }

    // 4. Ground resample (every 8 ticks). [orig: dump 319-326, cache entity+676]
    // The radius-0 leg of the retail sampler is ONE model-aware center probe:
    // ray from pos + 1.0u lift, 48u drop, clipped by terrain AND candidate
    // models — a soldier on a building floor grounds on the FLOOR, not the
    // terrain under it (the frozen-bunker-garrison fix). Without a collision
    // world (headless tests) the terrain average stands as before.
    // [orig: Entity_CalcAverageGroundHeight @0x457230 radius==0 ->
    //  Entity_RaycastGroundHeightAndObject(entity, 0, 0, 0x10000, 0x300000)
    //  -> raycast_entity_collision @0x413760 (terrain + candidate models)]
    if (terrain != nullptr && ((key & 7u) == 0 || !inf.ground_cache_valid)) {
        if (collision != nullptr && collision->instance_count() != 0) {
            inf.ground_cache = collision->raycast_ground(
                world, e.handle, e.pos, 0, 0, 0x10000, 0x300000, nullptr);
        } else {
            GroundClearance clearance = ground_clearance;
            clearance.has_physics = e.has_physics;
            clearance.use_dead = (e.health <= 0);
            inf.ground_cache = calc_average_ground_height(*terrain, e.pos, 0, clearance);
        }
        inf.ground_cache_valid = true;
    }

    // 5. The org2 heading/leg chase (org1 chased above, before its fire pass).
    if (inf.is_local_player) {
        // A stale mount can fall back to the ordinary mover during this tick.
        // Valid mounted bodies returned above; ordinary local bodies already
        // ran the view phase ahead of their scoped drift.
        // Raw tick, not the org1 stagger key [orig: @0x4B4147 / @0x4B467A].
        if (!local_view_prepared) infantry_local_view_tick(e, tick_entity, logic_tick);
    }

    // 6. The org2 slope pass: conform-or-decay body_pitch/roll + the steep-ground
    // slide (org1 ran its pass after the corpse leg). Cadence lives inside (org2
    // decay every tick, probes every 2nd on the logic tick). [orig: @0x4b6d95 block]
    if (!npc_body) infantry_slope_pass(e, world, logic_tick, key);

    // Horizontal slide decay. NPC (org1): (7v+4)>>3 with an abs<=8 deadzone, every state. Player
    // (org2): the selector is Flags & 0x2000 = IN-AIR (entity.h already names it
    // kEntityFlagInAir; the same function pins the sense — the jump SETS it with
    // anim 30/31, the >0xF000 edge sets it with 31, landing CLEARS it with the
    // fall sounds, and body-anim selection is SKIPPED on it @0x4b70b8). The
    // AIRBORNE arm preserves momentum: (63*v)>>6, NO deadzone [orig: @0x4b7949];
    // the GROUNDED arm kills a slide in ~10 ticks: (7v+4)>>3 with the abs<=8
    // snap, same as the NPC [orig: @0x4b7982]. The earlier reading (D-INF-9)
    // had the two arms swapped — a grounded slide persisted ~8x too long and an
    // airborne one died fast; corrected 2026-08-26 from the kong differential
    // (@0x4b78ab selector; kong 193989-194043). [inf.airborne here is last
    // tick's value — the vertical resolve below updates it.]
    if (!inf.is_local_player) {
        inf.vel[0] = damp_npc_slide(inf.vel[0]);
        inf.vel[1] = damp_npc_slide(inf.vel[1]);
    } else if (inf.airborne) {
        // Airborne STEER, before the decay [orig: @0x4b78b7..0x4b790f]: while
        // the moving bit is held, push the slide pair 64/tick along
        // (cos,sin)(lookYawBam16 * dbl_7C9BC0 + dir * dbl_7C9BB0). The yaw
        // term loads the signed HIGH WORD of the entity's LOOK heading
        // (movsx word entity+0x12 @0x4b78c5 — the +0x10 heading dword, not
        // the +0x8C body heading org2 elsewhere prefers). The two
        // doubles are retail's STORED approximations (2*pi/65536 and pi/4,
        // read from the image: 0x7C9BC0 = 3F1921F9F01B866E, 0x7C9BB0 =
        // 3FE921F9F01B866E) and are ported verbatim; the form is the subtract
        // of the ftol-truncated products of the x87 DOUBLE cos/sin and the
        // float -64.0 (fcos/fsin, fmul flt_7C9BD8, _ftol2_sse — no narrowing
        // before the multiply) [orig: @0x4b78e5..0x4b790f; flt_7C9BD8 =
        // C2800000]. The DOUBLED arm (Flags&0x20 chute deployed,
        // vertical vel <= -0x3800, dir == 0 [orig: @0x4b7920..0x4b793d]) adds
        // the same term again.
        if (inf.player_moving) {
            const double angle =
                    static_cast<double>(static_cast<int16_t>(e.heading >> 16)) *
                            9.587371826171875e-05 +
                    static_cast<double>(inf.player_move_dir_index) * 0.7853975;
            const int32_t cx = static_cast<int32_t>(std::cos(angle) * -64.0);
            const int32_t sy = static_cast<int32_t>(std::sin(angle) * -64.0);
            inf.vel[0] -= cx;
            inf.vel[1] -= sy;
            if ((tick_flags & kEntityFlagParachute) != 0 && inf.vel[2] <= -0x3800 &&
                    inf.player_move_dir_index == 0) {
                inf.vel[0] -= cx;
                inf.vel[1] -= sy;
            }
        }
        inf.vel[0] = (63 * inf.vel[0]) >> 6;
        inf.vel[1] = (63 * inf.vel[1]) >> 6;
    } else {
        inf.vel[0] = damp_npc_slide(inf.vel[0]);
        inf.vel[1] = damp_npc_slide(inf.vel[1]);
    }

    // Local player: entity Yaw/Pitch come STRAIGHT from the mouse — instant, no
    // body-turn smoothing. The original drives entity+0x10/+0x14 directly from input;
    // the slope pass only writes +0x14 for corpses (the slope lean lives in
    // body_pitch/roll). [orig: Input_HandleActionBinding_0 @0x4e1330; net-re
    // section 5.38]
    if (inf.is_local_player) {
        e.pitch = inf.look_pitch;
    }

    if (!inf.is_local_player && infantry_attachment_move(e, world, attachment)) {
        finish_infantry_tick(e, world);
        return;
    }

    // 7-8. Rotate the root delta into world axes and integrate. [orig: full-precision
    // sin/cos at 2^22; org1 pos += rotated + velocity @0x4bf684-0x4bf6a2 (the
    // drowning-0x8000/ladder-0x100000 zeroing @0x4bf667-0x4bf680 rides those
    // slices); org2 identical 1× @0x4b7cbf-0x4b7cd9 — its 2× local-player branch
    // @0x4b7c8d-0x4b7cb7 is gated on g_localPlayerPoofMode, the "!Poof!" ghost-mode
    // toggle (@0x42d450), NOT normal play, and stays unported:
    // docs/world/world-wac-ai-re.md (D-INF-21).]
    // The rotated deltas outlive the block: the org2 jump/fall edges carry 3/4 of
    // this tick's step into the slide velocity [orig: @0x4b7d97 keeps them live].
    int32_t root_wx = 0, root_wy = 0;
    {
        int32_t fwd = terrain_slide ? gradient_dx : frame.dx;
        int32_t lat = terrain_slide ? gradient_dy : frame.dy;
        // Root TRANSLATION is integrated for EVERY state, not just movement states. The original
        // advances the playing clip ONCE per tick (AnimMap_UpdateEntity @0x40b5f0) and integrates
        // the root delta unconditionally: the g_animStateFlagsTable bit0 flag gates the anim COMMIT rules
        // (@0x4bd85c) and the leg replant path (@0x4be95f), NOT the position integration. Idle
        // clips author a small mean-~0 root velocity — the bored weight-shift / "rock on the feet".
        // Integrating it sways the entity's centre of mass under the swaying skeleton, so the FEET
        // stay PLANTED (they pivot). Gating it (the prior D-INF-8 reading) held the body rigid while
        // the skeletal FK slid the feet — the "idle skating" this overturns. capsule bottom/top and
        // vel are MOVEMENT data only; the visual is the skeleton, which never reads them.
        // [orig: Entity_UpdateInfantryAI @0x4b9910 integrates root delta for all states; overturns D-INF-8]
        if (inf.is_local_player && inf.airborne) {
            // org2 zeroes the rotated ROOT while airborne — an in-air player
            // body moves on the slide triplet alone [orig: the airborne arm
            // zeroes channelData before the integrate, between @0x4b78ab and
            // @0x4b7949 (kong 194029-194030)]. The kJumpLoop root force below
            // is org1-witnessed and does not apply to the player body.
            fwd = 0;
            lat = 0;
        } else if (inf.anim_state == anim_state::kJumpLoop) {
            fwd = 1024; // [data: retail ADM dump root row 4756]
        }
        // Both motors rotate locomotion by BODY heading. Independent gaze must
        // not steer a walking actor. [orig: org1 entity+0x8C @0x4BF001;
        // org2 entity+0x8C @0x4B41E4, Q22 rotation @0x4B41F0..0x4B4255]
        const int32_t move_heading = inf.is_local_player ? player_root_heading : inf.body_heading;
        const double rad =
            static_cast<double>(move_heading) * io::kRadiansPerBam;
        const int32_t c = static_cast<int32_t>(std::cos(rad) * io::kQ22One);
        const int32_t s = static_cast<int32_t>(std::sin(rad) * io::kQ22One);
        int32_t wx = static_cast<int32_t>((static_cast<int64_t>(fwd) * c) >> 22) -
                     static_cast<int32_t>((static_cast<int64_t>(lat) * s) >> 22);
        int32_t wy = static_cast<int32_t>((static_cast<int64_t>(fwd) * s) >> 22) +
                     static_cast<int32_t>((static_cast<int64_t>(lat) * c) >> 22);
        // Org2 replaces the already-rotated world pair after slide decay.
        // It does not write the NPC path-state byte. [orig: @0x4B7A64..0x4B7A93]
        if (inf.is_local_player)
            infantry_terrain_motion(e, tick_entity, gradient_field, false, wx, wy);
        int32_t dz = frame.dz;
        // Root suppression: drowning zeroes the vertical lane, a ladder latch
        // zeroes the horizontal pair — on a ladder the clip's vertical lane IS
        // the climb motion while the body stays pinned to the anchor chase.
        // [orig: org2 @ 0x4b7ab5-0x4b7ac4; org1 @ 0x4bf667-0x4bf680 — same masks]
        const uint32_t integrate_flags = tick_entity != nullptr
                ? (tick_entity->flags | tick_entity->engine_flags)
                : 0u;
        if ((integrate_flags & kEntityFlagDrowning) != 0) dz = 0;
        if ((integrate_flags & kEntityFlagLadderContact) != 0) {
            wx = 0;
            wy = 0;
        }
        e.pos[0] += wx + inf.vel[0];
        e.pos[1] += wy + inf.vel[1];
        e.pos[2] += dz;
        root_wx = wx;
        root_wy = wy;
        inf.dbg_root_dx = wx + inf.vel[0]; // debug-card tap (frozen-clump instrument)
        inf.dbg_root_dy = wy + inf.vel[1];
    }

    // THE ORG1 ODD KEY TICK. Retail's NPC body runs gravity, the resolver, the
    // airborne/landing edges, the ladder block and the water block on even key
    // ticks only; an odd tick jumps straight to the tail and re-applies the
    // quarter step the last even tick stored, so the body keeps moving between
    // resolves. [orig: Entity_UpdateInfantryAI outYaw = key & 1 @0x4BF146 /
    // @0x4BF14F, the skip `cmp [outYaw],0; jnz loc_4BFC80` @0x4BF6A5..0x4BF6B2,
    // the re-apply @0x4BFC80..0x4BFC86]
    if (!inf.is_local_player && (key & 1u) != 0) {
        e.pos[2] = io::bam_add(e.pos[2], inf.z_quarter_step);
        finish_infantry_tick(e, world);
        return;
    }
    // The even tick's base: Z after the root integrate, before gravity; a
    // landing snap re-saves it below. [orig: @0x4BF6BA]
    int32_t org1_saved_z = e.pos[2];

    // 9. Vertical resolve. The original caller passes entityRadius = AnimMap bottom
    // (out[3]) and receives foot clearance from the collision resolver.
    // It lifts only on return <= 0; return > 0xF000 marks airborne; small positive
    // clearance is left as-is. [orig: Entity_UpdateInfantryAI @0x4b9910 and
    // Entity_UpdateInfantryPlayerBody @0x4b40e0 callers; resolver @0x4b2bd0]
    if (terrain != nullptr && inf.ground_cache_valid && inf.ground_cache != INT32_MIN) {
        // Gravity, per tick, asymmetric by motor (D-INF-10 CLOSED for both legs).
        // NPC org1: vel_z -= 416 then pos.z += 2*vel [orig: 0x108000 gate
        // @0x4bf7b8 (modeled below), step @0x4bf7bf, clamp @0x4bf7c9, pos
        // @0x4bf7ec]. Player org2: vel_z -= 208 then pos.z += vel
        // once [orig: gate @0x4b7ac8, step @0x4b7acf, clamp @0x4b7c77, pos @0x4b7cef
        // — folded into the root-dz store there; split here like org1's shape].
        // The gravity skip while drowning or latched on a ladder (Flags
        // 0x108000) — the ladder body's vertical state is owned by the climb
        // block / the CL chase, not the fall column.
        // [orig: org2 gate @ 0x4b7acd; org1 gate @ 0x4bf7bd]
        const uint32_t gravity_flags = tick_entity != nullptr
                ? (tick_entity->flags | tick_entity->engine_flags)
                : 0u;
        const bool gravity_skip =
            (gravity_flags & (kEntityFlagLadderContact | kEntityFlagDrowning)) != 0;
        // An org1 body reaches this block on EVEN key ticks only (the odd tick
        // returned above), so its `pos += 2 * vel` runs every second tick. The
        // quarter-step tail at the end of the block then keeps a quarter of the
        // even tick's Z change and re-applies it on the odd tick, so a falling
        // NPC moves about vel_z per two ticks, not 2 * vel_z.
        // [orig: Entity_UpdateInfantryAI skip @0x4BF6B2; integrate @0x4BF7EC;
        //  tail @0x4BFC65..0x4BFC86]
        // The org2 (local player) leg keeps its own cadence: it integrates
        // `pos += vel` once per tick and has no quarter-step tail.
        if (inf.is_local_player) {
            if (!gravity_skip) inf.vel[2] -= kGravityStepPlayer;
            if (tick_entity != nullptr) {
                uint32_t flags = tick_entity->flags | tick_entity->engine_flags;
                const ParachuteEvents events = parachute_tick(inf.parachute, flags,
                        tick_entity->carry_flags, inf.vel[2], is_authority, logic_tick);
                tick_entity->flags = (tick_entity->flags & ~kEntityFlagParachute) |
                        (flags & kEntityFlagParachute);
                tick_entity->engine_flags = (tick_entity->engine_flags & ~kEntityFlagParachute) |
                        (flags & kEntityFlagParachute);
                if ((flags & kEntityFlagParachute) != 0 && inf.anim_state != anim_state::kParachute)
                    inf.request_body_animation(anim_state::kParachute);
                if (events.opened) emit_slot_sound(world, e, audio::kSlotChuteOpen, e.pos);
                if (events.closed) emit_slot_sound(world, e, audio::kSlotChuteClose, e.pos);
                if (events.flap) emit_slot_sound(world, e, audio::kSlotChuteFlap, e.pos);
                if (events.free_fall) emit_slot_sound(world, e, audio::kSlotFreeFall, e.pos);
            } else {
                inf.vel[2] = std::max(inf.vel[2], kTerminalVelZ);
                if ((logic_tick & 63u) == 0 && inf.vel[2] < -0x3000)
                    emit_slot_sound(world, e, audio::kSlotFreeFall, e.pos);
            }
            e.pos[2] += inf.vel[2];
        } else if ((gravity_flags & kEntityFlagAiClimb) != 0) {
            // The org1 ladder-climb chase replaces gravity: sixteenth-step Z
            // toward the AI move target, capped 0x4000 up, floor -16384 (half
            // the fall terminal), on the even key tick like the gravity it
            // replaces. The order writer rides the AI-order slice.
            // [orig: @ 0x4bf6d2-0x4bf6e5; floor pick @ 0x4bf6e5 + clamp @ 0x4bf7d4]
            int32_t step = (inf.move_target[2] - e.pos[2] + 8) >> 4;
            if (step > 0x4000) step = 0x4000;
            inf.vel[2] = step;
            if (inf.vel[2] < -16384) inf.vel[2] = -16384;
            e.pos[2] += 2 * inf.vel[2];
        } else {
            // [orig: Entity_UpdateInfantryAI gate @0x4BF7B8, step @0x4BF7BF,
            //  floor @0x4BF7C9..0x4BF7D6, `pos.z += 2 * slideDecay` @0x4BF7E4..0x4BF7EE]
            if (!gravity_skip) inf.vel[2] -= kGravityStep;
            if (inf.vel[2] < kTerminalVelZ) inf.vel[2] = kTerminalVelZ;
            e.pos[2] += 2 * inf.vel[2];
        }

        // Foot clearance: with a collision world wired this is the full resolver —
        // candidate contact forces (CB wall push-out plus hurt/CL/CA/BB triggers)
        // + person repulsion + the ground probe THROUGH candidate models
        // (standing on buildings) [orig: collision resolver
        // @0x4b2bd0; burns down D-INF-3's terrain-only stand-in]. Without one, the
        // terrain-cache clearance stands (headless tests, no placed objects).
        const int32_t pre_resolve_x = e.pos[0]; // debug-card tap
        const int32_t pre_resolve_y = e.pos[1];
        int32_t foot_clearance;
        if (collision != nullptr && collision->instance_count() != 0) {
            // The climb-motor channels the resolver's CL legs read and write:
            // the entry gate, the per-tick alignment chase, and the exit push /
            // pitch restore (infantry_ladder.cpp). [orig: the resolver reads
            // the same entity fields inline @ 0x4b3245-0x4b3495 /
            // @ 0x4b3c5c-0x4b3d55]
            const int32_t eye_offset[3] = {inf.eye_offset_x, inf.eye_offset_y,
                                           inf.eye_offset_z}; // [orig: +0x74, see above]
            const LadderResolveIO lio = make_ladder_resolve_io(e, tick_start_z);
            const devtools::ProfileScope collision_scope(
                    world.profile, devtools::Slot::SIM_AI_INFANTRY_COLLISION);
            foot_clearance = collision->resolve_entity(
                world, e.handle, e.collide_state, e.pos, inf.vel, inf.vel[2],
                frame.capsule_bottom, frame.capsule_top, e.heading, e.pitch,
                entity_is_player_class(world, e.handle), is_authority, logic_tick,
                inf.anim_state, infantry_anim_flags(inf.anim_state), e.health,
                nullptr, &lio, eye_offset, world.profile);
            // The ladder legs may have written the view channels (the yaw
            // chase, the pitch restore); refresh the mouse-instant mirrors so
            // the render/aim pose and the embedder write-back see them.
            if (inf.is_local_player) {
                e.heading = inf.target_heading;
                e.pitch = inf.look_pitch;
            }
        } else {
            foot_clearance = e.pos[2] - frame.capsule_bottom - inf.ground_cache;
            // No probe ran this tick; the probe's +0x28 store is unconditional
            // (null on a miss), and this fallback IS the probe over an empty
            // candidate set — clear the link so the footstep pick cannot read
            // a stale platform. [orig: the unconditional store in
            // Entity_RaycastGroundHeightAndObject @ 0x414370]
            if (Entity *self = world.registry.get(e.handle)) self->ground_target = EntityHandle{};
        }
        inf.dbg_res_dx = e.pos[0] - pre_resolve_x; // debug-card tap
        inf.dbg_res_dy = e.pos[1] - pre_resolve_y;
        inf.dbg_contact_item =
            collision != nullptr ? collision->dbg_last_contact_item : 0;
        // The post-resolve latch state: the resolver cleared and possibly
        // re-latched the CL bit this tick; every leg below keys on the live value.
        const bool on_ladder_now =
            tick_entity != nullptr &&
            ((tick_entity->flags | tick_entity->engine_flags) &
             kEntityFlagLadderContact) != 0;
        // The airborne / landing edges belong to retail's even-tick block, so
        // an odd NPC tick (returned above) evaluates neither.
        // [orig: Entity_UpdateInfantryAI landing @0x4BF7FA, airborne @0x4BF8AE]
        if (foot_clearance > kInfantryAirborneGap) {
            // org2 includes DEAD in the gate that owns the airborne-bit write;
            // a dead player that was not already airborne stays that way. org1's
            // corresponding gate omits DEAD and sets airborne before its later
            // dead/carried animation gates. A ladder latch OR drowning
            // suppresses the edge AND the airborne set for both motors (the
            // masks' 0x108000 half) — mid-climb clearance is always deep.
            // [orig: org2 test 0x10A002 @0x4b7e22-0x4b7e3c; org1 test 0x10A000
            // + write @0x4bf8b5-0x4bf8cf]
            const bool fall_edge_allowed =
                (!inf.is_local_player || e.health > 0) && !on_ladder_now &&
                (tick_entity == nullptr ||
                 ((tick_entity->flags | tick_entity->engine_flags) &
                  kEntityFlagDrowning) == 0);
            if (fall_edge_allowed && !inf.airborne) {
                // The airborne EDGE (was grounded; the already-in-air 0x2000 test
                // skips it). The two motors differ in kind here:
                //   org2 (player): dead skips the WHOLE edge (gate mask 0x10A002,
                //   carried is force-cleared, not skipped); otherwise 3/4 of this
                //   tick's rotated root step carries into the slide velocity —
                //   running momentum off a ledge — pending clears, and 31 stamps
                //   STRAIGHT (47 while parachuting follows Flags 0x20;
                //   the has_clip guard is a reimpl guard the original lacks).
                //   [orig: @0x4b7e17-0x4b7e73]
                //   org1 (NPC): NO carry, and NO stamp on a plain fall — the 47/31
                //   availability ladder runs ONLY while parachuting (`test al,20h`
                //   @0x4bf8d8), so a live NPC keeps its walk/run clip off a ledge;
                //   live non-carried bodies just clear any pending anim (dead skips
                //   the clear too, airborne still sets). [orig: @0x4bf8ae-0x4bf901]
                if (inf.is_local_player) {
                    inf.vel[0] += (3 * root_wx) >> 2;
                    inf.vel[1] += (3 * root_wy) >> 2;
                    inf.anim_pending = 0; // [orig: @0x4b7e46, before the stamp]
                    const int falling_state = tick_entity != nullptr &&
                            ((tick_entity->flags | tick_entity->engine_flags) & kEntityFlagParachute) != 0
                            ? anim_state::kParachute : anim_state::kJumpLoop;
                    if (inf.anim_state != falling_state) inf.request_body_animation(falling_state);
                } else if (org1_body ? !org1_dead_now() : e.health > 0) { // [orig: `test al,2` @0x4BF8CD]
                    if (tick_entity && ((tick_entity->flags | tick_entity->engine_flags) & kEntityFlagParachute)) {
                        const int falling_state = !root_motion || root_motion->has_clip(inf.adm_id, anim_state::kParachute)
                                ? anim_state::kParachute : anim_state::kJumpLoop;
                        inf.request_body_animation(falling_state);
                    }
                    inf.anim_pending = 0; // [orig: @0x4bf901]
                }
            }
            if (fall_edge_allowed) {
                // The airborne set lands in the ONE retail Flags word; the
                // motor keeps its own copy for rowless bodies. org2's edge
                // store is `and eax,0FFFFFFBFh; or eax,2000h; mov [esi+24h],eax`
                // -- the same write drops the carried bit (0x40); org1's is
                // the bare `or eax,2000h`. Both stores sit behind a gate that
                // includes 0x2000 itself (0x10A002 / 0x10A000), so the carried
                // clear is an EDGE write; the re-sync below is the idempotent
                // half. Only the flag bit falls on the org2 edge: retail's
                // parentEntity (+0x16C) seat link -- Entity::mounted and the
                // mount trio here -- is cleared by Entity_DetachFromVehicle
                // alone [orig: Entity_DetachFromVehicle @0x4355f0, the +0x16C
                // clear @0x435915 (after the 0x40 clear @0x435910; the
                // attachBoneId @0x43591b and parentSlot @0x435921 follow)], so
                // `mounted` stays put exactly as the link does there.
                // [orig: org2 @0x4b7e34..0x4b7e3c; org1 @0x4bf8c8..0x4bf8cf]
                if (inf.is_local_player && !inf.airborne && tick_entity != nullptr) {
                    tick_entity->flags =
                            (tick_entity->flags & ~kEntityFlagMounted) | kEntityFlagInAir;
                    tick_entity->engine_flags =
                            (tick_entity->engine_flags & ~kEntityFlagMounted) |
                            kEntityFlagInAir;
                }
                inf.airborne = true;
                if (tick_entity != nullptr) {
                    tick_entity->flags |= kEntityFlagInAir;
                    tick_entity->engine_flags |= kEntityFlagInAir;
                }
            }
        } else if (foot_clearance <= 0) {
            // Landing. Both motors snap FIRST [orig: org2 `sub [esi+0Ch],eax`
            // @0x4b7d0a; org1 @0x4bf802], so the thump below sounds at the
            // snapped origin. Fall damage is AUTHORITY-only and skips Indestructible
            // (0x4000000) bodies on both legs [orig: org1 @0x4bf81e `is_authority`,
            // @0x4bf826 `Flags & 0x4000000`; org2 @0x4b7d3b, @0x4b7d43] -- a
            // joiner's own body runs this motor branch locally and must not
            // self-damage on top of the authority's damage. The rest of the gate
            // is per motor. org1 runs its whole landing arm behind the airborne
            // word [orig: `test edx,2000h; jz` @0x4bf812] and skips DEAD bodies
            // [orig: `test dl,2` @0x4bf843 -- without it a hard-landing corpse
            // would round its health back toward 0 through the clamp]. org2 tests
            // the threshold ALONE [orig: @0x4b7d0d..0x4b7d21]: no 0x2000 and no
            // dead test; its local-player damage feedback,
            // Player_OnDamageReceived @0x4b7d2b..0x4b7d2d, fires AHEAD of the
            // authority test and of the Indestructible test, so a joiner's own
            // body flashes on a hard landing it never charges (ported below). A
            // damaging landing also stages the fall death-anim selection (+0x2C0,
            // cause 4 -> 174) [orig: org1 @0x4bf85d-0x4bf879, org2
            // @0x4b7d6f-0x4b7d8b -- the staged selector matches our generic-death
            // fallback]. There is NO zero test on fallmps: a tolerance of 0
            // makes every landing (vel_z <= 0) damaging by (-vel_z) >> 4
            // [orig: org1 @0x4bf82e..0x4bf841 `imul eax, -1057; cmp ecx, eax;
            // jg skip`; org2 @0x4b7d0d..0x4b7d21].
            e.pos[2] -= foot_clearance;
            // org1 re-bases its quarter-step tail on the snapped Z, so a landing
            // lands in full on this tick. [orig: `mov [esp+117Ch+entity], edx`
            // @0x4BF808]
            if (!inf.is_local_player) org1_saved_z = e.pos[2];
            const int32_t fall_threshold = -1057 * world.script.wac_values.fallmps;
            const bool fall_charges = inf.is_local_player
                    ? inf.vel[2] <= fall_threshold
                    : inf.airborne && (org1_body ? !org1_dead_now() : e.health > 0) &&
                      inf.vel[2] <= fall_threshold;
            // org2's local-player damage feedback: red vignette + camera shake,
            // between the threshold test and the authority/Indestructible tests
            // [orig: @0x4b7d23..0x4b7d2d -> Player_OnDamageReceived @0x4dd880].
            // (org1's own arm @0x4b61e8 sits on ITS death leg instead -- that
            // motor's health-adjust block, health <= 0 and not already dead --
            // and this unified motor has no separate org1 death leg to hang it
            // on, so it stays unported; world-wac-ai-re carries the note.)
            if (inf.is_local_player && fall_charges) player_on_damage_received(world);
            if (fall_charges && is_authority &&
                (tick_flags & kEntityFlagIndestructible) == 0) {
                int32_t excess = fall_threshold - inf.vel[2];
                int32_t dmg = excess >> 4;
                if (dmg > e.health) dmg = e.health;
                e.health = static_cast<int16_t>(e.health - dmg);
                // The charge credits the body itself and stages the generic
                // death selection, so a fatal fall reaches the death edge with
                // a staged +0x2C0 and lastAttacker = self. [orig: org1
                // `mov [esi+178h],esi` @0x4BF86B, +0x2C0 @0x4BF879; org2
                // @0x4B7D7D / @0x4B7D8B]
                if (tick_entity != nullptr) {
                    tick_entity->last_attacker = e.handle;
                    tick_entity->death_anim_state =
                            compute_death_anim_state(0, 0, death_cause::kGeneric);
                }
            }
            if (inf.is_local_player) {
                // org2's arm ends at vel_z = 0 [orig: @0x4b7d91] with NO 0x2000
                // test and NO clear: the word is re-read whole @0x4b7d9f, the
                // jump gates @0x4b7ea4 still see the bit on the landing tick,
                // and the SSFall thump + the clear ride the post-jump else-leg
                // below [orig: @0x4b7f71..0x4b7fa1] -- a jump key held on the
                // landing tick launches on the NEXT one.
                inf.vel[2] = 0;
            } else {
                // org1: the thump on the airborne-clear edge, dead bodies included
                // (a corpse thrown airborne lands with SSFallDead): profile slot 16
                // SSFallAlive, 15 SSFallDead when dead, at the entity origin; then
                // the clear; then vel_z = 0.
                // [orig: @0x4bf87f-0x4bf89c (Flags&2 pick) -> `and [esi+24h],
                // 0FFFFDFFFh` @0x4bf89f -> `mov [esi+0A0h],ebx` @0x4bf8a6]
                if (inf.airborne)
                    emit_slot_sound(world, e,
                                    (org1_body ? !org1_dead_now() : e.health > 0)
                                            ? audio::kSlotFallAlive
                                            : audio::kSlotFallDead,
                                    e.pos);
                inf.airborne = false;
                if (tick_entity != nullptr) {
                    tick_entity->flags &= ~kEntityFlagInAir;
                    tick_entity->engine_flags &= ~kEntityFlagInAir;
                }
                inf.vel[2] = 0;
            }
        }

        // 9b. Player jump — witnessed org2 order: integrate -> resolver -> edges ->
        // the jump block [orig: @0x4b7de0-0x4b7f0c] -> the not-jumping tail (the
        // landing thump + clear, the ladder bottom dismount) [orig:
        // @0x4b7f71-0x4b8019]. The cooldown lives in the
        // REUSED +0x1A8 field there (org1's targetHeading slot): clamp [0,32], >1
        // counts down, held-at-1 until the key releases (no auto-repeat while held),
        // jump only from 0 [orig: maintenance @0x4b7de0-0x4b7e15, release edge
        // @0x4b7e78-0x4b7e82]. Gates [orig: @0x4b7e8c-0x4b7ebd]: cooldown 0, not
        // prone (the selection's prone local @0x4B417D/@0x4B4183), !(Flags & 0x1A002) — in-air, dead,
        // and the water pair (unmodeled) — the key held (MoveOrder bit 5), not
        // carried (0x40). The flag carriers are modeled even though the swimming
        // transition producer remains D-INF-3. The impulse: 3/4 of the rotated root step into
        // the slide velocity, vel_z = 0x1600, in-air set, anim 30 jump_start now
        // with 31 jump_loop queued, cooldown reloaded to 32; an on-ladder jump
        // adds the 0.5u back-push + unlatch [orig: @0x4b7f0c-0x4b7f68].
        if (inf.is_local_player) {
            if (inf.jump_cooldown < 0) inf.jump_cooldown = 0;   // [orig: @0x4b7de0]
            if (inf.jump_cooldown > 32) inf.jump_cooldown = 32; // [orig: @0x4b7dee]
            if (inf.jump_cooldown > 1) {
                --inf.jump_cooldown;                            // [orig: @0x4b7e0c]
            } else if (inf.jump_cooldown == 1 && !inf.jump_requested) {
                inf.jump_cooldown = 0;                          // [orig: @0x4b7e7a-0x4b7e82]
            }
            // The four gates [orig: @0x4b7e8c..0x4b7eb5] fail INTO the landing
            // else-leg; the carried test past them (`test al,40h; jnz 0x4b8020`
            // @0x4b7ebb) skips the whole tail instead.
            // The prone gate reads prone_local (the latch with none of Flags
            // 0x10A000), so a prone-latched body on a ladder can still jump off
            // [orig: prone_local @0x4b416c..0x4b4183; the jump gate @0x4b7e99].
            const uint32_t jump_flags = tick_entity != nullptr
                    ? (tick_entity->flags | tick_entity->engine_flags)
                    : 0u;
            const bool prone_effective =
                    inf.stance == InfantryState::Stance::kProne &&
                    (jump_flags & (kEntityFlagInAir | kEntityFlagDrowning |
                                   kEntityFlagLadderContact)) == 0;
            const bool jump_gates_open =
                inf.jump_cooldown == 0 && inf.jump_requested &&
                !player_jump_flags_blocked(inf, tick_entity) && e.health > 0 &&
                !prone_effective;
            if (jump_gates_open && !player_jump_carried(tick_entity)) {
                inf.vel[0] += (3 * root_wx) >> 2; // [orig: @0x4b7ec3-0x4b7ed5]
                inf.vel[1] += (3 * root_wy) >> 2;
                inf.vel[2] = kJumpImpulseVelZ;    // [orig: @0x4b7ee5]
                // Flags |= 0x2000 [orig: @0x4b7edb; the store @0x4b7eef]
                inf.airborne = true;
                if (tick_entity != nullptr) {
                    tick_entity->flags |= kEntityFlagInAir;
                    tick_entity->engine_flags |= kEntityFlagInAir;
                }
                inf.jump_cooldown = 32;           // [orig: @0x4b7f06]
                // STRAIGHT stamps — the org2 jump block has NO clip
                // availability check (world-wac-ai-re jump witness: "anim 30
                // jump_start NOW + 31 jump_loop PENDING, no availability
                // check") [orig: @0x4b7ef2 / @0x4b7efc].
                inf.request_body_animation(anim_state::kJumpStart);
                inf.anim_pending = anim_state::kJumpLoop;
                if (on_ladder_now) {
                    // Jumping off the ladder: the 0.5u back-push + unlatch ride
                    // the jump commit. [orig: @ 0x4b7f0c-0x4b7f68 + the shared
                    // tail @ 0x4b8016-0x4b8019]
                    ladder_push_back(e.pos, inf.body_heading);
                    ladder_unlatch(tick_entity);
                }
            }
            inf.jump_requested = false;
            // The not-jumping tail [orig: @0x4b7f71..0x4b8019]: a gate failed and
            // the resolver clearance is <= 0 (the clearance local's `jg 0x4b8020`
            // @0x4b7f71..0x4b7f76).
            if (!jump_gates_open && foot_clearance <= 0) {
                // The landing on the word's OWN bit: retail re-tests the Flags
                // read @0x4b7d9f, so a 0x2000 the deploy left behind (the death
                // and reset writers touch bits 0/1 only: the deploy leg
                // @0x519fdb, Server_ProcessPlayerDeath @0x51787a,
                // Entity_ResetToSpawnState @0x4b97b0; the spawn placer only ORs
                // 0x20/0x200 @0x50d42a/@0x50d44d) lands here on the first
                // grounded tick with the same thump, one tick of refused jump
                // included. Rowless bodies keep the motor's copy. The SSFall
                // pair: slot 16 SSFallAlive, 15 SSFallDead when dead (`test
                // al,2`), at the entity origin; then Flags &= ~0x2000.
                // [orig: @0x4b7f7c..0x4b7f9e; the clear @0x4b7fa1]
                const bool landed_from_air = tick_entity != nullptr
                        ? ((tick_entity->flags | tick_entity->engine_flags) &
                           kEntityFlagInAir) != 0
                        : inf.airborne;
                if (landed_from_air) {
                    emit_slot_sound(world, e,
                                    e.health > 0 ? audio::kSlotFallAlive
                                                 : audio::kSlotFallDead,
                                    e.pos);
                    inf.airborne = false;
                    if (tick_entity != nullptr) {
                        tick_entity->flags &= ~kEntityFlagInAir;
                        tick_entity->engine_flags &= ~kEntityFlagInAir;
                    }
                }
                // The grounded bottom dismount: standing on ground below the
                // anchor steps the climber 0.5u back off the face and drops the
                // latch -- how climbing down ends. The entry's stance Z-bump
                // exists exactly so a fresh mount is not instantly grounded here.
                // [orig: @0x4b7fa8-0x4b8019 -- latched (0x100000) and
                //  g_LadderContactZ > pos.z]
                if (on_ladder_now && collision != nullptr &&
                    collision->last_ladder_frame.anchor[2] > e.pos[2]) {
                    ladder_push_back(e.pos, inf.body_heading);
                    ladder_unlatch(tick_entity);
                }
            }
        }

        // org1 on-ladder (NPC, post-resolve): the congestion hold, the facing
        // press, the climb_up/climb_top select. Body in infantry_ladder.cpp.
        // [orig: Entity_UpdateInfantryAI @ 0x4bf907-0x4bfad8]
        infantry_ladder_org1_block(e, world, tick_entity);

        // org1 water (NPC, immediately after the ladder block, same as retail):
        // float on the plane instead of walking along the riverbed, and fan the
        // splash once on entry.
        if (!inf.is_local_player) {
            infantry_water_block(e, world, tick_entity, frame.capsule_bottom, logic_tick);
            // The org1 quarter-step tail: of everything gravity, the resolver,
            // the ladder and the water blocks did to Z since the save, keep a
            // quarter now and store it for the odd tick's re-apply.
            // [orig: Entity_UpdateInfantryAI @0x4BFC65..0x4BFC7D `sub ecx,eax;
            //  add ecx,2; sar ecx,2; mov [esi+0ACh],ecx; mov [esi+0Ch],eax`,
            //  then the shared add @0x4BFC80..0x4BFC86]
            inf.z_quarter_step = io::bam_sar(
                    io::bam_add(io::bam_sub(e.pos[2], org1_saved_z), 2), 2);
            e.pos[2] = io::bam_add(org1_saved_z, inf.z_quarter_step);
        }
        // org2 water (the LOCAL player body, immediately after its jump block --
        // the same mover-tail placement retail gives it @0x4b8020).
        else
            player_water_block(e, world, tick_entity, frame.capsule_bottom, is_authority,
                               logic_tick);
    }

    finish_infantry_tick(e, world);
}

void AiSystem::finish_infantry_tick(AiEntity &e, World &world) {
    InfantryState &inf = e.inf;
    // Mirror the mover-output brain fields the present snapshot reads (kWorkPosZ stays the
    // grounded Z; kOutSpeed reflects motion for anim-slot consumers).
    e.brain.f[AiBrain::kWorkPosX] = e.pos[0];
    e.brain.f[AiBrain::kWorkPosY] = e.pos[1];
    e.brain.f[AiBrain::kWorkPosZ] = e.pos[2];
    e.brain.f[AiBrain::kWorkHeading] = e.heading;

    // Two-store reconciliation: the motor advances AiEntity.pos/heading (16.16 / BAM32), but
    // the S2C 0x0A snapshot SERIALIZES the registry Entity (snapshot_of reads Entity.position).
    // Mirror EVERY motor entity's grounded pose back so the wire — and the listen-server
    // present that decodes it — carry the current grounded position, not the stale authored
    // spawn Z (which sank AI organics into the terrain on the wire path). Faithful: the
    // original engine has ONE Entity Position store that is both mover output and serializer
    // input; our AiEntity.pos vs registry Entity.position split is the tracked deviation, so
    // we keep them in sync here. (The direct AI-pool present still reads AiEntity.pos.)
    if (Entity *ent = world.registry.get(e.handle)) {
        ent->position.x = static_cast<float>(from_fixed(e.pos[0]));
        ent->position.y = static_cast<float>(from_fixed(e.pos[1]));
        ent->position.z = static_cast<float>(from_fixed(e.pos[2]));
        // Collision and landing damage mutate the motor-side health field. The original
        // has one Entity store; mirror that result into the registry store consumed by
        // scripts, the HUD, and wire snapshots.
        ent->health = e.health;
        // An org1 corpse stays dead on its dead bit even when a script writes
        // health back onto it. [orig: Entity_UpdateInfantryAI corpse leg
        // `test al,2` @0x4B9D58]
        const bool org1_corpse = !inf.is_local_player && !e.net_is_remote_peer &&
                ((ent->flags | ent->engine_flags) & (kEntityFlagPlayer | kEntityFlagDead)) ==
                        kEntityFlagDead;
        ent->alive = e.health > 0 && !org1_corpse;
        // BAM32 engine heading -> mission yaw (int16): mission_yaw = 90 - heading/deg.
        // (The exact yaw round-trip is the Q1 reconciliation handled with the present.)
        ent->yaw = static_cast<int16_t>(
            std::lround(normalize_mission_yaw_deg(mission_yaw_deg_from_bam_heading(e.heading))));
        // Body-anim slot for the present pass. The infantry motor (player AND AI) bypasses the
        // brain-state update_body_anim_slot (the ai.cpp dispatch `continue`s before reaching it),
        // so derive the present-pass BodyAnim slot from the motor's selected clip state here — else
        // every org1 soldier renders a static T-pose (body_anim_slot stays -1 and _apply_body_anim
        // no-ops). Leave the slot on death (the present hides / holds the death pose).
        // [orig: Entity_UpdateInfantryAI @0x4b9910 selects the body anim each tick]
        if (ent->alive && ent->health > 0)
            ent->body_anim_slot = body_anim_slot_from_state(inf.body_clip_state());
    }

    mirror_wire_anim(e, world); // wire-anim bytes for the 0x0A player record (D-NET-159)
}

} // namespace opennova::world
