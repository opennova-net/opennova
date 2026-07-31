# Required resources — reverse-engineering record (R8 / ENG-6)

The original engine's **boot-required, hardcoded-by-name resource set**: every
file `Jointops.exe` demands by literal name to boot to the main menu and to
start a mission, with the witnessed failure behavior for each. This defines
"what a person starts with to make a new game" — the source of truth for the
ENG-6 engine-side manifest (Wave 2: a table in `libs/` near gameprofile that
BOTH the game's boot validation and ONED's diagnostics consume) and for the
ONED REQ conveniences (missing-required diagnostics, the "new game" scaffold).

Binary: retail **Jointops.exe** (IDB `Jointops.exe.kong.i64`, imagebase
0x400000) — all addresses below are that binary's. Produced by a read-only
IDA session (R8, 2026-07-05); no IDB renames were made. This file is the
committed home for the `D-BOOT-…` divergence catalog.

Overlap note: the PFF mount mechanics themselves (search order, loose-first
resolution) are the PAR-R7 audit's territory (`docs/README.md`, VFS/PFF mount
stack — UNAUDITED); this record pins only the *names* and the boot contract.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Boot chain (WinMain → menu) resource order | **witnessed** | `Game_Run @ 0x4a7fb0` → `Game_InitSubsystems @ 0x4a6cd0` → MainMenu enter `sub_552500 @ 0x552500`; ordered sequence below |
| The fatal set (boot refuses without them) | **witnessed** | PFF set (all-missing) `@ 0x4a6f44`; `gametext.bin @ 0x4a6fed`; `vmacros.bin @ 0x4a702f`; `keyhelp.bin @ 0x4a7072`; `items.def @ 0x4a71a3` (fatal wired; see Not-witnessed) |
| Mission-start resource set | **witnessed** | `Game_StartMission @ 0x524360` chain below |
| Host conformance | **one divergence minted** | D-BOOT-1 (music-bank name resolution); hudpos/main.mnu literals match |

## The fatal set (boot exits or dead-ends without these)

| Resource | Failure behavior [orig] |
|---|---|
| `resource.pff` / `localres.pff` / `language.pff` (+ expansion `<n>.pff`/`<n>L.pff` slots) | **zero archives opened → `earlyerr.txt` line-3 dialog + exit** [orig: PFF_OpenAllArchives @ 0x4a4310 over the name table @ 0x829f90; fatal check Game_InitSubsystems @ 0x4a6f44]. Any individual archive missing is tolerated; only all-missing is fatal. Loose-first only under `/d` or a consumer-forced override; the default is archive-only (corrected by PAR-R7, [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md)). |
| `gametext.bin` | "Unable to load game strings" MessageBox + **exit** [orig: Game_InitSubsystems @ 0x4a6fed] |
| `vmacros.bin` | "Unable to load voice macro strings" MessageBox + **exit** [orig: @ 0x4a702f] |
| `keyhelp.bin` | "Unable to load keyboard map strings" MessageBox + **exit** [orig: @ 0x4a7072] |
| `items.def` | fatal "Unable to load items.def" wired at the callsite [orig: @ 0x4a71a3 → Game_FatalErrorWithMessageBox @ 0x4a5160] — but see Not-witnessed: the loader as decompiled always returns success |
| `main.mnu` (node `"Startup"`) | menu never appears (no dialog) [orig: sub_552500 @ 0x552651 → UIScene_LoadAndParseContent @ 0x63c830 → CUIScene_SelectNodeByName @ 0x63b6b0] |

`gameerr.bin` sits just below fatal: missing → `earlyerr.txt` line-4 dialog,
then boot **continues** [orig: @ 0x4a6fc8]. `earlyerr.txt` itself is the
pre-archive error text (loose beside the exe); if it too is missing the dialog
degrades to "Error: Unable to open EARLYERR.TXT"
[orig: Game_ShowEarlyError @ 0x4a68a0].

## Full findings

### Boot (Game_Run → Game_InitSubsystems)

| Resource (literal) | Category | Consumer [orig] | Failure behavior |
|---|---|---|---|
| `game.cfg` | optional-fallback | [orig: Game_LoadConfig @ 0x551480 via File_ParseASCIIFileWithCallback @ 0x53d980; called @ 0x4a7fbb, @ 0x4a70ab, menu enter @ 0x552534] | missing → silent Config_SetDefaults @ 0x54d030; version key != 0x1D → re-defaults |
| `fgn2.bin` | optional-fallback | [orig: @ 0x5514e8] | existence flag only (`byte_24D4DF9`), never parsed |
| `assets.cd` | optional-fallback | [orig: Game_ReadAssetsCDFile @ 0x4a5800] | missing → empty string, silent |
| `CC.BIN` | optional-fallback | [orig: Game_ReadCCBinFile @ 0x4a5860; value @ 0x4a5950] | missing → empty, silent (country code, XOR 0xABADABAD @ 0x4a6dd7) |
| `filter.txt` | optional-fallback | [orig: ChatFilter_LoadFromFile @ 0x4fd640] | silent skip |
| `expansion\<n>\<n>.pff`, `<n>L.pff` | expansion discovery | [orig: Expansion_ScanAndRegister @ 0x4a43d0; Expansion_LoadAssets @ 0x4a4730] | absent → expansion unregistered, base game proceeds; `<n>.bin` gives EXP_NAME/EXP_DESC (fallback "Unnamed Expansion"); `version.txt` → CRC |
| PFF set + `gameerr.bin`/`gametext.bin`/`vmacros.bin`/`keyhelp.bin` | **fatal set** | see table above | see table above |
| `weapon.def` | boot + mission | [orig: WeaponDef_LoadAll @ 0x54dd10; mission slots @ 0x5254b8] | missing → silent; table left with one "None" entry. SCR-encrypted supported, key 0x2A5A8EAD |
| `Avatars.def` | optional-fallback | [orig: CAvatarDefs_Init @ 0x57b180] | silent skip |
| `SndProf.def` | boot (mission-facing) | [orig: @ 0x4a7141 → SoundProfile_LoadAll @ 0x527490] | silent skip (empty 128-slot profile table) |
| `gt.ssc` | optional-fallback | [orig: Mission_LoadEncryptedConfig @ 0x4cdcd0] | silent skip (gate-tag override, key "jop:2:oyez") |
| `items.def` | **fatal (wired)** | [orig: @ 0x4a71a3 → ItemDefs_LoadAndValidate @ 0x4a1da0 → ItemDef_ParseProperty @ 0x49eb00] | dup-ID/name checks log to `_errlog.txt`; see Not-witnessed |
| `charattr.def` | boot (soft) | [orig: Game_Run @ 0x4a7fe3 → CharAttr_LoadFromDef @ 0x412140] | missing → `_errlog.txt` "Server ERROR! Could not load charattr definitions.", continues |
| `loading.pcx` | boot (soft) | [orig: Game_ShowLoadingScreen @ 0x4a544f] | graceful texture-miss pattern (not stepped through) |
| `admin.cfg` | optional-fallback | [orig: CAdminServer_LoadConfig @ 0x406d80; callsite @ 0x4a72c2] | silent skip |
| `*.npj` + `*.npz` (wildcard scan) | boot (mission list) | [orig: MissionList_ScanAndBuildFromFiles @ 0x563170] | none found → empty mission list (the only wildcard scan at boot) |
| `hiscore.txt` | optional-fallback | [orig: HUD_LoadHighScoreText @ 0x5630e5] | silent skip |

### Main menu (sub_552500 enter)

| Resource (literal) | Category | Consumer [orig] | Failure behavior |
|---|---|---|---|
| `game.bin` | menu-required | [orig: @ 0x552510, lazy resource @ 0x25510f8] | load unchecked; menu strings absent if missing |
| `player.sav`, `weapon.sav` | optional-fallback | [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0 (@ 0x54f586, @ 0x54f6c7)] | missing/bad magic → PlayerProfile_InitDefaults @ 0x54bb40, first-run flag; `weapon.sav` resolved `expansion\<n>\` first |
| `prolog.BIK`, `intro.BIK` | optional-fallback | [orig: Game_PlayIntroVideos @ 0x5637a0] | exists-checked, skipped |
| `MENUMUS.SBF` + `MENUMUS.BIN` | menu music | [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60; names set Expansion_LoadAssets @ 0x4a4798] | graceful; expansion form `M<n>.sbf`/`M<n>.bin` |
| `menu_style.mns`, `brand.mns` | menu-required | [orig: @ 0x552604 / @ 0x552616 via NapiConfigMap_LoadIncludeFile @ 0x63b970] | silent skip (unstyled UI); brand appended after style |
| `main.bik`, `header.bik`, `footer.bik` | optional-fallback | [orig: UI_CreateMenuBinkVideos @ 0x54b590; strings @ 0x7d2a00–0x7d2a54] | expansion → default path fallback; missing → no menu video |
| `nw_cdata.coo` | menu-required | [orig: @ 0x55262b → sub_63A500] | HRESULT ignored |
| `main.mnu` (`"Startup"`) | **the entry screen** | see fatal set | menu dead-ends silently |
| `menutxt.bin` | menu (lazy) | [orig: UIStringTable_LookupAndDup @ 0x63b290; e.g. @ 0x55840e, @ 0x5561b7] | fallback literals used (`nw_error.mnx` hardcoded @ 0x558449) |
| `Arial12b/14n/14b/16n/16b.fnt`, `Impac22b.fnt`, `Impac38b.fnt` | menu+HUD fonts | [orig: HUD_InitAllFonts @ 0x51ee20 → sub_580400] | missing → null font slot, scale 1.0, no crash (width breakpoints 640/800/1024) |
| `menu.lwf` | menu (player screen) | [orig: PlayerInfo_InitProfileSelector @ 0x5613ba] | graceful |
| `PI_Idle.BAD` (+ `PI_actv/PI_LookR/PI_lookL.BAD` table @ 0x83c830), `Dt1rst.bad`, `HwmCube.dds` | menu (player preview) | [orig: PlayerInfo_InitPreviewModel @ 0x5600d0 (@ 0x560107/@ 0x560138)] | graceful |
| `epass.bin`, `passgen.bin` | optional-fallback | [orig: load_stored_credentials @ 0x450b20 / EPass_LoadCredentials @ 0x450eb0] | silent skip |

### Mission start (Game_StartMission @ 0x524360)

| Resource (literal) | Category | Consumer [orig] | Failure behavior |
|---|---|---|---|
| `failsafe.bad` | mission-required (fallback anim) | [orig: AnimMap_Init @ 0x40be96; AnimMap_FindOrLoadBoneFile @ 0x40c285] | it IS the fallback when a mission `.bad` is missing; loaded every mission start |
| `<exp>L.lwf`, `<exp>.lwf`, `gamelocl.lwf`, `game.lwf`, `game3.lwf`, `game2.lwf` | mission sound banks | [orig: @ 0x525443 loop over the 260-byte-stride table @ 0x82a5b0 via SoundBank_LoadIfExists; names @ 0x82a7b8+; expansion slots @ 0x4a4972/@ 0x4a499d] | exists-checked per slot, silent skip |
| `ammo.def` | mission-required | [orig: @ 0x52548a → AmmoDef_LoadAll @ 0x40b0b0] | silent (empty ammo table); key 0x2A5A8EAD |
| `powerup.def` | mission-required | [orig: @ 0x5256cd → sub_443350] | `_errlog.txt` "Unable to load powerup.def", continues |
| `<missionbase>.bin` → `medmssn.bin` | mission text | [orig: TextResource_LoadMissionTextBin @ 0x51ed90] | mission-named exists-checked; literal fallback `medmssn.bin` |
| `game.wac`, `server.wac`, `<missionbase>.wac` | mission scripts (authority) | [orig: WacScript_InitAndLoad @ 0x4f91f0 (game @ 0x4f9454, server @ 0x4f94bc)] | each exists-checked, silent skip; compiled game→server→mission into one buffer |
| `GAMEMUS.SBF` + `GAMEMUS.BIN` | mission music (MP) | [orig: @ 0x525581–0x525598 → AudioVM_OpenMusicContext @ 0x6722a0; names @ 0x4a47da] | graceful; expansion form `G<n>.sbf`/`G<n>.bin`; SP stops the music context. Full driving witness (var writer map, the always-0 Var1, the dead WAC `music` stream via `Sbf_OpenFile_Gamemus @ 0x4ed6c0`): docs/audio/mus-sbf-re.md §Game music driving |
| `<missionbase>.pcx` → `loadscrn.pcx`, `Arials18.fnt`, `Arial22.fnt` | mission-load screen | [orig: render_loading_screen @ 0x521d10 (sidecar probe @ 0x521db5, fallback @ 0x521e20)] | graceful; per-mission image exists-checked first — see [interface/loading-screen-re.md](interface/loading-screen-re.md) |
| `cmap.mnu` | mission UI | [orig: @ 0x526316/@ 0x526332; Input_HandleActionBinding @ 0x49b91b] | unchecked. Siblings: `game.mnu @ 0x49b3b1`, `weapon.mnu @ 0x49b8de/@ 0x4e0b44`, `vehicle.mnu @ 0x49b892/@ 0x4e0af8`, `stat.mnu` [orig: UI_ProcessEndRoundScreenTransition @ 0x5b8636], `death.mnu` [orig: Render_ProcessMainSceneFrame @ 0x5cab7e], `mp.mnu` [orig: @ 0x5588fa] |
| `hudfx.def`, `hudpos.def` | mission HUD | [orig: HUD_InitOverlaySystem @ 0x5a4620 (hudfx @ 0x5a462e, hudpos @ 0x5a4931)] | silent skip (default positions) |
| `monogram.tga`, `boxtile.tga`, `border.tga` | mission UI textures | [orig: @ 0x525aa3–0x525aad] | graceful (plus the hardcoded HUD/effect texture sets: `MFD1.PCX @ 0x7d90dc`, `cross%02d.tga`, scorch/glass tables @ 0x8413a8+) |
| `upl.3di` | mission (celestial) | [orig: EffectWorld_LoadCelestialModels @ 0x5add25] | lazy, null on miss; sun/moon/glare/star model names are data-driven from the mission `.env`; hardcoded 3rd-person weapon/vehicle `.3di` table @ 0x83b490+ |
| `couri20b.fnt` | optional | [orig: @ 0x572a67 / @ 0x572f24] | graceful |

Write-side / debug outputs (not boot inputs): `SS%0.5d.tga`, `_errlog.txt`,
`_netlog.txt`, `SYSDUMP.TXT`, `activesrvr.txt`, `mru.txt`,
`hello.bin`/`hello2.bin` [orig: ChunkFile_TestWriteAndReload @ 0x56f810].

## Ordered boot sequence (witnessed)

1. `WinMain @ 0x763a80` → `Game_ParseCommandLineAndInit @ 0x4a7310` →
   `Game_CreateMainWindow @ 0x761ed0` → `Game_Run @ 0x4a7fb0`.
2. `Game_Run`: `game.cfg` (+ `fgn2.bin` flag) → `Game_InitSubsystems @ 0x4a6cd0`:
   `assets.cd` → `CC.BIN` → `filter.txt` → firewall-probe socket →
   expansion discovery → `Expansion_LoadAssets` (sets pff/music/lwf/text
   names) → **PFF mounts** [fatal if none] → `gameerr.bin` [dialog] →
   **`gametext.bin`** [fatal] → **`vmacros.bin`** [fatal] → **`keyhelp.bin`**
   [fatal] → `weapon.def` → `game.cfg` (again) → `Avatars.def` → video
   enumeration → `SndProf.def` → `gt.ssc` → **`items.def`** [fatal wired] →
   team names (from gametext.bin) → display mode → `loading.pcx` →
   audio/particles/render init → mission-list scan (`*.npj`/`*.npz`) →
   `hiscore.txt` → `admin.cfg`.
3. `Game_Run` cont.: `charattr.def` → `Game_MainLoop @ 0x52b630`, state 2 =
   "MainMenu" (state record @ 0x83b400).
4. MainMenu enter (`sub_552500`): `game.bin` → `game.cfg` →
   `player.sav`/`weapon.sav` → intro BIKs → menu music
   (`MENUMUS.SBF`/`.BIN`) → `CUIManager_Create` → `menu_style.mns` →
   `brand.mns` → menu BIKs → `nw_cdata.coo` → **`main.mnu`** ("Startup") →
   HUD fonts.
5. Mission start (`Game_StartMission @ 0x524360`): `failsafe.bad` → LWF banks
   → `<mission>.dbf` → `ammo.def` → `weapon.def` (slots) → `powerup.def` →
   `<mission>.bin`/`medmssn.bin` → WAC set → `GAMEMUS` (MP) →
   `loadscrn.pcx` + fonts → `cmap.mnu` → `hudfx.def`/`hudpos.def` → UI
   textures → `upl.3di`. Mission terrain/env/bms names are data-driven from
   the mission-list entry.

## Host conformance + divergence catalog (D-BOOT)

- `godot/game/main_game.gd` loads `hudpos.def` by the retail literal; ours
  warns on missing where retail silently skips — a host-side diagnostic on
  the same non-fatal behavior, not a divergence.
- `godot/modtools/tools/screenshot_capture.gd` seed list: `main.mnu` ✓,
  `hudpos.def` ✓; its `jo_gamemus.bin` is a fixture rename, not an engine
  name.
- Boot validation itself (refusing/erroring on the fatal set the way retail
  does) is the ENG-6 Wave-2 manifest deliverable, not a divergence row: the
  honest missing-resource errors land with the manifest table.

| ID | One-liner | Class | Disposition |
|---|---|---|---|
| D-BOOT-1 | Menu/game music bank resolution: retail hardcodes `MENUMUS.SBF/.BIN` + `GAMEMUS.SBF/.BIN` (`M<exp>`/`G<exp>` under an expansion) [orig: Expansion_LoadAssets @ 0x4a4798/@ 0x4a47da; AudioVM_InitMenuMusicStreaming @ 0x56aa60; Sbf_OpenFile_Gamemus @ 0x4ed6c0]; `godot/game/nova_menu_shell.gd` instead scanned `.bin`/`.mus` names containing "mus" from loose paths only | A | FIXED — `nova_menu_shell.gd` `_resolve_music_pair` resolves the witnessed hardcoded pairs (expansion `M<n>`/`G<n>` first, then the base pair, same-stem halves): `.bin` scripts load by name through the VFS (PFF-aware), `.sbf` banks stream loose by path; the game pair's bank swaps onto the director with its script |

## Not witnessed / follow-ups

- `ItemDefs_LoadAndValidate @ 0x4a1da0` always returns 1 as decompiled, so the
  wired boot fatal for `items.def` appears unreachable in retail; the actual
  missing-file behavior (empty item table downstream) was not runtime-verified.
- PFF name-table slot 5 (@ 0x82a4a4, count 6): no writer xref found — likely a
  reserved/mod slot.
- `pre.mnu` (@ 0x569445), `hud.def` (@ 0x5be21e), `help.wac` (@ 0x4f6de6):
  referencing code is outside IDA-defined functions; consumers unconfirmed.
- Menu font names resolved by `CUIManager_Create @ 0x64af10` are data-driven
  (expected from `menu_style.mns`); the specific `.fnt` names were not traced.
- `menutxt.bin`'s first load-from-archive site was not individually traced
  (witnessed only as lazy lookups).
- ~~Wave-2 deliverable: the engine-side manifest table~~ **LANDED (ENG-6,
  Wave-2 trunk)**: `libs/gameprofile/required_resources.h` instantiates this
  record (phase-major witnessed order, severity classes, per-row failure text
  + citation; `required_resources` ctest pins the fatal set and completeness).
  `NovaResourceRoot.list_missing_boot_resources()` /
  `boot_resource_failure_text()` probe the individually-fatal file rows
  against the mounted root (the archive-table trio stays `mount_runtime`'s
  own gate), and the game shell raises honest missing-resource errors at
  mount (`main_game.gd`, reported-not-enforced — the picker flow keeps a
  partial dir inspectable where retail MessageBox-exits). Edits to this
  record and the table land in the same change. ONED's diagnostics/new-game
  scaffold conveniences over the same table remain ONED-REQ (Wave 3).
