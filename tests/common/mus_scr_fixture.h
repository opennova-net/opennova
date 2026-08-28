// The in-memory MUS .bin fixtures the VFS and resource-index MUS tests share:
// a minimal plaintext SCR0 script and the two SCR wrappings the decoders must
// tell apart (an explicit SCR container, and the headerless ciphertext that
// is deliberately NOT auto-decoded). The keystream mirrors the engine's own
// SCR cipher so the tests describe the bytes a retail localres.pff carries
// without any retail bytes.
#ifndef OPENNOVA_TESTS_COMMON_MUS_SCR_FIXTURE_H
#define OPENNOVA_TESTS_COMMON_MUS_SCR_FIXTURE_H

#include <cstdint>
#include <vector>

namespace mus_scr_fixture {

constexpr uint32_t kScrKeyDefault = 0xABEEFACEu;
constexpr uint32_t kScrKeyJoDfx2 = 0x2A5A8EADu;

inline uint32_t rol32(uint32_t value, int shift) {
	return (value << shift) | (value >> (32 - shift));
}

inline void xor_scr_keystream(std::vector<uint8_t> &bytes, uint32_t key) {
	for (uint8_t &byte : bytes) {
		key = rol32(key + rol32(key, 11), 4) ^ 1u;
		byte ^= static_cast<uint8_t>(key);
	}
}

// A 48-byte SCR0 script: version 0x00000100, one chunk.
inline std::vector<uint8_t> minimal_plain_mus() {
	std::vector<uint8_t> plain(48, 0);
	plain[0] = 'S';
	plain[1] = 'C';
	plain[2] = 'R';
	plain[3] = '0';
	plain[5] = 0x01; // version 0x00000100
	plain[8] = 0x01; // chunk_count = 1
	return plain;
}

// The payload past the 4-byte magic, reversed and keyed with the JO/DFX2 key
// and NO container marker: opaque bytes to every auto-decoder.
inline std::vector<uint8_t> headerless_mus_ciphertext(const std::vector<uint8_t> &plain) {
	std::vector<uint8_t> payload;
	if (plain.size() > 4) {
		payload.assign(plain.begin() + 4, plain.end());
	}
	std::vector<uint8_t> out(payload.rbegin(), payload.rend());
	xor_scr_keystream(out, kScrKeyJoDfx2);
	return out;
}

// An explicit SCR container: "SCR" + the key selector byte, then the keyed,
// reversed plaintext. Selector 0 = the default key, 1 = the JO/DFX2 key.
inline std::vector<uint8_t> scr_wrapped(const std::vector<uint8_t> &plain, uint8_t key_selector) {
	std::vector<uint8_t> payload = plain;
	xor_scr_keystream(payload, key_selector == 0 ? kScrKeyDefault : kScrKeyJoDfx2);
	std::vector<uint8_t> out = {'S', 'C', 'R', key_selector};
	out.insert(out.end(), payload.rbegin(), payload.rend());
	return out;
}

} // namespace mus_scr_fixture

#endif // OPENNOVA_TESTS_COMMON_MUS_SCR_FIXTURE_H
