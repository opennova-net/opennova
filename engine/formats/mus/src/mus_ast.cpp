/* MUS bytecode -> structured statement tree (mus/ast.h).

   The structured twin of mus_decompile.cpp: it runs the SAME decode + control-
   flow analysis + expression reconstruction (mus_decompile_shared.h), but where
   the decompiler emits text into a flat buffer, this builds a per-section
   statement tree the editor can render row-by-row, highlight by pc, and (Phase
   2) author. mus_ast_emit_text walks the tree back to .mus text byte-identical
   to mus_decompile() -- pinned by mus_ast_test.cpp -- so the structured edit
   path compiles to bytecode the original VM runs faithfully.

   Implementation note: parse uses std::vector internally (like mus_model.cpp)
   then materialises malloc'd C arrays the public ABI hands out. Statement
   attribution is by code-offset ownership (the section whose code_offset is the
   greatest <= the instruction offset), identical to mus_model.cpp, so leaked
   tail code after a `done` is still attributed to its owning section. */

#include "mus/ast.h"
#include "mus/mus.h"

#include "mus_decode.h"            // Instruction, disassemble()
#include "mus_decompile_shared.h"  // resolve_*/CF/reconstruct_expression/...

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

static char *dup_str(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* A fresh statement with every pointer NULL and every index field cleared to
   the "N/A" sentinel, so a builder only sets what applies. */
static MusAstStmt blank_stmt(int kind, uint32_t offset, uint32_t size) {
    MusAstStmt s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.code_offset = offset;
    s.byte_size = size;
    s.text = NULL;
    s.track_index = -1;
    s.wait = 0;
    s.target_section = -1;
    s.var_name = NULL;
    s.var_offset = -1;
    s.is_local = 0;
    s.is_inc = 0;
    s.rhs_text = NULL;
    s.expr_text = NULL;
    s.has_call = 0;
    s.call_name = NULL;
    s.rhs_tree = NULL;
    s.expr_tree = NULL;
    s.then_body = NULL;
    s.then_count = 0;
    s.else_body = NULL;
    s.else_count = 0;
    s.switch_action = 0;
    s.targets = NULL;
    s.target_count = 0;
    return s;
}

/* Section that owns an instruction at `offset`: greatest code_offset <= offset
   (matches mus_model.cpp::owner_section). */
static int owner_section(const MusScript *s, uint32_t offset) {
    int best = -1;
    uint32_t best_off = 0;
    int min_idx = -1;
    uint32_t min_off = 0;
    for (uint32_t i = 0; i < s->section_count; ++i) {
        uint32_t co = s->sections[i].code_offset;
        if (min_idx < 0 || co < min_off) { min_idx = (int)i; min_off = co; }
        if (co <= offset && (best < 0 || co > best_off)) { best = (int)i; best_off = co; }
    }
    return (best >= 0) ? best : min_idx;
}

/* Section index whose code_offset == addr, or -1 (matches mus_decompile goto/
   tablexec label resolution). */
static int section_at_offset(const MusScript *s, uint32_t addr) {
    for (uint32_t i = 0; i < s->section_count; ++i) {
        if (s->sections[i].code_offset == addr) return (int)i;
    }
    return -1;
}

/* Scan [a, b) for the first `method` op; on a hit fill *out_name with the
   combined intrinsic name and return 1. Drives the editor's "a function ran
   here" icon for expression / assignment statements. */
static int range_first_method(const Instruction *insts, int a, int b,
                              const MusScript *script,
                              char *out_name, size_t cap) {
    for (int k = a; k < b; ++k) {
        if (insts[k].mnemonic && strcmp(insts[k].mnemonic, "method") == 0) {
            int idx = (insts[k].operand_count > 0) ? insts[k].operands[0] : 0;
            resolve_method_into(out_name, cap, idx,
                                script->intrinsic_names, script->intrinsic_count);
            return 1;
        }
    }
    if (cap) out_name[0] = 0;
    return 0;
}

/* Resolve a goto/brfalse/brtrue/callv target offset for rendering, mirroring
   decompile_block: it only looks the target up in the section table at the TOP
   level (suppress_entries == 0); inside an if/switch body it always renders
   "@HHHH". Writes the rendered name into out_name and returns the section index
   (or -1 when unresolved -- including the suppressed case, so the editor and the
   text agree). enter/setstate and tablexec targets use resolve_section_idx /
   section_at_offset unconditionally instead (both decompiler and AST). */
static int resolve_branch_target(const MusScript *script, uint32_t target,
                                 int suppress_entries, char *out_name, size_t cap) {
    int idx = section_at_offset(script, target);
    if (!suppress_entries && idx >= 0) {
        snprintf(out_name, cap, "%s", script->sections[idx].name);
        return idx;
    }
    snprintf(out_name, cap, "@%04X", (unsigned)target);
    return suppress_entries ? -1 : idx;   /* idx is always -1 here when !suppress */
}

/* Build one non-compound statement from insts[i]. Returns 1 and fills *out when
   the instruction produces a visible statement; returns 0 for expression-part
   ops (push/binop/method/...) and for an `empty` that drains no expression --
   exactly the instructions mus_decompile.cpp renders as nothing. `end` is the
   enclosing block's end index (loop bound for find_expr_start parity).

   suppress_entries mirrors decompile_block's flag: inside an if/switch body the
   decompiler does NOT resolve goto/brfalse/brtrue/callv targets to section names
   (it emits "@HHHH"), so the AST must do the same to stay byte-identical. Other
   target resolution (enter/setstate via resolve_section_idx, tablexec targets)
   is unconditional in both, so it is unaffected. */
static int try_make_node(const Instruction *insts, int i, int end,
                         const MusScript *script, int suppress_entries, MusAstStmt *out) {
    const Instruction *inst = &insts[i];
    const char *m = inst->mnemonic;
    if (!m) return 0;

    char buf256[256];
    char fallback[64];

    /* Expression-part ops are folded into the EXPR/ASSIGN sink, no node. */
    if (strcmp(m, "push") == 0 || strcmp(m, "push_g") == 0
     || strcmp(m, "push_l") == 0 || strcmp(m, "push_me") == 0
     || strcmp(m, "pushstr") == 0
     || binop_str(m) || unaryop_str(m)
     || strcmp(m, "method") == 0) {
        return 0;
    }

    if (strcmp(m, "empty") == 0) {
        int es = find_expr_start(insts, end, i);
        if (es >= i) return 0;
        char expr[256];
        reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr));
        if (!expr[0]) return 0;
        *out = blank_stmt(MUS_AST_EXPR, inst->offset, inst->size);
        out->text = dup_str(expr);
        out->expr_text = dup_str(expr);
        out->expr_tree = reconstruct_expression_tree(insts, es, i + 1, script);
        char mname[64];
        if (range_first_method(insts, es, i, script, mname, sizeof(mname))) {
            out->has_call = 1;
            out->call_name = dup_str(mname);
        }
        return 1;
    }

    if (strcmp(m, "pop_g") == 0 || strcmp(m, "pop_l") == 0) {
        int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
        int is_local = (strcmp(m, "pop_l") == 0);
        if (is_local) resolve_local_into(buf256, sizeof(buf256), idx);
        else          resolve_global_into(buf256, sizeof(buf256), idx,
                                          script->variables, script->variable_count);
        int es = find_expr_start(insts, end, i);
        char expr[256];
        reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
        const char *rhs = expr[0] ? expr : "?";
        char line[512];
        snprintf(line, sizeof(line), "%s = %s", buf256, rhs);
        *out = blank_stmt(MUS_AST_ASSIGN, inst->offset, inst->size);
        out->text = dup_str(line);
        out->var_name = dup_str(buf256);
        out->var_offset = idx;
        out->is_local = is_local;
        out->rhs_text = dup_str(rhs);
        /* Tree only when the RHS reconstructed cleanly (expr non-empty -- the
           "?" placeholder stays text-only). */
        out->rhs_tree = expr[0] ? reconstruct_expression_tree(insts, es, i, script) : NULL;
        char mname[64];
        if (range_first_method(insts, es, i, script, mname, sizeof(mname))) {
            out->has_call = 1;
            out->call_name = dup_str(mname);
        }
        return 1;
    }

    if (strcmp(m, "inc_g") == 0 || strcmp(m, "dec_g") == 0
     || strcmp(m, "inc_l") == 0 || strcmp(m, "dec_l") == 0) {
        int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
        int is_local = (m[4] == 'l');               /* inc_l / dec_l */
        int is_inc = (m[0] == 'i');                 /* inc_* */
        if (is_local) resolve_local_into(buf256, sizeof(buf256), idx);
        else          resolve_global_into(buf256, sizeof(buf256), idx,
                                          script->variables, script->variable_count);
        char line[300];
        snprintf(line, sizeof(line), "%s%s", buf256, is_inc ? "++" : "--");
        *out = blank_stmt(MUS_AST_INCDEC, inst->offset, inst->size);
        out->text = dup_str(line);
        out->var_name = dup_str(buf256);
        out->var_offset = idx;
        out->is_local = is_local;
        out->is_inc = is_inc;
        return 1;
    }

    if (strcmp(m, "goto") == 0) {
        int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
        char tname[64];
        int sidx = resolve_branch_target(script, (uint32_t)target, suppress_entries,
                                         tname, sizeof(tname));
        char line[128];
        snprintf(line, sizeof(line), "goto %s", tname);
        *out = blank_stmt(MUS_AST_GOTO, inst->offset, inst->size);
        out->text = dup_str(line);
        out->target_section = sidx;
        return 1;
    }

    if (strcmp(m, "brfalse") == 0 || strcmp(m, "brtrue") == 0) {
        int is_false = (strcmp(m, "brfalse") == 0);
        int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
        int es = find_expr_start(insts, end, i);
        char expr[256];
        reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
        char tname[64];
        int sidx = resolve_branch_target(script, (uint32_t)target, suppress_entries,
                                         tname, sizeof(tname));
        char line[600];
        if (is_false) snprintf(line, sizeof(line), "// if !(%s) goto %s", expr, tname);
        else          snprintf(line, sizeof(line), "// if (%s) goto %s", expr, tname);
        *out = blank_stmt(MUS_AST_BRANCH_COMMENT, inst->offset, inst->size);
        out->text = dup_str(line);
        out->expr_text = dup_str(expr);
        out->expr_tree = reconstruct_expression_tree(insts, es, i, script);
        out->target_section = sidx;
        return 1;
    }

    if (strcmp(m, "setstate") == 0) {
        int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
        const char *name = resolve_section_idx(idx, script, fallback, sizeof(fallback));
        char line[128];
        snprintf(line, sizeof(line), "enter %s", name);
        *out = blank_stmt(MUS_AST_TRANSITION, inst->offset, inst->size);
        out->text = dup_str(line);
        out->target_section = (idx >= 0 && (uint32_t)idx < script->section_count) ? idx : -1;
        return 1;
    }

    /* enter (0x38) is FRAME SETUP, not a transition: the operand is a locals dword
       COUNT, not a section index. mus_decompile.cpp renders it "enter <resolve_
       section_idx(operand)>" identically to setstate -- a known text-level collapse
       -- so we MUST keep that exact text for byte-identity with the decompiler (and
       the round-trip the golden test pins). But the editor must NOT treat it as a
       navigable/editable transition: no target_section, a distinct kind, and the
       locals count carried in var_offset for a read-only "frame setup" annotation.
       [orig: AudioVM_Op_Enter @0x672C20 copies N dwords into the frame; no IP move.] */
    if (strcmp(m, "enter") == 0) {
        int n_locals = (inst->operand_count > 0) ? inst->operands[0] : 0;
        const char *name = resolve_section_idx(n_locals, script, fallback, sizeof(fallback));
        char line[128];
        snprintf(line, sizeof(line), "enter %s", name);   /* byte-identical to decompile */
        *out = blank_stmt(MUS_AST_FRAME_ENTER, inst->offset, inst->size);
        out->text = dup_str(line);
        out->target_section = -1;        /* NOT a transition target */
        out->var_offset = n_locals;      /* frame locals dword count (read-only display) */
        return 1;
    }

    if (strcmp(m, "play") == 0 || strcmp(m, "playw") == 0) {
        int idx = (inst->operand_count > 0) ? inst->operands[0] : 0;
        int wait = (strcmp(m, "playw") == 0);
        char pname[64], ptok[72], line[96];
        resolve_play_name(idx, NULL, 0, pname, sizeof(pname));   /* names-less */
        format_play_token(pname, ptok, sizeof(ptok));
        snprintf(line, sizeof(line), "play %s", ptok);
        *out = blank_stmt(MUS_AST_PLAY, inst->offset, inst->size);
        out->text = dup_str(line);
        out->track_index = idx;
        out->wait = wait;
        return 1;
    }

    if (strcmp(m, "return") == 0) {
        *out = blank_stmt(MUS_AST_RETURN, inst->offset, inst->size);
        out->text = dup_str("return");
        return 1;
    }
    if (strcmp(m, "yield") == 0) {
        *out = blank_stmt(MUS_AST_YIELD, inst->offset, inst->size);
        out->text = dup_str("yield");
        return 1;
    }
    if (strcmp(m, "done") == 0) {
        *out = blank_stmt(MUS_AST_DONE, inst->offset, inst->size);
        out->text = dup_str("done");   /* emitter renders "}" */
        return 1;
    }
    if (strcmp(m, "nop") == 0) {
        *out = blank_stmt(MUS_AST_NOP, inst->offset, inst->size);
        out->text = dup_str("nop");    /* emitter renders nothing */
        return 1;
    }

    if (strcmp(m, "callvl") == 0) {
        int func = (inst->operand_count > 0) ? inst->operands[0] : 0;
        char line[64];
        snprintf(line, sizeof(line), "call func_%d", func);
        *out = blank_stmt(MUS_AST_CALL, inst->offset, inst->size);
        out->text = dup_str(line);
        return 1;
    }
    if (strcmp(m, "callv") == 0) {
        int target = (inst->operand_count > 0) ? inst->operands[0] : 0;
        char tname[64];
        int sidx = resolve_branch_target(script, (uint32_t)target, suppress_entries,
                                         tname, sizeof(tname));
        char line[128];
        snprintf(line, sizeof(line), "call %s", tname);
        *out = blank_stmt(MUS_AST_CALL, inst->offset, inst->size);
        out->text = dup_str(line);
        out->target_section = sidx;
        return 1;
    }

    if (strcmp(m, "tablexec") == 0) {
        int count    = inst->operand_count > 0 ? inst->operands[0] : 0;
        int inner_op = inst->operand_count > 1 ? inst->operands[1] : 0;
        int es = find_expr_start(insts, end, i);
        char expr[256];
        reconstruct_expression(insts, es, i, script, expr, sizeof(expr));
        if (!expr[0]) snprintf(expr, sizeof(expr), "condition");

        const char *target_type = "enter";
        if (inner_op == 0x3D || inner_op == 0x3E) target_type = "play";
        else if (inner_op == 0x30)                target_type = "goto";
        else                                       target_type = "enter";

        std::vector<MusAstSwitchTarget> tg;
        int es_size = inst->table_entry_size;
        for (int t = 0; t < count; ++t) {
            MusAstSwitchTarget st;
            memset(&st, 0, sizeof(st));
            st.section_index = -1;
            st.track_index = -1;
            const uint8_t *entry = inst->table_data
                                 ? inst->table_data + (size_t)t * es_size : NULL;
            if (entry && inner_op == 0x3B && es_size >= 2) {
                int sidx = entry[1];
                const char *n = resolve_section_idx(sidx, script, st.name, sizeof(st.name));
                snprintf(st.name, sizeof(st.name), "%s", n);
                if (sidx >= 0 && (uint32_t)sidx < script->section_count) st.section_index = sidx;
            } else if (entry && (inner_op == 0x3D || inner_op == 0x3E) && es_size >= 2) {
                int sidx = entry[1];
                if (inner_op == 0x3D && es_size >= 3) sidx = entry[1] | (entry[2] << 8);
                char raw[64];
                resolve_play_name(sidx, NULL, 0, raw, sizeof(raw));   /* names-less */
                format_play_token(raw, st.name, sizeof(st.name));
                st.track_index = sidx;
            } else if (entry && inner_op == 0x30 && es_size >= 5) {
                uint32_t addr = (uint32_t)entry[1]
                              | ((uint32_t)entry[2] << 8)
                              | ((uint32_t)entry[3] << 16)
                              | ((uint32_t)entry[4] << 24);
                int sidx = section_at_offset(script, addr);
                if (sidx >= 0) {
                    snprintf(st.name, sizeof(st.name), "%s", script->sections[sidx].name);
                    st.section_index = sidx;
                } else {
                    snprintf(st.name, sizeof(st.name), "Label_%04X", (unsigned)addr);
                }
            } else {
                snprintf(st.name, sizeof(st.name), "null");
            }
            tg.push_back(st);
        }

        /* Render the full names-less line exactly as mus_decompile.cpp does. */
        char targets[1024];
        targets[0] = 0;
        size_t tpos = 0;
        for (size_t t = 0; t < tg.size(); ++t) {
            const char *tn = tg[t].name;
            size_t tlen = strlen(tn);
            if (t > 0 && tpos + 1 < sizeof(targets)) targets[tpos++] = ' ';
            if (tpos + tlen < sizeof(targets)) { memcpy(targets + tpos, tn, tlen); tpos += tlen; }
        }
        targets[tpos < sizeof(targets) ? tpos : sizeof(targets) - 1] = 0;
        if (!targets[0]) snprintf(targets, sizeof(targets), "null");
        char line[1400];
        snprintf(line, sizeof(line), "on (%s) %s %s", expr, target_type, targets);

        *out = blank_stmt(MUS_AST_SWITCH, inst->offset, inst->size);
        out->text = dup_str(line);
        out->expr_text = dup_str(expr);
        /* An empty reconstruction (text fell back to the "condition" placeholder)
           naturally yields a NULL tree. */
        out->expr_tree = reconstruct_expression_tree(insts, es, i, script);
        out->switch_action = inner_op;
        if (!tg.empty()) {
            out->targets = (MusAstSwitchTarget *)malloc(tg.size() * sizeof(MusAstSwitchTarget));
            if (out->targets) {
                out->target_count = (uint32_t)tg.size();
                for (size_t t = 0; t < tg.size(); ++t) out->targets[t] = tg[t];
            }
        }
        return 1;
    }

    return 0;   /* unknown / unhandled */
}

/* Forward decls. */
static std::vector<MusAstStmt> build_body(const Instruction *insts, int begin, int end,
                                          const MusScript *script);
static void free_stmt(MusAstStmt *s);
static void free_stmt_vector(std::vector<MusAstStmt> &v);

/* Build an IF / IF_ELSE node from the cf block at insts[i] (mirrors
   decompile_block's two CF branches). */
static MusAstStmt build_if_node(const Instruction *insts, int i, int end,
                                const CFBlock *block, const MusScript *script) {
    int es = find_expr_start(insts, end, i);
    char expr[256];
    reconstruct_expression(insts, es, i + 1, script, expr, sizeof(expr));
    if (!expr[0]) snprintf(expr, sizeof(expr), "condition");

    /* byte_size spans the WHOLE control-flow block (condition through the end of
       the else / if), so a live VM pc anywhere inside maps to this IF via
       [code_offset, code_offset+byte_size). code_offset stays the brfalse offset
       (where the condition begins), matching block->start_offset. */
    uint32_t if_size = (block->end_offset > insts[i].offset)
                     ? (block->end_offset - insts[i].offset) : insts[i].size;
    MusAstStmt node = blank_stmt(MUS_AST_IF, insts[i].offset, if_size);
    node.text = dup_str(expr);
    node.expr_text = dup_str(expr);
    node.expr_tree = reconstruct_expression_tree(insts, es, i + 1, script);

    if (block->type == CF_TYPE_IF) {
        int body_begin = -1, body_end_idx = -1;
        for (int k = 0; k < end; ++k) {
            if (insts[k].offset >= block->body_start && body_begin < 0) body_begin = k;
            if (insts[k].offset >= block->body_end && body_end_idx < 0) { body_end_idx = k; break; }
        }
        if (body_begin >= 0) {
            if (body_end_idx < 0) body_end_idx = end;
            std::vector<MusAstStmt> then_v = build_body(insts, body_begin, body_end_idx, script);
            if (!then_v.empty()) {
                node.then_body = (MusAstStmt *)malloc(then_v.size() * sizeof(MusAstStmt));
                if (node.then_body) {
                    node.then_count = (uint32_t)then_v.size();
                    for (size_t t = 0; t < then_v.size(); ++t) node.then_body[t] = then_v[t];
                } else {
                    free_stmt_vector(then_v);   /* OOM: don't leak the dup_str'd fields */
                }
            }
        }
    } else { /* CF_TYPE_IF_ELSE */
        int if_begin = -1, if_end = -1;
        for (int k = 0; k < end; ++k) {
            if (insts[k].offset >= block->body_start && if_begin < 0) if_begin = k;
            if (insts[k].offset >= (block->else_offset - 5) && if_end < 0) { if_end = k; break; }
        }
        if (if_begin >= 0) {
            if (if_end < 0) if_end = end;
            std::vector<MusAstStmt> then_v = build_body(insts, if_begin, if_end, script);
            if (!then_v.empty()) {
                node.then_body = (MusAstStmt *)malloc(then_v.size() * sizeof(MusAstStmt));
                if (node.then_body) {
                    node.then_count = (uint32_t)then_v.size();
                    for (size_t t = 0; t < then_v.size(); ++t) node.then_body[t] = then_v[t];
                } else {
                    free_stmt_vector(then_v);
                }
            }
        }
        int el_begin = -1, el_end = -1;
        for (int k = 0; k < end; ++k) {
            if (insts[k].offset >= block->else_offset && el_begin < 0) el_begin = k;
            if (insts[k].offset >= block->end_offset && el_end < 0) { el_end = k; break; }
        }
        if (el_begin >= 0) {
            if (el_end < 0) el_end = end;
            std::vector<MusAstStmt> else_v = build_body(insts, el_begin, el_end, script);
            /* An empty else still distinguishes if-else from plain-if structurally;
               keep else_count from the materialised vector (decompiler always emits
               the else braces for an IF_ELSE block). */
            node.else_body = (MusAstStmt *)malloc((else_v.empty() ? 1 : else_v.size())
                                                  * sizeof(MusAstStmt));
            if (node.else_body) {
                node.else_count = (uint32_t)else_v.size();
                for (size_t t = 0; t < else_v.size(); ++t) node.else_body[t] = else_v[t];
            } else {
                free_stmt_vector(else_v);
            }
        } else {
            /* Mark as if-else with an empty else so the emitter still prints the
               else braces (matches decompile_block, which always does for IF_ELSE). */
            node.else_body = (MusAstStmt *)malloc(sizeof(MusAstStmt));
            node.else_count = 0;
        }
    }
    return node;
}

/* Linear body builder for if/switch bodies: no section detection, no nested CF
   detection (mirrors decompile_block with suppress_entries=1). */
static std::vector<MusAstStmt> build_body(const Instruction *insts, int begin, int end,
                                          const MusScript *script) {
    std::vector<MusAstStmt> out;
    for (int i = begin; i < end; ++i) {
        MusAstStmt node;
        /* suppress_entries=1: bodies match decompile_block's recursive call. */
        if (try_make_node(insts, i, end, script, /*suppress_entries=*/1, &node)) out.push_back(node);
    }
    return out;
}

/* Free a statement vector's heap fields (used when a materialising malloc fails
   so the elements' dup_str'd strings don't leak). */
static void free_stmt_vector(std::vector<MusAstStmt> &v) {
    for (size_t i = 0; i < v.size(); ++i) free_stmt(&v[i]);
}

static void free_stmt(MusAstStmt *s);

static void free_stmt_array(MusAstStmt *arr, uint32_t count) {
    if (!arr) return;
    for (uint32_t i = 0; i < count; ++i) free_stmt(&arr[i]);
    free(arr);
}

static void free_stmt(MusAstStmt *s) {
    if (!s) return;
    free(s->text);
    free(s->var_name);
    free(s->rhs_text);
    free(s->expr_text);
    free(s->call_name);
    free(s->targets);
    expr_tree_free_rec(s->rhs_tree);
    expr_tree_free_rec(s->expr_tree);
    free_stmt_array(s->then_body, s->then_count);
    free_stmt_array(s->else_body, s->else_count);
}

} // namespace

/* ---- Structured expression tree: public render/free -------------------- */

extern "C" void mus_expr_free(MusAstExpr *expr) {
    expr_tree_free_rec(expr);
}

/* Render with the SAME per-level 256-char buffers and formats as
   reconstruct_expression, so render(tree) reproduces the flat text
   byte-for-byte (pinned by mus_expr_tree_test.cpp). */
extern "C" int mus_expr_render(const MusAstExpr *expr, char *out, size_t out_capacity) {
    if (!out || out_capacity == 0) return -1;
    if (!expr) {
        out[0] = 0;
        return 0;
    }
    char a[256], b[256];
    switch (expr->kind) {
        case MUS_EXPR_LITERAL:
            return snprintf(out, out_capacity, "%d", expr->value);
        case MUS_EXPR_ME:
            return snprintf(out, out_capacity, "Me");
        case MUS_EXPR_VARREF:
            if (strcmp(expr->var_form, "Var") == 0)
                return snprintf(out, out_capacity, "Var%02d", expr->var_index);
            if (strcmp(expr->var_form, "g") == 0)
                return snprintf(out, out_capacity, "g_%d", expr->var_index);
            if (strcmp(expr->var_form, "l") == 0)
                return snprintf(out, out_capacity, "l_%d", expr->var_index);
            return snprintf(out, out_capacity, "%s", expr->name);
        case MUS_EXPR_RAW:
            return snprintf(out, out_capacity, "%s", expr->name);
        case MUS_EXPR_UNOP:
            mus_expr_render(expr->left, a, sizeof(a));
            return snprintf(out, out_capacity, "%s%s", expr->op, a);
        case MUS_EXPR_BINOP:
            mus_expr_render(expr->left, a, sizeof(a));
            mus_expr_render(expr->right, b, sizeof(b));
            return snprintf(out, out_capacity, "(%s %s %s)", a, expr->op, b);
        case MUS_EXPR_CALL: {
            char obj[16], mth[64];
            split_method_name(expr->name, obj, sizeof(obj), mth, sizeof(mth));
            if (expr->left) {
                mus_expr_render(expr->left, a, sizeof(a));
                if (obj[0]) return snprintf(out, out_capacity, "%s.%s(%s)", obj, mth, a);
                return snprintf(out, out_capacity, "%s(%s)", mth, a);
            }
            if (obj[0]) return snprintf(out, out_capacity, "%s.%s()", obj, mth);
            return snprintf(out, out_capacity, "%s()", mth);
        }
        default:
            out[0] = 0;
            return 0;
    }
}

extern "C" MusAstProgram *mus_parse_to_ast(const MusScript *script) {
    if (!script) return NULL;

    MusAstProgram *prog = (MusAstProgram *)calloc(1, sizeof(MusAstProgram));
    if (!prog) return NULL;

    memcpy(prog->name, script->name, sizeof(prog->name) < sizeof(script->name)
           ? sizeof(prog->name) : sizeof(script->name));
    snprintf(prog->source_path, sizeof(prog->source_path), "%s", script->source_path);
    prog->globals_size = script->globals_size;
    prog->entry_section_index = (int)script->entry_section_index;
    prog->intrinsic_count = script->intrinsic_count;
    for (uint32_t i = 0; i < script->intrinsic_count && i < (uint32_t)MUS_INTRINSIC_NAMES; ++i) {
        memcpy(prog->intrinsic_names[i], script->intrinsic_names[i], MUS_INTRINSIC_NAME_SIZE);
    }

    /* Copy named globals so the emitter can resolve user-global declarations. */
    if (script->variable_count > 0 && script->variables) {
        prog->variables = (MusVariable *)calloc(script->variable_count, sizeof(MusVariable));
        if (prog->variables) {
            prog->variable_count = script->variable_count;
            memcpy(prog->variables, script->variables,
                   script->variable_count * sizeof(MusVariable));
        }
    }

    /* Disassemble. */
    int n = 0;
    Instruction *insts = NULL;
    if (script->code && script->code_size > 0) {
        insts = (Instruction *)calloc(script->code_size, sizeof(Instruction));
        if (!insts) { mus_program_free(prog); return NULL; }
        disassemble(script->code, script->code_size, insts, (int)script->code_size, &n);
    }

    /* First pass: max_play_index + globals_used (sorted), mirroring the
       decompiler's header scan. */
    int max_play_idx = -1;
    std::vector<int> gused;
    for (int i = 0; i < n; ++i) {
        const char *m = insts[i].mnemonic;
        if (!m) continue;
        if (strcmp(m, "play") == 0 || strcmp(m, "playw") == 0) {
            if (insts[i].operand_count > 0 && insts[i].operands[0] > max_play_idx)
                max_play_idx = insts[i].operands[0];
        } else if (strcmp(m, "push_g") == 0 || strcmp(m, "pop_g") == 0
                || strcmp(m, "inc_g") == 0   || strcmp(m, "dec_g") == 0
                || strcmp(m, "push_ga") == 0 || strcmp(m, "pop_ga") == 0) {
            if (insts[i].operand_count > 0) {
                int g = insts[i].operands[0];
                int seen = 0;
                for (size_t k = 0; k < gused.size(); ++k) if (gused[k] == g) { seen = 1; break; }
                if (!seen) gused.push_back(g);
            }
        }
    }
    for (size_t i = 0; i < gused.size(); ++i)
        for (size_t j = i + 1; j < gused.size(); ++j)
            if (gused[j] < gused[i]) { int t = gused[i]; gused[i] = gused[j]; gused[j] = t; }
    prog->max_play_index = max_play_idx;
    if (!gused.empty()) {
        prog->globals_used = (int *)malloc(gused.size() * sizeof(int));
        if (prog->globals_used) {
            prog->globals_used_count = (uint32_t)gused.size();
            for (size_t i = 0; i < gused.size(); ++i) prog->globals_used[i] = gused[i];
        }
    }

    /* Section shells, in script index order. */
    std::vector<std::vector<MusAstStmt>> work(script->section_count);

    /* Control-flow analysis over the whole stream (top-level if/if_else). */
    CFMap cf{};
    if (n > 0) analyze_control_flow(insts, n, &cf);

    /* Top-level walk: attribute each statement to its owning section. */
    int i = 0;
    while (i < n) {
        const Instruction *inst = &insts[i];
        if (!inst->mnemonic) { ++i; continue; }
        int owner = owner_section(script, inst->offset);
        if (owner < 0) owner = 0;

        const CFBlock *block = cf_get(&cf, inst->offset);
        if (block && (block->type == CF_TYPE_IF || block->type == CF_TYPE_IF_ELSE)) {
            MusAstStmt node = build_if_node(insts, i, n, block, script);
            if ((size_t)owner < work.size()) work[owner].push_back(node);
            else free_stmt(&node);
            while (i < n && insts[i].offset < block->end_offset) ++i;
            continue;
        }

        MusAstStmt node;
        if (try_make_node(insts, i, n, script, /*suppress_entries=*/0, &node)) {
            if ((size_t)owner < work.size()) work[owner].push_back(node);
            else free_stmt(&node);
        }
        ++i;
    }

    cf_free(&cf);

    /* Materialise sections. */
    if (script->section_count > 0) {
        prog->sections = (MusAstSection *)calloc(script->section_count, sizeof(MusAstSection));
        if (!prog->sections) {
            if (insts) { free_instructions(insts, n); free(insts); }
            mus_program_free(prog);
            return NULL;
        }
        prog->section_count = script->section_count;
        for (uint32_t s = 0; s < script->section_count; ++s) {
            MusAstSection &sec = prog->sections[s];
            snprintf(sec.name, sizeof(sec.name), "%s", script->sections[s].name);
            sec.section_index = (int)s;
            sec.is_entry = (s == script->entry_section_index) ? 1 : 0;
            sec.code_offset = script->sections[s].code_offset;
            std::vector<MusAstStmt> &v = work[s];
            sec.statement_count = (uint32_t)v.size();
            if (!v.empty()) {
                sec.statements = (MusAstStmt *)malloc(v.size() * sizeof(MusAstStmt));
                if (sec.statements) {
                    for (size_t t = 0; t < v.size(); ++t) sec.statements[t] = v[t];
                } else {
                    sec.statement_count = 0;
                    for (size_t t = 0; t < v.size(); ++t) free_stmt(&v[t]);
                }
            }
        }
    }

    if (insts) { free_instructions(insts, n); free(insts); }
    return prog;
}

extern "C" void mus_program_free(MusAstProgram *program) {
    if (!program) return;
    if (program->sections) {
        for (uint32_t s = 0; s < program->section_count; ++s) {
            free_stmt_array(program->sections[s].statements,
                            program->sections[s].statement_count);
        }
        free(program->sections);
    }
    free(program->globals_used);
    free(program->variables);
    free(program);
}

/* ---------------------------------------------------------------------------
   Emitter: walk the tree back to .mus text, byte-identical to mus_decompile.
   --------------------------------------------------------------------------- */

namespace {

struct EBuf { char *p; size_t cap; size_t used; size_t line; };

static void e_putc(EBuf *b, char c) {
    if (b->p && b->used + 1 < b->cap) b->p[b->used] = c;
    ++b->used;
    if (c == '\n') ++b->line;   /* track the current 0-based line for span emit */
}
static void e_puts(EBuf *b, const char *s) { while (*s) e_putc(b, *s++); }
static void e_printf(EBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int nn = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (nn < 0) return;
    for (int i = 0; i < nn && i < (int)sizeof(tmp); ++i) e_putc(b, tmp[i]);
}
static void e_indent(EBuf *b, int indent) { for (int k = 0; k < indent; ++k) e_puts(b, "    "); }

/* Render a play target with the names table (or names-less fallback). */
static void emit_play_token(int track, const char *const *names, uint32_t name_count,
                            char *out, size_t cap) {
    char raw[96];
    resolve_play_name(track, names, name_count, raw, sizeof(raw));
    format_play_token(raw, out, cap);
}

static void emit_stmt_list(EBuf *b, const MusAstStmt *stmts, uint32_t count, int indent,
                           const char *const *names, uint32_t name_count);

static void emit_stmt(EBuf *b, const MusAstStmt *s, int indent,
                      const char *const *names, uint32_t name_count) {
    switch (s->kind) {
        case MUS_AST_NOP:
            /* decompile renders nothing for nop */
            return;
        case MUS_AST_DONE:
            e_puts(b, "}\n");   /* not indented, matches decompile */
            return;
        case MUS_AST_PLAY: {
            char ptok[128];
            emit_play_token(s->track_index, names, name_count, ptok, sizeof(ptok));
            e_indent(b, indent);
            e_printf(b, "play %s\n", ptok);
            return;
        }
        case MUS_AST_IF: {
            e_indent(b, indent);
            e_printf(b, "if (%s)\n", s->expr_text ? s->expr_text : "condition");
            e_indent(b, indent);
            e_puts(b, "{\n");
            emit_stmt_list(b, s->then_body, s->then_count, indent + 1, names, name_count);
            e_indent(b, indent);
            e_puts(b, "}\n");
            if (s->else_body != NULL) {  /* IF_ELSE always carries an else block */
                e_indent(b, indent);
                e_puts(b, "else\n");
                e_indent(b, indent);
                e_puts(b, "{\n");
                emit_stmt_list(b, s->else_body, s->else_count, indent + 1, names, name_count);
                e_indent(b, indent);
                e_puts(b, "}\n");
            }
            return;
        }
        case MUS_AST_SWITCH: {
            /* enter/goto switches are names-independent; re-render play switches
               with the bank names. */
            if (s->switch_action == 0x3D || s->switch_action == 0x3E) {
                char targets[1024];
                targets[0] = 0;
                size_t tpos = 0;
                for (uint32_t t = 0; t < s->target_count; ++t) {
                    char tok[128];
                    emit_play_token(s->targets[t].track_index, names, name_count,
                                    tok, sizeof(tok));
                    size_t tlen = strlen(tok);
                    if (t > 0 && tpos + 1 < sizeof(targets)) targets[tpos++] = ' ';
                    if (tpos + tlen < sizeof(targets)) { memcpy(targets + tpos, tok, tlen); tpos += tlen; }
                }
                targets[tpos < sizeof(targets) ? tpos : sizeof(targets) - 1] = 0;
                if (!targets[0]) snprintf(targets, sizeof(targets), "null");
                e_indent(b, indent);
                e_printf(b, "on (%s) play %s\n", s->expr_text ? s->expr_text : "condition", targets);
            } else {
                e_indent(b, indent);
                e_printf(b, "%s\n", s->text ? s->text : "");
            }
            return;
        }
        default:
            e_indent(b, indent);
            e_printf(b, "%s\n", s->text ? s->text : "");
            return;
    }
}

static void emit_stmt_list(EBuf *b, const MusAstStmt *stmts, uint32_t count, int indent,
                           const char *const *names, uint32_t name_count) {
    for (uint32_t i = 0; i < count; ++i) emit_stmt(b, &stmts[i], indent, names, name_count);
}

/* Shared emit core: header + bind/global/declsection blocks + section bodies, in
   the exact order and form mus_decompile produces (the byte-identity is pinned by
   mus_ast_test.cpp). When `spans` is non-NULL, each top-level statement's
   [line_start, line_end) line range is appended in emission order -- the editor's
   Phase-2 authoring uses it to address a single statement for splice/delete. */
static void emit_program(const MusAstProgram *prog,
                         const char *const *sbf_names, uint32_t sbf_name_count,
                         EBuf *b, std::vector<MusStmtLineSpan> *spans) {
    /* ---- Header ---- */
    e_printf(b, "// Script: %s\n", prog->name);
    if (prog->source_path[0]) e_printf(b, "// Original source: %s\n", prog->source_path);
    e_puts(b, "//\n");
    e_puts(b, "// IMPORTANT: Load MUSIC.LAN before opening this file in MDEdit\n");
    e_puts(b, "// The following methods are used: ");
    {
        int first = 1;
        for (uint32_t i = 0; i < prog->intrinsic_count; ++i) {
            const char *combined = prog->intrinsic_names[i];
            if (!combined[0] || combined[0] == '@') continue;
            char obj[16], mth[64];
            split_method_name(combined, obj, sizeof(obj), mth, sizeof(mth));
            if (!first) e_puts(b, ", ");
            first = 0;
            if (obj[0]) e_printf(b, "%s.%s", obj, mth);
            else        e_printf(b, "%s", combined);
        }
    }
    e_putc(b, '\n');
    e_putc(b, '\n');

    e_printf(b, "script %s\n\n", prog->name);

    /* Bind declarations. */
    if (prog->max_play_index >= 0) {
        e_puts(b, "// Sound bind declarations (play requires bind identifiers)\n");
        for (int k = 0; k <= prog->max_play_index; ++k) {
            char qname[96];
            resolve_play_name(k, sbf_names, sbf_name_count, qname, sizeof(qname));
            e_printf(b, "bind sound_%d \"%s\"\n", k, qname);
        }
        e_putc(b, '\n');
    }

    /* Global variables block. */
    if (prog->globals_used_count > 0) {
        e_puts(b, "// Global variables\n");
        e_puts(b, "// Used global offsets from original: [");
        for (uint32_t k = 0; k < prog->globals_used_count; ++k) {
            if (k > 0) e_puts(b, ", ");
            e_printf(b, "%d", prog->globals_used[k]);
        }
        e_puts(b, "]\n");
        e_printf(b, "// Original globals_area = %u bytes\n", prog->globals_size);
        e_puts(b, "// NOTE: MDEdit pre-defines Var00-Var15 at offsets 0-60 (64 bytes)\n");

        int max_user = INT32_MIN;
        for (uint32_t k = 0; k < prog->globals_used_count; ++k)
            if (prog->globals_used[k] >= 64 && prog->globals_used[k] > max_user)
                max_user = prog->globals_used[k];
        if (max_user >= 64) {
            for (int off = 64; off <= max_user; off += 4) {
                int found = 0;
                for (uint32_t k = 0; k < prog->globals_used_count; ++k)
                    if (prog->globals_used[k] == off) { found = 1; break; }
                if (found) {
                    char vname[64];
                    resolve_global_into(vname, sizeof(vname), off,
                                        prog->variables, prog->variable_count);
                    e_printf(b, "global INT %s\n", vname);
                } else {
                    e_printf(b, "global INT _pad_%d\n", off / 4);
                }
            }
        }
        e_putc(b, '\n');
    }

    /* declsection forward declarations (script index order). */
    if (prog->section_count > 0) {
        e_puts(b, "// Section forward declarations\n");
        for (uint32_t s = 0; s < prog->section_count; ++s)
            e_printf(b, "declsection %s\n", prog->sections[s].name);
        e_putc(b, '\n');
    }

    /* ---- Section bodies, in code-offset order (reproduces the decompiler's
       linear emission, including tail code leaked past a `done`). ---- */
    if (prog->section_count > 0) {
        std::vector<uint32_t> order(prog->section_count);
        for (uint32_t s = 0; s < prog->section_count; ++s) order[s] = s;
        for (size_t a = 0; a < order.size(); ++a)
            for (size_t c = a + 1; c < order.size(); ++c)
                if (prog->sections[order[c]].code_offset < prog->sections[order[a]].code_offset) {
                    uint32_t t = order[a]; order[a] = order[c]; order[c] = t;
                }
        for (size_t oi = 0; oi < order.size(); ++oi) {
            const MusAstSection &sec = prog->sections[order[oi]];
            e_putc(b, '\n');
            e_printf(b, "section %s\n", sec.name);
            e_puts(b, "{\n");
            /* Emit each top-level statement, recording its line span when asked.
               (NOP emits nothing, so its span is empty -- it has no editable text,
               matching the decompiler dropping it.) */
            for (uint32_t k = 0; k < sec.statement_count; ++k) {
                int ls = (int)b->line;
                emit_stmt(b, &sec.statements[k], 0, sbf_names, sbf_name_count);
                if (spans) {
                    MusStmtLineSpan sp;
                    sp.section_index = sec.section_index;
                    sp.ordinal       = (int)k;
                    sp.code_offset   = sec.statements[k].code_offset;
                    sp.kind          = sec.statements[k].kind;
                    sp.line_start    = ls;
                    sp.line_end      = (int)b->line;
                    spans->push_back(sp);
                }
            }
        }
    }
}

} // namespace

extern "C" int mus_ast_emit_text(const MusAstProgram *prog,
                                 const char *const *sbf_names, uint32_t sbf_name_count,
                                 char *out_text, size_t out_capacity) {
    if (!prog) return -1;
    EBuf b; b.p = out_text; b.cap = out_capacity; b.used = 0; b.line = 0;
    emit_program(prog, sbf_names, sbf_name_count, &b, NULL);
    if (b.p && b.cap > 0) {
        size_t end = b.used < b.cap ? b.used : b.cap - 1;
        b.p[end] = 0;
    }
    return (int)b.used;
}

extern "C" int mus_ast_emit_text_spans(const MusAstProgram *prog,
                                       const char *const *sbf_names, uint32_t sbf_name_count,
                                       char **out_text,
                                       MusStmtLineSpan **out_spans, uint32_t *out_span_count) {
    if (!prog || !out_text || !out_spans || !out_span_count) return -1;
    *out_text = NULL;
    *out_spans = NULL;
    *out_span_count = 0;

    /* Sizing pass (no buffer): get the byte length so we can allocate exactly. */
    EBuf sz; sz.p = NULL; sz.cap = 0; sz.used = 0; sz.line = 0;
    emit_program(prog, sbf_names, sbf_name_count, &sz, NULL);

    size_t need = sz.used + 1;
    char *text = (char *)malloc(need);
    if (!text) return -1;

    /* Write pass with span collection. */
    EBuf b; b.p = text; b.cap = need; b.used = 0; b.line = 0;
    std::vector<MusStmtLineSpan> spans;
    emit_program(prog, sbf_names, sbf_name_count, &b, &spans);
    {
        size_t end = b.used < b.cap ? b.used : b.cap - 1;
        text[end] = 0;
    }

    MusStmtLineSpan *arr = NULL;
    if (!spans.empty()) {
        arr = (MusStmtLineSpan *)malloc(spans.size() * sizeof(MusStmtLineSpan));
        if (!arr) { free(text); return -1; }
        for (size_t i = 0; i < spans.size(); ++i) arr[i] = spans[i];
    }
    *out_text = text;
    *out_spans = arr;
    *out_span_count = (uint32_t)spans.size();
    return 0;
}
