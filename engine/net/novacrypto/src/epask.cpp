#include <novacrypto/epask.h>

#include <novacrypto/nwu.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <string_view>
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

// A-P alphabet: byte → 2 chars, low nibble first ('A'+lo), high nibble
// second ('A'+hi). Matches onnw/protocol/crypto.py::epask_decode_nibbles
// and is the same packing as PUBcrypto's _decode_ap. Kept local because
// pubcrypto.cpp's helper has internal linkage.
std::vector<uint8_t> decode_ap(const std::string &s) {
	if (s.size() % 2 != 0) {
		throw std::runtime_error("EPASK ciphertext length must be even");
	}
	std::vector<uint8_t> out;
	out.reserve(s.size() / 2);
	for (size_t i = 0; i < s.size(); i += 2) {
		const int low  = s[i]     - 'A';
		const int high = s[i + 1] - 'A';
		if (low < 0 || low > 15 || high < 0 || high > 15) {
			throw std::runtime_error("EPASK ciphertext contains non-A-P character");
		}
		out.push_back(static_cast<uint8_t>(low | (high << 4)));
	}
	return out;
}

// [orig: NapiNP_EncodeToHexAlpha @ 0x666570 (retail) — A-P, low nibble first ('A'+lo, 'A'+hi)]
std::string encode_ap(const std::vector<uint8_t> &data) {
	std::string out;
	out.reserve(data.size() * 2);
	for (uint8_t b : data) {
		out.push_back(static_cast<char>('A' + (b & 0x0Fu)));
		out.push_back(static_cast<char>('A' + ((b >> 4) & 0x0Fu)));
	}
	return out;
}

// Brute-force modexp decrypt: for each 4-byte LE word in `data`, find
// the byte value b in [0, 255] such that pow(b + 2, exp, mod) == word.
// Returns the recovered byte stream (1/4 the input length). Used by
// epask_decrypt when we don't know the private exponent — feasible
// because mod < 300_000 means worst case is 256 modexp per byte.
std::vector<uint8_t> modexp_decrypt_bf(const std::vector<uint8_t> &data,
                                       uint32_t exponent, uint32_t modulus) {
	if (data.size() % 4 != 0) {
		throw std::runtime_error("EPASK modexp data length must be a multiple of 4");
	}
	std::vector<uint8_t> out;
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
			throw std::runtime_error("EPASK ciphertext word has no plaintext inverse — wrong params");
		}
	}
	return out;
}

// [orig: EPASK_ModexpEncrypt @ 0x666600 (retail) — per byte modular_exponentiation(byte+2,exp,mod)
//        @ 0x666470, stored as a 32-bit little-endian word. The "+2" is byte-confirmed.]
std::vector<uint8_t> modexp_encrypt(const std::vector<uint8_t> &data,
                                    uint32_t exponent, uint32_t modulus) {
	std::vector<uint8_t> out;
	out.reserve(data.size() * 4);
	for (uint8_t byte : data) {
		const uint32_t word = static_cast<uint32_t>(
			mod_pow(static_cast<uint64_t>(byte) + 2, exponent, modulus));
		out.push_back(static_cast<uint8_t>( word        & 0xFFu));
		out.push_back(static_cast<uint8_t>((word >>  8) & 0xFFu));
		out.push_back(static_cast<uint8_t>((word >> 16) & 0xFFu));
		out.push_back(static_cast<uint8_t>((word >> 24) & 0xFFu));
	}
	return out;
}

uint32_t atoi64_u32(std::string_view s) {
	const std::string owned(s);
	return static_cast<uint32_t>(std::strtoll(owned.c_str(), nullptr, 10));
}

} // namespace

EpaskParams generate_epask() {
	// Modulus must satisfy 200_000 < p*q < 300_000. With p, q in
	// [sqrt(200_000), sqrt(300_000)] ≈ [448, 547], product is in the
	// right range when p ≠ q. Sample primes by rejection.
	static thread_local std::mt19937_64 gen{std::random_device{}()};
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
				return params;
			}
		}
	}
	throw std::runtime_error("generate_epask: could not converge on valid params");
}

std::string epask_to_string(const EpaskParams &p) {
	return std::to_string(p.exponent) + ":" + std::to_string(p.modulus) + ":" + p.key;
}

// [orig: parse_colon_delimited_string @ 0x666710 (retail) — splits 'exp:mod:key']
EpaskParams epask_from_string(const std::string &s) {
	const auto first = s.find(':');
	EpaskParams p;
	if (first == std::string::npos) {
		p.exponent = atoi64_u32(s);
		return p;
	}
	const auto second = s.find(':', first + 1);
	p.exponent = atoi64_u32(std::string_view{s}.substr(0, first));
	if (second == std::string::npos) {
		p.modulus = atoi64_u32(std::string_view{s}.substr(first + 1));
		return p;
	}
	p.modulus = atoi64_u32(std::string_view{s}.substr(first + 1, second - first - 1));
	p.key = s.substr(second + 1);
	return p;
}

std::string epask_decrypt(const std::string &ciphertext, const EpaskParams &params) {
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
	auto step1 = decode_ap(ciphertext);
	if (!step1.empty()) nwu_encrypt(step1.data(), step1.size(), params.key);
	auto step3 = modexp_decrypt_bf(step1, params.exponent, params.modulus);
	if (!step3.empty()) nwu_encrypt(step3.data(), step3.size(), params.key);
	return std::string(step3.begin(), step3.end());
}

std::string epask_encrypt(const std::string &plaintext, const EpaskParams &params) {
	// [orig: EPASK_Encrypt @ 0x6669a0 (retail) — NWU(NapiNP_EncryptBufferAlt@0x6668e0, == 0x6187b0)
	//        -> modexp(EPASK_ModexpEncrypt) -> NWU -> A-P(NapiNP_EncodeToHexAlpha@0x666570). Dispatched from the
	//        edit-widget vtable +0x38 build_form_field_query_string@0x657760 (out buf = 8*len) <-
	//        build_url_and_submit_request@0x63e3f0 ("?EPASK=exp:mod:key"). grill wave 3 NW-C2, MATCHING.]
	// Mirror onnw/protocol/crypto.py::epask_encrypt:
	//   step1 = nwu_encrypt(plaintext, key)
	//   step2 = epask_modexp_encrypt(step1, exp, mod)
	//   step3 = nwu_encrypt(step2, key)
	//   return epask_encode_nibbles(step3)
	// Swap: onnet's nwu_encrypt == our nwu_decrypt.
	const size_t nul = plaintext.find('\0');
	const size_t plaintext_len = (nul == std::string::npos) ? plaintext.size() : nul;
	std::vector<uint8_t> step1(plaintext.begin(), plaintext.begin() + plaintext_len);
	if (!step1.empty()) nwu_decrypt(step1.data(), step1.size(), params.key);
	auto step2 = modexp_encrypt(step1, params.exponent, params.modulus);
	if (!step2.empty()) nwu_decrypt(step2.data(), step2.size(), params.key);
	return encode_ap(step2);
}

} // namespace opennova
