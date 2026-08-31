# `assets/` — the game's source-owned assets

These are **the game's assets** — the loose source tree OpenNova is built from
and committed here. They are not an ONED project or a test fixture: the
per-format `fixtures/<fmt>/` sets and the retail-derived ones are expected to
give way to this tree as it grows.

Every byte is authored from scratch by our own writers and tools; no retail
asset is committed. Retail `Jointops.exe` is the **oracle**, not the target: it
is the original consumer, so an asset it loads and renders is a correct asset.
That is the whole reason the retail loop exists — proving compatibility both
validates our writers and source files and tells our own engine what it has to
accept.

This is the concrete instantiation of the R8 required-resources manifest
([../docs/required-resources.md](../docs/required-resources.md)) — that
record answers "what a person starts with to make a new game"; this directory
*is* that starting set, built and validated. It is an end-to-end proof that our
parity writers emit bytes a stock client loads.

## Why this can be committed (asset policy)

[../docs/asset-gated-tests.md](../docs/asset-gated-tests.md) forbids
committing retail assets. Nothing here is a retail asset: every byte is
produced by an OpenNova writer from an authored source, MIT-licensed, exactly
like the existing per-format `fixtures/<fmt>/` sets. The retail install is
needed only to *validate* the set (launch JO), which is the asset-gated
acceptance step, never a committed input.

**One deliberate, temporary exception (2026-08-31):** the bring-up model +
anim + texture set below IS retail data, committed by explicit decision so the
minimal set runs from a fresh checkout while our own model and clip writers
catch up. Every such file is allowlisted **by name** under the TEMPORARY banner
in `.gitignore`, ships in both zip flavors until replaced, and is tracked as
debt in [`../TODO.md`](../TODO.md). Everything else in this tree keeps the
rule: authored from scratch by our own writers.

## Layout

- **this root, flat** — the committed authored assets (text ones as plain files
  in git, binaries in LFS), guarded by the `minimal_*` ctests. Retail resolves
  bare filenames at the root, which is why nothing loadable lives in a
  subdirectory.
- **the same root is also the game dir** the asset-gated validation runs retail
  out of, so `Jointops.exe`, `binkw32.dll`, the retail runtime's own writes
  (`game.cfg`, `player.sav`, `_filelog.txt`, ...) and the packaging tool's
  `.pff` output all land here beside the sources. `.gitignore` therefore
  blanket-ignores the root and allowlists our files **by name** — a denylist
  once let loose retail copies slip into a commit. `.gitattributes` keeps that
  `.gitignore` a plain text blob on every checkout (an LFS pointer ignores
  nothing).
- ONED's Stage & Run Retail stages this tree and the retail runtime into
  **ONED's own data dir** (`user://packed`) — never beside the source
  assets.
- **This tree also ships, in two flavors**: the dev zip (`opennova-windows`)
  stages the tracked files here under `assets/` — the game default-mounts that
  loose tree and ONED offers it as the implicit loose-data selection, so the
  game runs the tracked source files with no packing in the loop. The
  tagged-release zip
  (`opennova-game-windows`) carries the same game packed into `localres.pff`
  beside `opennova.exe`, built by ONED's hidden
  `opennova-modtools.exe --headless -- --pack-game <src> <game_dir>` command.

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
| `items.def` | authored text | `[orig: @ 0x4a71a3 → ItemDef_ParseProperty @ 0x49eb00]` — minimal: only what the mission places or the engine spawns by fixed id (witnessed mapping, D-ITEMDEF-1): the player item `105310` (spawned by its own id, never placed) and the mesh-less marker family (`106001` player start, `106003`/`106004` team starts, plus the retail-canonical `100000`/`106002`/`106005`). Model-bearing entries the map does not place are not carried ahead of their models. Ids the engine addresses **by number** are reserved; ids that merely also exist in retail's catalogue are free, and our own content goes at `108001+`. |
| `main.mnu` (`"Startup"` node) | project-authored MNU | the entry screen `[orig: Menu_InitShellResources @ 0x552651 -> UIScene_LoadAndParseContent @ 0x63c830 -> CUIScene_SelectNodeByName @ 0x63b6b0]`. |

### Host + join + single player — mission start + menus

| File | Origin | Notes |
|---|---|---|
| `mnml.bms` | project-authored BMS | the mission: the `106001` player start, both team starts (`106003`/`106004`), one objective, minimal item set. Retail does not spawn the player from a placed entity — it spawns item `105310` by its own id and reads the placed `106001` marker to learn where (confirmed against retail's `00TRa.bms`: 1331 entities, exactly one `106001`, no player entity; the two ids are observed from the shipped data and the spawn behaviour — neither appears as an immediate in `Jointops.exe`, so the binary site that carries them is unwitnessed). |
| `mnml.trn` + `mnml.cpt` | terrain writers (`save_trn` + the CDEP builder) | the terrain config and its baked polydata. JO reads the compressed CDEP depth `[orig: Terrain_LoadLodStorage @ 0x603550 — the 'CDEP' fourcc compare @ 0x603620 and the 'DPTH' compare @ 0x6037b3]`; the BHD-era DPTH the builder defaults to is a `.cpt` retail cannot decode. `sector_count` is the grid WIDTH, not a count of active sectors. |
| `mnml.env` | `engine/formats/env` writer | one time-of-day; defaults elsewhere. The mission header's Q8.8 start hour overrides the `.env`'s own `curtime`, so the mission starts at noon rather than rendering under the midnight ramp. |
| `mp.mnu` | project-authored MNU | the host/join menu `[orig: @ 0x5588fa]`. |
| `sp.mnu` | project-authored MNU | the single-player mission screen `[orig: SinglePlayer_PopulateMissionList @ 0x561840]` — where the packed mission has to appear. |
| `weapon.def`, `ammo.def` | authored text | minimal: one spawn weapon + its ammo `[orig: WeaponDef_LoadAll @ 0x54dd10; AmmoDef_LoadAll @ 0x40b0b0]`. Two entries for ONE rifle, because the engine addresses two weapon names by LITERAL and a set that wants an armed player with a visible viewmodel has to answer both: `WPN_M4AUTO` is the spawn/equip default resolved by name at player spawn `[orig: PlayerClass_InitEntity @ 0x4B1116 -> AvatarDef_FindIndexByName("WPN_M4AUTO")]` (without it the player spawns unarmed), and `WPN_AK47AUTO` is the name the first-person viewmodel bring-up resolves. Both carry the viewmodel slice — `ANIMADM`/`GFX1`/`GFX1A` plus the `pos`/`TPOS` hip and ADS offsets `[orig: WeaponDef_ParseProperty @ 0x54d730; pos/tpos handlers @ 0x54476b/@ 0x54471f]`. `GFX1A` is parse-and-discard in the original — the arms come from the CHARACTER's arms model `[orig: Player_RenderFirstPersonViewModel @ 0x4ded60]` — and is carried for retail-shape fidelity. No `PARTICLE` rows: the set ships no `.ptl` catalogue yet. |
| `game.wac` / `server.wac` | — | optional (silent skip) — add only if the join needs mission logic to progress. |

### Where the mission list looks (witnessed against retail)

Retail builds its mission table from two scans: a loose `FindFirstFile *.bms`
walk of the working directory, and a per-archive entry walk over the
**localres/language volumes only**
`[orig: MissionList_ScanAndBuildFromFiles @ 0x563170; Mission_BuildMapListFromPFF @ 0x562910]`.
A `.bms` archived in `resource.pff` mounts and loads by name and never appears
in any list (witnessed 2026-08-23: the menu simply empty, no error). Retail
keeps its own where the walk looks — all 116 stock JO `.bms` live in
`localres.pff`, none in `resource.pff` — so the release packer names its single
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
| `mnml.pcx` | project-authored PCX | the map overview — retail's are 800×600 8-bit indexed. |
| `mnml.til` | — deferred | the tile overlay; its loader is called ONLY from the render loop (`[orig: @ 0x5ca730, sole caller Render_ProcessMainSceneFrame @ 0x5ca0f0]`), so a miss cannot block boot/host — the terrain just renders untiled. |
| `mnml.wac` | — deferred | witnessed silent skip (see above). |

### The visible-menu set

Boot-clean was not menu-visible: the Startup screen drew text-only buttons on
a **null font slot** — a black screen (validated on retail 2026-07-05; the
`/FRISK` log showed every set file loading and the profile saves proved the
shell was running). The menu shell's own load list is witnessed at
`Menu_InitShellResources @ 0x552500`: `menu_style.mns` → `brand.mns` (append; not even
retail ships one) → the three menu Bink slots (`main.bik`/`header.bik`/
`footer.bik`) → `nw_cdata.coo` → `main.mnu` → `Startup` →
`HUD_InitAllFonts @ 0x51ee20` with **hardcoded font names** (width breakpoints
640/800/1024 pick `Arial12b`/`14n`, `14b`/`14n`, or `16b`/`16n`;
`Impac22b`/`Impac38b` always). So the set authors:

| File | Origin | Notes |
|---|---|---|
| the seven boot `.fnt`s | `engine/formats/fnt` writer | one authored glyph set emitted under every hardcoded name. Retail ships fonts in `localres.pff`. |
| `menu_style.mns` | authored text | the stylesheet the shell loads by canonical name `[orig: @ 0x552604]`; carries the retail key set (key NAMES witnessed vs the JOTAC install — `DEF_FONTNAME`/`DEF_FONTNAME_LG`/`IMPACT_FONTNAME` + colors); values are ours, fonts point at the authored set. |
| `menutxt.bin` | `engine/formats/rtxt` | the `TEXT_RSRC` string ids the authored menus reference (`MM_*`/`MP_*`). Retail ships it in `language.pff`. |
| `newarow1.tga` | authored TGA | **the menu cursor** — declared per screen in the `.mnu` `<CURSOR>` block (retail FILENAME, our arrow art; 32×32 type-2 BGRA, alpha-keyed `STANDARD_TRANSPARENT`, bottom-up rows matching retail's format — `minimal_art_validate` pins that format). Without it there is no cursor and nothing is clickable. |
| `menumus.sbf`/`gamemus.sbf` + `menumus.bin`/`gamemus.bin` | `engine/formats/sbf` + the `mus` compiler | the hardcoded base-game music pairs `[orig: Expansion_LoadAssets @ 0x4a4730]`: a silent bank written LOOSE at the root (the `.sbf` banks stream by path and never resolve through an archive) + a minimal `play/done` script per name in `localres.pff`. |
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
way. One LF-normalizing save silently breaks the boot again, so
`minimal_eol_guard` (ctest) pins all of them.

### Deliberately omitted (graceful-on-miss — keeps the set minimal)

Videos (`BIK` — see above), `powerup.def` (soft), `hudfx/hudpos.def` (default
positions), `game.bin` (fallback literals), `nw_cdata.coo`. Each is listed in
the R8 manifest with its graceful failure; adding any is a deliberate step up
from minimal, not a requirement. `Avatars.def` left this list with the
first-person arms, `charattr.def` rode the same bring-up, and `SndProf.def`
left it 2026-08-31: its "soft miss" survives boot but not a moving player —
the empty profile table's find-miss path hands the first footstep a garbage
sound-set pointer and retail AVs in `Sound_Play3DPositional` reading set+72
(`[orig: @ 0x527cd1]`; witnessed via SYSDUMP against this exact set). All
three are retail files under the TEMPORARY banner — see the bring-up section
below.

### Bring-up: the retail model + anim set (retail bytes, committed temporarily)

The set has no models of its own yet, so the player body, its animations and the
first-person viewmodel are brought up by **copying the retail files into this
directory**. Since 2026-08-31 they are **committed** — allowlisted by name under
`.gitignore`'s TEMPORARY banner (the policy exception above) — so the minimal
set runs from a fresh checkout, and `git ls-files assets` now stages them into
both zip flavors. Deleting a line from that banner is how a replacement lands.

What is committed, from an extracted retail resource tree (213 files, trimmed
2026-08-31 to what the validated retail run actually loads — see the trim note
below):

| Group | Files |
|---|---|
| player body + anims | `US01.3di`, `US01.ADM`, the **150** `.bad` clips its keys name, and `failsafe.bad` (the every-mission-start fallback, `../docs/required-resources.md`) |
| avatar combo | `Avatars.def` plus the first combo's three models — `Boonie.3di` (head), `JntOpsB1.3di` (body), `ArmsG.3di` (arms) |
| sound profiles | `SndProf.def` — the 49-profile retail table; a miss AVs the first footstep (see the omitted list) |
| weapon | `AKM_1st.3di`, `AKM_1ST.adm`, its six `rAKM_*.bad` clips |
| the default infantry clip set | `E_STAND.adm` and the shared subset of the `.bad` clips it names — the file itself is **required for the player to walk at all**, see below |
| textures | the stems the five models name, resolved to whatever extension ships them — 33 `.dds`, one `.tga`, plus the seven `.MDT` normal-map fills the materials actually request |

The `.fx` shaders are **no longer retail bytes**: since 2026-08-31 the set
ships our own authored effects (see "Shaders" below), so they moved out of
this banner entirely.

**The 2026-08-31 trim.** The validated retail run's `/FRISK` load log is the
witness for what this set needs: every committed retail file was diffed
against it, and the 27 files nothing loaded were deleted — the 25 `.bad`
clips only `E_STAND.adm` names (AI behavior clips; `mnml` fields no AI, and a
clip missing from a ring resolves to the RESET ring on both engines) and two
`.MDT` fills no material requests (`ACM2INDO.MDT`, `AUS1_HD1.MDT`).
`E_STAND.adm` itself stays: retail never loads it in this flow, but OUR
kernel's default-set gate does (the coupling below).

Three things about this set are worth knowing. **`E_STAND.adm` is load-bearing far
beyond the AI bodies it names.** Infantry locomotion is entirely root-motion driven —
the playing clip's translation track moves the entity, and there is no non-clip fallback
(`engine/runtime/world/infantry.cpp` header) — and the kernel resolves each entity's own
`items.def` `anim_def` only once the DEFAULT set
(`engine/runtime/mission/runtime_boot.h:35`, `"E_STAND.adm"`) has registered
(`engine/runtime/mission/mission_kernel.cpp:253`/`:299`). Without it `ai.root_motion` is
null, the local player never picks up `US01.adm`, and it spawns, renders and plays
`anim_idle` forever while refusing to walk — with the only symptom a single
`no infantry clips from 'E_STAND.adm'` warning. The coupling itself is tracked in
[`../TODO.md`](../TODO.md); until it is undone, this file is part of the minimal set.

Two further things. **`Avatars.def` moves the body
lookup**: with it present the local player's body is the avatar combo's head +
body pair, not `items.def`'s `graphic`; `US01` stays the fallback body and, as
`anim_def`, the clip map that drives the rig either way. And **every model names
its textures `.tga` while retail ships them `.dds`** — retail relies on its own
`.dds` substitution probe `[orig: Texture_LoadByNameWithChannel @ 0x58b52c]`,
and **the probe runs for loose files under `/d` too** (witnessed 2026-08-31 on
this set: the models ask `<stem>.tga`, miss loose, and all 37 staged loose
`<stem>.dds` load — `/FRISK` capture), so the loose layout needs no texture
archive. The 2026-08-18 "ship `.dds` archived" trap applies to a loose file
matching the request's *truncated literal* name, not to substitution.

This set is replaced by our own once that run is green. The model side already
has a path — `tests/fixtures/minimal_3di_builder.h` mints a nineteen-part skinned
`person` rig in the retail bone order through `threedi_3di3_write`. The clip side
needs a `.bad` **writer** first: `engine/formats/bad/bad.h` is parse-only today.

## Shaders: the authored `.fx` set

The 14 committed `.fx` files are **ours** — authored HLSL implementing the
witnessed effect contract (tags, technique/pass annotations, parameter names,
the fixed-function variant matrix), replacing the 47 retail files the bring-up
first carried. The plaintext sources live in `tests/fixtures/fx/`; the
committed artifacts are those sources wrapped in the SCR container retail's
effect loader requires — `'SCR',0x01` + the shader keystream, 0xA55B1EED
`[orig: ScriptFile_LoadAndDecrypt @ 0x5AE060]`; a bare-text `.fx` is rejected
(NULL) by that sniff, which is why the wrapped form is what ships. Eight are
effects (`_ffp.fx` — the 24-variant fixed-function matrix, loaded by literal
name; `phongt.fx` for the rifle's `VS_PHONGT`; the six skinned tags the
body/arms/head models carry) and six are shared includes (underscore-prefixed
like retail's, so the loose override walk skips them). The set covers exactly
the seven effect tags the committed models reference — the other retail
effects (glass, mirrors, tracer, flag, foliage, ...) return when models that
name them do.

`minimal_fx_gen` regenerates and guards the pair (`--write` after editing a
source); `fx_compile_validate` compiles every source through
`D3DXCreateEffect` under the loader's define sets — the loud version of
retail's silent boot drop (skips where D3DX9 or a D3D9 device is missing).
Validated on retail 2026-08-31: all 14 PFF-loaded, FP viewmodel drawn.

## Guards (ctest, run in CI)

The tracked files here are the canonical source-owned outputs, not packaging
artifacts. The guards check what the ENGINE and RETAIL need from each file
rather than asserting byte-equality against a throwaway generator:

| Guard | Covers |
|---|---|
| `minimal_rtxt_gen` | the string tables emit + round-trip |
| `minimal_def_validate` | `items.def` / `weapon.def` / `ammo.def` parse through `engine/formats/def` |
| `minimal_mnu_validate` | `main.mnu` (Startup), `mp.mnu` (LAN host/join), `sp.mnu` (single player) parse and carry their screens |
| `minimal_map_validate` | `mnml.env` loads; `mnml.bms` parses, places exactly one `106001` and both team starts, names the terrain, starts in daylight |
| `minimal_trn_gen` | `mnml.trn` round-trips, keeps the 8-wide sector grid + quadrant block, names exactly the shipped `mnml_*` art (`minimal_trn_gen_test --write` re-emits the config) |
| `minimal_art_validate` | every image `mnml.trn` names decodes; the colormap is big enough to quadrant-split; the cursor is a 32×32 type-2 32 bpp alpha TGA |
| `minimal_eol_guard` | every hand-authored text file is CRLF |
| `minimal_fx_gen` | each committed `.fx` byte-equals wrap(its `tests/fixtures/fx/` source) and no stray `.fx` rides in assets/ |
| `fx_compile_validate` | every authored effect compiles through `D3DXCreateEffect` under the loader's define sets (Skipped without D3DX9/D3D9) |

## Packaging

`tests/fixtures/minimal_pff_package.cpp` bundles the committed tree — it reads
`assets/` and never writes into it. Two layouts:

- `minimal_pff_package_test --write-pff` writes the **three boot-table
  archives** into this root (gitignored), each file in the archive retail uses for its kind
  (witnessed against the JOTAC JO install): `language.pff` = the boot text
  bins + the mission-family `.bin`/`.pcx`/`.lwf`, `localres.pff` = the menus,
  defs, mission, fonts and music scripts, `resource.pff` = the map and terrain
  (`.env`/`.trn`/`.cpt`/source art).
- `minimal_pff_package_test --install <dir>` assembles a runnable **loose**
  install: everything flat plus the shader-bearing `resource.pff` (it clears the
  boot gate AND carries the `.fx` set the PFF-walk-only precompile needs), run
  with `/d`.
  With `OPENNOVA_JO_DIR` set it also stages `Jointops.exe` + `binkw32.dll` +
  `game.cfg` from your own install (never committed).

ONED's Stage & Run Retail is the everyday compatibility loop: it stages the
selected tree loose under ONED's own data dir (`user://packed`), adds
the zero-entry `resource.pff` boot token, stages the retail runtime beside it,
and runs it with `/d`; Stop ends that managed child. The hidden `--pack-game`
command instead builds the packed `localres.pff` layout used by releases.

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

Validated on retail 2026-08-31: boot → menu → `mnml` → an armed player that
walks with a drawn first-person viewmodel, all from this set loose + the
shader-bearing `resource.pff`.

Known gaps: the shaders are ours (the authored `.fx` set above), but no model
or clip here is ours yet — the player body, its animations and the viewmodel
are the committed retail bring-up set described above, and the
committed tree still declares graphics it does not carry. The terrain has
relief and a full-size colormap but no tile overlay, and there is no `.ptl`
catalogue, so the weapon authors no muzzle-flash or casing effect.

## MVP convergence

When retail↔retail works on this set, it is simultaneously (a) the **MVP asset
target** — the exact inputs our runtime must load to host + join our own
engine — and (b) a **parity proof** that every writer in the chain
(rtxt/mission/trn/env/mnu/def/fnt/sbf/pff) emits retail-loadable output end to
end. The package-time standalone game build starts here.
