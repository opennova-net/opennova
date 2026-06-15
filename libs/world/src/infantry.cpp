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
//            @0x4b2bd0] is modeled as terrain-clamp + landing (no platforms/water yet);
//            horizontal slide velocity zeroes on ground contact (resolver internals
//            pending). The airborne anim overlay (entity+36 flags 0x2000/0x20 set, 0x40
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

namespace opennova::world {

namespace {

// [orig: 0x4b9910 — body turn clamp ±69273360/tick (~5.8 deg)]
constexpr int32_t kBodyTurnClamp = 69273360;
// [orig: gravity vel_z -= 416 every 2 ticks, terminal -32768; dump 5094]
constexpr int32_t kGravityStep = 416;
constexpr int32_t kTerminalVelZ = -32768;
// [orig: slope slide threshold 0x22222200, clamp 656175520; dump 943..984]
constexpr int32_t kSlopeClamp = 656175520;
constexpr int32_t kSlideThreshold = 572662272;
// [orig: turn-in-place gates; dump 2940-2952]
constexpr int32_t kTurnStopGate = 536870880;  // > 45 deg -> state 147 (stop)
constexpr int32_t kTurnWalkGate = 357913920;  // > 30 deg -> state 1 (walk turn)
// [orig: BAM bearing scale 683565275.5764316 = 2^31/pi (dbl_7C19D8)]
constexpr double kBamPerRadian = 683565275.5764316;

int32_t abs_bam(int32_t v) { return v < 0 ? -v : v; } // BAM diffs never hit INT_MIN in practice

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
int AiSystem::infantry_resolve_state(int state) const {
    if (root_motion == nullptr) return -1;
    auto has = [&](int s) { return root_motion->has_clip(s); };
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
        default:
            break;
    }
    if (has(anim_state::kIdle)) return anim_state::kIdle;
    return -1; // nothing playable: keep the current state
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

    // Turn-in-place overrides. [orig: dump 2940-2952]
    const int32_t err = abs_bam(inf.target_heading - inf.body_heading);
    if (err > kTurnStopGate) target = anim_state::kStop;
    else if (err > kTurnWalkGate) target = anim_state::kWalkForward;

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

    const int resolved = infantry_resolve_state(target);
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

    // 1. Advance the playing clip and fetch its root motion (every tick).
    RootMotionFrame frame;
    bool have_clip = false;
    if (root_motion != nullptr)
        have_clip = root_motion->advance(inf.anim_state, inf.clip_phase, frame);
    inf.last_events = have_clip ? frame.events : 0;

    // 2. Death edge: pick a death pose once, then only gravity/ground applies.
    // [orig: dump 751-913 — full matrix by bone-section + attack quadrant via
    // Entity_ComputeAnimSlotIndex; without combat integration the generic
    // torso-forward entry is used (combat pass refines this)]
    if (e.health <= 0) {
        if (infantry_anim_flags(inf.anim_state) != 0x82u) {
            const int death = anim_state::kDeathBulletBase + 4; // death_bullet_torso_forward
            inf.anim_prev = inf.anim_state;
            inf.anim_state =
                (root_motion != nullptr && root_motion->has_clip(death)) ? death
                                                                         : anim_state::kDeathFire;
            inf.anim_pending = 0;
            inf.clip_phase = 0;
            queue_death_event(e);
            inf.move_mode = 0;
            inf.target_dist = 0;
        }
    } else if (is_authority && (key & 15u) == 0) {
        // 4. Think + selection (every 16 ticks). [orig: gate (tick & 0xF) | !authority]
        infantry_think(e, world);
        infantry_select(e);
    }

    // 3. Ground resample (every 8 ticks). [orig: dump 319-326, cache entity+676]
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

    // 7-8. Rotate the root delta into world axes and integrate. [orig: dump 4758-4779,
    // 5083-5086 — full-precision sin/cos at 2^22, pos += rotated + velocity]
    {
        int32_t fwd = frame.dx, lat = frame.dy;
        if (inf.anim_state == anim_state::kJumpLoop) fwd = 1024; // [orig: dump 4756]
        const double rad =
            static_cast<double>(e.heading) * (3.14159265358979323846 / 2147483648.0);
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

    // 9. Gravity + ground resolve (every 2 ticks, even staggered key). [orig: dump 5088-5173]
    // Skipped entirely without a height field: the original always has terrain under it;
    // a terrain-free world (unit tests) keeps the authored Z instead of free-falling.
    if (terrain != nullptr && (key & 1u) == 0) {
        inf.vel[2] -= kGravityStep;
        if (inf.vel[2] < kTerminalVelZ) inf.vel[2] = kTerminalVelZ;
        e.pos[2] += 2 * inf.vel[2];
        if (terrain != nullptr && inf.ground_cache_valid && inf.ground_cache != INT32_MIN) {
            // Ground feet-on-terrain. The original's +0x50000 mover stand clearance
            // [orig: AI_ProcessMovementStep @0x466db0 brain[131] = ground + 0x50000] is
            // OMITTED here: our soldier .3di models import feet-origin (proven by correct
            // static placement), whereas the original pairs that offset with origin-above-feet
            // models, so adding it floated soldiers ~1 body. The player motor grounds the same
            // way (player.cpp). See docs/world/world-wac-ai-re.md (D-INF) — the IDA follow-up
            // confirms whether brain[131] is the entity Z or just the mover target.
            const int32_t floor_z = inf.ground_cache;
            if (e.pos[2] <= floor_z) {
                // Landing. Fall damage [orig: dump 5152 — vel_z <= -1057*scale ->
                // health -= (excess) >> 4]; horizontal slide stops on contact (D-INF-3).
                if (fall_damage_scale > 0 &&
                    inf.vel[2] <= -1057 * fall_damage_scale) {
                    int32_t excess = (-1057 * fall_damage_scale) - inf.vel[2];
                    int32_t dmg = excess >> 4;
                    if (dmg > e.health) dmg = e.health;
                    e.health = static_cast<int16_t>(e.health - dmg);
                }
                e.pos[2] = floor_z;
                inf.vel[2] = 0;
                inf.vel[0] = 0;
                inf.vel[1] = 0;
            }
        }
    }

    // Mirror the mover-output brain fields the present snapshot reads (kWorkPosZ stays the
    // grounded Z; kOutSpeed reflects motion for anim-slot consumers).
    e.brain.f[AiBrain::kWorkPosX] = e.pos[0];
    e.brain.f[AiBrain::kWorkPosY] = e.pos[1];
    e.brain.f[AiBrain::kWorkPosZ] = e.pos[2];
    e.brain.f[AiBrain::kWorkHeading] = e.heading;

    advance_part_anim(e); // PANM channels integrate regardless of the motor path
}

} // namespace opennova::world
