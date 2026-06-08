// opennova::audio::SoundSelector: the sound-set member-selection state machine pushed down from the
// Godot host (nova_sound_bank.gd _pick_member). Verifies the mode shapes (FIRST/SEQUENTIAL/RANDOM/
// RANDOM_SEQ), per-key independence, and edge cases. The random modes' exact stream is not asset-
// faithful (grill-gated), so they are asserted by shape (in-range; bag is a permutation), not value.
#include <set>
#include <vector>

#include "audio/sound_selector.h"
#include "common/test_expect.h"

using opennova::audio::SoundSelector;
namespace mode = opennova::audio;

int main() {
    // Empty layer -> no member.
    {
        SoundSelector s;
        TEST_EXPECT(s.select(SoundSelector::make_key(0, 0, 0), 0, mode::kFirst) == -1);
        TEST_EXPECT(s.select(SoundSelector::make_key(0, 0, 0), -3, mode::kRandom) == -1);
    }

    // One member: every mode returns it, with no state needed.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(1, 2, 3);
        for (int m = mode::kFirst; m <= mode::kRandomSeq; ++m) {
            TEST_EXPECT(s.select(k, 1, m) == 0);
        }
    }

    // FIRST: always index 0.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        for (int i = 0; i < 8; ++i) {
            TEST_EXPECT(s.select(k, 5, mode::kFirst) == 0);
        }
    }

    // SEQUENTIAL: 0,1,2,0,1,2 wrapping; an unknown mode falls back to FIRST.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        const int expect[6] = {0, 1, 2, 0, 1, 2};
        for (int i = 0; i < 6; ++i) {
            TEST_EXPECT(s.select(k, 3, mode::kSequential) == expect[i]);
        }
        TEST_EXPECT(s.select(SoundSelector::make_key(9, 9, 9), 3, 999) == 0); // unknown -> FIRST
    }

    // Per-key independence: two layers keep separate sequential cursors.
    {
        SoundSelector s;
        const uint64_t a = SoundSelector::make_key(0, 0, 0);
        const uint64_t b = SoundSelector::make_key(0, 0, 1);
        TEST_EXPECT(s.select(a, 4, mode::kSequential) == 0);
        TEST_EXPECT(s.select(a, 4, mode::kSequential) == 1);
        TEST_EXPECT(s.select(b, 4, mode::kSequential) == 0); // b starts fresh
        TEST_EXPECT(s.select(a, 4, mode::kSequential) == 2); // a continues
    }

    // reset() clears the cursor back to the start.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        TEST_EXPECT(s.select(k, 3, mode::kSequential) == 0);
        TEST_EXPECT(s.select(k, 3, mode::kSequential) == 1);
        s.reset();
        TEST_EXPECT(s.select(k, 3, mode::kSequential) == 0);
    }

    // RANDOM: every pick is in range, and over many draws it is not stuck on one value.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        std::set<int> seen;
        for (int i = 0; i < 200; ++i) {
            int idx = s.select(k, 6, mode::kRandom);
            TEST_EXPECT(idx >= 0 && idx < 6);
            seen.insert(idx);
        }
        TEST_EXPECT(seen.size() > 1);
    }

    // RANDOM_SEQ: each cycle of N picks is a permutation of [0,N) -- every member exactly once
    // before any repeats.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        const int n = 5;
        for (int cycle = 0; cycle < 3; ++cycle) {
            std::set<int> picks;
            for (int i = 0; i < n; ++i) {
                int idx = s.select(k, n, mode::kRandomSeq);
                TEST_EXPECT(idx >= 0 && idx < n);
                picks.insert(idx);
            }
            TEST_EXPECT(static_cast<int>(picks.size()) == n); // a full permutation, no repeats
        }
    }

    return 0;
}
