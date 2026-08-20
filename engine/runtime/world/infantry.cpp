// Infantry motor: per-frame update for AI soldiers (entity class org1).
// [orig: Entity_UpdateInfantryAI @ 0x4b9910]. Spec: docs/world/world-wac-ai-re.md §3.
//
// Locomotion is anim-driven: the 16-tick think picks a movement order, the selector maps
// it to an anim state, and the playing clip's root-motion track (injected through
// IRootMotionSource) moves the entity. Without a source every state is unavailable and
// the soldier stands — exactly the original's relationship between motion and clip data.
//
// Deviations (tracked):
//   D-INF-1  primary-channel blend windows are ported: old/new clips keep independent
//            playheads and their five numeric root lanes blend for 10 ticks (15 when
//            the target state has flag 0x400). The secondary weapon channel still
//            switches immediately.
//   D-INF-2  commands 123/124/125 (move-to-entity orders: staged vehicle boarding via the
//            E1..E8/S/G/H bones with per-soldier entry-slot claims at entity+866, UseGun
//            emplacement manning, seat attach on arrival; dump 1545-2330) and 126
//            (guard/hold) are decoded but not driven by a command source; they idle.
//            127 (follow local player) idles because the simulation has no local player.
//   D-INF-3  the ground/water resolver [orig: collision resolver
//            @0x4b2bd0]: with a CollisionWorld wired (AiSystem::collision) the full
//            resolver runs — wall push-out, standing on objects, hurt/zone volumes,
//            person repulsion, blink/indoors (world/collision.h; witness
//            docs/world/world-wac-ai-re.md §15, deferral tails D-COL-1..8). Without one
//            (headless tests) the terrain-cache clearance stands. Remaining D-INF-3
//            tail: water (swim transitions). The caller semantics are preserved either
//            way: return <= 0 lifts the foot out of the floor, return > 0xF000 marks
//            airborne, small positive clearance is left alone; the airborne edge stamps
//            jump_loop 31 for the PLAYER only (org1's 47/31 ladder is parachute-gated —
//            plain NPC falls keep the clip; the 47 variant rides the unmodeled Flags
//            0x20) [orig: org1 @0x4bf8d4-0x4bf901, org2 @0x4b7e3c-0x4b7e61]. NOTE:
//            patrol walking has NO
//            peer/obstacle avoidance in the original — entity separation is the
//            resolver's push-out, not a steering behavior (dump survey).
//   D-INF-4  CLOSED: the direction table generator is witnessed and ported —
//            Math_BuildSinTable @ 0x613050 builds ONE 1281-entry sin table at 2^22
//            by an accumulating x87 loop (angle += 2pi/1024 per entry, ftol2_sse
//            truncation); the cos read aliases table+256 entries (off_849934 =
//            outMillis + 0x400). See quantized_dir below.
//   D-INF-5  the idle look-at system (every-256-tick interest scan -> head-look + the
//            43->125 / 44->126 look-idle swaps + greeting voice cues; dump 4089-4429) and
//            its spotting side effects (enemy -> combat focus + alert 10, corpse -> alert
//            25) are not ported; alerts currently come from the BMS seed / explicit state.
//            Rides the combat pass with the rest of the targeting layer.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "audio/footstep_slot.h"
#include <io/bam.h>
#include <terrain_query/height_field.h>

#include "world/ai.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/dir_table.h"
#include "world/infantry_ladder.h"
#include "world/player_view.h" // player_view_floor_eye_to_terrain (the on-foot local eye leg)
#include "world/vehicle_attach.h"
#include "world/world.h" // registry.get for the local-player AiEntity->Entity mirror

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
// drowning/water (0x8000), and the second witnessed water-state bit (0x10000).
// Carried (0x40) is tested separately immediately afterward. The reimpl keeps
// flags in two mirrors plus a typed mounted relation, so collapse those carriers
// at the one shared local/remote eligibility seam.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b7ea0-0x4b7ebd]
constexpr uint32_t kPlayerJumpBlockedFlags = 0x1A002u;
bool player_jump_world_state_blocked(const InfantryState &inf, const Entity *ent) {
    if (inf.airborne) return true;
    if (ent == nullptr) return false;
    const uint32_t flags = ent->flags | ent->engine_flags;
    return (flags & kPlayerJumpBlockedFlags) != 0 || ent->mounted ||
           (flags & kEntityFlagMounted) != 0;
}
// Slope-pass constants. org1 (NPC): shifted small-angle slopes clamped +-656175520
// with the fixed 0x22222200 slide threshold [orig: @0x4ba1a8-0x4ba34c]. org2 (player):
// true atan2 slopes over the probe separations (45056 fore-aft / 11264 lateral, 16.16)
// with a 60-deg live / 48-deg dead threshold [orig: @0x4b6e41-0x4b6ff4; dbl_7C9BE8 /
// dbl_7C9BE0; thresholds @0x4b6ee5-0x4b6ef7].
constexpr int32_t kSlopeClamp = 656175520;
constexpr int32_t kSlideThreshold = 572662272;      // 0x22222200 (48 deg); org2 dead
constexpr int32_t kSlideThresholdLive = 715827840;  // 0x2AAAAA80 (60 deg); org2 alive
constexpr double kSlopeAtanFwdBase = 45056.0;       // [orig: dbl_7C9BE8]
constexpr double kSlopeAtanLatBase = 11264.0;       // [orig: dbl_7C9BE0]
// [orig: turn-in-place gates; dump 2940-2952]
constexpr int32_t kTurnStopGate = 536870880;  // > 45 deg -> state 147 (stop)
constexpr int32_t kTurnWalkGate = 357913920;  // > 30 deg -> state 1 (walk turn)
// [orig: BAM bearing scale 683565275.5764316 = 2^31/pi (dbl_7C19D8)]
constexpr double kBamPerRadian = 683565275.5764316;
// [orig: degrees -> BAM32 = 2^32/360 = 11930464; same const as ai.cpp/promote.cpp.
// Used only to mirror the local player's engine heading back to the registry Entity's
// mission yaw — the precise (90 - deg) Q1 reconciliation lives with the present path.]
int32_t abs_bam(int32_t v) { return opennova::io::bam_abs(v); } // x86 neg: INT32_MIN stays put, never UB

bool reset_capsule_bottom_state(int state) {
    return (state >= 32 && state <= 35) || (state >= 176 && state <= 179);
}

bool primary_blend_active(const InfantryState &inf) {
    return inf.body_blend_active();
}

int32_t blend_root_lane(int32_t previous, int32_t current, float current_weight) {
    // The provider has already quantized each channel to the integer RootMotionFrame
    // seam. Retail mixes the underlying float tracks before that conversion, so this
    // fallback can differ at the final integer by a bounded LSB. Keeping every
    // operation float32 still preserves its accumulated-weight behavior and signed
    // truncation rather than replacing it with a rational tick/tick count.
    const float previous_weight = 1.0f - current_weight;
    const float mixed = static_cast<float>(previous) * previous_weight +
                        static_cast<float>(current) * current_weight;
    return static_cast<int32_t>(mixed);
}

void blend_root_frame(const RootMotionFrame &previous, const RootMotionFrame &current,
                      float current_weight, RootMotionFrame &out) {
    out.dx = blend_root_lane(previous.dx, current.dx, current_weight);
    out.dy = blend_root_lane(previous.dy, current.dy, current_weight);
    out.dz = blend_root_lane(previous.dz, current.dz, current_weight);
    out.capsule_bottom =
            blend_root_lane(previous.capsule_bottom, current.capsule_bottom,
                            current_weight);
    out.capsule_top =
            blend_root_lane(previous.capsule_top, current.capsule_top, current_weight);
    // Event triggers are copied from the secondary/current channel, never blended
    // with or ORed against primary. [orig: AnimMap_UpdateEntity @0x40b5f0]
    out.events = current.events;
}

bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             RootMotionFrame &out) {
    out = RootMotionFrame{};

    if (!primary_blend_active(inf))
        return source.advance(inf.adm_id, inf.anim_state, inf.clip_phase, out);

    inf.anim_blend_weight += inf.anim_blend_step;
    if (inf.anim_blend_weight >= 1.0f) {
        inf.anim_blend_weight = 1.0f;
        inf.anim_blend_step = 0.0f;
    }
    return source.advance_blended(inf.adm_id,
                                  inf.anim_prev, inf.anim_prev_clip_phase,
                                  inf.anim_state, inf.clip_phase,
                                  inf.anim_blend_weight, out);
}

void advance_primary_channel_fallback(InfantryState &inf) {
    if (primary_blend_active(inf)) {
        inf.anim_prev_clip_phase = (inf.anim_prev_clip_phase + 1) % 62;
        inf.clip_phase = (inf.clip_phase + 1) % 62;
        inf.anim_blend_weight += inf.anim_blend_step;
        if (inf.anim_blend_weight >= 1.0f) {
            inf.anim_blend_weight = 1.0f;
            inf.anim_blend_step = 0.0f;
        }
        return;
    }
    inf.clip_phase = (inf.clip_phase + 1) % 62;
}

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

int32_t bearing_to(int32_t dx, int32_t dy) {
    return static_cast<int32_t>(std::atan2(static_cast<double>(dy), static_cast<double>(dx)) *
                                kBamPerRadian);
}

} // namespace

bool IRootMotionSource::advance_blended(int adm_id,
                                        int primary_state, int32_t &primary_phase_ticks,
                                        int target_state, int32_t &target_phase_ticks,
                                        float target_weight, RootMotionFrame &out) {
    RootMotionFrame primary;
    RootMotionFrame target;
    const bool have_primary = advance(adm_id, primary_state, primary_phase_ticks, primary);
    const bool have_target = advance(adm_id, target_state, target_phase_ticks, target);

    if (have_primary && have_target) {
        if (target_weight < 1.0f)
            blend_root_frame(primary, target, target_weight, out);
        else
            out = target;
        return true;
    }
    if (have_target) {
        out = target;
        return true;
    }
    if (have_primary) {
        out = primary;
        out.events = target.events;
        return true;
    }
    out = RootMotionFrame{};
    return false;
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

    const NavEntry *mk = nav.entry(nc->entries[node]);
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
        return static_cast<int32_t>(std::sqrt(dx * dx + dy * dy + dz * dz));
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

    bool hold_here = false;
    if (mk->wait_ticks != 0) {
        // Face the marker's authored heading and hold. [orig: dump 1468-1477 —
        // entity[106] = marker+16; entity[74] = (wait + 8) >> 4 think-ticks]
        inf.target_heading = mk->f[4];
        inf.wait_cooldown = (mk->wait_ticks + 8) >> 4;
        hold_here = true;
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
    if (hold_here) return;

    const NavEntry *next = nav.entry(nc->entries[node]);
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
        case anim_state::kWoundedRun:
            if (has(anim_state::kRunForward)) return anim_state::kRunForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kWoundedWalk:
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kStop:
            // [orig: 4084 — a missing stop clip forces idle (147 -> 43), not a gait]
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
    if (has(anim_state::kIdle)) return anim_state::kIdle;
    return -1; // nothing playable: keep the current state
}

// Commit a resolved target state under the flag-table arbitration [orig:
// @0x4b7356-0x4b7396, identical in the org1 selector dump 3693-3710]: an
// uninterruptible current (bit 0x4) queues the target to pending; an exit-gated
// current (0x20) commits only a movement-flagged (bit 0) target; else commit now.
// The channel retarget with the gait->stance transition insert [orig:
// AnimMap_UpdateEntity @0x40b662..0x40b737]: with no deferral armed, a forward
// gait committing to its crouch/prone walk plays the 169-172 transition clip
// first and re-arms the real target as pending for the clip-end promotion,
// gated on the adm actually carrying the clip. The netsim replica channel runs
// the same insert through the shared pair map (D-NET-209 / D-INF-23).
static void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                              const IRootMotionSource *root_motion) {
    if (inf.anim_pending == 0) {
        const int trans = gait_stance_transition_clip(inf.anim_state, resolved);
        if (trans >= 0 && root_motion != nullptr &&
            root_motion->has_clip(inf.adm_id, trans)) {
            inf.begin_body_transition(trans);
            inf.anim_pending = resolved; // deferred to the clip end [orig: @0x40b737]
            return;
        }
    }
    inf.begin_body_transition(resolved);
}

static void commit_body_state(InfantryState &inf, int resolved,
                              const IRootMotionSource *root_motion) {
    if (resolved < 0) return; // no clips at all: hold the current state
    if (resolved == inf.anim_state) { inf.anim_pending = 0; return; }
    const uint32_t curf = infantry_anim_flags(inf.anim_state);
    if ((curf & 0x4u) != 0) {
        inf.anim_pending = resolved;
    } else if ((curf & 0x20u) == 0 || (infantry_anim_flags(resolved) & 0x1u) != 0) {
        begin_body_transition_with_insert(inf, resolved, root_motion);
    } else {
        inf.anim_pending = resolved;
    }
}

void AiSystem::infantry_select(AiEntity &e) {
    InfantryState &inf = e.inf;
    // [orig: alerted = entity[190] || slot byte +136 || combat-reaction byte +875]
    const bool alerted =
        inf.alert_timer != 0 || e.slot.bytes()[AiSlot::kAlertByte] != 0 || inf.combat_reaction;

    int target = anim_state::kIdle; // [orig: targetAnimState seeds 43]
    const bool moving = inf.move_mode != 0 && inf.target_dist > 0;
    if (moving) {
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

    // Turn-in-place overrides. [orig: Entity_UpdateInfantryAI @0x4b9910 dump 2940-2952]
    {
        const int32_t err = abs_bam(opennova::io::bam_sub(inf.target_heading, inf.body_heading));
        if (err > kTurnStopGate) target = anim_state::kStop;
        else if (err > kTurnWalkGate) target = anim_state::kWalkForward;
    }

    // Alerted idle. [orig: dump 3027-3030 — 43 -> 49 with a target else 44; targeting
    // integration is the combat pass, so the no-target variant is used]
    if (!moving && alerted && target == anim_state::kIdle) target = anim_state::kIdle2;

    // Wounded gaits at half max-health. [orig: dump 3090-3151 — def+380 >> 1]
    if (e.health <= static_cast<int16_t>(inf.max_health / 2)) {
        if (target == anim_state::kRunForward || target == anim_state::kJogForward)
            target = anim_state::kWoundedRun;
        else if (target == anim_state::kWalkForward)
            target = anim_state::kWoundedWalk;
    }

    commit_body_state(inf, infantry_resolve_state(inf.adm_id, target), root_motion);
}

// The witnessed org2 player-body selection — see the ai.h declaration. One function
// for the local player AND the authority's remote-player path, exactly as the
// original runs the same @0x4b40e0 body for both. Inputs are the already-deposited
// entity+0x12C mirrors (player_moving / dir index / stance / lean bits) plus the
// per-tick weapon mirrors (scope_raised, wpn_run_anim, wpn_force_crouch).
// [orig: Entity_UpdateInfantryPlayerBody @0x4b7183-0x4b7396]
void AiSystem::player_body_select(AiEntity &e) {
    InfantryState &inf = e.inf;
    auto has = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // No in-air branch here: the original SKIPS this selection while airborne
    // (@0x4b70b8) — the jump block stamps 30/31 and the fall edge stamps 31 (47
    // parachute rides the unmodeled Flags 0x20) directly [orig: @0x4b7ef2/@0x4b7e5c].
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
    // tier = pitch_tier + run_anim; the pitch tier reads entity+0x37C, which has NO
    // writer in the retail image (zero-initialized pool memory), so it contributes
    // the constant 2 (0 <= 0 < 0x210000 band; thresholds recorded in the RE doc,
    // D-INF-16). tier 1 -> run_2 if available; tier >= 2 -> run_3, else run_2.
    // [orig: @0x4b729d-0x4b731b; scope Flags&0x10 test @0x4b72e2]
    if (target == anim_state::kWalkForward && !inf.scope_raised) {
        const int tier = 2 + inf.wpn_run_anim;
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

    commit_body_state(inf, infantry_resolve_state(inf.adm_id, target), root_motion);
}

// The physical recoil accumulator's per-body decay and orientation drift.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0]
void infantry_recoil_tick(InfantryState &inf, int32_t &heading,
                          int32_t &pitch, int32_t random16) {
    // The accumulator yields an eighth-step, then loses half of that step.
    // Pitch receives one eighth of the pre-halved step and yaw receives the
    // half-step with PRNG-selected sign. The caller draws PRNG_Next16 even when
    // recoil is zero. [orig: the entity+0x380 body-update block]
    const int32_t step = io::bam_sar(io::bam_add(inf.recoil_pitch, 4), 3);
    const int32_t half = io::bam_sar(step, 1);
    inf.recoil_pitch = io::bam_sub(inf.recoil_pitch, half);
    if (inf.recoil_pitch <= 0x300) inf.recoil_pitch = 0;
    pitch = io::bam_add(pitch, io::bam_sar(step, 3));
    heading = (random16 & 1) == 0 ? io::bam_add(heading, half)
                                  : io::bam_sub(heading, half);
}

void infantry_weapon_weight_spread_tick(
        InfantryState &inf, const InfantryWeightSpreadInputs &inputs) {
    if (inputs.produce) {
        const int32_t weight = io::bam_add(inputs.weaponweight_fp16,
                                           inputs.clipweight_fp16);
        int32_t increment = 0;
        if (inputs.aimed_shot_available ||
            (inputs.prone && !inputs.drowning)) {
            increment = weight / 3;
        } else if (inputs.crouched && !inputs.drowning) {
            increment = static_cast<int32_t>(
                    static_cast<double>(weight) * 2.0 / 3.0);
        } else {
            increment = static_cast<int32_t>(static_cast<double>(weight) * 1.5);
        }
        inf.weapon_weight_spread =
                io::bam_add(inf.weapon_weight_spread, increment);
        if (inputs.airborne_rising) {
            inf.weapon_weight_spread =
                    io::bam_add(inf.weapon_weight_spread, 0x01000000);
        }
    }

    // Shared decay is after the local producer; remote players and AI jump
    // directly here. There is no upper clamp.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B5945]
    inf.weapon_weight_spread = io::bam_sub(
            inf.weapon_weight_spread,
            io::bam_sar(io::bam_add(inf.weapon_weight_spread, 4), 4));
    if (inf.weapon_weight_spread <= 0x300) inf.weapon_weight_spread = 0;
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
    // The ladder latch blocks the ramp (hands on the rungs); the parachute half
    // of the same mask rides D-INF-20. [orig: (Flags & 0x100020) gate @0x4b7dad]
    if ((entity_flags & (kEntityFlagLadderContact | kEntityFlagParachute)) != 0)
        return;
    if (inf.stance == InfantryState::Stance::kProne) return; // [orig: the prone skip]
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
    inf.body_heading = heading;
    inf.target_heading = heading;
    inf.leg_yaw[0] = inf.leg_yaw[1] = heading;
    inf.leg_target[0] = inf.leg_target[1] = heading;
    inf.vel[0] = inf.vel[1] = inf.vel[2] = 0;
    inf.stance = InfantryState::Stance::kStand;
    inf.airborne = false;
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

// ----------------------------------------------------------------------------
// The slope pass — see the ai.h declaration. Both original updaters carry the same
// three-way conform selector in front of the probes; everything non-conforming
// DECAYS body_pitch/roll back to level, and the slide impulse only exists inside
// the conform branch (a live standing soldier neither slope-leans nor slides).
// [orig: org1 Entity_UpdateInfantryAI @0x4ba10f (selector) -> @0x4ba1a8 (probes) ->
//  @0x4ba320 (chase) / @0x4ba133 (decay), every 8th tick;
//  org2 Entity_UpdateInfantryPlayerBody @0x4b6d95 (selector) -> @0x4b6de4 (tick&1
//  probe gate) -> @0x4b6e41 (probes) -> @0x4b6fc1 (chase) / @0x4b6dbd (decay)]
// Witness + fix log: docs/world/world-wac-ai-re.md §3.5 item 4 (D-INF-19).
// ----------------------------------------------------------------------------
void AiSystem::infantry_slope_pass(AiEntity &e, uint32_t logic_tick, uint32_t key) {
    if (terrain == nullptr) return;
    InfantryState &inf = e.inf;
    // The org1/org2 split is load-bearing: org2 is the PLAYER-BODY updater's leg
    // (in the original it runs for every player-class body; our motor only ever
    // simulates the local one — remote peers net-snap and skip the motor, D-NET-89),
    // org1 is the NPC/AI updater's leg. The selector is witnessed identical in both,
    // but the cadence, slope math, chase rates, thresholds, and slide impulses are
    // NOT interchangeable — never collapse the legs.
    const bool org2 = inf.is_local_player;
    if (!org2 && (key & 7u) != 0) return; // org1 runs on the entity's 8-tick phase

    // Dead + in-air takes the corpse-tumble branch instead of the slope pass in both
    // originals (bodyPitch/roll/yaw spin ramps) — unported; the death-fall mover owns
    // the drop today. [orig: org1 @0x4ba0b2-0x4ba107; org2 @0x4b6ccb-0x4b6d90]
    const bool dead = e.health <= 0;
    if (dead && inf.airborne) return;

    // The conform selector [orig: @0x4ba10f / @0x4b6d95]: entity-def attrib 0x200,
    // an anim state with flag bit 2 (prone crawls 19-26, rolls 41/42, prone idle 48,
    // draggers 137-139), or a grounded corpse. The original's dead leg also requires
    // !(Flags & 0x10A000) — the swim/parachute flag legs, unmodeled here.
    const bool conform = (e.def_attrib & kItemAttribLandable) != 0 ||
                         (infantry_anim_flags(inf.anim_state) & 2u) != 0 || dead;
    if (!conform) {
        // Ease back to level, 1/16-step (org1: every 8th tick; org2: every tick).
        // [orig: @0x4ba133-0x4ba152 / @0x4b6dbd-0x4b6ddc]
        e.body_pitch -= (e.body_pitch + 8) >> 4;
        e.roll -= (e.roll + 8) >> 4;
        return;
    }
    // org2 probes/chases every 2nd tick and HOLDS between (the decay above is the
    // only every-tick leg). [orig: test tickCounter,1 @0x4b6de4]
    if (org2 && (logic_tick & 1u) != 0) return;

    // Probe ground at an offset of the entity. [orig: sub_4142C0 @0x4142c0 — heightmap
    // raycast at (x+dx, y+dy) in a [z+0x4000, z+0x4000-0x20000] window; we sample the
    // height field at the offset position (same surface for terrain)]
    auto probe = [&](int32_t dx, int32_t dy) -> int32_t {
        int32_t p[3] = {e.pos[0] + dx, e.pos[1] + dy, e.pos[2]};
        GroundClearance clearance = ground_clearance;
        clearance.has_physics = e.has_physics;
        clearance.use_dead = dead;
        return calc_average_ground_height(*terrain, p, 0, clearance);
    };

    int32_t c, s;
    quantized_dir(e.heading, c, s);
    // [orig: dir scaled 22528>>22 along heading; perpendicular probes at quarter offset]
    const int32_t fx = static_cast<int32_t>((22528LL * c) >> 22);
    const int32_t fy = static_cast<int32_t>((22528LL * s) >> 22);
    const int32_t h_ahead = probe(fx, fy);
    const int32_t h_behind = probe(-fx, -fy);
    const int32_t lx = -(fy >> 2), ly = fx >> 2;
    const int32_t h_left = probe(lx, ly);
    const int32_t h_right = probe(-lx, -ly);
    if (h_ahead == INT32_MIN || h_behind == INT32_MIN || h_left == INT32_MIN ||
        h_right == INT32_MIN)
        return; // off the height field

    int32_t pitch_slope, roll_slope, threshold;
    if (org2) {
        // True slope angles: ftol(atan2(dh, separation) * 2^32/2pi), the x87 fpatan
        // pair truncated to BAM. [orig: @0x4b6e68/@0x4b6ec8 fild/fpatan/fmul/_ftol2]
        pitch_slope = static_cast<int32_t>(
            std::atan2(static_cast<double>(h_ahead - h_behind), kSlopeAtanFwdBase) *
            kBamPerRadian);
        roll_slope = static_cast<int32_t>(
            std::atan2(static_cast<double>(h_left - h_right), kSlopeAtanLatBase) *
            kBamPerRadian);
        threshold = dead ? kSlideThreshold : kSlideThresholdLive; // [orig: @0x4b6ee5]
    } else {
        // Small-angle approximation, clamped. [orig: @0x4ba1cd <<14 / @0x4ba22b <<16]
        pitch_slope = static_cast<int32_t>(std::min<int64_t>(
            std::max<int64_t>((static_cast<int64_t>(h_ahead) - h_behind) << 14,
                              -kSlopeClamp),
            kSlopeClamp));
        roll_slope = static_cast<int32_t>(std::min<int64_t>(
            std::max<int64_t>((static_cast<int64_t>(h_left) - h_right) << 16,
                              -kSlopeClamp),
            kSlopeClamp));
        threshold = kSlideThreshold;
    }

    // Slide on steep ground: velocity gains dir<<11>>22 (org1, per 8-tick pass) or
    // dir<<9>>22 (org2, per 2-tick pass), along/against the facing for pitch and
    // perpendicular for roll. [orig: @0x4ba24c-0x4ba2fe <<11; @0x4b6f01-0x4b6fa3 <<9]
    const int shift = org2 ? 9 : 11;
    const int32_t slide_x = static_cast<int32_t>((static_cast<int64_t>(c) << shift) >> 22);
    const int32_t slide_y = static_cast<int32_t>((static_cast<int64_t>(s) << shift) >> 22);
    if (pitch_slope > threshold) {        // uphill ahead -> slide back
        inf.vel[0] -= slide_x;
        inf.vel[1] -= slide_y;
    } else if (pitch_slope < -threshold) { // downhill ahead -> slide forward
        inf.vel[0] += slide_x;
        inf.vel[1] += slide_y;
    }
    if (roll_slope > threshold) {          // high on the left -> slide right
        inf.vel[0] += slide_y;
        inf.vel[1] -= slide_x;
    } else if (roll_slope < -threshold) {  // high on the right -> slide left
        inf.vel[0] -= slide_y;
        inf.vel[1] += slide_x;
    }

    // The conform chase. org1: eighth-step on both fields; dead NPCs also aim along
    // the slope (aimPitch/aimHeading/aimFlag — unmodeled fields). org2: quarter-step;
    // a corpse additionally tips its LOOK pitch eighth-step, and the roll write is
    // skipped while a combat roll 41/42 plays (the torso-roll ramp owns those ticks).
    // [orig: @0x4ba320-0x4ba34c / @0x4b6fa9-0x4b6ff4]
    if (org2) {
        if (dead) e.pitch += (pitch_slope - e.pitch + 4) >> 3;
        e.body_pitch += (pitch_slope - e.body_pitch + 2) >> 2;
        if (inf.anim_state != anim_state::kRollLeft &&
            inf.anim_state != anim_state::kRollRight)
            e.roll += (roll_slope - e.roll + 2) >> 2;
    } else {
        e.body_pitch += (pitch_slope - e.body_pitch + 4) >> 3;
        e.roll += (roll_slope - e.roll + 4) >> 3;
    }
}

// ----------------------------------------------------------------------------
// The per-tick motor. [orig: Entity_UpdateInfantryAI @0x4b9910]
// ----------------------------------------------------------------------------
void AiSystem::tick_infantry(AiEntity &e, World &world, uint32_t logic_tick) {
    // Recoil/dispersion live ahead of the network-snap motor exit. Received
    // shots are applied during the network pump, then decay in this frame's
    // body pass; locally generated shots happen later and first decay on the
    // following frame. The recoil PRNG draw is unconditional, including R=0.
    // [orig: Entity_UpdateInfantryPlayerBody / Entity_UpdateInfantryAI]
    Entity *tick_entity = world.registry.get(e.handle);
    infantry_recoil_tick(e.inf, e.heading, e.pitch, world.next_prng16());
    InfantryWeightSpreadInputs weight_inputs;
    const uint32_t tick_flags = tick_entity != nullptr
            ? (tick_entity->flags | tick_entity->engine_flags)
            : 0u;
    const bool mounted_for_spread = tick_entity != nullptr && tick_entity->mounted;
    const WeaponTableEntry *held = tick_entity != nullptr
            ? world.weapons.by_index(tick_entity->equipped_adm_index)
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
    infantry_weapon_weight_spread_tick(e.inf, weight_inputs);

    // Network-snapped remote peer: its pose is SNAPPED each frame by the host read-apply
    // (netsim EntityWireBridge::apply_player_intent), so the movement motor must NOT
    // re-simulate it — it skips, exactly as the original exits before any motor work when
    // the entity+0x24 bit0 net-snap flag is set. The host never interpolates; the
    // smooth-target is staged for CLIENT-side interpolation only (a deferred concern).
    // [orig: Entity_UpdateInfantryAI @0x4b9a03 `test [esi+24h], 1; jnz loc_4BFC8B`;
    // docs/net/novaworld-net-re.md §5.38a / D-NET-89.]
    // The body-ANIM selection is NOT part of that skip: on the authority it runs for every
    // player from the replicated input, feeding the 0x0A anim bytes (D-NET-159).
    if (e.net_is_remote_peer) {
        if (is_authority) remote_player_body_anim(e, world, logic_tick);
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
    // Per-entity stagger key. [orig: tickCounter = current_tick + 36 * entity[31]]
    const uint32_t key = logic_tick + 36u * static_cast<uint32_t>(e.net_id);
    // Tick-start pose Z: the resolver's ladder entry gate measures the CL anchor
    // against the pose at the motor head, not the integrated one.
    // [orig: savedLivePose captured @ 0x4b4190-0x4b419c; entry read @ 0x4b327d]
    const int32_t tick_start_z = e.pos[2];

    RootMotionFrame frame;
    bool have_clip = false;
    int death_transition = -1;

    // 1. Death edge (once — the 0x82 death-family flag marks an already-posed corpse):
    // consume the damage-time anim selection, seed the corpse timer, drop any mount.
    // Then the per-tick corpse block: LeaveCorpse keeps the body forever, otherwise
    // the timer drains and the corpse despawns — held while the local player can see
    // it. [orig: Entity_UpdateInfantryAI @0x4b9c40-0x4b9d55 edge, @0x4b9e4d-0x4ba000
    // persistence]
    if (e.health <= 0) {
        Entity *ent = world.registry.get(e.handle);
        if (infantry_anim_flags(inf.anim_state) != 0x82u) {
            // The kill latches the entity dead bit (Flags |= 2) — the compact
            // state byte carries it unmasked, and the 0x0A priority list reads
            // it as the recipient's dead-or-spectator gate. Both views of our
            // split flags field take it; the spawn reset clears them.
            // [orig: Entity_HandleDeathOnAuthority @0x407CC0 `or Flags, 2`
            //  @0x407D34; the client death-event apply mirrors it @0x40727E]
            if (ent != nullptr) {
                ent->flags |= kEntityFlagDead;
                ent->engine_flags |= kEntityFlagDead;
            }
            // A mounted body detaches so the corpse falls with the world, not the
            // seat [orig: entity+0x16C -> Entity_DetachFromVehicleIfServer @0x4b9c57;
            // the edge also clears Flags 0x40 @0x4b9d2a].
            if (ent != nullptr && ent->mounted)
                entity_detach_from_vehicle(world, e.handle);
            // Corpse timer = the item's deathtime [orig: +0x148 = def+0x890 @0x4b9c97].
            // Unmodeled edge variant (D-AI-9): the +0x134-bit0 silent cleanup
            // (timer-61, tickets cleared, no scream @0x4b9c68) — JO persons never
            // author the bit.
            if (ent != nullptr) ent->corpse_timer = ent->deathtime_ticks;
            // The death scream. NPC (org1): profile slot 7 (sounddeath), or 8
            // (SSNightDead) on a night mission — the runtime reads the mission's
            // EnableNVG attribute as the night gate. [orig: @0x4b9ca3-0x4b9cc1
            // Bms_AttribFlags & 0x100000 pick; play at &entity->pos]
            // Player body (org2): the body-model composite set "<prefix>_DEATH"
            // ("_DEATH_K" at night) from the entity's anim-slot byte — NOT the
            // profile slots; a bank without the set is the id-0 silence with no
            // slot fallback. [orig: @0x4b4c4a-0x4b4c6a ->
            // SoundProfile_FindByEntityAndType @0x528180 type 5/0 ->
            // Entity_PlaySound3D_FullVolume]
            if (ent == nullptr || !ent->dismemberment_piece) {
                const bool night_death =
                    (world.mission_attrib_flags & World::kMissionAttribEnableNVG) != 0;
                if (inf.is_local_player) {
                    SoundSlotEvent scream;
                    scream.source_handle = e.handle.packed;
                    scream.pos[0] = e.pos[0];
                    scream.pos[1] = e.pos[1];
                    scream.pos[2] = e.pos[2];
                    scream.slot = static_cast<uint8_t>(
                            night_death ? audio::kSlotNightDeath : audio::kSlotDeath);
                    audio::compose_entity_sound_set(
                            ent != nullptr ? ent->anim_slot : 0,
                            night_death ? audio::kEntitySoundDeathNight
                                        : audio::kEntitySoundDeath,
                            scream.set_name, sizeof(scream.set_name));
                    world.slot_sounds.push_back(scream);
                } else {
                    emit_slot_sound(world, e,
                                    night_death ? audio::kSlotNightDeath : audio::kSlotDeath,
                                    e.pos);
                }
            }
            // Consume the kill's selection; none staged -> the generic death
            // (cause 4 -> 174 death_pungi) [orig: @0x4b9cc9 fallback + the
            // deathCallback(entity, 1, 0) dispatch; consumed +0x2C0 clears @0x4b9d38.
            // The Flags&0x8000 drowning override (175) rides the unmodeled swim flags.]
            int death = (ent != nullptr && ent->death_anim_state != 0)
                                ? ent->death_anim_state
                                : compute_death_anim_state(0, 0, death_cause::kGeneric);
            if (ent != nullptr) ent->death_anim_state = 0;
            // Stripped embedder .adm sets may lack the selected clip; keep the pre-P1c
            // stand-in ladder (torso-forward, then death_fire) rather than a T-pose.
            if (root_motion != nullptr && !root_motion->has_clip(inf.adm_id, death)) {
                const int torso = anim_state::kDeathBulletBase + 4;
                death = root_motion->has_clip(inf.adm_id, torso) ? torso
                                                                 : anim_state::kDeathFire;
            }
            // The state store is staged until after this tick's already-playing
            // channel tuple advances. This matters when death interrupts A->B:
            // retail outputs the next A/B blend, then replaces B with death C at w=0.
            death_transition = death;
            inf.move_mode = 0;
            inf.target_dist = 0;
            inf.player_moving = false;
        }
        // The corpse-persistence block, every dead tick (the edge tick included —
        // the original falls through the same frame). Persons author no
        // particledeath decay effect and no respawn tickets, so those legs are
        // omitted [orig: the 186-tick effect spawn @0x4b9e7d and the +0x35E path
        // @0x4b9fa0]; the SP watch-check gate (!in_session && no decay effect) maps
        // to "a local player exists" on our host.
        if (ent != nullptr && !ent->hidden && !ent->leave_corpse && !inf.is_local_player) {
            if (ent->corpse_timer > 0) --ent->corpse_timer; // [orig: @0x4b9e74]
            if (ent->corpse_timer <= 0) {
                bool watched = false;
                if (const Entity *lp = world.registry.get(world.cached.local_player)) {
                    // The local-player visibility watch [orig: Physics_RaycastTerrain-
                    // AndSectors(corpse, player) @0x4b9f77 on the entity origins; ours
                    // lifts both endpoints 0.9 u explicitly — the D-AI-9 feet-ray
                    // stand-in (ground-hugging rays false-block on the heightfield
                    // leg), deliberately NOT the muzzle seam: a corpse has no live
                    // pose and the watcher's eye is not a fire origin].
                    constexpr int32_t kWatchLift = 0xE666; // 0.9 u
                    const int32_t cpos[3] = {e.pos[0], e.pos[1], e.pos[2] + kWatchLift};
                    const int32_t ppos[3] = {to_fixed(lp->position.x),
                                             to_fixed(lp->position.y),
                                             to_fixed(lp->position.z) + kWatchLift};
                    watched = line_of_sight_clear(world, cpos, ppos, e.handle,
                                                  world.cached.local_player);
                }
                if (watched) {
                    ent->corpse_timer = 62; // seen -> retry in 1 s [orig: @0x4b9f83]
                } else {
                    // Despawn [orig: Entity_Destroy @0x4b9f93 frees the slot; our
                    // registry keeps the slot — hidden ends presentation and the
                    // health<=0 store already gates every consumer].
                    ent->hidden = true;
                }
            }
        }
    } else if (inf.is_local_player) {
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
        // While latched on a ladder the climb block below owns the state every
        // tick. Retail still runs the selection and overwrites the field in the
        // same tick (its anim layer reads only the final value); our
        // begin_body_transition machinery would restart the crossfade on each
        // intermediate stamp, so the selection is skipped instead. The one
        // selection side effect that matters is mirrored so the net state stays
        // identical: retail's STANDING-idle counter keeps advancing while
        // hanging idle (the fidget arrives right after a dismount) and resets
        // on movement in any stance; the crouch/prone idle branches leave it
        // untouched. [orig: ++entity[0x148] only in the standing-idle branch
        // @0x4b727b; crouch @0x4b724b / prone @0x4b722f skip it; reset
        // @0x4b719b runs stance-independent in the moving branch]
        const bool ladder_latched = ent != nullptr &&
                ((ent->flags | ent->engine_flags) & kEntityFlagLadderContact) != 0;
        if ((logic_tick & 3u) == 0 && !inf.airborne && !carried) {
            if (!ladder_latched) {
                player_body_select(e);
            } else if (inf.player_moving) {
                inf.idle_counter = 0;
            } else if (inf.stance == InfantryState::Stance::kStand) {
                ++inf.idle_counter;
            }
        }
    } else if (is_authority && (key & 15u) == 0) {
        // 2. Think + selection (every 16 ticks). [orig: gate (tick & 0xF) | !authority]
        infantry_think(e, world);
        // On a ladder the NPC's gait selection is suppressed — the org1
        // on-ladder block after the resolve owns states 32-35 (the same-tick
        // overwrite mapping as the player selection skip above; retail also
        // zeroes a speed local our selector has no carrier for).
        // [orig: @ 0x4bd18d — moveMode + the speed local zeroed on Flags 0x100000]
        if (tick_entity != nullptr &&
            ((tick_entity->flags | tick_entity->engine_flags) &
             kEntityFlagLadderContact) != 0)
            inf.move_mode = 0;
        else
            infantry_select(e);
    }

    // 2c. The on-ladder override + player dismounts (org2; EVERY tick — the
    // 4th-tick gate above covers only the stance selection). Body in
    // infantry_ladder.cpp. [orig: @ 0x4b7484-0x4b76d8]
    infantry_ladder_override(e, tick_entity);

    // 2b. The combat pass (NPCs, authority, alive): perception every 32 ticks, the
    // reaction/approach/aim layer per tick — its commits override the 16-tick gait pick,
    // matching the original's later-in-flow targetAnimState overrides.
    // [orig: Entity_UpdateInfantryAI @0x4b9910 §17.1-17.3/17.5 region]
    if (!inf.is_local_player && is_authority && e.health > 0)
        infantry_combat_think(e, world, key);

    // Mounted pose is a late phase, not an update bypass: death ran first and a
    // living NPC has already perceived, selected, and aimed. The mounted return
    // below suppresses only ordinary ground locomotion.
    // [orig: Entity_UpdateInfantryAI @0x4b9910; pose @0x4bec23..0x4bed3f;
    //  dedicated return @0x4bf5c6]
    const bool mounted = e.health > 0 && pose_if_mounted(e, world);
    const Entity *mounted_occ = mounted ? world.registry.get(e.handle) : nullptr;
    const bool mounted_gunner =
            mounted_occ != nullptr && mounted_occ->mount_type == SeatType::Gunner;

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

    // The secondary (weapon) channel. The org2 body runs the full producer — arms-dip
    // block, reload window, the 16-tick selection ladder, then the shared advance —
    // on every local-player body tick, including death ticks (the primary death state
    // disables rendering through its flag gate, but the independent playhead/timers
    // do not freeze on the corpse) [orig: Entity_UpdateInfantryPlayerBody @0x4b40e0;
    // witness §14.8]. The org1 body runs ONLY the shared dual-channel advance: both
    // updaters pass their out-array to AnimMap_UpdateDualChannels @0x40b8c0, so an AI
    // body's secondary channel promotes and steps like anyone's — but its SELECTION
    // writer @0x4b9a28 is unwitnessed, so its state is never re-selected here.
    if (inf.is_local_player) infantry_weapon_channel(e, world, logic_tick);
    else infantry_weapon_channel_advance(e);

    // 3. Advance the selected playing clip and fetch its root motion (every tick).
    if (reset_capsule_bottom_state(inf.anim_state)) inf.prev_capsule_bottom = 0;
    if (root_motion != nullptr)
        have_clip = advance_primary_channel(inf, *root_motion, frame);
    if (have_clip) {
        if (inf.prev_capsule_bottom != 0)
            frame.dz = frame.capsule_bottom - inf.prev_capsule_bottom;
        inf.prev_capsule_bottom = frame.capsule_bottom;
    }
    inf.last_events = have_clip ? frame.events : 0;

    // 3'. The eye-offset restamp (the entity+0x6C/+0x70/+0x74 triple).
    // Entity_UpdateInfantryPlayerBody restamps org2 bodies at two sites —
    // on foot vs mounted (the entity+0x16C carrier discriminator @0x4b6386)
    // — and each site splits local (exact posed head) vs non-local (capsule +
    // lean tilt). The LOCAL player with a shell-fed skeleton sample takes the
    // exact posed head minus Position, all three lanes; on foot the head z is
    // first floored to the five-tap terrain column (indoors exempt), and
    // NEITHER exact leg carries a 0x2000 floor — that floor belongs to the
    // capsule legs [orig: on-foot local @0x4b6bb3..0x4b6cc8 (taps
    // @0x4b6c1e..0x4b6c95, Flags & 0x800000 skip @0x4b6c08); mounted local
    // @0x4b6908..0x4b696c stores raw]. Everyone else takes the anim capsule
    // extent tilted by the lean angle at the retail Q22 precision: org1 NPCs
    // add the witnessed lateral pair — lat = (delta * sinQ22(lean) * 3) >> 2,
    // x = +lat*sin(yaw), y = -lat*cos(yaw) [orig: Entity_UpdateInfantryAI
    // @0x4bf078..0x4bf14c, stores @0x4bf141/0x4bf149/0x4bf14c]; the
    // sample-less player leg keeps z only (delta capped 0xD000 @0x4b698c,
    // floor 0x2000 @0x4b6b98; the mounted twin @0x4b66d9../@0x4b68e7) — its
    // full 3-angle lateral tilt (on-foot @0x4b69ab..0x4b6b7c, mounted
    // @0x4b66fc..0x4b68e5) is the tracked D-HUD-20 residue. The friendly-tag
    // anchor reads the mirrored z.
    if (have_clip) {
        if (inf.is_local_player && world.cached.local_head_valid) {
            float head[3] = {world.cached.local_head.x,
                             world.cached.local_head.y,
                             world.cached.local_head.z};
            const Entity *reg = world.registry.get(world.cached.local_player);
            player_view_floor_eye_to_terrain(
                terrain,
                reg != nullptr && (reg->flags & kEntityFlagIndoors) != 0, head);
            inf.eye_offset_x = to_fixed(head[0]) - e.pos[0];
            inf.eye_offset_y = to_fixed(head[1]) - e.pos[1];
            inf.eye_offset_z = to_fixed(head[2]) - e.pos[2];
        } else {
            const int32_t extent = frame.capsule_top - frame.capsule_bottom;
            const int32_t delta = inf.is_local_player ? std::min(extent, 0xD000)
                                                      : std::max(extent, 0x9000);
            const double lean_rad = static_cast<double>(inf.lean_angle) *
                                    (3.14159265358979323846 / 2147483648.0);
            const int32_t lean_cos =
                static_cast<int32_t>(std::cos(lean_rad) * 4194304.0);
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
                    static_cast<int32_t>(std::sin(lean_rad) * 4194304.0);
                const int32_t lat_raw = static_cast<int32_t>(
                    (static_cast<int64_t>(delta) * lean_sin) >> 22);
                const int32_t lat = (lat_raw * 3) >> 2;
                const double yaw_rad = static_cast<double>(e.heading) *
                                       (3.14159265358979323846 / 2147483648.0);
                const int32_t yaw_sin =
                    static_cast<int32_t>(std::sin(yaw_rad) * 4194304.0);
                const int32_t yaw_cos =
                    static_cast<int32_t>(std::cos(yaw_rad) * 4194304.0);
                inf.eye_offset_x = static_cast<int32_t>(
                    (static_cast<int64_t>(lat) * yaw_sin) >> 22);
                inf.eye_offset_y = -static_cast<int32_t>(
                    (static_cast<int64_t>(lat) * yaw_cos) >> 22);
            }
        }
    }

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

    // The death callback writes the replacement secondary only after the existing
    // animation update/event consume. End-of-tick motor, collision, wire, and render
    // state therefore see death at phase/weight zero, while this frame keeps the
    // pre-death root/event sample. [orig: death caller tail @0x4b9d55]
    if (death_transition >= 0)
        inf.begin_body_transition(death_transition);

    // 3b. Deferred promotion when a LOCKED (flag 0x4) playing clip reaches its end —
    // the PRIMARY channel's end-flag path, the same machinery the weapon channel uses.
    // Before this, a pending target parked behind a locked state (the prone rolls
    // 41/42, flags 0x285) could never land. [orig: AnimMap_UpdateEntity @0x40b77b
    // promotes the queued state on the channel end flag]
    if (inf.anim_pending != 0 && root_motion != nullptr) {
        const int32_t len = root_motion->clip_length_ticks(inf.adm_id, inf.anim_state, 0);
        if (len >= 0 && inf.clip_phase >= len) {
            const int next = inf.anim_pending;
            // Consume the deferral first: the promoted retarget runs the same
            // gait->stance insert, which may re-arm it behind the 169-172 clip
            // [orig: promotion @0x40b795/@0x40b7c3 re-enters the @0x40b662 insert].
            inf.anim_pending = 0;
            begin_body_transition_with_insert(inf, next, root_motion);
        }
    }

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
        if ((key & 7u) == 0 && collision != nullptr) {
            const LadderResolveIO mounted_lio = make_ladder_resolve_io(e, tick_start_z);
            collision->resolve_entity(
                    world, e.handle, e.collide_state, e.pos, inf.vel, inf.vel[2],
                    frame.capsule_bottom, frame.capsule_top, e.heading, e.pitch,
                    inf.is_local_player, is_authority, logic_tick, inf.anim_state,
                    infantry_anim_flags(inf.anim_state), e.health, nullptr,
                    &mounted_lio);
        }
        finish_infantry_tick(e, world);
        return;
    }

    // 4. Ground resample (every 8 ticks). [orig: dump 319-326, cache entity+676]
    if (terrain != nullptr && ((key & 7u) == 0 || !inf.ground_cache_valid)) {
        GroundClearance clearance = ground_clearance;
        clearance.has_physics = e.has_physics;
        clearance.use_dead = (e.health <= 0);
        inf.ground_cache = calc_average_ground_height(*terrain, e.pos, 0, clearance);
        inf.ground_cache_valid = true;
    }

    // 5. Heading + leg-chain chase, per motor (D-INF-12 CLOSED 2026-07-16: the org2
    // write sites to +0x8C/+0x2E4/+0x2E8 are displacement-scanned and byte-read; the
    // org1 block re-read at the same precision). The legs are consumed as the R/L
    // leg-chain bone yaw by Entity_BuildBoneTransformMatrices @0x4b1290 (§14).
    if (inf.is_local_player) {
        // org2 on-foot [orig: Entity_UpdateInfantryPlayerBody @0x4b4945-0x4b4ac1].
        // There is NO body chase: the LEGS chase the mouse yaw (+0x10) directly and
        // the body heading is written as their midpoint — the legs lead, the body
        // follows, and the §14 torso twist is (yaw − leg midpoint). The parachute
        // (Flags 0x20) sixteenth-step body chase @0x4b494d and the seat-bone
        // follow @0x4b654e ride the parachute/mount slices.
        //
        // While latched on a ladder the view yaw is clamped to ±120° of the
        // body heading (infantry_ladder.cpp). [orig: gate @ 0x4b4978; clamp
        // @ 0x4b4b04-0x4b4b42]
        infantry_ladder_view_clamp(
            inf, tick_entity != nullptr
                     ? (tick_entity->flags | tick_entity->engine_flags)
                     : 0u);
        e.heading = inf.target_heading; // mouse-instant render/aim yaw [orig:
                                        // Input_HandleActionBinding @0x49ad40 writes +0x10]
        const int32_t yaw = e.heading;
        if ((infantry_anim_flags(inf.anim_state) & 0x1u) != 0) {
            // A movement state re-plants both feet on the yaw every tick.
            // [orig: @0x4b4984 flag-table bit0 -> @0x4b49dd/@0x4b49e3]
            inf.leg_target[1] = yaw;
            inf.leg_target[0] = yaw;
        } else {
            // Idle: per-leg re-plant measured vs the CURRENT LEG YAW (org1 measures
            // vs the target), left window 32 ticks behind the right.
            // [orig: L @0x4b4993/@0x4b49ad-0x4b49bc ((tick-32)&0x3F);
            //  R @0x4b499b/@0x4b49d0-0x4b49e3 (ebp = tick&0x3F @0x4b4680)]
            const int32_t dl = io::bam_sub(yaw, inf.leg_yaw[1]);
            if (abs_bam(dl) > kLegReplantMin &&
                (abs_bam(dl) > kLegReplantSnap || ((key - 32) & 63u) == 0))
                inf.leg_target[1] = yaw;
            const int32_t dr = io::bam_sub(yaw, inf.leg_yaw[0]);
            if (abs_bam(dr) > kLegReplantMin &&
                (abs_bam(dr) > kLegReplantSnap || (key & 63u) == 0))
                inf.leg_target[0] = yaw;
        }
        for (int leg = 0; leg < 2; ++leg) {
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
        inf.body_heading = io::bam_add(
            inf.leg_yaw[1], io::bam_sar(io::bam_sub(inf.leg_yaw[0], inf.leg_yaw[1]), 1));
    } else {
        // org1 [orig: Entity_UpdateInfantryAI @0x4be8fd-0x4beb18]. Body: quarter-step
        // toward the target, clamped ±69273360; the render yaw moves by the SAME step
        // (ours pins them equal — they never diverge). [orig: @0x4be8fd-0x4be931]
        const int32_t diff = io::bam_sub(inf.target_heading, inf.body_heading);
        int32_t step = io::bam_sar(io::bam_add(diff, 2), 2);
        if (step > kBodyTurnClamp) step = kBodyTurnClamp;
        if (step < -kBodyTurnClamp) step = -kBodyTurnClamp;
        inf.body_heading = io::bam_add(inf.body_heading, step);
        e.heading = inf.body_heading;

        // 5b. Legs. The carried (Flags 0x40) body-snap rides the mount slice. A
        // movement state or a def+84&0x200 body takes the WALK path: the right foot
        // half-snaps to the target, the left pulls a quarter of the residual, both
        // targets = target — the alternating shuffle while walking/turning.
        // [orig: selector @0x4be944-0x4be967; walk path @0x4be9d4-0x4bea0b]
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
            // Idle: re-plant targets toward the MIDPOINT of (body, target), measured
            // vs the current TARGET, left window 32 ticks behind the right.
            // [orig: midpoint @0x4be969-0x4be975; L @0x4be977/@0x4be991-0x4be9a7;
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
            // Quarter-step (sixteenth for def+84&0x200 bodies), clamp ±0x5000000
            // (~7 deg/tick), twist limit ±0x20000000 (45 deg) vs the BODY.
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

    // 6. The slope pass: conform-or-decay body_pitch/roll + the steep-ground slide.
    // Cadence lives inside (org1 every 8th tick on `key`; org2 decay every tick,
    // probes every 2nd on the logic tick). [orig: @0x4ba10f block / @0x4b6d95 block]
    infantry_slope_pass(e, logic_tick, key);

    // Horizontal slide decay. NPC (org1): (7v+4)>>3 with an abs<=8 deadzone, every state. Player
    // (org2): grounded+moving decays by (63*v)>>6 with NO deadzone [orig: @0x4b7949]; airborne uses
    // the SAME (7v+4)>>3 + deadzone as the NPC [orig: @0x4b7982] (the two are mutually exclusive,
    // selected by the 0x2000 grounded flag). Before this the player's slide was never damped, so a
    // slope-slide impulse drifted the player forever. [D-INF-9; inf.airborne here is last
    // tick's value — the vertical resolve below updates it.]
    if (!inf.is_local_player) {
        inf.vel[0] = damp_npc_slide(inf.vel[0]);
        inf.vel[1] = damp_npc_slide(inf.vel[1]);
    } else if (!inf.airborne) {
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
        int32_t fwd = frame.dx, lat = frame.dy;
        // Root TRANSLATION is integrated for EVERY state, not just movement states. The original
        // advances the playing clip ONCE per tick (AnimMap_UpdateEntity @0x40b5f0) and integrates
        // the root delta unconditionally: the g_animStateFlagsTable bit0 flag gates the anim COMMIT rules
        // (@0x4bd85c) and the idle LOOK-AT scan (@0x4be95f), NOT the position integration. Idle
        // clips author a small mean-~0 root velocity — the bored weight-shift / "rock on the feet".
        // Integrating it sways the entity's centre of mass under the swaying skeleton, so the FEET
        // stay PLANTED (they pivot). Gating it (the prior D-INF-8 reading) held the body rigid while
        // the skeletal FK slid the feet — the "idle skating" this overturns. capsule bottom/top and
        // vel are MOVEMENT data only; the visual is the skeleton, which never reads them.
        // [orig: Entity_UpdateInfantryAI @0x4b9910 integrates root delta for all states; overturns D-INF-8]
        if (inf.anim_state == anim_state::kJumpLoop) fwd = 1024; // [data: retail ADM dump root row 4756]
        // Org2 consumes the same-tick leg-midpoint body heading at entity+0x8C,
        // while org1 keeps body/render heading unified in e.heading.
        // [orig: Entity_UpdateInfantryPlayerBody loads entity+0x8C @0x4B41E4,
        // then performs the Q22 root rotation @0x4B41F0..0x4B4255]
        const int32_t move_heading = inf.is_local_player ? inf.body_heading : e.heading;
        const double rad =
            static_cast<double>(move_heading) * (3.14159265358979323846 / 2147483648.0);
        const int32_t c = static_cast<int32_t>(std::cos(rad) * 4194304.0);
        const int32_t s = static_cast<int32_t>(std::sin(rad) * 4194304.0);
        int32_t wx = static_cast<int32_t>((static_cast<int64_t>(fwd) * c) >> 22) -
                     static_cast<int32_t>((static_cast<int64_t>(lat) * s) >> 22);
        int32_t wy = static_cast<int32_t>((static_cast<int64_t>(fwd) * s) >> 22) +
                     static_cast<int32_t>((static_cast<int64_t>(lat) * c) >> 22);
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
        if (inf.is_local_player) {
            if (!gravity_skip) {
                inf.vel[2] -= kGravityStepPlayer;
                if (inf.vel[2] < kTerminalVelZ) inf.vel[2] = kTerminalVelZ;
            }
            e.pos[2] += inf.vel[2];
            // The freefall rush while dropping fast without a parachute (the
            // chute flag 0x20 is unmodeled, so the "chute closed" leg always
            // applies): profile slot 44, refired every body tick — the engine's
            // finite channel pool folds the refires into a continuous rush; our
            // reimpl instead declines to restart the set while its voice still
            // plays. The chute family (slots 41-43 + the vel brake @0x4b7bfd)
            // rides the parachute slice. [orig: @0x4b7c4c-0x4b7c74; vel gate
            // < -0x3000 @0x4b7c52; the smoothTargetPos-delta gate skips
            // net-pulled bodies — our net peers skip the whole motor]
            if (inf.vel[2] < -0x3000) emit_slot_sound(world, e, audio::kSlotFreeFall, e.pos);
        } else if ((gravity_flags & kEntityFlagAiClimb) != 0) {
            // The org1 ladder-climb chase replaces gravity: sixteenth-step Z
            // toward the AI move target, capped 0x4000 up, floor -16384 (half
            // the fall terminal). The order writer rides the AI-order slice.
            // [orig: @ 0x4bf6d2-0x4bf6e5; floor pick @ 0x4bf6e5 + clamp @ 0x4bf7d4]
            int32_t step = (inf.move_target[2] - e.pos[2] + 8) >> 4;
            if (step > 0x4000) step = 0x4000;
            inf.vel[2] = step;
            if (inf.vel[2] < -16384) inf.vel[2] = -16384;
            e.pos[2] += 2 * inf.vel[2];
        } else {
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
            const LadderResolveIO lio = make_ladder_resolve_io(e, tick_start_z);
            foot_clearance = collision->resolve_entity(
                world, e.handle, e.collide_state, e.pos, inf.vel, inf.vel[2],
                frame.capsule_bottom, frame.capsule_top, e.heading, e.pitch,
                inf.is_local_player, is_authority, logic_tick, inf.anim_state,
                infantry_anim_flags(inf.anim_state), e.health, nullptr, &lio);
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
        // The post-resolve latch state: the resolver cleared and possibly
        // re-latched the CL bit this tick; every leg below keys on the live value.
        const bool on_ladder_now =
            tick_entity != nullptr &&
            ((tick_entity->flags | tick_entity->engine_flags) &
             kEntityFlagLadderContact) != 0;
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
                //   STRAIGHT (47 while parachuting rides the unmodeled Flags 0x20;
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
                    if (inf.anim_state != anim_state::kJumpLoop &&
                        root_motion != nullptr &&
                        root_motion->has_clip(inf.adm_id, anim_state::kJumpLoop)) {
                        inf.begin_body_transition(anim_state::kJumpLoop);
                    }
                } else if (e.health > 0) {
                    inf.anim_pending = 0; // [orig: @0x4bf901]
                }
            }
            if (fall_edge_allowed) inf.airborne = true;
        } else if (foot_clearance <= 0) {
            // Landing. Fall damage skips DEAD bodies [orig: org1 `test dl,2`
            // @0x4bf843 — without it a hard-landing corpse would round its health
            // back toward 0 through the clamp]; a damaging landing also stages the
            // fall death-anim selection (+0x2C0, cause 4 -> 174) [orig:
            // @0x4bf85d-0x4bf879 — the staged selector matches our generic-death
            // fallback].
            if (inf.airborne && e.health > 0 && fall_damage_scale > 0 &&
                inf.vel[2] <= -1057 * fall_damage_scale) {
                int32_t excess = (-1057 * fall_damage_scale) - inf.vel[2];
                int32_t dmg = excess >> 4;
                if (dmg > e.health) dmg = e.health;
                e.health = static_cast<int16_t>(e.health - dmg);
            }
            // The landing thump on the airborne-clear edge, dead bodies included
            // (a corpse thrown airborne lands with SSFallDead): profile slot 16
            // SSFallAlive, 15 SSFallDead when dead, at the entity origin.
            // [orig: org1 @0x4bf87f-0x4bf89c (Flags&2 pick) before the 0x2000
            // clear @0x4bf89f; org2 @0x4b7f7c-0x4b7f9e]
            if (inf.airborne)
                emit_slot_sound(world, e,
                                e.health > 0 ? audio::kSlotFallAlive : audio::kSlotFallDead,
                                e.pos);
            e.pos[2] -= foot_clearance;
            inf.vel[2] = 0;
            inf.airborne = false;
        }

        // 9b. Player jump — witnessed org2 order: integrate -> resolver -> edges ->
        // the jump block [orig: @0x4b7de0-0x4b7f0c]. The cooldown lives in the
        // REUSED +0x1A8 field there (org1's targetHeading slot): clamp [0,32], >1
        // counts down, held-at-1 until the key releases (no auto-repeat while held),
        // jump only from 0 [orig: maintenance @0x4b7de0-0x4b7e15, release edge
        // @0x4b7e78-0x4b7e82]. Gates [orig: @0x4b7e8c-0x4b7ebd]: cooldown 0, not
        // prone (the var_10AC selection local), !(Flags & 0x1A002) — in-air, dead,
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
            bool jumped = false;
            if (inf.jump_cooldown == 0 && inf.jump_requested &&
                !player_jump_world_state_blocked(inf, tick_entity) && e.health > 0 &&
                inf.stance != InfantryState::Stance::kProne) {
                jumped = true;
                inf.vel[0] += (3 * root_wx) >> 2; // [orig: @0x4b7ec3-0x4b7ed5]
                inf.vel[1] += (3 * root_wy) >> 2;
                inf.vel[2] = kJumpImpulseVelZ;    // [orig: @0x4b7ee5]
                inf.airborne = true;              // Flags |= 0x2000 [orig: @0x4b7edb]
                inf.jump_cooldown = 32;           // [orig: @0x4b7f06]
                // STRAIGHT stamps — the org2 jump block has NO clip
                // availability check (world-wac-ai-re jump witness: "anim 30
                // jump_start NOW + 31 jump_loop PENDING, no availability
                // check") [orig: @0x4b7ef2 / @0x4b7efc].
                inf.begin_body_transition(anim_state::kJumpStart);
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
            // The grounded bottom dismount: standing on ground below the anchor
            // steps the climber 0.5u back off the face and drops the latch —
            // how climbing down ends. The entry's stance Z-bump exists exactly
            // so a fresh mount is not instantly grounded here.
            // [orig: the not-jumping branch @ 0x4b7fba-0x4b8019 — clearance
            //  <= 0 (Yaw_high > 0 skips @ 0x4b7f76), latched, and
            //  g_LadderContactZ > pos.z]
            if (!jumped && foot_clearance <= 0 && on_ladder_now &&
                collision != nullptr &&
                collision->last_ladder_frame.anchor[2] > e.pos[2]) {
                ladder_push_back(e.pos, inf.body_heading);
                ladder_unlatch(tick_entity);
            }
        }

        // org1 on-ladder (NPC, post-resolve): the congestion hold, the facing
        // press, the climb_up/climb_top select. Body in infantry_ladder.cpp.
        // [orig: Entity_UpdateInfantryAI @ 0x4bf907-0x4bfad8]
        infantry_ladder_org1_block(e, world, tick_entity);
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
        ent->alive = e.health > 0;
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
            ent->body_anim_slot = body_anim_slot_from_state(inf.anim_state);
    }

    mirror_wire_anim(e, world); // wire-anim bytes for the 0x0A player record (D-NET-159)

    advance_part_anim(e); // PANM channels integrate regardless of the motor path
}

// ----------------------------------------------------------------------------
// The infantry combat pass. [orig: Entity_UpdateInfantryAI @0x4b9910; witness
// docs/world/world-wac-ai-re.md §17.1-17.5 (D-AI-4).] Perception every 32 ticks,
// behavior + aim per authority tick, fire on the .bad anim-event triggers.
// ----------------------------------------------------------------------------

namespace {

// The infantry threat scan: nearest visible enemy over pools 0/1 within the staged
// radius. [orig: Entity_FindNearestThreat @0x4b0990 -> Entity_FindTargets @0x53a610,
// ctx type 7 — the -fwd_dist nearest-first walk; §17.2.] Slice deviations (ledger
// D-AI-4 status): the fresh-corpse (<=16-tick) inclusion, the drowning/far x2
// penalties, the forced-target words, and heat/radar signatures are unmodeled; the
// candidate set is alive enemies, nearest LOS-clear first.
EntityHandle infantry_scan_nearest_threat(AiSystem &sys, World &world, AiEntity &e,
                                          int32_t range) {
    // [orig: @0x4b09a1 — visual radius = min(range/2, 40u); Flags&0x40 -> 0]
    int32_t radius = range >> 1;
    if (radius > 0x280000) radius = 0x280000;
    if (radius <= 0) return EntityHandle{};
    if ((e.slot.f[1] & 1) != 0) return EntityHandle{}; // [orig: aiSlot byte+4 & 1 -> no scan]
    // [orig: @0x4b0a02 — teamless scanners pose as team 2 when slot+4 & 8]
    uint8_t own_team = e.team;
    if (own_team == 0 && (e.slot.f[1] & 8) != 0) own_team = 2;
    const bool scanner_berserk = (e.slot.f[1] & 0x200) != 0;
    if (own_team == 0 && !scanner_berserk) return EntityHandle{};

    EntityHandle best{};
    int64_t best_d2 = static_cast<int64_t>(radius) * radius;
    int32_t best_pos[3] = {};
    for (int pool = 0; pool <= 1; ++pool) {
        const size_t cap = world.registry.pool_capacity(pool);
        for (size_t s = 0; s < cap; ++s) {
            const EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
            if (h == e.handle) continue;
            const Entity *c = world.registry.get(h);
            if (c == nullptr || c->health <= 0) continue;      // in-use + alive
            if ((c->engine_flags & 0x8000001u) != 0) continue; // [orig: flags skip]
            if (c->team == 0 || c->team == own_team) {
                // The shared infantry feed accepts the candidate when either side
                // carries AiSlot[1] bit 0x200. This is the authored Berserk
                // attack-anyone exception; ordinary same-team candidates still skip.
                // [orig: Entity_FindTargets @0x53a7ea..0x53a824]
                const AiEntity *candidate_ai = sys.for_handle(h);
                const bool candidate_berserk = candidate_ai != nullptr &&
                        (candidate_ai->slot.f[1] & 0x200) != 0;
                if (!scanner_berserk && !candidate_berserk)
                    continue;
            }
            const int32_t cpos[3] = {static_cast<int32_t>(c->position.x * 65536.0f),
                                     static_cast<int32_t>(c->position.y * 65536.0f),
                                     static_cast<int32_t>(c->position.z * 65536.0f)};
            const int64_t ddx = static_cast<int64_t>(cpos[0]) - e.pos[0];
            const int64_t ddy = static_cast<int64_t>(cpos[1]) - e.pos[1];
            const int64_t d2 = ddx * ddx + ddy * ddy;
            if (d2 >= best_d2) continue; // nearest-first [orig: -fwd_dist descending sort]
            // Fire-origin -> fire-origin endpoints via the muzzle seam
            // [orig: Entity_CheckMutualLineOfSight @0x539be0].
            int32_t sa[3];
            AiSystem::weapon_fire_origin(e, world.logic_tick, sa);
            int32_t sb[3];
            AiSystem::weapon_fire_origin(*c, world.logic_tick, sb);
            if (!sys.line_of_sight_clear(world, sa, sb, e.handle, h))
                continue; // LOS last, in order
            best = h;
            best_d2 = d2;
            best_pos[0] = cpos[0]; best_pos[1] = cpos[1]; best_pos[2] = cpos[2];
        }
    }
    (void)best_pos;
    return best;
}

} // namespace

void AiSystem::infantry_combat_think(AiEntity &e, World &world, uint32_t key) {
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;
    auto avail = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // damageTimer decays once per tick. [orig: the LABEL_373 block]
    if (inf.damage_timer > 0) --inf.damage_timer;

    // --- Perception (every 32 ticks). [orig: tick & 0x1F == 0; §17.1] ---
    if ((key & 0x1Fu) == 0) {
        int32_t range = slot.f[17]; // sight range, 16.16 [orig: slot+68]
        const bool calm = inf.damage_timer == 0 &&
                          slot.bytes()[AiSlot::kAlertByte] == 0 && !inf.was_hit;
        if (calm) range >>= 1; // calm NPCs see half as far
        // The 4-phase range schedule by (tick>>5)&3: full / 6u / half / 6u.
        const uint32_t phase = (key >> 5) & 3u;
        int32_t staged = std::min(range, 0x60000);
        if (phase == 0) staged = range;
        else if (phase == 2) staged = std::max(range >> 1, std::min(range, 0x60000));

        EntityHandle found = infantry_scan_nearest_threat(*this, world, e, staged);

        // Fallback: the last attacker, enemy + LOS-gated; consumed + cleared every scan.
        // [orig: @0x4bbf20-era block — slot+4 & 1 suppresses retaliation]
        if (!found.valid() && (phase == 0 || !inf.combat_target.valid()) &&
            inf.last_attacker.valid() && (slot.f[1] & 1) == 0) {
            if (const Entity *att = world.registry.get(inf.last_attacker)) {
                if (att->health > 0 && att->team != e.team) {
                    // The mutual-LOS fire-origin endpoints stand in here too —
                    // @0x53b130's own endpoint recipe is unwitnessed.
                    int32_t sa[3];
                    weapon_fire_origin(e, world.logic_tick, sa);
                    int32_t sb[3];
                    weapon_fire_origin(*att, world.logic_tick, sb);
                    if (line_of_sight_clear(world, sa, sb, e.handle, inf.last_attacker))
                        found = inf.last_attacker;
                }
            }
        }
        inf.last_attacker = EntityHandle{};

        if (found.valid()) {
            const Entity *t = world.registry.get(found);
            if (t != nullptr) {
                inf.aim_point[0] = static_cast<int32_t>(t->position.x * 65536.0f);
                inf.aim_point[1] = static_cast<int32_t>(t->position.y * 65536.0f);
                inf.aim_point[2] = static_cast<int32_t>(t->position.z * 65536.0f);
                inf.ai_focus = found;
                if (inf.damage_timer < 15) inf.damage_timer += 12; // stay alerted on sight
                if (inf.combat_target == found) ++inf.same_target_ticks;
                else inf.same_target_ticks = 0;
                inf.combat_target = found;
                slot.f[3] = static_cast<int32_t>(found.packed) + 1; // raw slot[3] write
                                                                    // [orig: @0x4bbf83 —
                                                                    // no refcount here]
                // The authority relation quads ride the scan hit [orig: the
                // Entity_FindNearestThreat authority block @0x4b0a6f..0x4b0ae2].
                if (is_authority) {
                    if (const Entity *se = world.registry.get(e.handle))
                        apply_engage_relations(world, *se, *t);
                }
            }
        } else {
            inf.same_target_ticks = 0;
            inf.combat_target = EntityHandle{};
            slot.f[3] = 0;
        }
        // The own priority-target mark decays each scan; firing re-arms it.
        // [orig: Flags &= ~0x4000 @0x4bbfa4]
        if (Entity *se = world.registry.get(e.handle)) se->engine_flags &= ~kEntityFlagPriorityTarget;
    }

    // --- Behavior + aim (per tick with a live target). [orig: §17.3/§17.5] ---
    Entity *tent =
        inf.combat_target.valid() ? world.registry.get(inf.combat_target) : nullptr;
    if (tent != nullptr && tent->health <= 0) {
        // Target died: play post_attack when close + clear. [orig: anim 151 + focus clear]
        const int64_t ddx = static_cast<int64_t>(tent->position.x * 65536.0f) - e.pos[0];
        const int64_t ddy = static_cast<int64_t>(tent->position.y * 65536.0f) - e.pos[1];
        if (ddx * ddx + ddy * ddy < static_cast<int64_t>(196608) * 196608 &&
            avail(anim_state::kPostAttack)) {
            commit_body_state(inf, anim_state::kPostAttack, root_motion);
            inf.ai_focus = EntityHandle{};
        }
        inf.combat_target = EntityHandle{};
        slot.f[3] = 0;
        tent = nullptr;
    }
    if (tent == nullptr) {
        if (inf.combat_move_timer > 0) --inf.combat_move_timer;
        inf.aim_valid = false;
        return;
    }

    const int32_t tpos[3] = {static_cast<int32_t>(tent->position.x * 65536.0f),
                             static_cast<int32_t>(tent->position.y * 65536.0f),
                             static_cast<int32_t>(tent->position.z * 65536.0f)};
    // The witnessed distance metric: sqrt(dx^2 + dy^2 + (dz/2)^2), 16.16.
    // [orig: outPitch[0] = dZ >> 1 into the fsqrt chain @0x4bd0xx]
    const double fdx = static_cast<double>(tpos[0]) - e.pos[0];
    const double fdy = static_cast<double>(tpos[1]) - e.pos[1];
    const double fdz = (static_cast<double>(tpos[2]) - e.pos[2]) * 0.5;
    const int32_t dist16 = static_cast<int32_t>(
        std::min(std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz), 2147418112.0));

    if (dist16 < slot.f[15] && inf.combat_move_timer <= 0) { // inside attack range
        // The combat reactions ARE the attack anims, availability-gated in the witnessed
        // order (each later hit overrides). The reaction flag re-derives only when this
        // region runs [orig: hasCombatReaction is the region's per-tick local -> +875].
        inf.combat_reaction = false;
        int reaction = 0;
        if (avail(anim_state::kAttack)) reaction = anim_state::kAttack;              // 155
        if (inf.was_hit && avail(anim_state::kCoverAttack)) reaction = anim_state::kCoverAttack; // 165
        if (e.health <= static_cast<int16_t>(inf.max_health / 2) &&
            avail(anim_state::kAttack4)) reaction = anim_state::kAttack4;            // 158
        if (dist16 < 589824 && avail(anim_state::kAttack3)) reaction = anim_state::kAttack3; // 157, 9u
        if (dist16 < 196608) {                                                       // 3 u
            if (avail(anim_state::kAttack2)) reaction = anim_state::kAttack2;        // 156
            if (inf.was_hit && avail(anim_state::kCoverAttack2))
                reaction = anim_state::kCoverAttack2;                                // 166
        }
        if (reaction != 0) {
            inf.combat_reaction = true;
            inf.combat_move_timer = slot.f[22] >> 4; // [orig: moveTimer = slot[22]>>4]
            inf.move_mode = 7;                       // hold + fight
            inf.target_dist = 0;
            commit_body_state(inf, infantry_resolve_state(inf.adm_id, reaction), root_motion);
        } else if (slot.f[16] < slot.f[17] && dist16 > slot.f[16]) {
            // Approach the target. [orig: moveMode 1, arrive 10 u]
            inf.move_mode = 1;
            inf.target_dist = dist16;
            inf.arrival_radius = 655360;
            inf.move_target[0] = tpos[0];
            inf.move_target[1] = tpos[1];
            inf.move_target[2] = tpos[2];
            inf.target_heading = bearing_to(tpos[0] - e.pos[0], tpos[1] - e.pos[1]);
        } else if (avail(anim_state::kIdle3)) {
            // Hold in the combat pose. [orig: anim 49 + moveMode 7]
            inf.move_mode = 7;
            inf.target_dist = 0;
            commit_body_state(inf, anim_state::kIdle3, root_motion);
        }
        inf.was_hit = false; // [orig: LABEL_721 wasHit = 0 once the response is chosen]
    }

    // The reload override: empty magazine + a clipsize + the reload clip -> anim 65;
    // while 65 plays the magazine refills. [orig: @0x4bc7xx — targetAnimState = 65,
    // moveMode 0; playing 65 -> word +0x35C = clipsize]
    if (e.profile.clip_size > 0) {
        if (inf.anim_state == anim_state::kReload) {
            inf.magazine = static_cast<int16_t>(e.profile.clip_size);
        } else if (inf.magazine <= 0 && avail(anim_state::kReload)) {
            inf.move_mode = 0;
            inf.target_dist = 0;
            commit_body_state(inf, anim_state::kReload, root_motion);
        }
    }

    // Hold-timer decay, faster when the enemy is close. [orig: LABEL_499]
    if (inf.combat_move_timer > 0) {
        --inf.combat_move_timer;
        if (dist16 < 196608 && inf.combat_move_timer > 0) --inf.combat_move_timer;
        if (dist16 < 655360 && inf.combat_move_timer > 0) --inf.combat_move_timer;
    }

    // --- The aim solution. [orig: §17.5 — lead + sawtooth error] ---
    // Gate: an aim-capable anim (flag bits 0x8 moving-fire / 0x10 attack stance).
    const uint32_t sflags = infantry_anim_flags(inf.anim_state);
    if ((sflags & 0x18u) == 0) {
        inf.aim_valid = false;
        return;
    }
    // Lead the target by its per-tick delta x (dist/0x81074 + 1). The previous-position
    // sample lives in aim_point between think ticks [orig: target savedLivePose +0x80..].
    const int32_t lead = dist16 / 0x81074 + 1;
    int32_t led[3];
    led[0] = tpos[0] + lead * (tpos[0] - inf.aim_point[0]);
    led[1] = tpos[1] + lead * (tpos[1] - inf.aim_point[1]);
    led[2] = tpos[2] + (lead >> 1) * (tpos[2] - inf.aim_point[2]); // vertical lead halved
    inf.aim_point[0] = tpos[0];
    inf.aim_point[1] = tpos[1];
    inf.aim_point[2] = tpos[2];

    // The sawtooth aim error: accuracy A when this target was already fired at
    // (aiRef0 == target), else B; scaled by the difficulty global; two phases.
    // [orig: (119304 * dword_C6EAE8 * acc) >> 5, x (32 - ((tick>>2 [+ tick>>9]) & 0x3F));
    // the prone-in-foliage +40 concealment term needs the foliage-mask seam — D-AI-6.]
    const int32_t acc = (inf.aim_ref0 == inf.combat_target) ? slot.f[10] : slot.f[11];
    const int64_t err_unit =
        (static_cast<int64_t>(119304) * world.wac_values.accuracy_spread * acc) >> 5;
    const int32_t err_a = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>(((key >> 2) + (key >> 9)) & 0x3Fu)));
    const int32_t err_b = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>((key >> 2) & 0x3Fu)));

    // The aim EYE rides the muzzle seam — retail's combat-pass aim anchor IS
    // the posed bone the stamp carries [orig: Entity_GetAttachmentWorldPosition
    // @0x4b2670 on bone +0x366, §21.1]; stampless rows keep the chest lift.
    // The horizontal eye components shift with the pose too, as retail's do.
    int32_t eye[3];
    AiSystem::weapon_fire_origin(e, world.logic_tick, eye);
    // The aim TARGET point is the target's fire origin, not its ground origin
    // [orig: §17.5 — target chest point via Entity_ComputeWeaponFireOrigin
    // @0x43b4b0]. The lead stays computed over the raw positions (inf.aim_point
    // is also the movement sample); the origin offset is added on top.
    int32_t t_origin[3];
    AiSystem::weapon_fire_origin(*tent, world.logic_tick, t_origin);
    const double adx = static_cast<double>(led[0]) + (t_origin[0] - tpos[0]) - eye[0];
    const double ady = static_cast<double>(led[1]) + (t_origin[1] - tpos[1]) - eye[1];
    const double adz = static_cast<double>(led[2]) + (t_origin[2] - tpos[2]) - eye[2];
    const double horiz = std::sqrt(adx * adx + ady * ady);
    inf.aim_heading = bearing_to(static_cast<int32_t>(adx), static_cast<int32_t>(ady)) + err_a;
    inf.aim_pitch = static_cast<int32_t>(std::atan2(adz, horiz) * kBamPerRadian) + err_b;
    inf.aim_valid = true;

    // Body re-face when the aim drifts far off the body. [orig: > 262470208 (~22 deg)]
    if (abs_bam(opennova::io::bam_sub(inf.aim_heading, inf.target_heading)) > 262470208)
        inf.target_heading = inf.aim_heading;

    // The walking-fire latch: muzzle within ~5 deg of the solution, inside the attack
    // range, on the slot[22] cadence. [orig: §17.4 — shouldFireSecondary = 1;
    // moveTimer = slot[22] >> 4; def attrib & 4 gate unmodeled]
    if (abs_bam(opennova::io::bam_sub(inf.aim_heading, e.heading)) < 59652320 &&
        dist16 < slot.f[15] && inf.combat_move_timer < (slot.f[22] >> 5)) {
        inf.combat_move_timer = slot.f[22] >> 4;
        inf.fire_secondary_latch = true;
    }
}

void AiSystem::emit_slot_sound(World &world, const AiEntity &e, int slot, const int32_t pos[3]) {
    if (slot < 0 || slot >= audio::kSoundProfileSlotCount) return;
    const auto &entries = world.sound_profiles.entries();
    if (entries.empty()) return;
    // An unresolved binding falls back to the "default" profile, which itself
    // falls back to the first profile when no "default" exists — the alloc-time
    // seed + the find-miss base return [orig: ItemDef_AllocateWithDefaults
    // @0x49e3f5 seeds FindSlotByName("default"); @0x526e30 miss -> base].
    int16_t profile_index = e.profile.sound_profile;
    const Entity *source = world.registry.get(e.handle);
    // Only player entities carry the packed avatar identity. NPC women author
    // their own primary item profile and must not be reinterpreted through a
    // coincident minimap/net id. Unknown character ids keep the primary.
    // [orig: Entity_GetProfileSlotSound @0x528300, female byte @0x52831c]
    if (source != nullptr && source->player_class != 0 &&
        world.character_traits.is_female(source->minimap_net_id))
        profile_index = e.profile.sound_profile_female;
    const audio::SoundProfile *p =
        (profile_index >= 0 && static_cast<size_t>(profile_index) < entries.size())
            ? &entries[profile_index]
            : world.sound_profiles.find("default");
    if (p == nullptr) return;
    const std::string &set = p->set_names[slot];
    if (set.empty()) return; // the resolved-id-0 no-op [orig: table[slot] == 0]
    SoundSlotEvent ev;
    ev.source_handle = e.handle.packed;
    ev.pos[0] = pos[0];
    ev.pos[1] = pos[1];
    ev.pos[2] = pos[2];
    ev.slot = static_cast<uint8_t>(slot);
    std::snprintf(ev.set_name, sizeof(ev.set_name), "%s", set.c_str());
    world.slot_sounds.push_back(ev);
}

void AiSystem::infantry_anim_sound_pass(AiEntity &e, World &world, uint32_t logic_tick,
                                        int32_t capsule_bottom) {
    InfantryState &inf = e.inf;
    // Opposite tick halves: the NPC updater consumes on ODD ticks, the player
    // body on EVEN [orig: org1 `and eax,1; jz skip` @0x4bf144-0x4bf156; org2
    // `test current_tick,1; jnz skip` @0x4b76e6 (var = current_tick @0x4b4147)].
    if (inf.is_local_player ? ((logic_tick & 1u) != 0) : ((logic_tick & 1u) == 0)) return;
    const uint32_t ev = inf.last_events;
    if (ev == 0) return; // [orig: org1 whole-block skip @0x4bf161-0x4bf163]

    // The six anim-driven foley sounds, bit order 0x20..0x400 -> SSAudio1..6
    // (JO persons author prone rolls, swim strokes, gear rustle here), at the
    // entity origin [orig: org1 @0x4bf169-0x4bf23e; org2 @0x4b76f1-0x4b77c6].
    for (int i = 0; i < 6; ++i) {
        if ((ev & (0x20u << i)) != 0)
            emit_slot_sound(world, e, audio::kSlotAudio1 + i, e.pos);
    }

    // Footsteps: bit 0x1 = left, 0x2 = right. The sound fires at FOOT level —
    // pos.z dipped by the root-motion frame's capsule bottom (the same value
    // the collision capsule uses; the original subtracts it in place, plays,
    // and restores) — and the slot picks by, in order: feet under the water
    // plane -> standing on an entity -> terrain surface 3 (snow) -> ground.
    // [orig: org1 @0x4bf23e-0x4bf2b0; org2 @0x4b77c6-0x4b78a8; the dip slot is
    // the AnimMap out[3] stack cell both bodies pass to the anim update]
    const Entity *went = world.registry.get(e.handle);
    for (int foot = 0; foot < 2; ++foot) {
        if ((ev & (foot == 0 ? 0x1u : 0x2u)) == 0) continue;
        const int32_t pos[3] = {e.pos[0], e.pos[1], e.pos[2] - capsule_bottom};
        // The witnessed test order lives in audio::footstep_slot, shared with
        // the wire-fed remote body channel so both consume one implementation.
        // The on-entity read is last tick's link: this pass runs BEFORE this
        // tick's resolve, the same order as org1 (sound block @0x4bf23e
        // precedes the resolve tail @0x4bf7b8+). Mounted bodies never reach
        // here — seat clips author no foot-event bits.
        const int slot = audio::footstep_slot(
                pos[2], world.env.water_z,
                went != nullptr && went->ground_target.valid(),
                terrain::surface_type_at_fixed(world.surface_map, pos[0], pos[1]),
                foot);
        emit_slot_sound(world, e, slot, pos);
    }
}

void AiSystem::infantry_fire_pass(AiEntity &e, World &world, uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    // The trigger word is consumed on ODD ticks. [orig: v489 & 1 @0x4bf15c]
    if ((logic_tick & 1u) == 0) return;
    if (e.profile.ammo_primary < 0) { // unarmed (the D-AI-5 seed is absent)
        inf.fire_secondary_latch = false;
        return;
    }
    const uint32_t ev = inf.last_events;
    const bool fire_primary = (ev & 0x4u) != 0;   // weapon +0x358, bone +0x365
    const bool fire_c = (ev & 0x10u) != 0;        // weapon +0x35B, bone +0x367
    if ((ev & 0x8u) != 0) inf.fire_secondary_latch = true;
    if (!fire_primary && !fire_c && !inf.fire_secondary_latch) return;

    // The muzzle origin: the embedder-fed posed gun-flash userpoint when FRESH,
    // chest lift otherwise — the shared seam helper (the D-AI-6 seam — [orig:
    // Entity_GetAttachmentWorldPosition @0x4b2670 transforms the fire-bone
    // userpoint's local position by the ANIMATED bone matrix, called from the
    // anim-event fire block @0x4bf326..0x4bf425]).
    int32_t origin[3];
    AiSystem::weapon_fire_origin(e, logic_tick, origin);
    const int32_t yaw = inf.aim_valid ? inf.aim_heading : e.heading;
    const int32_t pitch = io::bam_add(
            inf.aim_valid ? inf.aim_pitch : 0, inf.recoil_pitch);

    if (fire_primary)
        fire_ai_round(world, e, origin, yaw, pitch, e.profile.ammo_primary);
    if (fire_c)
        fire_ai_round(world, e, origin, yaw, pitch, e.profile.ammo_primary);
    if (inf.fire_secondary_latch) {
        inf.fire_secondary_latch = false;
        // Only the secondary path spends the magazine [orig: word +0x35C-- @0x4bf45a];
        // an empty one holds this leg until the reload refill (§17.3).
        if (e.profile.clip_size <= 0 || inf.magazine > 0) {
            if (fire_ai_round(world, e, origin, yaw, pitch, e.profile.ammo_primary) &&
                e.profile.clip_size > 0)
                --inf.magazine;
        }
    }
    inf.aim_ref0 = inf.combat_target; // [orig: aiRef0 = slot[3] after the fire block]
}

void AiSystem::infantry_mounted_fire_pass(AiEntity &e, World &world,
                                          uint32_t logic_tick, uint32_t key) {
    (void)logic_tick;
    InfantryState &inf = e.inf;
    Entity *occ = world.registry.get(e.handle);
    if (occ == nullptr || !occ->mounted || occ->mount_type != SeatType::Gunner)
        return;
    Entity *mount = world.registry.get(occ->mount_target);
    if (mount == nullptr) return;
    const bool slot_bound = vehicle_bind_use_gun_slot(world, *occ, *mount);
    if (!inf.combat_target.valid() || !slot_bound) return;

    // The dedicated request runs on its four-tick infantry cadence, then a
    // coordinate/entity stagger admits one 64-tick half-window and rejects the next.
    // [orig: Entity_UpdateInfantryAI @0x4bf4cf..0x4bf4ee]
    if ((key & 3u) != 0) return;

    const Entity *target = world.registry.get(inf.combat_target);
    if (target == nullptr || target->health <= 0) return;

    const int32_t target_pos[3] = {
        static_cast<int32_t>(target->position.x * 65536.0f),
        static_cast<int32_t>(target->position.y * 65536.0f),
        static_cast<int32_t>(target->position.z * 65536.0f)};
    const uint32_t stagger = static_cast<uint32_t>(target_pos[0]) -
            static_cast<uint32_t>(target_pos[1]) + key;
    if ((stagger & 0x40u) != 0) return;

    // UseGun already swapped EquippedSlot to the parent's persistent embedded
    // MountSlot at attach. This request never touches the personal magazine.
    // [orig: Entity_AttachToUseGunSlot @0x546c42..0x546c73]
    const uint8_t adm = mount->primary_weapon_slot_adm;
    const WeaponTableEntry *weapon =
            world.weapons.by_index(adm);
    if (weapon == nullptr || weapon->ammo_index < 0) return;

    const int32_t dx = io::bam_sub(target_pos[0], e.pos[0]);
    const int32_t dy = io::bam_sub(target_pos[1], e.pos[1]);
    // Retail deliberately halves the vertical component before the 3-D range test.
    const int32_t dz = io::bam_sar(io::bam_sub(target_pos[2], e.pos[2]), 1);
    const double fdx = static_cast<double>(dx);
    const double fdy = static_cast<double>(dy);
    const double fdz = static_cast<double>(dz);
    const double distance = std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz);
    if (distance >= static_cast<double>(e.slot.f[15])) return;

    const int32_t target_heading = bearing_to(dx, dy);
    constexpr int32_t kMountedFireArc = 178956960;
    if (abs_bam(io::bam_sub(target_heading, e.heading)) >= kMountedFireArc) return;

    // The AI leg checks only currentAction and queues FIRE. The later global action
    // pump owns timing, recoil, ammo, and the actual round spawn.
    // [orig: currentAction/nextAction @0x4bf583..0x4bf59e]
    if (mount->primary_weapon_slot.current != weapon_action::kIdle) return;
    mount->primary_weapon_slot.next = weapon_action::kFire;
}

// Mirror the motor-selected body-anim state + channel phase onto the world Entity — the store
// snapshot_of reads for the 0x0A player record bytes 14/15 (emit reads pending ?: current
// [orig: @0x4c0cc7]; ratio = elapsed ticks in the current loop pass, clamp 255 [orig:
// AnimChannel_AdvancePlayback @0x40B140 via @0x4c0cf2]). The LOCAL player additionally exports
// its packed MoveOrder low byte (bits 0-2 dir, bit 3 moving) so its own record echoes real
// input to the peers that motor-drive its avatar [orig: Player_PackInputStateToEntity
// @0x4df68f-0x4df6a1 packs it; the record write reads entity+0x12C low @0x4c0c9c].
void AiSystem::mirror_wire_anim(AiEntity &e, World &world) {
    if (!e.inf.active) return;
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;
    const InfantryState &inf = e.inf;
    ent->net_anim_state = static_cast<uint8_t>(inf.anim_state);
    ent->net_anim_pending = static_cast<uint8_t>(inf.anim_pending);
    // The eye-offset mirror (entity+0x6C/+0x70/+0x74): the body tick's restamp
    // reaches the registry entity the friendly-tag gather (z) and the retail
    // camera consumer (full triple) walk [orig: the same entity fields the
    // writers, HUD_DrawEntityLabel @0x5a3a84, and Camera_ComputeThirdPersonView
    // @0x437fa5 share].
    ent->eye_offset_x = inf.eye_offset_x;
    ent->eye_offset_y = inf.eye_offset_y;
    ent->eye_offset_z = inf.eye_offset_z;
    ent->net_anim_phase =
        static_cast<uint8_t>(inf.clip_phase < 0 ? 0 : (inf.clip_phase > 255 ? 255 : inf.clip_phase));
    if (inf.is_local_player) {
        // Bits 0-2 dir, 3 moving, 5 the HELD jump key, 6/7 the lean keys — the
        // MoveOrder LOW byte layout the uplink's byte 19 carries [orig: the
        // packer @0x4df68f-0x4df741; jump bit 5 @0x4df6fa-0x4df701]. A retail
        // host launches + stamps anim 30/31 from bit 5 [orig: the jump gate
        // @0x4b7e8c-0x4b7f06], so omitting it made a joiner's jump invisible.
        ent->net_move_input = static_cast<uint8_t>((inf.player_move_dir_index & 7) |
                                                   (inf.player_moving ? 8 : 0) |
                                                   (inf.jump_held ? Entity::kMoveOrderJump : 0) |
                                                   (inf.lean_left ? 0x40 : 0) |
                                                   (inf.lean_right ? 0x80 : 0));
        // Local stance mirrors into the MoveOrder bits 8-9 model too (prone bit0/crouch bit1)
        // so the host's own 0x0A tail echo carries it [orig: dword_B76484/dword_B76480 latch
        // the same bits the packer writes @0x4df6a7-0x4df6cd].
        ent->net_stance_bits = static_cast<uint8_t>(
            inf.stance == InfantryState::Stance::kProne
                ? 1u
                : (inf.stance == InfantryState::Stance::kCrouch ? 2u : 0u));
    }
}

// AUTHORITY body-anim selection for a net-snapped remote player (see the ai.h declaration).
// Runs INSTEAD of the movement motor for wire-snapped peers: position/heading stay owned by
// the read-apply snap; only the anim channel advances here. [orig: Entity_UpdateInfantryPlayerBody
// @0x4b40e0 — the same function body the local player runs; the pose work is inert for a
// net-snapped entity because the read-apply overwrites it, while the anim stores persist]
void AiSystem::remote_player_body_anim(AiEntity &e, World &world, uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    if (!inf.active) return;
    Entity *ent = world.registry.get(e.handle);
    if (ent == nullptr) return;

    // HIDDEN entities skip the whole body motor — the retail head bails on Flags bit0
    // before any anim work, which is why a deploy-pending (hidden) player's channel is
    // FROZEN on the wire (golden pre-deploy ratio constant at 40; ours swept to 255 in
    // v32 until this gate). [orig: Entity_UpdateInfantryPlayerBody @0x4b411b-0x4b4127
    // `mov edx,[esi+24h]; test dl,1; jnz return`]
    if ((ent->flags & 1u) != 0) return;

    // The motor's registry hydration is skipped for wire-snapped peers (tick_infantry
    // returns before it); sync the health copy the selection/lean gates read.
    e.health = ent->health;
    int death_transition = -1;

    if (ent->health <= 0) {
        // Death edge — one-shot to the death pose, same policy as the motor's death edge
        // (generic torso-forward bullet death, else the 173 fire fallback; the +0x2C0
        // deferred deathAnim / 175 falling-death variant selection is the combat pass).
        // [orig: the @0x4b40e0 death leg; digest: death 175 / deathAnim]
        // Relationship teardown is independent of animation state. A peer can
        // already be in a death-class clip when a late/replayed state restores a
        // mount, and that must not leave the seat claim or compact carrier alive.
        // [orig: infantry death detach @0x4b9c57..0x4b9c60]
        if (ent->mounted) entity_detach_from_vehicle(world, e.handle);
        if (infantry_anim_flags(inf.anim_state) != 0x82u) {
            const int death = anim_state::kDeathBulletBase + 4;
            const int target =
                (root_motion != nullptr && root_motion->has_clip(inf.adm_id, death))
                    ? death
                    : anim_state::kDeathFire;
            death_transition = target;
        }
    } else {
        // The replicated JUMP key (MoveOrder bit 5): the retail host derives the
        // jump launch + anims for a remote player inside the same authority-run
        // jump block the local body uses — cooldown clamp [0,32], >1 counts
        // down, parked at 1 while the key is held, release -> 0, launch only
        // from 0 [orig: maintenance @0x4b7de0-0x4b7e15, park @0x4b7e7a-0x4b7e82,
        // gate @0x4b7e8c-0x4b7eb5, anim 30 + pending 31 + reload 32
        // @0x4b7ef2-0x4b7f06]. A wire-snapped peer's MOTION is uplink-owned, so
        // only the anim/latch leg runs here. The C2S pose apply reconstructs the
        // terrain-backed airborne/landing state that this gate consumes; where
        // terrain is unavailable, the countdown window remains the conservative
        // animation-only fallback (tracked in the D-NET-159/196 record).
        const bool jump_key =
            (ent->net_move_input & Entity::kMoveOrderJump) != 0;
        // Stance-change (0x1D) and the extended movement uplink (0x0C) can arrive in
        // the same network pump.  The jump block is per-tick and reads the CURRENT
        // MoveOrder prone bit; the fourth-tick locomotion selector below is not an
        // eligibility cache. [orig: MoveOrder&0x100 -> var_10AC @0x4b4165-0x4b4181;
        // prone gate @0x4b7e99]
        const bool replicated_prone = (ent->net_stance_bits & 0x1u) != 0;
        const bool jump_state_blocked = player_jump_world_state_blocked(inf, ent);
        if (inf.jump_cooldown < 0) inf.jump_cooldown = 0;
        if (inf.jump_cooldown > 32) inf.jump_cooldown = 32;
        if (inf.jump_cooldown > 1) {
            --inf.jump_cooldown;
        } else if (inf.jump_cooldown == 1 && !jump_key) {
            inf.jump_cooldown = 0;
        }
        if (inf.jump_cooldown == 0 && jump_key && !replicated_prone &&
            !jump_state_blocked) {
            inf.jump_cooldown = 32;
            // Only latch the synthesized vertical state when the authority has
            // terrain and can therefore observe its landing on a later uplink.
            // Terrain-free harnesses retain the documented cooldown-only residual.
            if (world.terrain != nullptr && world.terrain->valid()) {
                inf.airborne = true;
                ent->flags |= kEntityFlagInAir;
                ent->engine_flags |= kEntityFlagInAir;
            }
            // STRAIGHT stamps, same as the local block — no availability
            // check in the witnessed org2 jump stamps [orig: @0x4b7ef2/@0x4b7efc].
            inf.begin_body_transition(anim_state::kJumpStart);
            inf.anim_pending = anim_state::kJumpLoop;
        }
        const bool jump_episode =
            (inf.anim_state == anim_state::kJumpStart ||
             inf.anim_state == anim_state::kJumpLoop) &&
            inf.jump_cooldown > 1;
        if ((logic_tick & 3u) == 0 && !jump_episode) {
            // Every 4th tick [orig: `test tickCounter, 3` @0x4b70ce]: decode the
            // REPLICATED MoveOrder byte (bits 0-2 = 8-way dir, bit 3 = moving,
            // bits 6-7 = lean [orig: @0x4b4153/@0x4b415c]) + the stance bits
            // (MoveOrder bits 8-9, fed by C2S 0x1D [orig: @0x4b4165-0x4b4181;
            // prone suppressed by Flags & 0x10A000 — swim/parachute unmodeled]),
            // then run the SAME witnessed selection the local player runs (one
            // function in the original; it skips while airborne @0x4b70b8 — the
            // jump-episode window above is the wire-snapped analog).
            inf.player_moving = (ent->net_move_input & 0x08u) != 0;
            inf.player_move_dir_index = ent->net_move_input & 0x07u;
            inf.lean_left = (ent->net_move_input & 0x40u) != 0;
            inf.lean_right = (ent->net_move_input & 0x80u) != 0;
            inf.stance = (ent->net_stance_bits & 0x1u) != 0
                             ? InfantryState::Stance::kProne
                             : ((ent->net_stance_bits & 0x2u) != 0
                                    ? InfantryState::Stance::kCrouch
                                    : InfantryState::Stance::kStand);
            player_body_select(e);
        }
    }
    // The lean angle runs on the authority for every player body (the wire echoes the
    // lean BITS, each end integrates the angle), and the torso roll rides the same
    // body pass. [orig: @0x4b5c97 / @0x4b7dbf / @0x4b5cff]
    infantry_lean_tick(e, ent != nullptr ? (ent->flags | ent->engine_flags) : 0u);
    infantry_torso_roll_tick(e);

    // The upper-body weapon channel runs for a WIRE PEER exactly as it does for the
    // local player. The original has no ownership test on it: the only locality check
    // in the whole region guards the refresh of Flags bits 2-4 from the local
    // g_weaponScopeActive / g_binocularsRaised / NVG globals, and a non-local entity
    // jumps straight past it into the hold-kind ladder [orig: @0x4b5d77
    // `cmp g_local_player_entity, esi ; jnz short loc_4B5DAD`]. For a peer those same
    // three bits arrive over the wire instead — the host has already replaced them
    // from the sender's C2S 0x0C state byte (mask 0x1C) — so the selection reads the
    // peer's OWN entity for both of its inputs: bit 0x10 scoped [orig: test @0x4b5deb]
    // and the equipped ADM index at +0x2B0 [orig: read @0x4b5dba]. Without this a
    // remote player holds a rifle pose whatever it carries, and never adopts the
    // scoped stance the wire is already reporting.
    inf.scope_raised = (ent->flags & kEntityFlagScopeRaised) != 0;
    inf.binoculars_raised = (ent->flags & kEntityFlagBinoculars) != 0;
    infantry_weapon_channel(e, world, logic_tick);

    // Advance the playing clip's channel every tick — the wire ratio source. Uses the real
    // .adm loop rate when the embedder has anim data; without it the phase self-advances on a
    // 62-tick loop stand-in (tracked divergence, D-NET-159 — the faithful source is the
    // anim data rate). Root motion output is discarded: the pose is wire-owned.
    if (root_motion != nullptr) {
        if (reset_capsule_bottom_state(inf.anim_state)) inf.prev_capsule_bottom = 0;
        RootMotionFrame discard;
        if (advance_primary_channel(inf, *root_motion, discard)) {
            inf.prev_capsule_bottom = discard.capsule_bottom;
            // The remote-player eye-offset restamp: retail runs the same body
            // updater for net-snapped peers, and the +0x74 store persists while
            // the pose work is inert — the org2 non-local formula, lean at rest.
            // [orig: Entity_UpdateInfantryPlayerBody @0x4b6984..0x4b68f5]
            const int32_t extent = discard.capsule_top - discard.capsule_bottom;
            int32_t eye_z = std::min(extent, 0xD000);
            if (eye_z < 0x2000) eye_z = 0x2000;
            inf.eye_offset_z = eye_z;
        }
        // The end-flag pending promotion, as on the local path [orig: @0x40b77b].
        if (death_transition >= 0) {
            inf.begin_body_transition(death_transition);
        } else if (inf.anim_pending != 0) {
            const int32_t len = root_motion->clip_length_ticks(inf.adm_id, inf.anim_state, 0);
            if (len >= 0 && inf.clip_phase >= len) {
                const int next = inf.anim_pending;
                inf.anim_pending = 0; // consumed; the insert may re-arm it
                begin_body_transition_with_insert(inf, next, root_motion);
            }
        }
    } else {
        advance_primary_channel_fallback(inf);
        if (death_transition >= 0)
            inf.begin_body_transition(death_transition);
    }

    // Present-pass clip for the host's own third-person view of this peer.
    if (ent->alive && ent->health > 0)
        ent->body_anim_slot = body_anim_slot_from_state(inf.anim_state);

    mirror_wire_anim(e, world);
}

} // namespace opennova::world
