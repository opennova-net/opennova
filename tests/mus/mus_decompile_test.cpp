/* The decompiler's emitter, byte-exact against a golden: the minted
   synth_gamemus.bin against golden_synth_gamemus.mus.txt (both from
   tests/fixtures/synth_mus_gen.cpp, the golden being the emitter's own output
   over the minted program) unconditionally. The retail leg reads the shipped
   jo_gamemus.bin (the reference fixture set, OPENNOVA_JO_ASSETS): its
   MessageHandler as its function, its parameters by their debug names, and each
   statement on the line MDEdit's line table names (its source's: SV(200) on 83,
   the handler's header on 87, the last `}` on 165) with no `#line` needed; that
   the text compiles back to the file's bytes is mus_encode_idempotence's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/mus/mus.h>

#include <string>

#include "common/retail_paths.h"

using namespace opennova::mus;

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static std::string g_retail_gamemus;

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

static int decompile_matches_golden(const char *bin_path, const char *golden_path) {
    MusFile mf;
    int rc = mus_open(&mf, bin_path);
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
    char *gold = slurp_text(golden_path, &gold_size);
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

static int test_decompile_synth_gamemus(void) {
    return decompile_matches_golden(MUS_FIXTURE_DIR "/synth_gamemus.bin",
                                    MUS_FIXTURE_DIR "/golden_synth_gamemus.mus.txt");
}

/* The 1-based line of the first occurrence of `needle` in `text`, 0 for none. */
static size_t line_of(const std::string &text, const char *needle) {
    const size_t at = text.find(needle);
    if (at == std::string::npos) return 0;
    size_t line = 1;
    for (size_t i = 0; i < at; ++i) line += text[i] == '\n';
    return line;
}

static int test_decompile_jo_gamemus(void) {
    MusFile mf;
    CHECK(mus_open(&mf, g_retail_gamemus.c_str()) == 0, "open jo_gamemus.bin");
    const MusScript &s = mf.scripts[0];
    CHECK(s.function_count == 1 && strcmp(s.functions[0].name, "MessageHandler") == 0 && s.local_count == 2 &&
          s.line_count == 55, "MDEdit's debug tables read");
    const int needed = mus_decompile(&s, NULL, 0);
    CHECK(needed > 0, "decompile");
    std::string text((size_t)needed + 1, '\0');
    mus_decompile(&s, &text[0], text.size());
    text.resize((size_t)needed);
    mus_close(&mf);
    CHECK(line_of(text, "SV(200)\n") == 83, "Begin's first statement on its line (83)");
    CHECK(line_of(text, "handler MessageHandler(msgtype, source)\n") == 87, "the handler's header on its line (87)");
    CHECK(line_of(text, "on (msgtype) enter Missionnull Missionwin Missionlose\n") == 90, "the handler's table");
    CHECK(text.find("#line") == std::string::npos, "the shipped layout fits without a #line");
    CHECK(text.find("frame ") == std::string::npos && text.find("l_32") == std::string::npos,
          "the frame setup and the parameters are the handler's");
    return 1;
}

static int test_decompile_two_pass_size(void) {
    MusFile mf;
    int rc = mus_open(&mf, MUS_FIXTURE_DIR "/synth_gamemus.bin");
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

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    RUN_TEST(test_decompile_synth_gamemus);
    RUN_TEST(test_decompile_two_pass_size);

    g_retail_gamemus = retail::reference_fixture("mus/jo_gamemus.bin");
    if (g_retail_gamemus.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mus/jo_gamemus.bin (the shipped game script)");
    } else {
        RUN_TEST(test_decompile_jo_gamemus);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
