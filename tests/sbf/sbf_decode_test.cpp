/* SBF decode: the sample/chunk/entry decoders over the synthetic bank
   fixtures/sbf/synth_gamemus.sbf (SILENCE = one partial chunk of 0x08A8
   samples, TONE01 = 4096 + 4096 + 1000 samples over three chunks). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <formats/sbf/sbf.h>

using namespace opennova::sbf;

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef SBF_FIXTURE_DIR
#define SBF_FIXTURE_DIR "fixtures/sbf"
#endif
#define SYNTH_BANK SBF_FIXTURE_DIR "/synth_gamemus.sbf"

static int test_decode_sample_silence(void) {
    /* 0x80 (128) is centred, should decode to 0 at every scale */
    for (uint8_t scale = 0; scale <= 7; ++scale) {
        CHECK(sbf_decode_sample(0x80, scale) == 0, "0x80 -> 0");
    }
    return 1;
}

/* The 8-bit buffer the stereo split makes of a chunk, at the mixer's 16-bit
   scale: ((byte - 128) * 256) >> shift, as an 8-bit AUD1 wave of the same
   bytes decodes (C-162) [orig: Audio_SubmitStereoSampleSplit @ 0x7BD205;
   sub_7BD671 @ 0x7BD6A1]. */
static int test_decode_sample_extremes(void) {
    /* scale 0: byte 0xFF -> 127 * 256 = 32512, the loudest a stream plays */
    CHECK(sbf_decode_sample(0xFF, 0) == 32512, "0xFF scale 0");
    /* scale 0: byte 0x00 -> -128 * 256 = -32768 */
    CHECK(sbf_decode_sample(0x00, 0) == -32768, "0x00 scale 0");
    /* scale 7: byte 0xFF -> (127 * 256) >> 7 = 254 */
    CHECK(sbf_decode_sample(0xFF, 7) == 254, "0xFF scale 7");
    CHECK(sbf_decode_sample(0x00, 7) == -256, "0x00 scale 7");
    /* an 8-bit AUD1 sample's 16-bit value at the same shift */
    CHECK(sbf_decode_sample(0x81, 3) == (1 * 256) >> 3, "0x81 scale 3");
    return 1;
}

/* Any shift byte plays: the mixer shifts the channel's coefficients by it,
   any count, so past 7 a chunk plays quieter and from 15 on silent [orig:
   AudioChannel_ComputeMixCoefficients @ 0x7BD5E6..0x7BD5F3, psraw]. */
static int test_decode_chunk_takes_any_shift(void) {
    uint8_t chunk[SBF_CHUNK_HEADER + 2] = {0};
    int16_t decoded[2] = {0};
    SbfChunkHeader h = {};
    h.valid_samples = 2;
    chunk[SBF_CHUNK_HEADER] = 0x00;
    chunk[SBF_CHUNK_HEADER + 1] = 0xFF;

    h.scale_a = 8;
    h.scale_b = 14;
    memcpy(chunk, &h, SBF_CHUNK_HEADER);
    CHECK(sbf_decode_chunk(chunk, sizeof(chunk), decoded, 2) == 2,
          "a chunk with shifts past 7 decodes");
    CHECK(decoded[0] == -128 && decoded[1] == 1, "shift 8 and 14");

    h.scale_a = 15;
    h.scale_b = 255;
    memcpy(chunk, &h, SBF_CHUNK_HEADER);
    CHECK(sbf_decode_chunk(chunk, sizeof(chunk), decoded, 2) == 2,
          "a chunk with shifts of 15 and more decodes");
    CHECK(decoded[0] == 0 && decoded[1] == 0, "from 15 on silent");
    return 1;
}

static int test_decode_chunk_tone01(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "TONE01");
    CHECK(e != NULL, "TONE01 found");
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

    /* A tone chunk is not silence. */
    int nonzero = 0;
    for (int i = 0; i < n; ++i) if (decoded[i] != 0) ++nonzero;
    CHECK(nonzero > 0, "tone chunk has non-silent samples");

    sbf_close(&arc);
    return 1;
}

static int test_decode_chunk_silence_partial(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    uint8_t chunk[0x1008];
    CHECK(sbf_read_chunk(&arc, e, 0, chunk, sizeof(chunk)) == (int)e->block_size,
          "read chunk");

    int16_t decoded[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, sizeof(chunk), decoded, SBF_CHUNK_AUDIO);
    CHECK(n == 0x08A8, "partial chunk: 2216 int16 samples");
    for (int i = 0; i < n; ++i) CHECK(decoded[i] == 0, "silence decodes to zero");

    sbf_close(&arc);
    return 1;
}

static int test_decode_all_silence(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    uint8_t *raw = (uint8_t *)malloc(e->total_size);
    CHECK(raw != NULL, "raw alloc");
    CHECK(sbf_read_raw(&arc, e, raw, e->total_size) == (int)e->total_size, "read raw");

    int16_t *out = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(out != NULL, "out alloc");
    int n = sbf_decode_all(raw, e->total_size, out, e->total_size);
    CHECK(n == 0x08A8, "SILENCE decodes to 2216 samples (single partial chunk)");

    free(out); free(raw);
    sbf_close(&arc);
    return 1;
}

static int test_decode_all_tone01_multichunk(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "TONE01");
    CHECK(e != NULL, "TONE01 found");
    uint8_t *raw = (uint8_t *)malloc(e->total_size);
    CHECK(raw != NULL, "raw alloc");
    CHECK(sbf_read_raw(&arc, e, raw, e->total_size) == (int)e->total_size, "read raw");

    int16_t *out = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(out != NULL, "out alloc");
    int n = sbf_decode_all(raw, e->total_size, out, e->total_size);
    /* Bounds: all-but-last chunks are full; the last is partial. */
    uint32_t chunks = e->total_size / e->block_size;
    CHECK(chunks == 3, "three chunks");
    int min_samples = (int)((chunks - 1) * SBF_CHUNK_AUDIO + 1);
    int max_samples = (int)(chunks * SBF_CHUNK_AUDIO);
    CHECK(n >= min_samples && n <= max_samples,
          "decoded count is bounded by chunk count");
    CHECK(n == 4096 + 4096 + 1000, "TONE01 decodes to its 9192 authored samples");

    free(out); free(raw);
    sbf_close(&arc);
    return 1;
}

int main(void) {
    RUN_TEST(test_decode_sample_silence);
    RUN_TEST(test_decode_sample_extremes);
    RUN_TEST(test_decode_chunk_takes_any_shift);
    RUN_TEST(test_decode_chunk_tone01);
    RUN_TEST(test_decode_chunk_silence_partial);
    RUN_TEST(test_decode_all_silence);
    RUN_TEST(test_decode_all_tone01_multichunk);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
