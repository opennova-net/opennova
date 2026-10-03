#include <net/novacrypto/pubcrypto.h>

#include <net/novacrypto/ap_alphabet.h>
#include <net/novacrypto/crc32.h>

#include <stdexcept>

namespace opennova {

namespace {

constexpr uint32_t MULTIPLIER = 0x04B05731u;

// 16-bit multiply (Python: `(a & 0xFFFF) * (b & 0xFFFF) & 0xFFFF`).
inline uint16_t word_mul(uint32_t a, uint32_t b) {
	return static_cast<uint16_t>((a & 0xFFFFu) * (b & 0xFFFFu));
}

// String → u32 hash: the same NapiNP_ComputeKeySeed the NWU cipher uses
// (nwu.cpp keeps the canonical copy). Key bytes are SIGNED — the original
// sign-extends before squaring (`movsx ecx, byte ptr [eax+edi]` @0x618460,
// `imul ebx, ecx` @0x618466) — and an empty key is 0 + 0 + 50; 3252 is the
// NULL-pointer arm only. Only the low byte and parity of the result reach
// the transform, so a high-bit key byte never changed the ciphertext, but
// the seed itself must match numerically.
// [orig: NapiNP_ComputeKeySeed @0x618430]
uint32_t key_fold(const std::string &key) {
	int64_t acc = 0;
	for (size_t i = 0; i < key.size(); ++i) {
		const int c = static_cast<int>(static_cast<signed char>(key[i]));
		acc += static_cast<int64_t>(i) + static_cast<int64_t>(c) * c;
	}
	return static_cast<uint32_t>((acc + static_cast<int64_t>(key.size()) + 50) & 0xFFFFFFFFu);
}

// Big-endian CRC-32/MPEG-2 over plaintext. Same polynomial/table as the
// NAPI envelope's CRC, just used here without the LSB-scatter wrapping.
// [orig: NapiNP_ComputeCRC @ 0x618770 (retail) — CRC-32/MPEG-2, table dword_849938, no final xor]
uint32_t crc32_be(const std::vector<uint8_t> &data) {
	return crc32_napi(data.data(), data.size());
}

// Phase 1 (encrypt) / Phase 4 (decrypt): 16-bit LCG step adds/subtracts
// per-byte. Returns final state for chaining if needed.
uint32_t phase_pseudorandom(std::vector<uint8_t> &buf, uint32_t state, bool subtract) {
	uint16_t value = state & 0xFFFFu;
	for (size_t i = 0; i < buf.size(); ++i) {
		value = word_mul(MULTIPLIER, value) + 1;
		const uint8_t delta = value & 0xFFu;
		buf[i] = subtract ? static_cast<uint8_t>(buf[i] - delta)
		                  : static_cast<uint8_t>(buf[i] + delta);
	}
	return value;
}

// Phase 3 (encrypt) / Phase 2 (decrypt): linear ramp add/subtract.
void phase_sequential(std::vector<uint8_t> &buf, uint8_t start, uint8_t step, bool subtract) {
	uint8_t current = start;
	for (size_t i = 0; i < buf.size(); ++i) {
		const uint8_t delta = static_cast<uint8_t>(current + i);
		buf[i] = subtract ? static_cast<uint8_t>(buf[i] - delta)
		                  : static_cast<uint8_t>(buf[i] + delta);
		current = static_cast<uint8_t>(current + step);
	}
}

// Phase 2 (encrypt) / Phase 3 (decrypt): conditional reverse.
void phase_reverse(std::vector<uint8_t> &buf, bool should_reverse) {
	if (!should_reverse || buf.empty()) return;
	for (size_t i = 0, j = buf.size() - 1; i < j; ++i, --j) {
		std::swap(buf[i], buf[j]);
	}
}

// Phase 4 (encrypt) / Phase 1 (decrypt): cycle through key bytes mod len.
void phase_keycycle(std::vector<uint8_t> &buf, const std::string &key, bool subtract) {
	if (key.empty()) return;
	const size_t key_len = key.size();
	for (size_t i = 0; i < buf.size(); ++i) {
		const uint8_t delta = static_cast<uint8_t>(key[i % key_len]);
		buf[i] = subtract ? static_cast<uint8_t>(buf[i] - delta)
		                  : static_cast<uint8_t>(buf[i] + delta);
	}
}

// [orig: == NWU NapiNP_EncryptBuffer@0x6187b0 / NapiNP_DecryptBuffer@0x618880 (retail). Python's
//        _ticket_transform is an inlined NWU copy; proven byte-identical to the NWU cipher (NW-C3).]
void ticket_transform(std::vector<uint8_t> &data, const std::string &key, bool decrypt) {
	if (data.empty() || key.empty()) return;

	const uint32_t key_hash = key_fold(key);
	const uint32_t phase_start = (MULTIPLIER * key_hash + 1) & 0xFFFFFFFFu;
	const uint8_t seq_start = static_cast<uint8_t>(phase_start);
	const uint8_t seq_step  = static_cast<uint8_t>(MULTIPLIER * seq_start + 1);

	uint16_t state = key_hash & 0xFFFFu;
	for (int i = 0; i < 3; ++i) {
		state = word_mul(MULTIPLIER, state) + 1;
	}
	const bool do_reverse = (state & 1u) != 0;

	if (decrypt) {
		phase_pseudorandom(data, state, /*subtract=*/true);
		phase_sequential(data, seq_start, seq_step, /*subtract=*/true);
		phase_reverse(data, do_reverse);
		phase_keycycle(data, key, /*subtract=*/true);
	} else {
		phase_keycycle(data, key, /*subtract=*/false);
		phase_reverse(data, do_reverse);
		phase_sequential(data, seq_start, seq_step, /*subtract=*/false);
		phase_pseudorandom(data, state, /*subtract=*/false);
	}
}

} // namespace

// [orig: NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0 (retail), single-key path: CRC32-append
//        -> NWU-encrypt -> A-P. grill wave 3 NW-C3, MATCHING. (Multi-key form = Python remember-cookie.)]
std::string encode_pub_value(const std::vector<uint8_t> &plaintext,
                             const std::string &pcid_key) {
	if (pcid_key.empty()) {
		throw std::runtime_error("pcid_key is required");
	}
	const uint32_t crc = crc32_be(plaintext);
	std::vector<uint8_t> buffer = plaintext;
	buffer.reserve(plaintext.size() + 4);
	// CRC appended little-endian to match Python's struct.pack("<I", crc).
	buffer.push_back(static_cast<uint8_t>( crc        & 0xFFu));
	buffer.push_back(static_cast<uint8_t>((crc >>  8) & 0xFFu));
	buffer.push_back(static_cast<uint8_t>((crc >> 16) & 0xFFu));
	buffer.push_back(static_cast<uint8_t>((crc >> 24) & 0xFFu));
	ticket_transform(buffer, pcid_key, /*decrypt=*/false);
	return encode_ap(buffer);
}

std::string encode_pub_value(const std::string &plaintext, const std::string &pcid_key) {
	return encode_pub_value(
		std::vector<uint8_t>(plaintext.begin(), plaintext.end()), pcid_key);
}

// One key's probe: decipher a copy, then compare the trailing LE CRC with the
// CRC of the bytes before it. [orig: NapiPacket_DecryptAndVerify @0x4c2ad0 —
//  copy @0x4c2bcb, NetPacket_EncryptPayload (the decipher) @0x4c2bde, the
//  embedded dword @0x4c2be3 against Cipher_ComputeCRC @0x4c2bfa]
bool try_decrypt_pub_bytes(std::vector<uint8_t> data, const std::string &pcid_key,
                           std::vector<uint8_t> &payload) {
	payload.clear();
	if (pcid_key.empty() || data.size() < 4) return false;
	ticket_transform(data, pcid_key, /*decrypt=*/true);
	const uint32_t expected_crc =
		static_cast<uint32_t>(data[data.size() - 4])
		| (static_cast<uint32_t>(data[data.size() - 3]) <<  8)
		| (static_cast<uint32_t>(data[data.size() - 2]) << 16)
		| (static_cast<uint32_t>(data[data.size() - 1]) << 24);
	payload.assign(data.begin(), data.end() - 4);
	return crc32_be(payload) == expected_crc;
}

// [orig: NapiNP_DecodeEncryptedString @ 0x619130 (retail) — A-P decode -> per-key NWU-decrypt + CRC check]
bool decode_pub_value(const std::string &encoded, const std::string &pcid_key,
                      std::vector<uint8_t> &out, std::string *error) {
	out.clear();
	const auto fail = [error](const char *why) {
		if (error != nullptr) *error = why;
		return false;
	};
	if (pcid_key.empty()) {
		return fail("pcid_key is required");
	}
	std::vector<uint8_t> data;
	if (!decode_ap(encoded, data)) {
		return fail("not an A-P value (odd length or a character outside A-P)");
	}
	if (data.size() < 4) {
		return fail("decoded payload too short");
	}
	std::vector<uint8_t> payload;
	if (!try_decrypt_pub_bytes(std::move(data), pcid_key, payload)) {
		return fail("crc mismatch");
	}
	out = std::move(payload);
	return true;
}

namespace {

// The keys of a chain in their order: the text between the ':'s (a chain with no ':' one key).
std::vector<std::string> chain_keys(const std::string &key_chain) {
	std::vector<std::string> keys(1);
	for (const char c : key_chain) {
		if (c == ':') keys.emplace_back();
		else keys.back().push_back(c);
	}
	return keys;
}

// The C library's isprint in the "C" locale the game runs in: 0x20..0x7E.
bool printable(unsigned char c) {
	return c >= 0x20 && c <= 0x7E;
}

} // namespace

// [orig: NapiNP_EncryptAndEncodeToHexAlpha @ 0x618fd0: each key in order @ 0x619074, the CRC of what
// the buffer holds appended @ 0x6190ae, NapiNP_EncryptBuffer @ 0x6190b7; the A-P pack @ 0x6190de]
std::string encode_key_chain(const std::vector<uint8_t> &plaintext, const std::string &key_chain) {
	std::vector<uint8_t> buffer = plaintext;
	for (const std::string &key : chain_keys(key_chain)) {
		const uint32_t crc = crc32_be(buffer);
		for (int shift = 0; shift < 32; shift += 8) buffer.push_back(static_cast<uint8_t>((crc >> shift) & 0xFFu));
		ticket_transform(buffer, key, /*decrypt=*/false);
	}
	return encode_ap(buffer);
}

// [orig: NapiNP_DecodeEncryptedString @ 0x619130: the printable count @ 0x6191c1 and the room for the
// keys' CRCs @ 0x61920f; each printable character a nibble, low first, one outside A-P refused
// @ 0x61923a, an odd count refused @ 0x619261; the keys right to left @ 0x619294, each
// NapiNP_DecryptBuffer @ 0x6192e1 then its CRC compared @ 0x6192f6]
bool decode_key_chain(const std::string &encoded, const std::string &key_chain,
                      std::vector<uint8_t> &plaintext) {
	plaintext.clear();
	const std::vector<std::string> keys = chain_keys(key_chain);
	size_t printables = 0;
	for (const char c : encoded) printables += printable(static_cast<unsigned char>(c)) ? 1 : 0;
	if (printables / 2 < 4 * keys.size()) return false;
	std::vector<uint8_t> data;
	data.reserve(printables / 2);
	bool high = false;
	for (const char c : encoded) {
		if (!printable(static_cast<unsigned char>(c))) continue;
		const int nibble = c - 'A';
		if (nibble < 0 || nibble > 15) return false;
		if (high) data.back() = static_cast<uint8_t>(data.back() | (nibble << 4));
		else data.push_back(static_cast<uint8_t>(nibble));
		high = !high;
	}
	if (high) return false;
	for (auto key = keys.rbegin(); key != keys.rend(); ++key) {
		if (data.size() < 4) return false;
		ticket_transform(data, *key, /*decrypt=*/true);
		const size_t length = data.size() - 4;
		const uint32_t stored = static_cast<uint32_t>(data[length]) | (static_cast<uint32_t>(data[length + 1]) << 8) |
		                        (static_cast<uint32_t>(data[length + 2]) << 16) |
		                        (static_cast<uint32_t>(data[length + 3]) << 24);
		data.resize(length);
		if (crc32_be(data) != stored) return false;
	}
	plaintext = std::move(data);
	return true;
}

} // namespace opennova
