# World / WAC / AI runtime — RE record and equivalence verdicts

Binary: `Jointops.exe` (retail JO:CA, Steam), IDB `Jointops.exe.kong.i64`, imagebase 0x400000.
Sessions: 2026-06-07 (WAC ISA + AI P1/P2, prior), 2026-06-08 (foundation), **2026-06-10 (entity-motor architecture grill — this record)**.
Scope: `libs/world` (entity registry, var store, AI), `libs/wac` (VM), the mission-runtime bridge, and the
locomotion/motor layer. Companion docs: `docs/mission/bms-event-runtime-re.md`,
[ADR 0007](../adr/0007-skeletal-runtime-and-entity-visual.md) (skeletal),
[`docs/runtime-architecture.md`](../runtime-architecture.md) (main loop).

## 1. The entity-motor architecture (discovered 2026-06-10)

The original drives entities through **two class-keyed tables**, both scanned by 8-byte class name
(the items.def class tag):

### 1.1 Event-callback table — `g_EntityClassEventCallbackTable @ 0x813000`
Records `{char name[8]; void *fn[4]}` × `g_EntityClassEventCallbackCount @ 0x8133D8` (41).
Consumed by `Entity_LookupRenderCallbacks @ 0x407dc0` (kong misnomer; it resolves *event* callbacks)
from `EntityDef_InitAllCallbacks @ 0x4a5a70`, storing fn1..fn4 into the item def at dword indices
[78]/[82]/[83]/[89]. fn1 = the **event callback** `(entity, eventType)` — for organics it is
`Entity_HandleDamageTrigger @ 0x407310` (types 1=death, 4=reset); for AI-vehicle classes it is the
**brain state machine itself**: `CHel → 0x4581b0` directly, `cbot/cpln/ctrn → thunks 0x462120/30/40`.
The old "driver @0x462120" lead was one of these thunks — a red herring.

### 1.2 Physics/motor table — `g_EntityClassPhysicsTable @ 0x82abc8`
Records `{char name[8]; void (*fn)(Entity*)}`, the **per-frame motor** per class:

| class | motor | notes |
|---|---|---|
| `org0` | nullsub | static organics |
| **`org1`** | **`Entity_UpdateInfantryAI @ 0x4b9910`** (25 KB) | **AI soldiers — THE infantry ground mover** |
| `org2` | `0x4b40e0` | player-body variant |
| `CHel`, `cpln` | `Entity_UpdateAircraftPhysics @ 0x490310` (renamed this session) | air motor: collective f[137], banking, separation |
| `cveh/ctank/cbike/cbot/catv/ctrn` | `Entity_DispatchPhysics_* @ 0x48ef90..0x48f060` | each tests `def+0x8DC`, routes into the shared `Entity_UpdateVehiclePhysics` family (slope cos², gravity −324/tick, surface-normal steering, pool collision) |
| projectiles/effects | shell/missile/flare physics | out of scope |

### 1.3 Consequence for OpenNova (the reframing verdict)
Our `libs/world` AI port (24-row SM @ 0x815238, states 16–18, `AI_BeginUpdate @ 0x457b40`,
`AI_UpdateWaypointMovement @ 0x457bd0`, targeting/combat P1/P2) is a **byte-exact port of the
AI-vehicle decision layer** (cveh/cbot/ctrn). Those verdicts stand. The divergence: OpenNova
currently routes **BMS organics** through that vehicle layer; the original routes them through
`Entity_UpdateInfantryAI @ 0x4b9910`, where **locomotion is animation-driven root motion**.
User decision 2026-06-10: port the infantry motor to full fidelity (§3) before extracting PR-B1.

Also confirmed: `AI_DispatchStateMachineByProfileClass @ 0x4680a0` (defined this session) is
**unreferenced dead code** (no callers, no data refs, no rel32 sites). `g_AIMoveStepFnTable @
0x8153b8` = `{id, fn}` pairs: ids 0–4 step movers (id4 = `AI_BeginUpdate` = the alive step;
ids 0–3 = directional death movers incl. `AI_ProcessMovementStep @ 0x466db0` = death-fall:
controller(brain[2])+16 phase += brain[7]/tick, thresholds 372/744, workZ = ground+0x50000,
180° flip when grounded, pitch rate brain[45]<<14), ids 0x10000–0x10005 target-calc fns.
`brain[2]` = per-entity 32-byte controller; controller[0] points at the current pair.

## 2. Correspondence map (libs/world ↔ Jointops.exe)

| reimpl (libs/world) | original | addr | evidence | verdict |
|---|---|---|---|---|
| `AiSystem::begin_update` | `AI_BeginUpdate` | 0x457b40 | byte-walk this session (budget 0x1F0, pend=8/brain[6], +brain[7]) | **matching** (as vehicle-layer) |
| `AiSystem::update_waypoint_movement` | `AI_UpdateWaypointMovement` | 0x457bd0 | prior grill + re-read | **matching** (vehicle mover) |
| `ai_waypoint_update_target` | `AIWaypoint_UpdateTarget` | 0x457380 | prior grill; also called by aircraft motor @0x490310 | **matching** |
| `process_infantry_state_machine` | SM dispatcher (vehicle classes) | 0x4581b0 | event-callback fn1 rows | **matching**; name is now known to be historical — it is the AI-VEHICLE SM |
| `h_ground_followwp_tick` etc. | state 16/17/18 rows | 0x815338+ | prior grill (raw bytes) | **matching** |
| P2 combat/targeting | `AI_FindBestTargetB` etc. | 0x466f60+ | prior adversarial grill | **matching** (recorded deviations stand) |
| `AiSystem::apply_locomotion` | — (model) | — | vehicle-layer kinematic model only (organics no longer pass through it); HELO/vehicle physics remain visible `not_yet_ported` stubs | tracked model (vehicle slice) |
| organics → `tick_infantry` routing | `g_EntityClassPhysicsTable` row "org1" | 0x82abc8 → 0x4b9910 | promote marks `inf.active`; `AiSystem::tick` branches before the SM | **matching** |
| `AiSystem::tick_infantry` (+think/select/slide) | `Entity_UpdateInfantryAI` | 0x4b9910 | structural translation, per-mechanic dump cites in libs/world/src/infantry.cpp; constants byte-pinned (turn clamp 69273360, gravity 416/−32768, slide 2048 @ threshold 0x22222200, gates 30°/45°, jog windows 139264/270336/73728) | **matching** w/ D-INF-1..10 (enumerated below) |
| `kInfantryAnimNames/Flags` | `off_8135F0` / `g_animStateFlagsTable` | 0x8135F0/0x8139E8 | all 200 entries index-verified vs IDB | **matching** |
| promote `init_infantry` + marker fill | `Entity_SpawnFromBMSRecord` | 0x40e9f0 | slot map (speeds %, accuracy, engagement, timers ×62, alert, route) + marker radius/facing/movetimer | **matching** |
| `InfantryRootMotion` (engine host) | `AnimMap_UpdateEntity` out-transform | 0x40b5f0 (+0x40b230, 0x40b140) | scales pinned by disasm + real-clip grill (tests/anim/root_motion_test.cpp: I_walkf 1.82 u/s, E_RUNF 5.28 u/s) | **matching** (playhead dt = open item 16) |
| `AiSystem::apply_ground_clamp` | per-motor ground sampling | 0x457230 + motors | 5-tap port matches; the infantry motor resamples on the faithful every-8 cadence (cache `inf.ground_cache`); the vehicle path still clamps per tick | matching-core (infantry aligned; vehicle cadence with its slice) |
| WAC VM (`libs/wac`) | `Script_Compile`/`WacScript_ExecuteBytecode` | 0x4f31f0/0x4f58b0 | oracle-extracted ISA + corpus | **matching** (165-cmd table, 0x7A7A7A7A) |

## 3. Infantry motor spec — `Entity_UpdateInfantryAI @ 0x4b9910` (port blueprint)

Everything below was decompiled and read this session (pseudocode dumps:
`notes/world/infantry_ai_0x4b9910.c`, `notes/world/infantry_update_0x490310.c` — scratch, untracked).

### 3.1 Cadences (all gates on `current_tick + 36*net_id` stagger)
- Motor + anim + integration: **every tick**.
- Ground resample: every **8** ticks (`entity+676` cache; subtract `brain[11]` stand offset first).
- Slope slide/lean: every **8** ticks. Gravity/ground resolve: every **2** ticks.
- AI think (nav/commands): every **16** ticks, authority only.
- Perception scan: every **32** ticks (phase `(tick>>5)&3`); health regen + drowning: every 64.

### 3.2 Navigation think (every 16 ticks)
`AiSlot = entity+104` (172 B, our struct): `slot[37] @+148` = **waypoint channel id**, with
**123–127 reserved as commands** → usable mission path ids are 1..122 (format truth!):
126 = hold/guard the group (10 u radius), 127 = follow local player (radius `max(slot[16], 4u)`),
123/124/125 = **Goto-SSN-and-board** (runtime seat filters: 123 `sitex`/passenger-only,
124 rejects `ctrlx`, 125 any; MED labels are in §11). `slot[38] @+152` = node index for a real path
(1..122), or the **target SSN**
(= `wp_number`) for the 123–125 Goto-SSN commands. `slot[35]` = active flag,
`slot[36]` = located entity ptr (rescans pools 0/1/2 by id when stale).
- Target node = pool-3 marker entity via `entryIndex[34*ch + node]` (`Pool_GetEntryUnchecked(3,·)`);
  the nav tables are the same `Buffer/dword_A71DD4/entryIndex` aliases our port rebased.
- Target Z = marker.z + 0x4000; distance = 3D with vertical slack `max(0, |dz| − 0x10000)`.
- **Arrival radius = marker dword[0]** (per-node!). On arrival:
  `RelationMatrix_SetBitB(entity+284, ch, node)` + `SetBitA(entity+124, ch, node)` (matches our
  recorded relmat calls); **marker wait-time @ marker+328** → `entity[74] = (wt+8)>>4` think-tick
  cooldown + face **marker heading @ marker+16** (`entity[106] = heading`); `++node` wrap;
  loop bit `Buffer[34*ch] & 1` = ONE-SHOT → stop at `count−1`, cooldown 20 (matches our port).
- Output: `moveMode` (0 stop, 3 move, 4 waypoint-walk, 1/2/5/7/8/12 combat maneuvers) +
  `targetDist` + target pos.
- TODO (next session): map marker+328/+16 to BMS record fields via `Entity_SpawnFromBMSRecord @ 0x40e9f0`.

### 3.3 Heading pipeline (per tick)
- **Body** `entity[35]`: `+= clamp((target(entity[106]) − body + 2) >> 2, ±69273360)` (~5.8°/tick);
  render yaw `entity[4]` moves with it.
- Aiming (`byte entity+864`): render yaw chases aim heading `entity[187]` quarter-step,
  clamp [−31457280, +503316480]; pitch `entity[5]` chases `entity[180]` likewise.
  Non-aim: eighth-step, clamp ±14680064 (cap 234881024). Per-tick sanity clamp vs prev: ±0x40000000.
- Torso `entity[181]/[182]` chases head-look `entity[185]/[186]` quarter-step (1/16 when def+84&0x200),
  rate clamp ±83886080, twist limit ±0x20000000 (45°) from body. Head-look hysteresis: re-aim only when
  |Δ| > 59652320 (~5°) and (|Δ| > 357913920 (~30°) or the per-entity 64-tick window hits).
  (Torso/head feed bone overlays — visual; ADR 0007 already defers the overlay skeleton work.)

### 3.4 Animation state machine (the locomotion driver)
- State id `entity[175]` (prev `entity[178]`); **state→name table `off_8135F0`** (200 entries, names
  ARE the `.adm` keys: `anim_<name>`): 0 reset, 1–8 walk 8-dir, 9/10 run variants, 11–18 crouch-walk,
  19–26 prone-walk, 30/31 jump_start/loop, 32–35 climb, 36–40 swim, 43/44/49 idles, 45/48
  crouch/prone idles, 47 parachute, 65 reload, 67–75 emplaced, 76–110 sit_*, 111–114 burn,
  115–124 emotes, 125–135 scripted idles, 137–139 dragger/draggee, 140–144 guard, 145/146 wounded,
  147 stop, 148 jog, 149 run_forward, 151/152 post/pre_attack, 153 out_of_ground, 154 swim_attack,
  155–158 attack, 159–162 grenade throws, 163–166 cover, 167 run_attack, 168 run_away,
  **169–172 stance transitions** (run2crouch/runl2crouch/runr2crouch/run2prone),
  173 death_fire, 174 death_pungi, 175 death_drown, **176–199 death matrix** (grenade/bullet ×
  hip/torso/head/Rshoulder/Lshoulder × F/R/B/L) — picked by
  `Entity_ComputeAnimSlotIndex(entity, boneSection, relativeDirection, 4)` with
  `relativeDirection = (heading − atan2(attackerVel) BAM − 0x60000000) >> 30` (2-bit quadrant).
- **Per-state flag table `g_animStateFlagsTable`** (≥190 dwords; extracted, embed in port as generated table):
  observed semantics — bit0 = movement state (client re-derives from velocity; auto-rebase to idle 43),
  bit1 = low-to-ground → slope-slide/drift participates (prone 0x603, dragger), bit2 = scripted/locked,
  bit10 (0x400) = slow blend (15 ticks vs 10), 0x80 weapon-pose, 0x10 sit/scripted-idle, 0x20 emote;
  bits 3/6 (0x48 idles, 0x449 walks) pinned at port time from use sites (&8 @ line 3165 dump,
  &0x10 @ 3352, &2 @ 931).
- `AnimMap_UpdateDualChannels @ 0x40b8c0` (secondary then primary channel; entity+392/396 active
  handles, +696/700/708/712 channel state) → `AnimMap_UpdateEntity @ 0x40b5f0`:
  pending-state compare, **stance-transition interposition** (149/1/9/10→19 via 172("0xAC"); →11 via
  169; 2→12 via 171; 8→18 via 170), **variant ring** (`table[id] = entry->next @ +36` each play),
  `AnimChannel_InitFromParams(channel, clip@entry+32, blendDur 10|15, flag887, 4096)`,
  advance (blended) playback, then **root-motion keyframe eval** (PINNED 2026-06-10, disasm
  0x40b82f..0x40b8a3 + real-clip grill `tests/anim/root_motion_test.cpp`): the keyframe record IS
  the `.bad` "events" array — 24-byte `{vel[3], capsule_bottom, capsule_top, trigger}`, fence-post
  (frame_count+1 records), lerped pairwise (`AnimChannel_InterpolateKeyframe @0x40b230`; trigger
  unlerped from the lower keyframe; playhead = normalized t∈[0,1) advanced per update, wrap-on-loop
  `AnimChannel_AdvancePlayback @0x40b140`). Out-transform:
  `out[0] = vel[2]·32768` (flt_7C32B4) = **forward step/tick 16.16** (32768 = 65536/2 bakes the
  ~2-sim-ticks-per-30fps-frame ratio; I_walkf mean 0.061 → 1.8 u/s, E_RUNF 0.176 → 5.3 u/s),
  `out[1] = vel[0]·32768` (lateral), `out[2] = Δ(capsule_bottom·65536)` (flt_7C32BC) = vertical
  root delta (prev in `anim_slot[19]`, reset on climb 32–35 / death_grenade 176–179 pendings;
  first-update fallback `vel[1]·32768`), `out[3] = capsule_bottom·65536`,
  `out[4] = 0x2000 + capsule_top·65536` (flt_7C32B8 = −65536) — the per-frame **collision capsule**
  (crouch/prone shrink; resolver input, not motor input); **`dword_A2ED08` = trigger** (.bad
  per-frame events): bit0/bit1 = footstep L/R (surface-typed sounds via weapon-slot table
  17–23, `Terrain_GetSurfaceTypeAtPosition`, water/platform variants, on odd ticks), 0x20..0x400 =
  cloth/gear sounds (24–29), bit2 = attachment event.

### 3.5 Integration (per tick, the motor core)
1. Rotate anim root delta by heading: `sin/cos(entity[4]) · 2^22` (FPU, dbl_7C3608 = π/2^31,
   dbl_7C3600 = 4194304.0); `fwd' = fwd·cos − strafe·sin; strafe' = fwd·sin + strafe·cos` (>>22).
   State 31 (jump_loop) forces fwd = 1024.
2. **`pos.xy += rotated_delta + vel(entity[38..39])`; `pos.z += vertical_delta`.**
   Flag 0x8000 (drowning) zeroes vertical; 0x100000 (deep water) zeroes horizontal root motion.
3. Every 2 ticks: **gravity `vel_z(entity[40]) −= 416`** (terminal −32768; ladder/climb chases
   `entity[193]` target at 1/16-step, cap 0x4000); `pos.z += 2·vel_z`;
   `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0 (entity, root_drop, height)`:
   ≤0 ⇒ ground push-out (`pos.z -= ret`), vel_z = 0, water-exit sounds (15/16),
   **fall damage** when `vel_z ≤ −1057·dword_C6EAE4`: `health −= (excess)>>4`;
   >61440 ⇒ set swim (flag 0x2000, states 47/31 hmm 47=tread/31 per anim availability).
   Water-edge climb-out: probe ahead 81920·dir for pool-0 entity with z-overlap → state 32.
4. Every 8 ticks: **slope slide/lean** — 4 probes `sub_4142C0(entity, ±dir·22528>>22 …, 0x4000,
   0x20000)` ahead/behind (pitch slope ×2^14) and left/right at quarter offset (roll slope ×2^16),
   clamp ±656175520; if |slope| > 572662272: **slide** `vel ∓= dir·2^11>>22`; body-pitch
   `entity[36]` and roll `entity[6]` chase slopes at eighth-step (decay 1/16 when N/A);
   gate: `def+84 & 0x200 || stateflag & 2 || dead`.
5. Inter-entity separation + 8-direction avoidance raycasts + swim details + combat maneuver modes
   (1/2/5/7/8/12) — **RE'd to address level, detail pass pending** (dump lines ~1080–1290, 3000–4550).

### 3.6 Death/corpse/respawn (health ≤ 0 edge)
Drop/detach, corpse timer `entity[82] = def+2192` (−61 when burning), death anim via
`Entity_ComputeAnimSlotIndex`, drowning ⇒ state 175; corpse never despawns while the local player
can see it (`Physics_RaycastTerrainAndSectors` watch-check, retry 62); respawn restores
`entity[198..205]` snapshot; death-by-fall splash effect via def+1042.

### 3.7 Port architecture (decided)
- New infantry system in `libs/world` beside the vehicle SM; promote routes **organics → infantry**,
  vehicle classes stay on the SM (correct per §1).
- **Root-motion source injected** (mirrors the terrain `TerrainHeightField` injection):
  `libs/world` defines the per-state clip-velocity interface; the real impl evaluates `.bad`
  root tracks via `libs/anim` and lives with the binding (NovaSimulation) + an env-gated
  real-assets ctest; unit tests inject synthetic velocities. State→clip resolution uses the
  `off_8135F0` names through `.adm` (libs/anim) — the exact original data path.
- Tables (`off_8135F0` names, `g_animStateFlagsTable` flags) land as generated C++ tables (wac-style).

## 4. Open items (tracked, addressed)
1. ~~Anim-state **selection thresholds** per moveMode (dump lines 3000–3700)~~ CLOSED 2026-06-10:
   selection + commit rules ported (walk/run/jog/turn/wounded + lock/emote queueing); the
   147-availability rule is a post-commit force `147 → 43 idle` (dump 4084), **not** a gait
   fallback — port aligned.
2. ~~**Avoidance** internals~~ CLOSED 2026-06-10 (negative result): patrol walking has **no
   peer/obstacle steering** in `0x4b9910`. The probe fans found are (a) a peer-cohesion
   *facing* average for combat idles 163/44/126 only (same-team peers ≤ 3u + a 9-direction
   `sub_53B130` clearance fan biasing `entity[106]`; dump 3734–4082), and (b) vehicle-entry
   approach probes inside the command path. Entity separation = the resolver's push-out
   (item 5), not AI steering.
3. **Swim**: no swim locomotion in the unread regions beyond the known state overlays
   (36/37/154 selection + wash 27–29); swimming physics lives in the resolver (item 5,
   water flags at entity+36). The airborne overlay decoded: flags 0x2000/0x20 set + 0x40
   clear → force parachute 47, fallback jump_loop 31 (dump 3679–3692).
4. `sub_4142C0 @ 0x4142c0` (0x59 bytes — ground probe at offset; exact param semantics 0x4000/0x20000).
5. `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0` internals (0x11e3 — ground/water/platform
   resolver). Related bookkeeping decoded: platform-rider counter entity[92]/[93] (+4 to 240 on a
   same-team platform underfoot, −1 decay; dump 3711–3733).
6. ~~Marker wait/facing **BMS field mapping**~~ CLOSED (spawn map ported into promote; see §3.2).
7. Perception scan fn (called at dump line 2377, kong-misnamed `Entity_SpawnProjectile`) + LOS `sub_53B130`.
8. `dword_C6EAE4` (fall-damage gravity scale) value/source. ~~1024-entry sin/cos table
   extraction~~ CLOSED 2026-07-05: the table is 1281 entries built by
   `Math_BuildSinTable @ 0x613050` (accumulating step `dbl_7DF578`, scale `dbl_7C3600` =
   4194304.0, `_ftol2_sse` truncation); `off_849934` = `outMillis + 0x400` — cos is a
   +256-entry alias into the SAME table, not a second table (see D-INF-4).
9. Death move-step movers `0x461c30`/`0x461cb0` (ids 0/2) — define + decode.
10. Vehicle-SM per-tick invocation site (event-callback path is confirmed; the tick-mode caller for
    vehicles not yet pinned — likely inside `Entity_UpdateVehiclePhysics`).
11. The 62-frame divider + spawn-event `f[3] = sub_4E7000()[17]` re-checks (carried from the plan).
12. **Command-path bodies decoded** (dump 1545–2330, rides the command-source phase / D-INF-2):
    move-to-entity orders resolve the target by net-id across pools 0–3; vehicle boarding is a
    staged bone walk (count free `E1..E8` entry bones, claim a slot via entity+866 cross-checked
    against other soldiers' claims, then approach `E`→`S`/`G`→`H` stages via entity+865 with stop
    147 / guard 140 alignment and eighth-step position pulls) ending in `Entity_FindBestSeatSlot`
    + `Entity_RequestVehicleAttach`; `UseGun` bone = emplacement manning (radius 1u/3u); net-ids
    11000/12000/12001 get hardcoded escort/approach offsets (heading ±90° at 2–4u). The board path is
    gated by the waypoint_id command sentinel — `slot[148] ∈ {123,124,125}` (Goto SSN; MED names in
    §11) with target SSN = `wp_number` (`slot[152]`) [orig: `Entity_UpdateInfantryAI @ 0x4ba9ad` tests
    `slot[148]==125` → keep carrier `slot[144]`, else clear]. Cross-validated on 00TRa: SSN 1/1715
    (List 125, Number 11/1714) → board DTruck1/DTruck2.
13. **Idle look-at system** (dump 4089–4429, rides the combat pass; D-INF-5): every-256-tick
    interest scan over predicted positions (≤ min(slot+68, 20u) per axis) scoring closeness +
    facing-me (+4, < 2u and bearing−their-heading < ~25°) + local-player (+2) − re-stare (−12,
    skip last-but-one), front-arc gate ~70°, LOS-gated; winner → entity[209] head-look (aim pitch
    clamp ±30°), idle swaps 43→125 / 44→126 (current AND pending), greeting voice cues
    (`PlayerSlot_SetTimeout` 5/6/7 by tick phase), per-state voice byte `byte_813DE0`. Side
    effects: enemy seen → entity[206] focus + aim point entity[195..197] + alert 10; corpse
    (flag&2) seen → alert 25; pick emits 4 relation-matrix marks.
14. **Recoil/flinch decay** (dump 4430–4462, combat pass): entity[224]/[225] impulses decay by
    sixteenth-steps (floor 768→0) feeding pitch entity[5] and a PRNG-signed heading jitter
    entity[4]; entity[44]/[219] decay; torso-roll chase entity[183] → entity[6] sixteenth-step
    clamped ±238609280 (prone 48 halves the chase).
15. **Mounted pose states** (dump 4464–4539, mount/B2 pass): emplaced gunners force 67–75
    (`emplaced_N` by mount config +2156); seat passengers pose from the seat bone and take
    `sit_N` = `atol(bone_name_digits) + 76`; sit_24 (=100) drivers lean 107–110 by steering
    (entity+24 of the vehicle, ±71582784) and speed (+668). Port status: `UseGun`/gunner seats
    select `anim_emplaced` 67 plus a host-fed variant when that clip exists; non-gunner seats use
    the parsed `sitexNN`/`ctrlxNN`/`drvrxNN` pose index (`anim_sit_N`). Remaining gaps: deriving the
    emplaced variant from the real mount config, the driver-lean 107–110 overlay, and true per-tick
    seat-bone follow (see §9.2).
16. **Playhead rate**: channel time is normalized [0,1) advanced by a per-clip dt seeded at
    `AnimChannel_InitFromParams` (the literal 4096 param) — the exact dt derivation (sim-tick →
    clip-frame rate, blend-window advance) is unpinned; the IRootMotionSource seam owns phase
    policy, so this only matters for byte-exact playback timing.

## 5. Per-system equivalence verdict (2026-06-10, infantry port complete)

- **Infantry ground locomotion** (`Entity_UpdateInfantryAI @ 0x4b9910` → `AiSystem::tick_infantry`,
  libs/world/src/infantry.cpp): **MATCHING**, with the named, cited deviations —
  - **D-INF-1** no blend windows (clip switches reset phase; the original blends 10/15 ticks,
    root motion included) — rides the skeletal/blend pass.
  - **D-INF-2** command channels 123–127 (`waypoint_id`; MED "Goto SSN/Group/Player", §11) are
    partially driven. Commands 123/124/125 authored spawn attachment now resolve `wp_number` as the
    target SSN, apply the IDA-confirmed seat filter (123 passenger-only, 124 rejects `ctrlx`, 125 any),
    mount occupants already authored near a host-provided seat, and render `UseGun`/gunner seats with
    `anim_emplaced` plus available variants (00TRa class). Remaining gaps: staged E/S/G/H
    walk-to-seat, 126/127, child-seat traversal, true seat-bone transform follow, and driver-lean
    mounted poses.
  - **D-INF-3** ground/water resolver modeled as terrain-clamp + landing (platforms/water + the
    horizontal capsule pending; the vertical capsule-bottom settle now landed — see **D-INF-6** —
    `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0`); horizontal slide velocity
    zeroes on contact; the airborne anim overlay waits on the entity+36 flags.
  - **D-INF-4** — **CLOSED 2026-07-05**: the generator is witnessed and ported —
    `Math_BuildSinTable @ 0x613050` builds ONE 1281-entry sin table at 2^22 by an
    ACCUMULATING x87 loop (`angle += dbl_7DF578 = 0.006135923151542565` per entry,
    `_ftol2_sse` truncation, end bound `0x31C0FC4`); the cos consumer reads the same
    table +256 entries (`off_849934 = outMillis + 0x400`). Ported structurally in
    `infantry.cpp quantized_dir` (double accumulation is integer-identical to the
    prior closed form for every entry — pinned in `infantry` ctest with landmark
    values 0 / 4194304 / 0 / −4194304; any residual x87-extended vs SSE2-double
    low-bit difference is the D-3DI-1 substrate class).
  - **D-INF-5** idle look-at system + its spotting side effects (§4.13) — rides the combat pass.
  - **D-INF-6** infantry/player grounding settles `pos[2]` (the model origin) to **`ground +
    capsule_bottom`**, so the entity's collision-capsule bottom (the origin→feet offset) rests on
    the terrain and a waist-origin model's feet land exactly on the ground. WITNESSED end-to-end
    (re-confirmed against `Jointops.exe.kong.i64`, imagebase 0x400000, this session):
    `Entity_ProcessCollisionAndPlatformPhysics @0x4b2bd0` resettles `entity[3] = entityRadius +
    groundHeight` (`@0x4b3da3`; `groundHeight = heightDelta − entityRadius @0x4b3d90`), where
    `entityRadius` is the current animation frame's `capsule_bottom × 65536` fed from the
    `.bad`/`.adm` root record [orig: `AnimMap_UpdateEntity @0x40b5f0` (out-transform block `@0x40b82f`) `out_transform[3] =
    bottom*65536 @0x40b84d`; `out_transform[4] = top*65536 + 0x2000` is the capsule top]. Both
    on-foot callers pass it straight in [orig: org1 `@0x4bf7fa`, org2 `@0x4b7cf9`]. The model
    renders at `pos[2]` with **no render-side lift** [orig: `Math_BuildFixedPointToFloatMatrix4x4
    @0x612200` Z store `@0x612457` — pure 1/65536 scale, only Y negated]. Stance-aware: crouch/
    prone clips carry a smaller `capsule_bottom`, lowering feet *and* the FP eye (`pos[2] + 0x10000`
    [orig: `Camera_ComputeThirdPersonView @0x437e8f`]). The `+0x50000` [orig: `AI_ProcessMovementStep
    @0x466db0` `ai_comp[131] = ground + 0x50000 @0x466e2d`] is the id-3 death-fall mover's vertical
    TARGET slot, **NOT** the live `pos[2]` — applying it to the render Z floated soldiers ~1 body
    (the regression this entry corrects; the earlier feet-origin/no-offset reading is REFUTED —
    dropping the term sinks them waist-deep). Our port: `RootMotionFrame` carries absolute
    `capsule_bottom`/`capsule_top` (godot `InfantryRootMotion` emits them from the `.bad` bottom/top
    tracks); `tick_infantry` — player AND AI, the player via `InfantryState::is_local_player` — floors
    `pos[2] = ground_cache + frame.capsule_bottom` (`libs/world/src/infantry.cpp`). The shared
    `ai_->root_motion` is loaded from `E_STAND.adm` at mission load (`mission_runtime.gd`), so every
    motor-driven soldier resolves a real standing `capsule_bottom`. `ground_stand_offset` (0x50000)
    is retained only for the vehicle/SM `apply_ground_clamp` path. Guarded by the capsule-settle case
    in `tests/world/infantry_test.cpp`.
  - **D-INF-9** player horizontal-slide decay. The player shares the NPC's slide-velocity damp, but
    the original splits it by the grounded flag: a GROUNDED player decays `inf.vel[0]/[1]` by
    `(63·v)>>6` with NO deadzone [orig: `Entity_UpdateInfantryPlayerBody @0x4b7949` — `shl 6 / sub /
    sar 6` on `entity+0x98/0x9C`, selected by the `entity+0x24 & 0x2000` grounded flag `@0x4b78ab`];
    an AIRBORNE player uses the same `(7v+4)>>3` + `abs<=8→0` deadzone as the NPC [orig: `@0x4b7982`].
    The two formulas are mutually exclusive, not sequential. A prior pass gated slide damping behind
    `!is_local_player` (and dropped the grounded velocity zero), so the player's slope-slide impulse
    drifted forever; FIXED in `tick_infantry` (`libs/world/src/infantry.cpp`), guarded by the
    player-slide case in `tests/world/infantry_test.cpp`. (Our `inf.airborne` here reads last tick's
    value — the vertical resolve updates it after — a negligible 1-tick lag vs the original reading
    the flag set in the same physics pass.)
  - **D-INF-10** per-tick gravity, asymmetric by motor. Neither infantry mover gates the vertical step
    on tick parity. The NPC (org1) falls `vel_z -= 416` EVERY tick then `pos.z += 2·vel_z` [orig:
    `Entity_UpdateInfantryAI @0x4bf7bf` (`add … 0xFFFFFE60`) / `@0x4bf7ec` (`add edx,edx`; `add
    [esi+0Ch],edx`)]; the player (org2) falls `vel_z -= 208` EVERY tick then `pos.z += vel_z` (once)
    [orig: `Entity_UpdateInfantryPlayerBody @0x4b7acf` (`add … 0xFFFFFF30`) / `@0x4b7cef`]; both clamp
    to terminal −32768. A prior pass applied one `−416 every 2 ticks` + `pos += 2·vel` to BOTH, which
    nets to the player's −208/tick + vel/tick (org2) but left the NPC at HALF the org1 fall rate.
    FIXED for the NPC (faithful per-tick `−416` + `2·vel`); the player keeps the 2-tick discretization
    (net-equivalent to org2 — its jump/fall tuning + tests pin the `−416`-per-application step, so
    making it per-tick `−208` is deferred to a dedicated player-physics grill). `libs/world/src/
    infantry.cpp`; guarded by the gravity-cadence case in `tests/world/infantry_test.cpp`.
  Everything else is structurally translated with per-mechanic dump citations and byte-pinned
  constants, unit-tested in tests/world/infantry_test.cpp and end-to-end in promote_test.
- **Root-motion data path** (`AnimMap_UpdateEntity @ 0x40b5f0` → engine `InfantryRootMotion`):
  **MATCHING** — record layout, scales (32768/65536), fence-post lerp, vertical-delta and event
  semantics pinned by disasm + the real-clip grill; the exact playhead dt derivation is open
  item 16 (the injection seam owns phase policy).
- **AI vehicle decision layer** (SM states 16–18, budget gate, waypoint mover, P1/P2 combat):
  **MATCHING** as before — those verdicts stand; it no longer drives organics. Vehicle/HELO
  movement physics remain visible `not_yet_ported` stubs (not deviations).
- **WAC VM**: **MATCHING** (unchanged this pass).

## 6. IDA write-backs applied (2026-06-10, IDB saved)
Renames: `Entity_UpdateAircraftPhysics @ 0x490310`, `AI_DispatchStateMachineByProfileClass @ 0x4680a0`,
`Entity_DispatchPhysics_{cbike,ctank,cveh,cbot,ctrn}` (two were bogus libc `__mkgmtime*` FLIRT hits),
`g_EntityClassEventCallbackTable/Count`, `g_EntityClassPhysicsTable`, `g_AIMoveStepFnTable`.
Defined: `0x4680a0`, `0x490310` (9.2 KB, jump tables had broken auto-analysis), the three dispatcher
stubs. Reverse-link + truth comments on 0x4b9910, 0x457bd0, 0x4581b0, 0x490310, 0x466db0, 0x4680a0,
0x82abc8. Root-motion pass: truth comments on the 0x40b5f0 out-transform block (0x40b82f) + function
comments on 0x40b5f0/0x40b230 recording the .bad-events record identity and the pinned scales.

---

*Appendices 7–12 consolidated 2026-06-10 from scratch notes `notes/mission/ai-driver-grill-2026-06-08.md`,
`notes/mission/anim-ai-grill-2026-06-07.md`, `notes/mission/mount-emplacement-2026-06-08.md`,
`notes/mission/phase4-coord-migration-2026-06-08.md`, `notes/mission/terrain-grounding-2026-06-08.md`,
`notes/mission/waypoint-slot-model-2026-06-07.md`, `notes/object-placement-grill.md`. Addresses are
Jointops.exe retail unless marked `dfx2med.exe` (the DFX2 mission editor, imagebase 0x400000).*

## 7. Appendix: AI driver + spawn-state grill (2026-06-08)

### 7.1 Spawn never sets state 16 — the 0→16 transition fires at the first AI tick
- `Entity_InitVehicleAI @ 0x460200`: allocates the 812-byte brain (`unk_AED380` stride 812), loads the
  AI profile, copies geometry, inits `ai[4]=ai[5]=ai[6]` from a saved field (fallback, effectively 0),
  scans the model user-points for weapon bones (`prim`/`bullet01`/`bullet02`/`flare`). **No state 16.**
- `Entity_SpawnFromBMSRecord @ 0x40e9f0`: for AI items (itemdef flag 0x100000) allocates the AiSlot
  (entity+104) and fills accuracy/engagement/FOV; when BMS record byte 79 is nonzero it sets
  `slot[140] = 1` (follow-path flag), `slot[148] = record[79]` (path number), `slot[152] = wp_number`.
  Still no state 16.
- ⇒ The 0 → 16 (GROUND_FOLLOWWP) transition fires at the **first AI tick** — the state-0 tick handler
  reads `slot[140]` — never at spawn. Our promote force-set of `kCurState=16` (gated by
  `opts.patrol_on_spawn`) is a tracked deviation; the faithful port routes through a state-0 handler
  that reads the follow flag.

### 7.2 Heading convention — RESOLVED: engine heading = 90 − yaw
Spawn writes `entityData[4] = (90 − bmsYawDeg) · kBamPerDegree` (`kBamPerDegree = 11930464 = 2^32/360`),
so the **engine heading is `90 − yaw`**, not yaw (closes the long-open "90-yaw vs 180-yaw" item). The
waypoint mover writes `kWorkHeading = atan2(dY, dX)` — an ENGINE-frame bearing.

**The bug this exposed:** our brain heading was inconsistent between parked and moving — the spawn seed
stored mission-frame yaw while the mover stored engine-frame bearing, and the present pass fed both to
`bms_to_godot_basis` as if they were mission yaw. A unit moving +Y (north) rendered as `90 − bearing`
and faced east: a direction-dependent error, not a constant offset.

**Fix (shipped):** store `heading` in the ENGINE frame everywhere, matching the original — seed
`(90 − yaw) · kBamPerDegree` (promote), mount pose `(90 − occ.yaw)`, and convert back once at the
present boundary (`mission_yaw = 90 − heading / kBamPerDegree`). The Godot host basis
(`MissionObjectPlacer.bms_to_godot_basis`) already applies the faithful `RotY(90 − v)` plus a
`RotY(90)` .3di model-forward correction (net `RotY(180 − v)`) where `v` is the MISSION yaw — the
`90 − yaw` belongs in the basis builder, never doubled into the seed.

### 7.3 Pitch/roll spawn scaling
`entityData[5]/[6] = deg · 2^32/360` — same BAM scaling as heading, **no 90 offset**.

### 7.4 Movement-step layer (the fuller ground mover — port deferred)
- `AI_UpdateMovementTarget @ 0x460e40` is the REAL ground waypoint mover (vs our lean
  `AI_UpdateWaypointMovement @ 0x457bd0` port): calls `AIWaypoint_UpdateTarget`, advances the node with
  the same loop/one-shot logic we ported, sets X/Y from the node (mode 1 bone / mode 3 literal), and
  drives the VERTICAL: `brain[131] = nodeZ` when `aiComp[108]` (= profile+14, set in
  `Entity_InitVehicleAI`), else ground via `Entity_CalcAverageGroundHeight(e, 0x200000)` plus the def
  heightOffset (`def[20]==2` selects the raw offset), floored at `def[232] + ground`
  (= `max(targetZ, ground)`); then heading = bearing + near-target speed halving.
- `AI_ProcessMovementStep @ 0x466db0` pins X/Y to the current pos, sets
  `brain[131] = Entity_CalcAverageGroundHeight(e, 0x50000) + 0x50000` unconditionally, with the 372/744
  phase counters. This session labeled it "the vehicle mover" — **superseded by §1.3**, which
  identified it as a directional **death-fall mover** (move-step ids 0–3); the mechanics stand.
- Dispatcher facts confirmed: SM dispatcher `0x4581b0` (24-row table @ 0x815238), per-frame budget gate
  `AI_BeginUpdate @ 0x457b40` (cap 496); dispatch event args 0=update / 1=spawn / 4=death. The
  then-open "per-entity driver near 0x462120" lead was later resolved by §1.1: those are the
  event-callback thunks for `cbot/cpln/ctrn`.

## 8. Appendix: animation selection from item-type ADM (2026-06-07)

Editor-side addresses below are `dfx2med.exe`; runtime dispatch addresses are Jointops.exe.

### 8.1 There is no per-entity animation field
The per-entity properties dialog (`Med_ObjectPropertiesDialog @ 0x4096d0`, dfx2med — mirrored by our
Selection panel) has **no animation control**. Per-entity animation comes from exactly two places:
1. **The item-type ADM**: graphic-def offset **+180** holds the `.adm` name — proven by the
   asset-manifest exporter (`sub_44EE10`, dfx2med), which prints the `%s.adm` listing from def+180.
   ADM is plaintext key/value; `anim_<name>` keys map names → `.bad` clips. Surfaced as `anim_def` in
   `NovaItemDatabase`. Runtime entry points: `AnimMap_LoadAdmFile @ 0x40cc40`,
   `AnimMap_PlayAnimBySlot @ 0x40bda0`, `AnimMap_FindSlotByName @ 0x40cfa0`.
2. **The PLAYPARTANIM mission action** (AI sub-type 34) — the vehicle/emplacement PART system (§8.4).

### 8.2 AI action sub-type param domains
Action types CHANGE_GROUP_AI(3), AREA_AI_RED(12), AREA_AI_BLUE(13), CHANGE_SINGLE_AI(21) share the AI
sub-type table (`Med_ActionSubTypeName @ 0x445EE0`, dfx2med). Record layout: `action_sub_type` is its
own dword; **param1 = target** (group / unit-SSN / zone), **param2/3/4 = the sub-type's slots 0/1/2**.
Per-sub-type slots from `Med_AiSubTypeParams @ 0x44A920` (dfx2med):

| sub | token | slots (widget) |
|----|-------|----------------|
| 2 | GUARD_BIT | BitToggle |
| 5/6/22 | RED/GREEN/YELLOW_ALERT | (no param) |
| 8 | ACCURACY_100 | IntSmall (0..100) |
| 15/16/17/21 | BLIND/BERSERK/CLIMBER/COWARD _BIT | BitToggle |
| 26/27 | DRIVESKILL/AIMSKILL | Fsm enum @ 0x5e6498 |
| 28 | AISETSTATE | Fsm enum @ 0x5e64c8: 1 FSMFORMATION / 2 FSMRTB / 3 FSMPRETTY / 4 FSMLAND / 5 FSMFOLLOWWP |
| 29/30 | COMBAT/PATROL SPEEDKMH | IntGeneric (0..999) |
| 31/44 | FIND_AND_USE / TARGETSSN | entity-SSN picker |
| 32/33 | AIUSEWPZ / AICLEARWPZ | (no param) |
| 34 | **PLAYPARTANIM** | 0=ANIMNUM IntGeneric; 1=ANIMPLAYTYPE enum @ 0x5e64f8 (1 play / 0 stop / −1 reverse); 2=ANIMTIME fixed-sec |
| 37 | HUDITEM | 0=enum @ 0x5e6414 (ns `off_5B5800`); 1=TICKS IntGeneric |
| 39 | TMATESTATUS | BitToggle |
| 40 | AINODEPATH_BIT | BitToggle |
| 41 | ATTACKDISTANCE_VALUE | IntGeneric |
| 42 | ENGAGEDISTANCE | 0=MIN, 1=MAX (IntGeneric) |
| 43 | INDESTRUCTABLE_BIT | BitToggle |
| 45 | STARTFIRING_BIT | BitToggle |
| 46 | FIRING_ANGLE | int 0 down to −359 (degrees) |

Widget domains (decompiled, dfx2med): IntGeneric = raw 0..999 (no <<16; runtime shifts at eval);
IntSmall = 0..100; BitToggle = {0,1}; ANIMTIME = raw 16.16 seconds shown "%2.4f", step 256, range
0..327424 (~0..5 s).

Runtime dispatch (Jointops): `EventAction_Dispatch @ 0x4542e0` switches on action_type (record +1 type,
+2 sub_type, +3 param1, +4..+6 param2..4); case 3 → `Entity_HandleAlertCommand`, case 0x15 →
`Entity_HandleAlertStateEvent @ 0x43dee0`. AI sub-type dispatcher = `Entity_ApplyCommand @ 0x43ab60`:
5/6/0x16 = alerts (stance ai+136 = 2/0/1), 8 = ACCURACY (`ai+40 = 100 − param2`), 0x1C = AISETSTATE
(queues AI event 7), 0x1D/0x1E = combat/patrol speed (events 10/11).

### 8.3 ANIMNUM is a part-anim CHANNEL, not an animation name/index
**Durable warning:** `Entity_ApplyCommand` case 0x22 validates ANIMNUM ∈ {1,2} — it selects one of two
model part-anim channels (slot = channel − 1); any other value is a no-op. The animation content is the
model's PANM. Do NOT wire ANIMNUM to `off_8135F0` — that is the separate infantry full-body table
(§3.4, AI-state-driven via `Script_ForceAnimation @ 0x4f2610` / `Entity_UpdateInfantryAI @ 0x4b9910`).
ONED therefore exposes ANIMNUM as a plain "Part #" raw int (the speculative name-picker was removed).

### 8.4 PLAYPARTANIM contract (case 0x22, exact rate formula)
- Stores per-channel **direction** (`play_type` ∈ {−1,0,+1}) at `comp+436+4·slot` and a **rate** at
  `comp+444+4·slot`; `comp` = the 812-byte AI struct at entity[25].
- Rate derivation: `seconds = ANIMTIME / 65536` (confirms param4 raw = sec·65536);
  `rate = ftol((0.016 / seconds) · 65536)`, min 1 — i.e. **1048.576/seconds 16.16-phase units per
  62.5 Hz tick**, so the phase crosses its full 0..1.0 range in exactly `seconds`. Constants verified
  IEEE-754 LE: `flt_7C3310 = 1/65536`, `flt_7C3B40 = 0.016 (= 1/62.5)`, `flt_7C32BC = 65536`.
- Writes ONLY direction + rate — it never resets the phase. PLAYPARTANIM is **velocity control from
  the current position** ("start moving part c at this speed/dir; play_type 0 = halt"), not
  reset-and-sweep.
- Channel defaults come from the object/vehicle def (`Entity_CopyVehicleDefToAIComp @ 0x45ddf9`:
  def+764..784 → comp+436..456) — objects can ship an idle part-anim (radar-dish spin).
- It never calls AnimMap: the part system (turret yaw / barrel pitch / dish) is DISTINCT from the
  infantry full-body ADM/BAD system.
- Port contract (`NovaObjectModel.play_part_anim(channel, play_type, time_s)`): channel ∈ {1,2} → PANM
  control register index `slot`; value 0..65535 = the 16.16 phase; speed = 65535/time_s per second
  (delta-based); velocity from CURRENT value; **endpoint = clamp [0,65535]** (one-shot; continuous-spin
  wrap is the def-default idle case, flagged as a possible per-part refinement — the consumer's
  clamp-vs-wrap was not captured).

### 8.5 bmsi attribute flags (checkbox dialog)
Flag label table @ 0x5b1c84 (dfx2med): REFLECTIVE, INDESTRUCTABLE, GUARDING, BLIND, **DEAF**,
**IGNORE_FOOTSTEPS**, NAVIGATION_WAYPT, MULTIPLAYER. Confirmed bit positions (our
`BmsiAttributeFlags`): Blind=0, Guarding=1, Multiplayer=6, Indestructible=21, NavigationWaypoint=22,
Reflective=23. **OPEN:** DEAF/IGNORE_FOOTSTEPS bit positions need the dialog's CheckDlgButton
load/save handlers (deep in the 0x4ffd-byte `Med_ObjectPropertiesDialog`); until decoded the editor
preserves unknown bits verbatim (merge-on-write), like event flags.

## 9. Appendix: mount/emplacement mechanics (2026-06-08)

### 9.1 Verified chain (AttachToEmplaced, action 37)
- `EventAction_Dispatch @ 0x4542e0` case 0x25: reads ONLY param1 (the occupant SSN), resolves it via
  `EntityPool_FindByNetId`, calls `WacScript_TryMountEntityToVehicle`. The gun/vehicle is found
  IMPLICITLY inside the mount fn.
- `WacScript_TryMountEntityToVehicle @ 0x4f70f0`: validate handle (≠0xFFFF, pool<5, slot<capacity);
  gate alive/model present; require `occupant-model+144` (a pre-established hierarchy link to the
  vehicle) and not-already-mounted (`entity->pad8[8] == 0`). Then
  `seatBone = Entity_FindBestSeatSlot(entity, *(model+144), &outEntity)` (outEntity = the chosen
  seat-owner, possibly a child) → `Entity_RequestVehicleAttach`. The attach-failure path clears
  `Flags & ~0x40` (the mounted bit).
- `Entity_FindBestSeatSlot @ 0x4351f0` (CONFIRMED EXACT): sentinel `bestWeight = 65536000`; iterates
  the vehicle + its child entities (vehicle+444/+448), 10 slots each; `boneIdx = model[605+slot]`
  (0 = empty); occupant u16 at `vehicle[400+2·slot]` (free if 0xFFFF or == playerHandle). Seat-bone
  NAME classification (bone record stride 48, name at +32): `sitex` → 1 passenger, `ctrlx` → 2
  controller (vehicle entity only), `drvrx` → 5 driver (vehicle entity only), `UseGun` → 3 gunner;
  else skip. Command/player-class acceptance gate: `model+148 == 123` accepts only passenger
  (`seatType == 1`); `model+148 == 124` rejects controller (`seatType != 2`); all other values
  accept any classified seat. This corrects the older "124 not-driver" reading.
  **Weights (LOWER wins):** ctrl/drvr `0x2000` < gunner `0x20000` < on-vehicle passenger `0x200000` <
  child-entity passenger `0x2000000`.
- True occupant pose comes from the seat bone transform (`Entity_GetBoneTransformAndOrientation @
  0x4b0c50`, `Entity_SerializeVehicleState @ 0x460560`); mounted-pose anim states (emplaced
  67–75, sit_N, driver lean) are §4.15.

### 9.2 Port (libs/world + libs/mission) and tracked deviations
Shipped: `Entity.seats` + occupant refs riding the registry value-copy (`World::Snapshot` ⇒ Play→Stop
rewinds mounts for free); `EntityCommands::{find_best_seat, mount, mount_boarding_command,
mount_best, dismount, find_mounted_on}` mirroring 0x4351f0/0x4f70f0/0x4355f0/0x4359f0;
`pose_mounted_occupant` (occ.pos = veh.pos + rotate(seat_local, veh.yaw), gunner yaw = veh.yaw −
yaw_offset); `AiSystem::pose_if_mounted` skips SM + locomotion and auto-dismounts when the vehicle
is gone; event-runtime case 0x25 → `mount_best(param1)`; command-123/124/125 promotion mounts
already-near occupants onto their target SSN with the runtime gate above; mounted infantry pose class
is selected from the occupied seat (`UseGun` → 67+variant if that clip exists, other seats →
`anim_sit_N` from the seat name digits). GDExtension debug cards expose the selected seat source name
and the full target-seat candidate list (`source_name`, type, pose index, local offset, occupancy) for
00TRa-style audits. Deviations (NOT silently absorbed):
1. **Proximity proxy vs occupant-model+144.** The original's vehicle is the occupant's model hierarchy
   link; we pick the nearest free-seat entity within 20 units (`kMountRadius`). Faithful for a soldier
   placed on its gun; wrong if two guns overlap.
2. **Seat specs are host-fed.** The original reads model seat bones directly (model[605..]); the
   port consumes host-extracted model userpoints through `ItemSeatSpec`, so callers without model
   metadata still seed no seats.
3. **Child-entity seat traversal** deferred (single-entity seats only).
4. **Mounted-pose variants are partial.** `UseGun` can consume a host-fed emplaced variant and
   non-gunners consume numbered `sit_N`, but deriving the variant from the real mount config and the
   sit_24 driver-lean variants remain open.
5. **Seat-local pose stand-in** — seat_local/yaw_offset come from host userpoints when available, but
   the true per-tick bone transform (`Entity_GetBoneTransformAndOrientation`) is still deferred. This
   is the known suspect when a rider attaches to the right logical seat but appears too far forward.

## 10. Appendix: coordinate frames + terrain grounding (2026-06-08)

### 10.1 The BMS↔Godot convention lives in GDScript (and the godot-cpp Basis trap)
Mission frame is Z-up; the Godot host is Y-up. The conversion is single-sourced in GDScript
(`MissionObjectPlacer` statics, used by both present + placer): basis =
`Basis(UP, 90−yaw) · Basis(BACK, −pitch) · Basis(RIGHT, roll) · Basis(UP, 90)` (engine heading +
.3di model-forward correction; see §7.2 for the 90−yaw truth).

**Durable warning:** a C++ migration (`NovaMissionGeometry`) was REVERTED because godot-cpp's
`Basis(Vector3 axis, real_t angle)` diverges from godot-core for **negative-component axes**: with the
pitch axis `BACK = (0,0,−1)`, a 30° pitch produced fwd = (0, **+0.5**, −0.866) in C++ vs the
engine-faithful (0, **−0.5**, −0.866) in GDScript — an exact Y sign flip, same formula, same constants.
Yaw (UP) and roll (RIGHT) terms matched, so yaw-only tests pass and hide it. If revisited: build the
C++ basis from explicit double-precision rotation matrices (or Quaternion) and add a C++/GDScript
parity test over a (pitch, yaw, roll) grid BEFORE wiring callers.

Also noted: the editor's ground sampling (`sample_world_height`) reads the live editable FORMAT_RF
Image while the runtime samples `cpt.depth_buffer` via the portable `TerrainHeightField` — two data
sources; unifying needs a shared sampler (open).

### 10.2 Ground sampling chain — `Entity_CalcAverageGroundHeight @ 0x457230` (CONFIRMED EXACT)
5-tap weighted ground height, `(entity, sampleRadius)`:
- N/S taps at (0, ±r), E/W at (±r, 0), C at center; `max` = highest positive tap (C included);
- `result = (N + S + E + W + 2·(C + 2·max)) / 10`, floored at C;
- water clamp: `if (*(entity+368) && worldY > result) result = worldY` (worldY @ 0x26C6454);
- def offset: `result += dead ? def+0x30 : def+0x2C`, where
  `dead = ((entity+36 & 2) || entity+286 <= 0) && entity+52`;
- radius 0 path: center tap only.

**Decompile-lossiness warning (durable):** the per-tap samplers `sub_4142C0` / `sub_414320` decompile
as if they ignore the dx/dy offsets and return a flat field — WRONG. Disasm + the pointer write-back
show each builds `pos = {x+dx, y+dy, z + 0x10000 − 0x300000}` (a ray from z+1.0 down to z−47.0) and
calls `raycast_entity_collision @ 0x413760`, which writes the sampled ground height back into `pos.z`
(returned in eax) — the 5 taps DO sample 5 distinct columns. `sub_414320` additionally stores the
raycast collision-object at record+40. The real sampler is
`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`: a LoRes pass, then back/forward step + 8-iteration
bisection refine (the per-step terrain samples are inlined FPU the decompiler dropped to
`null_stub()`); for a near-vertical down-ray this equals the bilinear heightmap column height.

### 10.3 The vertical assignment + our port
Both movers recompute `brain[131]` from the sampler EVERY tick: `0x466db0` sets
`ground(0x50000) + 0x50000`; `0x460e40` sets `max(nodeZ-or-ground+heightOffset, def[232]+ground)`
(§7.4). Our lean `AI_UpdateWaypointMovement @ 0x457bd0` port never wrote `brain[131]` — the root cause
of the floating-AI bug. Shipped: portable `terrain/height_field` (the three `NovaTerrainData`
samplers lifted verbatim, NovaTerrainData delegates); `world::calc_average_ground_height` (the
0x457230 math, 16.16); `AiSystem::apply_ground_clamp` SETs `brain[131]` + snaps
`pos[2] = ground + 0x50000` (SET not max — the lean mover leaves brain[131] stale; a max would strand
a float); `NovaSimulation::set_terrain_height_field` wired in game + editor preview. Tracked
deviations: (1) bilinear column height vs the hi-res along-ray bisection (faithful for grounding);
(2) GroundClearance def fields (def+0x2C/+0x30, def+216/+232, the aiComp[108] node-Z gate) default 0
until those def fields are RE'd; (3) pos[2] snaps (no climb-rate physics); (4) water clamp wired off
in the binding (water_height units vs the 16.16 worldY unverified; sampler + calc support it).

## 11. Appendix: waypoint slot model (2026-06-07)

- `waypoint_id` (BMS entity record **byte 79**) holds 0..127: 0 = None, **1..122 = path numbers**,
  **123..127 = AI commands** (Goto SSN/Group/Player — see the last bullet, not paths). dfx2med
  `Med_ParamWaypointList @ 0x449c60` lists 1..127 (0 = None), each backed by a 127-entry
  name array (stride 1548); the packer `Med_PackEntityRecord @ 0x44c8e0` writes byte 79. Our parser
  stores exactly **128 positional waypoint records** (`kWaypointRecordCount`), so array index == path
  number. Byte 78 = group/parent ref.
- Jointops reads byte 79 only as the follow-path inside the AI block (`Entity_SpawnFromBMSRecord @
  0x40e9f0`, itemdef flag 0x100000): `slot[140]=1; slot[148]=record[79]; slot[152]=wp_number` (§7.1).
  Note the infantry think reserves channel ids **123–127 as commands** (§3.2), so usable mission path
  ids are 1..122 even though the editor lists 1..127.
- **Attachment is RUNTIME state, not a BMS byte**: mount/attach is `entity+364` set at runtime
  (`Entity_ToggleVehicleMount @ 0x436950`, `Vehicle_HasEnemyOccupant @ 0x4359f0`). "Attached To
  SSN" / "ATTACH_TO_EMPLACED" are trigger/action name-table entries (PlayerAttachedToSsn = trigger 38,
  AttachToEmplaced = action 37), not entity-dialog fields.
- **`waypoint_id` 123–127 are AI COMMAND channels, not path numbers** (the MED "Waypoints > List"
  dropdown; usable mission path ids are 1..122). The dropdown names them: **123 = Goto SSN (not
  driver, gunner)**, **124 = Goto SSN (not driver)**, **125 = Goto SSN (any)**, **126 = Goto Group**,
  **127 = Goto Player**. So a 123–127 value backed by an empty path slot is EXPECTED — it is a
  command, not a route. For 123–125 the command = navigate to the entity whose SSN = `wp_number` (the
  editor "Number" field; spawn stores it in `slot[152]`, §7.1) and **BOARD it**. Runtime seat filter
  truth from `Entity_FindBestSeatSlot @0x4351f0`: 123 accepts only `sitex`/passenger, 124 rejects
  `ctrlx`/controller while keeping `drvrx` eligible, 125 accepts any classified seat. Engine witness:
  the server-authority infantry think tests `slot[148]==125` [orig: `Entity_UpdateInfantryAI
  @0x4ba9ad`] — `==125` preserves the carrier vehicle pointer in `slot[144]`, else clears it; when set
  it runs `Entity_FindBestSeatSlot @ 0x4351f0` (seat-type filter) → `Entity_RequestVehicleAttach
  @ 0x4364a0`.
  **CORRECTION (overturns the prior revision of this note):** an earlier version called 125 "valid
  leftover data … the game ignores it (the 'Value 125' inspector mystery)" — that was WRONG; 125 is an
  active Goto-SSN-and-board command. An inspector should name 123–127 by their command, not "Path N
  (no markers)". Cross-validated on `00TRa.bms` (env JOX): organic SSN 1 = List 125 / Number 11
  (= DTruck1, `id 101294`), SSN 1715 = List 125 / Number 1714 (= DTruck2, `id 101420`) — both
  "Goto SSN (any) → board the adjacent 5-ton truck". This is the root cause of the OpenNova "two
  friendly soldiers float in a crouched idle pose" bug in 00TRa: the reimpl never honors the 123–127
  command channels (**D-INF-2**), so the soldiers idle on the terrain instead of riding their carrier.

## 12. Appendix: entity placement — Ground userpoint (dfx2med.exe)

Question: does the engine apply the model's "Ground" userpoint at render/load, or only at author-time
placement? All addresses dfx2med.exe.

- Userpoint lookup `sub_459C70(model_userpoints, "Ground")` → struct with x/y/z at +0/+4/+8;
  `"Ground"` string @ 0x5b145c, referenced by `sub_401A90`, `sub_44D920`, `sub_43BAD0`.
- **`sub_401A90` — the place-object dialog (DECISIVE; proposed rename `Med_PlaceObjectDialogProc`).**
  Spawns the picked item via `sub_455900`, then ONCE subtracts the full **unrotated** 3-vector from
  the entity position (@ 0x401f6e; same in the scatter/multi-place loop @ 0x4021fe). The bake lands
  in the STORED position at placement; nothing applies a Ground offset at render (else the engine
  would double-count its own bake).
- `sub_44D920` / `sub_43BAD0` (callers `sub_440FD0`, `sub_468850`): a separate **vertical-only**
  terrain-conform — only the height component (+8) subtracted from a terrain height.
- **Verdict:** the Ground userpoint is an **author-time bake into the stored position**; stored
  positions render **directly**. Render-time anchoring on load is wrong.
- Fix (landed in PR #52): render stored positions directly (anchor_inv dropped from static batch,
  animated transform, place_single, pick colliders, drag visual); bake the Ground anchor at
  **terrain-drop only** (`place_entity_at_world` fresh insert + `_move_selected_to_world` drag) as
  `stored_bms = cursor_bms − godot_to_bms_position(get_ground_anchor())`, with the placer's position
  map `godot_vec3 = (−x, y, z)`; markers and the free gizmo translate unchanged. Load+save stays
  byte-identical (we never bake on load). Open visual check: whether the self-consistent bake
  byte-matches the engine's raw subtraction depends on the model↔entity axis consistency of our
  import pipeline — verify by loading an original `.bms` and checking objects sit on terrain.

## 13. Appendix: held-weapon visibility on mount/attach (engine-research, 2026-06-24)

Question: how does the original suppress the soldier's **held weapon** (the third-person gun
in the hands) when the soldier is attached to a vehicle seat or emplacement? Status:
**engine-research / confirm-only** — originals witnessed; OpenNova does not render a
third-person held weapon yet (the body `.adm` clip names encode the gait+weapon, but no
separate weapon model is mounted), so there is no reimpl to diverge. This records the
mechanism for when held-weapon rendering lands. Complements §9 (mount/emplacement) and §4.15
(mounted poses). All addresses `Jointops.exe.kong.i64`, imagebase 0x400000.

### 13.1 The render gate — `BoneCallback_org0_World @ 0x4e3940`
The per-class render callback for **organic** entities (the model class tag `org`; used by the
local player, remote players, AND NPCs alike — render is class-keyed independently of the
org0/org1/org2 *motor* split of §1.2). It builds the bone matrices
(`Entity_BuildBoneTransformMatrices @ 0x4b1290`) then issues up to **six**
`Render_SubmitEntity @ 0x5dad80` draws, in order:
1. **shadow blob** (gated on `entity+0x378`/`+0x37A`),
2. **body** — `key` model; suppressed for the camera-tracked entity in first person
   (`entity == dword_A890CC` = the camera target, unless `dword_A890C8` third-person — see
   §5.39 / correspondence.md `Camera_SetTrackedEntity @ 0x4391d0`),
3. **muzzle flash** — gated on `Flags & 4` (`entity+0x24`), drawn at the flare bone,
4. **weapon sight/scope** — gated on `g_animStateFlagsTable[animStateId] & 0x40 && (Flags & 8)`
   (the per-anim-state flag table of §3.4),
5. **held weapon** — see §13.2,
6. **mounted-child overlay** — gated on `mountedChild` (`entity+0x268`), draws the carried
   child/flag at a Z-rotated transform.

Draws 5 and 6 are *both* additionally suppressed wholesale when the render-pass flag
`numEntries & 0x10000000` is set (`skipWeaponOverlay` — e.g. a shadow/special pass).

### 13.2 The held-weapon submit (draw 5) and its predicate
[orig: `BoneCallback_org0_World @ 0x4e3940`]
```
if ( !(numEntries & 0x10000000) )            // not the overlay-skip pass
  if ( entity[0x2B0] )                        // held-weapon ADM index (u8) != 0
    if ( Entity_CanFireWeapon(entity) ) {     // <-- THE HIDE GATE
      weaponDef = AdmDef_GetEntryByIndex(entity[0x2B0]);   // [orig: 0x53fc80]
      entity->Weapon /* +0x298 */ = weaponDef;
      ... Render_SubmitEntity( weaponFrameData, boneMatrices @ hand/'prim' bone ) ...
    }
```
- `GamePlayerEntity+0x2B0` (typed `pad_2b0` today) = the **held-weapon ADM model index**;
  `+0x298` (`Weapon`) caches the resolved `AdmDef`. The weapon model is posed at the
  hand/`prim` bone matrix from `Entity_BuildBoneTransformMatrices @ 0x4b1290` (the same `prim`
  user-point the AI scans in `Entity_InitVehicleAI @ 0x460200`, §7.1).
- **The weapon is drawn iff the soldier may *fire* it.** One predicate,
  `Entity_CanFireWeapon @ 0x4dcb10`, drives both gameplay fire-permission and this draw.

### 13.3 `Entity_CanFireWeapon @ 0x4dcb10` — seat type decides
Returns 0 (→ weapon hidden) by the rider's **seat type in `parentSlot` (`entity+0x168`)**:
- Top gate, both branches: `if (entity->Flags & 2) return 0` — a separate "weapon disabled"
  state on `entity+0x24` (independent of mounting; cleared on spawn/respawn, §5.6
  `Entity_ResetToSpawnState`).
- **Remote** entities (NPCs + other players): `parentSlot ∈ {2, 3, 5} → return 0`.
- **Local** player (`entity == g_local_player_entity`): not mounted (`!parentEntity`) →
  return 1; else `parentSlot == 2 → 0`; `parentSlot == 3 → 0` *only if* `dword_A890C8`
  (third-person/vehicle camera, §5.39); `parentSlot == 5 → 0`.
- In both branches `parentSlot == 1` (passenger) is **not** in the hide set → the personal
  weapon stays visible (subject to the normal ammo checks).

### 13.4 Seat-type source and assignment
`parentSlot` is the seat-type code, classified from the vehicle/emplacement model's
**user-point name prefix** [orig: `Entity_GetBoneSlotType @ 0x434ed0`] and stored on the rider
at attach time [orig: `Entity_ProcessVehicleAttach @ 0x435aa0`; plumbing
`Entity_RequestVehicleAttach @ 0x4364a0` → `Entity_AttachToVehicleSlot @ 0x4946d0` /
`Entity_AttachToUseGunSlot @ 0x546b80`]. Same classifier as §9.1/§11:

| user-point prefix | `parentSlot` | role | held weapon |
|---|---|---|---|
| `sitex` | 1 | passenger | **shown** (can fire) |
| `ctrlx` | 2 | control / weapon-station | hidden |
| `UseGun` | 3 | gunner / **fixed emplacement** | hidden |
| `drvrx` | 5 | driver | hidden |

Fixed emplacements (mounted MGs) are manned through a `UseGun` slot, so they take the gunner
case (3) and hide the personal weapon. (Mounted *pose* selection — emplaced 67–75, `sit_N`,
driver-lean — is the separate system of §4.15.)

### 13.5 Note for the OpenNova port
The requester's premise listed **passenger** among the hidden cases; the binary disagrees —
`sitex` passengers keep the personal weapon visible and may fire (open boats/trucks). Only
control (2), gunner (3), and driver (5) hide it. When third-person held-weapon rendering is
implemented, gate the weapon node's visibility on this predicate, reusing the existing seat
taxonomy (`godot/engine/world/mission_seat_diagnostics.gd` SEAT_PASSENGER/CONTROLLER/GUNNER/
DRIVER) and the `mount_type` already exported through `nova_simulation.cpp`. The exact
`Entity_CanFireWeapon` predicate (incl. the `Flags & 2` weapon-disabled gate and the local
gunner third-person condition) is the faithful rule. **Open follow-ups:** IDB hygiene (rename
`pad_2b0` → `heldWeaponAdmIndex`; comment `Entity_CanFireWeapon` as the weapon-visibility
gate) is proposed but unapplied (shared IDB state).
