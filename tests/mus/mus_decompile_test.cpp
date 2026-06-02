#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static char *slurp_text(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = 0;
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int test_decompile_jo_gamemus(void) {
    MusFile mf;
    int rc = mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin");
    CHECK(rc == 0, "open");

    /* Two-pass: query then write. */
    int needed = mus_decompile(&mf.scripts[0], NULL, 0);
    CHECK(needed > 0, "needed size");
    char *out = (char *)malloc((size_t)needed + 1);
    CHECK(out, "alloc out buf");
    int written = mus_decompile(&mf.scripts[0], out, (size_t)needed + 1);
    CHECK(written > 0, "wrote");
    out[written] = 0;

    size_t gold_size = 0;
    char *gold = slurp_text(MUS_FIXTURE_DIR "/golden_jo_gamemus.mus.txt", &gold_size);
    CHECK(gold, "golden present");

    if (strcmp(out, gold) != 0) {
        /* Dump for diagnostics: write our output and a side-by-side preview. */
        FILE *dump = fopen("mus_decompile_actual.txt", "wb");
        if (dump) { fwrite(out, 1, (size_t)written, dump); fclose(dump); }
        fprintf(stderr, "  decompile mismatch; wrote actual to mus_decompile_actual.txt\n");
        fprintf(stderr, "  golden bytes: %zu, actual bytes: %d\n", gold_size, written);
        /* Find first diff offset for quick triage. */
        size_t dlim = (size_t)written < gold_size ? (size_t)written : gold_size;
        for (size_t i = 0; i < dlim; ++i) {
            if (out[i] != gold[i]) {
                fprintf(stderr, "  first diff at offset %zu (line ~%d)\n", i, 1 + (int)i / 40);
                fprintf(stderr, "  golden: %.40s\n", gold + (i > 20 ? i - 20 : 0));
                fprintf(stderr, "  actual: %.40s\n", out  + (i > 20 ? i - 20 : 0));
                break;
            }
        }
    }
    CHECK(strcmp(out, gold) == 0, "decompile matches golden");

    free(out);
    free(gold);
    mus_close(&mf);
    return 1;
}

static int test_decompile_two_pass_size(void) {
    MusFile mf;
    int rc = mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin");
    CHECK(rc == 0, "open");
    int needed = mus_decompile(&mf.scripts[0], NULL, 0);
    CHECK(needed > 0, "needed size positive");
    char *buf = (char *)malloc((size_t)needed + 1);
    int written = mus_decompile(&mf.scripts[0], buf, (size_t)needed + 1);
    CHECK(written == needed, "second-pass written equals needed");
    free(buf);
    mus_close(&mf);
    return 1;
}

int main(void) {
    RUN_TEST(test_decompile_jo_gamemus);
    RUN_TEST(test_decompile_two_pass_size);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
