// Guided class motors and local-frame pursuit. Positions/velocities are Q16,
// angles are BAM32. The round pool owns lifetime and position integration.
// [orig: 0x445CC0, 0x445DB0, 0x445EF0, 0x446060, 0x446690, 0x446BA0, 0x546B30]
#pragma once
#include <cstdint>
#include <base/io/tick_rate.h>

namespace opennova::world {
enum class GuidedFamily : uint8_t { None, Stinger, Hellfire, Javelin };
struct GuidedFlightState {
    int32_t pos[3] = {};
    int32_t yaw_bam = 0, pitch_bam = 0, roll_bam = 0;
    int32_t age = 0;
    int32_t velocity[3] = {};
    int32_t steer[3] = {}, saved[3] = {};
    uint16_t flags = 0, phase = 0; // entity+696/+698
    uint16_t target = 0xFFFF;
    int32_t timer = 0, initial_range = 0, previous_distance = INT32_MAX;
};
struct GuidedAmmo {
    int32_t speed = 0, max_pitch = 0, max_yaw = 0, tracking = 0;
};
// Inputs from live world services. These callbacks resolve poses at motor time,
// never by retaining the coordinate received in an earlier network packet.
struct GuidedInputs {
    bool authority = false, session = false, owner = true;
    bool owner_aim = false, owner_ai = false;
    uint16_t owner_target = 0xFFFF;
    int32_t aim[3] = {};
    bool target_present = false, target_alive = true;
    int32_t target_origin[3] = {};
    bool acquisition_ran = false, acquired = false, acquired_flare = false;
    uint16_t acquired_target = 0xFFFF;
    int32_t acquired_origin[3] = {};
};
struct GuidedMotorEvents {
    uint8_t groups = 0; // bit N requests serializer group N
    bool proximity = false, detonate = false;
    // The Stinger motor reached its lock-note site, after the pursuit and
    // ahead of the turn; the round tick notes the missile for the local
    // player's radar when the target is theirs (world/radar_contacts.h).
    bool threat_note = false;
};
enum class GuidedStepResult : uint8_t {
    kNone, kCoincident, kOvershoot, kGuard, kProximityNotify,
};
class GuidedFlight {
public:
    static constexpr int32_t kAgeGate = 31, kBoostFullAge = 39;
    static constexpr int32_t kTickDiv = io::kTicksPerSecondInt;
    static constexpr int32_t kTurnDefault = 6734910;
    static constexpr int32_t kOvershoot = 0x3FFFFFC0;
    static constexpr int32_t kSteerGuard = 0x8000, kProximity = 0x1900000;
    static void pursuit(const GuidedFlightState &, const int32_t steer[3], int32_t out[4]);
    static void turn(GuidedFlightState &, const int32_t *error, int32_t speed, const GuidedAmmo &);
    static void launch(GuidedFlightState &, GuidedFamily, const GuidedAmmo &, const GuidedInputs &);
    static GuidedMotorEvents motor(GuidedFlightState &, GuidedFamily, const GuidedAmmo &, const GuidedInputs &);
    // Convenience integration for standalone callers/tests; live rounds call motor.
    static GuidedStepResult step(GuidedFlightState &, const int32_t steer[3],
        int32_t velocity, int32_t max_pitch, int32_t max_yaw, bool authority);
    static void aim_at(GuidedFlightState &, const int32_t steer[3]);
    static int32_t wrap_bam(int64_t value);
};
} // namespace opennova::world
