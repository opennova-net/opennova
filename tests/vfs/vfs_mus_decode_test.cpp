/* vfs_decode_payload generic payload handling around SCR0.

   MUS .bin files in JO_CLIENT/localres.pff are already plaintext SCR0 payloads.
   SCR0 is not an encrypted SCR container and must pass through unchanged. The
   headerless MUS cipher experimented with in PR #53 is intentionally outside
   VFS auto-decode; without an explicit SCR/PFF marker it is just opaque bytes. */

#include "vfs/vfs_decode.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/test_expect.h"

namespace {

constexpr uint32_t kScrKeyDefault = 0xABEEFACEu;
constexpr uint32_t kScrKeyJoDfx2 = 0x2A5A8EADu;

uint32_t rol32(uint32_t value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

void xor_scr_keystream(std::vector<uint8_t> &bytes, uint32_t key) {
    for (uint8_t &byte : bytes) {
        key = rol32(key + rol32(key, 11), 4) ^ 1u;
        byte ^= static_cast<uint8_t>(key);
    }
}

std::vector<uint8_t> minimal_plain_mus() {
    std::vector<uint8_t> plain(48, 0);
    plain[0] = 'S';
    plain[1] = 'C';
    plain[2] = 'R';
    plain[3] = '0';
    plain[5] = 0x01; // version 0x00000100
    plain[8] = 0x01; // chunk_count = 1
    return plain;
}

std::vector<uint8_t> headerless_mus_ciphertext(const std::vector<uint8_t> &plain) {
    std::vector<uint8_t> payload;
    if (plain.size() > 4) {
        payload.assign(plain.begin() + 4, plain.end());
    }
    std::vector<uint8_t> out(payload.rbegin(), payload.rend());
    xor_scr_keystream(out, kScrKeyJoDfx2);
    return out;
}

std::vector<uint8_t> scr_wrap_default_key(const std::vector<uint8_t> &plain) {
    std::vector<uint8_t> payload = plain;
    xor_scr_keystream(payload, kScrKeyDefault);
    std::vector<uint8_t> out = {'S', 'C', 'R', 0};
    out.insert(out.end(), payload.rbegin(), payload.rend());
    return out;
}

} // namespace

int main() {
    const std::vector<uint8_t> plain = minimal_plain_mus();

    std::vector<uint8_t> data = plain;
    TEST_EXPECT(opennova::vfs_decode_payload(data));
    TEST_EXPECT(data == plain);

    std::vector<uint8_t> scr0_marker = {'S', 'C', 'R', '0', 'n', 'o', 't', '-', 'm', 'u', 's'};
    const std::vector<uint8_t> scr0_original = scr0_marker;
    TEST_EXPECT(opennova::vfs_decode_payload(scr0_marker));
    TEST_EXPECT(scr0_marker == scr0_original);

    std::vector<uint8_t> headerless = headerless_mus_ciphertext(plain);
    const std::vector<uint8_t> headerless_original = headerless;
    TEST_EXPECT(opennova::vfs_decode_payload(headerless));
    TEST_EXPECT(headerless == headerless_original);

    const std::vector<uint8_t> arbitrary_plain = {'I', 'T', 'E', 'M', 'S', '.', 'D', 'E', 'F', '\n'};
    std::vector<uint8_t> wrapped = scr_wrap_default_key(arbitrary_plain);
    TEST_EXPECT(opennova::vfs_decode_payload(wrapped));
    TEST_EXPECT(wrapped == arbitrary_plain);

    std::printf("vfs_mus_decode_test OK\n");
    return 0;
}
