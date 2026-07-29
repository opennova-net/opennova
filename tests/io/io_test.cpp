// opennova::io unit tests: LE primitives, fixed-point, bounds-checked byte
// cursors, LSB-first bit streams, and the ASCII string helpers.

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <io/bit_stream.h>
#include <io/byte_reader.h>
#include <io/byte_writer.h>
#include <io/bam.h>
#include <io/log.h>
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

    io::ByteReader r3(bytes, sizeof(bytes));
    r3.skip(1);
    TEST_EXPECT(!r3.has_bytes(SIZE_MAX));
    r3.skip(SIZE_MAX);
    TEST_EXPECT(r3.position() == sizeof(bytes));
    TEST_EXPECT(r3.remaining() == 0);
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

// Golden bytes for fields landing at 1-, 2-, and 3-byte offsets inside the
// dword. The writer used to store through a `uint32_t *` aimed at an arbitrary
// byte offset — strict-aliasing and alignment UB that x86 tolerated and UBSan
// flags. These are the exact bytes that store produced, so the byte-wise
// little-endian replacement is pinned as bit-for-bit identical.
static int test_bit_stream_unaligned_golden()
{
    io::BitWriter w;
    w.write_field(8, 0xA1);       // byte 0, aligned
    w.write_field(16, 0xBEEF);    // byte 1  -> 1-byte offset
    w.write_field(16, 0x1234);    // byte 3  -> 3-byte offset (crosses the dword)
    w.write_field(12, 0xABC);     // byte 5  -> 1-byte offset, ends mid-byte
    w.write_field(12, 0xDEF);     // byte 6 bit 4 -> 2-byte offset, unaligned in both axes

    static const uint8_t kGolden[] = {0xA1, 0xEF, 0xBE, 0x34, 0x12, 0xBC, 0xFA, 0xDE};
    TEST_EXPECT(w.high_water() >= sizeof(kGolden));
    TEST_EXPECT(std::memcmp(w.data(), kGolden, sizeof(kGolden)) == 0);

    io::BitReader r(w.data(), w.high_water());
    TEST_EXPECT(r.read_bits(8) == 0xA1);
    TEST_EXPECT(r.read_bits(16) == 0xBEEF);
    TEST_EXPECT(r.read_bits(16) == 0x1234);
    TEST_EXPECT(r.read_bits(12) == 0xABC);
    TEST_EXPECT(r.read_bits(12) == 0xDEF);
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


// --- io/log.h: the one libs/ diagnostic sink (W1-3) -------------------------
static std::vector<std::pair<int, std::string>> g_log_seen;
static void log_test_sink(opennova::io::LogLevel level, const char *msg)
{
    g_log_seen.emplace_back(static_cast<int>(level), msg);
}

static int test_log_sink()
{
    using opennova::io::LogLevel;
    // Silent default: no sink installed, the call is a no-op (and must not crash).
    opennova::io::set_log_sink(nullptr);
    opennova::io::logf(LogLevel::kWarn, "dropped %d", 1);
    // An installed sink receives the formatted message, no trailing newline.
    opennova::io::set_log_sink(&log_test_sink);
    opennova::io::logf(LogLevel::kInfo, "hello %s %d", "world", 7);
    opennova::io::logf(LogLevel::kError, "plain");
    opennova::io::set_log_sink(nullptr);
    opennova::io::logf(LogLevel::kWarn, "after uninstall %d", 2);
    TEST_EXPECT(g_log_seen.size() == 2);
    TEST_EXPECT(g_log_seen[0].first == static_cast<int>(LogLevel::kInfo));
    TEST_EXPECT(g_log_seen[0].second == "hello world 7");
    TEST_EXPECT(g_log_seen[1].first == static_cast<int>(LogLevel::kError));
    TEST_EXPECT(g_log_seen[1].second == "plain");
    return 0;
}


// --- W2-4: the vector-append writers + the ByteReader truncation latch ------
static int test_append_writers()
{
    std::vector<uint8_t> out;
    io::append_u8(out, 0xAB);
    io::append_u16_le(out, 0x1234);
    io::append_u32_le(out, 0xDEADBEEFu);
    io::append_i16_le(out, (int16_t)-2);
    io::append_i32_le(out, (int32_t)-2);

    const uint8_t want[] = {
        0xAB,
        0x34, 0x12,
        0xEF, 0xBE, 0xAD, 0xDE,
        0xFE, 0xFF,
        0xFE, 0xFF, 0xFF, 0xFF,
    };
    TEST_EXPECT(out.size() == sizeof(want));
    TEST_EXPECT(std::memcmp(out.data(), want, sizeof(want)) == 0);

    // The appenders and the pointer writers must agree byte for byte -- the
    // whole point is that a streaming encoder and a fixed-buffer encoder
    // produce the same wire bytes.
    uint8_t via_ptr[4] = {0, 0, 0, 0};
    io::write_u32_le(via_ptr, 0xDEADBEEFu);
    TEST_EXPECT(std::memcmp(via_ptr, want + 3, 4) == 0);

    // Float appends round-trip through the reader.
    std::vector<uint8_t> fbuf;
    io::append_f32_le(fbuf, 0.5f);
    TEST_EXPECT(fbuf.size() == 4);
    io::ByteReader fr(fbuf.data(), fbuf.size());
    TEST_EXPECT(fr.read_f32() == 0.5f);
    return 0;
}

static int test_byte_reader_truncation_latch()
{
    const uint8_t data[3] = {0x01, 0x02, 0x03};

    // A clean read leaves the cursor ok.
    io::ByteReader clean(data, sizeof(data));
    TEST_EXPECT(clean.ok());
    TEST_EXPECT(clean.read_u8() == 0x01);
    TEST_EXPECT(clean.read_u16() == 0x0302);
    TEST_EXPECT(clean.ok());

    // A clipped read latches -- and this is what a legitimate zero could not
    // be told apart from before.
    io::ByteReader trunc(data, sizeof(data));
    TEST_EXPECT(trunc.read_u32() == 0);
    TEST_EXPECT(!trunc.ok());
    // OBSERVATIONAL: the latch must not change the lenient recovery contract
    // format parsers depend on -- the cursor did not advance, so the next
    // read still returns real data.
    TEST_EXPECT(trunc.position() == 0);
    TEST_EXPECT(trunc.read_u8() == 0x01);

    // skip past the end latches and clamps.
    io::ByteReader skipper(data, sizeof(data));
    skipper.skip(99);
    TEST_EXPECT(!skipper.ok());
    TEST_EXPECT(skipper.remaining() == 0);

    // Bulk reads zero-fill AND latch.
    io::ByteReader bulk(data, sizeof(data));
    uint8_t dst[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    bulk.read_bytes(dst, sizeof(dst));
    TEST_EXPECT(!bulk.ok());
    TEST_EXPECT(dst[0] == 0 && dst[7] == 0);

    // A caller-declared semantic failure joins the same state.
    io::ByteReader semantic(data, sizeof(data));
    TEST_EXPECT(semantic.ok());
    semantic.mark_failed();
    TEST_EXPECT(!semantic.ok());
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
    if (test_bit_stream_unaligned_golden()) return 1;
    if (test_strutil()) return 1;
    if (test_append_writers()) return 1;
    if (test_byte_reader_truncation_latch()) return 1;
    if (test_log_sink()) return 1;
    std::printf("io_test: all checks passed\n");
    return 0;
}

