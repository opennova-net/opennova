#include <base/crt/crt_rng.h>

#include <base/io/crt_rand.h>

namespace opennova::crt {

namespace {

// The recurrence itself lives once, in io::CrtRand; this is the second,
// thread-local OWNER of it (the D-NET-115 two-stream split, see crt_rng.h).
thread_local opennova::io::CrtRand g_crt_rand;

} // namespace

// MSVC CRT LCG: state = state * 214013 + 2531011; return (state >> 16) & 0x7fff
// [orig: rand @ 0x76b00a; srand @ 0x76affd].
uint16_t crt_rand15(void) {
	return static_cast<uint16_t>(g_crt_rand.next());
}

void crt_srand(uint32_t seed) {
	g_crt_rand.seed(seed);
}

uint32_t crt_rand_state(void) {
	return g_crt_rand.state;
}

} // namespace opennova::crt
