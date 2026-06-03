/* Focused VM regression for the grill fixes (D-MUS-2/3/9/10).
   Drives the public mus_vm_* API over hand-built bytecode. Plain main() style. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); ++failed; } \
    else { ++passed; } } while (0)

static uint32_t rol(uint32_t v, int s){ return (v << s) | (v >> (32 - s)); }

static MusScript make_script(uint8_t *code, uint32_t n, MusSection *sec) {
    MusScript s; memset(&s, 0, sizeof(s));
    strcpy(s.name, "t");
    s.code = code; s.code_size = n;
    s.sections = sec; s.section_count = 1; s.entry_section_index = 0;
    s.globals_size = 68; s.locals_size = 0;
    return s;
}

int main(void) {
    MusSection sec; memset(&sec, 0, sizeof(sec)); strcpy(sec.name, "Begin"); sec.code_offset = 0;

    /* ---- Script 1: GGRnd (D-MUS-10), FIsClear true/false (D-MUS-9),
            block-copy 0x0A + 2-byte push_ga (D-MUS-2) ---- */
    uint8_t code1[] = {
        /* Var00 = ggrnd(10,20)  (push lo, push hi, method GGRnd) */
        0x01, 10,  0x01, 20,  0x40, 0x01,  0x08, 0x00,
        /* Var02 = 0x05 */
        0x01, 0x05,  0x08, 0x08,
        /* Var01 = FIsClear(mask=0x02, &Var02): 0x05&0x02==0 -> -1 */
        0x01, 0x02,  0x05, 0x08, 0x00,  0x40, 0x08,  0x08, 0x04,
        /* Var06 = FIsClear(mask=0x01, &Var02): 0x05&0x01!=0 -> 0 */
        0x01, 0x01,  0x05, 0x08, 0x00,  0x40, 0x08,  0x08, 0x18,
        /* Var05 = block-copy of Var02 (push_ga Var02; 0x0A dst=20,count=4) */
        0x05, 0x08, 0x00,  0x0A, 0x14, 0x04,
        0x3F /* done */
    };
    MusScript s1 = make_script(code1, sizeof(code1), &sec);
    MusVM *vm = mus_vm_create();
    CHECK(vm != NULL, "create vm");
    CHECK(mus_vm_load_script(vm, &s1) == 0, "load s1");
    mus_vm_start(vm);
    mus_vm_tick(vm, 16);

    /* expected GGRnd (first call -> seed starts 0xBABEFACE) */
    uint32_t seed = 0xBABEFACEu;
    seed = rol(seed + rol(seed, 11), 2);
    int32_t exp_rnd = 10 + (int32_t)((seed & 0xFFFFu) % 11);
    int32_t got = mus_vm_get_var(vm, 0);
    CHECK(got == exp_rnd, "GGRnd exact rol32 PRNG value");
    CHECK(got >= 10 && got <= 20, "GGRnd result in [lo,hi]");
    CHECK(mus_vm_get_var(vm, 1) == -1, "FIsClear true -> -1 (bits clear)");
    CHECK(mus_vm_get_var(vm, 6) == 0,  "FIsClear false -> 0 (a bit set)");
    CHECK(mus_vm_get_var(vm, 5) == 5,  "0x0A block-copy Var02 -> Var05");
    CHECK(mus_vm_state(vm) != MUS_VM_ERROR, "s1 ran without VM error");
    mus_vm_destroy(vm);

    /* ---- Script 2: empty (0x0F) full drain (D-MUS-3) ----
       push 1,2,3; empty; add. If empty drains the whole stack, add underflows
       -> ERROR. If it only dropped TOS, add would succeed (no error). */
    uint8_t code2[] = { 0x01,1, 0x01,2, 0x01,3, 0x0F, 0x10, 0x3F };
    MusScript s2 = make_script(code2, sizeof(code2), &sec);
    MusVM *vm2 = mus_vm_create();
    mus_vm_load_script(vm2, &s2);
    mus_vm_start(vm2);
    mus_vm_tick(vm2, 16);
    CHECK(mus_vm_state(vm2) == MUS_VM_ERROR, "empty fully drains stack (add underflows)");
    mus_vm_destroy(vm2);

    /* ---- Script 3: pushstr (0x07) does not fault, IP stays aligned ----
       pushstr 0; pop_g Var00; done. Must reach done cleanly (no unknown-opcode). */
    uint8_t code3[] = { 0x07, 0x00, 0x08, 0x00, 0x3F };
    MusScript s3 = make_script(code3, sizeof(code3), &sec);
    MusVM *vm3 = mus_vm_create();
    mus_vm_load_script(vm3, &s3);
    mus_vm_start(vm3);
    mus_vm_tick(vm3, 16);
    CHECK(mus_vm_state(vm3) != MUS_VM_ERROR, "0x07 pushstr handled (no unknown-opcode error)");
    CHECK(mus_vm_get_var(vm3, 0) == 0, "pushstr pushes 0 placeholder");
    mus_vm_destroy(vm3);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
