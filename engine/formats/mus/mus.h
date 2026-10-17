#pragma once

/* MUS (interactive-music script) container reader.

   SCR0 file header parser witnessed at
       Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20
   MU01 chunk pointer fixup at
       Jointops.exe!AudioVM_FixupPointers @ 0x00672470 */

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

namespace opennova::mus {

/* --- Format constants --- */
inline constexpr uint32_t MUS_MAGIC_SCR0 = 0x30524353u;  /* 'SCR0' little-endian */
inline constexpr uint32_t MUS_CHUNK_TAG_MU01 = 0x3130554Du;  /* 'MU01' little-endian */
inline constexpr int MUS_NAME_SIZE = 16;
inline constexpr int MUS_GLOBALS_BYTES = 68;  /* Var00..Var15 + 1 user global */
inline constexpr int MUS_OPCODE_COUNT = 65;  /* opcodes 0x00..0x40 */
/* A name of the file's method table, its NUL among them: the loader steps from a name to the next by its length
   byte read signed (the byte counts itself, the name and its NUL), so 125 characters is the longest name it steps
   over forward [orig: AudioVM_LoadScriptFile @ 0x672DEB movsx ecx, byte ptr [esi]; @ 0x672DFC add esi, ecx]. */
inline constexpr int MUS_METHOD_NAME_SIZE = 126;

/* The bytecode opcodes referenced by NAME outside the decoder table itself
   (mus_decompile.cpp's kOps stays the full-width authority): the flow ops the
   compiler emits and the decompiler classifies on. [orig: 65-entry dispatch table @ 0x84F220,
   AudioVM_DispatchLoop @ 0x672720] */
typedef enum MusOpcode {
    MUS_OP_GOTO     = 0x30,  /* unconditional branch */
    MUS_OP_TABLEXEC = 0x35,  /* on-switch dispatch */
    MUS_OP_ENTER    = 0x38,  /* frame setup, does NOT move the IP
                                [orig: AudioVM_Op_Enter @ 0x672C20] */
    MUS_OP_SETSTATE = 0x3B,  /* section transition [orig: AudioVM_Op_SetState @ 0x672C70] */
    MUS_OP_PLAYW    = 0x3D,  /* play, a 16-bit sound index (no wait: the handler is the
                                byte form's but for the width; the compiler writes it past
                                index 255) [orig: AudioVM_Op_PlayWait @ 0x672C90] */
    MUS_OP_PLAY     = 0x3E,  /* play, an 8-bit sound index [orig: AudioVM_Op_Play @ 0x672CB0] */
    MUS_OP_DONE     = 0x3F   /* section terminator */
} MusOpcode;

/* Locals frame base the `enter` (0x38) opcode banks caller arguments at:
   dst = base + 4k for the (k+1)-th input. The chunk carries it in the
   string_section_size field (instance[+0x3C]), which the game adds as it is, 0
   a frame at the locals' start [orig: AudioVM_Op_Enter @ 0x672C39 add ebx, [edx+3Ch]].
   MDEdit writes 0x20 in every chunk (MDEdit.exe, its chunk writer sub_40DF30 @
   0x40DF30: v1[15] = 32), and so does the compiler. */
inline constexpr int MUS_DEFAULT_LOCALS_BASE = 0x20;

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
    uint32_t name_count;                  /* # method names (the table; none read when 0) */
    uint32_t name_strings_blob_offset;    /* the names: each a length byte, the name, its NUL */
    uint32_t name_resolve_table_offset;   /* uint32 per name; the loader writes each one's handler */
    uint32_t reserved[4];                 /* 16 bytes; never read by engine */
} MusFileHeader;

/* MU01 chunk header. Witnessed: Jointops.exe!AudioVM_FixupPointers @ 0x00672470
   plus Jointops.exe!AudioVM_LoadScriptFile @ 0x00672D20. */
typedef struct MusChunkHeader {
    uint32_t tag;                         /* 'MU01'; never validated at runtime */
    uint32_t version;                     /* 0x00000100; not read */
    char     name[MUS_NAME_SIZE];         /* "gamescript" / "menuscript" etc */
    /* bytes (0x40 gamemus, 0x44 menumus): the game allocates the globals and the
       locals in one block of the two sizes [orig: ScriptInstance_Init @ 0x672F78]
       and a save keeps as many globals as this holds, up to 32 [orig: AudioVM_SaveScriptState
       @ 0x672583, AudioVM_RestoreScriptState @ 0x6725FB] */
    uint32_t globals_size;
    uint32_t locals_size;                 /* bytes (varies; 0 in menumus) */
    uint32_t bytecode_offset;             /* chunk-relative; relocated to ptr */
    uint32_t section_table_offset;        /* chunk-relative; entries also relocated */
    uint32_t section_count;
    uint32_t entry_section_index;         /* index into section table */
    /* +0x30/+0x34: the script's strings, a table of chunk-relative pointers
       (relocated [orig: AudioVM_FixupPointers @ 0x6724C5..0x6724D8]) that
       `pushstr` (0x07) indexes [orig: AudioVM_Op_PushStr @ 0x672836], then the
       strings; MDEdit writes the table at the code's end (MDEdit.exe sub_40DF30
       @ 0x40DF30: v1[12], v1[13]). No shipped script holds a string (count 0);
       the model holds the count alone (MusScript::string_count). */
    uint32_t strings_offset;
    uint32_t string_count;
    /* +0x38: the editor debug section (the source path, then MDEdit's tables),
       relocated when nonzero [orig: AudioVM_FixupPointers @ 0x672489] and read
       by nothing the game calls (AudioVM_GetScriptSourcePath @ 0x672540, whose
       one caller AudioVM_LoadScriptFile_0 @ 0x673590 has none). */
    uint32_t debug_offset;
    /* +0x3C: the `enter` frame base, added as it is [orig: AudioVM_Op_Enter @
       0x672C39]; 0x20 in every MDEdit chunk. */
    uint32_t frame_base;
    /* +0x40: chunk-relative code pointer of the MessageHandler entry, relocated
       when nonzero [orig: AudioVM_FixupPointers @ 0x672495 vmData[16]]; the
       restart frame jumps the VM here [orig: sub_672E50 @ 0x672eba]. Retail
       gamemus.bin carries 0x91 (`enter 2; method 4; empty; push_l 0x20;
       tablexec 3 [setstate 1 | setstate 2 | setstate 4]`), menumus.bin 0x94
       (= its bytecode start). 0 = no handler. */
    uint32_t message_handler_offset;
    /* +0x44: chunk-relative code pointer, relocated when nonzero
       [orig: AudioVM_FixupPointers @ 0x6724a1 vmData[17]]; both shipped chunks
       carry the bytecode start here (gamemus 0x88, menumus 0x94). No runtime
       reader witnessed. */
    uint32_t main_entry_offset;
} MusChunkHeader;

static_assert(sizeof(MusFileHeader)  == 44, "MusFileHeader must be 44 bytes");
static_assert(sizeof(MusChunkHeader) == 72, "MusChunkHeader must be 72 bytes");

/* --- Parsed in-memory representation --- */

inline constexpr int MUS_SECTION_NAME_SIZE = 32;  /* longer than MUS_NAME_SIZE to fit
                                              "Multiplayerstart" + NUL */
inline constexpr int MUS_SOURCE_PATH_SIZE = 256;
inline constexpr int MUS_GLOBAL_NAME_SIZE = 32;  /* a global's debug name, its NUL among them */

typedef struct MusSection {
    /* the debug table's label; empty where the file names the section none
       (mus_section_label names it for display) */
    char     name[MUS_SECTION_NAME_SIZE];
    uint32_t code_offset;                 /* bytecode-relative PC after parse */
} MusSection;

/* Named global variable declared in the editor-only debug section. The runtime
   addresses globals by byte offset; the names are decompiler aesthetic. */
typedef struct MusVariable {
    char     name[MUS_GLOBAL_NAME_SIZE];
    uint32_t byte_offset;                 /* offset within globals area */
} MusVariable;

/* MDEdit's debug tables beyond the sections and the globals (the editor's; the runtime relocates the region and
   reads none of it): a function's parameter (`Function::name`, its frame offset: base + 4k), a function (its code
   from its `enter` to the end of its body, bytecode-relative) and a line (an instruction's bytecode-relative offset
   and the line of the source text that wrote it). Witnessed in the shipped scripts: gamemus.bin's MessageHandler
   (`MessageHandler::msgtype` at 0x20, `::source` at 0x24, code 0x09..0x1B) and every program's line table, each
   increasing in offset and in line. */
typedef struct MusLocal {
    char     name[MUS_SECTION_NAME_SIZE];
    uint32_t frame_offset;
} MusLocal;

typedef struct MusFunction {
    char     name[MUS_SECTION_NAME_SIZE];
    uint32_t start;   /* its `enter` (0x38) */
    uint32_t end;     /* past its body's last instruction */
} MusFunction;

typedef struct MusLine {
    uint32_t code_offset;
    uint32_t line;
} MusLine;

typedef struct MusScript {
    char        name[MUS_NAME_SIZE];
    uint8_t    *code;
    uint32_t    code_size;
    MusSection *sections;
    uint32_t    section_count;
    uint32_t    entry_section_index;
    uint32_t    globals_size;             /* bytes */
    uint32_t    locals_size;              /* bytes */
    /* Locals frame base used by the `enter` opcode: dst = LocalsBase + this,
       as the chunk holds it (its frame_base field, instance[+0x3C], added as it
       is: 0 is 0 [orig: AudioVM_Op_Enter @ 0x672C39]). MDEdit writes 0x20, and
       so does the compiler (MUS_DEFAULT_LOCALS_BASE). */
    uint32_t    locals_frame_offset;
    /* The MessageHandler entry (bytecode-relative PC) the restart frame jumps
       to, from the chunk's +0x40 pointer; has_message_handler is 0 when the
       chunk carries none. MDEdit always writes one, and so does the compiler:
       its MessageHandler function's frame setup (gamemus.bin 0x09), else the
       code's leading nop (menumus.bin 0). [orig: sub_672E50 @ 0x672e95..0x672ec1] */
    uint32_t    message_handler_offset;
    int         has_message_handler;
    /* The strings the chunk holds (its +0x34 count), which `pushstr` reads and
       the model does not hold: a script holding any is not written
       (mus_encode_file, -6). 0 in every shipped script and compiled one. */
    uint32_t    string_count;

    /* The editor debug section (the chunk's debug_offset): the source path and
       MDEdit's tables, which MDEdit writes into every script from its source
       (MDEdit.exe sub_40DF30 @ 0x40DF30) and the game never reads. Empty when
       the chunk carries none. */
    char         source_path[MUS_SOURCE_PATH_SIZE];
    MusVariable *variables;               /* named globals (offset >= 0 ok) */
    uint32_t     variable_count;
    MusLocal    *locals;                  /* the functions' parameters */
    uint32_t     local_count;
    MusFunction *functions;
    uint32_t     function_count;
    MusLine     *lines;                   /* in code order */
    uint32_t     line_count;
    /* The file's method-name table (a copy of MusFile's, so the decompiler and
       the VM work on one MusScript; malloc'd, freed by mus_script_free): `method
       N` (0x40) calls the handler the game's loader resolved for the N-th name
       [orig: AudioVM_LoadScriptFile @ 0x672DE4 -> AudioVM_FindContextByName @
       0x6723E0; AudioVM_Op_Method @ 0x672CFD], so the table is what the code's
       method numbers mean, in its order. MDEdit's linker writes every method its
       language declares (MUSIC.LAN's eleven, GEcho .. TStop, in every shipped
       script), whichever the script calls (MDEdit.exe sub_40E6E0 @ 0x40E6E0).
       A hand-built script
       that calls a method must hold its table: with none, a `method` reads
       past it (the game calls whatever word it finds there), which the VM
       refuses (MUS_VM_ERROR). */
    char       (*intrinsic_names)[MUS_METHOD_NAME_SIZE];
    uint32_t     intrinsic_count;
} MusScript;

typedef struct MusFile {
    MusFileHeader header;
    /* The method-name table, one per file, every chunk's `method` calls
       resolving through it [orig: ScriptInstance_Init @ 0x672FAF, instance+20 =
       the header's +0x18]: name_count names as the loader reads them (malloc'd,
       freed by mus_close; each script holds a copy). */
    char     (*intrinsic_names)[MUS_METHOD_NAME_SIZE];
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

/* Whether two parsed files hold the same in what the game reads of a script: the method-name
   table it resolves the `method` calls through, and each program's name, code, sections, entry,
   the globals' and locals' sizes, the `enter` frame base, the MessageHandler entry and the count of
   the strings `pushstr` reads (each field's witness above). A save checks the bytes it wrote read
   back as the model so. False for a NULL file. */
bool mus_reads_the_same(const MusFile *a, const MusFile *b);

/* A section's name for display: its debug label, else "Section_N" (N its
   index in the section table) where the file names it none. The game reads no
   label; the decompiler, the VM's section names and the bindings show this. */
std::string mus_section_label(const MusScript *s, uint32_t index);

/* Lookup section by its label (mus_section_label). NULL on miss or NULL inputs. */
const MusSection *mus_find_section(const MusScript *s, const char *name);

/* Two-pass decompile to MUS source text. Pass `out=NULL, out_capacity=0` to
   query required size (excluding trailing NUL); call again with a buffer at
   least that large to write. Returns bytes written on success, negative on
   error. The text is the tool's own authoring syntax (the game reads only the
   binary), built on the reference Python decompiler this port started from
   (see mus_decompile.cpp), and mus_compile writes it back to the program's own
   bytes in MDEdit's layout: a function as `handler NAME(params) { ... }`, the
   debug table's user globals declared, and each statement on the line MDEdit's
   line table names (blank lines before it, `#line N` where the text runs past).

   From the Python decompiler: section bodies are bracketed by entry-point labels
   and `done` opcodes (so a section that ends without `done` leaks code into the
   next section's outer scope), and `bind sound_N "sound_N"` is synthesised
   aesthetic since the runtime carries no bind table.

   A malformed program does not decompile (negative): an instruction cut short by
   the code's end, a branch (goto, brfalse, brtrue, callv) into an instruction or past
   the end, or ifs nested more than 64 deep. A brfalse to its own offset or before it
   is no if; it prints as its `// if !(...) goto` comment. */
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
   populates err_line, err_col and err_msg with diagnostic info (the err_msg
   pointer is to a static string, do not free). */
int mus_compile(const char *text, MusScript *out_script,
                int *err_line, int *err_col, const char **err_msg);

/* A sound a `play` names (a statement's, or an entry of an `on (...) play` table): the byte offset and the
   length of its name in the text (a quoted name's without its quotes) and the sound index it compiles to, the
   stream of the script's bank the game plays by that index [orig: AudioVM_Op_Play @ 0x672CB0, AudioVM_Op_PlayWait
   @ 0x672C90 -> AudioVM_StartSound @ 0x671FF0]. */
struct MusPlayUse {
    uint32_t offset;
    uint32_t length;
    uint32_t index;
};
/* mus_compile, and the plays the text names in `plays`, in the text's order (none where the text does not
   compile). */
int mus_compile_plays(const char *text, MusScript *out_script, std::vector<MusPlayUse> *plays,
                      int *err_line, int *err_col, const char **err_msg);

/* The music bank a script plays from: the script's file name, its folders stripped, with its extension made
   .SBF, upper case (the upper case is ours, a name to compare without case: the game sprintf's an expansion's
   name in the case it is given). The game opens the two as a pair: its own GAMEMUS.BIN with GAMEMUS.SBF and
   MENUMUS.BIN with MENUMUS.SBF [orig: the names @ 0x7C8D50..0x7C8D74, copied into g_PathGameBin/Sbf and
   g_PathMenuBin/Sbf by Expansion_LoadAssets @ 0x4A478D..0x4A480D], an expansion's G<name>.bin with G<name>.sbf
   and M<name>.bin with M<name>.sbf [orig: Expansion_LoadAssets @ 0x4A4906..0x4A494A]; the menu pair opened
   together by AudioVM_InitMenuMusicStreaming @ 0x56AA6E..0x56AA78 and the game pair by Game_StartMission @
   0x52558E..0x525593 [orig: both -> AudioVM_OpenMusicContext @ 0x6722A0]. Where the bank lies is the caller's to
   know: an expansion's is a loose file of its folder, expansion\<name>\G<name>.sbf (the sprintf's path @
   0x4A4936, @ 0x4A4906), whatever archive its script comes from; the base game's beside the install's archives. */
std::string mus_bank_name(const std::string &script);

/* Free all malloc'd buffers held by `script` (code, sections, variables).
   Idempotent. The script value itself (if heap-allocated) is NOT freed. */
void mus_script_free(MusScript *script);

/* Encode an array of MusScript into a complete SCR0/MU01 .bin buffer, each
   chunk's fields as its script holds them (the globals' size, the frame base)
   and the method-name table the scripts hold, which the file holds once: the
   scripts written together carry the same table, or nothing is written (-4).
   A name not ended within its MUS_METHOD_NAME_SIZE slot is refused (-5): its
   length byte would pass 127, and the loader, stepping by it read signed,
   would read the names after it from elsewhere [orig: AudioVM_LoadScriptFile @
   0x672DEB..0x672DFC]. Nothing else of the table or the sizes is refused: the
   loader takes any [orig: AudioVM_LoadScriptFile @ 0x672D20; ScriptInstance_Init
   @ 0x672EF0]. A script holding strings (string_count) is refused (-6): the
   game's `pushstr` reads them [orig: AudioVM_Op_PushStr @ 0x672836], and the
   model holds their count alone, so the writer cannot write them. A script that
   calls a method needs its table (MusScript::intrinsic_names): a hand-built one
   written with none calls past the table the game reads. The returned buffer is malloc'd and must be released by the
   caller via `mus_free`. Returns 0 on success. */
int mus_encode_file(const MusScript *const *scripts, uint32_t script_count,
                    uint8_t **out_buf, size_t *out_size);

/* Free a buffer returned by `mus_encode_file`. */
void mus_free(void *p);

/* --- VM (interpreter) ---

   Witnessed: Jointops.exe!AudioVM_DispatchLoop @ 0x00672720 (32-instruction budget,
   65-entry dispatch table at Jointops.exe!0x0084F220, two stacks: data EBP-tracked
   and call EDI-tracked). `method N` calls the handler the loader resolved for
   the script's N-th method name, by name, against the game's registered table @
   0x84F0C8 (36-byte records: GEcho, GGRnd, GSV, GSDV, GFB, FSet, FClear, FIsSet,
   FIsClear @ 0x6720C0/0x672320/0x6720E0/0x672120/0x672150/0x672360/0x672380/
   0x6723A0/0x6723C0), of which the resolver walks the first 8 (the count @
   0x84F20C): FIsClear, TStart, TStop and any other name resolve to nothing, a
   call of which pushes 0. A number past the script's table reads past its
   resolve table [orig: AudioVM_Op_Method @ 0x672CFD], which the VM refuses
   (MUS_VM_ERROR). */

typedef struct MusVM MusVM;
struct MusGlobals;
// Compile-time access to the active context's actual globals, with lifetime retained
// independently of the streaming VM. Raw writes do not signal AudioVM variable hooks.
// [orig: sub_671FD0 @0x671FD0; WacScript_ResolveParameter @0x4F2A17..0x4F2A34]
std::shared_ptr<MusGlobals> mus_vm_globals(MusVM *vm);
int32_t mus_globals_read(const MusGlobals &globals, uint32_t index);
void mus_globals_write_raw(MusGlobals &globals, uint32_t index, int32_t value);
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
   side-effects (Phase A revisions, see spec §"engine/formats/mus C API"). */
typedef struct MusVMHooks {
    void *user;
    /* Witnessed: Jointops.exe!AudioVM_Op_Play @ 0x672CB0 (0x3E, 1B index)
       and       Jointops.exe!AudioVM_Op_PlayWait @ 0x672C90 (0x3D, 2B index).
       wait=1 for playw (0x3D), 0 for play (0x3E). */
    void (*on_play_sound)     (void *user, uint32_t sbf_entry_index, int wait);
    /* Fired when execution enters a section (via setstate or
       mus_vm_jump_to_section). Witnessed: Jointops.exe!AudioVM_Op_SetState @ 0x672C70. */
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
   `script->sections[script->entry_section_index].code_offset`; resolves the
   script's method names (intrinsic_names) as the game's loader does. Returns 0
   on success, negative on error. The caller retains ownership of `script`; it
   must outlive the VM (or be re-loaded before another tick). */
int mus_vm_load_script(MusVM *vm, const MusScript *script);

/* State transitions. start() requires a loaded script and switches STOPPED ->
   RUNNING. pause() RUNNING -> PAUSED; resume() PAUSED -> RUNNING; stop()
   transitions to STOPPED from any state. */
void mus_vm_start (MusVM *vm);
void mus_vm_stop  (MusVM *vm);
/* Drop the borrowed script pointer (the owner is about to free it); the VM
   parks STOPPED with no program until the next mus_vm_load_script. */
void mus_vm_unload_script(MusVM *vm);
void mus_vm_pause (MusVM *vm);
void mus_vm_resume(MusVM *vm);

/* Advance bytecode for one VM tick. The witnessed dispatch loop spends a
   32-instruction budget, then continues only while the data stack is not
   drained; halt opcodes (play/playw/done/setstate), pc out of bounds, and
   errors still stop earlier. dt_ms is currently informational (returned to
   the embedder) and reserved for future timer support. Returns the dt_ms argument
   on success, 0 if the VM is not RUNNING. */
int mus_vm_tick(MusVM *vm, uint32_t dt_ms);

/* The restart frame [orig: sub_672E50 @ 0x672e95..0x672ec1 -- the step the
   per-update pump (sub_672EE0) and MusicCtx_SelectEndTrack @ 0x672fd0 share]:
   when the context's restart byte (ctx+32) is set the step clears it, pushes
   the current IP on the return stack, pushes (value, 0) on the data stack,
   sets IP = the chunk's +0x40 MessageHandler pointer and runs the dispatch
   loop. MusicCtx_SelectEndTrack(value) sets the byte and steps at once
   whenever a script is loaded, so the flag never outlives the call; this
   entry point is that pair. Server_ProcessRoundEnd's single-player tail
   calls it with 1 (win, after Cine_InitPlayback @ 0x51696b) and 2 (lose,
   after Cine_StartPlayback @ 0x51698f); retail gamemus.bin's handler
   dispatches 1 -> Missionwin, 2 -> Missionlose, 0 -> the idle section.
   Runs regardless of the RUNNING/STOPPED state (retail's step ignores the
   streaming-active flag; it needs only a loaded script). Returns 0 when the
   frame ran, -1 with no script, -2 when the script carries no handler
   (retail would read through a null chunk pointer), -3 on a return-stack
   overflow. */
int mus_vm_signal(MusVM *vm, int32_t value);

/* State accessors. */
MusVMState  mus_vm_state          (const MusVM *vm);
const char *mus_vm_last_error     (const MusVM *vm);
const char *mus_vm_current_section(const MusVM *vm);
uint32_t    mus_vm_pc             (const MusVM *vm);
/* Return-stack depth (the interrupted PCs `enter`-frames and the restart
   frame push; `return` pops). */
int         mus_vm_call_depth     (const MusVM *vm);

/* Globals area accessors (Var00..Var15 at byte offsets 0..60 = var_index 0..15;
   one user global slot at var_index 16). Out-of-range indices are silent. */
int32_t mus_vm_get_var(const MusVM *vm, uint8_t var_index);
void    mus_vm_set_var(MusVM *vm, uint8_t var_index, int32_t value);

/* Set pc to the named section's code_offset, fires on_section_entered.
   Returns 0 on success, negative on error (NULL inputs / unknown section). */
int mus_vm_jump_to_section(MusVM *vm, const char *section_name);

} // namespace opennova::mus
