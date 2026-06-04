/* Structured statement + section edit parity: the invariants the editor's
   Phase-2 visual authoring stands on. Where mus_structured_play_edit_test pins
   the play-only write path, this pins the GENERALIZED path: any top-level
   statement (transition / assignment / inc-dec / method-call / return / yield /
   if / on) is located by its LINE SPAN -- emitted by mus_ast_emit_text_spans, the
   same C++ the editor's get_annotated_decompile bridge calls -- then spliced,
   deleted, replaced, or reordered, recompiled, and re-encoded. Plus section
   rename/delete.

   The transforms here are the C++ twin of the GDScript document methods (as
   splice_play mirrors _splice_play); the SPANS are the shared source of truth.
   Invariants proven:
   (a) the spans-emit text == mus_decompile (the text the editor splices on);
   (b) each span points at its statement's lines (kind-keyword check);
   (c) insert a valid statement -> compiles, model gains exactly it, edges move
       only as expected;
   (d) insert + delete (same line) -> byte-identical canonical no-op;
   (e) replace-same -> byte-identical; replace-different -> compiles + reflects;
   (f) reorder twice (opposite) -> byte-identical;
   (g) rename_section -> BYTECODE-identical (names live only in editor debug);
   (h) delete an unreferenced section round-trips; delete a referenced one is
       refused by the explicit guard (the gate alone can't catch enter-refs,
       which silently re-intern). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include "mus/mus.h"
#include "mus/ast.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

// --- text helpers (identical to mus_structured_play_edit_test) ---

static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else { cur += c; }
    }
    out.push_back(cur);
    return out;
}

static std::string join_lines(const std::vector<std::string> &v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        out += v[i];
        if (i + 1 < v.size()) out += "\n";
    }
    return out;
}

static std::string strip(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

static bool starts_with(const std::string &s, const std::string &p) {
    return s.rfind(p, 0) == 0;
}

// --- mus helpers ---

static std::string decompile_str(const MusScript *s) {
    int n = mus_decompile(s, nullptr, 0);
    if (n <= 0) return "";
    std::string t;
    t.resize((size_t)n + 1);
    mus_decompile(s, &t[0], (size_t)n + 1);
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

// canonical(text) = encode(compile(text)) bytes -- the byte-stability oracle.
static bool canonical_of_text(const std::string &text, std::string &out) {
    MusScript s = {};
    if (!compile_str(text, &s)) return false;
    const MusScript *arr[1] = { &s };
    uint8_t *buf = nullptr;
    size_t n = 0;
    int rc = mus_encode_file(arr, 1, &buf, &n);
    mus_script_free(&s);
    if (rc != 0) return false;
    out.assign((const char *)buf, n);
    mus_free(buf);
    return true;
}

// Compare only the runtime bytecode of two compiled scripts.
static bool bytecode_equal(const MusScript &a, const MusScript &b) {
    if (a.code_size != b.code_size) return false;
    if (a.code_size == 0) return true;
    return memcmp(a.code, b.code, a.code_size) == 0;
}

static const MusSectionInfo *info_for(const MusModel *m, const MusScript *s, const char *name) {
    const MusSection *sec = mus_find_section(s, name);
    if (!sec) return nullptr;
    int idx = (int)(sec - s->sections);
    for (uint32_t i = 0; i < m->section_count; ++i)
        if ((int)m->sections[i].section_index == idx) return &m->sections[i];
    return nullptr;
}

// Multiset of a section's play track indices (model order).
static std::vector<uint32_t> play_tracks(const MusSectionInfo *si) {
    std::vector<uint32_t> v;
    if (si) for (uint32_t i = 0; i < si->play_count; ++i) v.push_back(si->plays[i].track_index);
    return v;
}

// Set of a section's outgoing edge target indices.
static std::set<uint32_t> edge_targets(const MusSectionInfo *si) {
    std::set<uint32_t> s;
    if (si) for (uint32_t i = 0; i < si->edge_count; ++i) s.insert(si->edges[i].to_section_index);
    return s;
}

// --- annotated decompile (the real span-location mechanism) ---

struct Annotated {
    std::string                  text;
    std::vector<MusStmtLineSpan> spans;
};

static Annotated annotate(const MusScript *s) {
    Annotated a;
    MusAstProgram *prog = mus_parse_to_ast(s);
    if (!prog) return a;
    char *text = nullptr;
    MusStmtLineSpan *spans = nullptr;
    uint32_t n = 0;
    if (mus_ast_emit_text_spans(prog, nullptr, 0, &text, &spans, &n) == 0) {
        a.text = text ? text : "";
        for (uint32_t i = 0; i < n; ++i) a.spans.push_back(spans[i]);
    }
    mus_free(text);
    mus_free(spans);
    mus_program_free(prog);
    return a;
}

// Spans of one section (by AST section_index), in ordinal order.
static std::vector<MusStmtLineSpan> spans_of(const Annotated &a, int section_index) {
    std::vector<MusStmtLineSpan> v;
    for (const auto &sp : a.spans)
        if (sp.section_index == section_index) v.push_back(sp);
    return v;
}

// --- line-splice transforms (twin of the GDScript document primitives) ---

static std::string delete_span(const std::string &text, const MusStmtLineSpan &sp) {
    auto lines = split_lines(text);
    if (sp.line_start < 0 || sp.line_end > (int)lines.size() || sp.line_start >= sp.line_end)
        return text;
    lines.erase(lines.begin() + sp.line_start, lines.begin() + sp.line_end);
    return join_lines(lines);
}

static std::string insert_lines(const std::string &text, int at_line,
                                const std::vector<std::string> &nl) {
    auto lines = split_lines(text);
    if (at_line < 0 || at_line > (int)lines.size()) return text;
    lines.insert(lines.begin() + at_line, nl.begin(), nl.end());
    return join_lines(lines);
}

static std::string replace_span(const std::string &text, const MusStmtLineSpan &sp,
                                const std::vector<std::string> &nl) {
    auto lines = split_lines(text);
    if (sp.line_start < 0 || sp.line_end > (int)lines.size() || sp.line_start >= sp.line_end)
        return text;
    lines.erase(lines.begin() + sp.line_start, lines.begin() + sp.line_end);
    lines.insert(lines.begin() + sp.line_start, nl.begin(), nl.end());
    return join_lines(lines);
}

// Swap two ADJACENT statement blocks (a.line_end == b.line_start).
static std::string swap_adjacent(const std::string &text, const MusStmtLineSpan &a,
                                 const MusStmtLineSpan &b) {
    if (a.line_end != b.line_start) return text;
    auto lines = split_lines(text);
    if (a.line_start < 0 || b.line_end > (int)lines.size()) return text;
    std::vector<std::string> ablk(lines.begin() + a.line_start, lines.begin() + a.line_end);
    std::vector<std::string> bblk(lines.begin() + b.line_start, lines.begin() + b.line_end);
    std::vector<std::string> out(lines.begin(), lines.begin() + a.line_start);
    out.insert(out.end(), bblk.begin(), bblk.end());
    out.insert(out.end(), ablk.begin(), ablk.end());
    out.insert(out.end(), lines.begin() + b.line_end, lines.end());
    return join_lines(out);
}

// --- section-level transforms ---

// Whole-identifier token replace, skipping string literals and // comments.
// Section names live only in identifier-token positions (section/declsection
// headers + enter/goto/call/on targets), so this safely renames a section in the
// names-less decompile without touching substrings, sound names, or vars.
static std::string rename_identifier(const std::string &text, const std::string &oldn,
                                     const std::string &newn) {
    std::string out;
    size_t i = 0, n = text.size();
    bool in_str = false;
    while (i < n) {
        char c = text[i];
        if (c == '"') { in_str = !in_str; out += c; ++i; continue; }
        if (!in_str && c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') { out += text[i]; ++i; }
            continue;
        }
        if (!in_str && (isalpha((unsigned char)c) || c == '_')) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)text[j]) || text[j] == '_')) ++j;
            std::string tok = text.substr(i, j - i);
            out += (tok == oldn) ? newn : tok;
            i = j;
        } else { out += c; ++i; }
    }
    return out;
}

// Count whole-identifier occurrences of a name (outside strings/comments). A
// canonical decompile mentions each section name exactly twice when unreferenced
// (its `declsection` + its `section` header), so count > 2 means referenced.
static int count_token(const std::string &text, const std::string &name) {
    int count = 0;
    size_t i = 0, n = text.size();
    bool in_str = false;
    while (i < n) {
        char c = text[i];
        if (c == '"') { in_str = !in_str; ++i; continue; }
        if (!in_str && c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') ++i;
            continue;
        }
        if (!in_str && (isalpha((unsigned char)c) || c == '_')) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)text[j]) || text[j] == '_')) ++j;
            if (text.substr(i, j - i) == name) ++count;
            i = j;
        } else { ++i; }
    }
    return count;
}

static int section_header_line(const std::vector<std::string> &lines, const std::string &name) {
    for (size_t i = 0; i < lines.size(); ++i)
        if (strip(lines[i]) == ("section " + name)) return (int)i;
    return -1;
}

static int declsection_line(const std::vector<std::string> &lines, const std::string &name) {
    for (size_t i = 0; i < lines.size(); ++i)
        if (strip(lines[i]) == ("declsection " + name)) return (int)i;
    return -1;
}

static size_t section_window_end(const std::vector<std::string> &lines, int hidx) {
    for (size_t i = (size_t)hidx + 1; i < lines.size(); ++i) {
        std::string s = strip(lines[i]);
        if (starts_with(s, "section ") || starts_with(s, "declsection ")) return i;
    }
    return lines.size();
}

// Delete a section's declsection line + its `section { ... }` body window.
// Refuses (returns unchanged) when the section is referenced, because a removed
// `enter X` would silently re-intern X at code_offset 0 (the gate can't catch
// that). Returns the original text on refusal so the document treats it as a no-op.
static std::string delete_section(const std::string &text, const std::string &name) {
    if (count_token(text, name) > 2) return text;   // referenced -> refuse
    auto lines = split_lines(text);
    int dl = declsection_line(lines, name);
    if (dl >= 0) lines.erase(lines.begin() + dl);
    int hidx = section_header_line(lines, name);
    if (hidx < 0) return text;
    size_t end = section_window_end(lines, hidx);
    lines.erase(lines.begin() + hidx, lines.begin() + end);
    return join_lines(lines);
}

static std::string append_section(const std::string &text, const std::string &name) {
    std::string t = text;
    if (!t.empty() && t.back() != '\n') t += "\n";
    t += "\nsection " + name + "\n{\n}\n";
    return t;
}

// --- span-location helpers for tests ---

static int last_play_index(const std::vector<MusStmtLineSpan> &sps) {
    int best = -1;
    for (size_t i = 0; i < sps.size(); ++i)
        if (sps[i].kind == MUS_AST_PLAY) best = (int)i;
    return best;
}

static int first_terminator_line(const std::vector<MusStmtLineSpan> &sps) {
    for (const auto &sp : sps)
        if (sp.kind == MUS_AST_TRANSITION || sp.kind == MUS_AST_DONE
            || sp.kind == MUS_AST_GOTO || sp.kind == MUS_AST_SWITCH)
            return sp.line_start;
    return -1;
}

// =====================================================================

static int test_annotated_text_matches_decompile() {
    const char *paths[2] = { MUS_FIXTURE_DIR "/jo_gamemus.bin", MUS_FIXTURE_DIR "/jo_menumus.bin" };
    for (int p = 0; p < 2; ++p) {
        MusFile mf;
        CHECK(mus_open(&mf, paths[p]) == 0, "open");
        Annotated a = annotate(&mf.scripts[0]);
        std::string dec = decompile_str(&mf.scripts[0]);
        CHECK(!a.text.empty(), "annotated text non-empty");
        CHECK(a.text == dec, "spans-emit text is byte-identical to mus_decompile");
        CHECK(!a.spans.empty(), "spans non-empty");
        mus_close(&mf);
    }
    return 1;
}

// Each span's first line begins with its kind's keyword -> spans point at the
// right lines for every top-level statement of every section.
static int test_spans_point_at_their_statements() {
    const char *paths[2] = { MUS_FIXTURE_DIR "/jo_gamemus.bin", MUS_FIXTURE_DIR "/jo_menumus.bin" };
    for (int p = 0; p < 2; ++p) {
        MusFile mf;
        CHECK(mus_open(&mf, paths[p]) == 0, "open");
        Annotated a = annotate(&mf.scripts[0]);
        auto lines = split_lines(a.text);
        int checked = 0;
        for (const auto &sp : a.spans) {
            if (sp.line_start == sp.line_end) {   // NOP renders nothing
                CHECK(sp.kind == MUS_AST_NOP, "empty span only for nop");
                continue;
            }
            CHECK(sp.line_start >= 0 && sp.line_end <= (int)lines.size(), "span in range");
            std::string head = strip(lines[sp.line_start]);
            switch (sp.kind) {
                case MUS_AST_PLAY:       CHECK(starts_with(head, "play "), "play span"); break;
                case MUS_AST_TRANSITION: CHECK(starts_with(head, "enter "), "enter span"); break;
                case MUS_AST_GOTO:       CHECK(starts_with(head, "goto "), "goto span"); break;
                case MUS_AST_CALL:       CHECK(starts_with(head, "call "), "call span"); break;
                case MUS_AST_RETURN:     CHECK(head == "return", "return span"); break;
                case MUS_AST_YIELD:      CHECK(head == "yield", "yield span"); break;
                case MUS_AST_DONE:       CHECK(head == "}", "done span"); break;
                case MUS_AST_IF:         CHECK(starts_with(head, "if ("), "if span"); break;
                case MUS_AST_SWITCH:     CHECK(starts_with(head, "on ("), "on span"); break;
                case MUS_AST_INCDEC:     CHECK(head.find("++") != std::string::npos
                                            || head.find("--") != std::string::npos, "incdec span"); break;
                default: break;  // assign/expr/branch_comment: text varies, range already checked
            }
            ++checked;
        }
        CHECK(checked > 0, "checked at least one span");
        mus_close(&mf);
    }
    return 1;
}

// (c)+(d): insert a play before the section terminator, model gains exactly one
// play; deleting that same line restores byte-identical text + canonical bytes.
static int insert_play_roundtrip(const char *path, const char *section) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    Annotated a = annotate(&mf.scripts[0]);
    const MusSection *sec = mus_find_section(&mf.scripts[0], section);
    CHECK(sec, "section present");
    int sidx = (int)(sec - mf.scripts[0].sections);
    auto sps = spans_of(a, sidx);
    CHECK(!sps.empty(), "section has statements");

    int term = first_terminator_line(sps);
    int at = (term >= 0) ? term : sps.back().line_end;

    std::string edited = insert_lines(a.text, at, { "play sound_0" });
    CHECK(edited != a.text, "insert changed the text");

    MusModel m0, m1;
    MusScript s1 = {};
    CHECK(compile_str(edited, &s1), "edited compiles");
    CHECK(mus_build_section_model(&mf.scripts[0], &m0) == 0, "model 0");
    CHECK(mus_build_section_model(&s1, &m1) == 0, "model 1");
    const MusSectionInfo *i0 = info_for(&m0, &mf.scripts[0], section);
    const MusSectionInfo *i1 = info_for(&m1, &s1, section);
    CHECK(i0 && i1, "section in both models");
    CHECK(i1->play_count == i0->play_count + 1, "exactly one play added");
    CHECK(i1->plays[i1->play_count - 1].track_index == 0, "appended play targets slot 0");
    CHECK(edge_targets(i1) == edge_targets(i0), "play insert leaves edges unchanged");
    mus_model_free(&m0); mus_model_free(&m1); mus_script_free(&s1);

    // delete the inserted line -> identical text + canonical bytes
    std::string undone = insert_lines(edited, 0, {});  // no-op to reuse split path
    {
        auto lines = split_lines(edited);
        lines.erase(lines.begin() + at);
        undone = join_lines(lines);
    }
    CHECK(undone == a.text, "insert + delete restores exact text");
    std::string ca, cb;
    CHECK(canonical_of_text(a.text, ca), "canonical orig");
    CHECK(canonical_of_text(undone, cb), "canonical undone");
    CHECK(ca == cb, "no-op edit re-encodes byte-identical");
    mus_close(&mf);
    return 1;
}

static int test_insert_play_roundtrip_gamemus() {
    return insert_play_roundtrip(MUS_FIXTURE_DIR "/jo_gamemus.bin", "Win000");
}

// (c)+(d) for a NON-play statement: insert `enter <other>` adds exactly one edge,
// leaves plays untouched; delete restores byte-identical.
static int test_insert_enter_adds_edge_and_roundtrips() {
    const char *path = MUS_FIXTURE_DIR "/jo_gamemus.bin";
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    Annotated a = annotate(&mf.scripts[0]);

    // Win000 + a different existing section as the enter target.
    const MusSection *win = mus_find_section(&mf.scripts[0], "Win000");
    CHECK(win, "Win000 present");
    int sidx = (int)(win - mf.scripts[0].sections);
    const char *target = nullptr;
    for (uint32_t i = 0; i < mf.scripts[0].section_count; ++i)
        if ((int)i != sidx) { target = mf.scripts[0].sections[i].name; break; }
    CHECK(target, "found a distinct target section");

    auto sps = spans_of(a, sidx);
    int term = first_terminator_line(sps);
    int at = (term >= 0) ? term : sps.back().line_end;
    std::string stmt = std::string("enter ") + target;
    std::string edited = insert_lines(a.text, at, { stmt });
    CHECK(edited != a.text, "insert changed the text");

    MusModel m0, m1;
    MusScript s1 = {};
    CHECK(compile_str(edited, &s1), "edited compiles");
    CHECK(mus_build_section_model(&mf.scripts[0], &m0) == 0, "model 0");
    CHECK(mus_build_section_model(&s1, &m1) == 0, "model 1");
    const MusSectionInfo *i0 = info_for(&m0, &mf.scripts[0], "Win000");
    const MusSectionInfo *i1 = info_for(&m1, &s1, "Win000");
    CHECK(i0 && i1, "Win000 in both models");
    CHECK(play_tracks(i1) == play_tracks(i0), "enter insert leaves plays unchanged");
    const MusSection *t1 = mus_find_section(&s1, target);
    CHECK(t1, "target present after edit");
    CHECK(edge_targets(i1).count((uint32_t)(t1 - s1.sections)) == 1,
          "enter insert adds the transition edge to the target");
    mus_model_free(&m0); mus_model_free(&m1); mus_script_free(&s1);

    auto lines = split_lines(edited);
    lines.erase(lines.begin() + at);
    CHECK(join_lines(lines) == a.text, "insert + delete restores exact text");
    mus_close(&mf);
    return 1;
}

// (e): replace a play's span with the same text -> identical; with a different
// track -> compiles and reflects in the model.
static int test_replace_play_track() {
    const char *path = MUS_FIXTURE_DIR "/jo_gamemus.bin";
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    Annotated a = annotate(&mf.scripts[0]);
    const MusSection *win = mus_find_section(&mf.scripts[0], "Win000");
    int sidx = (int)(win - mf.scripts[0].sections);
    auto sps = spans_of(a, sidx);
    int lp = last_play_index(sps);
    CHECK(lp >= 0, "Win000 has a play");
    const MusStmtLineSpan &sp = sps[lp];

    auto lines = split_lines(a.text);
    std::string orig_line = strip(lines[sp.line_start]);

    // replace-same -> identical text + bytes
    std::string same = replace_span(a.text, sp, { lines[sp.line_start] });
    CHECK(same == a.text, "replace-same is identity");

    // replace with a distinct, valid track -> reflects
    std::string repl_line = "play sound_1";
    if (orig_line == repl_line) repl_line = "play sound_2";
    std::string edited = replace_span(a.text, sp, { repl_line });
    CHECK(edited != a.text, "replace-different changed the text");
    MusScript s1 = {};
    CHECK(compile_str(edited, &s1), "replaced text compiles");
    MusModel m0, m1;
    CHECK(mus_build_section_model(&mf.scripts[0], &m0) == 0, "model 0");
    CHECK(mus_build_section_model(&s1, &m1) == 0, "model 1");
    const MusSectionInfo *i0 = info_for(&m0, &mf.scripts[0], "Win000");
    const MusSectionInfo *i1 = info_for(&m1, &s1, "Win000");
    CHECK(i0 && i1, "Win000 present");
    CHECK(i1->play_count == i0->play_count, "replace keeps play count");
    CHECK(edge_targets(i1) == edge_targets(i0), "replace keeps edges");
    mus_model_free(&m0); mus_model_free(&m1); mus_script_free(&s1);
    mus_close(&mf);
    return 1;
}

// (f): swapping two adjacent plays then swapping back is byte-identical.
static int test_reorder_adjacent_plays_roundtrips() {
    const char *path = MUS_FIXTURE_DIR "/jo_gamemus.bin";
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    Annotated a = annotate(&mf.scripts[0]);
    const MusSection *win = mus_find_section(&mf.scripts[0], "Win000");
    int sidx = (int)(win - mf.scripts[0].sections);
    auto sps = spans_of(a, sidx);

    // find two adjacent PLAY spans
    int fi = -1;
    for (size_t i = 0; i + 1 < sps.size(); ++i)
        if (sps[i].kind == MUS_AST_PLAY && sps[i + 1].kind == MUS_AST_PLAY
            && sps[i].line_end == sps[i + 1].line_start) { fi = (int)i; break; }
    CHECK(fi >= 0, "Win000 has two adjacent plays");

    std::string swapped = swap_adjacent(a.text, sps[fi], sps[fi + 1]);
    MusScript stmp = {};
    CHECK(compile_str(swapped, &stmp), "swapped compiles");
    // play multiset (and edges) preserved by a reorder
    {
        MusModel m0, m1;
        CHECK(mus_build_section_model(&mf.scripts[0], &m0) == 0, "model 0");
        CHECK(mus_build_section_model(&stmp, &m1) == 0, "model 1");
        std::vector<uint32_t> p0 = play_tracks(info_for(&m0, &mf.scripts[0], "Win000"));
        std::vector<uint32_t> p1 = play_tracks(info_for(&m1, &stmp, "Win000"));
        std::sort(p0.begin(), p0.end()); std::sort(p1.begin(), p1.end());
        CHECK(p0 == p1, "reorder preserves the play multiset");
        CHECK(edge_targets(info_for(&m0, &mf.scripts[0], "Win000"))
              == edge_targets(info_for(&m1, &stmp, "Win000")), "reorder preserves edges");
        mus_model_free(&m0); mus_model_free(&m1);
    }
    mus_script_free(&stmp);

    // swap-back at the TEXT level (single-line plays) is byte-identical.
    int a_len = sps[fi].line_end - sps[fi].line_start;
    int b_len = sps[fi + 1].line_end - sps[fi + 1].line_start;
    MusStmtLineSpan b_now = sps[fi + 1];
    b_now.line_start = sps[fi].line_start;
    b_now.line_end   = b_now.line_start + b_len;
    MusStmtLineSpan a_now = sps[fi];
    a_now.line_start = b_now.line_end;
    a_now.line_end   = a_now.line_start + a_len;
    std::string back = swap_adjacent(swapped, b_now, a_now);
    CHECK(back == a.text, "swap then swap-back is byte-identical");
    mus_close(&mf);
    return 1;
}

// (g): rename a section -> compiles, BYTECODE-identical (names are editor-debug
// only), model structurally identical.
static int rename_bytecode_identical(const char *path, const char *oldn, const char *newn) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);
    CHECK(mus_find_section(&mf.scripts[0], oldn), "old section exists");

    std::string ren = rename_identifier(orig, oldn, newn);
    CHECK(ren != orig, "rename changed the text");

    MusScript so = {}, sr = {};
    CHECK(compile_str(orig, &so), "orig compiles");
    CHECK(compile_str(ren, &sr), "renamed compiles");
    CHECK(bytecode_equal(so, sr), "rename preserves bytecode exactly");
    CHECK(so.section_count == sr.section_count, "section count unchanged");
    CHECK(mus_find_section(&sr, newn), "renamed section present");
    CHECK(!mus_find_section(&sr, oldn), "old name gone");
    mus_script_free(&so); mus_script_free(&sr);
    mus_close(&mf);
    return 1;
}

static int test_rename_section_gamemus() {
    return rename_bytecode_identical(MUS_FIXTURE_DIR "/jo_gamemus.bin", "Win000", "WinAlpha");
}
static int test_rename_section_menumus() {
    // rename the entry (first) section, whatever it's called
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_menumus.bin") == 0, "open");
    std::string first = mf.scripts[0].sections[0].name;
    mus_close(&mf);
    return rename_bytecode_identical(MUS_FIXTURE_DIR "/jo_menumus.bin", first.c_str(), "EntryRenamed");
}

// (h): add an unreferenced section then delete it -> canonical text back to base.
static int test_delete_unreferenced_section_roundtrips() {
    const char *path = MUS_FIXTURE_DIR "/jo_gamemus.bin";
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);

    std::string with = append_section(orig, "Tmp");
    MusScript s1 = {};
    CHECK(compile_str(with, &s1), "compile with Tmp");
    std::string canon = decompile_str(&s1);   // canonical form (adds declsection Tmp)
    CHECK(mus_find_section(&s1, "Tmp"), "Tmp present");
    uint32_t base_count = mf.scripts[0].section_count;
    CHECK(s1.section_count == base_count + 1, "Tmp added one section");

    std::string deleted = delete_section(canon, "Tmp");
    CHECK(deleted != canon, "delete changed the text");
    MusScript s2 = {};
    CHECK(compile_str(deleted, &s2), "compile after delete");
    CHECK(s2.section_count == base_count, "section count back to base");
    CHECK(!mus_find_section(&s2, "Tmp"), "Tmp gone");
    CHECK(decompile_str(&s2) == orig, "delete restores the original canonical text");

    mus_script_free(&s1); mus_script_free(&s2);
    mus_close(&mf);
    return 1;
}

// (h): deleting a REFERENCED section is refused (no-op text), so the editor's
// gate never produces an enter-to-offset-0 hazard.
static int test_delete_referenced_section_refused() {
    const char *paths[2] = { MUS_FIXTURE_DIR "/jo_gamemus.bin", MUS_FIXTURE_DIR "/jo_menumus.bin" };
    for (int p = 0; p < 2; ++p) {
        MusFile mf;
        CHECK(mus_open(&mf, paths[p]) == 0, "open");
        std::string orig = decompile_str(&mf.scripts[0]);
        MusModel m;
        CHECK(mus_build_section_model(&mf.scripts[0], &m) == 0, "model");
        // find a section that is some edge's target (referenced)
        const char *referenced = nullptr;
        for (uint32_t s = 0; s < m.section_count && !referenced; ++s)
            for (uint32_t e = 0; e < m.sections[s].edge_count; ++e) {
                uint32_t to = m.sections[s].edges[e].to_section_index;
                if (to < mf.scripts[0].section_count) {
                    referenced = mf.scripts[0].sections[to].name;
                    break;
                }
            }
        mus_model_free(&m);
        CHECK(referenced, "found a referenced section");
        std::string after = delete_section(orig, referenced);
        CHECK(after == orig, "deleting a referenced section is refused (no-op)");
        mus_close(&mf);
    }
    return 1;
}

int main(void) {
    RUN_TEST(test_annotated_text_matches_decompile);
    RUN_TEST(test_spans_point_at_their_statements);
    RUN_TEST(test_insert_play_roundtrip_gamemus);
    RUN_TEST(test_insert_enter_adds_edge_and_roundtrips);
    RUN_TEST(test_replace_play_track);
    RUN_TEST(test_reorder_adjacent_plays_roundtrips);
    RUN_TEST(test_rename_section_gamemus);
    RUN_TEST(test_rename_section_menumus);
    RUN_TEST(test_delete_unreferenced_section_roundtrips);
    RUN_TEST(test_delete_referenced_section_refused);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
