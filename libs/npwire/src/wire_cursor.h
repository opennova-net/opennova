#pragma once
// The npwire decode cursor: a LATCHING little-endian byte reader.
//
// Internal to libs/npwire (the wire decoders and the replay/serverlog reader
// shared one copy each until this was hoisted).
//
// Why not io::ByteReader: that cursor's contract is the FORMAT-PARSER one --
// a clipped read yields 0 and the cursor keeps going, so a parser can recover
// field-by-field and still round-trip a file byte-exactly. A protocol decoder
// wants the opposite: the first read past the end POISONS the cursor, and
// every later read returns 0 without touching the buffer, so a truncated
// datagram can never be half-decoded into plausible-looking state. Both
// contracts are deliberate; this one is not a "better ByteReader" and must not
// be merged into it. (io::ByteReader::ok() reports truncation for parsers that
// only need to observe it.)
#include <cstddef>
#include <cstdint>
#include <string>

namespace opennova {
namespace npwire_detail {

struct Cursor {
	const uint8_t *p = nullptr;
	const uint8_t *end = nullptr;
	bool ok = true;

	uint8_t u8() {
		if (!ok || p + 1 > end) { ok = false; return 0; }
		return *p++;
	}
	uint16_t u16() {
		if (!ok || p + 2 > end) { ok = false; return 0; }
		uint16_t v = uint16_t(p[0]) | uint16_t(p[1]) << 8; p += 2; return v;
	}
	uint32_t u32() {
		if (!ok || p + 4 > end) { ok = false; return 0; }
		uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
		             uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
		p += 4; return v;
	}
	int16_t i16() { return int16_t(u16()); }
	int32_t i32() { return int32_t(u32()); }
	void skip(size_t n) {
		if (!ok || p + n > end) { ok = false; p = end; return; }
		p += n;
	}
	std::string cstr() {
		std::string s;
		while (ok && p < end) {
			uint8_t c = *p++;
			if (c == 0) return s;
			s.push_back(char(c));
		}
		ok = false;
		return s;
	}
};

} // namespace npwire_detail
} // namespace opennova
