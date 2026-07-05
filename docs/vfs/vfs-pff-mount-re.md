# VFS / PFF mount stack — reverse-engineering record (PAR-R7)

The original engine's file-resolution pipeline: the boot PFF mount, the
loose-vs-archive precedence and its `/d` gate, the search-path walk, the PFF
container/entry formats, and the read disciplines. Reimplementation surface:
`libs/vfs` (`vfs.cpp` — the engine-faithful mount stack), `libs/pff`
(container codec), `godot/engine/resource_index/nova_resource_root.cpp`
(`mount_runtime`) and `nova_launch_flags.gd` (`/d`). Binary: retail
**Jointops.exe** (IDB `Jointops.exe.kong.i64`); produced by the PAR-R7 audit
(2026-07-05) that converted this system's `UNAUDITED` ledger row into the
tracked **D-VFS** catalog below. IDB write-back applied in the same session
(the FileSystem_*/PFF_* renames + the `g_FS_*` globals; the old
"SetBypassFlag" name had the polarity backwards).

Write side: the runtime never writes PFFs — the 0x768200–0x768b97 cluster is
mount/lookup/read/close only; every `_lcreat/_lwrite` caller is a loose-file
writer. [ADR 0008](../adr/0008-pff-writer-policy.md) (our editor-side writer)
stands confirmed.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Boot mount (6-slot name table → 16 secondary slots) | **witnessed** | [orig: PFF_OpenAllArchives @ 0x4a4310; table @ 0x829f90 stride 260, count @ 0x82a5a8; FileSystem_SetSecondaryArchive @ 0x75ad40 → slots @ 0x3341818] |
| Resolution order + the `/d` gate | **witnessed — corrects R8's "loose always first"** | default archive-only (`g_FS_SearchLooseFirst @ 0x334180c`, static 0); `/D` sets it once at boot [orig: @ 0x4a7667 → @ 0x4a6fa3 → FileSystem_SetSearchLooseFirst @ 0x75a5a0]; consumers force it per call (§ resolution) |
| The by-name front door (6 functions, ~190 callsites) | **witnessed** | FileSystem_FileExists @ 0x75aa50 / OpenFile @ 0x75b1c0 / GetFileSize @ 0x75b390 / File_LoadResource @ 0x75b540 / ReadFileWithSearchPaths @ 0x75b700 / ReadFileEx @ 0x75b870 — one shared skeleton |
| PFF container open + entry lookup | **witnessed** | PFF_Open @ 0x7682e0; PFF_SortEntries @ 0x768280 (in-place `strupr` + qsort); PFF_FindEntry @ 0x7685d0 (bsearch); no header validation at all |
| Read disciplines (streaming vs whole-file; XOR entries) | **witnessed** | FileSystem_Read @ 0x75abe0 (raw, no decrypt); PFF_ReadFile @ 0x768a30 / PFF_LoadFileToMemory @ 0x768920 (XOR-decrypt on entry flags bit0 via Buffer_XorDecrypt @ 0x768760, seed 0x0312A4CE, ROL 7/byte) |
| Ours vs retail | **5 open rows + 4 permanent candidates** | the D-VFS catalog below |

## The witnessed pipeline

1. **Boot mount.** `Game_InitSubsystems @ 0x4a6cd0` →
   `Expansion_ScanAndRegister @ 0x4a43d0` → `Expansion_LoadAssets @ 0x4a4730`
   → `PFF_OpenAllArchives @ 0x4a4310` over the 6-slot name table
   (`@ 0x829f90`, stride 260): 0 = `expansion\<exp>\<exp>L.pff`,
   1 = `expansion\<exp>\<exp>.pff` (empty for base game), 2 = `language.pff`,
   3 = `localres.pff`, 4 = `resource.pff`, 5 = unused (no writer). Each open
   registers into the 16-slot secondary array (`@ 0x3341818`) at the
   name-table index — **slot order IS lookup precedence**. Only all-failed is
   fatal (earlyerr line 3, check @ 0x4a6f44 — required-resources.md). Archive
   paths open CWD-relative via raw `_lopen` (probe `File_CheckExists
   @ 0x75a5d0`).
2. **Container open** (`PFF_Open @ 0x7682e0`): 188-byte handle; first dword =
   header_size; header read to +132 (magic/count/entry_size/table_offset at
   +136/+140/+144/+148); entries read into a `36*count` buffer; then
   `PFF_SortEntries @ 0x768280` uppercases every entry name IN PLACE (+16)
   and qsorts the 36-byte records by `strcmp`. **No magic/entry_size/count
   validation** (an entry_size > 36 would overflow the buffer). The legacy
   sibling `PFF_OpenLegacyArchive @ 0x7683f0` (16-byte XOR-0xACEDDEAD
   records) has zero callers in JO.
3. **Resolution order** — the shared front-door skeleton:
   `if (!g_FS_SearchLooseFirst && any archive mounted)`: primary (transient;
   normally NULL) → secondary slots 0..15 ascending, first hit wins, miss =
   fail, **loose never consulted**. `else`: each search path
   (`_lopen("path\name")`), then bare `name` CWD-relative, then primary, then
   secondaries. Base game registers ZERO search paths; an expansion registers
   exactly one (`expansion\<exp>`, `FileSystem_AddSearchPath @ 0x75b0a0` —
   16 slots × 16 bytes, unbounded strcpy).
4. **The `/d` gate.** Default 0 (archive-only). `/D` → one boot-time
   `FileSystem_SetSearchLooseFirst(1)` [orig: @ 0x4a6fa3]. Consumers
   save/set/restore around their loads regardless of `/d`: **forced 1** —
   Expansion_ScanAndRegister @ 0x4a446a, Game_TryLoadSavedGame @ 0x4395a2,
   sub_439680 @ 0x439692, Terrain_LoadFoliageFile @ 0x60a74e,
   Mission_LoadEncryptedConfig @ 0x4cdcf4 (gt.ssc),
   CUIImage_LoadTextureFromFile @ 0x6541ba, minimap sub_59B120 @ 0x59b13a;
   **forced 0** — Mission_LoadBMSFromPFF @ 0x40d43c,
   FileSystem_ValidateBMSFile @ 0x40d39c. Texture_LoadByNameWithChannel
   @ 0x58b52c reads the flag directly (a loose TGA under /d skips the .dds
   substitution probe).
5. **Entry lookup** (`PFF_FindEntry @ 0x7685d0`): query strncpy'd to 32
   (truncates at 31 chars), uppercased, trailing-space-trimmed (query only),
   bsearch over the sorted table (plain strcmp). Case-insensitivity: PFF via
   both-sides-uppercase, loose via Win32. Duplicates across archives: lowest
   slot wins; within one archive: unstable qsort + bsearch = unspecified.
   Misses are silent (the +184 report gate is never set).
6. **Reads.** Streaming (FileSystem_Read @ 0x75abe0 / Seek @ 0x75ac40):
   raw `_lread` at the shared handle, cursor at +168, no bounds clamp, NO
   decryption; close is a no-op for archive opens. Whole-file
   (PFF_ReadFile @ 0x768a30 / PFF_LoadFileToMemory @ 0x768920): entry-size
   read, XOR-decrypt when entry flags bit0 (seed 0x0312A4CE, ROL 7/byte).
   PFF_ReadFilePartial @ 0x768ab0 never decrypts; PFF_GetFileSize @ 0x768b40
   returns entry+8. `/FRISK` logs every open to `_filelog.txt`
   (File_SetLoggingEnabled @ 0x75a470).
7. **Remount** (menu expansion switch): Expansion_SwitchTo @ 0x5688c0 —
   LoadAssets → PFF_CloseAllOpenArchives @ 0x4a4380 → OpenAllArchives →
   full asset reload.

## Structures + globals

**PFF handle (188 B):** +0 Win32 handle · +4 path[128] · +132 header_size ·
+136 magic (read, never checked) · +140 entry_count · +144 entry_size
(trusted) · +148 table_offset · +152 sorted entry-table ptr · +156 current
entry · +168 read cursor · +172 open flag · +176 last whole-load size ·
+180 last-lookup-failed (the loop tie-break) · +184 report-errors gate
(never set). **Entry (36 B):** +0 flags (bit0 = XOR-encrypted) · +4 offset ·
+8 size · +12 timestamp (unread) · +16 name[16] (uppercased in place) ·
+32 extra (unread).

**Globals:** `0x33417F8` shared raw handle · `0x3341800` primary ptr
(transient) · `0x3341804` strip-to-basename flag (DEAD — setter @ 0x75a590
unreferenced) · `0x334180C` `g_FS_SearchLooseFirst` (default 0) ·
`0x3341818` secondary slots[16] + count @ 0x3341864 · `0x3341868` search
paths (16×16 B) · `0x33428C0` /FRISK gate · `0x829F90` name table[6][260] ·
`0xB49A54` handles[6] · `0xB4C4D4` /D flag · `0xB4C584` expansion name.

## D-VFS divergence catalog (ours: libs/vfs, libs/pff, NovaResourceRoot)

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-VFS-1 | A | OPEN | Loose gating is session-global in ours (`/d` → PackedWithLooseOverride) vs retail's per-call forces: saves/foliage/gt.ssc/UI-images/minimap force loose-first regardless of /d [orig: @ 0x4395a2, 0x60a74e, 0x4cdcf4, 0x6541ba, 0x59b13a]; BMS-from-PFF forces archive-only even under /d [orig: @ 0x40d43c] |
| D-VFS-2 | A | OPEN | Fixed 6-slot archive name table vs our scan-all-*.pff-alphabetical (coincides on stock installs; extra .pff files mount in ours, never in retail) [orig: @ 0x829f90 + @ 0x4a4310] |
| D-VFS-3 | A | OPEN | Path-qualified names: retail passes queries verbatim (loose probes reach subdirs; archive lookups with a path never match — the basename-strip mode is DEAD, setter @ 0x75a590 unreferenced); our flat_key() strips paths everywhere |
| D-VFS-5 | B | NEEDS-RE | Encrypted-entry (bit0) streaming: retail decrypts ONLY whole-file reads; streaming + partial reads return ciphertext; ours always decrypts — needs the corpus check (does any retail JO pff carry bit0, ever streamed?) |
| D-VFS-7 | A | OPEN (minor) | Name normalization asymmetry: retail trims/truncates the QUERY only (31 chars); ours normalizes both sides — diverges only on pathological names |

**Permanent candidates** (in the ledger's register, awaiting ratification):
D-VFS-4 (snapshot index vs live per-call resolution — host cache),
D-VFS-6 (retail's zero container validation + two write-after-free bugs —
reproducing manufactures garbage, ADR 0003 class), D-VFS-8 (retail's
16-path/16-byte/16-slot caps incl. the >5-char expansion-name overflow —
capacity supersets), D-VFS-9 (`<exp>L.pff` as our persistent primary vs
retail's secondary slot 0 — identical effective precedence, model note).

## Not witnessed

- Forced-flag values at the remaining save/texture sites (@ 0x4ace12,
  @ 0x654b2a, @ 0x654d20, the 0x58xxxx texture loaders' direct writes).
- Whether any retail JO `.pff` entry carries bit0, and streamed (D-VFS-5).
- The +184 report gate's writer (none found; believed always silent) and
  the 0x33428BC load-notify callback's installer.
- Name-table slot 5's writer (none — matches required-resources.md).
- Runtime behavior with entry_size != 36 (asserted from code, not executed).
- Cross-title (dfx2med / demo) mount behavior — out of scope; all addresses
  are retail Jointops.exe.
