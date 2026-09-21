#include "remote_body_state.h"

#include <algorithm>
#include <cmath>

namespace opennova::anim {

// [orig: NetPacket_SerializePlayerState @0x4c09c0, arbitration
// @0x4c1153..0x4c11a6; same-state jump @0x4c115f; infantry twin
// @0x4c0600..0x4c0641 (compare @0x4c0606, jump @0x4c0608)]
BodyArrival body_arrival(int current, int incoming, uint32_t current_flags,
                        uint32_t incoming_flags) {
    if (incoming < 0 || incoming == current) return BodyArrival::keep;
    if (current < 0) return BodyArrival::commit;
    return remote_body_state_defers(current_flags, incoming_flags)
            ? BodyArrival::queue : BodyArrival::commit;
}

bool remote_body_state_defers(uint32_t current_flags, uint32_t incoming_flags) {
    return (current_flags & 0x4u) != 0u ||
            ((current_flags & 0x20u) != 0u && (incoming_flags & 0x1u) == 0u);
}

BodyArrival RemoteBodyState::request(int state, uint32_t flags) {
    const auto decision = body_arrival(current_, state, flags_, flags);
    if (decision == BodyArrival::commit) {
        current_ = state;
        flags_ = flags;
        clear_pending();
    } else if (decision == BodyArrival::queue) {
        // A retarget changes the request, not the current clip's armed end.
        if (!has_pending()) clear_pending();
        pending_ = state;
        pending_flags_ = flags;
    }
    return decision;
}

void RemoteBodyState::reset() { *this = RemoteBodyState{}; }

void RemoteBodyState::clear_pending() {
    pending_ = 0;
    pending_flags_ = 0;
    completion_ = 0.0;
    completion_armed_ = false;
}

// [orig: AnimMap_UpdateEntity @0x40b5f0, end-notify arm
// @0x40b7ad/@0x40b7db; AnimChannel_AdvancePlayback @0x40b140,
// one-shot end @0x40b188..0x40b18f / loop wrap @0x40b19e..0x40b1b1]
void RemoteBodyState::arm_completion(double playhead, double length, bool looping) {
    if (!has_pending() || completion_armed_) return;
    completion_armed_ = true;
    completion_ = length <= 0.0 ? 0.0 :
            looping ? (std::floor(playhead / length) + 1.0) * length : length;
}

// [orig: AnimMap_UpdateEntity @0x40b795/@0x40b7c3, phase clear @0x40b7e4]
bool RemoteBodyState::promote_if_due(double playhead) {
    if (!has_pending() || !completion_armed_ || playhead + 0.000001 < completion_)
        return false;
    current_ = pending_;
    flags_ = pending_flags_;
    clear_pending();
    return true;
}

// [orig: AnimMap_UpdateEntity @0x40b656..0x40b65d; channel-init call @0x40b761]
void RemoteBodyState::begin_blend(int source_phase, double source_time,
                                 int target_phase, uint32_t target_flags) {
    if (!blending_) {
        source_phase_ = source_phase;
        source_time_ = source_time;
    }
    blending_ = true;
    target_phase_ = target_phase;
    weight_ = 0.0f;
    step_ = static_cast<float>((target_flags & 0x400u) != 0 ? 1.0 / 15.0 : 0.1);
}

void RemoteBodyState::clear_blend() {
    blending_ = false;
    source_phase_ = 0;
    source_time_ = 0.0;
    target_phase_ = 0;
    weight_ = 1.0f;
    step_ = 0.0f;
}

// Both phases advance before accumulating the float32 blend weight.
bool RemoteBodyState::advance_blend(int wire_state) {
    if (!blending_ || (wire_state != current_ && (!has_pending() || wire_state != pending_))) return false;
    ++source_phase_;
    ++target_phase_;
    weight_ = std::min(static_cast<float>(double(weight_) + double(step_)), 1.0f);
    return true;
}

} // namespace opennova::anim
