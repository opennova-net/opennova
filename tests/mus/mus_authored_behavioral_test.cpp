/* Behavioral proof for EDITOR-AUTHORED programs.

   The visual editor only ever emits bytecode by compiling canonical .mus TEXT
   (the exact forms mus_stmt_text.gd / the forms produce) through mus_compile.
   The golden round-trip (mus_roundtrip_test) proves the compiler reproduces
   DECOMPILED stock text; this proves the compiler's output for FRESHLY AUTHORED
   constructs (play, if/else, on-switch + default, enter-transition, assign,
   inc/dec, method/volume) runs with the expected observable behavior on the VM
   (which the Tier-4 Unicorn differential proved == the original Jointops
   handlers). The companion mus_diff_jointops.py (local RE tooling; see docs/audio/mus-sbf-re.md) runs the SAME
   authored corpus through the ORIGINAL handlers for the end-to-end guarantee;
   this committed test locks the behavior without needing Jointops.exe. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    int before = failed; fn(); \
    if (failed == before) printf("PASS\n"); else printf("FAIL\n"); } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); ++failed; } \
    else { ++passed; } } while (0)

struct Trace {
    std::vector<int> plays;       /* sbf indices, in order */
    std::vector<int> sections;    /* section entries are names; store count via hook */
    int section_count = 0;
    int vol_count = 0;
    int last_vol_left = 0;
    bool error = false;
};

static void hk_play(void *u, uint32_t idx, int /*wait*/) {
    ((Trace *)u)->plays.push_back((int)idx);
}
static void hk_section(void *u, const char * /*name*/) {
    ((Trace *)u)->section_count++;
}
static void hk_vol(void *u, int32_t left, int32_t /*right*/) {
    Trace *t = (Trace *)u;
    t->vol_count++;
    t->last_vol_left = left;
}

/* Compile authored TEXT, seed one var (idx<0 to skip), run `ticks` VM ticks,
   capture the observable event stream. Returns false if the text fails to
   compile -- which would itself be an editor-can't-author-this bug. */
static bool run_authored(const char *src, int var_idx, int32_t var_val,
                         int ticks, Trace *out) {
    MusScript script;
    memset(&script, 0, sizeof(script));
    int el = 0, ec = 0;
    const char *err = NULL;
    if (mus_compile(src, &script, &el, &ec, &err) != 0) {
        fprintf(stderr, "  compile failed @%d:%d: %s\n", el, ec, err ? err : "?");
        return false;
    }
    MusVM *vm = mus_vm_create();
    MusVMHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.user = out;
    hooks.on_play_sound = hk_play;
    hooks.on_section_entered = hk_section;
    hooks.on_volume_changed = hk_vol;
    mus_vm_set_hooks(vm, &hooks);
    mus_vm_load_script(vm, &script);
    if (var_idx >= 0) mus_vm_set_var(vm, (uint8_t)var_idx, var_val);
    mus_vm_start(vm);
    for (int i = 0; i < ticks; ++i) mus_vm_tick(vm, 1);
    out->error = (mus_vm_state(vm) == MUS_VM_ERROR);
    mus_vm_destroy(vm);
    mus_script_free(&script);
    return true;
}

/* --- if/else: the dominant conditional-play case authored by the if form --- */
static const char *IF_SRC =
    "script t\n"
    "section Begin\n"
    "{\n"
    "  SV(200)\n"
    "  if ((Var01 == 1))\n"
    "  {\n"
    "    play sound_3\n"
    "  }\n"
    "  else\n"
    "  {\n"
    "    play sound_5\n"
    "  }\n"
    "  done\n"
    "}\n";

static void test_if_then_branch(void) {
    Trace t;
    CHECK(run_authored(IF_SRC, 1, 1, 4, &t), "if-program compiles");
    CHECK(!t.error, "if-program runs without VM error");
    CHECK(t.vol_count >= 1 && t.last_vol_left == (200 << 16),
          "SV(200) sets master volume 200 in 16.16");
    // The entry section loops (done re-runs it), so a play repeats; assert the
    // FIRST play rather than an exact count.
    CHECK(!t.plays.empty() && t.plays[0] == 3, "Var01==1 takes the then-branch (play 3)");
}

static void test_if_else_branch(void) {
    Trace t;
    CHECK(run_authored(IF_SRC, 1, 0, 4, &t), "if-program compiles");
    CHECK(!t.plays.empty() && t.plays[0] == 5, "Var01==0 takes the else-branch (play 5)");
}

/* --- on (selector) play t0 t1 t2 : the tablexec switch the on form authors --- */
static const char *SWITCH_SRC =
    "script t\n"
    "section Begin\n"
    "{\n"
    "  on (Var02) play sound_0 sound_1 sound_2\n"
    "  done\n"
    "}\n";

static void test_switch_selects_target(void) {
    for (int sel = 0; sel <= 2; ++sel) {
        Trace t;
        CHECK(run_authored(SWITCH_SRC, 2, sel, 4, &t), "switch-program compiles");
        CHECK(!t.error, "switch-program runs without VM error");
        CHECK(!t.plays.empty() && t.plays[0] == sel,
              "on(Var02) play table dispatches selector -> matching track");
    }
}

static void test_switch_default_out_of_range(void) {
    /* selector 5 is out of [0,3): the table's default falls through to `done`,
       playing nothing. */
    Trace t;
    CHECK(run_authored(SWITCH_SRC, 2, 5, 4, &t), "switch-program compiles");
    CHECK(!t.error, "OOB selector runs without VM error");
    CHECK(t.plays.empty(), "out-of-range selector falls through to default (no play)");
}

/* --- enter-transition across sections: play, move to a new state, play --- */
// The entry is the FIRST-interned section (index 0). Defining Begin first makes
// it the entry; `enter Loop` forward-interns Loop at index 1. (A declsection for
// Loop placed BEFORE Begin would intern Loop first and make IT the entry -- the
// decompiler avoids that by emitting the entry's declsection first; see C3.)
static const char *TRANSITION_SRC =
    "script t\n"
    "section Begin\n"
    "{\n"
    "  play sound_1\n"
    "  enter Loop\n"
    "}\n"
    "section Loop\n"
    "{\n"
    "  play sound_2\n"
    "  done\n"
    "}\n";

static void test_enter_transition_sequence(void) {
    Trace t;
    CHECK(run_authored(TRANSITION_SRC, -1, 0, 8, &t), "transition-program compiles");
    CHECK(!t.error, "transition-program runs without VM error");
    // Begin->Loop->(done loops back to Begin), so plays cycle 1,2,1,2,...; assert
    // the first two reflect the authored order.
    CHECK(t.plays.size() >= 2 && t.plays[0] == 1 && t.plays[1] == 2,
          "Begin plays 1, enters Loop, Loop plays 2");
    CHECK(t.section_count >= 1, "entering Loop fired on_section_entered");
}

/* --- assign + inc/dec feeding a branch: Var00=0; Var00++; if (==1) play 7 --- */
static const char *ASSIGN_SRC =
    "script t\n"
    "section Begin\n"
    "{\n"
    "  Var00 = 0\n"
    "  Var00++\n"
    "  if ((Var00 == 1))\n"
    "  {\n"
    "    play sound_7\n"
    "  }\n"
    "  done\n"
    "}\n";

static void test_assign_incdec_branch(void) {
    Trace t;
    CHECK(run_authored(ASSIGN_SRC, -1, 0, 4, &t), "assign/incdec-program compiles");
    CHECK(!t.error, "assign/incdec-program runs without VM error");
    CHECK(!t.plays.empty() && t.plays[0] == 7,
          "Var00=0 then ++ makes the (Var00==1) branch fire (play 7)");
}

int main(void) {
    RUN_TEST(test_if_then_branch);
    RUN_TEST(test_if_else_branch);
    RUN_TEST(test_switch_selects_target);
    RUN_TEST(test_switch_default_out_of_range);
    RUN_TEST(test_enter_transition_sequence);
    RUN_TEST(test_assign_incdec_branch);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
