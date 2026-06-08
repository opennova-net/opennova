#include "audio/sound_selector.h"

namespace opennova::audio {

// Numerical Recipes LCG. Used only for the random selection modes; the stream is seeded to a fixed
// constant so a run is reproducible. (The original engine's selection RNG is not yet pinned.)
uint32_t SoundSelector::next_rand() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
}

int SoundSelector::select(uint64_t key, int member_count, int mode) {
    if (member_count <= 0) {
        return -1;
    }
    if (member_count == 1) {
        return 0; // one member: every mode collapses to it (and needs no state)
    }
    switch (mode) {
        case kRandom:
            return static_cast<int>(next_rand() % static_cast<uint32_t>(member_count));
        case kSequential: {
            LayerState &st = state_[key];
            int idx = static_cast<int>(st.seq % static_cast<uint32_t>(member_count));
            st.seq = static_cast<uint32_t>(idx) + 1u;
            return idx;
        }
        case kRandomSeq: {
            LayerState &st = state_[key];
            if (st.bag.empty()) {
                st.bag.resize(static_cast<size_t>(member_count));
                for (int i = 0; i < member_count; ++i) {
                    st.bag[static_cast<size_t>(i)] = i;
                }
                // Fisher-Yates shuffle (draw from the back in select()).
                for (int i = member_count - 1; i > 0; --i) {
                    int j = static_cast<int>(next_rand() % static_cast<uint32_t>(i + 1));
                    int tmp = st.bag[static_cast<size_t>(i)];
                    st.bag[static_cast<size_t>(i)] = st.bag[static_cast<size_t>(j)];
                    st.bag[static_cast<size_t>(j)] = tmp;
                }
            }
            int pick = st.bag.back();
            st.bag.pop_back();
            return pick;
        }
        case kFirst:
        default:
            return 0;
    }
}

void SoundSelector::reset() {
    state_.clear();
}

} // namespace opennova::audio
