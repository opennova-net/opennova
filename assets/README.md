# `assets/` — the game's own authored assets

These are **the game's assets** — the ones OpenNova is built out from, authored
in ONED and committed here. Not a test fixture: the per-format `fixtures/<fmt>/`
sets and the retail-derived ones are expected to give way to this tree as it
grows.

Every byte is authored from scratch by our own writers and tools; no retail
asset is committed. Retail `Jointops.exe` is the **oracle**, not the target: it
is the original consumer, so an asset it loads and renders is a correct asset.
That is the whole reason the retail loop exists — proving compatibility both
validates what ONED wrote and tells our own engine what it has to accept.

This is the concrete instantiation of the R8 required-resources manifest
([../docs/required-resources.md](../docs/required-resources.md)) — that
record answers "what a person starts with to make a new game"; this directory
*is* that starting set, built and validated. It also seeds ONED-REQ's "new
game scaffold" and is an end-to-end proof that our parity writers emit bytes a
stock client loads.

## Why this can be committed (asset policy)

[../docs/asset-gated-tests.md](../docs/asset-gated-tests.md) forbids
committing retail assets. Nothing here is a retail asset: every byte is
produced by an OpenNova writer from an authored source, MIT-licensed, exactly
like the existing per-format `fixtures/<fmt>/` sets. The retail install is
needed only to *validate* the set (launch JO), which is the asset-gated
acceptance step, never a committed input.

## Layout

- **this root, flat** — the committed authored assets (text ones as plain files
  in git, binaries in LFS), guarded by the `minimal_*` ctests. Retail resolves
  bare filenames at the root, which is why nothing loadable lives in a
  subdirectory.
- **`src/`** — modelling sources (`.blend`, `.ase`) that retail never loads.
- **the same root is also the game dir** the asset-gated validation runs retail
  out of, so `Jointops.exe`, `binkw32.dll`, the retail runtime's own writes
  (`game.cfg`, `player.sav`, `_filelog.txt`, ...) and the packaging tool's
  `.pff` output all land here beside the sources. `.gitignore` therefore
  blanket-ignores the root and allowlists our files **by name** — a denylist
  once let loose retail copies slip into a commit. `.gitattributes` keeps that
  `.gitignore` a plain text blob on every checkout (an LFS pointer ignores
  nothing).
- **`../packed/`** (gitignored, self-ignoring) — where ONED's Play in Retail
  (F7) packs this tree and stages the retail runtime beside it.

## The set (grounded in required-resources.md)

Categories from the R8 manifest. **Only the fatal set + the host/join mission
set + the visible-menu set are authored here**; everything the engine skips
gracefully on miss is deliberately omitted to keep "minimal" honest.

### Fatal — boot refuses without these

| File | Origin | Notes |
|---|---|---|
| **PFF archives under the boot-table names (required — see below)** | `engine/formats/pff` write side (ADR 0008) | at least one PFF **must open** or boot exits, and the boot probes **only the fixed name table** — `language.pff` / `localres.pff` / `resource.pff` `[orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90]` (D-VFS-2). An arbitrary-named archive (the first `mnml.pff` attempt) is never probed: `opened_count` stays 0, the caller `test eax,eax; jz` at `@ 0x4a6f4c` falls through to `Game_ShowEarlyError(3)` ("missing CD?") + `Exit(-1)` — **validated on retail 2026-07-05**. `/d` (loose-first) does **NOT** clear this — the loose flag only reorders *lookup*, not the boot archive-open count (witnessed 2026-07-05). The gate counts archives OPENED, not entries: a zero-entry archive satisfies it (witnessed 2026-08-23). |
| `gameerr.bin` | `engine/formats/rtxt` | error strings `[orig: @ 0x4a6fc8]` — a miss is `ShowEarlyError(4)`, non-fatal, but the boot is dirty without it (validated on retail); authored empty-but-valid. Retail ships it in `language.pff`. |
| `gametext.bin` | `engine/formats/rtxt` | game strings RTXT `[orig: @ 0x4a6fed]` — minimal table (the menu/HUD keys the set references). |
| `vmacros.bin` | `engine/formats/rtxt` | voice-macro strings `[orig: @ 0x4a702f]` — may be empty-but-valid. |
| `keyhelp.bin` | `engine/formats/rtxt` | keyboard-map strings `[orig: @ 0x4a7072]` — may be empty-but-valid. |
| `items.def` | authored text | `[orig: @ 0x4a71a3 → ItemDef_ParseProperty @ 0x49eb00]` — minimal: only the item types the map spawns (witnessed mapping, D-ITEMDEF-1). Ids the engine addresses **by number** are reserved (`106001` is the player-start marker the engine reads to place its hardcoded player item `105310`); ids that merely also exist in retail's catalogue are free, and our own content sits at `108001+`. |
| `main.mnu` (`"Startup"` node) | ONED menu workspace | the entry screen `[orig: sub_552500 @ 0x552651]`. |

### Host + join + single player — mission start + menus

| File | Origin | Notes |
|---|---|---|
| `mnml.bms` | ONED mission workspace | the mission: the `106001` player start, both team starts (`106003`/`106004`), one objective, minimal item set. Retail does not spawn the player from a placed entity — it spawns item `105310` by its own id and reads the placed `106001` marker to learn where (confirmed against retail's `00TRa.bms`: 1331 entities, exactly one `106001`, no player entity). |
| `mnml.trn` + `mnml.cpt` | ONED terrain workspace (`save_trn` + the CDEP bake) | the terrain config and its baked polydata. JO reads the compressed CDEP depth; the BHD-era DPTH the builder defaults to is a `.cpt` retail cannot decode. `sector_count` is the grid WIDTH, not a count of active sectors. |
| `mnml.env` | ONED environment workspace (`save_env`) | one time-of-day; defaults elsewhere. The mission header's Q8.8 start hour overrides the `.env`'s own `curtime`, so the mission starts at noon rather than rendering under the midnight ramp. |
| `mp.mnu` | ONED menu workspace | the host/join menu `[orig: @ 0x5588fa]`. |
| `sp.mnu` | ONED menu workspace | the single-player mission screen `[orig: SinglePlayer_PopulateMissionList @ 0x561840]` — where the packed mission has to appear. |
| `weapon.def`, `ammo.def` | authored text | minimal: one spawn weapon + its ammo `[orig: WeaponDef_LoadAll @ 0x54dd10; AmmoDef_LoadAll @ 0x40b0b0]`. |
| `game.wac` / `server.wac` | — | optional (silent skip) — add only if the join needs mission logic to progress. |

### Where the mission list looks (witnessed against retail)

Retail builds its mission table from two scans: a loose `FindFirstFile *.bms`
walk of the working directory, and a per-archive entry walk over the
**localres/language volumes only**
`[orig: MissionList_ScanAndBuildFromFiles @ 0x563170; Mission_BuildMapListFromPFF @ 0x562910]`.
A `.bms` archived in `resource.pff` mounts and loads by name and never appears
in any list (witnessed 2026-08-23: the menu simply empty, no error). Retail
keeps its own where the walk looks — all 116 stock JO `.bms` live in
`localres.pff`, none in `resource.pff` — so ONED's packer names its single
archive `localres.pff` and ships the mission archived, exactly like retail. The
packaging tool's three-archive layout puts every `.bms` in `localres.pff` for
the same reason; its loose layout lists the mission through the loose walk.

### The mission file family (witnessed against retail)

Every retail mission ships as `<stem>.bms` + `.til` + `.pcx` + `.dbf` +
`.lwf` + `.wac` + `.bin` (sampled across the JOTAC archives). Placement:
`.bin`/`.pcx`/`.lwf` in `language.pff`; `.bms`/`.til`/`.dbf`/`.wac` in
`localres.pff`. The minimal set authors:

| File | Origin | Notes |
|---|---|---|
| `mnml.dbf` + `mnml.lwf` | `engine/formats/dbf` + `engine/formats/lwf` | the per-mission dialog chain — `DialogSystem_Init @ 0x5275e0` loads `<base>.dbf`, which co-loads `<base>.lwf` `[orig: @ 0x44e7d4]`. Empty-but-valid (the minimal mission speaks no dialog). |
| `mnml.bin` | `engine/formats/rtxt` | the briefing text table (`[Info] TITLE` / BRIEFING), looked up beside the `.bms` through the by-name front door. |
| `mnml.pcx` | ONED | the map overview — retail's are 800×600 8-bit indexed. |
| `mnml.til` | — deferred | the tile overlay; its loader is called ONLY from the render loop (`[orig: @ 0x5ca730, sole caller Render_ProcessMainSceneFrame @ 0x5ca0f0]`), so a miss cannot block boot/host — the terrain just renders untiled. |
| `mnml.wac` | — deferred | witnessed silent skip (see above). |

### The visible-menu set

Boot-clean was not menu-visible: the Startup screen drew text-only buttons on
a **null font slot** — a black screen (validated on retail 2026-07-05; the
`/FRISK` log showed every set file loading and the profile saves proved the
shell was running). The menu shell's own load list is witnessed at
`sub_552500 @ 0x552500`: `menu_style.mns` → `brand.mns` (append; not even
retail ships one) → the three menu Bink slots (`main.bik`/`header.bik`/
`footer.bik`) → `nw_cdata.coo` → `main.mnu` → `Startup` →
`HUD_InitAllFonts @ 0x51ee20` with **hardcoded font names** (width breakpoints
640/800/1024 pick `Arial12b`/`14n`, `14b`/`14n`, or `16b`/`16n`;
`Impac22b`/`Impac38b` always). So the set authors:

| File | Origin | Notes |
|---|---|---|
| the seven boot `.fnt`s | ONED fonts workspace (`engine/formats/fnt`) | one authored glyph set emitted under every hardcoded name. Retail ships fonts in `localres.pff`. |
| `menu_style.mns` | authored text | the stylesheet the shell loads by canonical name `[orig: @ 0x552604]`; carries the retail key set (key NAMES witnessed vs the JOTAC install — `DEF_FONTNAME`/`DEF_FONTNAME_LG`/`IMPACT_FONTNAME` + colors); values are ours, fonts point at the authored set. |
| `menutxt.bin` | `engine/formats/rtxt` | the `TEXT_RSRC` string ids the authored menus reference (`MM_*`/`MP_*`). Retail ships it in `language.pff`. |
| `newarow1.tga` | authored TGA | **the menu cursor** — declared per screen in the `.mnu` `<CURSOR>` block (retail FILENAME, our arrow art; 32×32 type-2 BGRA, alpha-keyed `STANDARD_TRANSPARENT`, bottom-up rows matching retail's format — `minimal_art_validate` pins that format). Without it there is no cursor and nothing is clickable. |
| `menumus.sbf`/`gamemus.sbf` + `menumus.bin`/`gamemus.bin` | ONED music workspace (`engine/formats/sbf` + the `mus` compiler) | the hardcoded base-game music pairs `[orig: Expansion_LoadAssets @ 0x4a4730]`: a silent bank written LOOSE at the root (the `.sbf` banks stream by path and never resolve through an archive) + a minimal `play/done` script per name in `localres.pff`. |
| `earlyerr.txt` | authored text | the pre-archive error text, read loose before any mount `[orig: Game_ShowEarlyError @ 0x4a68a0]`; line 3 is the no-archives message, line 4 the missing-`gameerr.bin` one. |

Videos (`BIK`) stay omitted **by design**: they load loose via Win32
`OpenFile` (`[orig: Game_PlayIntroVideos @ 0x5637a0 → 0x5636d0]`), never from
the PFFs, and a miss skips playback — the menu background just stays black
(cosmetic). `nw_cdata.coo` misses gracefully.

### CRLF is mandatory for every authored text resource

Retail's text parsers are **not LF-tolerant, and they fail silently.** A
LF-only file still "loads" (it is logged by `/FRISK`), the parse stops early,
and the game hangs or draws nothing instead of reporting an error.

Witnessed on retail 2026-08-23: a LF-only `menu_style.mns` dead-ends the menu
shell — `/FRISK` logs it twice and boot stops there, never reaching `main.mnu`,
with the window Not Responding. The byte-identical file converted to CRLF takes
the same install from 18 logged loads to 28, straight through `main.mnu`, the
hardcoded fonts and the cursor. Retail's own `menu_style.mns` ships CRLF, and
its file header documents a line-oriented parser with backslash continuations.

The same rule was already known one format over: a bare LF between blocks
stopped retail's `.def` parser after block 0 (witnessed while trimming a
retail install down to one mission, 2026-08; every `.def` there had to keep
its original line ending for the parser to read past the first block).

The writer-produced members of the set (`.trn` via `save_trn`, `.env` via
`save_env`) emit CRLF from the engine libraries already. The hand-authored
text files — `items.def`, `weapon.def`, `ammo.def`, `main.mnu`, `mp.mnu`,
`sp.mnu`, `menu_style.mns`, `earlyerr.txt` — have no writer to enforce it, and
they are `-text -eol` in `.gitattributes` so git will not normalize them either
way. One editor save in LF mode silently breaks the boot again, so
`minimal_eol_guard` (ctest) pins all of them.

### Deliberately omitted (graceful-on-miss — keeps the set minimal)

Videos (`BIK` — see above), `Avatars.def`, `SndProf.def`, `charattr.def`
(soft error, continues), `powerup.def` (soft), `hudfx/hudpos.def` (default
positions), `game.bin` (fallback literals), `nw_cdata.coo`. Each is listed in
the R8 manifest with its graceful failure; adding any is a deliberate step up
from minimal, not a requirement.

## Guards (ctest, run in CI)

Nothing here is generated any more: every file is authored in ONED (or by
hand, for the text ones) and committed, so the guards check what the ENGINE
and RETAIL need from each file rather than asserting byte-equality against a
throwaway generator:

| Guard | Covers |
|---|---|
| `minimal_rtxt_gen` | the string tables emit + round-trip |
| `minimal_def_validate` | `items.def` / `weapon.def` / `ammo.def` parse through `engine/formats/def` |
| `minimal_mnu_validate` | `main.mnu` (Startup), `mp.mnu` (LAN host/join), `sp.mnu` (single player) parse and carry their screens |
| `minimal_map_validate` | `mnml.env` loads; `mnml.bms` parses, places exactly one `106001` and both team starts, names the terrain, starts in daylight |
| `minimal_trn_gen` | `mnml.trn` round-trips, keeps the 8-wide sector grid + quadrant block, names exactly the shipped `mnml_*` art (`OPENNOVA_WRITE_MINIMAL_FIXTURES=1` re-emits the config) |
| `minimal_art_validate` | every image `mnml.trn` names decodes; the colormap is big enough to quadrant-split; the cursor is a 32×32 type-2 32 bpp alpha TGA |
| `minimal_eol_guard` | every hand-authored text file is CRLF |
| `minimal_pff_package` | the packaging tool (below); skips unless asked |

## Packaging

`tests/fixtures/minimal_pff_package.cpp` bundles the committed tree — it reads
`assets/` and never writes into it. Two layouts:

- `OPENNOVA_BUILD_MINIMAL_PFF=1` writes the **three boot-table archives** into
  this root (gitignored), each file in the archive retail uses for its kind
  (witnessed against the JOTAC JO install): `language.pff` = the boot text
  bins + the mission-family `.bin`/`.pcx`/`.lwf`, `localres.pff` = the menus,
  defs, mission, fonts and music scripts, `resource.pff` = the map and terrain
  (`.env`/`.trn`/`.cpt`/source art).
- `OPENNOVA_MINIMAL_INSTALL=<dir>` assembles a runnable **loose** install:
  everything flat plus a zero-entry `resource.pff` boot token, run with `/d`.
  With `OPENNOVA_JO_DIR` set it also stages `Jointops.exe` + `binkw32.dll` +
  `game.cfg` from your own install (never committed).

ONED's own Play in Retail (F7) is the everyday loop: it packs the mounted tree
into a single `localres.pff` under `../packed/`, stages the retail runtime
beside it, and launches — F8 stops it.

## Validation (asset-gated — needs a retail JO install)

1. Launch `Jointops.exe /w /d /FRISK` on a packed dir — confirm boot to the main
   menu (the fatal set is sufficient), then the mission list, then the mission
   load. **Done 2026-08-23** through menu → mission list → mission load. Debug
   with **`/FRISK`** — the retail file-access log
   (`[orig: File_SetLoggingEnabled @ 0x75a470 → File_LogFileAccess @
   0x75a480]`): every *successful* load is appended to `_filelog.txt` in the
   game dir as `PFF LOADED FILE:` / `LOADED FILE:` (misses are not logged).
2. Host the custom map from the MP menu on instance A.
3. Join from instance B (LAN). Confirm both spawn on `mnml` and can move.

Records the run under the asset-gated protocol (never commit the capture); the
recipe is the acceptance test for "the minimal set hosts + joins."

Known gaps: nothing `items.def` declares has a model yet; the terrain has
relief and a full-size colormap but no tile overlay.

## MVP convergence

When retail↔retail works on this set, it is simultaneously (a) the **MVP asset
target** — the exact inputs our runtime must load to host + join our own
engine — and (b) a **parity proof** that every writer in the chain
(rtxt/mission/trn/env/mnu/def/fnt/sbf/pff) emits retail-loadable output end to
end. The GOALS.md "export a game" path starts here.
