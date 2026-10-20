/* The decompiler's emitter, byte-exact against a golden, over the minted programs (tests/fixtures/
   synth_mus_gen.cpp): synth_gamemus.bin against golden_synth_gamemus.mus.txt (the emitter's own output
   over the minted program, its MessageHandler as its function, its parameters by their debug names, a
   `#line` where its compact source runs past the header the text emits); and synth_forms.bin, a program
   in the shipped menu programs' layout (its source binds every sound and declares every section ahead
   of its code, as MDEdit's do), against golden_synth_forms.mus.txt, its own source: each statement on
   the line MDEdit's line table names, the text padded to it with no `#line` needed, as the shipped
   gamemus.bin decompiles (SV(200) on its line 83, the handler's header on 87). That the text compiles
   back to the file's bytes is mus_encode_idempotence's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/mus/mus.h>

#include <string>
#include <vector>

using namespace opennova::mus;

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

/* The forms program decompiles to its own source; MDEdit's line table read (a line per statement, no
   function), the text has no `#line`, and every line the table names holds a statement of the text or a
   section's closing brace, its `done` (the shipped menumus.bin's table names its eleven so). */
static int test_decompile_synth_forms(void) {
    CHECK(decompile_matches_golden(MUS_FIXTURE_DIR "/synth_forms.bin", MUS_FIXTURE_DIR "/golden_synth_forms.mus.txt"),
          "the forms program decompiles to its source");
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/synth_forms.bin") == 0, "open synth_forms.bin");
    const MusScript &s = mf.scripts[0];
    CHECK(s.function_count == 0 && s.local_count == 0 && s.line_count == 78, "MDEdit's debug tables read");
    const int needed = mus_decompile(&s, NULL, 0);
    CHECK(needed > 0, "decompile");
    std::string text((size_t)needed + 1, '\0');
    mus_decompile(&s, &text[0], text.size());
    text.resize((size_t)needed);
    CHECK(text.find("#line") == std::string::npos, "the layout fits without a #line");
    std::vector<std::string> lines(1);
    for (const char c : text) {
        if (c == '\n') lines.emplace_back();
        else lines.back() += c;
    }
    uint32_t closes = 0;
    for (uint32_t i = 0; i < s.line_count; ++i) {
        const uint32_t at = s.lines[i].line;
        CHECK(at >= 1 && at < lines.size(), "a line the table names is the text's");
        const std::string &line = lines[at - 1];
        const size_t first = line.find_first_not_of(' ');
        closes += line == "}" ? 1 : 0;
        CHECK(line == "}" || (first != std::string::npos && line[first] != '{' && line[first] != '}' &&
                              line.compare(first, 2, "//") != 0),
              "the table's line holds a statement or a section's close");
    }
    CHECK(closes == s.section_count, "each section's close, its done, on its line");
    CHECK(line_of(text, "Var14 = 0\n") == s.lines[0].line, "Begin's first statement on the table's first line");
    mus_close(&mf);
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

int main(void) {
    RUN_TEST(test_decompile_synth_gamemus);
    RUN_TEST(test_decompile_synth_forms);
    RUN_TEST(test_decompile_two_pass_size);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
