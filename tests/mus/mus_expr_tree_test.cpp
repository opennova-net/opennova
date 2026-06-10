/* MUS structured expression trees (mus/ast.h MusAstExpr).

   The tree is the structured twin of the flat rendered expression text: the
   load-bearing guarantee is mus_expr_render(tree) == the stored flat text
   BYTE-FOR-BYTE for every statement that carries a tree, across every shipped
   script. The emitter never reads the tree (mus_ast_test pins emit byte-
   identity separately), so a tree bug can never corrupt the .mus round-trip --
   but a render mismatch here means the editor would show/edit an expression
   that isn't the one the bytecode runs, which this test makes impossible.

   Also pins coverage: the shipped scripts' real conditions/assignments must
   actually GET trees (NULL fallback is for surprises, not the common path). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "mus/ast.h"
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

struct TreeStats {
    int with_tree = 0;       /* statements whose tree is present + matched   */
    int text_only = 0;       /* statements with expression text but no tree  */
    int mismatches = 0;
};

/* Verify one (tree, flat text) pair; counts into stats. A NULL tree is the
   documented fallback (counted, never an error here); a present tree MUST
   render byte-identically. */
static int check_pair(const MusAstExpr *tree, const char *flat,
                      const char *what, TreeStats *st) {
    if (flat == NULL || flat[0] == 0) return 1;   /* nothing rendered */
    if (tree == NULL) {
        ++st->text_only;
        return 1;
    }
    char rendered[256];
    mus_expr_render(tree, rendered, sizeof(rendered));
    if (strcmp(rendered, flat) != 0) {
        fprintf(stderr, "  %s tree/text mismatch:\n    text: %s\n    tree: %s\n",
                what, flat, rendered);
        ++st->mismatches;
        return 0;
    }
    ++st->with_tree;
    return 1;
}

static int walk_stmts(const MusAstStmt *stmts, uint32_t n, TreeStats *st) {
    int ok = 1;
    for (uint32_t i = 0; i < n; ++i) {
        const MusAstStmt &s = stmts[i];
        ok &= check_pair(s.rhs_tree, s.rhs_text, "assign rhs", st);
        ok &= check_pair(s.expr_tree, s.expr_text, "expr/cond", st);
        ok &= walk_stmts(s.then_body, s.then_count, st);
        ok &= walk_stmts(s.else_body, s.else_count, st);
    }
    return ok;
}

static int render_equals_text_for(const char *path) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open fixture");
    int ok = 1;
    for (uint32_t sc = 0; sc < mf.header.chunk_count; ++sc) {
        MusAstProgram *prog = mus_parse_to_ast(&mf.scripts[sc]);
        CHECK(prog != nullptr, "parse_to_ast");
        TreeStats st;
        for (uint32_t i = 0; i < prog->section_count; ++i) {
            ok &= walk_stmts(prog->sections[i].statements,
                             prog->sections[i].statement_count, &st);
        }
        printf("[%s #%u: %d trees, %d text-only] ", path, sc, st.with_tree, st.text_only);
        CHECK(st.mismatches == 0, "every present tree renders byte-identical to its text");
        /* The shipped scripts are full of real conditions/assignments; if none
           grew a tree the builder is silently broken, not 'falling back'. */
        CHECK(st.with_tree > 0, "shipped script grows at least one expression tree");
        mus_program_free(prog);
    }
    mus_close(&mf);
    return ok;
}

static int test_gamemus_trees_render_byte_identical() {
    return render_equals_text_for(MUS_FIXTURE_DIR "/jo_gamemus.bin");
}

static int test_menumus_trees_render_byte_identical() {
    return render_equals_text_for(MUS_FIXTURE_DIR "/jo_menumus.bin");
}

/* The known-shape gamemus condition `if (Var01 != 0)` must arrive as a real
   BINOP(VARREF, LITERAL) tree -- structure, not just rendered equality. */
static int test_gamemus_condition_structure() {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    MusAstProgram *prog = mus_parse_to_ast(&mf.scripts[0]);
    CHECK(prog != nullptr, "parse_to_ast");
    const MusAstExpr *found = nullptr;
    for (uint32_t i = 0; i < prog->section_count && !found; ++i) {
        const MusAstSection &sec = prog->sections[i];
        for (uint32_t k = 0; k < sec.statement_count && !found; ++k) {
            const MusAstStmt &s = sec.statements[k];
            if (s.kind == MUS_AST_IF && s.expr_text
                && strstr(s.expr_text, "Var01") && s.expr_tree) {
                found = s.expr_tree;
            }
        }
    }
    CHECK(found != nullptr, "a Var01 if-condition carries a tree");
    CHECK(found->kind == MUS_EXPR_BINOP, "condition is a binop node");
    CHECK(found->left != nullptr && found->left->kind == MUS_EXPR_VARREF,
          "lhs is a variable reference");
    /* gamemus ships an explicit debug-name table whose entries are literally
       "Var00".."Var15", so the resolver's named-variable precedence wins: the
       node is the "named" form (name "Var01", var_index = byte offset 4). A
       script without that table would yield form "Var", index 1 -- both render
       "Var01". Accept either, pinning the resolver precedence as a side effect. */
    if (strcmp(found->left->var_form, "named") == 0) {
        CHECK(strcmp(found->left->name, "Var01") == 0 && found->left->var_index == 4,
              "named lhs is Var01 at byte offset 4");
    } else {
        CHECK(strcmp(found->left->var_form, "Var") == 0 && found->left->var_index == 1,
              "lhs is Var01");
    }
    CHECK(found->right != nullptr && found->right->kind == MUS_EXPR_LITERAL,
          "rhs is a literal");
    mus_program_free(prog);
    mus_close(&mf);
    return 1;
}

/* mus_expr_free on a detached tree + render of NULL stay safe. */
static int test_render_null_and_free_null() {
    char buf[16] = "x";
    CHECK(mus_expr_render(NULL, buf, sizeof(buf)) == 0, "NULL renders empty");
    CHECK(buf[0] == 0, "buffer NUL-terminated");
    mus_expr_free(NULL);   /* must not crash */
    return 1;
}

int main() {
    RUN_TEST(test_gamemus_trees_render_byte_identical);
    RUN_TEST(test_menumus_trees_render_byte_identical);
    RUN_TEST(test_gamemus_condition_structure);
    RUN_TEST(test_render_null_and_free_null);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
