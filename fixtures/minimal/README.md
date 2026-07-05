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

## Layout

- **`resources/`** — the committed authored sources (every file our writers
  emit, guarded by the `minimal_*` ctests). Nothing here is retail.
- **this root** — the "game dir": the packaging tool writes the three
  boot-table `.pff`s here, and the asset-gated validation step drops the
  retail `Jointops.exe` (+ `binkw32.dll`, the Bink redistributable the exe
  links) here to run them. Everything at the root is gitignored — only
  `resources/`, this README, and `.gitignore` are tracked.

## The set (grounded in required-resources.md)

Categories from the R8 manifest. **Only the fatal set + the host/join mission
set are authored here**; everything the engine skips gracefully on miss is
deliberately omitted to keep "minimal" honest.

### Fatal — boot refuses without these

| File | Our writer | Notes |
|---|---|---|
| **PFF archives under the boot-table names (required — see below)** | `libs/pff` write side (ADR 0008) | at least one PFF **must open** or boot exits, and the boot probes **only the fixed name table** — `language.pff` / `localres.pff` / `resource.pff` `[orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90]` (D-VFS-2). An arbitrary-named archive (the first `mnml.pff` attempt) is never probed: `opened_count` stays 0, the caller `test eax,eax; jz` at `@ 0x4a6f4c` falls through to `Game_ShowEarlyError(3)` ("missing CD?") + `Exit(-1)` — **validated on retail 2026-07-05**. `/d` (loose-first) does **NOT** clear this — the loose flag only reorders *lookup*, not the boot archive-open count (witnessed 2026-07-05). So the minimal set packages into the three boot-table names, mirroring retail's placement per kind; loose-alongside is optional. |
| `gameerr.bin` | `libs/rtxt` | error strings `[orig: @ 0x4a6fc8]` — a miss is `ShowEarlyError(4)`, non-fatal, but the boot is dirty without it (validated on retail); authored empty-but-valid. Retail ships it in `language.pff`. |
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

### The mission file family (witnessed against retail)

Every retail mission ships as `<stem>.bms` + `.til` + `.pcx` + `.dbf` +
`.lwf` + `.wac` + `.bin` (sampled across the JOTAC archives). Placement:
`.bin`/`.pcx`/`.lwf` in `language.pff`; `.bms`/`.til`/`.dbf`/`.wac` in
`localres.pff`. The minimal set authors:

| File | Our writer | Notes |
|---|---|---|
| `mnml.dbf` + `mnml.lwf` | `libs/dbf` + `libs/lwf` (`minimal_companion_gen`) | the per-mission dialog chain — `DialogSystem_Init @ 0x5275e0` loads `<base>.dbf`, which co-loads `<base>.lwf` `[orig: @ 0x44e7d4]`. Empty-but-valid (the minimal mission speaks no dialog). |
| `mnml.bin` | `libs/rtxt` (`minimal_rtxt_gen`) | the briefing text table — valid-but-empty until the briefing screen names its keys. |
| `mnml.pcx` | `libs/pcx` (`minimal_art_gen`) | the map overview — retail's are 800×600 8-bit indexed; a solid placeholder for now. |
| `mnml.til` | — deferred | the tile overlay; its loader is called ONLY from the render loop (`[orig: @ 0x5ca730, sole caller Render_ProcessMainSceneFrame @ 0x5ca0f0]`), so a miss cannot block boot/host — the terrain just renders untiled. Needs a `libs/til` writer when the overlay matters. |
| `mnml.wac` | — deferred | witnessed silent skip (see above). |

### The visible-menu set (added after the black-screen finding)

Boot-clean was not menu-visible: the Startup screen drew text-only buttons on
a **null font slot** — a black screen (validated on retail 2026-07-05; the
`/FRISK` log showed every set file loading and the profile saves proved the
shell was running). The menu shell's own load list is witnessed at
`sub_552500 @ 0x552500`: `menu_style.mns` → `brand.mns` (append; not even
retail ships one) → the three menu Bink slots (`main.bik`/`header.bik`/
`footer.bik`) → `nw_cdata.coo` → `main.mnu` → `Startup` →
`HUD_InitAllFonts @ 0x51ee20` with **hardcoded font names** (width breakpoints
640/800/1024 pick `Arial12b`/`14n`, `14b`/`14n`, or `16b`/`16n`;
`Impac22b`/`Impac38b` always). So the set now authors:

| File | Our writer | Notes |
|---|---|---|
| the seven boot `.fnt`s | `libs/fnt` `fnt_write` + `tests/fixtures/minimal_fnt_builder.h` | ONE authored 5×7 stroke glyph set (2× on a single 256×256 page) emitted under every hardcoded name; generated at package time, never committed; guarded by `minimal_fnt_gen`. Retail ships fonts in `localres.pff`. |
| `menu_style.mns` | authored text | the stylesheet the shell loads by canonical name `[orig: @ 0x552604]`; carries the retail key set (key NAMES witnessed vs the JOTAC install — `DEF_FONTNAME`/`DEF_FONTNAME_LG`/`IMPACT_FONTNAME` + colors); values are ours, fonts point at the generated set. |
| `menutxt.bin` | `libs/rtxt` | the `TEXT_RSRC` string ids the authored menus reference (`MM_*`/`MP_*`). Retail ships it in `language.pff`. |
| `newarow1.tga` | authored TGA (`minimal_art_gen`) | **the menu cursor** — declared per screen in the `.mnu` `<CURSOR>` block (retail FILENAME, our arrow art; 32×32 type-2 BGRA, alpha-keyed `STANDARD_TRANSPARENT`, bottom-up rows matching retail's format). Without it there is no cursor and nothing is clickable. |
| `menumus.sbf`/`gamemus.sbf` + `menumus.bin`/`gamemus.bin` | `libs/sbf` encoder + `libs/mus` compiler (`minimal_mus_builder.h`) | the hardcoded base-game music pairs `[orig: Expansion_LoadAssets @ 0x4a4730]`: a silent one-entry bank written loose at the root (retail ships the `.sbf` banks loose) + a minimal `play/done` script per name in `localres.pff`. Generated at package time; guarded by `minimal_mus_gen`. |

Videos (`BIK`) stay omitted **by design**: they load loose via Win32
`OpenFile` (`[orig: Game_PlayIntroVideos @ 0x5637a0 → 0x5636d0]`), never from
the PFFs, and a miss skips playback — the menu background just stays black
(cosmetic). `nw_cdata.coo` misses gracefully.

### Deliberately omitted (graceful-on-miss — keeps the set minimal)

All music (`SBF`/`BIN`), videos (`BIK` — see above),
`Avatars.def`, `SndProf.def`, `charattr.def` (soft error, continues),
`powerup.def` (soft), `hudfx/hudpos.def` (default positions), `game.bin`
(fallback literals), `nw_cdata.coo`. Each is listed in the
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
| `gameerr.bin`, `gametext.bin`, `vmacros.bin`, `keyhelp.bin`, `menutxt.bin` | `libs/rtxt` | `minimal_rtxt_gen` (emit + byte-stable + round-trip) | **done** — the boot string tables + menu labels |
| the seven boot `.fnt`s | `libs/fnt` + `minimal_fnt_builder.h` | `minimal_fnt_gen` (validity + byte-stable + label glyphs drawable) | **done** — generated at package time, never committed |
| `menu_style.mns` | authored text | packaged + entry-verified by `minimal_pff_package` | **done** — retail key set, our values |
| `items.def`, `weapon.def`, `ammo.def` | authored text | `minimal_def_validate` (parse through `libs/def`) | **done** — a spawnable person, a rifle, its round |
| `main.mnu`, `mp.mnu` | `libs/mnu` | `minimal_mnu_validate` (parse + screen present) | **done** — Startup node + LAN host/join, small authored |
| `mnml.env`, `mnml.bms` | `libs/env`, `libs/mission` | `minimal_map_gen` (round-trip + content) | **done** — one TOD + a named mission with two team spawns |
| `mnml.trn` | `libs/trn` `save_trn` | `minimal_trn_gen` (round-trip + refs) | **done** — the terrain config, names the `.cpt` + source art |
| `mnml_c/dm/dc1/t.tga`, `mnml_m/f.pcx` | hand-rolled TGA + `libs/pcx` | `minimal_art_gen` (byte-stable + PCX decode) | **done** — solid source art for the flat map |
| **the packaging tool** | `libs/terrain` + `libs/pff` | `minimal_pff_package` (build + bundle + open) | **done** — see below |

**The build chain is complete.** `minimal_pff_package` (opt-in via
`OPENNOVA_BUILD_MINIMAL_PFF=1`, since `build_terrain` is ~35 s / ~10 MB) is the
**generate-at-package step**: it runs `build_terrain` on a flat 1024×1024
depthmap (→ `mnml.cpt` + ~680 `.tms`/`.tml` LOD tiles into a gitignored
`_pff_build/`), then `pff_write_archive` bundles the set into the **three
boot-table archives**, each file in the archive retail uses for its kind
(witnessed against the JOTAC JO install): `language.pff` = the boot text bins
(`gameerr`/`gametext`/`vmacros`/`keyhelp.bin`), `localres.pff` = the menus,
defs, and mission (`.mnu`/`.def`/`.bms`), `resource.pff` = the map and terrain
(`.env`/`.trn`/source art + the generated `mnml.cpt` and tiles). The `.pff`s
and `_pff_build/` are gitignored (never committed). Committed is only the
small config surface in `resources/` (bins/defs/mnus/env/bms/trn + the tiny
source-art images); everything heavy is regenerated.

**Retail boot is validated (2026-07-05):** a single arbitrary-named `mnml.pff`
dies at the boot gate exactly as witnessed (`ShowEarlyError(3)` "missing
CD?" — the fixed name table never probes it); with the set under the
boot-table names, retail `Jointops.exe` boots **fully clean** — the `/FRISK`
log shows every set file loading from the PFFs (`gameerr` → `gametext` →
`vmacros` → `keyhelp` → `weapon.def` → `items.def` → `MNML.BMS` → `main.mnu`)
and the shell runs (profile saves written). The Startup screen is black until
the font leg lands (§ omitted). Remaining: the visible-menu legs, then
**asset-gated host + join acceptance** (§ Validation) — this is where the
`.trn`/art dimensions and the build-output→`.trn` mapping get their final
confirmation; it needs a retail install.

## Validation (asset-gated — needs a retail JO install)

1. Launch `Jointops.exe` with the authored `language.pff` / `localres.pff` /
   `resource.pff` present in the game root (and optionally `/d <loose dir>`) —
   confirm boot to the main menu (the fatal set is sufficient). The archives
   must bear the boot-table names (witnessed above; an arbitrary name never
   mounts) and `/d` alone is not enough. **Done 2026-07-05 on the three-way
   split**: full clean boot to the (black — see § omitted) Startup screen.
   Debug with **`/FRISK`** — the retail file-access log
   (`[orig: File_SetLoggingEnabled @ 0x75a470 → File_LogFileAccess @
   0x75a480]`): every *successful* load is appended to `_filelog.txt` in the
   game dir as `PFF LOADED FILE:` / `LOADED FILE:` (misses are not logged).
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
