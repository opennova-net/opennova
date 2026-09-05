#include <formats/scr/scr.h>

#include <cstdint>
#include <cstring>

#include "common/test_expect.h"

using namespace opennova::scr;

int main() {
    static const uint8_t encrypted[] = {
        0x53, 0x43, 0x52, 0x00, 0xbc, 0xe1, 0xc6, 0x36,
        0xeb, 0x54, 0x4c, 0x16, 0x43, 0x72, 0x55, 0xc4,
        0x09, 0x0f, 0xc4, 0x25, 0x2e, 0x9c,
    };
    static const uint8_t expected[] = "OpenNova SCR test\n";

    TEST_EXPECT(scr_is_scr(encrypted, sizeof(encrypted)) == 1);
    TEST_EXPECT(scr_get_version(encrypted, sizeof(encrypted)) == 0);
    TEST_EXPECT(scr_is_scr(encrypted, 3) == 0);
    TEST_EXPECT(scr_get_version(encrypted, 3) == 0);

    static const uint8_t scr0_plain[] = {
        'S', 'C', 'R', '0', 'm', 'u', 's', 'i', 'c'
    };
    TEST_EXPECT(scr_is_scr(scr0_plain, sizeof(scr0_plain)) == 0);
    TEST_EXPECT(scr_get_version(scr0_plain, sizeof(scr0_plain)) == 0);

    uint8_t tiny[4] = {};
    size_t tiny_size = sizeof(tiny);
    TEST_EXPECT(scr_decrypt_buf(encrypted, sizeof(encrypted), tiny, &tiny_size, SCR_KEY_DEFAULT) == -2);
    TEST_EXPECT(tiny_size == sizeof(expected) - 1);

    uint8_t out[64] = {};
    size_t out_size = sizeof(out);
    TEST_EXPECT(scr_decrypt_buf(encrypted, sizeof(encrypted), out, &out_size, SCR_KEY_DEFAULT) == 0);
    TEST_EXPECT(out_size == sizeof(expected) - 1);
    TEST_EXPECT(std::memcmp(out, expected, out_size) == 0);

    static const uint8_t invalid[] = {'N', 'O', 'P', 'E'};
    out_size = sizeof(out);
    TEST_EXPECT(scr_decrypt_buf(invalid, sizeof(invalid), out, &out_size, SCR_KEY_DEFAULT) == -1);

    out_size = sizeof(out);
    TEST_EXPECT(scr_decrypt_buf(scr0_plain, sizeof(scr0_plain), out, &out_size, SCR_KEY_DEFAULT) == -1);

    /* Write side: wrap plaintext, then read it back through the decrypt path
       with each retail key (the container is key-agnostic; the pair must
       round-trip byte-exactly). */
    static const uint8_t plain[] = "technique T0 { pass P0 {} }\r\n";
    const uint32_t keys[] = {SCR_KEY_DEFAULT, SCR_KEY_JO_DFX2, SCR_KEY_SHADERS};
    for (size_t k = 0; k < 3; ++k) {
        uint8_t wrapped[64] = {};
        size_t wrapped_size = sizeof(wrapped);
        TEST_EXPECT(scr_encrypt_buf(plain, sizeof(plain) - 1, wrapped, &wrapped_size, keys[k], 1) == 0);
        TEST_EXPECT(wrapped_size == sizeof(plain) - 1 + SCR_HEADER_SIZE);
        TEST_EXPECT(scr_is_scr(wrapped, wrapped_size) == 1);
        TEST_EXPECT(scr_get_version(wrapped, wrapped_size) == 1);

        uint8_t round[64] = {};
        size_t round_size = sizeof(round);
        TEST_EXPECT(scr_decrypt_buf(wrapped, wrapped_size, round, &round_size, keys[k]) == 0);
        TEST_EXPECT(round_size == sizeof(plain) - 1);
        TEST_EXPECT(std::memcmp(round, plain, round_size) == 0);
    }

    /* Capacity probe mirrors the decrypt side: too-small reports the need. */
    uint8_t small_out[4] = {};
    size_t small_size = sizeof(small_out);
    TEST_EXPECT(scr_encrypt_buf(plain, sizeof(plain) - 1, small_out, &small_size, SCR_KEY_SHADERS, 1) == -2);
    TEST_EXPECT(small_size == sizeof(plain) - 1 + SCR_HEADER_SIZE);

    return 0;
}
