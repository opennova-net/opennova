// A fresh random UUID in its text form (RFC 4122 version 4, variant 1), header-only: an
// identity a document keeps that no other document shares (an editor project's id).
//
// Not a port: nothing here is witnessed engine behaviour.
#pragma once

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace opennova::io {

// "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", lower-case hex, y one of 8, 9, a, b.
inline std::string make_uuid_v4() {
	std::random_device device;
	std::mt19937_64 rng(static_cast<uint64_t>(device()) << 32 ^ device());
	uint64_t hi = rng();
	uint64_t lo = rng();
	hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull; // version 4
	lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // variant 1
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
	              static_cast<unsigned>((hi >> 16) & 0xFFFF), static_cast<unsigned>(hi & 0xFFFF),
	              static_cast<unsigned>(lo >> 48), static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
	return buf;
}

} // namespace opennova::io
