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
//            @0x4b2bd0] is modeled as terrain-only collision (no platforms/water yet).
//            The caller semantics are preserved: return <= 0 lifts the foot out of the
//            floor, return > 0xF000 marks airborne, and small positive clearance is left
//            alone. The airborne anim overlay (entity+36 flags 0x2000/0x20 set, 0x40
//            clear -> parachute 47 else jump_loop 31; dump 3679) waits on those flags.
//            NOTE: patrol walking has NO peer/obstacle avoidance in the original — entity
//            separation is the resolver's push-out, not a steering behavior (dump survey).
//   D-INF-4  the 1024-entry direction tables are runtime-built in the original (pointer
//            globals); we compute trunc(sin/cos(idx*2pi/1024)*2^22) for the same quantized
//            index — assumed generator, see RE doc open item 8.
//   D-INF-5  the idle look-at system (every-256-tick interest scan -> head-look + the
//            43->125 / 44->126 look-idle swaps + greeting voice cues; dump 4089-4429) and
//            its spotting side effects (enemy -> combat focus + alert 10, corpse -> alert
//            25) are not ported; alerts currently come from the BMS seed / explicit state.
//            Rides the combat pass with the rest of the targeting layer.

#include <cmath>

#include "world/ai.h"
#include "world/world.h" // registry.get for the local-player AiEntity->Entity mirror

namespace opennova::world {

namespace {

// [orig: 0x4b9910 — body turn clamp ±69273360/tick (~5.8 deg)]
constexpr int32_t kBodyTurnClamp = 69273360;
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
// [orig: slope slide threshold 0x22222200, clamp 656175520; dump 943..984]
constexpr int32_t kSlopeClamp = 656175520;
constexpr int32_t kSlideThreshold = 572662272;
// [orig: turn-in-place gates; dump 2940-2952]
constexpr int32_t kTurnStopGate = 536870880;  // > 45 deg -> state 147 (stop)
constexpr int32_t kTurnWalkGate = 357913920;  // > 30 deg -> state 1 (walk turn)
// [orig: BAM bearing scale 683565275.5764316 = 2^31/pi (dbl_7C19D8)]
constexpr double kBamPerRadian = 683565275.5764316;
// [orig: degrees -> BAM32 = 2^32/360 = 11930464; same const as ai.cpp/promote.cpp.
// Used only to mirror the local player's engine heading back to the registry Entity's
// mission yaw — the precise (90 - deg) Q1 reconciliation lives with the present path.]
constexpr int32_t kBamPerDegree = 11930464;

int32_t abs_bam(int32_t v) { return v < 0 ? -v : v; } // BAM diffs never hit INT_MIN in practice

bool reset_capsule_bottom_state(int state) {
    return (state >= 32 && state <= 35) || (state >= 176 && state <= 179);
}

int32_t damp_npc_slide(int32_t v) {
    const int32_t out = (7 * v + 4) >> 3; // [orig: 0x4b9910 entity[38/39] decay]
    return abs_bam(out) <= 8 ? 0 : out;
}

bool in_directional_block(int state, int base) {
    return state >= base && state < base + 8;
}

int player_directional_state(int base, int move_dir_index) {
    static constexpr int kOffsetFromInputIndex[8] = {0, 7, 6, 5, 4, 3, 2, 1};
    return base + kOffsetFromInputIndex[move_dir_index & 7];
}

// Quantized heading -> direction vector, 22-bit scale. [orig: idx = (h + 0x200000) >> 22
// into the runtime-built 1024-entry sin/cos tables; dump 934-940, 4783-4797] (D-INF-4)
void quantized_dir(int32_t heading, int32_t &cos22, int32_t &sin22) {
    uint32_t idx = (static_cast<uint32_t>(heading) + 0x200000u) >> 22;
    double a = static_cast<double>(idx) * (6.283185307179586476925 / 1024.0);
    cos22 = static_cast<int32_t>(std::cos(a) * 4194304.0);
    sin22 = static_cast<int32_t>(std::sin(a) * 4194304.0);
}

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

// Player-only body-state selection. The local player consumes entity+0x12C's moving bit,
// stance bits, and move_direction_index: moving base 1/11/19 plus the IDA direction
// offset table; stationary 43/45/48. NPC org1 selection does not read these player
// stance bits. [orig: Entity_UpdateInfantryPlayerBody @0x4b40e0]
static int player_stance_remap(int state, InfantryState::Stance st, int move_dir_index) {
    const bool moving_state =
        in_directional_block(state, anim_state::kWalkForward) ||
        in_directional_block(state, anim_state::kWalkCrouchForward) ||
        in_directional_block(state, anim_state::kWalkProneForward) ||
        state == anim_state::kJogForward || state == anim_state::kRunForward ||
        state == anim_state::kRun2 || state == anim_state::kRun3 ||
        state == anim_state::kWoundedWalk || state == anim_state::kWoundedRun;

    if (moving_state) {
        int base = anim_state::kWalkForward;
        if (st == InfantryState::Stance::kCrouch)
            base = anim_state::kWalkCrouchForward;
        else if (st == InfantryState::Stance::kProne)
            base = anim_state::kWalkProneForward;
        return player_directional_state(base, move_dir_index);
    }

    switch (state) {
        case anim_state::kIdle:
        case anim_state::kIdle2:
        case anim_state::kIdle3:
        case anim_state::kStop:
            if (st == InfantryState::Stance::kCrouch) return anim_state::kIdleCrouch;
            if (st == InfantryState::Stance::kProne) return anim_state::kIdleProne;
            return anim_state::kIdle;
        default:
            return state;
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

    // Turn-in-place overrides are from org1 AI selection. The local player org2 body
    // selects movement from entity+0x12C input bits, not from this NPC turn gate.
    // [orig: Entity_UpdateInfantryAI @0x4b9910 dump 2940-2952]
    if (!inf.is_local_player) {
        const int32_t err = abs_bam(inf.target_heading - inf.body_heading);
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

    // Airborne local player overrides the gait with jump_loop. Player stance and the
    // move_direction_index are org2 input bits; do not apply them to NPC org1 selection.
    if (inf.is_local_player && inf.airborne)
        target = anim_state::kJumpLoop;
    else if (inf.is_local_player)
        target = player_stance_remap(target, inf.stance, inf.move_dir_index);

    const int resolved = infantry_resolve_state(inf.adm_id, target);
    if (resolved < 0) return; // no clips at all: hold the current state
    if (resolved == inf.anim_state) { inf.anim_pending = 0; return; }

    // Commit rules. [orig: dump 3693-3710 — locked states queue; emotes yield only to
    // movement-flagged targets]
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

// ----------------------------------------------------------------------------
// Slope sampling + slide (every 8 ticks). [orig: 0x4b9910 dump 930-1000]
// ----------------------------------------------------------------------------
void AiSystem::infantry_slope_slide(AiEntity &e) {
    if (terrain == nullptr) return;
    InfantryState &inf = e.inf;

    // Probe ground at an offset of the entity. [orig: sub_4142C0 @0x4142c0 — heightmap
    // raycast at (x+dx, y+dy) in a [z+0x4000, z+0x4000-0x20000] window; we sample the
    // height field at the offset position (same surface for terrain)]
    auto probe = [&](int32_t dx, int32_t dy) -> int32_t {
        int32_t p[3] = {e.pos[0] + dx, e.pos[1] + dy, e.pos[2]};
        GroundClearance clearance = ground_clearance;
        clearance.has_physics = e.has_physics;
        clearance.use_dead = (e.health <= 0);
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

    int32_t pitch_slope = static_cast<int32_t>(
        std::min<int64_t>(std::max<int64_t>((static_cast<int64_t>(h_ahead) - h_behind) << 14,
                                            -kSlopeClamp),
                          kSlopeClamp));
    int32_t roll_slope = static_cast<int32_t>(
        std::min<int64_t>(std::max<int64_t>((static_cast<int64_t>(h_left) - h_right) << 16,
                                            -kSlopeClamp),
                          kSlopeClamp));

    // Slide on steep ground: velocity gains dir<<11>>22 (= dir/2048 of the 22-bit unit
    // vector) per 8-tick pass, along/against the facing for pitch and perpendicular for
    // roll. [orig: dump 965-984 — entity[38/39] -+= (cos|sin) << 11 >> 22, exact signs]
    const int32_t slide_x = static_cast<int32_t>((static_cast<int64_t>(c) << 11) >> 22);
    const int32_t slide_y = static_cast<int32_t>((static_cast<int64_t>(s) << 11) >> 22);
    if (pitch_slope > kSlideThreshold) {        // uphill ahead -> slide back
        inf.vel[0] -= slide_x;
        inf.vel[1] -= slide_y;
    } else if (pitch_slope < -kSlideThreshold) { // downhill ahead -> slide forward
        inf.vel[0] += slide_x;
        inf.vel[1] += slide_y;
    }
    if (roll_slope > kSlideThreshold) {          // high on the left -> slide right
        inf.vel[0] += slide_y;
        inf.vel[1] -= slide_x;
    } else if (roll_slope < -kSlideThreshold) {  // high on the right -> slide left
        inf.vel[0] -= slide_y;
        inf.vel[1] += slide_x;
    }

    // Body lean toward the slope (visual pitch/roll), eighth-step chase.
    // [orig: dump 992-994 — entity[36]/entity[6] += (slope - cur + 4) >> 3]
    e.pitch += (pitch_slope - e.pitch + 4) >> 3;
    e.roll += (roll_slope - e.roll + 4) >> 3;
}

// ----------------------------------------------------------------------------
// The per-tick motor. [orig: Entity_UpdateInfantryAI @0x4b9910]
// ----------------------------------------------------------------------------
void AiSystem::tick_infantry(AiEntity &e, World &world, uint32_t logic_tick) {
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
        }
    } else if (inf.is_local_player) {
        // 2'. Local player: the move order is set from input each frame
        // (world::apply_player_move_order), NEVER the AI think — which would zero the order
        // and waypoint-walk. Map the order to an anim every tick (responsive). The player
        // takes the motor's simulate branch on host (is_authority) and on a client
        // (entity==local) alike. [orig: Entity_UpdateInfantryAI loc_4B9C3E; net-re §5.38]
        // Player jump: a grounded jump request launches the vertical impulse and enters the
        // jump arc; gravity (step 9) brings it back down. [orig: Entity_UpdateInfantryPlayerBody
        // @0x4b7ee5 sets entity+0xA0 (vel_z) = 0x1600 and entity+0x24 |= 0x2000 (in-air) on the
        // jump input bit; gravity @0x4b7acf decrements vel_z each tick.]
        if (inf.jump_requested && !inf.airborne && e.health > 0) {
            inf.vel[2] = kJumpImpulseVelZ;
            inf.airborne = true;
        }
        inf.jump_requested = false;
        infantry_select(e);
    } else if (is_authority && (key & 15u) == 0) {
        // 2. Think + selection (every 16 ticks). [orig: gate (tick & 0xF) | !authority]
        infantry_think(e, world);
        infantry_select(e);
    }

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
    {
        const int32_t diff = inf.target_heading - inf.body_heading;
        int32_t step = (diff + 2) >> 2;
        if (step > kBodyTurnClamp) step = kBodyTurnClamp;
        if (step < -kBodyTurnClamp) step = -kBodyTurnClamp;
        inf.body_heading += step;
        e.heading = inf.body_heading;
    }

    // 6. Slope slide + lean (every 8 ticks; alive only). [orig: gate dump 915 + flag rules]
    if (e.health > 0 && (key & 7u) == 0) infantry_slope_slide(e);

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

    // Local player: entity Yaw/Pitch come STRAIGHT from the mouse — instant, no body-turn
    // smoothing, and the look pitch wins over the slope lean. The original drives
    // entity+0x10/+0x14 directly from input. [orig: Input_HandleActionBinding_0
    // @0x4e1330; net-re section 5.38]
    if (inf.is_local_player) {
        inf.body_heading = inf.target_heading;
        e.heading = inf.target_heading;
        e.pitch = inf.look_pitch;
    }

    // 7-8. Rotate the root delta into world axes and integrate. [orig: dump 4758-4779,
    // 5083-5086 — full-precision sin/cos at 2^22, pos += rotated + velocity]
    {
        int32_t fwd = frame.dx, lat = frame.dy;
        // Root TRANSLATION is integrated for EVERY state, not just movement states. The original
        // advances the playing clip ONCE per tick (AnimMap_UpdateEntity @0x40b5f0) and integrates
        // the root delta unconditionally: the dword_8139E8 bit0 flag gates the anim COMMIT rules
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

        const int32_t foot_clearance = e.pos[2] - frame.capsule_bottom - inf.ground_cache;
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
        // BAM32 engine heading -> mission yaw (int16): mission_yaw = 90 - heading/deg.
        // (The exact yaw round-trip is the Q1 reconciliation handled with the present.)
        ent->yaw = static_cast<int16_t>(90 - e.heading / kBamPerDegree);
        // Body-anim slot for the present pass. The infantry motor (player AND AI) bypasses the
        // brain-state update_body_anim_slot (the ai.cpp dispatch `continue`s before reaching it),
        // so derive the present-pass BodyAnim slot from the motor's selected clip state here — else
        // every org1 soldier renders a static T-pose (anim_slot stays -1 and _apply_body_anim
        // no-ops). Leave the slot on death (the present hides / holds the death pose).
        // [orig: Entity_UpdateInfantryAI @0x4b9910 selects the body anim each tick]
        if (ent->alive && ent->health > 0)
            ent->anim_slot = body_anim_slot_from_state(inf.anim_state);
    }

    advance_part_anim(e); // PANM channels integrate regardless of the motor path
}

} // namespace opennova::world
