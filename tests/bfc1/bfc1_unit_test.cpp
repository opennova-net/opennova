#include <formats/bfc1/bfc1.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::bfc1;

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

    // Unpacked in place; bytes that are no BFC1 left as they are.
    std::vector<uint8_t> packed(blob, blob + sizeof(blob));
    TEST_EXPECT(bfc1_unpack(packed) && packed.size() == sizeof(expected) - 1 &&
                std::memcmp(packed.data(), expected, packed.size()) == 0);
    std::vector<uint8_t> plain(invalid, invalid + sizeof(invalid));
    TEST_EXPECT(bfc1_unpack(plain) && plain == std::vector<uint8_t>(invalid, invalid + sizeof(invalid)));

    // The game's read [orig: AudioFile_DecompressBFC_Aligned @ 0x75AFB0; ZLib_DecompressBuffer
    // @ 0x63E80E]: the header's size unless the stream reaches its own end. A stream cut short
    // keeps the header's 27 bytes, what did not inflate zero.
    std::vector<uint8_t> broken(blob, blob + 12);
    TEST_EXPECT(bfc1_unpack(broken) && broken.size() == sizeof(expected) - 1 && broken.back() == 0);
    // A stream whose checksum is damaged inflates whole and keeps the header's size: the text.
    std::vector<uint8_t> unchecked(blob, blob + sizeof(blob));
    unchecked.back() ^= 0xFF;
    TEST_EXPECT(bfc1_unpack(unchecked) && unchecked.size() == sizeof(expected) - 1 &&
                std::memcmp(unchecked.data(), expected, unchecked.size()) == 0);
    // A header naming more than the stream holds: the stream reaches its end first and gives its
    // own length.
    std::vector<uint8_t> roomy(blob, blob + sizeof(blob));
    roomy[4] = 40;
    TEST_EXPECT(bfc1_unpack(roomy) && roomy.size() == sizeof(expected) - 1 &&
                std::memcmp(roomy.data(), expected, roomy.size()) == 0);
    // A stream inflate refuses at once (no zlib header) keeps the header's size, all zero.
    std::vector<uint8_t> refused = {'B', 'F', 'C', '1', 4, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_EXPECT(bfc1_unpack(refused) && refused == std::vector<uint8_t>(4, 0));

    return 0;
}
