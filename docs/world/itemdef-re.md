# ItemDef — reverse-engineering record

Structure-mapping record for the original engine's runtime **`ItemDef`** (the
per-item-type template loaded from `items.def`) and its copy into the 904-byte
`GamePlayerEntity`. The reimplementation surface is the parsed model
`DefItemDef` (`libs/def`, `apps/importer/pyopennova`) and the Godot wrapper
`NovaItemDatabase` (`godot/engine/object`); the runtime entity copy lands in
`libs/world` / `libs/netsim`. Binary: retail **Jointops.exe** (IDB
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
| `type` enum (`ItemDef+0x5c`) | **MATCHING** (was DIVERGENT; fixed **D-ITEMDEF-1** 2026-07-05) | `libs/def` `item_type_from_string` now returns the witnessed engine values (named `DefItemType`); pinned by `tests/def/def_parse_item_type_test.cpp` |
| `attrib` / `attrib2` flags (`+0x54`/`+0x58`) | **documented + parsed** | full bit map witnessed in `ItemDef_ParseProperty`; now parsed into `DefItemDef.attrib`/`attrib2` (`libs/def`, `attrib:` token line) and consumed by the net `0x0D` AI-trailer gate (`Entity::is_ai_capable` ← `attrib & 0x100000` / `AIData`; see net-re D-NET-97) |
| `phrase_set` (`+0x86c`) | **documented + parsed with presence** | `ItemDef_ParseProperty @0x49eb00`: `_stricmp("phrase_set") @0x49f9de`, `atol @0x49f9f0`, store to the 0xADC-stride item at `@0x49fa0a`; mounted bone selection reads the target definition dword at `Entity_BuildBoneTransformMatrices @0x4b1884`. `DefItemDef` retains a separate validity bit because authored zero is meaningful |
| `DefItemDef` parsed model (`libs/def`) | **partial, MATCHING on covered fields** | parses a faithful subset (id/type/graphic/anim_def/husk/hp/sound_profile/soundloops/shots/`*_function`); the runtime struct is far wider (see follow-ups) |

## Globals

- `gItemDefs @ 0xB46250` — `ItemDef[]`, stride **2780** (0xADC).
- `gItemCount @ 0xB46254` — populated count.
- `ItemList_FindIndexByTypeId @ 0x49e100` — linear scan matching `.id`
  (`ItemDef+0x50`) → array index; the index is what lands in `entity+28`.

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
  (effect) and `type==3` (person) take special branches.
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
| 0x194/0x196/0x198 | `score`/`unitType`/`kz` | i16/i16/float | |
| 0x1a4–0x1ac | `destroyTiming0..2` | i32 | `destroy_timing` → entity `destroyTimer` |
| 0x1b0/0x1b2/0x1b8/0x1bc | `reverb`/`music`/`scale`/`debrisScale` | i16/i16/float/float | |
| 0x218 | `lightTransfer` | float | |
| 0x25c–0x266 | `seatMask`/`seatBoneIndex[8]`/`controlBone`/`useGunBone` | u8 | resolved from model bone user-points |
| 0x268/0x26c | `defaultRes`/`defaultResDup` | u32 | sound-profile slot handles |
| 0x270 | `foliageDebrisRef` | u32 | `cactdeb`/`palmdeb`/… |
| 0x278 | `particleEffects` | char[723] | the per-item effect table, decomposed 2026-07-13 (`ItemDef_ParseProperty @ 0x49eb00` key sites `@ 0x4a13ad..0x4a179d`): slot A `particlefx` {effect 0x278, userpoint 0x298}; slot B `particlefxs` {0x2ae, 0x2ee, secondary 0x2ce}; slots C–F `particlefxw1..4` {0x304/0x344/0x324; 0x35a/0x39a/0x37a; 0x3ae/0x3ce (no secondary); 0x3e2/0x402 (no secondary)}; effect-only `particledeath` 0x416, `particleh2odeath` 0x44a, `particlefire` 0x47e, `particleother` 0x4b2, `particlespawn` 0x506, `particlefinale` 0x4e4. Resolved at mission start (`resolve_item_materials_and_spawn_bone_trails @ 0x522ee0`): handles/masks pack just AHEAD of each name block (slot A handle 0x274 + mask 0x276; slot B +48/+50/+52 relative to the name base; death/fire/other mask the HUSK's fixed `Dead`/`Fire`/`Other` points); userpoint→mask = `ItemDef_GetBoneMaskByName @ 0x49ea40` (exact stricmp, first 16 points). Parsed by `libs/def` (`DefItemParticleFx`); the slot-A runtime attach is ported (ptl-format-re §4, D-PTL-15) |
| 0x54b–0x60b | `primaryWeapon`/`ammo*`(×4)/`launchups*`(×3) | char[32]/char[16] | weapon-loadout strings |
| 0x61b | `weaponPickupAnims` | char[12][16] | `weapl/r b/m/c up[2]` |
| 0x6db–0x76b | `soundDeath`/`doorOpenSound`/`doorCloseSound`/`dawnShot`/`dayShot`/`duskShot`/`nightShot` | char[24] | sound names |
| 0x783 | `soundLoop` | char[7][24] | `soundloop_1..7` |
| 0x82b–0x863 | `soundFlags`/`soundLoopId[7]`/`doorOpenSoundId`/`doorCloseSoundId`/`shotSoundId[4]`/`deathSoundId` | u8/u32 | resolved sound ids |
| 0x864/0x868 | `defaultResPlus64`/`…Dup` | u32 | sound-profile + 64 |
| 0x86c | `phraseSet` | i32 | `phrase_set` via `atol`; mounted target definition config consumed at `Entity_BuildBoneTransformMatrices @0x4b1884`. Key absence is distinct from an authored value of 0 in the reimplementation |
| 0x890–0x8a0 | `deathTime`/`clipsize`/`doorType`/`openRate`/`maxAngle` | i32/float | polymorphic: `clipsize`@0x894 is the door-item `door_dir` slot reused |
| 0x8d8–0x948 | physics block (`minAI`,`mass`,`torque`,`spring`,`flip`,…) | i32 | `ItemDef_ParsePhysicsProperty` |
| 0xa74 | `hudImage` | char[32] | `hud_image` |
| 0xad4/0xad8 | `groupMask`/`groupFlags` | u32 | |

## `ItemDef → GamePlayerEntity` copy (`Entity_InitFromItemDef @ 0x49e550`)

The caller sets `entity->ItemTypeIndex` (`+28`, from
`ItemList_FindIndexByTypeId`) first; then:

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
0x800`, `FastRope 0x1000`, `Takeable 0x2000`, `Easy 0x4000`, `4Team 0x10000`,
`ChangeTeam 0x20000`, `SpawnPoint 0x40000`, `Armory 0x80000`, **`Aidata
0x100000`** (the §5.6 AI-class flag), `LeaveCorpse 0x400000`, `NoDismember
0x800000`, `NoWeapon 0x1000000`, `Reflect 0x2000000`, `NoShadow 0x4000000`,
`Concave 0x8000000`, `NoScar 0x10000000`, `NoHud 0x20000000`, `NoDie
0x40000000`.

**`ItemDefAttrib2` (`+0x58`, bitmask)** — `VehicleBay 0x1`, `AutoInheritTeam
0x2`, `VehicleSpawn 0x4`, `DynamicShadow 0x10`, `StaticShadow 0x20`,
`TunnelPiece 0x40`, `UseVK 0x80`, `StaticDeath 0x100`, `OnTurret 0x400`,
`HasTurret 0x800`, `IsTurret 0x1000`, `Farp 0x2000`, `LandMine 0x4000`.

## Vehicle child-emplacement attachments

As of 2026-07-21, `libs/def` parses the authored `addeweap`, `addeweapG`,
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
| D-ITEMDEF-1 **[FIXED 2026-07-05, maturity-par-itemdef]** | `libs/def` `item_type_from_string` (`def.cpp:700`): marker=1, vehicle=2, person=3, building=4, decoration=5, foliage=6, object=7, powerup=8; `effect` unhandled | `ItemDef_ParseProperty`: **vehicle=1, decoration=2, foliage=2, person=3, marker=4, building=5, powerup=6, object=6, effect=8** | the reimpl invented sequential-by-order values; only `person=3` agreed. `type` is not wire-serialized, so no interop break, but any runtime/editor branch on `DefItemDef.type` expecting engine semantics (e.g. effect=8, person=3 special-casing in `EntityDef_LoadModelsAndCallbacks`) was wrong. **FIXED:** `item_type_from_string` now returns the witnessed engine values via named `DefItemType` constants (`libs/def/def.h`), including `effect=8`; the `DefItemDef.type` comment is corrected; the `mission` authoring `entity_kind_for_item_type` switch and the Godot `NovaItemDatabase::TYPE_*` mirror (static-asserted against `DefItemType`) were updated in the same change. Mapping pinned by `tests/def/def_parse_item_type_test.cpp` (full string→value table) and re-cited in `tests/def/def_parse_items_test.cpp` + `tests/mission/mission_authoring_test.cpp`. The mapping is non-injective (decoration=foliage=2, powerup=object=6). The reimpl is parse-only (string→value) and no consumer converts a numeric `type` back to a token, so the collision has no reverse-direction consequence and no reverse table was invented; if a value→string need ever arises it must be witnessed first (open question — `ItemDef_DumpToFile @0x49e250` is the candidate site). |
| D-ITEMDEF-2 | (IDB) `ItemDef+0xf0…0x12c` were auto-named `rtCounter0..4`; entity `+0x30/34/38` likewise | they are load-time resolved **model pointers** (`graphicModel`/`huskModel`/`huskFinalModel`/`graphicEnemyModel`/`virtualDisplayModel`), copied to the entity by `Entity_InitFromItemDef` | renamed in-IDB this session. The actual runtime counters are the resolved-sound-id block `+0x82c…` zeroed by `ItemDef_ResetAllRuntimeCounters`. Supersedes the "+48/52/56 counters" wording in `correspondence.md`/net-re §5.2b. |
| D-ITEMDEF-3 | net-re **§6.8** lifted `healthMax`/`armorMax` out of `pad_17C` and referenced a "§6.9" | full struct now mapped here; there is no §6.9 in net-re | net-re §6.8 is superseded by this record; the `+286`/`+288` health/armor flow is unchanged and re-cited here. |

## Open follow-ups (unwitnessed / partial)

- ~~`particleEffects` (0x278–0x54b) is mapped as one blob~~ — RESOLVED 2026-07-13:
  the key→offset map is decomposed in the field-map row above and parsed by
  `libs/def`; the remaining thread is the RUNTIME semantics of the fxs/fxw1..4
  movement tiers (`Entity_SpawnBoneEffectsAtMask @ 0x458750` from the movement
  updaters) and the damage-state death/fire/other spawns (ptl-format-re §8).
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
  seat/door/sound tables are not yet parsed by `libs/def`.
