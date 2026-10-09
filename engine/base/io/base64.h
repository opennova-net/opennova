// Base64 (RFC 4648 section 4: the standard alphabet, '=' padding), header-only: bytes as
// text a JSON string can carry (a PNG picture on the editor's wire).
//
// Not a port: nothing here is witnessed engine behaviour. (The NovaWorld service's password
// salts take bcrypt's own alphabet, third_party/bcrypt, not this one.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::io {

inline std::string base64_encode(const uint8_t *data, size_t size) {
	static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((size + 2) / 3 * 4);
	for (size_t i = 0; i < size; i += 3) {
		const uint32_t chunk = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
		                       (i + 2 < size ? uint32_t(data[i + 2]) : 0);
		out.push_back(kDigits[(chunk >> 18) & 63]);
		out.push_back(kDigits[(chunk >> 12) & 63]);
		out.push_back(i + 1 < size ? kDigits[(chunk >> 6) & 63] : '=');
		out.push_back(i + 2 < size ? kDigits[chunk & 63] : '=');
	}
	return out;
}

inline std::string base64_encode(const std::vector<uint8_t> &bytes) {
	return base64_encode(bytes.data(), bytes.size());
}

} // namespace opennova::io
