// Crypto round-trip through the canonical CPT codec helpers.

#include <cpt/crypto.h>
#include <cstdint>
#include <cstdio>
#include <vector>

static int roundtrip_check(std::vector<uint8_t> original) {
    std::vector<uint8_t> data = original;
    opennova::rotate_encrypt(data.data(), data.size());
    if (data == original) {
        std::fprintf(stderr,
                     "FAIL: encrypt produced same bytes (len=%zu)\n",
                     data.size());
        return 1;
    }
    opennova::rotate_decrypt(data.data(), data.size());
    if (data != original) {
        std::fprintf(stderr,
                     "FAIL: decrypt did not restore original (len=%zu)\n",
                     data.size());
        return 1;
    }
    return 0;
}

int main() {
    // Small case
    std::vector<uint8_t> small = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    if (roundtrip_check(small) != 0) return 1;

    // Larger case — 4 KiB
    std::vector<uint8_t> big(4096);
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<uint8_t>(i * 37);
    }
    if (roundtrip_check(big) != 0) return 1;

    std::printf("OK: crypto round-trip preserves data (16 and 4096 bytes)\n");
    return 0;
}
