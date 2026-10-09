// Cryptographically secure random bytes from the operating system: the one source for every
// secret or one-of-a-kind value the engine and its embedders mint (the NovaWorld service's bcrypt
// salts, session tags, NWHost.dll HOSTKEY, lobby session ids and GSIDs, PCIDs and EPASK
// parameters; the NW-UDP session keys; the NovaWorld clients' index / key draws; the LAN browse
// cookie; an editor project's UUID). `os_random_bytes` fills a buffer, `os_random_u32` /
// `os_random_u64` / `os_random_nonzero_u32` draw one value, `make_uuid_v4` spells a version 4
// UUID, and `OsRandom` hands the same source to the <random> distributions.
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
#include <string>

namespace opennova {

// Fills the `size` bytes at `buffer` (null only with a size of 0) and returns only once every
// one is written. Keeps no state, so any thread may call it at any time.
void os_random_bytes(void *buffer, size_t size);

uint32_t os_random_u32();
uint64_t os_random_u64();

// Uniform over [1, 2^32 - 1]: a 0 is drawn again. The id the Godot session bindings open a wire
// conversation with (the NovaWorld client's index / key draws, the LAN browse cookie).
uint32_t os_random_nonzero_u32();

// A fresh random UUID in its text form (RFC 4122 version 4, variant 1), its 122 random bits from
// os_random_bytes: an identity a document keeps that no other document shares (an editor
// project's id). "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", lower-case hex, y one of 8, 9, a, b.
std::string make_uuid_v4();

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
