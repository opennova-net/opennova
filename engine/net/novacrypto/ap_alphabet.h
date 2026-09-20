#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace opennova {

// The A-P alphabet (16 chars, 'A' + nibble) PUBcrypto and EPASK both ship
// their bytes in: each byte is two chars, low nibble first.
// [orig: NapiNP_EncodeToHexAlpha @ 0x666570 (retail) — 'A'+lo, 'A'+hi]
inline std::string encode_ap(const std::vector<uint8_t> &data) {
	std::string out;
	out.reserve(data.size() * 2);
	for (uint8_t b : data) {
		out.push_back(static_cast<char>('A' + (b & 0x0Fu)));
		out.push_back(static_cast<char>('A' + ((b >> 4) & 0x0Fu)));
	}
	return out;
}

// Throws std::runtime_error on an odd length or a character outside A-P.
inline std::vector<uint8_t> decode_ap(const std::string &encoded) {
	if (encoded.size() % 2 != 0) {
		throw std::runtime_error("A-P encoded value must have even length");
	}
	std::vector<uint8_t> out;
	out.reserve(encoded.size() / 2);
	for (size_t i = 0; i < encoded.size(); i += 2) {
		const int low  = encoded[i]     - 'A';
		const int high = encoded[i + 1] - 'A';
		if (low < 0 || low > 15 || high < 0 || high > 15) {
			throw std::runtime_error("A-P encoded value contains a character outside A-P");
		}
		out.push_back(static_cast<uint8_t>(low | (high << 4)));
	}
	return out;
}

} // namespace opennova
