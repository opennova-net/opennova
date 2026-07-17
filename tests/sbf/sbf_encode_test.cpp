#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sbf/sbf.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

/* Encode-after-decode round-trips within the integer-divide rounding tolerance.
   Validates encode_sample is the algebraic inverse of decode_sample at every
   (byte, scale) pair the decoder will encounter on disk. */
static int test_encode_inverts_decode(void) {
    for (uint8_t scale = 0; scale <= 7; ++scale) {
        for (int byte = 0; byte < 256; ++byte) {
            int16_t decoded   = sbf_decode_sample((uint8_t)byte, scale);
            uint8_t reencoded = sbf_encode_sample(decoded, scale);
            int diff = (int)reencoded - byte;
            if (diff < 0) diff = -diff;
            if (diff > 1) {
                fprintf(stderr,
                        "  scale=%u byte=0x%02X dec=%d re=0x%02X diff=%d\n",
                        scale, byte, (int)decoded, reencoded, diff);
                return 0;
            }
        }
    }
    return 1;
}

/* Public sample helpers accept caller-provided scales. Out-of-range values
   clamp to the narrowest supported scale instead of entering undefined
   signed-shift behavior. */
static int test_sample_helpers_bound_invalid_scales(void) {
    CHECK(sbf_decode_sample(0x00, UINT8_MAX) == -128,
          "decode clamps invalid scale to 7");
    CHECK(sbf_decode_sample(0xFF, UINT8_MAX) == 127,
          "decode high rail stays bounded");
    CHECK(sbf_encode_sample(-128, UINT8_MAX) == 0,
          "encode clamps invalid scale to 7");
    CHECK(sbf_encode_sample(127, UINT8_MAX) == 255,
          "encode high rail stays bounded");
    return 1;
}

/* Silence has zero amplitude; every scale fits, so pick_scale returns the
   largest (7), giving maximum quiet-sample precision. */
static int test_pick_scale_silence(void) {
    int16_t silence[16] = {0};
    CHECK(sbf_pick_scale(silence, 16) == 7, "silence -> scale 7 (max precision)");
    return 1;
}

/* Max-amplitude input clips at every scale; the impl returns 0 (smallest =
   most headroom) so the encoded bytes saturate at the rails rather than
   wrapping. */
static int test_pick_scale_loud(void) {
    int16_t loud[2] = {32767, -32768};
    CHECK(sbf_pick_scale(loud, 2) == 0, "max amplitude -> scale 0 (most headroom)");
    return 1;
}

/* Bigger amplitude needs a smaller (or equal) scale: shrinking the headroom
   never lets a louder chunk fit. */
static int test_pick_scale_monotonic(void) {
    int16_t small[2] = {100, -100};
    int16_t big[2]   = {10000, -10000};
    CHECK(sbf_pick_scale(small, 2) >= sbf_pick_scale(big, 2),
          "scale shrinks as amplitude grows");
    return 1;
}

/* End-to-end chunk round-trip: encode a 1 kHz sine wave then decode it back.
   Each int8 quantum spans 256 raw int16 at the loosest scale, so a per-sample
   tolerance of 256 covers every case sbf_pick_scale could choose here. */
static int test_encode_chunk_roundtrip(void) {
    int16_t pcm[4096];
    for (int i = 0; i < 4096; ++i) {
        double t = (double)i / 22050.0;
        pcm[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979 * 1000.0 * t));
    }

    uint8_t chunk[SBF_CHUNK_TOTAL];
    int n = sbf_encode_chunk(pcm, 4096, chunk, sizeof(chunk));
    CHECK(n == SBF_CHUNK_TOTAL, "chunk fully written");

    int16_t back[4096];
    int got = sbf_decode_chunk(chunk, n, back, 4096);
    CHECK(got == 4096, "decoded back");

    for (int i = 0; i < 4096; ++i) {
        int diff = pcm[i] - back[i];
        if (diff < 0) diff = -diff;
        CHECK(diff <= 256, "roundtrip within one quantum");
    }
    return 1;
}

/* Smallest possible bank: one entry, one chunk of silence. Validates the
   header / index / data layout is reparseable. */
static int test_encode_file_minimal(void) {
    int16_t silence[4096] = {0};
    const int16_t *pcm[1] = { silence };
    const size_t   counts[1] = { 4096 };
    const char    *names[1] = { "TEST001" };

    uint8_t *buf = NULL; size_t bufsize = 0;
    int rc = sbf_encode_file(names, 1, pcm, counts, &buf, &bufsize);
    CHECK(rc == 0, "encode succeeds");
    CHECK(buf != NULL && bufsize > 0, "buffer allocated");

    SbfArchive arc;
    CHECK(sbf_open_memory(&arc, buf, bufsize) == 0, "re-parse");
    CHECK(arc.header.entry_count == 1, "1 entry");
    CHECK(arc.header.flags == 1, "header flags = byte-paired stereo");
    CHECK(strncmp(arc.entries[0].name, "TEST001", 7) == 0, "name");
    CHECK(arc.entries[0].block_size == SBF_CHUNK_TOTAL, "block size");
    CHECK(arc.entries[0].sample_length_hint == 0,
          "sample_length_hint is an engine scheduler field, not caller sample count");
    sbf_close(&arc);
    sbf_free(buf);
    return 1;
}

/* A bank authored from scratch (NovaSbfBank::create_empty) and saved before any
   track is added encodes as a valid 24-byte header-only file: zero entries, no
   per-entry input arrays required. Re-parses to an empty-but-valid archive. */
static int test_encode_file_zero_entries(void) {
    uint8_t *buf = NULL; size_t bufsize = 0;
    int rc = sbf_encode_file(NULL, 0, NULL, NULL, &buf, &bufsize);
    CHECK(rc == 0, "zero-entry encode succeeds without input arrays");
    CHECK(buf != NULL, "buffer allocated");
    CHECK(bufsize == SBF_HEADER_SIZE, "header-only file is exactly 24 bytes");

    SbfArchive arc;
    CHECK(sbf_open_memory(&arc, buf, bufsize) == 0, "re-parse empty bank");
    CHECK(arc.header.entry_count == 0, "0 entries");
    CHECK(arc.header.magic == SBF_MAGIC, "magic stamped");
    CHECK(arc.header.flags == 1, "header flags = byte-paired stereo");
    sbf_close(&arc);
    sbf_free(buf);
    return 1;
}

int main(void) {
    RUN_TEST(test_encode_inverts_decode);
    RUN_TEST(test_sample_helpers_bound_invalid_scales);
    RUN_TEST(test_pick_scale_silence);
    RUN_TEST(test_pick_scale_loud);
    RUN_TEST(test_pick_scale_monotonic);
    RUN_TEST(test_encode_chunk_roundtrip);
    RUN_TEST(test_encode_file_minimal);
    RUN_TEST(test_encode_file_zero_entries);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
