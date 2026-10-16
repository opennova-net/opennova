#pragma once

// The NapiNP flat-TLV grammar: `[name\0][u16 LE size][size bytes]` fields run
// back to back with no count and no container; a reader walks until an empty
// name (a NUL first byte, or the data's end). The same shape carries the
// 0x41/0x42/0x81/0x82 hello/auth fields, the H:0x03 description, the 0x45/0x85
// ping body and the C2S 0x00 JOIN body, and every one of their readers walks it
// through the one ReadTLV.
// [orig: NapiNP_WriteTLV @0x61DD60 (name, NUL, u16 size, value);
//  NapiNP_ReadTLV @0x61DBE0 (the walk: it returns 0 for every cursor but a
//  null one, -1 @0x61DC40, so each caller's `< 0` exit never fires on data and
//  its walk ends only on an empty name, `cmp byte ptr [tag], 0; jz`: the JOIN
//  @0x512B64, the 0x41 @0x621544, the description @0x621B96, the ping
//  @0x623BF4, the goodbye @0x623EC4, the 0x81 @0x626F44, the 0x82 @0x629A15,
//  the 0x42 @0x62B907)]
// Header-only so every consumer (npwire, runtime/inmatch) shares one reader.

#include <base/io/le.h>
#include <net/napi/tlv.h> // napi_copy_string

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

struct FlatTlvField {
	std::string_view name;
	const uint8_t *value = nullptr;
	// The declared length, as ReadTLV hands it back, whether or not the value fits.
	uint16_t size = 0;
	// The value's bytes inside the body: `size`, or fewer for a value that runs
	// past the body's end (whose bytes past it are D-NET-410's).
	size_t present = 0;
};

inline constexpr size_t kFlatTlvEnd = static_cast<size_t>(-1);

// Read the field at `pos`, as ReadTLV reads it. kFlatTlvEnd for an empty name
// (its first byte NUL, or `pos` at the body's end): the one shape that ends a
// walk. Otherwise the field and the position the next read starts from, which
// is past `pos` for every field:
// - a name with no NUL inside the body is the bytes to the end; the cursor
//   clamps to the end (retail's strlen runs on past the datagram, D-NET-410),
//   and its size is cut;
// - a size prefix cut by the end reads 0, and the cursor stays where the size
//   would have been, so the next read takes a name from those bytes;
// - a value that runs past the end keeps its declared size and its value
//   pointer (`present` counts its bytes inside the body), and the cursor stays
//   at the value's start, so the next read takes a name from the value's bytes.
// [orig: NapiNP_ReadTLV @0x61DBE0 - the tag defaulted to g_EmptyStr (off_849F0C)
//  @0x61DC1E..0x61DC24 and kept for an empty name @0x61DCB9..0x61DCBE, the name
//  copy's end bound @0x61DC76..0x61DC79, the cursor past the name clamped to the
//  end @0x61DCAA..0x61DCB6, the tag @0x61DCC4, the cut size reading 0
//  @0x61DCCD..0x61DCD5, the length out @0x61DCEA, the value out @0x61DCF7, the
//  overrun leaving the cursor @0x61DD00..0x61DD10, `xor eax, eax` @0x61DD20]
inline size_t read_flat_tlv(const uint8_t *data, size_t len, size_t pos, FlatTlvField &out) {
	out = FlatTlvField{};
	if (pos >= len || data[pos] == 0)
		return kFlatTlvEnd;
	size_t p = pos;
	while (p < len && data[p] != 0)
		++p;
	out.name = std::string_view(reinterpret_cast<const char *>(data + pos), p - pos);
	p = p < len ? p + 1 : len;
	if (p + 2 > len) {
		out.value = data + p;
		return p;
	}
	out.size = io::read_u16_le(data + p);
	p += 2;
	out.value = data + p;
	if (out.size > len - p) {
		out.present = len - p;
		return p;
	}
	out.present = out.size;
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

// A string field: every reader of the grammar (the 0x41 / 0x42 handlers, the
// 0x81 / 0x82 readers, the description and goodbye walks, the C2S 0x00 JOIN
// handler) copies from the value pointer up to the first NUL and at most
// `buffer` less one characters, whatever the field's length, so an inner NUL
// ends the string and a value without its NUL runs on into the bytes that
// follow it (the next field's name, up to that name's NUL). Past the body's
// `end` ours reads zero, which ends the copy (D-NET-410).
// [orig: Napi_CopyString @0x617E10 - the NUL test @0x617E32, the `buffer - 1`
//  bound @0x617E41..0x617E44]
inline std::string load_value_string(const uint8_t *value, const uint8_t *end, size_t buffer) {
	return napi_copy_string(value, end > value ? static_cast<size_t>(end - value) : 0u, buffer);
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
