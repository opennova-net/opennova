// Guided-missile flight integrator tests [orig: Entity_UpdateGuidedMissile_0
// @0x446060; Entity_ComputeGuidedPursuitError pursuit error; Entity_UpdateTurretAim @0x445CC0 turn
// clamp]. Pins the witnessed semantics: the 31-tick booster gate, the boost
// ramp's integer-truncated ((velocity/divisor) << 16) / 62 speed, the per-axis
// BAM turn clamp (with the <=0 -> 6734910 default), the authority-only role
// split of the termination leg (overshoot marks but still advances, the steer
// guard returns before the advance, proximity is a non-terminal AI notify),
// and BAM short-way wrapping.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include <runtime/world/guided_missile_flight.h>

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

// Ignition retains the launch velocity; the booster only replaces it at age 31.
// [orig: launch @0x445F28..0x445F64; motor age gate @0x446538]
void test_preignition_launch_velocity() {
    GuidedFlightState st = launch_at_origin();
    const int32_t steer[3] = {1000 * kUnit, 0, 0};
    GuidedInputs in;
    GuidedFlight::launch(st, GuidedFamily::Stinger, {300,0,0,0}, in);
    const int32_t launch_speed = speed_at(300,8);
    for (int i = 0; i < GuidedFlight::kAgeGate - 1; ++i) {
        CHECK(GuidedFlight::step(st,steer,300,0,0,false)==GuidedStepResult::kNone);
        CHECK(st.pos[0]==(i+1)*launch_speed);
    }
    CHECK(st.pos[0]>0);
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

// These expected bytes come from the original executable, not this implementation.
static void test_original_instruction_vectors() {
    static const int32_t vectors[][76] = {
#include "fixtures/guided_missile_vectors.inc"
    };
    for (size_t n = 0; n < std::size(vectors); ++n) {
        const int32_t *p = vectors[n];
        auto read = [&] { return *p++; };
        const int root = read();
        GuidedFlightState s; GuidedAmmo a; GuidedInputs in;
        for (auto &v : s.pos) v = read();
        s.yaw_bam = read(); s.pitch_bam = read(); s.roll_bam = read(); s.age = read();
        for (auto &v : s.velocity) v = read();
        for (auto &v : s.steer) v = read();
        for (auto &v : s.saved) v = read();
        s.flags = uint16_t(read()); s.phase = uint16_t(read()); s.target = uint16_t(read());
        s.timer = read(); s.initial_range = read(); s.previous_distance = read();
        a.speed = read(); a.max_pitch = read(); a.max_yaw = read(); a.tracking = read();
        in.authority = read()!=0; in.session = read()!=0; in.owner = read()!=0;
        in.owner_aim = read()!=0; in.owner_ai = read()!=0; in.owner_target = uint16_t(read());
        for (auto &v : in.aim) v = read();
        in.target_present = read()!=0; in.target_alive = read()!=0;
        for (auto &v : in.target_origin) v = read();
        in.acquisition_ran = read()!=0; in.acquired = read()!=0; in.acquired_flare = read()!=0;
        in.acquired_target = uint16_t(read());
        for (auto &v : in.acquired_origin) v = read();
        int32_t error[4]; for (auto &v : error) v = read();
        const int32_t forward = read(); const bool error_pointer = read()!=0;
        if (root == 0) GuidedFlight::pursuit(s, s.steer, error);
        else if (root == 1) GuidedFlight::turn(s, error_pointer ? error : nullptr, forward, a);
        else if (root <= 4) GuidedFlight::motor(s, GuidedFamily(root-1), a, in);
        else GuidedFlight::launch(s, root == 5 ? GuidedFamily::Stinger : GuidedFamily::Javelin, a, in);
        const int32_t actual[] = {s.yaw_bam,s.pitch_bam,s.roll_bam,
            s.velocity[0],s.velocity[1],s.velocity[2],s.steer[0],s.steer[1],s.steer[2],
            s.saved[0],s.saved[1],s.saved[2],s.flags,s.phase,s.target,s.timer,s.initial_range,s.previous_distance,
            error[0],error[1],error[2],error[3]};
        for (int i = root == 0 ? 18 : 0; i < (root == 0 ? 22 : 18); ++i)
            if (actual[i] != p[i]) { std::printf("oracle vector %zu field %d expected %d got %d\n",n,i,p[i],actual[i]); ++failures; }
    }
}

int main() {
    test_original_instruction_vectors();
    test_preignition_launch_velocity();
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
