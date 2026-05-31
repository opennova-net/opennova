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

int main(void) {
    RUN_TEST(test_compile_minimal_script);
    RUN_TEST(test_encode_file_minimal);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
