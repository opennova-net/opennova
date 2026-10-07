/* Focused VM regression for the grill fixes (D-MUS-2/3/9/10).
   Drives the public mus_vm_* API over hand-built bytecode. Plain main() style. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <formats/mus/mus.h>

using namespace opennova::mus;

static int passed = 0, failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); ++failed; } \
    else { ++passed; } } while (0)

static uint32_t rol(uint32_t v, int s){ return (v << s) | (v >> (32 - s)); }

typedef struct VarCap {
    int count;
    int last_idx;
    int last_value;
} VarCap;

static void capture_var(void *user, uint8_t idx, int32_t v) {
    VarCap *cap = (VarCap *)user;
    ++cap->count;
    cap->last_idx = (int)idx;
    cap->last_value = (int)v;
}

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
    /* FIsClear never resolves in retail (resolver bound 8 @0x84F20C): both
       calls run the NULL-handler path and push 0. */
    CHECK(mus_vm_get_var(vm, 1) == 0, "FIsClear unresolved -> 0 (bits clear case)");
    CHECK(mus_vm_get_var(vm, 6) == 0, "FIsClear unresolved -> 0 (a bit set case)");
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

    /* ---- Script 4: enter (0x38) pops N dwords + frame offset (D-NEW-2 / D-MUS-6)
       [orig: AudioVM_Op_Enter @ 0x672C20: sub ebp,N*4 (pop) + dst=LocalsBase+
       instance[+0x3C] (frame, 0x20 in JO/MDEdit)].
         push 5; push 7; push 99; enter 2; push_l 0x20; pop_g 4; pop_g 0; done
       enter copies the top 2 (7,99) to locals[0x20]/[0x24] AND pops them, leaving
       [5]. push_l 0x20 -> 7; pop_g 4 -> Var1=7; pop_g 0 -> Var0=5.
       If enter did NOT pop (the old bug), TOS after enter would be 99 and Var0
       would be 99, not 5. If the frame were not 0x20, push_l 0x20 would read 0. */
    uint8_t code4[] = { 0x01,5, 0x01,7, 0x01,99, 0x38,0x02, 0x04,0x20,
                        0x08,0x04, 0x08,0x00, 0x3F };
    MusScript s4 = make_script(code4, sizeof(code4), &sec);
    MusVM *vm4 = mus_vm_create();
    mus_vm_load_script(vm4, &s4);
    mus_vm_start(vm4);
    mus_vm_tick(vm4, 16);
    CHECK(mus_vm_state(vm4) != MUS_VM_ERROR, "enter script ran without error");
    CHECK(mus_vm_get_var(vm4, 1) == 7, "enter wrote arg0 to locals[frame=0x20]");
    CHECK(mus_vm_get_var(vm4, 0) == 5, "enter popped its 2 args (TOS left = 5, not 99)");
    mus_vm_destroy(vm4);

    /* ---- Script 5: inc_g/dec_g mutate bytes but do not fire dirty/var hooks ----
       [orig: AudioVM_Op_IncGlobal @ 0x672AE0 / AudioVM_Op_DecGlobal @ 0x672AF0] only inc/dec
       globals[off] and CLC. The editor may poll vars for display, but the VM
       callback must remain tied to witnessed dirty writes such as pop_g. */
    uint8_t code5[] = { 0x01,10, 0x08,0x00, 0x28,0x00, 0x29,0x00, 0x3F };
    MusScript s5 = make_script(code5, sizeof(code5), &sec);
    MusVM *vm5 = mus_vm_create();
    VarCap cap; memset(&cap, 0, sizeof(cap));
    MusVMHooks hooks; memset(&hooks, 0, sizeof(hooks));
    hooks.user = &cap;
    hooks.on_var_changed = capture_var;
    mus_vm_set_hooks(vm5, &hooks);
    mus_vm_load_script(vm5, &s5);
    mus_vm_start(vm5);
    mus_vm_tick(vm5, 16);
    CHECK(mus_vm_get_var(vm5, 0) == 10, "inc_g then dec_g leaves Var00 value intact");
    CHECK(cap.count == 1, "only pop_g fired on_var_changed; inc_g/dec_g stayed silent");
    CHECK(cap.last_idx == 0 && cap.last_value == 10, "callback payload came from pop_g");
    mus_vm_destroy(vm5);

    /* ---- Script 6: a tablexec entry that dispatches a forward `setstate` must
       NOT halt the tick (found by the Tier-4 Unicorn differential vs Jointops).
       [orig: AudioVM_Op_TableExec @ 0x672C05: after the embedded `call eax`, the
       `cmp esi, entry_end` OVERWRITES the carry the setstate set; a forward
       target (esi >= entry_end) clears carry, so execution CONTINUES in the same
       tick.] Layout: sec0 { push 0; tablexec{ setstate sec1 } }  sec1 { Var0=42 }.
       With the fix, ONE tick reaches Var0=42; with the old halt-propagation bug it
       took two ticks (Var0 would still be 0 after the first). */
    uint8_t code6[] = {
        /* sec0 @ 0 */ 0x01,0x00, 0x35, 0x01,0x3b,0x02,0x07, 0x3b,0x01, 0x3f,
        /* sec1 @10 */ 0x01,0x2a, 0x08,0x00, 0x3f
    };
    MusSection sec6[2]; memset(sec6, 0, sizeof(sec6));
    strcpy(sec6[0].name, "S0"); sec6[0].code_offset = 0;
    strcpy(sec6[1].name, "S1"); sec6[1].code_offset = 10;
    MusScript s6; memset(&s6, 0, sizeof(s6));
    strcpy(s6.name, "t"); s6.code = code6; s6.code_size = sizeof(code6);
    s6.sections = sec6; s6.section_count = 2; s6.entry_section_index = 0;
    s6.globals_size = 68; s6.locals_size = 0;
    MusVM *vm6 = mus_vm_create();
    mus_vm_load_script(vm6, &s6);
    mus_vm_start(vm6);
    mus_vm_tick(vm6, 16);   /* exactly ONE tick */
    CHECK(mus_vm_get_var(vm6, 0) == 42, "tablexec forward setstate continues the SAME tick (Var0=42 after 1 tick)");
    CHECK(mus_vm_state(vm6) != MUS_VM_ERROR, "tablexec-setstate script ran without error");
    /* A script with no MessageHandler: the restart frame is refused (retail
       would read through a null chunk pointer) and nothing moves. */
    {
        uint32_t pc_before = mus_vm_pc(vm6);
        CHECK(mus_vm_signal(vm6, 1) == -2, "signal on a handler-less script returns -2");
        CHECK(mus_vm_pc(vm6) == pc_before && mus_vm_call_depth(vm6) == 0,
              "a refused restart frame pushes nothing");
    }
    mus_vm_destroy(vm6);

    /* ---- Script 7: the MessageHandler restart frame ----
       [orig: sub_672E50 @0x672e95..0x672ec1 (push IP on the return stack, push
       (value, 0), IP = chunk+0x40, dispatch); MusicCtx_SelectEndTrack
       @0x672fd0; Server_ProcessRoundEnd SP tail @0x51696b (1) / @0x51698f (2)].
       The sections mirror the retail gamemus.bin shape (Start/Idle, the
       Missionwin -> WinTracks pair playing entries 2..7, the Missionlose ->
       LoseTracks pair playing 8..12) and the handler is the retail bytecode:
       enter 2; method 4 (GFB); empty; push_l 0x20; tablexec 3 (inner 0x3b,
       2-byte entries) = [setstate 1 | setstate 2 | setstate 4]. */
    uint8_t code7[] = {
        /* sec0 Start       @0  */ 0x3E,0x00, 0x3B,0x01,
        /* sec1 Idle        @4  */ 0x3E,0x01, 0x3B,0x01,
        /* sec2 Missionwin  @8  */ 0x3B,0x03,
        /* sec3 WinTracks   @10 */ 0x3E,0x02, 0x3E,0x03, 0x3E,0x04, 0x3E,0x05, 0x3E,0x06, 0x3E,0x07, 0x3B,0x01,
        /* sec4 Missionlose @24 */ 0x3B,0x05,
        /* sec5 LoseTracks  @26 */ 0x3E,0x08, 0x3E,0x09, 0x3E,0x0A, 0x3E,0x0B, 0x3E,0x0C, 0x3B,0x01,
        /* handler          @38 */ 0x38,0x02, 0x40,0x04, 0x0F, 0x04,0x20,
                                   0x35, 0x03,0x3B,0x02,0x0B, 0x3B,0x01, 0x3B,0x02, 0x3B,0x04
    };
    MusSection sec7[6]; memset(sec7, 0, sizeof(sec7));
    const char *names7[6] = {"Start", "Idle", "Missionwin", "WinTracks", "Missionlose", "LoseTracks"};
    const uint32_t offs7[6] = {0, 4, 8, 10, 24, 26};
    for (int i = 0; i < 6; ++i) { strcpy(sec7[i].name, names7[i]); sec7[i].code_offset = offs7[i]; }
    MusScript s7; memset(&s7, 0, sizeof(s7));
    strcpy(s7.name, "gamescript"); s7.code = code7; s7.code_size = sizeof(code7);
    s7.sections = sec7; s7.section_count = 6; s7.entry_section_index = 0;
    s7.globals_size = 68; s7.locals_size = 0x28;
    s7.message_handler_offset = 38; s7.has_message_handler = 1;

    struct PlayCap { int count; int idx[32]; } plays = {0, {0}};
    MusVMHooks hooks7; memset(&hooks7, 0, sizeof(hooks7));
    hooks7.user = &plays;
    hooks7.on_play_sound = [](void *user, uint32_t idx, int) {
        PlayCap *cap = (PlayCap *)user;
        if (cap->count < 32) cap->idx[cap->count] = (int)idx;
        ++cap->count;
    };
    MusVM *vm7 = mus_vm_create();
    CHECK(mus_vm_load_script(vm7, &s7) == 0, "load s7");
    mus_vm_set_hooks(vm7, &hooks7);
    mus_vm_start(vm7);
    mus_vm_tick(vm7, 16);   /* Start: play 0, halt at the setstate */
    CHECK(plays.count == 1 && plays.idx[0] == 0, "the entry section plays entry 0");
    CHECK(mus_vm_call_depth(vm7) == 0, "no frame before the signal");

    /* signal(1): the frame lands in Missionwin at once; the interrupted pc
       sits on the return stack (setstate jumps away, nothing pops it). */
    plays.count = 0;
    CHECK(mus_vm_signal(vm7, 1) == 0, "signal(1) runs the restart frame");
    CHECK(strcmp(mus_vm_current_section(vm7), "Missionwin") == 0, "value 1 -> setstate 2 (Missionwin)");
    CHECK(mus_vm_pc(vm7) == 8, "pc = the Missionwin entry");
    CHECK(mus_vm_call_depth(vm7) == 1, "the interrupted pc holds on the return stack");
    CHECK(mus_vm_state(vm7) == MUS_VM_RUNNING, "the frame leaves the VM running");
    for (int t = 0; t < 8; ++t) mus_vm_tick(vm7, 16);   /* setstate 3; play 2..7; setstate 1 */
    CHECK(plays.count == 6, "Missionwin plays six entries");
    {
        int ok = plays.count == 6;
        for (int i = 0; ok && i < 6; ++i) ok = plays.idx[i] == 2 + i;
        CHECK(ok, "the win sting is entries 2..7 in order");
    }
    CHECK(strcmp(mus_vm_current_section(vm7), "Idle") == 0, "the win sting returns to the idle section");

    /* signal(2): Missionlose -> LoseTracks plays 8..12 then idles. */
    plays.count = 0;
    CHECK(mus_vm_signal(vm7, 2) == 0, "signal(2) runs the restart frame");
    CHECK(strcmp(mus_vm_current_section(vm7), "Missionlose") == 0, "value 2 -> setstate 4 (Missionlose)");
    for (int t = 0; t < 7; ++t) mus_vm_tick(vm7, 16);   /* setstate 5; play 8..12; setstate 1 */
    CHECK(plays.count == 5, "Missionlose plays five entries");
    {
        int ok = plays.count == 5;
        for (int i = 0; ok && i < 5; ++i) ok = plays.idx[i] == 8 + i;
        CHECK(ok, "the lose sting is entries 8..12 in order");
    }
    CHECK(strcmp(mus_vm_current_section(vm7), "Idle") == 0, "the lose sting returns to the idle section");
    CHECK(mus_vm_call_depth(vm7) == 2, "each frame leaves one interrupted pc behind");

    /* signal(0): the handler's first table entry is the idle section. */
    CHECK(mus_vm_signal(vm7, 0) == 0, "signal(0) runs the restart frame");
    CHECK(strcmp(mus_vm_current_section(vm7), "Idle") == 0 && mus_vm_pc(vm7) == 4,
          "value 0 -> setstate 1 (Idle)");

    /* The frame ignores the embedder's streaming state: a stopped VM with a
       loaded script still takes it [orig: sub_672E50 tests only ctx+32 and the
       chunk pointer; MusicCtx_SelectEndTrack @0x672fd0 tests chunk_04 alone].
       Retail's context stop is not this state: AudioVM_StopMusicContext
       @0x671e00 frees the instance and zeroes chunk_04 (AudioVM_FreeSoundBuffer
       @0x672e25), which the port does by unloading the script (MusicService
       stop_context; the unload leg below). */
    mus_vm_stop(vm7);
    CHECK(mus_vm_signal(vm7, 1) == 0, "signal on a stopped VM still runs");
    CHECK(strcmp(mus_vm_current_section(vm7), "Missionwin") == 0, "the stopped VM lands in Missionwin");
    CHECK(mus_vm_state(vm7) == MUS_VM_STOPPED, "the frame does not restart the embedder state");
    mus_vm_unload_script(vm7);
    CHECK(mus_vm_signal(vm7, 1) == -1, "after the context stop (script unloaded) the signal runs nothing");
    CHECK(mus_vm_signal(NULL, 1) == -1, "signal without a VM is refused");
    mus_vm_destroy(vm7);

    /* The +0x40 pointer round-trips the container: the encoder writes it
       chunk-relative like the section entries, the parser normalises it back
       [orig: AudioVM_FixupPointers @0x672495 vmData[16]]. */
    {
        const MusScript *arr[1] = { &s7 };
        uint8_t *buf = NULL; size_t buf_size = 0;
        CHECK(mus_encode_file(arr, 1, &buf, &buf_size) == 0 && buf != NULL, "encode the handler script");
        if (buf) {
            MusFile mf; memset(&mf, 0, sizeof(mf));
            CHECK(mus_open_memory(&mf, buf, buf_size) == 0, "reopen the encoded script");
            CHECK(mf.header.chunk_count == 1 && mf.scripts != NULL, "one chunk");
            if (mf.scripts) {
                CHECK(mf.scripts[0].has_message_handler == 1, "the handler pointer survives the container");
                CHECK(mf.scripts[0].message_handler_offset == 38, "and is bytecode-relative again");
                CHECK(mf.scripts[0].code_size == sizeof(code7), "the bytecode region is intact");
                /* The reopened script runs the same frame. */
                MusVM *vm8 = mus_vm_create();
                mus_vm_load_script(vm8, &mf.scripts[0]);
                mus_vm_start(vm8);
                CHECK(mus_vm_signal(vm8, 2) == 0 && mus_vm_pc(vm8) == 24,
                      "the reopened handler dispatches value 2 to Missionlose");
                mus_vm_destroy(vm8);
            }
            mus_close(&mf);
            mus_free(buf);
        }
        /* A handler-less script encodes +0x40 = 0 and parses back without one. */
        const MusScript *arr6[1] = { &s6 };
        buf = NULL; buf_size = 0;
        CHECK(mus_encode_file(arr6, 1, &buf, &buf_size) == 0 && buf != NULL, "encode the plain script");
        if (buf) {
            MusFile mf; memset(&mf, 0, sizeof(mf));
            CHECK(mus_open_memory(&mf, buf, buf_size) == 0, "reopen the plain script");
            CHECK(mf.scripts && mf.scripts[0].has_message_handler == 0, "no handler pointer, no handler");
            mus_close(&mf);
            mus_free(buf);
        }
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
