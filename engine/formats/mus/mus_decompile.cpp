/* MUS bytecode -> .mus source decompiler.

   Reference implementation: the pre-repo Python MUS decompiler (1726 lines)
   that this port structurally mirrors.

   The Python decompiler is a high-level source decompiler: it parses the
   editor debug-export table for section names and source path, performs
   stack-based expression reconstruction, recognises if/if_else/while
   control-flow patterns, decodes the embedded `tablexec` jump table, and
   synthesises `bind sound_N "sound_N"` aesthetic for the play opcodes
   (since the runtime carries no bind table). The minted gamescript's text is
   pinned byte for byte (`fixtures/mus/golden_synth_gamemus.mus.txt`), and every
   shipped script's text compiles back to its own bytes (mus_encode_idempotence).

   The opcode table below was transcribed from the Python `OPCODES = {...}`
   dict at line 23. The IDA witness [orig: 65-entry dispatch table @ 0x84F220, see
   engine/formats/mus/mus.h] confirmed all 65 entries (0x00..0x40);
   slots 0x0D, 0x0E, 0x1F, 0x26, 0x27, 0x2C..0x2F, 0x36, 0x37, 0x3C have
   no Python entry (those are unused / no-op slots in the dispatch table).

   This file is the whole decompiler: the bytecode decoder (the opcode table,
   Instruction, disassemble), the pure rendering helpers (name resolution,
   control-flow analysis, and stack-based expression reconstruction), and the
   linear text-emission machinery (Buf + decompile_block).

   Beyond the Python reference, the text carries what MDEdit's layout needs for the compiler to write the
   script's own bytes again (mus_compile.cpp): a function (`handler NAME(params) { ... }`, its frame setup
   0x38 and its parameters by their debug names; a 0x38 anywhere else `frame N`), the user globals the debug
   table declares, and the line table: each statement, `else`, section-closing brace and function header is put
   on the line the table names for its first instruction (blank lines before it, a `#line N` directive where the
   text has run past). The leading nop and the nops padding the code to four bytes are the layout's, not
   statements; any other nop prints as `nop`. */

#include <formats/mus/mus.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace opennova::mus {

namespace {

/* ---- The bytecode decoder: the IDA-derived opcode table and operand widths
   (behaviour guarded by the golden byte-compare in mus_decompile_test.cpp) ---- */

/* Operand types match the Python decompiler:
     0 = u8 (8-bit immediate)
     1 = u16 (16-bit immediate)
     2 = u32 (32-bit immediate)
     3 = u8 global var byte-offset
     4 = u8 local var index
     5 = u32 label / branch target offset
     6 = u8 function index (callvl)
     7 = u8 special (tablexec inner_opcode)
     9 = u8 method index */
struct OpInfo {
    const char *mnemonic;
    int         operand_count;
    int         operand_types[4];
};

#define MAX_OPERANDS 5    /* tablexec: 4 immediate + 1 dynamic table-data slot */

static const OpInfo kOps[256] = {
    /* 0x00 */ {"nop",      0, {0}},
    /* 0x01 */ {"push",     1, {0}},
    /* 0x02 */ {"push",     1, {2}},
    /* 0x03 */ {"push_g",   1, {3}},
    /* 0x04 */ {"push_l",   1, {4}},
    /* 0x05 push_ga: [orig 0x6727F0] movzx byte; inc esi; inc esi -> the engine
       advances IP by 2 (1-byte global offset + 1 reserved byte), NOT u8+u32.
       0x06/0x0A/0x0B likewise advance 2; 0x07 pushstr [0x672830] advances 1. */
    /* 0x05 */ {"push_ga",  2, {3, 0}},
    /* 0x06 */ {"push_la",  2, {4, 0}},
    /* 0x07 */ {"pushstr",  1, {0}},
    /* 0x08 */ {"pop_g",    1, {3}},
    /* 0x09 */ {"pop_l",    1, {4}},
    /* 0x0A */ {"pop_ga",   2, {3, 0}},
    /* 0x0B */ {"pop_la",   2, {4, 0}},
    /* 0x0C */ {"push_me",  0, {0}},
    /* 0x0D */ {NULL,       0, {0}},
    /* 0x0E */ {NULL,       0, {0}},
    /* 0x0F */ {"empty",    0, {0}},
    /* 0x10 */ {"add",      0, {0}},
    /* 0x11 */ {"sub",      0, {0}},
    /* 0x12 */ {"mult",     0, {0}},
    /* 0x13 */ {"div",      0, {0}},
    /* 0x14 */ {"mod",      0, {0}},
    /* 0x15 */ {"l_and",    0, {0}},
    /* 0x16 */ {"l_or",     0, {0}},
    /* 0x17 */ {"and",      0, {0}},
    /* 0x18 */ {"or",       0, {0}},
    /* 0x19 */ {"xor",      0, {0}},
    /* 0x1A */ {"neg",      0, {0}},
    /* 0x1B */ {"inverse",  0, {0}},
    /* 0x1C */ {"lshift",   0, {0}},
    /* 0x1D */ {"rshift",   0, {0}},
    /* 0x1E */ {"not",      0, {0}},
    /* 0x1F */ {NULL,       0, {0}},
    /* 0x20 */ {"equal",    0, {0}},
    /* 0x21 */ {"notequal", 0, {0}},
    /* 0x22 */ {"ge",       0, {0}},
    /* 0x23 */ {"le",       0, {0}},
    /* 0x24 */ {"gt",       0, {0}},
    /* 0x25 */ {"lt",       0, {0}},
    /* 0x26 */ {NULL,       0, {0}},
    /* 0x27 */ {NULL,       0, {0}},
    /* 0x28 */ {"inc_g",    1, {3}},
    /* 0x29 */ {"dec_g",    1, {3}},
    /* 0x2A */ {"inc_l",    1, {4}},
    /* 0x2B */ {"dec_l",    1, {4}},
    /* 0x2C */ {NULL,       0, {0}},
    /* 0x2D */ {NULL,       0, {0}},
    /* 0x2E */ {NULL,       0, {0}},
    /* 0x2F */ {NULL,       0, {0}},
    /* 0x30 */ {"goto",     1, {5}},
    /* 0x31 */ {"brfalse",  1, {5}},
    /* 0x32 */ {"brtrue",   1, {5}},
    /* 0x33 */ {"callvl",   1, {6}},
    /* 0x34 */ {"callv",    1, {5}},
    /* 0x35 */ {"tablexec", 4, {0, 7, 0, 0}},
    /* 0x36 */ {NULL,       0, {0}},
    /* 0x37 */ {NULL,       0, {0}},
    /* 0x38 */ {"enter",    1, {0}},
    /* 0x39 */ {"return",   0, {0}},
    /* 0x3A */ {"yield",    0, {0}},
    /* 0x3B */ {"setstate", 1, {0}},
    /* 0x3C */ {NULL,       0, {0}},
    /* 0x3D */ {"playw",    1, {1}},
    /* 0x3E */ {"play",     1, {0}},
    /* 0x3F */ {"done",     0, {0}},
    /* 0x40 */ {"method",   1, {9}},
    /* 0x41..0xFF default-zero (NULL mnemonic). */
};

struct Instruction {
    uint32_t    offset;
    uint8_t     opcode;
    const char *mnemonic;
    int32_t     operands[MAX_OPERANDS];
    int         operand_count;
    /* Embedded tablexec entry data (count * entry_size bytes). */
    uint8_t    *table_data;
    int         table_entry_size;
    uint32_t    size;       /* total bytes including operands and tablexec data */
    int         truncated;  /* an operand or the embedded table runs past the code's end */
};

/* Decode the bytecode into Instructions matching the Python disassembler
   semantics (same operand widths and tablexec embedded-data handling). */
static void disassemble(const uint8_t *bytes, uint32_t size,
                        Instruction *out, int max_inst, int *out_count) {
    int n = 0;
    uint32_t pos = 0;
    while (pos < size && n < max_inst) {
        Instruction &inst = out[n];
        memset(&inst, 0, sizeof(inst));
        inst.offset = pos;
        inst.opcode = bytes[pos++];

        const OpInfo &info = kOps[inst.opcode];
        if (info.mnemonic) {
            inst.mnemonic = info.mnemonic;
            for (int k = 0; k < info.operand_count && k < MAX_OPERANDS; ++k) {
                int t = info.operand_types[k];
                int32_t val = 0;
                if (t == 0 || t == 3 || t == 4 || t == 6 || t == 7 || t == 9) {
                    if (pos < size) val = bytes[pos];
                    pos += 1;
                } else if (t == 1) {
                    if (pos + 2 <= size)
                        val = (int32_t)(bytes[pos] | (bytes[pos + 1] << 8));
                    pos += 2;
                } else if (t == 2 || t == 5) {
                    if (pos + 4 <= size) {
                        val = (int32_t)((uint32_t)bytes[pos]
                                       | ((uint32_t)bytes[pos + 1] << 8)
                                       | ((uint32_t)bytes[pos + 2] << 16)
                                       | ((uint32_t)bytes[pos + 3] << 24));
                    }
                    pos += 4;
                } else {
                    if (pos < size) val = bytes[pos];
                    pos += 1;
                }
                inst.operands[inst.operand_count++] = val;
            }
            if (pos > size) inst.truncated = 1;
            /* tablexec (0x35): 4 immediates then count*entry_size embedded. */
            if (inst.opcode == MUS_OP_TABLEXEC && inst.operand_count >= 4) {
                int count = inst.operands[0] & 0xFF;
                int es    = inst.operands[2] & 0xFF;
                inst.table_entry_size = es;
                int total = count * es;
                if (total > 0 && pos + (uint32_t)total <= size) {
                    inst.table_data = (uint8_t *)malloc((size_t)total);
                    memcpy(inst.table_data, bytes + pos, (size_t)total);
                    pos += (uint32_t)total;
                } else if (total > 0) {
                    inst.truncated = 1;
                }
            }
        } else {
            /* Unknown opcode (Python uses "unknown_HH" mnemonic). */
            inst.mnemonic = NULL;
        }
        inst.size = pos - inst.offset;
        ++n;
    }
    *out_count = n;
}

static void free_instructions(Instruction *insts, int count) {
    for (int i = 0; i < count; ++i) {
        free(insts[i].table_data);
    }
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

/* The function a body belongs to: its name and its parameters' names by frame offset. */
struct FunctionView {
    char     name[MUS_SECTION_NAME_SIZE];
    uint32_t start = 0, end = 0;
    int      params = 0;                     /* its frame setup's count, up to 255 */
    std::vector<std::string> param_names;    /* one per parameter */
    std::vector<uint32_t>    param_offsets;
};

static void resolve_local_into(char *out, size_t cap, int idx, const FunctionView *fn = NULL) {
    if (fn) {
        for (int k = 0; k < fn->params; ++k)
            if ((int)fn->param_offsets[(size_t)k] == idx) {
                snprintf(out, cap, "%s", fn->param_names[(size_t)k].c_str());
                return;
            }
    }
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
     G* -> ('', "*")  (GLOBAL is default, no prefix)
     F* -> ('F', "*")
     T* -> ('T', "*")
     other -> ('', name)
   `obj` needs room for two bytes. */
static void split_method_name(const char *combined, char *obj,
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
    uint32_t body_end;      /* the if-body's end: the brfalse target, or an if/else's goto */
};

#define CF_TYPE_IF      1
#define CF_TYPE_IF_ELSE 2

/* CFBlock map keyed by start_offset. We store as parallel arrays for simplicity. */
struct CFMap {
    CFBlock *blocks;
    int      count;
};

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

/* The index of the instruction at `offset` (n for the code's end), -1 when `offset`
   lands inside an instruction or past the end. The instructions are in offset order. */
static int index_at(const Instruction *insts, int n, uint32_t offset) {
    if (n == 0) return offset == 0 ? 0 : -1;
    const uint32_t end = insts[n - 1].offset + insts[n - 1].size;
    if (offset == end) return n;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (insts[mid].offset == offset) return mid;
        if (insts[mid].offset < offset) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int is_section_entry(const MusScript *script, uint32_t offset) {
    for (uint32_t s = 0; s < script->section_count; ++s)
        if (script->sections[s].code_offset == offset) return 1;
    return 0;
}

/* Every brfalse a block, kept apart from the Python reference where the reference
   would mis-structure or loop:
   - an instruction cut short by the code's end, or a branch (goto, brfalse, brtrue,
     callv) to an offset inside an instruction or past the end, is malformed: the
     analysis fails and the script does not decompile (the editor reports it unreadable);
   - a brfalse to its own offset or before it is no if (a loop the text has no form
     for): no block, so it prints as its `// if !(...) goto` comment;
   - an if/else is a brfalse whose LAST body instruction is a goto to the brfalse
     target or past it that names no section, exactly what the compiler writes for
     `if (...) { } else { }` (mus_compile.cpp, the goto past the else then the
     brfalse patched to the else); a goto elsewhere in the body, or to a section, is
     the body's own `goto`.
   Returns 0, or -1 for a malformed program. */
static int analyze_control_flow(const Instruction *insts, int n, const MusScript *script, CFMap *out) {
    int cap = 0;
    out->blocks = NULL;
    out->count = 0;

    for (int i = 0; i < n; ++i) {
        const Instruction *inst = &insts[i];
        if (inst->truncated) return -1;
        if (!inst->mnemonic || inst->operand_count < 1) continue;
        const int branch = strcmp(inst->mnemonic, "brfalse") == 0 || strcmp(inst->mnemonic, "brtrue") == 0 ||
                           strcmp(inst->mnemonic, "goto") == 0 || strcmp(inst->mnemonic, "callv") == 0;
        if (!branch) continue;
        const uint32_t target = (uint32_t)inst->operands[0];
        const int target_idx = index_at(insts, n, target);
        if (target_idx < 0) return -1;
        if (strcmp(inst->mnemonic, "brfalse") != 0 || target <= inst->offset) continue;
        /* The instruction ending at the target: an if/else's goto past the else. */
        const Instruction *last = target_idx > i + 1 ? &insts[target_idx - 1] : NULL;
        int else_goto = 0;
        if (last && last->mnemonic && strcmp(last->mnemonic, "goto") == 0 && last->operand_count >= 1) {
            const uint32_t goto_target = (uint32_t)last->operands[0];
            else_goto = goto_target >= target && index_at(insts, n, goto_target) >= 0 &&
                        !is_section_entry(script, goto_target);
        }
        CFBlock b{};
        b.start_offset = inst->offset;
        b.body_start   = inst->offset + inst->size;
        if (else_goto) {
            b.type        = CF_TYPE_IF_ELSE;
            b.else_offset = target;
            b.end_offset  = (uint32_t)last->operands[0];
            b.body_end    = last->offset;
        } else {
            b.type       = CF_TYPE_IF;
            b.end_offset = target;
            b.body_end   = target;
        }
        cf_add(out, &cap, b);
    }
    return 0;
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
                                   const MusScript *script, char *out, size_t out_cap,
                                   const FunctionView *fn = NULL) {
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
            resolve_local_into(name, sizeof(name), idx, fn);
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
            split_method_name(combined, obj, mth, sizeof(mth));
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
        } else if (is_expr_end_op(m) || strcmp(m, "tablexec") == 0) {
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

/* Buffer that supports two-pass query (out=NULL, cap=0 → just count). */
struct Buf {
    char    *p;
    size_t   cap;
    size_t   used;
    int      failed;  /* the program nests past kMaxNesting: it does not decompile */
    uint32_t line = 1; /* the line the next character goes on */
};

/* How deep ifs nest before the decompile gives up (the recursion's bound: a crafted
   program of a few KB nested ifs would otherwise overflow the stack). Retail nests 1
   deep (MJox01.bin); the compiler takes the same bound (mus_compile.cpp). */
constexpr int kMaxNesting = 64;

static void buf_putc(Buf *b, char c) {
    if (b->p && b->used + 1 < b->cap) b->p[b->used] = c;
    ++b->used;
    if (c == '\n') ++b->line;
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

/* The line the script's line table names for the instruction at `offset`, 0 for none. */
static uint32_t table_line(const MusScript *script, uint32_t offset) {
    uint32_t lo = 0, hi = script->line_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (script->lines[mid].code_offset < offset) lo = mid + 1;
        else hi = mid;
    }
    return lo < script->line_count && script->lines[lo].code_offset == offset ? script->lines[lo].line : 0;
}

/* Put the next line, `before` lines ahead of the one written for `offset`, where the line table says: blank
   lines up to it, or a `#line` directive where the text has run past it (the compiler numbers lines alike). */
static void place(Buf *b, const MusScript *script, uint32_t offset, uint32_t before = 0) {
    const uint32_t line = table_line(script, offset);
    if (line == 0 || line <= before) return;
    const uint32_t want = line - before;
    if (want == b->line) return;
    if (want > b->line) {
        while (b->line < want) buf_putc(b, '\n');
        return;
    }
    buf_printf(b, "#line %u\n", want);
    b->line = want;
}

/* ---- High-level decompile (matches Python `generate_mus_source_with_labels`) ---- */

static void emit_indent(Buf *b, int indent) {
    for (int k = 0; k < indent; ++k) buf_puts(b, "    ");
}

/* Forward decl for recursion. */
static void decompile_block(Buf *out,
                            const Instruction *insts, int begin_idx, int end_idx,
                            const MusScript *script,
                            const CFMap *cf,        /* control flow map for the FULL section */
                            int suppress_entries,    /* recursive bodies don't emit "section X" */
                            int indent,
                            const char *const *sbf_names,
                            uint32_t sbf_name_count,
                            const FunctionView *fn,  /* the function a body belongs to, or NULL */
                            const FunctionView *functions, int function_count);

/* Resolve a branch/call code-offset target to its section name, else the
   "@XXXX" fallback (Python: entry_points.get(target, f"@{target:04X}")). A body
   names a section as the top level does: `suppress_entries` only keeps a body from
   printing section headers, so a `goto` or a `call` inside an if-body compiles back
   (the compiler resolves both by section name). Shared by goto/brfalse/brtrue/callv. */
static const char *resolve_branch_target(const MusScript *script, int target,
                                         int /*suppress_entries*/,
                                         char *fb, size_t fb_size) {
    for (uint32_t s = 0; s < script->section_count; ++s) {
        if ((int)script->sections[s].code_offset == target) {
            return script->sections[s].name;
        }
    }
    snprintf(fb, fb_size, "@%04X", (unsigned)target);
    return fb;
}

static void decompile_block(Buf *out,
                            const Instruction *insts, int begin_idx, int end_idx,
                            const MusScript *script,
                            const CFMap *cf,
                            int suppress_entries,
                            int indent,
                            const char *const *sbf_names,
                            uint32_t sbf_name_count,
                            const FunctionView *fn,
                            const FunctionView *functions, int function_count) {
    char buf256[256];
    char fallback[64];

    /* indent is the nesting depth: a body one deeper than its if. */
    if (indent > kMaxNesting) {
        out->failed = 1;
        return;
    }
    int i = begin_idx;
    while (i < end_idx && !out->failed) {
        const Instruction *inst = &insts[i];

        /* Section entry-point label. Only at top-level walks (recursive
           bodies pass suppress_entries=1 so the section header isn't
           re-emitted inside if/else/while bodies). */
        if (!suppress_entries) {
            for (uint32_t s = 0; s < script->section_count; ++s) {
                if (script->sections[s].code_offset == inst->offset) {
                    if (script->line_count == 0) buf_putc(out, '\n');
                    place(out, script, inst->offset, 2);
                    buf_printf(out, "section %s\n", script->sections[s].name);
                    buf_puts(out, "{\n");
                    break;
                }
            }
            /* A function: its header (its frame setup's line), its body, its brace. */
            const FunctionView *function = NULL;
            for (int k = 0; k < function_count; ++k)
                if (functions[k].start == inst->offset && inst->opcode == MUS_OP_ENTER) function = &functions[k];
            if (function) {
                if (script->line_count == 0) buf_putc(out, '\n');
                place(out, script, inst->offset);
                buf_printf(out, "handler %s(", function->name);
                for (int k = 0; k < function->params; ++k)
                    buf_printf(out, "%s%s", k ? ", " : "", function->param_names[(size_t)k].c_str());
                buf_puts(out, ")\n{\n");
                int body_end = i + 1;
                while (body_end < end_idx && insts[body_end].offset < function->end) ++body_end;
                decompile_block(out, insts, i + 1, body_end, script, cf, /*suppress_entries=*/1, indent,
                                sbf_names, sbf_name_count, function, functions, function_count);
                buf_puts(out, "}\n");
                i = body_end;
                continue;
            }
        }

        /* Control flow block detection, in a body as at the top level: the
           map holds every branch by its offset, so an if nested in an if's
           body is structured there too (the Python decompiler reanalyzes per
           inner call). Consulted at the top level alone, jox01's MJox01.bin
           lost sixteen nested ifs to `// if !(...) goto @...` comments, which
           compile to nothing: eight `if (Var02 != 11) { play; enter }` in
           MenuSP's first if and eight `if ((Var02 != 5) && (Var02 != 6))
           { play; enter }` in MenuMP's. A drawn block always moves past its
           brfalse, so a block can never be revisited. */
        const CFBlock *block = NULL;
        for (int k = 0; k < cf->count; ++k) {
            if (cf->blocks[k].start_offset == inst->offset) {
                block = &cf->blocks[k];
                break;
            }
        }
        const int brfalse_idx = i;
        if (block && block->type == CF_TYPE_IF) {
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr), fn);
            if (!expr[0]) snprintf(expr, sizeof(expr), "condition");
            place(out, script, insts[es].offset);
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
                                sbf_names, sbf_name_count, fn, functions, function_count);
            }
            emit_indent(out, indent);
            buf_puts(out, "}\n");
            while (i < end_idx && insts[i].offset < block->end_offset) ++i;
            if (i <= brfalse_idx) i = brfalse_idx + 1;
            continue;
        }
        if (block && block->type == CF_TYPE_IF_ELSE) {
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr), fn);
            if (!expr[0]) snprintf(expr, sizeof(expr), "condition");
            place(out, script, insts[es].offset);
            emit_indent(out, indent);
            buf_printf(out, "if (%s)\n", expr);
            emit_indent(out, indent);
            buf_puts(out, "{\n");
            /* if body is [body_start .. the goto past the else) (Python:
               block.else_offset - 5, the goto's offset: the goto is the last body
               instruction, analyze_control_flow) */
            int if_begin = -1, if_end = -1;
            for (int k = 0; k < end_idx; ++k) {
                if (insts[k].offset >= block->body_start && if_begin < 0) if_begin = k;
                if (insts[k].offset >= block->body_end && if_end < 0) {
                    if_end = k;
                    break;
                }
            }
            if (if_begin >= 0) {
                if (if_end < 0) if_end = end_idx;
                decompile_block(out, insts, if_begin, if_end, script, cf,
                                /*suppress_entries=*/1, indent + 1,
                                sbf_names, sbf_name_count, fn, functions, function_count);
            }
            emit_indent(out, indent);
            buf_puts(out, "}\n");
            place(out, script, block->body_end);
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
                                sbf_names, sbf_name_count, fn, functions, function_count);
            }
            emit_indent(out, indent);
            buf_puts(out, "}\n");
            while (i < end_idx && insts[i].offset < block->end_offset) ++i;
            if (i <= brfalse_idx) i = brfalse_idx + 1;
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
                reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr), fn);
                if (expr[0]) {
                    place(out, script, insts[es].offset);
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
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr), fn);
            place(out, script, insts[es < i ? es : i].offset);
            emit_indent(out, indent);
            if (expr[0]) buf_printf(out, "%s = %s\n", buf256, expr);
            else         buf_printf(out, "%s = ?\n", buf256);
        }
        else if (strcmp(m, "pop_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_local_into(buf256, sizeof(buf256), idx, fn);
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr), fn);
            place(out, script, insts[es < i ? es : i].offset);
            emit_indent(out, indent);
            if (expr[0]) buf_printf(out, "%s = %s\n", buf256, expr);
            else         buf_printf(out, "%s = ?\n", buf256);
        }
        else if (strcmp(m, "inc_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_global_into(buf256, sizeof(buf256), idx,
                                script->variables, script->variable_count);
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "%s++\n", buf256);
        }
        else if (strcmp(m, "inc_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_local_into(buf256, sizeof(buf256), idx, fn);
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "%s++\n", buf256);
        }
        else if (strcmp(m, "dec_g") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_global_into(buf256, sizeof(buf256), idx,
                                script->variables, script->variable_count);
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "%s--\n", buf256);
        }
        else if (strcmp(m, "dec_l") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            resolve_local_into(buf256, sizeof(buf256), idx, fn);
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "%s--\n", buf256);
        }
        else if (strcmp(m, "goto") == 0) {
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            /* Resolve label name (resolve_branch_target). */
            char fb[32];
            const char *target_name = resolve_branch_target(
                script, target, suppress_entries, fb, sizeof(fb));
            place(out, script, inst->offset);
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
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr), fn);
            char fb[32];
            const char *target_name = resolve_branch_target(
                script, target, suppress_entries, fb, sizeof(fb));
            emit_indent(out, indent);
            buf_printf(out, "// if !(%s) goto %s\n", expr, target_name);
        }
        else if (strcmp(m, "brtrue") == 0) {
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr), fn);
            char fb[32];
            const char *target_name = resolve_branch_target(
                script, target, suppress_entries, fb, sizeof(fb));
            emit_indent(out, indent);
            buf_printf(out, "// if (%s) goto %s\n", expr, target_name);
        }
        else if (strcmp(m, "setstate") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            const char *name = resolve_section_idx(idx, script, fallback,
                                                   sizeof(fallback));
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "enter %s\n", name);
        }
        else if (strcmp(m, "enter") == 0) {
            /* A frame setup at no function's start (D-MUS-14: the Python reference printed it as a section's
               enter, which compiles to setstate). */
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "frame %d\n", inst->operand_count > 0 ? inst->operands[0] : 0);
        }
        else if (strcmp(m, "play") == 0 || strcmp(m, "playw") == 0) {
            int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
            char pname[64];
            resolve_play_name(idx, sbf_names, sbf_name_count,
                              pname, sizeof(pname));
            char ptok[72];
            format_play_token(pname, ptok, sizeof(ptok));
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "play %s\n", ptok);
        }
        else if (strcmp(m, "return") == 0) {
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_puts(out, "return\n");
        }
        else if (strcmp(m, "yield") == 0) {
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_puts(out, "yield\n");
        }
        else if (strcmp(m, "done") == 0) {
            place(out, script, inst->offset);
            buf_puts(out, "}\n");
        }
        else if (strcmp(m, "nop") == 0) {
            /* MDEdit's leading nop and its padding are the layout's (the compiler writes them); any other a
               statement. */
            const int padding = inst->offset == 0 || (script->code_size % 4 == 0 && inst->offset + 4 > script->code_size &&
                                                       [&] {
                                                           for (int k = i; k < end_idx; ++k)
                                                               if (insts[k].opcode != 0) return false;
                                                           return true;
                                                       }());
            if (!padding) {
                place(out, script, inst->offset);
                emit_indent(out, indent);
                buf_puts(out, "nop\n");
            }
        }
        else if (strcmp(m, "callvl") == 0) {
            int func = (inst->operand_count > 0) ? inst->operands[0] : 0;
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "call func_%d\n", func);
        }
        else if (strcmp(m, "callv") == 0) {
            int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
            char fb[32];
            const char *target_name = resolve_branch_target(
                script, target, suppress_entries, fb, sizeof(fb));
            place(out, script, inst->offset);
            emit_indent(out, indent);
            buf_printf(out, "call %s\n", target_name);
        }
        else if (strcmp(m, "tablexec") == 0) {
            /* on (expr) <action> target1 target2 ... */
            int count       = inst->operand_count > 0 ? inst->operands[0] : 0;
            int inner_op    = inst->operand_count > 1 ? inst->operands[1] : 0;
            int es = find_expr_start(insts, end_idx, i);
            char expr[256];
            reconstruct_expression(insts, es, i, script, expr, sizeof(expr), fn);
            if (!expr[0]) snprintf(expr, sizeof(expr), "condition");
            place(out, script, insts[es < i ? es : i].offset);

            /* The header's +1 byte names the table's action for the text; the VM never
               reads it [orig: AudioVM_Op_TableExec @ 0x672BB0 reads +0 count @ 0x672BBC,
               +2 stride @ 0x672BCF, +3 skip @ 0x672BC3]: it dispatches each entry's own
               first byte, a 0 one skipping the table [orig: @ 0x672BE5..0x672C03]. So
               each entry is read by its own opcode (0 null, 0x3B a section, 0x3E a byte
               sound, 0x3D a word sound, 0x30 an address), the header a hint for the
               action word alone. */
            const char *target_type = "enter";
            if (inner_op == MUS_OP_PLAYW || inner_op == MUS_OP_PLAY) target_type = "play";
            else if (inner_op == MUS_OP_GOTO)         target_type = "goto";
            else if (inner_op == MUS_OP_SETSTATE)     target_type = "enter";
            else                                       target_type = "enter";

            char targets[1024];
            targets[0] = 0;
            size_t tpos = 0;
            int es_size = inst->table_entry_size;
            for (int t = 0; t < count; ++t) {
                const uint8_t *entry = inst->table_data + (size_t)t * es_size;
                const int entry_op = es_size >= 1 ? entry[0] : 0;
                char tname[64];
                if (entry_op == MUS_OP_SETSTATE && es_size >= 2) {
                    /* entry[0] = inner opcode byte, entry[1] = section idx */
                    int sidx = entry[1];
                    const char *n = resolve_section_idx(sidx, script,
                                                        tname, sizeof(tname));
                    snprintf(tname, sizeof(tname), "%s", n);
                } else if ((entry_op == MUS_OP_PLAY && es_size >= 2) || (entry_op == MUS_OP_PLAYW && es_size >= 3)) {
                    int sidx = entry_op == MUS_OP_PLAYW ? entry[1] | (entry[2] << 8) : entry[1];
                    char raw[64];
                    resolve_play_name(sidx, sbf_names, sbf_name_count,
                                      raw, sizeof(raw));
                    format_play_token(raw, tname, sizeof(tname));
                } else if (entry_op == MUS_OP_GOTO && es_size >= 5) {
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
    b.failed = 0;

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
            split_method_name(combined, obj, mth, sizeof(mth));
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

    /* The user globals the debug table declares (offset 64 and on) count as used: each is declared. */
    for (uint32_t v = 0; v < script->variable_count; ++v) {
        const int g = (int)script->variables[v].byte_offset;
        if (g < 64) continue;
        int seen = 0;
        for (int k = 0; k < n_globals; ++k) if (globals_used[k] == g) { seen = 1; break; }
        if (!seen && n_globals < (int)(sizeof(globals_used)/sizeof(globals_used[0]))) {
            int at = n_globals++;
            while (at > 0 && globals_used[at - 1] > g) { globals_used[at] = globals_used[at - 1]; --at; }
            globals_used[at] = g;
        }
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

    /* The functions: the debug table's, else the chunk's MessageHandler entry where it is a frame setup at no
       section's start (a stripped file), its body to the next section. Each parameter by its debug name
       (`Function::name` at its frame offset), else argN. */
    /* As many functions as the script has, each with as many parameters as its frame setup counts (a byte: up
       to 255); a parameter its debug table names nothing (or an empty name) is argN. */
    std::vector<FunctionView> function_list;
    const auto add_function = [&](const char *name, uint32_t start, uint32_t end) {
        if (start >= script->code_size || script->code[start] != MUS_OP_ENTER) return;
        function_list.emplace_back();
        FunctionView &f = function_list.back();
        snprintf(f.name, sizeof(f.name), "%s", name);
        f.start = start;
        f.end = end;
        f.params = start + 1 < script->code_size ? script->code[start + 1] : 0;
        const uint32_t base = script->locals_frame_offset ? script->locals_frame_offset : MUS_DEFAULT_LOCALS_BASE;
        const size_t own = strlen(f.name);
        for (int k = 0; k < f.params; ++k) {
            const uint32_t offset = base + 4u * (uint32_t)k;
            std::string named = "arg" + std::to_string(k + 1);
            for (uint32_t l = 0; l < script->local_count; ++l)
                if (script->locals[l].frame_offset == offset &&
                    strncmp(script->locals[l].name, f.name, own) == 0 && script->locals[l].name[own] == ':' &&
                    script->locals[l].name[own + 1] == ':' && script->locals[l].name[own + 2] != 0)
                    named = script->locals[l].name + own + 2;
            f.param_offsets.push_back(offset);
            f.param_names.push_back(named);
        }
    };
    for (uint32_t k = 0; k < script->function_count; ++k)
        add_function(script->functions[k].name, script->functions[k].start, script->functions[k].end);
    if (script->function_count == 0 && script->has_message_handler && !is_section_entry(script, script->message_handler_offset)) {
        uint32_t end = script->code_size;
        for (uint32_t k = 0; k < script->section_count; ++k)
            if (script->sections[k].code_offset > script->message_handler_offset && script->sections[k].code_offset < end)
                end = script->sections[k].code_offset;
        add_function("MessageHandler", script->message_handler_offset, end);
    }
    const FunctionView *functions = function_list.empty() ? NULL : function_list.data();
    const int function_count = (int)function_list.size();

    /* ---- Section bodies ---- */
    CFMap cf{};
    if (n_insts > 0) {
        if (analyze_control_flow(insts, n_insts, script, &cf) != 0) b.failed = 1;
        else decompile_block(&b, insts, 0, n_insts, script, &cf, /*suppress_entries=*/0, 0,
                             sbf_names, sbf_name_count, NULL, functions, function_count);
    }

    cf_free(&cf);
    if (insts) {
        free_instructions(insts, n_insts);
        free(insts);
    }
    /* A malformed program (an instruction cut short, a branch into an instruction or
       past the end) or one nested past kMaxNesting does not decompile. */
    if (b.failed) return -3;

    /* Two-pass: first call (out=NULL) returns required size; second call
       writes into a sufficient buffer. NUL-terminate when there's room. */
    if (b.p && b.cap > 0) {
        size_t end = b.used < b.cap ? b.used : b.cap - 1;
        b.p[end] = 0;
    }
    return (int)b.used;
}

}   /* anonymous namespace */

int mus_decompile(const MusScript *script, char *out_text, size_t out_capacity) {
    return decompile_into_buf(script, NULL, 0, out_text, out_capacity);
}

int mus_decompile_with_names(const MusScript *script,
                                        const char *const *sbf_names,
                                        uint32_t sbf_name_count,
                                        char *out_text, size_t out_capacity) {
    return decompile_into_buf(script, sbf_names, sbf_name_count,
                              out_text, out_capacity);
}

} // namespace opennova::mus
