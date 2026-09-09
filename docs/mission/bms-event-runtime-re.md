# BMS event runtime — equivalence record (vs Jointops.exe)

Grill session 2026-06-10, IDB `Jointops.exe.kong.i64` (imagebase 0x400000).
Scope: the BMS event evaluator (`engine/runtime/mission/event_runtime.{h,cpp}`), mission→world
promotion (`engine/runtime/mission/promote.{h,cpp}`), and the system tick order/cadence
(`engine/runtime/mission/mission_kernel.cpp` (`finish_load`), `engine/runtime/wac/wac_system.h`, `engine/runtime/world/world.{h,cpp}`).

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

The structural cases 0, 4, 11, 16..18, and 23..26 are now live with their
pool/flag distinctions; §10 is the implementation and regression map.

On every dispatch the original also activates linked spawn points:
`EventTrigger_MarkLinkedSpawnPoints @0x452ce0` (renamed 2026-08-15, ex the kong
misnomer "EventTrigger_NotifyEntityDeath") computes the FIRED EVENT's
own index (`(entry - g_Events)/24`), scans the spawn-point table (`dword_B76570`,
count `dword_B76568`) for records whose word +528 references that event, sets their
pending byte +536 (backward-chaining via byte +535), then `SpawnPoint_SkipBlocked
@0x4de310`. The SP route form is implemented by WaypointTrack; the MP POI/spectate reuse remains D-EVT-1.

### 1.6 Cadence — the fixed-timestep outer loop and per-system dividers

The 62 Hz engine tick is dispatched by the master loop's **fixed-timestep accumulator**
(`Game_MainLoop @0x52b630`, witnessed 2026-06-22). Per outer iteration it banks real elapsed
time (`GetTickCount`, in 1/16 ms units, `+= 16 * elapsed_ms` @0x52b7b2) and drains it in 4 ms
quanta; it invokes the UPDATE callback `Game_ProcessMainFrame @0x5263f0` (one `current_tick++`)
once per **16 ms = 62.5 Hz** — the inner loop consumes 64 units (4 ms) per pass (@0x52ba21) and
fires UPDATE only on `(phase & 3) == 0` (@0x52ba47), i.e. every 4×4 ms. It then invokes the
RENDER callback once per outer iteration (`Render_ProcessMainSceneFrame @0x5ca0f0`, the scene
descriptor's +0x28 slot @0x52bac6) at the **variable render rate**. So the simulation is
**decoupled from rendering**: a long frame runs **multiple** sim ticks (catch-up), a short frame
runs **zero**; the accumulator is clamped at **500 ms / ~31 ticks** (`0x1F40` units @0x52b83e)
against the spiral of death (plus a 7/8 frame-time EMA @0x52b85b and an optional vsync `Sleep`
cap @0x52b8b2 that gate only render). There is **no inter-tick render interpolation** — the
render callback reads current entity state. (The integer `62` in `Game_ProcessMainFrame`'s
per-second counters is the engine's rounding of the 16 ms / 62.5 Hz quantum.)

Witnessed per-tick frame structure inside `Game_ProcessMainFrame @0x5263f0` (the UPDATE
callback; `current_tick @0x24c1968` increments once per call):

1. `Server_TickUpdate @0x51d7e0` (authority only):
   - `WacScript_AdvanceTick @0x51d8bf` — the **WAC executor**: 14-instruction wrapper that gates
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
| `event_runtime`: three passes — pre (flag&2, whole list), post (flag&4, whole list, runtime-set phase), normal (16-tick gate + quarter cursor) | @0x454dc0/@0x454e00/@0x454d50/@0x51d7e0 |
| `event_runtime`: cat-3 Event trigger reads the latch window (`active && delay elapsed`), exposed as `event_fired()`; Simulation `has_event_fired` rerouted | @0x453a75 |
| `event_runtime`: ResetEvent clears only the latch | @0x454974 |
| `wac_system`: the 62-tick divider moved INSIDE WacSystem (accum `dword_C6EAD4`, pause `dword_C6EB28`, run counter `dword_C6EAD8`); skips the pre-mission pass | @0x4f81a0..@0x4f81d3 |
| `wac/vm`: WAC time base = completed executions (`time_`, [orig: dword_C6EAD8]) for `past`/`ontick`/`elapse`/Ticks — decoupled from the engine tick | @0x4f81d3 |
| `world`: `TickService` REMOVED (its 62:1 reducer gated the whole world tick — wrong layer; the original divides per system). `World::logic_tick` = the 62 Hz engine tick (`current_tick @0x24c1968`) | @0x5263f0 |
| `promote`: SSN = authored record id verbatim (PromoteOptions.first_ssn removed); spawn order items→buildings→markers→organics; markers spawn into pool 3 | @0x40e9f0/@0x40f4e0/@0x4f0a20 |
| the system registration (`MissionKernel::finish_load`, formerly `mission_systems.h`): grill-gate comment replaced with the witnessed order | @0x5263f0 |
| engine: Simulation drops the TickService member; `step()` = ONE 62 Hz logic tick — a render frame runs 0..N of them (**accumulator resolved 2026-06-22, see §2a**; the tick-mode enum that once selected between two identical entry points is gone, see §2b) | — |

Tests pinning the above: `tests/mission/event_runtime_test.cpp` (13 tests: cadence,
delay, signed wrap, cooldown window, reset_after=0 refire, pre-pass exclusivity, cat-3
window, ResetEvent), `tests/wac/wac_behavior_test.cpp` (`test_execution_cadence`),
`tests/mission/promote_test.cpp` (authored SSNs, pool-3 markers, find_by_net_id),
GUT `simulation_test.gd` / `mission_presentation_test.gd`.

## 2a. Fixed-timestep accumulator landed (2026-06-22, nw-merge)

The slice-D seam is closed. The reimpl previously advanced **one logic tick per rendered
`_process` frame** (`Simulation.advance_frame()` — since renamed `step()`, §2b — once
per the then-live presentation driver's `tick()`),
discarding the frame `delta` — a divergence from `Game_MainLoop @0x52b630` (§1.6) that coupled
gameplay speed to the render frame rate (the shared per-tick infantry motor, `tick_infantry`,
integrates a fixed displacement per tick, so locomotion/animation ran fast at high FPS and slow
at low FPS).

`inmatch::Session::advance(FrameInput)` now owns the original's accumulator: it banks `delta`,
runs `floor(accum / (1/62.5))` single ticks (clamped to 31, the 500 ms cap), and
the Godot presentation owner presents **once** after the batch — sim at a
constant 62.5 Hz, render decoupled at the render frame rate, no inter-tick
interpolation (faithful to §1.6). `MissionRoot.tick()` survives as the
deterministic primitive for F3/MCP Step and focused fixtures, but delegates to
the same session state machine. `MainGame` → `GameWorld` (its frame-leg table) is
the sole live real-time route. ADR 0025 retired the old ONED embedded preview;
ONED's Run OpenNova loose action launches the standalone game against the
selected loose assets. The portable `engine/runtime/world` per-tick
motors are unchanged — they were already correct per tick; only the driving tick **cadence** was
wrong. Pinned by `mission_presentation_test.gd`
(`test_session_frame_*`, `test_distance_per_real_second_is_frame_rate_independent`).

## 2b. The tick-mode enum retired (2026-07-14)

Cleanup tail of §2a, no behavior change. `Simulation` carried a `TickMode` enum
(`TICK_DIVIDED` / `TICK_EVERY_PROCESS`) selecting between `step()` and `advance_frame()` —
but the two methods had **identical bodies** (same `loaded_` guard, same
`HostRole`/`JoinerRole`/`LocalRole` ticks), differing only in return type. The
enum therefore chose between two copies of one behavior, and its comment still deferred the
62 Hz accumulator to "a future" that had already shipped in the presentation driver
(§2a). Its stated reason for surviving — "API stability" — is the internal back-compat that
is not kept pre-1.0 (CLAUDE.md).

Retired: one `bool step()` (false only when no mission is loaded), no enum, no
`tick_mode` property. The one behavioral wrinkle removed with it: the `TICK_EVERY_PROCESS`
branch of the old presentation driver's `_advance_one_tick_no_present` hard-coded `did_tick = true`, so an
unloaded sim reported a tick it never ran; the merged path returns the honest `step()`
result. Cadence parity among session consumers is now structural (one
path) rather than asserted, so the standalone game and
direct test/tooling fixtures cannot select divergent step implementations.
Direct fixtures continue to pin the surviving `loco_scale` behavior.

Naming: `step()` survives over `advance_frame()` because a render frame runs 0..N ticks (§2a)
— "frame" in our vocabulary is the render frame, not the engine tick. The
`Game_ProcessMainFrame @0x5263f0` correspondence lives in the `[orig:]` comment at the port
site, where this repo keeps such citations.

## 3. Tracked deviations

Dispositions after the 2026-07-05 grill (§3a carries the witnesses):

- **D-EVT-1: multiplayer POI/spectate reuse and live marker ownership remain.**
  The SP route form is already ported: promotion copies type-6005 marker
  linked-event/chain fields into WaypointTrack; the authority BMS hook marks
  matching positive event indices, walks flagged predecessors backward and
  skips completed selections (0x452CE0/0x4DE310). The same retail globals
  0xB76570/0xB76568/0xB7656C also serve the multiplayer POI list assembled by
  Entity_BuildMapPoiLists at 0x42DE40. That mode-specific builder and spectate
  cycling remain separate work. The copied SP entries also do not follow
  live marker removal or mutation.
- **D-EVT-2 — quarter-pass piggyback: FIXED 2026-07-05.** The earlier
  "unrelated entity bookkeeping" dismissal was wrong: when the quarter cursor
  is 0 (once per 64 ticks) the pass calls `Entity_UpdatePlayerAwolCounter @0x439dc0` (renamed 2026-08-15, ex Entity_UpdateStuckCounter)
  — the player-AWOL counter `dword_A89160` (`++` while
  `Entity_IsLocalPlayerOutOfBounds @0x439d40`, else reset) whose ONLY consumer
  is trigger cat-7 sub 36 PlayerAwol (`getter @0x439de0 >= param1`, @0x453d40),
  so "seconds AWOL" is really 64-tick (~1.02 s) quanta. Ported: the piggyback
  in `BmsEventSystem::tick`, `EntityCommands::local_player_out_of_bounds`
  (≥1 ACTIVE-flag zone, player X/Y inside none — Z ignored), and the PlayerAwol
  evaluator.
- **D-EVT-3 — condition categories:** cat 5 and cat 6 **FIXED 2026-07-05**;
  the cat-1/2 **relation-matrix + group alert/count family PORTED the same day
  (slice B)**: `world::TriggerRelations` (the twelve matrices, the 48-byte
  group records, the visited matrices with the 32-list clear quirk), the full
  cat-1 evaluator map + the cat-2 matrix/visited mirror, the 62-tick live
  recount inside the logic tick + the initial recount after the pre pass, the
  ChangeGroupAI alert stamps, load-time zeroing, and the damage-site SHOT
  writes in round_sim. **Waypoint arrivals FIXED 2026-07-22:** both the infantry
  channel mover and the shared SM/vehicle route mover now apply the witnessed
  SetBitB(group) then SetBitA(SSN) pair to those live visited matrices, so
  GroupAtWaypoint/SingleAtWaypoint can advance missions from actual NPC motion.
  **Cat-2 close-out grilled 2026-08-13 (§3b carries the witnesses) and PORTED:**
  the alert pair (subs 3/14, the per-entity `aiRuntime+0x88` byte), the health
  family (6/9/12 — health thresholds, not unit counts), the holding pair
  (cat-1/cat-2 sub 11 over the carried-object link `mountedChild` entity
  +0x268), and the 42-45 chain/distance/LOS family (all raw-positive senses;
  the authored negation bit does the flipping). The flag-family
  `mounted_child` producers are now live through `world::Match`, including
  pickup, death/drop, save, capture/reset/removal, and the S2C 0x2F/0x12
  replica folds; ordinary non-flag carryables and savegame restore remain with
  their owning systems. **Residual:** the sub-45 facing cone reads entity yaw, exact only for
  entities whose yaw our sim advances. (The
  acquisition/fire-time SEES+TARGETED quads LANDED 2026-07-16 with D-AI-3 —
  `AiSystem::apply_engage_relations`, live at the state-16/17 engage and
  infantry perception sites — the older "ride the combat pass" clause here
  was stale; corrected 2026-08-12.) Cat 7's input/view family
  rides its owning subsystems. Cat 5 "SecondTimeThrough" = the raw session load-parity word
  (`dword_815174`: static image value 1, XOR'd once per BMS load at the end of
  `EventTrigger_LoadAllData @0x454029`, read raw @0x453b24 — first session
  load reads 0, restart 1; save-persisted @0x4acee1/@0x4ad1ab, unported: no
  save system). Cat 6 "net" is really the Teammate category: sub 1
  TeammatesEnabled = in-session → false (@0x453b53) else `!(dword_24D1E34 &
  0x20)` (@0x453b67, the same option bit that suppresses type-5305 teammate
  spawns @0x40ea5a); subs 2/3 (MedicAssisting/Evacuating) are
  indistinguishable in retail — both read `dword_AC4F40 != 0` (the heli-lift
  active count, @0x453b42 → getter @0x451720, a FLIRT false-positive named
  `__uncaught_exception`).
- **D-EVT-4 — pre/post pass cadence: FIXED 2026-07-05 (witness settled).**
  One-shot per transition, never periodic: pre has exactly one call site
  (`Game_StartMission @0x525b86`, authority-gated, before `current_tick = 0`
  @0x525b9f); post is called once from `Game_TeardownMission @0x52266c` and
  the SP round-restart routine @0x5263a0 (kong-misnamed "Game_PlayVideoFile";
  a third xref @0x51ea89 sits in an unreachable dead blob). Our earlier
  per-phase-tick evaluation was itself a divergence; the port now exposes
  `run_post_mission_pass()` as the binding's one-shot and documents the
  one-pre-call contract (Simulation delivers exactly one).
- **D-EVT-5 — the BMS second chunk (header +0x246) is runtime-opaque.**
  Witnessed: `Mission_LoadBMSFile` fseeks past it on BOTH paths (in-session
  @0x40f6da–0x40f6ef, SP @0x40f756–0x40f76b); its only two data xrefs are
  those seeks — never read, validated, or written by the runtime. Our reader
  models it as the item-availability list and round-trips it; that grammar is
  editor-side (dfx2med) surface, gated on the D-MIS-3 grill. Contrast the
  +0x242 loadout chunk, which SP does consume (fread →
  `AIProfile_SanitizeConfigData @0x40f739` → weapon-restriction filter,
  fallback `WPN_KNIFE/-1/-1/-1`).

## 3a. 2026-07-05 grill — the cat-1/2 relation-matrix family (port spec)

Dispatch is a flat sub-type switch (`EventTrigger_EvaluateCondition @0x453620`,
cat-1 sub-switch @0x45364a). Two data stores back it, both zeroed per mission
load by `EventSystem_FreeAll @0x453210` and save-persisted
(`SaveFile_WriteTeamRelationBlocks @0x4aa320` / read @0x4a97e0):

**Per-group state — 48 B × 64 groups** (key = entity commandGroup +0x11C;
group 0 forced to count 0):

| field | base addr | maintained by |
|---|---|---|
| alert dword (0=green 1=yellow 2=red) | `0xA33FA4` | setters @0x40d5f0/=0, @0x40d610/=1, @0x40d630/=2 (all three kong names are misnomers); ChangeGroupAI subs 5→red 6→green 22→yellow (`Entity_HandleAlertCommand @0x43cff7`); AI death/damage → red (@0x465f9e, @0x4073ea, @0x465984) |
| initial count | `0xA33FA8` | `EntityPool_RecountByType @0x40e7e0` — pools 2,0,1 by entity+284; called ONCE from `Game_StartMission @0x525b8b` right after the pre pass |
| live count | `0xA33FAC` | `EntityPool_RecountLiveByGroup` full rescan (`!(flags&2) && health>0`), once per **62 ticks** in Server_TickUpdate (timer @0x51db6d, reload 0x3E @0x51db93, call @0x51dc02) and after the two group-reassign actions (@0x43c671, @0x43d75f) |

**Twelve sticky relation bitmatrices** (single key = DcbId +0x7C, the authored
SSN; group key = commandGroup):

| relation | G→G 64×64 `[2a+(b>>5)]` | S→G 128×64 | G→S transposed `[2b+(a>>5)]` | S→S 128×128 `[4a+(b>>5)]` |
|---|---|---|---|---|
| sees (sub 1/16) | `0xAC84E8` | `0xAC7CE8` | `0xAC70E8` | `0xAC60E8` |
| targeted (sub 2/15) | `0xAC82E8` | `0xAC78E8` | `0xAC6CE8` | `0xAC58E8` |
| shot (sub 13/17) | `0xAC80E8` | `0xAC74E8` | `0xAC68E8` | `0xAC50E8` |

Write events (authority-gated): **sees** at AI target acquisition
(`Entity_UpdateInfantryAI @0x4be41a..0x4be45b`; vehicle @0x466460, helicopter
@0x467730); **sees + targeted** at weapon fire (`Entity_SpawnProjectile
@0x4b0a6f..0x4b0ae2`; @0x471710, @0x472e00); **shot** when damage is actually
processed (`Projectile_ProcessDamageOnTarget @0x4e80ae..0x4e80ef` — skipped
when the friendly-fire gate @0x4e74f0 discards the damage). Setters
bound-check rows (<0x80; e.g. @0x452b60, @0x452bf0); the trigger-side tests
are unguarded reads (@0x452e50) — the port sanitizes reads to false out of
range (reproducing an OOB read would be manufacturing garbage, ADR 0003
class). Save quirk: the S→S matrices are 0x800 B but only 0x400 is persisted.

**Waypoint has-visited matrices** (sub 7): A `0xAC86F8` (row = single DcbId,
<128) and B `0xAD86F8` (row = group); cell `dword[row*128 + waypointList]`,
bit = waypointNumber (<32). Set when the AI advances past a waypoint
(@0x457c77/88, @0x460ec8/d9, @0x4bacfd/@0x4bad14, zone path @0x407c13..49).
Actions 32/33 clear via @0x4535e0/@0x453600 = `memset(row, 0, 0x80)` — only
lists 0..31 of the row (quirk, replicate). Vindicates dfx2med's
`[unit, WAYPOINT_LIST, WAYPOINT_NUMBER]` slot layout.

**Cat-1 evaluator map** (p1..p3 = params; cat 2 mirrors with the S-family
matrices keyed by raw authored SSN, no FindByNetId resolution):
sub 1/2/13 = `sees/targeted/shot` G→G bit [p1][p2]; sub 15/16/17 = the
transposed G→S rows; sub 3/14 = alert == red/yellow; sub 4 `live <= 0`;
sub 5 `live > 0`; sub 6 `init - live >= p2`; sub 9 `init == live`;
sub 12 `live >= p2`; sub 7 = visited-B bit [p1][p2] bit p3; sub 10 = the live
zone AABB test (@0x453763, already ported); sub 11 = exists pool-0 entity of
group p1 holding an object of group p2 (`TriggerGroup_AnyMemberHoldingItemGroup
@ 0x43c870` — the compared word on the HELD object is its commandGroup +0x11C,
not a "type word"; corrected by the 2026-08-13 grill, §3b).

## 3b. 2026-08-13 grill — the cat-2 single-state family + the held-object link

Closes every unwitnessed cat-2 sub. The full cat-2 switch (@0x4537f8, the
Single mirror of @0x45364a) was decompiled end to end: subs 1/2/13/15/16/17 =
the S-family matrix tests (§3a, keyed by raw SSN), 4/5 = `Entity_IsAliveByBmsRef
@ 0x43e640` (already ported), 7 = visited-A, 10 = the zone test, and the rest
below. Sub 8 is ABSENT from the jump table (default → false). **No case
negates its helper — each raw helper result IS the trigger truth**; the
negated flavor of our enum names (`SingleFartherThan`, `SingleHasNoLOS`,
`SingleDoesNotSeeOrFarther`) is authoring-display convention only (§8.2), and
the port must store the RAW positive sense and let the authored chain-negation
bit flip it.

1. **Alert pair — subs 3/14** = `Entity_IsSsnAtAlertLevel @ 0x43e780` (renamed
   this session from sub_43E780) with level 2/1: scan pools 0-1 for entity
   `+0x7C == ssn`; `aiRuntime` (entity +0x68) null → false; else the
   per-entity alert byte `aiRuntime+0x88 == level` (2 red / 1 yellow /
   0 green). This is a THIRD alert store beside the group record (§3a) and the
   brain alert (world record §17): a full writer sweep of `+0x88` byte stores
   found the producers — BMS spawn seed (`Entity_SpawnFromBMSRecord
   @ 0x40eea4`, the authored alert), the ChangeAI command arms
   (`Entity_ApplyCommand @ 0x43ab60`: case 5 → 2 @0x43ac2d, case 22 → 1
   @0x43ac8d, case 6 → 0 @0x43acfd; reached per-SSN from ChangeSingleAI and
   fanned per-member over pools 2/0/1 by commandGroup from ChangeGroupAI
   `Entity_HandleAlertCommand @ 0x43cff7`, which then also stamps the group
   record). The ChangeAI arms carry a SECOND half beyond the controller byte:
   each ALSO queues the brain `AIEvent {6, level}` → `AI_HandleCommand
   @ 0x465770` case 6, whose dispatch clamps the level 0..2, FORCES the
   stored level to 2 on any change (@0x4657cd), pushes pend state 10
   (ai-def type 1, current state not in {14, 6}) / pend state 18 (type 2,
   current != 22) — both behind `!(profile+96 & 2)` — then stores
   kPrevAlert+kAlert (@0x465803/@0x465809). PORTED 2026-08-15, both halves;
   commands reach brains through the state rows that route events (16/17/18)
   — the other rows' event handlers remain unported stand-ins. Other +0x88
   writers: the alert-state event handler (`Entity_HandleAlertStateEvent
   @ 0x43dee0`: 2/0/1 @0x43df8e/0x43dfa1/0x43dfb2), damage triggers → 2
   (@0x4073db, @0x40775e), `Entity_AlertNearbyAllies @ 0x46558a` → 2, the AI
   death/destruction transitions → 2 (@0x465f7e, @0x466b4c and the
   AI_TransitionToDeath family), the ground combat/evade state enters
   (@0x467665, @0x46741f), the idle/patrol resets (@0x4578f3, @0x457a83,
   @0x457f30, @0x4580a0), and HeliLift pickup → 2 (@0x45269e, @0x4526e4).
2. **Health family — subs 6/9/12** (health, not unit counts; signed i16
   `health` at entity +0x11E, `healthMax` at itemDef +0x17C, def ptr at entity
   +0x20; scans pools 0-1 by +0x7C; SSN 0 or not found → false):
   sub 6 `Entity_HasDamageCapacity @ 0x43e3d0` = `health <= healthMax - p2`
   ("has lost ≥ p2 HP"; NO null-def guard — reads def+0x17C unconditionally;
   no alive gate, so a corpse keeps satisfying it); sub 9 `Entity_HasFullHealth
   @ 0x43e470` = `health >= healthMax` (null def → false); sub 12
   `Entity_HasHealthAboveThreshold @ 0x43e350` = `health >= p2` (def not
   involved).
3. **Holding pair — sub 11 both cats** over the carried-object link
   `mountedChild` (entity +0x268) and its +0x11C commandGroup compare:
   cat-2 `Entity_IsSsnHoldingItemGroup @ 0x43e2f0` (renamed from sub_43E2F0) =
   pool-0 scan by +0x7C, `mountedChild` null → false, else
   `mountedChild->commandGroup == p2`; cat-1
   `TriggerGroup_AnyMemberHoldingItemGroup @ 0x43c870` (renamed from
   sub_43C870) = walk pool 0, gate `ItemTypeIndex` (+0x1C) nonzero, first
   entity with `commandGroup == p1 && mountedChild->commandGroup == p2` → true.
4. **The +0x268 writer/clear census** (re-verifying the ledger's "write-sites
   CLOSED"): set by `Entity_AttachToVehicle @ 0x43c130` (parent+0x268 = child,
   child Flags |= 1 @0x43c14a, child back-link +0x170 = parent; flag defs
   4091/4093/4095 additionally take the CTF return-timer/death-broadcast leg;
   other defs draw the carry progress bar) — callers: the authority pickup
   `Entity_ProcessWaypointInteraction @ 0x4ad820` (via `Entity_TryAttachToVehicle
   @ 0x43c1f0`; requires `Flags & 0x100`, no current child, and target def
   `attrib & 0x2000` carryable — also the flag-return and capture-zone logic
   for marker defs 0xFFB/0xFFD/0xFFF/0x1002/0x1004/0x1006/0x1007), and the
   joiner-side net appliers `NapiNPClientMsg_0x00A @ 0x42fec0` /
   `NapiNPClientMsg_0x02F @ 0x430e10`. Cleared by `Entity_DropCarriedObject
   @ 0x439df0` (renamed from the misnomer Entity_InitSpawnedChild — nulls the
   link and back-link, clears child Flags bit0/0x800000, terrain-snaps, tosses
   with the dropper's yaw, plays the def-name drop sound, authority broadcasts),
   the capture-zone clear @0x4ada07, and `Entity_Destroy @ 0x43ea03`. The
   savegame restore `SaveFile_ApplyEntityRecord @ 0x4abb00` (renamed from
   sub_4ABB00; caller `SaveFile_ReadOrWriteEntityRecord @ 0x4ac0c0`) resolves
   the serialized handle back into +0x268. Non-entity `+0x268` hits in the
   sweep (NapiNPProtocol_Create, CPreProcessor_*, the 0x4aab30 SetFilePointer
   orphan blob) are other structs.
5. **Chain/distance/LOS family — subs 42-45.** All four resolve BOTH params
   SSN → packed handle via `EntityPool_FindByNetId @ 0x4f0a20` (pools 0-3,
   low-16-bit compare of +0x7C, 0xFFFF when absent), then gate BOTH entities
   on `ItemTypeIndex` (+0x1C) nonzero; any failure → false. p3 is whole
   meters, shifted `<< 16` at the call sites (@0x45387f, @0x4538c9,
   @0x453913).
   - sub 42 `Entity_IsOnTopOfChain @ 0x4f19a0` (renamed from the misnomer
     Entity_IsInVehicleChain): follows A's groundEntity chain (+0x28 — the
     D-SND-11 `ground_target` link) up to 3 hops; true iff any hop == B.
   - sub 43 `Entity_CheckProximity @ 0x4f14c0`: float euclidean over the raw
     16.16 center deltas, clamped to ≤ 0x7FFF0000 (`flt_7C19E0` — the 16.16
     max, an ftol overflow guard, NOT a min-distance clamp), truncated;
     true iff `dist <= p3`.
   - sub 44 `Entity_CheckLineOfSightInRange @ 0x4f15e0` (renamed from the
     misnomer Entity_DrawConnectionLine): center distance > p3 → false (all
     failure paths `xor eax`); else a radius-0 ray between the two entities'
     `+0x1FC/+0x200/+0x204` offset points — pos + the host-stamped model
     collision-bbox CENTER, `Entity_InitFromModel` min+((max−min)>>1)
     `@ 0x40df1e..0x40df4a` (powerup zero rule: `attrib & 0x20` && type 6
     `@ 0x40df0a`; the effective def/entity scale uses the same signed Q16
     +0x8000 multiply as the matrix and bound), stamped and used by the port —
     `p3 <= 20 u` uses the
     entity-aware `Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130`,
     `> 20 u` the terrain/sector-only `Physics_RaycastTerrainAndSectors
     @ 0x539910` — returning the ray's clear flag.
   - sub 45 `Entity_CheckLineOfSight @ 0x4f17c0` ("sees") = sub 44 with the
     distance measured over the OFFSET points instead of the centers
     (`@ 0x4f18cd..0x4f18dd` — the range AND the bearing both ride the
     offset points), plus a facing gate after the ray: true iff
     `|int32(-yaw - int(atan2(dy, dx) * -(2^31/pi)))| <= 0x15555540` (±30.0°,
     int32 wrap = shortest arc; yaw = entity +0x10, the binary-angle heading;
     dy/dx = the offset-point deltas; scale `dbl_7C57B8 = -(2^31/pi)`).

## 4. Correspondence map

| ours | original |
|---|---|
| `BmsEventSystem::update_entry` | `EventTrigger_UpdateEntry @0x454c30` |
| `BmsEventSystem::evaluate_chain` | `@0x454050` |
| `BmsEventSystem::evaluate_trigger` | `EventTrigger_EvaluateCondition @0x453620` |
| `BmsEventSystem::dispatch_action` | `EventAction_Dispatch @0x4542e0` |
| `BmsEventSystem::tick` (pre/post/normal passes) | `@0x454dc0` / `@0x454e00` / `@0x454d50` + the 16-tick gate in `Server_TickUpdate @0x51d7e0` |
| `BmsEventSystem::load` | `EventTrigger_LoadAllData @0x453eb0` |
| `WacSystem::tick` (62-divider) | `WacScript_AdvanceTick @0x4f81a0` |
| `WacVm::time()` | `dword_C6EAD8` |
| `World::logic_tick` | `current_tick @0x24c1968` |
| `World::run_logic_tick` system order | `Game_ProcessMainFrame @0x5263f0` (Server_TickUpdate → Entity_UpdateAllEntities) |
| `promote_mission` | `Mission_LoadBMSFile @0x40f4e0` spawn loops |
| `Entity.net_id` | entity +124 ← record dword +8 (`@0x40e9f0`) |
| `EntityRegistry::find_by_net_id` | `EntityPool_FindByNetId @0x4f0a20` |

## 5. Proposed IDA write-backs (NOT applied — IDB writes were declined this session; apply after review)

Renames (dry-run validated 13/13):
- `sub_454050` → `EventTrigger_EvaluateChain` (anchored)
- `Entity_SetStateWreckage @0x454d50` → `EventTrigger_UpdateQuarterRoundRobin` (anchored) — **APPLIED 2026-06-25** (during the GamePlayerEntity grill: it was a kong-misnomer in the `Entity_*` namespace; verified callee `EventTrigger_UpdateEntry` + caller `Server_TickUpdate`)
- `EventTrigger_NotifyEntityDeath @0x452ce0` → **APPLIED 2026-08-15 as `EventTrigger_MarkLinkedSpawnPoints`**
- `WacScript_AdvanceTick` → `WacScript_TickEvery62` (anchored)
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
  `engine/runtime/mission/event_runtime.cpp`; on 0x4f81a0 → `engine/runtime/wac/wac_system.h`;
  on 0x40f4e0/0x40e9f0 → `engine/runtime/mission/promote.cpp`.

---

*Appendices consolidated 2026-06-10 from scratch notes: `S1_Mission_LoadBMSFile.md`,
`S2-S7_consolidated.md`, `param-semantics.md`, `event-grill-dfx2med.md`,
`game-mode-grill.md`, `mission/ida-writebacks-bms-event-runtime-2026-06-10.md`.
Addresses are Jointops.exe retail (imagebase 0x400000) except §8, which is dfx2med.exe.*

## 6. Appendix: BMS on-disk format evidence (S1–S7)

Format grill of `Mission_LoadBMSFile @0x40f4e0`. Primary fixture
`ash_i5b.reference.bms` (BMS v0x13; the reference tree's `fixtures/bms/` copy, read behind
`OPENNOVA_JO_ASSETS`); corpus = 115 shipped retail `.bms`
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
transform. Engine heading = **90 − yaw**, with pitch/roll stored direct. The
retail matrix builder applies those as
`Rz(90−yaw)·Ry(−pitch)·Rx(roll)`. Conjugating through `(x,z,−y)` and the
model-forward correction gives the resolved Godot basis
`RotY(90−yaw)·RotZ(+pitch)·RotX(roll)·RotY(90)`. The file stores the raw int16
degrees unchanged, so round-trip remains unaffected.

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

### 7.3 ZONE refs: ids in the FILE, indices at RUNTIME (resolved at mission start)

`bbox = &unk_A32D10 + 32 * param2` → at EVALUATION time param2 is the 0-based zone
**array index**; the zone record's id u32 @0 is never read by the bounds test
(`Entity_IsBmsRefInTriggerBounds @0x43e510` likewise). Zone-record layout confirmed:
x_min@4 x_max@8 y_min@12 y_max@16 z_min@20 z_max@24 flags@28 (bit 0x02 = constrain-Z;
else Z tested ±16384).

**Witnessed 2026-07-16 (session 6): the FILE carries zone IDS, remapped at load.**
Two mission-start resolvers walk every event's triggers/actions right after
`EventTrigger_LoadAllData`:

- `EventTrigger_ResolveZoneTriggerRefs @0x453000` (ex the IDB's
  `EventTrigger_ResolveWeaponActionRefs` misnomer): for triggers (main 1 Group /
  2 Single, sub 10 IsWithinArea → param2) and (main 7 Player, sub 37
  PlayerSatchel → param1), scans `unk_A32D10` for `record[0] == id` and rewrites
  the param to the array INDEX; a missing id or a degenerate box
  (x_min==x_max || y_min==y_max) NEUTERS the trigger (main_type=0 + sub_type=0,
  flags kept → the evaluator default, false — a negated dangling ref therefore
  reads TRUE, retail behavior).
- `EventTrigger_ResolveZoneActionRefs @0x453100` (ex `resolve_weapon_slot_triggers`):
  the same for actions 12/13 AreaAiRed/Blue (param1), additionally inlining the
  zone box into the action params (+12 x_min, +20 y_min, +24 x_max, +28 y_max);
  dangling refs zero the action_type.

Port: `BmsEventSystem::resolve_zone_refs` (one-shot per `load()`, run from
`on_load`; areas carry their authored id via `EntityRegistry::register_area`).
Our area-AI dispatch resolves boxes at dispatch time, so the index rewrite alone
preserves behavior (the inline copy is a noted non-port). Discovered via the
04TR.bms probe: its negated `SingleIsWithinArea(10000, zone 6)` out-of-bounds
watchdog fired RedWin at spawn while the refs were treated as indices. Editor
consequence stands: deleting a zone whose id others reference dangles them —
now with the witnessed neuter semantics rather than an OOB read.

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
| 11 | GroupHoldingGroup | `TriggerGroup_AnyMemberHoldingItemGroup(p1,p2) @ 0x43c870` — pool-0 member of group p1 with mountedChild->commandGroup == p2 (§3b) | GROUP | GROUP | — | high |
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
| 3 | SingleAtRedAlert | `Entity_IsSsnAtAlertLevel(p1,2) @ 0x43e780` — aiRuntime+0x88 == 2 (§3b) | ENTITY | — | — | high |
| 4 | SingleDestroyed | `!Entity_IsAliveByBmsRef(p1)` @0x453995 | ENTITY | — | — | high |
| 5 | SingleAlive | `Entity_IsAliveByBmsRef(p1)` @0x4539a1 | ENTITY | — | — | high |
| 6 | SingleHasLostMoreUnits | `Entity_HasDamageCapacity(p1,p2) @ 0x43e3d0` — `health <= max - p2` (§3b) | ENTITY | HP LOST | — | high |
| 7 | SingleAtWaypoint | `RelationMatrix_TestBitA(p1,p2,p3)` | ENTITY/team | ? | BIT | low |
| 9 | SingleIntact | `Entity_HasFullHealth(p1) @ 0x43e470` — `health >= max` (§3b) | ENTITY | — | — | high |
| 10 | SingleIsWithinArea | bounds, bmsRef=p1, zone=`[p2]` | ENTITY | **ZONE_REF (index)** | — | high |
| 11 | SingleHoldingGroup | `Entity_IsSsnHoldingItemGroup(p1,p2) @ 0x43e2f0` — mountedChild(+0x268)->commandGroup == p2 (§3b) | ENTITY | GROUP | — | high |
| 12 | SingleHasMoreUnits | `Entity_HasHealthAboveThreshold(p1,p2) @ 0x43e350` — `health >= p2` (§3b) | ENTITY | HP | — | high |
| 13 | SingleHasShotGroup | `TeamMatrix_TestAlive(p1,p2)` | ENTITY/team | GROUP | — | low |
| 14 | SingleAtYellowAlert | `Entity_IsSsnAtAlertLevel(p1,1) @ 0x43e780` — aiRuntime+0x88 == 1 (§3b) | ENTITY | — | — | high |
| 15 | SingleHasTargetedSingle | `TeamMatrix_TestSpottedBy(p1,p2)` | ENTITY | ENTITY | — | low |
| 16 | SingleSeesSingle | `TeamMatrix_TestEnemy(p1,p2)` | ENTITY | ENTITY | — | low |
| 17 | SingleHasShotSingle | `TeamMatrix_TestAttackedBy(p1,p2)` | ENTITY | ENTITY | — | low |
| 42 | SingleOnTopOf | `Entity_IsOnTopOfChain(hA,hB) @ 0x4f19a0` — groundEntity(+0x28) chain ≤3 hops (§3b) | ENTITY | ENTITY | — | high |
| 43 | SingleFartherThan | `Entity_CheckProximity(hA,hB,p3<<16) @ 0x4f14c0` — RAW true iff dist <= p3 (§3b) | ENTITY | ENTITY | **DISTANCE_M** | high |
| 44 | SingleHasNoLOS | `Entity_CheckLineOfSightInRange(hA,hB,p3<<16) @ 0x4f15e0` — RAW true iff in range AND ray clear (§3b) | ENTITY | ENTITY | DISTANCE_M | high |
| 45 | SingleDoesNotSeeOrFarther | `Entity_CheckLineOfSight(hA,hB,p3<<16) @ 0x4f17c0` — RAW true iff sub-44 AND ±30° facing cone (§3b) | ENTITY | ENTITY | DISTANCE_M | high |

Subs 42-45 resolve p1/p2 SSN → handle via `EntityPool_FindByNetId @ 0x4f0a20`
first; the raw senses are POSITIVE (the negated enum names are display
convention — §3b, §8.2).

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
| 36 | PlayerAwol | `Entity_GetPlayerAwolCounter() >= p1` | SECONDS outside mission area |
| 37 | PlayerSatchel | `EventTrigger_AnySatchelInArea(block)` | AREA (per dfx2med §8) |
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
| 8/9/10 | Blue/Red/GreenWin | `Server_ProcessRoundEnd(1/2/0)` — the shared round-end entry (the WAC win/lose handlers call the same; full decode + the SP end presentation in [world-wac-ai-re §20](../world/world-wac-ai-re.md)) | — | — | — | — |
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
| 23 | SingleVelocity | `sub_43DEA0(p1)` — witnessed retail no-op | ENTITY | SPEED_KPH | — | — |
| 24 | ChangeSteamAction | `Entity_FindByDCBAndSetFlag(p1)` | ENTITY | TEAM {0,1,2} | — | — |
| 25 | SingleChangeGroup | `Entity_SetNetIdByParentRef(p1,p2)` | ENTITY | GROUP | — | — |
| 26 | SingleTeleportAction | `EventAction_TeleportEntityToSpawn(p1)` | ENTITY | teleport-target | — | — |
| 27 | ParticleEffectAction | `EventAction_SpawnParticleEffect (ex sub_4540E0)(p1)` | target WP_NUMBER | — | — | All matching pool-3 ItemDef 6088 markers; entity+692/gen_string effect name, zero direction, store without releasing previous group. Shared typed particle consumer; entity+460 lifetime sharing remains D-PTL-26. |
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
| 40 | ShowWaypoints | `Game_SetShowWaypoints(p1)` | bool | — | — | — |
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
→ `bms.h EventFlags` trimmed to mask 0x07 and `MissionData::set_event` preserves
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

## 8b. IDA write-backs (2026-08-13 grill — APPLIED and saved in-session)

Renames landed in `Jointops.exe.kong.i64` (all vibe-confidence, behavior-derived):
`sub_43E780` → `Entity_IsSsnAtAlertLevel`; `sub_43C870` →
`TriggerGroup_AnyMemberHoldingItemGroup`; `sub_43E2F0` →
`Entity_IsSsnHoldingItemGroup`; `Entity_DrawConnectionLine` (misnomer — it
draws nothing) → `Entity_CheckLineOfSightInRange`; `Entity_IsInVehicleChain`
(misnomer — it walks the groundEntity chain) → `Entity_IsOnTopOfChain`;
`Entity_InitSpawnedChild` (misnomer — it DROPS the carried child) →
`Entity_DropCarriedObject`; `sub_4ABB00` → `SaveFile_ApplyEntityRecord`.
Witness comments appended at each function head plus the ApplyCommand case-22
line (@0x43ac8d) and the two PSShadow strcpy sites (the terrain record's
underwater grill, same session).

## 9. Appendix: IDA write-backs (2026-06-10)

Status: **applied 2026-07-16** (repo hygiene pass, maintainer's apply-the-held-IDB-updates
call) with three dispositions: `0x452ce0` and `0x4f81a0` had already been renamed by later
sessions to sharper names (`EventTrigger_MarkLinkedSpawnPoints`, `WacScript_AdvanceTick`) —
those two proposals below are superseded; `dword_815174` no longer exists as a standalone
symbol (re-adjudicate at the next event grill). Every other row landed in
`Jointops.exe.kong.i64`. This is the formal record of §5 with the orig→reimpl
correspondence made explicit.

### 9.1 Renames

| addr | current | proposed | confidence |
|---|---|---|---|
| 0x454050 | sub_454050 | EventTrigger_EvaluateChain | anchored |
| 0x454d50 | EventTrigger_UpdateQuarterRoundRobin | (applied) | **APPLIED 2026-06-25** (was kong-misnomer `Entity_SetStateWreckage`) |
| 0x452ce0 | EventTrigger_MarkLinkedSpawnPoints | (applied) | **APPLIED 2026-08-15** (ex kong misnomer `EventTrigger_NotifyEntityDeath`; arg is the EVENT entry, not an entity) |
| 0x4f81a0 | WacScript_AdvanceTick | WacScript_TickEvery62 | anchored |
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
| `EventTrigger_UpdateEntry @0x454c30` | `engine/runtime/mission/event_runtime.cpp` |
| `@0x454050` (chain eval) | `engine/runtime/mission/event_runtime.cpp` |
| `EventTrigger_EvaluateCondition @0x453620` | `engine/runtime/mission/event_runtime.cpp` |
| `EventAction_Dispatch @0x4542e0` | `engine/runtime/mission/event_runtime.cpp` |
| `@0x454d50` (quarter pass) | `engine/runtime/mission/event_runtime.cpp` |
| `WacScript_AdvanceTick @0x4f81a0` | `engine/runtime/wac/wac_system.h` |
| `Mission_LoadBMSFile @0x40f4e0` | `engine/runtime/mission/promote.cpp` |
| `Entity_SpawnFromBMSRecord @0x40e9f0` | `engine/runtime/mission/promote.cpp` |
| `EntityPool_FindByNetId @0x4f0a20` | engine/runtime/world entity registry (`EntityRegistry::find_by_net_id`) |

## 10. Structural action closure (grill-ida + port, 2026-08-30)

The action dispatcher previously emitted `unported_action` for several
world-structural cases whose retail callees and pool walks are now fully
witnessed. They now execute through `EntityCommands`, shared with the matching
WAC operations, and the default diagnostic is reserved for genuinely deferred
actions.

| action | Witnessed retail behavior | OpenNova landing |
|---|---|---|
| 0 `Null` | no-op switch arm [orig: EventAction_Dispatch @ 0x4542E0] | explicit no-op; no diagnostic |
| 4 `VaporizeGroup` | despite its curated callee name, removes matching nonempty rows while walking pools 2, 0, 1, 3 [orig: Entity_TeleportAllByNetId @ 0x43D5D0] | `EntityCommands::remove_group`; then live-count recount + collision refresh |
| 11 `GroupVelocity` | stores group speed at group-row +24 using the two truncating integer divisions `(256000*kph/60 << 8)/60` — 16.16 units per SECOND (kph × 1000/3600) [orig: Entity_SetMoveSpeedKPH @ 0x43A960] | `TriggerRelations::GroupState::move_speed_q16_per_sec` |
| 16 `ChangeGTeamAction` | scans pools 2, 0, 1 and rewrites team on every matching group row [orig: Entity_SetTeamByNetId @ 0x43C680] | `set_group_team`, including the live `AiEntity::team` mirror |
| 17 `ChangeGroupAction` | scans pools 2, 0, 1; pool 0 skips dead rows, pools 2/1 do not; rebuilds group counts [orig: Entity_UpdateNetIdReferences @ 0x43C5B0] | `change_group`, including the AI relation-matrix id mirror |
| 18 `GroupTeleportAction` | finds the first pool-3 type-6088 marker whose `WP_NUMBER` equals param2, then teleports group members in pools 0, 1, 2 [orig: Entity_TeleportTeamToSpawn @ 0x43D390] | `teleport_group_to_marker` |
| 23 `SingleVelocity` | walks its pool-1 lookup path and returns without a state write [orig: sub_43DEA0 @ 0x43DEA0] | explicit retail no-op; no diagnostic |
| 24 `ChangeSteamAction` | resolves the first SSN/DCB row in pools 0..2 and writes team [orig: Entity_FindByDCBAndSetFlag @ 0x43DB30] | `set_ssn_team` |
| 25 `SingleChangeGroup` | resolves the first SSN/DCB row in pools 0..2 and rewrites group [orig: Entity_SetNetIdByParentRef @ 0x43D6C0] | `set_ssn_group`, AI mirror, group recount |
| 26 `SingleTeleportAction` | marker lookup is pool 3 / type 6088 / `WP_NUMBER`; target lookup is the first SSN row in pools 0..2 [orig: EventAction_TeleportEntityToSpawn @ 0x43DFC0] | `teleport_ssn_to_marker` |

The two teleport forms deliberately differ:

- Group teleport copies marker position/yaw/pitch/roll. Pool 0 runs
  `Entity_ResetToSpawnState` but does not pre-clear Flags `0x20000`; pools 1/2
  clear it before rebuilding proximity [orig: Entity_TeleportTeamToSpawn @
  0x43D390].
- Single teleport clears Flags `0x20000` for every target pool. A pool-0 target
  also inherits marker Flags `0x20` when set and runs the reset path [orig:
  EventAction_TeleportEntityToSpawn @ 0x43DFC0].

Both ports copy the resolved pose into a resident `AiEntity`. This is the
split-pose translation of witnessed stores, not compensation: retail's single
teleport writes the ONE entity pose the motor reads plus `savedLivePose` and
the body pose per target [orig: EventAction_TeleportEntityToSpawn @ 0x43DFC0 —
savedLivePose @ 0x43E05F..0x43E071, bodyHeading/Pitch/Roll @ 0x43E07A..0x43E096];
our AI row carries the fixed-point mirror of exactly those fields
(`net_saved_live_pose` = `savedLivePose`). The group form writes only
position/yaw/pitch/roll (pool 0's body pose comes via the reset), so the group
arm's AI heading sync is slightly ahead of retail's chase-converging body
heading — a translation note, not a divergence row. Registry-shape changes
refresh collision/proximity once after the complete fan, not once per member.

The single-target SSN commands (24/25/26) resolve by walking pools 0, 1, 2 in
order — retail's own sequential DcbId scans, first match wins, `dcb_id != 0`
gated, with NO item gate on the team/group writers (the teleport walk keeps
its item gate @ 0x43E036) [orig: Entity_FindByDCBAndSetFlag @ 0x43DB30 —
team byte entity+354 @ 0x43DB63; Entity_SetNetIdByParentRef @ 0x43D6C0 —
commandGroup word entity+284 @ 0x43D6F4]. The 2026-09-01 re-grill replaced the
earlier resolve-then-reject shape, which could miss when a pool-3 row shadowed
the SSN.

`event_runtime_bms` pins pool coverage, the pool-0 dead-row exception, both
teleport flag rules, marker selection, AI mirrors, group speed conversion, the
Null/SingleVelocity no-op arms, and absence of `unported_action` for this set.

### 10.1 Teammate and remaining explicit action boundaries

Action 39 now dispatches subtypes 1/2 through the eight-slot teammate
operation owner, fully identified dynamic helper definitions and the shared
native motor/animation paths. Other subtypes are witnessed no-ops. Snapshot
and retry restore the owner; removal shares native relationship, AI,
collision and effect-bank cleanup. The exact state sequence, JO's missing
medic assets, the no-pilot flyover outcome and the guarded original null
helicopter fault are recorded in
[world-wac-ai-re section 33.32](../world/world-wac-ai-re.md#3332-teammate-operations-dynamic-helpers-and-removal-2026-09-09).
Group door actions 30/31 reach DoorSystem, collision and presentation
(section 33.14).

**D-EVT-6 closed 2026-09-09.** The remaining explicit action boundary of the
2026-09-01 residual is owned: 30/31 `[orig: EventAction_Dispatch @0x4542E0;
WacCmd_DoorOpen @0x4F70A0]`, 39 `[orig: HeliLift_SpawnPickup @0x4525E0;
HeliLift_SpawnFlyover @0x452730; HeliLift_UpdateSlotState @0x451730]`, 42..49
`[orig: Entity_SetActionByBmsRef @0x43DAC0; Entity_SetAlertByBmsRef @0x43DA50;
Entity_SetWaypointByBmsRef @0x43D9E0; sub_43D970 @0x43D970;
Entity_SetWeaponTypeByNetId @0x43D8F0; Entity_SetActionByNetId @0x43D870;
Entity_SetAlertByNetId @0x43D7F0; Entity_SetTargetByNetId @0x43D770]` (the
curated names are misleading: they write the +332/+334/+336/+338 target-policy
words, world-wac-ai-re section 33.2), and 41 `ExecuteWac` has no retail case
arm. The default arm of the dispatcher is diagnostic-only for unauthored action
types.

The 2026-09-08 pass implements actions 42..49 through the four target-policy
words and their infantry/weapon consumers. Action 41 `ExecuteWac` has no
retail switch arm and is now an explicit no-op. Writer addresses, actual
semantics, consumer evidence and regression coverage are recorded in
[world-wac-ai-re.md section 33](../world/world-wac-ai-re.md#33-script-timing-and-target-policy-2026-09-08).

**D-EVT-1 scope correction (2026-09-08):** WaypointTrack already owns the SP
route form of list 0xB76570, including positive linked-event matching,
backward completion chains and the blocked-selection walk. Its existing
waypoint_track test covers those operations, and BmsEventSystem invokes it
after both immediate and delayed actions on the authority. The remaining
variant is Entity_BuildMapPoiLists (0x42DE40), plus live marker ownership;
type-6088 structural teleport/effect markers are a different list.

The queued ChangeAI half of the same parity round is recorded in
[`world-wac-ai-re.md` §32](../world/world-wac-ai-re.md).
