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

/* ---- The instruction streams compared (review L7): the text fixed point cannot see what
   the decompiler drops or conflates the same way twice, so the original's bytecode and the
   recompile's are decoded by an oracle of their own (the operand widths each handler
   reads [orig: the 65-entry dispatch table @ 0x84F220, AudioVM_Op_* @ 0x672770..0x672CF0])
   and compared instruction by instruction: opcodes, operands, a branch's target as the
   instruction it lands on, a table's entries, and each section's entry. `nop`s are dropped
   (MDEdit pads with them; the text has none), and a push's width is the compiler's choice
   (0x01 u8 / 0x02 u32 push the same value). ---- */
struct OracleInst {
    uint32_t offset = 0, size = 0;
    uint8_t op = 0;
    std::vector<uint8_t> operand;  /* the bytes after the opcode, the table's included */
    uint32_t value = 0;            /* a push's value, a branch's target */
    bool branch = false;           /* goto, brfalse, brtrue, callv: `value` an offset */
};

/* The bytes each opcode's handler reads after it; -1 for a tablexec (its own walk), -2 for
   no opcode. */
static int oracle_width(uint8_t op) {
    switch (op) {
    case 0x01: case 0x03: case 0x04: case 0x07: case 0x08: case 0x09:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x33: case 0x38: case 0x3B: case 0x3E: case 0x40:
        return 1;
    case 0x05: case 0x06: case 0x0A: case 0x0B: case 0x3D: return 2;
    case 0x02: case 0x30: case 0x31: case 0x32: case 0x34: return 4;
    case 0x35: return -1;
    default: return op <= 0x40 ? 0 : -2;
    }
}

static bool oracle_decode(const uint8_t *code, uint32_t size, std::vector<OracleInst> &out) {
    out.clear();
    uint32_t pos = 0;
    while (pos < size) {
        OracleInst inst;
        inst.offset = pos;
        inst.op = code[pos++];
        int width = oracle_width(inst.op);
        if (width == -2) return false;
        if (width == -1) {
            if (pos + 4 > size) return false;
            width = 4 + code[pos] * code[pos + 2];
        }
        if (pos + (uint32_t)width > size) return false;
        inst.operand.assign(code + pos, code + pos + width);
        pos += (uint32_t)width;
        inst.size = pos - inst.offset;
        const auto le = [&](size_t at, size_t n) {
            uint32_t v = 0;
            for (size_t k = 0; k < n; ++k) v |= (uint32_t)inst.operand[at + k] << (8 * k);
            return v;
        };
        if (inst.op == 0x01 || inst.op == 0x02) {
            inst.value = le(0, inst.operand.size());
            inst.op = 0x01;
            inst.operand.clear();
        } else if (inst.op == 0x30 || inst.op == 0x31 || inst.op == 0x32 || inst.op == 0x34) {
            inst.branch = true;
            inst.value = le(0, 4);
        }
        if (inst.op != 0x00) out.push_back(inst);
    }
    return true;
}

/* The index of the first instruction (nops dropped) at `offset` or after it. */
static size_t oracle_index(const std::vector<OracleInst> &insts, uint32_t offset) {
    size_t i = 0;
    while (i < insts.size() && insts[i].offset < offset) ++i;
    return i;
}

/* Compare `a` (the original) with `b` (its recompile): 1 when the same program, instruction for
   instruction (a function's frame setup 0x38 too: D-MUS-14 closed, the text's `handler`); any
   difference is reported and fails. */
static int same_program(const MusScript &a, const MusScript &b) {
    std::vector<OracleInst> ia, ib;
    CHECK(oracle_decode(a.code, a.code_size, ia), "decode the original's bytecode");
    CHECK(oracle_decode(b.code, b.code_size, ib), "decode the recompile's bytecode");
    if (ia.size() != ib.size())
        fprintf(stderr, "  instructions: original %zu, recompiled %zu\n", ia.size(), ib.size());
    CHECK(ia.size() == ib.size(), "as many instructions (nops dropped)");
    for (size_t i = 0; i < ia.size(); ++i) {
        const OracleInst &x = ia[i], &y = ib[i];
        bool same = x.op == y.op && x.value == y.value && x.operand == y.operand;
        if (x.branch && y.branch && x.op == y.op)
            same = oracle_index(ia, x.value) == oracle_index(ib, y.value);
        if (x.op == 0x35 && y.op == 0x35 && x.operand.size() == y.operand.size() && x.operand.size() >= 4) {
            /* count, stride; each entry by its own opcode, an address entry by where it lands */
            same = x.operand[0] == y.operand[0] && x.operand[2] == y.operand[2];
            const size_t stride = x.operand[2];
            for (size_t e = 0; same && e < x.operand[0]; ++e) {
                const uint8_t *ex = &x.operand[4 + e * stride], *ey = &y.operand[4 + e * stride];
                if (stride >= 5 && ex[0] == 0x30 && ey[0] == 0x30) {
                    const uint32_t tx = ex[1] | ex[2] << 8 | ex[3] << 16 | (uint32_t)ex[4] << 24;
                    const uint32_t ty = ey[1] | ey[2] << 8 | ey[3] << 16 | (uint32_t)ey[4] << 24;
                    same = oracle_index(ia, tx) == oracle_index(ib, ty);
                } else {
                    same = memcmp(ex, ey, stride) == 0;
                }
            }
        }
        if (!same)
            fprintf(stderr, "  instruction %zu differs: original op %02X at %u, recompiled op %02X at %u\n", i, x.op,
                    x.offset, y.op, y.offset);
        CHECK(same, "the same instruction");
    }
    for (uint32_t s = 0; s < a.section_count; ++s) {
        bool found = false;
        for (uint32_t r = 0; r < b.section_count; ++r)
            if (strncmp(a.sections[s].name, b.sections[r].name, MUS_SECTION_NAME_SIZE) == 0) {
                found = true;
                if (oracle_index(ia, a.sections[s].code_offset) != oracle_index(ib, b.sections[r].code_offset))
                    fprintf(stderr, "  section %s enters elsewhere\n", a.sections[s].name);
                CHECK(oracle_index(ia, a.sections[s].code_offset) == oracle_index(ib, b.sections[r].code_offset),
                      "each section enters at the same instruction");
            }
        CHECK(found, "each section is the recompile's too");
    }
    return 1;
}

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
        CHECK(same_program(*orig, recomp), "the recompiled program is the original's");
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
   decompile again to the same text, the same program. jox01's MJox01.bin plays 61 sounds past index
   255 through the word-wide play [orig: AudioVM_Op_PlayWait @ 0x672C90], which the compiler once
   refused. The mount must be the expansion's own (a mount falls back to the base game silently, where
   no `<exp>` program is found); an install without expansions, or one whose expansions carry no
   program, is a skipped leg; one that ships jox01 must read both of jox01's. */
static void expansion_programs(void) {
    const std::string root = retail::install();
    const std::vector<std::string> expansions = retail::expansions();
    int programs = 0;
    for (const std::string &expansion : expansions) {
        opennova::Vfs vfs;
        printf("Mounting /exp %s... ", expansion.c_str());
        const bool mounted = vfs.mount_game(root, expansion, opennova::VfsMountMode::Packed) &&
                             vfs.mounted_expansion() == expansion;
        printf(mounted ? "PASS\n" : "FAIL\n");
        if (!mounted) { ++failed; continue; }
        ++passed;
        int read = 0;
        for (const char *prefix : {"G", "M"}) {
            const std::string name = prefix + expansion + ".bin";
            std::vector<uint8_t> bytes;
            if (!vfs.read_file(name, bytes)) continue; /* an expansion without its own program plays the base's */
            ++read;
            MusFile mf;
            printf("Running %s/%s... ", expansion.c_str(), name.c_str());
            const int ok = mus_open_memory(&mf, bytes.data(), bytes.size()) == 0 && entry_roundtrip_file(mf, true);
            printf(ok ? "PASS\n" : "FAIL\n");
            if (ok) ++passed;
            else ++failed;
        }
        if (expansion == "jox01" && read != 2) {
            fprintf(stderr, "  FAIL: jox01 ships GJox01.bin and MJox01.bin; %d read\n", read);
            ++failed;
        }
        programs += read;
    }
    if (programs == 0) retail::skip_leg("an installed expansion with a music program of its own (OPENNOVA_JO_DIR)");
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
