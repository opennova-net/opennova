#include <formats/cpt/crypto.h>

// The TPM1 payload cipher (docs/terrain/terrain-re.md, the TPM1 tile-mesh format): every
// vertex/index payload is rotate-crypted [orig: TrnGen.exe MeshData_LoadFromFile @0x404100;
//  pack/unpack family @0x403CD0 / @0x403DD0 / @0x403E70 / @0x403EF0].

namespace opennova {

static constexpr uint32_t ROTATE_KEY = 0xA55B1EED;

static inline uint8_t rol8(uint8_t value, int count) {
	count &= 7;
	return static_cast<uint8_t>((value << count) | (value >> (8 - count)));
}

static inline uint8_t ror8(uint8_t value, int count) {
	count &= 7;
	return static_cast<uint8_t>((value >> count) | (value << (8 - count)));
}

static inline uint32_t rol32(uint32_t value, int count) {
	count &= 31;
	return (value << count) | (value >> (32 - count));
}

void rotate_encrypt(uint8_t *buf, size_t len) {
	uint32_t key = ROTATE_KEY;
	for (size_t i = 0; i < len; ++i) {
		buf[i] = rol8(buf[i], static_cast<int>(key & 7));
		key = rol32(key, 1);
	}
}

void rotate_decrypt(uint8_t *buf, size_t len) {
	uint32_t key = ROTATE_KEY;
	for (size_t i = 0; i < len; ++i) {
		buf[i] = ror8(buf[i], static_cast<int>(key & 7));
		key = rol32(key, 1);
	}
}

} // namespace opennova
