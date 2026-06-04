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

/* D1 / D2: compiler skeleton + minimal grammar.

   The Phase A revisions to the spec dropped runtime binds: `bind sound_N`
   declarations are an aesthetic emitted by the decompiler so the source text
   reads naturally. The `play sound_N` opcode operand is a 1-byte SBF index,
   not a bind-table key. The compiler accepts (and ignores the encoded
   payload of) bind declarations purely so the round-trip text shape works. */
static int test_compile_minimal_script(void) {
    const char *src =
        "script test\n"
        "section Begin\n"
        "{\n"
        "  play sound_0\n"
        "  done\n"
        "}\n";
    MusScript out = {};
    int err_line = 0, err_col = 0;
    const char *err_msg = NULL;
    int rc = mus_compile(src, &out, &err_line, &err_col, &err_msg);
    CHECK(rc == 0, err_msg ? err_msg : "compile");
    CHECK(strncmp(out.name, "test", 4) == 0, "script name");
    CHECK(out.section_count == 1, "one section");
    CHECK(out.code_size > 0, "non-empty code");
    /* Section table offsets are bytecode-relative (matches mus_parse.cpp). */
    CHECK(out.sections != NULL, "sections allocated");
    mus_script_free(&out);
    return 1;
}

/* mus_encode_file: a compiled MusScript serialises to a valid SCR0 blob
   that mus_open_memory accepts and re-parses with the original name and
   bytecode preserved. The encoder is exercised end-to-end here; the
   roundtrip test (`mus_roundtrip_test.cpp`) covers the in-memory path. */
static int test_encode_file_minimal(void) {
    const char *src =
        "script test\n"
        "section Begin\n"
        "{\n"
        "  play sound_0\n"
        "  done\n"
        "}\n";
    MusScript script = {};
    int err_line = 0, err_col = 0;
    const char *err = NULL;
    int rc = mus_compile(src, &script, &err_line, &err_col, &err);
    CHECK(rc == 0, err ? err : "compile");

    const MusScript *scripts[1] = { &script };
    uint8_t *buf = NULL;
    size_t   bufsize = 0;
    rc = mus_encode_file(scripts, 1, &buf, &bufsize);
    CHECK(rc == 0, "encode");
    CHECK(buf != NULL, "buf written");
    CHECK(bufsize >= 44 + 4 + 72, "buf big enough for header + chunk");

    /* Re-parse the encoded buffer */
    MusFile mf;
    rc = mus_open_memory(&mf, buf, bufsize);
    CHECK(rc == 0, "re-parse");
    CHECK(mf.header.magic == MUS_MAGIC_SCR0, "magic");
    CHECK(mf.header.chunk_count == 1, "1 chunk");
    CHECK(mf.scripts != NULL, "scripts present");
    CHECK(strncmp(mf.scripts[0].name, "test", 4) == 0, "name preserved");
    CHECK(mf.scripts[0].section_count == 1, "section_count preserved");
    CHECK(mf.scripts[0].code_size == script.code_size, "code_size preserved");
    CHECK(mf.scripts[0].code != NULL && script.code != NULL, "code present");
    CHECK(memcmp(mf.scripts[0].code, script.code, script.code_size) == 0,
          "bytecode preserved");
    /* Intrinsic names must round-trip too. */
    CHECK(mf.intrinsic_count == MUS_INTRINSIC_NAMES, "intrinsic count");
    CHECK(strncmp(mf.intrinsic_names[0], "GEcho", 5) == 0, "intrinsic[0] GEcho");
    CHECK(strncmp(mf.intrinsic_names[10], "TStop", 5) == 0, "intrinsic[10] TStop");
    mus_close(&mf);
    mus_free(buf);
    mus_script_free(&script);
    return 1;
}

/* Operand-bounds guards (editor write-path safety). The compiler emits 1-byte
   operands for play targets, switch (tablexec) section/sound indices, and the
   tablexec count + skip_size. An out-of-range value used to silently wrap to a
   different-but-valid byte, so a visual edit could compile yet encode the wrong
   program. These now error, so the editor's compile gate rolls the edit back. */
static int compiles(const char *src) {
    MusScript out = {};
    int el = 0, ec = 0;
    const char *em = NULL;
    int rc = mus_compile(src, &out, &el, &ec, &em);
    mus_script_free(&out);   /* idempotent; safe on the partial/failed result */
    return rc == 0;
}

static int test_reject_play_track_over_255(void) {
    CHECK(!compiles(
        "script t\nsection Begin\n{\n  play sound_256\n  done\n}\n"),
        "play sound_256 must be rejected, not wrapped to sound_0");
    return 1;
}

static int test_accept_play_track_255(void) {
    /* 255 is the boundary: still a valid single-byte operand. */
    CHECK(compiles(
        "script t\nsection Begin\n{\n  play sound_255\n  done\n}\n"),
        "play sound_255 is in range and must compile");
    return 1;
}

static int test_reject_switch_over_64_targets(void) {
    char src[4096];
    int n = snprintf(src, sizeof(src), "script t\nsection Begin\n{\n  on (Var00) enter");
    for (int i = 0; i < 65; ++i)
        n += snprintf(src + n, sizeof(src) - (size_t)n, " Begin");
    snprintf(src + n, sizeof(src) - (size_t)n, "\n  done\n}\n");
    CHECK(!compiles(src), "65-target on(...) table must be rejected, not truncated");
    return 1;
}

static int test_reject_goto_table_too_large(void) {
    /* goto entries are 5 bytes each, so 51 targets make total_size = 5 + 51*5 =
       260 > 255 -- the skip_size byte would wrap. Count (51) is under 64, so this
       specifically exercises the total_size guard, not the target-count guard. */
    char src[4096];
    int n = snprintf(src, sizeof(src), "script t\nsection Begin\n{\n  on (Var00) goto");
    for (int i = 0; i < 51; ++i)
        n += snprintf(src + n, sizeof(src) - (size_t)n, " Begin");
    snprintf(src + n, sizeof(src) - (size_t)n, "\n  done\n}\n");
    CHECK(!compiles(src), "oversized goto table must be rejected, not wrapped");
    return 1;
}

int main(void) {
    RUN_TEST(test_compile_minimal_script);
    RUN_TEST(test_encode_file_minimal);
    RUN_TEST(test_reject_play_track_over_255);
    RUN_TEST(test_accept_play_track_255);
    RUN_TEST(test_reject_switch_over_64_targets);
    RUN_TEST(test_reject_goto_table_too_large);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
