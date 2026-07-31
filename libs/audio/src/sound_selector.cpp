#include "audio/sound_selector.h"

namespace opennova::audio {

namespace {

inline uint32_t rol32(uint32_t v, unsigned bits) {
    return (v << bits) | (v >> (32u - bits));
}

} // namespace

// The engine's shared sound RNG: state = ROL32(state + ROL32(state, 11), 3), pick scaled by the
// low byte. One stream for every layer, never reseeded.
// [orig: PRNG_ScaledRandom @ 0x75be50; inline copies at 0x75cdfc / 0x75c025 / 0x75c220]
uint32_t SoundSelector::scaled_random(uint32_t count) {
    rng_ = rol32(rng_ + rol32(rng_, 11), 3);
    return (count * (rng_ & 0xFFu)) >> 8;
}

// Structural translation of the engine's per-layer member pick
// [orig: SoundBank_PlayTriggerEntries @ 0x75cd5c..0x75ce16]:
//   if (flags & 0x10)      -> sequential: play cursor, advance, wrap to 0;
//   else if (!(flags & 0x80)) -> random: scaled_random(count);
//   else if (in_cycle)     -> play cursor, advance+wrap, cycle ends when cursor returns to anchor;
//   else                   -> new random anchor: play it, cursor = anchor, cycle starts.
// Quirk preserved: starting a random-sequential cycle leaves cursor == anchor, so the anchor
// member plays twice in a row (once from the anchor branch, once as the first cycle step) --
// exactly what the engine does.
int SoundSelector::select(uint64_t key, int member_count, int mode) {
    if (member_count <= 0) {
        return -1;
    }
    const uint32_t count = static_cast<uint32_t>(member_count);
    switch (mode) {
        case kFirst:
            // Reimpl extension for deterministic authoring/preview; the engine has no such mode.
            return 0;
        case kSequential: {
            LayerState &st = state_[key];
            if (st.seq >= count) {
                st.seq = 0; // reimpl safety: a shrunk layer must not index out of range
            }
            const uint32_t idx = st.seq;
            st.seq = (st.seq + 1u >= count) ? 0u : st.seq + 1u; // [orig: @ 0x75cd6f..0x75cd79]
            return static_cast<int>(idx);
        }
        case kRandomSeq: {
            LayerState &st = state_[key];
            if (!st.in_cycle) {
                // [orig: @ 0x75cd93..0x75cdaa] new anchor via PRNG_ScaledRandom; cursor = anchor.
                st.anchor = scaled_random(count);
                if (st.anchor >= count) {
                    st.anchor = count - 1u; // unreachable for count <= 256; defensive
                }
                st.seq = st.anchor;
                st.in_cycle = true;
                return static_cast<int>(st.anchor);
            }
            // [orig: @ 0x75cdb3..0x75cddf] play cursor, advance+wrap; cycle completes when the
            // cursor comes back around to the anchor.
            if (st.seq >= count) {
                st.seq = 0; // reimpl safety
            }
            const uint32_t idx = st.seq;
            st.seq = (st.seq + 1u >= count) ? 0u : st.seq + 1u;
            if (st.seq == st.anchor) {
                st.in_cycle = false;
            }
            return static_cast<int>(idx);
        }
        case kRandom:
        default:
            // The engine default: any layer without the sequential (0x10) or random-sequential
            // (0x80) flag picks scaled-random; flag 0x08 is never tested [orig: @ 0x75cdfc].
            return static_cast<int>(scaled_random(count));
    }
}

void SoundSelector::reset() {
    state_.clear();
}

} // namespace opennova::audio
