#pragma once

// The NapiNP flat-TLV grammar: `[name\0][u16 LE size][size bytes]` fields run
// back to back with no count and no container; a reader walks until the data
// ends, an empty name, or the first field that does not fit. The same shape
// carries the 0x41/0x42/0x81/0x82 hello/auth fields, the H:0x03 description,
// the 0x45/0x85 ping body and the C2S 0x00 JOIN body.
// [orig: NapiNP_WriteTLV @0x61DD60 (name, NUL, u16 size, value);
//  NapiNP_ReadTLV @0x61DBE0 (the walk, <0 on a short field)]
// Header-only so every consumer (npwire, runtime/inmatch) shares one reader.

#include <base/io/le.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace opennova {

struct FlatTlvField {
	std::string_view name;
	const uint8_t *value = nullptr;
	uint16_t size = 0;
};

inline constexpr size_t kFlatTlvEnd = static_cast<size_t>(-1);

// Read the field at `pos`. Returns the position after it, or kFlatTlvEnd when
// the name is unterminated, the size prefix is missing, or the value does not
// fit (retail's ReadTLV returns <0 and its callers stop the walk).
inline size_t read_flat_tlv(const uint8_t *data, size_t len, size_t pos, FlatTlvField &out) {
	size_t p = pos;
	while (p < len && data[p] != 0) ++p;
	if (p >= len) return kFlatTlvEnd;
	out.name = std::string_view(reinterpret_cast<const char *>(data + pos), p - pos);
	++p;
	if (p + 2 > len) return kFlatTlvEnd;
	out.size = io::read_u16_le(data + p);
	p += 2;
	if (p + out.size > len) return kFlatTlvEnd;
	out.value = data + p;
	return p + out.size;
}

// Append one field. `size` is the raw value length: a string value carries its
// NUL inside the size, a binary value is written as-is.
inline void append_flat_tlv(std::vector<uint8_t> &out, std::string_view name,
		const uint8_t *value, uint16_t size) {
	out.insert(out.end(), name.begin(), name.end());
	out.push_back(0);
	io::append_u16_le(out, size);
	out.insert(out.end(), value, value + size);
}

} // namespace opennova
