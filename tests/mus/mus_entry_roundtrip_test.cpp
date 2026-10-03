#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/mus/mus.h>
#include <base/vfs/vfs.h>

#include <string>
#include <vector>

#include "common/retail_paths.h"

using namespace opennova::mus;

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

/* Entry-section round-trip fidelity.

   The compiler hardcodes entry_section_index = 0 (mus_compile.cpp), i.e. the
   first-DECLARED section is the entry. The decompiler emits sections in
   code-offset order, so a faithful round-trip requires the original's entry
   section to also be its lowest-offset section. This is true for both shipped
   stock bins -- verified by reading the on-disk section tables: entry index 0 IS
   the lowest-offset section (offsets are monotonic by index) -- and for the
   minted programs, which the same compiler wrote. This test pins that
   property: re-compiling a decompiled script must keep playback starting at
   the SAME NAMED section.

   It exists because neither sibling test can see an entry drift: the text
   round-trip (mus_roundtrip_test) doesn't encode the entry index in the text, and
   encode-idempotence (mus_encode_idempotence_test) compares the compiler's output
   to itself. If a future change made the decompiler stop emitting the entry first
   (so the recompiled index 0 names a different section), this fails. The minted
   synth_{gamemus,menumus}.bin run unconditionally; the shipped pair from the
   reference fixture set (OPENNOVA_JO_ASSETS) is the retail leg, and so are the
   installed expansions' programs (OPENNOVA_JO_DIR), whose text is also a fixed
   point (decompiled, compiled and decompiled again: the same text). */
static int entry_roundtrip_file(MusFile &mf, bool same_text);

static int entry_roundtrip(const char *path) {
    MusFile mf;
    int rc = mus_open(&mf, path);
    CHECK(rc == 0, "open original");
    return entry_roundtrip_file(mf, true);
}

/* `same_text`: the recompiled program decompiles to the original's text too. */
static int entry_roundtrip_file(MusFile &mf, bool same_text) {
    int rc = 0;
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
    if (same_text) {
        /* The recompiled program decompiles to the same text: every play, wide or not, and every
           section and branch as the original's (MDEdit's own layout, a leading nop and the like, is
           not the compiler's, so the bytes themselves differ; the editor holds such a script read
           only, music_script_type.cpp). */
        const int again_needed = mus_decompile(&recomp, NULL, 0);
        CHECK(again_needed > 0, "decompile the recompiled program");
        char *again = (char *)malloc((size_t)again_needed + 1);
        CHECK(again, "alloc");
        CHECK(mus_decompile(&recomp, again, (size_t)again_needed + 1) == again_needed, "decompile written");
        again[again_needed] = 0;
        const bool same = strcmp(again, text) == 0;
        if (!same) {
            size_t at = 0;
            while (again[at] && again[at] == text[at]) ++at;
            fprintf(stderr, "  text differs at %zu: '%.60s' | recompiled: '%.60s'\n", at, text + at, again + at);
        }
        free(again);
        CHECK(same, "the recompiled program decompiles to the original's text");
    }

    free(text);
    mus_script_free(&recomp);
    mus_close(&mf);
    return 1;
}

static int test_entry_roundtrip_synth_gamemus(void) {
    return entry_roundtrip(MUS_FIXTURE_DIR "/synth_gamemus.bin");
}

static int test_entry_roundtrip_synth_menumus(void) {
    return entry_roundtrip(MUS_FIXTURE_DIR "/synth_menumus.bin");
}

static int test_entry_roundtrip_gamemus(void) {
    return entry_roundtrip(g_retail_gamemus.c_str());
}

static int test_entry_roundtrip_menumus(void) {
    return entry_roundtrip(g_retail_menumus.c_str());
}

/* ADR 0046 S16: each installed expansion's two music programs (`G<exp>.bin`, `M<exp>.bin`, which
   `/exp <exp>` loads [orig: Expansion_LoadAssets @ 0x4a491d / 0x4a494a]) decompile, compile and
   decompile again to the same text. jox01's MJox01.bin plays 61 sounds past index 255 through the word-wide
   play [orig: AudioVM_Op_PlayWait @ 0x672C90], which the compiler once refused. */
static int expansion_programs(void) {
    int failures = 0;
    const std::string root = retail::install();
    for (const std::string &expansion : retail::expansions()) {
        opennova::Vfs vfs;
        if (!vfs.mount_game(root, expansion, opennova::VfsMountMode::Packed)) { ++failures; continue; }
        for (const char *prefix : {"G", "M"}) {
            const std::string name = prefix + expansion + ".bin";
            std::vector<uint8_t> bytes;
            if (!vfs.read_file(name, bytes)) continue; /* an expansion without its own program plays the base's */
            MusFile mf;
            printf("Running %s/%s... ", expansion.c_str(), name.c_str());
            const int ok = mus_open_memory(&mf, bytes.data(), bytes.size()) == 0 && entry_roundtrip_file(mf, true);
            printf(ok ? "PASS\n" : "FAIL\n");
            if (ok) {
                ++passed;
            } else {
                ++failed;
                ++failures;
            }
        }
    }
    return failures;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    RUN_TEST(test_entry_roundtrip_synth_gamemus);
    RUN_TEST(test_entry_roundtrip_synth_menumus);

    g_retail_gamemus = retail::reference_fixture("mus/jo_gamemus.bin");
    g_retail_menumus = retail::reference_fixture("mus/jo_menumus.bin");
    if (g_retail_gamemus.empty() || g_retail_menumus.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mus/jo_{gamemus,menumus}.bin (the shipped programs)");
    } else {
        RUN_TEST(test_entry_roundtrip_gamemus);
        RUN_TEST(test_entry_roundtrip_menumus);
    }
    if (retail::install().empty()) retail::skip_leg("OPENNOVA_JO_DIR (the expansions' music programs)");
    else expansion_programs();
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
