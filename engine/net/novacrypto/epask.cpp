#include <net/novacrypto/epask.h>

#include <net/novacrypto/ap_alphabet.h>
#include <net/novacrypto/nwu.h>

#include <base/os_random/os_random.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova {

namespace {

// 16-bit unsigned mul, matches Python's `(a & 0xFFFF) * (b & 0xFFFF) & 0xFFFF`
// pattern used to derive ramp coefficients elsewhere in the protocol.
inline uint64_t mod_pow(uint64_t base, uint64_t exp, uint64_t mod) {
	if (mod == 1) return 0;
	uint64_t result = 1;
	base %= mod;
	while (exp > 0) {
		if (exp & 1) result = (result * base) % mod;
		exp >>= 1;
		base = (base * base) % mod;
	}
	return result;
}

uint64_t gcd(uint64_t a, uint64_t b) {
	while (b != 0) {
		uint64_t t = b;
		b = a % b;
		a = t;
	}
	return a;
}

bool is_prime(uint64_t n) {
	if (n < 2) return false;
	if (n < 4) return true;
	if ((n & 1) == 0) return false;
	for (uint64_t i = 3; i * i <= n; i += 2) {
		if (n % i == 0) return false;
	}
	return true;
}

// The parameter gate every modexp pass runs before touching a byte: the
// modulus must exceed 258 (so every byte+2 has a distinct residue) and the
// exponent must be strictly positive. A bundle that fails it is rejected
// outright — retail's encrypt returns -1 [orig: EPASK_Encrypt @0x666a9b]; it
// never reaches Crypto_ModularExponentiation, so a zero modulus can never
// divide. (The form builder discards that -1 [orig:
// CWnd_BuildFormFieldQueryString @0x657980] and posts the unwritten buffer;
// LobbyHttpFlow refuses such a bundle before the post.)
// [orig: EPASK_ModexpEncrypt @0x666600 — `*(__int64 *)this <= 258 ||
//  exponent <= 0` -> -1 @0x66668a]
constexpr const char *kRejectedParams =
		"EPASK params rejected: modulus must exceed 258 and exponent be positive";

bool modexp_params_pass(uint32_t exponent, uint32_t modulus) {
	return modulus > 258u && exponent != 0u;
}

// Brute-force modexp decrypt: for each 4-byte LE word in `data`, find
// the byte value b in [0, 255] such that pow(b + 2, exp, mod) == word.
// Returns the recovered byte stream (1/4 the input length). Used by
// epask_decrypt when we don't know the private exponent — feasible
// because mod < 300_000 means worst case is 256 modexp per byte.
// False (`error` named) on rejected params, a length not a multiple of 4, or a word with
// no 0..255 inverse: the ciphertext is input, a bad one a result (ADR 0049 d5).
bool modexp_decrypt_bf(const std::vector<uint8_t> &data, uint32_t exponent, uint32_t modulus,
                       std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	if (!modexp_params_pass(exponent, modulus)) {
		error = kRejectedParams;
		return false;
	}
	if (data.size() % 4 != 0) {
		error = "EPASK modexp data length must be a multiple of 4";
		return false;
	}
	out.reserve(data.size() / 4);
	for (size_t i = 0; i < data.size(); i += 4) {
		const uint32_t word = static_cast<uint32_t>(data[i])
		                    | (static_cast<uint32_t>(data[i + 1]) <<  8)
		                    | (static_cast<uint32_t>(data[i + 2]) << 16)
		                    | (static_cast<uint32_t>(data[i + 3]) << 24);
		bool found = false;
		for (uint32_t b = 0; b < 256; ++b) {
			if (mod_pow(b + 2, exponent, modulus) == word) {
				out.push_back(static_cast<uint8_t>(b));
				found = true;
				break;
			}
		}
		if (!found) {
			out.clear();
			error = "EPASK ciphertext word has no plaintext inverse — wrong params";
			return false;
		}
	}
	return true;
}

// [orig: EPASK_ModexpEncrypt @ 0x666600 (retail) — per byte Crypto_ModularExponentiation(byte+2,exp,mod)
//        @ 0x666470, stored as a 32-bit little-endian word. The "+2" is byte-confirmed.]
// False (`out` emptied) on params the gate rejects.
bool modexp_encrypt(const std::vector<uint8_t> &data, uint32_t exponent, uint32_t modulus,
                    std::vector<uint8_t> &out) {
	out.clear();
	if (!modexp_params_pass(exponent, modulus)) return false;
	out.reserve(data.size() * 4);
	for (uint8_t byte : data) {
		const uint32_t word = static_cast<uint32_t>(
			mod_pow(static_cast<uint64_t>(byte) + 2, exponent, modulus));
		out.push_back(static_cast<uint8_t>( word        & 0xFFu));
		out.push_back(static_cast<uint8_t>((word >>  8) & 0xFFu));
		out.push_back(static_cast<uint8_t>((word >> 16) & 0xFFu));
		out.push_back(static_cast<uint8_t>((word >> 24) & 0xFFu));
	}
	return true;
}

uint32_t atoi64_u32(std::string_view s) {
	const std::string owned(s);
	return static_cast<uint32_t>(std::strtoll(owned.c_str(), nullptr, 10));
}

} // namespace

bool generate_epask(EpaskParams &out) {
	// Modulus must satisfy 200_000 < p*q < 300_000. With p, q in
	// [sqrt(200_000), sqrt(300_000)] ≈ [448, 547], product is in the
	// right range when p ≠ q. Sample primes by rejection, drawing from the OS
	// CSPRNG (base/os_random).
	OsRandom gen;
	std::uniform_int_distribution<uint32_t> prime_pick(448, 547);
	std::uniform_int_distribution<uint32_t> exp_pick(10000, 49999);

	for (int attempt = 0; attempt < 200; ++attempt) {
		const uint32_t p = [&]() {
			while (true) {
				uint32_t v = prime_pick(gen);
				if (is_prime(v)) return v;
			}
		}();
		const uint32_t q = [&]() {
			while (true) {
				uint32_t v = prime_pick(gen);
				if (v != p && is_prime(v)) return v;
			}
		}();
		const uint64_t n = static_cast<uint64_t>(p) * q;
		if (n <= 200000ull || n >= 300000ull) continue;
		const uint64_t phi = static_cast<uint64_t>(p - 1) * (q - 1);
		for (int e_try = 0; e_try < 200; ++e_try) {
			const uint32_t e = exp_pick(gen);
			if (gcd(e, phi) == 1) {
				EpaskParams params;
				params.exponent = e;
				params.modulus  = static_cast<uint32_t>(n);
				// Key format: 13-digit ms timestamp + 6-digit random suffix
				// (matches onnw/protocol/epask.py::generate_epask_key).
				const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();
				char buf[32];
				std::snprintf(buf, sizeof(buf), "%013lld%06u",
				              static_cast<long long>(now_ms),
				              std::uniform_int_distribution<uint32_t>{0, 999999}(gen));
				params.key = buf;
				out = std::move(params);
				return true;
			}
		}
	}
	return false;
}

std::string epask_to_string(const EpaskParams &p) {
	return std::to_string(p.exponent) + ":" + std::to_string(p.modulus) + ":" + p.key;
}

// [orig: EPASK_ParseColonDelimitedString @ 0x666710 (retail) — splits 'exp:mod:key'.
//  Each numeric field is copied into a 512-byte temp and atoi64'd; the walk
//  returns -1 when the first (@0x66679b) or second (@0x6667e3) ':' is missing
//  or a numeric field reaches 512 bytes (@0x66678e / @0x6667d5). A missing
//  separator is therefore a rejected bundle, not a zero field.]
bool epask_from_string(const std::string &s, EpaskParams &out, std::string *error) {
	constexpr size_t kNumericFieldCap = 512;
	const auto fail = [error](const char *why) {
		if (error != nullptr) *error = why;
		return false;
	};
	const auto first = s.find(':');
	if (first == std::string::npos) {
		return fail("EPASK bundle rejected: missing first ':'");
	}
	const auto second = s.find(':', first + 1);
	if (second == std::string::npos) {
		return fail("EPASK bundle rejected: missing second ':'");
	}
	if (first >= kNumericFieldCap || second - first - 1 >= kNumericFieldCap) {
		return fail("EPASK bundle rejected: numeric field too long");
	}
	EpaskParams p;
	p.exponent = atoi64_u32(std::string_view{s}.substr(0, first));
	p.modulus = atoi64_u32(std::string_view{s}.substr(first + 1, second - first - 1));
	p.key = s.substr(second + 1);
	out = std::move(p);
	return true;
}

bool epask_decrypt(const std::string &ciphertext, const EpaskParams &params, std::string &out,
                   std::string *error) {
	// Mirror onnw/protocol/crypto.py::epask_decrypt:
	//   step1 = epask_decode_nibbles(ciphertext)
	//   step2 = nwu_decrypt(step1, key)
	//   step3 = epask_modexp_decrypt(step2, exp, mod, private_exp=None)  # brute force
	//   step4 = nwu_decrypt(step3, key)
	//   return ASCII
	//
	// Naming swap: onnet's nwu_decrypt == our nwu_encrypt (per memory
	// reference_nwu_names_swapped — the labels are reversed but the
	// transform pair is symmetric).
	out.clear();
	std::vector<uint8_t> step1;
	if (!decode_ap(ciphertext, step1)) {
		if (error != nullptr) *error = "EPASK field is not an A-P value (odd length or a character outside A-P)";
		return false;
	}
	if (!step1.empty()) nwu_encrypt(step1.data(), step1.size(), params.key);
	std::vector<uint8_t> step3;
	std::string why;
	if (!modexp_decrypt_bf(step1, params.exponent, params.modulus, step3, why)) {
		if (error != nullptr) *error = why;
		return false;
	}
	if (!step3.empty()) nwu_encrypt(step3.data(), step3.size(), params.key);
	out.assign(step3.begin(), step3.end());
	return true;
}

bool epask_encrypt(const std::string &plaintext, const EpaskParams &params, std::string &out,
                   std::string *error) {
	// [orig: EPASK_Encrypt @ 0x6669a0 (retail) — NWU(NapiNP_EncryptBufferAlt@0x6668e0, == 0x6187b0)
	//        -> modexp(EPASK_ModexpEncrypt) -> NWU -> A-P(NapiNP_EncodeToHexAlpha@0x666570). Dispatched from the
	//        edit-widget vtable +0x38 CWnd_BuildFormFieldQueryString@0x657760 (out buf = 8*len) <-
	//        UI_BuildURLAndSubmitRequest@0x63e3f0 ("?EPASK=exp:mod:key"). grill wave 3 NW-C2, MATCHING.]
	// Mirror onnw/protocol/crypto.py::epask_encrypt:
	//   step1 = nwu_encrypt(plaintext, key)
	//   step2 = epask_modexp_encrypt(step1, exp, mod)
	//   step3 = nwu_encrypt(step2, key)
	//   return epask_encode_nibbles(step3)
	// Swap: onnet's nwu_encrypt == our nwu_decrypt.
	const size_t nul = plaintext.find('\0');
	const size_t plaintext_len = (nul == std::string::npos) ? plaintext.size() : nul;
	std::vector<uint8_t> step1(plaintext.begin(), plaintext.begin() + plaintext_len);
	out.clear();
	if (!step1.empty()) nwu_decrypt(step1.data(), step1.size(), params.key);
	std::vector<uint8_t> step2;
	if (!modexp_encrypt(step1, params.exponent, params.modulus, step2)) {
		// The modexp pass's -1 is the encrypt's [orig: @0x666a99..0x666a9b].
		if (error != nullptr) *error = kRejectedParams;
		return false;
	}
	if (!step2.empty()) nwu_decrypt(step2.data(), step2.size(), params.key);
	out = encode_ap(step2);
	return true;
}

} // namespace opennova
