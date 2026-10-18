/* MUS bytecode VM (interpreter).

   Witnessed dispatch loop: Jointops.exe!AudioVM_DispatchLoop @ 0x00672720.
   Witnessed opcode table: Jointops.exe!g_AudioVMOpcodeDispatchTable @ 0x0084F220
   (65 entries, 0x00..0x40). Stack element size = 4 bytes (int32).
   Two stacks: data (EBP-tracked, 256 entries here) + call (EDI-tracked,
   64 frames here). 32-instruction budget per dispatch loop call.

   Halt convention (witnessed): the original sets the x86 carry flag (STC)
   from inside `play`/`playw`/`done`/`setstate` handlers and from
   `VmOp_Empty` (0x0F). The dispatch loop also halts when its 32-instruction
   budget hits zero. We model that with an explicit `halt` flag set by the
   handlers and inspected by the dispatch loop. */

#include <formats/mus/mus.h>

#include <base/io/bits.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>
#include <vector>

namespace opennova::mus {

namespace {

/* Static stack/area sizes. The original VM has 4 KB+ for both stacks (stack
   element 4 B, ~1024 frames each). 256 / 64 is more than enough headroom for
   any sane authored MUS script and keeps the per-VM allocation small. */
constexpr int kDataStackCap = 256;
constexpr int kCallStackCap = 64;
constexpr int kTickBudget   = 32;   /* witnessed: dword_3246B24 = 0x20 */

/* The instance's block and the strings' copy the VM holds: at most what a tagged
   address's 28-bit offset reaches (D-MUS-22). */
constexpr size_t kRegionCap = 0x10000000;

/* The block the game's allocator gives the instance: the size, 1 at least, rounded up to 64
   [orig: FastMem_Alloc @ 0x7697BD..0x7697D7, from AudioMem_AllocWithLabel @ 0x759DB7]; its
   bytes past the size are the instance's own, which the game leaves as the heap held them and
   the VM zeroes (D-MUS-22). */
uint64_t instance_block_bytes(uint64_t size) {
    if (size < 1) size = 1;
    return (size + 63) & ~uint64_t(63);
}

}   /* anonymous namespace */

/* The instance's data: one block of the script's globals_size then its locals_size,
   the globals at its start and the locals after them [orig: ScriptInstance_Init @
   0x672F78..0x672FA3: AudioMem_AllocWithLabel of the two sizes, memset of them,
   globals = the block, locals = the block + globals_size], as the allocator rounds it
   (instance_block_bytes). Byte-addressable (inc_g/dec_g step one byte); a global past
   its area reads the locals after it, and past them the block's rounding, as the
   game's does. */
struct MusGlobals {
    std::vector<uint8_t> block;
    uint32_t globals_size = 0;
};

/* A method's handler (an intrinsic of the game's registered table). */
typedef void (*Intrinsic)(MusVM *);

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

    /* The instance's block, globals then locals (MusGlobals). */
    std::shared_ptr<MusGlobals> globals = std::make_shared<MusGlobals>();

    /* The script's strings as the chunk lays them out (MDEdit's layout): string_count words, each
       the tagged address of its string (the relocated pointers [orig: AudioVM_FixupPointers @
       0x6724C5..0x6724D8]), then the strings, each to its NUL, then the zeros to the debug section's
       place at the next 16 of the chunk (MDEdit.exe sub_40DF30 @ 0x40E23C..0x40E24B). `pushstr`
       pushes the word at its index [orig: AudioVM_Op_PushStr @ 0x672836]. */
    std::vector<uint8_t> strings;

    /* Flag bitmap (FSet/FClear/FIsSet). 64 flags. */
    uint64_t flags;

    /* The script's method table resolved: the handler of each of its names, NULL where the name resolves to none
       (the resolve table the loader fills [orig: AudioVM_LoadScriptFile @ 0x672DE4..0x672DE9]). */
    std::vector<Intrinsic> methods;

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

MusVM *mus_vm_create(void) {
    try {
        return new MusVM{};
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

void mus_vm_destroy(MusVM *vm) {
    delete vm;
}

void mus_vm_set_hooks(MusVM *vm, const MusVMHooks *hooks) {
    if (!vm) return;
    if (hooks) vm->hooks = *hooks;
    else memset(&vm->hooks, 0, sizeof(vm->hooks));
}

MusVMState mus_vm_state(const MusVM *vm) {
    return vm ? vm->state : MUS_VM_STOPPED;
}

const char *mus_vm_last_error(const MusVM *vm) {
    if (!vm) return "ok";
    return vm->last_error[0] ? vm->last_error : "ok";
}

const char *mus_vm_current_section(const MusVM *vm) {
    return vm ? vm->current_section_name : "";
}

int mus_vm_call_depth(const MusVM *vm) {
    return vm ? vm->csp : 0;
}

uint32_t mus_vm_pc(const MusVM *vm) {
    return vm ? vm->pc : 0;
}

/* ---- Addresses --------------------------------------------------------- */

/* A pushed address: the game pushes a pointer into the instance's block or the script's chunk
   (`push_ga`, `push_la`, `pushstr`); the VM, in its own address space, pushes the region and the
   offset in it, tagged, and the handlers that take an address read and write through it. */
static constexpr int32_t kAddrTagBit    = (int32_t)0x40000000;   /* large flag */
static constexpr int32_t kAddrLocalBit  = (int32_t)0x20000000;
static constexpr int32_t kAddrStringBit = (int32_t)0x10000000;
static constexpr int32_t kStringAddrTag = kAddrTagBit | kAddrStringBit;
static constexpr int32_t kAddrOffsetMask = (int32_t)0x0FFFFFFF;

enum class Region { Globals, Locals, Strings };

/* `n` bytes at `off` in a region, or NULL past it: past the instance's block (or the strings' copy)
   the game reads and writes the heap after its allocation, which the VM does not (D-MUS-22). */
static uint8_t *region_at(MusVM *vm, Region region, int64_t off, size_t n);

/* ---- E2: load + start/stop/pause/resume -------------------------------- */

static Intrinsic resolve_method(const char *name);

/* The current section's name: the section's label (mus_section_label), "Section_N" where the file names it none. */
static void set_current_section(MusVM *vm, uint32_t index) {
    const std::string label = mus_section_label(vm->script, index);
    const size_t n = label.size() < sizeof(vm->current_section_name) ? label.size()
                                                                     : sizeof(vm->current_section_name) - 1;
    memcpy(vm->current_section_name, label.data(), n);
    vm->current_section_name[n] = 0;
}

int mus_vm_load_script(MusVM *vm, const MusScript *s) {
    if (!vm || !s) return -1;
    // A fresh context owns fresh storage. Existing compiled operands retain the
    // old context rather than following a replacement or borrowing freed memory.
    /* The instance's block and the strings' copy, made before anything of the VM changes. */
    const uint64_t block_size = instance_block_bytes((uint64_t)s->globals_size + s->locals_size);
    uint64_t image = 4ull * s->string_count;
    for (uint32_t i = 0; i < s->string_count && image <= kRegionCap; ++i)
        image += strlen(s->strings && s->strings[i] ? s->strings[i] : "") + 1;
    /* The table sits where the writer puts it, at the code's end past the chunk's 0x68 bytes of header
       and zeros and its section table (mus_encode_file), so the zeros run to that place's next 16. */
    const uint64_t strings_end = 0x68ull + 4ull * s->section_count + s->code_size + image;
    image += (16 - strings_end % 16) % 16;
    if (block_size > kRegionCap || image > kRegionCap) return -2;
    std::shared_ptr<MusGlobals> globals;
    std::vector<uint8_t> strings;
    try {
        globals = std::make_shared<MusGlobals>();
        globals->globals_size = s->globals_size;
        globals->block.assign((size_t)block_size, 0);
        strings.assign((size_t)image, 0);
    } catch (const std::bad_alloc &) {
        return -1;
    }
    size_t at = 4ull * s->string_count;
    for (uint32_t i = 0; i < s->string_count; ++i) {
        const char *text = s->strings && s->strings[i] ? s->strings[i] : "";
        const int32_t address = kStringAddrTag | (int32_t)at;
        memcpy(strings.data() + 4ull * i, &address, 4);
        memcpy(strings.data() + at, text, strlen(text) + 1);
        at += strlen(text) + 1;
    }
    vm->globals = std::move(globals);
    vm->strings = std::move(strings);
    vm->script = s;
    vm->state  = MUS_VM_STOPPED;
    vm->sp     = 0;
    vm->csp    = 0;
    vm->flags  = 0;
    vm->halt_latch = 0;
    memset(vm->data_stack, 0, sizeof(vm->data_stack));
    memset(vm->call_stack, 0, sizeof(vm->call_stack));
    vm->last_error[0] = 0;
    vm->current_section_name[0] = 0;
    /* Each method name resolved by name, as the loader does [orig: AudioVM_LoadScriptFile @ 0x672DE0..0x672E01]. */
    vm->methods.assign(s->intrinsic_names ? s->intrinsic_count : 0, NULL);
    for (size_t i = 0; i < vm->methods.size(); ++i) vm->methods[i] = resolve_method(s->intrinsic_names[i]);

    /* Witnessed: Jointops.exe!ScriptInstance_Init @ 0x00672EF0
       initial_pc = section_table[entry_section_index] (@0x672FB5..0x672FC3). */
    vm->pc = 0;
    if (s->section_count > 0 && s->entry_section_index < s->section_count) {
        vm->pc = s->sections[s->entry_section_index].code_offset;
        set_current_section(vm, s->entry_section_index);
    }
    return 0;
}

void mus_vm_start(MusVM *vm) {
    if (!vm || !vm->script) return;
    vm->state = MUS_VM_RUNNING;
}

void mus_vm_stop(MusVM *vm) {
    if (!vm) return;
    vm->state = MUS_VM_STOPPED;
}

/* The embedder dropping its script: the VM keeps only a borrowed pointer to
   the MusScript (mus_vm_load_script), so a context teardown must clear it
   before the owner frees the program, or a later signal/advance reads freed
   memory. */
void mus_vm_unload_script(MusVM *vm) {
    if (!vm) return;
    vm->script = NULL;
    vm->state  = MUS_VM_STOPPED;
    vm->pc = 0;
    vm->sp = 0;
    vm->csp = 0;
    vm->halt_latch = 0;
    vm->current_section_name[0] = 0;
    vm->methods.clear();
    vm->strings.clear();
    /* The block freed, as the game's context teardown zeroes its globals pointer [orig:
       AudioVM_FreeSoundBuffer @ 0x672E2B]: a variable's write is dropped and its read is 0
       [orig: the NULL tests @ 0x672645, @ 0x672665]. A compiled mission script keeps the
       block it took (mus_vm_globals). */
    vm->globals.reset();
}

void mus_vm_pause(MusVM *vm) {
    if (!vm) return;
    if (vm->state == MUS_VM_RUNNING) vm->state = MUS_VM_PAUSED;
}

void mus_vm_resume(MusVM *vm) {
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

static uint8_t *region_at(MusVM *vm, Region region, int64_t off, size_t n) {
    if (off < 0 || (region != Region::Strings && !vm->globals)) return NULL;
    std::vector<uint8_t> &bytes = region == Region::Strings ? vm->strings : vm->globals->block;
    const uint64_t at = (uint64_t)off + (region == Region::Locals ? vm->globals->globals_size : 0);
    if (at + n > bytes.size()) return NULL;
    return bytes.data() + at;
}

/* A word, byte by byte: one at a region's end takes the bytes in it, 0 for those past it (D-MUS-22). */
static int32_t region_read32(const MusVM *vm, Region region, int64_t off) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k)
        if (const uint8_t *at = region_at(const_cast<MusVM *>(vm), region, off + k, 1)) bytes[k] = *at;
    int32_t v;
    memcpy(&v, bytes, 4);
    return v;
}

static void region_write32(MusVM *vm, Region region, int64_t off, int32_t v) {
    uint8_t bytes[4];
    memcpy(bytes, &v, 4);
    for (int k = 0; k < 4; ++k)
        if (uint8_t *at = region_at(vm, region, off + k, 1)) *at = bytes[k];
}

/* A global at a byte offset of the globals area, a local at one of the locals area: both in the
   instance's one block (MusGlobals), little-endian, byte-addressable (inc_g/dec_g step a byte). */
static int32_t globals_read32(const MusVM *vm, int byte_off) { return region_read32(vm, Region::Globals, byte_off); }
static void globals_write32(MusVM *vm, int byte_off, int32_t v) { region_write32(vm, Region::Globals, byte_off, v); }
static int32_t locals_read32(const MusVM *vm, int byte_off) { return region_read32(vm, Region::Locals, byte_off); }
static void locals_write32(MusVM *vm, int byte_off, int32_t v) { region_write32(vm, Region::Locals, byte_off, v); }

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

/* [orig: AudioVM_Op_IncGlobal @ 0x672AE0, AudioVM_Op_DecGlobal @ 0x672AF0] inc/dec a single
   BYTE at globals[off] (NOT a full int32). D-MUS-5: unlike pop_g, the original
   does NOT raise the globals-dirty signal (dword_3246B28) and fires no embedder
   notification here, so we deliberately omit notify_var_changed to match. */
static void op_inc_g(MusVM *vm) {
    if (uint8_t *at = region_at(vm, Region::Globals, read_u8(vm), 1)) ++*at;
}
static void op_dec_g(MusVM *vm) {
    if (uint8_t *at = region_at(vm, Region::Globals, read_u8(vm), 1)) --*at;
}
static void op_inc_l(MusVM *vm) {
    if (uint8_t *at = region_at(vm, Region::Locals, read_u8(vm), 1)) ++*at;
}
static void op_dec_l(MusVM *vm) {
    if (uint8_t *at = region_at(vm, Region::Locals, read_u8(vm), 1)) --*at;
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
       implemented at the original's operand widths (see handler bodies). The compiler
       writes pushstr for a string parameter (D-MUS-20) and never the block pops. */
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
       The frame offset is instance[+0x3C] (== the chunk's frame_base field,
       0x20 in every MDEdit chunk), added as the chunk holds it (0 is a
       frame at the locals' start): MusScript.locals_frame_offset.
       D-MUS-6 (a.k.a. D-NEW-2): the original POPS the N args (`sub ebp,N*4`); we mirror with
       `vm->sp -= n`. (We guard sp >= n; the original does not bounds-check.) */
    int n = read_u8(vm);
    const int dst_off = vm->script ? (int)vm->script->locals_frame_offset : 0;
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
        vm->pc = vm->script->sections[vm->script->entry_section_index].code_offset;
        set_current_section(vm, vm->script->entry_section_index);
    }
    vm->halt_latch = 1;
}

static void op_setstate(MusVM *vm) {
    int sidx = read_u8(vm);
    if (vm->script && (uint32_t)sidx < vm->script->section_count) {
        vm->pc = vm->script->sections[sidx].code_offset;
        set_current_section(vm, (uint32_t)sidx);
        if (vm->hooks.on_section_entered) {
            vm->hooks.on_section_entered(vm->hooks.user, vm->current_section_name);
        }
    }
    vm->halt_latch = 1;
}

/* [orig: AudioVM_Op_Play @ 0x672CB0 (1B index), AudioVM_Op_PlayWait @ 0x672C90
   (2B index)] D-MUS-11 (a.k.a. D-NEW-3): BOTH handlers call the SAME AudioVM_StartSound(idx) and
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

/* Op 0x40 method: call the handler the loader resolved for the script's idx-th method name (instance[+0x14],
   the resolve table), push the result, continue; a name resolved to none pushes 0 [orig: AudioVM_Op_Method @
   0x672CF0..0x672D10]. A number past the script's table reads past the resolve table, a word of the names (or,
   with no table, of an address the loader never set) the game calls as a handler [orig: @ 0x672CFD mov eax,
   [ecx+eax*4]; @ 0x672D08 call eax]: refused here, the VM stopping in error. */
static void op_method(MusVM *vm) {
    const int idx = read_u8(vm);
    if ((size_t)idx >= vm->methods.size()) {
        snprintf(vm->last_error, sizeof(vm->last_error), "method %d past the script's %zu method names", idx,
                 vm->methods.size());
        vm->state = MUS_VM_ERROR;
        vm->halt_latch = 1;
        return;
    }
    const Intrinsic fn = vm->methods[(size_t)idx];
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

/* Jointops.exe!AudioVM_Intrinsic_GGRnd @ 0x672320 (byte-exact port).
   Pops 2 (NOS=lo, TOS=hi). The process-global seed g_AudioVMGGRndSeed @ 0x84F210 is
   statically initialised to 0xBABEFACE in the binary and updated in place:
       seed = rol32(seed + rol32(seed, 11), 2)
   Then (asm `and eax,0FFFFh; idiv ecx`): rnd = (seed & 0xFFFF) % span, span = hi-lo+1
   (rnd forced to 0 when span == 0); result = lo + rnd. We mirror the process-global
   seed with a file-static so the sequence matches a fresh game process. */
static void intrinsic_ggrnd(MusVM *vm) {
    int32_t hi = vm_pop(vm);
    int32_t lo = vm_pop(vm);
    static uint32_t s_ggrnd_seed = 0xBABEFACEu;   /* initial value of dword_84F210 */
    s_ggrnd_seed = opennova::io::rotl32(s_ggrnd_seed + opennova::io::rotl32(s_ggrnd_seed, 11), 2);
    int32_t span = hi - lo + 1;
    int32_t rnd  = 0;
    if (span != 0) rnd = (int32_t)(s_ggrnd_seed & 0xFFFFu) % span;
    vm_push(vm, lo + rnd);
}

/* Helper: decode an address tag value back to (region, byte_offset).
   Returns -1 byte_offset if not a tagged address. */
static int decode_tag_addr(int32_t tagged, Region *out_region) {
    if (!(tagged & kAddrTagBit)) return -1;
    *out_region = (tagged & kAddrStringBit) ? Region::Strings
                : (tagged & kAddrLocalBit)  ? Region::Locals
                                            : Region::Globals;
    return (int)(tagged & kAddrOffsetMask);
}

/* Read int32 from a tagged address (a global's, a local's or a string's). */
static int32_t read_tagged(MusVM *vm, int32_t tagged) {
    Region region = Region::Globals;
    int off = decode_tag_addr(tagged, &region);
    if (off < 0) return tagged;     /* not a tag -> treat as raw value */
    return region_read32(vm, region, off);
}
static void write_tagged(MusVM *vm, int32_t tagged, int32_t v) {
    Region region = Region::Globals;
    int off = decode_tag_addr(tagged, &region);
    if (off < 0) return;
    region_write32(vm, region, off, v);
    if (region == Region::Globals) notify_var_changed(vm, off, v);
}

/* Read a dword from a tagged source region at a sub-offset (for the block-copy ops). */
static int32_t read_tagged_at(MusVM *vm, int32_t tagged, int sub_off) {
    Region region = Region::Globals;
    int off = decode_tag_addr(tagged, &region);
    if (off < 0) return 0;            /* untagged source: no region to copy from */
    return region_read32(vm, region, (int64_t)off + sub_off);
}

/* Jointops.exe!AudioVM_Op_PushStr @ 0x672830: reads a u8 index (IP advances 1) and
   pushes the word at the string table's index, `mov ebx, [ecx+eax*4]` @ 0x67283D: the
   index-th string's address, or past the table the raw bytes the chunk holds after it
   (its strings' own). The VM pushes the word at that place of its copy of the table and
   the strings (MusVM::strings); past them 0, where the game pushes the chunk's zero padding
   or the debug section's bytes (D-MUS-22). */
static void op_push_str(MusVM *vm) {
    const int index = read_u8(vm);
    vm_push(vm, region_read32(vm, Region::Strings, 4ll * index));
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
   The inverse of FIsSet. The handler is in the registered table but past the
   count the resolver walks (kRegistered below), so its name never resolves. */
static void intrinsic_fisclear(MusVM *vm) {
    int32_t var_addr = vm_pop(vm);
    int32_t mask     = vm_pop(vm);
    int32_t cur = read_tagged(vm, var_addr);
    vm_push(vm, ((cur & mask) == 0) ? -1 : 0);
}

/* The game's registered methods: 36-byte records, a 32-byte name and its handler, which the loader resolves each
   script name against by strcmp [orig: g_AudioVMIntrinsicNameTable @ 0x84F0C8, registered by
   AudioVM_OpenContextFile @ 0x6721E1..0x6721ED -> AudioVM_RegisterIntrinsics @ 0x6726F0;
   AudioVM_FindContextByName @ 0x6723E0]. The table holds nine, but the count it is registered with is 8 [orig:
   g_AudioVMIntrinsicNameCount @ 0x84F20C], so the resolver never reaches FIsClear: its name, TStart's and
   TStop's (MUSIC.LAN's, which this build does not register) and any other resolve to none (jo-c cross-check
   2026-09-10). */
struct Registered {
    const char *name;
    Intrinsic fn;
};
static const Registered kRegistered[] = {
    {"GEcho", intrinsic_gecho}, {"GGRnd", intrinsic_ggrnd}, {"GSV", intrinsic_gsv},
    {"GSDV", intrinsic_gsdv},   {"GFB", intrinsic_gfb},     {"FSet", intrinsic_fset},
    {"FClear", intrinsic_fclear}, {"FIsSet", intrinsic_fisset}, {"FIsClear", intrinsic_fisclear},
};
static constexpr int kRegisteredCount = 8;
static_assert(kRegisteredCount <= (int)(sizeof(kRegistered) / sizeof(kRegistered[0])), "the count the resolver walks");

/* A script's method name to its handler: the first registered record of that name among the count, NULL for none
   [orig: AudioVM_FindContextByName @ 0x6723F0..0x672437]. */
static Intrinsic resolve_method(const char *name) {
    for (int i = 0; i < kRegisteredCount; ++i)
        if (strcmp(kRegistered[i].name, name) == 0) return kRegistered[i].fn;
    return NULL;
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
            const std::string name = mus_section_label(vm->script, i);
            if (!section_name_eq_local(vm->current_section_name, name.c_str())) {
                set_current_section(vm, i);
                if (vm->hooks.on_section_entered) {
                    vm->hooks.on_section_entered(vm->hooks.user,
                                                 vm->current_section_name);
                }
            }
            return;
        }
    }
}

/* The dispatch loop @ Jointops.exe!AudioVM_DispatchLoop @ 0x00672720 --
   shared by the per-tick step and the restart frame. Budget = 32
   instructions (dword_3246B24). Halt latches break early. `gate_on_running`
   is the embedder's start/stop/pause seam (the tick honours it; the restart
   frame runs on any loaded script, like retail's step). */
static void run_dispatch_loop(MusVM *vm, int gate_on_running) {
    vm->halt_latch = 0;
    /* [orig: AudioVM_DispatchLoop @ 0x672720] the instruction budget
       (dword_3246B24 = 32) is a SOFT floor, not a hard cap. The loop tail is
       `dec budget; jg loop; cmp ebp,stack_base; jnz loop`, i.e.
       `while (--budget > 0 || ebp != stack_base)`: once the 32 budget is spent
       it keeps executing until the data stack drains back to base (sp == 0) or
       a handler sets the carry/halt latch. D-MUS-12 (a.k.a. D-NEW-1): we mirror that by breaking
       only when the budget is spent AND the data stack is empty. (pc-range and
       unknown-opcode guards still bound malformed scripts; vm_push overflow ->
       ERROR.) */
    int budget = kTickBudget;
    /* Safety ceiling on the soft-drain extension: a well-formed statement is far
       under this, but a malformed `goto`-loop that never drains/halts would spin
       forever (the original hangs too); cap it so the embedder never wedges. */
    int extension = kTickBudget * 64;
    for (;;) {
        if (gate_on_running) {
            if (vm->state != MUS_VM_RUNNING) break;
        } else if (vm->state == MUS_VM_ERROR || vm->state == MUS_VM_HALTED) {
            break;
        }
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
}

/* Tick: the per-update step [orig: sub_672EE0 -> AudioVM_RunContext @ 0x672ec1 with
   the restart byte clear -> AudioVM_DispatchLoop @ 0x672720]. */
int mus_vm_tick(MusVM *vm, uint32_t dt_ms) {
    init_handlers();
    if (!vm) return 0;
    if (vm->state != MUS_VM_RUNNING) return 0;
    if (!vm->script || !vm->script->code) return 0;

    run_dispatch_loop(vm, 1);
    return (int)dt_ms;
}

/* The restart frame [orig: AudioVM_RunContext @ 0x672e95..0x672ec1]:
     movzx eax,[esi+20h]; test eax,eax; jz normal_step   ; restart byte (ctx+32)
     mov byte [esi+20h],0                                 ; clear it
     mov edx,[esi+18h]; mov [edi],edx; add edi,4          ; push IP on the return stack
     mov [ebp],eax; xor ebx,ebx; mov [ebp+4],ebx; add ebp,8 ; push (value, 0)
     mov ecx,chunk; mov esi,[ecx+40h]; jnz dispatch        ; IP = MessageHandler
     call AudioVM_DispatchLoop; mov [ctx+18h],esi          ; run, store the IP
   MusicCtx_SelectEndTrack @ 0x672fd0 sets the byte and calls this step at
   once whenever chunk_04 is loaded, so the two are one call here. */
int mus_vm_signal(MusVM *vm, int32_t value) {
    init_handlers();
    if (!vm || !vm->script || !vm->script->code) return -1;
    if (!vm->script->has_message_handler) return -2;
    if (vm->csp >= kCallStackCap) {
        snprintf(vm->last_error, sizeof(vm->last_error), "call stack overflow");
        vm->state = MUS_VM_ERROR;
        return -3;
    }
    vm->call_stack[vm->csp++] = vm->pc;
    vm_push(vm, value);
    vm_push(vm, 0);
    vm->pc = vm->script->message_handler_offset;
    run_dispatch_loop(vm, 0);
    return 0;
}

/* ---- Globals accessors ------------------------------------------------ */

std::shared_ptr<MusGlobals> mus_vm_globals(MusVM *vm) {
    return vm != nullptr && vm->script != nullptr ? vm->globals : nullptr;
}

/* The game reads and writes the area by index unbounded [orig: AudioVM_GetVariable @ 0x672660, AudioVM_GetVariablePtr @ 0x672680:
   globals + index * 4]: past the globals, the locals after them in the block; past the block nothing (D-MUS-22). */
int32_t mus_globals_read(const MusGlobals &globals, uint32_t index) {
    int32_t value = 0;
    if (4ull * index + 4 <= globals.block.size()) memcpy(&value, globals.block.data() + 4ull * index, 4);
    return value;
}

void mus_globals_write_raw(MusGlobals &globals, uint32_t index, int32_t value) {
    if (4ull * index + 4 <= globals.block.size()) memcpy(globals.block.data() + 4ull * index, &value, 4);
}

uint32_t mus_globals_count(const MusGlobals &globals) { return globals.globals_size / 4; }

const char *mus_vm_string(const MusVM *vm, int32_t value) {
    if (!vm || !vm->script) return NULL;
    Region region = Region::Globals;
    const int off = decode_tag_addr(value, &region);
    if (off < 0 || region != Region::Strings) return NULL;
    if ((uint64_t)off < 4ull * vm->script->string_count || (size_t)off >= vm->strings.size()) return NULL;
    /* Its NUL within the copy (a flag method may have written over it, as the game's writes the chunk). */
    const char *text = (const char *)vm->strings.data() + off;
    return memchr(text, 0, vm->strings.size() - (size_t)off) ? text : NULL;
}

int32_t mus_vm_get_var(const MusVM *vm, uint8_t var_index) {
    if (!vm) return 0;
    int byte_off = (int)var_index * 4;
    return globals_read32(vm, byte_off);
}

/* The embedder's write into the VM globals (the screen MUSICVAR, the mission
   var pumps) [orig: AudioVM_SetVariable @ 0x671FA0]. */
void mus_vm_set_var(MusVM *vm, uint8_t var_index, int32_t value) {
    if (!vm) return;
    int byte_off = (int)var_index * 4;
    if (!region_at(vm, Region::Globals, byte_off, 4)) return;
    globals_write32(vm, byte_off, value);
    notify_var_changed(vm, byte_off, value);
}

/* ---- E11: jump_to_section + section tracking --------------------------- */

int mus_vm_jump_to_section(MusVM *vm, const char *name) {
    if (!vm || !vm->script || !name) return -1;
    for (uint32_t i = 0; i < vm->script->section_count; ++i) {
        if (section_name_eq_local(mus_section_label(vm->script, i).c_str(), name)) {
            vm->pc = vm->script->sections[i].code_offset;
            set_current_section(vm, i);
            if (vm->hooks.on_section_entered) {
                vm->hooks.on_section_entered(vm->hooks.user, vm->current_section_name);
            }
            return 0;
        }
    }
    return -2;
}

} // namespace opennova::mus
