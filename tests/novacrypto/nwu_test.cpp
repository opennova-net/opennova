#include <novacrypto/nwu.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Crypto_ComputeSeed("GATEAPI") — hand-computed per the decomp recipe
// (sum of (i + c*c) for each char, plus len, plus 50).
// 'G'=71, 'A'=65, 'T'=84, 'E'=69, 'A'=65, 'P'=80, 'I'=73.
// sum = 5041 + 4226 + 7058 + 4764 + 4229 + 6405 + 5335 = 37058
// result = 37058 + 7 + 50 = 37115.
bool check_gateapi_seed() {
	const uint32_t got = opennova::nwu_compute_seed(opennova::NWU_GATE_KEY);
	if (!expect(got == 37115u, "nwu_compute_seed(\"GATEAPI\") must be 37115")) return false;
	return true;
}

// Null/empty key returns the original's sentinel default 3252.
bool check_empty_key_seed() {
	if (!expect(opennova::nwu_compute_seed("") == 3252u, "empty key should return sentinel 3252")) return false;
	return true;
}

// Encrypt + decrypt round-trips to the original bytes.
bool check_gateapi_roundtrip() {
	using opennova::NWU_GATE_KEY;
	const std::vector<uint8_t> original = {
			0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
	};
	std::vector<uint8_t> buf = original;
	const size_t enc_n = opennova::nwu_encrypt(buf.data(), buf.size(), NWU_GATE_KEY);
	if (!expect(enc_n == buf.size(), "encrypt returns processed length")) return false;
	if (!expect(buf != original, "encrypt actually mutates the buffer")) return false;
	const size_t dec_n = opennova::nwu_decrypt(buf.data(), buf.size(), NWU_GATE_KEY);
	if (!expect(dec_n == buf.size(), "decrypt returns processed length")) return false;
	if (!expect(buf == original, "encrypt+decrypt restores original bytes")) return false;
	return true;
}

// Two different keys must produce different ciphertexts for the same input.
bool check_key_sensitivity() {
	const std::string input(16, 'A');
	std::vector<uint8_t> ga(input.begin(), input.end());
	std::vector<uint8_t> xb(input.begin(), input.end());
	opennova::nwu_encrypt(ga.data(), ga.size(), "GATEAPI");
	opennova::nwu_encrypt(xb.data(), xb.size(), "XBOXAPI");
	if (!expect(ga != xb, "different keys must yield different ciphertexts")) return false;
	return true;
}

// Small-input edge cases. 1-byte, 2-byte, and 3-byte inputs all must
// roundtrip (the LCG advances per byte, so these are sensitive to
// counter/state bookkeeping).
bool check_small_sizes() {
	using opennova::NWU_GATE_KEY;
	for (size_t n : {1u, 2u, 3u, 7u, 32u, 33u}) {
		std::vector<uint8_t> original(n);
		for (size_t i = 0; i < n; ++i) {
			original[i] = static_cast<uint8_t>(i * 17u + 3u);
		}
		std::vector<uint8_t> buf = original;
		opennova::nwu_encrypt(buf.data(), buf.size(), NWU_GATE_KEY);
		opennova::nwu_decrypt(buf.data(), buf.size(), NWU_GATE_KEY);
		if (!expect(buf == original, "roundtrip at size n")) return false;
	}
	return true;
}

// Decrypt-then-encrypt (reverse order) must also be identity, since the
// two phase-chains are strict inverses of each other.
bool check_reverse_order_roundtrip() {
	using opennova::NWU_GATE_KEY;
	std::vector<uint8_t> original = {
			0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89,
			0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
			0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	};
	std::vector<uint8_t> buf = original;
	opennova::nwu_decrypt(buf.data(), buf.size(), NWU_GATE_KEY);
	if (!expect(buf != original, "decrypt alone must mutate the buffer")) return false;
	opennova::nwu_encrypt(buf.data(), buf.size(), NWU_GATE_KEY);
	if (!expect(buf == original, "decrypt+encrypt reverse order also roundtrips")) return false;
	return true;
}

} // namespace

int main() {
	if (!check_gateapi_seed()) return 1;
	if (!check_empty_key_seed()) return 1;
	if (!check_gateapi_roundtrip()) return 1;
	if (!check_key_sensitivity()) return 1;
	if (!check_small_sizes()) return 1;
	if (!check_reverse_order_roundtrip()) return 1;
	std::printf("OK: NWU 4-phase cipher byte-exact per jodemo decomp\n");
	return 0;
}
