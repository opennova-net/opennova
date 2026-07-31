# Avatars.def / player-info avatar selection — reverse-engineering record

Engine-research record for the Joint Operations `Avatars.def` system: the data
file that defines selectable player characters (modular head/body/arms parts
composed into `combo` entries under a `nationality → division` tree) and the
`PLAYER_INFO` menu that surfaces them. Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`, imagebase `0x400000`). All addresses below are that
binary's.

**Status: implemented and IDA-grilled for the parser/data model/editor bridge
(2026-06-16); the full `PLAYER_INFO` screen runtime orchestration was grilled
(read-only) on 2026-06-23 and the runtime `player.mnu` host landed the same
day.** `libs/avatars`, `NovaAvatarDatabase`, and the ONED Avatars workspace
implement the witnessed loader semantics below; the in-game menu population is
live as `PlayerInfoMenuCompanion` (godot/game/player_info_menu_companion.gd) — the
nat→div→combo cascade, team filter, RTXT display resolve, and 3D preview
(D-PLAYERINFO-7 FIXED), the voice preview (D-PLAYERINFO-10 FIXED 2026-07-22),
and the loadout weapon lists. Open residuals: profile persistence beyond the
callsign (D-PLAYERINFO-9), the ammo combos + weight readout (D-PLAYERINFO-11),
and the per-(slot, team) selection globals (D-PLAYERINFO-12). The runtime
combo -> spawned-player 3D model binding remains unwitnessed/open as
**D-PLAYERINFO-1**.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| `Avatars.def` grammar + parser | **matching** | full decompile of `CAvatarDefs_ParseConfigLine @ 0x57a3f0`; implementation re-verified on 2026-06-16 for part cap, duplicate slots, byte truncation, parse-time combo resolution, and required/optional combo refs |
| Avatar object layout (combo / nationality / division / part structs) | **matching** | allocators decompiled (`@ 0x579f40` / `@ 0x579ff0` / `@ 0x579e10`); `libs/avatars` keeps writer reference names but stores parse-time denormalized part snapshots for runtime/editor consumers |
| `PLAYER_INFO` menu consumption | **matching (read-only grill)** | `PlayerInfo_PopulateNationalityList @ 0x55d8c0`, `PlayerInfo_PopulateDivisionList @ 0x55da50`, `populate_avatar_combo_list @ 0x560210` decompiled; ONED editor exposes the same tree, alignment, and resolved-combo data but does not implement the in-game menu UI |
| combo → spawned-player 3D model binding | **unwitnessed (time-boxed)** | consumer set identified but not traced — see **D-PLAYERINFO-1** follow-up |
| second `AvatarDefs_Init` path (`@ 0x53d281`/`@ 0x53d2b4`) | **unwitnessed** | flagged follow-up; different buffer sizes, also parses `Avatars.def` |
| `PLAYER_INFO` screen orchestration (init + 28-control registration + nat→div→combo cascade + team) | **matching (ported 2026-06-23)** | `PlayerInfo_InitProfileSelector @ 0x5611b0`, `PlayerInfo_PopulateAllControls @ 0x5606f0`, `PlayerInfo_RegisterAllControls @ 0x561470`, cascade handlers `@ 0x560600`/`@ 0x560690` decompiled; ported as `PlayerInfoMenuCompanion` — cascade + team filter + RTXT resolve wired by control name onto the `.mnu`'s own control tree, pinned by `player_info_menu_seam_test` (D-PLAYERINFO-7 FIXED; the per-(slot, team) selection globals remain D-PLAYERINFO-12) |
| Voice preview + PLAYERVOICE list | **matching (ported 2026-07-22)** | `PlayerInfoMenuCompanion` binds the real `TESTPLAYERVOICE` control and requests the selected avatar's `VOICE_%d` trigger through `menu.lwf`; `player_info_menu_seam_test` pins the public sound request. Persisted profile overrides remain part of D-PLAYERINFO-9. `[orig: PlayerInfo_PreviewVoice @ 0x55ff70; PlayerInfo_HandleVoiceSelect @ 0x55fe00]` |
| ACCEPT commit + profile persistence | **partial (seam + callsign ported)** | `save_player_info_from_dialog @ 0x55ee10` decompiled; the host wires ACCEPT → `commit`/`avatar_chosen` and main_game persists the callsign (`NovaPlayerProfile.save_callsign` → `user://player_profile.cfg`); persisting + restoring the avatar/class/loadout selection remains D-PLAYERINFO-9 (selection-state globals D-PLAYERINFO-12) |
| Loadout weapon lists (PRIMARY/SECONDARY/ACCESSORY) | **matching** | producer `WeaponDef_ParseProperty @ 0x54d730` + consumer `populate_weapon_slot_lists @ 0x560430`; ported in `libs/def` (`DefWeaponDef` loadout fields + `def_parse_weapons_memory`) + `NovaWeaponDatabase` + the host's `_populate_loadout` (class/team filter, NONE-first). Pinned by `def_parse_weapons` ctest + `player_info_menu_seam_test` (D-PLAYERINFO-8/11) |
| Loadout ammo combos + weight readout + icons | **matching (ported 2026-07-30)** | `populate_weapon_accessory_ammo_ui @ 0x55e8b0`, `populate_ammo_combo_boxes @ 0x55def0`, `update_player_info_weight_and_weapon_icons @ 0x55f480`, `calculate_loadout_weight @ 0x55f1f0` fully decompiled + the handler map off `PlayerInfo_RegisterAllControls @ 0x561470` (see "Ammo combos, weight, and icons" below); ported in `PlayerInfoMenuCompanion` (`_populate_slot_ammo`/`_populate_grenades`/`_update_weight`/`_update_icons`) over the `NovaWeaponDatabase.loadout_weight`/`encumbrance_class` bindings; `player_info_menu_seam_test` pins the row models, labels, defaults, the `flags2 0x40` type lock, the subclass walk, the weight format, and the snapshot clips (D-PLAYERINFO-11 FIXED; saved-kit restore rides D-PLAYERINFO-9) |

## Load entry — witness map

- `[orig: CAvatarDefs_Init @ 0x57b180]` — `__thiscall(this)`. Stores `this` in
  the singleton `g_pAvatarDefs` (`dword_2697F08`); zeroes the avatar object:
  `*this = 0` (combo count), `memset(this+4, 0, 0x9000)` (combo array),
  `memset(this+0x9008, 0, 0x4D00)` (nationality array); then
  `File_ParseASCIIFile("Avatars.def", sub_57B160, key=0x2A56F6AD)`. The
  `"Avatars.def"` literal is `@ 0x7d76c0`.
- Called from `[orig: Game_InitSubsystems @ 0x4a6cd0]` at `0x4a70b5`, immediately
  preceded by `mov ecx, offset count_and_entries` (`@ 0x4a70b0`) — so **the
  avatar object IS the static `count_and_entries @ 0x26A7748`**. Load order in
  `Game_InitSubsystems`: avatars are parsed *before* `items.def`
  (`ItemDefs_LoadAndValidate @ 0x4a1da0`, decrypt key `0x2A5A8EAD`), `SndProf.def`,
  `Team_LoadNames @ 0x4fcff0`, and the mission scan.
- `[orig: sub_57B160 @ 0x57b160]` — thin line callback; forwards
  `(tokens, tokenCount)` to `CAvatarDefs_ParseConfigLine` on the `g_pAvatarDefs`
  singleton.

`File_ParseASCIIFile` pre-tokenizes each line (whitespace split) and invokes the
callback per line with `tokens[1]` = first word. The decrypt key `0x2A56F6AD` is
the NovaLogic per-file ASCII key for `Avatars.def` (distinct from `items.def`'s
`0x2A5A8EAD`); a reimpl loader decrypts to plaintext before tokenizing, and the
parser itself operates on plaintext tokens.

## Grammar / parser — witness map

`[orig: CAvatarDefs_ParseConfigLine @ 0x57a3f0]` —
`__thiscall(avatarDefs, const char **tokens, int tokenCount)`. A brace +
scope-depth state machine: scope depth `g_scopeDepth = dword_2697F0C`; the state
array `dword_2697F10` (with `dword_2697F14` aliasing `dword_2697F10 + 1`, i.e. the
next depth's slot). Per line:

- first token `}` with depth > 0 → `--depth`.
- first token `{` → push: stage the child scope's state.
- otherwise dispatch on `state = dword_2697F10[depth]`:
  - **state 0 (top level)** — `define head|body|arms <name>` (stages child state
    1/2/3 and allocates a part) **or** `nationality <id> <nameKey>` (→ state 4;
    error state 7 if `id > 31` or the slot is taken).
  - **states 1/2/3 (inside a `define head|body|arms` block)** — `name`,
    `graphic`, `graphic_d`, `graphic_j`, `graphic_s`, `camo <r> <g> <b>`,
    `voice <int>`, `sex M|F`.
  - **state 4 (inside `nationality`)** — `alignment good|evil`;
    `division <id> <nameKey>` (→ state 5; error state 8 if `id > 15` or taken).
  - **state 5 (inside `division`)** — `combo <id> <headName> <bodyName> <armsName>`.
- Hard cap guard: if the part count `dword_26A773C >= 512`,
  `MessageBoxA("ComboObj Parse Error", "Game Error")` and the callback returns 1
  (abort). See **D-PLAYERINFO-2**.

Canonical file shape (braces are their own lines; the keyword line precedes the
opening `{`):

```
define head USHead
{
    name        STR_AV_US_HEAD
    graphic     ushd.3di
    graphic_d   ushd_d.3di
    graphic_j   ushd_j.3di
    graphic_s   ushd_s.3di
    camo        128 128 128
    voice       3
    sex         M
}
nationality 0 STR_AV_NAT_US
{
    alignment good
    division 0 STR_AV_DIV_RANGER
    {
        combo 0 USHead USBody USArms
    }
}
```

## Data model — witness map

### Transient part pool (parse-time only; not retained at runtime)

`g_avatarParts = byte_2697F3C @ 0x2697F3C` — a static array, **124 bytes/entry,
max 512** (count `dword_26A773C @ 0x26A773C`; current-edit index
`dword_26A7740 @ 0x26A7740`). The `define` blocks fill it; `combo` lines resolve
against it by name + kind. It is *not* part of the runtime avatar object — once
combos are built it is dead. Part struct (124 B):

| Off | Type | Field | Store |
| --- | --- | --- | --- |
| +0 | char[32] | `name` — define identifier, matched by `combo` | `byte_2697F3C` |
| +32 | int | `kind` (0=head, 1=body, 2=arms) | `dword_2697F5C` (`31*idx` dwords) |
| +36 | char[32] | display-name key (`name` keyword; an RTXT key) | `unk_2697F60` |
| +68/+69/+70 | u8×3 | `camo` r/g/b | `byte_2697F80/81/82` |
| +71 | u8 | `voice` | `byte_2697F83` |
| +72 | int | `sex` (0=M, 1=F) | `dword_2697F84` (`31*idx` dwords) |
| +76 | char[16] | `graphic` (**and `graphic_d`** — see D-PLAYERINFO-3) | `unk_2697F88` |
| +92 | char[16] | `graphic_j` | `unk_2697F98` |
| +108 | char[16] | `graphic_s` | `unk_2697FA8` |

String copies are unbounded (`strcpy`/inline `do{}while`); the 16/32 widths are
the field strides, not bounds checks.

### Persistent avatar object — `count_and_entries @ 0x26A7748`

- **+0** `int comboCount`.
- **+4** combo array — **288 bytes/entry, max 128**, allocated by
  `[orig: sub_579E10 @ 0x579e10]` (`entry = this + 72*count + 1` dwords;
  `memset 0x120`; sets +0 nat-index, +4 div-index, +276 alignment copied from the
  nationality). The parser then denormalizes the resolved parts into it:

  | Off | Field |
  | --- | --- |
  | +0 / +4 | nationality index / division index |
  | +8 | `comboId` (`atol`) |
  | +12 | head display-name key (head part +36) |
  | +44/+45/+46 | head camo r/g/b |
  | +48 / +64 / +80 | head `graphic` / `graphic_j` / `graphic_s` |
  | +100 | body display-name key (body part +36) |
  | +132/+133/+134 | body camo |
  | +136 / +152 / +168 | body `graphic` / `graphic_j` / `graphic_s` |
  | +220/+221/+222 | arms camo (only if arms present) |
  | +224 / +240 / +256 | arms `graphic` / `graphic_j` / `graphic_s` |
  | +276 | alignment (from nationality) |
  | +280 | sex (from head part +72) |
  | +284 | voice (from head part +71) |

  **Key finding:** the combo **denormalizes** the resolved head/body/arms data
  into itself and does **not** retain the referenced part identifier names or
  indices. `combo` requires head **and** body (`if (!headPart || !bodyPart)
  return`); arms is optional. See **D-PLAYERINFO-4** for the round-trip
  implication.

- **+36872 (0x9008)** nationality array — **616 bytes/entry, max 32**, allocated
  by `[orig: sub_579F40 @ 0x579f40]` (`memset 616`; `strcpy nameKey @ +0`;
  alignment default 0 `@ +32`; valid flag 1 `@ +36`). Existence test =
  `nameKey[0] != 0`.

  | Off | Field |
  | --- | --- |
  | +0 | char[32] nationality name key (RTXT key) |
  | +32 | int alignment (0=good, 1=evil) |
  | +36 | int valid (=1) |
  | +40 | division array (16 × 36 B) |

  - Division — **36 bytes**, allocated by `[orig: sub_579FF0 @ 0x579ff0]` at
    `+40 + 36*divIdx` within the nationality (`memset 36`; `strcpy nameKey @ +0`;
    value 1 `@ +32`). Division struct: +0 char[32] name key; +32 int value (=1).

Parser scratch globals: `type = dword_2697F30` (current nat index),
`subtype = dword_2697F34` (current div index), `dword_2697F38` (current combo
pointer).

**Co-location note:** `count_and_entries` is a *composite* static. After the
avatar data it also holds an unrelated **HUD minimap-entity table** at
`this + 14153` dwords (≈ +56612), 256 × 36 B slots, managed by
`[orig: MinimapSlot_FindOrAllocByEntityId @ 0x57b1e0]` /
`HUD_DrawAllMinimapEntities @ 0x57b080`. That region is *not* part of
`Avatars.def`; it only shares the base address. This co-location is why several
avatar accessors carry auto-names prefixed `MinimapSlot_*` (see proposed
corrections below) — `MinimapSlot_GetFieldByIndex @ 0x579e70` is in fact the
nationality-alignment accessor.

## Menu consumption — witness map (`PLAYER_INFO` screen, `player.mnu`)

- `[orig: PlayerInfo_PopulateNationalityList @ 0x55d8c0]` `(teamIndex)` — fills
  the `NATIONALITY` list of screen `PLAYER_INFO`. Iterates nationalities via
  `[orig: sub_579f10 @ 0x579f10]` (name key by index; null terminates).
  **Team filter via alignment:** `[orig: MinimapSlot_GetFieldByIndex @ 0x579e70]`
  returns the nationality alignment, and a row shows only when
  `(alignment != 0) == (teamIndex != 0)` — good→team 0, evil→team 1
  (D-PLAYERINFO-5). Display string =
  `TextResource_GetStringWithFallback(resource, "Avatars", nameKey)` — the names
  in the `.def` are **keys into the `"Avatars"` RTXT string table**. Availability
  via `[orig: sub_579ea0 @ 0x579ea0]`; unavailable rows are greyed. Selection
  persists in `byte_2551131[…]`.
- `[orig: PlayerInfo_PopulateDivisionList @ 0x55da50]` `(nationality, teamIndex)`
  — 16 division slots via `[orig: sub_579fb0 @ 0x579fb0]` (name by nat+div),
  availability `[orig: sub_579ed0 @ 0x579ed0]`; same `"Avatars"` string-table
  display; selection `byte_2551132[…]`.
- `[orig: populate_avatar_combo_list @ 0x560210]` — fills `COMBO_LIST`.
  Enumerates combos for the selected (nat, div) via
  `[orig: sub_57AEC0 @ 0x57aec0]`, which packs matches into `word_26B7850`
  (bitfield: bits 0–4 nat, 5–8 div, 9–14 comboId, 15 alignment). Per combo the
  "last name" key = `[orig: sub_57A310 @ 0x57a310]` (combo +12, the head display
  name) and "first name" key = `[orig: sub_57A340 @ 0x57a340]` (combo +100, the
  body display name), both resolved through the `"Avatars"` RTXT table and shown
  as `sprintf("%s - %s", last, first)`. The voice list then comes from
  `[orig: populate_player_voice_combo @ 0x55dce0]`. Combo lookup by packed id:
  `[orig: MinimapSlot_FindByPackedId @ 0x57a270]`.

So the menu reads the parsed object directly; the display vocabulary lives in the
`"Avatars"` RTXT string table, keyed by the name fields, and the head/body
display names form a "lastname - firstname" character label.

**The `"Avatars"` section lives in `Game.bin`** — the MENU SHELL'S text resource,
loaded once at menu boot `[orig: the menu init @ 0x552510 —
TextResource_LoadFromArchive("game.bin") -> the menu resource @ 0x25510F8; lookup
TextResource_GetStringWithFallback @ 0x562ee0]` (NOT `menutxt.BIN`, which carries
only `Menu`/`MenuStats`, and NOT `g_TextGameText`, which loads `gametext.bin`
`[orig: Game_InitSubsystems @ 0x4A6CD0]` — the 2026-07-11 weapon-round grill
corrected the earlier conflation of the two). Verified by extraction: the retail
`Game.bin` has sections `MENU / RemapActions / RemapKeys / WeaponDescriptions /
Macros / Avatars`, and the `Avatars` keys resolve (`AV_NAT_RUSSIA → "Russia"`,
`AV_DIV_SEAL → "SEAL"`, `AV_BOONIEHAT → "Boonie Hat"`). Reimpl: `nova_menu_shell.gd`
registers `Game.bin` into the shared `NovaStrings` registry as **`gameui`**
(gametext = `gametext.bin`), and `player_info_menu_companion.gd::_display_name`
resolves the nationality/division/combo keys against `gameui`'s `"Avatars"`
section (raw-key fallback on a miss). On-disk-miss fallback to the raw key is the witnessed
`GetStringWithFallback` behavior; the expansion override table (JOX avatars) is a
follow-up via `NovaStrings.set_override_table`.

### Preview animation (PLAYER_PREVIEW)

The 3D preview is initialized by `[orig: PlayerInfo_InitPreviewModel @ 0x5600d0]` (from
`PlayerInfo_InitProfileSelector`): it sets up a skeleton (`BoneSystem_Init` + `BoneFile_Load`)
and binds the **idle animation** `AnimChannel_InitFromData("PI_Idle.BAD")` (plus `Dt1rst.bad`),
a `HwmCube.dds` reflection map, and the preview render target. So the original character
preview plays a **skeletal idle animation**, not a static pose.

Per frame, `[orig: update_player_preview_animation @ 0x55dba0]` drives two transform values
consumed by the preview render:
- **Zoom blend** `flt_25DC538 = blend*0.95 + target*0.05` — `target` is 1 while the cursor is
  over `PLAYER_PREVIEW`/`COMBO_LIST`/`DIVISION`/`NATIONALITY` (`sub_6467D0` = widget-hovered
  test), else 0; a damped 0→1 ramp that zooms the view in on hover.
- **Rotation** `dword_25DC53C` (BAM): idle `+= 0x800000`/frame (a steady spin); on hover a
  sinusoidal sway `sin(GetTickCount·0.0008)·2^28` (≈ ±22.5°). (Constants: `0.95`/`0.05`
  damping; sway freq `0.0008`/ms, amplitude `2^28` BAM.)

Reimpl `AvatarPreview` ports **both** halves. The **transform** half: a spin node rotates the
composed model (idle spin + hover sway, seeded with the original's random initial yaw
`(rand()%180)·0xB60B60`) while the camera only zooms (`_process`, `set_hovered`). The **skeletal
idle**: it builds one shared `NovaSkeletalAnim` from the raw `Dt1rst.bad` (rest/skeleton) +
`PI_Idle.BAD` (looping idle clip) via `NovaSkeletalAnim.load_from_bad_files` — the no-`.adm`
raw-`.bad` path that mirrors the original's two `BoneFile_Load` calls — and binds it onto the
composed skinned head/body parts (`set_skeletal_anim` + `play_body_clip("anim_idle")`). Retail IR
confirms those third-person graphics use weighted bone references within the 19-bone `Dt1rst`
domain. The combo arms graphics are different: all four retail variants are 38/40-part models
with weighted references through bone 36, and the current first-person runtime pairs `ArmsG`
with the active weapon's larger model table and ADM. `AvatarPreview` therefore keeps arms out of
the standing composition instead of binding them to `Dt1rst`. This asset/runtime evidence does
not close the still-unwitnessed combo → spawned-player consumer in D-PLAYERINFO-1. The `.bad`
assets resolve from the retail PFFs; when absent (a loose mount lacking them) the composed parts
render static at rest. (`HwmCube.dds` reflection map: not yet applied — minor.)

## Screen orchestration — witness map (grilled 2026-06-23)

The four populate functions above are driven by a per-screen init plus per-control
change handlers, all registered under the `"PLAYER_INFO"` category. Controls are
located by name through `[orig: sub_63AE80 @ 0x63ae80]` (find-widget:
`sub_63AE80(scene, "PLAYER_INFO", "<CONTROL>")`, scene `= dword_2551100`); combo
set-selected-index is `[orig: sub_645240 @ 0x645240]`, read-selected-value is
`[orig: sub_644660 @ 0x644660]`.

**Selection state** lives in a per-slot/per-team block of globals (NOT the
profile), keyed `[67596*g_curProfileSlot + 32774*team]` (`g_curProfileSlot @
0x25506B8` = active profile index): `g_charSelClass @ 0x2551130` (u8, class 5..9),
`g_charSelNationality @ 0x2551131` (u8), `g_charSelDivision @ 0x2551132` (u8),
`g_charSelCombo @ 0x2551134` (u16; also holds the per-character voice). The current
profile object is `g_curPlayerProfile @ 0x25510FC = &profile[15488 *
g_curProfileSlot]` (`profile @ 0x252de58`, 15488 B/entry; the array ends at
`0x2540CDC`). See **D-PLAYERINFO-12**.

- `[orig: PlayerInfo_InitProfileSelector @ 0x5611b0]` — screen show. Builds the
  `PLAYER` profile combobox (`"<ProfileName> (<CharName>)"`; profile name via the
  `"PROFILE_%d"`/`Profile %d` keys, `CS_NONAME` from the `"Menu"` table when
  `profile+48 & 1`); selects the current profile; reads `SIDE_BLUE` (vtable+96 =
  get-check) to derive the team; calls `PlayerInfo_PopulateAllControls(team)`; then
  `update_player_info_weight_and_weapon_icons` and `j_SoundBank_OpenFile("menu.lwf",
  &g_MenuSoundBank)` (the voice-preview bank `@ 0x25DC3E0`).
- `[orig: PlayerInfo_PopulateAllControls @ 0x5606f0]` `(teamIndex)` — the
  populate-everything pass, in order: `PlayerInfo_SetTeamAndClassMask(team)` →
  OPTIONS_AUTORELOAD ← `profile+1524` → OPTIONS_AUTOMEDIC ← `profile+1660 == 0`
  (inverted) → PLAYERCLASS sel ← `g_charSelClass` →
  `PlayerInfo_PopulateNationalityList(team)` + NATIONALITY sel ←
  `g_charSelNationality` → `PlayerInfo_PopulateDivisionList(nat, team)` + DIVISION
  sel ← `g_charSelDivision` → `populate_avatar_combo_list()` + COMBO_LIST sel ←
  `g_charSelCombo` → PLAYERNAME edit ← `profile+4` (vtable+76 = SetText) →
  `populate_player_voice_combo(...)` → `populate_weapon_slot_lists()`.
- `[orig: PlayerInfo_RegisterAllControls @ 0x561470]` — registers 28 PLAYER_INFO
  controls, each `[orig: sub_63C060 @ 0x63c060](category, name, handler, …)`. Change
  handlers route the selection-change notification (`0x5000001`, selected value in
  `eventData+16`):
  - `[orig: PlayerInfo_HandleNationalitySelect @ 0x560600]` — store
    `g_charSelNationality`, reset `g_charSelDivision = 0`, repopulate division then
    combo (the nat→div→combo **cascade**).
  - `[orig: PlayerInfo_HandleDivisionSelect @ 0x560690]` — store `g_charSelDivision`,
    repopulate combo.
  - `[orig: PlayerInfo_HandleClassSelect @ 0x560910]` — store `g_charSelClass` (and
    a mirror `@ 0x2559136`), re-`PlayerInfo_PopulateAllControls` (a class change
    re-filters the loadout).
  - `[orig: PlayerInfo_HandleProfileSelect @ 0x560960]` — on PLAYER combo change:
    `save_player_info_from_dialog` (commit current), switch `g_curPlayerProfile`/
    `g_curProfileSlot`, re-populate.
  - `[orig: PlayerInfo_HandleVoiceSelect @ 0x55fe00]` — store voice into
    `g_charSelCombo[…]`, rebuild PLAYERVOICE (DEFAULT_VOICE + per-character
    `CHARVOICE_%d` from the voice-def table `@ 0x83C7AC`, stride 12, ending at
    `ammoDef @ 0x83c830`).
  - Team radios `SIDE_BLUE`/`SIDE_RED` re-run the populate for the new team via
    `[orig: PlayerInfo_SaveAndRepopulate @ 0x5608f0]` (save → populate(team) →
    weight); team `0 = blue/good`, `1 = red/evil` (consistent with D-PLAYERINFO-5).

### Class → loadout filter mask (D-PLAYERINFO-8)
`[orig: PlayerInfo_SetTeamAndClassMask @ 0x55de60]` `(team)` — sets the global
`teamIndex` and maps the PLAYERCLASS byte (`g_charSelClass`, 5..9) to a
power-of-two mask `g_playerInfoClassMask @ 0x25DC550` (5→1, 6→2, 7→4, 8→8, 9→16)
plus `g_playerInfoTeamMask @ 0x25DC54C = 2 - (team != 0)`. Both gate the loadout.

### Voice preview — TESTPLAYERVOICE (D-PLAYERINFO-10)
`[orig: PlayerInfo_PreviewVoice @ 0x55ff70]` — on the click notification
`0x3000001`, the voice index is the profile override `*(g_curPlayerProfile + team +
1532)` when non-zero, else derived from the selected combo's avatar voice
(`[orig: sub_57AE60 @ 0x57ae60](g_avatarDefs, g_charSelCombo[…])`); then
`sprintf("VOICE_%d", idx)` → `[orig: SoundBank_FindTriggerAndPlay @ 0x75d010](key,
params, &g_MenuSoundBank)` (params `[0]=0x10000, [2]=255`). The `menu.lwf` bank is
loaded by the screen init.

The reimpl port binds `TESTPLAYERVOICE` by control name and routes the selected
avatar fallback through `NovaMnuMenu.play_widget_sound("VOICE_%d", "menu.lwf")`.
The persisted `profile+1532+team` override is intentionally still owned by the
profile-persistence work in D-PLAYERINFO-9.

### ACCEPT / commit + persistence (D-PLAYERINFO-9)
`[orig: save_player_info_from_dialog @ 0x55ee10]` reads each control back (combo
value via `sub_644660`, checkbox via `[orig: sub_64ACB0 @ 0x64acb0]`):
PLAYERCLASS → `g_charSelClass` for **both** teams (`side = 0, 32774`); NATIONALITY/
DIVISION/COMBO_LIST → their `g_charSel*`; OPTIONS_AUTORELOAD → `profile+1524`;
OPTIONS_AUTOMEDIC → `profile+1660 = (state == 0)` (inverted); PLAYERNAME →
`profile+4` (char[16]; whitespace-only rejected via `iswspace`, clearing the name
and setting `profile+52 |= 1`). Returns `[orig: serialize_weapon_loadout @
0x55e4b0]` (persists the loadout). The live session reads the same selection
globals via `[orig: apply_session_settings_to_globals @ 0x551500]`.

### Loadout population (D-PLAYERINFO-11)
`[orig: populate_weapon_slot_lists @ 0x560430]` fills PRIMARY/SECONDARY/ACCESSORY
from the weapon table `@ 0x2540D08` (192 B/entry, ending at `0x254CC48`). A row
shows only when `(entry+76 & g_playerInfoClassMask) != 0` **and**
`(g_playerInfoTeamMask & entry+72) != 0`; it routes to PRIMARY/SECONDARY/ACCESSORY
by `entry+68` (1/2/0); display name = `entry+0` (else the id at `entry-40`). A
`"NONE"` row (`"Menu"`/`NONE`) is inserted at index 0 of each. It then calls
`[orig: populate_weapon_accessory_ammo_ui @ 0x55e8b0]` (the `*_AMMO*` combos) and
`[orig: update_player_info_weight_and_weapon_icons @ 0x55f480]` (the
`STATIC_TOTAL_WEIGHT` budget + weapon icons; light/normal/heavy, sibling of
`UI_UpdateWeaponWeightDisplay @ 0x565640`). The weapon table itself is built by
`[orig: WeaponDef_LoadAll @ 0x54dd10]` (zeroes `g_weaponDefTable @ 0x2540CE0`, 0xBF40 B,
count `@ 0x2540CDC` starting at 1 with a `"None"` entry, then
`File_ParseASCIIFile("weapon.def", WeaponDef_ParseProperty, key 0x2A56F6AD)`); the
loadout table `@ 0x2540D08` is that table at `g_weaponDefTable + 0x28` (so the
consumer's `+68/+72/+76` and the `-40` name fallback are the absolute offsets below
minus `0x28`).

**Producer field map — `[orig: WeaponDef_ParseProperty @ 0x54d730]`** (192 B/entry,
written at `g_weaponDefTable + 192*count`; absolute offsets):

| Off | weapon.def token | Meaning |
|---|---|---|
| `+0`   | `weapon "<id>"` | weapon id / raw name (char[]) |
| `+32`  | `loadout_selectable` | **gate** — row appears only when non-zero |
| `+36`  | `loadout_subclasses` | sub-entry expansion count |
| `+40`  | `loadout_menu_textid` | display name = `GameText_GetString("WepDes", id)` ptr |
| `+44`  | `loadout_menu_ttdesc` | tooltip (char[]) |
| `+108` | `weapon_class` | **slot**: accessory=0, primary=1, secondary=2, grenade=3 |
| `+112` | `teamfilter` | mask: `blue`/`yellow` `\|= 2`, `red`/`violet` `\|= 1` |
| `+116` | `charfilter` | mask: medic=1, sniper=2, gunner=4, rifleman=8, engineer=16 |
| `+120` | `weaponweight` | float |
| `+124` | `startrounds` | int |
| `+128` | `round_type` | ammo name = `GameText_GetString("WepDes", id)` ptr |
| `+132` | `clipsize` | int |
| `+136` | `maxclips` | int |
| `+140` | `clipweight` | float |
| `+144` | `loadout_menu_icon` | char[32] |
| `+184`/`+188` | `flags` | lo/hi bit masks (table `off_830BF0`) |

Consumer `populate_weapon_slot_lists @ 0x560430` shows a row when `loadout_selectable
(+32) != 0` **and** `(charfilter +116 & g_playerInfoClassMask) != 0` **and**
`(teamfilter +112 & g_playerInfoTeamMask) != 0`; routes by `weapon_class (+108)` (1→
PRIMARY, 2→SECONDARY, 0→ACCESSORY; 3=grenade is handled by the ammo UI, not these
three lists); display = `loadout_menu_textid (+40)` resolved string, else the raw
`weapon_name (+0)`; `"NONE"` (the menu resource `"Menu"/"NONE"`) at index 0. Names
resolve through `g_TextGameText` (**`gametext.bin`**) section `"WepDes"`
`[orig: GameText_GetString @ 0x51ebd0; the textid resolve at parse time is
WeaponDef_ParseProperty @ 0x54d730 — GameText_GetString("WepDes", textid)]`.
(The earlier note blaming "REVX02's Game.bin" for raw-id fallbacks described the
port's own misload — Game.bin never carries `WepDes`; corrected 2026-07-11.)

Reimpl plan (now fully witnessed; producer + consumer + masks): extend `libs/def`
`DefWeaponDef` to capture the loadout fields above + add VFS (in-memory) parsers, add
a `NovaWeaponDatabase` binding, then wire the host combos + ammo + weight. Tracked in
TODO.md (Player info / loadout).

### Ammo combos, weight, and icons (D-PLAYERINFO-11 — witnessed 2026-07-30)

All decompiled this session; every read below is a field slice of
`g_weaponDefTable @ 0x2540CE0` (192 B/entry — the offsets in the producer map
above), never `g_ammoDefTable`. Entry 0 is the `"None"` record, and the combo
get-selected helper returns 0 for "no selection", so a NONE selection resolves
every lookup to the null entry.

**Saved-kit consumption — `[orig: populate_weapon_accessory_ammo_ui @ 0x55e8b0]`.**
The input is the profile kit page (net-re §5.66): repeated 4-string tuples
`(name, ammoPri, ammoSec, flags)` until an empty name. Per tuple:
`ammoPri` → `g_playerInfoAmmoPriCounts[2*idx] @ 0x25DC560`, `ammoSec` →
`g_playerInfoAmmoSecCounts[2*idx] @ 0x25DC564` (interleaved pair; `-1` = default;
the ammoSec store is gated on the subclass walk finding a differing round name),
`flags` → `g_playerInfoAmmoTypePri/Sec[teamIndex] @ 0x25DCD64/0x25DCD68` (the
ammo-TYPE byte, stored per team) — for PRIMARY/SECONDARY-class defs respectively.
`weapon_class` 1/2/0 selects the def in the PRIMARY/SECONDARY/ACCESSORY combo;
class 3 appends the def index to `g_playerInfoGrenadeSlots @ 0x25DC554` (3 slots).
It then fills the ammo combos (slots 0/1/3 via `@ 0x55def0`; the ACCESSORY block
is inlined) and is itself called from `populate_weapon_slot_lists @ 0x560430`.

**Ammo-combo fill — `[orig: populate_ammo_combo_boxes @ 0x55def0]`** (slot 0 =
PRIMARY, 1 = SECONDARY, 2 = ACCESSORY, ≥3 = grenades):

- `*_AMMO1`: hidden when nothing is selected or `clipsize <= 0`; else rows
  **1..maxclips**, label `sprintf("%d - %s", i*clipsize, round_type)`, row value
  = the def index. Selection = the saved count's row; saved `-1` selects the
  **maxclips** row (full default).
- `*_AMMO1_TYPE` (PRIMARY/SECONDARY only — the .mnu authors the FMJ/AP/SP
  statics, values 0/1/2): shown/hidden with AMMO1; selection = the saved
  per-team type byte (`-1` → 0). Weapon `flags2 (+188)` bit `0x40` = "no type
  choice": the combo is made non-interactive (`UIWidget_SetInteractiveRecursive
  @ 0x6462E0` — recursive widget flag) and the saved type byte is reset to 0.
- `*_AMMO2` (the sub-weapon): the walk starts at the parent's next table entry
  and skips entries whose `round_type` equals the parent's, bounded by
  `loadout_subclasses (+36)`; the first **differing** entry is the sub-weapon
  (satchel → detonator). Hidden when the walk finds none or the sub-def's
  `clipsize <= 0`; else rows 1..sub.maxclips from the sub-def's fields, selection
  from the saved ammoSec count (same `-1` = max rule).
- Grenades: walk the whole table in order; a def qualifies when
  `loadout_selectable && (charfilter & g_playerInfoClassMask) && (teamfilter &
  g_playerInfoTeamMask) && weapon_class == 3`; the first three fill
  `GRENADE_AMMO1..3` (and `g_playerInfoGrenadeSlots`), leftover widgets are
  **hidden**. Rows **0..maxclips including the zero row**, row value =
  `count*clipsize` (rounds); selection = saved count's row, `-1` → maxclips row.

**Recompute graph — `[orig: PlayerInfo_RegisterAllControls @ 0x561470]`** (all
handlers gate on notify `0x5000001`): PRIMARY/SECONDARY select
(`@ 0x55f710`/`@ 0x55f790`) → refill that slot's ammo combos + weight/icons;
ACCESSORY select (`handle_accessory_ammo_slot_selection @ 0x55f810`) → inlined
slot-2 refill + weight/icons; `*_AMMO1`/`*_AMMO2`
(`@ 0x55f730`/`@ 0x55f7b0`/`@ 0x55fb40`, ctx 0/1 = pri/sec column) → store
`selected_row + 1` keyed by the row's def-index value + weight/icons;
`*_AMMO1_TYPE` (`@ 0x55f760`/`@ 0x55f7e0`) → store the row value byte per team +
weight/icons; `GRENADE_AMMO1..3` (`@ 0x55fb70`, ctx 0..2) → store the **row
value** (rounds — see the quirk below) + weight/icons; ACCEPT (`@ 0x55fdf0`) →
`save_player_info_from_dialog`; SIDE_BLUE/SIDE_RED →
`PlayerInfo_SaveAndRepopulate(0/1)`.

**Weight + icons — `[orig: update_player_info_weight_and_weapon_icons @
0x55f480]`.** Weight = `calculate_loadout_weight @ 0x55f1f0` →
`g_playerInfoLoadoutWeight @ 0x25DCD5C`; bands ≥66.6 HEAVY / ≥33.3 NORMAL / else
LIGHT; rendered into `STATIC_TOTAL_WEIGHT` as `sprintf("%s %.1f %s (%s)")` with
menu-string keys `TOTAL_WEIGHT`, `LBS`, `LIGHT_/NORMAL_/HEAVY_ENCUMBRANCE`
(resolved through the control's own string table — the armory sibling
`update_weapon_weight_display @ 0x565640` uses the same keys). Icons: the
`PRIMARY/SECONDARY/ACCESSORY_ICON` windows are textured from the selected def's
`loadout_menu_icon (+144)`; no selection resolves to entry 0 (blank);
`GRENADE_ICON` is never touched (it keeps the .mnu's authored `m_nades.tga`).

**Weight terms — `[orig: calculate_loadout_weight @ 0x55f1f0]`** (confirms the
ported `def_loadout_weight` for parents, refines the rest):

- Parent slots (PRIMARY/SECONDARY/ACCESSORY): `weaponweight (+120) +
  effAmmo*clipweight (+140)`, `effAmmo = saved <= 0 ? maxclips : saved` — exactly
  `def_loadout_weight`'s contract.
- Sub-weapon (only when the `*_AMMO2` control exists, the subclass walk hits,
  and the sub-def's `clipsize > 0`): **clip term only** — no base weight — with
  the same `<= 0` → maxclips default.
- Grenades (only for grenade controls that exist **and are shown**): **clip term
  only**, with `saved == -1` → maxclips (a saved **0 stays 0** — the zero row).

**Kit-page writer — `[orig: serialize_weapon_loadout @ 0x55e4b0]`** (doc-only;
the write side is D-PLAYERINFO-9): first entry = the knife —
`g_playerInfoTeamMask & 2 || mask == 0` → `WPN_KNIFE`, else `WPN_KNIFE2` — then
`WPN_MEDPACK` when the class byte is 5 (medic), then the three category
selections and the grenade/registered tail; every entry is the 4-string tuple
with `"-1"` as the default filler. Field-level mining deferred to the
D-PLAYERINFO-9 session.

## Divergence / quirk catalog (D-PLAYERINFO)

These are witnessed original behaviors a faithful port must reproduce; IDs are
stable.

| ID | Original (Jointops.exe) | Why / consequence for the port |
| --- | --- | --- |
| D-PLAYERINFO-1 | combo → spawned-player 3D model binding not traced | **partially open**. The preview's **animation path** is witnessed and ported: the transform animation (`update_player_preview_animation @ 0x55dba0`) and skeletal idle (`PlayerInfo_InitPreviewModel @ 0x5600d0` binds `Dt1rst.bad` rest + `PI_Idle.BAD` idle on a `BoneSystem_Init` skeleton) — `AvatarPreview` plays `PI_Idle.BAD` on the compatible 19-bone head/body composition (see "Preview animation" above). Excluding arms is a geometry policy supported by retail asset structure and current-runtime evidence: the 38/40-part arm graphics require a larger rig. Still open: the **in-world** (spawned-player) combo→model binding — how a selected combo drives the in-mission avatar — which remains untraced. |
| D-PLAYERINFO-2 | `>= 512` parts → `MessageBoxA("ComboObj Parse Error")` + abort | **FIXED 2026-07-05 (verified enforced)**: the parser errors at the cap (`avatars.cpp` guard `[orig: CAvatarDefs_ParseConfigLine @ 0x57a456]`), `NovaAvatarDatabase` propagates, and `avatars_parse_test.cpp` pins the 512-part failure. |
| D-PLAYERINFO-3 | `graphic` and `graphic_d` write the **same** part field (+76) | `graphic_d` aliases/overwrites `graphic`; only `graphic_j` (+92) and `graphic_s` (+108) are distinct slots. A faithful parser stores both keywords into one field (last wins). |
| D-PLAYERINFO-4 | combo retains only denormalized part data, not the part names/indices | the runtime struct cannot reproduce the `combo <id> <head> <body> <arms>` line. The reimpl's authoring model must *additionally* keep the three reference names to round-trip the writer — a superset; runtime behavior is unchanged. |
| D-PLAYERINFO-5 | nationality list filtered by `alignment` vs `teamIndex` (good→0, evil→1) | the menu population is team-aware; the reimpl port must reproduce the filter and order. |
| D-PLAYERINFO-6 | `nationality`/`division` id token: `if (*idStr > '9') ++idStr;` then `atol` | a single leading non-digit character is skipped before parsing the numeric id. The reimpl parser must mirror this lenient id read. |
| D-PLAYERINFO-7 | screen = init (`PlayerInfo_InitProfileSelector @ 0x5611b0`) → `PlayerInfo_PopulateAllControls(team)` + 28 per-control handlers registered via `sub_63C060`; the nat→div→combo cascade (`@ 0x560600`/`@ 0x560690`, notify `0x5000001`) repopulates dependents and **resets the division on a nationality change** | the reimpl port reproduces the populate order and the cascade: selecting a nationality resets the division selection and refills division+combo; selecting a division refills combo. |
| D-PLAYERINFO-8 | PLAYERCLASS byte 5..9 → power-of-two class mask `g_playerInfoClassMask` (1/2/4/8/16); team → `g_playerInfoTeamMask = 2-(team!=0)` (`PlayerInfo_SetTeamAndClassMask @ 0x55de60`) | **implemented**: `player_info_menu_companion._selected_class_mask` (5..9→1/2/4/8/16) + team mask `2-(team!=0)` gate the weapon slot lists; repopulate on class/team change. |
| D-PLAYERINFO-9 | ACCEPT/commit (`save_player_info_from_dialog @ 0x55ee10`) writes class (both teams), nat/div/combo, autoreload→`profile+1524`, automedic→`profile+1660` (**inverted**), name→`profile+4` (whitespace-rejected), then `serialize_weapon_loadout` | the reimpl commit mirrors this field map, the automedic inversion, and the name validation; selections live in per-slot/per-team globals, not the profile. |
| D-PLAYERINFO-10 | TESTPLAYERVOICE previews `"VOICE_%d"` from `g_MenuSoundBank` (`menu.lwf`); voice index = profile override `profile+1532+team` else the avatar combo's voice; PLAYERVOICE list = DEFAULT_VOICE + per-character `CHARVOICE_%d` | **implemented**: the named button requests the selected avatar fallback as `VOICE_%d` through `menu.lwf`, and the avatar-derived list remains populated by `PlayerInfoMenuCompanion`; `test_voice_preview_requests_selected_avatar_voice` pins the public request. Persisted profile overrides ride D-PLAYERINFO-9. |
| D-PLAYERINFO-11 | loadout combos from the weapon table `@ 0x2540D08` (192 B), filtered by class+team mask, slot-routed by `weapon_class +108` (1/2/0 = PRIMARY/SECONDARY/ACCESSORY), `"NONE"` first; ammo `@ 0x55e8b0`; weight `@ 0x55f480`. Producer `WeaponDef_ParseProperty @ 0x54d730` grilled — full `weapon.def` field map (above). | **weapon lists implemented** (`libs/def` loadout fields + `NovaWeaponDatabase` + host `_populate_loadout`: slot routing, class/team filter, NONE-first, names via gametext "WepDes" else raw id). The **weight readout** math is now ported to `libs/def` (`def_loadout_weight`: Σ weaponweight + (ammo>0?ammo:maxclips)*clipweight; `def_encumbrance_class`: ≥66.6 HEAVY / ≥33.3 NORMAL / else LIGHT — witnessed thresholds, unit-tested in `def_loadout_weight_test`). Residual: the **ammo combos** (`@ 0x55e8b0`) + the UI host wiring (weight label + icons), which need the Godot runtime. |
| D-PLAYERINFO-12 | selection state lives in per-slot/per-team globals keyed `[67596*slot + 32774*team]` (`g_charSelClass/Nationality/Division/Combo @ 0x2551130/1/2/4`), distinct from the 15488-B profile object (`profile @ 0x252de58`: name`+4`, autoreload`+1524`, voice`+1532`, automedic`+1660`) | the reimpl keys avatar/loadout selection by (profile slot, team) and keeps it separate from the profile-level fields; the simplified single-profile reimpl may collapse the slot dimension but must keep the team dimension (D-PLAYERINFO-5/7). |

## Implementation grill notes (2026-06-16)

`libs/avatars` was re-checked against IDA after PR #162's first implementation.
The following axes are now pinned by native tests and surfaced through
`NovaAvatarDatabase` diagnostics:

- `combo` resolution is parse-time, not deferred: head/body must resolve against
  already-defined parts or the combo is skipped; arms is optional and becomes an
  absent arms snapshot when unresolved.
- Duplicate part names resolve as the original loops do: the last prior matching
  part wins for the combo snapshot, and later duplicates do not mutate an
  already-created combo.
- Duplicate nationality/division slots are ignored with diagnostics; they do not
  create extra authoring entries.
- `camo` and `voice` are byte fields; parsed values keep the low 8 bits.
- The 512-part guard aborts before dispatching another meaningful top-level line,
  matching `0x57a456`. The 128-combo allocator cap at `0x579e10` is retained as a
  safe parse error rather than reproducing the original null/overflow hazard.

IDB changes made during this fix pass: none. The IDB remained read-only; proposed
renames/types below still await maintainer approval.

## IDB changes made (2026-06-23 orchestration grill)

Applied to `Jointops.exe.kong.i64` (all auto-named, anchored; saved):

- **Functions:** `sub_560690 → PlayerInfo_HandleDivisionSelect`,
  `sub_560910 → PlayerInfo_HandleClassSelect`,
  `sub_560960 → PlayerInfo_HandleProfileSelect`,
  `sub_5608F0 → PlayerInfo_SaveAndRepopulate`,
  `sub_55FF70 → PlayerInfo_PreviewVoice`,
  `sub_55DE60 → PlayerInfo_SetTeamAndClassMask`.
- **Globals:** `dword_25DC550 → g_playerInfoClassMask`,
  `dword_25DC54C → g_playerInfoTeamMask`, `dword_25510FC → g_curPlayerProfile`,
  `dword_25506B8 → g_curProfileSlot`, `byte_2551130 → g_charSelClass`,
  `byte_2551131 → g_charSelNationality`, `byte_2551132 → g_charSelDivision`,
  `word_2551134 → g_charSelCombo`; `dword_2540CE0 → g_weaponDefTable`,
  `dword_2540CDC → g_weaponDefCount` (the loadout weapon table, anchored via
  `WeaponDef_LoadAll @ 0x54dd10`).
- Entry comments linking the eight orchestration functions + `WeaponDef_LoadAll` to
  this record.

Names already curated (used as-is): `PlayerInfo_PopulateAllControls @ 0x5606f0`,
`PlayerInfo_InitProfileSelector @ 0x5611b0`, `PlayerInfo_RegisterAllControls @
0x561470`, `PlayerInfo_HandleNationalitySelect @ 0x560600`,
`PlayerInfo_HandleVoiceSelect @ 0x55fe00`, `save_player_info_from_dialog @
0x55ee10`, `populate_weapon_slot_lists @ 0x560430`,
`update_player_info_weight_and_weapon_icons @ 0x55f480`.

Still proposed (NOT applied — generic UI framework / struct declarations, propose
first): rename `sub_63AE80 → UIScene_FindWidgetByName`, `sub_645240 →
CListWnd_SetSelectedIndex`, `sub_644660 → CListWnd_GetSelectedValue`, `sub_63C060 →
UI_RegisterScreenControlCallback`; declare the weapon-table struct `@ 0x2540D08`
(192 B) and the player-profile struct (15488 B, fields `+4/+1524/+1532/+1660`).

## Follow-ups / open questions

- **D-PLAYERINFO-1 — combo → spawned-player model binding (Phase 6 grill).**
  Consumers reading `count_and_entries` beyond the menus, as candidate anchors:
  `[orig: Entity_SpawnFromAnimSlotProperty @ 0x43c390]` (touches the object only
  for minimap-slot allocation; its model comes from `gItemDefs` + an `.adm` via
  `AnimMap_LoadAdmFile`, key `0x2A5A8EAD`),
  `[orig: NapiNPClientMsg_FullEntitySpawn @ 0x433780]`,
  `[orig: NapiNPClientMsg_HandlePlayerSpawn @ 0x431bb0]`,
  `[orig: player_ServerAdd @ 0x51cbc0]`,
  `[orig: Server_BuildPlayerInfoAndAdd @ 0x51d560]`,
  `[orig: PlayerSession_InitFromProfile @ 0x50ca80]`,
  `[orig: PlayerProfile_InitDefaults @ 0x54bb40]`,
  `[orig: apply_session_settings_to_globals @ 0x551500]`,
  `[orig: Game_StartMission @ 0x524360]`,
  `[orig: handle_entity_minimap_update @ 0x427d00]`,
  `[orig: CAdminServer_HandleFunCommand @ 0x404e30]`. Likely intersects the
  player-avatar `off_8135F0` 252-entry slot table — the deferred seam in
  [ADR 0007](../adr/0007-skeletal-runtime-and-entity-visual.md).
- **Second avatar-defs init path.** `AvatarDefs_Init` (referenced at `0x53d281`
  and `0x53d2b4` inside `avatar_def_function_size_callback`) clears a
  `0x6000 + 0x4D00` object and also parses `Avatars.def` — a different/alternate
  manager. Unwitnessed; resolve whether it is a separate consumer (e.g. server-
  side) before assuming a single object.

## IDB changes proposed (NOT applied — shared curated IDB, awaiting maintainer OK)

Per the project shared-IDB policy these were left as proposals, not written.

- **Rename auto-named functions (anchored via the `"Avatars.def"` string + the
  witnessed dispatch):** `sub_579F40 → CAvatarDefs_SetNationality`,
  `sub_579FF0 → CAvatarDefs_SetDivision`, `sub_579E10 → CAvatarDefs_AllocCombo`,
  `sub_579F10 → CAvatarDefs_GetNationalityNameKey`,
  `sub_579EA0 → CAvatarDefs_IsNationalityAvailable`,
  `sub_579FB0 → CAvatarDefs_GetDivisionNameKey`,
  `sub_579ED0 → CAvatarDefs_IsDivisionAvailable`,
  `sub_57AEC0 → CAvatarDefs_EnumCombosByNatDiv`,
  `sub_57A310 → CAvatarDefs_GetComboLastNameKey`,
  `sub_57A340 → CAvatarDefs_GetComboFirstNameKey`,
  `MinimapSlot_FindByPackedId → CAvatarDefs_FindComboByPackedId`.
- **Rename globals:** `dword_2697F08 → g_pAvatarDefs`,
  `count_and_entries → g_avatarDefs` (or `g_avatarDefsAndMinimap`, given the
  composite), `byte_2697F3C → g_avatarPartPool`,
  `dword_26A773C → g_avatarPartCount`, `dword_26A7740 → g_avatarPartCurIndex`,
  `dword_2697F30 → g_avatarParseNatIndex`, `dword_2697F34 → g_avatarParseDivIndex`,
  `dword_2697F38 → g_avatarParseCurCombo`,
  `dword_2697F0C → g_avatarParseScopeDepth`,
  `dword_2697F10 → g_avatarParseStateStack`.
- **Curated-name correction (proposal):**
  `MinimapSlot_GetFieldByIndex @ 0x579e70` is the nationality-**alignment**
  accessor, not a minimap routine → `CAvatarDefs_GetNationalityAlignment`.
- **Declare structs:** `AvatarPart` (124), `AvatarCombo` (288),
  `AvatarNationality` (616), `AvatarDivision` (36) per the layouts above.
