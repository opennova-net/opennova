// [orig: Entity_UpdateGuidedMissile_0 @0x446060; Entity_ComputeGuidedPursuitError
// @0x546B30 (call @0x4463B5); Entity_UpdateTurretAim @0x445CC0]
#include <runtime/world/guided_missile_flight.h>
#include <base/io/bam.h>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace opennova::world {
namespace {
using io::bam_add;
using io::bam_sub;
// Retail dbl_7C3608, including its original scale error (30.5 ppm above the
// exact 2*pi/2^32).
constexpr double radians_per_bam = 1.4629627251502471e-9;
int32_t q16(int64_t value) { return int32_t(uint64_t(value + 0x8000) >> 16); }
int32_t q22(int64_t value) { return int32_t(uint64_t(value) >> 22); }
void copy3(int32_t *to, const int32_t *from) { std::copy_n(from, 3, to); }
// Same individually truncated Euler products as 0x613F40/0x615400.
void rotation(int32_t yaw, int32_t pitch, int32_t roll, int32_t m[9]) {
    const auto sine = [](int32_t a) { return int32_t(std::sin(a * radians_per_bam) * 4194304.0); };
    const auto cosine = [](int32_t a) { return int32_t(std::cos(a * radians_per_bam) * 4194304.0); };
    const int32_t sr = sine(roll), cr = cosine(roll), sp = sine(pitch), cp = cosine(pitch);
    const int32_t sy = sine(yaw), cy = cosine(yaw);
    const int32_t p1 = q22(int64_t(sr) * -sp), p2 = q22(int64_t(cr) * -sp);
    m[0] = q22(int64_t(cp) * cy);
    m[1] = bam_add(q22(int64_t(p1) * cy), q22(int64_t(cr) * -sy));
    m[2] = bam_add(q22(int64_t(p2) * cy), q22(int64_t(-sr) * -sy));
    m[3] = q22(int64_t(cp) * sy);
    m[4] = bam_add(q22(int64_t(p1) * sy), q22(int64_t(cr) * cy));
    m[5] = bam_add(q22(int64_t(p2) * sy), q22(int64_t(-sr) * cy));
    m[6] = sp; m[7] = q22(int64_t(sr) * cp); m[8] = q22(int64_t(cr) * cp);
}
// Retail dbl_7C19D8, bit-identical to io::kBamPerRadian (no scale error).
int32_t angle(int32_t y, int32_t x) {
    return int32_t(int64_t(std::atan2(double(y), double(x)) * io::kBamPerRadian));
}
// Booster gate @0x446538/@0x44663D, divisor @0x446543/@0x446644,
// lower clamp @0x446548, /62 @0x446572/@0x446677.
int32_t boost(const GuidedFlightState &s, const GuidedAmmo &a) {
    return int32_t(uint32_t(a.speed / std::max(1, 39 - s.age)) << 16) / 62;
}
int32_t ratio(int32_t distance, int32_t initial) {
    return !initial || initial == distance ? 65536 : int32_t((int64_t(distance) * 65536) / initial);
}
constexpr int32_t hellfire[5][2] = {{65536,39321},{36044,32768},{32768,26214},{19660,9830},{9830,0}};
constexpr int32_t javelin[5][2] = {{65536,6553},{58982,32768},{45875,39321},{19660,26214},{655,0}};
void track_saved(GuidedFlightState &s, const GuidedInputs &in) {
    if (s.target != 0xFFFF && in.target_present) copy3(s.saved, in.target_origin);
    copy3(s.steer, s.saved);
}
}
int32_t GuidedFlight::wrap_bam(int64_t v) { return int32_t(uint32_t(v)); }
void GuidedFlight::pursuit(const GuidedFlightState &s, const int32_t steer[3], int32_t out[4]) {
    int32_t m[9], local[3], delta[3];
    rotation(s.yaw_bam, s.pitch_bam, s.roll_bam, m);
    for (int i = 0; i < 3; ++i) delta[i] = bam_sub(steer[i], s.pos[i]);
    for (int i = 0; i < 3; ++i) {
        uint64_t sum = 0x200000;
        for (int j = 0; j < 3; ++j) sum += uint64_t(int64_t(m[j * 3 + i]) * delta[j]);
        local[i] = int32_t(sum >> 22);
    }
    const auto square = [](int32_t v) { return q16(int64_t(v >> 8) * (v >> 8)); };
    // FISTP uses the game's nearest-even control word, unlike the FTOL angles.
    const auto root = [](int32_t v) { return v < 0 ? 0 : int32_t(uint32_t(int32_t(std::nearbyint(std::sqrt(double(v))))) << 16); };
    const int32_t xy = bam_add(square(local[0]), square(local[1]));
    out[2] = root(xy); out[3] = root(bam_add(xy, square(local[2])));
    out[0] = angle(local[2], out[2]); out[1] = angle(local[1], local[0]);
}
// Defaults @0x445CCD/@0x445CD1; rotate [speed,0,0] @0x445D70..0x445D8C.
void GuidedFlight::turn(GuidedFlightState &s, const int32_t *error, int32_t speed, const GuidedAmmo &a) {
    const int32_t p = a.max_pitch > 0 ? a.max_pitch : kTurnDefault;
    const int32_t y = a.max_yaw > 0 ? a.max_yaw : kTurnDefault;
    // ABS(INT_MIN) remains negative on x86 and therefore bypasses the clamp.
    const auto clamp = [](int32_t v, int32_t limit) { return io::bam_abs(v) > limit ? (v > 0 ? limit : -limit) : v; };
    s.pitch_bam = bam_add(s.pitch_bam, error ? clamp(error[0], p) : 0);
    s.yaw_bam = bam_add(s.yaw_bam, error ? clamp(error[1], y) : 0);
    int32_t m[9]; rotation(s.yaw_bam, s.pitch_bam, 0, m);
    for (int i = 0; i < 3; ++i) s.velocity[i] = q22(int64_t(m[i * 3]) * speed + 0x200000);
}
void GuidedFlight::launch(GuidedFlightState &s, GuidedFamily family, const GuidedAmmo &a, const GuidedInputs &in) {
    if (family == GuidedFamily::Javelin) {
        s.target = in.owner_target;
        if (s.target != 0xFFFF && in.target_present) {
            copy3(s.steer, in.target_origin); copy3(s.saved, s.steer);
        } else if (in.owner_ai && in.owner_aim) {
            copy3(s.steer, in.aim); copy3(s.saved, s.steer);
        }
        s.phase = 0;
    } else if (family == GuidedFamily::Stinger && in.owner_ai)
        s.target = in.acquisition_ran ? (in.acquired ? in.acquired_target : 0xFFFF) : in.owner_target;
    if (family == GuidedFamily::Javelin || family == GuidedFamily::Stinger)
        turn(s, nullptr, int32_t(uint32_t(a.speed / 8) << 16) / 62, a);
    if (family == GuidedFamily::Stinger) s.timer = in.session && !in.authority ? -1 : 31;
}
// Death early-out @0x44607A; track loss @0x44645A; proximity/termination
// @0x4464C2. Flare clear/send @0x4462EC..0x446324 uses the shell identity
// predicate Entity_IsShellProjectile @0x4E4040 at the acquisition boundary.
GuidedMotorEvents GuidedFlight::motor(GuidedFlightState &s, GuidedFamily family, const GuidedAmmo &a, const GuidedInputs &in) {
    GuidedMotorEvents ev;
    if (family == GuidedFamily::None) return ev;
    if (family != GuidedFamily::Stinger && !in.owner) s.flags |= 1;
    if (s.flags & 1) return ev;
    const auto emit = [&](int group) { if (in.session) ev.groups |= uint8_t(1u << group); };
    const auto detonate = [&] { s.flags |= 1; ev.detonate = true; if (in.authority) emit(1); };
    int32_t err[4] = {};
    if (family == GuidedFamily::Stinger) {
        if (s.timer > 0) --s.timer;
        if (s.age < 2) {
            if (s.flags & 4) return ev;
            s.flags |= 4;
            if (in.owner_aim) { copy3(s.steer, in.aim); s.target = in.owner_target; }
            if (s.target != 0xFFFF) s.flags |= 2;
            s.timer = (!in.session || in.authority) ? ((s.flags & 2) ? 31 : 62) : -1;
            if (in.session && in.authority && !(s.flags & 2)) s.timer = -1;
            return ev;
        }
        if ((s.flags & 2) && s.target != 0xFFFF && (!in.target_present || !in.target_alive)) {
            s.flags &= ~2u; s.target = 0xFFFF; s.timer = in.authority ? 0 : -1;
            if (in.authority) emit(2);
        }
        if (in.authority && s.timer == 0 && in.acquisition_ran) {
            s.timer = 62; s.target = in.acquired ? in.acquired_target : 0xFFFF;
            if (in.acquired) {
                if (in.acquired_flare) { copy3(s.steer, in.acquired_origin); s.target = 0xFFFF; s.timer = 31; emit(5); }
                else { s.flags |= 2; emit(3); }
            }
        }
        if (s.target == 0xFFFF) s.flags &= ~2u;
        if ((s.flags & 2) && in.target_present) copy3(s.steer, in.target_origin);
        else if (!s.steer[0] && !s.steer[1] && !s.steer[2]) {
            if (s.age >= 31) turn(s, nullptr, boost(s, a), a);
            return ev;
        }
        pursuit(s, s.steer, err);
        if (!in.session || in.authority) {
            if (io::bam_abs(err[0]) > 0x16C16C0 || io::bam_abs(err[1]) > 0x16C16C0) s.timer = 31;
            const uint32_t track = uint32_t(io::bam_abs(int32_t(uint32_t(a.tracking) * 8u)));
            if ((s.flags & 2) && (uint32_t(io::bam_abs(err[0])) > track || uint32_t(io::bam_abs(err[1])) > track)) {
                s.flags &= ~2u; s.timer = 0; emit(2);
            }
            if (io::bam_abs(err[0]) > kOvershoot || io::bam_abs(err[1]) > kOvershoot) detonate();
            s.previous_distance = err[3];
            if (err[3] < kSteerGuard) { detonate(); return ev; }
            ev.proximity = err[2] < kProximity;
        }
        if (s.age >= 31) turn(s, err, boost(s, a), a);
        return ev;
    }
    if (family == GuidedFamily::Hellfire) {
        if (s.phase == 0 && s.timer < 6) ++s.timer;
        else {
            if (s.phase == 0) {
                if (!(s.flags & 4)) {
                    s.flags |= 4;
                    if (in.owner_ai && in.owner_target != 0xFFFF && in.target_present) {
                        s.flags |= 2; s.target = in.owner_target; copy3(s.steer, in.target_origin);
                    } else if (in.owner_aim) { copy3(s.steer, in.aim); s.target = in.owner_target; }
                    pursuit(s, s.steer, err);
                    if (!err[2]) { detonate(); return ev; }
                    s.initial_range = std::clamp(err[2], 300 * 65536, 1000 * 65536);
                    copy3(s.saved, s.steer);
                }
                emit(6); s.flags |= 8; s.phase = 1;
            }
            if (s.phase == 1 || s.phase == 2 || s.phase == 3) {
                if (s.phase < 3) copy3(s.steer, s.saved);
                else if (in.owner_aim) { copy3(s.steer, in.aim); s.target = in.owner_target; }
                s.steer[2] = bam_add(s.steer[2], q16(int64_t(s.initial_range) * hellfire[s.phase][1]));
                pursuit(s, s.steer, err);
                if (ratio(err[2], s.initial_range) < hellfire[s.phase][0]) {
                    ++s.phase; s.flags |= 8;
                    if (s.phase == 4) { s.previous_distance = INT32_MAX; s.timer = 0; }
                }
            }
        }
        if (s.flags & 8) { s.flags &= ~8u; emit(4); }
        if (s.phase >= 4 && in.owner_aim) { copy3(s.steer, in.aim); s.target = in.owner_target; }
    } else {
        if (!s.phase && !(s.flags & 4)) {
            s.flags |= 4;
            if (s.target != 0xFFFF && in.target_present) { s.flags |= 2; copy3(s.steer, in.target_origin); }
            pursuit(s, s.steer, err);
            if (!err[2]) { detonate(); return ev; }
            s.initial_range = std::clamp(err[2], 75 * 65536, 1000 * 65536);
            copy3(s.saved, s.steer);
        }
        track_saved(s, in);
        if (s.phase < 4) {
            s.steer[2] = bam_add(s.steer[2], q16(int64_t(s.initial_range) * javelin[s.phase][1]));
            pursuit(s, s.steer, err);
            if (ratio(err[2], s.initial_range) < javelin[s.phase][0]) {
                ++s.phase;
                track_saved(s, in);
                s.steer[2] = bam_add(s.steer[2], q16(int64_t(s.initial_range) * javelin[s.phase][1]));
                pursuit(s, s.steer, err);
                if (s.phase == 4) { s.previous_distance = INT32_MAX; s.timer = 0; }
            }
        }
        s.flags &= ~8u;
    }
    if (s.phase >= 4) {
        pursuit(s, s.steer, err);
        if ((io::bam_abs(err[0]) > 1193046400 || io::bam_abs(err[1]) > 1193046400) && s.previous_distance < err[3]) detonate();
        s.previous_distance = err[3];
    }
    const int32_t maximum = q16(int64_t(98304) * (int32_t(uint32_t(a.speed) << 16) / 62));
    double magnitude = 0;
    for (int32_t v : s.velocity) magnitude += double(v) * v;
    const int32_t speed = std::clamp(int32_t(std::min(std::sqrt(magnitude), 2147418112.0)), q16(int64_t(maximum) * 16384), maximum);
    turn(s, err, speed, a);
    s.velocity[2] = bam_sub(s.velocity[2], 167);
    return ev;
}
void GuidedFlight::aim_at(GuidedFlightState &s, const int32_t steer[3]) {
    const int32_t dx = bam_sub(steer[0], s.pos[0]), dy = bam_sub(steer[1], s.pos[1]), dz = bam_sub(steer[2], s.pos[2]);
    s.yaw_bam = angle(dy, dx);
    s.pitch_bam = angle(dz, int32_t(std::sqrt(double(dx) * dx + double(dy) * dy)));
}
GuidedStepResult GuidedFlight::step(GuidedFlightState &s, const int32_t steer[3], int32_t speed, int32_t p, int32_t y, bool authority) {
    ++s.age; copy3(s.steer, steer);
    GuidedInputs in; in.authority = authority; in.session = !authority;
    const auto ev = motor(s, GuidedFamily::Stinger, {speed,p,y,0}, in);
    if (ev.detonate && s.previous_distance < kSteerGuard) return GuidedStepResult::kGuard;
    for (int i = 0; i < 3; ++i) s.pos[i] = bam_add(s.pos[i], s.velocity[i]);
    return ev.detonate ? GuidedStepResult::kOvershoot : ev.proximity ? GuidedStepResult::kProximityNotify : GuidedStepResult::kNone;
}
} // namespace opennova::world
