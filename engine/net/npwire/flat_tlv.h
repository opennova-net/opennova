#pragma once

// The NapiNP flat-TLV grammar: `[name\0][u16 LE size][size bytes]` fields run
// back to back with no count and no container; a reader walks until the data
// ends, an empty name, or the first field that does not fit. The same shape
// carries the 0x41/0x42/0x81/0x82 hello/auth fields, the H:0x03 description,
// the 0x45/0x85 ping body and the C2S 0x00 JOIN body.
// [orig: NapiNP_WriteTLV @0x61DD60 (name, NUL, u16 size, value);
//  NapiNP_ReadTLV @0x61DBE0 (the walk: it returns 0 for every cursor but a
//  null one, -1 @0x61DC40, so retail's walk ends only on an empty name; a field
//  that does not fit ends ours, D-NET-412)]
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
// fit. Retail's ReadTLV still returns 0 there, hands the field back and leaves
// its cursor at the value, so its callers handle that field and read on
// (D-NET-412); ours stops the walk.
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

// A field's fixed-width load: `n` bytes from the value pointer whatever the
// field's length, so a short value takes the bytes that follow it (the start of
// the next field: its name, then its NUL and size) and a long one gives its
// first `n`. Every reader of the grammar loads this way: the 0x41 / 0x42
// handlers, the 0x81 / 0x82 readers, the description and goodbye walks and the
// ping. Past the body's `end` retail reads on into whatever follows it: the 64
// KiB stack decrypt buffer the handlers, the 0x81 / 0x82 readers, the goodbye
// walk and the ping copy their datagram into, or for the description walk its
// message's own buffer; ours reads zero there (D-NET-410).
// [orig: NapiNP_ReadTLV @0x61DBE0 returns the value pointer for any length
//  @0x61DCF7]
inline void load_value_bytes(const uint8_t *value, const uint8_t *end, uint8_t *out, size_t n) {
	const size_t avail = static_cast<size_t>(end - value);
	for (size_t i = 0; i < n; ++i) out[i] = i < avail ? value[i] : 0;
}

// A dword field. [orig: NapiNPProtocol_HandleClientHello @0x6213B0 - `mov ecx,
//  [eax]` CI @0x62171E; NapiNPProtocol_HandleClientJoin @0x62B750 - `mov ecx,
//  [eax]` CI @0x62BAE7]
inline uint32_t load_value_dword(const uint8_t *value, const uint8_t *end) {
	uint8_t le[4];
	load_value_bytes(value, end, le, sizeof(le));
	return io::read_u32_le(le);
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
