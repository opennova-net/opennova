#include "bfc1/bfc1.h"

#include <cstdint>
#include <cstring>

#include "common/test_expect.h"

int main() {
    static const uint8_t blob[] = {
        'B', 'F', 'C', '1',
        0x1b, 0x00, 0x00, 0x00,
        0x78, 0x9c, 0xf3, 0x2f, 0x48, 0xcd, 0xf3, 0xcb,
        0x2f, 0x4b, 0x54, 0x70, 0x72, 0x73, 0x36, 0x54,
        0x28, 0x49, 0x2d, 0x2e, 0x51, 0x28, 0x48, 0xac,
        0xcc, 0xc9, 0x4f, 0x4c, 0xe1, 0x02, 0x00, 0x82,
        0x47, 0x09, 0x37,
    };
    static const uint8_t expected[] = "OpenNova BFC1 test payload\n";

    TEST_EXPECT(bfc1_is_bfc1(blob, sizeof(blob)) == 1);
    TEST_EXPECT(bfc1_is_bfc1(blob, BFC1_HEADER_SIZE - 1) == 0);

    uint32_t uncompressed_size = 0;
    TEST_EXPECT(bfc1_uncompressed_size(blob, sizeof(blob), &uncompressed_size) == 0);
    TEST_EXPECT(uncompressed_size == sizeof(expected) - 1);

    uint8_t tiny[4] = {};
    size_t tiny_size = sizeof(tiny);
    TEST_EXPECT(bfc1_decompress(blob, sizeof(blob), tiny, &tiny_size) == -2);

    uint8_t out[64] = {};
    size_t out_size = sizeof(out);
    TEST_EXPECT(bfc1_decompress(blob, sizeof(blob), out, &out_size) == 0);
    TEST_EXPECT(out_size == sizeof(expected) - 1);
    TEST_EXPECT(std::memcmp(out, expected, out_size) == 0);

    static const uint8_t invalid[] = {'N', 'O', 'P', 'E'};
    TEST_EXPECT(bfc1_uncompressed_size(invalid, sizeof(invalid), &uncompressed_size) == -1);
    out_size = sizeof(out);
    TEST_EXPECT(bfc1_decompress(invalid, sizeof(invalid), out, &out_size) == -1);

    return 0;
}
