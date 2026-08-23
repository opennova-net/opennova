// The water/float channel of the infantry motor (world-wac-ai-re.md §29.1),
// split out of infantry.cpp (W3-7 size gate): the org1 (AI) float/splash block
// and the org2 (player body) buoyant-rise block with the swim dive latch. The
// gravity/root suppression gates and the swim anim selection stay inline in
// infantry.cpp where the witnessed flow interleaves them.

#include <cmath>
#include <cstdint>

#include "world/ai.h"
#include "world/entity.h"
#include "world/world.h"

namespace opennova::world {

namespace {

// The org1 float model (see infantry_water_block). The hysteresis gap and the
// sink are 16.16 (0.625u and ~0.0187u); the bob shares the sink's magnitude, so
// a floating body rides between the plane and 0.037u under it — a ripple, not a
// visible heave. [orig: the 0xA000 entry bias @0x4bfb84, -0x4C9 @0x4bfbf1, the
// sin amplitude dbl_7C9C28 and the phase pair flt_7C6950 * dbl_7C9BD0]
// The phase pair is the LITERAL 1/256 * 3.1 (Jointops.exe bytes @0x7C6950 =
// 0.00390625f, @0x7C9BD0 = 3.1 double) -- not pi/256; corrected 2026-08-23.
// The org2 (player) arm reads the NEGATED amplitude dbl_7C9BC8 = -1224.0 and
// subtracts it (kong 149040-149047), so both motors bob in the same phase.
constexpr int32_t kWaterFloatHysteresis = 0xA000;
constexpr int32_t kWaterFloatSink = 1225;
constexpr double kWaterBobAmplitude = 1224.0;
constexpr double kWaterBobPhaseScale = 0.00390625 * 3.1;
// org2 dive bit: fully-submerged latch, set below the surface line - 0x2000,
// cleared at the surface clamp and on the not-submerged exit (~0x208000).
// [orig: set @0x4b81ef; clear @0x4b8176; exit @0x4b8373]
constexpr uint32_t kEntityFlagDiveLatch = 0x200000u;
constexpr int32_t kWaterDiveDepth = 0x2000;       // [orig: @0x4b81d0 `surf - 0x2000`]
constexpr int32_t kWaterRiseBias = 0x70;          // [orig: @0x4b8124 `+ 112`]
constexpr int32_t kWaterPitchTermBase = 0x1000;   // [orig: @0x4b80d6 `+ 4096`]
constexpr int32_t kWaterPitchTermClamp = 0x800;   // [orig: @0x4b80f0 `2048`]

} // namespace

// The org1 water block: an AI body that meets the water plane FLOATS on it
// rather than continuing to fall, and fans one splash on the way in.
//
// Entry is hysteretic on purpose. A body that is not yet floating has to get
// 0.625u BELOW the plane before the latch takes; once floating it keeps the
// latch until it is back at or above the plane. Without that gap a body resting
// at the surface would toggle the latch every tick and re-fan the splash with
// it. A body latched to a ladder never floats — the climb owns its vertical.
//
// While the latch is set, the gravity column is skipped: that is the same
// kEntityFlagDrowning half of the 0x108000 gate the fall path already reads, so
// setting the flag here is what stops the body sinking, and the quarter-chase
// below is what moves it. (The flag's name is ours and is narrower than the bit:
// retail uses it as the generic afloat latch, not only for drowning.)
//
// [orig: Entity_UpdateInfantryAI @0x4bfb84..0x4bfc7a — entry
//  `z - 0xA000*((Flags>>15)&1) + 0xA000 >= water || (Flags & 0x100000)`, the
//  exit clear `Flags &= 0xFFDF7FFF`, the float target, the splash edge
//  @0x4bfb87 gated on `(Flags & 0x8000) == 0`, the latch
//  `(Flags & ~0x2000) | 0x8000` @0x4bfc48 and the quarter-chase tail @0x4bfc65.
//  The player twin @0x4b8020 carries the same shape plus swim control and a
//  second, shallower dive edge (Flags 0x200000) — both stay with D-INF-3.]
void AiSystem::infantry_water_block(AiEntity &e, World &world, Entity *tick_entity,
                                    int32_t capsule_bottom, uint32_t logic_tick) {
    if (tick_entity == nullptr) return;
    const int32_t water = world.env.water_z;
    if (water == 0) return; // our no-water-world sentinel (retail worlds always carry a plane)

    const uint32_t flags = tick_entity->flags;
    const bool was_afloat = (flags & kEntityFlagDrowning) != 0;
    const int32_t entry_z = e.pos[2] + (was_afloat ? 0 : kWaterFloatHysteresis);
    if (entry_z >= water || (flags & kEntityFlagLadderContact) != 0) {
        tick_entity->flags = flags & ~kEntityFlagDrowning; // [orig: the 0xFFDF7FFF clear —
        return;                                            //  its 0x200000 half is the player's
    }                                                      //  dive latch, which org1 never sets]

    // The float target: the plane, plus a shallow bob whose phase is seeded from
    // the body's own XY so a squad in the water is not in lockstep, minus half
    // the eye offset and a fixed sink, plus the anim frame's capsule bottom when
    // that hangs below the origin.
    int32_t depth = capsule_bottom;
    if (depth > 0) depth = 0;                       // [orig: the `> 0 -> 0` clamp]
    const int32_t phase =
            ((e.pos[1] + e.pos[0]) >> 12) + 4 * static_cast<int32_t>(logic_tick);
    const int32_t bob = static_cast<int32_t>(
            std::sin(static_cast<double>(phase) * kWaterBobPhaseScale) * kWaterBobAmplitude);
    const int32_t target = water + bob - (tick_entity->eye_offset_z >> 1)
                         - kWaterFloatSink + depth;

    // The entry edge, fanned once. Which of the two sounds it takes is the body's
    // own airborne bit: a soldier who WADED in and one who JUMPED in are heard
    // differently. Position is the body's XY at the PLANE, not at its own Z.
    if (!was_afloat) {
        world.water_crossings.add(e.pos[0], e.pos[1], water,
                                  /*airborne=*/(flags & kEntityFlagInAir) != 0);
    }
    tick_entity->flags = (flags & ~kEntityFlagInAir) | kEntityFlagDrowning;
    e.inf.airborne = false; // the motor-side mirror of the 0x2000 clear

    // The vertical is a QUARTER-step toward the target, not a snap: that is what
    // makes a body entering water settle over a few ticks instead of popping.
    e.pos[2] += (target - e.pos[2] + 2) >> 2;
}

// The org2 (player body) water block -- see the ai.h declaration. Same
// hysteretic entry/exit as org1, then the BUOYANT-RISE form instead of the
// snap: the body rises by a fixed step each tick and is clamped from above at
// the surface line, the velocity triplet drags, and a look-pitch term lets a
// moving swimmer dive and surface. The local player alone rides the surface
// bob; a remote row on the authority gets the flat base.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b8020-0x4b8373; kong 149010-149165]
void AiSystem::player_water_block(AiEntity &e, World &world, Entity *tick_entity,
                                  int32_t capsule_bottom, bool is_authority,
                                  uint32_t logic_tick) {
    if (tick_entity == nullptr) return;
    InfantryState &inf = e.inf;
    const int32_t water = world.env.water_z;
    if (water == 0) {
        // Our no-water-world sentinel: the channel is off, and stale float/dive
        // bits clear so the gravity gate can never wedge on them.
        tick_entity->flags &= ~(kEntityFlagDrowning | kEntityFlagDiveLatch);
        tick_entity->engine_flags &= ~(kEntityFlagDrowning | kEntityFlagDiveLatch);
        return;
    }
    const uint32_t flags = tick_entity->flags | tick_entity->engine_flags;
    const bool was_afloat = (flags & kEntityFlagDrowning) != 0;
    // [orig: @0x4b8020-0x4b804d `z - 0xA000*((Flags>>15)&1) + 0xA000 >= water
    //  || (Flags & 0x100000)` -> `Flags &= 0xFFDF7FFF`]
    const int32_t entry_z = e.pos[2] + (was_afloat ? 0 : kWaterFloatHysteresis);
    if (entry_z >= water || (flags & kEntityFlagLadderContact) != 0) {
        tick_entity->flags &= ~(kEntityFlagDrowning | kEntityFlagDiveLatch);
        tick_entity->engine_flags &= ~(kEntityFlagDrowning | kEntityFlagDiveLatch);
        return;
    }

    int32_t depth = capsule_bottom;
    if (depth > 0) depth = 0;                       // [orig: @0x4b8053 `> 0 -> 0`]
    // base: the local player subtracts the NEGATED-amplitude bob (so it adds
    // 1224*sin) and folds the capsule bottom; the else arm is the flat -1225.
    // [orig: @0x4b8063-0x4b80a5 `v = -1225 - ftol(sin(...) * -1224.0) + cb`]
    int32_t base;
    if (inf.is_local_player) {
        const int32_t phase =
                ((e.pos[0] + e.pos[1]) >> 12) + 4 * static_cast<int32_t>(logic_tick);
        const int32_t bob = static_cast<int32_t>(
                std::sin(static_cast<double>(phase) * kWaterBobPhaseScale) * -kWaterBobAmplitude);
        base = -kWaterFloatSink - bob + depth;
    } else {
        base = -kWaterFloatSink;
    }

    // The look-pitch dive/rise term: a MOVING swimmer (MoveOrder bit 3) on the
    // local or authority row scales |base/2| + 0x1000 by Pitch>>14 and clamps
    // to +-0x800; everyone else gets zero. [orig: @0x4b80aa-0x4b8113]
    int32_t pitch_term = 0;
    if (inf.player_moving && (inf.is_local_player || is_authority)) {
        const int32_t half = base >> 1;
        const int64_t scale = (half < 0 ? -half : half) + kWaterPitchTermBase;
        const int64_t term = ((static_cast<int64_t>(e.pitch >> 14) * scale) + 0x8000) >> 16;
        pitch_term = static_cast<int32_t>(term);
        if (pitch_term > kWaterPitchTermClamp) pitch_term = kWaterPitchTermClamp;
        if (pitch_term < -kWaterPitchTermClamp) pitch_term = -kWaterPitchTermClamp;
    }

    // The buoyant rise and the velocity-triplet drag. [orig: @0x4b8124-0x4b8163]
    const int32_t base_q = base >> 4;
    e.pos[2] += (base_q < 0 ? -base_q : base_q) + pitch_term + kWaterRiseBias;
    inf.vel[0] -= (inf.vel[0] + 16) >> 5;
    inf.vel[1] -= (inf.vel[1] + 16) >> 5;
    inf.vel[2] -= (inf.vel[2] + 16) >> 5;

    // The surface line, clamped from above; the dive bit below it - 0x2000 with
    // the dive splash once (the non-airborne sound; the overlay fan rides the
    // crossing queue). [orig: @0x4b8169-0x4b81f5]
    const int32_t surf = water + (base >> 1) - (tick_entity->eye_offset_z >> 1);
    if (e.pos[2] < surf) {
        if (e.pos[2] < surf - kWaterDiveDepth &&
            (flags & kEntityFlagDiveLatch) == 0) {
            tick_entity->flags |= kEntityFlagDiveLatch;
            tick_entity->engine_flags |= kEntityFlagDiveLatch;
            world.water_crossings.add(e.pos[0], e.pos[1], water, /*airborne=*/false);
        }
    } else {
        tick_entity->flags &= ~kEntityFlagDiveLatch;
        tick_entity->engine_flags &= ~kEntityFlagDiveLatch;
        e.pos[2] = surf;
    }

    // The entry splash, once, selected by the was-airborne bit; then the latch
    // `(Flags & ~0x2000) | 0x8000` -- swimming overrides airborne. The local
    // scope auto-untoggle that sits between them (@0x4b8304-0x4b8360) is
    // presentation and is not modeled here. [orig: @0x4b8182 / @0x4b8363]
    if (!was_afloat) {
        world.water_crossings.add(e.pos[0], e.pos[1], water,
                                  /*airborne=*/(flags & kEntityFlagInAir) != 0);
    }
    tick_entity->flags = (tick_entity->flags & ~kEntityFlagInAir) | kEntityFlagDrowning;
    tick_entity->engine_flags =
            (tick_entity->engine_flags & ~kEntityFlagInAir) | kEntityFlagDrowning;
    inf.airborne = false; // the motor-side mirror of the 0x2000 clear
}

} // namespace opennova::world
