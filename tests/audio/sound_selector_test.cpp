// opennova::audio::SoundSelector: the sound-set member-selection state machine pushed down from the
// Godot layer (nova_sound_bank.gd _pick_member). Grilled vs Jointops.exe 2026-06-09: the selection
// shapes AND the random stream are asserted against the engine algorithm
// [orig: SoundBank_PlayTriggerEntries @ 0x75ccd0, PRNG_ScaledRandom @ 0x75be50, seed @ 0x85A3DC]:
//   state = ROL32(state + ROL32(state, 11), 3); pick = (count * (state & 0xFF)) >> 8;
//   seed 0x2B0749C1. Expected values below were computed with an independent implementation.
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

    // FIRST (reimpl preview extension; no engine equivalent): always index 0.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        for (int i = 0; i < 8; ++i) {
            TEST_EXPECT(s.select(k, 5, mode::kFirst) == 0);
        }
    }

    // SEQUENTIAL (flag 0x10): 0,1,2,0,1,2 wrapping [orig: @ 0x75cd6f..0x75cd79].
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        const int expect[6] = {0, 1, 2, 0, 1, 2};
        for (int i = 0; i < 6; ++i) {
            TEST_EXPECT(s.select(k, 3, mode::kSequential) == expect[i]);
        }
    }

    // RANDOM: the exact engine stream from the static seed. First picks for count=6:
    // low bytes CB,03,F9,E5,95,7A,F1,60 -> (6*lo)>>8 = 4,0,5,5,3,2,5,2.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        const int expect[8] = {4, 0, 5, 5, 3, 2, 5, 2};
        for (int i = 0; i < 8; ++i) {
            TEST_EXPECT(s.select(k, 6, mode::kRandom) == expect[i]);
        }
    }

    // Unknown mode behaves like RANDOM (the engine default for unflagged layers
    // [orig: @ 0x75cdfc]), NOT like FIRST: fresh stream, count=3 -> (3*0xCB)>>8 = 2.
    {
        SoundSelector s;
        TEST_EXPECT(s.select(SoundSelector::make_key(9, 9, 9), 3, 999) == 2);
    }

    // RANDOM_SEQ (flag 0x80): random anchor, then one full in-order cycle back to the anchor.
    // The anchor plays twice in a row (anchor draw, then the first cycle step starts at the same
    // cursor) -- the engine quirk, preserved deliberately [orig: @ 0x75cd9f vs @ 0x75cdba].
    // Fresh stream, count=5: anchor1 = (5*0xCB)>>8 = 3 -> 3,3,4,0,1,2; anchor2 = (5*0x03)>>8 = 0
    // -> 0,0,1,2,3,4.
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        const int expect[12] = {3, 3, 4, 0, 1, 2, 0, 0, 1, 2, 3, 4};
        for (int i = 0; i < 12; ++i) {
            TEST_EXPECT(s.select(k, 5, mode::kRandomSeq) == expect[i]);
        }
    }

    // One stream shared across keys and modes [orig: one global state @ 0x85A3DC]:
    // random(6) consumes step1 (->4); a random-seq anchor on another key then consumes
    // step2 (->(5*0x03)>>8 = 0).
    {
        SoundSelector s;
        TEST_EXPECT(s.select(SoundSelector::make_key(0, 0, 0), 6, mode::kRandom) == 4);
        TEST_EXPECT(s.select(SoundSelector::make_key(0, 0, 1), 5, mode::kRandomSeq) == 0);
        TEST_EXPECT(s.select(SoundSelector::make_key(0, 0, 1), 5, mode::kRandomSeq) == 0); // cycle re-plays anchor
        TEST_EXPECT(s.select(SoundSelector::make_key(0, 0, 1), 5, mode::kRandomSeq) == 1);
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

    // reset() clears cursors/anchors but leaves the RNG stream running (the engine never
    // reseeds across bank reloads).
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        TEST_EXPECT(s.select(k, 3, mode::kSequential) == 0);
        TEST_EXPECT(s.select(k, 3, mode::kSequential) == 1);
        s.reset();
        TEST_EXPECT(s.select(k, 3, mode::kSequential) == 0);
        // Stream continues: first random AFTER the (stream-untouched) sequential picks is
        // still step1 of the stream -> count 6 -> 4.
        TEST_EXPECT(s.select(k, 6, mode::kRandom) == 4);
    }

    // Sequential cursor survives a count shrink without going out of range (reimpl safety;
    // the engine would read out of bounds here).
    {
        SoundSelector s;
        const uint64_t k = SoundSelector::make_key(0, 0, 0);
        for (int i = 0; i < 5; ++i) {
            (void)s.select(k, 6, mode::kSequential);
        }
        const int idx = s.select(k, 2, mode::kSequential);
        TEST_EXPECT(idx >= 0 && idx < 2);
    }

    return 0;
}
