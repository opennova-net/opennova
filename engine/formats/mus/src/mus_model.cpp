/* MUS bytecode -> structural section model (editor-facing, read-only).

   Companion to mus_decompile.cpp. Where the decompiler renders the bytecode as
   .mus source TEXT (and therefore collapses `setstate` 0x3B and `enter` 0x38 to
   the same `enter` token), this builds a STRUCTURAL model that reads the opcode
   directly, so the state-machine topology (which section moves to which, and how)
   is exact. It is the single source of truth for the editor's section map and
   structured views, replacing the previous approach of string-parsing the
   decompiled text (which scanned for a `setstate ` token the decompiler never
   emits, yielding an empty graph for every shipped script).

   Decoding reuses the shared mus_decode.h so opcode widths never drift from the
   decompiler. Read-only: never mutates bytecode, independent of the compile path. */

#include "mus/mus.h"

#include "mus_decode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

/* Find the section that owns an instruction at `offset`: the section with the
   greatest code_offset <= offset. Falls back to the section with the smallest
   code_offset for any instruction that precedes every section entry (rare). */
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

/* Section index whose code_offset == addr, or -1. */
static int section_at_offset(const MusScript *s, uint32_t addr) {
    for (uint32_t i = 0; i < s->section_count; ++i) {
        if (s->sections[i].code_offset == addr) return (int)i;
    }
    return -1;
}

struct WorkSection {
    std::vector<MusSectionEdge> edges;
    std::vector<MusSectionPlay> plays;
};

/* Append an edge, deduped by target: a target already present keeps the
   strongest (numerically smallest) kind, so TRANSITION beats SWITCH beats
   BRANCH and the graph shows one arrow per (from, to) pair. */
static void add_edge(WorkSection &ws, uint32_t to, int kind) {
    for (auto &e : ws.edges) {
        if (e.to_section_index == to) {
            if (kind < e.kind) e.kind = kind;
            return;
        }
    }
    MusSectionEdge e;
    e.to_section_index = to;
    e.kind = kind;
    ws.edges.push_back(e);
}

} // namespace

extern "C" int mus_build_section_model(const MusScript *script, MusModel *out) {
    if (!script || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (script->section_count == 0) return 0; /* empty but valid */

    /* Decode the whole code buffer. Each instruction is >= 1 byte, so code_size
       is a safe upper bound on the instruction count. */
    uint32_t cap = script->code_size ? script->code_size : 1;
    Instruction *insts = (Instruction *)calloc(cap, sizeof(Instruction));
    if (!insts) return -2;
    int n = 0;
    if (script->code && script->code_size > 0) {
        disassemble(script->code, script->code_size, insts, (int)cap, &n);
    }

    std::vector<WorkSection> work(script->section_count);

    for (int i = 0; i < n; ++i) {
        const Instruction *inst = &insts[i];
        if (!inst->mnemonic) continue;
        int owner = owner_section(script, inst->offset);
        if (owner < 0) continue;
        WorkSection &ws = work[(size_t)owner];
        const char *m = inst->mnemonic;

        if (strcmp(m, "setstate") == 0) {
            /* 0x3B: operand is a section index (op_setstate indexes sections[idx]). */
            int sidx = (inst->operand_count > 0) ? inst->operands[0] : -1;
            if (sidx >= 0 && (uint32_t)sidx < script->section_count) {
                add_edge(ws, (uint32_t)sidx, MUS_EDGE_TRANSITION);
            }
        } else if (strcmp(m, "play") == 0 || strcmp(m, "playw") == 0) {
            MusSectionPlay p;
            p.track_index = (uint32_t)((inst->operand_count > 0) ? inst->operands[0] : 0);
            p.wait = (strcmp(m, "playw") == 0) ? 1 : 0;
            ws.plays.push_back(p);
        } else if (strcmp(m, "goto") == 0 || strcmp(m, "brfalse") == 0
                || strcmp(m, "brtrue") == 0) {
            /* Branch target is a code offset; an edge only when it lands on a
               section entry (intra-section branches are not topology edges). */
            uint32_t target = (uint32_t)((inst->operand_count > 0) ? inst->operands[0] : 0);
            int tidx = section_at_offset(script, target);
            if (tidx >= 0) add_edge(ws, (uint32_t)tidx, MUS_EDGE_BRANCH);
        } else if (strcmp(m, "tablexec") == 0) {
            /* 0x35: a switch. Decode entries the same way mus_decompile.cpp does:
               inner_op (operand[1]) selects how each entry's bytes are read. */
            int inner_op = (inst->operand_count > 1) ? inst->operands[1] : 0;
            int es = inst->table_entry_size;
            for (int t = 0; t < inst->table_count && inst->table_data; ++t) {
                const uint8_t *entry = inst->table_data + (size_t)t * es;
                if (inner_op == 0x3B && es >= 2) {
                    int sidx = entry[1];
                    if (sidx >= 0 && (uint32_t)sidx < script->section_count) {
                        add_edge(ws, (uint32_t)sidx, MUS_EDGE_SWITCH);
                    }
                } else if (inner_op == 0x30 && es >= 5) {
                    uint32_t addr = (uint32_t)entry[1]
                                  | ((uint32_t)entry[2] << 8)
                                  | ((uint32_t)entry[3] << 16)
                                  | ((uint32_t)entry[4] << 24);
                    int tidx = section_at_offset(script, addr);
                    if (tidx >= 0) add_edge(ws, (uint32_t)tidx, MUS_EDGE_SWITCH);
                } else if ((inner_op == 0x3D || inner_op == 0x3E) && es >= 2) {
                    MusSectionPlay p;
                    int sidx = entry[1];
                    if (inner_op == 0x3D && es >= 3) sidx = entry[1] | (entry[2] << 8);
                    p.track_index = (uint32_t)sidx;
                    p.wait = (inner_op == 0x3D) ? 1 : 0;
                    ws.plays.push_back(p);
                }
            }
        }
        /* enter (0x38) is intentionally NOT an edge: it is frame setup (pops N
           dwords into locals), not a transition. This is the key fidelity point
           the decompiled text cannot express. */
    }

    /* Materialise into the C output structs. */
    out->section_count = script->section_count;
    out->sections = (MusSectionInfo *)calloc(script->section_count, sizeof(MusSectionInfo));
    if (!out->sections) { free_instructions(insts, n); free(insts); return -2; }

    for (uint32_t i = 0; i < script->section_count; ++i) {
        MusSectionInfo &si = out->sections[i];
        WorkSection &ws = work[i];
        si.section_index = i;
        si.is_entry = (i == script->entry_section_index) ? 1 : 0;

        si.edge_count = (uint32_t)ws.edges.size();
        if (si.edge_count) {
            si.edges = (MusSectionEdge *)calloc(si.edge_count, sizeof(MusSectionEdge));
            if (si.edges) {
                for (uint32_t e = 0; e < si.edge_count; ++e) si.edges[e] = ws.edges[e];
            } else {
                si.edge_count = 0;
            }
        }

        si.play_count = (uint32_t)ws.plays.size();
        if (si.play_count) {
            si.plays = (MusSectionPlay *)calloc(si.play_count, sizeof(MusSectionPlay));
            if (si.plays) {
                for (uint32_t p = 0; p < si.play_count; ++p) si.plays[p] = ws.plays[p];
            } else {
                si.play_count = 0;
            }
        }

        /* Idle loop: at least one edge and every edge points back at itself. */
        si.is_idle_loop = 0;
        if (si.edge_count > 0) {
            int all_self = 1;
            for (uint32_t e = 0; e < si.edge_count; ++e) {
                if (si.edges[e].to_section_index != i) { all_self = 0; break; }
            }
            si.is_idle_loop = all_self;
        }
    }

    free_instructions(insts, n);
    free(insts);
    return 0;
}

extern "C" void mus_model_free(MusModel *model) {
    if (!model) return;
    if (model->sections) {
        for (uint32_t i = 0; i < model->section_count; ++i) {
            free(model->sections[i].edges);
            free(model->sections[i].plays);
        }
        free(model->sections);
    }
    memset(model, 0, sizeof(*model));
}
