#include <novacrypto/nwu.h>

#include <cstring>

namespace opennova {

namespace {

// Byte-exact ports of the buffer primitives. All operate in place and
// wrap uint8_t naturally on add/subtract; progression uses char arithmetic
// so startValue wraps modulo 256 across iterations just like the original.

struct LCGState {
	uint32_t state;   // low 16 bits drive the LCG; upper are written but unused
	uint32_t magic;   // == NWU_LCG_MAGIC; only low 16 bits participate
	uint32_t counter; // ++per LCG step
};

// Mirrors CLCG_Init@0x5f3170.
void clcg_init(LCGState &s, uint32_t seed) {
	s.state = seed;
	s.magic = NWU_LCG_MAGIC;
	s.counter = 0;
}

// One LCG step: returns the new low-16 state and mutates `s`.
// Corresponds to the inner update inside Buffer_ScrambleWithLCG/Crypto_ApplyPRNG:
//     state = uint16(state_word * magic_word + 1); counter += 1
inline uint16_t clcg_step(LCGState &s) {
	const uint16_t state_w = static_cast<uint16_t>(s.state & 0xFFFFu);
	const uint16_t magic_w = static_cast<uint16_t>(s.magic & 0xFFFFu);
	const uint16_t next = static_cast<uint16_t>(static_cast<uint16_t>(state_w * magic_w) + 1u);
	s.state = next; // full dword store; upper bits become zero (mirrors orig)
	s.counter += 1;
	return next;
}

// Buffer_AddWithKeyString@0x5e5080 — ADD (not XOR despite kong's comment):
//     buf[i] += key[i % keylen]
void add_with_keystring(uint8_t *buf, size_t size, std::string_view key) {
	if (!buf || size == 0 || key.empty()) {
		return;
	}
	const size_t klen = key.size();
	for (size_t i = 0; i < size; ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] + static_cast<uint8_t>(key[i % klen]));
	}
}

// Buffer_SubtractWithKeyString@0x5e50f0 — mirror of add.
void subtract_with_keystring(uint8_t *buf, size_t size, std::string_view key) {
	if (!buf || size == 0 || key.empty()) {
		return;
	}
	const size_t klen = key.size();
	for (size_t i = 0; i < size; ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] - static_cast<uint8_t>(key[i % klen]));
	}
}

// Buffer_ReverseInPlace@0x5e4fc0 — swap front/back halves.
void reverse_in_place(uint8_t *buf, size_t size) {
	if (!buf || size == 0) {
		return;
	}
	size_t lo = 0;
	size_t hi = size - 1;
	while (lo < hi) {
		const uint8_t tmp = buf[lo];
		buf[lo] = buf[hi];
		buf[hi] = tmp;
		++lo;
		--hi;
	}
}

// Buffer_AddWithProgression@0x5e5000 —
//     for (i=0; i<size; ++i) { buf[i] += startValue + i; startValue += increment; }
// `startValue` is a signed char in the original, updated each iteration;
// the sum with `i` is an int but only the low 8 bits land in buf[i].
void add_with_progression(uint8_t *buf, size_t size, int8_t start_value, int8_t increment) {
	if (!buf || size == 0) {
		return;
	}
	int8_t s = start_value;
	for (size_t i = 0; i < size; ++i) {
		const int sum = static_cast<int>(s) + static_cast<int>(i);
		buf[i] = static_cast<uint8_t>(buf[i] + static_cast<uint8_t>(sum));
		s = static_cast<int8_t>(static_cast<int>(s) + static_cast<int>(increment));
	}
}

// Buffer_SubtractWithProgression@0x5e5040.
void subtract_with_progression(uint8_t *buf, size_t size, int8_t start_value, int8_t increment) {
	if (!buf || size == 0) {
		return;
	}
	int8_t s = start_value;
	for (size_t i = 0; i < size; ++i) {
		const int sum = static_cast<int>(s) + static_cast<int>(i);
		buf[i] = static_cast<uint8_t>(buf[i] - static_cast<uint8_t>(sum));
		s = static_cast<int8_t>(static_cast<int>(s) + static_cast<int>(increment));
	}
}

// Buffer_ScrambleWithLCG@0x62a8b0 — add each LCG step's low byte.
void scramble_with_lcg(uint8_t *buf, size_t size, LCGState &s) {
	if (!buf || size == 0) {
		return;
	}
	for (size_t i = 0; i < size; ++i) {
		const uint16_t step = clcg_step(s);
		buf[i] = static_cast<uint8_t>(buf[i] + static_cast<uint8_t>(step & 0xFFu));
	}
}

// Crypto_ApplyPRNG@0x5e51f0 — subtract each LCG step's low byte.
void apply_prng(uint8_t *buf, size_t size, LCGState &s) {
	if (!buf || size == 0) {
		return;
	}
	for (size_t i = 0; i < size; ++i) {
		const uint16_t step = clcg_step(s);
		buf[i] = static_cast<uint8_t>(buf[i] - static_cast<uint8_t>(step & 0xFFu));
	}
}

// Shared pre-phase setup. Encrypt + decrypt derive the same triple
// (start_value, increment, reverse_flag) from three LCG advances.
struct NwuDerived {
	LCGState state;      // advanced 3 steps; counter == 3
	int8_t start_value;  // 1-step-ahead LCG value (low byte as signed char)
	int8_t increment;    // 2-step-ahead LCG value
	bool reverse_flag;   // bit 0 of (low_byte(2step) * low_byte(magic) + 1)
};

NwuDerived nwu_derive(uint32_t seed) {
	NwuDerived d{};
	clcg_init(d.state, seed);
	const uint16_t magic_w = static_cast<uint16_t>(d.state.magic & 0xFFFFu);
	// Replicate the decomp's three-step advance:
	//   step1 = (state_0 * magic + 1)
	//   step2 = (step1    * magic + 1)
	//   step3 = (step2    * magic + 1)
	const uint16_t step1 = static_cast<uint16_t>(static_cast<uint16_t>((d.state.state & 0xFFFFu) * magic_w) + 1u);
	const uint16_t step2 = static_cast<uint16_t>(static_cast<uint16_t>(step1 * magic_w) + 1u);
	const uint16_t step3 = static_cast<uint16_t>(static_cast<uint16_t>(step2 * magic_w) + 1u);
	d.state.state = step3;
	d.state.counter = 3;
	d.start_value = static_cast<int8_t>(step1 & 0xFFu);
	d.increment = static_cast<int8_t>(step2 & 0xFFu);
	// reverse_flag = ((_BYTE)step2 * (_BYTE)magic + 1) & 1 from the decomp
	const uint8_t byte_prod = static_cast<uint8_t>(
			static_cast<uint8_t>(step2 & 0xFFu) * static_cast<uint8_t>(magic_w & 0xFFu));
	d.reverse_flag = ((byte_prod + 1u) & 1u) != 0u;
	return d;
}

} // namespace

uint32_t nwu_compute_seed(std::string_view key) {
	// [orig: NapiNP_ComputeKeySeed @ 0x618430 (retail) | Crypto_ComputeSeed @ 0x5e5160 (demo)]
	// Mirrors Crypto_ComputeSeed@0x5e5160: sum of (i + key[i]*key[i]) +
	// keylen + 50. Null-key sentinel (3252) is not reachable via string_view,
	// but we return it on empty to preserve the 'no key provided' signal.
	if (key.empty()) {
		return 3252u;
	}
	const int klen = static_cast<int>(key.size());
	int acc = 0;
	for (int i = 0; i < klen; ++i) {
		// key[i] is signed char in the original; promotion to int preserves sign.
		const int c = static_cast<int>(static_cast<signed char>(key[i]));
		acc += i + c * c;
	}
	return static_cast<uint32_t>(acc + klen + 50);
}

size_t nwu_encrypt(uint8_t *buf, size_t len, std::string_view key) {
	// [orig: NapiNP_DecryptBuffer @ 0x618880 (retail) | Crypto_DecryptBuffer @ 0x5e5230 (demo)]
	// SUBTRACT chain. Retail/onnet call this the *decrypt* entry (name-swap).
	if (!buf || len == 0 || key.empty()) {
		return 0;
	}
	const uint32_t seed = nwu_compute_seed(key);
	NwuDerived d = nwu_derive(seed);
	apply_prng(buf, len, d.state);
	subtract_with_progression(buf, len, d.start_value, d.increment);
	if (d.reverse_flag) {
		reverse_in_place(buf, len);
	}
	subtract_with_keystring(buf, len, key);
	return len;
}

size_t nwu_decrypt(uint8_t *buf, size_t len, std::string_view key) {
	// [orig: NapiNP_EncryptBuffer @ 0x6187b0 (retail) | PFF_EncryptBuffer @ 0x5e5300 (demo)]
	// ADD chain. Retail/onnet call this the *encrypt* entry (name-swap).
	if (!buf || len == 0 || key.empty()) {
		return 0;
	}
	const uint32_t seed = nwu_compute_seed(key);
	NwuDerived d = nwu_derive(seed);
	add_with_keystring(buf, len, key);
	if (d.reverse_flag) {
		reverse_in_place(buf, len);
	}
	add_with_progression(buf, len, d.start_value, d.increment);
	scramble_with_lcg(buf, len, d.state);
	return len;
}

} // namespace opennova
