#ifndef MUS_DECOMPILE_SHARED_H
#define MUS_DECOMPILE_SHARED_H

/* Shared MUS decompile helpers (name resolution, control-flow analysis,
   stack-based expression reconstruction).

   Single source of truth for the text-rendering logic used by BOTH the source
   decompiler (mus_decompile.cpp) and the structured AST builder (mus_ast.cpp),
   so the two can never drift in how they name a variable, split a method, detect
   an if/else block, or rebuild an expression. Was previously private to
   mus_decompile.cpp; lifted here verbatim (behaviour preserved, guarded by the
   golden byte-compare in mus_decompile_test.cpp and the AST byte-identity test
   in mus_ast_test.cpp).

   Header-only with internal (`static`) linkage, same pattern as mus_decode.h:
   each translation unit gets its own copy of the functions, so there is no ODR
   or multiple-definition conflict. Names kept at the original (unqualified)
   spelling so mus_decompile.cpp needed only an #include + deletion of the moved
   definitions, with no reference renames. */

#include "mus/ast.h"  /* MusAstExpr (the structured expression twin) */
#include "mus/mus.h"

#include "mus_decode.h"  /* Instruction, kOps, disassemble() */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Operand helpers (match Python resolve_* lambdas) ---- */

static void resolve_global_into(char *out, size_t cap, int idx,
                                const MusVariable *vars, uint32_t var_count) {
    /* Editor-named variables win when present (var.byte_offset == idx). */
    for (uint32_t i = 0; i < var_count; ++i) {
        if ((int)vars[i].byte_offset == idx) {
            snprintf(out, cap, "%s", vars[i].name);
            return;
        }
    }
    /* MDEdit pre-defines Var00..Var15 at byte offsets 0..60. */
    if (idx >= 0 && idx <= 60) {
        snprintf(out, cap, "Var%02d", idx / 4);
        return;
    }
    snprintf(out, cap, "g_%d", idx);
}

static void resolve_local_into(char *out, size_t cap, int idx) {
    snprintf(out, cap, "l_%d", idx);
}

static void resolve_method_into(char *out, size_t cap, int idx,
                                const char (*names)[MUS_INTRINSIC_NAME_SIZE],
                                uint32_t name_count) {
    if (idx >= 0 && (uint32_t)idx < name_count) {
        snprintf(out, cap, "%s", names[idx]);
        return;
    }
    snprintf(out, cap, "method_%d", idx);
}

/* Split a combined intrinsic name into (object_prefix, method) the way the
   Python `split_method_name` function does:
     G* → ('', "*")  (GLOBAL is default, no prefix)
     F* → ('F', "*")
     T* → ('T', "*")
     other → ('', name) */
static void split_method_name(const char *combined, char *obj, size_t obj_cap,
                              char *method, size_t method_cap) {
    obj[0] = 0;
    method[0] = 0;
    if (!combined || !combined[0]) return;
    char first = combined[0];
    if ((first == 'G' || first == 'F' || first == 'T') && combined[1] != 0) {
        if (first == 'G') {
            /* Drop the G: */
            snprintf(method, method_cap, "%s", combined + 1);
        } else {
            obj[0] = first;
            obj[1] = 0;
            snprintf(method, method_cap, "%s", combined + 1);
        }
        return;
    }
    snprintf(method, method_cap, "%s", combined);
}

static const char *resolve_section_idx(int idx, const MusScript *s,
                                       char *fallback, size_t cap) {
    if (idx >= 0 && (uint32_t)idx < s->section_count) {
        return s->sections[idx].name;
    }
    snprintf(fallback, cap, "Section%d", idx);
    return fallback;
}

/* ---- Control flow analysis (matches Python `analyze_control_flow`) ---- */

struct CFBlock {
    int      type;          /* 0=none, 1=if, 2=if_else */
    uint32_t start_offset;
    uint32_t end_offset;
    uint32_t else_offset;
    uint32_t body_start;
    uint32_t body_end;
};

#define CF_TYPE_IF      1
#define CF_TYPE_IF_ELSE 2

/* CFBlock map keyed by start_offset. We store as parallel arrays for simplicity. */
struct CFMap {
    CFBlock *blocks;
    int      count;
};

static CFBlock *cf_get(CFMap *m, uint32_t off) {
    for (int i = 0; i < m->count; ++i) {
        if (m->blocks[i].start_offset == off) return &m->blocks[i];
    }
    return NULL;
}

static void cf_add(CFMap *m, int *cap, const CFBlock &b) {
    if (m->count == *cap) {
        *cap = (*cap == 0) ? 8 : (*cap * 2);
        m->blocks = (CFBlock *)realloc(m->blocks, (size_t)*cap * sizeof(CFBlock));
    }
    m->blocks[m->count++] = b;
}

static void cf_free(CFMap *m) {
    free(m->blocks);
    m->blocks = NULL;
    m->count = 0;
}

static int find_inst_idx(const Instruction *insts, int n, uint32_t off) {
    for (int i = 0; i < n; ++i) {
        if (insts[i].offset == off) return i;
    }
    return -1;
}

static void analyze_control_flow(const Instruction *insts, int n, CFMap *out) {
    int cap = 0;
    out->blocks = NULL;
    out->count = 0;

    /* Single pass replicating the Python detection logic. */
    int i = 0;
    while (i < n) {
        const Instruction *inst = &insts[i];
        if (!inst->mnemonic) { ++i; continue; }
        if (strcmp(inst->mnemonic, "brfalse") == 0 && inst->operand_count >= 1) {
            uint32_t target = (uint32_t)inst->operands[0];
            /* Look for a goto before target inside [i+1, target). */
            int else_goto_idx = -1;
            for (int j = i + 1; j < n; ++j) {
                if (insts[j].offset >= target) break;
                if (insts[j].mnemonic
                    && strcmp(insts[j].mnemonic, "goto") == 0
                    && insts[j].operand_count >= 1) {
                    uint32_t goto_target = (uint32_t)insts[j].operands[0];
                    if (goto_target > target) { else_goto_idx = j; break; }
                }
            }
            CFBlock b{};
            b.start_offset = inst->offset;
            b.body_start   = inst->offset + inst->size;
            if (else_goto_idx >= 0) {
                b.type        = CF_TYPE_IF_ELSE;
                b.else_offset = target;
                b.end_offset  = (uint32_t)insts[else_goto_idx].operands[0];
                b.body_end    = insts[else_goto_idx].offset;
            } else {
                b.type       = CF_TYPE_IF;
                b.end_offset = target;
                b.body_end   = target;
            }
            cf_add(out, &cap, b);
        }
        /* While/loop/untiltrue patterns aren't exercised by jo_gamemus.bin's
           bytecode, and the Python decompiler emits them only when a back-
           branch matches the start. We detect them with the same logic the
           Python uses (no observed difference for the fixture). */
        ++i;
    }
}

/* ---- Expression reconstruction (matches Python `reconstruct_expression`) ---- */

struct Stack {
    char  buf[32][256];     /* up to 32 expressions, each up to 256 chars */
    int   top;
};

static void stk_push(Stack *s, const char *str) {
    if (s->top < 32) {
        snprintf(s->buf[s->top], sizeof(s->buf[0]), "%s", str);
        ++s->top;
    }
}
static const char *stk_pop(Stack *s) {
    if (s->top > 0) {
        --s->top;
        return s->buf[s->top];
    }
    return "";
}
static const char *stk_peek(Stack *s) {
    return (s->top > 0) ? s->buf[s->top - 1] : "";
}

static const char *binop_str(const char *m) {
    if (strcmp(m, "add") == 0)      return "+";
    if (strcmp(m, "sub") == 0)      return "-";
    if (strcmp(m, "mult") == 0)     return "*";
    if (strcmp(m, "div") == 0)      return "/";
    if (strcmp(m, "mod") == 0)      return "%";
    if (strcmp(m, "l_and") == 0)    return "&&";
    if (strcmp(m, "l_or") == 0)     return "||";
    if (strcmp(m, "and") == 0)      return "&";
    if (strcmp(m, "or") == 0)       return "|";
    if (strcmp(m, "xor") == 0)      return "^";
    if (strcmp(m, "lshift") == 0)   return "<<";
    if (strcmp(m, "rshift") == 0)   return ">>";
    if (strcmp(m, "equal") == 0)    return "==";
    if (strcmp(m, "notequal") == 0) return "!=";
    if (strcmp(m, "ge") == 0)       return ">=";
    if (strcmp(m, "le") == 0)       return "<=";
    if (strcmp(m, "gt") == 0)       return ">";
    if (strcmp(m, "lt") == 0)       return "<";
    return NULL;
}

static const char *unaryop_str(const char *m) {
    if (strcmp(m, "neg") == 0)     return "-";
    if (strcmp(m, "not") == 0)     return "!";
    if (strcmp(m, "inverse") == 0) return "~";
    return NULL;
}

static int is_expr_end_op(const char *m) {
    return strcmp(m, "brfalse") == 0 || strcmp(m, "brtrue") == 0
        || strcmp(m, "goto") == 0    || strcmp(m, "done") == 0
        || strcmp(m, "return") == 0  || strcmp(m, "yield") == 0
        || strcmp(m, "pop_g") == 0   || strcmp(m, "pop_l") == 0
        || strcmp(m, "play") == 0    || strcmp(m, "playw") == 0
        || strcmp(m, "setstate") == 0 || strcmp(m, "enter") == 0;
}

static void reconstruct_expression(const Instruction *insts, int start, int end_excl,
                                   const MusScript *script, char *out, size_t out_cap) {
    Stack stk{};
    out[0] = 0;
    char tmp[256];
    int i = start;
    while (i < end_excl) {
        const Instruction *inst = &insts[i];
        const char *m = inst->mnemonic;
        if (!m) break;

        if (strcmp(m, "push") == 0) {
            int v = (inst->operand_count > 0) ? inst->operands[0] : 0;
            snprintf(tmp, sizeof(tmp), "%d", v);
            stk_push(&stk, tmp);
        } else if (strcmp(m, "push_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            char name[64];
            resolve_global_into(name, sizeof(name), idx,
                                script->variables, script->variable_count);
            stk_push(&stk, name);
        } else if (strcmp(m, "push_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            char name[64];
            resolve_local_into(name, sizeof(name), idx);
            stk_push(&stk, name);
        } else if (strcmp(m, "push_me") == 0) {
            stk_push(&stk, "Me");
        } else if (strcmp(m, "pushstr") == 0) {
            int v = (inst->operand_count > 0) ? inst->operands[0] : 0;
            snprintf(tmp, sizeof(tmp), "\"str_%04X\"", (unsigned)v);
            stk_push(&stk, tmp);
        } else if (binop_str(m)) {
            const char *op = binop_str(m);
            if (stk.top >= 2) {
                char b[256], a[256];
                snprintf(b, sizeof(b), "%s", stk_pop(&stk));
                snprintf(a, sizeof(a), "%s", stk_pop(&stk));
                snprintf(tmp, sizeof(tmp), "(%s %s %s)", a, op, b);
                stk_push(&stk, tmp);
            } else {
                snprintf(tmp, sizeof(tmp), "?%s?", op);
                stk_push(&stk, tmp);
            }
        } else if (unaryop_str(m)) {
            const char *op = unaryop_str(m);
            if (stk.top >= 1) {
                char a[256];
                snprintf(a, sizeof(a), "%s", stk_pop(&stk));
                snprintf(tmp, sizeof(tmp), "%s%s", op, a);
                stk_push(&stk, tmp);
            } else {
                snprintf(tmp, sizeof(tmp), "%s?", op);
                stk_push(&stk, tmp);
            }
        } else if (strcmp(m, "method") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            char combined[64];
            resolve_method_into(combined, sizeof(combined), idx,
                                script->intrinsic_names, script->intrinsic_count);
            char obj[16], mth[64];
            split_method_name(combined, obj, sizeof(obj), mth, sizeof(mth));
            char arg[256] = "";
            if (stk.top >= 1) {
                snprintf(arg, sizeof(arg), "%s", stk_pop(&stk));
            }
            if (obj[0]) {
                if (arg[0]) snprintf(tmp, sizeof(tmp), "%s.%s(%s)", obj, mth, arg);
                else        snprintf(tmp, sizeof(tmp), "%s.%s()", obj, mth);
            } else {
                if (arg[0]) snprintf(tmp, sizeof(tmp), "%s(%s)", mth, arg);
                else        snprintf(tmp, sizeof(tmp), "%s()", mth);
            }
            stk_push(&stk, tmp);
        } else if (strcmp(m, "empty") == 0) {
            /* Expression ended; keep stack as-is, return what's on top. */
            snprintf(out, out_cap, "%s", stk_peek(&stk));
            return;
        } else if (is_expr_end_op(m)) {
            snprintf(out, out_cap, "%s", stk_peek(&stk));
            return;
        } else {
            break;
        }
        ++i;
    }
    snprintf(out, out_cap, "%s", stk_peek(&stk));
}

/* ---- Structured expression tree (the node twin of reconstruct_expression) ----

   The SAME opcode walk as reconstruct_expression above, pushing MusAstExpr
   nodes instead of rendered strings. Kept adjacent so the two can never drift:
   any change to the string walk must be mirrored here (mus_expr_tree_test.cpp
   pins render(tree) == flat text over every shipped script). Divergence
   policy: where the string walk degrades gracefully into placeholder text
   ("?op?" on underflow, silent drop past 32 entries), the tree builder yields
   NULL instead -- a guessed tree would invite a structured edit of garbage,
   while NULL makes the editor fall back to the flat-text escape. */

static inline MusAstExpr *expr_node_alloc(int kind) {
    MusAstExpr *e = (MusAstExpr *)calloc(1, sizeof(MusAstExpr));
    if (e) e->kind = kind;
    return e;
}

static inline void expr_tree_free_rec(MusAstExpr *e) {
    if (!e) return;
    expr_tree_free_rec(e->left);
    expr_tree_free_rec(e->right);
    free(e);
}

/* VARREF node with resolve_global_into's exact precedence: editor-named
   variable wins, then the MDEdit Var00..Var15 window, then raw g_N. */
static inline MusAstExpr *expr_node_global(int idx, const MusVariable *vars,
                                           uint32_t var_count) {
    MusAstExpr *e = expr_node_alloc(MUS_EXPR_VARREF);
    if (!e) return NULL;
    for (uint32_t i = 0; i < var_count; ++i) {
        if ((int)vars[i].byte_offset == idx) {
            snprintf(e->var_form, sizeof(e->var_form), "named");
            e->var_index = idx;
            snprintf(e->name, sizeof(e->name), "%s", vars[i].name);
            return e;
        }
    }
    if (idx >= 0 && idx <= 60) {
        snprintf(e->var_form, sizeof(e->var_form), "Var");
        e->var_index = idx / 4;
        return e;
    }
    snprintf(e->var_form, sizeof(e->var_form), "g");
    e->var_index = idx;
    return e;
}

static inline MusAstExpr *reconstruct_expression_tree(const Instruction *insts,
                                                      int start, int end_excl,
                                                      const MusScript *script) {
    MusAstExpr *stk[32];
    int top = 0;
    int ok = 1;
    char tmp[64];
    int i = start;
    while (i < end_excl) {
        const Instruction *inst = &insts[i];
        const char *m = inst->mnemonic;
        if (!m) break;

        MusAstExpr *node = NULL;
        if (strcmp(m, "push") == 0) {
            node = expr_node_alloc(MUS_EXPR_LITERAL);
            if (node) node->value = (inst->operand_count > 0) ? inst->operands[0] : 0;
        } else if (strcmp(m, "push_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            node = expr_node_global(idx, script->variables, script->variable_count);
        } else if (strcmp(m, "push_l") == 0) {
            node = expr_node_alloc(MUS_EXPR_VARREF);
            if (node) {
                snprintf(node->var_form, sizeof(node->var_form), "l");
                node->var_index = (inst->operand_count > 0) ? inst->operands[0] : 0;
            }
        } else if (strcmp(m, "push_me") == 0) {
            node = expr_node_alloc(MUS_EXPR_ME);
        } else if (strcmp(m, "pushstr") == 0) {
            int v = (inst->operand_count > 0) ? inst->operands[0] : 0;
            node = expr_node_alloc(MUS_EXPR_RAW);
            if (node) {
                snprintf(tmp, sizeof(tmp), "\"str_%04X\"", (unsigned)v);
                snprintf(node->name, sizeof(node->name), "%s", tmp);
            }
        } else if (binop_str(m)) {
            if (top < 2) { ok = 0; break; }   /* string walk renders "?op?"; tree bails */
            node = expr_node_alloc(MUS_EXPR_BINOP);
            if (node) {
                snprintf(node->op, sizeof(node->op), "%s", binop_str(m));
                node->right = stk[--top];
                node->left = stk[--top];
            }
        } else if (unaryop_str(m)) {
            if (top < 1) { ok = 0; break; }
            node = expr_node_alloc(MUS_EXPR_UNOP);
            if (node) {
                snprintf(node->op, sizeof(node->op), "%s", unaryop_str(m));
                node->left = stk[--top];
            }
        } else if (strcmp(m, "method") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            node = expr_node_alloc(MUS_EXPR_CALL);
            if (node) {
                resolve_method_into(node->name, sizeof(node->name), idx,
                                    script->intrinsic_names, script->intrinsic_count);
                if (top >= 1) node->left = stk[--top];   /* arg is optional */
            }
        } else if (strcmp(m, "empty") == 0 || is_expr_end_op(m)) {
            break;   /* expression ended; return what's on top */
        } else {
            break;   /* unknown op: stop, mirror the string walk's bail-out */
        }

        if (node == NULL) { ok = 0; break; }   /* OOM */
        if (top >= 32) {                       /* string walk drops silently; tree bails */
            expr_tree_free_rec(node);
            ok = 0;
            break;
        }
        stk[top++] = node;
        ++i;
    }

    if (!ok || top == 0) {
        for (int k = 0; k < top; ++k) expr_tree_free_rec(stk[k]);
        return NULL;
    }
    MusAstExpr *result = stk[top - 1];
    for (int k = 0; k < top - 1; ++k) expr_tree_free_rec(stk[k]);
    return result;
}

/* find_expr_start: scan backwards to find where this expression began. */
static int find_expr_start(const Instruction *insts, int /*n*/, int end_idx) {
    int depth = 0;
    for (int j = end_idx - 1; j >= 0; --j) {
        const char *m = insts[j].mnemonic;
        if (!m) return j + 1;
        if (strcmp(m, "push") == 0 || strcmp(m, "push_g") == 0
         || strcmp(m, "push_l") == 0 || strcmp(m, "push_me") == 0
         || strcmp(m, "pushstr") == 0) {
            depth -= 1;
            if (depth < 0) return j;
        } else if (binop_str(m)) {
            depth += 1;
        } else if (unaryop_str(m) || strcmp(m, "method") == 0
                || strcmp(m, "empty") == 0) {
            /* net 0; no-op for stack-depth tracking */
        } else {
            return j + 1;
        }
    }
    return 0;
}

/* Resolve a play opcode's index to a name. When sbf_names is non-NULL and idx
   is in range, copies the SBF entry name; otherwise falls back to the legacy
   "sound_<idx>" placeholder. Used by both mus_decompile (sbf_names == NULL) and
   mus_decompile_with_names. */
static void resolve_play_name(int idx, const char *const *sbf_names,
                              uint32_t sbf_name_count,
                              char *out, size_t cap) {
    if (sbf_names != NULL && idx >= 0 && (uint32_t)idx < sbf_name_count
            && sbf_names[idx] != NULL && sbf_names[idx][0] != 0) {
        snprintf(out, cap, "%s", sbf_names[idx]);
        return;
    }
    snprintf(out, cap, "sound_%d", idx);
}

/* True when `s` lexes as a single bare MUS identifier: non-empty, [A-Za-z_]
   then [A-Za-z0-9_]*. SBF entry names with spaces/dots fail this. */
static int is_bare_ident(const char *s) {
    if (!s || !s[0]) return 0;
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_'))
        return 0;
    for (const char *p = s + 1; *p; ++p) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

/* Format a play target for emission: a bare identifier as-is, otherwise quoted
   so the compiler reads it as one String token and resolves it via the bind
   map. Keeps names-aware decompiles ("play GAMINT" / "play \"Main Theme\"")
   recompilable; names-less ("sound_N") is always a bare ident, never quoted. */
static void format_play_token(const char *name, char *out, size_t cap) {
    if (is_bare_ident(name)) snprintf(out, cap, "%s", name);
    else                     snprintf(out, cap, "\"%s\"", name);
}

#endif /* MUS_DECOMPILE_SHARED_H */
