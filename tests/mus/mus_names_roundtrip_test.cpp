/* Names-aware decompile must recompile to the SAME bytecode as the names-less
   decompile. The editor shows real SBF entry names at play/bind sites when a
   bank is loaded (mus_decompile_with_names); before the bind-aware compiler that
   text failed to recompile ("play GAMINT" -> "expected 'sound_N'"), which broke
   Compile/Compile&Run/Save with a bank. These tests pin:
     1. real fixture: decompile_with_names(jo_gamemus) recompiles, byte-identical
        to the names-less decompile's bytecode (incl. a name with a space -> the
        play target is quoted and still resolves);
     2. a hand-written script: `play "Name"` / `play Name` / `play sound_N` all
        resolve through the bind map to the right index; an unbound name fails. */

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

/* Decompile `s` (names-aware when names!=NULL), compile, and hand the caller the
   recompiled bytecode. Returns 1 on success. */
static int decompile_compile(const MusScript *s,
                             const char *const *names, unsigned name_count,
                             MusScript *out) {
    int needed = names
        ? mus_decompile_with_names(s, names, name_count, NULL, 0)
        : mus_decompile(s, NULL, 0);
    if (needed <= 0) return 0;
    char *text = (char *)malloc((size_t)needed + 1);
    if (!text) return 0;
    int written = names
        ? mus_decompile_with_names(s, names, name_count, text, (size_t)needed + 1)
        : mus_decompile(s, text, (size_t)needed + 1);
    if (written != needed) { free(text); return 0; }
    text[needed] = 0;
    const char *err = NULL;
    int el = 0, ec = 0;
    int rc = mus_compile(text, out, &el, &ec, &err);
    if (rc != 0) {
        fprintf(stderr, "  compile error at %d:%d: %s\n", el, ec, err ? err : "(null)");
    }
    free(text);
    return rc == 0;
}

static int test_names_roundtrip_jo_gamemus(void) {
    MusFile mf;
    int rc = mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin");
    CHECK(rc == 0, "open original");

    /* SBF entry names: index 0 deliberately contains a space (forces a quoted
       play target) so we exercise the quote-safe emit + the String play path. */
    static const char *names[64];
    static char storage[64][32];
    for (int i = 0; i < 64; ++i) {
        if (i == 0)      snprintf(storage[i], sizeof(storage[i]), "Main Theme");
        else if (i == 1) snprintf(storage[i], sizeof(storage[i]), "AMB.LOOP");
        else             snprintf(storage[i], sizeof(storage[i]), "Trk_%d", i);
        names[i] = storage[i];
    }

    MusScript named = {}, plain = {};
    CHECK(decompile_compile(&mf.scripts[0], names, 64, &named),
          "names-aware decompile recompiles");
    CHECK(decompile_compile(&mf.scripts[0], NULL, 0, &plain),
          "names-less decompile recompiles");

    CHECK(named.code_size == plain.code_size, "bytecode size matches");
    CHECK(named.code_size > 0, "non-empty bytecode");
    CHECK(memcmp(named.code, plain.code, named.code_size) == 0,
          "names-aware and names-less compile to identical bytecode");

    mus_script_free(&named);
    mus_script_free(&plain);
    mus_close(&mf);
    return 1;
}

/* Count occurrences of the 2-byte play opcode (0x3E, idx) in a code buffer. */
static int has_play(const uint8_t *code, uint32_t n, uint8_t idx) {
    for (uint32_t i = 0; i + 1 < n; ++i) {
        if (code[i] == 0x3E && code[i + 1] == idx) return 1;
    }
    return 0;
}

static int test_bind_resolves_play_by_name(void) {
    const char *src =
        "script T\n"
        "bind sound_0 \"Main Theme\"\n"
        "bind sound_1 \"AMB.LOOP\"\n"
        "bind sound_2 \"Trk2\"\n"
        "section Main\n"
        "{\n"
        "  play \"Main Theme\"\n"   /* quoted, spaced -> index 0 */
        "  play \"AMB.LOOP\"\n"     /* quoted, dotted -> index 1 */
        "  play Trk2\n"            /* bare bound name -> index 2 */
        "  play sound_0\n"         /* names-less still works -> index 0 */
        "}\n";
    MusScript out = {};
    const char *err = NULL;
    int el = 0, ec = 0;
    int rc = mus_compile(src, &out, &el, &ec, &err);
    if (rc != 0) fprintf(stderr, "  compile error %d:%d: %s\n", el, ec, err ? err : "(null)");
    CHECK(rc == 0, "bound-name play compiles");
    CHECK(has_play(out.code, out.code_size, 0), "play \"Main Theme\" -> 0");
    CHECK(has_play(out.code, out.code_size, 1), "play \"AMB.LOOP\" -> 1");
    CHECK(has_play(out.code, out.code_size, 2), "play Trk2 -> 2");
    mus_script_free(&out);

    /* An unbound name must be rejected, not silently mis-resolved. */
    const char *bad =
        "script T\n"
        "section Main\n"
        "{\n"
        "  play Nonexistent\n"
        "}\n";
    MusScript out2 = {};
    err = NULL;
    rc = mus_compile(bad, &out2, &el, &ec, &err);
    CHECK(rc != 0, "unbound play target rejected");
    mus_script_free(&out2);
    return 1;
}

int main(void) {
    RUN_TEST(test_names_roundtrip_jo_gamemus);
    RUN_TEST(test_bind_resolves_play_by_name);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
