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
| `AiSystem::tick_infantry` (+think/select/slide) | `Entity_UpdateInfantryAI` | 0x4b9910 | structural translation, per-mechanic dump cites in libs/world/src/infantry.cpp; constants byte-pinned (turn clamp 69273360, gravity 416/−32768, slide 2048 @ threshold 0x22222200, gates 30°/45°, jog windows 139264/270336/73728) | **matching** w/ D-INF-1..5 (enumerated below) |
| `kInfantryAnimNames/Flags` | `off_8135F0` / `dword_8139E8` | 0x8135F0/0x8139E8 | all 200 entries index-verified vs IDB | **matching** |
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
126 = hold/guard (10 u radius), 127 = follow local player (radius `max(slot[16], 4u)`),
123/124/125 = move-order family. `slot[38] @+152` = node index. `slot[35]` = active flag,
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
- **Per-state flag table `dword_8139E8`** (≥190 dwords; extracted, embed in port as generated table):
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
  `off_8135F0` names through `.adm` (libs/adm) — the exact original data path.
- Tables (`off_8135F0` names, `dword_8139E8` flags) land as generated C++ tables (wac-style).

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
8. `dword_C6EAE4` (fall-damage gravity scale) value/source; 1024-entry sin (`outMillis` local-misname)
   and cos (`off_849934`) table extraction for exactness.
9. Death move-step movers `0x461c30`/`0x461cb0` (ids 0/2) — define + decode.
10. Vehicle-SM per-tick invocation site (event-callback path is confirmed; the tick-mode caller for
    vehicles not yet pinned — likely inside `Entity_UpdateVehiclePhysics`).
11. The 62-frame divider + spawn-event `f[3] = sub_4E7000()[17]` re-checks (carried from the plan).
12. **Command-path bodies decoded** (dump 1545–2330, rides the command-source phase / D-INF-2):
    move-to-entity orders resolve the target by net-id across pools 0–3; vehicle boarding is a
    staged bone walk (count free `E1..E8` entry bones, claim a slot via entity+866 cross-checked
    against other soldiers' claims, then approach `E`→`S`/`G`→`H` stages via entity+865 with stop
    147 / guard 140 alignment and eighth-step position pulls) ending in `Entity_FindBestSeatSlot`
    + `Entity_AttachToVehicleSeat`; `UseGun` bone = emplacement manning (radius 1u/3u); net-ids
    11000/12000/12001 get hardcoded escort/approach offsets (heading ±90° at 2–4u).
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
    (entity+24 of the vehicle, ±71582784) and speed (+668).
16. **Playhead rate**: channel time is normalized [0,1) advanced by a per-clip dt seeded at
    `AnimChannel_InitFromParams` (the literal 4096 param) — the exact dt derivation (sim-tick →
    clip-frame rate, blend-window advance) is unpinned; the IRootMotionSource seam owns phase
    policy, so this only matters for byte-exact playback timing.

## 5. Per-system equivalence verdict (2026-06-10, infantry port complete)

- **Infantry ground locomotion** (`Entity_UpdateInfantryAI @ 0x4b9910` → `AiSystem::tick_infantry`,
  libs/world/src/infantry.cpp): **MATCHING**, with five named, cited deviations —
  - **D-INF-1** no blend windows (clip switches reset phase; the original blends 10/15 ticks,
    root motion included) — rides the skeletal/blend pass.
  - **D-INF-2** command channels 123–127 decoded but not driven (move-to-entity bodies incl. the
    staged vehicle boarding are documented in §4.12) — rides the command-source phase.
  - **D-INF-3** ground/water resolver modeled as terrain-clamp + landing (platforms/water/capsule
    pending, `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0`); horizontal slide velocity
    zeroes on contact; the airborne anim overlay waits on the entity+36 flags.
  - **D-INF-4** computed sin/cos tables (trunc(f(idx)·2^22)) for the runtime-built originals.
  - **D-INF-5** idle look-at system + its spotting side effects (§4.13) — rides the combat pass.
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
