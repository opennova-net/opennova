#pragma once

#include <stdint.h>

namespace opennova::crt {

// The statically linked MSVC CRT random stream used by retail (one state per
// thread, getptd). Retail routes EVERY rand()/srand() through that one
// per-thread stream; our tree deliberately splits it in two (D-NET-115):
// the sim-visible draws (slot-table, far-marker spawn scores, the 0x100
// death roll) own the session-seeded `World::crt_rand` (io/crt_rand.h),
// while this thread-local owner carries the ported RENDER/EFFECTS consumers
// — scorch texture rolls, material/PANM waveform noise, the GRM gaze jitter
// (CScarDecal_Update @0x57FA50, gated on the display counter) and the
// Effect_RollSurfaceEffectProbability glass-reseed target — so their draw
// and reseed order stays shared among themselves in retail order.
// [orig: rand @0x76B00A; srand @0x76AFFD]
uint16_t crt_rand15(void);
void crt_srand(uint32_t seed);
uint32_t crt_rand_state(void);

} // namespace opennova::crt
