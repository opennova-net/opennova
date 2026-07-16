// Infantry motor: per-frame update for AI soldiers (entity class org1).
// [orig: Entity_UpdateInfantryAI @ 0x4b9910]. Spec: docs/world/world-wac-ai-re.md §3.
//
// Locomotion is anim-driven: the 16-tick think picks a movement order, the selector maps
// it to an anim state, and the playing clip's root-motion track (injected through
// IRootMotionSource) moves the entity. Without a source every state is unavailable and
// the soldier stands — exactly the original's relationship between motion and clip data.
//
// Deviations (tracked):
//   D-INF-1  blend windows: the original blends old/new clips for 10/15 ticks (root motion
//            included); we switch clips immediately (phase reset).
//   D-INF-2  commands 123/124/125 (move-to-entity orders: staged vehicle boarding via the
//            E1..E8/S/G/H bones with per-soldier entry-slot claims at entity+866, UseGun
//            emplacement manning, seat attach on arrival; dump 1545-2330) and 126
//            (guard/hold) are decoded but not driven by a command source; they idle.
//            127 (follow local player) idles because the simulation has no local player.
//   D-INF-3  the ground/water resolver [orig: Entity_ProcessCollisionAndPlatformPhysics
//            @0x4b2bd0]: with a CollisionWorld wired (AiSystem::collision) the full
//            resolver runs — wall push-out, standing on objects, hurt/zone volumes,
//            person repulsion, blink/indoors (world/collision.h; witness
//            docs/world/world-wac-ai-re.md §15, deferral tails D-COL-1..8). Without one
//            (headless tests) the terrain-cache clearance stands. Remaining D-INF-3
//            tail: water (swim transitions). The caller semantics are preserved either
//            way: return <= 0 lifts the foot out of the floor, return > 0xF000 marks
//            airborne, small positive clearance is left alone; the airborne anim overlay
//            (entity+36 flags 0x2000/0x20 set, 0x40 clear -> parachute 47 else jump_loop
//            31; dump 3679) waits on those flags. NOTE: patrol walking has NO
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
#include <limits>

#include <io/bam.h>

#include "world/ai.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/dir_table.h"
#include "world/world.h" // registry.get for the local-player AiEntity->Entity mirror

namespace opennova::world {

namespace {

// [orig: 0x4b9910 — body turn clamp ±69273360/tick (~5.8 deg)]
constexpr int32_t kBodyTurnClamp = 69273360;
// Leg-chain chase constants [orig: §3.3 — +0x2d4/+0x2d8 chase +0x2e4/+0x2e8 quarter-step,
// rate clamp ±83886080/tick (~7 deg), twist limit ±0x20000000 (45 deg) from the body;
// re-plant hysteresis |Δ| > 59652320 (~5 deg) and (|Δ| > 357913920 (~30 deg) or the
// per-entity 64-tick window). The 1/16-step def+84&0x200 variant is unused for persons.]
constexpr int32_t kLegChaseClamp = 83886080;
constexpr int32_t kLegTwistLimit = 0x20000000;
constexpr int32_t kLegReplantMin = 59652320;
constexpr int32_t kLegReplantSnap = 357913920;
// [orig: gravity vel_z step 416; terminal -32768. Witnessed cadence: NPC org1 -416 EVERY tick
// (@0x4bf7bf) then pos.z += 2*vel (@0x4bf7ec); player org2 -208 EVERY tick (@0x4b7acf) then
// pos.z += vel (@0x4b7cef) — neither gates on tick parity. The player keeps a 2-tick
// discretization (-416 every 2 ticks + 2*vel, net -208/tick + vel/tick = org2) that its jump/fall
// tuning + tests pin; the NPC runs the faithful per-tick path. See the gravity block. D-INF-10]
constexpr int32_t kGravityStep = 416;
constexpr int32_t kTerminalVelZ = -32768;
// Foot-above-floor gap (16.16): the collision caller marks airborne only when the
// returned positive clearance exceeds this value. [orig: org1 @0x4b9910 / org2
// @0x4b40e0 compare collision return against 0xF000]
constexpr int32_t kAirborneGap = 0xF000;
// [orig: jump launch vel_z impulse, Entity_UpdateInfantryPlayerBody @0x4b7ee5
// mov [esi+0A0h], 1600h; the in-air flag entity+0x24 |= 0x2000 the same block sets]
constexpr int32_t kJumpImpulseVelZ = 0x1600;
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

// ----------------------------------------------------------------------------
// Navigation think (every 16 ticks, authority). [orig: 0x4b9910 dump 1293-1540]
// ----------------------------------------------------------------------------
void AiSystem::infantry_think(AiEntity &e, World &world) {
    (void)world;
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;

    // [orig: entity[74] decremented once per think; dump 1338]
    if (inf.wait_cooldown > 0) --inf.wait_cooldown;

    inf.move_mode = 0;
    inf.target_dist = 0;
    inf.at_final_oneshot = false;

    const int32_t ch = slot.f[37]; // [orig: slot+148 = waypoint channel / command]
    // Reserved command range [orig: dump 1341-1407]: 126 guard/hold, 127 follow local
    // player, 123/124/125 scripted move orders. (D-INF-2: order sources not wired yet.)
    if (ch >= 123 && ch <= 127) return;
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
    relmat_calls.push_back({1, static_cast<int32_t>(static_cast<int16_t>(e.relmat_id)), ch, node});
    relmat_calls.push_back({0, e.net_id, ch, node});

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
static void commit_body_state(InfantryState &inf, int resolved) {
    if (resolved < 0) return; // no clips at all: hold the current state
    if (resolved == inf.anim_state) { inf.anim_pending = 0; return; }
    const uint32_t curf = infantry_anim_flags(inf.anim_state);
    if ((curf & 0x4u) != 0) {
        inf.anim_pending = resolved;
    } else if ((curf & 0x20u) == 0 || (infantry_anim_flags(resolved) & 0x1u) != 0) {
        inf.anim_prev = inf.anim_state;
        inf.anim_state = resolved;
        inf.anim_pending = 0;
        inf.clip_phase = 0; // (D-INF-1: no blend window; clip restarts)
        if (reset_capsule_bottom_state(resolved)) inf.prev_capsule_bottom = 0;
    } else {
        inf.anim_pending = resolved;
    }
}

void AiSystem::infantry_select(AiEntity &e) {
    InfantryState &inf = e.inf;
    // [orig: alerted = entity[190] || slot byte +136 || combat-reaction byte +875]
    const bool alerted =
        inf.alert_timer != 0 || e.slot.bytes()[AiSlot::kMoveFlagByte] != 0 || inf.combat_reaction;

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

    commit_body_state(inf, infantry_resolve_state(inf.adm_id, target));
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

    int target;
    if (inf.airborne) {
        // The in-air overlay wins over ground selection (jump arc / falling); the
        // parachute 47 variant rides the unported 0x40 flag. [orig: the 0x2000 in-air
        // flag path @0x4b7e22-0x4b7e3f; overlay states 31/47 dump 3679]
        target = anim_state::kJumpLoop;
    } else if (inf.player_moving) {
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
    // modeled (health/airborne); the 0x10000/0x100000 legs are unmodeled tails.
    // [orig: @0x4b731b-0x4b7354]
    if (inf.stance == InfantryState::Stance::kProne && e.health > 0 && !inf.airborne) {
        if (inf.lean_left) target = anim_state::kRollLeft;   // [orig: @0x4b7335]
        if (inf.lean_right) target = anim_state::kRollRight; // [orig: @0x4b734c]
    }

    commit_body_state(inf, infantry_resolve_state(inf.adm_id, target));
}

// The lean-angle producer — see the ai.h declaration. Decay runs every body tick for
// every infantry body (the corpse keeps decaying, matching the original's placement
// before the weapon-channel block); the ramp needs a live, non-prone body.
// [orig: decay @0x4b5c97 lean -= (lean+8)>>4; ramp @0x4b7dbf/@0x4b7dd6]
void AiSystem::infantry_lean_tick(AiEntity &e) {
    InfantryState &inf = e.inf;
    inf.lean_angle =
        io::bam_sub(inf.lean_angle, io::bam_sar(io::bam_add(inf.lean_angle, 8), 4));
    if (e.health <= 0) return;                      // [orig: the Flags&2 gate legs]
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

// ----------------------------------------------------------------------------
// The upper-body weapon channel — the entity's SECONDARY AnimMap channel.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5cab..0x4b5ea9 (selection + commit)
//  + AnimMap_UpdateDualChannels @0x40b8c0 (advance; deferred promotion at clip end
//  via AnimMap_UpdateEntity @0x40b77b); witness world-wac-ai-re.md §14.8]
// ----------------------------------------------------------------------------
void AiSystem::infantry_weapon_channel(AiEntity &e) {
    InfantryState &inf = e.inf;

    // The arms-dip / head-look decay block [orig: @0x4b5cab..0x4b5ce7]: while the dip
    // window runs, the decay term drops 0x2800000 per tick BEFORE the eighth-step ease;
    // the window byte decrements in BOTH branches — twice per tick — so the 20-tick
    // weapon-switch stamp dips for 10 ticks (the 0x49 remote-reload 80 for 40).
    if (inf.arms_dip_ticks > 0) {
        --inf.arms_dip_ticks;             // [orig: @0x4b5cb5]
        inf.head_look_decay -= 0x2800000; // [orig: @0x4b5cb7 += 0xFD800000]
    }
    inf.head_look_decay -=
        io::bam_sar(io::bam_add(inf.head_look_decay, 4), 3); // [orig: @0x4b5cc7..0x4b5cd5]
    if (inf.arms_dip_ticks > 0) --inf.arms_dip_ticks;        // [orig: @0x4b5cdb..0x4b5ce7]

    // The 3P reload-anim window counts down once per tick [orig: @0x4b5cf9].
    if (inf.reload_anim_ticks > 0) --inf.reload_anim_ticks;

    // Desired state [orig: @0x4b5dad..0x4b5e6f]: the held weapon's hold kind (the
    // AdmDefs dword @0x24E8084 + 0x460*idx = the def's special_hold key) selects the
    // pose ladder; the default (rifles, kind 0) MIRRORS the primary state.
    int desired;
    switch (inf.wpn_hold_kind) {
        case 1: // knife family -> 50 [orig: @0x4b5dc0 lea eax,[ecx+31h]]
            desired = anim_state::kHoldKnife;
            break;
        case 2: // pistol -> 51 [orig: @0x4b5dcd]
            desired = anim_state::kHoldPistol;
            break;
        case 3: // grenade -> 52 [orig: @0x4b5dd7]
            desired = anim_state::kHoldGrenade;
            break;
        case 4: // stinger/AT4/RPG -> 53 [orig: @0x4b5de1]
            desired = anim_state::kHoldStinger;
            break;
        case 5: // designator -> 54, scoped 55 [orig: @0x4b5deb test Flags&0x10]
            desired = inf.scope_raised ? anim_state::kHoldDesignatorScoped
                                       : anim_state::kHoldDesignator;
            break;
        case 6: // P90 -> 56, scoped 57 [orig: @0x4b5dfe]
            desired = inf.scope_raised ? anim_state::kHoldP90Scoped : anim_state::kHoldP90;
            break;
        case 7: // MP7 -> 58, scoped 59 [orig: @0x4b5e11]
            desired = inf.scope_raised ? anim_state::kHoldMP7Scoped : anim_state::kHoldMP7;
            break;
        case 8: // javelin -> 60, scoped 61 [orig: @0x4b5e24]
            desired = inf.scope_raised ? anim_state::kHoldJavelinScoped
                                       : anim_state::kHoldJavelin;
            break;
        default:
            // MIRROR the primary state — 43 idle when the primary is locked (flag 4);
            // 49 idle_3 when scoped [orig: @0x4b5e37..0x4b5e4e].
            desired = (infantry_anim_flags(inf.anim_state) & 0x4u) != 0 ? anim_state::kIdle
                                                                        : inf.anim_state;
            if (inf.scope_raised) desired = anim_state::kIdle3;
            break;
    }
    // Overrides, strongest last [orig: @0x4b5e53..0x4b5e6f]: binoculars 64, then the
    // reload window — 66 reload2 when the hold kind is 2 (pistol), else 65 reload.
    if (inf.binoculars_raised) desired = anim_state::kBinoculars; // [orig: @0x4b5e53]
    if (inf.reload_anim_ticks > 0)
        desired = inf.wpn_hold_kind == 2 ? anim_state::kReload2
                                         : anim_state::kReload; // [orig: @0x4b5e5e..0x4b5e6f]

    // Commit [orig: @0x4b5e72]: same -> skip; a locked (flag 4: attacks 62/63, reloads
    // 65/66) or emote (0x20) current defers the change to clip end; else stamp now.
    if (desired != inf.wpn_state) {
        const uint32_t curf = infantry_anim_flags(inf.wpn_state);
        if ((curf & 0x4u) != 0 || (curf & 0x20u) != 0) {
            inf.wpn_deferred = desired; // [orig: @0x4b5e88/@0x4b5e95]
        } else {
            inf.wpn_state = desired;    // [orig: @0x4b5e9d]
            inf.wpn_deferred = 0;       // [orig: @0x4b5ea3]
            inf.wpn_clip_phase = 0;     // channel re-init (D-INF-1: no blend window)
        }
    }

    // Deferred promotion when the playing clip reaches its end — the channel end-flag
    // path [orig: AnimMap_UpdateEntity @0x40b77b, reached through the @0x40b8c0 swap].
    if (inf.wpn_deferred != 0 && root_motion != nullptr) {
        const int32_t len = root_motion->clip_length_ticks(inf.adm_id, inf.wpn_state);
        if (len >= 0 && inf.wpn_clip_phase >= len) {
            inf.wpn_state = inf.wpn_deferred;
            inf.wpn_deferred = 0;
            inf.wpn_clip_phase = 0;
        }
    }

    // Advance the secondary playhead every tick; root motion is DISCARDED — the weapon
    // layer never feeds the parent transform [orig: parentEntity=0 @0x40b8f3].
    if (root_motion != nullptr) {
        RootMotionFrame discard;
        root_motion->advance(inf.adm_id, inf.wpn_state, inf.wpn_clip_phase, discard);
    }
}

// The fire-path attack stamp — see the infantry.h declaration. Unlike the per-tick
// selection this writes the target immediately, whatever the current state's flags.
// [orig: WeaponAction_Fire @0x542bbc..0x542bea; ebx = 0 from @0x542b22]
void infantry_weapon_attack_stamp(InfantryState &inf, int attack_kind) {
    int state;
    if (attack_kind == 1)
        state = anim_state::kKnifeAttack;   // 62 [orig: @0x542bcb]
    else if (attack_kind == 2)
        state = anim_state::kGrenadeAttack; // 63 [orig: @0x542be0]
    else
        return; // rifle fire stamps NO body state [orig: only the 1/2 compares]
    // The channel re-inits only on a target CHANGE [orig: AnimMap_UpdateEntity @0x40b5f0
    // pulls a new clip only when target differs] — a repeat stamp of the same attack
    // state mid-clip does not restart the playing clip.
    if (inf.wpn_state != state) inf.wpn_clip_phase = 0; // (D-INF-1: no blend window)
    inf.wpn_state = state;
    inf.wpn_deferred = 0;
}

void infantry_weapon_switch_stamp(InfantryState &inf, uint64_t anim_map_serial) {
    if (anim_map_serial == 0 || inf.wpn_anim_map_serial == anim_map_serial) return;
    inf.wpn_anim_map_serial = anim_map_serial;
    inf.arms_dip_ticks = 20;
}

bool infantry_weapon_channel_visible(const InfantryState &inf, bool weapon_in_hands,
                                     bool mount_blocks_channel) {
    return inf.active && weapon_in_hands && !mount_blocks_channel &&
           (infantry_anim_flags(inf.anim_state) & 0x40u) != 0;
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
    const bool conform = (e.def_attrib & 0x200u) != 0 ||
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

    RootMotionFrame frame;
    bool have_clip = false;

    // 1. Death edge: pick a death pose once, then only gravity/ground applies.
    // [orig: dump 751-913 — full matrix by bone-section + attack quadrant via
    // Entity_ComputeAnimSlotIndex; without combat integration the generic
    // torso-forward entry is used (combat pass refines this)]
    if (e.health <= 0) {
        if (infantry_anim_flags(inf.anim_state) != 0x82u) {
            const int death = anim_state::kDeathBulletBase + 4; // death_bullet_torso_forward
            inf.anim_prev = inf.anim_state;
            inf.anim_state =
                (root_motion != nullptr && root_motion->has_clip(inf.adm_id, death)) ? death
                                                                         : anim_state::kDeathFire;
            inf.anim_pending = 0;
            inf.clip_phase = 0;
            queue_death_event(e);
            inf.move_mode = 0;
            inf.target_dist = 0;
            inf.player_moving = false;
        }
    } else if (inf.is_local_player) {
        // 2'. Local player: the player-body input is set from host input each frame
        // (world::apply_player_body_input), never by the org1 AI think path. The body
        // selection is the witnessed org2 selector, every 4th tick like the original
        // (idle 43->44 counts SELECTION passes, so the cadence is load-bearing). The
        // player takes the motor's simulate branch on host (is_authority) and on a
        // client (entity==local) alike. [orig: Entity_UpdateInfantryPlayerBody
        // @0x4b40e0; 4th-tick gate @0x4b70ce; net-re §5.38]
        // Player jump: a grounded jump request launches the vertical impulse and enters the
        // jump arc; gravity (step 9) brings it back down. [orig: Entity_UpdateInfantryPlayerBody
        // @0x4b7ee5 sets entity+0xA0 (vel_z) = 0x1600 and entity+0x24 |= 0x2000 (in-air) on the
        // jump input bit; gravity @0x4b7acf decrements vel_z each tick.]
        if (inf.jump_requested && !inf.airborne && e.health > 0) {
            inf.vel[2] = kJumpImpulseVelZ;
            inf.airborne = true;
        }
        inf.jump_requested = false;
        if ((logic_tick & 3u) == 0) player_body_select(e);
    } else if (is_authority && (key & 15u) == 0) {
        // 2. Think + selection (every 16 ticks). [orig: gate (tick & 0xF) | !authority]
        infantry_think(e, world);
        infantry_select(e);
    }

    // 2b. The combat pass (NPCs, authority, alive): perception every 32 ticks, the
    // reaction/approach/aim layer per tick — its commits override the 16-tick gait pick,
    // matching the original's later-in-flow targetAnimState overrides.
    // [orig: Entity_UpdateInfantryAI @0x4b9910 §17.1-17.3/17.5 region]
    if (!inf.is_local_player && is_authority && e.health > 0)
        infantry_combat_think(e, world, key);

    // The lean angle decays every body tick (corpse included — the decay sits before
    // the weapon-channel block in the original) and ramps while a lean key is held;
    // the torso roll chases the slope roll in the same pass [orig: @0x4b5cff].
    // [orig: @0x4b5c97 / @0x4b7dbf; see infantry_lean_tick]
    if (inf.is_local_player) {
        infantry_lean_tick(e);
        infantry_torso_roll_tick(e);
    }

    // The secondary (weapon) channel and its arms/head-look decay block run on every
    // local-player body tick, including death ticks. The primary death state disables
    // rendering through its flag gate, but the independent playhead/timers do not
    // freeze on the corpse. NPC/remote threading rides D-NET-117.
    // [orig: the same body updater drives both pairs @0x4b40e0; witness §14.8]
    if (inf.is_local_player) infantry_weapon_channel(e);

    // 3. Advance the selected playing clip and fetch its root motion (every tick).
    if (reset_capsule_bottom_state(inf.anim_state)) inf.prev_capsule_bottom = 0;
    if (root_motion != nullptr)
        have_clip = root_motion->advance(inf.adm_id, inf.anim_state, inf.clip_phase, frame);
    if (have_clip) {
        if (inf.prev_capsule_bottom != 0)
            frame.dz = frame.capsule_bottom - inf.prev_capsule_bottom;
        inf.prev_capsule_bottom = frame.capsule_bottom;
    }
    inf.last_events = have_clip ? frame.events : 0;

    // 3a. The fire pass: consume the fresh trigger bits + the walking-fire latch into
    // authoritative rounds (odd ticks). [orig: the @0x4bf15c-0x4bf4b0 fire block runs
    // after the anim advance refreshed g_animEventTriggerBits; §17.4]
    if (!inf.is_local_player && is_authority && e.health > 0)
        infantry_fire_pass(e, world, logic_tick);

    // 3b. Deferred promotion when a LOCKED (flag 0x4) playing clip reaches its end —
    // the PRIMARY channel's end-flag path, the same machinery the weapon channel uses.
    // Before this, a pending target parked behind a locked state (the prone rolls
    // 41/42, flags 0x285) could never land. [orig: AnimMap_UpdateEntity @0x40b77b
    // promotes the queued state on the channel end flag]
    if (inf.anim_pending != 0 && root_motion != nullptr) {
        const int32_t len = root_motion->clip_length_ticks(inf.adm_id, inf.anim_state);
        if (len >= 0 && inf.clip_phase >= len) {
            inf.anim_prev = inf.anim_state;
            inf.anim_state = inf.anim_pending;
            inf.anim_pending = 0;
            inf.clip_phase = 0;
            if (reset_capsule_bottom_state(inf.anim_state)) inf.prev_capsule_bottom = 0;
        }
    }

    // 4. Ground resample (every 8 ticks). [orig: dump 319-326, cache entity+676]
    if (terrain != nullptr && ((key & 7u) == 0 || !inf.ground_cache_valid)) {
        GroundClearance clearance = ground_clearance;
        clearance.has_physics = e.has_physics;
        clearance.use_dead = (e.health <= 0);
        inf.ground_cache = calc_average_ground_height(*terrain, e.pos, 0, clearance);
        inf.ground_cache_valid = true;
    }

    // 5. Body heading: quarter-step toward the target, clamped. [orig: dump 4600-4611 —
    // step = (diff + 2) >> 2 clamped ±69273360; body and render yaw move together]
    if (inf.is_local_player) {
        // The player's render/aim yaw is the mouse, instant [orig: Input_HandleActionBinding
        // @0x49ad40 cases 0xA6/0xA7 write entity Yaw directly]; only the BODY lags behind it,
        // which is what the third-person overlay renders as the torso twist. The org2 chase
        // math is unwitnessed — the org1 quarter-step is applied per D-INF-12.
        const int32_t diff = io::bam_sub(inf.target_heading, inf.body_heading);
        int32_t step = io::bam_sar(io::bam_add(diff, 2), 2);
        if (step > kBodyTurnClamp) step = kBodyTurnClamp;
        if (step < -kBodyTurnClamp) step = -kBodyTurnClamp;
        inf.body_heading = io::bam_add(inf.body_heading, step);
        e.heading = inf.target_heading;
    } else {
        const int32_t diff = io::bam_sub(inf.target_heading, inf.body_heading);
        int32_t step = io::bam_sar(io::bam_add(diff, 2), 2);
        if (step > kBodyTurnClamp) step = kBodyTurnClamp;
        if (step < -kBodyTurnClamp) step = -kBodyTurnClamp;
        inf.body_heading = io::bam_add(inf.body_heading, step);
        e.heading = inf.body_heading;
    }

    // 5b. Leg-chain chase + re-plant (per tick, both motors). The feet hold their planted
    // yaw until the body has twisted past the hysteresis, then shuffle after it at a
    // clamped quarter-step, never more than 45 deg from the body. [orig: §3.3 chase of
    // +0x2d4/+0x2d8 toward +0x2e4/+0x2e8; consumed as the R/L leg-chain bone yaw by
    // Entity_BuildBoneTransformMatrices @0x4b1290 — world-wac-ai-re.md §14. The re-plant
    // TARGET source is unwitnessed for org2 and both legs share one target here: D-INF-12.]
    for (int leg = 0; leg < 2; ++leg) {
        const int32_t drift = io::bam_sub(inf.body_heading, inf.leg_target[leg]);
        if (abs_bam(drift) > kLegReplantMin &&
            (abs_bam(drift) > kLegReplantSnap || (key & 63u) == 0)) {
            inf.leg_target[leg] = inf.body_heading;
        }
        const int32_t ldiff = io::bam_sub(inf.leg_target[leg], inf.leg_yaw[leg]);
        int32_t lstep = io::bam_sar(io::bam_add(ldiff, 2), 2);
        if (lstep > kLegChaseClamp) lstep = kLegChaseClamp;
        if (lstep < -kLegChaseClamp) lstep = -kLegChaseClamp;
        inf.leg_yaw[leg] = io::bam_add(inf.leg_yaw[leg], lstep);
        const int32_t twist = io::bam_sub(inf.leg_yaw[leg], inf.body_heading);
        if (twist > kLegTwistLimit) inf.leg_yaw[leg] = io::bam_add(inf.body_heading, kLegTwistLimit);
        else if (twist < -kLegTwistLimit) inf.leg_yaw[leg] = io::bam_sub(inf.body_heading, kLegTwistLimit);
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

    // 7-8. Rotate the root delta into world axes and integrate. [orig: dump 4758-4779,
    // 5083-5086 — full-precision sin/cos at 2^22, pos += rotated + velocity]
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
        if (inf.anim_state == anim_state::kJumpLoop) fwd = 1024; // [orig: dump 4756]
        int32_t move_heading = e.heading;
        const double rad =
            static_cast<double>(move_heading) * (3.14159265358979323846 / 2147483648.0);
        const int32_t c = static_cast<int32_t>(std::cos(rad) * 4194304.0);
        const int32_t s = static_cast<int32_t>(std::sin(rad) * 4194304.0);
        const int32_t wx = static_cast<int32_t>((static_cast<int64_t>(fwd) * c) >> 22) -
                           static_cast<int32_t>((static_cast<int64_t>(lat) * s) >> 22);
        const int32_t wy = static_cast<int32_t>((static_cast<int64_t>(fwd) * s) >> 22) +
                           static_cast<int32_t>((static_cast<int64_t>(lat) * c) >> 22);
        e.pos[0] += wx + inf.vel[0];
        e.pos[1] += wy + inf.vel[1];
        e.pos[2] += frame.dz;
    }

    // 9. Vertical resolve. The original caller passes entityRadius = AnimMap bottom
    // (out[3]) and receives foot clearance from Entity_ProcessCollisionAndPlatformPhysics.
    // It lifts only on return <= 0; return > 0xF000 marks airborne; small positive
    // clearance is left as-is. [orig: Entity_UpdateInfantryAI @0x4b9910 and
    // Entity_UpdateInfantryPlayerBody @0x4b40e0 callers; resolver @0x4b2bd0]
    if (terrain != nullptr && inf.ground_cache_valid && inf.ground_cache != INT32_MIN) {
        // Gravity. Witnessed: neither motor gates on tick parity. The NPC (org1) falls EVERY tick
        // (-416, then pos.z += 2*vel) [orig: @0x4bf7bf / @0x4bf7ec]; the player (org2) keeps its
        // 2-tick discretization (-416 every 2 ticks + 2*vel nets to org2's -208/tick + vel/tick),
        // which the jump/fall tuning + tests pin. [D-INF-10]
        if (inf.is_local_player ? ((key & 1u) == 0) : true) {
            inf.vel[2] -= kGravityStep;
            if (inf.vel[2] < kTerminalVelZ) inf.vel[2] = kTerminalVelZ;
            e.pos[2] += 2 * inf.vel[2];
        }

        // Foot clearance: with a collision world wired this is the full resolver —
        // candidate contact forces (wall push-out, hurt/ladder/blink volumes, platform
        // standing-on) + person repulsion + the ground probe THROUGH candidate models
        // (standing on buildings) [orig: Entity_ProcessCollisionAndPlatformPhysics
        // @0x4b2bd0; burns down D-INF-3's terrain-only stand-in]. Without one, the
        // terrain-cache clearance stands (headless tests, no placed objects).
        int32_t foot_clearance;
        if (collision != nullptr && collision->instance_count() != 0) {
            foot_clearance = collision->resolve_entity(
                world, e.handle, e.collide_state, e.pos, inf.vel, inf.vel[2],
                frame.capsule_bottom, frame.capsule_top, e.heading, e.pitch,
                inf.is_local_player, is_authority, logic_tick, inf.anim_state,
                infantry_anim_flags(inf.anim_state), e.health);
        } else {
            foot_clearance = e.pos[2] - frame.capsule_bottom - inf.ground_cache;
        }
        if (foot_clearance > kAirborneGap) {
            inf.airborne = true;
        } else if (foot_clearance <= 0) {
            if (inf.airborne && fall_damage_scale > 0 &&
                inf.vel[2] <= -1057 * fall_damage_scale) {
                int32_t excess = (-1057 * fall_damage_scale) - inf.vel[2];
                int32_t dmg = excess >> 4;
                if (dmg > e.health) dmg = e.health;
                e.health = static_cast<int16_t>(e.health - dmg);
            }
            e.pos[2] -= foot_clearance;
            inf.vel[2] = 0;
            inf.airborne = false;
        }
    }

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
    if (own_team == 0 && !e.see_all) return EntityHandle{};

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
            if (c->team == 0 || c->team == own_team) {         // enemies only
                if (!e.see_all) continue;
            }
            const int32_t cpos[3] = {static_cast<int32_t>(c->position.x * 65536.0f),
                                     static_cast<int32_t>(c->position.y * 65536.0f),
                                     static_cast<int32_t>(c->position.z * 65536.0f)};
            const int64_t ddx = static_cast<int64_t>(cpos[0]) - e.pos[0];
            const int64_t ddy = static_cast<int64_t>(cpos[1]) - e.pos[1];
            const int64_t d2 = ddx * ddx + ddy * ddy;
            if (d2 >= best_d2) continue; // nearest-first [orig: -fwd_dist descending sort]
            if (!sys.line_of_sight_clear(e.pos, cpos)) continue; // LOS last, in order
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
                          slot.bytes()[AiSlot::kMoveFlagByte] == 0 && !inf.was_hit;
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
                    const int32_t apos[3] = {static_cast<int32_t>(att->position.x * 65536.0f),
                                             static_cast<int32_t>(att->position.y * 65536.0f),
                                             static_cast<int32_t>(att->position.z * 65536.0f)};
                    if (line_of_sight_clear(e.pos, apos)) found = inf.last_attacker;
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
        if (Entity *se = world.registry.get(e.handle)) se->engine_flags &= ~0x4000u;
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
            commit_body_state(inf, anim_state::kPostAttack);
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
            commit_body_state(inf, infantry_resolve_state(inf.adm_id, reaction));
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
            commit_body_state(inf, anim_state::kIdle3);
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
            commit_body_state(inf, anim_state::kReload);
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
    const int64_t err_unit = (static_cast<int64_t>(119304) * ai_difficulty * acc) >> 5;
    const int32_t err_a = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>(((key >> 2) + (key >> 9)) & 0x3Fu)));
    const int32_t err_b = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>((key >> 2) & 0x3Fu)));

    const int32_t eye = 0xE666; // chest/eye lift, 0.9 u — the fire-origin stand-in (D-AI-6)
    const double adx = static_cast<double>(led[0]) - e.pos[0];
    const double ady = static_cast<double>(led[1]) - e.pos[1];
    const double adz = static_cast<double>(led[2]) - (static_cast<double>(e.pos[2]) + eye);
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

    // The muzzle origin: entity pos + the chest lift — a tracked stand-in for the
    // muzzle-bone transform (D-AI-6). [orig: Entity_GetAttachmentWorldPosition(bone) ->
    // WeaponSlot_FireAndSpawnEffects @0x53f440]
    const int32_t origin[3] = {e.pos[0], e.pos[1], e.pos[2] + 0xE666};
    const int32_t yaw = inf.aim_valid ? inf.aim_heading : e.heading;
    const int32_t pitch = inf.aim_valid ? inf.aim_pitch : 0;

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
    ent->net_anim_phase =
        static_cast<uint8_t>(inf.clip_phase < 0 ? 0 : (inf.clip_phase > 255 ? 255 : inf.clip_phase));
    if (inf.is_local_player) {
        // Bits 0-2 dir, 3 moving, 6/7 the lean keys — the MoveOrder LOW byte layout the
        // uplink's byte 19 carries [orig: the packer @0x4df68f-0x4df741].
        ent->net_move_input = static_cast<uint8_t>((inf.player_move_dir_index & 7) |
                                                   (inf.player_moving ? 8 : 0) |
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

    if (ent->health <= 0) {
        // Death edge — one-shot to the death pose, same policy as the motor's death edge
        // (generic torso-forward bullet death, else the 173 fire fallback; the +0x2C0
        // deferred deathAnim / 175 falling-death variant selection is the combat pass).
        // [orig: the @0x4b40e0 death leg; digest: death 175 / deathAnim]
        if (infantry_anim_flags(inf.anim_state) != 0x82u) {
            const int death = anim_state::kDeathBulletBase + 4;
            inf.anim_prev = inf.anim_state;
            inf.anim_state =
                (root_motion != nullptr && root_motion->has_clip(inf.adm_id, death))
                    ? death
                    : anim_state::kDeathFire;
            inf.anim_pending = 0;
            inf.clip_phase = 0;
        }
    } else if ((logic_tick & 3u) == 0) {
        // Every 4th tick [orig: `test tickCounter, 3` @0x4b70ce]: decode the REPLICATED
        // MoveOrder byte (bits 0-2 = 8-way dir, bit 3 = moving, bits 6-7 = lean
        // [orig: @0x4b4153/@0x4b415c]) + the stance bits (MoveOrder bits 8-9, fed by
        // C2S 0x1D [orig: @0x4b4165-0x4b4181; prone suppressed by Flags & 0x10A000 —
        // swim/parachute unmodeled]), then run the SAME witnessed selection the local
        // player runs (one function in the original).
        inf.player_moving = (ent->net_move_input & 0x08u) != 0;
        inf.player_move_dir_index = ent->net_move_input & 0x07u;
        inf.lean_left = (ent->net_move_input & 0x40u) != 0;
        inf.lean_right = (ent->net_move_input & 0x80u) != 0;
        inf.stance = (ent->net_stance_bits & 0x1u) != 0
                         ? InfantryState::Stance::kProne
                         : ((ent->net_stance_bits & 0x2u) != 0 ? InfantryState::Stance::kCrouch
                                                               : InfantryState::Stance::kStand);
        player_body_select(e);
    }
    // The lean angle runs on the authority for every player body (the wire echoes the
    // lean BITS, each end integrates the angle), and the torso roll rides the same
    // body pass. [orig: @0x4b5c97 / @0x4b7dbf / @0x4b5cff]
    infantry_lean_tick(e);
    infantry_torso_roll_tick(e);

    // Advance the playing clip's channel every tick — the wire ratio source. Uses the real
    // .adm loop rate when the host has anim data; without it the phase self-advances on a
    // 62-tick loop stand-in (tracked divergence, D-NET-159 — the faithful source is the
    // anim data rate). Root motion output is discarded: the pose is wire-owned.
    if (root_motion != nullptr) {
        RootMotionFrame discard;
        root_motion->advance(inf.adm_id, inf.anim_state, inf.clip_phase, discard);
        // The end-flag pending promotion, as on the local path [orig: @0x40b77b].
        if (inf.anim_pending != 0) {
            const int32_t len = root_motion->clip_length_ticks(inf.adm_id, inf.anim_state);
            if (len >= 0 && inf.clip_phase >= len) {
                inf.anim_prev = inf.anim_state;
                inf.anim_state = inf.anim_pending;
                inf.anim_pending = 0;
                inf.clip_phase = 0;
            }
        }
    } else {
        inf.clip_phase = (inf.clip_phase + 1) % 62;
    }

    // Present-pass clip for the host's own third-person view of this peer.
    if (ent->alive && ent->health > 0)
        ent->body_anim_slot = body_anim_slot_from_state(inf.anim_state);

    mirror_wire_anim(e, world);
}

} // namespace opennova::world
