// Cryptographically secure random bytes from the operating system: the one source for every
// secret or one-of-a-kind value the NovaWorld service mints (bcrypt salts, the session tags, the
// NWHost.dll HOSTKEY, lobby session ids and GSIDs, PCIDs, the EPASK parameters, the NW-UDP
// session keys). `os_random_bytes` fills a buffer, `os_random_u32` / `os_random_u64` draw one
// value, and `OsRandom` hands the same source to the <random> distributions.
//
// Not std::random_device: libstdc++'s reads the CPU's RDSEED on x86, and AMD's RDSEED erratum
// answers concurrent draws with 0 (11 of 32 simultaneous draws on a Zen 5 machine), so every
// per-thread generator seeded from it started on the same sequence. os_random.cpp names the call
// each platform makes; none of them settles for weaker bytes: a failed call is reported through
// io::logf at kError and the process aborts.
//
// Platform primitive, not a port: nothing here is witnessed engine behaviour.
#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova {

// Fills the `size` bytes at `buffer` (null only with a size of 0) and returns only once every
// one is written. Keeps no state, so any thread may call it at any time.
void os_random_bytes(void *buffer, size_t size);

uint32_t os_random_u32();
uint64_t os_random_u64();

// The <random> distributions' UniformRandomBitGenerator over os_random_bytes: no state and no
// seed, each draw its own OS call, so one instance serves any thread. (min) and (max) are
// parenthesised so a <windows.h> min/max macro in the including TU cannot expand them.
class OsRandom {
public:
	using result_type = uint64_t;
	static constexpr result_type(min)() { return 0; }
	static constexpr result_type(max)() { return ~result_type(0); }
	result_type operator()() const { return os_random_u64(); }
};

} // namespace opennova
