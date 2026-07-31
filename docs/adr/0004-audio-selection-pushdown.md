# ADR 0004 — Sound-set member selection pushed down to libs/audio

Status: accepted (authored on `unified-edit-history` as ADR 0002; renumbered on extraction to
master — 0001..0003 are reserved for the MNU ADRs that land with the menu slice)

## Context

`GOALS.md`: "the portable engine core is C++; Godot is the host that renders it." The runtime audio
code lived entirely in GDScript (`nova_sound_bank.gd`, `nova_mission_audio.gd`). The genuinely portable
engine logic in there was the **per-layer sound-set member-selection state machine** (FIRST / RANDOM /
SEQUENTIAL / RANDOM_SEQUENTIAL with per-layer cursor/bag state — originally the archive's
`sound_set.cpp`), which a headless host would also need.

## Decision

Move the selection state machine into portable C++; keep the shell-bound parts in GDScript.

- **`libs/audio` — `opennova::audio::SoundSelector`**: `select(key, member_count, mode) -> index`,
  per-`(bank,set,layer)` state, engine-faithful machine (sequential cursor; random-anchor full
  cycle; random default with the engine's shared ROL-LCG stream and static seed).
- **`NovaSoundSelector`** (GDExtension shim) exposes `select_member(...)`; `NovaSoundBank` holds one and
  its `_pick_member` now only feeds it the layer's member count + mode and poses the chosen member. The
  bank keeps the `.lwf` data access, the WAV decode/cache, and the `AudioStreamPlayer3D` spawning.

## What stayed in GDScript, and why

- **Dialog-id → set-name resolution** (`play_dialog`/`resolve_dialog_set`): the meaningful step is the
  `.DBF` lookup, bound to `NovaDbfData` (a GDExtension resource). Only trivial fallback string forms
  would move; not worth a shim round-trip.
- **Voice culling** (`NovaMissionAudio.tick`): runs per-voice per-frame. Routing each voice through a
  C++ shim call each frame would add Variant-boxing overhead for a one-line distance predicate — the
  opposite of the perf intent. The `CULL_RADIUS` value stays a documented host constant.
- **Frame-loop ordering** (also part of this pass's "push core down") is single-sourced in the
  mission runtime, which lands with the mission slice (its ADR ships then).

## Divergence (resolved 2026-06-09)

The original concern — "the selection RNG is not the engine's" — was closed by the sound-stack
grill: the modes and the RNG are now a structural translation of the engine's member pick
(`SoundBank_PlayTriggerEntries @0x75ccd0`; `PRNG_ScaledRandom @0x75be50`, shared state
`@0x85A3DC`, static seed `0x2B0749C1`), and the test pins the exact stream. The remaining,
documented gap: the engine interleaves volume/pitch jitter draws on the same stream during
playback, which the host does not reproduce. Full record: `docs/audio/lwf-dbf-sound-re.md`.

## IDA anchors

`SoundBank_PlayTriggerEntries @0x75ccd0` / `SoundBank_SelectTriggerEntryFromBank @0x75bf20`
(member pick), `PRNG_ScaledRandom @0x75be50` (RNG), `SoundProfile_FindLoadedByName @0x5274f0` and
`SoundBank_FindTriggerByName @0x75be90` (name-keyed set lookup). Seams left in GDScript: reverb
table `Audio_LoadReverbDefs @0x766d80`, MUS music `AudioVM_OpenMusicContext @0x6722a0`.

## Verification

`tests/audio/sound_selector_test.cpp` (C++ ctest: mode shapes, per-key independence, edge cases). GUT
sound suite green after the refactor (`sound_runtime`, `sound_dialog`, `sound_integration`,
`sound_controller`, `sound_bus`).
