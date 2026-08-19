// Guided-missile flight integrator tests [orig: Entity_UpdateGuidedMissile_0
// @0x446060; Entity_ComputeGuidedPursuitError pursuit error; Entity_UpdateTurretAim @0x445CC0 turn
// clamp]. Pins the witnessed semantics: the 31-tick ignition hold, the boost
// ramp's integer-truncated ((velocity/divisor) << 16) / 62 speed, the per-axis
// BAM turn clamp (with the <=0 -> 6734910 default), the authority-only role
// split of the termination leg (overshoot marks but still advances, the steer
// guard returns before the advance, proximity is a non-terminal AI notify),
// and BAM short-way wrapping.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "world/guided_missile_flight.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t kUnit = 65536;

GuidedFlightState launch_at_origin() {
    GuidedFlightState st;
    st.pos[0] = 0; st.pos[1] = 0; st.pos[2] = 0;
    return st;
}

// The witnessed speed build [orig: ((velocity / divisor) << 16) / 0x3E
// @0x446572] — integer truncation before the shift.
int32_t speed_at(int32_t velocity, int32_t divisor) {
    return static_cast<int32_t>(
            (static_cast<int64_t>(velocity / divisor) << 16) / GuidedFlight::kTickDiv);
}

// Age gate: ticks 1..30 must not move the missile [orig: cmp 31 @0x446538].
void test_ignition_hold() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 1000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, steer);
    for (int i = 0; i < GuidedFlight::kAgeGate - 1; ++i) {
        CHECK(GuidedFlight::step(st, steer, 300, 0, 0, false) == GuidedStepResult::kNone);
        CHECK(st.pos[0] == 0 && st.pos[1] == 0 && st.pos[2] == 0);
    }
    CHECK(st.age == GuidedFlight::kAgeGate - 1);
    GuidedFlight::step(st, steer, 300, 0, 0, false);   // age 31 — first moving tick
    CHECK(st.pos[0] > 0);
}

// Boost ramp: divisor 39-age clamps to 1 at full boost, and the per-tick
// advance is the exact integer-truncated formula [orig: @0x446543..0x446572].
void test_boost_ramp_exact_speeds() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 20000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, steer);
    st.age = GuidedFlight::kBoostFullAge;   // past full boost -> divisor 1
    const int32_t before = st.pos[0];
    GuidedFlight::step(st, steer, 300, 0, 0, false);
    // Heading dead-ahead: the advance is the speed exactly.
    CHECK(st.pos[0] - before == speed_at(300, 1));
    CHECK(speed_at(300, 1) == (300 << 16) / 62);
    // At age 31 (divisor 8) the witnessed value truncates 300/8 -> 37 FIRST.
    GuidedFlightState early = launch_at_origin();
    GuidedFlight::aim_at(early, steer);
    early.age = GuidedFlight::kAgeGate - 1;
    GuidedFlight::step(early, steer, 300, 0, 0, false);   // age becomes 31, divisor 8
    CHECK(early.pos[0] == speed_at(300, 8));
    CHECK(speed_at(300, 8) == ((300 / 8) << 16) / 62);    // 37<<16/62 = 39110
    CHECK(speed_at(300, 8) == 39110);
}

// Turn clamp: a steer point 60 degrees off must rotate the heading by exactly
// the clamp per tick; <=0 uses the 6734910 default [orig: @0x445CC0].
void test_turn_clamp() {
    GuidedFlightState st = launch_at_origin();
    const int32_t ahead[3] = { 1000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, ahead);
    st.age = GuidedFlight::kBoostFullAge;
    const int32_t side[3] = { 1000 * kUnit, 1732 * kUnit, 0 };   // ~60 deg left
    const int32_t yaw_before = st.yaw_bam;
    CHECK(GuidedFlight::step(st, side, 300, 0, 0, false) == GuidedStepResult::kNone);
    const int64_t turned = static_cast<int64_t>(st.yaw_bam) - yaw_before;
    CHECK(std::llabs(turned) == GuidedFlight::kTurnDefault);
    // An explicit clamp overrides the default.
    GuidedFlightState st2 = launch_at_origin();
    GuidedFlight::aim_at(st2, ahead);
    st2.age = GuidedFlight::kBoostFullAge;
    const int32_t yaw2 = st2.yaw_bam;
    GuidedFlight::step(st2, side, 300, 123456, 123456, false);
    CHECK(std::llabs(static_cast<int64_t>(st2.yaw_bam) - yaw2) == 123456);
}

// Overshoot (authority): an error beyond ~90 degrees marks the detonation but
// the tick STILL advances — the block falls through to the advance
// [orig: @0x446484 sets the bits with no early return].
void test_overshoot_marks_and_still_advances() {
    GuidedFlightState st = launch_at_origin();
    const int32_t ahead[3] = { 1000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, ahead);
    st.age = GuidedFlight::kBoostFullAge;
    const int32_t behind[3] = { -1000 * kUnit, -5 * kUnit, 0 };
    const int32_t px = st.pos[0];
    CHECK(GuidedFlight::step(st, behind, 300, 0, 0, true) == GuidedStepResult::kOvershoot);
    CHECK(st.pos[0] != px);   // the advance still happened on the marking tick
    // A NON-authority client never evaluates the overshoot [orig: gate
    // @0x4463cb] — same geometry flies on.
    GuidedFlightState cl = launch_at_origin();
    GuidedFlight::aim_at(cl, ahead);
    cl.age = GuidedFlight::kBoostFullAge;
    CHECK(GuidedFlight::step(cl, behind, 300, 0, 0, false) == GuidedStepResult::kNone);
}

// Steer guard (authority): total steer distance under 0.5 units detonates
// BEFORE any advance [orig: @0x4464b7 returns]. Non-authority skips it.
void test_steer_guard() {
    GuidedFlightState st = launch_at_origin();
    const int32_t near_pt[3] = { GuidedFlight::kSteerGuard / 2, 0, 0 };
    GuidedFlight::aim_at(st, near_pt);
    st.age = GuidedFlight::kBoostFullAge;
    const int32_t px = st.pos[0];
    CHECK(GuidedFlight::step(st, near_pt, 300, 0, 0, true) == GuidedStepResult::kGuard);
    CHECK(st.pos[0] == px);   // no advance on the guard tick
    GuidedFlightState cl = launch_at_origin();
    GuidedFlight::aim_at(cl, near_pt);
    cl.age = GuidedFlight::kBoostFullAge;
    CHECK(GuidedFlight::step(cl, near_pt, 300, 0, 0, false) == GuidedStepResult::kNone);
}

// Proximity (authority): within 400 horizontal units the verdict is the AI
// notify — the tick advances and the missile KEEPS FLYING (it is not a
// detonation) [orig: @0x446587 -> AIEvent_QueueEntry(12)].
void test_proximity_notifies_without_terminating() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 399 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, steer);
    st.age = GuidedFlight::kBoostFullAge;
    CHECK(GuidedFlight::step(st, steer, 300, 0, 0, true) == GuidedStepResult::kProximityNotify);
    CHECK(st.pos[0] > 0);   // the advance happened
    // Still inside the notify radius next tick — still flying, still notifying.
    CHECK(GuidedFlight::step(st, steer, 300, 0, 0, true) == GuidedStepResult::kProximityNotify);
    // Non-authority: no notify surface at all.
    GuidedFlightState cl = launch_at_origin();
    GuidedFlight::aim_at(cl, steer);
    cl.age = GuidedFlight::kBoostFullAge;
    CHECK(GuidedFlight::step(cl, steer, 300, 0, 0, false) == GuidedStepResult::kNone);
}

// Convergence: flown from launch on the authority, the missile reaches the
// proximity-notify radius of a fixed steer point within a bounded tick count
// (the fork's smoke shape).
void test_converges_on_steer_point() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 800 * kUnit, 600 * kUnit, 50 * kUnit };
    GuidedFlight::aim_at(st, steer);
    GuidedStepResult r = GuidedStepResult::kNone;
    int ticks = 0;
    while (r == GuidedStepResult::kNone && ticks < 1200) {
        r = GuidedFlight::step(st, steer, 300, 0, 0, true);
        ++ticks;
    }
    CHECK(r == GuidedStepResult::kProximityNotify);
    CHECK(ticks < 400);
}

// BAM wrap: the short way across the seam.
void test_wrap_bam() {
    CHECK(GuidedFlight::wrap_bam(0x7FFFFFFFLL + 2LL) == static_cast<int32_t>(0x80000001u));
    CHECK(GuidedFlight::wrap_bam(-0x80000000LL - 2LL) == 0x7FFFFFFE);
    CHECK(GuidedFlight::wrap_bam(1234) == 1234);
}

} // namespace

int main() {
    test_ignition_hold();
    test_boost_ramp_exact_speeds();
    test_turn_clamp();
    test_overshoot_marks_and_still_advances();
    test_steer_guard();
    test_proximity_notifies_without_terminating();
    test_converges_on_steer_point();
    test_wrap_bam();
    if (failures == 0) std::printf("guided_missile_flight_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
