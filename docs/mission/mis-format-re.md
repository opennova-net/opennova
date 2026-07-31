# MIS mission metafile support

Status date: 2026-07-07 (Nile `misldr.dll` grill; supersedes the 2026-06-18
writer-subset-only status).

Witness images used by this record (never mix their addresses):

- **`misldr.dll`** — the original NovaLogic **Nile** editor's mission-import
  plugin (`C:\Program Files (x86)\Novalogic\Nile\misldr.dll`, Apr 2005;
  IDB `misldr.dll.i64`, imagebase `0x10000000`). Every `@ 0x100xxxxx`
  citation below is in this module.
- **`dfx2med.exe`** — the shipped retail Mission Editor (IDB
  `dfx2med.exe.i64` exists on disk; entity-record field NAMES were witnessed
  from its `Med_WriteMisFile @ 0x454630` at the 2026-06-06 pass — see
  `libs/mission/include/mission/bms.h`). Its full `.mis` grammar grill is
  still pending (D-MIS-3).

## Current verdict

| Facet | Verdict |
|---|---|
| Writer: section set + item keyword vocabulary | **matching** the Nile importer's accepted grammar (`MisLdr_ParseMisLine @ 0x100017b0`, misldr.dll — section keywords `general_information`/`item`/`waypoint`/`event`/`group`/`wav_list`/`layer`/`wind`/`areatrig`/`briefing`/`longbriefing`; unknown sections skip benignly via state 99, unknown keys in known sections fall to a benign default) |
| Position units + component order | **matching** — `.mis` positions are raw 16.16 fixed-point ints, 3rd component vertical (parse stores raw `@ 0x1000288b..0x100028e4`; every consumer scales by `0.000015258789 = 1/65536` in `MisLdr_WriteNileProjectXml @ 0x10004930`) |
| Height SEMANTICS | **divergent → FIXED (D-MIS-4)** — plain item z is the editor-frame (terrain-relative) height; `height_lock 1` declares z ABSOLUTE with `extra_bheight` carrying the baked base height (full witness below). Our exporter wrote absolute BMS z with neither → every object floated by the local terrain height in the original editor |
| Facing convention | **matching** — text order `facing <yaw> <pitch> <roll>`; the editor consumes token 1 as the heading with the engine's `90 − yaw` convention (`THETA = 90 − rec[+28]` `@ 0x10005112` et al.), token 2 raw (`PHI`), token 3 raw (`OMEGA`) — the same `90 −` in-plane zero witnessed at the engine BMS grill (bms-event-runtime-re.md §6.6) |
| Numeric parsing | **divergent → FIXED (D-MIS-5)** — the original parses every `.mis` numeric with `atol` (base 10, always; `j__atol` callers throughout `MisLdr_ParseMisLine`); our reader used `strtol(..., 0)` (auto-base: zero-padded values parsed as OCTAL) |
| Reader/writer field symmetry | **divergent → FIXED (D-MIS-5)** — `fog_level`/`water_level` write/read used different header fields (u32 raw vs u16 override, truncating), `gen_def_val1..4` were write-only, and parse-time authoring defaults mutated zero-valued fields on round-trip |
| Item pool classification on read | tracked open (D-MIS-1, narrowed — the Nile witness shows classification by `items.def` TYPE STRING; below) |
| Full MED (`dfx2med.exe`) grammar | tracked open (D-MIS-3) |

## The Nile importer witness (misldr.dll, 2026-07-07)

The Nile editor imports `.mis` through `misldr.dll` and converts it to its
native `NILE_PROJECT` XML scene; the DLL is import-only (Nile saves its own
format). Driver: `MisLdr_ImportMisFile @ 0x10006610` — tokenize/parse the
`.mis` via `MisLdr_ParseMisLine @ 0x100017b0` → load `items.def` through the
SCR text loader with key `0x2A5A8EAD` (the JO/DFX2 `.def` key,
`libs/scr` `SCR_KEY_JO_DFX2`) via `MisLdr_ParseItemsDefLine @ 0x10006370`
(`id` stored −100000; `attrib:` tokens LFP/EWeap/SpawnPoint/ChangeTeam set
flag bits) → emit the XML scene via `MisLdr_WriteNileProjectXml @ 0x10004930`.

**The item record** (0x170 bytes at doc+0xC0308, `MisLdr_ParseMisLine`
case 2): `type_id`→+0, `id`→+4, `position` tokens 1/2/3 → +12/+16/+20 (raw
`atol`), `facing` tokens → +24(tok 2)/+28(tok 1)/+32(tok 3), team→+36,
attributes→+44, name_index→+56, waypoint pair id→+76, radius→+80,
`formation`→+0xA0, `icon_value`→+0xA4, ammo set→+0xA8..+0xB4,
`extra_val1..5`→+0x100..+0x110, `extra_valmode`→+0x114,
**`extra_bheight`→+292**, `ai_textfile`→+0x128 (string), `gen_string`→+0x13C,
`sibling_id`→+0x15C, `mission_critical`→+352, **`height_lock`→+356**,
`lfp_group`→+360, `team_budget`→+364. `iai_name` is read and IGNORED (the
editor rederives it from `items.def`). `name "Ruler"`/`"MapTile"` overrides
the record to the terrain-tile pseudo-type 101.

**The height semantics** (`MisLdr_WriteNileProjectXml @ 0x10004930`, all six
entity branches, e.g. the STATIC branch `@ 0x100061c0..0x10006296`):

```
scene X = −position[2nd] / 65536
scene Y =  position[3rd] / 65536 − (height_lock ? extra_bheight / 65536 : 0)
scene Z =  position[1st] / 65536
<ABSOLUTE>TRUE</ABSOLUTE> emitted iff height_lock (rec+356)
THETA = 90 − facing[+28];  PHI = facing[+24];  OMEGA = facing[+32]
```

So: an **un-locked** item's `.mis` z is already the editor-frame
(terrain-relative) height; a **height-locked** item's z is ABSOLUTE world
height and `extra_bheight` carries the base height under it, letting the
editor recover the relative offset (`z − extra_bheight`). BMS entity z is
always absolute (the engine spawn extractor,
[bms-event-runtime-re.md](bms-event-runtime-re.md) §6.6), so a faithful
`.bms`-sourced export writes `height_lock 1` on every item plus the sampled
base height in `extra_bheight`. Area triggers follow the same pattern on
their Y1/Y2 pair (`@ 0x10005a92/0x10005b35`).

**Other value semantics witnessed**: `WATER_LEVEL = header × 0.5`
(half-units, matching the BMS header convention); waypoint-list membership
= items of type 6005 grouped by the pair id at +76; area triggers = 6013
pairs; classification into the scene = the `items.def` TYPE STRING —
`decoration`/`building`/`foliage` → STATIC, `object` → DYNAMIC (LFP-flagged
→ LFP), `powerup` → STATIC, `vehicle` → VEHICLE, `person` → AI, `marker` →
MARKER, `effect` → EFFECT (`@ 0x10004f06..0x10005e63`). The
`general_information` keys parse into the doc header with plain `atol`
(`lowest_elev`→doc+0x270, `wp_names_blue/red` arrays→+0x278/+0x298,
`gen_def_val1..4`→+0x40EEB0..BC, `win_scores`/`subgoals_*` byte/word
arrays, ...).

## Implemented parser/writer subset

The writer emits the Nile-accepted grammar above from the in-memory
document: `begin general_information` (header fields incl. the raw-u32
`fog_level`/`water_level` at header offsets 156/152 and `gen_def_val1..4`),
empty `weapon_availability` (benignly skipped by Nile), optional
`briefing`, `areatrig`/`waypoint`/`group`/`layer` records, events with
nested `triggers`/`actions`, and `begin item N` blocks — now including
**`height_lock` + `extra_bheight`** per item (D-MIS-4) and unconditional
emission of the fields whose parse defaults are non-zero (D-MIS-5's
round-trip stability). The reader accepts the same shape, parses base-10
only, and round-trips `height_lock`/`extra_bheight` as `.mis`-interchange
fields on `bms::Entity` (never serialized into `.bms` bytes — the 172-byte
BMS record has no such fields; they are text-authoring concepts).

## Divergence catalog (D-MIS)

| ID | Divergence | Disposition |
|---|---|---|
| D-MIS-1 | All parsed `begin item` records land in the generic item pool; the `.mis` text does not carry the BMS pool kind | **NEEDS-RE (narrowed 2026-07-07)** — the Nile witness pins classification to the `items.def` TYPE STRING (`MisLdr_WriteNileProjectXml` branch chain), matching the placer's empirically-verified `_kind_for_item_type` mapping (Person→Organic; Building/Decoration/Foliage→Building; Vehicle/Object/Powerup→Item; 185,325 entities / 114 missions, 1:1). Porting the same classification into the `.mis` reader closes this; the `dfx2med.exe` confirmation rides D-MIS-3. |
| D-MIS-2 | `weapon_availability` is emitted as an empty section and skipped on read | **SEMANTICS CLOSED (2026-07-18, the loadout grill)** — Nile skips unknown/empty sections benignly (state 99). The .bms secondary chunk it mirrors is the per-map `{name, statusByte}` weapon-rules list: `Mission_LoadBMSFile` seek-skips it at load `[orig: @ 0x40f6d1/@ 0x40f751]`, and the mission-LIST scanners compile it into the 4584-B mission-list entry (+3356 availability template, +4376 class word) at menu time `[orig: Mission_BuildMapListFromPFF @ 0x562910 / MissionList_ScanAndBuildFromFiles @ 0x563170 -> build_item_restriction_table @ 0x54ddb0]`; value semantics + the runtime promote (`MissionDocument::item_availability` -> `NovaSimulation.set_weapon_availability`) in net-re §5.63. The .mis text section stays empty-emitted pending the dfx2med grammar grill (D-MIS-3). |
| D-MIS-3 | Hand-authored / legacy `.mis` variants and the full shipped-MED grammar are not fully mapped | **NEEDS-RE** — the `dfx2med.exe` grill (IDB on disk, not yet MCP-opened). The Nile misldr grammar (this record) is the first original-consumer witness. |
| D-MIS-4 | `.mis` item heights: our exporter wrote absolute BMS z with `extra_bheight 0` and no `height_lock` — the original editor reads un-locked z as terrain-relative, floating every object by the local terrain height (the 2026-07-07 user repro) | **FIXED (2026-07-07)**: the writer emits `height_lock 1` per BMS-sourced item + `extra_bheight` from the editor-sampled base height (mission workspace passes terrain heights in write order; 0 when no terrain); the reader round-trips both as transient interchange fields `[orig: MisLdr_ParseMisLine @ 0x100017b0 (height_lock→+356, extra_bheight→+292); MisLdr_WriteNileProjectXml @ 0x10004930 (Y = z − (lock ? bheight : 0)); both misldr.dll]` |
| D-MIS-5 | Reader/writer asymmetries corrupted round-trips: `strtol` base-0 parsed zero-padded numerics as OCTAL (`minutes_per_day 0120` → 80 → 0 on the next pass) vs the witnessed base-10 `atol`; `fog_level`/`water_level` wrote header u32s @156/@152 but read back into u16 override fields (45875200 → 0); `gen_def_val1..4` write-only; parse-time authoring defaults rewrote zero-valued fields (all 1365 entities on retail 00TRg gained `nomorethan 1`) | **FIXED (2026-07-07)**: base-10 parsing throughout; write/read symmetric on the same header fields; `gen_def_val*` parsed; the defaulted item fields emit unconditionally — `.bms`→`.mis`→`.mis` is byte-idempotent on the retail fixture (ctest-pinned) |

## Verification coverage

- `tests/mission/mission_mis_test.cpp`: author → save `.mis` → reload →
  field asserts, now incl. `height_lock`/`extra_bheight` round-trip, the
  octal regression (`0120` → 120), and `fog_level`/`gen_def_val*` symmetry.
- Retail-fixture idempotency: `fixtures/bms/ash_i5b.reference.bms` →
  `.mis` gen1 → parse → gen2, byte-equal (the fixture-blind-spot closer:
  the authored-fixture tests exercise the same defaults the parser seeds,
  which is how D-MIS-5 hid).
- `tests/mission/mission_c_abi_test.cpp`: extension dispatch.
- Godot: `NovaMissionData` save/open dispatch, the workspace Save As
  filters, and the base-height sampling seam.
- Acceptance: export retail `00TRg.bms` → `.mis`, open in the Nile editor —
  object heights must match the `.bms` placement (the original repro).
