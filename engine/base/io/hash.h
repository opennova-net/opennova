#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace opennova::io {

// 64-bit FNV-1a (the FNV offset basis and prime), the content-identity stamp the
// terrain page caches, the static-shadow planner, the model PANM cache, the editor's
// build ids and import records and its change fingerprints hash with. One definition
// so no producer's copy can drift: a drifted stamp silently misses every cache it
// keys. (The NovaWorld identity hash is 32-bit FNV-1a with its own constants and
// stays beside its wire use.)
inline constexpr uint64_t kFnv1a64Offset = UINT64_C(14695981039346656037);
inline constexpr uint64_t kFnv1a64Prime = UINT64_C(1099511628211);

inline constexpr uint64_t fnv1a64_byte(uint64_t hash, uint8_t byte) noexcept {
	return (hash ^ byte) * kFnv1a64Prime;
}

inline uint64_t fnv1a64_bytes(uint64_t hash, const void *data, size_t size) noexcept {
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (size_t index = 0; index < size; ++index) {
		hash = fnv1a64_byte(hash, bytes[index]);
	}
	return hash;
}

// Mix a trivially copyable value's object representation.
template <typename T>
inline uint64_t fnv1a64_value(uint64_t hash, const T &value) noexcept {
	static_assert(std::is_trivially_copyable_v<T>,
			"fnv1a64_value hashes the object representation");
	return fnv1a64_bytes(hash, &value, sizeof(value));
}

// The spelling a 64-bit hash takes in a text record (the editor's build ids, an
// import record's content hash): 16 lower-case hex digits, zero-padded.
inline std::string hex64(uint64_t value) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string text(16, '0');
	for (size_t index = 16; index-- > 0; value >>= 4) text[index] = kDigits[value & 0xF];
	return text;
}

// The inverse of hex64: 1 to 16 hex digits of either case; false (and `out` left
// alone) on anything else.
inline bool parse_hex64(std::string_view text, uint64_t &out) {
	if (text.empty() || text.size() > 16) return false;
	uint64_t value = 0;
	for (const char c : text) {
		int digit;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
		else return false;
		value = (value << 4) | uint64_t(digit);
	}
	out = value;
	return true;
}

} // namespace opennova::io
