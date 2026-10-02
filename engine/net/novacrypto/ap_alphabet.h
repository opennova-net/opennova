#pragma once

#include <cstddef>
#include <cstdint>
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

// The bytes of an A-P string. False (`out` emptied) on an odd length or a character
// outside A-P: a value off the wire is input, so a bad one is a result, never a throw
// (ADR 0049 d5).
inline bool decode_ap(const std::string &encoded, std::vector<uint8_t> &out) {
	out.clear();
	if (encoded.size() % 2 != 0) {
		return false;
	}
	out.reserve(encoded.size() / 2);
	for (size_t i = 0; i < encoded.size(); i += 2) {
		const int low  = encoded[i]     - 'A';
		const int high = encoded[i + 1] - 'A';
		if (low < 0 || low > 15 || high < 0 || high > 15) {
			out.clear();
			return false;
		}
		out.push_back(static_cast<uint8_t>(low | (high << 4)));
	}
	return true;
}

} // namespace opennova
