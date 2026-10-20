/* Write-path parity for MUS: a program decompiled to its MUS text, compiled and encoded again is its own bytes.
   The text form is the tool's own authoring syntax (the game reads only the binary); the compiler writes MDEdit's
   layout (mus_compile.cpp: the leading nop, the code padded to four bytes, the editor debug section with its
   sections, globals, functions, their parameters and the line table, the alignment and the name table), and the
   decompiler puts each statement on the line the table names. So encode(compile(decompile(x))) == x byte for
   byte, as it is for every music script the shipped install's archives hold (base and jox01's: gamemus.bin,
   menumus.bin, GJox01.bin, MJox01.bin), over the minted programs (tests/fixtures/synth_mus_gen.cpp): the pair in
   the shipped pair's shapes (synth_{gamemus,menumus}.bin; the gamescript carries a MessageHandler function) and
   synth_forms.bin, the shipped menu programs' form (a global of its own, assignments, `==` and `&&`, nested ifs,
   plays past 255 through the word-wide play, the layout's text with no `#line`). And the text is a fixed
   point: the recompiled program decompiles to the same text. Each file is first written from its model alone
   (read, then mus_encode_file over its parsed programs: the method table, the chunk's fields and its tables as
   read), which is its own bytes too. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <formats/mus/mus.h>
#include <formats/sbf/sbf.h>


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


static bool decompile(const MusScript &s, std::string &text) {
    const int needed = mus_decompile(&s, NULL, 0);
    if (needed <= 0) return false;
    text.assign((size_t)needed + 1, '\0');
    if (mus_decompile(&s, &text[0], text.size()) != needed) return false;
    text.resize((size_t)needed);
    return true;
}

/* The bytes' program through its MUS text and back, `text` its decompile. 1 when the bytes come back. */
static int assert_text_roundtrip(const std::vector<uint8_t> &bytes, const std::string &what) {
    MusFile mf;
    CHECK(mus_open_memory(&mf, bytes.data(), bytes.size()) == 0, "open the program");
    /* The model: the file read, written again from what it holds (the method table, each chunk's fields and
       tables), is its own bytes. */
    {
        std::vector<const MusScript *> programs;
        for (uint32_t i = 0; i < mf.header.chunk_count; ++i) programs.push_back(&mf.scripts[i]);
        uint8_t *out = NULL;
        size_t n = 0;
        const int rc = mus_encode_file(programs.data(), (uint32_t)programs.size(), &out, &n);
        const std::vector<uint8_t> written(out, out + (rc == 0 ? n : 0));
        mus_free(out);
        if (rc != 0 || written != bytes) {
            mus_close(&mf);
            fprintf(stderr, "  %s: the model does not write back as its bytes (encoder %d)\n", what.c_str(), rc);
            return 0;
        }
    }
    std::string text;
    const bool decompiled = decompile(mf.scripts[0], text);
    mus_close(&mf);
    CHECK(decompiled, "decompile the program");
    MusScript recomp = {};
    const char *err = NULL;
    int err_line = 0, err_col = 0;
    if (mus_compile(text.c_str(), &recomp, &err_line, &err_col, &err) != 0) {
        fprintf(stderr, "  %s: compile error at %d:%d: %s\n", what.c_str(), err_line, err_col, err ? err : "?");
        return 0;
    }
    const MusScript *arr[1] = { &recomp };
    uint8_t *out = NULL;
    size_t n = 0;
    const int rc = mus_encode_file(arr, 1, &out, &n);
    mus_script_free(&recomp);
    CHECK(rc == 0, "encode the recompiled program");
    std::vector<uint8_t> again(out, out + n);
    mus_free(out);
    if (again != bytes) {
        size_t i = 0;
        while (i < again.size() && i < bytes.size() && again[i] == bytes[i]) ++i;
        fprintf(stderr, "  %s: %zu bytes, recompiled %zu, first difference at %zu\n", what.c_str(), bytes.size(),
                again.size(), i);
    }
    CHECK(again == bytes, "the program's MUS text compiles back to its own bytes");
    /* The text is a fixed point. */
    MusFile back;
    CHECK(mus_open_memory(&back, again.data(), again.size()) == 0, "open the recompiled program");
    std::string text2;
    const bool ok = decompile(back.scripts[0], text2);
    mus_close(&back);
    CHECK(ok && text2 == text, "the recompiled program decompiles to the same text");
    printf("\n  %s: %zu bytes, through its text byte for byte\n", what.c_str(), bytes.size());
    return 1;
}

/* The writer's bytes of each minted program (FNV-1a64): a change to a vector is a change to the bytes the
   compiler and the encoder write, which needs an [orig] witness (MDEdit's layout) or a D-MUS entry
   (`mus_encode_idempotence_test --dump` prints the table). */
struct WriterPin {
    const char *name;
    size_t len;
    uint64_t hash;
};
static const WriterPin kWriterPins[] = {
    {"synth_gamemus.bin", 2313u, 0xabc7a9acdd8d76d7ull},
    {"synth_menumus.bin", 1849u, 0xe66fd68fead52c63ull},
    {"synth_forms.bin", 2953u, 0x9ba058c5c92bf940ull},
};
static bool g_dump = false;

static int roundtrip_file(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "open the file");
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes((size_t)(n > 0 ? n : 0));
    const bool read = fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    CHECK(read, "read the file");
    if (!assert_text_roundtrip(bytes, path)) return 0;
    /* The bytes the writer wrote back, pinned. */
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    uint64_t h = 1469598103934665603ull;
    for (const uint8_t b : bytes) h = (h ^ b) * 1099511628211ull;
    if (g_dump) {
        printf("\n\t{\"%s\", %zuu, 0x%016llxull},", name, bytes.size(), (unsigned long long)h);
        return 1;
    }
    for (const WriterPin &pin : kWriterPins)
        if (strcmp(pin.name, name) == 0) {
            if (pin.len != bytes.size() || pin.hash != h)
                fprintf(stderr, "  %s written: %zu bytes, 0x%016llx; pinned %zu bytes, 0x%016llx\n", name,
                        bytes.size(), (unsigned long long)h, pin.len, (unsigned long long)pin.hash);
            CHECK(pin.len == bytes.size() && pin.hash == h, "the writer's bytes as pinned");
            return 1;
        }
    CHECK(false, "a pin for every program");
    return 0;
}

static int test_synth_gamemus(void) { return roundtrip_file(MUS_FIXTURE_DIR "/synth_gamemus.bin"); }
static int test_synth_menumus(void) { return roundtrip_file(MUS_FIXTURE_DIR "/synth_menumus.bin"); }
static int test_synth_forms(void) { return roundtrip_file(MUS_FIXTURE_DIR "/synth_forms.bin"); }

/* A script's plays (mus_compile_plays over its MUS text), each with the sound index it compiles to. */
static int script_plays(const char *path, std::vector<MusPlayUse> &plays) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open the script");
    std::string text;
    const bool decompiled = decompile(mf.scripts[0], text);
    mus_close(&mf);
    CHECK(decompiled, "decompile the script");
    MusScript compiled = {};
    int line = 0, col = 0;
    const char *err = NULL;
    CHECK(mus_compile_plays(text.c_str(), &compiled, &plays, &line, &col, &err) == 0, "compile its text");
    mus_script_free(&compiled);
    return 1;
}

/* A script's plays against the bank it plays from: every play's index below the bank's count of streams. One
   past would play the bank's first stream's data in its place [orig: AudioVM_StartSound @ 0x671FF0 ->
   AudioVM_SelectStream @ 0x671BC4..0x671BCC, an index past the entries made 0], which no shipped script does
   (gamemus.bin's 32 plays over GAMEMUS.SBF's 13 streams, menumus.bin's 191 over 47, GJox01.bin's 32 over 13,
   MJox01.bin's 884 over 317). Over the minted pair: synth_gamemus.bin's plays over synth_gamemus.sbf (its bank
   by mus_bank_name, GAMEMUS.SBF's shape). And the forms program's plays, the three past 255 among them, each
   at the index its text names. The counts are pinned. */
static int test_minted_plays(void) {
    std::vector<MusPlayUse> plays;
    CHECK(script_plays(MUS_FIXTURE_DIR "/synth_gamemus.bin", plays), "the gamescript's plays");
    CHECK(mus_bank_name("synth_gamemus.bin") == "SYNTH_GAMEMUS.SBF", "the script's bank by its name");
    FILE *f = fopen(MUS_FIXTURE_DIR "/../sbf/synth_gamemus.sbf", "rb");
    CHECK(f != NULL, "open the bank");
    std::vector<uint8_t> bank;
    fseek(f, 0, SEEK_END);
    bank.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    const bool bank_read = fread(bank.data(), 1, bank.size(), f) == bank.size();
    fclose(f);
    CHECK(bank_read, "read the bank");
    opennova::sbf::SbfArchive arc = {};
    CHECK(opennova::sbf::sbf_open_memory(&arc, bank.data(), bank.size()) == 0, "open the bank");
    const uint32_t streams = arc.header.entry_count;
    opennova::sbf::sbf_close(&arc);
    uint32_t highest = 0;
    for (const MusPlayUse &play : plays) {
        highest = std::max(highest, play.index);
        CHECK(play.index < streams, "every play's stream one its bank has");
    }
    printf("\n  synth_gamemus.bin: %zu plays, the highest %u, %u streams", plays.size(), highest, streams);
    CHECK(plays.size() == 9 && highest == 6 && streams == 13, "the minted pair's counts");

    std::vector<MusPlayUse> forms;
    CHECK(script_plays(MUS_FIXTURE_DIR "/synth_forms.bin", forms), "the forms program's plays");
    uint32_t past = 0;
    highest = 0;
    for (const MusPlayUse &play : forms) {
        highest = std::max(highest, play.index);
        past += play.index > 255 ? 1 : 0;
    }
    printf("\n  synth_forms.bin: %zu plays, the highest %u, %u past 255\n", forms.size(), highest, past);
    CHECK(forms.size() == 15 && highest == 316 && past == 3, "the forms program's counts");
    return 1;
}

int main(int argc, char **argv) {
    g_dump = argc > 1 && strcmp(argv[1], "--dump") == 0;
    RUN_TEST(test_synth_gamemus);
    RUN_TEST(test_synth_menumus);
    RUN_TEST(test_synth_forms);
    RUN_TEST(test_minted_plays);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
