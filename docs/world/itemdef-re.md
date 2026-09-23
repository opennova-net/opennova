# ItemDef — reverse-engineering record

Structure-mapping record for the original engine's runtime **`ItemDef`** (the
per-item-type template loaded from `items.def`) and its copy into the 904-byte
`GamePlayerEntity`. The reimplementation surface is the parsed model
`DefItemDef` (`engine/formats/def`) and the Godot wrapper
`ItemDatabase` (`godot/src/object`); the runtime entity copy lands in
`engine/runtime/world` / `engine/runtime/replication`. Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the divergence catalog code comments cite as
`docs/world/itemdef-re.md (D-ITEMDEF-…)`.

This record was produced by a read-only IDA grill: the `ItemDef` and
`GamePlayerEntity` structs in the IDB were expanded/renamed during the session
(`ItemDef` 71 → 142 named members; the entity gained the model-pointer and
`deathCallback` names), and 1821 dead `KONG_*` analyzer types were purged. No
behavioral ctest is produced here — the evidence is the cited decompilation.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| `ItemDef` struct layout (2780 B) | **MATCHING (read-only grill)** | 142 members named from the parser/dumper/allocator/resolver/loader; field offsets witnessed by `ItemDef_ParseProperty @0x49eb00`, `ItemDef_DumpToFile @0x49e250`, `ItemDef_AllocateWithDefaults @0x49e3b0`, `ItemDef_ResolveAllResources @0x49e5f0`, `EntityDef_LoadModelsAndCallbacks @0x439f50` |
| `ItemDef → GamePlayerEntity` copy | **MATCHING** | `Entity_InitFromItemDef @0x49e550` decompiles field-for-field clean (callbacks/models/health/armor/timer) |
| Type-id resolution and `ItemTypeIndex` | **MATCHING** (2026-09-23; was last-wins) | `ItemList_FindIndexByTypeId @0x49e100` first match, stored at `@0x40EBFC`; ctests `mission_item_traits`, `route_parity`, `netsim_item_replication_catalog`; GUT `mission_data_test.gd` |
| Scaled numeric keys (`_ftol2_sse`, `scale`) | **MATCHING** (2026-09-23) | the SSE2 `cvttsd2si` leg and the inline `fistp qword` for `scale` (witness map below); ctests `def_parse_items`, `doors` |
| `type` enum (`ItemDef+0x5c`) | **MATCHING** (was DIVERGENT; fixed **D-ITEMDEF-1** 2026-07-05) | `engine/formats/def` `item_type_from_string` now returns the witnessed engine values (named `DefItemType`); pinned by `tests/def/def_parse_item_type_test.cpp` |
| `attrib` / `attrib2` flags (`+0x54`/`+0x58`) | **documented + parsed** | full bit map witnessed in `ItemDef_ParseProperty`; now parsed into `DefItemDef.attrib`/`attrib2` (`engine/formats/def`, `attrib:` token line) and consumed by the net `0x0D` AI-trailer gate (`Entity::is_ai_capable` ← `attrib & 0x100000` / `AIData`; see net-re D-NET-97) |
| `phrase_set` (`+0x86c`) | **documented + parsed with presence** | `ItemDef_ParseProperty @0x49eb00`: `_stricmp("phrase_set") @0x49f9de`, `atol @0x49f9f0`, store to the 0xADC-stride item at `@0x49fa0a`; mounted bone selection reads the target definition dword at `Entity_BuildBoneTransformMatrices @0x4b1884`. `DefItemDef` retains a separate validity bit because authored zero is meaningful |
| `DefItemDef` parsed model (`engine/formats/def`) | **partial, MATCHING on covered fields** | parses a faithful subset (id/type/graphic/anim_def/husk/hp/sound_profile/soundloops/shots/`*_function`); the runtime struct is far wider (see follow-ups) |

## Globals

- `gItemDefs @ 0xB46250` — `ItemDef[]`, stride **2780** (0xADC).
- `gItemCount @ 0xB46254` — populated count.
- `ItemList_FindIndexByTypeId @ 0x49e100` — linear scan from row 0 matching
  `.id` (`ItemDef+0x50`) → array index; it returns on the FIRST hit
  (`cmp [ecx],esi; jz` `@0x49E120..0x49E122`) and 0 when no row carries the id.
  The index is what lands in `entity+28` (`ItemTypeIndex`).

## Witness map

- **`ItemDef_ParseProperty @ 0x49eb00`** (0x31B0 B) — the NSI property
  dispatcher. Each `_stricmp(propName, "<name>")` branch stores into
  `gItemDefs[ctx.idx]+offset`; this is the primary source of the field map.
  Passed as the per-property callback by `ItemDefs_LoadAndValidate @0x4a1da0`
  and `ItemDefs_LoadFromNSIFiles @0x4a1cb0`.
- Its **`phrase_set` branch** is exact: `mov eax,[edi+4] @0x49f9db` selects the
  0xADC-stride item, `_stricmp("phrase_set") @0x49f9de` recognizes the key,
  `mov ecx,[edi+8] @0x49f9f0` supplies the value to `atol`, and
  `mov [edx+esi+86Ch],eax @0x49fa0a` stores the signed dword. The consumer reads
  this field from the **mounted target's definition** at
  `Entity_BuildBoneTransformMatrices @0x4b1884`; it is the skeletal overlay
  config as well as the `emplaced_N` family selector, not collision metadata.
- Its **scaled numeric keys** convert through `_ftol2_sse`: `sqb_rate`,
  `sqb_distance`, `sqb_error` (the calls `@0x49F093`, `@0x49F0DC`,
  `@0x49F125`), `open_rate` / `max_angle` (`@0x49F91E`, `@0x49F96D`),
  `destroy_timing` (`@0x49EE7E` / `@0x49EEA5` / `@0x49EECF`),
  `particletesttime` (`@0x49FAD1`) and the four regional shots
  (`@0x49FC79..0x49FE5C`). The shipped game takes that helper's SSE2 leg: the
  CRT flag `dword_334A444`, set by `sub_7887AF @0x7887AF` (the store
  `@0x7887B4`) on every SSE2 processor, selects the `cvttsd2si` path
  (`_ftol2_sse`, the leg `@0x76BC15`), so a NaN, an infinity (`open_rate 0`,
  `sqb_rate 0`) or any truncation outside int32 (`max_angle` above 180) stores
  `0x80000000`. `scale` is the one inline conversion: `atof` (`@0x49F6F9`) times
  65536 (`fmul dbl_7C3CC0` `@0x49F6FE`), round-toward-zero (`or eax,0C00h`
  `@0x49F710`), `fistp qword` (`@0x49F728`) and the low dword stored to
  `+0x1B8` (`@0x49F736`): a value past int32 wraps and an infinity stores 0.
  Ported 2026-09-23 (`io::retail_fistp_truncate_low_dword` for `scale`, the
  shared SSE2 ftol helper for the rest; ctests `def_parse_items`, `doors`).
- **`ItemDef_DumpToFile @ 0x49e250`** — debug dump; the cleanest field↔name
  pairs: `type_id` `+0x50`, `attrib` `+0x54`, `attrib2` `+0x58`, `type`
  `+0x5c`, `graphicName` `+0x60`, `huskName` `+0x70`, `shadowName` `+0xa0`,
  `graphic ptr` `+0xf0`, `graphicx ptr` `+0xf4`.
- **`ItemDef_AllocateWithDefaults @ 0x49e3b0`** — `memset(0, 2780)`, sets
  `defaultRes = SoundProfile_FindSlotByName("default")` (so the `defaultRes*`
  slots are sound-profile handles), then the vehicle-physics defaults
  (climbSpeed=1, torque=3, mass=5, shock=4, springComp=20, lean=5, flip=45…).
- **`ItemDef_ParsePhysicsProperty @ 0x49d870`** — parses the physics block at
  `+0x8d8…+0x948` (already named in-IDB).
- **`ItemDef_ResolveAllResources @ 0x49e5f0`** — resolves the sound region of
  `pad_270`: a 7-entry stride-24 name array (`soundDeath`/door/shot-TOD at
  `0x6db…0x76b`), `soundLoop[7]` names `0x783…0x82b` → resolved ids
  `0x82c…0x844`, then door/shot/death resolved ids `0x848…0x863`; also copies
  default-resource render/anim slots into later tail fields. The now-named
  `phraseSet @+0x86c` is the parser-written mounted config, not that scratch.
- **`ItemDef_ResetAllRuntimeCounters @ 0x49e9c0`** — zeroes exactly the
  resolved-sound-id dwords `pad_270[1468…1520]` (NOT `0xf0…0x12c`; that
  confirms `0xf0…0x12c` are load-time model pointers, not runtime counters —
  **D-ITEMDEF-2**).
- **`EntityDef_LoadModelsAndCallbacks @ 0x439f50`** — loads each name string
  via `sub_5B6160` into its model pointer (`graphic→0xf0`, `husk→0xf4`,
  `huskFinal→0xf8`, `graphicEnemy→0xfc`, `huskShadow→0x118`,
  `virtualDisplay→0x12c`), binds bone callbacks, and resolves seat attach
  points from the primary model's bone user-points: `"sitex"` → `seatMask`
  `+0x25c` bit + `seatBoneIndex[8]` `+0x25d`; `"ctrlx"`/`"drvrx"` →
  `controlBone` `+0x265`; `"UseGun"` → `useGunBone` `+0x266`. `type==8`
  (effect) and `type==3` (person) take special branches. The seat walk
  (`@0x43A47B..0x43A5CD`) zeroes the slot bytes (`@0x43A47B`,
  `@0x43A484` / `@0x43A48A`) and scans the raw userpoint names (+0x20, 48-byte
  stride, no trim): `sitex`, `ctrlx` and `drvrx` are five-character
  `strnicmp` prefixes (`@0x43A4BC`, `@0x43A50B`, `@0x43A549`; the seat store
  `seatBoneIndex[passengers++] = row + 1` `@0x43A4F0`), `UseGun` a whole-name
  `_stricmp` (`@0x43A582`); the last match wins for `controlBone` and
  `useGunBone` (`@0x43A532`, `@0x43A570`, `@0x43A5A9`), and `cmp ebp, 8; jg`
  (`@0x43A5AF`) ends the scan after a ninth `sitex`, whose store lands on
  `controlBone` (+0x25D + 8). Ported 2026-09-22 as `extract_seats`
  (`mission/seat_spec_extract.cpp`, ctest `mission_seat_spec_extract`); no stock
  JOX model has more than eight `sitex` rows, a second `ctrlx`/`drvrx` or a
  suffixed or padded `UseGun`, so stock seat tables are unchanged.
- **`Entity_InitFromItemDef @ 0x49e550`** — the item-template→entity copy
  (table below).

## ItemDef field map (2780 B; selected — full layout is in the IDB)

| Offset | Field | Type | Source property / note |
|---|---|---|---|
| 0x00 | `name` | char[48] | definition name |
| 0x30 | `alias` | char[16] | |
| 0x40 | `uiname` | char[16] | `uiname` |
| 0x50 | `id` | u32 | unique type_id (player infantry = 0x14B9) |
| 0x54 | `attrib` | `ItemDefAttrib` | `attrib:` bitset (`&0x100000` = AI class, §5.6) |
| 0x58 | `attrib2` | `ItemDefAttrib2` | second bitset (vehicle/turret/shadow) |
| 0x5c | `type` | `ItemDefType` | `type` (person/vehicle/…) — see enum + **D-ITEMDEF-1** |
| 0x60/0x70/0x80 | `graphic`/`husk`/`huskFinal` | char[16] | model names |
| 0x90/0xa0/0xb0 | `graphicEnemy`/`shadow`/`huskShadow` | char[16] | model names |
| 0xc0/0xd0/0xe0 | `animDef`/`virtualDisplay`/`attachBoneName` | char[16] | |
| 0xf0–0xfc | `graphicModel`/`huskModel`/`huskFinalModel`/`graphicEnemyModel` | void* | resolved by `EntityDef_LoadModelsAndCallbacks` |
| 0x118 / 0x12c | `huskShadowModel` / `virtualDisplayModel` | void* | |
| 0x130/0x13c/0x150/0x15c/0x168 | `aiFunctionClass`/`renderFunctionClass`/`moveFunctionClass`/`diskFunctionClass`/`inputFunctionClass` | void* | `*_function` class slots (§5.10b) |
| 0x138/0x148/0x158/0x164 | `deathCallback`/`initCallback`/`updateCallback`/`serializeCallback` | void* | the four entity-copied callbacks |
| 0x178/0x17a | `radarSig`/`heatSig` | u16 | |
| 0x17c/0x17e | `healthMax`/`armorMax` | i16 | `hp` / `armor`(=`mana`) → entity (§6.8) |
| 0x180/0x182/0x184 | `criticalHp`/`criticalDrain`/`nonCriticalRegen` | i16 | |
| 0x188 | `damageReducPp` | float | `damage_reduc_pp` |
| 0x194/0x196/0x198 | `score`/`unitType`/`kz` | i16/i16/float | `score` (`ItemDef_ParseProperty @0x49eb00`, the key `@0x4A0228..0x4A0242`) is read off the VICTIM's definition: `Score_ProcessKillEvent @0x4FD400` returns when the victim's ItemDef is missing or its `score` word is zero (`@0x4FD41E` / `@0x4FD422`), and scorer event 12 books the signed word as the unit score (world-wac-ai-re §20.4). No Player definition authors `score`, so a Player victim never tallies; the port carries it as `Entity::item_score` |
| 0x1a4–0x1ac | `destroyTiming0..2` | i32 | `destroy_timing` → entity `destroyTimer` |
| 0x1b0/0x1b2/0x1b8/0x1bc | `reverb`/`music`/`scale`/`debrisScale` | i16/i16/i32/float | `scale` is Q16: `atof` × 65536 through the inline `fistp qword` (witness map above) |
| 0x218 | `lightTransfer` | float | |
| 0x25c–0x266 | `seatMask`/`seatBoneIndex[8]`/`controlBone`/`useGunBone` | u8 | resolved from model bone user-points |
| 0x268/0x26c | `defaultRes`/`defaultResDup` | u32 | sound-profile slot handles |
| 0x270 | `foliageDebrisRef` | u32 | `cactdeb`/`palmdeb`/… |
| 0x278 | `particleEffects` | char[723] | the per-item effect table, decomposed 2026-07-13 (`ItemDef_ParseProperty @ 0x49eb00` key sites `@ 0x4a13ad..0x4a179d`): slot A `particlefx` {effect 0x278, userpoint 0x298}; slot B `particlefxs` {0x2ae, 0x2ee, secondary 0x2ce}; slots C–F `particlefxw1..4` {0x304/0x344/0x324; 0x35a/0x39a/0x37a; 0x3ae/0x3ce (no secondary); 0x3e2/0x402 (no secondary)}; effect-only `particledeath` 0x416, `particleh2odeath` 0x44a, `particlefire` 0x47e, `particleother` 0x4b2, `particlespawn` 0x506, `particlefinale` 0x4e4. Resolved at mission start (`resolve_item_materials_and_spawn_bone_trails @ 0x522ee0`): handles/masks pack just AHEAD of each name block (slot A handle 0x274 + mask 0x276; slot B +48/+50/+52 relative to the name base; death/fire/other mask the HUSK's fixed `Dead`/`Fire`/`Other` points); userpoint→mask = `ItemDef_GetBoneMaskByName @ 0x49ea40` (exact stricmp, first 16 points). Parsed by `engine/formats/def` (`DefItemParticleFx`); slot A is ported by D-PTL-15 and the watercraft W3/W4 live route by D-PTL-25 |
| 0x548 | `parent` (the tail of the 0x278 blob, `particleEffects[720]`) | u8 | the items.def attrib token `Parent` (`ItemDef_ParseProperty @ 0x49eb00` attrib arm; jo-c kong.c 193609..193611): the class initializers test it after the enter handler (`@ 0x46895a` / `@ 0x468688`) and call `Entity_SetupGunnerAttachments @ 0x468100`, which rides the vehicle's same-refNum pool-1 peers on its `agun*` userpoints; the vehicle DYING enter kills that list under the same byte (`@ 0x467b6e`). Witnessed 2026-09-12 (vehicle-client-movers-re §26.2); parsed the same day into `DefItemDef::attrib_parent` (`def_items.cpp` attrib arm, `@ 0x4a0cd6..0x4a0ce2`) and copied into `VehicleTraits::attrib_parent` by the items.def traits sweep (`mission/item_traits.cpp`); the `agun*` points ride `VehicleTraits::agun_points` from the collision resolve (`mission/collision_resolve.cpp`) |
| 0x54b–0x60b | `primaryWeapon`/`ammo*`(×4)/`launchups*`(×3) | char[32]/char[16] | weapon-loadout strings |
| 0x61b | `weaponUserpoints` | char[12][16] | the twelve weapon userpoint NAMES `weaplbup, weaplmup, weaplcup, weaprbup, weaprmup, weaprcup, weaplbup2 .. weaprcup2` (`ItemDef_ParseProperty @ 0x4a0ff2..0x4a1301`): b = fire origin, m = flash anchor, c = casing anchor; r/l/r2/l2 = weapon slots 0/1/2/3 (`Entity_InitBoneReferences @ 0x441470`; world-wac-ai-re §21.5) — `DefItemDef::weapon_userpoints` |
| 0x6db–0x76b | `soundDeath`/`doorOpenSound`/`doorCloseSound`/`dawnShot`/`dayShot`/`duskShot`/`nightShot` | char[24] | sound names |
| 0x783 | `soundLoop` | char[7][24] | `soundloop_1..7` |
| 0x82b–0x863 | `soundFlags`/`soundLoopId[7]`/`doorOpenSoundId`/`doorCloseSoundId`/`shotSoundId[4]`/`deathSoundId` | u8/u32 | resolved sound ids |
| 0x864/0x868 | `defaultResPlus64`/`…Dup` | u32 | sound-profile + 64 |
| 0x86c | `phraseSet` | i32 | `phrase_set` via `atol`; mounted target definition config consumed at `Entity_BuildBoneTransformMatrices @0x4b1884`. Key absence is distinct from an authored value of 0 in the reimplementation |
| 0x890–0x8a0 | `deathTime`/`clipsize`/`doorType`/`openRate`/`maxAngle` | i32/float | polymorphic: `clipsize`@0x894 is the door-item `door_dir` slot reused; the low byte of 0x890 is the door count (`num_doors`), which the `attrib: Door` token defaults to 1 when it is still 0 (`@ 0x4a0cb0..0x4a0cb9`; an authored `num_doors` wins in either order) — `DefItemDef::deathtime_ticks` low byte |
| 0x8d8–0x948 | physics block (`minAI`,`mass`,`torque`,`spring`,`flip`,…) | i32 | `ItemDef_ParsePhysicsProperty` |
| 0x8fc / 0x900 / 0x904 / 0x918 | `spring` / `springComp` / `shock` / `topHeavy` | i32 | raw `atol` `[orig: ItemDef_ParsePhysicsProperty @0x49d870 — spring @0x49db86, spring_comp @0x49dbfe, shock @0x49dc3a, top_heavy @0x49dbc2]`; defaults spring 0, spring_comp 20 `@0x49e496`; per-family clamps spring [0,10], spring_comp [0,100] with `travel = (100 − spring_comp) · 0xFFFF` `@0x476190/@0x47c51f`; `shock` is clamped [0,10] IN PLACE inside both oscillators `@0x45d18f..0x45d1a2` / `@0x45d2b3..0x45d2d2` (no caller-side clamp); `top_heavy` has NO runtime consumer (parser, allocator default, debug editor only — dead). Parsed 2026-08-21 into `DefItemDef` (+ both Python FFI mirrors and the native-stride pins) and `VehicleTraits` — vehicle-client-movers-re §7.3 |
| 0xa74 | `hudImage` | char[32] | `hud_image` |
| 0xad4/0xad8 | `groupMask`/`groupFlags` | u32 | |

## `ItemDef → GamePlayerEntity` copy (`Entity_InitFromItemDef @ 0x49e550`)

The caller sets `entity->ItemTypeIndex` (`+28`, from
`ItemList_FindIndexByTypeId`: the first-match row ordinal, 0 when no row
matches; the store is `mov [esi+1Ch],ebp` in `Entity_SpawnFromBMSRecord
@0x40E9F0` (the site `@0x40EBFC`), and the ItemDef pointer is `gItemDefs +
index` `@0x40EBFF..0x40EC07`) first; then:

| ItemDef field | → GamePlayerEntity field (offset) |
|---|---|
| `&gItemDefs[idx]` | `itemDef` (+0x20) |
| `deathCallback` (0x138) | `deathCallback` (+0x1c8) — invoked by `Entity_KillByNetId` |
| `updateCallback` (0x158) | `updateCallback` (+0x1c4) |
| `graphicModel`/`huskModel`/`huskFinalModel` (0xf0/0xf4/0xf8) | `graphicModel`/`huskModel`/`huskFinalModel` (+0x30/+0x34/+0x38, was `rtCounter0/1/2`) |
| `destroyTiming0` (0x1a4) | `destroyTimer` (+0x1b0) |
| `healthMax` (0x17c) | `Health` (+0x11e) |
| `armorMax` (0x17e) | `Armor` (+0x120) |
| `initCallback` (0x148) | tail-called as `cb(entity)` if non-null and not `Entity_InitFromItemDef` itself |

**A type id resolves to its first row.** The loader never merges rows: each
`begin` allocates the next one (`ItemDef_ParseProperty @0x49eb00`, the
`ItemDef_AllocateWithDefaults` call `@0x49EBA8`; `gItemCount++` `@0x49E3BE`), so
a later row repeating an id is unreachable through `ItemList_FindIndexByTypeId`,
on the host (the spawn store above) and on the client (`NapiNPClientMsg_0x00D
@0x432C40`, the call `@0x4332DA`, which takes `+0x1C`, `+0x20` and the row's
callbacks from that index `@0x4332DF..0x43331A`). The shipped `ITEMS.DEF`
repeats four ids: 100508 (rows 97/98, identical LFP tower poles), 100415 (rows
460/461, the 15- and 30-count mosquito effects), 100439 (rows 485/486, the Heavy
then the Light ground-layer fog mist) and 102044 (row 980, the "Map Named
Location" marker; row 1126, the "Power Up Med Pack Infinite" powerup). Every
port lookup by type id resolves first-wins since 2026-09-23: the items.def
traits sweep and `find_item_def` (`mission/item_traits.cpp`,
`mission/collision_resolve.h`), the facial table, the replication catalog
([ADR 0026](../adr/0026-one-client-replica-pipeline.md) decision 5), the Godot
`ItemDatabase` and `nw_pp`. They had resolved last-wins (the catalog failed
closed), which gave every 102044 marker the med pack's traits and every 100439
fog layer the Light row.

`Entity::item_type_index` carries the ordinal itself: the traits sweep stamps
it, the player template and the throwable class rows seed it for their spawns,
and a placed device copies its template's `+0x1C` as
`Entity_CloneFromTemplateByType @0x4398A0` does (the template gate
`@0x4398A5`). The retail ItemTypeIndex gates (`cmp [reg+1Ch],0`) read it: the
WAC entity handlers, the script trigger, proximity and line-of-sight predicates
and the teleport walks (world-wac-ai-re §32), which had tested the type id as a
stand-in. The two differ only for an entity whose type id has no row. The boat
and aircraft avoid brakes compare the ordinal against 1
([vehicle record](vehicle-client-movers-re.md) section 12, step 8).

## Enums (witnessed in `ItemDef_ParseProperty`)

**`ItemDefType` (`+0x5c`)** — note non-sequential, with shared values:

| value | names |
|---|---|
| 1 | vehicle |
| 2 | decoration, foliage |
| 3 | person |
| 4 | marker |
| 5 | building |
| 6 | powerup, object |
| 8 | effect |

`0` = unset; `7` is unused. `EntityDef_LoadModelsAndCallbacks` branches on
`type==3` (person registration) and `type==8` (effect: sets `entity+436=60`).

**`ItemDefAttrib` (`+0x54`, bitmask)** — `Movecb 0x1`, `Powerup 0x2`,
`NoMoveShoot 0x4`, `NoTool 0x8`, `Snap 0x10`, `EWeap 0x20`, `PlayerControl
0x40`, `Door 0x80`, `NoTarget 0x100`, `Landable 0x200`, `Missile 0x400`, `Tire
0x800`, `FastRope 0x1000`, `Takeable 0x2000`, `Easy 0x4000`, `S&D 0x8000` (the
S&D/A&D objective target: `ItemDef_ParseProperty @ 0x4a084e..0x4a086d` compares the
whole token case-insensitively against the string at `0x7C84E8` = `53 26 44 00`, which
the IDB mis-types as `off_7C84E8`; consumers `reset_round_counters @ 0x516d3d /
@ 0x516d89` count the pool-1/2 carriers per team byte +354 into the S&D target counts,
and the blast applier's same-team gate (jo-c 261654) makes them immune to friendly
blast — `DEF_ITEM_ATTRIB_SD`, `ItemDeathTraits::team_protect`), `4Team 0x10000`,
`ChangeTeam 0x20000`, `SpawnPoint 0x40000`, `Armory 0x80000`, **`Aidata
0x100000`** (the §5.6 AI-class flag), `LeaveCorpse 0x400000`, `NoDismember
0x800000`, `NoWeapon 0x1000000`, `Reflect 0x2000000`, `NoShadow 0x4000000`,
`Concave 0x8000000`, `NoScar 0x10000000`, `NoHud 0x20000000`, `NoDie
0x40000000`. Two tokens have side effects beyond a bit: `Door` (0x80) also
defaults the door count (the low byte of +0x890) to 1 when it is still 0
`@ 0x4a0cb0..0x4a0cb9`; `Parent` sets no bit at all — it writes the byte +0x548 = 1
`@ 0x4a0cd6..0x4a0ce2` (`DefItemDef::attrib_parent`). Both parsed 2026-09-12
(`def_parse_item_attrib` ctest).

**`ItemDefAttrib2` (`+0x58`, bitmask)** — `VehicleBay 0x1`, `AutoInheritTeam
0x2`, `VehicleSpawn 0x4`, `DynamicShadow 0x10`, `StaticShadow 0x20`,
`TunnelPiece 0x40`, `UseVK 0x80`, `StaticDeath 0x100`, `OnTurret 0x400`,
`HasTurret 0x800`, `IsTurret 0x1000` (consumer: Entity_UpdateChildAttachment @0x440a36 selects the rate-limited turret slew + gunner tether, world-wac-ai-re §26.5a), `Farp 0x2000`, `LandMine 0x4000`.

## Vehicle child-emplacement attachments

As of 2026-07-21, `engine/formats/def` parses the authored `addeweap`, `addeweapG`,
and `addeweapC` rows used to attach child guns/equipment to vehicle model
userpoints. The port retains authored order, the retail four-row cap and
15-character userpoint limit, full child item IDs, the last designated G/C
slot, and the optional all-or-none down/up/right/left limits in signed BAM
units. Invalid partial angle tails remain unparsed rather than inventing
defaults.

Mission promotion recursively creates the child item entities and resolves
their model userpoints case-insensitively (falling back to the parent root when
the anchor is absent). On the authority, children follow the resolved live
USRP/PANM pose. The attachment owns the userpoint's complete authored direction
frame, not a gunner-seat yaw offset: retail builds a direction look-at matrix,
multiplies it through the live owning bone and carrier matrix, then writes the
resulting child position plus yaw/pitch/roll every pool-1 update
[`Entity_UpdateTransformAndTurret @ 0x440CA0`, attachment call `@ 0x44109D`,
`build_bone_attachment_matrix @ 0x56C630`,
`build_direction_look_at_matrix @ 0x612C90`]. Missing anchors copy the full
parent pose. The retail helper emits a row-vector render matrix, so the Godot
port applies the same transpose plus X-axis conjugation used for PANM matrices.
`Entity_UpdateAllEntities @ 0x4C2100` walks attachment ancestors parent-first
before `Entity_UpdatePool1Slot @ 0x4B8DD0` invokes the child's `ewep` update
callback, so a driven carrier and its child are recomposed in the same tick.

Their S2C `0x0D` records retain absolute spawn position and use the witnessed
`0x0100` relation flag plus the entity+368 parent handle; after the complete
batch, a remote client derives a rigid parent-local pose and follows the
decoded parent. A replicated zero-health carrier compact retires the decoded
attachment subtree. Direct scripted carrier removal remains open until its
witnessed destroy-list message is mapped; S2C `0x4E` is a paged loadout
transaction and is deliberately not repurposed for this lifecycle.

The entity+290 bone-byte consumer remains unwitnessed, so remote generic
PANM-bone articulation is still open rather than encoded into a guessed wire
field. The G/C designation and authored limits are carried into runtime
metadata, but their specialized control/HUD consumers are likewise deferred;
this slice does not claim those behaviors.

## Divergence catalog

| ID | Ours | Original (Jointops.exe) | Why / consequence |
| --- | --- | --- | --- |
| D-ITEMDEF-1 **[FIXED 2026-07-05, maturity-par-itemdef]** | `engine/formats/def` `item_type_from_string` (`def.cpp:700`): marker=1, vehicle=2, person=3, building=4, decoration=5, foliage=6, object=7, powerup=8; `effect` unhandled | `ItemDef_ParseProperty`: **vehicle=1, decoration=2, foliage=2, person=3, marker=4, building=5, powerup=6, object=6, effect=8** | the reimpl invented sequential-by-order values; only `person=3` agreed. `type` is not wire-serialized, so no interop break, but any runtime/editor branch on `DefItemDef.type` expecting engine semantics (e.g. effect=8, person=3 special-casing in `EntityDef_LoadModelsAndCallbacks`) was wrong. **FIXED:** `item_type_from_string` now returns the witnessed engine values via named `DefItemType` constants (`engine/formats/def/def.h`), including `effect=8`; the `DefItemDef.type` comment is corrected; the `mission` authoring `entity_kind_for_item_type` switch and the Godot `ItemDatabase::TYPE_*` mirror (static-asserted against `DefItemType`) were updated in the same change. Mapping pinned by `tests/def/def_parse_item_type_test.cpp` (full string→value table) and re-cited in `tests/def/def_parse_items_test.cpp` + `tests/mission/mission_authoring_test.cpp`. The mapping is non-injective (decoration=foliage=2, powerup=object=6). The reimpl is parse-only (string→value) and no consumer converts a numeric `type` back to a token, so the collision has no reverse-direction consequence and no reverse table was invented; if a value→string need ever arises it must be witnessed first (open question — `ItemDef_DumpToFile @0x49e250` is the candidate site). |
| D-ITEMDEF-2 | (IDB) `ItemDef+0xf0…0x12c` were auto-named `rtCounter0..4`; entity `+0x30/34/38` likewise | they are load-time resolved **model pointers** (`graphicModel`/`huskModel`/`huskFinalModel`/`graphicEnemyModel`/`virtualDisplayModel`), copied to the entity by `Entity_InitFromItemDef` | renamed in-IDB this session. The actual runtime counters are the resolved-sound-id block `+0x82c…` zeroed by `ItemDef_ResetAllRuntimeCounters`. Supersedes the "+48/52/56 counters" wording in `correspondence.md`/net-re §5.2b. |
| D-ITEMDEF-3 | net-re **§6.8** lifted `healthMax`/`armorMax` out of `pad_17C` and referenced a "§6.9" | full struct now mapped here; there is no §6.9 in net-re | net-re §6.8 is superseded by this record; the `+286`/`+288` health/armor flow is unchanged and re-cited here. |

## Open follow-ups (unwitnessed / partial)

- The RUNTIME semantics of the remaining `particleEffects` fxs/fxw1/fxw2 tiers
  (`Entity_SpawnBoneEffectsAtMask @ 0x458750` from the movement updaters) and the
  damage-state death/fire/other spawns (ptl-format-re §8); the key→offset map itself
  (0x278–0x54b) is decomposed in the field-map row above and parsed by
  `engine/formats/def`. Watercraft W3/W4 are routed by D-PTL-25.
- The `*_function` class slots (`0x130/0x13c/0x150/0x15c/0x168`) are typed
  `void*` from their single-store witness; the exact class-binding record
  (tag vs resolved fn pointers, and how the chosen class' `fn[3]` lands in
  `serializeCallback`/§5.10b dispatch) is not fully traced.
- Residual `gap_*` spans in the original executable layout remain genuinely
  unwitnessed: `+0x1c0…0x218` (the authoring-level `addeweap*` rows are now
  parsed by the port, but their exact retail in-struct representation beyond
  the observed selector bytes at `0x1c1-0x1c3` is not claimed), `+0x21c…0x25c`,
  `pad_94C` interior, the still-unmapped fields surrounding the now-named
  `phraseSet @+0x86c`, and parts of `pad_1B0`.
- `DefItemDef` covers only the net/render-relevant subset; the physics block,
  attrib flags, particle keys, `phrase_set` (with presence), `primary_weapon`,
  and `addeweap*` child attachments ARE parsed. The remaining ordinary
  seat/door/sound tables are not yet parsed by `engine/formats/def`.
