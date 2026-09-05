#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace opennova::io {

// 64-bit FNV-1a, the content-identity stamp the terrain page caches, the
// static-shadow planner and the scorch bucket keys hash with. One definition
// so no producer's copy can drift: a drifted stamp silently misses every cache
// it keys. (The NovaWorld identity hash is 32-bit FNV-1a with its own
// constants and stays beside its wire use.)
inline constexpr uint64_t kFnv1a64Offset = UINT64_C(1469598103934665603);
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

} // namespace opennova::io
