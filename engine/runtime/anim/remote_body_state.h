#pragma once

#include <cstdint>

namespace opennova::anim {

enum class BodyArrival { keep, queue, commit };

// Shared receive/presentation arbitration. A repeated current state is a
// no-op, including its pending request and one-shot phase seed.
BodyArrival body_arrival(int current, int incoming, uint32_t current_flags,
                        uint32_t incoming_flags);

// The remote body-state queue gate over two per-state flags words
// [orig: the queue classes @0x4c1169..0x4c1190 / @0x4c060a..0x4c0633].
bool remote_body_state_defers(uint32_t current_flags, uint32_t incoming_flags);

// Numeric playback state for a raw-request presentation channel. Clip names,
// clip lookup and pose writes belong to the device adapter. Its current
// playhead is supplied explicitly; the receive-record owner uses body_arrival
// with its own tick-domain channel and spawn/death gates.
class RemoteBodyState {
public:
    BodyArrival request(int state, uint32_t flags);
    void reset();
    void clear_pending();
    void arm_completion(double playhead, double length, bool looping);
    bool promote_if_due(double playhead);

    void begin_blend(int source_phase, double source_time, int target_phase,
                     uint32_t target_flags);
    void clear_blend();
    // False for an inactive blend or an unrelated wire state. Completion is
    // observed through weight() before the caller consumes and clears it.
    bool advance_blend(int wire_state);
    void set_source_time(double time) { source_time_ = time; }

    int current() const { return current_; }
    uint32_t flags() const { return flags_; }
    int pending() const { return pending_; }
    bool has_pending() const { return pending_ != 0; }
    bool blending() const { return blending_; }
    bool needs_tick() const { return blending_ || has_pending(); }
    int source_phase() const { return source_phase_; }
    double source_time() const { return source_time_; }
    int target_phase() const { return target_phase_; }
    float weight() const { return weight_; }

private:
    int current_ = -1;
    uint32_t flags_ = 0;
    int pending_ = 0;
    uint32_t pending_flags_ = 0;
    double completion_ = 0.0;
    bool completion_armed_ = false;
    bool blending_ = false;
    int source_phase_ = 0;
    double source_time_ = 0.0;
    int target_phase_ = 0;
    float weight_ = 1.0f;
    float step_ = 0.0f;
};

} // namespace opennova::anim
