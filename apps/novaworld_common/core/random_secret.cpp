#include "random_secret.h"

#include <base/os_random/os_random.h>

#include <array>
#include <cstdint>

namespace opennova::novaworld_server {

std::string random_secret(size_t length) {
	static constexpr char kAlphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	constexpr unsigned kAlphabetSize = sizeof(kAlphabet) - 1; // 62
	// Bytes past the largest multiple of 62 are drawn again, so every character
	// is equally likely.
	constexpr unsigned kAccept = 256 - 256 % kAlphabetSize;
	std::string out;
	out.reserve(length);
	std::array<uint8_t, 64> pool{};
	while (out.size() < length) {
		os_random_bytes(pool.data(), pool.size());
		for (const uint8_t b : pool) {
			if (b >= kAccept) continue;
			out.push_back(kAlphabet[b % kAlphabetSize]);
			if (out.size() == length) break;
		}
	}
	return out;
}

} // namespace opennova::novaworld_server
