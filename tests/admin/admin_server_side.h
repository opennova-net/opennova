#pragma once

// The retail server's side of the remote-admin login, for the tests that play the server: the
// challenge AcceptConnection sends and the decrypt HandleLogin runs.
// [orig: CAdminServer_AcceptConnection @0x405785..0x4057d5; Crypto_DecryptBuffer @0x4375b0]

#include <net/admin/admin_protocol.h>
#include <net/novacrypto/nwu.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace admin_test {

// A challenge as AcceptConnection sends it: 32 bytes of 1..255 with byte 0 forced to 1, then the NUL.
inline std::vector<uint8_t> server_challenge(uint32_t seed) {
	std::vector<uint8_t> out(opennova::ADMIN_CHALLENGE_BYTES, 0);
	for (size_t i = 0; i < 32; ++i) {
		seed = seed * 1103515245u + 12345u;
		out[i] = static_cast<uint8_t>((seed >> 16) % 255 + 1);
	}
	out[0] = 1;
	return out;
}

// The server's decrypt, step for step the inverse of the encrypt: the LCG stream off, the
// progressive key off, the reverse, the key off. [orig: Crypto_DecryptBuffer @0x4375b0 ->
// Crypto_PRNGDecryptSub @0x4372a0, Crypto_SubProgressiveKey @0x4372f0, Buffer_ReverseInPlace2
// @0x437330, sub_437370]; HandleLogin then NULs bytes 31 and 63 (@0x4058e7/@0x4058f0).
inline void server_decrypt_login(uint8_t *buf, size_t size, const std::vector<uint8_t> &challenge) {
	const size_t key_len = static_cast<size_t>(
			std::find(challenge.begin(), challenge.end(), uint8_t{0}) - challenge.begin());
	uint32_t seed = 0;
	for (size_t i = 0; i < key_len; ++i) {
		const int32_t v = static_cast<int8_t>(challenge[i]);
		seed += static_cast<uint32_t>(v * v) + static_cast<uint32_t>(i);
	}
	seed += static_cast<uint32_t>(key_len) + 0x32u;
	const uint32_t m = opennova::NWU_LCG_MAGIC;
	const uint32_t s1 = (seed * m + 1) & 0xFFFFu;
	const uint32_t s2 = (m * s1 + 1) & 0xFFFFu;
	const uint32_t s3 = (m * s2 + 1) & 0xFFFFu;
	uint32_t state = s3;
	for (size_t i = 0; i < size; ++i) {
		state = (m * state + 1) & 0xFFFFu;
		buf[i] = static_cast<uint8_t>(buf[i] - (state & 0xFFu));
	}
	uint8_t add = static_cast<uint8_t>(s1);
	for (size_t i = 0; i < size; ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] - i - add);
		add = static_cast<uint8_t>(add + static_cast<uint8_t>(s2));
	}
	if ((s3 & 1u) != 0) std::reverse(buf, buf + size);
	for (size_t i = 0; i < size; ++i) buf[i] = static_cast<uint8_t>(buf[i] - challenge[i % key_len]);
	if (size > 63) {
		buf[31] = 0;
		buf[63] = 0;
	}
}

} // namespace admin_test
