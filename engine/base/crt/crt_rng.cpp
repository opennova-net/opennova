#include "crt/crt_rng.h"

namespace {

thread_local uint32_t g_crt_rand_state = 1u;

} // namespace

// MSVC CRT LCG: state = state * 214013 + 2531011; return (state >> 16) & 0x7fff
// [orig: rand @ 0x76b00a; srand @ 0x76affd].
uint16_t crt_rand15(void) {
	g_crt_rand_state =
			g_crt_rand_state * 214013u + 2531011u;
	return static_cast<uint16_t>(
			(g_crt_rand_state >> 16u) & 0x7fffu);
}

void crt_srand(uint32_t seed) {
	g_crt_rand_state = seed;
}

uint32_t crt_rand_state(void) {
	return g_crt_rand_state;
}
