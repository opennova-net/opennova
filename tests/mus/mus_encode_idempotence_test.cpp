/* Write-path parity invariant for canonical MUS re-encoding.

   This pins what is actually achievable when a format tool rewrites a script,
   and documents what is NOT.

   NOT achievable: byte-identity to the ORIGINAL shipped .bin. The shipped
   SCR0/MU01 files carry debug info the runtime decode path does
   not capture and mus_encode_file does not reproduce -- the section-name
   blob, the 256-byte source path, the string section, and aux tables A/B
   (MusChunkHeader fields debug_info_offset / string_section_offset /
   aux_table_*; see mus.h). So encode(compile(decompile(original))) is a
   smaller, canonical form (e.g. jo_gamemus 2521 -> ~1121 bytes), first
   diverging around offset 20 (the chunk-table / debug-info region). A gate
   worded "recompile == original file bytes" would therefore FAIL and
   reject every valid rewrite -- do not word it that way.

   ACHIEVABLE: the re-encoded canonical form is a byte-stable FIXED POINT.
   Define canonical(b) = encode(compile(decompile(open(b)))). Then
   canonical(original) == canonical(canonical(original)) byte-for-byte. This
   is the writer invariant: a no-op decompile/compile/encode cycle must
   reproduce the canonical bytes exactly. The minted synth_{gamemus,menumus}.bin
   (already the canonical form, so canonical(minted) == minted as well) run
   unconditionally; the shipped pair from the reference fixture set
   (OPENNOVA_JO_ASSETS) is the retail leg.

   The decompiler emitter must never change (its golden text round-trip is
   byte-exact); this test exercises the compile+encode side only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/mus/mus.h>

#include <string>

#include "common/retail_paths.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static std::string g_retail_gamemus, g_retail_menumus;

/* canonical(script) = encode_file(compile(decompile(script))). Allocates
   *out via mus_encode_file (release with mus_free). Returns 1 on success. */
static int canonical_from_script(const MusScript *s, uint8_t **out, size_t *out_size) {
    int needed = mus_decompile(s, NULL, 0);
    if (needed <= 0) return 0;
    char *text = (char *)malloc((size_t)needed + 1);
    if (!text) return 0;
    if (mus_decompile(s, text, (size_t)needed + 1) != needed) { free(text); return 0; }
    text[needed] = 0;

    MusScript recomp = {};
    const char *err = NULL;
    int err_line = 0, err_col = 0;
    int rc = mus_compile(text, &recomp, &err_line, &err_col, &err);
    free(text);
    if (rc != 0) {
        fprintf(stderr, "  compile error at line %d col %d: %s\n",
                err_line, err_col, err ? err : "(null)");
        return 0;
    }
    const MusScript *arr[1] = { &recomp };
    rc = mus_encode_file(arr, 1, out, out_size);
    mus_script_free(&recomp);
    return rc == 0;
}

/* The shared assertion: canonical(original) is a byte-stable fixed point. */
static int assert_fixed_point(const char *path) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open original");

    uint8_t *lap1 = NULL; size_t n1 = 0;
    CHECK(canonical_from_script(&mf.scripts[0], &lap1, &n1), "lap1 = canonical(original)");
    CHECK(n1 > 0, "lap1 non-empty");

    /* Re-open the canonical bytes and canonicalize again. */
    MusFile mf2;
    CHECK(mus_open_memory(&mf2, lap1, n1) == 0, "open lap1 bytes");
    uint8_t *lap2 = NULL; size_t n2 = 0;
    CHECK(canonical_from_script(&mf2.scripts[0], &lap2, &n2), "lap2 = canonical(lap1)");

    if (n1 != n2 || memcmp(lap1, lap2, n1) != 0) {
        fprintf(stderr, "  lap1 (%zu bytes) and lap2 (%zu bytes) differ\n", n1, n2);
        size_t lim = n1 < n2 ? n1 : n2;
        for (size_t i = 0; i < lim; ++i) {
            if (lap1[i] != lap2[i]) {
                fprintf(stderr, "  first byte diff at offset %zu: %02x vs %02x\n",
                        i, lap1[i], lap2[i]);
                break;
            }
        }
    }
    CHECK(n1 == n2, "canonical form is size-stable (lap1 == lap2)");
    CHECK(memcmp(lap1, lap2, n1) == 0, "canonical form is a byte-stable fixed point");

    /* Informational: document the shrink vs the original (not a gate). */
    fprintf(stderr, "  [info] %s: canonical %zu bytes (a shipped original is larger; "
            "editor debug-info region is not reproduced)\n", path, n1);

    mus_free(lap1);
    mus_free(lap2);
    mus_close(&mf2);
    mus_close(&mf);
    return 1;
}

/* The minted programs are the encoder's own output, so they are ALREADY the
   canonical form: canonical(minted) reproduces the committed bytes. */
static int assert_minted_is_canonical(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "open minted");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *committed = (uint8_t *)malloc((size_t)n);
    CHECK(committed != NULL && fread(committed, 1, (size_t)n, f) == (size_t)n, "read minted");
    fclose(f);

    MusFile mf;
    CHECK(mus_open_memory(&mf, committed, (size_t)n) == 0, "open minted bytes");
    uint8_t *lap = NULL; size_t lap_n = 0;
    CHECK(canonical_from_script(&mf.scripts[0], &lap, &lap_n), "canonical(minted)");
    CHECK(lap_n == (size_t)n && memcmp(lap, committed, lap_n) == 0,
          "the minted program is its own canonical form");
    mus_free(lap);
    mus_close(&mf);
    free(committed);
    return 1;
}

static int test_synth_gamemus_fixed_point(void) {
    return assert_fixed_point(MUS_FIXTURE_DIR "/synth_gamemus.bin")
        && assert_minted_is_canonical(MUS_FIXTURE_DIR "/synth_gamemus.bin");
}

static int test_synth_menumus_fixed_point(void) {
    return assert_fixed_point(MUS_FIXTURE_DIR "/synth_menumus.bin")
        && assert_minted_is_canonical(MUS_FIXTURE_DIR "/synth_menumus.bin");
}

static int test_gamemus_fixed_point(void) {
    return assert_fixed_point(g_retail_gamemus.c_str());
}

static int test_menumus_fixed_point(void) {
    return assert_fixed_point(g_retail_menumus.c_str());
}

int main(void) {
    RUN_TEST(test_synth_gamemus_fixed_point);
    RUN_TEST(test_synth_menumus_fixed_point);

    g_retail_gamemus = retail::reference_fixture("mus/jo_gamemus.bin");
    g_retail_menumus = retail::reference_fixture("mus/jo_menumus.bin");
    if (g_retail_gamemus.empty() || g_retail_menumus.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mus/jo_{gamemus,menumus}.bin (the shipped programs)");
    } else {
        RUN_TEST(test_gamemus_fixed_point);
        RUN_TEST(test_menumus_fixed_point);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
