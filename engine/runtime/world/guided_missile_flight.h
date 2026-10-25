// Guided class motors and local-frame pursuit. Positions/velocities are Q16,
// angles are BAM32. The round pool owns lifetime and position integration.
// [orig: 0x445CC0, 0x445DB0, 0x445EF0, 0x446060, 0x446690, 0x446BA0, 0x546B30]
#pragma once
#include <cstdint>
#include <vector>
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
    // +0x2D4 holding a round (a RoundSim::rounds slot; -1 none): `target` then reads
    // 0xFFFF, the word the wire's pool walk writes for it (D-NET-64).
    int32_t target_round = -1;
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
    int32_t owner_target_round = -1; // the owner's +0xC word holds a round (owner_target 0xFFFF)
    int32_t aim[3] = {};
    bool target_present = false, target_alive = true;
    int32_t target_origin[3] = {};
    bool acquisition_ran = false, acquired = false, acquired_flare = false;
    uint16_t acquired_target = 0xFFFF;
    int32_t acquired_round = -1; // the query's winner is a round (a ground flare)
    int32_t acquired_origin[3] = {};
    // The acquired target's in-flight sample, which the steer takes once the acquisition
    // stores it; target_present / target_alive / target_origin stay the held target's,
    // whose liveness is tested first. Unset: the steer reads target_origin.
    bool acquired_present = false;
    int32_t acquired_sample[3] = {};
};
// One send of a guided motor: the serializer group and the round's words as they stand
// at the call, since the send serializes the round then and there.
// [orig: Entity_SendEffectPacket @0x445C40 (NetPacket_SerializeEntityForServer called
//  @0x445C6C)]
struct GuidedSend {
    uint8_t group = 0;
    GuidedFlightState state;
};
struct GuidedMotorEvents {
    // The motor's sends in call order, each serialized at its call: one Stinger pass can
    // send group 2, 3, then 2 again (the liveness loss, the acquisition, the track loss)
    // and group 1 twice (the overshoot, then the steer guard).
    // [orig: Entity_UpdateGuidedMissile_0 @0x446060 (the calls @0x4461E6, @0x446324 /
    //  @0x446345, @0x44645A, @0x4464AE, @0x4464F8); Entity_UpdateGuidedMissile @0x446690
    //  (the calls @0x4467F7, @0x44685C, @0x4469F9, @0x446AAB)]
    std::vector<GuidedSend> sends;
    bool proximity = false, detonate = false;
    // The Stinger motor reached its lock-note site, after the pursuit and
    // ahead of the turn; the round tick notes the missile for the local
    // player's radar when the target is theirs (world/radar_contacts.h).
    bool threat_note = false;
};
// One send of the authority's guided motor: an S2C 0x44 carrying the shooter's and the
// round's handles, the group, then the group's fields from `state` (the round at the
// call), mask 0x90. Each is queued at the motor's call, so the round pass's sends keep
// their order and their place among the entity pass's other inline records
// (WorldOutbox::entity_events) and lead the next frame's queue.
// [orig: Entity_SendEffectPacket @0x445C40 (mask 0x90 @0x445C98, NapiNPServer_SendFiltered
//  called @0x445CA2) from Entity_UpdateGuidedMissile_0 @0x446060 / Entity_UpdateGuidedMissile
//  @0x446690; Weapon_UpdateAllProjectiles called @0x4C223A in Entity_UpdateAllEntities]
struct GuidedUpdateEvent {
    uint16_t shooter = 0, net_id = 0;
    uint8_t group = 0;
    GuidedFlightState state;
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
