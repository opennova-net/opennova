// Guided-missile flight integrator tests [orig: Entity_UpdateGuidedMissile_0
// @0x446060 non-authority branch; sub_546B30 pursuit error; Entity_UpdateTurretAim
// @0x445CC0 turn clamp]. Pins the witnessed semantics: the 31-tick ignition hold,
// the 39-tick boost ramp reaching velocity<<16/62 at full boost, the per-axis BAM
// turn clamp (with the <=0 -> 6734910 default), the three termination tests and
// their before/after-advance ordering, and BAM short-way wrapping.
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

// Age gate: ticks 1..30 must not move the missile [orig: cmp 0x1f @0x446535].
void test_ignition_hold() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 1000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, steer);
    for (int i = 0; i < GuidedFlight::kAgeGate - 1; ++i) {
        CHECK(GuidedFlight::step(st, steer, 300, 0, 0) == GuidedDetonate::kNone);
        CHECK(st.pos[0] == 0 && st.pos[1] == 0 && st.pos[2] == 0);
    }
    CHECK(st.age == GuidedFlight::kAgeGate - 1);
    GuidedFlight::step(st, steer, 300, 0, 0);   // age 31 — first moving tick
    CHECK(st.pos[0] > 0);
}

// Boost ramp: at age >= 39 the divisor clamps to 1 and the per-tick advance is
// velocity * 65536 / 62 [orig: @0x44654f-0x446572, @0x446641].
void test_boost_ramp_reaches_full_speed() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 20000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, steer);
    st.age = GuidedFlight::kBoostFullAge;   // past full boost
    const int32_t before = st.pos[0];
    GuidedFlight::step(st, steer, 300, 0, 0);
    const int32_t full_step = static_cast<int32_t>(300.0 * 65536.0 / 62.0);
    const int32_t moved = st.pos[0] - before;
    CHECK(std::abs(moved - full_step) <= 2);
    // At age 31 (divisor 8) the step is one eighth of that.
    GuidedFlightState early = launch_at_origin();
    GuidedFlight::aim_at(early, steer);
    early.age = GuidedFlight::kAgeGate - 1;
    GuidedFlight::step(early, steer, 300, 0, 0);   // age becomes 31, divisor 8
    const int32_t early_step = early.pos[0];
    CHECK(std::abs(early_step - full_step / 8) <= 2);
}

// Turn clamp: a steer point 60 degrees off (inside the overshoot bound) must
// rotate the heading by exactly the clamp per tick; <=0 uses the 6734910
// default [orig: @0x445CC0]. (A full 90-degree error is ABOVE the overshoot
// bound 0x3FFFFFC0 and detonates instead — covered below.)
void test_turn_clamp() {
    GuidedFlightState st = launch_at_origin();
    const int32_t ahead[3] = { 1000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, ahead);
    st.age = GuidedFlight::kBoostFullAge;
    const int32_t side[3] = { 1000 * kUnit, 1732 * kUnit, 0 };   // ~60 deg left
    const int32_t yaw_before = st.yaw_bam;
    CHECK(GuidedFlight::step(st, side, 300, 0, 0) == GuidedDetonate::kNone);
    const int64_t turned = static_cast<int64_t>(st.yaw_bam) - yaw_before;
    CHECK(std::llabs(turned) == GuidedFlight::kTurnDefault);
    // An explicit clamp overrides the default.
    GuidedFlightState st2 = launch_at_origin();
    GuidedFlight::aim_at(st2, ahead);
    st2.age = GuidedFlight::kBoostFullAge;
    const int32_t yaw2 = st2.yaw_bam;
    GuidedFlight::step(st2, side, 300, 123456, 123456);
    CHECK(std::llabs(static_cast<int64_t>(st2.yaw_bam) - yaw2) == 123456);
}

// Overshoot: an error beyond 90 degrees detonates BEFORE any advance
// [orig: @0x44646c].
void test_overshoot_detonates_before_advance() {
    GuidedFlightState st = launch_at_origin();
    const int32_t ahead[3] = { 1000 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, ahead);
    st.age = GuidedFlight::kBoostFullAge;
    const int32_t behind[3] = { -1000 * kUnit, -5 * kUnit, 0 };
    const int32_t px = st.pos[0];
    CHECK(GuidedFlight::step(st, behind, 300, 0, 0) == GuidedDetonate::kOvershoot);
    CHECK(st.pos[0] == px);   // no advance on the detonating tick
}

// Steer guard: a steer point closer than 0.5 units detonates [orig: @0x4464b7].
void test_steer_guard() {
    GuidedFlightState st = launch_at_origin();
    const int32_t near_pt[3] = { GuidedFlight::kSteerGuard / 2, 0, 0 };
    GuidedFlight::aim_at(st, near_pt);
    st.age = GuidedFlight::kBoostFullAge;
    CHECK(GuidedFlight::step(st, near_pt, 300, 0, 0) == GuidedDetonate::kGuard);
}

// Proximity: within 400 units the tick still advances, then detonates
// [orig: @0x44657f].
void test_proximity_detonates_after_advance() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 399 * kUnit, 0, 0 };
    GuidedFlight::aim_at(st, steer);
    st.age = GuidedFlight::kBoostFullAge;
    CHECK(GuidedFlight::step(st, steer, 300, 0, 0) == GuidedDetonate::kProximity);
    CHECK(st.pos[0] > 0);   // the advance happened
}

// Convergence: flown from launch, the missile reaches proximity of a fixed
// steer point within a bounded tick count (the fork's smoke shape).
void test_converges_on_steer_point() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = { 800 * kUnit, 600 * kUnit, 50 * kUnit };
    GuidedFlight::aim_at(st, steer);
    GuidedDetonate det = GuidedDetonate::kNone;
    int ticks = 0;
    while (det == GuidedDetonate::kNone && ticks < 1200) {
        det = GuidedFlight::step(st, steer, 300, 0, 0);
        ++ticks;
    }
    CHECK(det == GuidedDetonate::kProximity);
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
    test_boost_ramp_reaches_full_speed();
    test_turn_clamp();
    test_overshoot_detonates_before_advance();
    test_steer_guard();
    test_proximity_detonates_after_advance();
    test_converges_on_steer_point();
    test_wrap_bam();
    if (failures == 0) std::printf("guided_missile_flight_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
