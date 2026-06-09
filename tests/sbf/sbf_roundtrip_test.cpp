#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sbf/sbf.h"

#ifndef SBF_FIXTURE_DIR
#define SBF_FIXTURE_DIR "fixtures/sbf"
#endif

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

/* End-to-end fixture roundtrip: decode the BHD MENU101 entry, re-encode it,
   reparse the encoded image, decode it again, and compare. The bound is one
   int8 quantum (256 raw int16), the worst-case error per sample at the
   loosest scale sbf_pick_scale can choose. */
static int test_roundtrip_bhd_menu101(void) {
    SbfArchive arc;
    CHECK(sbf_open(&arc, SBF_FIXTURE_DIR "/bhd_menumus.sbf") == 0, "open fixture");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "MENU101");
    CHECK(e != NULL, "MENU101 found");

    uint8_t *raw = (uint8_t *)malloc(e->total_size);
    CHECK(raw != NULL, "raw alloc");
    CHECK(sbf_read_raw(&arc, e, raw, e->total_size) == (int)e->total_size,
          "read raw");

    int16_t *dec = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(dec != NULL, "dec alloc");
    int n_dec = sbf_decode_all(raw, e->total_size, dec, e->total_size);
    CHECK(n_dec > 0, "first decode produces samples");

    const char *names[1] = { "MENU101" };
    const int16_t *pcm[1] = { dec };
    const size_t counts[1] = { (size_t)n_dec };
    uint8_t *enc = NULL; size_t enc_size = 0;
    CHECK(sbf_encode_file(names, 1, pcm, counts, &enc, &enc_size) == 0,
          "re-encode");

    /* sbf_open_memory leaves _file NULL, so sbf_read_raw won't work; walk
       the encoded buffer's audio range directly via sbf_decode_all. */
    SbfArchive arc2;
    CHECK(sbf_open_memory(&arc2, enc, enc_size) == 0, "reparse encoded");
    const SbfRawEntry *e2 = &arc2.entries[0];
    int16_t *dec2 = (int16_t *)malloc(e2->total_size * sizeof(int16_t));
    CHECK(dec2 != NULL, "dec2 alloc");
    int n_dec2 = sbf_decode_all(enc + e2->data_offset, e2->total_size,
                                 dec2, e2->total_size);
    CHECK(n_dec == n_dec2, "sample counts equal across roundtrip");

    double sse = 0.0;
    for (int i = 0; i < n_dec; ++i) {
        double d = (double)dec[i] - (double)dec2[i];
        sse += d * d;
    }
    double rmse = sqrt(sse / (double)n_dec);
    printf("  RMSE = %.2f (threshold 256)\n", rmse);
    CHECK(rmse < 256.0, "RMSE within one int8 quantum");

    sbf_free(enc);
    free(dec); free(dec2); free(raw);
    sbf_close(&arc); sbf_close(&arc2);
    return 1;
}

int main(void) {
    RUN_TEST(test_roundtrip_bhd_menu101);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
