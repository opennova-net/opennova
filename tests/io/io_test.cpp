// opennova::io unit tests: LE primitives, fixed-point, bounds-checked byte
// cursors, LSB-first bit streams, the ASCII string helpers, the 64-bit
// FNV-1a hash with its hex spelling, and the checked cp1252 encoder.

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <base/io/bit_stream.h>
#include <base/io/byte_reader.h>
#include <base/io/byte_writer.h>
#include <base/io/bam.h>
#include <base/io/cp1252.h>
#include <base/io/log.h>
#include <base/io/log_ring.h>
#include <base/io/fixed.h>
#include <base/io/hash.h>
#include <base/io/le.h>
#include <base/io/strutil.h>

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

// The protocol handlers' two string/skip forms: a NUL-terminated read that
// clamps to the end, and a skip that does not move on a clipped field.
static int test_byte_reader_cstr_and_skip_if_available()
{
    const uint8_t bytes[] = {'A', 'B', 0, 7, 'Z'};
    io::ByteReader r(bytes, sizeof(bytes));
    TEST_EXPECT(r.read_cstr() == "AB");
    TEST_EXPECT(r.position() == 3 && r.ok());
    TEST_EXPECT(r.read_u8() == 7);
    TEST_EXPECT(r.read_cstr() == "Z"); // no terminator: the rest, clipped
    TEST_EXPECT(r.position() == sizeof(bytes) && !r.ok());
    TEST_EXPECT(r.read_cstr().empty());

    io::ByteReader s(bytes, sizeof(bytes));
    s.skip_if_available(4);
    TEST_EXPECT(s.position() == 4 && s.ok());
    s.skip_if_available(4); // one byte left: no move
    TEST_EXPECT(s.position() == 4 && !s.ok());
    TEST_EXPECT(s.read_u8() == 'Z');
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

// The stream: fields of 3, 7 and 13 bits, a dword align, a 32-bit field, two
// raw bytes, then a 5-bit field, packed LSB-first in a little-endian dword
// stream.
static int test_bit_stream_fields()
{
    static const uint8_t kStream[] = {0x25, 0xFF, 0x3F, 0x00, 0xEF, 0xBE, 0xAD, 0xDE,
                                      0x12, 0x34, 0x15};
    io::BitReader r(kStream, sizeof(kStream));
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
// dword: 8 bits at byte 0, 16 at byte 1, 16 at byte 3 (crossing the dword), 12
// at byte 5 (ending mid-byte) and 12 at byte 6 bit 4 (unaligned in both axes).
static int test_bit_stream_unaligned_golden()
{
    static const uint8_t kGolden[] = {0xA1, 0xEF, 0xBE, 0x34, 0x12, 0xBC, 0xFA, 0xDE, 0x00};
    io::BitReader r(kGolden, sizeof(kGolden));
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

// parse_int / parse_ulong / parse_ullong / parse_float accept exactly what
// std::stoi / stoul / stoull / stof accept and fail exactly where those throw.
static int test_strutil_parse_numbers()
{
    TEST_EXPECT(strutil::parse_int("42") == 42);
    TEST_EXPECT(strutil::parse_int("  -17") == -17);
    TEST_EXPECT(strutil::parse_int("+8") == 8);
    TEST_EXPECT(strutil::parse_int("12abc") == 12);
    TEST_EXPECT(strutil::parse_int("2147483647") == 2147483647);
    TEST_EXPECT(strutil::parse_int("-2147483648") == INT_MIN);
    TEST_EXPECT(!strutil::parse_int(""));
    TEST_EXPECT(!strutil::parse_int("   "));
    TEST_EXPECT(!strutil::parse_int("abc"));
    TEST_EXPECT(!strutil::parse_int("-"));
    TEST_EXPECT(!strutil::parse_int("2147483648"));
    TEST_EXPECT(!strutil::parse_int("-2147483649"));
    TEST_EXPECT(!strutil::parse_int("99999999999999999999"));
    TEST_EXPECT(strutil::parse_int("0x10") == 0);

    TEST_EXPECT(strutil::parse_ulong("ff8000", 16) == 0xff8000ul);
    TEST_EXPECT(strutil::parse_ulong("FFffFFzz", 16) == 0xfffffful);
    TEST_EXPECT(!strutil::parse_ulong("zz", 16));
    TEST_EXPECT(!strutil::parse_ulong("", 16));
    TEST_EXPECT(!strutil::parse_ulong("fffffffffffffffffffff", 16));

    TEST_EXPECT(strutil::parse_ullong("18446744073709551615") == 18446744073709551615ull);
    TEST_EXPECT(strutil::parse_ullong(" 60000ms") == 60000ull);
    TEST_EXPECT(!strutil::parse_ullong("18446744073709551616"));
    TEST_EXPECT(!strutil::parse_ullong("ms"));
    TEST_EXPECT(!strutil::parse_ullong(""));

    TEST_EXPECT(strutil::parse_float("1.5") == 1.5f);
    TEST_EXPECT(strutil::parse_float(" -0.25x") == -0.25f);
    TEST_EXPECT(!strutil::parse_float("x1.5"));
    TEST_EXPECT(!strutil::parse_float(""));
    TEST_EXPECT(!strutil::parse_float("1e999"));
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


// --- io/log.h: the one engine/ diagnostic sink (W1-3) -------------------------
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

static int test_log_ring_cursor_drain_and_wrap()
{
    using opennova::io::LogLevel;
    using opennova::io::LogRing;
    using opennova::io::LogRingEntry;

    // Level labels are the one transport mapping.
    TEST_EXPECT(std::string(opennova::io::log_level_name(LogLevel::kDebug)) == "debug");
    TEST_EXPECT(std::string(opennova::io::log_level_name(LogLevel::kWarn)) == "warn");

    LogRing ring;
    TEST_EXPECT(ring.last_sequence() == 0);
    TEST_EXPECT(ring.entries_after(0).empty());

    ring.record(LogLevel::kInfo, "one");
    ring.record(LogLevel::kWarn, "two");
    ring.record(LogLevel::kError, "three");
    TEST_EXPECT(ring.last_sequence() == 3);

    // Cursor drain: strictly after, oldest first, sequences monotonic from 1.
    std::vector<LogRingEntry> all = ring.entries_after(0);
    TEST_EXPECT(all.size() == 3);
    TEST_EXPECT(all[0].sequence == 1 && all[0].text == "one");
    TEST_EXPECT(all[1].level == LogLevel::kWarn);
    TEST_EXPECT(all[2].sequence == 3 && all[2].text == "three");
    std::vector<LogRingEntry> tail = ring.entries_after(2);
    TEST_EXPECT(tail.size() == 1 && tail[0].text == "three");
    TEST_EXPECT(ring.entries_after(3).empty());

    // max_entries pages from the oldest unread entry.
    std::vector<LogRingEntry> page = ring.entries_after(0, 2);
    TEST_EXPECT(page.size() == 2 && page[1].sequence == 2);

    // Wrap: capacity + 3 records keep the newest kCapacity, sequences intact,
    // and a stale cursor surfaces the gap as a first sequence > cursor + 1.
    ring.reset();
    for (size_t i = 0; i < LogRing::kCapacity + 3; ++i)
        ring.record(LogLevel::kDebug, ("entry " + std::to_string(i + 1)).c_str());
    std::vector<LogRingEntry> wrapped = ring.entries_after(0);
    TEST_EXPECT(wrapped.size() == LogRing::kCapacity);
    TEST_EXPECT(wrapped.front().sequence == 4);
    TEST_EXPECT(wrapped.front().text == "entry 4");
    TEST_EXPECT(wrapped.back().sequence == LogRing::kCapacity + 3);
    TEST_EXPECT(ring.entries_after(LogRing::kCapacity + 2).size() == 1);
    return 0;
}

static int test_log_ring_concurrent_record_and_drain()
{
    using opennova::io::LogLevel;
    using opennova::io::LogRing;
    using opennova::io::LogRingEntry;

    // The header's contract: the mutex keeps record/drain safe if an embedder
    // ever logs off the main thread. One producer records kCapacity * 4
    // entries while the main thread drains by cursor: every drained batch is
    // strictly increasing, no batch spans more than the ring's capacity, a
    // gap only ever means the ring wrapped past unread entries (never a
    // duplicate or a reordering), and the final sequence is the total.
    static constexpr size_t kTotal = LogRing::kCapacity * 4;
    LogRing ring;
    std::atomic<bool> done{false};
    std::thread producer([&ring, &done]() {
        for (size_t i = 0; i < kTotal; ++i)
            ring.record(LogLevel::kInfo, ("worker " + std::to_string(i + 1)).c_str());
        done.store(true);
    });
    uint64_t cursor = 0;
    uint64_t last_seen = 0;
    size_t drained = 0;
    bool monotonic = true;
    bool bounded = true;
    while (true) {
        const bool finished = done.load();
        const std::vector<LogRingEntry> batch = ring.entries_after(cursor);
        if (!batch.empty()) {
            if (batch.size() > LogRing::kCapacity) bounded = false;
            for (const LogRingEntry &entry : batch) {
                if (entry.sequence <= last_seen) monotonic = false;
                if (entry.text != "worker " + std::to_string(entry.sequence)) monotonic = false;
                last_seen = entry.sequence;
            }
            drained += batch.size();
            cursor = batch.back().sequence;
        }
        if (finished && batch.empty()) break;
        std::this_thread::yield();
    }
    producer.join();
    TEST_EXPECT(monotonic);
    TEST_EXPECT(bounded);
    TEST_EXPECT(drained > 0 && drained <= kTotal);
    TEST_EXPECT(last_seen == kTotal);
    TEST_EXPECT(ring.last_sequence() == kTotal);
    // Everything after the drain's cursor is gone: the ring holds exactly the
    // newest kCapacity entries, already seen.
    TEST_EXPECT(ring.entries_after(cursor).empty());
    TEST_EXPECT(ring.entries_after(0).size() == LogRing::kCapacity);
    TEST_EXPECT(ring.entries_after(0).front().sequence == kTotal - LogRing::kCapacity + 1);
    return 0;
}

static int test_log_ring_install_chains_downstream()
{
    using opennova::io::LogLevel;
    using opennova::io::LogRing;
    using opennova::io::LogRingEntry;

    // A pre-installed sink (the GDExtension's push_warning forwarder in the
    // real embedder) must keep receiving every message after install().
    g_log_seen.clear();
    opennova::io::set_log_sink(&log_test_sink);
    LogRing::instance().reset();
    LogRing::install();
    LogRing::install(); // idempotent: no self-chaining loop

    opennova::io::logf(LogLevel::kWarn, "chained %d", 42);
    std::vector<LogRingEntry> drained = LogRing::instance().entries_after(0);
    TEST_EXPECT(drained.size() == 1);
    TEST_EXPECT(drained[0].sequence == 1);
    TEST_EXPECT(drained[0].level == LogLevel::kWarn);
    TEST_EXPECT(drained[0].text == "chained 42");
    TEST_EXPECT(g_log_seen.size() == 1);
    TEST_EXPECT(g_log_seen[0].second == "chained 42");

    opennova::io::set_log_sink(nullptr);
    LogRing::instance().reset();
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

// The FNV reference vectors: the offset basis is the hash of no bytes.
static int test_fnv1a64()
{
    TEST_EXPECT(io::kFnv1a64Offset == UINT64_C(0xcbf29ce484222325));
    TEST_EXPECT(io::fnv1a64_bytes(io::kFnv1a64Offset, "", 0) == UINT64_C(0xcbf29ce484222325));
    TEST_EXPECT(io::fnv1a64_bytes(io::kFnv1a64Offset, "a", 1) == UINT64_C(0xaf63dc4c8601ec8c));
    TEST_EXPECT(io::fnv1a64_bytes(io::kFnv1a64Offset, "foobar", 6) == UINT64_C(0x85944171f73967e8));
    TEST_EXPECT(io::hex64(UINT64_C(0x85944171f73967e8)) == "85944171f73967e8");
    uint64_t parsed = 0;
    TEST_EXPECT(io::parse_hex64("85944171F73967E8", parsed) && parsed == UINT64_C(0x85944171f73967e8));
    return 0;
}

// The checked cp1252 encoder: every character with a byte encodes (the 0x80..0x9F
// specials and a raw undefined C1 position included); one without leaves `out` as it was
// and is named, each once, in the order met; a text that is not UTF-8 names U+FFFD.
static int test_cp1252_checked()
{
    std::string out = "kept";
    TEST_EXPECT(utf8_to_cp1252("Caf\xC3\xA9 \xE2\x82\xAC \xC2\x81", out) && out == "Caf\xE9 \x80 \x81");
    std::u32string unstorable;
    out = "kept";
    TEST_EXPECT(!utf8_to_cp1252("\xE2\x9C\x93 a \xE4\xB8\xAD \xE2\x9C\x93", out, &unstorable));
    TEST_EXPECT(out == "kept" && unstorable == std::u32string({0x2713, 0x4E2D}));
    TEST_EXPECT(!utf8_to_cp1252("\xE2\x9C\x93", out));
    unstorable.clear();
    TEST_EXPECT(!utf8_to_cp1252("bad \xC3", out, &unstorable) && unstorable == std::u32string({0xFFFD}));
    TEST_EXPECT(cp1252_to_utf8("Caf\xE9") == "Caf\xC3\xA9");
    return 0;
}

int main()
{
    if (test_le_primitives()) return 1;
    if (test_cp1252_checked()) return 1;
    if (test_fnv1a64()) return 1;
    if (test_bam_wrap_arithmetic()) return 1;
    if (test_fixed_point()) return 1;
    if (test_byte_reader_bounds()) return 1;
    if (test_byte_writer_roundtrip()) return 1;
    if (test_bit_stream_fields()) return 1;
    if (test_bit_stream_unaligned_golden()) return 1;
    if (test_strutil()) return 1;
    if (test_strutil_parse_numbers()) return 1;
    if (test_append_writers()) return 1;
    if (test_byte_reader_truncation_latch()) return 1;
    if (test_byte_reader_cstr_and_skip_if_available()) return 1;
    if (test_log_sink()) return 1;
    if (test_log_ring_cursor_drain_and_wrap()) return 1;
    if (test_log_ring_concurrent_record_and_drain()) return 1;
    if (test_log_ring_install_chains_downstream()) return 1;
    std::printf("io_test: all checks passed\n");
    return 0;
}

