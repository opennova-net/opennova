/* MUS structured AST (mus/ast.h) tests.

   The AST is the structured twin of the text decompiler. Its load-bearing
   guarantee: mus_ast_emit_text(mus_parse_to_ast(S)) is BYTE-IDENTICAL to
   mus_decompile(S) (and the names-aware emit to mus_decompile_with_names) for
   the shipped scripts, so the editor's structured surfaces re-emit the exact
   .mus the decompiler would and therefore compile to bytecode the original VM
   runs faithfully (compat bar: the VM understands our bytecode).

   Beyond byte-identity these tests assert the tree actually decomposes the
   program: every statement kind appears with the right code offset, if/else
   nest, the on-switch fans out, and the AST's plays match mus_build_section_model. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "mus/mus.h"
#include "mus/ast.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

// --- helpers ---

static std::string decompile_str(const MusScript *s) {
    int n = mus_decompile(s, nullptr, 0);
    if (n < 0) return "";
    std::string t; t.resize((size_t)n + 1);
    mus_decompile(s, &t[0], (size_t)n + 1);
    t.resize((size_t)n);
    return t;
}

static std::string decompile_named_str(const MusScript *s,
                                        const char *const *names, uint32_t count) {
    int n = mus_decompile_with_names(s, names, count, nullptr, 0);
    if (n < 0) return "";
    std::string t; t.resize((size_t)n + 1);
    mus_decompile_with_names(s, names, count, &t[0], (size_t)n + 1);
    t.resize((size_t)n);
    return t;
}

static std::string emit_str(const MusAstProgram *p,
                            const char *const *names, uint32_t count) {
    int n = mus_ast_emit_text(p, names, count, nullptr, 0);
    if (n < 0) return "";
    std::string t; t.resize((size_t)n + 1);
    mus_ast_emit_text(p, names, count, &t[0], (size_t)n + 1);
    t.resize((size_t)n);
    return t;
}

static bool compile_str(const std::string &text, MusScript *out) {
    const char *err = nullptr;
    int el = 0, ec = 0;
    int rc = mus_compile(text.c_str(), out, &el, &ec, &err);
    if (rc != 0)
        fprintf(stderr, "  compile error %d:%d %s\n", el, ec, err ? err : "(null)");
    return rc == 0;
}

static bool encode_bytes(const MusScript *s, std::string &out) {
    const MusScript *arr[1] = { s };
    uint8_t *buf = nullptr; size_t n = 0;
    if (mus_encode_file(arr, 1, &buf, &n) != 0) return false;
    out.assign((const char *)buf, n);
    mus_free(buf);
    return true;
}

// Synthetic SBF names with non-bare entries to exercise quoting. Stable across
// the run; index N -> a deterministic name.
struct FakeNames {
    std::vector<std::string> store;
    std::vector<const char *> ptrs;
    FakeNames(int n) {
        for (int i = 0; i < n; ++i) {
            if (i == 0) store.push_back("Main Theme");      // has a space
            else if (i == 1) store.push_back("AMB.LOOP");    // has a dot
            else { char b[32]; snprintf(b, sizeof(b), "trk_%d", i); store.push_back(b); }
        }
        for (auto &s : store) ptrs.push_back(s.c_str());
    }
    const char *const *data() const { return ptrs.data(); }
    uint32_t count() const { return (uint32_t)ptrs.size(); }
};

// Recursively count PLAY statements + play-switch targets in a statement array
// (the AST analog of mus_build_section_model's per-section play_count).
static uint32_t count_plays(const MusAstStmt *stmts, uint32_t n) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const MusAstStmt &s = stmts[i];
        if (s.kind == MUS_AST_PLAY) ++total;
        if (s.kind == MUS_AST_SWITCH
            && (s.switch_action == 0x3D || s.switch_action == 0x3E))
            total += s.target_count;
        total += count_plays(s.then_body, s.then_count);
        total += count_plays(s.else_body, s.else_count);
    }
    return total;
}

// --- tests ---

static int byte_identity_for(const char *path) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    const MusScript *s = &mf.scripts[0];

    MusAstProgram *prog = mus_parse_to_ast(s);
    CHECK(prog != nullptr, "parse_to_ast");

    // Names-less: emit must equal mus_decompile byte-for-byte.
    std::string dec = decompile_str(s);
    std::string emi = emit_str(prog, nullptr, 0);
    if (dec != emi) {
        fprintf(stderr, "  names-less emit != decompile for %s\n", path);
        // Show first divergence.
        size_t k = 0;
        while (k < dec.size() && k < emi.size() && dec[k] == emi[k]) ++k;
        fprintf(stderr, "  first diff at byte %zu\n", k);
        fprintf(stderr, "  dec: ...%.60s\n", dec.c_str() + (k > 30 ? k - 30 : 0));
        fprintf(stderr, "  emi: ...%.60s\n", emi.c_str() + (k > 30 ? k - 30 : 0));
        CHECK(false, "names-less byte identity");
    }

    // Names-aware: emit(names) must equal mus_decompile_with_names byte-for-byte.
    FakeNames fn(64);
    std::string decn = decompile_named_str(s, fn.data(), fn.count());
    std::string emin = emit_str(prog, fn.data(), fn.count());
    CHECK(decn == emin, "names-aware byte identity");

    mus_program_free(prog);
    mus_close(&mf);
    return 1;
}

static int test_byte_identity_gamemus() {
    return byte_identity_for(MUS_FIXTURE_DIR "/jo_gamemus.bin");
}
static int test_byte_identity_menumus() {
    return byte_identity_for(MUS_FIXTURE_DIR "/jo_menumus.bin");
}

// compile(emit(parse(S))) == compile(decompile(S)) -- canonical bytes the VM runs.
static int compile_equiv_for(const char *path) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    const MusScript *s = &mf.scripts[0];

    MusAstProgram *prog = mus_parse_to_ast(s);
    CHECK(prog != nullptr, "parse");

    std::string via_dec = decompile_str(s);
    std::string via_ast = emit_str(prog, nullptr, 0);

    MusScript sd = {}, sa = {};
    CHECK(compile_str(via_dec, &sd), "compile decompiled");
    CHECK(compile_str(via_ast, &sa), "compile ast-emitted");

    std::string bd, ba;
    CHECK(encode_bytes(&sd, bd), "encode decompiled");
    CHECK(encode_bytes(&sa, ba), "encode ast-emitted");
    CHECK(bd.size() == ba.size() && memcmp(bd.data(), ba.data(), bd.size()) == 0,
          "AST-emitted and decompiled compile to identical canonical bytes");

    // The names-aware emit must also compile (bind-aware compiler) and to the
    // SAME bytecode (names are aesthetic; play indices are unchanged).
    FakeNames fn(64);
    std::string via_ast_named = emit_str(prog, fn.data(), fn.count());
    MusScript san = {};
    CHECK(compile_str(via_ast_named, &san), "compile names-aware ast emit");
    std::string ban;
    CHECK(encode_bytes(&san, ban), "encode names-aware");
    CHECK(ban.size() == ba.size() && memcmp(ban.data(), ba.data(), ba.size()) == 0,
          "names-aware emit compiles to identical bytes");

    mus_script_free(&sd); mus_script_free(&sa); mus_script_free(&san);
    mus_program_free(prog);
    mus_close(&mf);
    return 1;
}

static int test_compile_equiv_gamemus() {
    return compile_equiv_for(MUS_FIXTURE_DIR "/jo_gamemus.bin");
}
static int test_compile_equiv_menumus() {
    return compile_equiv_for(MUS_FIXTURE_DIR "/jo_menumus.bin");
}

static const MusAstSection *section_named(const MusAstProgram *p, const char *name) {
    for (uint32_t i = 0; i < p->section_count; ++i)
        if (strcmp(p->sections[i].name, name) == 0) return &p->sections[i];
    return nullptr;
}

static const MusAstStmt *first_of_kind(const MusAstStmt *a, uint32_t n, int kind) {
    for (uint32_t i = 0; i < n; ++i) if (a[i].kind == kind) return &a[i];
    return nullptr;
}

// The tree must actually decompose gamescript: distinct statement kinds, offsets
// in ascending order, the if/else nest, and the on-switch fan-out.
static int test_statement_tree_gamemus() {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    MusAstProgram *p = mus_parse_to_ast(&mf.scripts[0]);
    CHECK(p != nullptr, "parse");
    CHECK(p->section_count == mf.scripts[0].section_count, "section count matches");

    // Begin: SV(200) expr-with-call, enter Testmission, done, leaked enter/FB/on.
    const MusAstSection *begin = section_named(p, "Begin");
    CHECK(begin != nullptr, "Begin present");
    CHECK(begin->is_entry, "Begin is entry");
    const MusAstStmt *expr = first_of_kind(begin->statements, begin->statement_count, MUS_AST_EXPR);
    CHECK(expr != nullptr, "Begin has an expression statement");
    CHECK(expr->has_call && expr->call_name && strcmp(expr->call_name, "GSV") == 0,
          "Begin's first expr is the GSV intrinsic call");
    CHECK(strcmp(expr->text, "SV(200)") == 0, "rendered SV(200)");
    const MusAstStmt *sw = first_of_kind(begin->statements, begin->statement_count, MUS_AST_SWITCH);
    CHECK(sw != nullptr, "Begin owns the on-switch (leaked past done)");
    CHECK(sw->switch_action == 0x3B, "switch action is enter (setstate)");
    CHECK(sw->target_count == 3, "switch fans to 3 targets");
    CHECK(first_of_kind(begin->statements, begin->statement_count, MUS_AST_DONE) != nullptr,
          "Begin has a done statement");
    const MusAstStmt *tr = first_of_kind(begin->statements, begin->statement_count, MUS_AST_TRANSITION);
    CHECK(tr != nullptr && tr->target_section >= 0, "Begin has a transition with a target section");

    // The leaked tail's `enter` (opcode 0x38) is FRAME SETUP, not a transition: a
    // distinct FRAME_ENTER kind with NO navigable target, so the editor never
    // treats it as a state change (editing it would corrupt the frame). Its text
    // stays "enter <name>" for byte-identity with the decompiler; the kind/target
    // distinguish it. [orig: AudioVM_Op_Enter @0x672C20 vs setstate 0x3B @0x672C70.]
    const MusAstStmt *fe = first_of_kind(begin->statements, begin->statement_count, MUS_AST_FRAME_ENTER);
    CHECK(fe != nullptr, "Begin's leaked enter (0x38) is FRAME_ENTER, not a transition");
    CHECK(fe->target_section == -1, "frame_enter carries no transition target");
    CHECK(strncmp(fe->text, "enter ", 6) == 0, "frame_enter text stays 'enter <name>' (byte-identity)");

    // statements are in ascending code offset order.
    for (uint32_t i = 1; i < begin->statement_count; ++i)
        CHECK(begin->statements[i].code_offset >= begin->statements[i - 1].code_offset,
              "statements ascend by code offset");

    // Testmission: an if/else whose branches are transitions.
    const MusAstSection *tm = section_named(p, "Testmission");
    CHECK(tm != nullptr, "Testmission present");
    const MusAstStmt *iff = first_of_kind(tm->statements, tm->statement_count, MUS_AST_IF);
    CHECK(iff != nullptr, "Testmission has an if");
    CHECK(iff->expr_text && strstr(iff->expr_text, "Var01") && strstr(iff->expr_text, "!="),
          "if condition mentions Var01 !=");
    CHECK(iff->then_count >= 1 && iff->then_body[0].kind == MUS_AST_TRANSITION,
          "if-then is a transition");
    CHECK(iff->else_body != nullptr && iff->else_count >= 1
          && iff->else_body[0].kind == MUS_AST_TRANSITION,
          "if-else is a transition");
    // The IF node's byte span must cover its whole block, so a live VM pc inside
    // the then/else body maps to this IF via [code_offset, code_offset+byte_size).
    CHECK(iff->code_offset + iff->byte_size > iff->else_body[0].code_offset,
          "IF byte_size spans into its else branch");

    // Win000 plays 6 tracks (sound_2..sound_7) then transitions.
    const MusAstSection *win = section_named(p, "Win000");
    CHECK(win != nullptr, "Win000 present");
    CHECK(count_plays(win->statements, win->statement_count) == 6, "Win000 has 6 plays");
    const MusAstStmt *p0 = first_of_kind(win->statements, win->statement_count, MUS_AST_PLAY);
    CHECK(p0 && p0->track_index == 2, "Win000 first play is track 2");

    mus_program_free(p);
    mus_close(&mf);
    return 1;
}

// The AST's plays must match mus_build_section_model per section (the model is
// a strict subset of the AST; this binds them so neither can drift).
static int model_crosscheck_for(const char *path) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    const MusScript *s = &mf.scripts[0];

    MusAstProgram *p = mus_parse_to_ast(s);
    CHECK(p != nullptr, "parse");
    MusModel m;
    CHECK(mus_build_section_model(s, &m) == 0, "build model");
    CHECK(m.section_count == p->section_count, "section counts agree");

    for (uint32_t i = 0; i < p->section_count; ++i) {
        const MusAstSection &sec = p->sections[i];
        // model section info for this index
        const MusSectionInfo *si = nullptr;
        for (uint32_t k = 0; k < m.section_count; ++k)
            if ((int)m.sections[k].section_index == sec.section_index) { si = &m.sections[k]; break; }
        CHECK(si != nullptr, "model has this section");
        uint32_t ast_plays = count_plays(sec.statements, sec.statement_count);
        if (ast_plays != si->play_count) {
            fprintf(stderr, "  section %s: ast plays %u != model %u\n",
                    sec.name, ast_plays, si->play_count);
            CHECK(false, "AST play count matches model");
        }
    }
    mus_model_free(&m);
    mus_program_free(p);
    mus_close(&mf);
    return 1;
}

static int test_model_crosscheck_gamemus() {
    return model_crosscheck_for(MUS_FIXTURE_DIR "/jo_gamemus.bin");
}
static int test_model_crosscheck_menumus() {
    return model_crosscheck_for(MUS_FIXTURE_DIR "/jo_menumus.bin");
}

static int test_null_safe() {
    CHECK(mus_parse_to_ast(nullptr) == nullptr, "null script -> null program");
    mus_program_free(nullptr);   // no crash
    char buf[8];
    CHECK(mus_ast_emit_text(nullptr, nullptr, 0, buf, sizeof(buf)) < 0, "null program rejected");
    return 1;
}

int main() {
    RUN_TEST(test_byte_identity_gamemus);
    RUN_TEST(test_byte_identity_menumus);
    RUN_TEST(test_compile_equiv_gamemus);
    RUN_TEST(test_compile_equiv_menumus);
    RUN_TEST(test_statement_tree_gamemus);
    RUN_TEST(test_model_crosscheck_gamemus);
    RUN_TEST(test_model_crosscheck_menumus);
    RUN_TEST(test_null_safe);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
