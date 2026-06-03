/* Structural section-model tests: mus_build_section_model against the shipped
   fixtures. The model reads opcodes (not decompiled text), so it gets the
   state-machine topology exact -- including the setstate-vs-enter distinction
   the text collapses and the tablexec switch fan-out the old string-parser
   missed entirely. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static int sec_idx(const MusScript *s, const char *name) {
    const MusSection *sec = mus_find_section(s, name);
    return sec ? (int)(sec - s->sections) : -1;
}

static const MusSectionInfo *info_for(const MusModel *m, int idx) {
    for (uint32_t i = 0; i < m->section_count; ++i) {
        if ((int)m->sections[i].section_index == idx) return &m->sections[i];
    }
    return NULL;
}

static int has_edge(const MusSectionInfo *si, int to, int kind /* -1 = any */) {
    if (!si) return 0;
    for (uint32_t e = 0; e < si->edge_count; ++e) {
        if ((int)si->edges[e].to_section_index == to
            && (kind < 0 || si->edges[e].kind == kind)) return 1;
    }
    return 0;
}

static void dump_model(const MusScript *s, const MusModel *m) {
    fprintf(stderr, "  --- model: %u sections ---\n", m->section_count);
    for (uint32_t i = 0; i < m->section_count; ++i) {
        const MusSectionInfo *si = &m->sections[i];
        fprintf(stderr, "  [%u] %-18s%s%s plays=%u edges=%u:",
                si->section_index, s->sections[si->section_index].name,
                si->is_entry ? " ENTRY" : "", si->is_idle_loop ? " IDLE" : "",
                si->play_count, si->edge_count);
        for (uint32_t e = 0; e < si->edge_count; ++e) {
            const char *k = si->edges[e].kind == MUS_EDGE_TRANSITION ? "T"
                          : si->edges[e].kind == MUS_EDGE_SWITCH ? "S" : "B";
            fprintf(stderr, " %s->%s", k,
                    s->sections[si->edges[e].to_section_index].name);
        }
        fprintf(stderr, "\n");
    }
}

static int test_model_gamemus(void) {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_gamemus.bin") == 0, "open");
    const MusScript *s = &mf.scripts[0];

    MusModel m;
    CHECK(mus_build_section_model(s, &m) == 0, "build model");
    if (getenv("MUS_MODEL_DUMP")) dump_model(s, &m);

    CHECK(m.section_count == s->section_count, "section count matches");

    /* Exactly one entry section, and it is flagged. */
    int entries = 0;
    for (uint32_t i = 0; i < m.section_count; ++i) if (m.sections[i].is_entry) ++entries;
    CHECK(entries == 1, "exactly one entry section");

    int begin = sec_idx(s, "Begin");
    int missionnull = sec_idx(s, "Missionnull");
    int missionwin = sec_idx(s, "Missionwin");
    int missionlose = sec_idx(s, "Missionlose");
    int win000 = sec_idx(s, "Win000");
    int lose000 = sec_idx(s, "Lose000");
    int testmission = sec_idx(s, "Testmission");
    int mpstart = sec_idx(s, "Multiplayerstart");
    CHECK(begin >= 0 && missionnull >= 0 && testmission >= 0 && mpstart >= 0
          && win000 >= 0 && lose000 >= 0 && missionwin >= 0 && missionlose >= 0,
          "all named sections present");

    /* Missionnull { enter Missionnull } is a self-loop idle state. */
    const MusSectionInfo *mn = info_for(&m, missionnull);
    CHECK(mn && mn->is_idle_loop, "Missionnull is idle-loop");
    CHECK(has_edge(mn, missionnull, MUS_EDGE_TRANSITION), "Missionnull -> self transition");

    /* Testmission branches to two sections (if/else), both real transitions. */
    const MusSectionInfo *tm = info_for(&m, testmission);
    CHECK(has_edge(tm, missionnull, MUS_EDGE_TRANSITION), "Testmission -> Missionnull");
    CHECK(has_edge(tm, mpstart, MUS_EDGE_TRANSITION), "Testmission -> Multiplayerstart");
    CHECK(tm && !tm->is_idle_loop, "Testmission not idle");

    /* Win000 plays sound_2..sound_7 (6) then transitions to Missionnull. */
    const MusSectionInfo *w = info_for(&m, win000);
    CHECK(w && w->play_count == 6, "Win000 has 6 plays");
    CHECK(w->plays[0].track_index == 2 && w->plays[5].track_index == 7, "Win000 play indices 2..7");
    CHECK(has_edge(w, missionnull, MUS_EDGE_TRANSITION), "Win000 -> Missionnull");

    /* Lose000 plays sound_8..sound_12 (5). */
    const MusSectionInfo *l = info_for(&m, lose000);
    CHECK(l && l->play_count == 5, "Lose000 has 5 plays");

    /* Multiplayerstart queues sound_0 x20 then sound_1 (21 plays). */
    const MusSectionInfo *mp = info_for(&m, mpstart);
    CHECK(mp && mp->play_count == 21, "Multiplayerstart has 21 plays");
    CHECK(mp->plays[0].track_index == 0 && mp->plays[20].track_index == 1, "MP play indices");
    CHECK(has_edge(mp, missionnull, MUS_EDGE_TRANSITION), "Multiplayerstart -> Missionnull");

    /* Begin owns a tablexec switch fanning to Missionnull/Missionwin/Missionlose,
       plus the real transition to Testmission. The switch targets are decoded
       from the table data (deterministic), independent of the enter/setstate
       text ambiguity. */
    const MusSectionInfo *b = info_for(&m, begin);
    CHECK(b, "Begin info");
    int has_switch = 0;
    for (uint32_t e = 0; e < b->edge_count; ++e)
        if (b->edges[e].kind == MUS_EDGE_SWITCH) has_switch = 1;
    CHECK(has_switch, "Begin has a switch edge");
    CHECK(has_edge(b, missionnull, -1) && has_edge(b, missionwin, -1)
          && has_edge(b, missionlose, -1), "Begin switch targets present");
    CHECK(has_edge(b, testmission, MUS_EDGE_TRANSITION), "Begin -> Testmission transition");

    mus_model_free(&m);
    /* free is idempotent. */
    mus_model_free(&m);
    mus_close(&mf);
    return 1;
}

static int test_model_menumus(void) {
    MusFile mf;
    CHECK(mus_open(&mf, MUS_FIXTURE_DIR "/jo_menumus.bin") == 0, "open");
    const MusScript *s = &mf.scripts[0];
    MusModel m;
    CHECK(mus_build_section_model(s, &m) == 0, "build model");
    if (getenv("MUS_MODEL_DUMP")) dump_model(s, &m);
    CHECK(m.section_count == s->section_count, "section count matches");
    CHECK(m.section_count > 0, "menumus has sections");
    /* The menu script is variable-driven (push/setstate/brfalse/tablexec); it
       must produce at least some transitions and at least one play. */
    uint32_t total_edges = 0, total_plays = 0;
    for (uint32_t i = 0; i < m.section_count; ++i) {
        total_edges += m.sections[i].edge_count;
        total_plays += m.sections[i].play_count;
    }
    CHECK(total_edges > 0, "menumus has transitions");
    CHECK(total_plays > 0, "menumus has plays");
    mus_model_free(&m);
    mus_close(&mf);
    return 1;
}

static int test_model_null_safe(void) {
    MusModel m;
    CHECK(mus_build_section_model(NULL, &m) < 0, "null script rejected");
    CHECK(mus_build_section_model((const MusScript *)0x1, NULL) < 0, "null out rejected");
    mus_model_free(NULL); /* no crash */
    return 1;
}

int main(void) {
    RUN_TEST(test_model_gamemus);
    RUN_TEST(test_model_menumus);
    RUN_TEST(test_model_null_safe);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
