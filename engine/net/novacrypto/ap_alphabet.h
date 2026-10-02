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

// The non-throwing decode: false on an odd length or a character outside A-P.
// [orig: NetPacket_WriteStringAndShort @0x4d6fb0 (the host's cookie decoder,
//  misnamed) — odd length -1 @0x4d6fcd, 'A'..'P' gates @0x4d6ff8/@0x4d700f]
inline bool try_decode_ap(const std::string &encoded, std::vector<uint8_t> &out) {
	out.clear();
	if (encoded.size() % 2 != 0) return false;
	out.reserve(encoded.size() / 2);
	for (size_t i = 0; i < encoded.size(); i += 2) {
		const int low  = encoded[i]     - 'A';
		const int high = encoded[i + 1] - 'A';
		if (low < 0 || low > 15 || high < 0 || high > 15) return false;
		out.push_back(static_cast<uint8_t>(low | (high << 4)));
	}
	return true;
}

// Throws std::runtime_error on an odd length or a character outside A-P.
inline std::vector<uint8_t> decode_ap(const std::string &encoded) {
	if (encoded.size() % 2 != 0) {
		throw std::runtime_error("A-P encoded value must have even length");
	}
	std::vector<uint8_t> out;
	if (!try_decode_ap(encoded, out)) {
		throw std::runtime_error("A-P encoded value contains a character outside A-P");
	}
	return out;
}

} // namespace opennova
