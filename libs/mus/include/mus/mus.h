#ifndef MUS_H
#define MUS_H

/* MUS (interactive-music script) container reader.

   SCR0 file header parser witnessed at
       Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20
   MU01 chunk pointer fixup at
       Jointops.exe!AudioVM_FixupPointers @ 0x00672470 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Format constants --- */
#define MUS_MAGIC_SCR0      0x30524353u    /* 'SCR0' little-endian */
#define MUS_CHUNK_TAG_MU01  0x3130554Du    /* 'MU01' little-endian */
#define MUS_NAME_SIZE       16
#define MUS_GLOBALS_BYTES   68             /* Var00..Var15 + 1 user global */
#define MUS_OPCODE_COUNT    65             /* opcodes 0x00..0x40 */
#define MUS_INTRINSIC_NAMES 11             /* full set; runtime may bind a subset */

/* --- On-disk structs --- */

/* SCR0 file-level header. Witnessed: Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20.
   Field offsets confirmed against the load+relocation loop in that function: magic
   compare vs 'SCR0' 0x30524353, chunk-table ptr relocate at +0x0C, name string-blob
   and resolve-table relocate at +0x14/+0x18 (each resolved via AudioVM_FindContextByName). */
typedef struct MusFileHeader {
    uint32_t magic;                       /* 'SCR0' */
    uint32_t version;                     /* 0x00000100; not read by engine */
    uint32_t chunk_count;                 /* # MU01 chunks (typically 1) */
    uint32_t chunk_table_offset;          /* file offset to chunk-pointer table */
    uint32_t name_count;                  /* # intrinsic-method names that follow */
    uint32_t name_strings_blob_offset;    /* Pascal-style length-prefixed strings */
    uint32_t name_resolve_table_offset;   /* uint32 per name; populated at load */
    uint32_t reserved[4];                 /* 16 bytes; never read by engine */
} MusFileHeader;

/* MU01 chunk header. Witnessed: Jointops.exe!AudioVM_FixupPointers @ 0x00672470
   plus Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20. */
typedef struct MusChunkHeader {
    uint32_t tag;                         /* 'MU01'; never validated at runtime */
    uint32_t version;                     /* 0x00000100; not read */
    char     name[MUS_NAME_SIZE];         /* "gamescript" / "menuscript" etc */
    uint32_t globals_size;                /* bytes (typically 0x40 / 0x44) */
    uint32_t locals_size;                 /* bytes (varies; 0 in menumus) */
    uint32_t bytecode_offset;             /* chunk-relative; relocated to ptr */
    uint32_t section_table_offset;        /* chunk-relative; entries also relocated */
    uint32_t section_count;
    uint32_t entry_section_index;         /* index into section table */
    uint32_t debug_info_offset;           /* editor-only; runtime relocates */
    uint32_t debug_info_count;            /* 0 in stripped runtime files */
    uint32_t string_section_offset;       /* editor-only */
    uint32_t string_section_size;         /* observed 0x20 */
    uint32_t aux_table_a_offset;          /* relocated if nonzero */
    uint32_t aux_table_b_offset;          /* relocated if nonzero */
} MusChunkHeader;

#ifdef __cplusplus
static_assert(sizeof(MusFileHeader)  == 44, "MusFileHeader must be 44 bytes");
static_assert(sizeof(MusChunkHeader) == 72, "MusChunkHeader must be 72 bytes");
#endif

/* --- Parsed in-memory representation --- */

#define MUS_SECTION_NAME_SIZE  32         /* longer than MUS_NAME_SIZE to fit
                                              "Multiplayerstart" + NUL */
#define MUS_SOURCE_PATH_SIZE   256
#define MUS_INTRINSIC_NAME_SIZE 32

typedef struct MusSection {
    char     name[MUS_SECTION_NAME_SIZE]; /* from debug export table or "Section_N" */
    uint32_t code_offset;                 /* bytecode-relative PC after parse */
} MusSection;

/* Named global variable declared in the editor-only debug section. The runtime
   addresses globals by byte offset; the names are decompiler aesthetic. */
typedef struct MusVariable {
    char     name[MUS_INTRINSIC_NAME_SIZE];
    uint32_t byte_offset;                 /* offset within globals area */
} MusVariable;

typedef struct MusScript {
    char        name[MUS_NAME_SIZE];
    uint8_t    *code;
    uint32_t    code_size;
    MusSection *sections;
    uint32_t    section_count;
    uint32_t    entry_section_index;
    uint32_t    globals_size;             /* bytes */
    uint32_t    locals_size;              /* bytes */
    /* Locals frame base used by the `enter` opcode: dst = LocalsBase + this.
       Witnessed as instance[+0x3C] (the chunk's string_section_size field) in
       AudioVM_Op_Enter @ 0x672C20; MDEdit invariantly emits 0x20. Parsed from
       the chunk; the compiler defaults it to 0x20. 0 is treated as 0x20. */
    uint32_t    locals_frame_offset;

    /* Editor debug info (string_section / aux tables in the chunk). Empty when
       the chunk was stripped. Witnessed: editor MDEdit writes a 256-byte source
       path followed by an entry table of (offset, name). */
    char         source_path[MUS_SOURCE_PATH_SIZE];
    MusVariable *variables;               /* named globals (offset >= 0 ok) */
    uint32_t     variable_count;
    /* File-level intrinsic-method names, copied here so the decompiler can
       work on a single MusScript pointer per the spec API. */
    char         intrinsic_names[MUS_INTRINSIC_NAMES][MUS_INTRINSIC_NAME_SIZE];
    uint32_t     intrinsic_count;
} MusScript;

typedef struct MusFile {
    MusFileHeader header;
    /* Intrinsic-method name table at file level. MDEdit-authored files always
       have the same 11 names in the same order, so position == method index.
       intrinsic_count is the actual count read from name_count (typically 11). */
    char       intrinsic_names[MUS_INTRINSIC_NAMES][MUS_INTRINSIC_NAME_SIZE];
    uint32_t   intrinsic_count;
    MusScript *scripts;                   /* chunk_count entries */
} MusFile;

/* --- API --- */

/* Validate raw bytes look like an SCR0 header. 0 = OK, negative = error. */
int mus_validate(const uint8_t *data, size_t size);

/* Open from a contiguous buffer. Copies header, chunk data, and intrinsic-name
   table into freshly-malloc'd buffers owned by `out`. Caller retains ownership
   of `data`. 0 on success, negative on error. */
int mus_open_memory(MusFile *out, const uint8_t *data, size_t size);

/* Open by file path. Slurps the file into memory and delegates to
   mus_open_memory. 0 on success, negative on error. */
int mus_open(MusFile *out, const char *path);

/* Free all malloc'd buffers held by `file` and zero the struct. Idempotent. */
void mus_close(MusFile *file);

/* Lookup section by name within a script (synthesized "Section_N" or any
   future name from debug info). NULL on miss or NULL inputs. */
const MusSection *mus_find_section(const MusScript *s, const char *name);

/* Two-pass decompile to MUS source text. Pass `out=NULL, out_capacity=0` to
   query required size (excluding trailing NUL); call again with a buffer at
   least that large to write. Returns bytes written on success, negative on
   error. The output is line-for-line compatible with the reference Python
   decompiler this port was built from (see mus_decompile.cpp).

   The C++ port preserves the Python decompiler's quirks: section bodies are
   bracketed by entry-point labels and `done` opcodes (so a section that ends
   without `done` leaks code into the next section's outer scope), the leading
   "// Decompiled from <path>" line is dropped (we have no filename context),
   and `bind sound_N "sound_N"` is synthesised aesthetic since the runtime
   carries no bind table. */
int mus_decompile(const MusScript *script, char *out_text, size_t out_capacity);

/* Names-aware variant. When `sbf_names` is non-NULL and the play index falls
   inside [0, sbf_name_count), the emitter uses the SBF entry name in place of
   the synthesised "sound_N" placeholder. Useful in editor contexts where the
   bank is loaded alongside the script. Same two-pass query/write contract as
   `mus_decompile`. */
int mus_decompile_with_names(const MusScript *script,
                             const char *const *sbf_names, uint32_t sbf_name_count,
                             char *out_text, size_t out_capacity);

/* --- Compiler --- */

/* Compile MUS source text into a MusScript. The script's malloc'd buffers
   (`code`, `sections`, `variables`) are owned by the caller and freed with
   `mus_script_free`. On success returns 0; on error returns negative and
   populates *err_line/*err_col/*err_msg with diagnostic info (the err_msg
   pointer is to a static string, do not free). */
int mus_compile(const char *text, MusScript *out_script,
                int *err_line, int *err_col, const char **err_msg);

/* Free all malloc'd buffers held by `script` (code, sections, variables).
   Idempotent. The script value itself (if heap-allocated) is NOT freed. */
void mus_script_free(MusScript *script);

/* Encode an array of MusScript into a complete SCR0/MU01 .bin buffer with
   the canonical 11 intrinsic-method names (GEcho, GGRnd, GSV, GSDV, GFB,
   FSet, FClear, FIsSet, FIsClear, TStart, TStop). The returned buffer is
   malloc'd and must be released by the caller via `mus_free`. Returns 0 on
   success. */
int mus_encode_file(const MusScript *const *scripts, uint32_t script_count,
                    uint8_t **out_buf, size_t *out_size);

/* Free a buffer returned by `mus_encode_file`. */
void mus_free(void *p);

/* --- Structural section model (editor-facing, read-only) ---

   A per-section, opcode-level view of a script for editor surfaces (the section
   map / state-machine view and structured editors). Built from the SAME decoded
   instruction stream the decompiler uses (mus_decode.h), but reading the OPCODE
   so it distinguishes the real state transition `setstate` (0x3B, witnessed
   Jointops.exe!VmOp_SetState @ 0x672C70: seeks pc to the target section + fires
   on_section_entered) from the frame-setup `enter` (0x38, VmOp_Enter @ 0x672C20:
   pops N dwords into locals, does NOT change section) -- a distinction the
   decompiled TEXT collapses (both print `enter`). Read-only: builds nothing into
   the bytecode and is independent of the round-trip compile path. Every
   instruction is bound to exactly one owning section by code offset (the section
   with the greatest code_offset <= the instruction's offset), so tail code that
   the text decompiler leaks past a `done` is still attributed to its section. */

typedef enum MusEdgeKind {
    MUS_EDGE_TRANSITION = 0,  /* setstate 0x3B: unconditional move to a section */
    MUS_EDGE_SWITCH     = 1,  /* tablexec 0x35 entry: one branch of a switch */
    MUS_EDGE_BRANCH     = 2,  /* goto/brfalse/brtrue whose target is a section entry */
} MusEdgeKind;

typedef struct MusSectionEdge {
    uint32_t to_section_index;  /* index into script->sections */
    int      kind;              /* MusEdgeKind */
} MusSectionEdge;

typedef struct MusSectionPlay {
    uint32_t track_index;       /* SBF bank entry index from the play/playw operand */
    int      wait;              /* 1 for playw (0x3D), 0 for play (0x3E) */
} MusSectionPlay;

typedef struct MusSectionInfo {
    uint32_t        section_index;  /* index into script->sections */
    int             is_entry;       /* == script->entry_section_index */
    int             is_idle_loop;   /* has >=1 edge and every edge targets itself */
    MusSectionEdge *edges;          /* outgoing edges, deduped by target (TRANSITION wins) */
    uint32_t        edge_count;
    MusSectionPlay *plays;          /* play/playw triggers, in code order */
    uint32_t        play_count;
} MusSectionInfo;

typedef struct MusModel {
    MusSectionInfo *sections;       /* section_count entries, parallel to script->sections */
    uint32_t        section_count;
} MusModel;

/* Build the structural model for a script. Allocates buffers owned by `*out`;
   release via mus_model_free. Returns 0 on success, negative on error (NULL
   inputs). The caller retains ownership of `script`. */
int mus_build_section_model(const MusScript *script, MusModel *out);

/* Free all malloc'd buffers held by `model` and zero it. Idempotent. */
void mus_model_free(MusModel *model);

/* --- VM (interpreter) ---

   Witnessed: Jointops.exe!AudioVM_DispatchLoop @ 0x00672720 (32-instruction budget,
   65-entry dispatch table at Jointops.exe!0x0084F220, two stacks: data EBP-tracked
   and call EDI-tracked). The intrinsic name table @ 0x84F0C8 has 9 entries, ALL
   bound: GEcho, GGRnd, GSV, GSDV, GFB, FSet, FClear, FIsSet, FIsClear
   (@ 0x6720C0/0x672320/0x6720E0/0x672120/0x672150/0x672360/0x672380/0x6723A0/0x6723C0).
   TStart/TStop from the canonical MDEdit set do NOT exist in this build; method
   indices >= 9 resolve to a NULL handler that no-ops + pushes 0. */

typedef struct MusVM MusVM;
typedef enum MusVMState {
    MUS_VM_STOPPED = 0,
    MUS_VM_RUNNING = 1,
    MUS_VM_PAUSED  = 2,
    MUS_VM_HALTED  = 3,
    MUS_VM_ERROR   = 4,
} MusVMState;

/* Embedder-supplied callbacks. All take a void* user pointer the embedder registered
   with `mus_vm_set_hooks`. Any callback may be NULL, in which case the VM
   silently elides the call. Hook signatures match the witnessed Jointops.exe
   side-effects (Phase A revisions, see spec §"libs/mus C API"). */
typedef struct MusVMHooks {
    void *user;
    /* Witnessed: Jointops.exe!VmOp_Play @ 0x672CB0 (0x3E, 1B index)
       and       Jointops.exe!VmOp_PlayWait @ 0x672C90 (0x3D, 2B index).
       wait=1 for playw (0x3D), 0 for play (0x3E). */
    void (*on_play_sound)     (void *user, uint32_t sbf_entry_index, int wait);
    /* Fired when execution enters a section (via setstate or
       mus_vm_jump_to_section). Witnessed: Jointops.exe!VmOp_SetState @ 0x672C70. */
    void (*on_section_entered)(void *user, const char *section_name);
    /* Fired by pop_g (0x08), GSV, GSDV intrinsics whenever a global var slot
       is written. var_index is the byte offset / 4 (Var00..Var15 fit indices
       0..15; user globals at index 16+). */
    void (*on_var_changed)    (void *user, uint8_t var_index, int32_t new_value);
    /* GEcho intrinsic: pops 1 arg, fires this hook with the int32 value.
       Witnessed: Jointops.exe!Intrinsic_GEcho @ 0x6720C0. */
    void (*on_echo)           (void *user, int32_t arg);
    /* GSV / GSDV set master / right-channel volumes in 16.16 fixed point.
       Witnessed: Jointops.exe!Intrinsic_GSV @ 0x6720E0, GSDV @ 0x672120. */
    void (*on_volume_changed) (void *user, int32_t left_16_16, int32_t right_16_16);
} MusVMHooks;

/* Allocate a new VM instance. Returns NULL on OOM. State starts at STOPPED;
   no script is loaded. */
MusVM *mus_vm_create(void);

/* Free the VM. Idempotent on NULL. */
void mus_vm_destroy(MusVM *vm);

/* Replace the embedder hook table. Pass NULL `hooks` to clear all hooks. The
   pointed-to MusVMHooks struct is copied by value; the caller may free it
   immediately after. */
void mus_vm_set_hooks(MusVM *vm, const MusVMHooks *hooks);

/* Bind a parsed MusScript to the VM. Resets the data and call stacks, zeros
   the globals/locals/flags, and seeks pc to
   `script->sections[script->entry_section_index].code_offset`. Returns 0 on
   success, negative on error. The caller retains ownership of `script`; it
   must outlive the VM (or be re-loaded before another tick). */
int mus_vm_load_script(MusVM *vm, const MusScript *script);

/* State transitions. start() requires a loaded script and switches STOPPED ->
   RUNNING. pause() RUNNING -> PAUSED; resume() PAUSED -> RUNNING; stop()
   transitions to STOPPED from any state. */
void mus_vm_start (MusVM *vm);
void mus_vm_stop  (MusVM *vm);
void mus_vm_pause (MusVM *vm);
void mus_vm_resume(MusVM *vm);

/* Advance bytecode for one VM tick. The witnessed dispatch loop spends a
   32-instruction budget, then continues only while the data stack is not
   drained; halt opcodes (play/playw/done/setstate), pc out of bounds, and
   errors still stop earlier. dt_ms is currently informational (returned to
   the embedder) and reserved for future timer support. Returns the dt_ms argument
   on success, 0 if the VM is not RUNNING. */
int mus_vm_tick(MusVM *vm, uint32_t dt_ms);

/* State accessors. */
MusVMState  mus_vm_state          (const MusVM *vm);
const char *mus_vm_last_error     (const MusVM *vm);
const char *mus_vm_current_section(const MusVM *vm);
uint32_t    mus_vm_pc             (const MusVM *vm);

/* Globals area accessors (Var00..Var15 at byte offsets 0..60 = var_index 0..15;
   one user global slot at var_index 16). Out-of-range indices are silent. */
int32_t mus_vm_get_var(const MusVM *vm, uint8_t var_index);
void    mus_vm_set_var(MusVM *vm, uint8_t var_index, int32_t value);

/* Set pc to the named section's code_offset, fires on_section_entered.
   Returns 0 on success, negative on error (NULL inputs / unknown section). */
int mus_vm_jump_to_section(MusVM *vm, const char *section_name);

#ifdef __cplusplus
}
#endif

#endif /* MUS_H */
