// opennova::io unit tests: LE primitives, fixed-point, bounds-checked byte
// cursors, LSB-first bit streams, and the ASCII string helpers.

#include <cstring>
#include <string>
#include <vector>

#include <io/bit_stream.h>
#include <io/byte_reader.h>
#include <io/byte_writer.h>
#include <io/bam.h>
#include <io/fixed.h>
#include <io/le.h>
#include <io/strutil.h>

#include "common/test_expect.h"

using namespace opennova;

static int test_le_primitives()
{
    const uint8_t bytes[] = {0x78, 0x56, 0x34, 0x12};
    TEST_EXPECT(io::read_u8(bytes) == 0x78);
    TEST_EXPECT(io::read_u16_le(bytes) == 0x5678);
    TEST_EXPECT(io::read_u32_le(bytes) == 0x12345678u);
    TEST_EXPECT(io::read_s16_le(bytes) == 0x5678);
    TEST_EXPECT(io::read_s32_le(bytes) == 0x12345678);

    uint8_t out[4] = {0};
    io::write_u32_le(out, 0x12345678u);
    TEST_EXPECT(std::memcmp(out, bytes, 4) == 0);
    io::write_u16_le(out, 0xBEEF);
    TEST_EXPECT(out[0] == 0xEF && out[1] == 0xBE);

    io::write_f32_le(out, 1.5f);
    TEST_EXPECT(io::read_f32_le(out) == 1.5f);

    const uint8_t neg[] = {0xFF, 0xFF};
    TEST_EXPECT(io::read_s16_le(neg) == -1);
    return 0;
}

static int test_fixed_point()
{
    TEST_EXPECT(io::fp16_16_to_float(65536) == 1.0f);
    TEST_EXPECT(io::fp16_16_to_float(-32768) == -0.5f);
    TEST_EXPECT(io::float_to_fp16_16(1.0f) == 65536);
    TEST_EXPECT(io::fp14_to_float(16384) == 1.0f);
    TEST_EXPECT(io::fp14_to_float(-16384) == -1.0f);

    const uint8_t one_16_16[] = {0x00, 0x00, 0x01, 0x00};
    TEST_EXPECT(io::read_fp_16_16(one_16_16) == 1.0f);
    const uint8_t one_14[] = {0x00, 0x40};
    TEST_EXPECT(io::read_fp_14(one_14) == 1.0f);
    return 0;
}

static int test_byte_reader_bounds()
{
    const uint8_t bytes[] = {0xAA, 0xBB, 0xCC};
    io::ByteReader r(bytes, sizeof(bytes));
    TEST_EXPECT(r.read_u16() == 0xBBAA);
    // 2 bytes left needed for u32: out-of-range returns 0 and does not advance.
    TEST_EXPECT(r.read_u32() == 0);
    TEST_EXPECT(r.position() == 2);
    TEST_EXPECT(r.remaining() == 1);
    TEST_EXPECT(r.read_u8() == 0xCC);
    TEST_EXPECT(r.read_u8() == 0);
    TEST_EXPECT(r.position() == 3);

    io::ByteReader r2(bytes, sizeof(bytes));
    uint8_t buf[8] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
    r2.read_bytes(buf, 8); // overflow: zero-fill, no advance
    for (size_t i = 0; i < 8; ++i)
        TEST_EXPECT(buf[i] == 0);
    TEST_EXPECT(r2.position() == 0);
    r2.skip(100);
    TEST_EXPECT(r2.position() == 3 && r2.remaining() == 0);
    return 0;
}

static int test_byte_writer_roundtrip()
{
    io::ByteWriter w;
    w.write_u8(0x01);
    w.write_i8(-2);
    w.write_u16(0x0304);
    w.write_i16(-5);
    w.write_u32(0x06070809u);
    w.write_i32(-10);
    w.write_f32(2.25f);
    w.write_fixed16(1.5f);
    w.write_fixed_string("ab", 4);

    const std::vector<uint8_t> bytes = w.data();
    io::ByteReader r(bytes.data(), bytes.size());
    TEST_EXPECT(r.read_u8() == 0x01);
    TEST_EXPECT(r.read_i8() == -2);
    TEST_EXPECT(r.read_u16() == 0x0304);
    TEST_EXPECT(r.read_i16() == -5);
    TEST_EXPECT(r.read_u32() == 0x06070809u);
    TEST_EXPECT(r.read_i32() == -10);
    TEST_EXPECT(r.read_f32() == 2.25f);
    TEST_EXPECT(r.read_fixed16() == 1.5f);
    char s[4];
    r.read_fixed_string(s, 4);
    TEST_EXPECT(s[0] == 'a' && s[1] == 'b' && s[2] == 0 && s[3] == 0);
    TEST_EXPECT(r.remaining() == 0);
    return 0;
}

static int test_bit_stream_roundtrip()
{
    io::BitWriter w;
    w.write_field(3, 5);
    w.write_field(7, 100);
    w.write_field(13, 4095);
    w.align_dword();
    w.write_field(32, 0xDEADBEEFu);
    const uint8_t raw[] = {0x12, 0x34};
    w.write_bytes(raw, sizeof(raw));
    w.write_field(5, 21);

    io::BitReader r(w.data(), w.high_water());
    TEST_EXPECT(r.read_bits(3) == 5);
    TEST_EXPECT(r.read_bits(7) == 100);
    TEST_EXPECT(r.read_bits(13) == 4095);
    r.align_dword();
    TEST_EXPECT(r.read_bits(32) == 0xDEADBEEFu);
    TEST_EXPECT(r.read_bits(8) == 0x12);
    TEST_EXPECT(r.read_bits(8) == 0x34);
    TEST_EXPECT(r.read_bits(5) == 21);

    // Tail path: reader window smaller than a dword stays exact.
    const uint8_t tail[] = {0xB5}; // 0b1011_0101
    io::BitReader tr(tail, sizeof(tail));
    TEST_EXPECT(tr.read_bits(4) == 0x5);
    TEST_EXPECT(tr.read_bits(4) == 0xB);
    TEST_EXPECT(tr.at_end());
    TEST_EXPECT(tr.read_bits(8) == 0);
    return 0;
}

static int test_strutil()
{
    TEST_EXPECT(strutil::ascii_tolower('A') == 'a');
    TEST_EXPECT(strutil::ascii_tolower('z') == 'z');
    TEST_EXPECT(strutil::ascii_tolower('0') == '0');
    TEST_EXPECT(strutil::to_lower("MiXeD09.TGA") == "mixed09.tga");
    TEST_EXPECT(strutil::iequals("Water.TGA", "water.tga"));
    TEST_EXPECT(!strutil::iequals("water", "water "));
    TEST_EXPECT(strutil::iless("abc", "abd"));
    TEST_EXPECT(!strutil::iless("ABD", "abc"));
    TEST_EXPECT(strutil::iless("ab", "abc"));
    TEST_EXPECT(strutil::ends_with_icase("terrain.TRN", ".trn"));
    TEST_EXPECT(!strutil::ends_with_icase(".trn", "terrain.trn"));
    return 0;
}

static int test_bam_wrap_arithmetic()
{
    // The x86 semantics the BAM ports rely on, pinned as defined behavior:
    // add/sub/dbl wrap two's-complement, sar shifts arithmetically, abs maps
    // INT32_MIN to itself (x86 neg).
    constexpr int32_t kMax = 2147483647;               // INT32_MAX
    constexpr int32_t kMin = -kMax - 1;                // INT32_MIN
    TEST_EXPECT(io::bam_add(kMax, 1) == kMin);         // wrap over the seam
    TEST_EXPECT(io::bam_sub(kMin, kMax) == 1);         // shortest arc across it
    TEST_EXPECT(io::bam_sub(kMax, kMin) == -1);
    TEST_EXPECT(io::bam_dbl(0x40000001) == kMin + 2);  // 2*x wraps like shl
    TEST_EXPECT(io::bam_sar(-1, 1) == -1);             // arithmetic, not logical
    TEST_EXPECT(io::bam_sar(-8, 2) == -2);
    TEST_EXPECT(io::bam_sar(kMin, 2) == kMin / 4);
    TEST_EXPECT(io::bam_sar(7, 1) == 3);
    TEST_EXPECT(io::bam_abs(-5) == 5);
    TEST_EXPECT(io::bam_abs(kMin) == kMin);            // x86 neg: stays put
    return 0;
}

int main()
{
    if (test_le_primitives()) return 1;
    if (test_bam_wrap_arithmetic()) return 1;
    if (test_fixed_point()) return 1;
    if (test_byte_reader_bounds()) return 1;
    if (test_byte_writer_roundtrip()) return 1;
    if (test_bit_stream_roundtrip()) return 1;
    if (test_strutil()) return 1;
    std::printf("io_test: all checks passed\n");
    return 0;
}
