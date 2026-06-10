# BMS event runtime — equivalence record (vs Jointops.exe)

Grill session 2026-06-10, IDB `Jointops.exe.kong.i64` (imagebase 0x400000).
Scope: the BMS event evaluator (`libs/mission/event_runtime.{h,cpp}`), mission→world
promotion (`libs/mission/promote.{h,cpp}`), and the system tick order/cadence
(`libs/mission/mission_systems.h`, `libs/wac/wac_system.h`, `libs/world/world.{h,cpp}`).

**Verdict: MATCHING**, with the tracked deviations D-EVT-1..4 below. Every behavioral
claim in this record was read from the decompilation this session; addresses cited inline.

## 1. The original system

### 1.1 Data model (the 24-byte event record)

`EventTrigger_LoadAllData @0x453eb0` freads the event/trigger/action arrays raw and
fixes up per-event trigger/action indices into pointers (24-byte events, 32-byte
triggers, 32-byte actions; allocator tags "Events"/"Triggers"/"Actions"). The on-disk
event record IS the runtime record:

| off | type | meaning |
|---|---|---|
| +0  | u32 | flags: bit0 = repeat (RESET_AFTER), bit1 = PreMission pass, bit2 = PostMission pass |
| +4  | i32 | trigger index → pointer after fixup |
| +8  | i32 | action index → pointer after fixup |
| +12 | u16 | live repeat-cooldown countdown (0 on disk) |
| +14 | u16 | repeat-cooldown reload = authored_value << 6 |
| +16 | u16 | live activation-delay countdown (0 on disk) |
| +18 | u16 | activation-delay reload = authored_value << 6 |
| +20 | u8  | active latch |
| +21 | u8  | trigger count |
| +22 | u8  | action count |
| +23 | u8  | reserved |

### 1.2 Per-event update — `EventTrigger_UpdateEntry @0x454c30`

- If the activation countdown (+16) is nonzero: decrement by 64; on expiry (signed
  16-bit `<= 0` test @0x454cef) clamp to 0, dispatch every action (32-byte stride loop
  @0x454d0e), call the spawn-point fire hook (§1.5). Then FALL THROUGH to the cooldown
  decrement — both timers tick in one call.
- Else if the cooldown (+12) is zero and the latch (+20) is clear: evaluate the trigger
  chain (§1.3). On pass: latch +20=1 (@0x454c7a), arm +16 from +18 (or dispatch
  immediately when +18 == 0), and if flags bit0 (repeat): arm +12 from +14 and RETURN
  (no decrement on the arming call); when +14 == 0, clear the latch immediately
  (the `goto LABEL_24` path) so the event re-fires on its next processing pass.
- Bottom block (@0x454d2d): if +12 nonzero, decrement by 64; on expiry clear the latch
  → the chain is evaluated again. Fire-once events never reach a latch-clearing path:
  +20 latches forever (only the ResetEvent action clears it).
- **Signedness quirk**: countdowns are loaded as unsigned words but the decremented
  value is tested as SIGNED int16 (@0x454cef/@0x454d40). Reload values ≥ 513<<6 wrap
  negative on the first decrement and expire immediately. Replicated, and pinned by
  `test_activation_delay_signed_wrap`.

### 1.3 Trigger chain — `@0x454050` (was sub_454050; rename proposed §5)

Zero triggers → true. Each trigger's own flag bit0 negates its result; the join
operator between the accumulated value and trigger *i* comes from trigger *i−1*'s
flag byte: bit1 = OR, bit2 = XOR, default AND. Ours: `evaluate_chain` — byte-equivalent.

### 1.4 Condition categories — `EventTrigger_EvaluateCondition @0x453620`

- cat 1 = team/group matrix family (group alive/dead counts at `dword_A33FA4/..A8/..AC`,
  12-dword stride), cat 2 = entity state (alive/dead/health/area/vehicle-chain),
  **cat 3 = event-fired** (@0x453a75: `entry[+20] && entry[+16 word] == 0` — the live
  latch window, NOT a sticky bit), cat 4 = mission variable (`dword_C6B240[param1]`
  ==/</>/<=/>= param2 — ours matches case-for-case), cat 5 = a global toggle
  (`dword_815174`, XOR'd 1 on every `EventTrigger_LoadAllData` — unmodeled, D-EVT-3),
  cat 6 = network/session state, cat 7 = input/gameplay checks.
- The kong comment "3=dialog" on 0x453620 is wrong (it reads the event table); comment
  fix proposed in §5.
- Cat-7 input triggers consume bits transactionally: `g_InputActionBits (dword_B3B738)`
  is mirrored into `dword_AE06F8` at chain-eval entry (@0x45405a), bit-toggled on match
  (@0x453bab), and committed back before action dispatch (@0x454c8b/@0x454cfa).
  Unmodeled (input categories return false), D-EVT-3.

### 1.5 Action dispatch — `EventAction_Dispatch @0x4542e0`

Verified case map (ours matches): case 5 MisvarChange sub 1..5 = Set/Add/Sub/Inc/Dec on
`dword_C6B240`; cases 8/9/0xA = Blue/Red/Green win via `Server_ProcessRoundEnd(1/2/0)`;
case 7 PlayWavList gated `(param2==1 || !dedicated)`; cases 3/0x15 = the AI-change
family (`Entity_HandleAlertCommand`/`Entity_HandleAlertStateEvent @0x43dee0`);
**case 0x22 ResetEvent clears ONLY the +20 latch (@0x454974)** — live countdowns keep
ticking; case 0x25 AttachToEmplaced = `EntityPool_FindByNetId(param1)` →
`WacScript_TryMountEntityToVehicle @0x4f70f0`.

On every dispatch the original also activates linked spawn points:
`@0x452ce0` (kong-misnamed "EventTrigger_NotifyEntityDeath") computes the FIRED EVENT's
own index (`(entry - g_Events)/24`), scans the spawn-point table (`dword_B76570`,
count `dword_B76568`) for records whose word +528 references that event, sets their
pending byte +536 (backward-chaining via byte +535), then `SpawnPoint_SkipBlocked
@0x4de310`. Spawn points are not ported — D-EVT-1.

### 1.6 Cadence — three passes, per-system dividers

Witnessed frame structure (`Game_ProcessMainFrame @0x5263f0`, the 62 Hz engine tick —
`current_tick @0x24c1968` increments once per call):

1. `Server_TickUpdate @0x51d7e0` (authority only):
   - `sub_4F81A0 @0x51d8bf` — the **WAC executor**: 14-instruction wrapper that gates
     on `dword_C6EB28` (script disable), counts `dword_C6EAD4` up to **0x3E (62)**
     (@0x4f81b1), then runs `WacScript_ExecuteBytecode` once and increments the run
     counter `dword_C6EAD8` (@0x4f81d3). One VM execution per 62 ticks.
   - every 16th tick (`++dword_C8D808 > 15`): the **normal-event quarter pass**
     `@0x454d50` (kong-misnamed "Entity_SetStateWreckage") — processes ¼ of the event
     list (entries with `(flags & 6) == 0`), cursor `dword_AE06FC` cycling 0..3. Each
     normal event is therefore evaluated once per **64 ticks**, which is exactly the
     64-unit timer quantum: one authored delay unit amortizes to 64 ticks ≈ 1.02 s.
2. `Entity_UpdateAllEntities @0x4c2100` — the AI/entity motor pass, every tick (the
   infantry motor's 2/8/16-tick stagger lives inside it).

So the authoritative order is **WAC → BMS events → AI**, each with its own divider.
PreMission events (`flags & 2`) run via `EventTrigger_UpdateAllWithFlag2 @0x454dc0`
(whole list per call) from the mission-start context (call site 0x525b86); PostMission
events (`flags & 4`) via `UpdateAllWithFlag4 @0x454e00` from the debrief/video contexts
(0x51ea89 / 0x52266c / 0x5263ae). The per-call frequency of those two contexts is not
yet witnessed (D-EVT-4).

### 1.7 Promotion — `Mission_LoadBMSFile @0x40f4e0` / `Entity_SpawnFromBMSRecord @0x40e9f0`

- Spawn order = file order: **items (pool 1) → buildings (pool 2) → markers (pool 3)
  → organics (pool 0)**; organics' used-count counts only successful spawns.
- The SSN every trigger/action references is **authored in the record**: spawn copies
  record dword +8 to entity +124; `EntityPool_FindByNetId @0x4f0a20` matches its low
  16 bits scanning pools 0..3 (mask 0xF; pool 4 excluded). No load-time assignment.
- Markers are full pool-3 entities (type 2044 gets location strings + nav radius).

## 2. What changed in our source this session (all on #61)

| change | grounding |
|---|---|
| `event_runtime`: full `UpdateEntry` port — activation delay + repeat cooldown words, 64-unit decrement, unsigned-load/signed-test wrap, latch semantics, arming-call early-return, concurrent decrement | @0x454c30 |
| `event_runtime`: three passes — pre (flag&2, whole list), post (flag&4, whole list, host-set phase), normal (16-tick gate + quarter cursor) | @0x454dc0/@0x454e00/@0x454d50/@0x51d7e0 |
| `event_runtime`: cat-3 Event trigger reads the latch window (`active && delay elapsed`), exposed as `event_fired()`; NovaSimulation `has_event_fired` rerouted | @0x453a75 |
| `event_runtime`: ResetEvent clears only the latch | @0x454974 |
| `wac_system`: the 62-tick divider moved INSIDE WacSystem (accum `dword_C6EAD4`, pause `dword_C6EB28`, run counter `dword_C6EAD8`); skips the pre-mission pass | @0x4f81a0..@0x4f81d3 |
| `wac/vm`: WAC time base = completed executions (`time_`, [orig: dword_C6EAD8]) for `past`/`ontick`/`elapse`/Ticks — decoupled from the engine tick | @0x4f81d3 |
| `world`: `TickService` REMOVED (its 62:1 reducer gated the whole world tick — wrong layer; the original divides per system). `World::logic_tick` = the 62 Hz engine tick (`current_tick @0x24c1968`) | @0x5263f0 |
| `promote`: SSN = authored record id verbatim (PromoteOptions.first_ssn removed); spawn order items→buildings→markers→organics; markers spawn into pool 3 | @0x40e9f0/@0x40f4e0/@0x4f0a20 |
| `mission_systems.h`: grill-gate comment replaced with the witnessed order | @0x5263f0 |
| engine: NovaSimulation drops the TickService member; `advance_frame()` = one tick per host frame (both tick modes equivalent pending a fixed-62 Hz accumulator, slice D) | — |

Tests pinning the above: `tests/mission/event_runtime_test.cpp` (13 tests: cadence,
delay, signed wrap, cooldown window, reset_after=0 refire, pre-pass exclusivity, cat-3
window, ResetEvent), `tests/wac/wac_behavior_test.cpp` (`test_execution_cadence`),
`tests/mission/promote_test.cpp` (authored SSNs, pool-3 markers, find_by_net_id),
GUT `nova_simulation_test.gd` / `mission_runtime_test.gd`.

## 3. Tracked deviations

- **D-EVT-1 — spawn-point activation on fire is unported.** The original marks linked
  spawn-point records pending on every event dispatch (@0x452ce0 → @0x4de310). No spawn
  point subsystem exists in libs/world yet; revisit when respawn logic is ported.
- **D-EVT-2 — quarter-pass piggyback skipped.** `@0x454d50` also calls
  `Entity_UpdateStuckCounter @0x439dc0` once per 4-pass cycle; unrelated entity
  bookkeeping, not an event concern.
- **D-EVT-3 — condition categories 1 (team/zone matrix), 5 (load-toggle), 6 (net),
  7 (input, incl. the B3B738/AE06F8 transactional bit consumption) return false.**
  Unmodeled subsystems; the matrix family (cat 1/2 bit tests) needs the relation/
  event-matrix port first.
- **D-EVT-4 — pre/post pass call frequency unwitnessed.** Our port runs them once per
  pre-/post-phase tick; the original's mission-start and debrief loops haven't been
  traced for their call rate. Delays on pre/post events depend on it.

## 4. Correspondence map

| ours | original |
|---|---|
| `BmsEventSystem::update_entry` | `EventTrigger_UpdateEntry @0x454c30` |
| `BmsEventSystem::evaluate_chain` | `@0x454050` |
| `BmsEventSystem::evaluate_trigger` | `EventTrigger_EvaluateCondition @0x453620` |
| `BmsEventSystem::dispatch_action` | `EventAction_Dispatch @0x4542e0` |
| `BmsEventSystem::tick` (pre/post/normal passes) | `@0x454dc0` / `@0x454e00` / `@0x454d50` + the 16-tick gate in `Server_TickUpdate @0x51d7e0` |
| `BmsEventSystem::load` | `EventTrigger_LoadAllData @0x453eb0` |
| `WacSystem::tick` (62-divider) | `sub_4F81A0 @0x4f81a0` |
| `WacVm::time()` | `dword_C6EAD8` |
| `World::logic_tick` | `current_tick @0x24c1968` |
| `World::run_logic_tick` system order | `Game_ProcessMainFrame @0x5263f0` (Server_TickUpdate → Entity_UpdateAllEntities) |
| `promote_mission` | `Mission_LoadBMSFile @0x40f4e0` spawn loops |
| `Entity.net_id` | entity +124 ← record dword +8 (`@0x40e9f0`) |
| `EntityRegistry::find_by_net_id` | `EntityPool_FindByNetId @0x4f0a20` |

## 5. Proposed IDA write-backs (NOT applied — IDB writes were declined this session; apply after review)

Renames (dry-run validated 13/13):
- `sub_454050` → `EventTrigger_EvaluateChain` (anchored)
- `Entity_SetStateWreckage @0x454d50` → `EventTrigger_UpdateQuarterRoundRobin` (anchored; current name is wrong)
- `EventTrigger_NotifyEntityDeath @0x452ce0` → `Event_OnEventFired_MarkLinkedSpawnEntries` (probable)
- `sub_4F81A0` → `WacScript_TickEvery62` (anchored)
- globals: `trigger @0xae0704`→`g_Events`, `dword_AE0700`→`g_EventCount`,
  `dword_AE070C`→`g_EventTriggers`, `dword_AE0708`→`g_EventTriggerCount`,
  `dword_AE0714`→`g_EventActions`, `dword_AE0710`→`g_EventActionCount`,
  `dword_AE06FC`→`g_EventQuarterCursor`, `dword_815174`→`g_EventSpecialToggle` (anchored);
  `dword_B3B738`→`g_InputActionBits`, `dword_AE06F8`→`g_EventInputBitsMirror` (probable)

Comment fixes:
- 0x453620 header: cat 3 is event-fired (reads event+20 && +16==0), not "dialog".
- 0x454c30: struct comment — +12/+14 = repeat cooldown live/reload, +16/+18 = activation
  delay live/reload, +21 = trigger_count; flags bit1/bit2 = pre/post PASS selectors.
- Reverse links: on 0x454c30/0x454050/0x453620/0x4542e0/0x454d50 → opennova
  `libs/mission/src/event_runtime.cpp`; on 0x4f81a0 → `libs/wac/include/wac/wac_system.h`;
  on 0x40f4e0/0x40e9f0 → `libs/mission/src/promote.cpp`.

---

*Appendices consolidated 2026-06-10 from scratch notes: `S1_Mission_LoadBMSFile.md`,
`S2-S7_consolidated.md`, `param-semantics.md`, `event-grill-dfx2med.md`,
`game-mode-grill.md`, `mission/ida-writebacks-bms-event-runtime-2026-06-10.md`.
Addresses are Jointops.exe retail (imagebase 0x400000) except §8, which is dfx2med.exe.*

## 6. Appendix: BMS on-disk format evidence (S1–S7)

Format grill of `Mission_LoadBMSFile @0x40f4e0`. Primary fixture
`fixtures/bms/ash_i5b.reference.bms` (BMS v0x13); corpus = 115 shipped retail `.bms`
(all v0x13), **115/115 round-trip byte-exact** (`tests/mission/mission_corpus_test.cpp`,
parse→write byte-exact + `bms::equal` + count invariants; GUT
`mission_corpus_binding_test.gd`, env-gated).

### 6.1 Magic + version gate (@0x40f5aa)

Bytes [0..2] must be `'B','M','S'` AND byte[3] (version) **>= 19 (0x13)** or the load
rejects. Our `is_bms` checks only the three letters — no version floor (lenient;
divergence tracked). The old `bms.h` comment "BMS\x03" was wrong: byte[3] is a version,
fixture value 0x13.

### 6.2 Section order on disk (`Mission_LoadBMSFile @0x40f4e0`)

| # | section | size | notes |
|---|---|---|---|
| 1 | header | 0x268 (616) | staging buffer `byte_A761D0`; all `*_A76xxx` globals = header fields |
| 2 | weapon loadout | header +0x242 bytes | SP: fread → `AIProfile_SanitizeConfigData` rewrites the len; MP: fseek past |
| 3 | second chunk | header +0x246 bytes | **always fseek past** (SP and MP). Our parser never consumes it — latent divergence; round-trips only while the value is 0 (fixture: 0) |
| 4 | items | count@+0xA4 × 0xAC | spawn → pool 1 |
| 5 | buildings | count@+0xA8 × 0xAC | spawn → pool 2 |
| 6 | markers | count@+0xAC × 0xAC | spawn → pool 3 |
| 7 | organics | count@+0xB0 × 0xAC | spawn → pool 0 (file-last, runtime pool 0; used-count = successful spawns) |
| 8 | waypoints | fixed 136 × 128 | flag fixup after read (§6.5) |
| 9 | groups | 0x20 × 64 | only dwords 0/2/3 of each record kept at load |
| 10 | layers | 0x14 × 32 | read and discarded |
| 11 | area triggers | count@+0x240 × 0x20 | → `unk_A32D10` |
| 12 | events/triggers/actions | 3 × i32 counts, then 24/32/32-byte records | `EventTrigger_LoadAllData @0x453eb0`, contiguous events→triggers→actions; event +4/+8 = trigger/action index (runtime pointer fixup). Exact match to our parse |
| 13 | bbox count | i32 | → `dword_A77640` |
| 14 | bboxes | count × 0x24 | min/max canonicalization on load (§6.5) |

Fixed counts (groups 64, layers 32, waypoints 128) match our
`kGroupRecordCount`/`kLayerRecordCount`/`kWaypointRecordCount`.

### 6.3 Header fields (offsets from `byte_A761D0`)

| off | engine | ours | ash_i5b |
|---|---|---|---|
| +0xA4 | item count (`dword_A76274`) | num items | 87 |
| +0xA8 | building count (`dword_A76278`) | num buildings | 1088 |
| +0xAC | marker count (`dword_A7627C`) | num markers | 432 |
| +0xB0 | organic count (`dword_A76280`) | num people | 0 |
| +0x240 | area-trigger count (`word_A76410`) | `area_trigger_count` | 0 |
| +0x242 | weapon-loadout chunk len (`word_A76412`) | `weapon_loadout_chunk_len` | 136 |
| +0x244 | NOT a chunk length (never used as a seek) | `bonus_expiration` | 10 |
| +0x246 | second-chunk len (`word_A76416`, always seeked past) | `unknown8` | 0 |

### 6.4 Entity record 0xAC (`Entity_SpawnFromBMSRecord @0x40e9f0`)

Byte-correct vs our `parse_entity`; round-trip proven. Verdict: format MATCHING; the
AI/runtime semantics below are doc refinements, not parser changes.

| off | field | notes |
|---|---|---|
| +0 | type_id | special branches: 5305 net-id remap, 6005 weapon, 6088 vehicle/teleport-target, 6006 powerup, 2044 navpoint |
| +4 | name_index | `"STRNAME%03i"`/`"LOCATION%03i"` lookup |
| +8 | id | the authored SSN → entity +124 (§1.7) |
| +12 | bmsi_attributes | |
| +16/+20/+24 | pos X/Y/Z | raw fixed-point; file Z = vertical (§6.6) |
| +28 | dword | wp-distance / nav-radius / powerup-radius (`<<16`) per special type; **low word doubles as health** — dual-use |
| +48 | wp_number | → aiData+152 (path index) |
| +52/+54 | accuracy words | aiData+40/+44 = 100 − value |
| +56/+58/+60 | yaw/pitch/roll | int16 degrees; heading = (90 − yaw)·65536/360, pitch/roll used directly |
| +73 | team | values 3/4 skipped when `dword_24D2150 != 4` |
| +74/+75 | no_more_than / no_less_than | attrs 0x20 / 0x10 |
| +76/+77 | two signed-byte AI params | our `unk19` i16 lumps both — round-trip-safe |
| +79 | waypoint enable | → aiData+148 |
| +80 | firing angle | `(b<<8)/360` |
| +120 | gen_string[36] | type 6088: first 31 bytes = parking-spot name (strncpy 0x1F). Bytes 153/154/155 of the record are runtime sub-fields inside it (153 entity-table index; 155 invincible → entityData+538). Our fixed-string IO preserves all 36 bytes verbatim |

### 6.5 Load-time fixups (runtime-only; disk bytes unchanged)

- **Waypoint flag fixup (@0x40fb72)**: the loop advances 4 × 136-byte records per
  iteration; any record with marker_count (dword@4) == 1 gets flags (dword@0) `|= 1`
  (DoesNotLoop). Disproves the 34-byte-subrecord theory; our
  `flags(4) + marker_count(4) + markers` model is correct. Doc-only.
- **Bbox canonicalization (@0x40fcf4)**: 0x24 records, dwords 0..5 =
  (minX,minY,minZ,maxX,maxY,maxZ) in natural X/Y/Z order (no Y/Z swap); per-axis swap
  when min > max. Retail disk is already canonical; **do NOT add the swap to the
  parser** (would break round-trip on a min>max file) — canonicalize when authoring.
- **Area triggers**: fixture count is 0, so our 32-byte AreaTrigger layout and its Y/Z
  swap remain inferred, UNTESTED by the corpus. (Zone-index addressing is confirmed
  separately — §7.3.)
- **Count clamps** (`BMS_LoadAndValidateHeader @0x40e326` and `@0x40f5b5`):
  items/buildings 0x4B0 (1200), markers 0x300 (768), organics 0x100 (256). Over-limit
  shows a warning dialog (fatal if dismissed) then continues — warn-not-reject. Our
  parser stays lenient (no limits).

### 6.6 Axis/rotation conventions (`Mission_LoadBMSAndExtractSpawnPoints @0x40d650`)

Spawn-grid math reads (X@16, Y@20) as the horizontal plane and Z@24 as vertical; grid
`X = (x>>18)+512`, `Y = 512−(y>>18)` (Y inverted). Confirms our `(x, z, −y)` import
transform. Engine heading = **90 − yaw**, pitch/roll direct. Our editor's euler
`(−pitch, −yaw+180, +roll)` differs in the in-plane zero (90 vs 180) and pitch sign —
frame-dependent, **UNRESOLVED pending a visual A/B against the running game**; the file
stores raw int16 degrees either way, so round-trip is unaffected.

### 6.7 Load orchestration

`Game_StartMission @0x524360` drives the load: `BMS_LoadAndValidateHeader` ×3
(@0x524774/@0x524adb/@0x524f66), then `Mission_LoadBMSFile` ×2 (@0x524b5d/@0x524ffa),
plus terrain/net/HUD. Doc-only; we do not mirror the call shape.

## 7. Appendix: trigger/action param semantics

Per-sub-type param domains, RE'd from the runtime evaluators (§1.4/§1.5). Designer
phrasing was cross-checked against the C# format lib
(`github.com/taylorfinnell/opennova` `GetDescription()`); where the two disagreed, IDA
won (one conflict: the MissionVariable operators, §7.4). Scheduler/fold semantics live
in §1.2–1.3 and are not repeated here.

### 7.1 Field → param index map (confirmed)

Trigger record read as `int[8]`: `[0]` condition_flags, `[1]` main_type, `[2]`
sub_type, **`[3..6]` param1..param4**, `[7]` unknown7 (never read → reserved).
Action record as `dword[8]`: `[0]` reserved0, `[1]` action_type, `[2]`
action_sub_type, **`[3..6]` param1..param4**, `[7]` reserved1 (reserved0/1 unused).
Event byte +23 is likewise never read → reserved; byte +20 is the runtime latch
(§1.1), 0 on disk.

### 7.2 Param kinds

| kind | meaning |
|---|---|
| GROUP_REF | group index 0..63 (engine reads `array[12*p]`, stride-48 group records, 64-wide team matrices) |
| ENTITY_REF | BMS/net id resolved via `EntityPool_FindByNetId`/`*ByBmsRef` — NOT an array index |
| ZONE_REF | area-trigger **array index** (§7.3) |
| EVENT_REF | event array index |
| MISSION_VAR | index into `dword_C6B240` |
| DIALOG / WAYPOINT (−1 = nearest) | indices |
| COUNT / THRESHOLD / DISTANCE_M / SECONDS / SPEED_KPH / BOOL / TEXT / BIT | scalars; distances evaluate as `param<<16` → the file holds whole meters |

### 7.3 ZONE refs are indices (`Entity_IsTeamInTriggerBounds @0x43c730`)

`bbox = &unk_A32D10 + 32 * param2` → param2 is the 0-based zone **array index**; the
zone record's id u32 @0 is never read by the bounds test (`Entity_IsBmsRefInTriggerBounds
@0x43e510` likewise). Zone-record layout confirmed: x_min@4 x_max@8 y_min@12 y_max@16
z_min@20 z_max@24 flags@28 (bit 0x02 = constrain-Z; else Z tested ±16384). Editor
consequence: deleting a zone can auto-repair higher param2 refs by decrement; an exact
hit dangles.

### 7.4 Triggers (`EventTrigger_EvaluateCondition @0x453620`)

**main_type 1 = Group.** State arrays indexed by group id: `dword_A33FA4` alert
{1=yellow, 2=red}, `dword_A33FA8` initial count, `dword_A33FAC` current count.

| sub | name | engine test | p1 | p2 | p3 | conf |
|---|---|---|---|---|---|---|
| 1 | GroupSeesGroup | `EventMatrix_TestSpecialBit(p1,p2)` | GROUP | GROUP | — | med |
| 2 | GroupHasTargetedGroup | `TeamMatrix_TestAllied(p1,p2)` | GROUP | GROUP | — | med |
| 3 | GroupAtRedAlert | `alert[p1]==2` | GROUP | — | — | high |
| 4 | GroupDestroyed | `count[p1]==0` | GROUP | — | — | high |
| 5 | GroupAlive | `count[p1]>0` | GROUP | — | — | high |
| 6 | GroupHasLostMoreUnits | `(init[p1]-count[p1])>=p2` | GROUP | COUNT | — | high |
| 7 | GroupAtWaypoint | `RelationMatrix_TestBitB(p1,p2,p3)` | GROUP | GROUP/? | BIT/wp | low |
| 9 | GroupIntact | `init[p1]==count[p1]` | GROUP | — | — | high |
| 10 | GroupIsWithinArea | bounds, zone=`[p2]`, team=p1 | GROUP | **ZONE_REF (index)** | — | high |
| 11 | GroupHoldingGroup | `sub_43C870(p1,p2)` | GROUP | GROUP | — | med |
| 12 | GroupHasMoreUnits | `count[p1]>=p2` | GROUP | COUNT | — | high |
| 13 | GroupHasShotGroup | `TeamMatrix_TestCanSee(p1,p2)` | GROUP | GROUP | — | med |
| 14 | GroupAtYellowAlert | `alert[p1]==1` | GROUP | — | — | high |
| 15 | GroupHasTargetedSingle | `EntityMatrix_TestDamagedBit(p1,p2)` | GROUP | GROUP/ENTITY? | — | low |
| 16 | GroupSeesSingle | `EntityMatrix_TestProximityBit(p1,p2)` | GROUP | GROUP/ENTITY? | — | low |
| 17 | GroupHasShotSingle | `EntityMatrix_TestVisibilityBit(p1,p2)` | GROUP | GROUP/ENTITY? | — | low |

**main_type 2 = Single** (entity by BMS/net id).

| sub | name | engine test | p1 | p2 | p3 | conf |
|---|---|---|---|---|---|---|
| 1 | SingleSeesGroup | `TeamMatrix_TestEnemyBit(p1,p2)` | ENTITY/team | GROUP | — | low |
| 2 | SingleHasTargetedGroup | `TeamMatrix_TestDead(p1,p2)` | ENTITY/team | GROUP | — | low |
| 3 | SingleAtRedAlert | `sub_43E780(p1,2)` | ENTITY | — | — | high |
| 4 | SingleDestroyed | `!Entity_IsAliveByBmsRef(p1)` | ENTITY | — | — | high |
| 5 | SingleAlive | `Entity_IsAliveByBmsRef(p1)` | ENTITY | — | — | high |
| 6 | SingleHasLostMoreUnits | `Entity_HasDamageCapacity(p1,p2)` | ENTITY | THRESHOLD | — | high |
| 7 | SingleAtWaypoint | `RelationMatrix_TestBitA(p1,p2,p3)` | ENTITY/team | ? | BIT | low |
| 9 | SingleIntact | `Entity_HasFullHealth(p1)` | ENTITY | — | — | high |
| 10 | SingleIsWithinArea | bounds, bmsRef=p1, zone=`[p2]` | ENTITY | **ZONE_REF (index)** | — | high |
| 11 | SingleHoldingGroup | `sub_43E2F0(p1,p2)` | ENTITY | GROUP | — | med |
| 12 | SingleHasMoreUnits | `Entity_HasHealthAboveThreshold(p1,p2)` | ENTITY | THRESHOLD (health) | — | high |
| 13 | SingleHasShotGroup | `TeamMatrix_TestAlive(p1,p2)` | ENTITY/team | GROUP | — | low |
| 14 | SingleAtYellowAlert | `sub_43E780(p1,1)` | ENTITY | — | — | high |
| 15 | SingleHasTargetedSingle | `TeamMatrix_TestSpottedBy(p1,p2)` | ENTITY | ENTITY | — | low |
| 16 | SingleSeesSingle | `TeamMatrix_TestEnemy(p1,p2)` | ENTITY | ENTITY | — | low |
| 17 | SingleHasShotSingle | `TeamMatrix_TestAttackedBy(p1,p2)` | ENTITY | ENTITY | — | low |
| 42 | SingleOnTopOf | `Entity_IsInVehicleChain(p1,p2)` | ENTITY | ENTITY | — | high |
| 43 | SingleFartherThan | `Entity_CheckProximity(p1,p2,p3<<16)` | ENTITY | ENTITY | **DISTANCE_M** | high |
| 44 | SingleHasNoLOS | `Entity_DrawConnectionLine(p1,p2,p3<<16)` | ENTITY | ENTITY | DISTANCE_M | high |
| 45 | SingleDoesNotSeeOrFarther | `Entity_CheckLineOfSight(p1,p2,p3<<16)` | ENTITY | ENTITY | DISTANCE_M | high |

**main_type 3 = Event**: `events[p1]` latch window (byte +20 set && word +16 == 0,
§1.4 cat 3). param1 = **EVENT_REF**. High confidence.

**main_type 4 = MissionVariable**: `dword_C6B240[p1] <op> p2`; param1 = MISSION_VAR
index, param2 = compare value. Operators (IDA truth): sub 1 `==`, 2 `<`, **3 `>`**,
**4 `<=`**, 5 `>=`. The C# reference (and our inherited enum) had 3 and 4 swapped;
`bms.h` now carries IsGreaterThan=3 / IsLessThanOrEqual=4, vindicated by dfx2med
(§8.2).

**main_type 5 = SecondTimeThrough**: returns the global `dword_815174` (§1.4 cat 5).
No params. **main_type 6 = Teammate**: sub1 IsEnabled (`!in_session & flag`), sub2
MedicAssisting, sub3 Evacuating. No params.

**main_type 7 = Player.**

| sub | name | engine | p1 |
|---|---|---|---|
| 18 | PlayerBerserk | weap-state & 0x200 | — |
| 19/20/21 | FirstPerson/ThirdPerson/Cockpit | view bits 0x4000000/0x8000000/0x10000000 in `dword_B3B738` | — |
| 22–30 | (unnamed in our enum) | fixed view/state bits | — |
| 32 | (unnamed) | `1 << p1` | BIT index |
| 33 | (unnamed) | `1 << (byte@12 + 15)` | BIT index |
| 34 | PlayerDialogDone | `Dialog_ExistsByIndex(p1)==0` | DIALOG |
| 35 | PlayerDialogFinished | `sub_44E220(p1)` | DIALOG |
| 36 | PlayerAwol | `sub_439DE0() >= p1` | SECONDS outside mission area |
| 37 | PlayerSatchel | `sub_547160(block)` | AREA (per dfx2med §8) |
| 38/39/40/41 | AttachedToSsn/OnSsn/DrivingSsn/OnGun | `FindByNetId(p1)` → vehicle/mount check | ENTITY |

The engine handles player sub-types 22–30, 32, 33 that our enum does not name; they
display as raw values and round-trip.

### 7.5 Actions (`EventAction_Dispatch @0x4542e0`)

| type | name | engine call | p1 | p2 | p3 | sub_type |
|---|---|---|---|---|---|---|
| 1 | RedirectGroupTo | `Entity_SetWaypointByTeam(p1,p2,p3)` | GROUP | wp-type | WAYPOINT (−1=nearest) | — |
| 2 | KillGroup | `Entity_KillAllByNetId(p1)` | GROUP | — | — | — |
| 3 | ChangeGroupAI | `Entity_HandleAlertCommand(block)` | GROUP | value | — | AI sub-type (§8.2) |
| 4 | VaporizeGroup | `Entity_TeleportAllByNetId(p1)` | GROUP | — | — | — |
| 5 | MisvarChange | `dword_C6B240[p1] op p2` | MISSION_VAR | value | — | 1=Set 2=Add 3=Sub 4=Inc 5=Dec |
| 6 | OutputText | `HUD_DisplayTriggeredText(p1)` | TEXT id | — | — | — |
| 7 | PlayWavList | `Dialog_PlayByIndex(p1)` gated p2 | DIALOG/wav | BOOL (1=always) | — | — |
| 8/9/10 | Blue/Red/GreenWin | `Server_ProcessRoundEnd(1/2/0)` | — | — | — | — |
| 11 | GroupVelocity | `Entity_SetMoveSpeedKPH(p1,p2)` | GROUP | SPEED_KPH | — | — |
| 12/13 | AreaAiRed/Blue | `Entity_KillTeamInBounds(block)` | AREA_ID | — | — | AI sub-type |
| 14/15 | SubGoalWon/Lost | win/lose subgoal p1 | SUBGOAL 1..8 | — | — | — |
| 16 | ChangeGTeamAction | `Entity_SetTeamByNetId(p1,p2)` | GROUP/ENTITY | TEAM {0,1,2} | — | — |
| 17 | ChangeGroupAction | `Entity_UpdateNetIdReferences(p1,p2)` | GROUP | GROUP | — | — |
| 18 | GroupTeleportAction | `Entity_TeleportTeamToSpawn(p1)` | GROUP | teleport-target | — | — |
| 19 | RedirectSingleTo | `Entity_SetWaypointForTeam(p1,p2,p3)` | ENTITY | wp-type | WAYPOINT | — |
| 20 | KillSingle | `Entity_KillByNetId(p1)` | ENTITY | — | — | — |
| 21 | ChangeSingleAI | `Entity_HandleAlertStateEvent(block)` | ENTITY | value | — | AI sub-type |
| 22 | VaporizeSingle | `find_entity_by_parent_and_dispatch(p1)` | ENTITY | — | — | — |
| 23 | SingleVelocity | `sub_43DEA0(p1)` | ENTITY | SPEED_KPH | — | — |
| 24 | ChangeSteamAction | `Entity_FindByDCBAndSetFlag(p1)` | ENTITY | TEAM {0,1,2} | — | — |
| 25 | SingleChangeGroup | `Entity_SetNetIdByParentRef(p1,p2)` | ENTITY | GROUP | — | — |
| 26 | SingleTeleportAction | `EventAction_TeleportEntityToSpawn(p1)` | ENTITY | teleport-target | — | — |
| 27 | ParticleEffectAction | `sub_4540E0(p1)` | id | — | — | — |
| 28 | (special) | `EventAction_HandleSpecialTypes(block)` | — | — | — | — (editor marks 28/29 unused) |
| 30 | GroupOpenDoorAction | `Entity_KillDestructiblesByTeam(p1)` (dmg-transition 7) | GROUP/team | — | — | — |
| 31 | GroupCloseDoorAction | `Entity_KillDestructiblesByOwner(p1)` | owner ref | — | — | — |
| 32 | GroupResetHasVisited | `EventTrigger_ClearSlotB(p1)` | GROUP | — | — | — |
| 33 | SingleResetHasVisited | `EventTrigger_ClearSlotA(p1)` | ENTITY | — | — | — |
| 34 | ResetEvent | `events[p1]` latch clear (§1.5) | **EVENT_REF** | — | — | — |
| 35 | ShowWinSubgoal | `dword_AC86EC` bit p1 + HUD notif | SUBGOAL 1..8 | BOOL show/hide | — | — |
| 36 | ShowLoseSubgoal | same via `dword_AC86E8` | SUBGOAL 1..8 | BOOL | — | — |
| 37 | AttachToEmplaced | `FindByNetId(p1)` → mount (§1.5 case 0x25) | ENTITY | — | — | — |
| 38 | SetLightState | `sub_5A8C80(p1,p2)` | id | state | — | — |
| 39 | Teammates | sub1 `HeliLift_SpawnPickup(p1,p2)`, sub2 `…Flyover` | p1 | p2 | — | 1=pickup 2=flyover 3=evac-AT |
| 40 | ShowWaypoints | `sub_58FB50(p1)` | bool | — | — | — |
| 41 | ExecuteWac | not dispatched here (WAC subsystem; no-op in this dispatcher) | — | — | — | — |
| 42–49 | Ssn/GroupTarget{Ssn,Group}{Pri,Exc} | `Entity_Set{Alert,Action,Waypoint,Target,Weapon}*(p1,p2)` | ENTITY/GROUP | target | — | — |

Types 42–49: the helper names (`SetAlert/SetAction/SetWaypoint/SetTarget/SetWeapon
ByNetId/ByBmsRef`) are shared and do not map 1:1 onto the enum's `*Pri/Exc` names —
per-value semantics **uncertain**; the editor offers pickers with a
"semantics unverified" note rather than asserting a meaning.

## 8. Appendix: dfx2med editor cross-checks

Cross-grill against `dfx2med.exe` (DFX2 Mission EDitor, md5
`e690e69e94029c6b9cc44e934e96e3be`, imagebase 0x400000, IDB `dfx2med.exe.i64`) — same
engine lineage as JO, shared `.bms` format. **All addresses in this section are
dfx2med.exe.** Verdict (events editor vs dfx2med): **MATCHING** — enum tables, action
types, condition tokens, and misvar operators vindicated; param scaling raw; value
domains as below.

### 8.1 Name-getter functions (canonical integer → token)

All route through `Med_LookupConfigString @0x4627e0` (`("Triggers", TOKEN)` config
lookup); the TOKEN literal is the canonical engine name.

| fn | role | fallback string |
|---|---|---|
| `@0x445960` | trigger main-type name(type) | `??? UNKNOWN Trigger (%d)` |
| `@0x445a50` | action-type name(type) | `??? UNKNOWN Action (%d)` |
| `@0x446330` | trigger-condition name(main, sub) | `??? UNKNOWN Trigger Condition (%d)` |
| `@0x445ee0` | action sub-type name(actiontype, sub) | `??? UNKNOWN SubAction (%d)` |

### 8.2 Enum-table vindication

- **Trigger main types 0–7** and **action types 0–49** match our tables exactly
  (28/29 unused; 24 CHANGE_STEAM = change Single-team).
- **Condition tokens**: GROUP/SINGLE share sub values, differ in display semantics —
  single sub 6 = LOST_X_HP (damage, not units), sub 9 = FULL_HP, sub 12 = HAS_X_HP
  (health); sub 11 = HOLDS_ITEM (param ITEM_GROUP). Single extras 42–45 carry the
  positive base tokens ("On Top of" / "Near" / "Has LOS to" / "Sees" SSN); our names
  encode the negated sense — the trigger's negation flag flips the display (format
  strings @0x5b5010–0x5b5130; printf arg order != display word order).
- **MissionVariable operators**: editor 1 EQUALS, 2 LESS_THAN, 3 GREATER_THAN,
  4 LESS_THAN_EQUAL, 5 GREATER_THAN_EQUAL — vindicates the §7.4 3↔4 un-swap.
- **AI sub-types** (shared by action types 3/12/13/21; canonical names after the
  reconcile — `Skill1/Skill2/SpeedKmh1/SpeedKmh2/TargetSsn1` were wrong, and
  5/6/22/32/33 were missing from our table):

| sub | token | params |
|---|---|---|
| 2 | GUARDER | bit toggle |
| 5 / 6 / 22 | RED_ALERT / GREEN_ALERT / YELLOW_ALERT | — |
| 8 | ACCURACY | 0–100 % |
| 15 / 16 / 17 / 21 | BLIND / BERSERK / CLIMBER / COWARD | bit toggles |
| 26 / 27 | DRIVESKILL / AIMSKILL | skill value |
| 28 | AISETSTATE | state value |
| 29 / 30 | COMBATSPEED / PATROLSPEED | km/h |
| 31 | FIND_AND_USE | target pick |
| 32 / 33 | AIUSEWPZ / AICLEARWPZ | — |
| 34 | PLAYPARTANIM | [ANIMNUM, ANIMPLAYTYPE, ANIMTIME] |
| 37 | HUDITEM | [HUDITEM, TICKS] |
| 39 | TMATESTATUS | bit toggle |
| 40 | AINODEPATH | bit toggle |
| 41 | ATTACKDISTANCE | value |
| 42 | ENGAGEDISTANCE | [MINVALUE, MAXVALUE] |
| 43 | INDESTRUCTABLE | bit toggle |
| 44 | TARGETSSN | single pick |
| 45 | STARTFIRING | bit toggle |
| 46 | FIRING_ANGLE | value |

- Param-slot layout providers: trigger-condition slots `Med_TriggerConditionParams
  @0x44a390`, AI-sub slots `Med_AiSubTypeParams @0x44a920`, action slots
  `Med_ActionParams @0x44ac30`. Notable slot facts: group/single sub 7 = [unit,
  WAYPOINT_LIST, WAYPOINT_NUMBER]; subs 15–17 take a second SINGLE; player 37
  (satchel) takes an AREA; waypoint-list sentinels **123..125** in
  REDIRECT_GROUP_TO/REDIRECT_SINGLE_TO retarget to a SINGLE unit instead of a
  waypoint.

### 8.3 Param scaling — RAW in editor and file

`Med_ParamIntGeneric @0x449580` builds 0..999 raw-int combos (Distance / TimeSec /
Speed); `Med_ParamIntSmall @0x4496a0` 0..100 (accuracy %, AI skill). No `<<16`
anywhere in the editor — the runtime shift (§7.2) is eval-time only. Our schema
storing raw ints is byte-exact correct.

### 8.4 Value domains

- **SUBGOAL = fixed 1..8** (`Med_ParamSubGoalWon @0x449710` / `…Lost @0x449830`);
  slot text comes from the companion mission config keys `STRWINCOND%.3d` /
  `STRLOSECOND%.3d` (not in the `.bms`). Actions 14/15/35/36 param1.
- **TEAM = {0 NEUTRALTEAM, 1 GOODTEAM, 2 EVILTEAM}** (`Med_ParamTeam @0x449a90`,
  null-terminated id/name table @`unk_5E63B0`). Actions 16/24 param2.
- **Teleport target**: dynamic picker over placed **type-6088** entities, ids 1..99
  (`Med_ParamTeleportTargetNum @0x449b00`). Left raw in our schema (round-trips
  exactly).

### 8.5 Event flags: exactly three author-facing bits

EVENTS dialog `Med_EventDialogProc @0x411e10`; populate `@0x411690` reads bits
0/1/2 into three checkboxes, commit `@0x4118d0` writes ONLY those bits, preserving
every other bit verbatim. So the flags dword carries exactly **0x01 RESET_AFTER,
0x02 PRE_MISSION, 0x04 POST_MISSION** as authorable; 0x08/0x10/0x20/≥0x40 have no
editor control. Our speculative `Unknown4=0x10`/`Unknown5=0x20` checkboxes were wrong
→ `bms.h EventFlags` trimmed to mask 0x07 and `NovaMissionData::set_event` preserves
`flags & ~0x07` (mirrors the original |=/&=~ commit). What bits ≥0x08 mean at eval
time is unwitnessed; treat as preserve-only.

### 8.6 AttribFlags game-mode map (single 32-bit field)

Host game-settings dialog proc `sub_402770` (proposed `Med_HostGameSettingsDialogProc`)
edits attrib_flags at settings struct **+0x1D4**. The adjacent +0x1D8 dword is a
SEPARATE 31-bit flag set, not attrib_flags. Game-mode bits live under mask
**0xFF830000**; encode clears with `AND 0x7CFFFF` then ORs exactly one bit
(@0x4031cd, write-back @0x403242); decode (@0x4050c7) priority-tests the same bits;
none set → SINGLE PLAYER. Read and write are symmetric → **our 11 AttribFlags mode
bits are complete and correct** (two-binary agreement with the JO-derived enum).

| bit | mode (combobox idx @0x404eff) |
|---|---|
| (none) | SINGLE PLAYER (0) |
| 0x1000000 | COOPERATIVE (1) |
| 0x2000000 | DEATHMATCH (2) |
| 0x20000000 | TEAM_DEATHMATCH (3) |
| 0x4000000 | KING_OF_THE_HILL (4) |
| 0x40000000 | TEAM_KING_OF_THE_HILL (5) |
| 0x10000000 | CAPTURE_THE_FLAG (6) |
| 0x800000 | ATTACK_AND_DEFEND (7) |
| 0x80000000 | SEARCH_AND_DESTROY (8) |
| 0x8000000 | FLAGBALL (9) |
| 0x10000 | ATTACK_AND_SECURE (10) — JO/classic label "Advance & Secure" |
| 0x20000 | CONQUER_AND_CONTROL (11) |

Option bits share the SAME dword: 0x1 water, 0x40 SP-respawn, 0x100000 NVG,
0x400000 StartNVG (the latter two written as sub-byte ORs at +0x1D6,
@0x403255/@0x403272).

## 9. Appendix: IDA write-backs (2026-06-10)

Status: **proposed, NOT applied** (propose-first rule on the shared IDB
`Jointops.exe.kong.i64`); renames dry-run validated 13/13. This is the formal record
of §5 with the orig→reimpl correspondence made explicit.

### 9.1 Renames

| addr | current | proposed | confidence |
|---|---|---|---|
| 0x454050 | sub_454050 | EventTrigger_EvaluateChain | anchored |
| 0x454d50 | Entity_SetStateWreckage | EventTrigger_UpdateQuarterRoundRobin | anchored (current name WRONG) |
| 0x452ce0 | EventTrigger_NotifyEntityDeath | Event_OnEventFired_MarkLinkedSpawnEntries | probable (arg is the EVENT entry, not an entity) |
| 0x4f81a0 | sub_4F81A0 | WacScript_TickEvery62 | anchored |
| 0xae0704 | trigger | g_Events | anchored |
| 0xae0700 | dword_AE0700 | g_EventCount | anchored |
| 0xae070c | dword_AE070C | g_EventTriggers | anchored |
| 0xae0708 | dword_AE0708 | g_EventTriggerCount | anchored |
| 0xae0714 | dword_AE0714 | g_EventActions | anchored |
| 0xae0710 | dword_AE0710 | g_EventActionCount | anchored |
| 0xae06fc | dword_AE06FC | g_EventQuarterCursor | anchored |
| 0x815174 | dword_815174 | g_EventSpecialToggle | anchored (cat-5 condition; XOR'd each LoadAllData) |
| 0xb3b738 | dword_B3B738 | g_InputActionBits | probable (written by Input_HandleActionBinding; cat-7 bit tests) |
| 0xae06f8 | dword_AE06F8 | g_EventInputBitsMirror | probable (transactional copy around event eval/dispatch) |

### 9.2 Comment clarifications

- `0x453620` header: cat 3 = **event-fired** (reads event +20 latch && +16 countdown
  == 0), NOT "dialog".
- `0x454c30` header: +12/+14 = repeat-cooldown live/reload, +16/+18 =
  activation-delay live/reload, +20 = active latch, +21 = trigger_count, +22 =
  action_count; flags bit1/bit2 select the pre/post-mission PASS (not gameplay
  semantics). Timer words load unsigned but the decremented value is tested as signed
  int16 — reloads ≥ 513<<6 wrap negative on the first decrement (§1.2).

### 9.3 Reverse links (orig → reimpl)

| orig | reimpl |
|---|---|
| `EventTrigger_UpdateEntry @0x454c30` | `libs/mission/src/event_runtime.cpp` |
| `@0x454050` (chain eval) | `libs/mission/src/event_runtime.cpp` |
| `EventTrigger_EvaluateCondition @0x453620` | `libs/mission/src/event_runtime.cpp` |
| `EventAction_Dispatch @0x4542e0` | `libs/mission/src/event_runtime.cpp` |
| `@0x454d50` (quarter pass) | `libs/mission/src/event_runtime.cpp` |
| `sub_4F81A0 @0x4f81a0` | `libs/wac/include/wac/wac_system.h` |
| `Mission_LoadBMSFile @0x40f4e0` | `libs/mission/src/promote.cpp` |
| `Entity_SpawnFromBMSRecord @0x40e9f0` | `libs/mission/src/promote.cpp` |
| `EntityPool_FindByNetId @0x4f0a20` | libs/world entity registry (`EntityRegistry::find_by_net_id`) |
