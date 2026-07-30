// Bounded transport for entity-attached persistent sound intents.
#ifndef OPENNOVA_WORLD_SOUND_EMITTER_MAILBOX_H
#define OPENNOVA_WORLD_SOUND_EMITTER_MAILBOX_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "world/geom.h"

namespace opennova::world {

// One registration into the shared entity-attached sound-emitter table. Unlike a
// one-shot SoundSlotEvent, this is a short-lived persistent voice intent:
// producers refresh the same (source_spawn_id, lane, LWF layer) every physics
// tick and the host lets an unrefreshed row expire. A source-only row updates
// the live spatial anchor of existing lanes without refreshing their lifetimes.
// Pitch or volume zero is the explicit source+lane clear operation.
// [orig: SoundEmitter_Register @0x529270 -> SoundEmitter_RegisterSetLayers @0x528340]
struct SoundEmitterEvent {
    uint64_t source_spawn_id = 0; // registry lifetime serial; survives handle reuse safely
    uint16_t source_handle = 0xFFFF;
    Vec3 pos{};                   // mission frame; the Godot adapter axis-maps on drain
    int32_t source_bms_id = 0;    // occlusion source identity
    uint32_t emitted_tick = 0;    // post-producer world clock; preserves catch-up chronology
    uint8_t lane = 0;             // original slot-type byte (vehicle: 0/10/20)
    uint8_t slot = 0;             // authored SoundProfileSlot, for observability
    uint16_t lifetime_ticks = 0;
    int32_t pitch_q16 = 0;
    uint16_t volume_q8_8 = 0;     // high byte is the emitter volume consumed by the mixer
    bool source_only = false;
    std::string set_name;         // name-keyed LWF lookup; empty on clear/source-only rows
};

// A latest-intent mailbox rather than an append-only event log. The final
// registration per (source lifetime, lane) is sufficient to reconstruct the
// emitter table at the next render: it carries both its producer tick and its
// keep-alive. Coalescing also prevents catch-up batches and audio-less/headless
// hosts from accumulating one heap-owning row per vehicle per tick.
//
// Capacity deliberately matches retail's 767 transient emitter slots. A full
// mailbox still accepts keyed refreshes. New allocating keys drop like a retail
// registration when no slot is available, while non-allocating clears/source
// anchors displace an allocation intent so stale live state cannot survive
// solely because the transport saturated.
class SoundEmitterMailbox {
public:
    static constexpr size_t kCapacity = 767;

    bool publish(SoundEmitterEvent event);
    std::vector<SoundEmitterEvent> drain();

    // Retire intents that no longer matter even if no presentation host drains
    // them. Unsigned tick subtraction preserves normal uint32 wrap behavior.
    void prune(uint32_t current_tick);
    void clear();

    size_t size() const { return pending_.size(); }
    bool empty() const { return pending_.empty(); }
    const SoundEmitterEvent &operator[](size_t index) const {
        return pending_[index];
    }
    const std::vector<SoundEmitterEvent> &pending() const { return pending_; }

private:
    std::vector<SoundEmitterEvent> pending_;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_SOUND_EMITTER_MAILBOX_H
