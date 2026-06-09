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

/* Entry-section round-trip fidelity.

   The compiler hardcodes entry_section_index = 0 (mus_compile.cpp), i.e. the
   first-DECLARED section is the entry. The decompiler emits sections in
   code-offset order, so a faithful round-trip requires the original's entry
   section to also be its lowest-offset section. This is true for both shipped
   stock bins -- verified by reading the on-disk section tables: entry index 0 IS
   the lowest-offset section (offsets are monotonic by index). This test pins that
   property: re-compiling a decompiled stock script must keep playback starting at
   the SAME NAMED section.

   It exists because neither sibling test can see an entry drift: the text
   round-trip (mus_roundtrip_test) doesn't encode the entry index in the text, and
   encode-idempotence (mus_encode_idempotence_test) compares the compiler's output
   to itself. If a future change made the decompiler stop emitting the entry first
   (so the recompiled index 0 names a different section), this fails. */
static int entry_roundtrip(const char *path) {
    MusFile mf;
    int rc = mus_open(&mf, path);
    CHECK(rc == 0, "open original");
    CHECK(mf.scripts != NULL && mf.header.chunk_count >= 1, "has a chunk");
    const MusScript *orig = &mf.scripts[0];
    CHECK(orig->section_count > 0, "original has sections");
    CHECK(orig->entry_section_index < orig->section_count, "original entry in range");

    char entry_name[MUS_SECTION_NAME_SIZE];
    strncpy(entry_name, orig->sections[orig->entry_section_index].name,
            sizeof(entry_name) - 1);
    entry_name[sizeof(entry_name) - 1] = 0;

    /* Sanity: the entry must be the lowest code-offset section, since that is the
       only layout the hardcoded-0 compiler can faithfully reproduce. */
    uint32_t entry_off = orig->sections[orig->entry_section_index].code_offset;
    for (uint32_t i = 0; i < orig->section_count; ++i)
        CHECK(orig->sections[i].code_offset >= entry_off,
              "entry section is the lowest-offset section");

    int needed = mus_decompile(orig, NULL, 0);
    CHECK(needed > 0, "decompile size");
    char *text = (char *)malloc((size_t)needed + 1);
    CHECK(text, "alloc decompiled text");
    int written = mus_decompile(orig, text, (size_t)needed + 1);
    CHECK(written == needed, "decompile written");
    text[needed] = 0;

    MusScript recomp = {};
    const char *err = NULL;
    int el = 0, ec = 0;
    rc = mus_compile(text, &recomp, &el, &ec, &err);
    CHECK(rc == 0, err ? err : "recompile decompiled text");
    CHECK(recomp.entry_section_index < recomp.section_count, "recomp entry in range");

    const char *recomp_entry = recomp.sections[recomp.entry_section_index].name;
    if (strncmp(recomp_entry, entry_name, MUS_SECTION_NAME_SIZE) != 0)
        fprintf(stderr, "  entry drifted: original '%s' -> recompiled '%s'\n",
                entry_name, recomp_entry);
    CHECK(strncmp(recomp_entry, entry_name, MUS_SECTION_NAME_SIZE) == 0,
          "recompiled entry resolves to the same named section as the original");

    free(text);
    mus_script_free(&recomp);
    mus_close(&mf);
    return 1;
}

static int test_entry_roundtrip_gamemus(void) {
    return entry_roundtrip(MUS_FIXTURE_DIR "/jo_gamemus.bin");
}

static int test_entry_roundtrip_menumus(void) {
    return entry_roundtrip(MUS_FIXTURE_DIR "/jo_menumus.bin");
}

int main(void) {
    RUN_TEST(test_entry_roundtrip_gamemus);
    RUN_TEST(test_entry_roundtrip_menumus);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
