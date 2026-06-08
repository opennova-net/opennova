// Portable sound-set member selection state machine.
//
// A NovaLogic sound set (.lwf Multi) has one or more layers, each holding a list of member
// sounds and a selection mode that decides which member plays when the set is triggered. This is
// the faithful port of that per-layer selection logic (originally the archive's sound_set.cpp),
// pulled out of the Godot host (godot/engine/world/nova_sound_bank.gd) so the engine core stays
// C++ and a headless server can resolve the same member without Godot. The host keeps the lwf data
// access and the AudioStreamPlayer spawning; this only decides WHICH member index plays.
//
// [orig: SoundProfile selection is name-keyed (SoundProfile_FindLoadedByName @ Jointops 0x5274f0);
//  the per-layer member pick is the sound_set member-selection mode. See notes/lwf/grill.md.]
#ifndef OPENNOVA_AUDIO_SOUND_SELECTOR_H
#define OPENNOVA_AUDIO_SOUND_SELECTOR_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace opennova::audio {

// Mirrors the .lwf selection_mode field (and NovaSoundBank.SELECTION_* / NovaLwfData).
enum SelectionMode {
    kFirst = 0,      // always the first member
    kRandom = 1,     // a uniformly random member each time
    kSequential = 2, // members in order, wrapping
    kRandomSeq = 3,  // a shuffled bag: random order, but every member once before repeating
};

// Per-(bank,set,layer) member selection. State (sequence cursor / shuffle bag) is kept per key, so
// one selector instance backs a whole loaded bank set. Deterministic for kFirst/kSequential; the
// random modes draw from a seeded LCG (its exact stream is not the original engine's RNG yet -- a
// grill-gated divergence -- but no member sequence is asset-faithful-tested, only the mode shape).
class SoundSelector {
public:
    // Pick a member index in [0, member_count) for the layer identified by `key`, or -1 when the
    // layer is empty. `mode` is a SelectionMode (unknown values fall back to kFirst).
    int select(uint64_t key, int member_count, int mode);

    // Drop all per-key state (sequence cursors + shuffle bags). The RNG stream is left running.
    void reset();

    // Compose the per-layer key the host addresses state by. Distinct (bank,set,layer) triples map
    // to distinct keys for the bank sizes the format allows.
    static uint64_t make_key(int bank, int set_index, int layer_index) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(bank)) << 42) ^
               (static_cast<uint64_t>(static_cast<uint32_t>(set_index)) << 21) ^
               static_cast<uint64_t>(static_cast<uint32_t>(layer_index));
    }

private:
    struct LayerState {
        uint32_t seq = 0;      // kSequential cursor
        std::vector<int> bag;  // kRandomSeq remaining-this-cycle members (drawn from the back)
    };

    uint32_t next_rand();

    std::unordered_map<uint64_t, LayerState> state_;
    uint32_t rng_ = 0x9E3779B9u; // fixed seed: reproducible across runs (golden-ratio constant)
};

} // namespace opennova::audio

#endif // OPENNOVA_AUDIO_SOUND_SELECTOR_H
