# Avatars.def / player-info avatar selection — reverse-engineering record

Engine-research record for the Joint Operations `Avatars.def` system: the data
file that defines selectable player characters (modular head/body/arms parts
composed into `combo` entries under a `nationality → division` tree) and the
`PLAYER_INFO` menu that surfaces them. Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`, imagebase `0x400000`). All addresses below are that
binary's.

**Status: witnessed (read-only), reimplementation pending.** No `libs/`
counterpart exists yet — this record is the format/behavior specification a
faithful `libs/avatars` + `NovaAvatarDatabase` will be ported from, and the
committed home for the `D-PLAYERINFO-…` catalog that the future code comments
will cite as `docs/playerinfo/avatars-re.md (D-PLAYERINFO-…)`.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| `Avatars.def` grammar + parser | **witnessed — port pending** | full decompile of `CAvatarDefs_ParseConfigLine @ 0x57a3f0`; keyword/brace state machine, all field stores cited |
| Avatar object layout (combo / nationality / division / part structs) | **witnessed — port pending** | three allocators decompiled (`@ 0x579f40` / `@ 0x579ff0` / `@ 0x579e10`); offsets read off field stores; caps off loop/`memset` bounds |
| `PLAYER_INFO` menu consumption | **witnessed — port pending** | `PlayerInfo_PopulateNationalityList @ 0x55d8c0`, `PlayerInfo_PopulateDivisionList @ 0x55da50`, `populate_avatar_combo_list @ 0x560210` decompiled; accessor surface + RTXT string-table resolution + alignment-as-team-filter |
| combo → spawned-player 3D model binding | **unwitnessed (time-boxed)** | consumer set identified but not traced — see **D-PLAYERINFO-1** follow-up |
| second `AvatarDefs_Init` path (`@ 0x53d281`/`@ 0x53d2b4`) | **unwitnessed** | flagged follow-up; different buffer sizes, also parses `Avatars.def` |

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
  `[orig: sub_57A270 @ 0x57a270]`.

So the menu reads the parsed object directly; the display vocabulary lives in the
`"Avatars"` RTXT string table, keyed by the name fields, and the head/body
display names form a "lastname - firstname" character label.

## Divergence / quirk catalog (D-PLAYERINFO)

These are witnessed original behaviors a faithful port must reproduce; IDs are
stable.

| ID | Original (Jointops.exe) | Why / consequence for the port |
| --- | --- | --- |
| D-PLAYERINFO-1 | combo → spawned-player 3D model binding not traced | **open** — see follow-ups. The runtime model resolution from a selected combo is not yet witnessed; the port stops at resolved combo data behind this seam. |
| D-PLAYERINFO-2 | `>= 512` parts → `MessageBoxA("ComboObj Parse Error")` + abort | the part-pool cap is 512; the port should enforce it (error, not silent truncation). |
| D-PLAYERINFO-3 | `graphic` and `graphic_d` write the **same** part field (+76) | `graphic_d` aliases/overwrites `graphic`; only `graphic_j` (+92) and `graphic_s` (+108) are distinct slots. A faithful parser stores both keywords into one field (last wins). |
| D-PLAYERINFO-4 | combo retains only denormalized part data, not the part names/indices | the runtime struct cannot reproduce the `combo <id> <head> <body> <arms>` line. The reimpl's authoring model must *additionally* keep the three reference names to round-trip the writer — a superset; runtime behavior is unchanged. |
| D-PLAYERINFO-5 | nationality list filtered by `alignment` vs `teamIndex` (good→0, evil→1) | the menu population is team-aware; the host port must reproduce the filter and order. |
| D-PLAYERINFO-6 | `nationality`/`division` id token: `if (*idStr > '9') ++idStr;` then `atol` | a single leading non-digit character is skipped before parsing the numeric id. The reimpl parser must mirror this lenient id read. |

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
  `sub_57A270 → CAvatarDefs_FindComboByPackedId`.
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
