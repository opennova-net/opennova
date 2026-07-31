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
| `godot/engine/audio` glue | reimpl code, **not grillable**; pacing now witnessed | hook map cited in `nova_music_director.cpp` (`AudioVM_LoadScriptFile @0x672D20`, `VmOp_Play @0x672CB0`, `VmOp_SetState @0x672C70`, `Intrinsic_GSV @0x6720E0`, `GEcho @0x6720C0`); bus routing is Godot-idiomatic; the VM-advance **pacing** is grilled below (the golden tests prove the opcode stream, not real-time pacing) |

## AudioVM playback pacing — the VM advances on track completion (grilled 2026-06-15; re-verified 2026-07-11)

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
rather than a byte counter (reimpl-idiomatic), and stream one music context at a time as the
original does.

Related reimpl note (2026-07-09; rate witness corrected 2026-07-11): `NovaSbfAudioStreamPlayback`
extends Godot's `AudioStreamPlaybackResampled` and reports the SBF content rate (22050 Hz), so
the mixer resamples to the device rate. The original's audio service runs at a **44100 Hz
device rate** — one-shot wavs carry a device-relative pitch ratio
`(nSamplesPerSec << 16 + 22050) / 44100` (the +22050 is the rounding half-add)
[orig: Audio_LoadWavFileFromArchive @ 0x766735], so 22050-content wavs play at ratio 0.5 —
and the music stream pump submits split L/R buffers at queue pitch 0x10000 through an inline
ADPCM-decode/interpolate stage (`play_stereo_sample @ 0x7bcf95`, stepper
`Audio_AdpcmDecodeNibbleStep @ 0x7bf250`, ex kong "noop_stub"), netting the same 22050 Hz
content rate in real time. The decoded PCM itself is pinned byte-exact by the `sbf_roundtrip`
golden tests; the prior reimpl 1:1 frame mapping played SBF content at half speed on the 44100 Hz
mix rate.

## Music state variable selection — the shell sets the section discriminator (grilled 2026-06-15; re-verified 2026-07-11)

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
var is set later by the shell when a `.mnu` screen is shown (its `MUSICVAR` → the discriminator
var). The JO main-menu screen `STARTUP` has `MUSICVAR=1`, selecting the menumus `var2=1` theme.

### Divergence D-MUS-VAR / fix

| ID | Ours | Original | Why / consequence |
| --- | --- | --- | --- |
| D-MUS-VAR | the runtime pushed the screen `MUSICVAR` to var **index 0** (`nova_menu_shell.gd MUSIC_VAR_INDEX` / `nova_mnu_menu.cpp music_var_index_`), so menumus' `var2` stayed 0 | the host sets the discriminator var the script actually reads (menumus `var2`, gamemus `var1`) | at index 0 the screen `MUSICVAR` was inert; the menu always ran the `var2=0` path (`P1,P2` then a `P0` loop) instead of the screen's `MUSICVAR=1` theme (`P2..P8`). Fixed: `MUSIC_VAR_INDEX = 2`; the shell pushes it synchronously in `setup()` (before the director's first `_process` tick) so the VM starts in the selected section. gamemus `var1` is never driven in retail — see "Game music driving" below. |

## Game music driving — the full retail writer map (witnessed 2026-07-09; re-verified 2026-07-11)

How the original drives the gamemus context in-mission. Every retail write of the
music VM globals goes through `AudioVM_SetVariable @ 0x671fa0`
(`g_audiovm_globals[idx] = val`); the COMPLETE caller set is the five functions
below (exhaustive xref sweep of 0x671fa0).

### The music pair names and the expansion reselect (witnessed 2026-07-09; landed 2026-07-11)

`Expansion_LoadAssets` builds the (bank .sbf, script .bin) pair paths once at
expansion (re)load:

- Base names are constants: `GAMEMUS.BIN/SBF`, `MENUMUS.BIN/SBF`
  (const table `@ 0x7C8D50`, copied into `g_path_menu_sbf/bin`,
  `g_path_game_sbf/bin` `@ 0x4a4798-0x4a4801`). They are VFS basenames — the
  `.bin` resolves from PFF archives; the `.sbf` streams loose.
- The ONLY reselect is the expansion `.pff` existence check:
  `File_CheckExists("expansion\\<n>\\<n>.pff") @ 0x4a4767`; missing → the
  expansion name clears (`@ 0x4a4775`) and the base names stand. Once the
  `.pff` exists, the expansion music paths are set **unconditionally** — bank
  `expansion\<n>\M<n>.sbf` / `expansion\<n>\G<n>.sbf` as loose paths
  (`@ 0x4a4906 / 0x4a4936`), script `M<n>.bin` / `G<n>.bin` as VFS basenames
  (`@ 0x4a491d / 0x4a494a`) — with **no probe of the music files themselves**.
  An expansion that ships partial or no music is therefore SILENT in retail:
  the context open bails at the bank's CreateFileA
  (`AudioVM_OpenContextFile @ 0x672160`) before loading the script.
- The same pass sets the expansion LWF bank-slot names (`<n>L.lwf`
  `@ 0x4a4989`, `<n>.lwf` `@ 0x4a495e` — slots 0/1 of the six-slot table, see
  docs/audio/lwf-dbf-sound-re.md).

### Closed divergence D-MUS-PAIRGRACE (2026-07-22)

| ID | Ours | Original | Closure |
| --- | --- | --- | --- |
| D-MUS-PAIRGRACE | `NovaMusicService.resolve_music_pair` now sets the expansion bank and script names unconditionally once the expansion is mounted; an incomplete pair fails during the normal bank/script open and remains silent | the `.pff` exists-check is the only reselect; a partial-music expansion is silent (`@ 0x4a4767/0x4a4775`) | **FIXED.** Removed the completeness probe and covered missing-bank, missing-script, and musicless expansions with regressions. Halves still always come from the same stem. |

### Context lifecycle

- Menu context: opened once at boot — `AudioVM_InitMenuMusicStreaming @ 0x56aa60`
  calls `AudioVM_OpenMusicContext(g_path_menu_sbf, g_path_menu_bin, "music")` +
  `AudioVM_SetGlobalVolume` (no initial var).
- Game context: `Game_StartMission @ 0x524360` at `@ 0x525581` tests
  `g_napi_np_ctx.is_mp_session_peer`: MP peer →
  `AudioVM_OpenMusicContext(g_path_game_sbf, g_path_game_bin, "music")` +
  `AudioVM_SetGlobalVolume` (`@ 0x525589-0x5255a4`); NOT a peer →
  `AudioVM_StopMusicContext @ 0x671e00` (`@ 0x5255ae`) — retail single-player
  plays **no front-end music in-mission at all** (the stop also kills the menu
  context that was still streaming).
- Var seeding then runs on BOTH branches (`@ 0x5255b3-0x52561b+`):
  `Var1 = dword_A762E0`, `Var2..Var6 = 0`, `Var7 = 100`, `Var8..Var12 = 0`.
- `AudioVM_SetGlobalVolume @ 0x671f20` clamps [0,255] and stores `vol << 16`
  (8.16); the streamer applies `(word_31C37FE * word_31C3802) >> 8` per update
  (`@ 0x671cef`).

### Var1 is ALWAYS 0 — retail JO's in-game music is one MP loop

`dword_A762E0` (the Var1 seed) has **no writers anywhere in Jointops.exe** (its
single xref is the seed read `@ 0x5255b3`), so gamemus always enters its
`var1=0` path: the `Multiplayerstart` section looping track `P0`. The
`Missionnull`/`Missionwin`/`Missionlose` sections in the shipped gamemus.bin are
**unreachable dead content** (BHD-era mission-music machinery) — retail JO has
no win/lose stings and no mission-state music transitions. Do not invent them.

### Per-frame var writes (local player only)

`Entity_UpdateInfantryPlayerBody @ 0x4b40e0`, gated
`entity == g_local_player_entity` (`@ 0x4b6234`; a second gate `@ 0x4b635b`):

| Var | Value | Witness |
| --- | --- | --- |
| Var5 | distance to the nearest threat in whole units + 1 (`sqrt(dpos²) >> 16 + 1`), 0 when none. Threat = `Entity_FindNearestThreat @ 0x4b0990` (ex kong "Entity_SpawnProjectile" — it SEARCHES via `Entity_FindTargets @ 0x53a610`, range `min(fog_dist/2, 40u)`, and on authority side-writes spotted/enemy relation bits) called with `Env_FogDistCurrent` | `@ 0x4b6240-0x4b62a9` |
| Var6 | that threat's current target is the local player (bool) | `@ 0x4b62b3-0x4b62c9` |
| Var2 | local-player view pitch | `@ 0x4b62d8` |
| Var3 / Var4 | body-update stack args (orientation/state; low confidence) | `@ 0x4b62e4 / 0x4b62f0` |
| Var10 | local-player team | `@ 0x4b62fc` |
| Var7 | health % = `cur*100/max` (`Entity_GetMaxHealthWithDifficulty @ 0x43b8a0`), clamped 100 | `@ 0x4b6324` |
| Var8 | `g_scoreGameType` (`dword_24C1970`) — the match's scoring game TYPE, not a dynamic state | `@ 0x4b6335` |
| Var7/Var8 (alt path) | re-written on a second local-player-gated path (Var7 source low confidence) | `@ 0x4b6366 / 0x4b6374` |

### Divergence D-MUS-VARPUMP

| ID | Ours | Original | Why / consequence |
| --- | --- | --- | --- |
| D-MUS-VARPUMP | `GameWorld._music_var_pump` re-drives Var7 (health %, the exact `max > cur ? cur*100/max : 100` form `@ 0x4b6313-0x4b6324`) and Var10 (team) per frame; Var2/Var3/Var4/Var5/Var6/Var8 are witnessed but unpumped | all eight per-frame writes above | none for retail JO content: the shipped gamemus reads ONLY its var1 discriminator (always 0), so every per-frame var is inert. The unpumped set needs engine pieces we haven't ported (Var5/6: `Entity_FindNearestThreat @ 0x4b0990`; Var2: raw engine angle units; Var8: retail scoring-mode ids). Close by pumping them when a consumer (custom gamemus content) materializes. |

Menu-side writers (menuscript context): `UI_DispatchScreenEvent` stores the
active screen's `MUSICVAR` to Var2 on every screen event (`@ 0x54eff4`) — this
fires for in-game menus too; while gamemus runs, the per-frame ViewPitch write
overwrites it next frame, so pause/resume needs no special music handling.
Session teardown writes `Var10 = (reason==1 ? 2 : 1)` when not in session
(`sub_568460 @ 0x56866a`); an expansion/mod reload re-drives Var2
(`Game_ReloadExpansionAndMods @ 0x55287a`).

### The WAC `music` command and the .bms header `music` field are DEAD in retail JO

- WAC `music N` (command-table entry `@ 0x82e1f0`) = `Sbf_StartEntry @ 0x4ed910`:
  seeks a **separate** gamemus SBF stream (globals `0xC60Dxx`) to entry N. That
  stream's opener `Sbf_OpenFile_Gamemus @ 0x4ed6c0` (CreateFileA on
  `g_path_game_sbf`, header+entry parse) has **zero references** in
  Jointops.exe — the stream never opens, `dword_C60D80` stays null, and
  `Sbf_StartEntry` early-returns success. The per-frame pump exists and runs
  (`Audio_StreamNextChunk @ 0x4ed7d0` from `Audio_UpdateAmbientStream
  @ 0x4ed9c0`, called in the frame loop `@ 0x5ca348` inside
  `Render_ProcessMainSceneFrame`) but has nothing to stream;
  `WacScript_FreeAll @ 0x4f634c` closes the never-opened handle. Net: **the WAC
  `music` command produces no audio in retail JO** (Delta Force-era leftover).
  Our WAC VM accordingly leaves the emitted `music` effect unconsumed.
- No other code path starts gamemus SBF entries, so the `.bms` header `music`
  field (offset 272) has **no live consumer** in JO — it is vestigial data the
  mission editor round-trips. `NovaMissionAudio._apply_music` stays a
  witnessed no-op.

### Divergence D-MUS-SPGATE

| ID | Ours | Original | Why / consequence |
| --- | --- | --- | --- |
| D-MUS-SPGATE | the gamemus context opens at mission start in ALL sessions | opens only when `g_napi_np_ctx.is_mp_session_peer`; otherwise `AudioVM_StopMusicContext` (retail SP is music-silent in-mission) | maintainer decision 2026-07-09: our single-player runs as a listen server (ADR 0009/0011/0012), so every session is architecturally an MP session, and silent-SP reads as a defect to players. One-line seam at the open site; retail parity available by gating on the session-peer flag. |

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
