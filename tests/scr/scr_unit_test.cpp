#include "scr/scr.h"

#include <cstdint>
#include <cstring>

#include "common/test_expect.h"

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

    return 0;
}
