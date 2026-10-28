// Base64 (RFC 4648 section 4: the standard alphabet, '=' padding), header-only: bytes as
// text a JSON string can carry (a PNG picture on the editor's wire), and that text read back
// strictly (a hosted container's files, a NovaWorld lobby's gt.ssc).
//
// Not a port: nothing here is witnessed engine behaviour. (The NovaWorld service's password
// salts take bcrypt's own alphabet, third_party/bcrypt, not this one.)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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

namespace detail {

template <typename Bytes>
bool base64_decode_strict_into(std::string_view text, Bytes &out) {
	using Byte = typename Bytes::value_type;
	const auto digit = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	out.clear();
	if (text.size() % 4 != 0) return false;
	out.reserve(text.size() / 4 * 3);
	for (size_t i = 0; i < text.size(); i += 4) {
		int v[4] = {};
		int pad = 0;
		for (int j = 0; j < 4; ++j) {
			const char c = text[i + static_cast<size_t>(j)];
			if (c == '=') {
				if (i + 4 != text.size() || j < 2) return false;
				++pad;
				continue;
			}
			if (pad) return false;
			v[j] = digit(c);
			if (v[j] < 0) return false;
		}
		if ((pad == 2 && (v[1] & 0xF)) || (pad == 1 && (v[2] & 0x3))) return false;
		const uint32_t chunk = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
		out.push_back(static_cast<Byte>((chunk >> 16) & 0xFF));
		if (pad < 2) out.push_back(static_cast<Byte>((chunk >> 8) & 0xFF));
		if (pad < 1) out.push_back(static_cast<Byte>(chunk & 0xFF));
	}
	return true;
}

} // namespace detail

// `text` read back strictly: RFC 4648's standard alphabet with its padding, no whitespace, no
// missing padding, nothing after it, the unused bits 0, so each byte string has the one text
// base64_encode makes of it. False (and `out` partial) for any other text; "" reads as no bytes.
inline bool base64_decode_strict(std::string_view text, std::vector<uint8_t> &out) {
	return detail::base64_decode_strict_into(text, out);
}

inline bool base64_decode_strict(std::string_view text, std::string &out) {
	return detail::base64_decode_strict_into(text, out);
}

} // namespace opennova::io
