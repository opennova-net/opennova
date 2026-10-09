// SHA-256 (FIPS 180-4), header-only: the digest a stored secret is compared through (the
// NovaWorld website's session tokens are kept only as their SHA-256), and the content digest
// the retail-pinned tests compare listings and payloads by. A streaming `Sha256`
// (update/finish), the one-shot `sha256`, and `sha256_hex`, the 64 lower-case hex digits.
//
// Not a port: nothing here is witnessed engine behaviour.
//
// STAGED, NOT WIRED: its include consumer is the NovaWorld service's web-session store
// (apps/novaworld_server, the hosted-servers plan's PR E), which hashes each session token
// before it is kept; delete this paragraph when that lands.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace opennova::io {

inline constexpr size_t kSha256DigestSize = 32;
using Sha256Digest = std::array<uint8_t, kSha256DigestSize>;
static_assert(sizeof(Sha256Digest) == 32, "a SHA-256 digest is 32 bytes");

namespace detail {

inline constexpr uint32_t sha256_rotr(uint32_t x, int n) noexcept {
	return (x >> n) | (x << (32 - n)); // 0 < n < 32 at every call
}

} // namespace detail

// The streaming hash. finish() returns the digest and resets the object, so it is reusable at
// once and never finalises the same state twice. The length is counted in a uint64_t, so it
// stays exact where size_t is 32-bit.
class Sha256 {
public:
	Sha256() noexcept { reset(); }

	void reset() noexcept {
		static constexpr uint32_t kInitial[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
		};
		std::memcpy(state_, kInitial, sizeof(state_));
		std::memset(buffer_, 0, sizeof(buffer_));
		buffered_ = 0;
		total_bytes_ = 0;
	}

	void update(const void *data, size_t size) noexcept {
		const auto *bytes = static_cast<const uint8_t *>(data);
		total_bytes_ += uint64_t(size);
		if (buffered_ > 0) {
			const size_t take = size < 64 - buffered_ ? size : 64 - buffered_;
			std::memcpy(buffer_ + buffered_, bytes, take);
			buffered_ += take;
			bytes += take;
			size -= take;
			if (buffered_ < 64) return;
			compress(buffer_);
			buffered_ = 0;
		}
		while (size >= 64) {
			compress(bytes);
			bytes += 64;
			size -= 64;
		}
		if (size > 0) {
			std::memcpy(buffer_, bytes, size);
			buffered_ = size;
		}
	}

	void update(std::string_view text) noexcept { update(text.data(), text.size()); }

	Sha256Digest finish() noexcept {
		const uint64_t bit_length = total_bytes_ << 3;
		buffer_[buffered_++] = 0x80;
		if (buffered_ > 56) {
			std::memset(buffer_ + buffered_, 0, 64 - buffered_);
			compress(buffer_);
			buffered_ = 0;
		}
		std::memset(buffer_ + buffered_, 0, 56 - buffered_);
		for (int i = 0; i < 8; ++i) buffer_[63 - i] = uint8_t(bit_length >> (8 * i));
		compress(buffer_);
		Sha256Digest digest{};
		for (int i = 0; i < 8; ++i) {
			digest[size_t(i) * 4] = uint8_t(state_[i] >> 24);
			digest[size_t(i) * 4 + 1] = uint8_t(state_[i] >> 16);
			digest[size_t(i) * 4 + 2] = uint8_t(state_[i] >> 8);
			digest[size_t(i) * 4 + 3] = uint8_t(state_[i]);
		}
		reset();
		return digest;
	}

private:
	void compress(const uint8_t block[64]) noexcept {
		using detail::sha256_rotr;
		static constexpr uint32_t k[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
			0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
			0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
			0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
			0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
			0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
			0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
			0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
			0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
		};
		uint32_t w[64];
		for (int i = 0; i < 16; ++i) {
			w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
					(uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
		uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
		for (int i = 0; i < 64; ++i) {
			const uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
			const uint32_t ch = (e & f) ^ (~e & g);
			const uint32_t t1 = h + s1 + ch + k[i] + w[i];
			const uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
			const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = s0 + maj;
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		state_[0] += a;
		state_[1] += b;
		state_[2] += c;
		state_[3] += d;
		state_[4] += e;
		state_[5] += f;
		state_[6] += g;
		state_[7] += h;
	}

	uint32_t state_[8];
	uint8_t buffer_[64];
	size_t buffered_;
	uint64_t total_bytes_;
};

inline Sha256Digest sha256(const void *data, size_t size) noexcept {
	Sha256 hash;
	hash.update(data, size);
	return hash.finish();
}

inline Sha256Digest sha256(std::string_view text) noexcept { return sha256(text.data(), text.size()); }

// 64 lower-case hex digits, the first byte first.
inline std::string sha256_hex(const Sha256Digest &digest) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string out;
	out.reserve(kSha256DigestSize * 2);
	for (const uint8_t byte : digest) {
		out.push_back(kDigits[byte >> 4]);
		out.push_back(kDigits[byte & 0xFu]);
	}
	return out;
}

inline std::string sha256_hex(const void *data, size_t size) { return sha256_hex(sha256(data, size)); }

} // namespace opennova::io
