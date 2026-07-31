/* MUS bytecode VM (interpreter).

   Witnessed dispatch loop: Jointops.exe!AudioVM_DispatchLoop @ 0x00672720.
   Witnessed opcode table: Jointops.exe!g_vm_opcode_dispatch_table @ 0x0084F220
   (65 entries, 0x00..0x40). Stack element size = 4 bytes (int32).
   Two stacks: data (EBP-tracked, 256 entries here) + call (EDI-tracked,
   64 frames here). 32-instruction budget per dispatch loop call.

   Halt convention (witnessed): the original sets the x86 carry flag (STC)
   from inside `play`/`playw`/`done`/`setstate` handlers and from
   `VmOp_Empty` (0x0F). The dispatch loop also halts when its 32-instruction
   budget hits zero. We model that with an explicit `halt` flag set by the
   handlers and inspected by the dispatch loop. */

#include "mus/mus.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/* Static stack/area sizes. The original VM has 4 KB+ for both stacks (stack
   element 4 B, ~1024 frames each). 256 / 64 is more than enough headroom for
   any sane authored MUS script and keeps the per-VM allocation small. */
constexpr int kDataStackCap = 256;
constexpr int kCallStackCap = 64;
constexpr int kLocalsCap    = 256;
constexpr int kTickBudget   = 32;   /* witnessed: dword_3246B24 = 0x20 */

/* Globals area is byte-addressable per the witness (inc_g/dec_g operate on
   1 byte at the indexed offset, NOT on a full int32). MUS_GLOBALS_BYTES is
   68 to fit Var00..Var15 + 1 user var. We expose it as a byte buffer here
   and read int32s out via memcpy for stack interop. */
constexpr int kGlobalsBytes = MUS_GLOBALS_BYTES;
constexpr int kLocalsBytes  = kLocalsCap * 4;

}   /* anonymous namespace */

struct MusVM {
    const MusScript *script;
    MusVMHooks       hooks;
    MusVMState       state;

    /* Data stack (top grows up; sp = next-free). */
    int32_t  data_stack[kDataStackCap];
    int      sp;

    /* Call stack: 4-byte return PCs. */
    uint32_t call_stack[kCallStackCap];
    int      csp;

    /* Globals + locals as raw bytes for byte-granular access. */
    uint8_t  globals[kGlobalsBytes];
    uint8_t  locals [kLocalsBytes];

    /* Flag bitmap (FSet/FClear/FIsSet). 64 flags. */
    uint64_t flags;

    /* Bytecode-relative PC. */
    uint32_t pc;

    /* Halt latch: set by play/playw/done/setstate/empty handlers; cleared at
       the top of every mus_vm_tick() call. */
    int      halt_latch;

    /* Diagnostic strings. last_error = "" treated as "ok". current_section
       has +1 for NUL. */
    char     last_error[256];
    char     current_section_name[MUS_SECTION_NAME_SIZE + 1];
};

/* ---- E1: lifecycle + hooks --------------------------------------------- */

extern "C" MusVM *mus_vm_create(void) {
    MusVM *vm = (MusVM *)calloc(1, sizeof(MusVM));
    if (!vm) return NULL;
    vm->state = MUS_VM_STOPPED;
    return vm;
}

extern "C" void mus_vm_destroy(MusVM *vm) {
    free(vm);
}

extern "C" void mus_vm_set_hooks(MusVM *vm, const MusVMHooks *hooks) {
    if (!vm) return;
    if (hooks) vm->hooks = *hooks;
    else memset(&vm->hooks, 0, sizeof(vm->hooks));
}

extern "C" MusVMState mus_vm_state(const MusVM *vm) {
    return vm ? vm->state : MUS_VM_STOPPED;
}

extern "C" const char *mus_vm_last_error(const MusVM *vm) {
    if (!vm) return "ok";
    return vm->last_error[0] ? vm->last_error : "ok";
}

extern "C" const char *mus_vm_current_section(const MusVM *vm) {
    return vm ? vm->current_section_name : "";
}

extern "C" uint32_t mus_vm_pc(const MusVM *vm) {
    return vm ? vm->pc : 0;
}

/* ---- E2: load + start/stop/pause/resume -------------------------------- */

extern "C" int mus_vm_load_script(MusVM *vm, const MusScript *s) {
    if (!vm || !s) return -1;
    vm->script = s;
    vm->state  = MUS_VM_STOPPED;
    vm->sp     = 0;
    vm->csp    = 0;
    vm->flags  = 0;
    vm->halt_latch = 0;
    memset(vm->data_stack, 0, sizeof(vm->data_stack));
    memset(vm->call_stack, 0, sizeof(vm->call_stack));
    memset(vm->globals,    0, sizeof(vm->globals));
    memset(vm->locals,     0, sizeof(vm->locals));
    vm->last_error[0] = 0;
    vm->current_section_name[0] = 0;

    /* Witnessed: Jointops.exe!AudioVM_ScriptInstanceInit @ 0x00672D20
       initial_pc = section_table[entry_section_index]. */
    vm->pc = 0;
    if (s->section_count > 0 && s->entry_section_index < s->section_count) {
        const MusSection *entry = &s->sections[s->entry_section_index];
        vm->pc = entry->code_offset;
        size_t n = strlen(entry->name);
        if (n >= sizeof(vm->current_section_name)) n = sizeof(vm->current_section_name) - 1;
        memcpy(vm->current_section_name, entry->name, n);
        vm->current_section_name[n] = 0;
    }
    return 0;
}

extern "C" void mus_vm_start(MusVM *vm) {
    if (!vm || !vm->script) return;
    vm->state = MUS_VM_RUNNING;
}

extern "C" void mus_vm_stop(MusVM *vm) {
    if (!vm) return;
    vm->state = MUS_VM_STOPPED;
}

extern "C" void mus_vm_pause(MusVM *vm) {
    if (!vm) return;
    if (vm->state == MUS_VM_RUNNING) vm->state = MUS_VM_PAUSED;
}

extern "C" void mus_vm_resume(MusVM *vm) {
    if (!vm) return;
    if (vm->state == MUS_VM_PAUSED) vm->state = MUS_VM_RUNNING;
}

/* ---- Stack + globals helpers ------------------------------------------- */

static void vm_push(MusVM *vm, int32_t v) {
    if (vm->sp >= kDataStackCap) {
        snprintf(vm->last_error, sizeof(vm->last_error), "data stack overflow");
        vm->state = MUS_VM_ERROR;
        return;
    }
    vm->data_stack[vm->sp++] = v;
}

static int32_t vm_pop(MusVM *vm) {
    if (vm->sp <= 0) {
        snprintf(vm->last_error, sizeof(vm->last_error), "data stack underflow");
        vm->state = MUS_VM_ERROR;
        return 0;
    }
    return vm->data_stack[--vm->sp];
}

/* Read int32 from globals at byte_offset (little-endian). The globals area
   is byte-addressable (witnessed: inc_g/dec_g operate on a byte). */
static int32_t globals_read32(const MusVM *vm, int byte_off) {
    if (byte_off < 0 || byte_off + 4 > kGlobalsBytes) return 0;
    int32_t v;
    memcpy(&v, vm->globals + byte_off, 4);
    return v;
}

static void globals_write32(MusVM *vm, int byte_off, int32_t v) {
    if (byte_off < 0 || byte_off + 4 > kGlobalsBytes) return;
    memcpy(vm->globals + byte_off, &v, 4);
}

static int32_t locals_read32(const MusVM *vm, int byte_off) {
    if (byte_off < 0 || byte_off + 4 > kLocalsBytes) return 0;
    int32_t v;
    memcpy(&v, vm->locals + byte_off, 4);
    return v;
}

static void locals_write32(MusVM *vm, int byte_off, int32_t v) {
    if (byte_off < 0 || byte_off + 4 > kLocalsBytes) return;
    memcpy(vm->locals + byte_off, &v, 4);
}

/* Notify on_var_changed for a globals byte_offset -> var_index conversion.
   The hook reports var_index = byte_offset / 4 so Var00..Var15 (offsets 0..60)
   map to indices 0..15. */
static void notify_var_changed(MusVM *vm, int byte_off, int32_t v) {
    if (vm->hooks.on_var_changed) {
        uint8_t var_index = (uint8_t)(byte_off / 4);
        vm->hooks.on_var_changed(vm->hooks.user, var_index, v);
    }
}

/* ---- E3: push/pop opcodes + tick dispatch ------------------------------ */

/* Read 1B from bytecode at vm->pc and advance. */
static uint8_t read_u8(MusVM *vm) {
    if (vm->pc >= vm->script->code_size) return 0;
    return vm->script->code[vm->pc++];
}

static uint16_t read_u16(MusVM *vm) {
    if (vm->pc + 2 > vm->script->code_size) {
        vm->pc = vm->script->code_size;
        return 0;
    }
    uint16_t v = (uint16_t)vm->script->code[vm->pc]
               | ((uint16_t)vm->script->code[vm->pc + 1] << 8);
    vm->pc += 2;
    return v;
}

static int32_t read_u32(MusVM *vm) {
    if (vm->pc + 4 > vm->script->code_size) {
        vm->pc = vm->script->code_size;
        return 0;
    }
    int32_t v = (int32_t)((uint32_t)vm->script->code[vm->pc]
              | ((uint32_t)vm->script->code[vm->pc + 1] << 8)
              | ((uint32_t)vm->script->code[vm->pc + 2] << 16)
              | ((uint32_t)vm->script->code[vm->pc + 3] << 24));
    vm->pc += 4;
    return v;
}

/* [orig: AudioVM_Op_PushImm8 @ 0x672790] `movzx eax, byte ptr [esi]; inc esi`
   -- the 1-byte immediate is ZERO-extended to int32 (range 0..255). */
static void op_push_imm8(MusVM *vm) {
    vm_push(vm, (int32_t)(uint8_t)read_u8(vm));
}

/* Witnessed: Jointops.exe!VmOp_PushImm32 @ 0x6727A0. */
static void op_push_imm32(MusVM *vm) {
    vm_push(vm, read_u32(vm));
}

/* Witnessed: Jointops.exe!VmOp_PushGlobal @ 0x6727B0. */
static void op_push_global(MusVM *vm) {
    int byte_off = read_u8(vm);
    vm_push(vm, globals_read32(vm, byte_off));
}

/* Witnessed: Jointops.exe!VmOp_PushLocal @ 0x6727D0. The Jointops.exe operand is a byte
   offset into the locals area (matches globals shape). */
static void op_push_local(MusVM *vm) {
    int byte_off = read_u8(vm);
    vm_push(vm, locals_read32(vm, byte_off));
}

/* Witnessed: Jointops.exe!VmOp_PopGlobal @ 0x672850; sets globals_dirty=1 and we
   fire on_var_changed in its place. */
static void op_pop_global(MusVM *vm) {
    int byte_off = read_u8(vm);
    int32_t v = vm_pop(vm);
    globals_write32(vm, byte_off, v);
    notify_var_changed(vm, byte_off, v);
}

/* Witnessed: Jointops.exe!VmOp_PopLocal @ 0x672870. */
static void op_pop_local(MusVM *vm) {
    int byte_off = read_u8(vm);
    int32_t v = vm_pop(vm);
    locals_write32(vm, byte_off, v);
}

/* Witnessed: Jointops.exe!AudioVM_Op_PushGlobalAddr @ 0x6727F0. Pushes a synthetic
   "address" that downstream method handlers (FSet/FClear/FIsSet/FIsClear) treat as a
   pointer. We can't push a real pointer (we run in our own address space), so encode
   the byte offset and decode it back inside the F* handlers (high bit tags it as an
   address vs a raw value).

   Operand width: the original reads ONE byte as the offset but ADVANCES THE IP BY TWO
   (`movzx eax, byte ptr [esi]; inc esi; inc esi`) -- the 2nd operand byte is reserved
   and ignored. We consume both so the IP stays aligned when running real bytecode. */
static constexpr int32_t kAddrTagBit  = (int32_t)0x40000000;   /* large flag */
static constexpr int32_t kAddrLocalBit= (int32_t)0x20000000;

static void op_push_global_addr(MusVM *vm) {
    int byte_off = read_u8(vm);
    (void)read_u8(vm);                 /* reserved 2nd operand byte (engine advances 2) */
    vm_push(vm, kAddrTagBit | byte_off);
}

/* Witnessed: Jointops.exe!AudioVM_Op_PushLocalAddr @ 0x672810 (same 2-byte operand). */
static void op_push_local_addr(MusVM *vm) {
    int byte_off = read_u8(vm);
    (void)read_u8(vm);                 /* reserved 2nd operand byte (engine advances 2) */
    vm_push(vm, kAddrTagBit | kAddrLocalBit | byte_off);
}

/* Witnessed: Jointops.exe!VmOp_PushSelf @ 0x6728F0; Jointops.exe pushes the current
   instance pointer. We have no analogue; push 0 and document. */
static void op_push_self(MusVM *vm) {
    vm_push(vm, 0);
}

/* Jointops.exe!AudioVM_Op_Empty @ 0x672900: `mov ebp, off_84F218; clc; ret`, i.e. it
   resets the data-stack pointer to the stack base -- a FULL DRAIN of the data stack at
   a statement boundary, not a single pop. (The original `method`/intrinsic ops leave
   their args on the stack and push the result on top; `empty` clears that residue.)
   Does not touch the call stack and does not halt (clc). */
static void op_empty(MusVM *vm) {
    vm->sp = 0;
}

/* Witnessed: Jointops.exe!VmOp_Nop @ 0x672780. */
static void op_nop(MusVM *vm) { (void)vm; }

/* [orig: AudioVM_Op_IncGlobal/DecGlobal @ 0x672AE0/0x672AF0] inc/dec a single
   BYTE at globals[off] (NOT a full int32). D-MUS-5: unlike pop_g, the original
   does NOT raise the globals-dirty signal (dword_3246B28) and fires no embedder
   notification here, so we deliberately omit notify_var_changed to match. */
static void op_inc_g(MusVM *vm) {
    int byte_off = read_u8(vm);
    if (byte_off >= 0 && byte_off < kGlobalsBytes) ++vm->globals[byte_off];
}
static void op_dec_g(MusVM *vm) {
    int byte_off = read_u8(vm);
    if (byte_off >= 0 && byte_off < kGlobalsBytes) --vm->globals[byte_off];
}
static void op_inc_l(MusVM *vm) {
    int byte_off = read_u8(vm);
    if (byte_off >= 0 && byte_off < kLocalsBytes) ++vm->locals[byte_off];
}
static void op_dec_l(MusVM *vm) {
    int byte_off = read_u8(vm);
    if (byte_off >= 0 && byte_off < kLocalsBytes) --vm->locals[byte_off];
}

/* ---- Halt opcodes (forward decl; bodies in later tasks) --------------- */

static void op_done    (MusVM *vm); /* 0x3F: implemented in E6 */
static void op_setstate(MusVM *vm); /* 0x3B: implemented in E6 */
static void op_play    (MusVM *vm); /* 0x3E: implemented in E10 */
static void op_playw   (MusVM *vm); /* 0x3D: implemented in E10 */

/* ---- Dispatch table + tick -------------------------------------------- */

typedef void (*OpHandler)(MusVM *);

static OpHandler kHandlers[256] = {0};
static int       kHandlersInit = 0;

/* Forward decl for handlers added in later tasks. */
static void op_push_str(MusVM *vm);
static void op_pop_global_block(MusVM *vm); static void op_pop_local_block(MusVM *vm);
static void op_add(MusVM *vm); static void op_sub(MusVM *vm);
static void op_mul(MusVM *vm); static void op_div(MusVM *vm);
static void op_mod(MusVM *vm); static void op_l_and(MusVM *vm);
static void op_l_or(MusVM *vm); static void op_b_and(MusVM *vm);
static void op_b_or(MusVM *vm); static void op_b_xor(MusVM *vm);
static void op_neg(MusVM *vm); static void op_b_not(MusVM *vm);
static void op_lshift(MusVM *vm); static void op_rshift(MusVM *vm);
static void op_l_not(MusVM *vm);
static void op_eq(MusVM *vm); static void op_neq(MusVM *vm);
static void op_ge(MusVM *vm); static void op_le(MusVM *vm);
static void op_gt(MusVM *vm); static void op_lt(MusVM *vm);
static void op_goto(MusVM *vm); static void op_brfalse(MusVM *vm);
static void op_brtrue(MusVM *vm); static void op_callvl(MusVM *vm);
static void op_callv(MusVM *vm); static void op_enter(MusVM *vm);
static void op_return(MusVM *vm); static void op_yield(MusVM *vm);
static void op_method(MusVM *vm);
static void op_tablexec(MusVM *vm);

static void init_handlers(void) {
    if (kHandlersInit) return;
    kHandlersInit = 1;
    /* 65 entries 0x00..0x40; unmapped slots stay NULL and trip the unknown
       opcode error path. Handlers added per E-task; the table below is the
       full set of opcodes referenced anywhere in the plan. */
    kHandlers[0x00] = op_nop;
    kHandlers[0x01] = op_push_imm8;
    kHandlers[0x02] = op_push_imm32;
    kHandlers[0x03] = op_push_global;
    kHandlers[0x04] = op_push_local;
    kHandlers[0x05] = op_push_global_addr;
    kHandlers[0x06] = op_push_local_addr;
    /* D-MUS-2 fixed: 0x07 pushstr, 0x0A pop_global_block, 0x0B pop_local_block are now
       implemented at the original's operand widths (see handler bodies). They appear in
       real Jointops bytecode; the reimpl compiler never emits them, so this only affects
       running real .mus data. */
    kHandlers[0x07] = op_push_str;
    kHandlers[0x08] = op_pop_global;
    kHandlers[0x09] = op_pop_local;
    kHandlers[0x0A] = op_pop_global_block;
    kHandlers[0x0B] = op_pop_local_block;
    kHandlers[0x0C] = op_push_self;
    kHandlers[0x0F] = op_empty;
    /* Slots 0x0D 0x0E nop; tolerate. */
    kHandlers[0x0D] = op_nop;
    kHandlers[0x0E] = op_nop;
    /* Arithmetic/comparison (E4) */
    kHandlers[0x10] = op_add;  kHandlers[0x11] = op_sub;
    kHandlers[0x12] = op_mul;  kHandlers[0x13] = op_div;
    kHandlers[0x14] = op_mod;
    kHandlers[0x15] = op_l_and; kHandlers[0x16] = op_l_or;
    kHandlers[0x17] = op_b_and; kHandlers[0x18] = op_b_or;
    kHandlers[0x19] = op_b_xor;
    kHandlers[0x1A] = op_neg;   kHandlers[0x1B] = op_b_not;
    kHandlers[0x1C] = op_lshift; kHandlers[0x1D] = op_rshift;
    kHandlers[0x1E] = op_l_not;
    /* 0x1F nop */
    kHandlers[0x1F] = op_nop;
    kHandlers[0x20] = op_eq;  kHandlers[0x21] = op_neq;
    kHandlers[0x22] = op_ge;  kHandlers[0x23] = op_le;
    kHandlers[0x24] = op_gt;  kHandlers[0x25] = op_lt;
    /* 0x26 0x27 nop */
    kHandlers[0x26] = op_nop;
    kHandlers[0x27] = op_nop;
    kHandlers[0x28] = op_inc_g; kHandlers[0x29] = op_dec_g;
    kHandlers[0x2A] = op_inc_l; kHandlers[0x2B] = op_dec_l;
    /* 0x2C..0x2F nop */
    kHandlers[0x2C] = op_nop; kHandlers[0x2D] = op_nop;
    kHandlers[0x2E] = op_nop; kHandlers[0x2F] = op_nop;
    /* Branches (E5) */
    kHandlers[0x30] = op_goto;
    kHandlers[0x31] = op_brfalse;
    kHandlers[0x32] = op_brtrue;
    /* Calls (E6/E7) */
    kHandlers[0x33] = op_callvl;
    kHandlers[0x34] = op_callv;
    kHandlers[0x35] = op_tablexec;
    /* 0x36 0x37 nop */
    kHandlers[0x36] = op_nop;
    kHandlers[0x37] = op_nop;
    kHandlers[0x38] = op_enter;
    kHandlers[0x39] = op_return;
    kHandlers[0x3A] = op_yield;
    kHandlers[0x3B] = op_setstate;
    /* 0x3C nop */
    kHandlers[0x3C] = op_nop;
    kHandlers[0x3D] = op_playw;
    kHandlers[0x3E] = op_play;
    kHandlers[0x3F] = op_done;
    kHandlers[0x40] = op_method;
}

/* Stub bodies for handlers fully implemented in later tasks. They no-op the
   operand bytes so the dispatch loop keeps moving until those tasks land. */

static void op_add    (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a + b); }
static void op_sub    (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a - b); }
static void op_mul    (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a * b); }
static void op_div    (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, b ? a / b : 0); }
/* Witnessed Jointops.exe!VmOp_Mod @ 0x672950 does div-then-mod with both writes
   landing on the same slot; the final stored value is plain `a % b`. */
static void op_mod    (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, b ? a % b : 0); }
/* NB: opcodes 0x15/0x16 are AudioVM_Op_BitwiseAnd/BitwiseOr @ 0x672960/0x672980
   and 0x17/0x18 are AudioVM_Op_And/Or @ 0x6729A0/0x6729B0 -- both PAIRS are
   plain bitwise & / | in the original (0x15/0x16 carry a dead boolean-ize that
   is computed but never stored). The "l_" prefix here is a historical misnomer;
   all four are bitwise, matching the binary. */
static void op_l_and  (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a & b); }
static void op_l_or   (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a | b); }
static void op_b_and  (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a & b); }
static void op_b_or   (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a | b); }
static void op_b_xor  (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a ^ b); }
static void op_neg    (MusVM *vm) { int32_t a = vm_pop(vm); vm_push(vm, -a); }
static void op_b_not  (MusVM *vm) { int32_t a = vm_pop(vm); vm_push(vm, ~a); }
static void op_lshift (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a << (b & 31)); }
static void op_rshift (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a >> (b & 31)); }
/* Witnessed: VmOp_LogicalNot @ 0x672A10. result = -1 when 0, else 0. */
static void op_l_not  (MusVM *vm) { int32_t a = vm_pop(vm); vm_push(vm, a == 0 ? -1 : 0); }
/* Witnessed: comparison handlers @ 0x672A20..0x672AC0 push -1 when true (signed). */
static void op_eq (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a == b ? -1 : 0); }
static void op_neq(MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a != b ? -1 : 0); }
static void op_ge (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a >= b ? -1 : 0); }
static void op_le (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a <= b ? -1 : 0); }
static void op_gt (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a >  b ? -1 : 0); }
static void op_lt (MusVM *vm) { int32_t b = vm_pop(vm); int32_t a = vm_pop(vm); vm_push(vm, a <  b ? -1 : 0); }

/* Branch / jump handlers (filled in E5). */
static void op_goto   (MusVM *vm) { vm->pc = (uint32_t)read_u32(vm); }
static void op_brfalse(MusVM *vm) { int32_t target = read_u32(vm);
                                    int32_t cond = vm_pop(vm);
                                    if (!cond) vm->pc = (uint32_t)target; }
static void op_brtrue (MusVM *vm) { int32_t target = read_u32(vm);
                                    int32_t cond = vm_pop(vm);
                                    if ( cond) vm->pc = (uint32_t)target; }

/* Call/return handlers (E6). */
static void op_callvl (MusVM *vm); /* impl below; needs intrinsic table */
static void op_callv  (MusVM *vm) {
    int32_t target = read_u32(vm);
    if (vm->csp < kCallStackCap) vm->call_stack[vm->csp++] = vm->pc;
    vm->pc = (uint32_t)target;
}
/* Witnessed: Jointops.exe!VmOp_TableExec @ 0x672BB0.
   Used for switch-statement codegen in MDEdit-authored scripts (menumus.bin
   uses several). Layout at vm->pc on entry (the dispatcher already consumed
   the 0x35 opcode byte):
     +0  size           (count of slots in the jump table)
     +1  stride_a       (loaded into ebx in Jointops.exe, then overwritten unread)
     +2  entry_stride   (bytes per slot; typically 5 for `goto target`)
     +3  skip_size      (TOTAL bytes in the encoded instruction including
                         the opcode + header + table_data; pc_at_opcode +
                         skip_size = next-instruction address)
     +4  size * entry_stride bytes of table data

   Pops idx from the data stack. If idx >= size or the chosen entry's
   first byte is 0, advances pc past the whole table by setting
   pc = pc_at_header + skip_size - 1. The trailing -1 mirrors the
   `dec esi` in Jointops.exe; under our dispatcher (which pre-incremented past
   the opcode), this lands pc on the next opcode byte. Otherwise the
   entry's first byte is dispatched as a recursive opcode with pc set
   just past it. If the recursive handler ended exactly at the end of
   its slot (typical for non-branching ops, never happens for `goto`),
   the rest of the table is skipped. The common case `goto LabelN`
   leaves pc at the branch target instead, and we leave it there. */
static void op_tablexec(MusVM *vm) {
    uint32_t pc_at_header = vm->pc;
    if (pc_at_header + 4 > vm->script->code_size) {
        snprintf(vm->last_error, sizeof(vm->last_error),
                 "tablexec header truncated at pc=0x%X", pc_at_header);
        vm->state = MUS_VM_ERROR;
        return;
    }
    uint8_t size         = vm->script->code[pc_at_header + 0];
    /* +1 stride_a is read by Jointops.exe into ebx but never used (overwritten
       before the next read). Mirror by ignoring. */
    uint8_t entry_stride = vm->script->code[pc_at_header + 2];
    uint8_t skip_size    = vm->script->code[pc_at_header + 3];

    int32_t idx = vm_pop(vm);

    if (size == 0 || (uint32_t)idx >= (uint32_t)size) {
        vm->pc = pc_at_header + (uint32_t)skip_size - 1;
        return;
    }

    uint32_t slot      = (uint32_t)idx % size;
    uint32_t entry_off = pc_at_header + 4 + slot * entry_stride;
    uint32_t entry_end = entry_off + entry_stride;
    if (entry_end > vm->script->code_size) {
        snprintf(vm->last_error, sizeof(vm->last_error),
                 "tablexec entry out of bounds at pc=0x%X", pc_at_header);
        vm->state = MUS_VM_ERROR;
        return;
    }
    uint8_t embedded_op = vm->script->code[entry_off];
    OpHandler eh = (embedded_op == 0) ? NULL : kHandlers[embedded_op];
    if (!eh) {
        vm->pc = pc_at_header + (uint32_t)skip_size - 1;
        return;
    }

    vm->pc = entry_off + 1;
    eh(vm);

    /* [orig: AudioVM_Op_TableExec @ 0x672C05] after the embedded `call eax`, the
       engine executes `cmp esi, entry_end`, which OVERWRITES the carry flag the
       embedded op may have set (setstate/play/done all STC). The tick therefore
       halts ONLY when the post-dispatch IP landed strictly BELOW entry_end (a
       backward jump, CF=1); a forward setstate/play/goto -- the common case --
       clears carry and CONTINUES in the same tick (its pc-jump and any sound
       still take effect). So we recompute halt_latch from the IP instead of
       letting the embedded op's halt stand. (A tablexec whose entry resolves to
       a forward target never ends the tick by itself.) */
    vm->halt_latch = (vm->pc < entry_end) ? 1 : 0;
    if (vm->pc == entry_end) {
        vm->pc = pc_at_header + (uint32_t)skip_size - 1;
    }
}

static void op_enter  (MusVM *vm) {
    /* [orig: AudioVM_Op_Enter @ 0x672C20]
         movzx ecx,[esi]; inc esi          ; N
         lea ecx,[ecx*4]; sub ebp,ecx      ; POP N dwords off the data stack
         mov ebx,LocalsBase; add ebx,[instance+0x3C]
         loop: copy N dwords from the popped stack region to locals[frame + i*4]
       The frame offset is instance[+0x3C] (== the chunk's string_section_size
       field, witnessed as 0x20 in jo_gamemus/menumus; MDEdit invariantly emits
       0x20). We plumb it via MusScript.locals_frame_offset (default 0x20) so the
       handler is faithful to any chunk rather than hardcoding 0x20.
       D-NEW-2: the original POPS the N args (`sub ebp,N*4`); we mirror with
       `vm->sp -= n`. (We guard sp >= n; the original does not bounds-check.) */
    int n = read_u8(vm);
    int dst_off = vm->script ? (int)vm->script->locals_frame_offset : 0x20;
    if (dst_off <= 0) dst_off = 0x20;
    if (n > 0 && vm->sp >= n) {
        for (int i = 0; i < n; ++i) {
            int32_t v = vm->data_stack[vm->sp - n + i];
            locals_write32(vm, dst_off + i * 4, v);
        }
        vm->sp -= n;
    }
}
static void op_return (MusVM *vm) {
    if (vm->csp > 0) vm->pc = vm->call_stack[--vm->csp];
    else vm->halt_latch = 1;
}
static void op_yield  (MusVM *vm) { vm->halt_latch = 1; }

/* Halt opcodes (E6/E10): set the latch so the dispatch loop exits. */
static void op_done(MusVM *vm) {
    /* Witnessed: VmOp_Done @ 0x672CD0 sets instance entry_section_index back
       to chunk->entry_section_index (0) and STC. We mirror by re-seeking pc
       to the entry section's code_offset and halting. */
    if (vm->script && vm->script->section_count > 0
        && vm->script->entry_section_index < vm->script->section_count) {
        const MusSection *entry = &vm->script->sections[vm->script->entry_section_index];
        vm->pc = entry->code_offset;
        size_t n = strlen(entry->name);
        if (n >= sizeof(vm->current_section_name)) n = sizeof(vm->current_section_name) - 1;
        memcpy(vm->current_section_name, entry->name, n);
        vm->current_section_name[n] = 0;
    }
    vm->halt_latch = 1;
}

static void op_setstate(MusVM *vm) {
    int sidx = read_u8(vm);
    if (vm->script && (uint32_t)sidx < vm->script->section_count) {
        const MusSection *sec = &vm->script->sections[sidx];
        vm->pc = sec->code_offset;
        size_t n = strlen(sec->name);
        if (n >= sizeof(vm->current_section_name)) n = sizeof(vm->current_section_name) - 1;
        memcpy(vm->current_section_name, sec->name, n);
        vm->current_section_name[n] = 0;
        if (vm->hooks.on_section_entered) {
            vm->hooks.on_section_entered(vm->hooks.user, vm->current_section_name);
        }
    }
    vm->halt_latch = 1;
}

/* [orig: AudioVM_Op_Play @ 0x672CB0 (1B index), AudioVM_Op_PlayWait @ 0x672C90
   (2B index)] D-NEW-3: BOTH handlers call the SAME AudioVM_StartSound(idx) and
   STC (halt) identically -- the original makes NO play-vs-wait behavioral
   distinction; the only real difference is the operand width (u8 vs u16, so
   0x3D allows sound indices > 255). The `wait` arg we pass to on_play_sound is
   a reimpl convenience, not a witnessed semantic; embedders should treat both as
   "start sound idx". */
static void op_play (MusVM *vm) {
    int idx = read_u8(vm);
    if (vm->hooks.on_play_sound) {
        vm->hooks.on_play_sound(vm->hooks.user, (uint32_t)idx, /*wait=*/0);
    }
    vm->halt_latch = 1;
}
static void op_playw(MusVM *vm) {
    int idx = read_u16(vm);
    if (vm->hooks.on_play_sound) {
        vm->hooks.on_play_sound(vm->hooks.user, (uint32_t)idx, /*wait=*/1);
    }
    vm->halt_latch = 1;
}

/* method/callvl: implemented in E7/E8/E9. */
static void op_method (MusVM *vm); /* impl below */
static void op_callvl (MusVM *vm) {
    /* Jointops.exe!AudioVM_Op_CallVL @ 0x672B70 is LIVE here: it dispatches through
       the resolved-name table dword_3245958[byte] (populated by AudioVM_LoadScriptFile's
       name-resolution pass) and pushes the handler result. In the Jointops.exe build that
       table was uninitialised BSS, so the opcode was dead. DIVERGENCE: we mirror the
       Jointops.exe dead-stub (push 0). MDEdit music scripts use opcode 0x40 `method` for the
       built-in intrinsics (GEcho/GSV/...), not callvl, so this is inert for known
       fixtures. See docs/audio/mus-sbf-re.md (D-MUS-7). */
    (void)read_u8(vm);
    vm_push(vm, 0);
}

/* Intrinsic table forward decl (E7-E9). */
typedef void (*Intrinsic)(MusVM *);
static Intrinsic kIntrinsics[MUS_INTRINSIC_NAMES] = {0};
static int       kIntrinsicsInit = 0;
static void init_intrinsics(void);

static void op_method(MusVM *vm) {
    init_intrinsics();
    int idx = read_u8(vm);
    Intrinsic fn = (idx >= 0 && idx < MUS_INTRINSIC_NAMES) ? kIntrinsics[idx] : NULL;
    if (fn) {
        fn(vm);
    } else {
        /* Witnessed: NULL handler skip + push 0. */
        vm_push(vm, 0);
    }
}

/* ---- Intrinsic methods (E7-E9) ---------------------------------------- */

/* Witnessed: Jointops.exe!Intrinsic_GEcho @ 0x6720C0.
   Pops 1 (TOS) and pushes 0. Fires on_echo with the popped int32. */
static void intrinsic_gecho(MusVM *vm) {
    int32_t arg = vm_pop(vm);
    if (vm->hooks.on_echo) vm->hooks.on_echo(vm->hooks.user, arg);
    vm_push(vm, 0);
}

static inline uint32_t mus_rol32(uint32_t v, int s) {
    return (v << s) | (v >> (32 - s));
}

/* Jointops.exe!AudioVM_Intrinsic_GGRnd @ 0x672320 (byte-exact port).
   Pops 2 (NOS=lo, TOS=hi). The process-global seed AudioVM_GGRndSeed @ 0x84F210 is
   statically initialised to 0xBABEFACE in the binary and updated in place:
       seed = rol32(seed + rol32(seed, 11), 2)
   Then (asm `and eax,0FFFFh; idiv ecx`): rnd = (seed & 0xFFFF) % span, span = hi-lo+1
   (rnd forced to 0 when span == 0); result = lo + rnd. We mirror the process-global
   seed with a file-static so the sequence matches a fresh game process. */
static void intrinsic_ggrnd(MusVM *vm) {
    int32_t hi = vm_pop(vm);
    int32_t lo = vm_pop(vm);
    static uint32_t s_ggrnd_seed = 0xBABEFACEu;   /* initial value of dword_84F210 */
    s_ggrnd_seed = mus_rol32(s_ggrnd_seed + mus_rol32(s_ggrnd_seed, 11), 2);
    int32_t span = hi - lo + 1;
    int32_t rnd  = 0;
    if (span != 0) rnd = (int32_t)(s_ggrnd_seed & 0xFFFFu) % span;
    vm_push(vm, lo + rnd);
}

/* Helper: decode an address tag value back to (is_local, byte_offset).
   Returns -1 byte_offset if not a tagged address. */
static int decode_tag_addr(int32_t tagged, int *out_local) {
    if (!(tagged & kAddrTagBit)) return -1;
    *out_local = (tagged & kAddrLocalBit) ? 1 : 0;
    return (int)(tagged & 0x00FFFFFF);
}

/* Read int32 from a tagged address (globals or locals). */
static int32_t read_tagged(MusVM *vm, int32_t tagged) {
    int local = 0;
    int off = decode_tag_addr(tagged, &local);
    if (off < 0) return tagged;     /* not a tag -> treat as raw value */
    return local ? locals_read32(vm, off) : globals_read32(vm, off);
}
static void write_tagged(MusVM *vm, int32_t tagged, int32_t v) {
    int local = 0;
    int off = decode_tag_addr(tagged, &local);
    if (off < 0) return;
    if (local) locals_write32(vm, off, v);
    else { globals_write32(vm, off, v); notify_var_changed(vm, off, v); }
}

/* Read a dword from a tagged source region at a sub-offset (for the block-copy ops). */
static int32_t read_tagged_at(MusVM *vm, int32_t tagged, int sub_off) {
    int local = 0;
    int off = decode_tag_addr(tagged, &local);
    if (off < 0) return 0;            /* untagged source: no region to copy from */
    return local ? locals_read32(vm, off + sub_off) : globals_read32(vm, off + sub_off);
}

/* Jointops.exe!AudioVM_Op_PushStr @ 0x672830: reads a u8 index (IP advances 1) and
   pushes context[+0x30][index] from the script's string/aux table. The single-context
   reimpl does not model that table, so we push 0 -- the key fix is that the IP advances
   correctly (1 byte) so the rest of a real script keeps decoding. */
static void op_push_str(MusVM *vm) {
    (void)read_u8(vm);
    vm_push(vm, 0);
}

/* Jointops.exe!AudioVM_Op_PopGlobalBlock @ 0x672890 (0x0A): pops a source address
   (TOS, from push_*_addr), reads two u8 operands (dst byte-offset, byte count), and
   copies `count` bytes (dword-granular, matching the engine's 4-byte chunk loop) from
   the source region into the globals area at dst_off. Sets the globals-dirty signal. */
static void op_pop_global_block(MusVM *vm) {
    int dst_off = read_u8(vm);
    int count   = read_u8(vm);
    int32_t src = vm_pop(vm);
    for (int i = 0; i + 4 <= count; i += 4) {
        int32_t v = read_tagged_at(vm, src, i);
        globals_write32(vm, dst_off + i, v);
        notify_var_changed(vm, dst_off + i, v);
    }
}

/* Jointops.exe!AudioVM_Op_PopLocalBlock @ 0x6728C0 (0x0B): same as 0x0A but the
   destination is the locals area and no dirty signal is raised. */
static void op_pop_local_block(MusVM *vm) {
    int dst_off = read_u8(vm);
    int count   = read_u8(vm);
    int32_t src = vm_pop(vm);
    for (int i = 0; i + 4 <= count; i += 4) {
        locals_write32(vm, dst_off + i, read_tagged_at(vm, src, i));
    }
}

/* Witnessed: Jointops.exe!Intrinsic_GSV @ 0x6720E0.
   Sets master and right-channel volume to clamp(TOS,0,255)<<16 (16.16 fixed
   point). Hooks on_volume_changed. Returns the original arg so net stack
   delta is 0 (call site sees the value still on stack). */
static void intrinsic_gsv(MusVM *vm) {
    int32_t v = vm_pop(vm);
    int32_t clipped = v;
    if (clipped < 0)   clipped = 0;
    if (clipped > 255) clipped = 255;
    int32_t fixed = clipped << 16;
    if (vm->hooks.on_volume_changed) {
        vm->hooks.on_volume_changed(vm->hooks.user, fixed, fixed);
    }
    vm_push(vm, fixed);
}

/* Witnessed: Jointops.exe!Intrinsic_GSDV @ 0x672120.
   Sets right-channel only. Reuse on_volume_changed but pass current(left)
   unchanged; we don't track the previous left value, so pass `fixed` for
   both channels and let the embedder disambiguate. */
static void intrinsic_gsdv(MusVM *vm) {
    int32_t v = vm_pop(vm);
    int32_t clipped = v;
    if (clipped < 0)   clipped = 0;
    if (clipped > 255) clipped = 255;
    int32_t fixed = clipped << 16;
    /* GSDV only changes the right channel. We pass left=0 to indicate
       "left unchanged" -- not perfectly faithful but the only practical
       choice without a per-channel state mirror. */
    if (vm->hooks.on_volume_changed) {
        vm->hooks.on_volume_changed(vm->hooks.user, 0, fixed);
    }
    vm_push(vm, fixed);
}

/* [orig: AudioVM_Intrinsic_GFB @ 0x672150] `mov dword_31C37EC, 0; retn` -- takes
   NO args (does not deref the &TOS pointer), returns void. Side effect: clears
   the fade-base global dword_31C37EC. We don't model the fade subsystem, so this
   is a no-op stub; we push 0 to satisfy the method-call convention (which always
   pushes the handler's return slot). */
static void intrinsic_gfb(MusVM *vm) {
    vm_push(vm, 0);
}

/* Witnessed: Jointops.exe!Intrinsic_FSet @ 0x672360.
   Pops 2 (NOS=mask, TOS=&var). Sets *var |= mask, returns new value.
   Operands typically come from `push <mask>; push_ga <var>; method FSet`.

   Phase E uses the bound-flag bitmap as well: when the operand pushed via
   push_ga refers to the conventional flag-byte global (e.g. byte offset 64),
   the script gets per-bit semantics from the bitmap. For tagged-address
   inputs we route through globals memory directly. */
static void intrinsic_fset(MusVM *vm) {
    int32_t var_addr = vm_pop(vm);
    int32_t mask     = vm_pop(vm);
    int32_t cur = read_tagged(vm, var_addr);
    int32_t result = cur | mask;
    write_tagged(vm, var_addr, result);
    vm_push(vm, result);
}
static void intrinsic_fclear(MusVM *vm) {
    int32_t var_addr = vm_pop(vm);
    int32_t mask     = vm_pop(vm);
    int32_t cur = read_tagged(vm, var_addr);
    int32_t result = cur & ~mask;
    write_tagged(vm, var_addr, result);
    vm_push(vm, result);
}
static void intrinsic_fisset(MusVM *vm) {
    int32_t var_addr = vm_pop(vm);
    int32_t mask     = vm_pop(vm);
    int32_t cur = read_tagged(vm, var_addr);
    /* Witness: returns -1 if (*TOS & NOS) == NOS, else 0. */
    vm_push(vm, ((cur & mask) == mask) ? -1 : 0);
}

/* Jointops.exe!AudioVM_Intrinsic_FIsClear @ 0x6723C0: pops 2 (NOS=mask, TOS=&var),
   returns -1 when NONE of the mask bits are set (`(mask & *var) == 0`), else 0.
   This is a real bound handler in Jointops (idx 8), the inverse of FIsSet. */
static void intrinsic_fisclear(MusVM *vm) {
    int32_t var_addr = vm_pop(vm);
    int32_t mask     = vm_pop(vm);
    int32_t cur = read_tagged(vm, var_addr);
    vm_push(vm, ((cur & mask) == 0) ? -1 : 0);
}

/* NULL-handler fallback -> push 0. Jointops.exe's intrinsic name table (@ 0x84F0C8)
   has 9 entries (GEcho,GGRnd,GSV,GSDV,GFB,FSet,FClear,FIsSet,FIsClear) -- all bound.
   TStart/TStop from the canonical MDEdit set do not exist in this build, so any index
   >= 9 resolves here and no-ops. */
static void intrinsic_unbound(MusVM *vm) {
    vm_push(vm, 0);
}

static void init_intrinsics(void) {
    if (kIntrinsicsInit) return;
    kIntrinsicsInit = 1;
    /* Jointops.exe intrinsic name table (@ 0x84F0C8, 9 records x 36B): GEcho,
       GGRnd, GSV, GSDV, GFB, FSet, FClear, FIsSet, FIsClear -- all 9 bound (matches
       Jointops). TStart/TStop from the canonical MDEdit set are NOT present in this
       build; indices 9/10 resolve to the no-op fallback. */
    kIntrinsics[0]  = intrinsic_gecho;
    kIntrinsics[1]  = intrinsic_ggrnd;
    kIntrinsics[2]  = intrinsic_gsv;
    kIntrinsics[3]  = intrinsic_gsdv;
    kIntrinsics[4]  = intrinsic_gfb;
    kIntrinsics[5]  = intrinsic_fset;
    kIntrinsics[6]  = intrinsic_fclear;
    kIntrinsics[7]  = intrinsic_fisset;
    kIntrinsics[8]  = intrinsic_fisclear;  /* bound in Jointops (@ 0x6723C0) */
    kIntrinsics[9]  = intrinsic_unbound;   /* TStart: absent in this build */
    kIntrinsics[10] = intrinsic_unbound;   /* TStop:  absent in this build */
}

/* Bytecode-name compare bounded to MUS_SECTION_NAME_SIZE; treats either NUL
   as end of string (matches mus_find_section). */
static int section_name_eq_local(const char *a, const char *b) {
    for (size_t i = 0; i < MUS_SECTION_NAME_SIZE; ++i) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

/* Notify on_section_entered if the current pc lands on a section's
   code_offset and that section differs from the current_section_name.
   Setstate (0x3B) and jump_to_section already fire this hook directly; the
   dispatch loop calls this after each opcode so flow that lands on a section
   entry via goto/callv also notifies. */
static void check_section_transition(MusVM *vm) {
    if (!vm->script || !vm->script->sections) return;
    for (uint32_t i = 0; i < vm->script->section_count; ++i) {
        if (vm->script->sections[i].code_offset == vm->pc) {
            const char *name = vm->script->sections[i].name;
            if (!section_name_eq_local(vm->current_section_name, name)) {
                size_t n = strlen(name);
                if (n >= sizeof(vm->current_section_name)) {
                    n = sizeof(vm->current_section_name) - 1;
                }
                memcpy(vm->current_section_name, name, n);
                vm->current_section_name[n] = 0;
                if (vm->hooks.on_section_entered) {
                    vm->hooks.on_section_entered(vm->hooks.user,
                                                 vm->current_section_name);
                }
            }
            return;
        }
    }
}

/* Tick: witnessed dispatch loop @ Jointops.exe!AudioVM_DispatchLoop @ 0x00672720.
   Budget = 32 instructions (dword_3246B24). Halt latches break early. */
extern "C" int mus_vm_tick(MusVM *vm, uint32_t dt_ms) {
    init_handlers();
    if (!vm) return 0;
    if (vm->state != MUS_VM_RUNNING) return 0;
    if (!vm->script || !vm->script->code) return 0;

    vm->halt_latch = 0;
    /* [orig: AudioVM_DispatchLoop @ 0x672720] the instruction budget
       (dword_3246B24 = 32) is a SOFT floor, not a hard cap. The loop tail is
       `dec budget; jg loop; cmp ebp,stack_base; jnz loop`, i.e.
       `while (--budget > 0 || ebp != stack_base)`: once the 32 budget is spent
       it keeps executing until the data stack drains back to base (sp == 0) or
       a handler sets the carry/halt latch. D-NEW-1: we mirror that by breaking
       only when the budget is spent AND the data stack is empty. (pc-range and
       unknown-opcode guards still bound malformed scripts; vm_push overflow ->
       ERROR.) */
    int budget = kTickBudget;
    /* Safety ceiling on the soft-drain extension: a well-formed statement is far
       under this, but a malformed `goto`-loop that never drains/halts would spin
       forever (the original hangs too); cap it so the embedder never wedges. */
    int extension = kTickBudget * 64;
    for (;;) {
        if (vm->state != MUS_VM_RUNNING) break;
        if (vm->pc >= vm->script->code_size) {
            vm->state = MUS_VM_HALTED;
            break;
        }
        uint8_t op = vm->script->code[vm->pc++];
        OpHandler h = kHandlers[op];
        if (!h) {
            snprintf(vm->last_error, sizeof(vm->last_error),
                     "unknown opcode 0x%02X at pc=0x%X", op, vm->pc - 1);
            vm->state = MUS_VM_ERROR;
            break;
        }
        h(vm);
        check_section_transition(vm);
        if (vm->halt_latch) break;
        if (--budget <= 0 && vm->sp <= 0) break;   /* budget spent + stack drained */
        if (--extension <= 0) break;               /* safety: never spin forever */
    }
    return (int)dt_ms;
}

/* ---- Globals accessors ------------------------------------------------ */

extern "C" int32_t mus_vm_get_var(const MusVM *vm, uint8_t var_index) {
    if (!vm) return 0;
    int byte_off = (int)var_index * 4;
    return globals_read32(vm, byte_off);
}

extern "C" void mus_vm_set_var(MusVM *vm, uint8_t var_index, int32_t value) {
    if (!vm) return;
    int byte_off = (int)var_index * 4;
    if (byte_off + 4 > kGlobalsBytes) return;
    globals_write32(vm, byte_off, value);
    notify_var_changed(vm, byte_off, value);
}

/* ---- E11: jump_to_section + section tracking --------------------------- */

extern "C" int mus_vm_jump_to_section(MusVM *vm, const char *name) {
    if (!vm || !vm->script || !name) return -1;
    for (uint32_t i = 0; i < vm->script->section_count; ++i) {
        if (section_name_eq_local(vm->script->sections[i].name, name)) {
            const MusSection *sec = &vm->script->sections[i];
            vm->pc = sec->code_offset;
            size_t n = strlen(sec->name);
            if (n >= sizeof(vm->current_section_name)) {
                n = sizeof(vm->current_section_name) - 1;
            }
            memcpy(vm->current_section_name, sec->name, n);
            vm->current_section_name[n] = 0;
            if (vm->hooks.on_section_entered) {
                vm->hooks.on_section_entered(vm->hooks.user, vm->current_section_name);
            }
            return 0;
        }
    }
    return -2;
}
