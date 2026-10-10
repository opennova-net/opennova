/* Write-path parity for MUS: a program decompiled to its MUS text, compiled and encoded again is its own bytes.

   The text form is the tool's own authoring syntax (the game reads only the binary); the compiler writes MDEdit's
   layout (mus_compile.cpp: the leading nop, the code padded to four bytes, the editor debug section with its
   sections, globals, functions, their parameters and the line table, the alignment and the name table), and the
   decompiler puts each statement on the line the table names. So encode(compile(decompile(x))) == x byte for
   byte: the minted pair (synth_{gamemus,menumus}.bin, the encoder's own output; the gamescript carries a
   MessageHandler function) in core; the reference fixture set's shipped pair (OPENNOVA_JO_ASSETS) and every
   music script the install's archives hold, base and each installed expansion's (OPENNOVA_JO_DIR: gamemus.bin,
   menumus.bin, jox01's GJox01.bin and MJox01.bin), as the retail legs. And the text is a fixed point: the
   recompiled program decompiles to the same text. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <formats/mus/mus.h>

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
    return assert_text_roundtrip(bytes, path);
}

static int test_synth_gamemus(void) { return roundtrip_file(MUS_FIXTURE_DIR "/synth_gamemus.bin"); }
static int test_synth_menumus(void) { return roundtrip_file(MUS_FIXTURE_DIR "/synth_menumus.bin"); }
static int test_reference_gamemus(void) { return roundtrip_file(g_retail_gamemus.c_str()); }
static int test_reference_menumus(void) { return roundtrip_file(g_retail_menumus.c_str()); }

/* Every SCR0 program the install's archives hold, base and each installed expansion's. */
static int test_install_scripts(void) {
    const std::string install = retail::install();
    std::vector<std::string> mounts{std::string()};
    for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
    size_t programs = 0;
    std::vector<std::string> seen;
    for (const std::string &expansion : mounts) {
        opennova::Vfs vfs;
        CHECK(vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed), "mount the install");
        for (const auto &location : vfs.list_files()) {
            const std::string &name = location.logical_name;
            if (name.size() < 4 || retail::lower_ascii(name.substr(name.size() - 4)) != ".bin") continue;
            std::vector<uint8_t> bytes;
            if (!vfs.read_file(name, bytes) || mus_validate(bytes.data(), bytes.size()) != 0) continue;
            const std::string key = retail::lower_ascii(name);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            if (!assert_text_roundtrip(bytes, name + (expansion.empty() ? std::string() : " (" + expansion + ")")))
                return 0;
            ++programs;
        }
    }
    CHECK(programs >= 2, "the install holds its music scripts");
    printf("\n  %zu shipped music scripts, each through its text byte for byte\n", programs);
    return 1;
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    RUN_TEST(test_synth_gamemus);
    RUN_TEST(test_synth_menumus);

    g_retail_gamemus = retail::reference_fixture("mus/jo_gamemus.bin");
    g_retail_menumus = retail::reference_fixture("mus/jo_menumus.bin");
    if (g_retail_gamemus.empty() || g_retail_menumus.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mus/jo_{gamemus,menumus}.bin (the shipped programs)");
    } else {
        RUN_TEST(test_reference_gamemus);
        RUN_TEST(test_reference_menumus);
    }
    if (retail::install().empty()) {
        retail::skip_leg("OPENNOVA_JO_DIR (every music script the install's archives hold)");
    } else {
        RUN_TEST(test_install_scripts);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
