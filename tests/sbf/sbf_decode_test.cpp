#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sbf/sbf.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef SBF_FIXTURE_DIR
#define SBF_FIXTURE_DIR "fixtures/sbf"
#endif

static int test_decode_sample_silence(void) {
    /* 0x80 (128) is centred, should decode to 0 at every scale */
    for (uint8_t scale = 0; scale <= 7; ++scale) {
        CHECK(sbf_decode_sample(0x80, scale) == 0, "0x80 -> 0");
    }
    return 1;
}

static int test_decode_sample_extremes(void) {
    /* scale 0: byte 0xFF -> ((127) * 256) >> 0 >> 1 = 16256 */
    CHECK(sbf_decode_sample(0xFF, 0) == 16256, "0xFF scale 0");
    /* scale 0: byte 0x00 -> ((-128) * 256) >> 0 >> 1 = -16384 */
    CHECK(sbf_decode_sample(0x00, 0) == -16384, "0x00 scale 0");
    /* scale 7: byte 0xFF -> ((127) * 256) >> 7 >> 1 = 127 */
    CHECK(sbf_decode_sample(0xFF, 7) == 127, "0xFF scale 7");
    return 1;
}

static int test_decode_chunk_rejects_invalid_scales(void) {
    uint8_t chunk[SBF_CHUNK_HEADER + 2] = {0};
    int16_t decoded[2] = {0};
    SbfChunkHeader h = {};
    h.valid_samples = 2;

    h.scale_a = 8;
    memcpy(chunk, &h, SBF_CHUNK_HEADER);
    CHECK(sbf_decode_chunk(chunk, sizeof(chunk), decoded, 2) < 0,
          "scale_a above 7 is rejected");

    h.scale_a = 0;
    h.scale_b = 255;
    memcpy(chunk, &h, SBF_CHUNK_HEADER);
    CHECK(sbf_decode_chunk(chunk, sizeof(chunk), decoded, 2) < 0,
          "scale_b above 7 is rejected");
    return 1;
}

/* TODO golden WAV compare once apps/sbf_cli (deferred) lands.
   Phase B fallback: sanity-decode + first-byte hand check verifies the
   decoder produces reasonable output without an external reference. */
static int test_decode_chunk_bhd_menu101(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/bhd_menumus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "MENU101");
    CHECK(e != NULL, "MENU101 found");
    uint8_t chunk[0x1008];
    CHECK(sbf_read_chunk(&arc, e, 0, chunk, sizeof(chunk)) == (int)e->block_size,
          "read chunk");

    int16_t decoded[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, sizeof(chunk), decoded, SBF_CHUNK_AUDIO);
    CHECK(n == 0x1000, "full chunk decoded to 4096 int16 samples");

    /* Hand-verify the first audio byte using sbf_decode_sample with scale_a. */
    SbfChunkHeader h;
    memcpy(&h, chunk, SBF_CHUNK_HEADER);
    int16_t expected_first = sbf_decode_sample(chunk[SBF_CHUNK_HEADER], h.scale_a);
    CHECK(decoded[0] == expected_first, "first sample matches hand decode");

    /* At least some samples must be non-silent (music chunk, not all 0x80). */
    int nonzero = 0;
    for (int i = 0; i < n; ++i) if (decoded[i] != 0) ++nonzero;
    CHECK(nonzero > 0, "music chunk has non-silent samples");

    sbf_close(&arc);
    return 1;
}

static int test_decode_chunk_jo_nulls_partial(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/jo_gamemus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "NULLS");
    CHECK(e != NULL, "NULLS found");
    uint8_t chunk[0x1008];
    CHECK(sbf_read_chunk(&arc, e, 0, chunk, sizeof(chunk)) == (int)e->block_size,
          "read chunk");

    int16_t decoded[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, sizeof(chunk), decoded, SBF_CHUNK_AUDIO);
    CHECK(n == 0x08A8, "partial chunk: 2216 int16 samples");

    sbf_close(&arc);
    return 1;
}

static int test_decode_all_jo_nulls(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/jo_gamemus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "NULLS");
    CHECK(e != NULL, "NULLS found");
    uint8_t *raw = (uint8_t *)malloc(e->total_size);
    CHECK(raw != NULL, "raw alloc");
    CHECK(sbf_read_raw(&arc, e, raw, e->total_size) == (int)e->total_size, "read raw");

    int16_t *out = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(out != NULL, "out alloc");
    int n = sbf_decode_all(raw, e->total_size, out, e->total_size);
    CHECK(n == 0x08A8, "JO NULLS decodes to 2216 samples (single partial chunk)");

    free(out); free(raw);
    sbf_close(&arc);
    return 1;
}

static int test_decode_all_bhd_menu101_multichunk(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/bhd_menumus.sbf") == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "MENU101");
    CHECK(e != NULL, "MENU101 found");
    uint8_t *raw = (uint8_t *)malloc(e->total_size);
    CHECK(raw != NULL, "raw alloc");
    CHECK(sbf_read_raw(&arc, e, raw, e->total_size) == (int)e->total_size, "read raw");

    int16_t *out = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(out != NULL, "out alloc");
    int n = sbf_decode_all(raw, e->total_size, out, e->total_size);
    /* Bounds: at least all-but-last chunks are full; last chunk may be partial. */
    uint32_t chunks = e->total_size / e->block_size;
    int min_samples = (int)((chunks - 1) * SBF_CHUNK_AUDIO + 1);
    int max_samples = (int)(chunks * SBF_CHUNK_AUDIO);
    CHECK(n >= min_samples && n <= max_samples,
          "decoded count is bounded by chunk count");

    free(out); free(raw);
    sbf_close(&arc);
    return 1;
}

int main(void) {
    RUN_TEST(test_decode_sample_silence);
    RUN_TEST(test_decode_sample_extremes);
    RUN_TEST(test_decode_chunk_rejects_invalid_scales);
    RUN_TEST(test_decode_chunk_bhd_menu101);
    RUN_TEST(test_decode_chunk_jo_nulls_partial);
    RUN_TEST(test_decode_all_jo_nulls);
    RUN_TEST(test_decode_all_bhd_menu101_multichunk);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
