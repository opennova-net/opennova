# MUS / SBF / SCR — reverse-engineering record

Validation record for the music subsystem (`libs/mus`, `libs/sbf`, `libs/scr`,
`godot/engine/audio`) against the original engine as witnessed in IDA Pro.
Binary: retail **Jointops.exe** (IDB `Jointops.exe.kong.i64`). All addresses
below are that binary's. This file is the committed home for the divergence
catalog that code comments cite as `docs/audio/mus-sbf-re.md (D-…)`.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| MUS VM (`libs/mus/src/mus_vm.cpp`) | **MATCHING** (behavioral proof) | 52 inline citations (dispatch `@0x672720`, 65 opcodes, intrinsics); golden event-stream ctest `mus_vm` reproduces the original handlers' output byte-identically for gamemus/menumus across 5 var scenarios; local Unicorn differential (see "RE tooling" below) |
| MUS compiler write path | **MATCHING** (read-only grill) | entry-index round-trip + operand-width checks (`mus_entry_roundtrip`, `mus_encode_idempotence`, 25-case `mus_authored_behavioral`) |
| SBF bank codec (`libs/sbf`) | **MATCHING** | 12 citations: `Sbf_OpenFile_Gamemus @0x4ED6C0`, `Sbf_StartEntry @0x4ED910`, `Audio_StreamNextChunk @0x4ED7D0`, mix coefficients `@0x7BD4B0`; `sbf_roundtrip` et al. |
| SCR container codec (`libs/scr`) | **MATCHING** (grilled 2026-06-09) | keystream + reverse pass byte-exact vs `Scr_DecryptBuffer @0x53D090`; two documented policy divergences (D-SCR-1/2 below) |
| PFF entry encryption | **MATCHING** (pre-existing) | flag bit 0 + rol-7 XOR keystream vs `PFF_LoadFileToMemory @0x768920`, cited in `libs/pff` |
| `godot/engine/audio` glue | host code, **not grillable**; pacing now witnessed | hook map cited in `nova_music_director.cpp` (`AudioVM_LoadScriptFile @0x672D20`, `VmOp_Play @0x672CB0`, `VmOp_SetState @0x672C70`, `Intrinsic_GSV @0x6720E0`, `GEcho @0x6720C0`); bus routing is Godot-idiomatic; the VM-advance **pacing** is grilled below (the golden tests prove the opcode stream, not real-time pacing) |

## AudioVM playback pacing — the VM advances on track completion (grilled 2026-06-15)

The MUS VM is **not** stepped per frame; the original advances it only when the
currently-playing track finishes streaming. `audio_stream_update @ 0x671c60` (the
per-update streaming pump) is:

```
if (g_audiovm_context_active) {
    if (remaining_bytes (dword_31C37EC) <= 0)   // current track drained
        AudioVM_StepScript();                    // sub_672EE0 -> sub_672E50 -> AudioVM_DispatchLoop @0x672720
    else if (Audio_GetPendingBufferCount() < 7)  // else keep streaming this track
        { ReadFile next chunk; submit; dword_31C37EC -= chunk_bytes; }
}
```

`remaining_bytes` is seeded from the entry's `total_size` in `sub_671BC0` (the context
selector, which `SetFilePointer`s back to the entry's `data_offset` — so a fresh
`VmOp_Play`/`AudioVM_StartSound @0x671ff0` **restarts** the stream from the start; it is not
idempotent). A `play` op halts the dispatch loop (STC), so each step runs to exactly one
`play`. Net: **one `play` per track completion** — the script plays a track, waits for it
to finish, then steps to the next `play`/`setstate`. Section transitions therefore land on
track boundaries, not every frame.

**Divergence D-MUS-PACE / fix.** `NovaMusicDirector` previously called `mus_vm_tick` every
`_process` frame, so the VM raced through `play`/`setstate` (~20 plays + ~40 section
changes per second measured) and `_on_play_sound` re-`play()`'d the `NovaSbfAudioStream`
every frame — a constant low buzz. The director now gates VM advance on the active track
still playing (`_active_play->is_playing()`), reproducing the
`remaining_bytes <= 0 -> step` rule. We pace on the Godot `AudioStreamPlayer` finishing
rather than a byte counter (host-idiomatic), and stream one music context at a time as the
original does.

## Music state variable selection — the host sets the section discriminator (grilled 2026-06-15)

The MUS scripts are var-driven state machines: a "discriminator" global selects which
section/track loop plays, and its var **index is per-script** (golden test
`tests/mus/mus_vm_test.cpp` `test_vm_jo_behavioral`):
- **gamemus → var index 1** (`var1=0` → `Multiplayerstart` loops `P0`; `var1=1` → silent
  `Missionnull`).
- **menumus → var index 2** (`var2=0` → `P1 P2` then a `P0` loop; `var2=1` → `P1 P2` then the
  `P2..P8` theme loop; `var2=2` → `P2`+volume loop).

The original starts each context with the var at 0: `AudioVM_InitMenuMusicStreaming @0x56aa60`
opens the menumus context (`g_path_menu_sbf` + `g_path_menu_bin` via
`AudioVM_OpenMusicContext @0x6722a0`) and sets volume only — **no initial var**. The selecting
var is set later by the host when a `.mnu` screen is shown (its `MUSICVAR` → the discriminator
var). The JO main-menu screen `STARTUP` has `MUSICVAR=1`, selecting the menumus `var2=1` theme.

### Divergence D-MUS-VAR / fix

| ID | Ours | Original | Why / consequence |
| --- | --- | --- | --- |
| D-MUS-VAR | the runtime pushed the screen `MUSICVAR` to var **index 0** (`menu_shell.gd MUSIC_VAR_INDEX` / `nova_mnu_menu.cpp music_var_index_`), so menumus' `var2` stayed 0 | the host sets the discriminator var the script actually reads (menumus `var2`, gamemus `var1`) | at index 0 the screen `MUSICVAR` was inert; the menu always ran the `var2=0` path (`P1,P2` then a `P0` loop) instead of the screen's `MUSICVAR=1` theme (`P2..P8`). Fixed: `MUSIC_VAR_INDEX = 2`; the shell pushes it synchronously in `setup()` (before the director's first `_process` tick) so the VM starts in the selected section. Mission-driven gamemus `var1` transitions remain a follow-up. |

## SCR container codec — witness map (grill of 2026-06-09)

Jointops.exe has exactly **two** SCR decrypt sites, both delegating to one
codec routine:

- **Codec** — `Scr_DecryptBuffer @ 0x53D090` (renamed this session from the
  misleading auto-name `Network_DecryptBuffer`): byte-reverse the whole
  buffer (`@0x53D0B3`), then per byte
  `key = ROL32(key + ROL32(key, 11), 4) ^ 1; *p ^= (uint8)key` (`@0x53D0D3`).
  `libs/scr` `scr_decrypt` is a byte-exact structural translation.
- **Site 1, .def text data** — `File_ParseASCIIFile @ 0x53D810`, sniff
  `@0x53D899`: decrypts only when the caller passed a nonzero key AND the
  header is exactly `'S','C','R',0x01`. Otherwise the buffer parses as
  plaintext. All 27 Jointops call sites pass key `0x2A5A8EAD`.
- **Site 2, HLSL .fx effects** — `ScriptFile_LoadAndDecrypt @ 0x5AE060`,
  sniff `@0x5AE0A9`: header must be exactly `'SCR',0x01`; key hardcoded
  `0xA55B1EED` (`@0x5AE0C0`); non-SCR input is rejected (NULL); one trailing
  NUL is trimmed after decrypt (call-site convenience, not codec).
- Key `0xABEEFACE` (`SCR_KEY_DEFAULT`) does **not** appear in Jointops.exe.
  It is an earlier-title key (JO demo era) kept for cross-title tooling.

### Divergences

| ID | Ours | Original (Jointops.exe) | Why / consequence |
| --- | --- | --- | --- |
| D-SCR-1 | `scr_is_scr` accepts version byte 0–2 | both sniff sites require version byte == 1 exactly | deliberate multi-title superset so one codec serves JO-demo-era and shader containers. Load-bearing equivalence holds: plaintext MUS (`"SCR0"`, version byte 0x30) is rejected by both implementations and passes through undecoded |
| D-SCR-2 | key chosen from the version byte (1→`0x2A5A8EAD`, 2→`0xA55B1EED`, else `0xABEEFACE`), overridable via `VFS_SCR_FORCE_*` policy | key is fixed per call site; the version byte is always 1 in retail data | the original encodes file-kind in the call site, we encode it in data + policy. For everything retail JO ships through the text parser the two agree (`SCR\x01` → `0x2A5A8EAD`). An `.fx` payload routed through our VFS decode would get the wrong key — `.fx` does not flow through VFS decode; the `FORCE_SHADERS` policy exists for that caller when it arrives |

## MUS on-disk forms (corrected by this grill)

The original **music path never invokes the SCR container codec.**
`AudioVM_LoadScriptFile @ 0x672D20` requires the buffer returned by
`File_LoadResource @ 0x75B540` to begin with plaintext `"SCR0"`
(dword `0x30524353`, gate `@0x672D73` — this is the MUS file's own magic,
not a container header). Upstream of that gate there are exactly two byte
sources:

1. **Loose file** — `File_LoadEntireFile @ 0x75A780`: raw read, no transform.
2. **PFF entry** — `PFF_LoadFileToMemory @ 0x768920`: if the directory
   entry's flag bit 0 is set, the payload is XOR-decrypted with the rol-7
   keystream. This — not an SCR variant — is how retail encrypted
   gamemus/menumus ship. Implemented and cited in `libs/pff`
   (`PFF_FLAG_ENCRYPTED`).

So the supported forms are: (a) plaintext `SCR0` (loose, or PFF entry without
the flag — JO_CLIENT `localres.pff`), and (b) PFF-entry-encrypted (flag bit 0,
transparent at the PFF layer). Our VFS additionally tolerates an
`SCR\x01`-wrapped payload as a convenience superset (see D-SCR-1/2). The
"headerless SCR-style cipher" experimented with during PR #53 is intentionally
**out of scope**: without an SCR or PFF marker it is opaque bytes
(`tests/vfs/vfs_mus_decode_test.cpp` pins the pass-through).

## MUS VM divergence catalog (D-MUS)

The original catalog file (`notes/audio/sbf-mus-format.md`, untracked) was
lost; this list is regenerated from the surviving code markers and their
regression tests (`tests/mus/mus_vm_fixes_test.cpp`). IDs keep their original
numbers; gaps (D-MUS-1/4/8) were resolved fixes whose notes did not survive —
their behavior is locked by the golden event-stream test either way.

| ID | Status | Summary |
| --- | --- | --- |
| D-MUS-2 | fixed | `0x07 pushstr`, `0x0A pop_global_block`, `0x0B pop_local_block` operand widths now match the original; the reimpl compiler never emits them, so this only affects running real `.mus` data (`mus_vm.cpp:401`) |
| D-MUS-3 | fixed | `empty (0x0F)` drains the full stack |
| D-MUS-5 | intentional mirror | `inc_g`/`dec_g (0x11/0x12)` operate on **1 byte** at the globals offset and do not raise the globals-dirty notify — matching the original's silence (`mus_vm.cpp:333`) |
| D-MUS-6 | fixed (a.k.a. D-NEW-2) | `enter (0x38)` pops N dwords and applies the frame offset |
| D-MUS-7 | intentional mirror | `op_callvl (0x0A call form)` resolved-name table is uninitialised BSS in Jointops, so the opcode is dead; reimpl mirrors the dead-stub (push 0). MDEdit scripts use opcode `0x40 method` instead (`mus_vm.cpp:681`) |
| D-MUS-9 | fixed | `FIsClear` true/false semantics |
| D-MUS-10 | fixed | `GGRnd` behavior |

## RE tooling (not in repo)

The Unicorn differential harness (`mus_diff_jointops.py`) ran the same script
corpus through the original Jointops handlers and our VM: original == reimpl
for 5 stock + 8 authored scripts. The script itself was session-local RE
tooling and is not tracked; the committed `mus_vm` golden event-stream test
and `mus_authored_behavioral` lock the same behavior in CI without the binary.

## IDB changes made during the 2026-06-09 SCR grill

- Renamed `Network_DecryptBuffer` → `Scr_DecryptBuffer` (`0x53D090`) — its
  only callers are the .def text parser and the .fx loader; nothing network.
- Comments added at `0x53D090`, `0x53D899` (sniff #1), `0x5AE0A9` (sniff #2 —
  also corrects an older comment that claimed key `0xA55AA56D`; the immediate
  is `0xA55B1EED`), and `0x672D73` (music-path plaintext gate).
