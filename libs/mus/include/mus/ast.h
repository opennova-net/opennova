#ifndef MUS_AST_H
#define MUS_AST_H

/* MUS structured program model (editor-facing, read + author).

   Where mus_model.cpp captures only the section TOPOLOGY (edges + plays) and
   mus_decompile.cpp renders the bytecode as flat .mus TEXT, this builds the full
   STATEMENT TREE the editor needs to make MUS a visual-first language: every
   play, transition, assignment, inc/dec, method/intrinsic call, if/else, and
   on-switch, in source order, per section, each carrying its originating
   bytecode offset (so the running VM's pc can highlight the live statement) and
   its rendered text (so the tree re-emits byte-for-byte the same .mus the
   decompiler would, which then compiles to bytecode the original VM executes).

   Single source of truth shared with the decompiler: the rendering helpers
   (mus_decompile_shared.h) are the same, so the AST's emitted text is pinned
   byte-identical to mus_decompile() by mus_ast_test.cpp. mus_build_section_model
   is a strict subset of what this captures.

   Read path: mus_parse_to_ast (bytecode -> tree). Emit path: mus_ast_emit_text
   (tree -> .mus text -> mus_compile). The round-trip
   compile(emit(parse(bytecode))) is byte-identical to compile(decompile(bytecode))
   for the shipped scripts -- the canonical form the Tier-4 differential proved
   the original VM runs faithfully. */

#include "mus/mus.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Statement kinds. The text renders are exactly what mus_decompile.cpp emits. */
typedef enum MusAstStmtKind {
    MUS_AST_PLAY = 0,        /* play / playw  -> "play sound_2"               */
    MUS_AST_TRANSITION,      /* enter (setstate 0x3B) -> "enter Missionnull"  */
    MUS_AST_GOTO,            /* goto -> "goto Label"                          */
    MUS_AST_CALL,            /* callv 0x34 / callvl 0x33 -> "call Target"     */
    MUS_AST_RETURN,          /* return                                        */
    MUS_AST_YIELD,           /* yield                                         */
    MUS_AST_NOP,             /* nop (emits no text; visible to the editor)    */
    MUS_AST_DONE,            /* done 0x3F -> "}" (section terminator)         */
    MUS_AST_ASSIGN,          /* var = expr (pop_g/pop_l) -> "Var07 = (...)"   */
    MUS_AST_INCDEC,          /* var++ / var-- (inc_g/dec_g/inc_l/dec_l)       */
    MUS_AST_EXPR,            /* expression statement (empty 0x0F sink): "SV(200)" */
    MUS_AST_IF,              /* if (cond) {then} [else {else}]                */
    MUS_AST_SWITCH,          /* on (sel) action t1 t2 ... (tablexec 0x35)     */
    MUS_AST_BRANCH_COMMENT,  /* undetected brfalse/brtrue -> "// if ... goto" */
} MusAstStmtKind;

/* One target of an on-switch (tablexec). */
typedef struct MusAstSwitchTarget {
    char name[MUS_SECTION_NAME_SIZE];  /* rendered token (section / play / label) */
    int  section_index;                /* dest section index, or -1 (play/label)  */
    int  track_index;                  /* SBF index when action is play, else -1  */
} MusAstSwitchTarget;

typedef struct MusAstStmt MusAstStmt;
struct MusAstStmt {
    int      kind;          /* MusAstStmtKind */
    uint32_t code_offset;   /* bytecode offset of the originating opcode (pc map) */
    uint32_t byte_size;     /* total encoded size of the originating opcode(s)    */

    /* Fully-rendered line for this statement (no indentation, no newline). For
       compound statements (IF) it is the header expression only; SWITCH stores
       the whole "on (...) action targets" line. DONE/NOP carry their token but
       the emitter special-cases them. Always non-NULL (may be ""). */
    char    *text;

    /* --- structured fields (editor semantics; -1 / 0 / NULL when N/A) --- */
    int      track_index;     /* PLAY: SBF entry index                          */
    int      wait;            /* PLAY: 1 for playw (0x3D)                        */
    int      target_section;  /* TRANSITION/GOTO/CALL: dest section index, or -1 */
    char    *var_name;        /* ASSIGN/INCDEC: target variable display name     */
    int      var_offset;      /* ASSIGN/INCDEC global byte offset, or local idx  */
    int      is_local;        /* ASSIGN/INCDEC: 1 if a local (l_N)               */
    int      is_inc;          /* INCDEC: 1 for ++, 0 for --                      */
    char    *rhs_text;        /* ASSIGN: rendered RHS expression                 */
    char    *expr_text;       /* EXPR/IF/SWITCH: the (condition) expression      */
    int      has_call;        /* EXPR/ASSIGN: expression contains a method call  */
    char    *call_name;       /* EXPR/ASSIGN: combined intrinsic name (first), or NULL */

    /* --- IF children --- */
    MusAstStmt *then_body;
    uint32_t    then_count;
    MusAstStmt *else_body;    /* NULL / else_count 0 => plain if (no else)       */
    uint32_t    else_count;

    /* --- SWITCH --- */
    int                 switch_action;  /* inner opcode: 0x3B enter / 0x3E|0x3D play / 0x30 goto */
    MusAstSwitchTarget *targets;
    uint32_t            target_count;
};

typedef struct MusAstSection {
    char        name[MUS_SECTION_NAME_SIZE];
    int         section_index;  /* index into script->sections */
    int         is_entry;       /* == script->entry_section_index */
    uint32_t    code_offset;    /* bytecode-relative entry pc */
    MusAstStmt *statements;     /* owned statements, in source (offset) order */
    uint32_t    statement_count;
} MusAstSection;

typedef struct MusAstProgram {
    char           name[MUS_NAME_SIZE];
    char           source_path[MUS_SOURCE_PATH_SIZE];

    /* Header data needed to re-emit a compilable .mus file. */
    int            max_play_index;       /* highest play track index, or -1 */
    int           *globals_used;         /* sorted ascending byte offsets */
    uint32_t       globals_used_count;
    uint32_t       globals_size;

    char           intrinsic_names[MUS_INTRINSIC_NAMES][MUS_INTRINSIC_NAME_SIZE];
    uint32_t       intrinsic_count;

    /* Named globals (editor debug table), copied from the script so the emitter
       can resolve user-global declarations without the source MusScript. */
    MusVariable   *variables;
    uint32_t       variable_count;

    /* Sections in script->sections index order (declsection / map order). The
       emitter walks them in code_offset order, which reproduces the decompiler's
       linear section emission. */
    MusAstSection *sections;
    uint32_t       section_count;
    int            entry_section_index;
} MusAstProgram;

/* Build the structured program from a parsed MusScript. Allocates a tree owned
   by the caller; release with mus_program_free. Returns NULL on NULL input/OOM.
   The caller retains ownership of `script`. */
MusAstProgram *mus_parse_to_ast(const MusScript *script);

/* Free a tree returned by mus_parse_to_ast. Idempotent on NULL. */
void mus_program_free(MusAstProgram *program);

/* Emit .mus source text from the tree. Two-pass query/write contract identical
   to mus_decompile: pass out=NULL, out_capacity=0 to size, then a buffer at
   least that large to write. Returns bytes written (excluding NUL) or negative.

   When sbf_names is non-NULL, play targets render as SBF entry names (quoted if
   not bare identifiers) instead of the "sound_N" placeholder -- the names-aware
   form, recompilable via the bind-aware compiler. Pass NULL for the names-less
   form, which is byte-identical to mus_decompile(). */
int mus_ast_emit_text(const MusAstProgram *program,
                      const char *const *sbf_names, uint32_t sbf_name_count,
                      char *out_text, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* MUS_AST_H */
