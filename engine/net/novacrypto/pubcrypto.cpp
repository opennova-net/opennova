#include <novacrypto/pubcrypto.h>

#include <novacrypto/crc32.h>

#include <stdexcept>

namespace opennova {

namespace {

constexpr uint32_t MULTIPLIER = 0x04B05731u;

// 16-bit multiply (Python: `(a & 0xFFFF) * (b & 0xFFFF) & 0xFFFF`).
inline uint16_t word_mul(uint32_t a, uint32_t b) {
	return static_cast<uint16_t>((a & 0xFFFFu) * (b & 0xFFFFu));
}

// String → u32 hash (Python: `_key_fold`).
uint32_t key_fold(const std::string &key) {
	if (key.empty()) return 3252;
	uint64_t acc = 0;
	for (size_t i = 0; i < key.size(); ++i) {
		const uint32_t c = static_cast<uint8_t>(key[i]);
		acc += i + c * c;
	}
	return static_cast<uint32_t>((acc + key.size() + 50) & 0xFFFFFFFFu);
}

// Big-endian CRC-32/MPEG-2 over plaintext. Same polynomial/table as the
// NAPI envelope's CRC, just used here without the LSB-scatter wrapping.
// [orig: NapiNP_ComputeCRC @ 0x618770 (retail) — CRC-32/MPEG-2, table dword_849938, no final xor]
uint32_t crc32_be(const std::vector<uint8_t> &data) {
	uint32_t crc = 0xFFFFFFFFu;
	for (uint8_t byte : data) {
		const uint32_t idx = ((crc >> 24) ^ byte) & 0xFFu;
		crc = ((crc << 8) & 0xFFFFFFFFu) ^ CRC32_TABLE[idx];
	}
	return crc & 0xFFFFFFFFu;
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

// A-P alphabet (16 chars, 'A'+nibble). Each byte → two chars, low nibble first.
std::string encode_ap(const std::vector<uint8_t> &data) {
	std::string out;
	out.reserve(data.size() * 2);
	for (uint8_t b : data) {
		out.push_back(static_cast<char>('A' + (b & 0x0Fu)));
		out.push_back(static_cast<char>('A' + ((b >> 4) & 0x0Fu)));
	}
	return out;
}

std::vector<uint8_t> decode_ap(const std::string &encoded) {
	if (encoded.size() % 2 != 0) {
		throw std::runtime_error("encoded value must have even length");
	}
	std::vector<uint8_t> out;
	out.reserve(encoded.size() / 2);
	for (size_t i = 0; i < encoded.size(); i += 2) {
		const int low  = encoded[i]     - 'A';
		const int high = encoded[i + 1] - 'A';
		if (low < 0 || low > 15 || high < 0 || high > 15) {
			throw std::runtime_error("encoded character outside A-P range");
		}
		out.push_back(static_cast<uint8_t>(low | (high << 4)));
	}
	return out;
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

// [orig: NapiNP_DecodeEncryptedString @ 0x619130 (retail) — A-P decode -> per-key NWU-decrypt + CRC check]
std::vector<uint8_t> decode_pub_value(const std::string &encoded,
                                      const std::string &pcid_key) {
	if (pcid_key.empty()) {
		throw std::runtime_error("pcid_key is required");
	}
	std::vector<uint8_t> data = decode_ap(encoded);
	ticket_transform(data, pcid_key, /*decrypt=*/true);
	if (data.size() < 4) {
		throw std::runtime_error("decoded payload too short");
	}
	std::vector<uint8_t> payload(data.begin(), data.end() - 4);
	const uint32_t expected_crc =
		static_cast<uint32_t>(data[data.size() - 4])
		| (static_cast<uint32_t>(data[data.size() - 3]) <<  8)
		| (static_cast<uint32_t>(data[data.size() - 2]) << 16)
		| (static_cast<uint32_t>(data[data.size() - 1]) << 24);
	const uint32_t actual_crc = crc32_be(payload);
	if (actual_crc != expected_crc) {
		throw std::runtime_error("crc mismatch");
	}
	return payload;
}

} // namespace opennova
