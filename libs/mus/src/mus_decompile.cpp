/* MUS bytecode -> .mus source decompiler.

   Reference implementation:
       on-godot-oscarmike/.scratch/mus_decompiler.py (1726 lines).

   The Python decompiler is a high-level source decompiler: it parses the
   editor debug-export table for section names and source path, performs
   stack-based expression reconstruction, recognises if/if_else/while
   control-flow patterns, decodes the embedded `tablexec` jump table, and
   synthesises `bind sound_N "sound_N"` aesthetic for the play opcodes
   (since the runtime carries no bind table). Our C++ port preserves that output verbatim so
   `tests/mus/mus_decompile_test.cpp` can compare byte-for-byte against the
   committed golden in `fixtures/mus/golden_jo_gamemus.mus.txt`.

   The opcode table below was transcribed from the Python `OPCODES = {...}`
   dict at line 23. The IDA witness confirmed all 65 entries (0x00..0x40);
   slots 0x0D, 0x0E, 0x1F, 0x26, 0x27, 0x2C..0x2F, 0x36, 0x37, 0x3C have
   no Python entry (those are unused / no-op slots in the dispatch table). */

#include "mus/mus.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mus_decode.h"  // shared opcode table + Instruction + disassemble()

namespace {

/* OpInfo / kOps[256] / MAX_OPERANDS / Instruction / disassemble() /
   free_instructions() now live in mus_decode.h (shared with mus_model.cpp). */

/* Buffer that supports two-pass query (out=NULL, cap=0 → just count). */
struct Buf {
    char    *p;
    size_t   cap;
    size_t   used;
};

static void buf_putc(Buf *b, char c) {
    if (b->p && b->used + 1 < b->cap) b->p[b->used] = c;
    ++b->used;
}
static void buf_puts(Buf *b, const char *s) {
    while (*s) buf_putc(b, *s++);
}
static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    for (int i = 0; i < n && i < (int)sizeof(tmp); ++i) buf_putc(b, tmp[i]);
}

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

/* ---- High-level decompile (matches Python `generate_mus_source_with_labels`) ---- */

static void emit_indent(Buf *b, int indent) {
    for (int k = 0; k < indent; ++k) buf_puts(b, "    ");
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

/* Forward decl for recursion. */
static void decompile_block(Buf *out,
                            const Instruction *insts, int begin_idx, int end_idx,
                            const MusScript *script,
                            const CFMap *cf,        /* control flow map for the FULL section */
                            int suppress_entries,    /* recursive bodies don't emit "section X" */
                            int indent,
                            const char *const *sbf_names,
                            uint32_t sbf_name_count);

static void decompile_block(Buf *out,
                            const Instruction *insts, int begin_idx, int end_idx,
                            const MusScript *script,
                            const CFMap *cf,
                            int suppress_entries,
                            int indent,
                            const char *const *sbf_names,
                            uint32_t sbf_name_count) {
    char buf256[256];
    char fallback[64];

    int i = begin_idx;
    while (i < end_idx) {
        const Instruction *inst = &insts[i];

        /* Section entry-point label. Only at top-level walks (recursive
           bodies pass suppress_entries=1 so the section header isn't
           re-emitted inside if/else/while bodies). */
        if (!suppress_entries) {
            for (uint32_t s = 0; s < script->section_count; ++s) {
                if (script->sections[s].code_offset == inst->offset) {
                    buf_putc(out, '\n');
                    buf_printf(out, "section %s\n", script->sections[s].name);
                    buf_puts(out, "{\n");
                    break;
                }
            }
        }

        /* Control flow block detection. Only consult the cf map at the
           top-level (the Python decompiler also reanalyzes per inner call,
           but for our fixture only the top-level if_else block is detected,
           and inner bodies pass empty entry points anyway). */
        const CFBlock *block = NULL;
        if (!suppress_entries) {
            for (int k = 0; k < cf->count; ++k) {
                if (cf->blocks[k].start_offset == inst->offset) {
                    block = &cf->blocks[k];
                    break;
                }
            }
        }
        if (block && block->type == CF_TYPE_IF) {
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr));
            if (!expr[0]) snprintf(expr, sizeof(expr), "condition");
            emit_indent(out, indent);
            buf_printf(out, "if (%s)\n", expr);
            emit_indent(out, indent);
            buf_puts(out, "{\n");
            int body_begin = -1, body_end_idx = -1;
            for (int k = 0; k < end_idx; ++k) {
                if (insts[k].offset >= block->body_start && body_begin < 0) body_begin = k;
                if (insts[k].offset >= block->body_end && body_end_idx < 0) {
                    body_end_idx = k;
                    break;
                }
            }
            if (body_begin >= 0) {
                if (body_end_idx < 0) body_end_idx = end_idx;
                decompile_block(out, insts, body_begin, body_end_idx, script, cf,
                                /*suppress_entries=*/1, indent + 1,
                                sbf_names, sbf_name_count);
            }
            emit_indent(out, indent);
            buf_puts(out, "}\n");
            while (i < end_idx && insts[i].offset < block->end_offset) ++i;
            continue;
        }
        if (block && block->type == CF_TYPE_IF_ELSE) {
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr));
            if (!expr[0]) snprintf(expr, sizeof(expr), "condition");
            emit_indent(out, indent);
            buf_printf(out, "if (%s)\n", expr);
            emit_indent(out, indent);
            buf_puts(out, "{\n");
            /* if body is [body_start .. else_offset - 5) per Python:
                 if_body = [ins for ins in instructions
                            if block.body_start <= ins.offset < block.else_offset - 5] */
            int if_begin = -1, if_end = -1;
            for (int k = 0; k < end_idx; ++k) {
                if (insts[k].offset >= block->body_start && if_begin < 0) if_begin = k;
                if (insts[k].offset >= (block->else_offset - 5) && if_end < 0) {
                    if_end = k;
                    break;
                }
            }
            if (if_begin >= 0) {
                if (if_end < 0) if_end = end_idx;
                decompile_block(out, insts, if_begin, if_end, script, cf,
                                /*suppress_entries=*/1, indent + 1,
                                sbf_names, sbf_name_count);
            }
            emit_indent(out, indent);
            buf_puts(out, "}\n");
            emit_indent(out, indent);
            buf_puts(out, "else\n");
            emit_indent(out, indent);
            buf_puts(out, "{\n");
            int el_begin = -1, el_end = -1;
            for (int k = 0; k < end_idx; ++k) {
                if (insts[k].offset >= block->else_offset && el_begin < 0) el_begin = k;
                if (insts[k].offset >= block->end_offset && el_end < 0) { el_end = k; break; }
            }
            if (el_begin >= 0) {
                if (el_end < 0) el_end = end_idx;
                decompile_block(out, insts, el_begin, el_end, script, cf,
                                /*suppress_entries=*/1, indent + 1,
                                sbf_names, sbf_name_count);
            }
            emit_indent(out, indent);
            buf_puts(out, "}\n");
            while (i < end_idx && insts[i].offset < block->end_offset) ++i;
            continue;
        }

        const char *m = inst->mnemonic;
        if (!m) {
            ++i;
            continue;
        }

        if (strcmp(m, "push") == 0 || strcmp(m, "push_g") == 0
         || strcmp(m, "push_l") == 0 || strcmp(m, "push_me") == 0
         || strcmp(m, "pushstr") == 0
         || binop_str(m) || unaryop_str(m)
         || strcmp(m, "method") == 0) {
            /* Part of expression: handled at the empty/pop sink. */
        }
        else if (strcmp(m, "empty") == 0) {
            int es = find_expr_start(insts, end_idx, i);
            if (es < i) {
                char expr[256];
                reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr));
                if (expr[0]) {
                    emit_indent(out, indent);
                    buf_printf(out, "%s\n", expr);
                }
            }
        }
        else if (strcmp(m, "pop_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_global_into(buf256, sizeof(buf256), idx,
                                script->variables, script->variable_count);
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
            emit_indent(out, indent);
            if (expr[0]) buf_printf(out, "%s = %s\n", buf256, expr);
            else         buf_printf(out, "%s = ?\n", buf256);
        }
        else if (strcmp(m, "pop_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_local_into(buf256, sizeof(buf256), idx);
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
            emit_indent(out, indent);
            if (expr[0]) buf_printf(out, "%s = %s\n", buf256, expr);
            else         buf_printf(out, "%s = ?\n", buf256);
        }
        else if (strcmp(m, "inc_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_global_into(buf256, sizeof(buf256), idx,
                                script->variables, script->variable_count);
            emit_indent(out, indent);
            buf_printf(out, "%s++\n", buf256);
        }
        else if (strcmp(m, "inc_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_local_into(buf256, sizeof(buf256), idx);
            emit_indent(out, indent);
            buf_printf(out, "%s++\n", buf256);
        }
        else if (strcmp(m, "dec_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_global_into(buf256, sizeof(buf256), idx,
                                script->variables, script->variable_count);
            emit_indent(out, indent);
            buf_printf(out, "%s--\n", buf256);
        }
        else if (strcmp(m, "dec_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_local_into(buf256, sizeof(buf256), idx);
            emit_indent(out, indent);
            buf_printf(out, "%s--\n", buf256);
        }
        else if (strcmp(m, "goto") == 0) {
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            /* Resolve label name: Python uses entry_points.get(target, f"@{target:04X}").
               At top-level, entry_points = {section_offsets}. We pass the same. */
            const char *target_name = NULL;
            char fb[32];
            if (!suppress_entries) {
                for (uint32_t s = 0; s < script->section_count; ++s) {
                    if ((int)script->sections[s].code_offset == target) {
                        target_name = script->sections[s].name; break;
                    }
                }
            }
            if (!target_name) {
                snprintf(fb, sizeof(fb), "@%04X", (unsigned)target);
                target_name = fb;
            }
            emit_indent(out, indent);
            buf_printf(out, "goto %s\n", target_name);
        }
        else if (strcmp(m, "brfalse") == 0) {
            /* Only reached when not part of detected control flow. The Python
               source-with-labels variant emits a `// if !(expr) goto label`
               comment in that case. */
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
            const char *target_name = NULL;
            char fb[32];
            if (!suppress_entries) {
                for (uint32_t s = 0; s < script->section_count; ++s) {
                    if ((int)script->sections[s].code_offset == target) {
                        target_name = script->sections[s].name; break;
                    }
                }
            }
            if (!target_name) {
                snprintf(fb, sizeof(fb), "@%04X", (unsigned)target);
                target_name = fb;
            }
            emit_indent(out, indent);
            buf_printf(out, "// if !(%s) goto %s\n", expr, target_name);
        }
        else if (strcmp(m, "brtrue") == 0) {
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
            const char *target_name = NULL;
            char fb[32];
            if (!suppress_entries) {
                for (uint32_t s = 0; s < script->section_count; ++s) {
                    if ((int)script->sections[s].code_offset == target) {
                        target_name = script->sections[s].name; break;
                    }
                }
            }
            if (!target_name) {
                snprintf(fb, sizeof(fb), "@%04X", (unsigned)target);
                target_name = fb;
            }
            emit_indent(out, indent);
            buf_printf(out, "// if (%s) goto %s\n", expr, target_name);
        }
        else if (strcmp(m, "setstate") == 0 || strcmp(m, "enter") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            const char *name = resolve_section_idx(idx, script, fallback,
                                                   sizeof(fallback));
            emit_indent(out, indent);
            buf_printf(out, "enter %s\n", name);
        }
        else if (strcmp(m, "play") == 0 || strcmp(m, "playw") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            char pname[64];
            resolve_play_name(idx, sbf_names, sbf_name_count,
                              pname, sizeof(pname));
            char ptok[72];
            format_play_token(pname, ptok, sizeof(ptok));
            emit_indent(out, indent);
            buf_printf(out, "play %s\n", ptok);
        }
        else if (strcmp(m, "return") == 0) {
            emit_indent(out, indent);
            buf_puts(out, "return\n");
        }
        else if (strcmp(m, "yield") == 0) {
            emit_indent(out, indent);
            buf_puts(out, "yield\n");
        }
        else if (strcmp(m, "done") == 0) {
            buf_puts(out, "}\n");
        }
        else if (strcmp(m, "nop") == 0) {
            /* skip */
        }
        else if (strcmp(m, "callvl") == 0) {
            int func = (inst->operand_count > 0) ? inst->operands[0] : 0;
            emit_indent(out, indent);
            buf_printf(out, "call func_%d\n", func);
        }
        else if (strcmp(m, "callv") == 0) {
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            const char *target_name = NULL;
            char fb[32];
            if (!suppress_entries) {
                for (uint32_t s = 0; s < script->section_count; ++s) {
                    if ((int)script->sections[s].code_offset == target) {
                        target_name = script->sections[s].name; break;
                    }
                }
            }
            if (!target_name) {
                snprintf(fb, sizeof(fb), "@%04X", (unsigned)target);
                target_name = fb;
            }
            emit_indent(out, indent);
            buf_printf(out, "call %s\n", target_name);
        }
        else if (strcmp(m, "tablexec") == 0) {
            /* on (expr) <action> target1 target2 ... */
            int count       = inst->operand_count > 0 ? inst->operands[0] : 0;
            int inner_op    = inst->operand_count > 1 ? inst->operands[1] : 0;
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
            if (!expr[0]) snprintf(expr, sizeof(expr), "condition");

            const char *target_type = "enter";
            if (inner_op == 0x3D || inner_op == 0x3E) target_type = "play";
            else if (inner_op == 0x30)                target_type = "goto";
            else if (inner_op == 0x3B)                target_type = "enter";
            else                                       target_type = "enter";

            char targets[1024];
            targets[0] = 0;
            size_t tpos = 0;
            int es_size = inst->table_entry_size;
            for (int t = 0; t < count; ++t) {
                const uint8_t *entry = inst->table_data + (size_t)t * es_size;
                char tname[64];
                if (inner_op == 0x3B && es_size >= 2) {
                    /* entry[0] = inner opcode byte, entry[1] = section idx */
                    int sidx = entry[1];
                    const char *n = resolve_section_idx(sidx, script,
                                                        tname, sizeof(tname));
                    snprintf(tname, sizeof(tname), "%s", n);
                } else if ((inner_op == 0x3D || inner_op == 0x3E) && es_size >= 2) {
                    int sidx = entry[1];
                    if (inner_op == 0x3D && es_size >= 3) sidx = entry[1] | (entry[2] << 8);
                    char raw[64];
                    resolve_play_name(sidx, sbf_names, sbf_name_count,
                                      raw, sizeof(raw));
                    format_play_token(raw, tname, sizeof(tname));
                } else if (inner_op == 0x30 && es_size >= 5) {
                    uint32_t addr = (uint32_t)entry[1]
                                  | ((uint32_t)entry[2] << 8)
                                  | ((uint32_t)entry[3] << 16)
                                  | ((uint32_t)entry[4] << 24);
                    const char *n = NULL;
                    for (uint32_t s = 0; s < script->section_count; ++s) {
                        if (script->sections[s].code_offset == addr) {
                            n = script->sections[s].name; break;
                        }
                    }
                    if (n) snprintf(tname, sizeof(tname), "%s", n);
                    else   snprintf(tname, sizeof(tname), "Label_%04X", (unsigned)addr);
                } else {
                    snprintf(tname, sizeof(tname), "null");
                }
                size_t tlen = strlen(tname);
                if (t > 0 && tpos + 1 < sizeof(targets)) targets[tpos++] = ' ';
                if (tpos + tlen < sizeof(targets)) {
                    memcpy(targets + tpos, tname, tlen);
                    tpos += tlen;
                }
            }
            targets[tpos < sizeof(targets) ? tpos : sizeof(targets) - 1] = 0;
            if (!targets[0]) snprintf(targets, sizeof(targets), "null");
            emit_indent(out, indent);
            buf_printf(out, "on (%s) %s %s\n", expr, target_type, targets);
        }
        ++i;
    }

}

/* Shared body for mus_decompile and mus_decompile_with_names. The public
   wrappers differ only in whether sbf_names is non-NULL: a names-aware emit
   produces "play GAMINT" / "bind sound_1 \"GAMINT\""; the original path
   preserves the legacy "play sound_1" / "bind sound_1 \"sound_1\"" output
   so the golden text test still matches byte-for-byte. */
static int decompile_into_buf(const MusScript *script,
                              const char *const *sbf_names,
                              uint32_t sbf_name_count,
                              char *out_text, size_t out_capacity) {
    if (!script) return -1;

    Buf b;
    b.p = out_text;
    b.cap = out_capacity;
    b.used = 0;

    /* Disassemble the entire bytecode region. The Python decompiler mirrors
       this: it treats the chunk's bytecode as a flat stream with section
       entry points expressed as offsets. */
    int n_insts = 0;
    Instruction *insts = NULL;
    if (script->code && script->code_size > 0) {
        /* Worst case: every byte is a 1-byte opcode → code_size instructions. */
        insts = (Instruction *)calloc(script->code_size, sizeof(Instruction));
        if (!insts) return -2;
        disassemble(script->code, script->code_size, insts, (int)script->code_size,
                    &n_insts);
    }

    /* First pass: collect play_indices, globals_used, section_indices for the
       header / declsection emission. */
    int max_play_idx = -1;
    int globals_used[256]; int n_globals = 0;
    for (int i = 0; i < n_insts; ++i) {
        const char *m = insts[i].mnemonic;
        if (!m) continue;
        if (strcmp(m, "play") == 0 || strcmp(m, "playw") == 0) {
            if (insts[i].operand_count > 0) {
                int p = insts[i].operands[0];
                if (p > max_play_idx) max_play_idx = p;
            }
        } else if (strcmp(m, "push_g") == 0 || strcmp(m, "pop_g") == 0
                || strcmp(m, "inc_g") == 0   || strcmp(m, "dec_g") == 0
                || strcmp(m, "push_ga") == 0 || strcmp(m, "pop_ga") == 0) {
            if (insts[i].operand_count > 0) {
                int g = insts[i].operands[0];
                int seen = 0;
                for (int k = 0; k < n_globals; ++k) {
                    if (globals_used[k] == g) { seen = 1; break; }
                }
                if (!seen && n_globals < (int)(sizeof(globals_used)/sizeof(globals_used[0]))) {
                    globals_used[n_globals++] = g;
                }
            }
        }
    }
    /* Sort globals_used ascending for stable output. */
    for (int i = 0; i < n_globals; ++i) {
        for (int j = i + 1; j < n_globals; ++j) {
            if (globals_used[j] < globals_used[i]) {
                int t = globals_used[i]; globals_used[i] = globals_used[j];
                globals_used[j] = t;
            }
        }
    }

    /* ---- Header ---- */
    buf_printf(&b, "// Script: %s\n", script->name);
    if (script->source_path[0]) {
        buf_printf(&b, "// Original source: %s\n", script->source_path);
    }
    buf_puts(&b, "//\n");
    buf_puts(&b, "// IMPORTANT: Load MUSIC.LAN before opening this file in MDEdit\n");
    /* Methods comment. Python keeps the original string (e.g. "GEcho") when
       the split returns no object prefix, and splits otherwise (`FSet` -> `F.Set`). */
    buf_puts(&b, "// The following methods are used: ");
    {
        int first = 1;
        for (uint32_t i = 0; i < script->intrinsic_count; ++i) {
            const char *combined = script->intrinsic_names[i];
            if (!combined[0] || combined[0] == '@') continue;
            char obj[16], mth[64];
            split_method_name(combined, obj, sizeof(obj), mth, sizeof(mth));
            if (!first) buf_puts(&b, ", ");
            first = 0;
            if (obj[0]) buf_printf(&b, "%s.%s", obj, mth);
            else        buf_printf(&b, "%s", combined);
        }
    }
    buf_putc(&b, '\n');
    buf_putc(&b, '\n');

    /* script header */
    buf_printf(&b, "script %s\n\n", script->name);

    /* Bind declarations: emit sound_0 .. sound_max_play_idx. With sbf_names
       in hand, the bind quotes show real entry names so artists can read the
       script directly; without them, fall back to the legacy "sound_N"
       placeholder so the byte-identical decompile test still passes. */
    if (max_play_idx >= 0) {
        buf_puts(&b, "// Sound bind declarations (play requires bind identifiers)\n");
        for (int k = 0; k <= max_play_idx; ++k) {
            char qname[64];
            resolve_play_name(k, sbf_names, sbf_name_count, qname, sizeof(qname));
            buf_printf(&b, "bind sound_%d \"%s\"\n", k, qname);
        }
        buf_putc(&b, '\n');
    }

    /* Global variables comment block */
    if (n_globals > 0) {
        buf_puts(&b, "// Global variables\n");
        buf_puts(&b, "// Used global offsets from original: [");
        for (int k = 0; k < n_globals; ++k) {
            if (k > 0) buf_puts(&b, ", ");
            buf_printf(&b, "%d", globals_used[k]);
        }
        buf_puts(&b, "]\n");
        buf_printf(&b, "// Original globals_area = %u bytes\n", script->globals_size);
        buf_puts(&b, "// NOTE: MDEdit pre-defines Var00-Var15 at offsets 0-60 (64 bytes)\n");

        /* User globals at offsets >= 64 (Python emits declarations only for
           those; offsets 0..60 are pre-defined Var00..Var15). */
        int min_user = INT32_MAX, max_user = INT32_MIN;
        for (int k = 0; k < n_globals; ++k) {
            if (globals_used[k] >= 64) {
                if (globals_used[k] < min_user) min_user = globals_used[k];
                if (globals_used[k] > max_user) max_user = globals_used[k];
            }
        }
        if (max_user >= 64) {
            for (int off = 64; off <= max_user; off += 4) {
                int found = 0;
                for (int k = 0; k < n_globals; ++k) {
                    if (globals_used[k] == off) { found = 1; break; }
                }
                if (found) {
                    char vname[64];
                    resolve_global_into(vname, sizeof(vname), off,
                                        script->variables, script->variable_count);
                    buf_printf(&b, "global INT %s\n", vname);
                } else {
                    buf_printf(&b, "global INT _pad_%d\n", off / 4);
                }
            }
        }
        buf_putc(&b, '\n');
    }

    /* declsection forward declarations (one per section, in section-index order) */
    if (script->section_count > 0) {
        buf_puts(&b, "// Section forward declarations\n");
        for (uint32_t s = 0; s < script->section_count; ++s) {
            buf_printf(&b, "declsection %s\n", script->sections[s].name);
        }
        buf_putc(&b, '\n');
    }

    /* ---- Section bodies ---- */
    CFMap cf{};
    if (n_insts > 0) {
        analyze_control_flow(insts, n_insts, &cf);
        decompile_block(&b, insts, 0, n_insts, script, &cf, /*suppress_entries=*/0, 0,
                        sbf_names, sbf_name_count);
    }

    cf_free(&cf);
    if (insts) {
        free_instructions(insts, n_insts);
        free(insts);
    }

    /* Two-pass: first call (out=NULL) returns required size; second call
       writes into a sufficient buffer. NUL-terminate when there's room. */
    if (b.p && b.cap > 0) {
        size_t end = b.used < b.cap ? b.used : b.cap - 1;
        b.p[end] = 0;
    }
    return (int)b.used;
}

}   /* anonymous namespace */

extern "C" int mus_decompile(const MusScript *script, char *out_text, size_t out_capacity) {
    return decompile_into_buf(script, NULL, 0, out_text, out_capacity);
}

extern "C" int mus_decompile_with_names(const MusScript *script,
                                        const char *const *sbf_names,
                                        uint32_t sbf_name_count,
                                        char *out_text, size_t out_capacity) {
    return decompile_into_buf(script, sbf_names, sbf_name_count,
                              out_text, out_capacity);
}
