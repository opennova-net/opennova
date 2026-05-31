#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

/* Roundtrip: decompile(compile(decompile(x))) must equal decompile(x).

   This is the validation that the compiler is the inverse of the
   decompiler at the text-level: any text the decompiler can produce, the
   compiler must accept, and re-decompiling the recompiled bytecode must
   yield the same text. The bytecode itself is allowed to differ (different
   but equivalent opcode sequences are fine). */
static int test_roundtrip_jo_gamemus(void) {
    MusFile mf;
    int rc = mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin");
    CHECK(rc == 0, "open original");

    /* Decompile the original to text1. */
    int needed = mus_decompile(&mf.scripts[0], NULL, 0);
    CHECK(needed > 0, "decompile size");
    char *text1 = (char *)malloc((size_t)needed + 1);
    CHECK(text1, "alloc text1");
    int written = mus_decompile(&mf.scripts[0], text1, (size_t)needed + 1);
    CHECK(written == needed, "decompile written");
    text1[needed] = 0;

    /* Compile text1 -> recomp. */
    MusScript recomp = {};
    const char *err = NULL;
    int err_line = 0, err_col = 0;
    rc = mus_compile(text1, &recomp, &err_line, &err_col, &err);
    if (rc != 0) {
        fprintf(stderr, "  compile error at line %d col %d: %s\n",
                err_line, err_col, err ? err : "(null)");
    }
    CHECK(rc == 0, err ? err : "compile");

    /* Decompile the recompiled script to text2. */
    int needed2 = mus_decompile(&recomp, NULL, 0);
    CHECK(needed2 > 0, "decompile2 size");
    char *text2 = (char *)malloc((size_t)needed2 + 1);
    int written2 = mus_decompile(&recomp, text2, (size_t)needed2 + 1);
    CHECK(written2 == needed2, "decompile2 written");
    text2[needed2] = 0;

    if (strcmp(text1, text2) != 0) {
        FILE *d1 = fopen("mus_roundtrip_text1.txt", "wb");
        if (d1) { fwrite(text1, 1, (size_t)needed, d1); fclose(d1); }
        FILE *d2 = fopen("mus_roundtrip_text2.txt", "wb");
        if (d2) { fwrite(text2, 1, (size_t)needed2, d2); fclose(d2); }
        fprintf(stderr,
                "  text1 (%d bytes) and text2 (%d bytes) differ\n",
                needed, needed2);
        size_t dlim = (size_t)written < (size_t)written2
                          ? (size_t)written : (size_t)written2;
        for (size_t i = 0; i < dlim; ++i) {
            if (text1[i] != text2[i]) {
                fprintf(stderr, "  first diff at offset %zu\n", i);
                size_t a = i > 40 ? i - 40 : 0;
                fprintf(stderr, "  text1: %.80s\n", text1 + a);
                fprintf(stderr, "  text2: %.80s\n", text2 + a);
                break;
            }
        }
    }
    CHECK(strcmp(text1, text2) == 0, "decompile(compile(decompile(x))) idempotent");

    free(text1);
    free(text2);
    mus_script_free(&recomp);
    mus_close(&mf);
    return 1;
}

int main(void) {
    RUN_TEST(test_roundtrip_jo_gamemus);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
