// Portable sound-set member selection state machine.
//
// A NovaLogic sound set (.lwf Multi) has one or more layers, each holding a list of member
// sounds and selection flags that decide which member plays when the set is triggered. This is
// the faithful port of the engine's per-layer member selection, pulled out of the Godot layer
// (godot/engine/world/nova_sound_bank.gd) so the engine core stays C++ and a headless server can
// resolve the same member without Godot. The embedder keeps the lwf data access and the
// AudioStreamPlayer spawning; this only decides WHICH member index plays.
//
// Grilled vs Jointops.exe 2026-06-09 (see docs/audio/lwf-dbf-sound-re.md):
// [orig: SoundBank_PlayTriggerEntries @ 0x75ccd0 / SoundBank_SelectTriggerEntryFromBank @ 0x75bf20]
// - flags & 0x10 -> sequential cursor with wrap;
// - flags & 0x80 -> random anchor, then one full in-order cycle back to the anchor;
// - anything else -> uniformly random (flag 0x08 is never tested; random is the default).
// [orig: PRNG_ScaledRandom @ 0x75be50] one global ROL-LCG stream drives every pick:
//   state = ROL32(state + ROL32(state, 11), 3); index = (count * (state & 0xFF)) >> 8;
// seeded statically with 0x2B0749C1 [orig: .data @ 0x85A3DC] and never reseeded.
#ifndef OPENNOVA_AUDIO_SOUND_SELECTOR_H
#define OPENNOVA_AUDIO_SOUND_SELECTOR_H

#include <cstdint>
#include <unordered_map>

namespace opennova::audio {

// Mirrors the .lwf selection flags (and NovaSoundBank.SELECTION_* / NovaLwfData).
// kFirst has no engine equivalent (the engine default is kRandom); it is kept as a
// deterministic authoring/preview mode for the editor.
enum SelectionMode {
    kFirst = 0,      // always the first member (reimpl extension, not engine behavior)
    kRandom = 1,     // a scaled-random member each time (the engine DEFAULT for unflagged layers)
    kSequential = 2, // members in order, wrapping (flag 0x10)
    kRandomSeq = 3,  // random anchor, then a full in-order cycle back to the anchor (flag 0x80)
};

// Per-(bank,set,layer) member selection. State (sequence cursor / cycle anchor) is kept per key,
// so one selector instance backs a whole loaded bank set. The RNG is one shared stream across all
// keys, exactly like the engine's global @ 0x85A3DC. Caveat: the engine interleaves volume/pitch
// jitter draws on the same stream during playback; the reimpl does not reproduce those draws, so
// long-run streams diverge from a real game session even though the algorithm and seed match.
class SoundSelector {
public:
    // Pick a member index in [0, member_count) for the layer identified by `key`, or -1 when the
    // layer is empty. `mode` is a SelectionMode; unknown values behave like kRandom, matching the
    // engine's "no selection flags -> random" default [orig: @ 0x75cdfc].
    int select(uint64_t key, int member_count, int mode);

    // Drop all per-key state (sequence cursors + cycle anchors). The RNG stream is left running;
    // the engine never reseeds it across bank reloads.
    void reset();

    // Compose the per-layer key the embedder addresses state by. Distinct (bank,set,layer) triples map
    // to distinct keys for the bank sizes the format allows.
    static uint64_t make_key(int bank, int set_index, int layer_index) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(bank)) << 42) ^
               (static_cast<uint64_t>(static_cast<uint32_t>(set_index)) << 21) ^
               static_cast<uint64_t>(static_cast<uint32_t>(layer_index));
    }

private:
    struct LayerState {
        uint32_t seq = 0;       // kSequential cursor (next index to play)
        uint32_t anchor = 0;    // kRandomSeq cycle anchor [orig: layer word +12 @ 0x75cd9b]
        bool in_cycle = false;  // kRandomSeq "cycle running" runtime flag [orig: flags bit 0x100 @ 0x75cdaa]
    };

    // Advance the shared ROL-LCG and return a member index scaled into [0, count).
    // [orig: PRNG_ScaledRandom @ 0x75be50: (count * low_byte(state)) >> 8]
    uint32_t scaled_random(uint32_t count);

    std::unordered_map<uint64_t, LayerState> state_;
    uint32_t rng_ = 0x2B0749C1u; // the engine's static seed [orig: .data initializer @ 0x85A3DC, no runtime reseed]
};

} // namespace opennova::audio

#endif // OPENNOVA_AUDIO_SOUND_SELECTOR_H
