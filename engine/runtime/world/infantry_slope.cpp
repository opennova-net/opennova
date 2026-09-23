#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/dir_table.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>

namespace opennova::world {
namespace {
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
using io::kBamPerRadian;
} // namespace

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
void AiSystem::infantry_slope_pass(AiEntity &e, World &world, uint32_t logic_tick, uint32_t key) {
    if (terrain == nullptr && collision == nullptr) return;
    InfantryState &inf = e.inf;
    // The org1/org2 split is load-bearing: org2 is the PLAYER-BODY updater's leg
    // (in the original it runs for every player-class body; our motor only ever
    // simulates the local one — remote peers net-snap and skip the motor, D-NET-89),
    // org1 is the NPC/AI updater's leg. The selector is witnessed identical in both,
    // but the cadence, slope math, chase rates, thresholds, and slide impulses are
    // NOT interchangeable — never collapse the legs.
    const bool org2 = inf.is_local_player;
    if (!org2 && (key & 7u) != 0) return; // org1 runs on the entity's 8-tick phase

    // Org1 reads "dead" off the Flags word its death edge latches, not health.
    // [orig: `and ecx,2` @0x4BA084]
    const Entity *slope_entity = world.registry.get(e.handle);
    const uint32_t slope_flags = slope_entity != nullptr
            ? (slope_entity->flags | slope_entity->engine_flags) : 0u;
    const bool dead = !org2 && slope_entity != nullptr
            ? (slope_flags & kEntityFlagDead) != 0
            : e.health <= 0;
    // A dead org1 body in the air tumbles instead: two key-phase ramps a and b
    // (each (32 - phase) * 0xFFFFFF, phase = key & 63 and ((key >> 6) - key) & 63)
    // set the aim pitch to a + b and the aim heading to the target heading, pull
    // body pitch and roll an eighth of the way to a and b, spin the target heading
    // by b / 4 (the heading chase follows it) and clear the aim flag.
    // A rowless body reads its airborne mirror for the in-air bit, as `dead`
    // reads its health. [orig: Entity_UpdateInfantryAI in-air test @0x4BA08D,
    //  tumble @0x4BA094..0x4BA10A]
    const bool in_air = slope_entity != nullptr ? (slope_flags & kEntityFlagInAir) != 0
                                                : inf.airborne;
    if (!org2 && dead && in_air) {
        const int32_t k = static_cast<int32_t>(key);
        const int32_t a = (32 - (k & 63)) * 0xFFFFFF;
        const int32_t b = (32 - (io::bam_sub(k >> 6, k) & 63)) * 0xFFFFFF;
        inf.aim_pitch = io::bam_add(b, a);
        inf.aim_heading = inf.target_heading;
        e.body_pitch = io::bam_add(
                e.body_pitch, io::bam_sar(io::bam_add(io::bam_sub(a, e.body_pitch), 4), 3));
        const int32_t roll = io::bam_add(
                e.roll, io::bam_sar(io::bam_add(io::bam_sub(b, e.roll), 4), 3));
        inf.target_heading = io::bam_add(inf.target_heading, io::bam_sar(b, 2));
        inf.aim_valid = false;
        e.roll = roll;
        return;
    }
    // The player body's own dead in-air tumble (bodyPitch/roll/yaw spin ramps) is
    // not ported; the death-fall mover owns its drop. [orig: org2 @0x4b6ccb-0x4b6d90]
    if (org2 && dead && inf.airborne) return;

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

    // The four short ground columns include the body's candidate models.
    // [orig: Entity_RaycastGroundHeight @0x4142C0 -> raycast_entity_collision
    // @0x413760; org1 probes @0x4BA1A8, org2 @0x4B6E41]
    auto probe = [&](int32_t dx, int32_t dy) -> int32_t {
        if (collision != nullptr)
            return collision->raycast_ground(world, e.handle, e.pos, dx, dy,
                                             0x4000, 0x20000, nullptr);
        // The bare terrain-only embedder has no collision world.
        int32_t p[3] = {io::bam_add(e.pos[0], dx), io::bam_add(e.pos[1], dy), e.pos[2]};
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
        // Small-angle approximation, clamped. [orig: @0x4BA1CB <<14 / @0x4BA227 <<16]
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
    // perpendicular for roll. [orig: @0x4BA249..0x4BA2FB <<11; @0x4b6f01-0x4b6fa3 <<9]
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

    // A dead org1 body aims along the slope: aim pitch = the pitch slope, aim
    // heading = the target heading, aim flag clear; its look then settles there.
    // [orig: Entity_UpdateInfantryAI `test byte ptr [esi+24h],2` @0x4BA301,
    //  stores @0x4BA307..0x4BA319]
    if (!org2 && (slope_flags & kEntityFlagDead) != 0) {
        inf.aim_pitch = pitch_slope;
        inf.aim_heading = inf.target_heading;
        inf.aim_valid = false;
    }
    // The conform chase. org1: eighth-step on both fields. org2: quarter-step;
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

} // namespace opennova::world
