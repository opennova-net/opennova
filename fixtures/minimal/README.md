# `fixtures/minimal/` — the smallest authored game that hosts + joins

Goal: the **minimal resource set that runs retail `Jointops.exe` as a host and
lets a second instance join**, over a **custom minimal map**, with every file
**authored from scratch by our own writers** — no retail asset committed. Once
retail↔retail works on this set, the *same* set is the seed for our engine's
MVP (host + join our own runtime on the identical inputs).

This is the concrete instantiation of the R8 required-resources manifest
([../../docs/required-resources.md](../../docs/required-resources.md)) — that
record answers "what a person starts with to make a new game"; this directory
*is* that starting set, built and validated. It also seeds ONED-REQ's "new
game scaffold" and is an end-to-end proof that our parity writers emit bytes a
stock client loads.

## Why this can be committed (asset policy)

[../../docs/asset-gated-tests.md](../../docs/asset-gated-tests.md) forbids
committing retail assets. Nothing here is a retail asset: every byte is
produced by an OpenNova writer from an authored source, MIT-licensed, exactly
like the existing per-format `fixtures/<fmt>/` sets. The retail install is
needed only to *validate* the set (launch JO), which is the asset-gated
acceptance step, never a committed input.

## The set (grounded in required-resources.md)

Categories from the R8 manifest. **Only the fatal set + the host/join mission
set are authored here**; everything the engine skips gracefully on miss is
deliberately omitted to keep "minimal" honest.

### Fatal — boot refuses without these

| File | Our writer | Notes |
|---|---|---|
| **one PFF archive (required — see below)** | `libs/pff` write side (ADR 0008) | at least one PFF **must open** or boot exits. `PFF_OpenAllArchives @ 0x4a4310` returns `!opened_count`; the caller `test eax,eax; jz` at `@ 0x4a6f4c` falls through to `Game_ShowEarlyError(3)` + `Exit(-1)` when zero opened. `/d` (loose-first) does **NOT** clear this — the loose flag only reorders *lookup*, not the boot archive-open count (witnessed 2026-07-05). So the minimal set authors one PFF holding the fatal + mission files; loose-alongside is optional. |
| `gametext.bin` | `libs/rtxt` `write()` | game strings RTXT `[orig: @ 0x4a6fed]` — minimal table (the menu/HUD keys the set references). |
| `vmacros.bin` | `libs/rtxt` | voice-macro strings `[orig: @ 0x4a702f]` — may be empty-but-valid. |
| `keyhelp.bin` | `libs/rtxt` | keyboard-map strings `[orig: @ 0x4a7072]` — may be empty-but-valid. |
| `items.def` | text (author) | `[orig: @ 0x4a71a3 → ItemDef_ParseProperty @ 0x49eb00]` — minimal: only the item types the map spawns (witnessed mapping, D-ITEMDEF-1). |
| `main.mnu` (`"Startup"` node) | `libs/mnu` writer | the entry screen `[orig: sub_552500 @ 0x552651]`. Reuse the shape of `fixtures/mnu/jo_main.mnu`, trimmed to Startup → MP. |

### Host + join — mission start + MP menu

| File | Our writer | Notes |
|---|---|---|
| `<map>.bms` | `libs/mission` `write_bms_bytes` | the custom map's mission: spawn points (both teams), one objective, minimal item set. |
| `<map>.trn` (+ tiles/env refs) | `libs/trn` (TrnGen, byte-identical) | a small flat/simple heightfield — the smallest valid terrain. |
| `<map>.env` | `libs/env` writer | one time-of-day; defaults elsewhere. |
| `mp.mnu` | `libs/mnu` | the host/join menu `[orig: @ 0x5588fa]` (co-op LAN bring-up used this). |
| `weapon.def`, `ammo.def` | text (author) | minimal: one spawn weapon + its ammo `[orig: WeaponDef_LoadAll @ 0x54dd10; AmmoDef_LoadAll @ 0x40b0b0]`. |
| `game.wac` / `server.wac` | `libs/wac` | optional (silent skip) — add only if the join needs mission logic to progress. |
| mission-list registration | — | how the MP menu finds `<map>` (`.npj`/`.npz` scan `[orig: MissionList_ScanAndBuildFromFiles @ 0x563170]` vs a direct MP host pick — resolve in § Validation). |

### Deliberately omitted (graceful-on-miss — keeps the set minimal)

Fonts (null slot, no crash), all music (`SBF`/`BIN`), videos (`BIK`),
`Avatars.def`, `SndProf.def`, `charattr.def` (soft error, continues),
`powerup.def` (soft), `hudfx/hudpos.def` (default positions), menu styling
(`.mns`), `game.bin`/`menutxt.bin` (fallback literals). Each is listed in the
R8 manifest with its graceful failure; adding any is a deliberate step up from
minimal, not a requirement.

## Build

Each writer-emitted file is generated *and guarded* by a ctest — the committed
bytes are provably reproducible from the writer, no retail input, so it runs in
CI. Regenerate the writer-emitted files with `OPENNOVA_WRITE_MINIMAL_FIXTURES=1`.

## Status

Authored so far (each guarded by a ctest):

| File(s) | Writer | Guard | State |
|---|---|---|---|
| `gametext.bin`, `vmacros.bin`, `keyhelp.bin` | `libs/rtxt` | `minimal_rtxt_gen` (emit + byte-stable + round-trip) | **done** — the fatal string tables |
| `items.def`, `weapon.def`, `ammo.def` | authored text | `minimal_def_validate` (parse through `libs/def`) | **done** — a spawnable person, a rifle, its round |
| `main.mnu`, `mp.mnu` | `libs/mnu` | `minimal_mnu_validate` (parse + screen present) | **done** — Startup node + LAN host/join, small authored |
| `mnml.env`, `mnml.bms` | `libs/env`, `libs/mission` | `minimal_map_gen` (round-trip + content) | **done** — one TOD + a named mission with two team spawns |

Remaining slices:

- `mnml.trn` — the terrain, generated **at package time, not committed**. A
  feasibility probe settled the approach: `build_terrain` on a flat 1024×1024
  depthmap (`MAP_SIZE²`) takes **~35 s and emits 685 files / ~10 MB** (the
  `.cpt` ≈3 MB, `Output.dep` ≈2 MB, and ~680 `.tml`/`.tms` quadtree LOD
  tiles) — the full retail terrain set. That is far too heavy to commit and
  too slow for a per-build ctest, so the terrain is produced by the packaging
  step (below) into the PFF. What this repo commits is only the tiny `.trn`
  config + the flat-depthmap recipe (a generator, not the 1 MB raw); a light
  ctest guards the `.trn` config round-trips. Pipeline: a flat depthmap →
  `libs/terrain` `build_terrain` (a `TpjProject`, output prefix `mnml`) →
  `.cpt`/`Output.dep`/`.tms` tiles; the `.trn` config (`libs/trn` `save_trn`)
  references them. Getting the build-output → `.trn`-field mapping exactly
  right is what makes the terrain load in retail — the care in this leg.
- the PFF packaging (`libs/pff` write side) — a tool that runs the terrain
  build, then bundles the whole set (the committed files + the generated
  terrain) into ONE archive, so the fatal not-all-archives-missing gate clears
  (witnessed above). This is where the heavy terrain lives.
- asset-gated retail validation (§ Validation) — needs a retail install.

## Validation (asset-gated — needs a retail JO install)

1. Launch `Jointops.exe` with the authored PFF present in the game root (and
   optionally `/d <loose dir>`) — confirm boot to the main menu (the fatal set
   is sufficient). One PFF must open (witnessed above); `/d` alone is not
   enough.
2. Host the custom map from the MP menu on instance A.
3. Join from instance B (LAN). Confirm both spawn on `<map>` and can move.

Records the run under the asset-gated protocol (never commit the capture); the
recipe is the acceptance test for "the minimal set hosts + joins."

## MVP convergence

When retail↔retail works on this set, it is simultaneously (a) the **MVP asset
target** — the exact inputs our runtime must load to host + join our own
engine — and (b) a **parity proof** that every writer in the chain
(rtxt/mission/trn/env/mnu/def/pff) emits retail-loadable output end to end.
The GOALS.md "export a game" path starts here.
