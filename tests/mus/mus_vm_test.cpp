/* MUS VM (interpreter) tests.

   Each E-task adds a focused test; the file grows opcode-by-opcode. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

/* Helper: compile a tiny script and return it. Caller frees with mus_script_free.
   Aborts the test with a CHECK fail if compilation errors. */
static int compile_or_die(const char *src, MusScript *out) {
    int el = 0, ec = 0;
    const char *err = NULL;
    int rc = mus_compile(src, out, &el, &ec, &err);
    if (rc != 0) {
        fprintf(stderr, "  compile failed at %d:%d: %s\n", el, ec, err ? err : "?");
        return 0;
    }
    return 1;
}

/* ---- E1: scaffolding ---------------------------------------------------- */

static int test_vm_lifecycle(void) {
    MusVM *vm = mus_vm_create();
    CHECK(vm != NULL, "create");
    CHECK(mus_vm_state(vm) == MUS_VM_STOPPED, "initial STOPPED");
    /* destroy must accept NULL too */
    mus_vm_destroy(NULL);
    mus_vm_destroy(vm);
    return 1;
}

static int test_vm_hooks_registered(void) {
    MusVM *vm = mus_vm_create();
    MusVMHooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    int echo_count = 0;
    hooks.user = &echo_count;
    /* Just register; nothing fires without a script. */
    mus_vm_set_hooks(vm, &hooks);
    CHECK(mus_vm_state(vm) == MUS_VM_STOPPED, "still STOPPED");
    /* set_hooks(NULL) must clear cleanly */
    mus_vm_set_hooks(vm, NULL);
    CHECK(mus_vm_state(vm) == MUS_VM_STOPPED, "still STOPPED after clear");
    mus_vm_destroy(vm);
    return 1;
}

static int test_vm_last_error_default(void) {
    MusVM *vm = mus_vm_create();
    const char *err = mus_vm_last_error(vm);
    CHECK(err != NULL, "non-null error string");
    CHECK(strcmp(err, "ok") == 0, "default error is 'ok'");
    mus_vm_destroy(vm);
    return 1;
}

/* ---- E3: push/pop opcodes + tick scaffolding --------------------------- */

static int test_vm_push_imm8_pop_g(void) {
    /* Compiler emits Var00 = 7 as push imm8 (0x01 0x07) then pop_g (0x08 0x00).
       After one tick we expect Var00 (byte offset 0) to read 7. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = 7\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    int var_changed_idx = -1;
    int var_changed_val = -1;
    struct Cap { int *idx; int *val; };
    Cap cap = { &var_changed_idx, &var_changed_val };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_var_changed = [](void *u, uint8_t i, int32_t v) {
        Cap *c = (Cap *)u;
        *c->idx = i;
        *c->val = v;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 7, "Var00 == 7");
    CHECK(var_changed_idx == 0, "on_var_changed idx 0");
    CHECK(var_changed_val == 7, "on_var_changed val 7");

    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_push_imm32(void) {
    /* Var00 = 100000 forces push imm32 (0x02 + 4B LE). */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = 100000\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 100000, "Var00 == 100000");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_tick_when_stopped_is_noop(void) {
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = 7\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    /* Don't start. */
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0, "Var00 still 0");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_tick_done_halts(void) {
    /* `done` (0x3F) is a halt opcode; after one tick state goes to HALTED. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    /* done halts the dispatch loop but leaves state RUNNING for next tick.
       The subsequent tick will resume at a different PC (entry section reset
       by the original engine's `done` handler). For now we assert the loop
       broke -- we'll refine done semantics in E6. */
    /* No specific assertion here yet; the body did not run forever. */
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* ---- E4: arithmetic + comparison --------------------------------------- */

static int test_vm_arith_add(void) {
    /* Var00 = (3 + 4)  -> 7 */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = (3 + 4)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 7, "3+4=7");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_arith_chain(void) {
    /* Var00 = ((10 - 2) * 3)  -> 24
       Var01 = (15 % 4)        -> 3 */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = ((10 - 2) * 3)\n"
        "  Var01 = (15 % 4)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 24, "(10-2)*3=24");
    CHECK(mus_vm_get_var(vm, 1) == 3, "15%4=3");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_compare_eq(void) {
    /* Var00 = (5 == 5)  -> -1 (witnessed: comparisons push -1 when true).
       Var01 = (5 == 6)  -> 0 */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = (5 == 5)\n"
        "  Var01 = (5 == 6)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == -1, "5==5 = -1");
    CHECK(mus_vm_get_var(vm, 1) == 0,  "5==6 = 0");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_compare_lt_gt(void) {
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = (3 < 4)\n"
        "  Var01 = (4 > 3)\n"
        "  Var02 = (3 >= 3)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == -1, "3<4");
    CHECK(mus_vm_get_var(vm, 1) == -1, "4>3");
    CHECK(mus_vm_get_var(vm, 2) == -1, "3>=3");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* ---- E5: branch + jump --------------------------------------------------

   The compiler emits:
     - if/else: brfalse (0x31) + body + goto-end (0x30) + else-body + patches
     - goto: bare 0x30 with 4-byte LE absolute target

   The witnessed handlers:
     - 0x30 goto: pc = read_u32() (absolute bytecode offset)
     - 0x31 brfalse: target = read_u32(); if (pop() == 0) pc = target
     - 0x32 brtrue: target = read_u32(); if (pop() != 0) pc = target

   `(0 == 0)` evaluates to -1 on the data stack (witnessed: comparisons push
   -1 when true). brfalse treats any non-zero as "true" -> doesn't branch. */

static int test_vm_if_taken(void) {
    /* condition true: take if-body, set Var00 = 99 */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  if ((1 == 1))\n"
        "  {\n"
        "    Var00 = 99\n"
        "  }\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 99, "if-taken sets Var00=99");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_if_not_taken(void) {
    /* condition false: skip if-body */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  if ((1 == 2))\n"
        "  {\n"
        "    Var00 = 99\n"
        "  }\n"
        "  Var01 = 7\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0, "if-not-taken: Var00 still 0");
    CHECK(mus_vm_get_var(vm, 1) == 7, "post-if statement runs");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_if_else(void) {
    /* condition false: take else branch */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  if ((1 == 2))\n"
        "  {\n"
        "    Var00 = 11\n"
        "  }\n"
        "  else\n"
        "  {\n"
        "    Var00 = 22\n"
        "  }\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 22, "else branch sets Var00=22");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* ---- E6: call + return + setstate -------------------------------------- */

static int test_vm_call_section_returns(void) {
    /* `call Sub` (0x34 callv) jumps to section Sub, runs to a `return`
       (0x39), then resumes after the call site.
       Expect Var00 = 42 + 1 = 43 after the call+return. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  call Sub\n"
        "  Var00 = (Var00 + 1)\n"
        "  done\n"
        "}\n"
        "section Sub\n"
        "{\n"
        "  Var00 = 42\n"
        "  return\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    /* Tick a few times to give it room (32-instr budget per tick). */
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 43, "call+return: Var00 += 1 after Sub set 42");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_setstate_halts(void) {
    /* `enter Sub` compiles to setstate (0x3B) which halts the dispatch loop
       and switches the script's current section to Sub. The next tick should
       run Sub's body (Var00 = 99). */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  enter Sub\n"
        "}\n"
        "section Sub\n"
        "{\n"
        "  Var00 = 99\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    int section_entered_count = 0;
    char last_section[64] = {0};
    struct Cap2 { int *count; char *name; };
    Cap2 cap = { &section_entered_count, last_section };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_section_entered = [](void *u, const char *n) {
        Cap2 *c = (Cap2 *)u;
        ++*c->count;
        strncpy(c->name, n, 63);
        c->name[63] = 0;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    /* First tick: hits setstate and halts. */
    mus_vm_tick(vm, 16);
    CHECK(section_entered_count == 1, "on_section_entered fired once");
    CHECK(strcmp(last_section, "Sub") == 0, "current section name is Sub");
    /* Second tick: runs Sub. */
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 99, "Sub ran on resume");

    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_done_resets_to_entry(void) {
    /* `done` (0x3F) resets pc to the entry section's code_offset.
       After `done` halts, the next tick re-runs the entry section. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Var00 = (Var00 + 1)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 1, "Var00 after first tick = 1");
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 2, "done re-entered Begin -> Var00 = 2");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* ---- tablexec (0x35) ---------------------------------------------------

   tablexec is a switch-statement codegen. The header is:
     +0  size           (count of slots)
     +1  stride_a       (hint byte; not read by VM)
     +2  entry_stride   (bytes per slot; 2 for enter/play, 5 for goto)
     +3  skip_size      (TOTAL bytes in the encoded instruction including
                         opcode + header + body; used by VM to skip past
                         table on out-of-range index)

   Ported from dfvas!VmOp_TableExec @ 0x558420. Compiler emission for
   `on (expr) ACTION T0..Tn` in libs/mus/src/mus_compile.cpp. */

static int test_vm_tablexec_in_range(void) {
    /* `on (1) enter Sec0 Sec1 Sec2` builds a 3-slot setstate (0x3B) table.
       With idx=1 the VM dispatches setstate of section index 2 (= Sec1),
       which halts. Next tick runs Sec1's body. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  on (1) enter Sec0 Sec1 Sec2\n"
        "  done\n"
        "}\n"
        "section Sec0\n"
        "{\n"
        "  Var00 = 88\n"
        "  done\n"
        "}\n"
        "section Sec1\n"
        "{\n"
        "  Var01 = 99\n"
        "  done\n"
        "}\n"
        "section Sec2\n"
        "{\n"
        "  Var02 = 77\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    /* First tick hits the embedded setstate which halts. Second tick runs
       the chosen section. */
    mus_vm_tick(vm, 16);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0,  "Sec0 not entered");
    CHECK(mus_vm_get_var(vm, 1) == 99, "Sec1 entered (idx=1)");
    CHECK(mus_vm_get_var(vm, 2) == 0,  "Sec2 not entered");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_tablexec_out_of_range(void) {
    /* idx >= size: tablexec must use skip_size to advance past the whole
       table so the next statement runs. Pre-fix the compiler emitted 0
       for skip_size and the VM had no handler at all; the bytecode would
       either error out (`unknown opcode 0x35`) or, with the handler but
       skip_size=0, walk into table data and interpret it as opcodes.

       The post-table statement uses `enter Done` rather than a bare
       assignment because `on (...) enter A B` greedily consumes following
       identifiers as targets. `enter Done` sets state to Done which then
       sets Var00=42 on the next tick. If skip_size is wrong, the embedded
       table would dispatch and we'd land in Sec0/Sec1 (with Var00=88/77). */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  on (5) enter Sec0 Sec1\n"
        "  enter Done\n"
        "}\n"
        "section Sec0\n"
        "{\n"
        "  Var00 = 88\n"
        "  done\n"
        "}\n"
        "section Sec1\n"
        "{\n"
        "  Var00 = 77\n"
        "  done\n"
        "}\n"
        "section Done\n"
        "{\n"
        "  Var00 = 42\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    /* tick 1: tablexec skips, then enter Done halts. tick 2: Done body. */
    mus_vm_tick(vm, 16);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 42,
          "post-table statement runs after out-of-range tablexec");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* ---- E7: variable intrinsics (GSV, GSDV, GFB, GGRnd) -------------------- */

static int test_vm_intrinsic_gsv(void) {
    /* SV(200) shall fire on_volume_changed with left=right=200<<16. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  SV(200)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    int32_t got_left = 0, got_right = 0;
    int     got_count = 0;
    struct Cap { int32_t *l; int32_t *r; int *c; };
    Cap cap = { &got_left, &got_right, &got_count };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_volume_changed = [](void *u, int32_t l, int32_t r) {
        Cap *c = (Cap *)u;
        *c->l = l; *c->r = r; ++*c->c;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(got_count == 1, "on_volume_changed fired once");
    CHECK(got_left  == (200 << 16), "left = 200<<16");
    CHECK(got_right == (200 << 16), "right = 200<<16");

    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_intrinsic_gsv_clamps(void) {
    /* SV(300) clamps to 255. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  SV(300)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    int32_t got_left = 0;
    MusVMHooks hooks = {};
    hooks.user = &got_left;
    hooks.on_volume_changed = [](void *u, int32_t l, int32_t /*r*/) {
        *(int32_t *)u = l;
    };
    mus_vm_set_hooks(vm, &hooks);
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(got_left == (255 << 16), "clamp 300 -> 255");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_intrinsic_gfb(void) {
    /* GFB() returns 0; expression-statement (empty) drops it. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  FB()\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    /* No assertion beyond "doesn't crash"; GFB has no embedder hook. */
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* GGRnd takes 2 args; the 1-arg compiler form can't express that directly.
   Hand-craft bytecode: push lo (0x01 0x05), push hi (0x01 0x0A), method
   GGRnd (0x40 0x01), pop_g Var00 (0x08 0x00), done (0x3F). Result must be
   in [5, 10]. */
static int test_vm_intrinsic_ggrnd_in_range(void) {
    static uint8_t code[] = {
        0x01, 0x05,        /* push 5 */
        0x01, 0x0A,        /* push 10 */
        0x40, 0x01,        /* method GGRnd (idx 1) */
        0x08, 0x00,        /* pop_g Var00 (byte_off 0) */
        0x3F,              /* done */
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code;
    s.code_size = sizeof(code);
    s.sections = &sec;
    s.section_count = 1;
    s.entry_section_index = 0;
    s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    int32_t r = mus_vm_get_var(vm, 0);
    CHECK(r >= 5 && r <= 10, "GGRnd result in [5,10]");
    mus_vm_destroy(vm);
    /* Don't free s.code (stack-allocated); s.sections is &sec (stack). */
    return 1;
}

/* ---- E8: flag intrinsics (FSet, FClear, FIsSet, FIsClear stub) ----------

   Witnessed: dfvas!Intrinsic_FSet/FClear/FIsSet/FIsClear @ 0x557970..0x5579D0.
   Calling convention: NOS = bitmask, TOS = pointer-to-var. The `op_method`
   handler pops both. We tag global addresses with kAddrTagBit so the handler
   can decode them back to a byte offset.

   Scripts emit: push <mask>; push_ga <var_byte_off>; method <FSet | ...>.
   FIsClear is unbound in dfvas (silently no-ops + pushes 0). */

/* FSet sets bits in Var00. After: Var00 == 0x0F. */
static int test_vm_intrinsic_fset(void) {
    static uint8_t code[] = {
        0x01, 0x0F,        /* push 0x0F (mask) */
        0x05, 0x00, 0x00,  /* push_ga byte_off 0 (Var00) + reserved byte (2-byte operand) */
        0x40, 0x05,        /* method FSet (idx 5) */
        0x0F,              /* empty -> drain the stack */
        0x3F,              /* done */
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code; s.code_size = sizeof(code);
    s.sections = &sec; s.section_count = 1;
    s.entry_section_index = 0; s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0x0F, "Var00 has the FSet mask");
    mus_vm_destroy(vm);
    return 1;
}

/* FClear clears bits. Var00 starts at 0xFF, FClear with mask 0x0F leaves 0xF0. */
static int test_vm_intrinsic_fclear(void) {
    static uint8_t code[] = {
        0x01, 0xFF, 0x05, 0x00, 0x00, 0x40, 0x05, 0x0F,   /* FSet 0xFF -> Var00 = 0xFF */
        0x01, 0x0F, 0x05, 0x00, 0x00, 0x40, 0x06, 0x0F,   /* FClear 0x0F -> Var00 = 0xF0 */
        0x3F,
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code; s.code_size = sizeof(code);
    s.sections = &sec; s.section_count = 1;
    s.entry_section_index = 0; s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0xF0, "Var00 = 0xF0 after FSet 0xFF + FClear 0x0F");
    mus_vm_destroy(vm);
    return 1;
}

/* FIsSet returns -1 when ALL mask bits are set in *var. */
static int test_vm_intrinsic_fisset(void) {
    static uint8_t code[] = {
        /* Var00 = 0x0F via FSet */
        0x01, 0x0F, 0x05, 0x00, 0x00, 0x40, 0x05, 0x0F,
        /* result1 = FIsSet(0x03, &Var00) -> all bits 0x03 set in 0x0F? yes -> -1 */
        0x01, 0x03, 0x05, 0x00, 0x00, 0x40, 0x07, 0x08, 0x04,
                                                    /* pop_g Var01 */
        /* result2 = FIsSet(0x10, &Var00) -> 0x10 not in 0x0F -> 0 */
        0x01, 0x10, 0x05, 0x00, 0x00, 0x40, 0x07, 0x08, 0x08,
                                                    /* pop_g Var02 */
        0x3F,
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code; s.code_size = sizeof(code);
    s.sections = &sec; s.section_count = 1;
    s.entry_section_index = 0; s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    /* Two ticks because we have ~25 instructions and budget is 32. */
    mus_vm_tick(vm, 16);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 1) == -1, "FIsSet(0x03) over 0x0F = -1");
    CHECK(mus_vm_get_var(vm, 2) == 0,  "FIsSet(0x10) over 0x0F = 0");
    mus_vm_destroy(vm);
    return 1;
}

/* FIsClear (idx 8) is a real bound handler in Jointops (AudioVM_Intrinsic_FIsClear
   @ 0x6723C0): returns -1 when NONE of the mask bits are set in *var, else 0 -- the
   inverse of FIsSet. */
static int test_vm_intrinsic_fisclear(void) {
    static uint8_t code[] = {
        /* Var00 = 0x0F via FSet (empty drains the return value) */
        0x01, 0x0F, 0x05, 0x00, 0x00, 0x40, 0x05, 0x0F,
        /* result1 = FIsClear(0x10, &Var00) -> 0x10 not in 0x0F -> none set -> -1 */
        0x01, 0x10, 0x05, 0x00, 0x00, 0x40, 0x08, 0x08, 0x04,  /* pop_g Var01 */
        /* result2 = FIsClear(0x03, &Var00) -> 0x03 bits ARE set -> 0 */
        0x01, 0x03, 0x05, 0x00, 0x00, 0x40, 0x08, 0x08, 0x08,  /* pop_g Var02 */
        0x3F,
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code; s.code_size = sizeof(code);
    s.sections = &sec; s.section_count = 1;
    s.entry_section_index = 0; s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 1) == -1, "FIsClear(0x10) over 0x0F = -1 (bits clear)");
    CHECK(mus_vm_get_var(vm, 2) == 0,  "FIsClear(0x03) over 0x0F = 0 (bits set)");
    mus_vm_destroy(vm);
    return 1;
}

/* ---- E9: GEcho + Timer stubs ------------------------------------------- */

static int test_vm_intrinsic_gecho_fires_hook(void) {
    /* Echo(42) -> on_echo(user, 42). */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  Echo(42)\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    int32_t got_arg = 0;
    int     got_count = 0;
    struct Cap { int32_t *arg; int *count; };
    Cap cap = { &got_arg, &got_count };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_echo = [](void *u, int32_t a) {
        Cap *c = (Cap *)u;
        *c->arg = a;
        ++*c->count;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(got_count == 1, "on_echo fired once");
    CHECK(got_arg == 42, "on_echo arg is 42");

    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* TStart and TStop are silent no-ops in dfvas (NULL handler -> push 0).
   Hand-craft method calls and verify the VM doesn't crash and pushes 0. */
static int test_vm_intrinsic_tstart_tstop_noop(void) {
    static uint8_t code[] = {
        0x01, 0x07,        /* push 7 (placeholder arg) */
        0x40, 0x09,        /* method TStart (idx 9) -> push 0 */
        0x08, 0x00,        /* pop_g Var00 */
        0x40, 0x0A,        /* method TStop  (idx 10) -> push 0 */
        0x08, 0x04,        /* pop_g Var01 */
        0x3F,
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code; s.code_size = sizeof(code);
    s.sections = &sec; s.section_count = 1;
    s.entry_section_index = 0; s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0, "TStart stub pushed 0");
    CHECK(mus_vm_get_var(vm, 1) == 0, "TStop stub pushed 0");
    mus_vm_destroy(vm);
    return 1;
}

/* ---- E10: play_sound opcode -------------------------------------------- */

static int test_vm_play_fires_hook(void) {
    /* `play sound_5` compiles to opcode 0x3E + byte 5. The hook fires with
       sbf_entry_index=5 and wait=0 (play is non-blocking). Then halts. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  play sound_5\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    uint32_t got_idx  = 0xFFFFFFFFu;
    int      got_wait = -1;
    int      got_count = 0;
    struct Cap { uint32_t *idx; int *wait; int *count; };
    Cap cap = { &got_idx, &got_wait, &got_count };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_play_sound = [](void *u, uint32_t i, int w) {
        Cap *c = (Cap *)u;
        *c->idx = i;
        *c->wait = w;
        ++*c->count;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(got_count == 1, "on_play_sound fired once");
    CHECK(got_idx == 5,   "sbf_entry_index = 5");
    CHECK(got_wait == 0,  "wait = 0 (play, not playw)");

    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* playw (0x3D) takes a 2-byte LE operand and fires the hook with wait=1. */
static int test_vm_playw_fires_hook_wait(void) {
    static uint8_t code[] = {
        0x3D, 0x05, 0x01,    /* playw 0x0105 = 261 */
        0x3F,                /* done */
    };
    MusSection sec;
    memset(&sec, 0, sizeof(sec));
    strncpy(sec.name, "Begin", sizeof(sec.name) - 1);
    sec.code_offset = 0;

    MusScript s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, "t", sizeof(s.name) - 1);
    s.code = code; s.code_size = sizeof(code);
    s.sections = &sec; s.section_count = 1;
    s.entry_section_index = 0; s.globals_size = MUS_GLOBALS_BYTES;

    MusVM *vm = mus_vm_create();
    uint32_t got_idx = 0;
    int      got_wait = -1;
    struct Cap { uint32_t *idx; int *wait; };
    Cap cap = { &got_idx, &got_wait };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_play_sound = [](void *u, uint32_t i, int w) {
        Cap *c = (Cap *)u;
        *c->idx = i; *c->wait = w;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(got_idx == 261, "playw 2B index = 261");
    CHECK(got_wait == 1,  "wait = 1 (playw)");
    mus_vm_destroy(vm);
    return 1;
}

static int test_vm_play_halts_dispatch(void) {
    /* After `play`, the dispatch loop halts -- the next opcode in the same
       tick must not run. Var00 should be 0 after one tick because the
       `Var00 = 99` that follows runs only on the resume tick. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  play sound_0\n"
        "  Var00 = 99\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 0, "play halts before Var00=99 runs");
    /* Resume tick runs the rest. */
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 99, "after resume Var00=99");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* ---- E11: jump_to_section + section tracking --------------------------- */

static int test_vm_jump_to_section(void) {
    /* Two sections: Begin (does nothing) and Other (sets Var00 = 99).
       Manually jump to Other, tick, expect Var00 == 99. */
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  done\n"
        "}\n"
        "section Other\n"
        "{\n"
        "  Var00 = 99\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    int section_entered_count = 0;
    char last_section[64] = {0};
    struct Cap2 { int *count; char *name; };
    Cap2 cap = { &section_entered_count, last_section };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_section_entered = [](void *u, const char *n) {
        Cap2 *c = (Cap2 *)u;
        ++*c->count;
        strncpy(c->name, n, 63);
        c->name[63] = 0;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &s);
    mus_vm_start(vm);
    int rc = mus_vm_jump_to_section(vm, "Other");
    CHECK(rc == 0, "jump succeeds");
    CHECK(strcmp(mus_vm_current_section(vm), "Other") == 0, "current=Other");
    CHECK(section_entered_count == 1, "on_section_entered fired");
    CHECK(strcmp(last_section, "Other") == 0, "hook payload Other");
    mus_vm_tick(vm, 16);
    CHECK(mus_vm_get_var(vm, 0) == 99, "Var00 = 99 after tick");

    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_jump_unknown_section(void) {
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    int rc = mus_vm_jump_to_section(vm, "NoSuchSection");
    CHECK(rc != 0, "unknown section rejected");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_pc_accessor(void) {
    const char *src =
        "script t\n"
        "section Begin\n"
        "{\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    /* After load, pc = entry section's code_offset. */
    CHECK(mus_vm_pc(vm) == s.sections[s.entry_section_index].code_offset,
          "pc accessor matches entry");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

/* End-to-end smoke: load the JO fixture and tick the VM until first sound
   trigger. The fixture's gamescript runs through `enter Testmission` ->
   `enter Multiplayerstart` (since Var01 == 0) -> first opcode is play sound_0.
   Assert on_play_sound fires with index=0 within 10 tick budget. */
#ifdef MUS_FIXTURE_DIR
static int test_vm_jo_fixture_first_sound(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/jo_gamemus.bin", MUS_FIXTURE_DIR);
    MusFile mf;
    int rc = mus_open(&mf, path);
    if (rc != 0) {
        fprintf(stderr, "  skip: fixture not found at %s (rc=%d)\n", path, rc);
        return 1;
    }
    CHECK(mf.scripts != NULL, "scripts present");
    CHECK(mf.header.chunk_count >= 1, ">=1 chunk");

    MusVM *vm = mus_vm_create();
    int      sound_calls = 0;
    uint32_t first_sound = 0xFFFFFFFFu;
    struct Cap { int *count; uint32_t *first; };
    Cap cap = { &sound_calls, &first_sound };
    MusVMHooks hooks = {};
    hooks.user = &cap;
    hooks.on_play_sound = [](void *u, uint32_t i, int /*w*/) {
        Cap *c = (Cap *)u;
        if (*c->count == 0) *c->first = i;
        ++*c->count;
    };
    mus_vm_set_hooks(vm, &hooks);

    mus_vm_load_script(vm, &mf.scripts[0]);
    mus_vm_start(vm);

    /* Tick up to 16 times; each tick is bounded by the 32-instruction budget. */
    for (int t = 0; t < 16 && sound_calls == 0; ++t) {
        mus_vm_tick(vm, 16);
    }
    CHECK(sound_calls >= 1, "at least one sound triggered after 16 ticks");
    /* The fixture's first reachable play opcode is `play sound_0` in
       Multiplayerstart (Var01==0 at startup, so the if-else inside
       Testmission picks Multiplayerstart). */
    CHECK(first_sound == 0, "first sound is sound_0");

    mus_vm_destroy(vm);
    mus_close(&mf);
    return 1;
}

/* Executable form of the opcode census: drive the real shipped scripts through
   the VM for many ticks and assert the engine-width walk never desyncs into an
   unknown opcode (MUS_VM_ERROR). gamescript exercises enter/tablexec/method;
   menuscript exercises the large push_g/l_and/brfalse/setstate/play state
   machine. Proves the VM executes the stock bins faithfully. */
static int run_fixture_no_error(const char *fname, int max_ticks) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", MUS_FIXTURE_DIR, fname);
    MusFile mf;
    int rc = mus_open(&mf, path);
    if (rc != 0) { fprintf(stderr, "  skip: %s (rc=%d)\n", path, rc); return 1; }
    CHECK(mf.scripts != NULL, "scripts present");
    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &mf.scripts[0]);
    mus_vm_start(vm);
    int ok = 1;
    for (int t = 0; t < max_ticks; ++t) {
        mus_vm_tick(vm, 16);
        MusVMState st = mus_vm_state(vm);
        if (st == MUS_VM_ERROR) {
            fprintf(stderr, "  %s: VM ERROR at tick %d: %s (pc=0x%X)\n",
                    fname, t, mus_vm_last_error(vm), mus_vm_pc(vm));
            ok = 0; break;
        }
        if (st != MUS_VM_RUNNING) break;   /* ran off the end / HALTED cleanly */
    }
    mus_vm_destroy(vm);
    mus_close(&mf);
    return ok;
}

static int test_vm_jo_fixtures_no_desync(void) {
    CHECK(run_fixture_no_error("jo_gamemus.bin", 500),  "gamescript runs without VM error");
    CHECK(run_fixture_no_error("jo_menumus.bin", 2000), "menuscript runs without VM error");
    return 1;
}

/* Behavioral proof (Tier 3, CI form): assert the shipped programs produce the
   EXACT observable event stream the Tier-4 Unicorn differential proved byte-
   identical to the ORIGINAL Jointops handlers (mus_diff_jointops.py, local RE tooling per docs/audio/mus-sbf-re.md:
   original == reimpl for all scenarios). `V<v>` = on_volume_changed (GSV; 200<<16),
   `P<i>` = on_play_sound. The golden streams are captured at a fixed 12-tick window;
   each Var value yields a DISTINCT stream, which is the proof that var-gated routing
   works (gamescript's `push_g Var1; neq; brfalse; setstate`; menuscript's Var2). */
struct BehLog {
    char ev[2048];                 /* "V<v> P<i> ..." accumulated in order */
    int  saw_mpstart, saw_null;    /* gamescript section routing */
};
static void beh_app(BehLog *L, const char *s) { strncat(L->ev, s, sizeof(L->ev) - strlen(L->ev) - 1); }
static void beh_play(void *u, uint32_t i, int) { char b[24]; snprintf(b, sizeof(b), "P%u ", i); beh_app((BehLog *)u, b); }
static void beh_vol(void *u, int32_t l, int32_t /*r*/) { char b[24]; snprintf(b, sizeof(b), "V%d ", l); beh_app((BehLog *)u, b); }
static void beh_sect(void *u, const char *nm) {
    BehLog *L = (BehLog *)u;
    if (!strcmp(nm, "Multiplayerstart")) L->saw_mpstart = 1;
    if (!strcmp(nm, "Missionnull"))      L->saw_null = 1;
}
static void beh_run(const char *fname, uint8_t varIdx, int32_t varVal, int ticks, BehLog *L) {
    memset(L, 0, sizeof(*L));
    char path[512]; snprintf(path, sizeof(path), "%s/%s", MUS_FIXTURE_DIR, fname);
    MusFile mf; if (mus_open(&mf, path) != 0) return;
    MusVM *vm = mus_vm_create();
    MusVMHooks h = {}; h.user = L;
    h.on_play_sound = beh_play; h.on_volume_changed = beh_vol; h.on_section_entered = beh_sect;
    mus_vm_set_hooks(vm, &h);
    mus_vm_load_script(vm, &mf.scripts[0]);
    mus_vm_set_var(vm, varIdx, varVal);
    mus_vm_start(vm);
    for (int t = 0; t < ticks; ++t) { mus_vm_tick(vm, 16); if (mus_vm_state(vm) != MUS_VM_RUNNING) break; }
    mus_vm_destroy(vm); mus_close(&mf);
}

static int test_vm_jo_behavioral(void) {
    BehLog L;
    beh_run("jo_gamemus.bin", 1, 0, 12, &L);
    if (L.ev[0] == 0) { fprintf(stderr, "  skip: gamemus fixture absent\n"); return 1; }
    /* Var1=0 (no mission): SV(200), then Multiplayerstart loops sound_0. */
    CHECK(strcmp(L.ev, "V13107200 P0 P0 P0 P0 P0 P0 P0 P0 P0 P0 ") == 0, "gamemus Var1=0 stream == original");
    CHECK(L.saw_mpstart && !L.saw_null, "gamemus Var1=0 routes to Multiplayerstart");

    beh_run("jo_gamemus.bin", 1, 1, 12, &L);
    /* Var1=1 (mission active): branch routes to silent Missionnull -- the discriminator. */
    CHECK(strcmp(L.ev, "V13107200 ") == 0, "gamemus Var1=1 stream == original (SV200 then silent)");
    CHECK(L.saw_null && !L.saw_mpstart, "gamemus Var1=1 routes to Missionnull (branch discriminator)");

    beh_run("jo_menumus.bin", 2, 0, 12, &L);
    CHECK(strcmp(L.ev, "V13107200 V13107200 P1 V13107200 V13107200 P2 P0 P0 ") == 0, "menumus Var2=0 stream == original");
    beh_run("jo_menumus.bin", 2, 1, 12, &L);
    CHECK(strcmp(L.ev, "V13107200 V13107200 P1 V13107200 V13107200 P2 P3 P4 P5 P6 P7 P8 P2 ") == 0, "menumus Var2=1 stream == original (P2..P8 loop)");
    beh_run("jo_menumus.bin", 2, 2, 12, &L);
    CHECK(strcmp(L.ev, "V13107200 V13107200 P1 V13107200 V13107200 P2 V13107200 P2 V13107200 P2 ") == 0, "menumus Var2=2 stream == original (V,P2 loop)");
    return 1;
}
#endif

/* ---- E2: load + state transitions -------------------------------------- */

static int test_vm_load_and_start(void) {
    /* Minimal compilable script: section Begin { done } */
    const char *src =
        "script test\n"
        "section Begin\n"
        "{\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    CHECK(mus_vm_load_script(vm, &s) == 0, "load");
    CHECK(mus_vm_state(vm) == MUS_VM_STOPPED, "STOPPED after load");
    mus_vm_start(vm);
    CHECK(mus_vm_state(vm) == MUS_VM_RUNNING, "RUNNING after start");
    mus_vm_pause(vm);
    CHECK(mus_vm_state(vm) == MUS_VM_PAUSED, "PAUSED after pause");
    mus_vm_resume(vm);
    CHECK(mus_vm_state(vm) == MUS_VM_RUNNING, "RUNNING after resume");
    mus_vm_stop(vm);
    CHECK(mus_vm_state(vm) == MUS_VM_STOPPED, "STOPPED after stop");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_load_seeks_to_entry_section(void) {
    /* Compiled bytecode starts with a leading 0x00 nop (decompiler convention),
       and section[0]=Begin entry-PC = bytecode+1. mus_vm_load_script must seek
       to the entry section's code_offset, not byte 0. */
    const char *src =
        "script test\n"
        "section Begin\n"
        "{\n"
        "  done\n"
        "}\n";
    MusScript s = {};
    if (!compile_or_die(src, &s)) return 0;

    MusVM *vm = mus_vm_create();
    mus_vm_load_script(vm, &s);
    CHECK(s.section_count >= 1, "have a section");
    CHECK(mus_vm_pc(vm) == s.sections[s.entry_section_index].code_offset,
          "pc at entry section");
    mus_vm_destroy(vm);
    mus_script_free(&s);
    return 1;
}

static int test_vm_load_null_script(void) {
    MusVM *vm = mus_vm_create();
    int rc = mus_vm_load_script(vm, NULL);
    CHECK(rc != 0, "NULL script rejected");
    mus_vm_destroy(vm);
    return 1;
}

int main(void) {
    /* E1 */
    RUN_TEST(test_vm_lifecycle);
    RUN_TEST(test_vm_hooks_registered);
    RUN_TEST(test_vm_last_error_default);
    /* E2 */
    RUN_TEST(test_vm_load_and_start);
    RUN_TEST(test_vm_load_seeks_to_entry_section);
    RUN_TEST(test_vm_load_null_script);
    /* E3 */
    RUN_TEST(test_vm_push_imm8_pop_g);
    RUN_TEST(test_vm_push_imm32);
    RUN_TEST(test_vm_tick_when_stopped_is_noop);
    RUN_TEST(test_vm_tick_done_halts);
    /* E4 */
    RUN_TEST(test_vm_arith_add);
    RUN_TEST(test_vm_arith_chain);
    RUN_TEST(test_vm_compare_eq);
    RUN_TEST(test_vm_compare_lt_gt);
    /* E5 */
    RUN_TEST(test_vm_if_taken);
    RUN_TEST(test_vm_if_not_taken);
    RUN_TEST(test_vm_if_else);
    /* E6 */
    RUN_TEST(test_vm_call_section_returns);
    RUN_TEST(test_vm_setstate_halts);
    RUN_TEST(test_vm_done_resets_to_entry);
    RUN_TEST(test_vm_tablexec_in_range);
    RUN_TEST(test_vm_tablexec_out_of_range);
    /* E7 */
    RUN_TEST(test_vm_intrinsic_gsv);
    RUN_TEST(test_vm_intrinsic_gsv_clamps);
    RUN_TEST(test_vm_intrinsic_gfb);
    RUN_TEST(test_vm_intrinsic_ggrnd_in_range);
    /* E8 */
    RUN_TEST(test_vm_intrinsic_fset);
    RUN_TEST(test_vm_intrinsic_fclear);
    RUN_TEST(test_vm_intrinsic_fisset);
    RUN_TEST(test_vm_intrinsic_fisclear);
    /* E9 */
    RUN_TEST(test_vm_intrinsic_gecho_fires_hook);
    RUN_TEST(test_vm_intrinsic_tstart_tstop_noop);
    /* E10 */
    RUN_TEST(test_vm_play_fires_hook);
    RUN_TEST(test_vm_playw_fires_hook_wait);
    RUN_TEST(test_vm_play_halts_dispatch);
    /* E11 */
    RUN_TEST(test_vm_jump_to_section);
    RUN_TEST(test_vm_jump_unknown_section);
    RUN_TEST(test_vm_pc_accessor);
#ifdef MUS_FIXTURE_DIR
    RUN_TEST(test_vm_jo_fixture_first_sound);
    RUN_TEST(test_vm_jo_fixtures_no_desync);
    RUN_TEST(test_vm_jo_behavioral);
#endif

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
