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
