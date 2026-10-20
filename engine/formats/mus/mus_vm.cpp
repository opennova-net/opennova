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

/* The stacks' sizes, the game's: the data stack 1024 words from off_84F218 (0x3244950) to
   dword_3245950, the call stack 1024 entries from off_84F21C (0x3245B20) to dword_3246B20. Past
   the data stack's top the game's pushes write the VM's debug flag and chunk pointer; the VM
   stops (D-MUS-27). */
constexpr int kDataStackCap = 1024;
constexpr int kCallStackCap = 1024;

/* The script instance's bytes, AudioVmScriptInstance's 36 [orig: g_AudioVmInstance @ 0x3245AE8 ..
   0x3245B0B], and its fields' offsets, as ScriptInstance_Init @ 0x672EF0 fills them: the dispatch
   function, the chunk, the data block, the globals and the locals, the method table, the IP, the
   section, the restart byte and its 3 reserved bytes. */
constexpr int kInstanceSize = 0x24;
constexpr int kInstDispatch = 0x00, kInstChunk = 0x04, kInstBlock = 0x08, kInstGlobals = 0x0C;
constexpr int kInstLocals = 0x10, kInstBindings = 0x14, kInstIp = 0x18, kInstSection = 0x1C;
constexpr int kInstRestart = 0x20;
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

/* A method's handler (an intrinsic of the game's registered table): it takes the address of the data
   stack's top word, `top` its index, reads its arguments there and below, pops none and returns the
   word `method` pushes [orig: AudioVM_Op_Method @ 0x672D04..0x672D10]. */
typedef int32_t (*Intrinsic)(MusVM *, int top);

struct MusVM {
    const MusScript *script;
    MusVMHooks       hooks;
    MusVMState       state;

    /* Data stack (top grows up; sp = next-free; below 0 where the game's pointer runs below its
       base, stack_word / vm_push). */
    int32_t  data_stack[kDataStackCap];
    int      sp;

    /* The run's globals and locals bases [orig: g_AudioVMGlobalsBase @ 0x3245B10, g_AudioVMLocalsBase
       @ 0x3244948, set every run from the instance's +0x0C and +0x10 @ 0x672E5B..0x672E67]: every
       global and local access goes through them. The locals' base is also the word below the
       context's pointer under the data stack, which a push below the stack's base writes. */
    int32_t  globals_base;
    int32_t  locals_base;

    /* The instance's bytes, little-endian, at the game's offsets (kInst*): what a read or a write
       through `push_me`'s word reaches (Region::Instance). The chunk's and the method table's
       pointers are the loaded file's heap addresses, which the VM's model has none of: their bytes
       stay 0 and are never written (region_at). */
    uint8_t  instance[kInstanceSize];

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

/* The running context's pointer [orig: dword_324494C, set by AudioVM_RunContext @ 0x672E56]:
   the word `push_me` pushes, and the word below the data stack's base, which starts right after
   it (off_84F218 = 0x3244950): the one instance's address, &g_AudioVmInstance, which both run
   entries pass [orig: sub_672EE0 @ 0x672EE0; MusicCtx_SelectEndTrack @ 0x672FE2] (D-MUS-26). A
   word in its 36 bytes is an address of the VM's instance (Region::Instance). */
static constexpr int32_t kSelfWord = 0x03245AE8;

/* The instance's dispatch word once ScriptInstance_Init has run, AudioVM_RunContext's address
   [orig: ScriptInstance_Init @ 0x672FC6]; nothing the VM runs reads it. */
static constexpr int32_t kRunContextAddr = 0x00672E50;

/* GFB's address [orig: AudioVM_Intrinsic_GFB @ 0x672150]: GFB sets no eax, so `method` pushes
   the eax it called the handler through (@ 0x672D08 call eax) (D-MUS-26). */
static constexpr int32_t kGfbAddr = 0x00672150;

/* The data block's address, global 0's: the instance's +0x08 and +0x0C [orig: ScriptInstance_Init
   @ 0x672F95..0x672F98]. */
static constexpr int32_t kBlockWord = kAddrTagBit;

/* The locals' base at a run's start, the instance's +0x10 [orig: ScriptInstance_Init @ 0x672FA3;
   g_AudioVMLocalsBase @ 0x3244948, AudioVM_RunContext @ 0x672E64..0x672E67]: local 0's address. */
static constexpr int32_t kLocalsBaseWord = kAddrTagBit | kAddrLocalBit;

/* A code address, the instance's IP (+0x18): the tag with the local and string bits, the
   bytecode-relative offset under them; a read through one reads the script's code. */
static constexpr int32_t kCodeAddrTag = kAddrTagBit | kAddrLocalBit | kAddrStringBit;

enum class Region { Globals, Locals, Strings, Instance, Code };

/* `n` bytes at `off` in a region, or NULL outside what the VM holds: past the instance's block
   (or the strings' copy) the game reads and writes the heap after its allocation, which the VM
   does not (D-MUS-22). A local is the block's byte globals_size past its offset, so one below
   the locals' start is a global's. */
static uint8_t *region_at(MusVM *vm, Region region, int64_t off, size_t n);

/* A word of the instance at its offset (little-endian, as the game's). */
static int32_t instance_word(const MusVM *vm, int off) {
    int32_t v;
    memcpy(&v, vm->instance + off, 4);
    return v;
}
static void set_instance_word(MusVM *vm, int off, int32_t v) { memcpy(vm->instance + off, &v, 4); }

/* The VM's address of a bytecode-relative pc (the instance's IP). */
static int32_t code_addr(uint32_t pc) { return kCodeAddrTag | (int32_t)(pc & (uint32_t)kAddrOffsetMask); }

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
    /* The stacks keep their words, as the game's one pair does across scripts (no load clears
       them); a fresh VM's are zero, as the game's BSS starts. */
    vm->globals_base = kBlockWord;
    vm->locals_base = kLocalsBaseWord;
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
    /* The instance as ScriptInstance_Init fills it [orig: @ 0x672EF5 the dispatch word, then
       @ 0x672FC6 AudioVM_RunContext's address; @ 0x672F95..0x672FA3 the block, its globals and its
       locals; @ 0x672FB5 the entry section; @ 0x672FC3 the IP]; the chunk's and the method table's
       pointers (@ 0x672F72, @ 0x672FAF) are heap addresses the VM has none of, and the restart byte
       and its reserved bytes it leaves as they were. */
    set_instance_word(vm, kInstDispatch, kRunContextAddr);
    set_instance_word(vm, kInstBlock, kBlockWord);
    set_instance_word(vm, kInstGlobals, kBlockWord);
    set_instance_word(vm, kInstLocals, kLocalsBaseWord);
    set_instance_word(vm, kInstIp, code_addr(vm->pc));
    set_instance_word(vm, kInstSection, (int32_t)s->entry_section_index);
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
    /* The instance's words the teardown zeroes [orig: AudioVM_FreeSoundBuffer @ 0x672E25..0x672E43:
       the chunk, the globals, the locals, the IP, the section, the dispatch word]; the block's
       pointer (freed, not zeroed), the method table's and the restart byte stay. */
    set_instance_word(vm, kInstDispatch, 0);
    set_instance_word(vm, kInstGlobals, 0);
    set_instance_word(vm, kInstLocals, 0);
    set_instance_word(vm, kInstIp, 0);
    set_instance_word(vm, kInstSection, 0);
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

/* A push [orig: e.g. AudioVM_Op_PushImm8 @ 0x672794 mov [ebp+0],eax; add ebp,4]. Below the
   stack's base (a pop past it, or a frame setup over fewer words than its count, left the pointer
   there) it writes what the game keeps there: at -2 the locals' base, which every local access
   then goes through; at -1 the running context's pointer, which the game's next setstate, done
   and method and its run's end (AudioVM_RunContext @ 0x672EC9) read and write the instance
   through, so a word other than it stops the VM; below them the music stream's sample buffer,
   which the VM does not hold (D-MUS-27). */
static void vm_push(MusVM *vm, int32_t v) {
    if (vm->sp >= kDataStackCap) {
        snprintf(vm->last_error, sizeof(vm->last_error), "data stack overflow");
        vm->state = MUS_VM_ERROR;
        return;
    }
    if (vm->sp >= 0) {
        vm->data_stack[vm->sp] = v;
    } else if (vm->sp == -2) {
        vm->locals_base = v;
    } else if (vm->sp == -1 && v != kSelfWord) {
        snprintf(vm->last_error, sizeof(vm->last_error),
                 "a push below the data stack's base over the running context's pointer");
        vm->state = MUS_VM_ERROR;
        return;
    }
    ++vm->sp;
}

/* The word at a data stack index: the stack's own from its base up; below the base (off_84F218 =
   0x3244950) what the game keeps before it: the running context's pointer (dword_324494C, index
   -1: kSelfWord), the locals' base (g_AudioVMLocalsBase @ 0x3244948, index -2: the address of
   local 0 at a run's start, as `push_la 0` pushes it) and below them the music stream's sample
   buffer's tail (word_3243850, AudioStream_Update @ 0x671D3A), which the VM reads as 0
   (D-MUS-27). */
static int32_t stack_word(const MusVM *vm, int index) {
    if (index >= 0) return index < kDataStackCap ? vm->data_stack[index] : 0;
    if (index == -1) return kSelfWord;
    if (index == -2) return vm->locals_base;
    return 0;
}

/* A pop [orig: e.g. AudioVM_Op_PopLocal @ 0x672870 mov ebx,[ebp-4]; @ 0x672880 sub ebp,4]: past
   the stack's base the game reads on below it, its pointer going below the base. */
static int32_t vm_pop(MusVM *vm) {
    --vm->sp;
    return stack_word(vm, vm->sp);
}

/* The locals are the block past its globals [orig: ScriptInstance_Init @ 0x672F9B..0x672FA3,
   instance[+0x10] = the block + globals_size], so a local's offset below 0 (the frame setup's
   word below a frame base under 4, AudioVM_Op_Enter @ 0x672C40) is a global's place. */
/* The instance (Region::Instance) is its 36 bytes but the chunk's and the method table's pointers,
   heap addresses of the loaded file the VM's model has none of (D-MUS-22). The script's code
   (Region::Code) is the model's, read only: a read takes its bytes (byte_at), a write writes
   nothing, where the game's writes its loaded file (D-MUS-22). */
static uint8_t *region_at(MusVM *vm, Region region, int64_t off, size_t n) {
    if (region == Region::Instance) {
        if (off < 0 || off + (int64_t)n > kInstanceSize) return NULL;
        for (int64_t k = off; k < off + (int64_t)n; ++k)
            if ((k >= kInstChunk && k < kInstChunk + 4) || (k >= kInstBindings && k < kInstBindings + 4)) return NULL;
        return vm->instance + off;
    }
    if (region == Region::Code) return NULL;
    if (region != Region::Strings && !vm->globals) return NULL;
    std::vector<uint8_t> &bytes = region == Region::Strings ? vm->strings : vm->globals->block;
    const int64_t at = off + (region == Region::Locals ? (int64_t)vm->globals->globals_size : 0);
    if (at < 0 || (uint64_t)at + n > bytes.size()) return NULL;
    return bytes.data() + at;
}

/* A byte a read takes: the region's, the script's code's for a code address, 0 outside. */
static uint8_t byte_at(const MusVM *vm, Region region, int64_t off) {
    if (region == Region::Code)
        return vm->script && off >= 0 && (uint64_t)off < vm->script->code_size ? vm->script->code[off] : 0;
    const uint8_t *at = region_at(const_cast<MusVM *>(vm), region, off, 1);
    return at ? *at : 0;
}

/* A word, byte by byte: one at a region's end takes the bytes in it, 0 for those past it (D-MUS-22). */
static int32_t region_read32(const MusVM *vm, Region region, int64_t off) {
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) bytes[k] = byte_at(vm, region, off + k);
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

/* A global, a local and an embedder's variable are a base word plus a byte offset: the run's
   globals base [orig: AudioVM_Op_PushGlobal @ 0x6727B0 over g_AudioVMGlobalsBase], the run's
   locals base [orig: AudioVM_Op_PushLocal @ 0x6727D0 mov edx,g_AudioVMLocalsBase; mov
   ebx,[edx+eax]], the instance's globals word [orig: AudioVM_GetVariable @ 0x672660 over
   instance+0x0C]: the region and the offset the base word's address names (the block's globals
   and locals, unless a write through `push_me`'s word or a push below the data stack's base
   wrote another), little-endian and byte-addressable (inc_g/dec_g step a byte). A base that is no
   address of the VM's (an untagged word, which the game dereferences) names none: 0 read, nothing
   written (D-MUS-27). */
static int decode_tag_addr(int32_t tagged, Region *out_region);
static bool base_place(int32_t base_word, int64_t byte_off, Region *region, int64_t *off) {
    const int base = decode_tag_addr(base_word, region);
    if (base < 0) return false;
    *off = (int64_t)base + byte_off;
    return true;
}
static int32_t based_read32(const MusVM *vm, int32_t base_word, int64_t byte_off) {
    Region region = Region::Globals;
    int64_t off = 0;
    return base_place(base_word, byte_off, &region, &off) ? region_read32(vm, region, off) : 0;
}
static void based_write32(MusVM *vm, int32_t base_word, int64_t byte_off, int32_t v) {
    Region region = Region::Globals;
    int64_t off = 0;
    if (base_place(base_word, byte_off, &region, &off)) region_write32(vm, region, off, v);
}
static uint8_t *based_byte(MusVM *vm, int32_t base_word, int64_t byte_off) {
    Region region = Region::Globals;
    int64_t off = 0;
    return base_place(base_word, byte_off, &region, &off) ? region_at(vm, region, off, 1) : NULL;
}
static int32_t globals_read32(const MusVM *vm, int byte_off) { return based_read32(vm, vm->globals_base, byte_off); }
static void globals_write32(MusVM *vm, int byte_off, int32_t v) { based_write32(vm, vm->globals_base, byte_off, v); }
static int32_t locals_read32(const MusVM *vm, int64_t byte_off) {
    return based_read32(vm, vm->locals_base, byte_off);
}
static void locals_write32(MusVM *vm, int64_t byte_off, int32_t v) {
    based_write32(vm, vm->locals_base, byte_off, v);
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
   and ignored. We consume both so the IP stays aligned when running real bytecode. The address
   is the run's globals base plus the operand (`add eax, g_AudioVMGlobalsBase` @ 0x6727F5). */
static void op_push_global_addr(MusVM *vm) {
    int byte_off = read_u8(vm);
    (void)read_u8(vm);                 /* reserved 2nd operand byte (engine advances 2) */
    vm_push(vm, (int32_t)((uint32_t)vm->globals_base + (uint32_t)byte_off));
}

/* Witnessed: Jointops.exe!AudioVM_Op_PushLocalAddr @ 0x672810 (same 2-byte operand): the locals'
   base plus the operand (`add eax, g_AudioVMLocalsBase` @ 0x672815), the VM's address of that local
   while the base is its own. */
static void op_push_local_addr(MusVM *vm) {
    int byte_off = read_u8(vm);
    (void)read_u8(vm);                 /* reserved 2nd operand byte (engine advances 2) */
    vm_push(vm, (int32_t)((uint32_t)vm->locals_base + (uint32_t)byte_off));
}

/* Witnessed: Jointops.exe!VmOp_PushSelf @ 0x6728F0; Jointops.exe pushes the current
   instance pointer (kSelfWord). */
static void op_push_self(MusVM *vm) {
    vm_push(vm, kSelfWord);
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
    if (uint8_t *at = based_byte(vm, vm->globals_base, read_u8(vm))) ++*at;
}
static void op_dec_g(MusVM *vm) {
    if (uint8_t *at = based_byte(vm, vm->globals_base, read_u8(vm))) --*at;
}
static void op_inc_l(MusVM *vm) {
    if (uint8_t *at = based_byte(vm, vm->locals_base, read_u8(vm))) ++*at;
}
static void op_dec_l(MusVM *vm) {
    if (uint8_t *at = based_byte(vm, vm->locals_base, read_u8(vm))) --*at;
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
/* Op 0x34 callv [orig: AudioVM_Op_CallV @ 0x672B90: mov eax,[esi]; add esi,4; mov [edi],esi;
   add ebp,4; mov esi,chunk; add esi,eax]: the return address is written at the call stack's top,
   which it does not advance, and the data stack's pointer steps over one word instead, the one its
   slot holds from an earlier push. A `return` therefore pops the word below the call stack's top,
   not this one (D-MUS-25). */
static void op_callv  (MusVM *vm) {
    int32_t target = read_u32(vm);
    if (vm->csp < kCallStackCap) vm->call_stack[vm->csp] = vm->pc;
    if (vm->sp >= kDataStackCap) {
        snprintf(vm->last_error, sizeof(vm->last_error), "data stack overflow");
        vm->state = MUS_VM_ERROR;
        return;
    }
    ++vm->sp;
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
         lea ecx,[ecx*4]; sub ebp,ecx      ; POP N dwords (ebp: the data stack's next free word)
         mov ebx,LocalsBase; add ebx,[chunk+0x3C]
         @ 0x672C3C: mov eax,[ebp+ecx-4]; mov [ebx+ecx-4],eax; sub ecx,4; jg @ 0x672C3C
       The loop copies from the last word down and tests after the copy, so it moves the N popped
       words to locals[frame + i*4] and, for N = 0, one word all the same: [ebp-4], the stack's
       top, left on it, into locals[frame - 4], the word below the frame base (D-MUS-16); on an
       empty stack that is the word below the stack's base, the context's (stack_word).
       The frame offset is instance[+0x3C] (== the chunk's frame_base field,
       0x20 in every MDEdit chunk), added as the chunk holds it, 32 bits wide (0 is a
       frame at the locals' start, the word below it the globals' last):
       MusScript.locals_frame_offset.
       D-MUS-6 (a.k.a. D-NEW-2): the original POPS the N args (`sub ebp,N*4`); we mirror with
       `vm->sp -= n`. With fewer than N words on the stack the game does not check: it copies the
       words below the stack's base too (stack_word) and leaves its stack pointer below the base
       (vm_push, vm_pop). The destination is the locals' base word plus the frame (locals_write32). */
    const int n = read_u8(vm);
    const int64_t frame = vm->script ? (int64_t)(int32_t)vm->script->locals_frame_offset : 0;
    vm->sp -= n;
    int64_t k = 4ll * n;                                         /* ecx */
    do {
        const int32_t v = stack_word(vm, vm->sp + (int)(k / 4) - 1);   /* [ebp+ecx-4] */
        locals_write32(vm, frame + k - 4, v);                    /* [ebx+ecx-4] */
        k -= 4;
    } while (k > 0);                                             /* sub ecx,4; jg */
}
/* Op 0x39 return [orig: AudioVM_Op_Return @ 0x672C50: mov esi,[edi-4]; sub edi,4; test esi,esi;
   jg clc, else stc]: the IP popped from the call stack, the word below its pointer, the run going
   on there (the game's IPs are addresses, never 0 or below). At the call stack's base the game pops
   the word below it, the script file's size (dword_3245B1C, AudioVM_LoadScriptFile @ 0x672D57),
   and runs on at that as an address, which faults; the VM, with nothing to return to, stops
   (MUS_VM_HALTED) (D-MUS-27). */
static void op_return (MusVM *vm) {
    if (vm->csp > 0) {
        vm->pc = vm->call_stack[--vm->csp];
        return;
    }
    snprintf(vm->last_error, sizeof(vm->last_error), "a return at the call stack's base");
    vm->state = MUS_VM_HALTED;
    vm->halt_latch = 1;
}
static void op_yield  (MusVM *vm) { vm->halt_latch = 1; }

/* Halt opcodes (E6/E10): set the latch so the dispatch loop exits. */
static void op_done(MusVM *vm) {
    /* Witnessed: VmOp_Done @ 0x672CD0 sets instance entry_section_index back
       to chunk->entry_section_index (0) and STC. We mirror by re-seeking pc
       to the entry section's code_offset and halting; the instance's section word is the entry's
       [orig: @ 0x672CD9..0x672CE2 mov ebx,[edx+2Ch]; mov [esi+1Ch],ebx]. */
    if (vm->script) set_instance_word(vm, kInstSection, (int32_t)vm->script->entry_section_index);
    if (vm->script && vm->script->section_count > 0
        && vm->script->entry_section_index < vm->script->section_count) {
        vm->pc = vm->script->sections[vm->script->entry_section_index].code_offset;
        set_current_section(vm, vm->script->entry_section_index);
    }
    vm->halt_latch = 1;
}

static void op_setstate(MusVM *vm) {
    int sidx = read_u8(vm);
    /* The instance's section word is the operand [orig: AudioVM_Op_SetState @ 0x672C79..0x672C82
       movzx ebx,[esi]; mov esi,dword_324494C; mov [esi+1Ch],ebx]. */
    set_instance_word(vm, kInstSection, sidx);
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
    /* Op 0x33 [orig: Jointops.exe!AudioVM_Op_CallVL @ 0x672B70]: reads its byte, loads
       dword_3245958[byte], calls it when nonzero and pushes eax, so a null entry pushes 0
       (@ 0x672b74..0x672b81). That table's only writer is an unreferenced function at
       0x6726a0 (no code xref, no pointer to it); AudioVM_LoadScriptFile does not fill it.
       So every entry is 0 and push 0 is the game's own result for idx 0..31. Past 31 the
       game reads past the 32-dword table (dword_32459D8 and on) and calls what is there; we
       push 0 (D-MUS-7, docs/audio/mus-sbf-re.md). MDEdit music scripts use opcode 0x40
       `method` for the built-in intrinsics (GEcho/GSV/...), not callvl. */
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
    /* The handler takes the address of the stack's top word and pops nothing: its arguments stay
       under the word pushed above them, its eax [orig: @ 0x672D04 lea ebx,[ebp-4]; push ebx; call
       eax; @ 0x672D0D mov [ebp],eax; add ebp,4]; a name resolved to none pushes 0 (@ 0x672D02,
       eax the table's 0) (D-MUS-24). */
    const Intrinsic fn = vm->methods[(size_t)idx];
    vm_push(vm, fn ? fn(vm, vm->sp - 1) : 0);
}

/* ---- Intrinsic methods (E7-E9) ---------------------------------------- */

/* Each reads its arguments at the top word's index and below (stack_word: below the stack's base,
   what the game keeps there) and returns what `method` pushes. */

/* Witnessed: Jointops.exe!Intrinsic_GEcho @ 0x6720C0: the echo callback (dword_31C3814) tail-called
   with the top word, its eax returned; with none set, 0. Fires on_echo with the top word. */
static int32_t intrinsic_gecho(MusVM *vm, int top) {
    const int32_t arg = stack_word(vm, top);
    if (vm->hooks.on_echo) vm->hooks.on_echo(vm->hooks.user, arg);
    return 0;
}

/* Jointops.exe!AudioVM_Intrinsic_GGRnd @ 0x672320 (byte-exact port).
   Reads 2 (the word below the top lo, `mov ebx,[ebp-4]`; the top hi, `mov ecx,[ebp+0]`). The
   process-global seed g_AudioVMGGRndSeed @ 0x84F210 is
   statically initialised to 0xBABEFACE in the binary and updated in place:
       seed = rol32(seed + rol32(seed, 11), 2)
   Then (asm `and eax,0FFFFh; idiv ecx`): rnd = (seed & 0xFFFF) % span, span = hi-lo+1
   (rnd forced to 0 when span == 0); result = lo + rnd. We mirror the process-global
   seed with a file-static so the sequence matches a fresh game process. */
static int32_t intrinsic_ggrnd(MusVM *vm, int top) {
    int32_t hi = stack_word(vm, top);
    int32_t lo = stack_word(vm, top - 1);
    static uint32_t s_ggrnd_seed = 0xBABEFACEu;   /* initial value of dword_84F210 */
    s_ggrnd_seed = opennova::io::rotl32(s_ggrnd_seed + opennova::io::rotl32(s_ggrnd_seed, 11), 2);
    int32_t span = hi - lo + 1;
    int32_t rnd  = 0;
    if (span != 0) rnd = (int32_t)(s_ggrnd_seed & 0xFFFFu) % span;
    return lo + rnd;
}

/* Helper: decode an address tag value back to (region, byte_offset).
   Returns -1 byte_offset if not a tagged address. */
static int decode_tag_addr(int32_t tagged, Region *out_region) {
    /* A word in the instance's 36 bytes is its address, as `push_me`'s word is (D-MUS-26). */
    const uint32_t in_instance = (uint32_t)tagged - (uint32_t)kSelfWord;
    if (in_instance < (uint32_t)kInstanceSize) {
        *out_region = Region::Instance;
        return (int)in_instance;
    }
    if (!(tagged & kAddrTagBit)) return -1;
    const bool string_bit = (tagged & kAddrStringBit) != 0, local_bit = (tagged & kAddrLocalBit) != 0;
    *out_region = string_bit && local_bit ? Region::Code
                : string_bit              ? Region::Strings
                : local_bit               ? Region::Locals
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
   Sets master and right-channel volume to clamp(top,0,255)<<16 (16.16 fixed
   point) and returns it. Hooks on_volume_changed. */
static int32_t intrinsic_gsv(MusVM *vm, int top) {
    int32_t v = stack_word(vm, top);
    int32_t clipped = v;
    if (clipped < 0)   clipped = 0;
    if (clipped > 255) clipped = 255;
    int32_t fixed = clipped << 16;
    if (vm->hooks.on_volume_changed) {
        vm->hooks.on_volume_changed(vm->hooks.user, fixed, fixed);
    }
    return fixed;
}

/* Witnessed: Jointops.exe!Intrinsic_GSDV @ 0x672120.
   Sets right-channel only. Reuse on_volume_changed but pass current(left)
   unchanged; we don't track the previous left value, so pass `fixed` for
   both channels and let the embedder disambiguate. */
static int32_t intrinsic_gsdv(MusVM *vm, int top) {
    int32_t v = stack_word(vm, top);
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
    return fixed;
}

/* [orig: AudioVM_Intrinsic_GFB @ 0x672150] `mov dword_31C37EC, 0; retn` -- takes
   NO args (does not deref the &TOS pointer) and sets no eax, so `method` pushes the
   handler's own address, the eax it was called through (@ 0x672D08 call eax) (D-MUS-26).
   Side effect: clears the fade-base global dword_31C37EC, which the VM does not model. */
static int32_t intrinsic_gfb(MusVM *vm, int top) {
    (void)vm;
    (void)top;
    return kGfbAddr;
}

/* Witnessed: Jointops.exe!Intrinsic_FSet @ 0x672360.
   Reads 2 (the top word &var, `mov ebx,[ebp+0]`; the word below it the mask, `mov ecx,[ebp-4]`).
   Sets *var |= mask, returns new value.
   Operands typically come from `push <mask>; push_ga <var>; method FSet`.

   Phase E uses the bound-flag bitmap as well: when the operand pushed via
   push_ga refers to the conventional flag-byte global (e.g. byte offset 64),
   the script gets per-bit semantics from the bitmap. For tagged-address
   inputs we route through globals memory directly. */
static int32_t intrinsic_fset(MusVM *vm, int top) {
    int32_t var_addr = stack_word(vm, top);
    int32_t mask     = stack_word(vm, top - 1);
    int32_t cur = read_tagged(vm, var_addr);
    int32_t result = cur | mask;
    write_tagged(vm, var_addr, result);
    return result;
}
static int32_t intrinsic_fclear(MusVM *vm, int top) {
    int32_t var_addr = stack_word(vm, top);
    int32_t mask     = stack_word(vm, top - 1);
    int32_t cur = read_tagged(vm, var_addr);
    int32_t result = cur & ~mask;
    write_tagged(vm, var_addr, result);
    return result;
}
static int32_t intrinsic_fisset(MusVM *vm, int top) {
    int32_t var_addr = stack_word(vm, top);
    int32_t mask     = stack_word(vm, top - 1);
    int32_t cur = read_tagged(vm, var_addr);
    /* Witness: returns -1 if (*TOS & NOS) == NOS, else 0. */
    return ((cur & mask) == mask) ? -1 : 0;
}

/* Jointops.exe!AudioVM_Intrinsic_FIsClear @ 0x6723C0: reads 2 (the top word &var, the word below
   it the mask), returns -1 when NONE of the mask bits are set (`(mask & *var) == 0`), else 0.
   The inverse of FIsSet. The handler is in the registered table but past the
   count the resolver walks (kRegistered below), so its name never resolves. */
static int32_t intrinsic_fisclear(MusVM *vm, int top) {
    int32_t var_addr = stack_word(vm, top);
    int32_t mask     = stack_word(vm, top - 1);
    int32_t cur = read_tagged(vm, var_addr);
    return ((cur & mask) == 0) ? -1 : 0;
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
       only when the budget is spent AND the data stack is at its base (sp == 0: a pointer
       below the base runs on too, as the game's `jnz` does). (pc-range and
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
        if (--budget <= 0 && vm->sp == 0) break;   /* budget spent + stack at its base */
        if (--extension <= 0) break;               /* safety: never spin forever */
    }
}

/* A run's start: both stacks at their bases, whatever the last run left on them, and the run's
   globals and locals bases the instance's [orig: AudioVM_RunContext @ 0x672E5B..0x672E67 from
   instance+0x0C and +0x10; @ 0x672E89 mov edi,off_84F21C (the call stack, 0x3245B20); @ 0x672E8F
   mov ebp,off_84F218 (the data stack, 0x3244950)], every run, the step's and the restart frame's
   alike (D-MUS-23). */
static void begin_run(MusVM *vm) {
    vm->sp = 0;
    vm->csp = 0;
    vm->globals_base = instance_word(vm, kInstGlobals);
    vm->locals_base = instance_word(vm, kInstLocals);
}

/* One run, the step both entries take [orig: AudioVM_RunContext @ 0x672E50]:
     movzx eax,[esi+20h]; test eax,eax; jz normal_step   ; restart byte (ctx+32)
     mov byte [esi+20h],0                                 ; clear it
     mov edx,[esi+18h]; mov [edi],edx; add edi,4          ; push IP on the return stack
     mov [ebp],eax; xor ebx,ebx; mov [ebp+4],ebx; add ebp,8 ; push (value, 0)
     mov ecx,chunk; mov esi,[ecx+40h]; jnz dispatch        ; IP = MessageHandler
     call AudioVM_DispatchLoop; mov [ctx+18h],esi          ; run, store the IP
   (@ 0x672E95..0x672ECE). The restart byte is the instance's, so one a write through
   `push_me`'s word set takes the frame on the next run too. With no MessageHandler the game
   reads through a null pointer (`mov esi,[esi+18h]` with esi 0 @ 0x672EC1); the VM, having no
   handler to run, drops the byte and takes the plain step (D-MUS-27). */
static void run_step(MusVM *vm, int gate_on_running) {
    begin_run(vm);
    const uint8_t pending = vm->instance[kInstRestart];
    vm->instance[kInstRestart] = 0;                       /* @ 0x672E9D */
    if (pending != 0 && vm->script->has_message_handler) {
        vm->call_stack[vm->csp++] = vm->pc;
        vm_push(vm, pending);
        vm_push(vm, 0);
        vm->pc = vm->script->message_handler_offset;
    }
    run_dispatch_loop(vm, gate_on_running);
    set_instance_word(vm, kInstIp, code_addr(vm->pc));
}

/* Tick: the per-update step [orig: sub_672EE0 -> AudioVM_RunContext @ 0x672E50 ->
   AudioVM_DispatchLoop @ 0x672720]. */
int mus_vm_tick(MusVM *vm, uint32_t dt_ms) {
    init_handlers();
    if (!vm) return 0;
    if (vm->state != MUS_VM_RUNNING) return 0;
    if (!vm->script || !vm->script->code) return 0;

    run_step(vm, 1);
    return (int)dt_ms;
}

/* The restart frame [orig: MusicCtx_SelectEndTrack @ 0x672FD0]: the instance's restart byte set
   to the value's low byte (`mov al,[esp+value]; mov restart_20,al` @ 0x672FD9..0x672FDD) and the
   step run at once whenever chunk_04 is loaded, so the two are one call here: a value whose low
   byte is 0 runs the plain step, the frame pushing the byte. Both stacks start at their bases
   (begin_run), so the frame's three words are all the run starts with. A byte on a script with
   no MessageHandler is dropped and the plain step runs (run_step), reported as -2. */
int mus_vm_signal(MusVM *vm, int32_t value) {
    init_handlers();
    if (!vm || !vm->script || !vm->script->code) return -1;
    const uint8_t pending = (uint8_t)value;
    const bool dropped = pending != 0 && !vm->script->has_message_handler;
    vm->instance[kInstRestart] = pending;
    run_step(vm, 0);
    return dropped ? -2 : 0;
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

/* The embedder's read of a variable: the instance's globals word plus its index's bytes [orig:
   AudioVM_GetVariable @ 0x672660..0x67266D, instance+0x0C, 0 where it is null]. */
int32_t mus_vm_get_var(const MusVM *vm, uint8_t var_index) {
    if (!vm) return 0;
    int byte_off = (int)var_index * 4;
    return based_read32(vm, instance_word(vm, kInstGlobals), byte_off);
}

/* The embedder's write into the VM globals (the screen MUSICVAR, the mission
   var pumps) [orig: AudioVM_SetVariable @ 0x671FA0; @ 0x672640..0x672651 through instance+0x0C,
   dropped where it is null]. */
void mus_vm_set_var(MusVM *vm, uint8_t var_index, int32_t value) {
    if (!vm) return;
    int byte_off = (int)var_index * 4;
    Region region = Region::Globals;
    int64_t off = 0;
    if (!base_place(instance_word(vm, kInstGlobals), byte_off, &region, &off) || !region_at(vm, region, off, 4))
        return;
    region_write32(vm, region, off, value);
    notify_var_changed(vm, byte_off, value);
}

/* ---- E11: jump_to_section + section tracking --------------------------- */

int mus_vm_jump_to_section(MusVM *vm, const char *name) {
    if (!vm || !vm->script || !name) return -1;
    for (uint32_t i = 0; i < vm->script->section_count; ++i) {
        if (section_name_eq_local(mus_section_label(vm->script, i).c_str(), name)) {
            vm->pc = vm->script->sections[i].code_offset;
            set_current_section(vm, i);
            /* The embedder's jump (the game has none) moves the instance's IP and section as a
               setstate and a run's end would. */
            set_instance_word(vm, kInstIp, code_addr(vm->pc));
            set_instance_word(vm, kInstSection, (int32_t)i);
            if (vm->hooks.on_section_entered) {
                vm->hooks.on_section_entered(vm->hooks.user, vm->current_section_name);
            }
            return 0;
        }
    }
    return -2;
}

} // namespace opennova::mus
