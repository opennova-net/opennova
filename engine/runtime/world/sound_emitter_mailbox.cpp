#include "world/sound_emitter_mailbox.h"

#include <algorithm>
#include <utility>

namespace opennova::world {

namespace {

bool same_key(const SoundEmitterEvent &a, const SoundEmitterEvent &b) {
    return a.source_spawn_id == b.source_spawn_id &&
           a.source_only == b.source_only &&
           (a.source_only || a.lane == b.lane);
}

int admission_priority(const SoundEmitterEvent &event) {
    // Clears and source-position updates do not allocate a retail emitter slot:
    // they operate on an already-live source. Preserve them at saturation by
    // displacing an allocating registration. A clear also outranks an anchor
    // because losing it can leave a stale voice alive for another lifetime.
    // [orig: SoundEmitter_ClearByEntityAndSlot @0x527a50; fixed emitter table
    // g_SoundEmitterSlots @0x24D66A8]
    if (event.source_only) return 1;
    if (event.pitch_q16 == 0 || event.volume_q8_8 == 0) return 2;
    return 0;
}

bool expired_at(const SoundEmitterEvent &event, uint32_t current_tick) {
    const uint32_t lifetime =
            std::max<uint32_t>(1, event.lifetime_ticks);
    return current_tick - event.emitted_tick > lifetime;
}

} // namespace

bool SoundEmitterMailbox::publish(SoundEmitterEvent event) {
    prune(event.emitted_tick);
    for (SoundEmitterEvent &pending : pending_) {
        if (same_key(pending, event)) {
            pending = std::move(event);
            return true;
        }
    }
    if (pending_.size() >= kCapacity) {
        const int incoming_priority = admission_priority(event);
        if (incoming_priority == 0) return false;

        // Replace the newest intent at the lowest lower priority, then append
        // the control at its true producer order. At most kCapacity controls
        // can target live retail slots, so the mailbox remains strictly bounded.
        auto victim = pending_.end();
        int victim_priority = incoming_priority;
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            const int priority = admission_priority(*it);
            if (priority < victim_priority ||
                (priority == victim_priority && priority < incoming_priority)) {
                victim = it;
                victim_priority = priority;
            }
        }
        if (victim == pending_.end()) return false;
        pending_.erase(victim);
    }
    pending_.push_back(std::move(event));
    return true;
}

std::vector<SoundEmitterEvent> SoundEmitterMailbox::drain() {
    std::vector<SoundEmitterEvent> out = std::move(pending_);
    pending_.clear();
    return out;
}

void SoundEmitterMailbox::prune(uint32_t current_tick) {
    pending_.erase(
            std::remove_if(pending_.begin(), pending_.end(),
                           [current_tick](const SoundEmitterEvent &event) {
                               return expired_at(event, current_tick);
                           }),
            pending_.end());
}

void SoundEmitterMailbox::clear() {
    pending_.clear();
}

} // namespace opennova::world
