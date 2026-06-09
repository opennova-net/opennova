/* Structured play-edit parity: validates the invariant the editor's
   drag-to-add-play write path stands on. The edit is a TEXT transform over the
   faithful decompiled text (splice one `play <bind>` line into a section's
   body, after its last existing play so it lands before the halting transition),
   recompiled and re-encoded. Never hand-assembles bytecode; never touches the
   decompiler emitter.

   Three things must hold:
   (a) a real insert adds EXACTLY ONE play to that section's model, with the
       right track index, and leaves the section's edges unchanged;
   (b) the inserted text re-decompiles to the original plus exactly one more
       play line;
   (c) a no-op edit (insert then remove the same line) returns byte-identical
       text and therefore byte-identical canonical bytes -- this is what makes
       the gate safe (an undone edit can never drift the file). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

// --- text helpers (mirror MusicEditorDocument._splice_play in GDScript) ---

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

// Find the "section <name>" header line index, or -1.
static int section_header_line(const std::vector<std::string> &lines, const std::string &section) {
    for (size_t i = 0; i < lines.size(); ++i)
        if (strip(lines[i]) == ("section " + section)) return (int)i;
    return -1;
}

// End of the section's window: the next section/declsection header, or EOF.
static size_t section_window_end(const std::vector<std::string> &lines, int hidx) {
    for (size_t i = hidx + 1; i < lines.size(); ++i) {
        std::string s = strip(lines[i]);
        if (starts_with(s, "section ") || starts_with(s, "declsection ")) return i;
    }
    return lines.size();
}

// Insert "play <bind>" into the section's STRAIGHT-LINE body (brace depth 1),
// never inside a nested if/else/on(...) block. Anchor on the last top-level
// play; else before the first top-level transition/halt; else just inside the
// opening brace. Mirrors MusicEditorDocument._splice_play.
static std::string splice_play(const std::string &text, const std::string &section,
                               const std::string &bind) {
    std::vector<std::string> lines = split_lines(text);
    int hidx = section_header_line(lines, section);
    if (hidx < 0) return text;
    size_t end = section_window_end(lines, hidx);
    int depth = 0, open_brace = -1, last_top_play = -1, first_top_transition = -1;
    for (size_t i = hidx + 1; i < end; ++i) {
        std::string s = strip(lines[i]);
        if (s == "{") { ++depth; if (open_brace < 0) open_brace = (int)i; continue; }
        if (s == "}") { --depth; continue; }
        if (depth == 1) {
            if (starts_with(s, "play ") || starts_with(s, "playw ")) last_top_play = (int)i;
            else if (first_top_transition < 0 &&
                     (starts_with(s, "enter ") || s == "done" || starts_with(s, "on (")))
                first_top_transition = (int)i;
        }
    }
    int insert_at;
    if (last_top_play >= 0) insert_at = last_top_play + 1;
    else if (first_top_transition >= 0) insert_at = first_top_transition;
    else if (open_brace >= 0) insert_at = open_brace + 1;
    else insert_at = hidx + 1;
    lines.insert(lines.begin() + insert_at, "play " + bind);
    return join_lines(lines);
}

// Remove the LAST top-level "play <bind>" so it cancels splice_play exactly.
static std::string remove_one_play(const std::string &text, const std::string &section,
                                   const std::string &bind) {
    std::vector<std::string> lines = split_lines(text);
    int hidx = section_header_line(lines, section);
    if (hidx < 0) return text;
    size_t end = section_window_end(lines, hidx);
    int depth = 0, remove_at = -1;
    for (size_t i = hidx + 1; i < end; ++i) {
        std::string s = strip(lines[i]);
        if (s == "{") { ++depth; continue; }
        if (s == "}") { --depth; continue; }
        if (depth == 1 && s == ("play " + bind)) remove_at = (int)i;
    }
    if (remove_at >= 0) lines.erase(lines.begin() + remove_at);
    return join_lines(lines);
}

static int count_play_lines(const std::string &text) {
    int n = 0;
    for (const std::string &line : split_lines(text))
        if (starts_with(strip(line), "play ")) ++n;
    return n;
}

// Top-level (brace depth 1) plays in a section -- the unconditional sequence.
static int count_top_level_plays(const std::string &text, const std::string &section) {
    std::vector<std::string> lines = split_lines(text);
    int hidx = section_header_line(lines, section);
    if (hidx < 0) return -1;
    size_t end = section_window_end(lines, hidx);
    int depth = 0, n = 0;
    for (size_t i = hidx + 1; i < end; ++i) {
        std::string s = strip(lines[i]);
        if (s == "{") { ++depth; continue; }
        if (s == "}") { --depth; continue; }
        if (depth == 1 && (starts_with(s, "play ") || starts_with(s, "playw "))) ++n;
    }
    return n;
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

static const MusSectionInfo *info_for(const MusModel *m, const MusScript *s, const char *name) {
    const MusSection *sec = mus_find_section(s, name);
    if (!sec) return nullptr;
    int idx = (int)(sec - s->sections);
    for (uint32_t i = 0; i < m->section_count; ++i)
        if ((int)m->sections[i].section_index == idx) return &m->sections[i];
    return nullptr;
}

// canonical(script) = encode(compile(decompile(script))) bytes.
static bool canonical_bytes(const MusScript *s, std::string &out) {
    std::string text = decompile_str(s);
    if (text.empty()) return false;
    MusScript recomp = {};
    if (!compile_str(text, &recomp)) return false;
    const MusScript *arr[1] = { &recomp };
    uint8_t *buf = nullptr;
    size_t n = 0;
    int rc = mus_encode_file(arr, 1, &buf, &n);
    mus_script_free(&recomp);
    if (rc != 0) return false;
    out.assign((const char *)buf, n);
    mus_free(buf);
    return true;
}

static int test_real_insert_adds_one_play_unchanged_edges() {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);
    CHECK(!orig.empty(), "decompile original");

    // Baseline Win000 model: 6 plays, one outgoing transition edge.
    MusModel m0;
    CHECK(mus_build_section_model(&mf.scripts[0], &m0) == 0, "model 0");
    const MusSectionInfo *w0 = info_for(&m0, &mf.scripts[0], "Win000");
    CHECK(w0, "Win000 present");
    uint32_t base_plays = w0->play_count;
    uint32_t base_edges = w0->edge_count;
    CHECK(base_plays == 6, "Win000 baseline 6 plays");

    // Splice one play of bank slot 0 (bind "sound_0") into Win000.
    std::string edited = splice_play(orig, "Win000", "sound_0");
    CHECK(edited != orig, "splice changed the text");
    MusScript s2 = {};
    CHECK(compile_str(edited, &s2), "compile edited");
    MusModel m1;
    CHECK(mus_build_section_model(&s2, &m1) == 0, "model 1");
    const MusSectionInfo *w1 = info_for(&m1, &s2, "Win000");
    CHECK(w1, "Win000 present after edit");
    CHECK(w1->play_count == base_plays + 1, "exactly one play added");
    CHECK(w1->plays[w1->play_count - 1].track_index == 0, "appended play targets bank slot 0");
    CHECK(w1->edge_count == base_edges, "section edges unchanged by a play insert");

    mus_model_free(&m0);
    mus_model_free(&m1);
    mus_script_free(&s2);
    mus_close(&mf);
    return 1;
}

static int test_inserted_text_redecompiles_with_one_more_play() {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);
    int orig_plays = count_play_lines(orig);

    std::string edited = splice_play(orig, "Win000", "sound_0");
    MusScript s2 = {};
    CHECK(compile_str(edited, &s2), "compile edited");
    std::string redec = decompile_str(&s2);
    CHECK(count_play_lines(redec) == orig_plays + 1,
          "re-decompiled text has exactly one more play line");

    mus_script_free(&s2);
    mus_close(&mf);
    return 1;
}

static int test_noop_edit_is_byte_identical() {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);

    // Insert then remove the same play -> identical text.
    std::string edited = splice_play(orig, "Win000", "sound_0");
    std::string undone = remove_one_play(edited, "Win000", "sound_0");
    CHECK(undone == orig, "insert + remove restores the exact text");

    // ... therefore identical canonical bytes.
    std::string canon_orig, canon_undone;
    MusScript so = {}, su = {};
    CHECK(compile_str(orig, &so), "compile orig");
    CHECK(compile_str(undone, &su), "compile undone");
    const MusScript *ao[1] = { &so };
    const MusScript *au[1] = { &su };
    uint8_t *bo = nullptr, *bu = nullptr;
    size_t no = 0, nu = 0;
    CHECK(mus_encode_file(ao, 1, &bo, &no) == 0, "encode orig");
    CHECK(mus_encode_file(au, 1, &bu, &nu) == 0, "encode undone");
    CHECK(no == nu && memcmp(bo, bu, no) == 0, "no-op edit re-encodes byte-identical");
    mus_free(bo);
    mus_free(bu);
    mus_script_free(&so);
    mus_script_free(&su);

    mus_close(&mf);
    return 1;
}

// No-op byte-identity must hold even when the section ALREADY contains a play
// of that track (insert-after-last-top + remove-last-top cancel exactly).
static int noop_byte_identical(const char *path, const char *section, const char *bind) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);
    std::string edited = splice_play(orig, section, bind);
    CHECK(edited != orig, "splice changed the text");
    std::string undone = remove_one_play(edited, section, bind);
    CHECK(undone == orig, "insert + remove restores the exact text for an existing track");

    MusScript so = {}, su = {};
    CHECK(compile_str(orig, &so), "compile orig");
    CHECK(compile_str(undone, &su), "compile undone");
    const MusScript *ao[1] = { &so };
    const MusScript *au[1] = { &su };
    uint8_t *bo = nullptr, *bu = nullptr;
    size_t no = 0, nu = 0;
    CHECK(mus_encode_file(ao, 1, &bo, &no) == 0, "encode orig");
    CHECK(mus_encode_file(au, 1, &bu, &nu) == 0, "encode undone");
    CHECK(no == nu && memcmp(bo, bu, no) == 0, "byte-identical canonical after no-op on existing track");
    mus_free(bo);
    mus_free(bu);
    mus_script_free(&so);
    mus_script_free(&su);
    mus_close(&mf);
    return 1;
}

static int test_noop_existing_track_multiplayerstart(void) {
    // Multiplayerstart plays sound_0 x20 then sound_1 -- the case the first cut
    // got wrong (insert-after-last + remove-first reordered the score).
    return noop_byte_identical(MUS_FIXTURE_DIR "/jo_gamemus.bin", "Multiplayerstart", "sound_0");
}

static int test_noop_existing_track_win000(void) {
    return noop_byte_identical(MUS_FIXTURE_DIR "/jo_gamemus.bin", "Win000", "sound_2");
}

// A play insert must land in the section's STRAIGHT-LINE body (depth 1), never
// inside a conditional branch -- checked across EVERY section of a fixture
// (menumus buries plays in if/on blocks, so this is the real stress case).
static int top_level_insert_for_fixture(const char *path) {
    MusFile mf;
    CHECK(mus_open(&mf, path) == 0, "open");
    std::string orig = decompile_str(&mf.scripts[0]);
    const MusScript *s = &mf.scripts[0];
    int checked = 0;
    for (uint32_t i = 0; i < s->section_count; ++i) {
        std::string name = s->sections[i].name;
        int before = count_top_level_plays(orig, name);
        if (before < 0) continue;
        std::string edited = splice_play(orig, name, "sound_0");
        int after = count_top_level_plays(edited, name);
        if (after != before + 1) {
            fprintf(stderr, "  section %s: top-level plays %d -> %d (insert landed nested?)\n",
                    name.c_str(), before, after);
            CHECK(false, "insert added exactly one TOP-LEVEL play");
        }
        MusScript s2 = {};
        if (!compile_str(edited, &s2)) {
            fprintf(stderr, "  section %s: edited text failed to compile\n", name.c_str());
            CHECK(false, "edited text compiles");
        }
        mus_script_free(&s2);
        ++checked;
    }
    CHECK(checked > 0, "checked at least one section");
    mus_close(&mf);
    return 1;
}

static int test_insert_top_level_gamemus(void) {
    return top_level_insert_for_fixture(MUS_FIXTURE_DIR "/jo_gamemus.bin");
}

static int test_insert_top_level_menumus(void) {
    return top_level_insert_for_fixture(MUS_FIXTURE_DIR "/jo_menumus.bin");
}

int main(void) {
    RUN_TEST(test_real_insert_adds_one_play_unchanged_edges);
    RUN_TEST(test_inserted_text_redecompiles_with_one_more_play);
    RUN_TEST(test_noop_edit_is_byte_identical);
    RUN_TEST(test_noop_existing_track_multiplayerstart);
    RUN_TEST(test_noop_existing_track_win000);
    RUN_TEST(test_insert_top_level_gamemus);
    RUN_TEST(test_insert_top_level_menumus);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
