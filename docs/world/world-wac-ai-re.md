# World / WAC / AI runtime — RE record and equivalence verdicts

Binary: `Jointops.exe` (retail JO:CA, Steam), IDB `Jointops.exe.kong.i64`, imagebase 0x400000.
Sessions: 2026-06-07 (WAC ISA + AI P1/P2, prior), 2026-06-08 (foundation), **2026-06-10 (entity-motor architecture grill — this record)**, 2026-07-08 (§14 aim overlay), 2026-07-09 (§14.8 weapon-channel producer), 2026-07-16 (§16 ground-AI combat chain — targeting feed + fire convergence), 2026-07-20 (§26 allegiance, damage response, and mounted-weapon parity).
Scope: `engine/runtime/world` (entity registry, var store, AI), the WAC pipeline (`engine/formats/wac` lexer/parser/command table + `engine/runtime/wac` compiler/VM), the mission-runtime bridge, and the
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
Our `engine/runtime/world` AI port (24-row SM @ 0x815238, states 16–18, `AI_BeginUpdate @ 0x457b40`,
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

## 2. Correspondence map (engine/runtime/world ↔ Jointops.exe)

| reimpl (engine/runtime/world) | original | addr | evidence | verdict |
|---|---|---|---|---|
| `AiSystem::begin_update` | `AI_BeginUpdate` | 0x457b40 | byte-walk this session (budget 0x1F0, pend=8/brain[6], +brain[7]) | **matching** (as vehicle-layer) |
| `AiSystem::update_waypoint_movement` | `AI_UpdateWaypointMovement` | 0x457bd0 | prior grill + re-read | **matching** (vehicle mover) |
| `ai_waypoint_update_target` | `AIWaypoint_UpdateTarget` | 0x457380 | prior grill; also called by aircraft motor @0x490310 | **matching** |
| `process_infantry_state_machine` | SM dispatcher (vehicle classes) | 0x4581b0 | event-callback fn1 rows | **matching**; name is now known to be historical — it is the AI-VEHICLE SM |
| `h_ground_followwp_tick` etc. | state 16/17/18 rows | 0x815338+ | prior grill (raw bytes) | **matching** |
| P2 combat/targeting | `AI_FindBestTargetB` etc. | 0x466f60+ | prior adversarial grill; candidate FEED + LOS + refcount witnessed 2026-07-16 (§16.2/16.3) | **matching** (scoring core; the feed port is D-AI-1, the SM combat states D-AI-2/3) |
| `AiSystem::apply_locomotion` | — (model) | — | vehicle-layer kinematic model only (organics no longer pass through it); HELO/vehicle physics remain visible `not_yet_ported` stubs | tracked model (vehicle slice) |
| organics → `tick_infantry` routing | `g_EntityClassPhysicsTable` row "org1" | 0x82abc8 → 0x4b9910 | promote marks `inf.active`; `AiSystem::tick` branches before the SM | **matching** |
| `AiSystem::tick_infantry` (+think/select/slide) | `Entity_UpdateInfantryAI` | 0x4b9910 | structural translation, per-mechanic dump cites in engine/runtime/world/infantry.cpp; constants byte-pinned (turn clamp 69273360, gravity 416/−32768, slide 2048 @ threshold 0x22222200, gates 30°/45°, jog windows 139264/270336/73728) | **matching** w/ D-INF-1..10 (enumerated below) |
| `kInfantryAnimNames/Flags` | `g_animStateNameTable` (ex `off_8135F0`) / `g_animStateFlagsTable` | 0x8135F0/0x8139E8 | body entries index-verified vs IDB; full table is 253 entries — body 0–239 + `wpn_*` 240–251 + `EOF` 252 (§14.8.2) | **matching** (body slice) |
| promote `init_infantry` + marker fill | `Entity_SpawnFromBMSRecord` | 0x40e9f0 | slot map (speeds %, accuracy, engagement, timers ×62, alert, route) + marker radius/facing/movetimer | **matching** |
| `InfantryRootMotion` (engine binding) | `AnimMap_UpdateEntity` out-transform | 0x40b5f0 (+0x40b230, 0x40b140) | scales pinned by disasm + real-clip grill (tests/anim/root_motion_test.cpp: I_walkf 1.82 u/s, E_RUNF 5.28 u/s) | **matching** (playhead dt = open item 16) |
| `AiSystem::apply_ground_clamp` | per-motor ground sampling | 0x457230 + motors | 5-tap port matches; the infantry motor resamples on the faithful every-8 cadence (cache `inf.ground_cache`); the vehicle path still clamps per tick | matching-core (infantry aligned; vehicle cadence with its slice) |
| WAC pipeline (`engine/formats/wac` front end + `engine/runtime/wac` compiler/VM) | `Script_Compile`/`WacScript_ExecuteBytecode` | 0x4f31f0/0x4f58b0 | oracle-extracted ISA + corpus | **matching** (165-cmd table, 0x7A7A7A7A) |
| `player_toggle_vehicle_mount` | `Entity_ToggleVehicleMount` (+ `Entity_TryEnterNearestVehicle`) | 0x436950 / 0x4368c0 | §23.1 witness; ctest `vehicle_mount` | **matching** w/ D-AI-11 (weapon gate at the sim binding) |
| `find_nearest_free_seat` | `Entity_FindNearestSeatOrArmory` (both legs) | 0x435d50 | §23.1 — 4.0 u gate, score `horiz + d3/512`, enemy-occupant reject, LOS-last; the armory leg (searchMode 1, seatType 4) landed with the attach labels (hud-re.md) | **matching** w/ D-AI-11 a/b |
| `find_best_vehicle_seat` | `Entity_FindBestSeatSlot` | 0x4351f0 | §23.1 exact root/ground-child walk and weights (ctrl 0x2000 < gun 0x20000 < root sitex 0x200000 < child sitex 0x2000000) | **matching** (canonical world operation; D-AI-11 g closed) |
| `presnap_vehicle_attach_heading` (both attach entry points) | `Entity_RequestVehicleAttach` | 0x4364a0 | §23.1 (pre-relationship snap; UseGun yaw = veh.Yaw − stored offset); ctest `vehicle_mount` | **matching for UseGun; matching-core with §9.2.5 for moving generic seats** |
| `Simulation::sync_local_mounted_input_heading` | no separate retail seam (one input-owned entity Yaw) | n/a | §23.1/§26.5; asset-backed B50 GUT | **matching adapter** |
| live UseGun root-position feedback (host parent pose → sim occupant) | `Entity_AttachToBoneAndUpdateTransform` | 0x5463d0 (player call 0x4b63c7; AI call 0x4bec23) | §23.5/§26.5a; asset-gated 00TRc E50triB GUT | **matching for UseGun root position** (joiner C2S 0x26/0x27 + requester-local 0x0A relationship confirmation landed; generic seats and the full matrix basis remain open) |
| joiner S2C 0x13/0x26 death fold (`ClientReplicaPipeline::apply_entity_death` → `destruction_notify_item_damage(…, 4)`) | `NapiNPClientMsg_EntityDeath` / `Entity_KillBySlotId` | 0x42eb50 / 0x42bce0 | §24.3 client fold; ctest `npruntime_entity_lifecycle_net` + `destruction` net-kill case | **matching** for destructible victims (D-NET-208; organic 0x13 death-anim + local-player camera legs deferred) |
| `EntityCommands::local_player_attached_to_ssn` | `Entity_IsLocalPlayerSeatedOnSsn` (renamed) | 0x4f10d0 | §23.2; ctest `vehicle_mount` | **matching** |
| `EntityCommands::local_player_standing_on_ssn` | `Entity_IsLocalPlayerStandingOnSsn` (renamed) | 0x4f1260 | §23.2 | **matching** (persistence nuance D-AI-11 h) |
| `EntityCommands::local_player_driving_ssn` | `Entity_IsLocalPlayerDrivingSsn` (renamed) | 0x4f1150 | §23.2 | **matching** |
| `EntityCommands::local_player_on_gun_of_ssn` | `Entity_IsLocalPlayerOnGunOfSsn` (renamed) | 0x4f11e0 | §23.2 | **matching** |
| `AiSystem::vehicle_ai_drive` + motor AI branch | `Entity_UpdateVehiclePhysics` parked/AI-driver legs | 0x48c002 / 0x48bc12 | §23.3 — state 22 stamp, 22→16 hand-back, turn budget, 0.75x damps, steer `Yaw+Δ+Δ/8`, the pool-1 avoid brake (heading-aware ellipse + the 0.25-0.75 id/frame damp @ 0x48bd8f); ctest `vehicle_mount` | **matching** core (boarders-wait/handbrake/aim-lock/minAI = D-NET-161) |
| `spawn_player_entity` group stamp | the deploy leg (`commandGroup = 1`) | 0x519fd0 | §23.3; ctest `vehicle_mount` | **matching** |

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
- The marker+328/+16 -> BMS record field mapping is CLOSED: the spawn map is ported into
  promote (see §3.2; `Entity_SpawnFromBMSRecord @ 0x40e9f0`).

### 3.3 Heading pipeline (per tick)
- **Body** `entity[35]`: `+= clamp((target(entity[106]) − body + 2) >> 2, ±69273360)` (~5.8°/tick);
  render yaw `entity[4]` moves with it.
- Aiming (`byte entity+864`): render yaw chases aim heading `entity[187]` quarter-step,
  clamp [−31457280, +503316480]; pitch `entity[5]` chases `entity[180]` likewise.
  Non-aim: eighth-step, clamp ±14680064 (cap 234881024). Per-tick sanity clamp vs prev: ±0x40000000.
- Legs (**CORRECTED 2026-07-08** — previously misread as "torso chases head-look";
  **RE-CORRECTED at byte level 2026-07-16, session 8 — see §22**):
  `entity[181]/[182]` (+0x2d4/+0x2d8; IDB `GamePlayerEntity.legChaseYawR/L`, renamed 2026-07-16
  ex the `torsoYaw`/`torsoPitch` misnomers) are the
  **right/left leg-chain chase yaws**; each chases its re-plant target `entity[185]/[186]`
  (+0x2e4/+0x2e8; IDB `legReplantYawR/L`, renamed ex `headLookYaw`/`headLookPitch`) quarter-step (1/16 when
  def+84&0x200), rate clamp ±83886080, twist limit ±0x20000000 (45°) from body
  [orig: `@ 0x4bea11-0x4beb12`]. Re-plant (idle bodies): the target VALUE is the
  **midpoint of (bodyHeading, targetHeading)** [orig: `@ 0x4be969-0x4be975`] — not the body
  itself (the 2026-06 dump reading) — re-targeted only when |Δ vs the current target| >
  59652320 (~5°) and (|Δ| > 357913920 (~30°) or the leg's 64-tick window hits), the LEFT
  window running 32 ticks behind the right (`(tick−32)&0x3F` vs `tick&0x3F`
  [orig: `@ 0x4be991` / `@ 0x4be9bb`]). A MOVING state (flag-table bit 0) or a
  def+84&0x200 body takes the walk path instead: legYawR half-snaps toward the target,
  legYawL += (target − newLegYawR)>>2, both targets = target — the alternating foot
  shuffle [orig: `@ 0x4be9d4-0x4bea0b`]; carried (Flags 0x40) snaps both legs to the body
  [orig: `@ 0x4beb0c`]. The feet shuffle around to catch up once the body has twisted far
  enough. **The org2 player body runs a DIFFERENT model** — no body chase at all: its legs
  chase the render yaw and `bodyHeading` is written as their midpoint (§22, D-INF-12 closure).
  Proof of the leg reading: the render bone-overlay switch consumes [181]/[182] as the **yaw** of the
  R/L thigh/calf/foot bone chains (with bodyPitch), §14 `[orig: Entity_BuildBoneTransformMatrices @ 0x4b1290]`.
  `entity[183]` (+0x2dc `torsoRoll`) genuinely is a torso roll (recoil/flinch chase, §4.14). Head-look
  proper is the separate `headLookTarget` (+0x344) system (§4.13) — that pointer is ALL of it.
  **`+0x36c` is NOT part of it (measured 2026-07-27):** it is a PITCH-OFFSET accumulator, renamed
  `pitchKickAccum` in the IDB (ex the `headLookDecay` misnomer) and `pitch_kick_accum` across the
  port. Its witnessed consumer is the §14 render pass, which temporarily sets the entity's own
  Pitch to `savedPitch + pitchKickAccum + 2*pitchBlend` while building the overlay and
  held-weapon attachment matrices `[orig: load @ 0x4b1bd4, store @ 0x4b1bf5; sibling reads
  @ 0x4b1c1b and @ 0x4b1d96]`. It is driven by the arms-dip window `+0x371` (§14.8.5), not by any
  look-at target. `pitchBlend` (+0x380) is the second term of that same sum, likewise not a
  head-look term.

### 3.4 Animation state machine (the locomotion driver)
- State id `entity[175]` (prev `entity[178]`); **state→name table `g_animStateNameTable @ 0x8135F0`**
  (**252** entries — the earlier 200-entry reading stopped short; `AnimMap_FindSlotByName @ 0x40cfa0`
  scans 252, §19.1 — names ARE the `.adm` keys: `anim_<name>`): 0 reset, 1–8 walk 8-dir, 9/10 run variants, 11–18 crouch-walk,
  19–26 prone-walk, 30/31 jump_start/loop, 32–35 climb, 36–40 swim, 43/44/49 idles, 45/48
  crouch/prone idles, 47 parachute, 65 reload, 67–75 emplaced, 76–110 sit_*, 111–114 burn,
  115–124 emotes, 125–135 scripted idles, 137–139 dragger/draggee, 140–144 guard, 145/146 wounded,
  147 stop, 148 jog, 149 run_forward, 151/152 post/pre_attack, 153 out_of_ground, 154 swim_attack,
  155–158 attack, 159–162 grenade throws, 163–166 cover, 167 run_attack, 168 run_away,
  **169–172 stance transitions** (run2crouch/runl2crouch/runr2crouch/run2prone),
  173 death_fire, 174 death_pungi, 175 death_drown, **176–239 death matrix** (grenade ×4, then
  bullet × FIFTEEN bone groups — hip/torso/head/R-L shoulder/arm/hand/thigh/calf/foot × F/R/B/L)
  — picked by `Entity_ComputeAnimSlotIndex(entity, boneSection, relativeDirection, cause)`,
  quadrant `(heading − atan2BAM(roundVel) − 0x60000000) >> 30`; **240–251 the `wpn_*` FP viewmodel
  states**. Full decode + the bone→group table: §19.1/§19.2.
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
  per-frame events): bit0/bit1 = footstep L/R (surface-typed sound-profile slots
  17–23, §17.4b — NPC body odd ticks, player body even ticks), 0x20..0x400 =
  the SSAudio foley sounds (24–29), bit2 = attachment event.
- **Primary-channel port status (2026-07-29, D-INF-1 primary leg: FIXED):**
  `InfantryState` now retains the outgoing channel and its playhead independently from
  the requested target. A retarget during A→B therefore keeps A as the stable primary
  and replaces only B with C, exactly as `AnimMap_UpdateEntity @ 0x40b5f0` does. The
  target weight is accumulated in float32 by `0.1f` or `1.0f/15.0f` before each sample;
  the five raw `.bad` numeric lanes are blended before fixed-point conversion, while
  the trigger word is copied only from the target channel. Missing semantic state keys
  resolve to the registered RESET channel, matching the registration backfill
  `[orig: AnimMap_RegisterEntity @ 0x40bb60, backfill @ 0x40bc24 / 0x40bd2e]`.
  The same source/target playheads and weight now drive the body pose before the
  weapon-mask and aim-overlay composition
  `[orig: AnimChannel_BlendTwoChannels @ 0x410740]`, including authoritative organic
  collision. The **secondary weapon channel's own state-change blend remains open**
  under D-INF-1; its mask composition is live, but its transitions still switch
  immediately.

### 3.5 Integration (per tick, the motor core)
1. Rotate the already blended primary-channel root delta by heading:
   `sin/cos(entity[4]) · 2^22` (FPU, dbl_7C3608 = π/2^31,
   dbl_7C3600 = 4194304.0); `fwd' = fwd·cos − strafe·sin; strafe' = fwd·sin + strafe·cos` (>>22).
   State 31 (jump_loop) forces fwd = 1024.
2. **`pos.xy += rotated_delta + vel(entity[38..39])`; `pos.z += vertical_delta`.**
   Flag 0x8000 (drowning) zeroes vertical; 0x100000 (CL ladder contact) zeroes
   horizontal root motion while the body is aligned to the ladder.
   On a death edge, the death callback stores the replacement target only **after**
   the already-playing channel tuple has advanced and its events have been consumed
   `[orig: death caller tail @ 0x4b9d55]`. The port now stages death in that same
   order: the edge tick keeps the current A or A→B sample, exposes death at phase zero
   and weight zero at the end of the tick, then the next tick consumes the first
   A→death blend. Consequently `anim_slot[19]` reconciles consecutive **blended**
   capsule-bottom samples instead of subtracting the first death bottom from the last
   idle bottom—the cross-clip delta that caused the visible position snap. The same
   ordering is used for authoritative NPC/local-player bodies and wire-owned remote
   player bodies.
3. Every tick (the "every 2 ticks" first reading corrected by D-INF-10; both legs
   byte-witnessed 2026-07-16 §22): **gravity `vel_z(entity[40]) −= 416`** skipped while
   `Flags & 0x108000` (ladder/drowning) [orig: `@ 0x4bf7b8`] (terminal −32768; the
   `Flags 0x80` org1 CLIMB mode replaces gravity with the 1/16-step chase to the
   AI move-target Z `entity[193]`/+0x304, cap 0x4000, floor −16384 — §30);
   `pos.z += 2·vel_z`;
   `movement collision resolver @ 0x4b2bd0 (entity, root_drop, height)`:
   ≤0 ⇒ ground push-out (`pos.z -= ret`), vel_z = 0, water-exit sounds (15/16),
   **fall damage** when `vel_z ≤ −1057·dword_C6EAE4`: `health −= (excess)>>4`
   (`dword_C6EAE4` is the writable named value `fallmps`, seeded 13 by
   `WacScript_FreeAll @ 0x4f638b` at every mission load; ported 2026-08-28 as
   `World::wac_values.fallmps`, authority-gated and skipping Indestructible bodies);
   >61440 ⇒ set swim (flag 0x2000, states 47/31 hmm 47=tread/31 per anim availability).
   The 81920·dir pool-0 probe → state 32 sits in the ON-LADDER block, not a
   water-edge climb-out: it is the ladder CONGESTION hold (someone climbing
   ahead of you) — corrected 2026-08-15, witness §30.
4. Every 8 ticks: **the slope pass** — the CONFORM SELECTOR first [orig: `@0x4ba10f`]:
   `def+84 & 0x200 || g_animStateFlagsTable[state] & 2 || (Flags&2 && !(Flags & 0x10A000))`
   (the flag-2 states = the low-to-ground family: prone crawls 19–26 `0x603`, rolls 41/42
   `0x285`, prone idle 48 `0x202`, draggers 137–139; the dead leg excludes swim/parachute).
   Non-conforming bodies take the DECAY [orig: `@0x4ba133`]: `bodyPitch(+0x90)` and
   `Roll(+0x18)` ease to level 1/16-step — a live standing/crouched soldier neither
   slope-leans nor slope-slides (the slide impulse only exists inside the conform branch;
   dead+airborne diverts to the corpse tumble `@0x4ba0b2` instead). Conforming: 4 probes
   `Entity_RaycastGroundHeight(entity, ±dir·22528>>22 …, 0x4000, 0x20000)` ahead/behind (pitch slope ×2^14)
   and left/right at quarter offset (roll slope ×2^16), clamp ±656175520; if |slope| >
   572662272: **slide** `vel ∓= dir·2^11>>22`; then `bodyPitch(+0x90)` and `Roll(+0x18)`
   chase the slopes at eighth-step [orig: `@0x4ba320`]; a dead NPC also aims along the
   slope (`+0x2D0 = pitchSlope`, `+0x2EC = targetHeading(+0x1A8)`, byte `+0x360 = 0`
   [orig: `@0x4ba301-0x4ba319`]). The **org2 player leg** (`Entity_UpdateInfantryPlayerBody`) carries the
   SAME selector every tick [orig: `@0x4b6d95`] with the decay every tick [orig: `@0x4b6dbd`],
   but probes/chases only every 2nd tick (hold between) [orig: `test tick,1 @0x4b6de4`] and
   differs in kind: slopes are TRUE angles `ftol(atan2(dh, separation)·2^32/2π)`
   (separations 45056 fore-aft / 11264 lateral [orig: `dbl_7C9BE8/dbl_7C9BE0·dbl_7C19D8
   @0x4b6e68`]), the slide threshold is 60° alive / 48° dead [orig: `@0x4b6ee5`] with the
   impulse `dir·2^9>>22` [orig: `@0x4b6f01`], the chase is QUARTER-step [orig: `@0x4b6fc1`]
   with the Roll write skipped while a combat roll 41/42 plays [orig: `@0x4b6fd7`], and a
   corpse additionally tips its LOOK pitch `Pitch(+0x14)` eighth-step [orig: `@0x4b6fa9`].
   This selector is what keeps the standing FP camera LEVEL on hillsides: Roll decays,
   `torsoRoll(+0x2DC)` chases Roll (§ camera), `fp_roll = torsoRoll + lean/4 @0x437fe6`
   stays 0 (D-INF-19 fix log). Port: `AiSystem::infantry_slope_pass`
   (`engine/runtime/world/infantry.cpp`), pinned by the `test_slope_*` cases in
   `tests/world/infantry_test.cpp`.
5. Inter-entity separation + 8-direction avoidance raycasts + combat maneuver modes
   (1/2/5/7/8/12) — **RE'd to address level, detail pass pending** (dump lines ~1080–1290, 3000–4550).
   The swim/float details are done: §29.1 (both motors, byte-witnessed). Avoidance
   (negative result, 2026-06-10): patrol walking has **no peer/obstacle steering** in
   `0x4b9910`. The probe fans are (a) a peer-cohesion *facing* average for combat idles
   163/44/126 only (same-team peers ≤ 3u + a 9-direction
   `Entity_CheckLineOfSightTerrainAndEntities` clearance fan biasing `entity[106]`; dump
   3734–4082), and (b) vehicle-entry approach probes inside the command path. Entity
   separation = the resolver's push-out (§4 item 5), not AI steering.

### 3.6 Death/corpse/respawn (health ≤ 0 edge)
Drop/detach, corpse timer `entity[82] = def+2192` (−61 in the silent-cleanup variant), death anim
via `Entity_ComputeAnimSlotIndex`, drowning ⇒ state 175; corpse never despawns while the local
player can see it (`Physics_RaycastTerrainAndSectors` watch-check, retry 62); respawn restores
`entity[198..205]` snapshot; death-by-fall splash effect via def+1042. **Superseded at porting
precision by §19** (the edge decode, the corpse block, `deathtime`/`particledeath`/`LeaveCorpse`,
and the vehicle rows 21/23) — ported 2026-07-16.

### 3.7 Port architecture (decided)
- New infantry system in `engine/runtime/world` beside the vehicle SM; promote routes **organics → infantry**,
  vehicle classes stay on the SM (correct per §1).
- **Root-motion source injected** (mirrors the terrain `TerrainHeightField` injection):
  `engine/runtime/world` defines the per-state clip-velocity interface; the real impl evaluates `.bad`
  root tracks via `engine/runtime/anim` and lives with the binding (Simulation) + an env-gated
  real-assets ctest; unit tests inject synthetic velocities. State→clip resolution uses the
  `off_8135F0` names through `.adm` (engine/runtime/anim) — the exact original data path.
- Tables (`off_8135F0` names, `g_animStateFlagsTable` flags) land as generated C++ tables (wac-style).
- Anim-state selection + commit rules per moveMode are ported (walk/run/jog/turn/wounded +
  lock/emote queueing; dump lines 3000–3700). The 147-availability rule is a post-commit
  force `147 → 43 idle` (dump 4084), **not** a gait fallback — the port is aligned to that.

## 4. Open items (tracked, addressed)
3. **Swim**: no swim locomotion in the unread regions beyond the known state overlays
   (36/37/154 selection + wash 27–29); swimming physics lives in the resolver (item 5,
   water flags at entity+36). The airborne overlay decoded: flags 0x2000/0x20 set + 0x40
   clear → force parachute 47, fallback jump_loop 31 (dump 3679–3692).
4. `Entity_RaycastGroundHeight @ 0x4142c0` (0x59 bytes — ground probe at offset; exact param semantics 0x4000/0x20000).
5. `movement collision resolver @ 0x4b2bd0` internals (0x11e3 — ground/water/BVOL
   resolver). Type-4 bookkeeping was initially decoded under the incorrect platform reading;
   the manual pins CL as ladder, and the entry/chase/exit semantics landed with the
   D-COL-5 port 2026-08-15 (§30).
7. Perception scan fn (called at dump line 2377, kong-misnamed `Entity_SpawnProjectile`) + LOS `Entity_CheckLineOfSightTerrainAndEntities`.
9. Death move-step movers `0x461c30`/`0x461cb0` (ids 0/2) — define + decode.
10. Vehicle-SM per-tick invocation site (event-callback path is confirmed; the tick-mode caller for
    vehicles not yet pinned — likely inside `Entity_UpdateVehiclePhysics`).
11. The 62-frame divider + spawn-event `f[3] = Projectile_GetHitRecord()[17]` re-checks (carried from the plan).
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
    entity[4]; entity[44]/[219] decay. Torso roll entity[183] (`@0x4b5cff-0x4b5d6d`,
    corrected 2026-07-13 — the earlier "prone 48 halves the chase" reading was wrong):
    anim 48 idle_prone → decay toward level `t −= (t+8)>>4` `@0x4b5d0a`; anims 41/42 →
    skipped here but RAMPED −/+0x4000000 (5.625°) per tick at the separate site
    `@0x4b700e/@0x4b701d` (the FP barrel-roll view); else chase entity[6]
    `t += (roll−t+8)>>4` with the LAG clamped to roll ±238609280 (20°) `@0x4b5d2a..6d`
    — the clamp snaps the wrapped post-roll value back once the clip ends. Ported:
    `AiSystem::infantry_torso_roll_tick`.
15. **Mounted pose states** (dump 4464–4539, mount/B2 pass): emplaced gunners force 67–75
    (`emplaced_N` by the target item definition's `phrase_set @+0x86c`); seat passengers pose from the seat bone and take
    `sit_N` = `atol(bone_name_digits) + 76`; sit_24 (=100) drivers lean 107–110 by steering
    (entity+24 of the vehicle, ±71582784) and speed (+668). Port status: `UseGun`/gunner seats
    select `anim_emplaced` 67 plus the production-extracted config variant when that clip exists;
    non-gunner seats use the parsed `sitexNN`/`ctrlxNN`/`drvrxNN` pose index (`anim_sit_N`).
    Config presence is explicit, so unknown does not alias authored config 0. UseGun root position
    now follows its live control-posed parent userpoint. Remaining gaps are the driver-lean 107–110
    overlay and true per-tick bone follow for non-UseGun `sitex`/`ctrlx`/`drvrx` seats (see §9.2).
16. **Playhead rate**: channel time is normalized [0,1) advanced by a per-clip dt seeded at
    `AnimChannel_InitFromParams` (the literal 4096 param) — the exact dt derivation (sim-tick →
    clip-frame rate, blend-window advance) is unpinned; the IRootMotionSource seam owns phase
    policy, so this only matters for byte-exact playback timing.

## 5. Per-system equivalence verdict (2026-06-10, infantry port complete)

- **Round-outcome loop** (§20, 2026-07-16 session 6; multiplayer completion
  2026-08-22: `Server_ProcessRoundEnd @ 0x5164f0` →
  `World::process_round_end`; `Server_CheckWinConditions @0x51ad40` →
  `world::Match`; WAC win/lose + the named-value builtins; the BMS win actions;
  the SP auto-lose; kill tallies; the SP end presentation): **MATCHING at the
  shared outcome core**. All retail multiplayer game-type arms and co-op's
  script-owned result now converge through the same latch; D-AI-10 retains only
  its explicitly listed SP scoring/presentation residuals. Evidence:
  `match`, `wac_behavior` (outcome builtins + the verbatim 04TR else-if
  block), `npruntime_round_end`, `event_runtime_bms` (BlueWin + zone-ref resolution),
  and the `lose_flow_04tr` ctest (formerly `round_outcome_probe.gd`) 04TR PASS (real-round teammate kill →
  bluekills → Lose(1) → the retail KILLEDBLUE string → round end winner 2 → the
  FAILED screen → ESC to menu).

- **Infantry ground locomotion** (`Entity_UpdateInfantryAI @ 0x4b9910` → `AiSystem::tick_infantry`,
  engine/runtime/world/infantry.cpp): **MATCHING**, with the named, cited deviations —
  - **D-INF-1 — PRIMARY FIXED 2026-07-29; SECONDARY FIXED 2026-08-17.** Primary locomotion/body
    switches now retain independent source/target playheads, accumulate the exact
    float32 10/15-tick weight, blend raw root/capsule lanes before conversion, carry
    only target events, and pose render plus authoritative collision from the same
    blend `[orig: AnimMap_UpdateEntity @ 0x40b5f0;
    AnimChannel_BlendTwoChannels @ 0x410740]`. The secondary weapon channel re-inits
    through the same shared body since 2026-08-17 (`InfantryState::begin_weapon_transition`
    mirrors `begin_body_transition`; §14.8 blend window).
  - **D-INF-2** command channels 123–127 (`waypoint_id`; MED "Goto SSN/Group/Player", §11) are
    partially driven. Commands 123/124/125 authored spawn attachment now resolve `wp_number` as the
    target SSN, apply the IDA-confirmed seat filter (123 passenger-only, 124 rejects `ctrlx`, 125 any),
    mount occupants already authored near a binding-provided seat, render `UseGun`/gunner seats with
    `anim_emplaced` plus available variants (00TRa class), and carry a UseGun occupant at the live
    control-posed parent userpoint. Remaining gaps: staged E/S/G/H walk-to-seat, 126/127,
    child-seat traversal, true bone-transform follow for non-UseGun seats, and driver-lean poses.
  - **D-INF-3** movement resolver now includes the horizontal CB capsule, object/terrain
    ground probes, triggers, and landing. Water/swim transitions remain; CL climb locomotion
    LANDED with the **D-COL-5** port 2026-08-15 (§30). The vertical capsule-bottom settle is **D-INF-6**
    (`movement collision resolver @ 0x4b2bd0`); horizontal slide velocity zeroes on contact.
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
    `movement collision resolver @0x4b2bd0` resettles `entity[3] = entityRadius +
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
    `pos[2] = ground_cache + frame.capsule_bottom` (`engine/runtime/world/infantry.cpp`). The shared
    `ai_->root_motion` is loaded from `E_STAND.adm` at mission load (`mission_presentation.gd`), so every
    motor-driven soldier resolves a real standing `capsule_bottom`. `ground_stand_offset` (0x50000)
    is retained only for the vehicle/SM `apply_ground_clamp` path. Guarded by the capsule-settle case
    in `tests/world/infantry_test.cpp`.
  - (No D-INF-7 was ever minted — the list keeps session ids, so the numbering jumps
    6 → 8, like the documented D-MUS-1/4/8 gaps.)
  - **D-INF-8** idle root-motion integration — **OVERTURNED 2026-06-21**. The refuted
    session reading gated the root-delta position integration on the `g_animStateFlagsTable`
    bit0 flag, holding idle bodies rigid — which slid the FEET under the swaying skeletal FK
    ("idle skating"). Witness: that flag gates the anim COMMIT rules (`@0x4bd85c`) and the idle
    LOOK-AT scan (`@0x4be95f`), NOT position integration; `Entity_UpdateInfantryAI @0x4b9910`
    integrates the root delta for ALL states, and idle clips author a small mean-~0 root
    velocity (the bored weight-shift) that keeps the feet planted by swaying the centre of
    mass under the skeleton. The reimpl integrates unconditionally; the overturn note lives
    beside the code (`engine/runtime/world/infantry.cpp`, the `overturns D-INF-8` marker).
  - **D-INF-9** player horizontal-slide decay. The player shares the NPC's slide-velocity damp, but
    the original splits it by the grounded flag: a GROUNDED player decays `inf.vel[0]/[1]` by
    `(63·v)>>6` with NO deadzone [orig: `Entity_UpdateInfantryPlayerBody @0x4b7949` — `shl 6 / sub /
    sar 6` on `entity+0x98/0x9C`, selected by the `entity+0x24 & 0x2000` grounded flag `@0x4b78ab`];
    an AIRBORNE player uses the same `(7v+4)>>3` + `abs<=8→0` deadzone as the NPC [orig: `@0x4b7982`].
    The two formulas are mutually exclusive, not sequential. A prior pass gated slide damping behind
    `!is_local_player` (and dropped the grounded velocity zero), so the player's slope-slide impulse
    drifted forever; FIXED in `tick_infantry` (`engine/runtime/world/infantry.cpp`), guarded by the
    player-slide case in `tests/world/infantry_test.cpp`. (Our `inf.airborne` here reads last tick's
    value — the vertical resolve updates it after — a negligible 1-tick lag vs the original reading
    the flag set in the same physics pass.)
  - **D-INF-10** per-tick gravity, asymmetric by motor — **CLOSED for both legs 2026-07-16
    (§22)**. Neither infantry mover gates the vertical step on tick parity. The NPC (org1)
    falls `vel_z -= 416` EVERY tick then `pos.z += 2·vel_z` [orig:
    `Entity_UpdateInfantryAI @0x4bf7bf` (`add … 0xFFFFFE60`) / `@0x4bf7ec` (`add edx,edx`; `add
    [esi+0Ch],edx`)]; the player (org2) falls `vel_z -= 208` EVERY tick then `pos.z += vel_z` (once,
    folded into the root-dz store) [orig: `Entity_UpdateInfantryPlayerBody @0x4b7acf`
    (`add … 0xFFFFFF30`), gate `@0x4b7ac8`, clamp `@0x4b7c77`, pos `@0x4b7cef`]; both clamp
    to terminal −32768 and skip the step while `Flags & 0x108000` (ladder/drowning — those
    flag legs ride their slices). A prior pass applied one `−416 every 2 ticks` + `pos += 2·vel`
    to BOTH; the NPC was fixed to the faithful per-tick `−416` + `2·vel` first, and the player's
    deferred 2-tick discretization is now the faithful per-tick `−208` + `vel` (the dedicated
    player-physics grill this entry waited on = §22). `engine/runtime/world/infantry.cpp`; guarded by
    the gravity-cadence + player-jump cases in `tests/world/infantry_test.cpp`.
  - **D-INF-11** third-person body aim overlay (the torso bend) — **LOCAL PLAYER LANDED
    2026-07-08; MOUNTED/PLACED/WIRE SELECTOR SEAM IMPLEMENTED 2026-07-19** (§14.6:
    `engine/runtime/anim/aim_overlay`, the leg-chase sim fields, the shared present snapshot result,
    and `SkeletalAnim.eval_pose_overlay`; the original local bend was verified in-play
    via the retired `bend_capture_probe.gd`; today `local_player_look` + `game_capture_bundle`, docs/mcp.md). REMAINING (row stays open, partial):
    NPC/remote upper-body weapon-channel threading (rides D-INF-1), attachment matrices, and the
    pitchBlend/pitchKickAccum/lean/torsoRoll sources. `[orig: Entity_BuildBoneTransformMatrices
    @ 0x4b1290]`, witness §14.
  - **D-INF-12** player (org2) chase sources approximated by org1 math — **CLOSED
    2026-07-16 (§22)**: the org2 writes to +0x8C/+0x2E4/+0x2E8 were displacement-scanned
    (the tooling limit that cut the earlier re-verify is gone) and byte-read. The witnessed
    model is different in kind and now ported: on foot the player has NO body chase — the
    LEGS chase the render yaw (+0x10, mouse-instant) at a quarter-step with rate clamp
    ±0x3000000 (~4.2°, 3/5 the org1 rate) and twist limit ±0x30000000 (67.5°) vs the YAW
    [orig: `@0x4b49e9-0x4b4aa9`]; re-plant targets = the yaw itself — every tick in a
    movement state [orig: `@0x4b4984→@0x4b49dd`], else per-leg 5°/30° hysteresis measured
    vs the CURRENT LEG YAW with the L window 32 ticks behind the R [orig:
    `@0x4b4993-0x4b49e3`; ebp = tick&0x3F `@0x4b4680`]; then **bodyHeading = the leg
    midpoint** [orig: `@0x4b4aa9-0x4b4abb`] — the legs lead, the body follows, and the §14
    torso twist is (yaw − midpoint), which is why small aim moves twist the torso without
    moving the feet. Ported in `tick_infantry` step 5 (player leg); pinned by the rewritten
    `test_player_body_chase_and_legs` / `test_player_body_chase_crosses_the_bam_seam`.
    Residual branches ride their slices: parachute (Flags 0x20) sixteenth-step body chase
    [orig: `@0x4b494d`] (D-INF-20), the carried/ladder ±0x55555500 (120°) yaw clamp
    [orig: `@0x4b4afb-0x4b4b5f`] and generic non-UseGun seat-bone follow
    [orig: `@0x4b654e`] (D-INF-2 / mount). UseGun root-position follow is separately matched
    through `Entity_AttachToBoneAndUpdateTransform @ 0x5463d0`.
  - **D-INF-16** the run promotion's pitch-tier term ported as the constant 2. The
    original reads `entity+0x37C` (`>0x430000 or <0 → 0; ≥0x210000 → 1; else 2` before
    adding `run_anim` [orig: `@0x4b72aa-0x4b72cf`]), but the field has NO writer anywhere
    in the retail image (full-image displacement sweep, 2026-07-13) — pool memory is
    zero-initialized, so the band is constantly 2. `player_body_select` bakes the 2 and
    records the thresholds here; if a sibling title (DFX/BHD) turns out to write +0x37C,
    lift the term into a live field. `engine/runtime/world/infantry.cpp`.
  - **D-INF-17** lean producer gate legs unmodeled. The on-foot lean ramp skips on
    `Flags & 0x100020` (bit 5 + the on-ladder bit) and the prone roll-anim selection
    skips on `Flags & 0x112002`'s 0x10000/0x100000 legs [orig: `@0x4b7da2/@0x4b7322`];
    our port gates on alive/prone/airborne only (the modeled equivalents of 0x2/0x2000).
    The seated (`+0x168 == 1`) ±0x1400000 ramp variant [orig: `@0x4b66b5`] rides the
    mounting slice. `infantry_lean_tick` / `player_body_select`.
  - **D-INF-18** the remote CameraOffset approximation unported; the FP eye's terrain
    clamp FIXED 2026-08-12. The local head-bone eye is sampled binding-side from the
    render skeleton (the structural translation of the `@0x4b6bb3` bone path; the
    reimpl's Position + 0.125 eye floor is a DEFENSIVE stand-in — the witnessed `0x2000`
    floor `@0x4b6b98` belongs to the sample-less on-foot CAPSULE leg, and neither exact
    head leg floors [witnessed 2026-08-19: on-foot `@0x4b6bb3..0x4b6cc8`, mounted
    `@0x4b6908..0x4b696c`]); the original's FIVE-sample terrain
    floor — the eye column plus ±0x4000 along each ground axis, each sample + 0x1000,
    max-folded onto the eye Z unless `Flags & 0x800000` (indoors — the heightmap has no
    interiors) [orig: `@0x4b6c08-0x4b6ca4`; sampler `Terrain_SampleHeightBilinear
    @0x6067b0`] — is now ported as `world::player_view_floor_eye_to_terrain`
    (terrain_query seam), applied inside `player_view_compose_camera`'s head-bone path
    AND on the third-person anchor-chase feed (retail floors the one CameraOffset both
    consumers read), pinned by `player_view_test::test_compose_camera_terrain_floor`.
    The world-side below-water classifier's `CameraOffset.Z` projection FIXED
    2026-08-12: the round-source stance/recoil classifier and both Godot-side
    aimed-shot/HUD-crosshair rows compare `Position.Z + eye_offset_z` (the ported
    `entity+0x74`) with `Env_WaterHeightFixed`, strictly below, wherever the drowning
    bit is clear [orig: `RoundData_SpawnRound` stance leg `@0x4ec2d5..0x4ec2ea`
    (`Flags & 0x108020` folds the ladder/parachute bits); the recoil x4 legs
    `@0x4ec342..0x4ec35a` / `@0x4ec86d..0x4ec885`], pinned by `npruntime_round_sim`'s
    eyes-dry/eyes-wet split; decoded wire rows keep their documented bounded
    projection (no CameraOffset carrier on the wire).
    2026-08-19: the sim-side eye-offset store took the exact local leg — with the
    shell-fed head sample, `Entity::eye_offset_{x,y,z}` = posed head − Position, the
    head z first terrain-floored on foot (the same five-tap walk, indoors exempt), no
    `0x2000` floor on either exact leg [orig: on-foot `@0x4b6bb3..0x4b6cc8`; mounted
    `@0x4b6908..0x4b696c` — the sites split on the entity+0x16C carrier `@0x4b6386`];
    NPCs gained the org1 lateral pair; only the sample-less player legs' full 3-angle
    lateral tilt [orig: on-foot `@0x4b69ab..0x4b6b7c`, mounted `@0x4b66fc..0x4b68e5`]
    keeps the capsule z-only approximation. The camera's
    `torsoRoll(+0x2DC)` and `2·pitchBlend(+0x380)` terms are fed live from
    `InfantryState.torso_roll` / `.recoil_pitch` through
    `player_view_compose_camera` (the earlier "producers open" note is superseded;
    residual gate legs stay on D-INF-17). `local_player_presenter.gd`.
  - **D-INF-19** the slope pass's conform selector dropped by the port — FIXED 2026-07-13.
    The dump-based port applied the slope lean+slide to EVERY live body and wrote the look
    pitch (`+0x14`) instead of `bodyPitch(+0x90)`, so a standing local player's Roll chased
    the side-slope, `torsoRoll` chased Roll, and the FP camera (`torsoRoll + lean/4
    @0x437fe6`) leaned on hillsides with no lean input (the reported bug). The original
    gates BOTH updaters' slope passes on the three-way selector (§3.5 item 4 [orig:
    `@0x4ba10f` / `@0x4b6d95`]) and decays `bodyPitch/Roll` to level otherwise; the org2
    player leg additionally differs in kind (atan2 slopes, quarter-step, 512 slide,
    60°/48° thresholds, 2-tick cadence, 41/42 roll-write skip, corpse-only `Pitch`
    tip). Ported as `AiSystem::infantry_slope_pass` with both legs; `AiEntity.body_pitch`
    (+0x90) added and fed to the §14 overlay body-pitch term; `AiEntity.def_attrib`
    carries the `def+84 & 0x200` selector leg (binding wiring deferred — JO infantry defs
    leave it clear). Residuals: the dead+airborne corpse TUMBLE branch [orig: `@0x4ba0b2` /
    `@0x4b6ccb`] unported (the pass holds instead); the dead leg's `Flags & 0x10A000`
    swim/parachute exclusion unmodeled (rides D-INF-17's flag legs); org1 cadence uses the
    port's entity-salted `key` (net_id-staggered) where the original uses the global tick.
    Guarded by `test_slope_standing_camera_stays_level` / `test_slope_prone_body_conforms_org2`
    / `test_slope_pass_org1_selector_and_chase` + the motor-level standing case in
    `tests/world/infantry_test.cpp`.
  - **D-INF-20** the parachute system is unmodeled (Flags 0x20 = parachute deployed).
    Witnessed legs (§22): auto-deploy on the authority when alive, `vel_z ≤ −14336`, and
    aux `+0x2C & 0x10` [orig: `@0x4b7aef-0x4b7afa`]; the in-air anim while set: org2 adds
    0x10 to its straight 31 stamp (the `(Flags&0x20)?0x10:0 + 0x1F` trick
    `@0x4b7e3f-0x4b7e61`), and org1's ENTIRE 47→31 availability ladder is parachute-gated
    (`test al,20h` `@0x4bf8d8` — a plain NPC fall stamps nothing) [orig:
    `@0x4bf8d4-0x4bf8f7`]; the org2 heading model
    sixteenth-chases the yaw + snaps the leg targets while set [orig: `@0x4b494d-0x4b496c`];
    gravity skips while drowning/platform but NOT while parachuting — the descent physics
    live in the `@0x4b7b18+` swim/parachute block (unread). Ours never sets the flag; the
    player's jump and fall stamp 31 (NPC falls keep the clip, as witnessed).
  - **D-INF-21** the "!Poof!" ghost mode is deliberately unported: `g_localPlayerPoofMode
    @ 0xA82298` (renamed this session, ex `dword_A82298`) is toggled by a net-message
    handler that debug-prints `!Poof!` [orig: `@0x42d450` — the IDB's
    `NetPacket_HandleWeaponSwitch` name is a misnomer for this 0x70-byte body, rename
    proposed] and, while set, the local player's horizontal root-motion integrate runs at
    2× [orig: `@0x4b7c8d-0x4b7cb7`]. Normal play (mode 0) takes the same 1× integrate as
    org1 [orig: `@0x4b7cbf-0x4b7cd9`], which is what the port implements — a dev/admin
    feature, not a gameplay path. (The handler is renamed `NetMsg_HandlePoofToggle`,
    ex the `NetPacket_HandleWeaponSwitch` misnomer.)
  - **D-INF-23** the authority body channel lacked the gait->stance transition-clip
    insert — FIXED 2026-08-11. A forward gait {1,2,8,9,10,149} committing to its
    crouch/prone walk {11,12,18,19} first plays `run2crouch`/`runl2crouch`/
    `runr2crouch`/`run2prone` (169-172) and defers the real target to the clip's
    completion boundary, gated on the adm carrying the clip and no deferred armed
    [orig: `AnimMap_UpdateEntity @0x40b662..0x40b737`; promotion `@0x40b795/@0x40b7c3`].
    Surfaced by the S11 grill (2026-08-07), which landed the replica-side twin first
    (D-NET-209, netsim `row_root_motion_tick`); the authority port runs the insert in
    `commit_body_state`'s direct-commit branch and both clip-end promotion sites, and
    the pair map is the shared `world::gait_stance_transition_clip` (`infantry.h`)
    consumed by BOTH channels so they cannot drift. Guarded by
    `test_gait_stance_transition_insert` (pair map, insert + clip-end promotion,
    crouch twin, no-clip direct commit) and the unchanged netsim arbitration suite.
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
present boundary (`mission_yaw = 90 − heading / kBamPerDegree`). The Godot basis
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
   `ItemDatabase`. Runtime entry points: `AnimMap_LoadAdmFile @ 0x40cc40`,
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
5/6/0x16 = alerts (the controller alert LEVEL byte ai+136 = 2/0/1 — not a stance — PLUS the queued
brain `AIEvent {6, level}`; both halves PORTED 2026-08-15, semantics in the bms record §3b item 1),
8 = ACCURACY (`ai+40 = 100 − param2`), 0x1C = AISETSTATE
(queues AI event 7), 0x1D/0x1E = combat/patrol speed (events 10/11).

### 8.3 ANIMNUM is a part-anim CHANNEL, not an animation name/index
**Durable warning:** `Entity_ApplyCommand` case 0x22 validates ANIMNUM ∈ {1,2} — it selects one of two
model part-anim channels (slot = channel − 1); any other value is a no-op. The animation content is the
model's PANM. Do NOT wire ANIMNUM to `off_8135F0` — that is the separate infantry full-body table
(§3.4, AI-state-driven via `Script_ForceAnimation @ 0x4f2610` / `Entity_UpdateInfantryAI @ 0x4b9910`).
The former ONED Object workspace exposed ANIMNUM as a plain "Part #" raw int;
ADR 0037 removed that UI. The runtime contract remains the raw integer channel.

### 8.4 PLAYPARTANIM contract (case 0x22, exact rate formula)
- Stores per-channel **direction** (`play_type` ∈ {−1,0,+1}) at `comp+436+4·slot` and a **rate** at
  `comp+444+4·slot`; `comp` = the 812-byte AI struct at entity[25].
- Rate derivation: `seconds = ANIMTIME / 65536` (confirms param4 raw = sec·65536);
  `rate = ftol((0.016 / seconds) · 65536)`, min 1 — i.e. **1048.576/seconds 16.16-phase units per
  62.5 Hz tick** before truncation. Because the integer rate is truncated and the clamp is strict,
  a nominal one-second sweep reaches `0x10000` on tick 63 and stops on the following overshoot.
  Constants verified
  IEEE-754 LE: `flt_7C3310 = 1/65536`, `flt_7C3B40 = 0.016 (= 1/62.5)`, `flt_7C32BC = 65536`.
- Writes ONLY direction + rate — it never resets the phase. PLAYPARTANIM is **velocity control from
  the current position** ("start moving part c at this speed/dir; play_type 0 = halt"), not
  reset-and-sweep.
- Channel defaults come from the object/vehicle def (`Entity_CopyVehicleDefToAIComp @ 0x45ddf9`:
  def+764..784 → comp+436..456) — objects can ship an idle part-anim (radar-dish spin).
- It never calls AnimMap: the part system (turret yaw / barrel pitch / dish) is DISTINCT from the
  infantry full-body ADM/BAD system.
- **The integrator, and the CTRL separation (witnessed 2026-07-22).** The per-tick integrator is
  `Entity_UpdateSuspensionBounce @ 0x456710` — an IDA misnomer for its first half. Per channel it reads
  the direction `comp[109]/comp[110]` and the rate `comp[111]/comp[112]`. Direction `+1` performs a
  wrapping 32-bit ADD into `comp[113]/comp[114]` and clamps/stops only when the signed result is
  strictly greater than `0x10000`; every other nonzero direction performs wrapping SUB and
  clamps/stops only when the signed result is negative. Landing exactly on either endpoint therefore
  remains active until the next tick. `ANIMTIME=0` exposes the raw arithmetic: x87 conversion yields
  `INT_MIN`, so forward from zero alternates `INT_MIN`/zero without stopping, while reverse from zero
  clamps to zero and stops on its first tick `[orig: @0x456740..0x4567A9]`.
  The phases are stored separately from the ordinary named-CTRL target/current banks, but they are
  **published onto the global CTRL bus before presentation**. `[orig: HUD_CacheEntityDisplayInfo
  @ 0x4A3E18..0x4A3E38]` maps `comp[113]` to `VEHICLE_SPECIAL1` (global ordinal 71) only when the
  item's attribute bit `0x1000` is clear, and maps `comp[114]` to `VEHICLE_SPECIAL2` (ordinal 72)
  unconditionally. This is the missing second stage behind the earlier, incorrect conclusion that
  PLAYPARTANIM never reaches a named register.
- Port contract (`ObjectModel.play_part_anim(channel, play_type, time_s)`): channel ∈ {1,2} → part
  channel `slot` = channel − 1 `[orig: Entity_ApplyCommand case 0x22 @ 0x43B192]`; the retained
  integrator uses the same truncated rate, fixed 16 ms ticks, wrapping ADD/SUB, and strict
  overshoot rules as the authority runtime. Ordinary values occupy `0..0x10000`, but wrapped signed
  dwords are preserved rather than normalized. Velocity starts from the CURRENT value. Channel 1 targets
  `VEHICLE_SPECIAL1` subject to the item-attribute `0x1000` gate, and channel 2 targets
  `VEHICLE_SPECIAL2`. There is **no model-order selection and no engine-name blacklist**. The former
  bridge walked the model's first two CTRL entries and then tried to blacklist collisions, which put
  B50Cal's first phase on `HEAT_GLOW`; D-WPN-31 records that fixed divergence.

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
- The live carry has two retail paths. Slot-3 `UseGun` calls
  `Entity_AttachToBoneAndUpdateTransform @ 0x5463d0` from the player body at `0x4b63c7` and the AI
  body at `0x4bec23`. Ordinary seats use `Entity_GetBoneTransformAndOrientation @ 0x4b0c50`
  (`Entity_SerializeVehicleState @ 0x460560` is the adjacent wire witness). Mounted-pose anim states
  (emplaced 67–75, sit_N, driver lean) are §4.15.

### 9.2 Port (engine/runtime/world + engine/runtime/mission) and tracked deviations
Shipped: `Entity.seats` + occupant refs riding the registry value-copy (`World::Snapshot` ⇒ Play→Stop
rewinds mounts for free); canonical `find_best_vehicle_seat`, `attach_to_vehicle_seat`, and
`entity_detach_from_vehicle` operations plus `EntityCommands::{mount, mount_boarding_command,
mount_best, dismount, find_mounted_on}` mirroring 0x4351f0/0x4f70f0/0x4355f0/0x4359f0.
Wire, command, USE, and mobile-deployment entry points share those relationship operations;
the former duplicate command-side write/detach blocks were removed. Every attach runs
`presnap_vehicle_attach_heading` before writing the relationship. The request-time seat yaw is copied into `Entity.yaw` and the
occupant's `AiEntity.heading`; for the local player it also initializes
`InfantryState.target_heading`. `Simulation` owns an additional binding-only
`PlayerInput.look_heading` latch that retail does not need because its input and entity yaw are one
state. `sync_local_mounted_input_heading` mirrors the snapped local target into that latch immediately
after a successful mount toggle and after local/host logic ticks, before the next
`apply_player_input_pre_tick` can restore the pre-attach look. Both layers are yaw-only; pitch remains
player-owned and deliberately untouched. The joiner now queues C2S 0x26/0x27 without local prediction;
the host validates the relationship and the requester applies it only after the S2C 0x0A carrier/bone
echo. The same heading synchronization then runs on that authoritative attach/detach edge.
`pose_mounted_occupant` (occ.pos = veh.pos + rotate(seat_local, veh.yaw), gunner yaw = veh.yaw −
yaw_offset); `AiSystem::pose_if_mounted` captures that seat frame before the local LOOK mirror mutates
`Entity.yaw`, synchronizes `body_heading`, both `leg_yaw`/`leg_target` chains, `body_pitch`,
and roll, then skips SM + locomotion and auto-dismounts when the vehicle is gone. Remote
`AiEntity.heading` stays in the seat frame; the local `heading`/`pitch` remain the full-precision
`target_heading`/`look_pitch` while registry yaw is only the rounded wire/motor look mirror.
For UseGun only, the host evaluates the authored userpoint through its owning 3DI part's live PANM
after the semantic EWEAP yaw/pitch controls and feeds that world position back to the mounted
occupant. This overrides the static `seat_local` result for the root-position contract while leaving
the full matrix basis and non-UseGun seat follow unclaimed.
Each target carries the parsed `phrase_set` as an explicit `{valid,value}` pair; attachment copies
that pair to the occupant, registry snapshots value-copy it, restore recovers it, and every dismount
path clears the occupant copy and validity. This is deliberately separate from seat-frame pose state:
the frame synchronization above supplies body/leg/pitch/roll, while animation's `MountMode` selects
which witnessed overlay matrix each skeletal class consumes.
Event-runtime case 0x25 → `mount_best(param1)`; command-123/124/125 promotion mounts
already-near occupants onto their target SSN with the runtime gate above; mounted infantry pose class
is selected from the occupied seat (`UseGun` → 67+variant if that clip exists, other seats →
`anim_sit_N` from the seat name digits). GDExtension debug cards expose the selected seat source name
and the full target-seat candidate list (`source_name`, type, pose index, local offset, occupancy) for
00TRa-style audits. Per-entity ADM resolver inputs remain live mission state: newly appended AI entries
(including joiner-local and host-admitted players) register their model ADM before the configured mounted
selector runs. This matches the entity-registration lifetime at `AnimMap_RegisterEntity @ 0x40bb60`; a
one-time post-load sweep left late players on the default map, silently collapsing B50 `phrase_set=4`
from `anim_emplaced_5` to the visibly offset generic `anim_emplaced` pose. The host admission hook runs
after connection spawning but before `Server_TickUpdate`; direct local/joiner spawn paths resolve before
their first body update. Clearing/reloading the animation registry resets the resolver high-water and
repopulates every live `adm_id`, while Play→Stop restore rewinds both the AI array and resolver mark before
re-resolving the baseline. `test_late_spawn_player_resolves_own_adm_before_configured_usegun_pose` pins
the former failure. Deviations (NOT silently absorbed):
1. **Proximity proxy vs occupant-model+144.** The original's vehicle is the occupant's model hierarchy
   link; we pick the nearest free-seat entity within 20 units (`kMountRadius`). Faithful for a soldier
   placed on its gun; wrong if two guns overlap.
2. **Seat specs are binding-fed.** The original reads model seat bones directly (model[605..]); the
   port consumes binding-extracted model userpoints through `ItemSeatSpec`, so callers without model
   metadata still seed no seats.
3. **Child-entity seat traversal** deferred (single-entity seats only).
4. **Mounted-pose variants.** `UseGun` consumes the target definition's production-parsed
   `phrase_set` (including valid zero), and non-gunners consume numbered `sit_N`. The sit_24
   driver-lean 107–110 variants remain open.
5. **Generic seat-local pose stand-in** — UseGun root position now follows the owning part's live
   control-posed userpoint, but non-UseGun seats still use binding-fed `seat_local`/`yaw_offset` rather
   than the true per-tick `Entity_GetBoneTransformAndOrientation @ 0x4b0c50` result. The full UseGun
   matrix basis is also not claimed. These residuals remain distinct from the matching request-time
   yaw pre-snap.

## 10. Appendix: coordinate frames + terrain grounding (2026-06-08)

### 10.1 The BMS↔Godot convention lives in GDScript (and the godot-cpp Basis trap)
Mission frame is Z-up; the Godot side is Y-up. The conversion is single-sourced in GDScript
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

Historical note: the former editor's `sample_world_height` read a live
FORMAT_RF image while runtime samples `cpt.depth_buffer` through
`TerrainHeightField`. ADR 0037 removed the editor consumer, so this is no
longer an open product divergence.

### 10.2 Ground sampling chain — `Entity_CalcAverageGroundHeight @ 0x457230` (CONFIRMED EXACT)
5-tap weighted ground height, `(entity, sampleRadius)`:
- N/S taps at (0, ±r), E/W at (±r, 0), C at center; `max` = highest positive tap (C included);
- `result = (N + S + E + W + 2·(C + 2·max)) / 10`, floored at C;
- water clamp: `if (*(entity+368) && worldY > result) result = worldY` (worldY @ 0x26C6454);
- def offset: `result += dead ? def+0x30 : def+0x2C`, where
  `dead = ((entity+36 & 2) || entity+286 <= 0) && entity+52`;
- radius 0 path: center tap only.

**Decompile-lossiness warning (durable):** the per-tap samplers `Entity_RaycastGroundHeight` / `Entity_RaycastGroundHeightAndObject` decompile
as if they ignore the dx/dy offsets and return a flat field — WRONG. Disasm + the pointer write-back
show each builds `pos = {x+dx, y+dy, z + 0x10000 − 0x300000}` (a ray from z+1.0 down to z−47.0) and
calls `raycast_entity_collision @ 0x413760`, which writes the sampled ground height back into `pos.z`
(returned in eax) — the 5 taps DO sample 5 distinct columns. `Entity_RaycastGroundHeightAndObject` additionally stores the
raycast collision-object at record+40. The real sampler is
`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`: a LoRes pass, then back/forward step + 8-iteration
bisection refine (the per-step terrain samples are `Terrain_SampleHeightBilinear @ 0x6067b0` —
that callee sat IDB-mistyped as a `void()` "null_stub" so decompiles elided it; retyped at the
ENG-3 B0 grill, full witness in [terrain-re.md](../terrain/terrain-re.md) §Runtime terrain
queries); for a near-vertical down-ray this equals the bilinear heightmap column height.

### 10.3 The vertical assignment + our port
Both movers recompute `brain[131]` from the sampler EVERY tick: `0x466db0` sets
`ground(0x50000) + 0x50000`; `0x460e40` sets `max(nodeZ-or-ground+heightOffset, def[232]+ground)`
(§7.4). Our lean `AI_UpdateWaypointMovement @ 0x457bd0` port never wrote `brain[131]` — the root cause
of the floating-AI bug. Shipped: portable `terrain/height_field` (the three `TerrainData`
samplers lifted verbatim, TerrainData delegates); `world::calc_average_ground_height` (the
0x457230 math, 16.16); `AiSystem::apply_ground_clamp` SETs `brain[131]` + snaps
`pos[2] = ground + 0x50000` (SET not max — the lean mover leaves brain[131] stale; a max would strand
a float); `Simulation::set_terrain_height_field` wired through GameWorld
and direct test/tooling fixtures. Tracked
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
**engine-research + PORTED** — originals witnessed 2026-06-24, the render side completed
2026-07-26, and the port landed 2026-07-26/27 as D-WPN-32 increments 1-3 (the local player's
own body, then every remote player, then the second attach frame of §14.4). The hide gate
below is live in `Simulation::local_held_weapon_visible`; §13.5 carries what landed and
what is still missing from this callback.

**2026-07-26 update:** the render side is now witnessed IN FULL, not just the hide gate —
the model source (`gfx3` at `AdmDef+0x170`), the rigid all-bones fill, and the exact
placement matrix (body bone slot 16 + a def offset triple) are all decompiled below, and
two labels in §13.1 plus the `prim` user-point placement claim in §13.2 were CORRECTED as
part of it. **The reimpl is no longer absent** — it landed 2026-07-26/27 as D-WPN-32
increments 1-3; see §13.5 for what landed and what from this callback is still missing, and
§14.4/§14.4a-c for the attach half. Complements §9 (mount/emplacement) and §4.15
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
3. **night-vision goggles** — gated on `Flags & 4` (`entity+0x24`), model
   `gItemDefs[g_NightVissionGoggleItemIndex].graphicModel`, rigid at the `outMuzzleFlashMatrix`
   bone [orig: `@ 0x4e3b54`..`@ 0x4e3be7`]. **Corrected 2026-07-26**: this row read
   "muzzle flash" until the draw was decompiled in full — it is the NVG item model, and the
   caller's local name `muzzleFlashMatrix` is an IDB misnomer for the head bone matrix,
4. **binoculars** — gated on `g_animStateFlagsTable[animStateId] & 0x40 && (Flags & 8)`
   (the per-anim-state flag table of §3.4), model `gItemDefs[g_BinocItemIndex].graphicModel`,
   rigid at the `outSightMatrix` bone [orig: `@ 0x4e3c04`..`@ 0x4e3c82`]. **Corrected
   2026-07-26** from "weapon sight/scope" for the same reason; `sightMatrix` is the face/eye
   bone matrix, not a scope,
5. **held weapon** — see §13.2,
6. **mounted-child overlay** — gated on `mountedChild` (`entity+0x268`), draws the carried
   child/flag at a Z-rotated transform.

Draws 5 and 6 are *both* additionally suppressed wholesale when the render-pass flag
`numEntries & 0x10000000` is set. REN-3 identified that word as `Render_SubmitEntity`'s
RENDER FLAGS argument and `0x10000000` as the **repeat-draw marker** — dual-LOD entities
draw twice (far LOD, then near LOD with the flag set), so one-shot overlay children
render once ([render-order-re.md](../render/render-order-re.md), submit-flags table).

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
  `+0x298` (`Weapon`) caches the resolved `AdmDef`.
- **The model is `weapon.def` `gfx3`** — `weaponDef[92]` = byte `+0x170`
  [orig: `@ 0x4e3cd3`]. NOTE the IDB locals in `WeaponDef_ResolveAllReferences @ 0x54042c`
  are SWAPPED: `model_1p` there reads `+0x170` (which is `gfx3`, THIRD person) and `model_3p`
  reads `+0x16C` (`gfx1`, the FIRST-person viewmodel). The struct field names
  `fpModel(+0x16C)` / `tpModel(+0x170)` are the correct ones.
- **PLACEMENT — corrected 2026-07-26.** This section previously said the weapon is posed "at
  the hand/`prim` bone matrix ... the same `prim` user-point the AI scans". That was a guess
  and it is WRONG; porting it would have invented placement. The witnessed shape is:
  the weapon model is drawn **RIGID** — the single `outWeaponMatrix` is `qmemcpy`'d into
  **every** bone slot of the weapon model, so it carries no pose of its own
  [orig: the fill loop `@ 0x4e3d71`, submit `@ 0x4e3d99`]. And `outWeaponMatrix` itself is
  built by `Entity_BuildBoneTransformMatrices` from **bone matrix slot 16** of the body
  skeleton (`boneMatrixBuffer + 1024`, 64-byte matrices) translated by a **3-float offset
  triple read from the model def at `+0x424`/`+0x428`/`+0x42C`**
  [orig: `@ 0x4b2180`..`@ 0x4b22f8`]. Its siblings come from the same builder:
  `outSightMatrix` is bone slot 15 (`+960`, offset triple at `+0x3E4`..) and
  `outMuzzleFlashMatrix` is the head bone. When the entity has **no `graphicModel`** all
  three degenerate to the entity's own world-position matrix
  [orig: `Math_BuildFixedPointToFloatMatrix4x4 @ 0x4b12f2`].
- A size/visibility cull sits in front of the submit: the draw is skipped unless the render
  state carries bits `& 6`, or the projected point clears `dword_A784F0 >= 0x20000`
  [orig: `@ 0x4e3d4b`].
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
control (2), gunner (3), and driver (5) hide it. **LANDED 2026-07-26/27** (D-WPN-32 increments 1-3): the predicate is ported as
`Simulation::local_held_weapon_visible` for the local player, while a REMOTE body
evaluates two of that branch's terms — alive, and `MountMode::OnFoot`, which is exactly the
complement of retail's `{2,3,5}` hide set, so a passenger keeps its weapon. The remote verdict
is folded into the `PF_HELD_WEAPON_ADM` snapshot field: a hidden or unarmed body reports index
0, which is simultaneously our table's null row and the original's own
`if (entity->equippedAdmIndex)` precondition. STILL UNPORTED from this callback: AI/NPC bodies
(a placed `.bms` soldier never receives an equipped ADM index and its source in the original
is UNWITNESSED — every witnessed writer of `entity+0x2B0` is a player path, so that increment
is blocked on research rather than effort), the NVG and BINOCULAR draws 3 and 4, the
projected-size cull `@ 0x4e3d4b`, the mounted-branch correlation of §14.4, and the DEATH
family's rows in the same 0x80 flag table the hand-frame branch reads. The original guidance
below still describes the seat taxonomy the port reuses.

The predicate gates the weapon node's visibility, reusing the existing seat
taxonomy (`item_seat_specs.gd`, now `engine/runtime/simassets/seat_spec_extract.h`; SEAT_PASSENGER/CONTROLLER/GUNNER/
DRIVER) and the `mount_type` already exported through `simulation.cpp`. The exact
`Entity_CanFireWeapon` predicate (incl. the `Flags & 2` weapon-disabled gate and the local
gunner third-person condition) is the faithful rule. **Open follow-ups:** IDB hygiene (rename
`pad_2b0` → `heldWeaponAdmIndex`; comment `Entity_CanFireWeapon` as the weapon-visibility
gate) is proposed but unapplied (shared IDB state).

## 14. Appendix: third-person body aim overlay — the torso bend (engine-research, 2026-07-08)

The question: how does the original bend the soldier at the waist when looking up/down in third
person (on foot and mounted)? Answer: there is no procedural spine IK — every skeleton bone's
**world orientation** is `animPose × overlay(boneIndex)`, where the overlay is one of seven full
entity-orientation matrices built per frame from blends of aim vs body angles; the per-segment
blend gradient IS the bend. Witnessed end-to-end in
`[orig: Entity_BuildBoneTransformMatrices @ 0x4b1290]` (callers
`[orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0]`, `[orig: Entity_UpdateInfantryAI @ 0x4b9910]`;
render consumers `BoneCallback_org0_* @ 0x4e34b0/0x4e3940`, `Entity_GetCameraTransform @ 0x4b8c00`,
`Entity_GetAttachmentWorldPosition @ 0x4b2670`). The reimplementation's animation-owned selector is
`opennova::anim::compute_aim_overlay_angles`; simulation, collision, and both presentation paths share
its selected result rather than reconstructing these branches independently (see §14.6).

### 14.1 Pipeline

1. `AnimChannel_ComputeBoneMatrices @ 0x410da0` samples the active channel; the copy into the
   per-bone 4×4 buffer applies the (−x, y, z) handedness flip (ADR 0007 convention).
2. If entity `Flags & 0x100` (weapon in hands), not gun/ctrl/driver-mounted, and the PRIMARY
   channel's anim state (`+0x2BC`) has flag 0x40 `[orig: gate @ 0x4b14a7..0x4b14d1]`, the SECOND
   channel (`animChannelA @ +0x18c`, the weapon layer — producer witnessed §14.8) overwrites the
   **upper-body mask** bones {3,4,5,6,9,10,13,14,15,16} = clavicles, upper arms, forearms, neck,
   head, both hands — the two-channel upper/lower split (`AnimChannel_BlendTwoChannels @ 0x410740`
   is the blended variant; this site is the hard override form, second compute `@ 0x4b16a7`).
3. Seven overlay matrices are built by temp-mutating entity `Yaw/Pitch/Roll` (+0x10/+0x14/+0x18)
   and calling `Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200` on the entity block (position +
   YPR); originals restored after. Angle sources: aim = render `Yaw`/`Pitch`, body =
   `bodyHeading/bodyPitch` (+0x8c/+0x90), legs = `+0x2d4/+0x2d8` (§3.3), `torsoRoll +0x2dc`,
   `leanAngle +0xb0`, `pitchBlend +0x380`, `pitchKickAccum +0x36c`.
4. Per bone: `switch (boneIndex)` picks the overlay (table below); result = anim × overlay; then
   the bone is re-anchored on its parent: translation = parentMatrix·pivot, composed with
   translate(−pivot) — pivot/parent from the **model bone-def table** at `modelDef+56`
   (stride 64 B: parent index @ +0x14, pivot float3 @ +0x24). `modelDef = graphicModel[8]`, bone
   count @ +52.
5. Special rows: a bone whose `sectionMask` (+0x134) bit is set is zeroed (hidden/dismembered).
   Bone 16 (R hand) is zeroed when mounted without weapon-use (`parentSlot∈{2,3,5}` and
   `!(Flags & 0x100)`) or when dead with an attacker ref — the held-weapon stow of §13. When the
   anim channel drives fewer bones than the model and covers ≥15, the FIRST bone past the channel
   copies bone 14 (head) — the helmet/head-gear convention.

### 14.2 Bone → overlay map

Bone index = the model/`.bad` bone order (BN## − 1; order witnessed in the BINOC.bad fixture and
pinned by the upper-body mask's exact complement of the leg chains). Hex-Rays pseudocode shows the
switch labels **shifted −1** (the jump table indexes `boneIdx−1` via the byte table @ 0x4b25c0;
bone 0 takes the `default`) — the disasm is the truth.

| idx | bone | overlay (on-foot aim state, flag 0x40) |
|---|---|---|
| 0 | BN01 Hips | body: yaw=bodyHeading, pitch=bodyPitch (default case) |
| 1 | BN02 Lower Spine | yaw = aim + (body−aim)/2, pitch = body + (aim−body)/4 + pitchBlend/2, roll = Roll + lean/2 |
| 2 | BN03 Upper Spine | yaw = aim + (body−aim)/4, pitch = aim + pitchBlend, roll = Roll + lean |
| 3,4 | BN04/BN05 R/L Clavicle | same as BN03 (copy) |
| 5,6,9,10 | BN06/07 upper arms, BN10/11 forearms | yaw = aim + (body−aim)/4, pitch = aim + pitchKickAccum + 2·pitchBlend, roll = Roll + lean |
| 15 | BN16 L Hand | same as arms |
| 7,11,17 | BN08/12/18 R thigh/calf/foot | yaw = `+0x2d4` (R leg chase), pitch = bodyPitch |
| 8,12,18 | BN09/13/19 L thigh/calf/foot | yaw = `+0x2d8` (L leg chase), pitch = bodyPitch |
| 13 | BN14 Neck | = BN03's matrix on foot; = FULL aim when seated (driver looks around) |
| 14 | BN15 Head | full aim: yaw = aim, pitch = aim (+ local-player kick, §14.3), roll = torsoRoll + lean/2 |
| 16 | BN17 R Hand | stow-hide rule (§14.1.5), else the arm matrix |
| ≥19 | accessories | body matrix (default) |

The gradient hips(0%) → lower spine(50% yaw, 25% pitch) → upper spine/chest(75% yaw, 100% pitch)
→ head(100%) is the visible waist bend; legs ignore aim pitch entirely and follow their §3.3
chase yaws.

### 14.3 Branches and gates

- **Aim-state gate**: `g_animStateFlagsTable[animStateId] & 0x40` selects the bend branch. Prone
  states (0x603) and rolls/deaths lack 0x40 → no-bend branch: all body bones take the body matrix;
  arms get aim pitch + pitchKickAccum/4 + 2·pitchBlend (skipped when `Flags & 0x100000`); head keeps
  full aim. Anim states 41/42 (`roll_left/right`) additionally zero `Roll`/`torsoRoll` — the clip
  owns the whole body during combat rolls.
- **Mounted config source and validity**: the gunner block reads the **target entity's item
  definition** dword `+0x86c` at `@0x4b1884`. Its producer is the items.def `phrase_set` branch in
  `ItemDef_ParseProperty`: `_stricmp @0x49f9de` → `atol @0x49f9f0` → 0xADC-stride store
  `@0x49fa0a`. Retail always reaches this block with a target definition; the port therefore carries
  key presence separately. Unknown config takes the ordinary on-foot selector, while valid config 0
  enters the witnessed zero branch. It is never legitimate to use a default integer zero as proof of
  config 0.
- **Seated** (`parentSlot ∈ {2,5}`): lower/upper spine, clavicles, and arms take body; neck and head
  take full aim; both leg classes keep the already-built seated leg-chain matrices. The player body
  updater additionally HALVES the render pitch fed into the build while in vehicle third person
  `[orig: @ 0x4b4942]`.
- **Mounted gunner** (`parentSlot == 3`): let `A/P` be aim yaw/pitch, `B/Q` body yaw/pitch,
  `R` roll, `LN` lean, and `PB` pitchBlend (all BAM32; subtractions wrap and shifts are arithmetic).
  The valid-config table is:

  | valid config | lower spine | upper spine | clavicles + arms | neck | head |
  |---|---|---|---|---|---|
  | `0` | body | body | yaw `B - ((A-B)>>2)`; pitch `(PB>>1) + 2Q - P`; roll `R+LN` | aim | aim |
  | `3`, `5`, `7` | body | body | body | body | body |
  | `6` | body | body | body | body | aim |
  | other nonzero | body | yaw `B - ((A-B)>>4)`; pitch `Q+(PB>>1)`; roll `R+(LN>>1)` | yaw `B - ((A-B)>>2)`; pitch `Q+(PB>>1)-((P-Q)>>1)`; roll `R+LN` | aim | aim |

  Gunner legs keep the unconditional leg-chain matrices. Configs 3/5/7 are the fully body-locked
  witnessed upper skeleton; config 6 differs only at the head. Config 0 and the other-nonzero row are
  distinct counter-lean recipes; there is no collision-specific mounted recipe.
- **Local player**: when `entity == dword_C6EC38` (the view/local entity global; identity from
  reads — HUD self-label skip, audio listener), head/aim pitch gets
  `+ (g_audioOutLevel << 17)` `[orig: @ 0x4b17f0]`. **RESOLVED (2026-07-09)**:
  `g_audioOutLevel @ 0x3346FA8` is the software audio mixer's smoothed OUTPUT POWER
  meter — every 0x400 mixed samples the mixer loop takes its block amplitude
  accumulator, computes `(a²) >> 15 − 0x400` clamped to 0..0x3FF, and double-smooths it
  (`(prev+new)/2` into `g_audioOutLevelStage1 @ 0x3346FA4`, then again into
  `g_audioOutLevel`) `[orig: the mixer loop @ 0x7bef3e..0x7bef55, function-less blob;
  zeroed by AudioMixer_Init @ 0x7bd405; the MMX blend routine ptr @ 0x3346F9C]`. The
  "recoil twitch" on fire is literally the game's LOUDNESS kicking the POV body's
  head/aim pitch — own gunfire being the loudest nearby source. There is no dedicated
  rifle recoil body anim. Porting it needs an audio-output level tap in the reimpl mixer
  (deferred; the overlay input carries the term).
- **Rigid defs**: `itemDef->attrib & 0x200` → every overlay = body matrix (no aim skeleton).

### 14.4 Attachment out-matrices (weapon / sight / muzzle)

- Held-weapon world matrix — **orientation corrected 2026-07-26.** This bullet said "full-aim
  orientation"; that is wrong about PITCH and ROLL, and porting it as written would give a
  visibly mis-pitched rifle. The 3x3 is a **TENTH attachment matrix** built beside the nine
  bone-class overlays (Hex-Rays stack local `neckMatrix` / `var_13C0` — itself a misnomer, it is
  the attachment-orientation slot) and never read by the bone loop: a scan of
  `0x4b1290..0x4b2500` finds no `var_13C0` reference inside the bone walk `0x4b1f10..0x4b2160`,
  so it exists solely to place the weapon. On-foot in an aim state (`flags & 0x40`) its triple is
  the ARM recipe with the yaw blend replaced by PURE AIM yaw:
  `(yaw = aimYaw, pitch = Pitch + pitchKickAccum + 2*pitchBlend, roll = Roll + leanAngle)`
  `[orig: @ 0x4b1bdc..0x4b1bf8]`. It is therefore NOT reusable from `kOverlayHead` (which carries
  full-aim pitch) nor from `kOverlayArm` (which carries blended yaw).
  **Refinement 2026-07-26 — it is not a matrix blend at all.** The original writes the triple
  onto the ENTITY (`entity->Roll = savedRoll + leanAngle` `@ 0x4b1bdc`, `entity->Yaw = aimYaw`
  `@ 0x4b1bf2`, `entity->Pitch = savedPitch + pitchKickAccum + 2*pitchBlend` `@ 0x4b1bf5`) and then
  builds a transform straight from the entity
  (`Math_BuildFixedPointToFloatMatrix4x4(attach, &entity->Position.X)` `@ 0x4b1bf8`), which is why
  the result reads as an entity orientation rather than an overlay delta. Outside an aim state the
  same idiom runs WITHOUT the head-look term (`savedPitch + 2*pitchBlend`)
  `[orig: @ 0x4b1dd9..0x4b1dfa, sibling @ 0x4b1d60..0x4b1d80]`.
  **The MOUNTED branches are not a separate recipe** — an earlier note here called them unread;
  they are plain copies of an already-built matrix, either the body one or the arm one
  `[orig: qmemcpy @ 0x4b193e (arm) / @ 0x4b19cf (body) / @ 0x4b1ab2 (arm) / @ 0x4b1b35 (body) /
  @ 0x4b1b94 (body)]`. Correlating each copy to its mount mode is the only piece still open, and
  the exposure is small: the draw gate hides the weapon outright for control/gunner/driver seats,
  so only the passenger seat can show one. Ported as `anim::compute_held_weapon_attach_angles`
  (on-foot branches only, mounted explicitly out of scope at the call site).
  Position = bone 16 (R hand) × the attach offset. **That offset is bone-table row 16's own
  PIVOT**, not a separate attach field: the model bone-def table is `*(modelDef+56)` with row
  stride 64 and the pivot float3 at row `+0x24`, so `0x424 = 64*16 + 0x24` — and the sight and
  muzzle triples fall out of the same arithmetic (`0x3E4 = 64*15 + 0x24`, `0x3A4 = 64*14 + 0x24`),
  1:1 with bone matrix slots `+1024`/`+960`/`+896` `[orig: base load @ 0x4b2186; row walk
  @ 0x4b201e..0x4b2036; stride @ 0x4b2145]`. It is the ABSOLUTE model-space pivot, so on our side
  it is already parsed and plumbed as `ObjectData::get_bone_origins()` — no format work.
  Nudges, per component: `X + 0.05`, `Y - 0.05`, `Z + 0.051` (`flt_7C68E8` = 0.05000000074505806,
  `flt_7C9BA8` = 0.050999999046325684); they are authored in the original's x-negated render
  frame, so the X sign must be derived the way `positions_from_model` already does rather than
  copied literally.
- **The out-matrix is the attachment 3×3 with only its TRANSLATION row replaced — measured
  2026-07-27.** The tail of the weapon block writes the three transformed pivot components into
  `var_13C0`'s translation row and then `rep movsd`s the whole 64-byte matrix into
  `outWeaponMatrix` `[orig: stores @ 0x4b22cf / 0x4b22e6 / 0x4b22f1, copy @ 0x4b22f8]`. Nothing
  else touches the 3×3, and the draw site adds nothing either — it stamps that one matrix into
  every bone slot and submits `[orig: fill @ 0x4e3d71, submit @ 0x4e3d99]`. So the held weapon's
  orientation is the attachment matrix, exactly and only.
- **There is a SECOND attach frame, and it is an ordinary-play path — measured 2026-07-27.**
  When the WEAPON CHANNEL's state (`+0x2C8` — the IDB field `prevAnimStateId` is a misname, see
  §14.8.6) has table flag 0x80, the attachment 3×3 is REPLACED wholesale by two fixed rotations
  of bone 16's own matrix: `Ry_e · Rz_e · boneMatrix[16]`, row-major
  `[orig: gate @ 0x4b21b6, branch @ 0x4b220f; Rz @ 0x4b2215..0x4b2251 (axisIndex 0 @ 0x4b2240,
  dbl_7C9BA0 = 0.5759761961496483 rad); Ry @ 0x4b2256..0x4b22c2 (axisIndex 2 @ 0x4b2294,
  dbl_7C9B98 = −1.3613982818082597 rad)]`. `Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0` places
  elements exactly as the entity builder's blocks do and is fed `sin = −sin θ`
  (`dbl_7C57B0` @ 0x4b221f/0x4b2273), so each block is a row-vector rotation by −θ.
  **Correction, same day:** this row previously read "a sin/cos wobble … dropped-weapon sprawl",
  and a first pass at re-measuring it concluded DEATH-ONLY. Both are wrong, and the second error
  came from assuming `+0x2C8` holds a `wpn_*` id (rows 240–251, all flag `0x0`). It does not.
  Its writer is `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b5dad..0x4b5ea9]`, which stores
  `AdmDefs[entity+0x2B0].kind + 0x31` — i.e. a HOLD state in the 43–66 band — never 240–251.
  Flag 0x80 in that band is set for **50 `knife`, 52 `grenade`, 54 `designator`, 62
  `knife_attack`, 63 `grenade_attack`, 64 `binoculars`, 65 `reload`, 66 `reload2`** (plus the
  death family 190–239). So the branch fires for every knife, every grenade, an unscoped
  designator, both melee attacks, binoculars, **and every reload of every firearm**. A port that
  omits it is wrong for all of those — most visibly for the `+Y`-long models of §14.4b, which the
  entity frame draws standing on end.
- Sight matrix from bone 15 + `modelDef+0x3E4` offsets. Its two rotations are **unconditional**
  (no flag gate) and now measured exactly: `Rx(−15°) · Rz(−135°) · boneMatrix[15]`
  `[orig: @ 0x4b2308..0x4b2416; dbl_7C9B88 = −2.356266256975834 rad = −135.0°,
  dbl_7C9B80 = −0.2618073618862038 rad = −15.0°]`, with per-component offsets `X − 0.12`,
  `Y − 0.03`, `Z + 0.03` (`flt_7C9B94`, `flt_7C9B90`). Muzzle-flash matrix from bone 14 +
  `modelDef+0x3A4`.

#### 14.4a The attachment builder is the ORDINARY entity placement builder

`Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200` — the function the attach build calls
`@ 0x4b1bf8` — is the same one the **generic (non-organic) render callback** uses to place an
ordinary world object `[orig: BoneCallback_gnrc_World @ 0x4e2912]`, among 50 call sites. That
settles the port question that the AABB evidence alone could not: **a held `gfx3` is posed
exactly like a placed `.3di` at the attach angles**, so any placement path already correct for
mission objects is correct for the weapon, and no weapon-specific frame correction exists in the
original to port. Its shape, read off the decompilation:

- row-vector, row-major; rotations composed **Z(roll) → X(pitch) → Y(yaw)**, then translation;
- the quantized trig stores **−sin θ** (`fsin` × `dbl_7C57B0` = −4194304.0, cosine × `dbl_7C3600`
  = +4194304.0), so each block is a rotation by −θ in row-vector form;
- translation is `(−posY, posZ, posX) / 65536` — the render frame is mission (X east, Y north,
  Z up) relabelled as `renderX = −missionY, renderY = missionZ, renderZ = missionX`.

Already ported verbatim as `world::render_matrix_from_pose` (`engine/runtime/world/occlusion.cpp`).
Under the repo's Godot mapping `godot = (mX, mZ, −mY)`, render and Godot differ by an X↔Z swap,
and a `.3di` vertex reaches our model node through the ADR 0007 `(−x, y, z)` flip, so the exact
relation is `bms_to_godot_basis == swap ∘ Mᵀ ∘ flipX` — two reflections, hence a proper rotation,
and `swap ∘ flipX` factored out IS the trailing `Ry(+90°)`. Expanded, that is
`Ry(heading) · Rz(pitch) · Rx(roll) · Ry(+90°)`: `MissionObjectPlacer.bms_to_godot_basis` term for
term. **The GDScript basis is a faithful image of the original builder.** Pinned executably by
`present_held_weapon_test.gd::test_bms_basis_matches_the_original_placement_matrix`, which rebuilds
the original's quantized-trig row-vector matrix and compares all three model axes over five angle
triples — including two with pitch, yaw AND roll all non-zero, the case a roll-free live sample can
never falsify. Note for future readers: the earlier live measurement had `roll = 0`, so it could not
have detected a pitch/roll axis or composition-order swap; this test can.

#### 14.4b The authored frame of the third-person weapon models

Measured from the shipped assets, not inferred from bounding boxes. `M4_3RD`'s userpoints put
the muzzle flash `MFLASH01` at `(+0.003, +0.089, +0.781)` and the `scope` at
`(−0.002, +0.194, +0.141)`: **muzzle is +Z, up is +Y**, origin at the grip, and every node in the
built scene carries an identity transform so the rendered frame IS model space. A census of all
41 distinct `gfx3` models (the retired `held_weapon_census_probe.gd`, 2026-08 census) finds 33 `+Z`-long and 8
`+Y`-long; the `+Y` set is exactly the melee/throwable/utility items — `M9K_3rd`, `mach_3rd`,
`Flsh_3RD`, `Frag_3RD`, `smok_3RD`, `medp_3rd`, `stch_3rd`, `det_3rd`. **No firearm is `+Y`-long.**
That `+Y` set is **exactly the set of AdmDef hold kinds whose state carries flag 0x80** — knife,
machete, the three grenades, medpack, satchel, detonator. This is the asset half of §14.4's second
frame, and the two halves lock each other in: those models are authored blade-up/handle-down
because retail poses them at the HAND, and only at the hand.

Measured directly at the live attach triple `(pitch 17.95, yaw 13.57, roll 0)`
(the retired `held_weapon_livetick_probe.gd`, which also confirmed no `gfx3` carries live PANM —
the build-time and after-30-frames bounds are identical, so a static measurement is valid):
under the ENTITY frame **every firearm's long axis draws at elevation +17.95°, exactly the attach
pitch, while `M9K_3rd` and `mach_3rd` draw at +72.05°.** The arithmetic is exact and unavoidable —
for `B = bms_to_godot_basis(p, y, r)`, `elev(B·+Z) = p` and `elev(B·+Y) = 90 − p`, so at level aim
a `+Y`-long model stands at exactly 90°. That is the "rotated, often near-vertical" report, and it
is produced by a basis that is otherwise correct term for term: the fault is not the basis but the
MISSING SECOND FRAME. Moving a constant in the entity frame would break all 33 firearms to "fix" 8
items that are not supposed to use it at all.

#### 14.4c ADM slot 0 is a reserved `"null"` row — index 1 is the first `weapon.def` row

`AnimDef_InitAll @ 0x5435c0` zeroes all 255 ADM slots, applies
`AdmDef_InitEntryDefaults` to each, then takes the first free slot — scanning from index 0
`[orig: AdmDef_FindFreeSlot @ 0x53fc50]` — and names it `"null"` `[orig: @ 0x543613..0x543620]`.
Slot 0 is therefore consumed before any `weapon.def` row is parsed, and
`AdmDef_GetEntryByIndex @ 0x53fc80` indexes `AdmDefs[280 * index]` with no bias. So the wire's
equipped ADM index is **1-based over `weapon.def` file order**: index 1 = `WPN_KNIFE`
(`gfx3 M9K_3rd`). This confirms the port's `world::WeaponTable` layout (null row 0, `by_index`
indexing directly) rather than the 0-based file order `WeaponDatabase` exposes — the two
parses do disagree, and only the former may be used for a runtime ADM index.

### 14.5 Open follow-ups

- ~~`dword_3346FA8` (local pitch kick): value source unwitnessed~~ **RESOLVED
  2026-07-09**: the audio mixer's output power meter, renamed `g_audioOutLevel` — full
  verdict in §14.3's local-player bullet. Remaining: the Godot-side port needs a mixer
  output-level tap (the overlay carries the term, fed 0).
- `dword_C6EC38` (view/local entity): reads witnessed only; no direct writer xref (register-based
  store suspected). Name-proposal held until a writer is pinned.
- The InfantryAI leg-chase excerpt re-verify (the session's sub-investigation was cut by a spend
  limit after confirming the §3.1 36-tick re-plant stagger applies per DcbId): the §3.3 math rows
  predate this session and stand; the relabel is anchored by the bone-overlay consumers.
- Complete shipped-data census of which emplacement definitions author each `phrase_set` value remains
  a data-sweep follow-up; the parser, `+0x86c` field, consumer, and branch meanings are witnessed.
- `Entity_GetCameraTransform @ 0x4b8c00` (bone-camera for mode-0 mounted view) not yet decompiled.

### 14.6 Port status (D-INF-11 — LOCAL 2026-07-08; SHARED MOUNTED SELECTOR 2026-07-19; FINAL BN17 ROW 2026-07-21)

**2026-08-21 — the mounted camera legs, PORTED.** The chase camera's
vehicle branch (parentSlot `+0x168` ∈ {2, 5} — our `SeatType`
Controller/Driver, `Entity::is_vehicle_control_seat()`) is wired into
`player_view.cpp` from the constants in `engine/runtime/world/tp_camera_mount.h`
(2026-08-22: and REACHED in play — the mode arbiter `player_view_resolve_mode`
now resolves third person from the chase preference and that same seat test
every tick, so a driver arrives in this leg without any camera key; net-re
§5.39's 2026-08-22 addendum carries the witness):

- **Anchor** `[orig: ThirdPersonCamera_Update @0x437af0]`: the mounted target
  is the CARRIER position + `(0, 0, max(1.0, (24576·boundRadius + 0x8000)
  >> 16))` `@0x437b1f..0x437b4b` — NOT + CameraOffset (the on-foot target) —
  eased `(d+8)>>4` on xy and `(d+16)>>5` on z `@0x437c56..0x437c79`
  (`player_view_tick`'s Q16 `tp_anchor_q16`); dead targets reel the chase
  distance `@0x437cc0..0x437d02`.
- **Eye** `[orig: Camera_ComputeThirdPersonView @0x437d10, the mounted fork
  @0x4380ed]`: distance `1.0 + 1.5·r` `@0x43811b..0x438136`; yaw =
  parentYaw + ((entityYaw − parentYaw) >> 2) `@0x438138..0x43814a` (the
  earlier `@0x4380a3` cite was the shake block); pitch ASSIGNED `0xF8000000`
  `@0x438150`; the water floor `water + 0x4000` only while the entity is
  above it and the eye below `@0x438409..0x43841e`; the terrain floor
  `terrain + 0x4000` skipped under Flags `0x800000` `@0x438422..0x438456`
  (our `kEntityFlagIndoors`); the mounted slope march `@0x43845a..0x438619`
  — 0x8000 steps along the eye−anchor xy unit, `maxSlope` seeded 0.0
  `@0x438599` so the 0.333 margin ALWAYS applies, `eye.z = max(eye.z,
  anchor.z + ftol(maxSlope·dist + 0.333·dist))` `@0x4385f8..0x438619`; the
  WATERCRAFT drop `boundRadius >> 1` on the eye AND the anchor when the
  parent def's `unitType (+0x196) ∈ {3, 4}` `@0x43861d..0x43864c` —
  watercraft (`Entity_ClassifyForMinimap @0x50fa70`: 3/4 → icon 11, 5..8 →
  helo), correcting the staged header's "aircraft drop"; the look-ahead
  `@0x438811..0x4388b5`: target = parentMatrix(+0xB4) × (6.0, 0, 0),
  `g_camera_lookahead += (target − lookahead + 16) >> 5` per axis, the
  look-at through fpatan (`compose_mounted_camera`).
- **Device**: `simulation_player_view.cpp` fills `PlayerViewState::mount`
  per tick (control seat, `carrier_pose_fixed` pos/yaw, bound radius,
  watercraft, the water plane, `carrier_forward` in the yaw-only form — a
  stated fold, the seam reads the fixed heading rather than the chassis
  matrix). Pinned by ctest `tp_camera_mount`, `player_view` (crossover at
  `0.375r == 1.0`, 40° → 10° yaw, the watercraft drop, the slope floor, the
  water polarity, indoors skipping the terrain floor, the Q16 ease rates) and
  GUT `local_player_presenter_test`. The `Camera_SetTrackedEntity @0x4391d0`
  orbit/tracked seeds and the bone-collision forces `@0x4382d9` remain the
  tracked deferrals (net-re §5.39).

The local controller train remains, and mounted selection is now an animation-owned shared seam:

- **Blends + bone map**: `engine/runtime/anim/{include/anim,src}/aim_overlay.{h,cpp}` — exact int32 BAM
  blends of the on-foot aim/non-aim branches, the full §14.3 mounted table, the BN##→class map,
  and the pose compose
  (`apply_aim_overlay`: FK → per-class world delta → back to parent-locals; with parent-local
  poses the pivot re-anchor preserves every local origin, so only rotations change).
  `AimOverlayInputs` owns `MountMode::{OnFoot,Seated,Gunner}` and a separate config-valid/config-value
  pair; a Gunner with unknown config bypasses the config switch, while valid zero enters it.
  `tests/anim/aim_overlay_test.cpp` pins the map, on-foot formulas, and the compose
  invariants (identity = passthrough; uniform delta = world delta; differential = bend).
- **Production metadata + lifecycle**: `engine/formats/def` parses signed `phrase_set` and presence;
  `ItemDatabase` exposes both; `ItemSeatSpecs` and `ItemSeatSpec` promote them to the
  target entity. Both mount entry paths copy the pair to the occupant; both dismount paths clear it;
  registry snapshot/restore value-copies it. This replaces `emplaced_pose_variant`'s ambiguous default
  without changing the existing seat-frame body/leg/pitch/roll synchronization. `Simulation`
  retains the per-entity ADM resolver inputs and advances an append-only AI-entry high-water at the
  direct-spawn and host pre-tick boundaries, so players created after mission-load registration still own
  US01 before the mounted selector reads B50's configured pose (`AnimMap_RegisterEntity @ 0x40bb60`).
  An animation-registry rebuild invalidates every stored `adm_id` and therefore resets/repopulates the
  high-water; Play→Stop restore likewise rewinds the AI array and resolver mark, even when the restored
  entry count equals the prior count.
- **Sim**: `InfantryState.leg_yaw/leg_target` + the §3.3 chase/re-plant/twist-limit tick in
  `engine/runtime/world/infantry.cpp`; the local player's `body_heading` now CHASES the aim
  (quarter-step, clamped) while render yaw stays mouse-instant — the aim/body split the
  overlay renders. `tests/world/infantry_test.cpp::test_player_body_chase_and_legs`.
- **One selector, four consumers**: `Simulation` is the production adapter from
  `AiEntity` + mounted `Entity` state to `AimOverlayInputs`; it calls
  `compute_aim_overlay_angles` for authoritative collision and local pose export. The present snapshot
  packs the resulting body frame and nine absolute overlay angles. `MissionPresentPass` and
  `WirePresentPass` share the result adapter that converts those packed angles to body-relative Godot
  deltas; neither pass reads mount config or repeats the selector. Organic collision still emits final
  bone matrix `i` for COBJ section `i`; CXLT remains independent metadata and is not a selector or extra
  transform.
- **BN17 final-row clipping is derived, not transported**: the terminal §14.1.5
  predicate is deliberately separate from the broader mounted weapon-channel gate.
  Authority requires a live Controller/Gunner/Driver mount and
  `!(Entity.engine_flags & 0x100)`; Passenger and player-classified rows remain
  untouched. The packed `PF_RIGHT_HAND_COLLAPSED` value is presentation-derived
  state, not a new network field. A joiner reconstructs it from the decoded organic
  class (Player is the wire form of Flags 0x100; Infantry is not), carrier handle,
  raw mount bone, and the same production `ItemSeatSpec` table already used by the
  mounted overlay selector. Each compact sample clears carrier/bone defaults first,
  so dismount cannot retain a stale verdict.
- **The clip has consumer-specific matrix semantics**: `SkeletalAnim` applies
  BN17's terminal zero-scale pose after both normal overlay composition and the
  invalid/empty-overlay `eval_pose` fallback while preserving the sampled local
  joint origin. `ObjectModel` collapses the skin there; translating the joint
  to world origin makes mixed-weight triangles stretch across the frame. The
  authoritative collision adapter still bypasses ordinary body-world composition
  for COBJ 16 and emits the witnessed literal all-zero `CollisionMatrix`. Rendering
  and hit geometry share the same retail predicate but adapt it to their respective
  skinning and final-collision-row representations.
- **Local presenter**: `Simulation.get_local_player_aim_overlay()` (BAM→mission-euler once, native) →
  `LocalPlayerPresenter._update_avatar` builds the per-class deltas via the single-sourced
  `bms_to_godot_basis` and sets the avatar node to the BODY frame →
  `ObjectModel.set_aim_overlay` → `SkeletalAnim.eval_pose_overlay`. The 3P camera now
  uses the witnessed 3.0 / 5.625° / ¼-step-anchor numbers (net-re §5.39 addendum; the orbit pitch was misconverted as 22.5° until 2026-07-13).
- **Historical verification**: the retired `bend_capture_probe.gd(.tscn)`
  originally booted ONED play-in-editor, injected F4 + mouse-look, and
  captured poses; look-down bent the spine/head forward and look-up arched
  back (05TR.bms, JOX root). That probe now enters the standalone game through
  `MainGame` and a saved loose mission; ONED no longer hosts PIE.

The mounted slice is verified by table-driven coverage of every witnessed seated/
gunner branch (including unknown versus valid zero), metadata extraction, snapshot/restore and dismount
clearing, local render/collision parity, and placed/wire parity. The BN17 regressions additionally pin
Passenger/Controller/Gunner/Driver selection, the Flags-0x100 exclusion, invalid/empty-overlay fallback,
real Skeleton3D dismount restoration, joint-local render clipping without stretched triangles, and the
separate literal zero row for authoritative COBJ 16. The posed rendered-head-versus-shot case remains
live beside that zero-row check.
The focused gate also reruns the rotated mounted-enemy, CXLT, and F3
cutoff/cadence/dense-batching regressions before the maturity ratchet and full CI matrix.

Remaining under this row: NPC/remote secondary weapon-channel pose composition; weapon/sight/muzzle
attachment matrices; and the pitchBlend / lean / torsoRoll
sources (terms carried, fed 0; pitchKickAccum now carries the PORTED arms-dip feed —
§14.8.5/§14.8.7 — its other producers, the idle look-at and the audio pitch kick,
stay open). The upper-body weapon channel's producer is witnessed and FULLY ported for
the local player (§14.8, 2026-07-09 sessions 1–2). Player-motor approximations are
ledgered as **D-INF-12**.

### 14.7 IDB write-backs (2026-07-08 session, saved)

33 camera-system globals renamed from auto names (`g_camera_mode @ 0xA890C8`,
`g_camera_tracked_entity @ 0xA890CC`, `g_camera_chase_distance @ 0xA8910C`,
`g_camera_orbit_yaw/pitch @ 0xA89104/0xA89108`, anchor/lookahead/lerp/spectator families,
`g_camera_third_person_selected @ 0xA860DF`, `g_cfg_default_camera_mode @ 0x24D20C4`);
entry comments on `0x4b1290` (the bone map), `0x4b25c0` (case table off-by-one), `0x437af0`,
`0x437d10`, `0x4391d0` (+ its real 2-arg signature note), `0x49c073` (view actions),
`0x5ca1d2` (mode arbiter), `0x4b4942` (vehicle-3P pitch halving). **Applied 2026-07-16**
(repo hygiene pass): `output_matrix @ 0xA890C0 → g_camera_anchor_z`, `world_x_1616 @
0xA890EC → g_spectator_cam_x`, `outPos @ 0xA89110 → g_camera_lerp_from_x`, `outMillis @
0x31BFBC0 → g_bam_sin_table_q22`. Still held (struct members, needs struct tooling):
`torsoYaw/torsoPitch → legChaseYawR/L`, `headLookYaw/headLookPitch → legTargetYawR/L`.

## 14.8 Appendix: the upper-body weapon channel — producer half (engine-research, 2026-07-09)

The question (the D-INF-11 "weapon channel" tail declared in PR #213): what plays the
third-person body's weapon-action animations (reload, knife/grenade attacks, weapon-hold
poses), on which channel, keyed how, and how does it sync with the FP weapon FSM
(net-re §5.62)? The consumer half (the §14.1 step-2 mask override) was already witnessed;
this session witnessed the producer. All addresses `Jointops.exe.kong.i64`, imagebase
0x400000.

### 14.8.1 Two channels, one shared update

Every organic entity carries TWO AnimMap channels, allocated together by
`AnimMap_RegisterEntity @ 0x40bb60` from the SAME body `.adm`: the primary (locomotion)
handle at `entity+0x188`, the secondary (weapon layer, §14.1's `animChannelA`) at
`entity+0x18C`. Both bind `channel+44` to the `.adm` slot-0 reset `.bad` (the shared
skeleton bind, net-re §5.40 correction) and share the `.adm`'s state→clip table
(`channel+72`; entry pointer per ANIMNUM, clip at entry+32, variant-ring next at
entry+36). At registration, every NULL state entry is backfilled with entry 0 (the reset
anim) — in the original a state whose `anim_<name>` key is absent plays the RESET clip,
it does not no-op `[orig: the unrolled backfill loops @ 0x40bc24 / 0x40bd2e]`.

Channel state lives on the ENTITY as (deferred, target) dword pairs: primary
`+0x2B8/+0x2BC`, secondary `+0x2C4/+0x2C8`. `AnimMap_UpdateDualChannels @ 0x40b8c0`
updates the SECONDARY first by swapping its pair into the primary's fields, zeroing the
blend-suppress byte `+0x377`, and passing `parentEntity = 0` — the weapon layer never
feeds root motion — then restores and updates the primary normally
`[orig: AnimMap_UpdateDualChannels @ 0x40b8c0]`. `AnimMap_UpdateEntity @ 0x40b5f0` (the
shared body) on a target change re-inits the channel with blend 10 (15 when the target
state has table flag 0x400) and pulls the clip from `channelTable[target]`; a nonzero
DEFERRED state is promoted to target only when the playing clip signals end (channel
flag 0x20000) — the two-step transition machinery §3.4 already recorded.

### 14.8.2 ANIMNUM — one 253-entry name table

`g_animStateNameTable @ 0x8135F0` (renamed this session from `off_8135F0`) is the single
ANIMNUM enum: indices 0–239 are the body states (§3.4's table, now complete: 43 idle,
50–61 the weapon-hold poses `knife, pistol, grenade, stinger, designator,
designator_scoped, P90, P90_scoped, MP7, MP7_scoped, javelin, javelin_scoped`,
62 `knife_attack`, 63 `grenade_attack`, 64 `binoculars`, **65 `reload`, 66 `reload2`**,
67–75 emplaced, 76–110 sit family, 176+ the death matrix), **240–251 the `wpn_*` FP
viewmodel states** (`wpn_reset, wpn_idle(241), wpn_empty_idle, wpn_fire(243),
wpn_recoil, wpn_reload(245), wpn_empty, wpn_switchto, wpn_switchfrom, wpn_switchrank,
wpn_scopeup, wpn_scopedown`), 252 `EOF`. `g_animStateFlagsTable @ 0x8139E8` sits
immediately after (= base + 4×254). The `.adm` binder resolves `anim_<name>` clip keys
against this enum (`AnimMap_FindSlotByName @ 0x40cfa0` scans it) — so the "action→body
key mapping" the port needed does not exist as a translation: US01.adm's `anim_reload` IS
state 65 and ak47.adm's `anim_wpn_reload` IS state 245; the FSM and the body run two
independent producers that share triggers.

Key flags for this appendix: 65/66 = 0x84, 62/63 = 0x94 (both carry **bit 2 = locked**
and 0x80 = weapon-pose family); holds 50/52/54/64 = 0x80; 51/53/55–61 = 0.

### 14.8.3 The FP path plays on the WeaponDef's channel, not the entity's

`ActionSlot_BeginActivePhase @ 0x53f830` and both shims (`ActionSlot_ExecuteActionWithEffect
@ 0x541860`, `ActionSlot_ExecuteActionNoEffect @ 0x5419e0`) play the action's anim slot via
`AnimMap_PlayAnimBySlot(*(WeaponDef+0x174), actionDef+24)` — the **equipped WeaponDef's own
adm channel** (the FP viewmodel rig), gated `owner == g_local_player_entity`. §5.62's
"owner's animadm" phrasing is corrected in place. The WithEffect/NoEffect fork
(`ActionSlot_ExecuteActionTick @ 0x541a70`) differs only in muzzle/particle effect
spawning — third person (`g_camera_mode`), vehicle-attack and remote entities take
WithEffect; the anim play target is identical. Nothing in the ActionSlot family touches
the entity's channels.

### 14.8.4 The body producer — secondary-state selection @ 0x4b5dad

`Entity_UpdateInfantryPlayerBody @ 0x4b40e0` (org2; org1 has its own writer @ 0x4b9a28,
unwitnessed) selects the desired secondary state on the **16-tick slow pass** — NOT each
tick, as this section claimed until 2026-07-26. The gate is `(current_tick & 0xF) == 0`
`[orig: key stored @0x4b4e79, tested @0x4b5d71]`, keyed on the RAW tick (unstaggered,
unlike the org1 `logic_tick + 36*net_id` idiom), and it covers far more than this ladder:
`@0x4b5d77..0x4b637b` also carries `WeaponSlot_DecrementTimer @0x4b5f6f`,
`Entity_ComputeAnimSlotIndex`, `Player_OnDamageReceived`, `Entity_FindNearestThreat` and
the `AudioVM_SetVariable` music block — the weapon channel is one tenant of retail's
general slow pass. Only the flag refresh + ladder + commit sit behind it; the deferred
promotion and the playhead advance stay per-tick, in `AnimMap_UpdateDualChannels`
`@0x40b8c0` ahead of the gate. Observable consequences, all witnessed rather than
approximated: a hold-pose change lands 0-15 ticks (0-242 ms) late, and the 80-tick reload
window is sampled by five passes, not eighty. The selection itself `[orig:
0x4b5dad..0x4b5ea9]`:

1. **Local-player flag refresh** `[orig: @ 0x4b5d7f]`: Flags(+0x24) bits cleared then
   re-set from `g_binocularsRaised @ 0xB7653A` → |8 (binoculars raised),
   `g_weaponScopeActive` → |0x10 (scoped), `g_NVGActive` → |4. The binoculars chain
   (witnessed 2026-07-09 session 2): an input action binding (jumptable `0x4E048B`
   case 26) TOGGLES `g_binocularsToggle @ 0xB76539` unless fire-charging or scoped with
   `+0x168 == 3` `[orig: Input_HandleActionBinding_0 @ 0x4e064c]`, cleared on weapon
   switch `[orig: @ 0x4e11d7]` and round init/reset; each frame
   `Player_UpdatePerFrame @ 0x4de37b` copies toggle → raised, forcing 0 when dead
   (`entity+0x11E <= 0`), when the spawn-success gate is set, or when
   `g_inputFlags & 0x1E`.
2. **Weapon-hold kind**: `kind = dword @ (0x24E8084 + 0x460 × byte entity+0x2B0)` — the
   held-weapon record's dword **+0xA4** (`AdmDefs` base `0x24E7FE0`, stride 0x460, entry
   pointer via `AdmDef_GetEntryByIndex @ 0x53fc80`; the held index is §13.2's
   `entity+0x2B0`). kind 1–4 → states 50–53; kind 5–8 → 54/56/58/60, +1 when scoped
   (the `*_scoped` variants). Default (rifles etc.): MIRROR the primary state `+0x2BC`
   (43 `idle` if the primary is locked/flag-4; 49 `idle_3` when scoped).
   **Data source (RESOLVED)**: the weapon.def key **`special_hold`**, plain `atol`
   `[orig: WeaponDefs_ParseLineCallback @ 0x543cb7/0x543cd8]`; the memset defaults leave
   it 0 `[orig: AdmDef_InitEntryDefaults @ 0x53fef0]`. JOX corpus (94 weapon blocks):
   1 = the knife FAMILY (knife/knife2/medpack/satchel+detonator/claymore/antitank),
   2 = the five pistols, 3 = the three grenades, 4 = AT4/Stinger/RPG, 5 = designator,
   6 = P90/P90AUTO, 8 = javelin; 7 (MP7) unused in JO retail; WPN_RemmingtonSG sets an
   explicit 0.
3. **Overrides**, strongest last: Flags&8 → 64 `binoculars`;
   **`entity+0x372` ≠ 0 → 65 `reload`, or 66 `reload2` when kind == 2 (pistol)**.
4. **Commit** `[orig: @ 0x4b5e72]`: same state → skip. Else if the CURRENT secondary
   state has flag 4 (locked: attacks 62/63, reloads 65/66) or 0x20 (emote) → write the
   desired state to `+0x2C4` (deferred; applied at clip end). Else stamp `+0x2C8` now
   and clear `+0x2C4`.

**Attack stamps** bypass the selection: `WeaponAction_Fire` writes 62/63 directly with
`+0x2C4 = 0` (ebx zeroed `@ 0x542b22`), keyed on the held record's dword **+0xA8**
`@ 0x24E8088 + 0x460×idx` (1 = knife → 62, 2 = grenade → 63)
`[orig: @ 0x542bcb/0x542be0]`. **Data source (RESOLVED)**: the weapon.def key
**`attack_anim`**, plain `atol` `[orig: @ 0x543ce9/0x543d0a]` — JOX ships 1 on the two
knives, 2 on the three grenades, explicit 0 on the launcher/designator family. The net
receive path mirrors the same stamp for remote entities
`[orig: NetPacket_DeserializeRoundEvent @ 0x42f79d/0x42f7ba]`; `NapiNPClientMsg_0x02D
@ 0x427f12` also writes `+0x2C8` (unwitnessed detail, follow-up).

**Rifle-fire verdict (2026-07-09 session 2)**: rifle fire plays NO third-person body
clip — parity means we don't either. The evidence closes airtight: (a) the only
`+0x2C8` writes in `WeaponAction_Fire` are gated on kind-B 1/2; (b) the action clip
plays on the WeaponDef's FP channel (§14.8.3), local-player-gated; (c) the fire path's
remaining anim side effect, `ActionSlot_TryAllocCtrlRegAnim @ 0x401f00` →
`CtrlRegAnimSlot_Allocate @ 0x401ca0` / `CtrlRegAnimSlot_UpdateAll @ 0x401bf0`, drives
the **.3di CONTROL REGISTERS** (a 30-slot table @ 0x85C440 ping-ponging 0..0xFFFF into
`dword_83FCE8`) — a weapon-MODEL visual, never the skeleton. The visible 3P fire
feedback is the §13.1 muzzle-flash draw plus the audio-level pitch kick (§14.3,
resolved below).

**.adm data coverage (JOX sweep)**: `US01.adm` — the player body — is the ONLY body
`.adm` carrying the full hold/attack/binocular key set (all of `anim_knife..
anim_javelin_scoped`, `anim_binoculars`, `anim_reload/reload2`,
`anim_knife_attack/grenade_attack`, 185 `anim_*` keys total). The AI bodies
(Eindo/FSldr/pilots/ESTAND) carry `anim_reload` only; the Cindo family not even that.
With the original's RESET backfill (§14.8.1), an AI body in a hold state plays the
reset pose. The primary body path now mirrors that backfill for local, NPC, remote,
render, and posed-collision sampling; the secondary weapon-channel fallback remains a
separate follow-up.

### 14.8.5 The reload window and the FSM sync

`WeaponSlot_ReloadAmmo @ 0x541720` (the §5.58 refill) stamps **`entity+0x372 = 80`**
(ticks) at entry `[orig: @ 0x54173c]`; the body updater decrements it once per 62.5 Hz
tick `[orig: @ 0x4b5cf9]`. While nonzero the selection wants 65/66; because 65/66 are
LOCKED states, the reload clip always plays to its own end even if the window expires
mid-clip, and the exit transition (back to hold/mirror, blend 10) is deferred to clip
end. There is NO duration coupling to the FSM's reload span (~220 ticks baked from the
`wpn_reload` clip, §5.62): the shared trigger is the reload round-trip — C2S 0x25 on the
FSM reload's first tick → the refill applies on the server handler
(`NapiNPServerMsg_HandleReloadRequest @ 0x514df0`, remote requesters) and on the S2C
0x49 receive (`NapiNPClientMsg_WeaponReload_0x049 @ 0x42c0a0`, the local player) — i.e.
the body reload anim starts ≈ one loopback after the FSM reload begins. Loadout init
(`WeaponSlot_InitFromAvatarDef @ 0x542883/0x542909`) also calls the refill (and thus
stamps the window).

**`entity+0x371` is NOT a clip window** (correcting the §5.58 "3P reload anim" reading):
it is the arms-dip window feeding `pitchKickAccum(+0x36C)` — the §14.2 overlay term. The
exact per-tick block (witnessed 2026-07-09 session 2, `[orig: @ 0x4b5cab..0x4b5ce7]`):
if the window byte is nonzero, decrement it AND `+0x36C -= 0x2800000`
`[orig: @ 0x4b5cb5/0x4b5cb7]`; then the eighth-step ease
`+0x36C -= (+0x36C + 4) >> 3` `[orig: @ 0x4b5cc7..0x4b5cd5]`; then a SECOND
window decrement `[orig: @ 0x4b5cdb..0x4b5ce7]` — the byte drains at 2/tick, so the
20-tick weapon-switch stamp dips for 10 ticks and the 80-tick remote-reload seed for 40.
Seeds: 80 by the 0x49 handler for OTHER players' reloads `[orig: @ 0x42c10b]`; 20 on a
weapon switch — the body tick compares the PREVIOUS held index's record pointer against
the current one (`AdmDefs[+0x370×0x460]+0` vs `AdmDefs[+0x2B0×0x460]+0` — the resolved
anim-def dword, so two weapons sharing an AnimMap do NOT dip) and stamps on mismatch
before latching `+0x370 = +0x2B0` `[orig: @ 0x4b46d0..0x4b4701]`. So a pure client shows
remote reloads as the arms-dip only; the HOST (which calls the refill on its copy for
remote requesters) shows the full 65/66 clip. `PlayerClass_InitEntity
@ 0x4b1182/0x4b115b` zeroes the trio at spawn.

### 14.8.6 Composition (consumer, gates corrected)

In `Entity_BuildBoneTransformMatrices @ 0x4b1290`: the primary channel fills ALL bones
(`AnimChannel_ComputeBoneMatrices @ 0x4b1388`); then, gated on `Flags & 0x100` AND not
gun/ctrl/driver-mounted AND `g_animStateFlagsTable[PRIMARY state +0x2BC] & 0x40`
`[orig: @ 0x4b14a7..0x4b14d1]` — the gate reads the PRIMARY state, not the weapon
channel's — the mask array for bones {3,4,5,6,9,10,13,14,15,16} is set
`[orig: @ 0x4b14db]` and channel A's matrices (its own clip at its own playhead, same
skeleton bind) HARD-OVERWRITE those bones `[orig: second compute @ 0x4b16a7]`. The §14.1
step-3+ overlay multiply and pivot re-anchor then apply to the composed pose — the aim
overlay rides ON TOP of the weapon-channel bones. §14.4 correction: the held-weapon
HAND-FRAME gate — there is no "wobble"; the branch swaps the attachment 3x3 wholesale for
`Ry_e · Rz_e · boneMatrix[16]` (§14.4) — reads the SECONDARY state `+0x2C8` table flag 0x80
`[orig: gate @ 0x4b21b6, branch @ 0x4b220f]`; "the PREVIOUS anim state" was the
`prevAnimStateId` IDB misname (renamed to `weaponHoldStateId` 2026-07-27). That field holds
the weapon channel's CURRENT hold state in the **43-66 band**, written as
`AdmDefs[entity+0x2B0].kind + 0x31` — the DEFERRED one is `+0x2C4`
(`weaponHoldStatePending`). It is NEVER a `wpn_*` id (rows 240-251, all flag `0x0`), and
reading it as one is precisely what made a first re-measurement call the branch death-only.

### 14.8.7 Port status (2026-07-09, this train — local player) and follow-ups

Landed: the secondary channel's state machine in `engine/runtime/world/infantry.cpp`
(`AiSystem::infantry_weapon_channel` — the per-tick selection with the rifle-mirror
default, the 80-tick reload window → state 65, the locked/emote defer rule, the
clip-end deferred promotion via the new `IRootMotionSource::clip_length_ticks`, and the
root-motion-discarding playhead advance; ctest `infantry`
`test_player_weapon_channel`); the refill's window stamp on `Simulation`'s FSM
`reload_applied` event; the typed-record exposure (`PlayerWeaponView.body_anim_key/
body_anim_phase` — same-state channels remain populated because their playheads are
independent; key empty only when the §14.8.6 gate is off); the mask-bone splice
in `engine/runtime/anim` (`kWeaponChannelMaskBones`) + `SkeletalAnim::splice_weapon_channel`
composed in WORLD-rotation space inside `eval_pose_overlay` in the witnessed order;
`ObjectModel.set_weapon_channel` and `LocalPlayerPresenter._update_avatar` consumption.
Live-verified (the `infantry` ctest's retail weapon-channel leg, formerly
`body_reload_probe`; historical ONED PIE,
JOX 05TR, F4 + R through the real input path, completion waited by state): mid-reload
`body_anim_key=anim_reload` playhead advancing, the R-forearm mask bone 73.3° off the
locomotion pose, clean mirror return post-reload.

Session 2 (2026-07-09, same train) closed the kind-dword gap and landed the rest of the
producer: `engine/formats/def` parses `special_hold`/`attack_anim` (ctest `def_parse_weapons`,
the ctypes mirrors that then pinned the layout retired with the FFI, ADR 0038);
`WeaponDatabase` exposes them; `Simulation` refreshes the held kind + the
scoped flag onto the entity pre-tick (`apply_player_input_pre_tick` — the @0x4b5d7f
refresh) and stamps the fire-path attack via `infantry_weapon_attack_stamp`
(`[orig: @ 0x542bbc..0x542bea]`, repeat-stamp keeps the playhead);
`infantry_weapon_channel` now runs the full witnessed ladder — kinds 1–8 → 50–61 with
the scoped +1 variants, the scoped rifle-mirror coercion to 49, binoculars 64
(the `binoculars_raised` input exists and is tested; the shell input toggle is deferred
until a binoculars item exists), reload 65 / reload2 66 by kind==2 — plus the exact
+0x371 arms-dip block (dip-before-ease, double decrement) feeding
`InfantryState.head_look_decay` into the §14 overlay's `head_look_decay` term, seeded
20 on a weapon-switch mount edge. ctest `infantry` (`test_player_weapon_hold_kinds`,
`test_player_weapon_attack_stamp`, `test_player_arms_dip` + the original
`test_player_weapon_channel`). Live-verified (the `infantry` ctest's retail
weapon-channel leg, formerly `body_holds_probe`;
historical ONED PIE, JOX 05TR, NOVA_VM_WEAPON, completion by state): WPN_colt45 —
steady `anim_pistol`, reload plays `anim_reload2`, clean hold return; WPN_KNIFE —
steady `anim_knife`, fire stamps `anim_knife_attack` with an advancing playhead,
locked exit back to the hold; the rifle leg re-run green (mirror at
idle, `anim_reload`, 73.4° mask-bone delta).

**Session 3 (2026-08-17) closed the secondary channel's three tails and threaded the
shared advance onto AI bodies:**

1. **Blend window (D-INF-1 CLOSED).** The secondary channel re-inits through the SAME
   `AnimMap_UpdateEntity` body the dual-channel update routes it through, so it takes
   the primary's blend-10 window (15 for a target with table flag `0x400`), keeps both
   playheads advancing while the float32 weight ramps, and a retarget mid-blend keeps
   the stable outgoing `[orig: AnimMap_UpdateDualChannels @0x40b8c0 →
   AnimMap_UpdateEntity @0x40b5f0; AnimChannel_InitFromParams @0x410640;
   AnimChannel_BlendTwoChannels @0x410740]`. Port: `InfantryState::begin_weapon_transition`
   (mirrors `begin_body_transition`; the `wpn_prev/wpn_prev_clip_phase/wpn_blend_weight/
   wpn_blend_step` trio), the advance in `AiSystem::infantry_weapon_channel_advance`,
   and ONE composition seam for both presentation and authoritative collision —
   `SkeletalAnim::splice_weapon_channel` / `AdmSkeletalClips::splice_weapon_channel`
   evaluate the outgoing/target weapon clips through `eval_pose_blended` before the
   mask splice. The `PF_WPN_SOURCE_STATE / _SOURCE_PHASE_TICKS / _BLEND_WEIGHT`
   present fields mirror the primary's `PF_ANIM_SOURCE_*` trio so wire peers cross-fade
   too. Note the ladder's own outputs (43–66) never carry `0x400`; the slow window is
   reachable only through the rifle-mirror default landing a walk state (`0x449`) —
   pinned exactly by `infantry::test_player_weapon_channel_blend_window`.
2. **RESET backfill.** A weapon key whose clip is absent now plays RESET instead of
   no-opping — the same registration rule the primary already honored (§14.8.1
   `@0x40bc24 / @0x40bd2e`); an EMPTY key (the §14.8.6 gate off) remains the only
   no-splice case. Both splices route through `eval_pose_blended`, which already owns
   that fallback, so the two channels share one rule. Pinned by the new
   `simassets_adm_skeletal_clips_weapon_channel` ctest over the committed BINOC.bad rig
   with a clip SYNTHESIZED in-test (one mask bone and one leg bone turned 90°) — the
   healthy-export trap makes a real-retail-only fixture pass for the wrong reason.
3. **Per-entity BODY-adm variant rings.** `AdmRootMotion` now keeps the RING of tracks
   per state (every quoted token on the row, file order) and `IRootMotionSource` gained
   `variant_count` / `advance_variant` (defaulted, so headless/test providers are
   unchanged) `[orig: AnimMap_ParseConfigLine @0x40cb60; AnimMap_RegisterBoneNode
   @0x40c2d0]`. The secondary channel serves head-then-advance per play through
   `InfantryState::wpn_ring_serve` (per-state heads, the `animState+72` cursor analog)
   and latches the served entry as `wpn_variant` (the `+68` latch), which rides the
   weapon view (`body_anim_variant`), the present row (`PF_WPN_VARIANT /
   PF_WPN_SOURCE_VARIANT`), and both composition seams
   `[orig: AnimMap_PlayAnimBySlot @0x40bda0]`. Pinned by `infantry::
   test_player_weapon_channel_variant_ring` and the ring block of
   `simassets_adm_root_motion`. The PRIMARY channel's `eval_pose_blended`s also
   accept variants now (defaulted 0), so AI body clip rings can ride the same seam
   once their producer serves them. Grilled 2026-08-19: the ring rotate replaces the
   table slot with `entry+36` and re-inits the CHANNEL from the served entry
   `[orig: @0x40b740-0x40b749]`, so every later frame-count/keyframe/trigger read
   runs on that entry's own clip `[orig: frame_count read @0x40b25d; unlerped
   trigger @0x40b32f]` — ring entries may author different lengths, and the served
   entry's length is the promotion clock. `clip_length_ticks` therefore takes the
   variant (defaulted overload), `scan_triggers`/`capsule_bottom_at` accept one,
   and the secondary channel's deferred promotion compares `wpn_clip_phase`
   against the SERVED entry's length (pinned by the `simassets_adm_root_motion`
   ring block).
4. **AI (org1) bodies run the shared advance.** Both body updaters pass their out-array
   to `AnimMap_UpdateDualChannels @0x40b8c0` (§14.8.1, the feet-dip note in §17.4), so
   the org1 body's secondary channel promotes, steps, and blends like anyone's —
   `AiSystem::infantry_weapon_channel_advance` is split out of the org2 producer and
   called for every non-local organic body. What it does NOT do is SELECT: the org1
   secondary-state writer `@0x4b9a28` is unread, and a placed `.bms` soldier's equipped
   ADM source is unwitnessed (§13.5), so running the org2 ladder on AI entities would
   port a different function's behavior. An AI body's channel therefore holds its reset
   state (idle → RESET-backfilled, the arms following the primary — the observable
   retail appearance for AI bodies, whose `.adm`s carry `anim_reload` at most). Ledgered
   **D-INF-24**; pinned by `infantry::test_ai_weapon_channel_advances_without_selection`.

Deferred (later slices): the binoculars input toggle (case-26 binding + forced-clear
rules; the ladder side is ported), the audio-level pitch kick (needs a mixer level tap —
§14.3/§14.5), and D-INF-24.

Follow-ups: `NapiNPClientMsg_0x02D` (+0x2C8 writer) semantics; **the org1 AI writer
`@ 0x4b9a28` + the AI equipped-ADM source (D-INF-24)**; `WeaponSlot_InitFromAvatarDef`'s
spawn-time window stamp (does retail visibly reload on spawn?).

### 14.8.8 IDB write-backs (2026-07-09 session, saved)

Renamed: `off_8135F0 → g_animStateNameTable` (anchored: "reload" string data xref at
index 65, "wpn_idle" at 241 = §5.62's global slot 241, `AnimMap_FindSlotByName` scan).
Entry/line comments: `0x53f830` (FP-channel correction), `0x541720` (+0x372 stamp),
`0x40b8c0` (state pairs + swap protocol), `0x4b5dad` (selection block), `0x542bcb` /
`0x42f79d` (attack stamps), `0x4b14a7` (mask gate), `0x4b21b0` (wobble gate),
`0x8135f0` (table layout). **Proposed, NOT applied** (curated-name policy):
`GamePlayerEntity.prevAnimStateId(+0x2C8) → animStateIdWpn`,
`pad_2c4(+0x2C4) → animStateDeferredWpn`, `+0x370/371/372 →
prevHeldAdmIndex/armsDipTicks/reloadAnimTicks`.

Session 2 (2026-07-09, saved): renamed `byte_B76539 → g_binocularsToggle`,
`byte_B7653A → g_binocularsRaised`, `dword_3346FA4 → g_audioOutLevelStage1`,
`dword_3346FA8 → g_audioOutLevel` (anchored: `AudioMixer_Init @ 0x7bd405` zeroing +
the mixer-loop smoother @ 0x7bef3e..0x7bef55 + the @ 0x4b17f0 consumer). Line comments:
`0x543cb7`/`0x543ce9` (the special_hold/attack_anim keys), `0x4de37b` (the binoculars
gate refresh), `0x4e064c` (the case-26 toggle), `0x7bef3e` (the output power meter),
`0x4b17f0` (the pitch kick resolved), `0x401f00` (control-registers-only fire side
effect).

## 15. World-object collision + blink boxes (engine-research 2026-07-09; re-grilled 2026-07-11)

The runtime consumers of the `.3di` collision block (CDTA: CMDL/BVOL/BPLN/COBJ —
format landed in [3di-gp-format-re.md](../threedi/3di-gp-format-re.md), previously
"no downstream consumer traced"). Ported as `engine/runtime/world/collision.{h,cpp}`
(`CollisionWorld` + the query set), consumed by the infantry motor's vertical
resolve (infantry.cpp step 9 — the D-INF-3 seam) and fed by the binding sweep
`Simulation::resolve_collision_instances`. Evidence ctest: `collision`
(tests/world/collision_test.cpp). All addresses: retail `Jointops.exe`
(`Jointops.exe.kong.i64`, imagebase 0x400000). The 2026-07-11 full re-grill
(the collision extraction slice) walked every ported function against fresh
decompiles: it corrected the port's type-4 contact-frame math (now identified
as CL ladder alignment), type-8 ordinals,
skip-throttle triggers, repulsion gates, proximity cadence, and groundEntity
store (details inline below), added D-COL-9, and extended D-COL-5/-8.

### 15.1 Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Runtime collision records (COBJ 108 B / BVOL 40 B / BPLN 12 B query-side layout) | MATCHING (read-only grill) | field offsets witnessed in all three query functions; `collision` ctest |
| Point-vs-blink query (`collision_test_blink`) | MATCHING | `[orig: Entity_TestCollisionSections @ 0x4aef90]`; `collision` ctest blink cases |
| Segment-vs-solid convex clip (`collision_raycast_model`) | MATCHING | `[orig: Entity_RaycastCollisionModel @ 0x413060]`; `collision` ctest ray cases |
| Contact force + type dispatch (`collision_contact_force`) | MATCHING (core paths; D-COL-2/4/5 tails) | `[orig: Entity_ComputeBoneCollisionForce @ 0x4ae150]`; `collision` ctest push-out/hurt/zones |
| World raycast + ground probes (`CollisionWorld::raycast_ground`) | MATCHING (vertical; D-COL-7 terrain leg) | `[orig: raycast_entity_collision @ 0x413760; Entity_RaycastGroundHeight @ 0x4142c0 / ...AndObject @ 0x414320]` |
| Proximity tables + candidate slices (cadence) | MATCHING (structural; cadence ported 2026-08-26) | statics at mission start/teleport only `[orig: Entity_BuildAllProximityLists @ 0x4c20f0 -> Entity_BuildProximityLists_Pool2 @ 0x4b9430; xrefs Entity_InitAllFromModels @ 0x40e5a1, Game_StartMission @ 0x525c90, the teleport paths]`; pool-0/1 tables every tick `[orig: Entity_BuildProximityLists_Pool01 @ 0x4b9340 <- Entity_UpdateAllEntities @ 0x4c240a]`; slices every 17th tick `[orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0, gate @ 0x4c2416]`. The reimpl had rebuilt the statics table every tick from two whole-registry walks; it now rebuilds on the instance/registry edges that stand in for the retail start/teleport paths and the per-tick walk covers pools 0 and 1 |
| The movement resolver (`CollisionWorld::resolve_entity`) | MATCHING (core; the CL climb legs and CT contact stream ported; deferral D-COL-8) | `[orig: collision resolver @ 0x4b2bd0]` — the §4 open item 5 internals now decoded |
| Blink boxes -> indoors | MATCHING | `[orig: @ 0x4aef90 / @ 0x4aea68-0x4aeae8 / Entity_BuildProximityList @ 0x4b3dc0]`; `collision` ctest blink cases |
| Projectile per-section callback matrices | MATCHING for non-organic effective LOD-0 ordinary/spinner PANM and for pool-0 person current-pose skeletal spheres; camera-derived generic modes remain D-COL-10 | `[orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0; Physics_RaycastAgainstBoneSections @ 0x4e4670; BoneCallback_Generic @ 0x4e26d0; BoneCallback_org0_Bone @ 0x4e34b0]`; strict COBJ ordinal providers, generic shared-DWORD PANM vs per-entity organic sim pose, one final-world composition; native + GUT source/clock/ordinal/person cases |
| Armory/vehicle loadout-zone gates | MATCHING (read-only grill) | `[orig: Input_HandleActionBinding @ 0x49b83d case 218]`; menu side in [menu-re.md](../mnu/menu-re.md) §In-game armory |

### 15.2 Witness map — the query set

- **Runtime records.** COBJ section record (108 B): `+28` volume count, `+32`
  first type-7/12 vehicle-pass volume index (-1 none), `+36` volume ptr,
  `+68..+88` local AABB
  (minX,maxX,minY,maxY,minZ,maxZ), `+92..+100` bound-sphere center, `+104` radius.
  BVOL volume record (40 B): `+0` collidable type, `+4..+24` local AABB (same
  order), `+28` plane count, `+32` plane ptr, `+36` flags. Retail models can
  author TRAILING BVOLs owned by no COBJ (JOX corpus: the Zodiacs, several
  mounted-weapon items, and large buildings carry them); every runtime walker
  consumes volumes only through the per-COBJ `+28`/`+36` runs, so an unowned
  tail is unreachable dead data — a loader/validator must tolerate it (the
  2026-07-20 `threedi_ir_collision_is_runtime_safe` fix — since ADR 0027 the validator is `threedi_3di3_collision_is_runtime_safe` on the raw parse; ctest `threedi_collision_3di`).
  BPLN plane record
  (12 B): `+0` s16 flags, `+2/+4/+6` s16 Q14 normal, `+8` 16.16 distance.
  Witnessed as the common field reads of `@ 0x4aef90`, `@ 0x413060`, `@ 0x4ae150`.
  On-disk BVOL types are ALREADY the runtime types — the remap switch cited in the
  format record is the ModSuperOed writer side, not a JO load step.
- **Transforms.** Per-section 16-dword fixed matrices from the model callback
  (`model+168`; collision header at `model+176`, ready gate dword`[32]`): row-major
  3x4, Q22 rotation rows with `+0x200000` rounding, world 16.16 translation at
  `[3]/[7]/[11]`, `[15]` bit 0 = section disabled. The callback contract is
  strictly ordinal: `callback_matrix[i]` transforms `COBJ[i]`. The ray walk
  advances those pointers by 64 and 108 bytes respectively; COBJ
  `parent_subobject_index`/offset and CXLT are neither matrix selectors nor
  additive collision transforms. `BoneCallback_Simple @ 0x4e2600` duplicates
  entity placement into every slot; the live non-organic PANM provider fills
  animated slots with their current posed matrices. `BoneCallback_Generic`
  dereferences the canonical first RLOD, so collision always evaluates effective
  LOD0 (local PANM when present, otherwise model-level PANM), never the
  render-selected or first-live LOD. `Render_SubmitEntity @ 0x5dad80` stores
  `GetTickCount()` in the shared render DWORD before
  `Model_TransformBoneMatrices @ 0x58e390`; the Generic collision callback
  calls that same transformer. The port mirrors this with one frame-guarded
  32-bit `PanmClock` sample shared by visual/material and collision paths
  (deterministic `logic_tick * 16` only when a direct/headless sim has no
  driving clock). The float multiply follows the witnessed x87 PC53 operand order
  before the one binary32 store/final fixed truncation. `[orig:
  Math_TransformPointWithTranslation22 @ 0x412f60 (translate-then-rotate — the
  inverse application), Math_TransformPointFixedPoint22 @ 0x412e90 (rotate only),
  Math_FixedPointTransformPoint22 @ 0x615810 (rotate-then-translate),
  Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0 (rigid inverse)]`. Husk select:
  entity Flags & 4 -> husk model else graphic `[orig: @ 0x413086 / @ 0x4ae233]`.
- **Blink point query** `[orig: Entity_TestCollisionSections @ 0x4aef90]`: gate
  itemDef type == 5 (building); broad phase |p - entityPos| <= boundRadius+r per
  axis; per section (matrix flag skip) inverse-transform each point; section AABB
  then TYPE-8 volumes only: volume AABB then all-planes-inside test
  `dot(local,n)>>14 + dist - r < 0`. On hit: `g_BlinkFlagsAccum |= flags ^ 6`,
  packed hit `((section & 0x1F) + (pool2_index << 8)) << 12` into
  `g_BlinkHitSlot0..3` (count `g_BlinkHitCount`, cap 4, type-8 ordinal < 16).
- **Segment clip** `[orig: Entity_RaycastCollisionModel @ 0x413060]`: ray record
  {start[3], end[3] (in/out), mid[3], half[3], dir[3] float-normalized 16.16};
  per section: bound-sphere-vs-ray-line reject (project via dir dot >>16, float
  sqrt distance), then TYPE-1 volumes only: volume bound-sphere reject, convex
  clip of [start,end] against the plane run (d = dot>>14 + dist; both >= 0 miss,
  straddle clips at t = d0<<16/(d0-d1) with +0x8000 rounding); entry point = the
  clipped start, transformed back and written into the record end (progressive
  narrowing across sections); mid/half refreshed at exit.
- **Contact force** `[orig: Entity_ComputeBoneCollisionForce @ 0x4ae150]`: args
  (source, points stride-4, radii, n, target, outForce[4], outFlags, mask). Entry
  rejects a target with `Flags & 1` `[orig: @ 0x4ae1bd]`. Mask:
  1 ladder recontact (inflated CL/type-4 test), 2 player (CP/type-19), 8 vehicle
  collision pass (VC/type 7 and type 12
  only, from the section's `+32` start), 0x10 type-12. The type-8 ordinal counter
  restores its section-entry save per POINT (`v118` `@ 0x4ae384/0x4ae4f6`) so a
  volume keeps a stable ordinal across points; the same pattern gates the blink
  query's `faceIdx` (`@ 0x4af0d3/0x4af165`). Solid path: plane test in
  Q21 (`dot>>9` vs `32*dist`), a plane is a separator candidate only when the
  source's PREVIOUS position (savedLivePose) sat outside it within +r margin;
  min-penetration plane wins, second-best assists (added at half when it grows the
  component); per-section force rotated to world; total clamped to
  `sourceBoundRadius << 7` then `(f+16)>>5` (the length ftol is min-clamped by
  `flt_7C19E0 = 2147418112.0` (0x7FFF0000), the shared sqrt-overflow guard on every distance
  in the query set). Type dispatch on containment:
  4 CL ladder frame -> flag 0x1 — the anchor is TWO rotations through the section
  matrix (x/y from `(midX, midY, point-local z)`, z from `(midX, midY,
  maxZ - 1.0u)` `[orig: @ 0x4ae8f2/0x4ae903]`); yaw/pitch are TARGET-RELATIVE —
  `entity.Yaw − ftol(atan2(−ny,−nx) · dbl_7C57B8[−2^31/π])` and
  `entity.Pitch − ftol(atan2(nz, ftol(lenXY)) · same)` `[orig:
  @ 0x4ae938-0x4ae9d9]` — and the 0.375u pull-in uses REAL fsin/fcos of
  `yaw · 2π/2^32` scaled 2^22 truncated, not the quantized dir table `[orig:
  @ 0x4ae9df-0x4aea30]`; plane[0] is read unguarded even for a 0-plane volume;
  5 contact-no-force; 6 CA armory volume -> 0x4; 7/12 vehicle-mask solids;
  8 BB blink accumulate
  (buildings, body/eye points only) -> 0x10; 9 CD door activation touch mask on
  target+692 -> 0x20; 10 CT change-team touch -> 0x200; 11 vehicle-loadout volume
  -> 0x400; 13 CF flag/special-function touch, grounded-on-target only -> 0x800;
  16/17/18 DH/DM/DL damage -> 0x100/0x80/0x40; 19 CP player collision; 1/others solid.
- **World raycast** `[orig: raycast_entity_collision @ 0x413760]`: terrain clamp
  first (`Terrain_RaycastHeightmapHiRes_0 @ 0x60e710`) — SKIPPED when the source
  entity is indoors (Flags & 0x800000); then the source's candidate list
  (`entity+0x1BC` ptr / `+0x1C0` count): broad AABB + ray-line distance vs
  boundRadius, skip Flags & 0x2000001 and candidates standing on the source
  (3-level groundEntity chain), narrow clip via `@ 0x413060`; returns the hit
  entity, the clipped end stays in the record. Ground probes `[orig:
  Entity_RaycastGroundHeight @ 0x4142c0 / ...AndObject @ 0x414320 — renamed this
  session from Entity_RaycastGroundHeight @0x4142c0 / Entity_RaycastGroundHeightAndObject @0x414320]`: ray {x+dx, y+dy, z+zUp} down zDrop,
  return end Z; the AndObject variant stores the hit into entity->groundEntity
  (+0x28). The 5-tap slope sampler and the resolver tail are its callers.
  `Entity_FindNearestByRay @ 0x413af0` is the projectile-side sibling over the
  global static (count `g_StaticProxCount`) + dynamic (`g_DynProxCount`) tables.
- **Proximity tables — the witnessed cadence (corrected 2026-07-11).**
  `Entity_BuildAllProximityLists @ 0x4c20f0` (statics + pool-0/1 together) is
  the SPAWN/TELEPORT path (`Entity_TeleportTeamToSpawn @ 0x43d4f8`,
  `EventAction_TeleportEntityToSpawn @ 0x43e1f2`, mission start) — NOT per
  tick. Per tick, `Entity_UpdateAllEntities @ 0x4c2100` calls the pool-0/1
  builder directly `[orig: @ 0x4c240a]` and the candidate-slice builder only
  every 17th tick: `g_ProxSliceRefreshCounter @ 0xB57C84` (renamed this
  session) increments per tick and `@ 0x4c240f` gates the
  `Entity_BuildProximityListsFromPools` call on `>= 0x10` (the builder zeroes
  it `@ 0x4b8ed0`) — slices are up to 16 ticks stale by design, and BSS-zero
  start means mission starts run 16 sliceless ticks (`collision` ctest cadence
  pin). Content: pool-2 statics quantized `(p+0x8000)>>16` u16 with radius
  padded +111876, buildings first (`g_StaticProxBuildingCount`) then all
  (`g_StaticProxCount`; the `count < 1199` post-increment gate `@ 0x4b94cb`
  means the 1200th write is never counted — effective cap 1199)
  `[orig: @ 0x4b9430]`; pool-0 persons + pool-1 dynamics full-precision
  (`g_PersonProx* / g_DynProx*`), both gated `!(Flags & 1)` (statics are not)
  `[orig: @ 0x4b9340]`; per-entity candidate slices into the 3000-entry
  `g_ProxCandidateArena` with ptr/count at `entity+0x1BC/+0x1C0`
  `[orig: @ 0x4b8eb0]` — pool-0 SOURCES at boundRadius+4.0u, pool-1 SOURCES at
  +6.0u gated on parent-def foliage-attrib 0x20 clear unless def type 1 (the
  attrib gate is on the pool-1 source, not the candidate); dyn CANDIDATES with
  `+0x114` flag 0x4000 are skipped. That field is the round/ammo `noage` bit:
  OpenNova keeps transient rounds in `RoundSim`, outside the entity registry,
  and the witnessed round-to-placed conversion clears noage before the pool-1
  entity is materialized, so registry dynamics satisfy the gate structurally.
  Statics have no extra slack (the +111876 pad absorbs quantization error).
  Port status (2026-08-22): the source builder now uses the exact ItemDef/type/
  attrib predicate and builds all eligible active pool-1 slices; it no longer
  substitutes a `VehicleTraits::physics` gate. Tables carry every modeled
  collision/host-bound entity. Persons' savedLivePose
  stamp site: pool-1 dynamics stamp per tick `@ 0x4c23b8` (+4..+18 → +0x80);
  the pool-0 stamp is NOT in `@ 0x4c2100` — unwitnessed (open follow-up); our
  ResolveState.prev_pos = last-resolve-end is behaviorally equivalent if the
  stamp sits at update start.

### 15.3 Witness map — the movement resolver `[orig: collision resolver @ 0x4b2bd0]`

Closes §4 open item 5 (the 0x11e3-byte internals). Per call (from each motor's
gravity block, EVERY tick — the "every 2 ticks" first reading died with
D-INF-10/§22):

1. **Idle skip-throttle**: full update when the anim-state table bit 0 is set,
   velocity/slide non-zero (slide > 0 or < -420), displaced > 200 from
   savedLivePose (X/Y only), swim flag 0x2000, or every 64th tick; otherwise
   counter 0..10 full, 11..20 skip (revert the caller's gravity displacement,
   zero it, return 0), reset to 10. The revert is PER MOTOR — `pos.Z -= slide`
   x1 for Flags&0x100 PLAYER bodies (matching the org2 `pos += vel` integrate),
   x2 otherwise (the org1 `pos += 2*vel`) `[orig: test ecx,100h @ 0x4b2cd9 →
   @ 0x4b2ce9]`; the resolver's "0x100=mounted" comment gloss is a kong
   misnomer — the kill router (§20) and the AI target filters (§16) key PLAYERS
   on 0x100. Port history: the reimpl deliberately ran x2-for-both while the
   player kept the pre-§22 2-tick cadence; when §22 restored the per-tick
   `-208 + pos += vel` org2 integrate the mismatch leaked +208 per skip tick —
   the standing player's visible rise-and-snap sawtooth — fixed 2026-07-17 to
   the witnessed split (pinned by `test_player_idle_skip_throttle_no_bounce`,
   the motor+resolver coupled seam; the direct-resolver case hand-rolled the
   caller cadence and could not catch the pair diverging). A net
   push-out later in the resolve resets the counter to 0 `[orig: @ 0x4b3773]`.
2. **Per-query state**: blink globals cleared; entity Flags &= ~0x00D00800
   (indoors 0x800000, armory 0x400000, ladder 0x100000, vehicle-zone 0x800)
   plus the `+0x2c` aux bit 0x40 (the CF/type-13 grounded-touch latch, D-COL-9);
   local player clears `g_LocalPlayerBlinkFlags`. A mounted/carried source
   (parentEntity set + alive, or Flags 0x40) suppresses force ACCUMULATION while
   the flag dispatch still runs (`savedPosY`, D-COL-9).
3. **Capsule points** (not on a ladder): 3 points — head (z + collisionRadius -
   halfRadius + 4096), eye (pos + CameraOffset), feet — radii {collisionRadius,
   20480, outerRadius} where halfRadius = capsuleBottom>>4, collisionRadius =
   halfRadius + |capsuleTop - capsuleBottom|/2 (floor 57344 - 2*cr, min 4096;
   both radii floored at 6144). On-ladder recontact: 2 points — head (z + the
   same lift) and feet — radii 25088 both `[orig: @ 0x4b2e1c-0x4b2ed7]`; the
   bodyPitch/bodyHeading sincos chain built alongside multiplies a constant-zero
   length operand (var_34 = 0), so the "orientation" is arithmetically dead in
   the shipped image and both points sit on the entity column (§30).
4. **Candidate loop** over the entity slice: `@ 0x4ae150` per candidate; the
   contact-flag dispatch runs EVEN ON A ZERO-FORCE RETURN (`the goto @ 0x4b2fa5`
   — a pure CL/zone touch still latches; `collision` ctest ladder-contact pin);
   forces accumulate NEGATED; a mostly-vertical negative force is dropped
   (standing pressure `@ 0x4b3010`); while swimming (Flags 0x2000) an UPWARD
   force damps slideDecay toward -167 (-83 steps; `@ 0x4b304e-0x4b308c` —
   rides the D-INF-3 water tail, unported); contact-flag dispatch:
   0x40/0x80/0x100 DL/DM/DH damage -1/-6/-50 HP (authority only `@ 0x4b317b-0x4b31d7`,
   skipped entirely for Flags 0x4000000 sources `@ 0x4b3148`; each hit also
   stamps the damage-source attribution); 0x200 CT change-team/capture touch ->
   `Server_OnPlayerTouchCaptureZone @ 0x500ba0` (def attrib 0x20000, spawn gates);
   0x1 CL ladder (entry-gated: not Flags 2 = not DEAD, and previous ladder
   contact OR the player class bit 0x100 OR the AI climb order aiRuntime[1] &
   0x400; a fresh PLAYER entry additionally needs (above the anchor OR facing
   within 60°/0x2AAAAAA0 of the frame yaw) AND (heightDiff>0)==(Pitch>0) vs
   savedLivePose.Z `@ 0x4b32a5-0x4b32c0`): Flags |= 0x100000 + groundEntity =
   candidate (`@ 0x4b3291`); a fresh entry SNAPS X/Y to the anchor and bumps Z
   +20480 / +39936 (crouch, MoveOrder 0x200) / +60416 (prone 0x100 && !savedPosY)
   below the anchor — breaking the grounded state so the climb can start — or
   snaps to anchorZ − 4096 from above, then re-stages the 2-point capsule at
   the anchor and ZEROES the accumulated force (`@ 0x4b32f7-0x4b3392`); the
   per-tick chase (re-latch and entry alike, `@ 0x4b33a4-0x4b3495`): player yaw
   eases `(delta+8)>>4` into Yaw + bodyHeading (+ g_LocalPlayerLookYaw), AI
   hard-sets aimHeading/targetHeading/Yaw/bodyHeading, bodyPitch = frame pitch,
   +0x2C bit 4 disarmed, the 0.0625u facing press (sincos·4096, `<<12>>22`) plus
   the `(anchor-pos+32)>>6` X/Y chase — PORTED 2026-08-15, §30 (the earlier
   "24576*sincos>>22 facing offset" gloss conflated the extraction pull-back).
   The resolver's player predicate is the class bit at every physics leg
   (`Flags & 0x100` @ 0x4b2cd9 / @ 0x4b2f7c / @ 0x4b3271 / @ 0x4b33aa /
   @ 0x4b3c78) — local and remote bodies alike; only the local side-writes add
   `entity == g_local_player_entity`: the blink mirror (@ 0x4b2d39 / @ 0x4b34ce),
   the `g_LocalPlayerLookYaw` drag (@ 0x4b33ca, so a remote body's heading
   eases without touching the local mirror) and the pitch restore (@ 0x4b3cdc /
   @ 0x4b3cfe). The port's `is_player_class` argument is that flag bit at all
   three motor call sites since 2026-08-24 (the local motor used to pass its
   local-player bit, hiding the ladder legs from a listen host's remote bodies);
   0x4 -> Flags 0x400000
   (armory zone); 0x400 -> Flags 0x800 (vehicle-loadout zone); 0x800 -> the
   `+0x2c` aux 0x40 latch (CF/type 13, D-COL-9); 0x10 blink apply —
   bit 2 of the accum -> Flags 0x800000, local player ORs into
   `g_LocalPlayerBlinkFlags` (`@ 0x4b34c2-0x4b3502`); 0x20 CD door/animated-part
   section-touch callback (candidate vtbl+456)(6,0); walking over a live body plays the def sound
   (`@ 0x4b30da`). itemDef attrib 1 -> `Entity_ProcessWaypointInteraction
   @ 0x4ad820`; attrib 2 + player -> `Entity_InvokeCollisionCallback @ 0x442350`.
5. **Second relaxation pass** at the force-shifted points, adding half the fresh
   X/Y force when no CL contact is active (`@ 0x4b3549-0x4b36ec`); packed blink hits
   copied onto the entity quad (`@ 0x4b36f0` — only when a candidate slice
   exists; sliceless entities keep the refresh-stamped quad). When a push was
   applied and the push direction roughly opposes targetHeading (atan2 gates
   ~8°/~15°), a "blocked" latch is set at `entity pad_368[1]` (`@ 0x4b378a-
   0x4b37bb` — AI-steering consumer unmapped; D-COL-8).
6. **Run-over kill**: pushed by a moving vehicle (itemDef type 1) of another team
   (or MoveOrder 0x200) with push > 10160 on both axes -> Health 0 +
   `Score_ProcessKillEvent @ 0x4fd400` + death anim via
   `Entity_ComputeAnimSlotIndex @ 0x43a690`; crush sound above 1016.
7. **Person repulsion** (no model contact; anim states 27/137/138/139 exempt;
   Flags 0x43 exempt; radius +2.0u under Flags 0x20 `@ 0x4b3aac-0x4b3abc`):
   the pool-0 table SNAPSHOT for the coarse rejects, then the LIVE entity
   position for the second distance + the push (dead/hidden peers Flags 2
   skipped `@ 0x4b3b8d`), threshold 30% of summed radii, push (thr - dist)/4
   along the `(0x200000 - atan2BAM)>>22`-indexed sin/cos pair
   (`@ 0x4b3a5c-0x4b3c52`); the push store here also latches
   `g_ResolverAppliedPush` — "applied a nonzero net push" (slot re-zero
   `@ 0x4b3734`, set on a nonzero X/Y total `@ 0x4b3767`, the repulsion
   store `@ 0x4b3a5c-0x4b3a62`; the separate pass-2 contact bool's only
   reader is the half-force gate `@ 0x4b36da`) — read by the org1
   on-ladder press gate. Leaving a ladder (previous CL flag, not relatched, live player) pushes
   0.375u ALONG +bodyHeading — over the lip on a natural top-out — and arms the
   local pitch restore (+0x2C bit 4, target 4096, g_LadderPitchRestorePrev =
   Pitch); the chase then eases Pitch back at quarter-step, per-tick clamp
   ±0x1E00000, done at target+16, canceled by a user pitch-up past prev
   (`@ 0x4b3c5c-0x4b3d69` — PORTED 2026-08-15, §30).
8. **Ground settle tail**: quantize Z up to the 6144 grid (`(z+6143) & ~0x17FF`),
   probe `@ 0x414320 (entity,0,0,0,0x20000)` (2.0u drop), restore Z, return
   feetZ - groundZ (`@ 0x4b3d6e-0x4b3da9`); the probe's hit lands in
   groundEntity UNCONDITIONALLY (`the +0x28 store @ 0x414370` — null on a miss,
   overwriting even the same-resolve CL latch; generic CB/terrain ground is a
   separate result); callers treat <= 0 as grounded (lift by the return), > 0xF000
   airborne (section 3.5, unchanged).

Blink refresh also runs position-only on spawn/teleport/net-create and per net
position update for remote persons (`NapiNPClientMsg_0x00F @ 0x42e442`,
`NetPacket_HandleEntityCreate @ 0x42f227`, `NapiNPClientMsg_0x02F @ 0x4310ec`);
the per-tick `@ 0x4c229c` walk in `Entity_UpdateAllEntities` is POOL-2 STATICS
on an 8-per-tick stagger with a 62-tick per-entity countdown (`+0x2AC`), not
persons `[orig: Entity_BuildProximityList @ 0x4b3dc0 — one point, radius 0x8000;
def type 1/3 (vehicle/person) walks the entity's candidate list testing
building-kind candidates, def-null/others the global building prefix at
buildingRadius+0x8000; writes the packed quad + Flags 0x800000]`, and the
lighting sampler runs
the same point query per sector sample with an INDOOR result keyed on hit-slot
presence, hit slot -> pool-2 entity (>>20) + section ((>>12)&0x1F) ->
`Lighting_SetInteriorLightGroup @ 0x5a90e0` `[orig:
terrain_sector_compute_lighting @ 0x5c7550 — its "0x8000" is the query RADIUS]`.
Local-player accumulated flags (`g_LocalPlayerBlinkFlags @ 0x24C1934`) are
consumed by the render collectors and frame gates — witnessed 2026-07-16 and
recorded in [render-occlusion-re.md](../render/render-occlusion-re.md) (the
consumer-side record: section masks, portal traversal, occluder culling, the
indoor terrain/sky/water/foliage skips, and the camera-side blink query
`Entity_QueryBlinkBoxesAtPoint @ 0x4af350` over this section's machinery).
Projectiles refresh blink state per tick
(`Projectile_UpdatePhysics @ 0x4e9d70, call @ 0x4e9f21`) and indoor rays skip the
terrain clamp (the `@ 0x413785` gate).

### 15.4 Collidable-type semantics (now witnessed at runtime)

The bundled **Super OED Manual v1.1 §1.1.3.4** supplies the authoring names,
which correct earlier behavior-only labels: CB = Generic Collision Box, CL =
Collision for Ladder, CA = Collision Box for Armory, VC = Collision for Vehicles,
BB = Blink Box, CD = Collision for Doors, CT = Change Team Box, and CP = collision
that affects players but not AI. The project calls CF **Flag**; the same manual
describes that family functionally as “Activates Special Functions (ex: FARPs),”
and the runtime witnesses a grounded-only special-function latch. DH/DM/DL are the
project's Damage High/Medium/Low names, consistent with their ordered -50/-6/-1
effects. Numeric decoding was already correct. These BVOLs remain separate from
the CVRT/CNRM/CFAC polygon mesh used by ordinary bullets.

| Type | Runtime behavior | Witness |
|---|---|---|
| 1 (`CB`) (and unlisted) | generic collision solid — SAT push-out; the ONLY BVOL type generic rays clip | `@ 0x413298`, `@ 0x4aebdd` |
| 4 (`CL`) | ladder contact 0x1 + authored alignment anchor/yaw/pitch; the climb motor rides it (entry gate, alignment chase, states 32–35, dismount/exit legs — §30) | `@ 0x4ae894-0x4aea30`; manual §1.1.3.4 |
| 5 | contact marker, no force | `@ 0x4ae874` |
| 6 (`CA`) | armory volume — Flags 0x400000, gates weapon.mnu on action 218 | `@ 0x4aea45`, `@ 0x49b848`; manual §1.1.3.4 |
| 7 (`VC`) | vehicle-collision solid, selected by vehicle mask 8 | `@ 0x4ae558`; manual §1.1.3.4 |
| 8 (`BB`) | blink box — blink accumulate (buildings), indoors bit | `@ 0x4aea68`; manual §§1.1.3.2/1.1.3.4 |
| 9 (`CD`) | door / door-like moving-part activation — bit per touched section on target+692; the bit index is `sectionIdx − itemDef+2193` (boneMapStart) through a char shift (x86 `shl cl` masks &31) — equal to `si & 31` while the bone map is unmodeled (D-COL-2); retail then invokes the target callback | `@ 0x4aeb0f-0x4aeb22`; manual §§1.1.3.4/1.2.2.9 |
| 10 (`CT`) | Change Team Box touch; downstream capture/team-change request | `@ 0x4aeb7b`, `@ 0x4b31e3`; manual §1.1.3.4 |
| 11 | vehicle-loadout volume — Flags 0x800, gates vehicle.mnu | `@ 0x4aeb92`, `@ 0x49b858` |
| 12 | masked volume (mask 0x10) | `@ 0x4ae568` |
| 13 (`CF`) | Flag (project term); special-function/FARP activation in the manual — latches only while grounded on the target | `@ 0x4aebb3`; manual §1.1.3.4 |
| 16/17/18 (`DH`/`DM`/`DL`) | Damage High/Medium/Low: -50/-6/-1 HP per resolver touch (authority only) | `@ 0x4aeb39/50/67` |
| 19 (`CP`) | Player Collision — solid only on player mask 2, so AI passes through | `@ 0x4ae543`; manual §1.1.3.4 |
| 20..23 | occlusion list (not in the collision walkers) | format record |

The manual officially defines BB suffix `W`/`S`/`V` as preserving water/sky/voxels,
including combinations. The exporter additionally reconstructs `L`/`O`; those two
letter expansions remain unresolved. All five clear bits from initial `0x3E`.
Runtime consumption is witnessed separately: flags accumulate as `flags ^ 6`, and
accum bit 2 (set by an authored bit-1-cleared box) is the indoors trigger (entity
Flags 0x800000). The manual also pins convex-only BBs, a maximum of 16 per model,
and a 0.5 m player detection sphere (§1.2.2.7).

### 15.5 Divergence catalog (D-COL)

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-COL-1 | ~~one yaw-only world matrix shared by every section~~ CLOSED for full-Euler statics and non-organic effective-LOD0 ordinary/spinner PANM. `CollisionWorld::target_view` requests the final array from `ICollisionSectionMatrixProvider`; `Simulation` uses canonical LOD0 only (a nonempty local PANM block wins, otherwise model-level PANM is inherited), scopes liveness to the active transform family, applies current AI controls, and defaults untouched slots to the Simple entity matrix. `PanmClock` samples one full 32-bit process-uptime value per rendered frame for models/materials/collision; direct/headless sims use deterministic `logic_tick * 16`. The fixed→render, pose × entity, render→Q22/16.16 sandwich preserves retail x87 PC53 add order and final truncation. Missing, inert, invalid, or count-mismatched data retains the exact Simple fallback | Generic loads the canonical first RLOD rather than the render-selected/first-live LOD; callback returns one final matrix per COBJ and `callback_matrix[i]` ↔ `COBJ[i]` by `+64`/`+108` pointer lockstep. COBJ parent/offset and CXLT are not selectors or additive transforms; render and collision consume the same GetTickCount-derived DWORD | tilted statics and ordinary/spinner parts collide at their rendered pose. Covered by `collision`, `threedi_panm_runtime`, `simulation_test.gd`, `panm_clock_test.gd`, `mission_presentation_test.gd`. Camera-derived types 3/4 are D-COL-10; pool-0 skeletal zones now use the separately ported per-entity current-pose path (§15.8b), not Generic PANM |
| D-COL-2 | building destroyed/animated section skip not modeled | itemDef+2192/2193 bone map + the `dword_A8A418` state table skips sections (gated !player) | destroyed-wall pass-through pending the destruction system |
| D-COL-3 | **FIXED 2026-08-23:** production models consume GHDR+24's exact Q16 gpm[5]; `items.def scale` parses by the retail `atof × 65536` truncation and feeds visual matrices, collision matrices/inverses, bbox midpoint, movement/proximity, projectile local/wire proxies, and shadow entity bounds. The collision-block gate suppresses the whole bound/center stamp, the base bound is scaled with the signed `+0x8000` multiply before the signed max against the unscaled first husk, then receives +0x1000. Typed wire rows use the same `ResolvedCollisionShape`; the 1u replica compatibility radius and out-param shape API are deleted. | entity+0 boundRadius = max(scale × exact model gpm[5], first-husk gpm[5]) + 0x1000, stamped only when the model carries collision data; bbox center and matrix use the same effective scale [orig: `Entity_InitFromModel @ 0x40dc30`] | native parser/FFI/model/collision/replica/projectile regressions plus GUT wire-pose coverage pin the cutover; only headerless in-memory model fixtures derive a fallback radius |
| D-COL-4 | NARROWED 2026-08-23: the eye test point is the org1 at-rest CameraOffset stand-in built in `collision_resolve.cpp` when the caller carries no offset (h = max(top - bottom, 0x9000), Z = h, lean at rest so X = Y = 0) | eye point = pos + the entity's +0x74 CameraOffset, written by the think before the resolver call (org1 `@0x4b9910` kong 155519-155521; lateral = (3*(h*sin(lean)))>>2 rotated by Yaw) | residual: the live-lean CameraOffset vs the at-rest stand-in; head and eye no longer share a column (the 00TRg wave-3 convoy pin, probe diff 2026-08-23) |
| D-COL-5 | PORTED 2026-08-15 (§30): entry gate + anchor snap/bump, recontact mask 0x1 + the 2-point capsule, the per-tick alignment chase, states 32–35 selection (org2 every-tick override; org1 33/35 select + congestion hold), gravity suppression + horizontal-root zeroing, the ±120° view clamp, the arms lock, the side/back dismounts, the on-ladder jump push, the grounded bottom dismount, the exit push + pitch restore, and the org1 `Flags 0x80` Z-chase gravity variant. Evidence: `collision` ctest (entry/recontact/exit trio) + `infantry` ctest (climb cycle, bottom exit + jump-off, org1 hold/top/0x80) | the same legs `@ 0x4b3245-0x4b3495 / 0x4b3c5c-0x4b3d69 / 0x4b7484-0x4b76d8 / 0x4b7f0c / 0x4b7fba-0x4b8019 / 0x4bf6c1-0x4bf6e5 / 0x4bf917-0x4bfad8` | residuals: the AI move-order WRITER (aiRuntime 0x400 entry orders, `attachParent==self` + `+0x2FC/+0x300` X/Y direct-move chase, MoveOrder 0x100/0x200 AI bump variants) rides the AI-order slice — the org1 legs are dormant until it lands; the carried/parachute halves of the shared 0x100060/0x100020 gates ride their slices; the authority now resolves a snapshot-owned remote player's collision tail, but `remote_player_body_anim` does not yet apply org2's every-tick climb-state override (MP display residual). The earlier "platform/seat/deck carry" description was a terminology error corrected from the Super OED manual |
| D-COL-6 | **FIXED 2026-08-23:** authority pass-0 type-10 contacts are published as exact source/trigger pairs by `CollisionWorld::resolve_entity`; snapshot-owned remote org2 bodies run the same collision tail; `zone_capture_contact_tick` drains the stream into request/presence state with no MoveOrder or radius fallback | `Entity_ComputeBoneCollisionForce @0x4AE150` sets 0x200 at `@0x4AEB7B`; resolver callback gate `@0x4B31DD..0x4B3238`; `Server_OnPlayerTouchCaptureZone @0x500BA0` | Pinned by `collision_test` (authority/dedupe), `infantry_test` (stationary remote body producer), and `zone_chain_test` (authored narrow CT box vs broad gameplay radius) |
| D-COL-7 | vertical ground probe = bilinear column height | `Terrain_RaycastHeightmapHiRes_0 @ 0x60e710` march + bisect | equal for vertical rays on a heightfield (the terrain-re B1 note); oblique rays use terrain_raycast_refined |
| D-COL-8 | run-over kill / crush sound / walk-over-body sound / non-flag attrib-1 waypoint branches + the attrib-2 collision callback / the CD 0x20 door-section vtbl callback / the blocked-push AI latch (pad_368[1]) not ported; the shared ItemDef branch order and the exact attrib-1 flag-family contact producer are ported (2026-08-23); the resolver's player predicate is the class bit (`Flags & 0x100`) at every physics leg for local and remote bodies alike, the local-entity compare reserved for the side-writes (2026-08-24) | steps 4/5/6 above | CD containment and its section mask are detected, but doors/lifts remain operationally inert; needs the animated-object callback plus Score/net + sound + destruction hooks |
| D-COL-9 | mounted-organic cadence and force suppression PORTED 2026-07-20: a live mounted source calls the resolver every eight salted ticks and still processes contact/flag callbacks, but skips model push accumulation when it has a live modeled parent or `Flags & 0x40`; the MoveOrder-0x100 bump and the `+0x2c` bit-4 pitch-restore latch landed with the ladder slice (§30) | `[orig: Entity_UpdateInfantryAI @ 0x4bf5a5-0x4bf5c6]`; `[orig: movement collision resolver @ 0x4b2be0-0x4b2d3f]`; force gates `@ 0x4b3045-0x4b30af` / `@ 0x4b3658-0x4b36b9` | mounted contact phase is live and no longer receives ordinary mover push; the step-up/auxiliary tails landed with the D-COL-5 port (§30) |
| D-COL-10 | PANM rotation types 3/4 are correctly classified as live and routed through per-section matrices, but `ObjectData::evaluate_panm` currently passes an identity `view_inverse` | retail types 3/4 derive their matrix from the current global inverse-view matrix in `PANM_BuildNodeMatrices` | camera-facing/upright billboard parts can have a camera-relative visual/collision pose mismatch; no committed collidable type-3/4 witness yet. Requires sharing the render camera matrix beside `PanmClock` |
| D-COL-11 | `LiveRound` has no BB/indoors state; projectile terrain arbitration only has the ammo-flag bypass | retail refreshes each projectile's blink state per tick and skips the terrain clamp while the round is indoors (`Projectile_UpdatePhysics @ 0x4e9d70`, refresh call `@ 0x4e9f21`, terrain gate `@ 0x413785`) | a shot inside an underground/interior BB can falsely hit the terrain heightfield. Port after the projectile probe radius/state lifetime is pinned; do not guess from the player’s 0.5 m BB sphere |

**D-INF-3 status**: the horizontal capsule + object standing now land through
this port (walls push out, roofs carry via the model-aware ground probe), and
the ladder locomotion tail landed with the D-COL-5 port 2026-08-15 (§30), and
the swim transitions landed 2026-08-24 (#566 — §29.1 + `world/infantry_water.cpp`,
states 36-40 through the 8-case jumptable `@ 0x4b7411`); the remaining D-INF-3
tail is the wash overlay 27/28, the submerged scope auto-untoggle, the resolver
swim damping and the forward-speed question (ledger row).

### 15.6 The armory / loadout-zone flow (cross-record pointer)

Input action 218 `[orig: Input_HandleActionBinding @ 0x49b83d]`: Flags 0x400000
-> `UI_OpenMenuScreen("weapon.mnu", "WEAPON") @ 0x49b8e3` (+ latch
`g_WeaponScreenOpen @ 0x24C1884`), Flags 0x800 -> vehicle.mnu VEHICLE
(occupancy checks via groundEntity+0x162), case 221 -> cmap.mnu CMAP deploy.
The WEAPON screen wiring, population and ACCEPT apply are recorded in
[menu-re.md](../mnu/menu-re.md) (its §In-game armory record rides the armory
slice). Reimpl here: the collision-side zone gate
(`Simulation.local_player_in_armory_zone`) and the sim apply seam
(`apply_local_player_loadout`); the WEAPON-screen UI host and the GameWorld
viewmodel re-mount ride the armory slice.

### 15.7a IDB write-backs (2026-07-11 re-grill, saved)

Rename: `g_ProxSliceRefreshCounter @ 0xB57C84` (ex `dword_B57C84` — the 17-tick
candidate-slice cadence counter). Comments: the cadence gate `@ 0x4c240f`, the
statics 1199 count saturation `@ 0x4b94cb`, the per-point type-8 ordinal
restore `@ 0x4ae4f6`, the unconditional groundEntity store `@ 0x414370`, the
push-resets-skip-counter `@ 0x4b3773`, and the target-relative ladder
yaw/pitch + real-sincos pull-in `@ 0x4ae938`. Confirmed no drift: the 0x4142c0
rename from the 2026-07-09 session is intact (a stale Hex-Rays cache had shown
the auto name).

### 15.7 IDB write-backs (2026-07-09 session, saved)

Renames (auto names -> anchored): `Entity_RaycastGroundHeight @ 0x4142c0`,
`Entity_RaycastGroundHeightAndObject @ 0x414320`, `UI_OpenMenuScreen @ 0x54e520`
(ex "renderer init" misnomer), and 35 globals — the blink set
(`g_BlinkFlagsAccum @ 0xB57C70`, `g_BlinkHitSlot0..3 @ 0xB57C74..80`,
`g_BlinkHitCount @ 0x82AE20`, `g_LocalPlayerBlinkFlags @ 0x24C1934`), the
proximity tables (`g_StaticProx* / g_DynProx* / g_PersonProx*`, counts
`g_StaticProxCount @ 0xB52FD0`, `g_StaticProxBuildingCount @ 0xB4D20C`,
`g_DynProxCount @ 0xB4D208`, `g_PersonProxCount @ 0xB52FD4`,
`g_ProxCandidateArena @ 0xB57C90` + used), the ladder-contact anchors
(`g_LadderContact* @ 0xB5AB70..80`, `g_CollisionQueryIsPlayer @ 0xB5AB84`),
and the screen latches (`g_WeaponScreenOpen @ 0x24C1884`, `g_VehicleScreenOpen
@ 0x24C1890`, `g_CmapScreenOpen @ 0x24C188C`). Entry comments on the ground
probes, pool builders, blink query, action-218 gate, the ACCEPT handler, the
WEAPON registration, and the type-6/11 dispatch sites. Applied 2026-07-16
(repo hygiene pass): `collisionModel @ 0xB52FD8 -> g_StaticProxEntity`,
`result @ 0xB5AB78 -> g_LadderContactX` (corrected from the earlier
CL-as-platform misread; recorded 2026-07-19, IDB write applied 2026-07-20).

IDB addendum (2026-07-20 PR-276 validation grill): the full ladder-anchor set
applied — `g_PlatformContact{Pitch,Yaw,X,Y,Z} @ 0xB5AB70..80 ->
g_LadderContact*`; `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0 ->
Entity_MovementCollisionResolver` (the record's "movement collision resolver"
demotion made symbol-real); comments refreshed at the CL frame build
(`@ 0x4ae894`, `@ 0x4ae938`) and the resolver's CL latch (`@ 0x4b3291`).

IDB addendum (2026-07-16 hygiene pass): `Entity_ComputeWeaponFirePositions @ 0x455ef0`
renamed `AIEntity_ReleaseFlareCountermeasures` — re-witnessed this session: it swaps the
entity's ammo index to `FLARE`/`GROUND_FLARE` (renderInstance type 2 selects the ground
variant), fires one round per AI fire slot via `Weapon_FireProcess @ 0x53f5b0`, then
restores the original ammo; a countermeasure dispenser, not a generic fire-position
helper. The two `ai.cpp` citations (the file has since split into `engine/runtime/world/ai_system.cpp` and its siblings) updated in the same commit.
Body detail (2026-07-16): while `brain[9]` (the flare timer, not a weapon timer) > 0 it
fires `FLARE` / `GROUND_FLARE` (`profile+16 == 2` selects GROUND_FLARE; ids cached in
`dword_B21F84/B21F88` via `AmmoDef_LookupByName @ 0x409870`) from the brain's fire-point
array (`brain[89]` count, `brain+360` point ptrs `{pos xyz, dir xyz; dirZ 0 → 24576}`),
each transformed by the entity matrix → yaw/pitch → `Weapon_FireProcess`;
`equippedAdmIndex` saved/restored around the volley.

### 15.8 The projectile face raycast — the CFAC "bullet LOD" (engine-research 2026-07-17)

Rounds do NOT hit the BVOL volume solids the movement/LOS queries walk — they hit the
collision block's **CFAC triangle mesh**, per section. Ported as
`collision_raycast_faces` + `CollisionWorld::raycast_entity_faces`
(engine/runtime/world/collision_query.cpp), consumed by the RoundSim item leg; ctest `collision`
(`test_face_raycast*`).

- **Dispatch** `[orig: Projectile_UpdatePhysics @ 0x4e9d70]`: the 5-way closest-hit
  switch — terrain (`Terrain_RaycastHeightmapHiRes_Thunk`), water
  (`Projectile_CheckWaterIntersection @ 0x4e59d0`), pool 2 then pool 1 via
  `Projectile_RaycastProximitySlots @ 0x4e5340` (hit types 1/2 →
  `Projectile_HandleEntityImpact @ 0x4e9390`), and the pool-0/3 proximity list
  (`Physics_RaycastAgainstProximityList @ 0x4e4a30`, min radius 0.1u `@ 0x4ea263`,
  the `g_FatBullets` 0.1u floor for remote players).
- **Broad phase** `[orig: Projectile_RaycastProximitySlots @ 0x4e5340]`: per prox slot,
  per-axis |center − rayCenter| ≤ radius + halfExtent, then perpendicular
  line-distance ≤ radius; skip Flags & 0x2000001 and the shooter chain (ray[17..20]);
  survivors run the face walk. (The `+533` refNum gate is NOT here — re-read
  2026-07-20: it lives in the narrow phases, split by channel, below.) The
  function's DEFAULT slotType (neither 1 nor 2) walks the PERSON prox table
  through this same face walk — the consumer set of a person model's CFAC mesh:
  the knife kill zone (`Weapon_RaycastAndSpawnImpact @ 0x4e8460`, all three slot
  legs), the NVG laser (`Entity_RenderNVGLaserBeam @ 0x5c6090`), wreck/falling/
  shell physics (`@ 0x445500/0x4472f0/0x4482a0`), `compute_clamped_displacement
  @ 0x4ad6a0`, and `raycast_proximity_entities @ 0x538350`. Ordinary bullets
  never see person CFAC — their person leg is the bone-sphere pair above.
- **The refNum self-site gates** (witnessed 2026-07-20; ported in
  `trace_projectile`): the mission-authored refNum group (BMS byte 153 →
  entity+533, D-NET-94) suppresses hits through two DIFFERENT reference
  channels. The ITEM face walk skips a candidate whose nonzero refNum equals
  the **ray[18] mount exclusion's** refNum, null-guarded
  `[orig: @ 0x4e4d40]` — a mounted shooter's rounds pass through the site items
  sharing the carrier's group. The PERSON sphere walk skips a candidate whose
  nonzero refNum equals the **ray[17] shooter's** `[orig: @ 0x4e4688-0x4e46a3]`
  — authored same-group persons are immune to each other's fire; retail derefs
  a null ray[17] unguarded there (a nonzero-refNum person vs an ownerless
  flags&4 round would crash retail), so the port requires a live owner.
- **The face walk** `[orig: Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]`: model =
  `(Flags & 4 && huskModel) ? huskModel : graphicModel` (the husk swap again).
  Its callback-matrix and COBJ cursors advance +64/+108 bytes in lockstep:
  `callback_matrix[i]` always pairs with `COBJ[i]`, independent of COBJ
  parent/offset or CXLT metadata. Per COBJ section — skip
  `(boneMatrix+60) & 3`, invert the section matrix
  (`Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0`), transform the segment + unit dir
  into bone-local space; per 44-B face — AABB reject, flags & 0x100 never-hit,
  material 17 skip when the ammo carries flag 0x4000000, plane sides
  d = (v·n_Q14 >> 14) + dist at both endpoints (straddle required), direction rule
  (flag 1 = both sides; 0x800 = double-sided — enabled by a witnessed UNINITIALIZED
  stack-slot read `[esp+12Ch]` that is nonzero in practice; else enter-front
  d0>0 ∧ d1≤0), hitDist = |d0|·len/(|d0|+|d1|) (`"Rounds Divide Error"` log +
  0x40000000 clamp on the overflow guard), accept at ≤ best, and the odd-even
  point-in-triangle (`Math_PointInTriangle2D @ 0x414050`: Q8 int16 vertex table << 8,
  the CNRM projection-axis flag 1=XY/2=XZ/4=YZ, crossings capped at 2, inside == 1).
  The hit record: ray[21] = face flags, ray[22] = face MATERIAL byte, ray[28..33] =
  hit/dist/entity/bone/face.
- **The impact effect** `[orig: Projectile_HandleEntityImpact @ 0x4e9390 →
  AmmoDef_ProcessImpactEffect @ 0x40a170]`: effect index = face material + 4
  (`ray[22] + 4 @ 0x4e982b`; the ammo effects_table of §17.4), plus the slot-2
  local-player-hit feedback when the victim is `g_local_player_entity`. JO data:
  metal props author material 14 → tag 18 `metal` (Mbarel1X: 20×14 + 8×1 faces).
- **The runtime arrays** `[orig: Threedi_BuildCollisionModelFromChunks @ 0x5b3bf0 —
  ex ThreediGp_BuildCollisionModel, renamed (the chunked form is 3DI3)]`: one arena;
  8-B Q8 int16 vertex records, 8-B Q14 normal records keeping the dominate-axis
  word, 44-B faces — the CFAC copy PERMUTES the disk AABB (sequential min_xyz/max_xyz
  at +12..+32) into the runtime per-axis interleave (minX@+12, maxX@+16, minY@+20,
  maxY@+24, minZ@+28, maxZ@+32); BVOL AABBs permute the same way. COBJ dword 3 =
  the per-object CNRM run count (the parse-struct `num_planes` misnomer renamed
  `num_normals`; faces' `normal_index` is local to that run).

Residuals live as D-ITEM-1 (§24.7): the sphere stand-in for face-less models and
the prox-slot tables themselves (we scan the pools). The `+533` refNum gates are
PORTED 2026-07-20 (both channels; `collision` ctest `test_refnum_group_immunity`).

Person-model geometry roles (witnessed 2026-07-20, settling the F3 phantom-sphere
question): the whole-player broad sphere is the ENTITY `+0` boundRadius stored per
person prox slot (`Entity_BuildProximityLists_Pool01 @ 0x4b9340` copies entity+0;
consumed by `Physics_RaycastAgainstProximityList @ 0x4e4a30` as slot radius +
extraRadius) — it is NOT a COBJ. The bone-sphere walk reads ONLY each COBJ's
authored mid (+92..+100) and radius (+104); COBJ min/max (+68..+88) are never
read on the person path. The trailing CFAC mesh row (Indo01 COBJ 19) authors
radius 0 + sentinel inverted bounds — a JOX corpus sweep (953 collision models)
shows sentinels appear ONLY on rows owning zero BVOLs (0 of 2167 BVOL-owning
rows), i.e. the exporter folds only the BVOL run into its bounds accumulator and
rows with nothing to fold keep the untouched init. At radius 0 the row's only
narrow-phase footprint is the extra+0xCCC floor at its (0,0,0) mid (inside the
pelvis sphere); its 4630 faces serve the knife/NVG-laser/generic-ray consumers
listed under the §15.8 broad phase, never bullets.

**Port status (2026-07-20): FIXED.** The person-section primitive, live projectile
path, and F3 diagnostic walk no longer discard an authored radius of zero before
the shared effective-radius calculation. The `collision` ctest pins the exact
floor boundary (0.049u hits, 0.051u misses), the trailing mesh row's dead-center
primary ordinal, and the absence of the former large vertex-derived sphere.

Person-model subobject construction (US01 read 2026-07-20): a character `.3di`
authors `.bad bone count + 1` subobjects on BOTH the render and collision sides.
Subobjects 0..N-1 pair 1:1 BY ORDINAL with the `.bad` bones — US01's 20-part /
20-COBJ layout against the 19-bone `Dt1rst.bad` (`anim_reset` of `US01.adm`)
reproduces the bone parent chain exactly (Hips→Spine→…→Head, both sides) — the
render parts are empty transform nodes (the skinned body's primitive batches
hang off part 0/Hips) and the COBJs are the authored hit spheres. Subobject N is
the WHOLE-BODY row: render side an empty node at the model ground point carrying
the model bound radius (US01 part 19: r 1.019 @ y −1.02), collision side the
CVRT/CNRM/CFAC mesh container above. At runtime `BoneCallback_org0_Bone
@ 0x4e34b0` converts COBJ-COUNT matrices (`modelData+104`) straight out of the
renderer's per-part pose array (`Entity_BuildBoneTransformMatrices @ 0x4b1290`),
so slots 0..N-1 carry live skeletal poses while slot N — bone-less — keeps its
authored rest transform under the entity placement (which is why the derived
phantom sphere sat un-animated at the feet).

### 15.8a The hit-chain re-grill (grill-ida 2026-07-18)

Full-chain verification pass over the §15.8 port after an in-play "hit detection
seems weird" report. Format layer CLEARED: the OED writer stores CVRT/CFAC in
internal space with NO per-COBJ rebase, the runtime builder copies them
unmodified (the AABB interleave is layout-only), the COBJ `offset`/CXLT pivot
fields are copied but never read by the raycast `[orig:
Threedi_BuildCollisionModelFromChunks @ 0x5b3bf0]`, and a JOX data probe
(Chair03X/Cbunker1: every COBJ's CVRT run centroid lands ON its header offset)
proves the vertex runs are MODEL-space — so the shared entity matrix per
section is structurally right for the nonanimated `BoneCallback_Simple` path,
and our IR consumption
(`collision_model_from_ir`, disk `dominate_axis` word kept) is faithful.

CXLT is collision metadata, not a per-COBJ additive transform. Retail keeps it
at collision +0x70/+0x74, while `Physics_RaycastAgainstBoneCollision @ 0x4e4cb0`
consumes callback-produced render-bone matrices instead. Its matrix pointer
advances +64 while its 108-B COBJ pointer advances +108 in the same loop, proving
the strict `callback_matrix[i]` ↔ `COBJ[i]` mapping; no parent, offset, or
CXLT field participates in selection. `JetSki.3di` pins the
distinction in `threedi_collision_ir`: its one CXLT equals COBJ 1's offset, but
that COBJ's CVRT run is already in model space, so applying CXLT would
double-shift it. `DCHNK1` (five CXLT, one COBJ) also rules out positional
association by index.

Face-walk axes re-verified MATCHING against `[orig:
Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]`: the AABB interleave order,
flags 0x100 never-hit, material-17 vs ammo 0x4000000, Q14 plane sides with the
strict straddle, the flag-1/0x800/enter-front direction rule (the witnessed
uninitialized `backfaceCullFlag` arg), `|d0|·len/(|d0|+|d1|)` with the
`"Rounds Divide Error"` 0x40000000 clamp, accept `<=` best, the rounded
hit-point stepping, and the odd-even point-in-triangle on the CNRM
dominate-axis plane. The husk model pick (`Flags & 4` + fallback) and the
transform chain match. Static placement uses D-COL-1's full-Euler entity matrix;
live non-organic effective-LOD0 PANM runs the retail float sandwich (fixed
entity `@ 0x611080` → x87-PC53 row-vector pose × entity → final
Q22/16.16 `@ 0x611140`) through `ICollisionSectionMatrixProvider`
and the production `Simulation` binding. Local LOD0 PANM suppresses
the model-level fallback even when inert; LOD1+ never drives COBJ. The visual
model and collision provider consume one frame-sampled 32-bit presentation
clock. Pool-0 persons use the separate
`Physics_RaycastAgainstBoneSections @ 0x4e4670` sphere walk, now ported through
the same section-matrix provider; its organic callback is current-pose and
per-entity rather than the generic PANM clock described above (see §15.8b).

Divergences found and FIXED this session (engine/runtime/world/round_sim.cpp):

- **The exclusion set** `[orig: the ray[17..20] build @ 0x4ea2a5-0x4ea2f8]`:
  beyond the shooter, the original skips the shooter's MOUNT when the seat
  class is Controller(2)/Gunner(3)/Driver(5) — a Passenger(1) fills no slot,
  so a passenger's rounds CAN hit their own vehicle — and for a Gunner also
  the mount's standing-on carrier (`mount+40` groundEntity). Ported; the
  fourth slot (`projectile+388` ← the fire request's +40, an uninitialized
  extra on the client path `[orig: RoundData_SpawnRound @ 0x4ec0d0 [97]]`)
  stays an open witness (D-ITEM-11).
- **The terrain tie-break** `[orig: @ 0x4ea50e/@ 0x4ea54e]`: pool hits accept
  only STRICTLY closer than the terrain/water winner — ours let entities win
  ties; flipped to `<`.

Divergences found and LEDGERED (new §24.7 rows): D-ITEM-12 — the round
ballistics layer is absent (gravity −167/tick `@ 0x4eaa5a`; the
`g_ProjectileDragTable @ 0xB7B300` power-law drag `[orig:
Entity_ApplyDragAndBounceForce @ 0x4e5ec0 / Projectile_InitDragTable
@ 0x4e78d0]` with wind, the 25× underwater multiplier, the reversal clamp and
the low-speed tumble kick; the water hitType-4 leg + the underwater slow-kill
`@ 0x4ea13e`). D-ITEM-13 retains the terrain sub-stepping and non-person round
parking/offset residuals; its person bone-SECTION leg closed in §15.8b.

Tooling landed with the grill (developer window, not witnessed behavior): the
F3 **Rounds** tab + "Show round trails" world view over a new persistent
`RoundSim` debug ring (`RoundDebugEvent`, cap 48) recording every resolved
outcome — including face-miss fly-ons — exposed via
`Simulation.get_round_debug()`.

Session 2 (same day, after an in-play "shoot through a building wall" report):
the item-leg broad phase was still our segment-vs-sphere — a boundary-crossing
test, so a tick segment entirely INSIDE a big bound sphere (standing near or
inside a building) skipped the entity and rounds crossed its walls untested.
Replaced with the witnessed gate — per-axis |center − rayBoxCenter| ≤ radius +
halfExtent, then the UNCLAMPED perpendicular line distance ≤ radius `[orig:
@ 0x4e53d4-0x4e554a]` — and the face-less sphere stand-in now hits at t = 0
from inside. D-ITEM-13(d) closed; ctest `collision`
`test_round_inside_bound_sphere_hits_wall` pins it. (The JOX face census that
ruled out data-level transparency: 951 models, 618k faces, dominate_axis
always ∈ {1,2,4}, zero misaligned vert runs, 27 zero-normal faces that cannot
straddle in retail either.)

Session 3 (same day, from a user pose dump on 00TRg): the pose-replay probe
(the retired `pose_replay_probe.gd` + `Simulation.debug_spawn_round`, a
diagnostic injector through the real `RoundSim::spawn`) reproduced a
deterministic through-shot — a 2° aim change at a rock formation (RckS07,
bms 1484) flipped a stone face hit into a clean pass-through. Root cause:
the placement authors **pitch −355 / roll 19**, the visual leans with it, but
the collision matrix was yaw-only (D-COL-1) — the shell stood upright ~2.5 u
off the visible surface. Ported the full placement matrix
`[orig: Math_BuildFixedPointMatrixFromEulerAngles @ 0x613f40 — Rz(90−yaw)·
Ry(−pitch)·Rx(roll), Q22 rows with per-product >>22 truncations and exact-zero
stage skips; the spawn euler pack (90−yaw, +pitch, +roll) @ 0x40eb66-0x40eba6]`
as `collision_matrix_from_euler`, used by `target_view` whenever a static
authors pitch or roll (pure-yaw keeps the table path bit-for-bit). D-COL-1's
tilted-static clause closed; ctest `collision` `test_face_raycast_rolled_entity`.
The residual near-silhouette pass-throughs are AUTHORED: the CFAC bullet mesh
is a deliberately coarser bake than the render mesh (RckS07: 146 collision
verts vs 268 render), and with the witnessed enter-front backface rule a ray
threading the mesh's open underside or a silhouette gap exits without a front
face — retail-identical behavior, visible as red graze rings in the F3 Rounds
view.

### 15.8b Organic/person posed bone sections (engine-research + port 2026-07-18)

The pool-0 person path is not the CFAC face walker and does not use an organic
body cylinder. `Physics_RaycastAgainstProximityList @ 0x4e4a30` sends each
surviving person to `Physics_RaycastAgainstBoneSections @ 0x4e4670`. Its
`BoneCallback_org0_Bone @ 0x4e34b0` calls
`Entity_BuildBoneTransformMatrices @ 0x4b1290`, then
`Math_FloatMatrixToFixedPoint22 @ 0x611140`; the callback output is already the
FINAL world-space pose, including the current primary animation, body/aim
overlays, any live secondary weapon channel, pivots, and entity placement.
The port mirrors that boundary in `Simulation::build_section_matrices`:
each organic entity owns its ADM source and playhead, the pose is sampled
synchronously from simulation state (including headless authority), FK is
resolved against the canonical shared rest, and the entity transform is
applied exactly once.

Mounted organics use the same `AimOverlayInputs` adapter and
`compute_aim_overlay_angles` result as local pose export and the packed
Mission/Wire presentation snapshots. Collision has no mount-config heuristic
of its own. Dismount clearing therefore changes all consumers together on the
next sample, while the authoritative primary section and incoming direction
continue through the unchanged reaction/death selection path below.

The pairing rule is strictly ordinal: `COBJ[i]` consumes `boneMatrix[i]`.
COBJ's parent/offset fields and CXLT are ignored by this organic person path;
they are neither selectors nor additional transforms. This is the same
lockstep discipline as the generic face path but a separate retail callback.
The COBJ center and authored radius therefore remain meaningful even when a
skeletal row has no BVOL or CFAC payload.

The fixed-point section walk is now reproduced by
`collision_raycast_person_sections`:

- Walk COBJ sections from highest ordinal to zero, skipping a bit set in the
  entity section mask. Transform the authored center by that section's current
  final matrix. Projection onto the finite fixed16 ray is unclamped and
  unrounded; reject only projections outside `[0, rayLength]`. The Q22 center
  transform rounds with `+0x200000` before `>>22`, each closest-point axis uses
  `+0x8000` before `>>16`, and the Euclidean distance truncates toward zero.
- Effective radius is `extraRadius + 0xCCC + authoredRadius * scale / 100`,
  with scale 65 for head section 14 and 45 otherwise. Sections 15/16 cap the
  final effective radius at `0x3000`. `extraRadius` is the ammo bullet-radius
  feed (the retail fat-bullet floor is a separate caller policy).
- The first accepted section in the reverse walk — therefore the highest
  overlapping ordinal — sets primary `ray[31]`, material 19, and
  `ray[29] = projection - (authoredRadius >> 1)`. Every accepted overlap
  overwrites secondary `ray[32]`, so it ends as the lowest overlapping
  ordinal. The walk deliberately does NOT stop after its first bone. These
  are independent retail channels: `Projectile_ProcessDamageOnTarget` copies
  `ray[31]` to the hit record used by reactions/death, while
  `Weapon_CalcImpactDamage` reads `ray[32]` for normal-infantry damage.
- The outer PERSON candidate walk does stop at the first entity whose section
  walk hits. That person replaces an existing terrain/item winner only when
  `ray[29]` is strictly closer. Its impact point uses `ray[29] - 0x800`
  without clamping.

`RoundSim` carries both ordinals. The primary/reaction bone feeds `RoundHit`
and the existing `compute_death_anim_state(bone, incoming quadrant, bullet)`
table, so head/limb/torso hits select the matching directional death animation.
The secondary/damage zone feeds the normal-infantry retail multiplier before
the ammo min/max clamps: sections 0-4 x1.25, 5-8 x1.0, 9-12 and 15-18 x0.5,
and 13-14 x3.0. The old bounded torso sphere survives only as a compatibility
fallback when graphic resolution cannot supply a usable COBJ model; it keeps
neutral damage and is never the normal retail path.

The mission-start graphic sweep is not a lifetime assumption. `Simulation`
retains the item database and placer caches, and both RoundSim and F3 request an
idempotent collision attach when a player or scripted organic appears later.
Packed pool handles can be reused, so registry entities carry a binding-only
monotonic spawn identity; collision instances, skeletal sources, and negative
resolution attempts all validate that identity before reuse. State restoration
preserves the live identity high-water mark. Thus a failed lookup for
one slot occupant cannot suppress or inherit the model/husk/pose of the next
occupant.

F3 exposes the same collision truth rather than reconstructing a display-only
approximation. On each six-Hz diagnostic sample, **Rounds → Show hit meshes**
draws every then-current-pose person section at its effective projectile-zero
radius, color-coded by the damage table (0-4 orange, 5-8 cyan, 9-12/15-18 lime,
13-14 magenta; masked dark red, unresolved fallback amber), and labels
entity/bone/radii/multiplier. The Rounds event list names the independent
reaction bone and damage zone. Both views consume
`CollisionWorld::debug_person_sections` / `Simulation.get_hitbox_debug()`
and the same matrix provider used by live bullets. Between reads the view
retains that snapshot, one shared unit-sphere mesh, and its packed MultiMesh
batch; unchanged input causes no geometry, visibility, or label churn. The
native object leg rejects organic and out-of-range candidates before invoking
its matrix provider. The posed-person view omits the local avatar and shares
the object-triangle view's local 80-unit / 96-actor debug budget. This cadence
is diagnostic-only: live projectile queries do not consume the retained
snapshot and remain exact, authoritative per-tick queries. Native `collision`
tests pin the moved-head pose, reverse-scan/mask behavior, radius rules,
primary-bone propagation into a directional death animation, and a
primary-14/secondary-2 775-damage vector, plus object-leg prefiltering before
matrix-provider work. GUT pins the late-spawn/recycled-slot attach, F3's
remote-only 80-unit/96-entity budget/dead-body fallback, the zone colors, the
packed 96×19 sphere batch, six-Hz cadence, unchanged-snapshot cache and
real-input/label-only invalidation, and animated CFAC cache invalidation when
only transformed triangles move.

IDB write-backs (2026-07-18, saved): rename `g_ProjectileDragTable @ 0xB7B300`
(ex `dword_B7B300`); comments at `@ 0x4ea291` (the exclusion-set build map)
and `@ 0x4e78d0` (the drag-table generator).

## 16. Appendix: ground-AI combat chain — SM layer + shared targeting/fire primitives (engine-research, 2026-07-16)

Slice-1 witness pass for the mission-playability track: how AI acquires live targets and
fires. Two layers share one primitive set: the 24-row **AI-vehicle SM** (§1.3 — cveh/cbot/
ctrn/emplacements) carries combat as states 17/18; **organic infantry** carries it inside
`Entity_UpdateInfantryAI @ 0x4b9910`'s combat pass (§3.5 item 5 — witness target still open,
item 7). Everything below is decompile/disasm-witnessed at anchored confidence unless marked.

### 16.1 Dispatch rows 16/17/18 decoded (raw dwords @ 0x815338..0x815367)

| state | enter | tick | exit | event |
|---|---|---|---|---|
| 16 GROUND_FOLLOWWP | `AI_SetStateIdle @ 0x457f00` | `@ 0x467730` (engage block, P2) | `@ 0x457670` | `@ 0x4679f0` |
| 17 GROUND_COMBAT | `AI_EnterState_GroundCombat @ 0x467650` | **`AIEntity_ProcessWeaponFire @ 0x472e00`** | `AI_ClearBoneFlag @ 0x457ee0` | `@ 0x4676a0` |
| 18 GROUND_EVADE | `AI_EnterState_GroundEvade @ 0x467400` | `AI_UpdatePatrolBehavior @ 0x457d70` | `@ 0x457670` | `AI_HandleEvent_GroundEvade @ 0x4675c0` |

The fire routine IS the state-17 tick. Corrections to prior session notes: `0x467650`/
`0x467400` are the combat/evade **enters**, not "death-transition enters"; the IDB had
`0x467400` spanning three glued functions (boundary-fixed this session, §16.6).

- **Combat enter** `[orig: AI_EnterState_GroundCombat @ 0x467650]`: `AiSlot+136 = 2`; brain
  alert cur/pend (`+184/+188`) = 2; `TriggerGroup_SetAlertRed(entity+284)`;
  `Entity_AlertNearbyAllies(entity, 0x640000)`; brain moveStep (`+28`) = 1.
- **Evade enter** `[orig: AI_EnterState_GroundEvade @ 0x467400]`: same alert block + ally
  wake, then routes by def flags (`profile+96`): bit0 organic → pending 17 if brain[38]
  (target) else 16, tail-calling that state's enter via `off_815238[4*state]`; bit2 tracked
  → 17; else wheeled-alive (`health@+286 > 0`) → flee waypoint (controller triple
  `{1, 0x7FFFFFFF, 1}`, work pos/heading, moveStep 16); dead → `AIEvent_QueueEntry` type 3
  (|vel| ≥ 1057) else type 4.
- **Evade event** `[orig: AI_HandleEvent_GroundEvade @ 0x4675c0]`: `AI_HandleCommand` first;
  evt 1 damage → brain[39] = evt[3], pending 18 unless pending/current is 21 or `profile+96`
  bit1; evt 3 → 21; evt 4 → 23 (same family as `@ 0x4676a0` / `@ 0x4679f0`, ported P2).

### 16.2 Target acquisition — the candidate feed is inside `AI_FindBestTargetB @ 0x466f60`

P2 ported the scoring core over an injected candidate list; the feed — the class-driven
pool walk over `g_pool_list @ 0xA892E0` — is witnessed and PORTED 2026-08-13
(`AiSystem::acquire_target`, `engine/runtime/world/ai_combat.cpp`; ctests `ai`,
`promote`). Outer loop = the profile's four weapon-slot class slots (`profile+40+4*i`,
each holding a class id 0..3), each class gated by its priority word (`+80+4*class`,
zero → skip via `LABEL_73`), each selecting pools + sub-filter legs
(the sub-filter reads corrected 2026-08-13 from the full decompile — the earlier
table had class 0's helo leg attributed to class 1):

| class | gate | pools + sub-filter |
|---|---|---|
| 0 (air) | `profile+80` | pool 1 accepting no-brain OR brain-profile `+16 == 1` (HELO) `@ 0x46715d`; then pool 0 accepting `flags & 0x100` (the Player class bit, §28) with the LOCAL player excluded when `dword_24C1930 & 0x800` `@ 0x46714b-0x467155` |
| 1 (ground) | `profile+84` | pool 1, else-leg filter: `!(flags & 0x100)` AND (no brain or brain-profile `+16 != 1`) `@ 0x467169-0x46717e` |
| 2 (organics) | `profile+88` | pool 0, the same else-leg filter |
| 3 (decorations) | `profile+92` | pool 2; case 3 never assigns `target_vehicles` — it INHERITS the previous slot's value (function-entry 0), so a prior class-0 slot flips its filter to the helo leg (inert for brainless, non-Player buildings, but the quirk is real and the port carries it) `@ 0x467048-0x46705a` |

Entry gates: no brain → null; own team byte (`+354`) == 0 requires `AiSlot[1] & 0x200`;
`g_spawn_success_gate @ 0x24C1928` nonzero → null. Per-candidate gates, in order:
`+28` (in-use) ≠ 0; team byte `+354` — teamless and same-team candidates are accepted
when either scanner or candidate carries `AiSlot[1] & 0x200` (`attack anyone`); skip
`flags & 2` and `flags & 0x8000000`; health word `+286 > 0`; not self;
**aiTargetRefCount word `+530` ≤ 16** unless `profile+100 & 8` (anti-pile-on, §16.3);
FOV/range as ported (primary FOV byte `profile+75|1` / range `profile+78`, secondary
`+67|1` / `+70`) plus **per-candidate engage-range caps** words `+422` (primary) / `+420`
(secondary). Priority target `brain[37] (+148)` == candidate bypasses scoring entirely on
mutual LOS. Score = angle × range × priority (`flags & 0x4000` → ×6.0 = 0x60000) ×
refcount-decay, each stage 16.16 with `+0x8000` rounding (as ported); range base is
`0x640000/dist`. **LOS runs last, only for a would-be best**:
`Entity_CheckMutualLineOfSight @ 0x539be0` = `Entity_ComputeWeaponFireOrigin @ 0x43b4b0`
× 2 + `Physics_RaycastTerrainAndSectors @ 0x539910` (terrain + sector raycast, flag 0;
internals = open item, §16.5).

### 16.2a The class walk's data supply (witnessed 2026-08-13, ported with the feed)

- **The `+40..+52` slot-class writer** `[orig: AIProfile_LoadOrFind @ 0x45fd80]`: the
  248-byte profile record is memset-0 on allocation (128-record table `byte_AE577C`,
  count `dword_AED37C`), the name copied in, `File_ParseASCIIFile` runs
  `AIProfile_ParseProperty @ 0x45de70` over the `.aip`, then the four class ids
  {0,1,2,3} are qsorted by their priority words (`CompareFunction @ 0x455d90` =
  `a[1] - b[1]`, ascending; MSVC qsort insertion-sorts ≤ 8 entries — stable) and stored
  REVERSED into `+40/+44/+48/+52` `@ 0x45fed9-0x45ff04` — the walk runs
  priority-DESCENDING. The sort keys load only for `profile+16` (type) ∈ {1 HELO,
  2 GROUND} `@ 0x45fe7f/0x45fe9a`; other types keep all-zero keys → tie order
  {3,2,1,0}. **A missing `.aip` therefore leaves an all-zero record: zero priorities
  gate every class off, and the brain acquires nothing — witnessed retail behavior for
  unresolved profiles** (the shipped drivable transports author `priority_* 0`
  explicitly, same outcome).
- **The profile is never null**: `Entity_InitVehicleAI @ 0x460200` allocates the brain
  from the 812-byte pool (`unk_AED380`), stores `brain+4 = AIProfile_LoadOrFind(name)`
  unconditionally, and scans the model's userpoints for `prim`/`bullet01`/`bullet02`/
  `flare` bone lists (§17.9's muzzle tables).
- **The `+422/+420` per-candidate cap writer** `[orig: Entity_InitFromModel @ 0x40dc30,
  the word copies @ 0x40e136-0x40e15d]`: `entity+422 = itemDef+376` (**radarSig**,
  the primary-FOV cap) and `entity+420 = itemDef+378` (**heatSig**, secondary) —
  the items.def `radarsig`/`heatsig` u16 keys (itemdef-re.md +0x178/+0x17A; the JOX
  corpus authors them on 38/39 persons and 39/39 vehicles). An unauthored (0)
  signature makes the entity undetectable to this feed. Ported: `engine/formats/def`
  parses the pair, the item-traits sweep stamps `Entity::radar_sig/heat_sig`
  (`engine/runtime/simassets/item_traits.cpp`), the feed supplies them as the caps.
- **brain[37] (`+148`) priority-target producers**: the only witnessed writers are the
  savegame/record restore `Entity_CopyVehicleDefToAIComp @ 0x45db30` (`brain+148 =
  EntityPool_GetPtrFromHandle(record+476)`, beside `+152 = handle(record+480)`
  `@ 0x45dbb3-0x45dbc5`) and the release/clear walkers (`Entity_ClearAllReferences
  @ 0x4656bd`-family nulls). A flat-displacement sweep of every `[reg+94h]` store
  found NO live gameplay producer — an indexed-store writer may exist unswept
  (open follow-up below). The consumer is ported (packed+1 rebase in
  `AiBrain::kPriorityTarget`); with no producer the slot stays null, as today.
  (The `Entity_SetWaypointByTeam @ 0x43cd20` `+0x94` writes are the CONTROLLER
  struct at `entity+0x68` — waypoint type/index at controller `+0x8C..+0x98` —
  not the brain; the two structs were disambiguated this session.)
- **Open follow-ups**: the brain[37] live producer (indexed-store sweep; check the
  WAC `ai` command family); the `dword_24C1930` bit `0x800` writer (an MP-rules
  word — the reimpl seam `World::ai_rules_skip_local_player` defaults clear and the
  net wire into it is pending); variant A `AI_FindBestTarget @ 0x465a50` remains
  unwitnessed (§16.5 item 7).

### 16.3 Target bookkeeping

- `[orig: Entity_SetAITarget @ 0x45d760]` — word `+530` on the TARGET is a **targeted-by
  refcount** (dec old target clamp ≥ 0, inc new), read by §16.2 as saturation gate + score
  decay (`0x10000 − (1 << (16 − n))`, ≥16 → 0). The P2 "stealth/visibility" reading is
  refuted. Target ptr lands in brain[38] (`+152`) AND `AiSlot[3]` (`+12`).
- `[orig: AIEntity_TryAcquireTarget @ 0x4716b0]` — retarget cadence: brain[42] (`+168`)
  must exceed **248 ticks** (~4 s), reset to 0 on scan; `profile+16 == 1` selects
  `AI_FindBestTarget @ 0x465a50` (variant A, unwitnessed) else `@ 0x466f60`; result →
  `Entity_SetAITarget`. Called from both fire ticks (`@ 0x472e00`, `@ 0x471710`).
  brain[42]'s incrementer is unwitnessed (open, §16.5).
- `[orig: Entity_AlertNearbyAllies @ 0x4654b0]` (ex "ScanForNearbyEnemies" — misnomer, the
  team compare is EQUAL): pool-1 scan for same-team, alive, non-building entities within
  0x640000 (100 u); sets own `AiSlot+136 = 2` and each ally's brain alert (`+184`) = 2.

### 16.4 The fire chain converges with the player path

State-17 tick `[orig: AIEntity_ProcessWeaponFire @ 0x472e00]` (5965 B, cc 133 — full body
digest pending, §16.5; curated IDB comments give: cooldowns `+208/+210` vs `def+124` rate,
ammo `+212/+216`, `def+100` mode flags 0x80 stationary / 0x40 burst / 0x20 sweep, PRNG
scatter `mod (6 − accuracy)`, sweep wrap −196608) calls per shot:

`Weapon_FireProcess @ 0x53f5b0` — `g_ammoDefTable @ 0xA2ECE8` (stride 276, by weapon
slot): `+64` fire sound → `Sound_PlayWithDistanceAttenuation`, `+68` muzzle-effect id →
`submit_effect_descriptor` (direction from aim pitch/yaw sin/cos, `>>22` fixed chain).
Fire gate: `occupant == g_local_player_entity || !in_session || is_authority` — **the host
fires AI rounds; clients only see the broadcast**. Passes `AiSlot[3]` (current target) as
the hint → `Entity_FireWeaponAndSendPacket @ 0x42bd80` — the already-witnessed shared
fire entry (correspondence row: authority → LOCAL-mode `Server_ClientFiredRound
@ 0x50baa0`; client → `RoundData_SpawnRound @ 0x4ec0d0` + C2S 0x06), i.e. **AI fire enters
the same authoritative round path our `RoundSim` ports (net-re §5.60)** — no new round
plumbing is needed for the port, only the aim/cadence layer that produces fire requests.

Also witnessed: `@ 0x472e00`'s callee set includes the relation-matrix writers
(`TeamMatrix_SetEnemy/SetAllied/SetSpottedBy`, `EntityMatrix_SetProximityBit/
SetDamagedBit`, `EventMatrix_SetSpecialBit`) — the concrete apply-sites for P2's
recorded-not-applied rel-ops — plus `AI_UpdateWaypointMovement` / `AI_UpdateMovementTarget`
(it moves while fighting) and `Entity_ComputeWeaponFireTransform_0` /
`AI_GetSuspensionFirePoint` (aim transforms).

### 16.5 Open follow-ups (this session's unknowns)

4. `Entity_ProcessInfantryWeaponFire @ 0x471710` is wired as the **state-8 tick**
   (`@ 0x8152bc`, HELO enum range) yet named "Infantry" — identity/misname unresolved.
7. `AI_FindBestTarget @ 0x465a50` (variant A, `profile+16 == 1` classes).

### 16.6 IDB write-backs (2026-07-16, saved)

Boundary fix: `0x467400` (was one 0x29c-byte function) split into `[0x467400, 0x4675c0)` /
`[0x4675c0, 0x467648)` / `[0x467650, 0x46769c)` — the SM table's enter-17/event-18 pointers
land on the split points (anchored). Renames (kong misnomers → anchored):
`AI_EnterState_GroundEvade @ 0x467400` (ex `AI_TransitionToDeath_VehicleGeneric`),
`AI_HandleEvent_GroundEvade @ 0x4675c0` (ex `sub_4675C0`), `AI_EnterState_GroundCombat
@ 0x467650` (ex `sub_467650`), `Entity_AlertNearbyAllies @ 0x4654b0` (ex
`Entity_ScanForNearbyEnemies` — code compares team EQUAL). Entry comments on all four plus
`@ 0x466f60` (candidate-feed map), `@ 0x45d760` (+530 refcount), `@ 0x4716b0` (cadence/
variant select), `@ 0x53f5b0` (ammo-def table + authority gate + target hint).

## 17. Appendix: the infantry combat pass + the state-17 tick digest (engine-research, 2026-07-16)

Slice-1 witness session 2, closing four of the §16.5 follow-ups: how an org1 rifleman perceives,
maneuvers, aims, and fires inside `Entity_UpdateInfantryAI @ 0x4b9910`, and the full body
of the SM state-17 tick `AIEntity_ProcessWeaponFire @ 0x472e00`. All addresses retail
`Jointops.exe` (imagebase 0x400000, IDB `Jointops.exe.kong.i64`). In everything below,
"slot" = the AiSlot (`entity+104`, `aiRuntime`); slot fields map 1:1 onto the promote
seeds in §3.2 (`slot[15]/[16]/[17]` = max/min-engagement/max-attack ranges ×65536,
`slot[10]/[11]` = `100−w_accuracy2/1`, `slot[22]` = 62×advancetimer, byte `+136` = alert).

PORT (same day): the infantry pass = `AiSystem::infantry_combat_think` /
`infantry_fire_pass` (`engine/runtime/world/infantry.cpp`), the feed =
`AiSystem::acquire_target` + `infantry_scan_nearest_threat`, the relation apply =
`apply_engage_relations` / `ai_set_target`, the SM rows = `h_enter_ground_combat` /
`h_enter_ground_evade` / `h_ground_combat_tick` (`engine/runtime/world/ai_handlers.cpp`), AI fire →
ring + RoundSim = `fire_ai_round`. Exit pin: the `ai` ctest's NPC-kills-player block.
Residual deviations: ledger D-AI-1/2/4 (residuals), D-AI-5/6 (open); the §17.4
presentation tail and the D-AI-7 LOS collision leg landed 2026-07-16 session 4 (§18).

### 17.1 Perception — the 32-tick scan `[orig: @ 0x4bbe80–0x4bbf60 region, call @ 0x4bbecc]`

Runs when `tick & 0x1F == 0` (the per-entity staggered tick, §3.1):

- **Range staging**: base = `slot[17]` (+68, sight range), HALVED when calm
  (`damageTimer == 0 && !slot_byte136 && !wasHit`). A 4-phase schedule by
  `(tick>>5) & 3`: phase 0 → full base, phases 1/3 → `min(base, 0x60000)` (6 u),
  phase 2 → `max(base/2, min(base, 6u))`. Full-range scans thus run every ~128 ticks
  (~2 s); near scans every 32.
- **Teamless override**: `slot[1] & 8` → the entity scans AS team 2 while teamless
  (byte +354 swapped around the call).
- The scan itself = `Entity_FindNearestThreat @ 0x4b0990` (ex kong
  "Entity_SpawnProjectile" — §17.2) with the staged range.
- **Fallback**: no hit AND (phase 0 or no current target) → `entity->lastAttacker`
  becomes the target if its team differs, `slot[1] & 1` is clear, and
  `Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130` (ex `sub_53B130`; terrain LOS
  `@ 0x53b080` + entity-collision raycast `@ 0x539a70`, returns 1 = clear) passes.
  `lastAttacker` is consumed + cleared every scan — retaliation has a 32-tick memory.
- **Commit**: `aimPoint = target pos`, `aiFocus = target`, `damageTimer += 12` (cap ~27,
  −1/tick — stays alerted while seeing a target), `slot[3] = target` (same field the SM
  layer uses, §16.3), same-target tick counter `+0x33C` increments (reset on switch),
  and `Flags &= ~0x4000` — the own priority-target mark DECAYS each scan and is
  re-armed by firing (§17.4), so recent shooters score ×6 to everyone (§16.2).

### 17.2 The feed — `Entity_FindNearestThreat @ 0x4b0990` → `Entity_FindTargets @ 0x53a610`

`Entity_FindNearestThreat(entity, range)`: effective visual radius =
`min(range/2, 0x280000 = 40u)` (zeroed when `Flags & 0x40`); `slot[1] & 1` → no scan.
Builds a scan ctx: `{&pos, entity, flags 0x1F1, type, 0, range, clamped, 0x7FFFFF80, 0,
0x31C7FFC0, team}` with type = 7 (8 when `def attrib & 0x400`, 9 when mounted), then
`Entity_FindTargets`. On authority a found target also gets the SAME 8 relation-matrix
writes as the SM engage block (§16.4) — the infantry-side apply site of D-AI-3.

`Entity_FindTargets @ 0x53a610` (2475 B, cc 124 — also the weapon-effect targeter):

- ctx flags: `0x20` scan pool 0, `0x40` pool 1, `0x80` pool 2, `0x200` the special list
  `@ 0xB7D388` (count `@ 0xB7C678`), `0x100` enables the team leg, `0x8` attack-anyone
  (skip all team checks), `0x400` skip the entity you stand on, `0x8000` OR'd in during
  the pool loops = defer LOS (restored after — LOS runs once, sorted).
- Entry gate: shooter's AiSlot missing 0x200 AND team == 0 → scan only if the ctx team
  word is nonzero (same teamless rule as §16.2).
- Per candidate: in-use (`+28`); alive (`!(flags&2) && health>0`) OR a corpse hit within
  16 ticks (`current_tick − *(+428) <= 16` — lets NPCs shoot fresh bodies); not
  self/platform; **forced-target words** `shooter+332/+334` (DcbId) `+336/+338`
  (relmat) force-include matching candidates; else the team leg (`flags 0x100`: either
  side's AiSlot `0x200` see-all, or candidate team ≠ 0 and ≠ shooter team).
- `Entity_ValidateWeaponTarget @ 0x53a400`: in-use/alive(/fresh-corpse) again, the
  building filter (`Flags & 0x100` excluded when `dword_24C1930 & 0x800` or
  `g_spawn_success_gate`), shooter forced-target restrict words `+332/+336`, then
  computes the candidate fire origin (`Entity_ComputeWeaponFireOrigin @ 0x43b4b0`) and
  its forward/off-axis distances in the shooter frame
  (`compute_relative_position_metrics @ 0x545710`; off-axis = octagon max+5·min/16).
  Range legs by ctx flag: `0x2` heat (`ctx[8]` off-axis, `ctx[4]` fwd, needs
  `fwd < heatSig<<16`), `0x1` radar (`ctx[9]/ctx[5]`; passes when `radarSig == 0`
  — every organic), `0x10` visual (`ctx[6]` fwd cap; `0x4` adds the `ctx[7]` off-axis
  cap). ctx type 10 + candidate's `slot[3] == shooter` → always valid (it is attacking
  you). LOS deferred when `0x8000`.
- Score = `Weapon_CalcDamageByType @ 0x539140` keyed on ctx TYPE (not ammo): types
  7/8/9 → `−fwd_dist` (nearest first), ×2 penalties (further down): very far
  (`> 0x2AAAAA80`), dead (`health < 0`), drowning organics; type 8 quarters vehicles
  (`def+92 == 1`) and doubles organics; candidates whose AiSlot has `flags[1] & 8` →
  excluded (0x7FFFFFFF); air-attrib defs (`+84 & 0x40`) with no physics ptr excluded.
  Final: shooter forced-words mismatch → score >>= 2 (4× preference for the forced
  target). Untargetable defs (words `+400/+402` both 0xFFFF) skipped.
- Bubble-sort descending, then LOS in order: raycast fire-origin → fire-origin
  (`Physics_RaycastTerrainAndSectors @ 0x539910`, flag 0, TRUE = clear); a shooter with
  `Flags & 0x800000` retries from a 24576 (0.375 u) forward-nudged start. **First
  visible wins** (and up to N visible fill the out array; `ctx[11]` = count).

### 17.3 Combat behavior — reactions, move modes, cover

With a live target (`slot[3]`) and `dist < slot[15]` (attack range) and no hold timer:
**the combat reactions ARE the attack animations** (each taken only if the .adm has the
clip — `animMap[state] != animMap[0]`): 155 `attack`, 165 `cover_attack` when `wasHit`,
158 `attack_4` when `health <= healthMax/2`, 157 `attack_3` under 9 u, 156 `attack_2`
+ 166 `cover_attack_2` under 3 u, 152 `pre_attack` when prev moveMode ∈ {0,3,4}, 151
`post_attack` when the target is dead within 3 u (clears `aiFocus`). A reaction arms
`moveTimer = slot[22]>>4` (the hold between reactions; decrements faster under 10 u/3 u
or in anim 49). No reaction available → approach: `slot[16] < slot[17]` and command
≠ 126 and `dist > slot[16]` and the target position has ground → **moveMode 1** (move
to target, arrive 10 u) else anim 49 + **moveMode 7** (hold). Beyond `slot[15]` with
`damageTimer` and a stale focus: dead focus swaps to corpse-watch; else **moveMode 2**
(move to `aimPoint` = last known, arrive 2 u) or anim 44 + **moveMode 8** (scan idle).
Other modes: 5 = flee (prev-mode latch, 20 u at 10 u arrival, anims 43/167/168
`run_attack`/`run_away`), 6 = board-approach (attach point, arrive 1 u → anim 150
`hold_rope`), 12 = deep water, 33/44/55/66 = the no-cover arrived variants.
`ai_find_cover_position @ 0x4afab0` filters every move target (fail → the per-mode
fallback anim); `aiRef2 == self` = the retreat sentinel (bodyHeading flipped 180° —
walks backwards facing the enemy). Turn-in-place: |targetHeading − bodyHeading| > ~45°
→ anim 147 `stop`, > ~30° → anim 1. Gait: moveMode 2/3 arrival-graded 148/1/149;
damaged/alerted idle 43 → 49 (armed) / 44. Reload: `def clipsize != 0` and magazine
word ≤ 0 and anim 65 available → **anim 65 `reload`** (moveMode 0); while 65 plays the
magazine refills to `clipsize`. Burn states `+0x368[0]` 1–4 → anims 111–114 `burn*`
with `moveTimer` countdown. `Flags & 0x40` → the guard family (140 `guard`, 143
`guard_cover` on roll, 142 `guard_attack` on reaction; leaving → 144 `guard_leave`).
Swim/wash/wounded overlays as §3.5; anims 27–29 `wash_*` when
`Terrain_FindNearestAmbientSoundSource(pos, 15u)` hits. Anim commit uses the §3.4
flag-table arbitration (bit 2 locked → pending; bit 5 + target not bit 0 → pending).

### 17.4 The fire chain — `.bad` anim events pull the trigger

The `.bad` event record's **trigger word** (lerped per frame by `AnimMap_UpdateEntity
@ 0x40b5f0` into `g_animEventTriggerBits @ 0xA2ED08`, ex `dword_A2ED08` — the same
record that feeds our `RootMotionFrame`) is consumed on ODD ticks (`tick & 1`)
`[orig: @ 0x4bf15c–0x4bf4b0]`:

| bit | action |
|---|---|
| 0x1 / 0x2 | footstep L/R — sound-profile slot by surface (water 23 `SSFootWater`, on-entity 21/22 `SS*FootOBJ`, surface-3 19/20 `SS*FootSnow`, else 17/18 `SS*FootGND`), position Z-dipped to FOOT level by the frame's `out[3]` capsule bottom (§17.4b — the earlier "water offset" reading was wrong) |
| 0x4 | **FIRE** weapon byte `entity+0x358` from muzzle bone `entity+0x365` |
| 0x8 | latch `shouldFireSecondary` (fired at the block tail) |
| 0x10 | FIRE weapon byte `entity+0x35B` from bone `entity+0x367` |
| 0x20–0x400 | the six SSAudio foley slots (24–29) |

The secondary latch fires `entity+0x359` from bone `+0x366` and decrements the
**magazine word `entity+0x35C`** (the reload trigger, §17.3; reseeded to
`itemDef->clipsize` on respawn — the def dword `+0x894`, read as a word
`[orig: Entity_ResetToSpawnState @ 0x4b97a9/0x4b97b5]`; parsed by plain atol
`[orig: ItemDef_ParseProperty 'clipsize' @ 0x49fa1c-0x49fa48]`), then
also fires `entity+0x35A` when it differs from `+0x359`. `shouldFireSecondary` is ALSO
latched by the **walking-fire aim gate**: while moving with the muzzle within ~5°
(59652320 BAM) of the aim solution, inside `slot[15]`, def `attrib & 4` clear, and
`moveTimer < slot[22]>>5` → latch + `moveTimer = slot[22]>>4` (the between-shots
cadence). Every fire: muzzle transform via `Entity_GetAttachmentWorldPosition
@ 0x4b2670` → `WeaponSlot_FireAndSpawnEffects @ 0x53f440` (gate
`!in_session || is_authority` — the host fires AI rounds; no occupant leg) →
`Entity_FireWeaponAndSendPacket @ 0x42bd80` (authority → `Server_ClientFiredRound
@ 0x50baa0` = ring append + `RoundData_SpawnRound @ 0x4ec0d0`, §5.60/§16.4; the AI call
passes targetFlags 0, targetId 1, no clip slot) + ammo-def fire sound/muzzle effect
(`g_ammoDefTable[276·id]` +64/+68 — the full presentation-leg witness incl. the
propagation-delay sound model, the MF_Light glow, and the tracer decision is §18),
then `Flags |= 0x4000` (priority mark) and `aiRef0 (+0x2F0) = slot[3]` (the
accuracy-settling memory, §17.5).

**Weapon-byte source**: the four ids `+0x358..0x35B` and bones `+0x365..0x367` are the
items.def `ammo_closeattack / ammo_easyrocket / ammo_advancedrocket / ammo_marker3` +
`launchups_*` family (parsed as names into the def `[orig: ItemDef_ParseProperty
@ 0x4a1843–0x4a1996, def+0x56B/0x58B/0x5AB/0x5CB/0x5EB/0x5FB]` — 32-byte name slots,
compares at `@ 0x4a1823/0x4a186b/0x4a18ae/0x4a18f1`; JO riflemen author all
four = the rifle round, e.g. `AMMO_AK47_556MM`, `clipsize 30`). The resolved-id
block-copy onto the entity is the one unwitnessed link (§17.7 item 1; no per-field
writer exists — it rides a struct copy). `Entity_InitHardpoints @ 0x4417d0` separately
resolves `ammo_closeattack → entity+0x2B4` and `ammo_marker3 → entity+0x2B8` (dwords,
the hardpoint/close-attack consumers — NOT the anim-fire bytes). Port note: until the
block-copy is witnessed, the host seeds ONE ammo id + clipsize per NPC from the def
names at mission load (`Simulation::resolve_ai_weapons`, after the ammo table
loads) — the D-AI-5 stand-in.

### 17.4b The sound legs — footsteps, foley, landing, screams (witnessed + ported 2026-07-17)

The trigger word's SOUND consumers, fully witnessed in both bodies and ported
(`AiSystem::infantry_anim_sound_pass` / `emit_slot_sound` -> `World::slot_sounds`
-> `Simulation::drain_slot_sounds` -> `fire_present_pass._drain_slot_sounds`;
ctest `slot_sound`). The slot table and its SndProf.def source are the audio
record's §sound-profile ([lwf-dbf-sound-re.md](../audio/lwf-dbf-sound-re.md),
D-SND-10..15). All plays go through `Entity_GetProfileSlotSound @ 0x528300`
-> `Entity_PlaySound3D_FullVolume @ 0x528e20` (`Sound_Play3DPositional(id,
&entity->pos, entity, 255)`).

- **Tick parity**: the NPC body consumes on ODD ticks (`v & 1; jz skip`
  `[orig: @ 0x4bf144-0x4bf156]`), the PLAYER body on EVEN ticks
  (`test current_tick, 1; jnz skip` `[orig: @ 0x4b76e6; the var is
  current_tick @ 0x4b4147]`) — the two updaters split the 62 Hz tick.
- **Block order** (org1 `@ 0x4bf169-0x4bf2b0`, org2 `@ 0x4b76f1-0x4b78a8`,
  before the org1 fire bits `@ 0x4bf322`): the six SSAudio bits 0x20..0x400 ->
  slots 24-29 at the entity origin, then feet. org1 additionally skips the
  whole block when the trigger word is 0 (`@ 0x4bf161`); the latch-fire tail
  still runs.
- **Feet** (bit 0x1 L `@ 0x4bf23e`/`@ 0x4b77c6`, 0x2 R `@ 0x4bf2b0`/
  `@ 0x4b7837`): `pos.z -= out[3]` (the AnimMap out-array's capsule-bottom
  stack cell — both bodies pass the array to `AnimMap_UpdateDualChannels`, so
  the "dip" is TO FOOT LEVEL, not a water constant; restored after the play).
  Slot pick in order: `Env_WaterHeightFixed != 0 && dipped z < it` -> 23; the
  `entity+0x28 groundEntity` link -> 21/22; `Terrain_GetSurfaceTypeAtPosition
  @ 0x606510 == 3` -> 19/20 (snow); else 17/18. The OBJ leg is wired to the
  live `Entity::ground_target` as of 2026-08-13 (D-SND-11 FIXED,
  lwf-dbf-sound-re.md).
- **Landing** (org1 `@ 0x4bf87f-0x4bf89f`, org2 `@ 0x4b7f7c-0x4b7fa1`): on the
  airborne-flag(0x2000)-clear edge, dead (Flags&2) -> slot 15 `SSFallDead`
  else 16 `SSFallAlive`, at the entity origin — the pair the earlier
  "water-exit sounds (15/16)" note misread; then vel_z zeroes (org1) and the
  flag clears. Runs for corpses (a thrown body lands with 15).
- **Death scream** (the org1 death edge `@ 0x4b9ca3-0x4b9cc1`): slot 8
  `SSNightDead` when `Bms_AttribFlags & 0x100000` else 7 `sounddeath` — the
  bit is the mission **EnableNVG** attribute (engine/runtime/mission `AttribFlags`; the
  §19.7 "what authors 0x100000" open item closes: the dfx2med encoder writes
  it as the NVG checkbox, and the runtime reuses it as the night gate). The
  `byte+0x134`-bit0 silent-cleanup variant skips the scream (unchanged,
  D-AI-9). The org2 player edge instead plays the BODY-MODEL composite
  `SoundProfile_FindByEntityAndType(entity, night ? 5 : 0)` name
  `@ 0x4b4c4a-0x4b4c6a` — `"%s_%s"` of `Entity_GetBodyModelPrefix @ 0x5280F0`
  (the anim-slot byte +0x374 -> `BM1`..`BM6`/`BF1`/`BF2`/`RM2`/`RF1`, 0 -> 1 ->
  `BM1`) and the `g_entity_sound_type_table @ 0x82F548` suffix (0 `DEATH`,
  5 `DEATH_K`), resolved by name across the loaded banks
  (`SoundBank_FindSetByNameAnyBank @ 0x5274F0`); a miss plays id 0 = silence,
  no profile-slot fallback — ported (D-SND-14 FIXED 2026-08-11,
  `audio::compose_entity_sound_set` + the `is_local_player` death-edge branch).
- **The org2 airborne family** (outside the tick-parity gate — every body
  tick): the chute edge on `Flags & 0x20` vs its `+0x2C` mirror bit
  (`@ 0x4b7b1e-0x4b7b75`): open -> slot 41 `ChuteOpen` + zero the two
  accumulator words `+0x378/+0x37A`; close -> 42 `ChuteClose`. While OPEN
  (`@ 0x4b7b78-0x4b7c08`): accumulators ramp +0x300/+0x200 to 0x7FFF, anim
  state 47, slot 43 `ChuteFlap` refires every tick, and the chute BRAKE
  `vel_z += 0x29C while < -0x1C00` (`@ 0x4b7bfd`). While CLOSED
  (`@ 0x4b7c0d-0x4b7c8d`): accumulators decay -0x40/-0x280 to 0, slot 44
  `FreeFall` refires while `vel_z < -0x3000`, vel floor -0x8000. Both plays
  gate on the `smoothTargetPos - savedLivePose` delta being zero (`var_10A8`
  `@ 0x4b42c1-0x4b42cd`) — net-pulled remote bodies stay silent, matching our
  net-peer motor skip. Ported: the freefall leg (the chute flag is unmodeled;
  chute 41-43 + the brake ride the parachute slice); the accumulator pair's
  consumer is unwalked (likely the canopy flap visual).

### 17.5 The aim model — lead, error, concealment

Recomputed for the stationary leg (anim-state flag `& 0x10`) and the moving leg
(flag `& 8`, seat ≠ 2/5) `[orig: @ 0x4bd0xx–0x4be5xx region]`:

- **Lead**: `lead = fwd_dist / 0x81074 + 1` (≈ per 8 u) →
  `aimPoint = target + lead · (target − target.savedLivePose)` (the previous-tick
  position delta = per-tick velocity; vertical lead halved). Target chest point via
  `Entity_ComputeWeaponFireOrigin @ 0x43b4b0` (muzzle bone `def+1350` transformed by
  the entity matrix; seat-3 occupants get the tick-jittered bone-offset variant).
- **Error**: two accuracy params `slot[10]` (used when `aiRef0 == slot[3]` — already
  fired at this target = settled) / `slot[11]` (fresh target), each
  `err = (119304 · dword_C6EAE8 · acc) >> 5` with `dword_C6EAE8` the difficulty
  global; two sawtooth phases `err · (32 − ((tick>>2 [+ tick>>9]) & 0x3F))` wander the
  yaw/pitch solution (±31·err) — `aimHeading = bearing + errA`,
  `aimPitch = elevation + errB`. **Concealment**: a target in-game, in a prone-family
  anim (flag & 2) AND on foliage (`Foliage_SampleFoliageMapMask @ 0x606620`) adds
  +40 to BOTH accuracy params — hard to hit while prone in grass.
- Body re-faces the aim when it drifts > ~22° (262470208 BAM); mounted `parentSlot 3`
  rotates the solution into the parent frame; scripted idles 130–136 with
  `aiFocus == self` aim at the local player (the pose-track leg).
- The recoil/flinch jitter decays per §4 item 14 (`entity[224]/[225]`, PRNG-signed).

### 17.6 The state-17 tick digest — `AIEntity_ProcessWeaponFire @ 0x472e00` (D-AI-2 body)

Per tick (health ≤ 0 → the §16.1 death event 3/4 instead): `brain[52] += 0x10001·step`
— the PACKED cooldown pair (u16 words at brain bytes +208/+210) both advance by `step`
in one add; `brain[8] += step` accumulates and every ≥16 fires a **processed tick**:
`brain[40] += 16` (no-target/give-up), `brain[41] −= 16` clamp 0 (the §16.4 fire
delay), `brain[42] += 16` (the §16.3 retarget timer — its incrementer), and while
`brain[9] > 0` the flare dispenser runs (`AIEntity_ReleaseFlareCountermeasures`, the §15.7 IDB addendum). `profile+100` mode bits:

- **0x80 stationary** (.aip `RC_FIRE`, emplacements): fire only while byte
  `brain+785` is set — CORRECTED 2026-08-12: that byte is NOT solver-written; it is
  the commanded weapons-free gate (the WAC AI command case 0x15 `@ 0x4659a7` writes
  it — arg 0 clears, nonzero pushes the type's combat state (GROUND → 17, HELO → 8)
  into pending then sets 1 — and the savegame restore `@ 0x45ddba` reloads it; spawn
  default 0). Primary = profile byte `+148`, interval `+124` vs cooldown word +208,
  ammo `brain[53]`, list count `brain[55]`/list `brain+224`, def block `profile+120`;
  secondary = `+180/+156`/word +210/`brain[54]`/`brain[72]/brain+292`/`profile+152`.
  Each shot: `Entity_ComputeWeaponFireTransform_0 @ 0x456980` (CORRECTED — the old
  `0x455b30` cite was a transcription slip; full digest §17.9) solves the fire pose
  (arg 7 = the ctx 0x8000 defer-LOS bit, 0 at the stationary/continuation sites, 1
  at the mobile sites — NOT a track-only switch) → `Weapon_FireProcess @ 0x53f5b0`
  → ammo−−, cooldown word = 0, `brain[106]` = which (1/2), bone-flag byte 784 |=
  0x40 `@ 0x47306f`. Not aligned ≥ 620 → pending = fallback. Every processed tick
  `Entity_SetAITarget(entity, 0)` (stationary mode keeps no brain[38] target — so in
  practice only the WEAPON_PITCHLOCKED* solver legs, which need no target, fire) and
  `profile+100 & 1` → `AI_UpdateMovementTarget @ 0x460e40`.
- **0x40 burst** (.aip `ATEAM_LOCK`): `brain[181]` = the window (armed to 1 by each
  targeted shot, +step while ≤ 186, else 0). CORRECTED 2026-08-12: the between-ticks
  `brain[106]` continuation sites (`@ 0x473d57` primary / `@ 0x473e44` secondary) DO
  re-solve — same solver, arg 7 = 0 (LOS on), fresh aim offset, then scatter — the
  old "re-fires the saved solution without re-solving" gloss was wrong. The saved
  deltas `brain[182..187]` (primary) / `[188..193]` (secondary) = out-transform minus
  entity pos/angles, captured at each mobile/continuation solve `@ 0x4735f9..0x47363a`
  (their consumer is elsewhere — presentation/suspension, unwalked).
- **0x20 sweep**: each shot `brain[180] += 10918` (1/6 u), passed as the transform's
  lateral bias; on > 196608 (3.0) reset to −196608 AND `brain[38] = 0` (drop target →
  rescan) — the strafing-MG walk.

No target (`brain[38]` null): `brain[180] = −196608`, movement flag 1 → waypoint walk,
flag 4 → hold heading; else the **search sweep** (workHeading = yaw ± 0x3FFFFFC0 by
`brain[40]` phases ≤124 / >434, speed = `brain[50]`) and `AI_FindBestTargetB
@ 0x466f60` → on found: the 8 relation ops in the §16.4 order, `Entity_SetAITarget`,
`brain[40] = 0`, `brain[41] = profile+104` (+ `LCG_31BFBB8 % 62` when nonzero — NOTE:
this site jitters both controller-branches alike, unlike the state-16 engage's A/B
split, §16.4) → return; none + `brain[40] > 620` → SetAITarget(0), pending = fallback.
Target dead → SetAITarget(0). Fire leg (processed ticks or `brain[48]` — a forced-
process flag, writer unwitnessed): `brain[41]` nonzero → move only;
`AIEntity_TryAcquireTarget @ 0x4716b0` may retarget; chase = approach cap
`profile+76`, min range `+188`, match the target's speed inside `+184` (target
`brain[136]` or |velocity|), give-up > 620 beyond the cap; **fire gate** = folded
|targetHeading − yaw| ≤ `((profile+67 | 1) | 2) >> 1` (the secondary-FOV arc — byte
+67 is the top byte of the +64 `view_fov` BAM dword, §17.9c); weapon select as
stationary (+ the anti-building swap: target `Flags & 0x100` → `byte_AE076E/F` via
`sub_545930`, §17.7 item 4); solve with the aim offset passed through the
`distance` global `@ 0xB21F8C` (written at every mobile/continuation site
`@ 0x473367..0x47337e`: `0x20` ATEAM → `brain[180]`, `0x40` ATEAM_LOCK → −196608
(−3.0 u), else 0) then **scatter** `@ 0x473640..0x473716`: two `dword_31BFBB8`
rol-LCG draws (`x' = rol(rol(x,11)+x,4) ^ 1`, value = low u16), each
`(u16 % (6 − brain[43])) · 8947848.0f` (`flt_7C6F60` = 0x888888 ≈ 0.75°;
`brain[43]` = accuracy 0–5), the scaled value's parity picks the sign (odd adds,
even subtracts), applied to yaw then pitch → `Weapon_FireProcess`. Between
processed ticks the `brain[106]` continuation keeps the volley running
cooldown-gated through the same solver (§17.6 burst bullet), and
`profile+136/+168` bit 0 (`WEAPON_TURRET`) stages/refreshes the turret solution
without firing.

### 17.7 Open follow-ups (this session's unknowns)

1. The block-copy writer of the anim-fire weapon bytes `entity+0x358..0x35B` + bones
   `+0x365..0x367` (no per-field instruction writes them; §17.4 pins the def source).
   The SM sibling — the spawn writer of `brain[53]/[54]` (live ammo, seeded from the
   `.aip` `primary/secondary_ammo` capacities) and of `brain[43]` (accuracy; the
   0..4-clamped `aim_skill` at profile+28 is the PROBABLE source) — is the same
   unwitnessed copy family (the only found +785/+0x311 writers are the command and
   the savegame restore, so the plain spawn path is a block/memset).
2. RESOLVED 2026-08-12 → §17.9: `Entity_ComputeWeaponFireTransform_0 @ 0x456980`
   (the old `0x455b30` cite was wrong) witnessed in full and ported (D-AI-2).
3. `brain[48]` (the forced-process flag read by the state-17 tick) writer.
4. The anti-building weapon swap (`sub_545930` + `byte_AE076E/F` selection).
5. RESOLVED 2026-08-12 → §17.9b: `compute_relative_position_metrics @ 0x545710`
   witnessed in full (the metrics ARE the SM aim solve's angle source).
6. RESOLVED 2026-08-12 → §17.9b: `Entity_GetWeaponFirePosition @ 0x43b630` (the old
   `0x53a2e0` cite was off) — quality 1 = def+1351 muzzle bone through the current
   matrix (persons route the type-3 seat/userpoint legs), quality 2 = pos + 49152
   (0.75 u) when a model exists but no muzzle bone, quality 3 = raw pos; callers
   fall back to `Entity_ComputeWeaponFireOrigin @ 0x43b4b0` when quality != 1.
7. The scripted-idle aim leg's `Flags & 0x80000` gate writer (aim-at-player poses).
8. NEW: the saved continuation-delta blocks' (`brain[182..193]`) consumer (captured
   at every mobile/continuation solve; nothing in the fire pump reads them back).
9. NEW: `Weapon_FireProcess @ 0x53f5b0`'s composition of the out-transform with the
   weapon mount/bone rest frame — with an authored nonzero `primary_facing` the
   solver's world compose alone lands at (bearing − bias); whether the spawn re-adds
   the mount facing is unwalked. Corpus check (JOX, 2026-08-12): no shipped `.aip`
   resolves a nonzero facing — the GROUND files omit the key entirely and the helo
   files author `0` or the `WEAPON_TURRET` token typo (atof → 0) — so the port's
   `heading + rel_yaw` is exact for shipped data.

### 17.8 IDB write-backs (2026-07-16 session 2, saved)

Renames (anchored, ex auto-names): `Entity_CheckLineOfSightTerrainAndEntities
@ 0x53b130` (ex `sub_53B130`), `g_animEventTriggerBits @ 0xA2ED08` (ex `dword_A2ED08`).
Entry/site comments: `@ 0xA2ED08` (trigger-bit map), `@ 0x4b0990` (two callers — the
"sole caller" note was stale), `@ 0x455ef0` (flare-dispenser misnomer), `@ 0x472e00`
(full digest), `@ 0x53a610` (ctx layout), `@ 0x43b4b0` (fire origin), `@ 0x4bbecc`
(perception scan), `@ 0x4bf31d` (anim-event fire block), `@ 0x4b97b5` (magazine
reseed), `@ 0x4418a5` (+0x2B4/+0x2B8 hardpoint ammo), `@ 0x53f440` (AI fire entry).
Rename proposal pending maintainer OK (curated name): `Entity_ComputeWeaponFirePositions
@ 0x455ef0` → `AIEntity_ReleaseFlareCountermeasures`.

### 17.9 The SM/turret fire-transform solver — `Entity_ComputeWeaponFireTransform_0 @ 0x456980` (engine-research 2026-08-12; the D-AI-2 core, PORTED)

Cdecl, SEVEN args (the IDB's 12-arg prototype over-decompiles the frame):
`(entity, outTransform6, boneListCount, boneListPtr, weaponDefBlock, seed, deferLOS)`.
`outTransform6` = `{pos xyz 16.16, yaw, pitch, roll BAM}`, pre-seeded by every caller
from the entity's live pos/angles; `weaponDefBlock` = the `.aip` block at
`profile+120`/`+152` (§17.9c); `seed` = the live ammo count (`brain[53]/[54]`);
`deferLOS` ORs the §17.2 ctx `0x8000` defer-LOS bit (1 at the mobile sites, 0 at
stationary/continuation). Callers: the 8 sites in `AIEntity_ProcessWeaponFire
@ 0x472e00` + 8 in `Entity_ProcessInfantryWeaponFire @ 0x471710`-family.

- **Head gate** `@ 0x4569b2`: no target (`brain[38]` null) and `!(block+16 & 0x18)`
  → return 0. Bone-flag byte `brain+0x310` cleared `@ 0x4569d5`.
- **Muzzle origin**: with a bone list, slot = `seed % count` (seed > 0) or the
  round-robin counter `brain+0x1AC` (0 reseeds to −1, then `ctr-- % count`)
  `@ 0x4569f1`; byte `+0x310` = slot | 0x80; the bone/userpoint local position
  (block+16 bit 0 picks the skeleton row's rest position at bone-row `+0x38` over
  the authored point) transforms through the entity euler matrix. With an EMPTY
  list `@ 0x456ae4`: pos + 0x20000 Z (2.0 u). Then `out[3] += block+20` (the yaw
  facing bias) `@ 0x456b1c`.
- **WEAPON_PITCHLOCKED legs** `@ 0x45705d` (block+16 & 0x18, no aim solve, no cone,
  no slew, always return 1): `0x10` → pitch += 0xE0000020 (−45°-ish); `0x8` →
  pitch += `brain+0x314` (the commanded elevation — the WAC AI command case 0x16
  writes `arg × 11930464` deg→BAM `@ 0x4659ef`). Bit 0 additionally stages the
  pose (below) and, with a model, re-transforms the muzzle through the CURRENT pose
  (`Model_TransformBoneMatrices`, bone row ×0x40) `@ 0x4571ae`.
- **Aim solve** (via the `sub_53afc0` wrapper `@ 0x456c84`): the frame matrix is
  built from the OUT transform (muzzle pos + biased angles); the shooter fire
  position (`Entity_GetWeaponFirePosition @ 0x43b630`, fallback
  `Entity_ComputeWeaponFireOrigin @ 0x43b4b0`) is the LOS start; the target's own
  fire origin is the aim point, plus the `distance`-global lateral offset =
  `R_yaw(entity yaw)·(d, 0, 0)` via `Math_RotateOffsetByEulerFixedPoint (ex sub_6158f0)` (added to the target pos, restored
  after) `@ 0x456bee..0x456c73`. `compute_relative_position_metrics @ 0x545710`
  then yields `{hdist = √(Σ((v>>8)²>>16, +0x8000 rounding)) << 16, ?, dist, yaw =
  atan2(local_y, local_x)·2³¹/π, pitch = atan2(local_z, hdist)·2³¹/π, ?}` — the
  RELATIVE angles in that frame. `Entity_ValidateWeaponTarget @ 0x53a400` applies
  the §17.2 ctx range legs + LOS (unless deferred).
- **Cone gate** `@ 0x456d17`: fold each relative angle to |top byte| (≥ 0x80 →
  256−); limit = `(block+8 | 0x2000000) >> 25` (a floor of 1); over → return 0.
- **World compose** `@ 0x456ce5`: `Math_BuildFixedPointRotationMatrixYXZ` multiplies
  `R(rel_yaw, rel_pitch)` INTO the entity frame matrix (identity-seeded semantics
  witnessed at `Math_RotateOffsetByEulerFixedPoint`), euler re-extract → `out[3..5]` world angles.
- **WEAPON_TURRET staging + slew** (block+16 bit 0) `@ 0x456d7b`: staging block
  `brain+0x1E4..0x1F8` = `{hdist, ?, dist, −1 − rel_yaw − block+20, rel_pitch, ?}`;
  no SLOW/FAST → snap to the active block `brain+0x1CC..0x1E0` and return 1. SLOW
  (bit 1) `@ 0x456e52`: |staged − active(brain+0x1D8)| < `step·0x18C6318` → snap +
  return 1, else active ± 0x2108421 and return 0. FAST (bit 2) `@ 0x456ef9`:
  exactly double (`step·0x318C631`, ±0x4210842). On every snap the per-type turret
  diagnostic globals get the active yaw/pitch high words (`profile+16` == 2 GROUND →
  `dword_83FEE0/83FEE8`, == 1 HELO → `dword_83FE88/83FE90`) `@ 0x456e33/0x456f9c`.
- `AI_GetSuspensionFirePoint @ 0x456860` reads the active yaw (`brain[118]`) +
  `profile+140` for the suspension fire point — the D-AI-2 adjacency consumer.

**PORT** (2026-08-12): `AiSystem::solve_weapon_fire_transform`
(engine/runtime/world/ai_combat.cpp) + the three `h_ground_combat_tick` fire legs
(stationary/continuation/mobile, ai_handlers.cpp `sm_weapon_fire`) + the WAC AI
commands 0x15/0x16 (`ai_handle_command`). Bounded deviations, cited at the port
sites: no SM muzzle bone lists in the world model (the empty-list leg always runs —
muzzle = pos + 2.0 u Z; the bone-list/current-pose refine rides the skeletal-pose
seam like D-AI-6), the solve frame is yaw-only (AiEntity carries no pitch/roll —
level shooters identical), the §17.2 ctx range legs stay with the acquire-time
gates, and the CTRL diagnostic globals are unported (D-3DI-2 owns the bus). The
`ai` ctest `test_sm_turret_fire` pins the mobile solve/fire, the SLOW slew hold,
and the RC_FIRE + PITCHLOCKED_MINUS45 stationary leg behind the guard byte.

### 17.9b The aim-solve callee family (witnessed 2026-08-12)

`sub_53afc0 @ 0x53afc0` (the solve wrapper): frame matrix ← *ctx[0] (the OUT
transform), fire position ← `Entity_GetWeaponFirePosition(ctx[1])` (quality != 1 →
`Entity_ComputeWeaponFireOrigin`), then `Entity_ValidateWeaponTarget(target, ctx,
&out6, frame, firePos)`; out6 copied to the caller `@ 0x53b047`.
`compute_relative_position_metrics @ 0x545710`: full digest in §17.9 (the fpatan
operand order and the `dbl_7C19D8` = 683565275.5764316 = 2³¹/π scale are exact in
the disasm; the decompiler's tail for out[3] is wrong).

### 17.9c The .aip GROUND property set — `AIProfile_ParseProperty @ 0x45de70` (witnessed 2026-08-12)

Dispatch is gated on the profile `type` (+16: HELO 1 / GROUND 2 / ORGANIC 3);
ORGANIC returns before any key dispatch (organic .aip files carry data for
NOTHING; their keys are dead), and the HELO set remains unwitnessed. GROUND keys
(offset ← conversion): `rank`→+60 atol; `default_state`→+24
(`AIState_LookupByName`); `view_fov`→+64 and `radar_fov`→+72 (atof ×
`dbl_7C6E18` = 11930464.0, deg→BAM32, x87-chopped — profile bytes +67/+75 read by
the FOV gates are these dwords' top bytes); `view_dist`→+68, `radar_dist`→+76,
`tether_dist`→+116, `primary_range`→+132, `secondary_range`→+164 (atol << 16);
`priority_air/ground/organics/decorations`→+80/+84/+88/+92 (atol — the §16.2
class-enable words, closing D-AI-1's data-supply witness); `react_time`→+104 and
`primary/secondary_rate`→+124/+156 (atof × `dbl_7C3B48` = 62.5, seconds→ticks);
`aim_skill`→+28 (atol clamped 0..4); `check_six_rate`→+108, `target_eval_rate`→
+112 (atof × dbl_7C6AC0); `primary/secondary_ammo`→+120/+152 (atol);
`primary/secondary_weap`→ byte +148/+180 (`AmmoDef_LookupByName`);
`primary/secondary_facing`→+140/+172 and `_pitch`→+144/+176 (deg→BAM32);
`min/max_chase_dist`→+188/+184 (atof × 65536); `patrol/combat_speed`→+192/+196
(the D-AI-11 pair). Flag token loops: `primary/secondary_flags`→+136/+168 —
`WEAPON_TURRET` 0x1, `WEAPON_SLOW` 0x2, `WEAPON_FAST` 0x4, `WEAPON_PITCHLOCKED`
0x8, `WEAPON_PITCHLOCKED_MINUS45` 0x10; `EVADE_FLAGS`→+96 / `COMBAT_FLAGS`→+100 —
`FOLLOW_WP` 0x1, `NO_ACTION` 0x2, `FLEE` 0x4, `NO_CAP` 0x8, `COUNTER` 0x10, plus
COMBAT-only `ATEAM` 0x20, `ATEAM_LOCK` 0x40, `RC_FIRE` 0x80 (the §17.6 mode bits'
authored names). PORT: `aip::parse_profile` (engine/formats/aip), resolved per
mission ai_textfile by `mission::resolve_ai_profiles` and seeded at promote
(weapon blocks + ammo counts + COMBAT_FLAGS; the ammo NAMES resolve against the
loaded table at the item-traits sweep). Corpus notes (JOX): all four GROUND
transports author `primary_weap AI_STINGER` with priorities 0 (they carry an armed
block but never acquire), and no shipped file resolves a nonzero facing.

### 17.10 Divergences (the ledger's D-AI-4 row, owned here since 2026-08-25)

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-AI-4 | The infantry combat pass is ported (2026-07-16, `AiSystem::infantry_combat_think`/`infantry_fire_pass`): 32-tick staged perception (calm half-range, 4-phase schedule, lastAttacker fallback, slot[3]+aimPoint+damageTimer commit, priority-mark decay), the attack-anim reactions (155–158/165/166 by distance/health/hit + post_attack 151), approach/hold move modes, reload (anim 65 + magazine), lead + sawtooth aim error, the walking-fire latch, and the `.bad` anim-event fire into the ring + RoundSim. The 2026-07-20 parity pass added direct `AiSlot[1]&0x200` Berserk friendly filtering [orig: `Entity_FindTargets @ 0x53a7ea-0x53a824`] and the actual-hit move/group + wasHit/timer/attacker chain [orig: `Entity_HandleDamageTrigger @ 0x407310`; `Entity_OnDamageReceived @ 0x4af800`] | retail near misses remain listener-only [orig: `Projectile_UpdatePhysics @ 0x4ea99a-0x4ea9f2`]; the nearest-first scan penalties (corpse/drowning/far x2), fresh-corpse (<=16-tick) targets, forced-target words, cover-seeking (`ai_find_cover_position`), retreat/board modes, and the §4-item-13 idle look-at are unported. The 2026-08-28 pass ported the pre_attack 152 reaction, the cover_idle/cover_run 163/164 hit flinch with the think-cadence wasHit consume, the unconditional moveTimer stamp and the no-reaction fall-through into the approach arm, the command-126 and target-ground approach gates (`Entity_CheckGroundHeightAtPosition @ 0x4aff70`), the alerted-idle predicate (49 with a slot[3] target, wasHit not a term), the availability-gated wounded gaits, the same-think waypoint step on a 0 cooldown, and the flt_7C19E0 sqrt clamp on the waypoint distance | OPEN (friendly/hit reactions fixed; the remaining behavior residuals tracked) — §17.1-§17.6 carry the witnesses |

## 18. Appendix: fire presentation + the LOS raycast internals (engine-research, 2026-07-16 session 4)

The §17.4 presentation tail (how the firing host makes its own AI rounds audible and
visible) and the §16.5-item-6 closure (`Physics_RaycastTerrainAndSectors @ 0x539910`
internals — the D-AI-7 sector leg). All addresses retail `Jointops.exe` (imagebase
0x400000, IDB `Jointops.exe.kong.i64`).

PORT (same day, worktree play; sound legs re-homed 2026-08-07, S12a): the ammo.def
presentation tokens = `engine/formats/def`
(`def_parse_ammo` + `ammo_tracer_type_from_name`) → `AmmoTableEntry` (npruntime
builder); the tracer decision + the per-spawn `FireEvent` record =
`world::RoundSim::spawn`; the SOUND legs of both arms + the pending-delay queue =
`engine/runtime/world/fire_sound.cpp` on the logic clock (seeded inline at
spawn, the original's fire-time moment; the shell stamps the camera listener via
`Simulation::set_sound_listener` and plays `drain_fire_sounds()` rows); the
binding drains = `Simulation::
drain_fire_presentation_events` / `get_tracer_trails` (ex `get_tracer_rounds` —
replaced by the witnessed trail channels, §24); the EFFECT presentation =
`godot/game/world/fire_present_pass.gd` (muzzle effect + the §24 tracer
ribbons); the LOS legs = `CollisionWorld::raycast_clear` +
`los_terrain_blocked` (`engine/runtime/world/collision_los.cpp`) behind
`AiSystem::line_of_sight_clear`. Pins: the `def` ctest (token fields), the
`npruntime_round_sim` ctest (tracer cadence/forcetracer/FireEvent + the trail
channels), the `fire_sound` ctest (gate/formula/pool/arm-split sounds), the
`collision` ctest (`test_raycast_clear_los`), and the
`ai_threat_probe` in-game stats gate.

### 18.1 The ammo-def presentation fields — parse + resolve

`AmmoDef_ParseProperty @ 0x40a2d0` fills four presentation slots per 276-B record
(all four author on the JO rifle rounds, e.g. `AMMO_AK47_556MM`: `ai_launch
GS_AK47_2`, `ai_Launcheffect Effect_EnmyMuzz`, `Mf_Light 100`, `tracer_type stdred
stdgreen`):

| token | record | resolve at parse |
|---|---|---|
| `ai_launch <SET>` | +64 | `SoundBank_FindSetByNameAnyBank @ 0x5274f0` scan of the loaded 204-B sound-profile slots → SET POINTER `[orig: @ 0x40a8c8-0x40a8ef]` |
| `ai_launcheffect <FX>` | +68 | `CEffectWorld_InternEffectHandle @ 0x5f7310` → 1-based interned handle `[orig: @ 0x40a8f6-0x40a91d]` |
| `MF_Light <n>` | +36 = 1, +40 = atol | presence flag + value `[orig: @ 0x40a804-0x40a837]` |
| `tracer_type <f> [<e>]` | +232 / +236 | `AmmoDef_ParseTypeName` name→id (stdred 1, stdgreen 2, rocket 3, at4 4, grenade 5, rapidred 6, rapidgreen 7, sniperred 9, snipergreen 10, df1red 11, df1green 12; atol fallback); one value copies into both `[orig: @ 0x40a78b-0x40a7fa]` |

The two sibling GRAPHIC slots (the tracer round's visible item models): +0x10
friendly / +0x14 enemy item ids, resolved through the item list with the parse
warnings `"couldn't find ammodef frndlyTrcrID"` / `"... type_id"`
`[orig: @ 0x40a5f8-0x40a68d]` — PARSED 2026-07-18 (`frndlyTrcrID`/`foeTrcrID` in
engine/formats/def, raw type ids in the ammo table; the model leg itself rides D-AI-12d,
§24.4). Our port stores names/ids instead of resolved pointers/handles and
resolves in the reimpl at use time — same case-insensitive namespaces (the bank's
soundsets, the effect world's interned .ptl names, the item list).

### 18.2 The fire sound leg — `Sound_PlayWithDistanceAttenuation @ 0x528e40`

Called from `WeaponSlot_FireAndSpawnEffects @ 0x53f440` with (ammoDef+64, fire
origin, shooter). Gate: `is_mp_session_peer` (`g_napi_np_ctx+0x64` — the is_client
bit, TRUE in SP mode 3 per net-re §5.0) — i.e. skipped only on a DEDICATED server
(no local listener; a prior IDB gloss had this inverted — fixed). Then:

- distance = |origin − listener| (`listener_pos @ 0x24D6630`), 16.16 → int units;
- range gate: distance > soundDef+72 (the set's max range) → silent drop;
- **propagation delay**: distance ≥ 30 u and `g_SoundSpeedFixed @ 0x24D6660`
  (= 0x14A0000 = 330.0 u/s 16.16, set by `DialogSystem_Init @ 0x5275fc`) nonzero →
  a pending slot with countdown `(62·dist/330) >> 2` ticks (the witnessed
  quarter-compression of physical travel time) `[orig: @ 0x528ed4-0x528ef2]`;
  else immediate `Entity_PlaySound3D_FullVolume @ 0x528e20` =
  `Sound_Play3DPositional(set, pos, entity, 255)`.

The pending queue is 128 × 24-B slots `@ 0x24DF678..0x24E0278` (`flags|1`, set ptr,
pos[3], countdown) allocated by `EffectSlot_AllocateAndInit @ 0x527c30` (a FULL
pool drops the sound `[orig: the failed scan return @ 0x527c47]`; a zero countdown
seeds 1 `[orig: @ 0x527c94]`) and drained
once per tick by `Sound_TickPendingSlots @ 0x529310` (ex `sub_529310`, renamed this
session): countdown-- → 0 plays `Sound_Play3DPositional(set, pos, null, 255)`
(flag-2 slots are the dialog-trigger variant). In the frame, the drain sits
between `Client_ProcessNetworkFrame` and `Server_TickUpdate` /
`Entity_UpdateAllEntities` `[orig: @ 0x526697 in Game_ProcessMainFrame
@ 0x5263f0]` — client-received seeds decrement the same frame; host/AI seeds the
next. Port (S12a, 2026-08-07): `world::FireSoundQueue`
(`engine/runtime/world/fire_sound.cpp`) — seeded inline from
`RoundSim::spawn`, counted down at the head of `World::run_logic_tick`, listener
stamped per frame by the shell (`set_sound_listener`; never on a dedicated host,
the witnessed `is_mp_session_peer` gate `@ 0x528e57`); our host applies wire fire
on the pre-tick receive boundary, projecting those seeds one countdown earlier
than retail's post-drain server tick. The range check runs at PLAY time in our
bank vs fire time in retail — tracked in D-AI-8.

The adm-indexed receive arm plays NO ammo-def sound and takes NO propagation
delay: `ActionSlot_ExecuteAction @ 0x4020a0` plays the row's soundset
immediately at the SHOOTER'S entity position (`Entity_PlaySound3D_FullVolume
@ 0x528e20` with `entity+4` `[orig: @ 0x4020ef]`), and the receive path's
one-shot stamp (`slot+90 = 64` `[orig: @ 0x42f728/@ 0x42f8ff]`) makes the end
shim `ActionSlot_PlayEndSoundAndDupes @ 0x401100` play the row's soundsetend
immediately too (the local pump defers that to the active phase's END — JOX
fire rows author the audible gunshot in soundsetend, 83/89 rows). Both the
FIRE (+684) and RECOIL (+688) rows execute `[orig: @ 0x42f777/@ 0x42f785 and
@ 0x42f98f/@ 0x42f9d0]`. The end shim's "dupe" leg (ActionDef+44 count /
+48 interval delayed repeats through the same slot pool `[orig: @ 0x401148/
@ 0x40118b]`) is a tracked deferral — the JOX weapon.def authors no dupe keys.
Port: the adm arm of `world::fire_sound_on_spawn` (immediate rows at the
shooter's position; a joiner's entity-less wire shooter supplies its decoded
row position, `RoundSpawnParams::shooter_pos`).

### 18.3 The muzzle effect + MF_Light glow legs

- Muzzle `[orig: @ 0x53f4a9-0x53f583]`: ammoDef+68 nonzero → a zeroed 34-dword
  spawn transform whose [6..8] receive the fire-direction vector (sin/cos of the
  aim yaw/pitch, the `>> 22` fixed-point trig noted on the §16.4 row), then
  `submit_effect_descriptor @ 0x5f6f80` builds the 56-B spawn descriptor
  {[1] = effect handle, [3] = shooter, [4..6] = fire origin} →
  `CEffectWorld_SpawnEmitterAtPosition @ 0x5f6df0`. Every fire spawns one (no
  per-shooter live-handle guard on this leg — unlike the player action-slot's
  slot+24 guard). Port: `EffectWorld.spawn_effect(name, origin, forward)`.
- MF_Light `[orig: @ 0x53f58b → Entity_UpdateMuzzleGlowEffect @ 0x56c960]`
  (ex `sub_56C960`, renamed): ammoDef+36 → a light-pool glow
  (`LightPool_SpawnGlowEffect @ 0x5a8d50`, radius 98304 = 1.5 u, color table
  `@ 0xFFE0A0`) cached per shooter at entity+436, repositioned to the muzzle and
  re-blended to 1.0 every shot (fade params 4/5). ROUTED through
  `EffectLightDirector.on_muzzle_fire`; the shared entity lease and exact
  per-draw owner selection now reproduce retail's self-only illumination
  (D-RLIT-4 closed 2026-08-23). The +40 value's consumer is unwitnessed (not
  passed at this site).

### 18.4 The tracer decision — `RoundData_SpawnRound @ 0x4ec0d0`

`[orig: @ 0x4ec184-0x4ec1e5]` per spawned round: tracer defaults ON; rate byte
+226 == 0 → off; else the shooter's WEAPON-SLOT counter byte (`weaponSlot(+0x68)
+0x80`) increments, wraps to 0 at ≥ rate, and the round is a tracer exactly on the
wrap (no shooter/slot → every round); ammo flags & 0x8000 FORCETRACER → tracer
regardless. Round team byte +0x162 = shooter team (slot+4 & 0x200 → 0xFF)
`[orig: @ 0x4ec705-0x4ec721]`.

Tracer VISUALS (per presenting client, selected against `g_local_player_entity`'s
team — shooter == local or team match = friendly `[orig: @ 0x4ec740-0x4ec79b]`):
the `tracer_type` id (+0xE8 friendly / +0xEC enemy) allocates a slot in the
dedicated tracer/trail emitter pool (`CEffectEmitterPool_AllocSlot @ 0x5db7a0`
over `g_TracerEmitterPool @ 0x2BF5270`, channel init `CEffectChannel_Init
@ 0x5db130`) into round+0x2B4; the round's visible MODEL is the +0x10/+0x14 item
graphic via `Entity_InitFromItemDef @ 0x49e550`; a per-round glow light lands in
round+0x1B4 `[orig: @ 0x4ec8da]`. The MP `NoTracers` rules bit (`dword_24D1E34 &
1`, net-re §6.8 mp_attributes 0x001) kills the visual unless forcetracer.
**Non-tracer rounds get `graphicModel` (+0x30) zeroed — invisible in flight**
`[orig: @ 0x4ec900]`. Port (fully witnessed + rebuilt 2026-07-18, §24):
`LiveRound.tracer/team/trail_slot` + the witnessed per-SLOT cadence byte
(retail `weaponSlot+0x80`; D-AI-8a CLOSED 2026-08-12 — the local player's fire
passes its active `WeaponSlotState::tracer_shot_counter` through
`RoundSpawnParams::tracer_counter`, so each weapon keeps its own phase across
switches; slot-less NPC fire keeps the shooter-entity byte, exactly one
modeled weapon per NPC under D-AI-5); the trail pool =
`world/tracer_trails.{h,cpp}`, the spawn-time style select + NoTracers gate =
`RoundSim::spawn`, the ribbons = `fire_present_pass.gd` via
`get_tracer_trails()`; the round graphic / glow / smoke-anim residuals are
D-AI-12.

### 18.5 `Physics_RaycastTerrainAndSectors @ 0x539910` — the LOS raycast (closes the §16.5 raycast follow-up)

Returns TRUE = CLEAR. Arg 5 is a RAY RADIUS (not a flag): the terrain leg lowers
both endpoint Z by it (thick-ray conservative) and the sector leg inflates every
bound test by it; LOS callers pass 0 (`Entity_CheckMutualLineOfSight @ 0x539be0`).

1. **Terrain leg** `[orig: @ 0x53993d-0x539968]`: skipped when BOTH entities carry
   `Flags & 0x800000` INDOORS (the heightmap has no interiors — indoor-to-indoor
   sight would false-block); the null-entity variant instead skips it when either
   ENDPOINT is below terrain (`Terrain_GetHeightAtPosition @ 0x606720`). Otherwise
   `Terrain_RaycastHeightmapHiRes @ 0x60c760` (null out-hit) — hit → BLOCKED.
   Port: `terrain_raycast_refined` (the ported `@ 0x60e710` sibling —
   boolean-equivalent with a null out; the sibling's internal delta stays the
   terrain-re open item) over the runtime height field, engine (X,Y) → field
   (x, −y).
2. **Ray context** `[orig: Physics_RaycastIntContext @ 0x5385e0]`: float-normalized
   16.16 direction, per-axis min/max box, length; segments < 16 raw (1/4096 u)
   return CLEAR without walking.
3. **Sector leg** `[orig: raycast_against_entity_pool @ 0x538720; pool 2 statics
   then pool 1 dynamics @ 0x539a3a]` per entity: in-use (+0x28) with a collision
   block (`graphicModel+176`, husk model +52 substituted when `Flags & 4`
   destroyed); skip `Flags & 1`, skip `Flags & 0x8000000` (unless the include
   flag), skip entityA/B, their collision handles (entity +364/+616 → ctx[19]/[20])
   and entities whose +0x28 owner-link equals A/B; bound-sphere broad phase
   (segment box + unclamped closest-approach projection, the shared @ 0x4139a4
   block); **itemDef type 3 (person)** = bound-sphere-only BLOCK with a same-team
   < 3.0 u exemption (ray end clamps to the person) `[orig: @ 0x5389b1-0x538a10]`;
   everything else clips the segment against the model's TYPE-1 volumes in
   section-local space (inverted section matrix from the model+168 transform
   callback — the same convex clip core as `Entity_RaycastCollisionModel
   @ 0x413060`). A hit shortens the ray (ctx[3..5]/[9], hit flag ctx[23], entity
   ctx[25]) and the walk continues for the nearest point; the boolean result is
   blocked either way `[orig: miss_result = 0 @ 0x5390e6]`.

Port: `CollisionWorld::raycast_clear` (terrain leg + the two-pass registry walk +
the owner-link exclusion + the `ray_line_distance` broad phase +
`collision_raycast_model`), first-hit early-out (boolean-equivalent). Residuals in
D-AI-7: the husk-model swap (no husk collision instances yet) and the type-3
person case (person-kind residents of the walked pools don't exist in our world —
organics are pool 0, unwalked, matching retail residency).

### 18.6 IDB write-backs (2026-07-16 session 4, saved)

Renames (anchored, ex auto-names): `Sound_TickPendingSlots @ 0x529310` (ex
`Sound_TickPendingSlots`), `Entity_UpdateMuzzleGlowEffect @ 0x56c960` (ex `sub_56C960`),
`g_SoundSpeedFixed @ 0x24d6660` (ex `dword_24D6660`). Entry comments:
`@ 0x528e40` (the inverted "dedicated server only" gloss corrected — the gate is
the is_client bit, TRUE in SP; delay formula + speed global), `@ 0x527c30`
(pending-sound queue shape + drain pointer), `@ 0x24d6660` (330.0 16.16),
`@ 0x539910` (ray-radius arg semantics, leg order, exclusions, type-3 person
case, reimpl link), `@ 0x538720` (per-entity walk detail), `@ 0x4ec0d0` (the
tracer decision + visuals map), appended `@ 0x53f440` (the ammo-def token
provenance for +64/+68/+36/+40).

## 19. Appendix: death presentation — the kill's anim pick, corpses, and the vehicle death rows (engine-research, 2026-07-16 session 5)

How a death LOOKS: the kill selects a directional death animation at damage time, the
infantry death edge consumes it into the anim state and seeds a corpse timer, the
corpse persists (indefinitely under `LeaveCorpse`, else until the timer drains AND the
local player cannot see it), and the AI-vehicle layer runs its own two-row death chain
(states 21 → 23). All addresses retail `Jointops.exe` (imagebase 0x400000, IDB
`Jointops.exe.kong.i64`).

PORT (same day, worktree play): the selector + the full 252-entry state tables =
`engine/runtime/world` (`compute_death_anim_state`, `death_quadrant_from_round`,
`kInfantryAnimNames/Flags`); the kill-time selection = `world::RoundSim::tick` (the
organic-kill leg); the death-edge consume + the corpse block = `AiSystem::tick_infantry`
(`engine/runtime/world/infantry.cpp`); rows 21/23 = `ai.cpp` (`h_enter_vehicle_dying`,
`h_vehicle_dying_tick/event`, `h_enter_vehicle_dead`, `h_vehicle_dead_tick/event`);
`deathtime` parse = `engine/formats/def`; the def traits stamp = `Simulation::
resolve_item_traits` (`leave_corpse`, `deathtime_ticks`); presentation =
the native `PresentApplier` (`godot/src/simulation/present_applier.cpp`;
dead ORGANICS stay visible; the sim ends the corpse via `Entity::hidden`). Pins: the `infantry` ctest (selector matrix, consume, countdown/
despawn, LeaveCorpse, the watch retry), the `ai` ctest (rows 21/23, the integration
kill's selection + group alert), the `def` items test.

### 19.1 The selector — `Entity_ComputeAnimSlotIndex @ 0x43a690` and the REAL table size

`(entity, boneIndex 0..31, quadrant 0..3, cause)`; out-of-range bone/quadrant clamp
to 0. Cause routes the family: `1` bullet → `180 + quadrant + 4*group` through a
32-entry bone→group table; `2` explosive → `176 + quadrant` (`death_grenade_*`);
`3` fire → 173 `death_fire`; `5` drown → 175 `death_drown`; anything else → 174
`death_pungi` (the preset default — the death edge passes 4).

The bullet matrix spans **15 groups × 4 quadrants = states 180..239**, NOT the 20
states the earlier §3.4 note assumed: `AnimMap_FindSlotByName @ 0x40cfa0` scans
exactly **252** entries of `g_animStateNameTable @ 0x8135F0`. Groups in table order:
hip, torso, head, rightshoulder, leftshoulder, rightarm, leftarm, righthand,
lefthand, rightthigh, leftthigh, rightcalf, leftcalf, rightfoot, leftfoot; quadrant
suffix order forward/right/back/left. States 240..251 are the FP viewmodel
`wpn_reset/idle/empty_idle/fire/recoil/reload/empty/switchto/switchfrom/switchrank/
scopeup/scopedown`. `g_animStateFlagsTable @ 0x8139E8` rows 200..239 = `0x82`
(the death family), 240..251 = 0. Bone→group (index: group): 0:0, 1-4:1, 5:3, 6:4,
7:9, 8:10, 9:5, 10:6, 11:11, 12:12, 13-14:2, 15:8, 16:7, 17:13, 18:14, 19-21:7,
22-24:8, 25-26:7, 27-28:8, 29-31:1.

### 19.2 The bullet kill selects at DAMAGE time — `Entity_HandleDamageTrigger @ 0x407310` type 1

This is the person item's `entity+0x1C8` damage/death callback (`Entity_InitFromItemDef
@ 0x49e550` installs it). Type 1 (the round kill, reading the global hit record
`Projectile_GetHitRecord()`: `[14]` = hit BONE section, `[16]` = the round entity):

- players (`Flags & 0x100`) → `Score_ProcessNetworkKillEvent`; NPCs → aiSlot move
  byte 2 + `TriggerGroup_SetAlertRed(commandGroup)` — a member's death alerts its
  group `[orig: @ 0x4073db-0x4073ea]`.
- quadrant = `(victimYaw − atan2BAM(roundVel.y, roundVel.x) − 0x60000000) >> 30`
  (BAM scale 683565275.5764316 = 2^32/2π) `[orig: @ 0x407478]`; selection =
  `ComputeAnimSlotIndex(entity, hitBone, quadrant, 1)` → **`entity+0x2C0`
  deathAnimStateId** `[orig: @ 0x407483]`.
- `Entity_ApplyCollisionForce @ 0x4af4a0` with ammo bytes +224/+225 (knockback).
- ammo dword +72 (a burn effect id) → spawn attached emitter (`entity+0x1CC` handle)
  and OVERRIDE the selection to 173 `death_fire` `[orig: @ 0x4076d5]` — incendiary
  kills burn regardless of bone.
- bodyRoll nudge on torso-region hits (signed `bone < 5`, not yet dead), on any
  machine processing the trigger (this leg sits BEFORE the authority gate):
  quadrant 0 → `bodyRoll (+0x94) = +0x5B05B00` (~8.0°) `[orig: @ 0x407564]`,
  quadrant 2 → `−0x5B05B00` `[orig: @ 0x407575]`; the slope conform then chases
  the grounded corpse's bodyRoll toward terrain (§3.5 item 4).
- DISMEMBERMENT (authority, NPC, bone > 0, `!(attrib & 0x800000 NoDismember)`,
  `health <= 0`, `health <= healthMax >> 1` — structurally redundant after the
  `<= 0` test, kept as written — not yet dead): the cut mask starts as the hit
  bone's own bit `1 << bone` `[orig: @ 0x407601]` and the 13-case switch ORs a
  linked-section addend (case 1 `0x1E67C`, 2 `0x1E678`, 3 `0x1E670`, 4 `0x1E668`,
  5 `0x10200`, 6 `0x8400`, 7 `0x20800`, 8 `0x41000`, 9 `0x10000`, 10 `0x8000`,
  11 `0x20000`, 12 `0x40000`, 13 `0x4000`; bones past 13 keep just their own
  bit) `[orig: @ 0x407608-0x40766f]`. The victim takes `sectionMask |=
  newBoneBits` (`newBoneBits = ~old & cutMask`) BEFORE the clone call
  `[orig: @ 0x407675-0x407684]`; `Entity_CloneFromTemplateByType @ 0x4398a0`
  (person → pool 0, memcpy of the FIRST `0x2B4` entity bytes + the `0xAC` AI
  block — NetId/Ssn/Team and `animSlotPtr +0x188` are inherited, so the two
  halves SHARE one anim slot; the tail past +0x2B4 keeps recycled-slot data)
  returns the clone, unchecked (pool exhaustion would fault). The clone then
  takes `sectionMask = ~newBoneBits` `[orig: @ 0x407691]`, `Health = 0`
  `[orig: @ 0x407697]`, emitter link `+0x1CC = 0`, `shadowSlot0 +0x1B6 = 0`,
  `DcbId +0x7C = 0` `[orig: @ 0x40769e-0x4076ab]`, and `velocity X/Y +=
  roundVel X/Y >> 8` — no Z kick `[orig: @ 0x4076b7/@ 0x4076c9]`. JO CP01
  soldiers author `nodismember`.
- Type 4 = reset/re-kill: health = 0 + re-select from the hit record. Any other
  type (the damage appliers call mode 2) → `entity+0x148 = 62` (a 1 s
  recently-damaged hold on the same field the corpse timer reuses).

The explosive path (`Entity_ApplyWeaponDamage @ 0x4e6820` person leg `@ 0x4e6a01`)
selects inline instead: bone hardcoded 1 (torso), quadrant from the IMPACT-to-victim
position delta, cause 2 — with a 25% fire roll in the 4..8 u band and cause 4 for
ammo type 7 — then `deathCallback(entity, 2, 0)`.

#### Port status (2026-08-15; corrected to the re-witness 2026-08-19)

The authoritative bullet/person path ports the witnessed body-roll and
dismemberment legs. `RoundSim` writes the exact quadrant-0/2
`bodyRoll = ±0x5B05B00` into the AI body's roll (`AiEntity::roll`, the same
`+0x94` store the §3.5 slope pass chases for a grounded corpse) and applies the
`(1 << bone) | addend` mask table plus every gate above. The victim hides
`oldMask | cutMask`; the pool-0 clone hides `~newBoneBits` (equivalently
`oldMask | ~cutMask`), starts at health zero, and adds the witnessed
`roundVel X/Y >> 8` horizontal kick. Two deliberate divergences, both
crash-guards on the original's unchecked clone: the victim mask commits only
AFTER a successful allocation (retail commits first and would fault through the
null return on actor-pool exhaustion), and the clone gets an independent
AiEntity copy seeded to the victim's pose/anim state instead of sharing the
victim's anim slot pointer (both halves start the same death clip on the same
tick, so the observable pose matches). How the retail CLONE's own §19.3 death
edge behaves is unwalked (its copied `Flags` lack bit 2, so the edge would run
against a recycled-slot `+0x2C0`; whether that doubles the scream is unknown) —
the port suppresses the clone's scream and seeds its selection explicitly.

Identity and the wire (WITNESSED 2026-08-19, closing the LAN question): retail
severs the connection id, shadow slot, and burn-emitter link but
memcpy-inherits NetId/Ssn/Team — and the clone reaches every wire channel an
NPC corpse does. The 0x0A priority walk admits every live pool-0 slot with a
def and a nonzero ItemDef+356 class tag — no dead/team/connection filter
`[orig: Server_BuildEntityPriorityList @0x50e590, pool-0 admission
@0x50e6cb-0x50e6da]`; the join download serializes every pool-0 slot (the
0x0C batch's single caller is `Server_SendInitialGameStateToPlayer`, call
@0x51bd24); and a connected joiner that sees compacts for its empty local
slot self-heals it via C2S 0x0F → S2C 0x18 (net-re §5.46). Crucially, NONE of
those records carries `sectionMask +0x134` (§5.23 / §5.46 field maps; the
0x0C decode memsets the entity first), so every REMOTE view renders both
halves as whole-body corpses — the section-mask split is host-local
presentation in retail. The port mirrors all of it: the clone rides
`snapshot_world` and `build_pool0_organic_batch` (the 0x0F→0x18 reply path
already resolves any registry slot), masks present host/standalone-only
through the entity-resolved rows (exact 16-bit halves into `ObjectModel`
part visibility), and the one wire-visible divergence is deliberate: the
clone's net_id/name are cleared instead of duplicated, because our ascending
first-match `find_by_net_id`/`find_by_name` could misroute scripted refs to
a lower-slot clone (retail's backward-scanning allocator makes the dupe
mostly land above the victim).

Ammo properties `secondary_anim` and `kz_physics` now parse and bake into their
witnessed +224/+225 byte slots. They are deliberately not interpreted yet: the
interior arithmetic of `Entity_ApplyCollisionForce` remains unwitnessed. The
ammo-dword +72 authoring/load source is likewise still unresolved, so the
incendiary emitter and death-state-173 override remain open rather than being
guessed.

### 19.3 The infantry death edge — `Entity_UpdateInfantryAI @ 0x4b9c40` (health ≤ 0, once)

Guards: word `entity+0x11E > 0` skips the edge (writer unwalked); `Flags & 2`
already-dead skips. Then, in order `[orig: @ 0x4b9c40-0x4b9d55]`:

1. mounted (`entity+0x16C`) → `Entity_DetachFromVehicleIfServer @ 0x4359d0` — the
   corpse drops out of its seat; the edge tail also clears `Flags & 0xC0`.
2. corpse timer `entity+0x148` = `def+0x890` deathtime. The `byte entity+0x134 & 1`
   variant instead: respawn tickets `+0x35E` = 0, timer = deathtime − 61, and NO
   scream (a silent-cleanup mode; the bit's writer is unwalked).
3. the death scream: `Entity_PlaySound3D_FullVolume(Entity_GetProfileSlotSound(
   entity, slot))`, slot 8 `SSNightDead` when `Bms_AttribFlags & 0x100000`
   (EnableNVG = night) else 7 `sounddeath` — the def's resolved sound-profile
   table, not ammo sounds. PORTED 2026-07-17 (§17.4b).
4. consume `+0x2C0`: zero → `ComputeAnimSlotIndex(0,0,4)` = 174 + dispatch
   `deathCallback(entity, 1, 0)`; then **animState `+0x2BC` = the selection**
   (drowning `Flags & 0x8000` overrides to 175), pending `+0x2B8` = 0, `Flags |= 2`,
   death tick stamp `+0x1AC = current_tick`, `+0x2C0` = 0 (consumed), authority →
   `Entity_CheckAndProcessDeath @ 0x51b550` (the §5.60 net/scoring router).

The death clips are non-looping — the anim channel clamps at the last frame, so the
corpse holds its pose (the port's `InfantryRootMotion` documents the same clamp).

### 19.4 Corpse persistence — the dead leg `@ 0x4b9e4d-0x4ba000`

Per dead tick, after the weapon/drag block:

- **LeaveCorpse** (`attrib & 0x400000`) && `!(byte +0x134 & 1)` → skip everything:
  the corpse never expires.
- decrement `+0x148` toward 0. At exactly **186** remaining (~3 s) with a
  `particledeath` handle (`def+0x412`, word) → release any `+0x1CC` emitter and
  spawn the decay effect attached at the corpse (persons author none; vehicles do).
- at 0: respawn tickets `word +0x35E > 0` → the NPC-respawn path (spawn pose
  `+0x318`, a `+0x354`-linked same-team gate that can set `Flags |= 1` hidden)
  — unported, mission NPCs author none. Else: in SP (`!is_in_session`) with no
  decay effect, raycast corpse→local player (`Physics_RaycastTerrainAndSectors`,
  entity origins); CLEAR = the player can SEE the corpse → `+0x148 = 62` and retry
  next second `[orig: @ 0x4b9f83]`. Blocked/MP → `Entity_Destroy @ 0x43e810`.

Bonus decode: the `entity+0x350` branch above this block is the MEDIC-DRAG follow —
a linked dragger's hand-bone world delta moves the corpse each tick and forces anim
139 `draggee` `[orig: @ 0x4b9d9c-0x4b9e41]`.

The port maps despawn to `Entity::hidden` (our registry keeps the slot; the health
store already gates every consumer) and runs the watch-check whenever a local player
exists — our SP listen-server equivalence for the retail `!is_in_session` gate; the
watch ray lifts both endpoints 0.9 u explicitly at the call site (since 2026-08-13
`line_of_sight_clear` takes exact endpoints), standing in for the entity-origin ray
(the feet-ray false-block on the heightfield leg; deliberately NOT the muzzle seam —
a corpse has no live pose). Both are D-AI-9 residual notes.

### 19.5 The def side — `deathtime`, `particledeath`

- `deathtime` `[orig: ItemDef_ParseProperty @ 0x49fa5a-0x49faa0]`: `def+0x890 =
  (v ? 62·v : 496) + 62` ticks — the token is SECONDS at the 62 Hz tick with a
  one-second grace; an explicit 0 authors 9 s. Absent token = 0 (zero-init) — such
  a corpse expires on its first dead tick (watch permitting). JO persons author
  `deathtime 30` → 1922 ticks ≈ 31 s. The port scales at parse, exactly.
- `particledeath <effect>` `[orig: @ 0x4a164b → def+0x416 name]`, resolved at
  mission start by `resolve_item_materials_and_spawn_bone_trails @ 0x52310f`:
  `CEffectWorld_InternEffectHandle` → `def+0x412` (1-based handle; doubles as the
  vehicle death effect), plus `def+0x414 = ItemDef_GetBoneMaskByName(def, "Dead")`.
- `LeaveCorpse` = attrib `0x400000`, `NoDie` = `0x40000000`, `nodismember` =
  `0x800000` `[orig: @ 0x4a09a3-0x4a0a10]` (already in the engine/formats/def attrib table).

### 19.6 The vehicle death chain — dispatch rows 21 and 23

Row dwords `@ 0x815388` / `@ 0x8153A8` (enter/tick/exit/event; shared exit
`nullsub_70 @ 0x457670`). Only the authority runs the full SM; clients run rows
21/23 too (`EntityAI_ProcessVehicleStateMachine @ 0x4583c0` — wrecks settle
everywhere).

- **enter 21** `AI_TransitionToDeath_GroundVehicle @ 0x467b20`: when not yet husked
  (`!(Flags & 4)`) → `Entity_UpdateDeathTransforms @ 0x494660` (savedLivePose
  snapshot `+0x80..` = Position/euler, `Entity_DispatchDeathCallback @ 0x493ef0`,
  `Entity_InitDeathSounds @ 0x4939b0`) + authority-in-session S2C 0x26; `def+1352`
  → kill each mounted child (health 0 + child `deathCallback(child, 1, 0)`); the
  §16.1 alert block (alert 2/2, group red, ally wake 100 u); `moveStep = 16`;
  already slow (horizontal |vel| < 1057) → queue AIEvent **4** now.
- **tick 21** `AI_TickState_VehicleDying @ 0x467cd0` (ex kong `AI_CheckVehicleStuck`):
  `Entity_ProcessFallingDeathPhysics @ 0x461d30` settle; |vel| < 1057 OR static vs
  savedLivePose (< 1024 per axis) → queue event 4; still moving → zero work
  pitch/roll (`ai[133]/[134]`), the commanded speed (`ai[136]`), the mover output
  (`ai[128]`).
- **event 21** `AI_HandleEvent_VehicleDying @ 0x457f50` (ex `AI_HandleRetreatEvent`):
  ONLY type 4 lands → pending 23.
- **enter 23** `AI_TransitionToDestroyed_Vehicle @ 0x467de0`: the alert block;
  `entity+0x148 = 0` (wrecks never run the corpse countdown) and `entity+0x162
  team byte = 0` (a wreck goes teamless and drops out of target scans); death
  transforms + 0x26 when not yet husked; death tick `+0x1AC` (first write wins);
  `Entity_ClearAllReferences @ 0x465670` + `Entity_SetAITarget(0)`; `moveStep = 62`.
- **tick 23** `AI_TickState_VehicleDead @ 0x467ea0` (ex
  `Entity_ProcessMountedInfantryFrame` misnomer): keep settling; `itemDef attrib &
  0x40` + authority = the wreck RESPAWN watcher (`thinkCooldown` countdown after 15
  ticks → teleport to the `pad_24c` spawn pose — parent-relative if the parent
  lives — or the `+0x2c`-bit0 bury-at-+5000/−1000 variant → `Entity_RespawnVehicle
  @ 0x45ff40`). Without the attrib the wreck settles forever.
- **event 23** `AI_HandleEvent_ConsumeAll @ 0x458080`: swallow everything.

`Entity_DispatchDeathCallback` routes `def unitType` through the table `@ 0x815410`
(stride 16: {type, flagBits, param, callback}); the DEFAULT (no row) is
`Entity_SpawnDeathPieces @ 0x493400` + `Flags = Flags & ~0x20006 | 6` — dead (2) +
**husk swap (4)**: rendering and ray collision switch to the def's husk model
(`Entity_InitFromItemDef` +52/+56), which is how a wreck LOOKS destroyed. The
port's shared production-mode destruction pass walks installed death callbacks
in pools 1/2, including AI-capable entities, so the rows 21/23 settle is live;
the row-handler `unported_calls` counters remain diagnostic. The def+1352
child-kill loop and attrib-0x40 wreck-respawn watcher remain visible stubs
(D-AI-9). The organic infantry edge queues NO SM death event (organics never run
the cveh SM tick; our earlier bring-up event is removed).

### 19.7 Open follow-ups (this session's unknowns)

1. `entity+0x11E` (the death-edge skip word) and `byte +0x134` bit 0 (the silent
   cleanup) — writers unwalked.
2. The drowning source (`Flags & 0x8000` → 175) rides the unmodeled swim flags.
3. The player edge (`Entity_UpdateInfantryPlayerBody @ 0x4b4c72/0x4b61c6/0x4b7d83`
   sites) shares the same consume; the player-death PRESENTATION (death camera,
   respawn flow) is the P2b slice.

### 19.8 IDB write-backs (2026-07-16 session 5, saved)

Renames (anchored, ex kong misnomers/auto-names): `AI_TickState_VehicleDying
@ 0x467cd0` (ex `AI_CheckVehicleStuck`), `AI_TickState_VehicleDead @ 0x467ea0` (ex
`Entity_ProcessMountedInfantryFrame`), `AI_HandleEvent_VehicleDying @ 0x457f50` (ex
`AI_HandleRetreatEvent`), `AI_HandleEvent_ConsumeAll @ 0x458080` (ex `sub_458080`).
Entry comments: the four rows, `@ 0x49fa96` (the deathtime formula), `@ 0x4b9c40`
(the death edge), `@ 0x4b9e4d` (the corpse block), `@ 0x43a690` (cause routing +
the 15 groups + the quadrant convention), `@ 0x52310f` (particledeath intern).

## 20. Appendix: the round-outcome loop — WAC/BMS win-lose, round end, SP end presentation (engine-research, 2026-07-16 session 6)

The P2 playability slice: how a mission DECIDES it is over, what the server does
at round end, and what the SP player then SEES. Port surfaces:
`engine/runtime/wac/vm.cpp` (win/lose + the outcome builtins),
`engine/runtime/mission/event_runtime.cpp` (the BMS win actions + zone-ref resolution),
`engine/runtime/world/world.cpp` (`World::process_round_end`, `EntityCommands::resolve_ssn`),
`engine/net/npruntime/server_tick.cpp` (kill tallies, `humans`, the win-condition
check, the respawn hold), `godot/game/world/game_hud_presenter.gd` (the lose banner),
`godot/game/mission_end_screen.gd` + `main_game.gd` (the end screens + exit).
Evidence ctests: `wac_behavior` (the outcome builtins + the 04TR else-if block),
`npruntime_round_end`, `event_runtime_bms` (BlueWin + zone-ref resolution);
in-game (today the `lose_flow_04tr` ctest): the former `round_outcome_probe.gd` (04TR.bms PASS 2026-07-16: real-round
teammate kill → bluekills → `Lose(1)` → KILLEDBLUE banner → round end winner 2 →
FAILED screen → ESC to menu).

### 20.1 Producers — every caller of the round end

`Server_ProcessRoundEnd @ 0x5164f0` is the single round-end entry. Witnessed
producers:

- **WAC `win n`** [orig: WacAction_Win @ 0x4ed4a0] — `Server_ProcessRoundEnd(n)`
  straight through (1 = blue/player win; 0 = green).
- **WAC `lose n`** [orig: WacAction_Lose @ 0x4ed3f0] — n=0 resolves
  `Misc/STRMISC_KILLEDGREEN`, n=1 `Misc/STRMISC_KILLEDBLUE`, each through the
  banner trio (§20.6), then `Server_ProcessRoundEnd(2)` (red wins = the player
  side loses). Any other n is a NO-OP returning 0. Both handlers were undefined
  code before this session (define_func'd).
- **BMS Blue/Red/GreenWin** (actions 8/9/10) [orig: EventAction_Dispatch
  @ 0x45447b/0x454495/0x4544af] — `Server_ProcessRoundEnd(1/2/0)`; the call sites
  check the round-over latch first (our latch lives inside `process_round_end`).
- **The SP auto-lose** [orig: Server_CheckWinConditions @ 0x51ad40, SP leg
  @ 0x51ad6f] — the ONLY automatic SP condition: local player dead
  (`Flags & 2`) and the mission does not author `SinglePlayerRespawn`
  (`Bms_AttribFlags & 0x40`) → `Server_ProcessRoundEnd(2)`. Runs at 1 Hz from
  the periodic-second block [orig: @ 0x51df5a]; the latch no-ops it
  [orig: @ 0x51ad4a]. Every other SP outcome comes from the script producers.
- **MP legs** (unported, net track): the zone-ownership sweep
  (`ZoneSlotChain_GetWinningTeamIfAllOwned @ 0x4a2920`) and per-game-type
  score/time/kill-limit checks over the team stat blocks (game types 0,
  0x10000, 65537, 65540, 65538/589826, 65544, 8, 65552/327696); admin
  goto/mission commands and a round-end tail fragment `@ 0x4d31c0`
  (`Server_ProcessRoundEnd` + a 620-tick linger) — defined this session.

### 20.2 `Server_ProcessRoundEnd @ 0x5164f0` (fully decompiled)

Order of operations: double-run guard on `g_spawn_success_gate @ 0x24c1928`
[orig: @ 0x516502] → `g_round_winning_team @ 0x24c1924 = winner` [orig: @ 0x516528]
→ the winner-team scoring pass (team games: `GameEvent_ProcessScoring @ 0x52f550`
per winning-team member, team byte entity+354, gate `g_GameType & 0x10000`)
[orig: @ 0x516530] → `Server_BuildEndOfRoundScoreboard(1, winner) @ 0x508f30` →
server tick-phase metrics → phase 4 → **MP-only** `g_endround_linger_timer
@ 0xc8d820 = 2790` (45 s) [orig: @ 0x5166c4] → per active slot in state 6: slot
bookkeeping reset, S2C 0x61 round-end marker (4 zero bytes) [orig: @ 0x516790],
the winner-bonus scoring leg, S2C 0x1D scoreboard header [orig: @ 0x516839],
`CNetPlayer_SetGameState(11)` [orig: @ 0x516846], slot state 6→7
[orig: @ 0x51685e] → the per-team round-win counters for game types
0x10000/65537/65540 [orig: @ 0x5168a0] → **the latch** `g_spawn_success_gate = 1`
[orig: @ 0x5168e4] → binocular/scope clears → the SP tail (`!is_in_session`)
[orig: @ 0x51691d]: `DialogAudio_PlayNextChunkOrStop(0) @ 0x44e320` (force-stops
the dialog channel) + `Dialog_ResetAll @ 0x44dc90` + `sub_5280B0` (parks the
active music state) + winner==1 ? `Cine_InitPlayback @ 0x578390` +
`MusicCtx_SelectEndTrack(1)` : `Cine_StartPlayback @ 0x577840` +
`MusicCtx_SelectEndTrack(2)`.

Port: `World::process_round_end` carries the guard, the winner, the latch, and
surfaces the SP tail as the `round_end` shell effect; the scoring pass, the
scoreboard block, and every wire leg are cited stubs (net track).

The gate CLEARS at `Game_StartMission @ 0x524a1f` and the client GameReset
handler [orig: NapiNPClientMsg_GameReset @ 0x422849]; `Cine_StartPlayback` also
SETS it `@ 0x577848` (the lose cine implies round-over). The SP restart
[orig: Game_RestartRoundSP @ 0x5263a0, `g_mission_exit_reason == 4`]:
`Game_DestroyAllEntitiesAndReset` → (authority) the flag-4 event-trigger pass →
if the SP round was WON copy `dword_24C1960 → dword_24D2500` → round-state init
(`sub_54D6A0`) → `Server_DisconnectAndResetAllPlayerSlots @ 0x516160` →
`Game_StartMission(1)`.

### 20.3 The WAC named-value table (the `bluekills` family)

One static table drives every named engine value WAC scripts resolve:
**24 records `{char name[16]; u32 param_type; u32 value_ptr}` at `@ 0x82EEF0`,
count dword at `@ 0x82F130`**, resolved case-insensitively by the third lookup
leg of `WacScript_ResolveParameter @ 0x4f2940` (`*outType = 1` → pointer).
Param types seen: 2 = int var, 9 = time (CurTOD), 0xB = entity handle.

| name | value | witnessed semantics |
|---|---|---|
| result | `@ 0xC6EB24` | the VM accumulator |
| ticks | `@ 0xC6EAD8` | script executions; +1 per run [orig: WacScript_AdvanceTick @ 0x4f81d3]; seeded 0 at load |
| GameOver | `@ 0xC6EB0C` | derived `winner != 0` — a green(0) outcome never raises it [orig: cache pre-pass @ 0x4f57bb] |
| WinVar | `@ 0xC6EB08` | derived `winner == 1` [orig: @ 0x4f57c9] |
| LoseVar | `@ 0xC6EB04` | derived `winner == 2` [orig: @ 0x4f57cf] |
| SquadSSN / SquadWho | `@ 0xC60DCC` / `@ 0xC60DC4` | squad slots (type 0xB / 2) |
| night / seatbelt / wind | `@ 0x26C645C` / `@ 0xC6EADC` / `@ 0x26C68C0` | env/options mirrors |
| breathtime / fallmps / accuracyspread | `@ 0xC6EAE0` / `@ 0xC6EAE4` / `@ 0xC6EAE8` | player tuning; **accuracyspread IS the D-AI-6 aim-error global** (read by `Entity_UpdateInfantryAI @ 0x4bc5ea`) — mission scripts can set it |
| autogain / health / mana | `@ 0xC6EAFC` / `@ 0xC6EB00` / `@ 0xC6EAF8` | health = live player hp mirror (entity+0x11E); mana = entity+0x120 (cached @ 0x4f582e; field semantics open) |
| bluekills / greenkills | `@ 0xC846F0` / `@ 0xC846F8` | §20.4 |
| humans | `@ 0xC6EB14` | active human slot count [orig: Server_BuildEntitySlotLists @ 0x4f97c6 zero / @ 0x4f98b1 +1]; ALSO the world-run gate: entities/WAC advance while `humans > 0 \|\| ticks == 0` (empty-dedicated-server sleep) [orig: @ 0x51d8bd / Game_ProcessMainFrame @ 0x52671c] |
| RND / Player / Item / auto / CurTOD | `@ 0xC6B23C` / `@ 0xC6EC3C` (x3) / `@ 0xC6EB10` | Player/Item/auto share the slot-handle cache; CurTOD = `Env_CurTimeFixed24 / 279620` |

**Correction:** an earlier session read this table phase-shifted by one record
(name paired with the FOLLOWING record's value) — that mapping (`autogain →
0xC6EAE8`, `bluekills → 0xC6EAF8`, `GameOver → 0xC6EAD8`, ...) was wrong; the
resolver decompile pins the true anchors (name @ +0, type @ +0x10, value @ +0x14).
Port: `Builtin` ids 8-13 in `engine/runtime/wac` (`bluekills/greenkills/humans/GameOver/
WinVar/LoseVar`) read `World::kill_stats` / `cached.humans` /
`World::match.outcome()`.
The writable `accuracyspread` row is also ported: case-insensitive compilation
resolves it as a named engine-value lvalue, VM reads/writes
`World::wac_values.accuracy_spread`, and the infantry aim pass consumes that same
field in the witnessed formula. `event_runtime_bms` pins write/readback, no V0
alias, and the resulting NPC aim heading.

### 20.4 The kill tallies (`bluekills`/`greenkills` and the epilog buckets)

`Score_ProcessKillEvent @ 0x4fd400` runs per kill from the damage chain
(`Entity_ApplyWeaponDamage @ 0x4e6bfe/0x4e6fb4`, `Projectile_ProcessDamageOnTarget
@ 0x4e8133`, vehicle/collision legs), authority-gated, and **outside net sessions
only** [orig: the `!is_in_session` gate @ 0x4fd447]. Killer == the local player →
`Score_TallyKillByLocalPlayer @ 0x4fd160` (ex-`Score_ProcessDamageEvent`
misnomer); anyone else → `Score_TallyKillByOthers @ 0x4fd300`. Bucket family
(count @ first addr, score-sum pair at +4):

- by-player: infantry `@ 0xC846D8`, vehicle (types 3/4) `@ 0xC846E0`, aircraft
  (type 9) `@ 0xC846E8`, **team-1 persons `@ 0xC846F0` = WAC `bluekills`**,
  **team-0 persons `@ 0xC846F8` = WAC `greenkills`** (person gate itemdef+92==3;
  blue/green NON-persons tally NOTHING; unit type from def+406, score def+404,
  difficulty scaling ¾/3⁄2 on `dword_24D2110`).
- by-others: infantry `@ 0xC846A8`, vehicle `@ 0xC846B0`, heli `@ 0xC846B8`,
  team `@ 0xC846C0`, friendly `@ 0xC846C8`, human-player victims `@ 0xC846A0`
  (entity+534 flag).

Team space matches the round-end codes: 0 = green, 1 = blue, 2+ = enemy. The
epilog count lines sum the pairs (TEAMUNITS = `0xC846F0 + 0xC846C0`, etc.).
Port: `MissionKillStats` intentionally remains the count-only WAC/epilog view,
tallied in `route_round_deaths` from `RoundDeath.victim/killer`; the SP gate is
`!world.mp_session` because our SP-as-listen-server always runs
`ctx.is_in_session = 1`. Independently, the same death enters `world::Match`'s
42-field player/team records with the signed `score.ini` event values used by
every competitive game type, co-op, and the end-round board. The SP epilog's difficulty-scaled
score sums remain unmodeled. `[orig: GameEvent_ProcessScoring @0x52F550;
CPlayerStats_RecordEvent @0x52C8E0]`

### 20.5 The outcome state + the client commit (S2C 0x1D)

`@ 0x24c1970` (ex kong `g_scoreGameType` — renamed `g_endround_winner_team`) is
**the winning team number**, the first dword of the 0xE444-byte end-round
scoreboard block: memset 0 at mission start [orig: Game_StartMission @ 0x5249df]
and by `Server_BuildEndOfRoundScoreboard @ 0x508f3f`, then `= winningTeam`
[orig: @ 0x508f77]. The WAC outcome builtins derive from it each pre-script tick
[orig: WacScript_CacheLocalPlayerState @ 0x4f5780]. The scoreboard header
serializer [orig: EndRoundScoreboard_SerializeHeader @ 0x505280, ex
`WeaponOverlay_SerializeToBuffer` misnomer] writes the SP/team-game form
`[u8 winner][s16 teamScore0][s16 teamScore1][u8 draw][s8 myEntryIndex]` (7 B;
non-team MP substitutes 3 winner-name strings + 3 s16). The client commit
[orig: NapiNPClientMsg_0x01D @ 0x430840]: non-authority latches the gate +
`linger = INT_MAX`, parses the form, stores winner/scores/draw
(`g_endround_draw_flag @ 0x24cfdb0`), recalcs scoreboard tiers, MP peers ack 0x2B.

The 2790-tick linger is **MP-only**: `Server_TickUpdate` drains it on the
authority [orig: @ 0x51d9cc] → mission metrics + `g_mission_exit_reason = 3`
(4 when replay-chaining); `Client_ProcessNetworkFrame` drains the peer copy
[orig: @ 0x42c3d1] → `reason = 4` → the "Game Loop" scene. SP never drains it —
the epilog owns the SP exit (§20.6).

Port (completed 2026-08-22): `World::process_round_end` latches one immutable
`MatchResult`; the authority pushes 0x61 + the exact recipient-specific 0x1D,
serves the board through requester-only 200-byte 0x56 chunks, and drains the
2790-tick MP linger. The joiner starts and continues the 0x2B pull loop
automatically. The full producer schema and its remaining presentation-only
residue are tracked in net-re §5.68.

### 20.6 The SP end presentation

The lose banner trio [orig: WacAction_Lose → `GameMsg_AddChatLineAndRelay
@ 0x5ba170` (chat channel 1 id 930; authority relays the gametext KEY, clients
re-resolve), `GameMsg_SetBannerText @ 0x5ba200` (`g_banner_text @ 0x28E3DA0`),
`GameMsg_SetTeamBannerText @ 0x5ba1d0` (`@ 0x28E41A0` + team)] — the banners
persist until the round-start HUD reset clears them [orig: Game_InitMissionRoundState (ex sub_5B71B0) @ 0x5b72ad].

Per-frame cine dispatch [orig: Cinematic_EpilogUpdate @ 0x577950] on
`g_cine_mode @ 0x2696dc4` (1 = win set by `Cine_InitPlayback`, 2 = lose set by
`Cine_StartPlayback`):

- **WIN epilog** [orig: epilog_cinematic_state_machine_update @ 0x576240,
  `g_epilog_win_state @ 0x2696db4`]: `Cine_InitPlayback` loads `<mission>.cne`
  if present, else a static player-pose camera + letterbox; states 0-3 = the
  flyaway (altitude wait, accelerate, steer at the extraction point with
  distance fades + a proximity sound); state 4 builds the score screen —
  `jo_Epil.tga` + `Epilog/STREPILOG_OBJECTIVEBONUS,_ENEMYUNITS,_TEAMUNITS,
  _FRIENDLYUNITS,_KEYINFO` count lines fed by the §20.4 buckets (enemy line
  capped at `@ 0xC84690`), `g_epilog_screen_active @ 0xa87054 = 1`; state 5
  fades, 18600-tick timeout → `g_mission_exit_reason = 1`. A FULLER sibling
  `@ 0x575a90` (adds FAST_PLAY_BONUS + TOTALPOINTS with point values) has
  ZERO xrefs — dead code in retail Jointops.exe.
- **LOSE screen** [orig: the mode-2 leg `@ 0x5744fd..`, `g_epilog_lose_state
  @ 0x2696d9c`]: fade pair + `jo_Epil2.tga` + `Overlays/STROVER_MISSION_FAILED`
  (y=120) + **the `g_banner_text` line (the WAC lose cause, y=230)** + the
  saved-game list + `STREPILOG_KEYINFO`/`_KEYINFO2`; state 2 fades 0.01/tick,
  18600-tick timeout → `reason = 1`.

Exit keys: ESC (0x1B) → `reason = 1` [orig: Input_HandleSpecialKeys @ 0x49c8e2];
the exit action binding → 1 [orig: Input_HandleActionBinding @ 0x49af26]; the
death-screen key → 4 (SP restart) [orig: @ 0x49c8ad]. The main loop
[orig: Game_ProcessMainFrame @ 0x526806..0x526867]: `reason == 4` in SP →
`Game_RestartRoundSP`; any other nonzero reason → push the **"Post Menu"**
scene. `MusicCtx_SelectEndTrack @ 0x672fd0` (thunk `@ 0x671ba0`) writes 1|2
into a stream context byte `@ 0x3245B08` — the end-music selector shape; the
reader rides the context pointer (unwalked). The SP world keeps ticking through
the epilog (`humans >= 1` holds the run gate; MP freezes entities on the gate
instead [orig: @ 0x526742]).

Port: `game_hud_presenter.gd` resolves the lose KEY against gametext `Misc` (chat
line + stored banner); `main_game.gd` consumes the `round_end` effect →
`mission_end_screen.gd` (win = letterbox + jo_Epil.tga + the four count lines;
lose = jo_Epil2.tga + STROVER_MISSION_FAILED + the banner + KEYINFO), 300 s
timeout, ESC → teardown to the menu (the Post Menu stand-in). In-game verified
via the `lose_flow_04tr` ctest, formerly `round_outcome_probe.gd` (the retail string "You have killed a teammate."
resolved from gametext.bin).

### 20.7 Script SSN + zone-ref resolution (fixed during the probe)

Two load/response-time resolutions the probe forced out:

1. **The player SSN**: mission scripts address the local player as SSN 10000
   (retail player entities carry 10000+slot as their net id, so
   `EntityPool_FindByNetId @ 0x4f0a20` resolves them like any SSN). Our player
   entities deliberately carry `net_id 0` (the wire is handle-based), so
   `EntityCommands::resolve_ssn` restores the mapping at the script seam
   (10000 → `cached.local_player`); MP joiner SSNs (10001+) wait on the net
   track.
2. **Zone refs are IDs in the file, indices at runtime**: see
   [bms-event-runtime-re §7.3](../mission/bms-event-runtime-re.md) — the
   mission-start resolvers [orig: EventTrigger_ResolveZoneTriggerRefs
   @ 0x453000 / EventTrigger_ResolveZoneActionRefs @ 0x453100, both ex-"weapon"
   misnomers] rewrite trigger/action zone ids to array indices and NEUTER
   dangling/degenerate refs. Unported, 04TR's negated
   `SingleIsWithinArea(10000, zone 6)` out-of-bounds watchdog fired RedWin at
   spawn; ported (`BmsEventSystem::resolve_zone_refs`), the mission plays.

### 20.8 Divergences

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-AI-10 | Multiplayer core is no longer a stand-in: `world::Match` owns TDM score/time and A&S all-owned/zone-count decisions while WAC/BMS drives co-op; one guarded result awards the round marker, freezes the retail scoreboard producer order, pushes 0x61 + recipient-specific 0x1D, serves requester-only 0x2B/0x56 chunks ≤200 bytes, moves peers to game state 11, closes ordinary replication through Match's sole outcome gate, and drains the phase-exact 2790 ticks `[orig: Server_CheckWinConditions @0x51ad40; Server_ProcessRoundEnd @0x5164f0; Server_BuildEndOfRoundScoreboard @0x508f30; Server_TickUpdate @0x51d7e0]`. The one semantic gate replaces retail's redundant slot-state 6→7 write without changing wire behavior. Default VAR/FIELD rows and VERSION 40 `score.ini` overlays are shared by gameplay, S2C 0x58, and the frozen board `[orig: GameType_CreateDefaultSettings @0x52dd00; ScoreConfig_LoadFile @0x52d8a0]`. Remaining stand-ins: (a) SP kill tallies are COUNTS only (no def+404 points, difficulty scaling, per-type split, or human bucket); (b) additional score events await their gameplay producers; (c) the SP end presentation is a shell overlay — no flyaway cine/`.cne`, count-up lines, saved-game list, or end-music switch; (d) `sub_5280B0` music-park and the `@0x3245B08` end-track selector remain unported; (e) the SP gate reads `world.mp_session` (our listen server always has `ctx.is_in_session = 1`) | the full @0x5164f0 transaction + §20.6 cines | MP core CLOSED; SP scoring/presentation and additional gameplay score producers remain |

**D-AI-10 update (2026-08-22, superseding the TDM/A&S-only wording above):**
`world::Match` now implements every branch of `Server_CheckWinConditions
@0x51AD40`: DM/TDM, solo/team KOTH, S&D/A&D, CTF, FlagBall, Flag Me, A&S/C&C,
plus the universal all-zones-owned check; co-op remains intentionally
WAC/BMS-owned. It also owns the recovered per-mode `ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52C850`
primary score, flag carry/pickup/save/capture/return transitions, demolition
target scoring, live 0x16 team projection, and authored objective census.
Therefore residual (b) no longer applies to these retail game-mode producers.
The remaining D-AI-10 scope is SP-only tallies/presentation/music and any
ordinary combat event producer not enumerated here.

The hill hold itself is accumulated by the per-second team pass: a team with at
least one alive holder in the volume gains one tick, an empty team loses
`koth_delta` per second clamped at zero, and the same pass folds each in-game
player's slot dword 23595 (the objective-ticks counter) into the team row's
+328 accumulator. `[orig: Game_AccumulateTeamScores @0x508DA0..0x508DC2,
@0x508DDE..0x508E19; koth_delta = g_koth_delta @0x24D2148]`

The co-op family is now composed through its authored result producers at the
host boundary instead of being inferred from a direct latch test. Stock Co-op
executes WAC `Win` on the VM's 62-tick cadence; Objective Co-op dispatches BMS
`RedWin` on the normal 16-tick event pass. Both are observed entering
`Server_ProcessRoundEnd`, emitting 0x61/0x1D, and consuming their originating
tick from the 2790-tick linger exactly like retail. `[orig: WacAction_Win
@0x4ED4A0; EventAction_Dispatch EventAction_Dispatch @0x4542E0, the RedWin case @0x454495;
Server_TickUpdate @0x51DA04; Server_ProcessRoundEnd @0x5164F0]`

KOTH/TKOTH now retains the authored mission producer as well as the decision
branch. BMS type 6006 copies `wp_distance << 16` (or default `0x8000`) into
entity+0; the 1 Hz proximity pass compares players against that exact hill
radius. The pool-3 S2C 0x20 load writer and client handler carry the same raw
dword under flag 0x02, so a joining replica reconstructs the authored volume
without a mode-only side channel. `[orig: Entity_SpawnFromBMSRecord
@0x40F157..0x40F173; Server_UpdateCaptureZoneProximity
@0x5089E8..0x508A68; serialize_entity_pool_to_packet
@0x503593..0x5035A9; NapiNPClientMsg_0x020 @0x425D07..0x425D1B]`

S&D/A&D demolition is connected to ordinary combat rather than a script-only
score hook. A lethal kinetic or blast hit on an ItemDef-attrib `0x8000` target
stages the shared `RoundDeath`; host death routing records scorer event 11
(raw field 14 plus the score-table delta) before the 1 Hz authored-census win
check, and the result enters the same 0x61/0x1D round transaction. The composed
native regression covers both game types from the death boundary through the
wire, while the projectile/destruction regressions pin both upstream lethal
producers. `[orig: Entity_ApplyWeaponDamage @0x4E6820;
GameEvent_ProcessScoring @0x52F550; Server_CheckWinConditions @0x51B18B]`

Flag Me retains one retail defect rather than silently becoming a private
OpenNova ruleset. Its selector is 12, but the FIELD loader, event scorer, and
session-status value copy each accept only rows `<=11`; captures still run the
ordinary event/reset transaction, while no player/team stat changes and the
MaxScore winner arm is consequently unreachable through ordinary play. The
shared `game_type::score_table_index` pins that fact for all three consumers.
`[orig: load_scoring_table_for_game_type @0x52D300;
GameEvent_ProcessScoring @0x52F550; Server_BuildStatusReport @0x530A60;
Server_CheckWinConditions @0x51AD40]`

C&C keeps both halves of its retail objective vocabulary. Flag and bay contact
is item-ID driven, not game-type gated, so C&C can carry/capture/reset authored
flags while numbered-zone takeovers remain its primary score and win metric.
The authority path now consumes the resolver's exact successful first-pass
contact stream; the former gameplay-side sphere-overlap scan is deleted.
`MoveCB` must be set and `Powerup` clear, and those callback targets suppress
ordinary solid push. The sole retail callsite then repeats the source
`MoveOrder & 8`, player-class (`Flags & 0x100`), and not-dead gates before
dispatching flag/bay item IDs. `[orig: Entity_MovementCollisionResolver
@0x4B2F90..0x4B2FF5; sole handler xref @0x4B2FF5;
Entity_ProcessWaypointInteraction @0x4AD820;
GameType_CreateDefaultSettings @0x52DD00]`

`D-AI-6` update (2026-07-20): the aim-error global `@ 0xC6EAE8` is the WAC
named variable **accuracyspread** — its config source is mission scripts (the
table §20.3); the earlier `autogain` attribution came from the phase-shifted
table read. This leg is now ported through `World::wac_values` and consumed
directly by `AiSystem::infantry_combat_think`. D-AI-6 remains open for only the
bone-derived LOS/aim endpoints and prone-in-foliage concealment term.

### 20.9 Open follow-ups

1. `mana` (entity+0x120, cached `@ 0x4f582e`) — the field's true semantics
   (IDB gloss "Armor") and its writers are unwalked.
2. The end-music selector reader (the `@ 0x3245B08` byte through the stream
   context `@ 0x3245AE8`) and `sub_5280B0`'s parked global (`@ 0x24D20CC`,
   IDB gloss "g_SoundVolumeOption" — suspect) are unwalked.
3. The win epilog's extraction-point source (`@ 0x2696FD8/0x2696FDC`) — who
   stamps it (the `.cne`? the mission?) is unwalked; our stand-in skips the
   flyaway entirely.
4. WAC named vars as WRITE targets (`set(health, ...)` — the original resolver
   returns pointers, so any table name is an lvalue): our builtins are
   read-only; extend `WacVm::write` when a mission needs it.
5. The 0x1D non-team MP string form + `dword_24C1AD4/BB8/C9C` values, the
   draw/tie semantics (`byte_24CFDB0` beyond the equal-scores case), and the
   0x2B ack are net-track items.
6. `Score_ProcessKillEvent`'s killer `itemDef->score` gate (@ 0x4fd422) is
   unmodeled — verify whether any SP loadout authors score 0.

### 20.10 IDB write-backs (2026-07-16 session 6, saved)

Renames: `WacAction_Win @ 0x4ed4a0`, `WacAction_Lose @ 0x4ed3f0`,
`WacScript_AdvanceTick @ 0x4f81a0`, `GameMsg_AddChatLineAndRelay @ 0x5ba170`,
`GameMsg_SetBannerText @ 0x5ba200`, `GameMsg_SetTeamBannerText @ 0x5ba1d0`,
`Score_TallyKillByLocalPlayer @ 0x4fd160` (ex `Score_ProcessDamageEvent`),
`Score_TallyKillByOthers @ 0x4fd300` (ex `Score_ClassifyKillByType`),
`DialogAudio_PlayNextChunkOrStop @ 0x44e320`, `MusicCtx_SelectEndTrack
@ 0x672fd0`, `EndRoundScoreboard_SerializeHeader @ 0x505280` (ex
`WeaponOverlay_SerializeToBuffer`), `EventTrigger_ResolveZoneTriggerRefs
@ 0x453000` (ex `EventTrigger_ResolveWeaponActionRefs`),
`EventTrigger_ResolveZoneActionRefs @ 0x453100` (ex
`resolve_weapon_slot_triggers`); globals `g_endround_winner_team @ 0x24c1970`
(ex `g_scoreGameType`), `g_mission_exit_reason @ 0x24c1918` (ex `reason`),
`g_endround_linger_timer @ 0xc8d820` (ex `g_mission_metrics_timer`),
`g_round_time_remaining @ 0x24c1958`, `g_endround_draw_flag @ 0x24cfdb0`,
`g_banner_text @ 0x28e3da0`, `g_team_banner_text @ 0x28e41a0`,
`g_cine_mode @ 0x2696dc4`, `g_epilog_win_state @ 0x2696db4`,
`g_epilog_lose_state @ 0x2696d9c`, `g_epilog_screen_active @ 0xa87054`, and the
`wac_var_*` cluster (ticks/GameOver/WinVar/LoseVar/humans/result/health/mana/
autogain/accuracyspread/CurTOD/auto_item) + `g_stat_bluekills_by_player
@ 0xc846f0` / `g_stat_greenkills_by_player @ 0xc846f8`. `define_func` on the
WAC win/lose handlers and the `@ 0x4d31c0` tail fragment. Entry comments on the
named-value table, the round-end/cine/scoreboard functions, and the two zone-ref
resolvers.

## 21. Appendix: the fire-origin / userpoint chain — where bullets come FROM (engine-research, 2026-07-16 session 7)

The user-visible P1 residual (D-AI-6): our AI fire origin was `entity pos + 0.9 u`,
but infantry model origins sit at the PELVIS (US01 mesh spans Y −1.02..+0.97), so the
lift landed at head height. The original never lifts — it fires from a MODEL
USERPOINT through the animated skeleton.

### 21.1 The two origin functions

- **`Entity_ComputeWeaponFireOrigin @ 0x43b4b0`** — the aim/LOS/HUD/missile origin
  (callers: `Entity_FindTargets`/`Entity_CheckMutualLineOfSight`/`Entity_ValidateWeaponTarget`,
  the guided-missile family, `HUD_DrawCrosshair`, the targeting-line debug). Person
  leg (def+92 == 3): `pos + (entity+0x6C..0x74 >> 1)` (or `>> 2` on X/Y when bit 7 of
  the low byte of `&current_tick + 36*entity[+0x7C]` — a per-entity ADDRESS-HASH, not
  randomness) plus a ±0.06 u jitter from bits 5-6 of the same byte; the +0x6C vector's
  writer is unwalked (open). Non-person leg: transform **the def muzzle userpoint**
  (`def+1350`, a 1-based byte index) from the model userpoint table by the entity's
  euler rotation matrix (entity+0xB4, three builder variants); fallback the entity
  `+0x1FC/+0x200/+0x204` point (the model collision-bbox CENTER,
  `Entity_InitFromModel` min+((max−min)>>1) `@ 0x40df1e..0x40df4a` — see the bms
  record §3b item 5) or the raw position. `def+1350` resolves at model init from the hardcoded name
  **"TARGET"** [orig: Entity_InitFromModel @ 0x40dd04] — US01 has no TARGET point, so
  persons ride the person leg.
- **`Entity_GetAttachmentWorldPosition @ 0x4b2670`** — the ROUND/EFFECT spawn origin:
  the userpoint's local position transformed by its bone's **LIVE posed matrix**
  (`Entity_BuildBoneTransformMatrices @ 0x4b1290`; matrix index = userpoint record
  +24), orientation out = entity euler. Callers: FIVE sites, all in
  `Entity_UpdateInfantryAI` — the §17.4 anim-event fire block (bit 0x4 → bone byte
  entity+0x365 firing weapon +0x358; bit 0x10 → +0x367/+0x35B; the secondary latch →
  +0x366/+0x359) and the combat-pass aim anchor (+0x366). A mounted occupant whose
  vehicle def carries attrib 0x20 delegates to `Entity_ComputeUserpointWorldTransform
  @ 0x545c60` (the VEHICLE's userpoint — D-AI-2 adjacency, unwalked).

### 21.2 The model userpoint table + the authored-name resolution

Model userpoint table @ model+0xC0 (count @ +0xBC): 48-byte records, local position
at +0, matrix/bone index at +24, NAME at +32; matched case-insensitively
[orig: modelgpm_FindUserpointByName @ 0x5b2170]; `Entity_FindUserpointIndexByName
@ 0x545540` returns the 1-based row. At spawn, the item def's TWELVE authored
userpoint names (16-byte strings at def+0x61B..0x6CB) + the weapon def's own name
(weaponDef+856) resolve into entity byte clusters, with a groups-of-3 fallback fill
([3..5]←[0..2], [6..8]←[0..2], [9..11]←[3..5], [12]←[6]) [orig: Entity_ResolveBoneUserpoints (ex sub_545940)]:

- infantry: `Entity_InitInfantryBoneData @ 0x490160` → entity+0x4D8..0x4E4 (def
  order shuffled 3,4,5,0,1,2,9,10,11,6,7,8 + the weapon-def name);
- vehicles/seats: `Entity_InitBoneReferences @ 0x441470` → +0x318 "CAMERA",
  +0x31A "USEGUN", +0x327..0x333 (same def names + weapon-def name).

The §17.4 FIRE bone bytes entity+0x365..0x367 are a third cluster whose block
writer remains unwitnessed (the D-AI-5 family, with the weapon-id bytes +0x358..0x35B).

Ground truth (retail JOX models): US01 = `GFlash01`@part15 + `Look`; EIndo01-08 =
`MFlash01`/`bullet` (+`GFlash01`/`bcasing` on some); CIndo civilians = `LOOK` only
(no muzzle — civilians never fire).

### 21.3 The port: the fire origin is computed at fire time from the sim's own pose

Retail never pushes a muzzle anywhere per frame: every consumer computes the userpoint
synchronously at its own call site, on the logic tick that needs it, building the model pose
on demand. The port does the same (2026-08-26; the present-pass push and its stamps are gone):

- `World::muzzle_pose_provider` (`IMuzzlePoseProvider`, `engine/runtime/world/muzzle_pose.h`)
  is the native collision rig, `SimCollisionPoseProvider`
  (`engine/runtime/simassets/sim_collision_pose.cpp`).
- Persons — `resolve_muzzle_pose`: the composed clip/blend/aim-overlay pose of the current
  tick, the launch bone's FK prefix against `rest_global_inverse`, the def's
  `launchups_closeattack` userpoint through `collision_matrix_from_euler(body overlay,
  position)` — the in-sim `Entity_GetAttachmentWorldPosition @ 0x4b2670`. Without a rig,
  model, table, or resolvable point the consumers copy the RAW entity origin
  (`@ 0x4b2767..0x4b278e`) — no lift of any kind.
- Vehicles/emplacements — `resolve_userpoint_transform(entity, 1-based userpoint index,
  out[6])`: the entity placement matrix (`collision_matrix_from_euler`; the Q16 scale on the
  rotation diagonal when set, `Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale
  @ 0x614210`), the userpoint's part matrix from the sim-clock PANM evaluation over the retail
  CTRL bus (`panm_part_matrices`, shared with the generic collision leg), the record position
  through it, and the posed bone's euler via `collision_matrix_to_euler` — the in-sim
  `Entity_ComputeUserpointWorldTransform @ 0x545c60` -> `Userpoint_ComputeWorldTransform
  @ 0x56c420`. A model without live PANM poses every part in the entity frame.
- The DEF names the points: `launchups_closeattack` (persons) and the twelve
  `weap[lr][bmc]up[2]` keys (vehicles/emplacements, §21.2) are parsed into `DefItemDef` and
  the latter resolved on the entity model into `Entity::weapon_userpoint_bytes[4][3]` at the
  collision sweep (`collision_resolve.cpp`); `weapon_userpoint_byte(entity, slot, field)` is
  `Entity_GetWeaponSlotByte @ 0x5459c0`.
- Consumers share `AiSystem::weapon_fire_origin(World&, ...)`: the fire pass spawns rounds
  from it, the LOS endpoints ray between two of them, the aim solution reads it for the EYE
  and the target point. `pump_mounted_weapon_slots` fires a UseGun shot from the
  EMPLACEMENT's posed FIRE point (`weapon_userpoint_byte(mount, 0, 0)` — the fire tick
  reaches `@ 0x545c60` with currentAction FIRE / nextAction 0, so the `@ 0x545d17..0x545d3f`
  select yields field b, and `Server_ClientFiredRound @ 0x50c1f4` names field 0 outright; the
  m/c fields anchor the effect legs) ALONG THAT BONE'S EULER — retail's
  `Entity_CalcWeaponFirePosition @ 0x4dc750` parentSlot-3 leg; byte 0 / no model copies the
  parent's raw position/euler. The presented `ObjectModel` muzzle userpoint survives only as
  the muzzle-flash anchor.

Evidence: `ai` ctest (`test_fire_pass_uses_embedder_fed_muzzle`, `test_weapon_fire_origin_fallback_chain`,
the LOS band case, the aim-pitch case, the mounted UseGun case — all through a fake
`IMuzzlePoseProvider`); the `ai_muzzle_pose` ctest (`tests/world/ai_muzzle_pose_test.cpp`,
gated on `OPENNOVA_JO_DIR`) reads the same provider
through `get_entity_debug` (`muzzle_valid` / `muzzle`) and A/Bs it against the presented
skeleton's `ObjectModel.get_muzzle_world_position()`: CP01 2026-08-26 PASS, 92/102 infantry in
the rifle envelope, the sim point within 1e-5 u of the node's for the in-range bodies. That A/B
caught the skeletal leg feeding the userpoint in the render-swizzled frame (0.5 u low, ~90°
off) — the fixed matrix chain works in the native model frame and retail transforms the raw
record position (`@ 0x4b272e..0x4b2743`); both legs now use the raw 16.16 record.

### 21.4 Divergences + open follow-ups

D-AI-6 (ledger): the fire origin is exact in-sim at fire time for persons with a rig and for
UseGun shots. The aim/LOS origin (`Entity_ComputeWeaponFireOrigin @ 0x43b4b0`) is
`AiSystem::weapon_aim_origin` at both mutual-LOS ends and the aim TARGET point: its
non-person leg is ported 2026-08-26 (the def TARGET userpoint def+1350 — resolved from the
hardcoded name at `Entity_InitFromModel @ 0x40dd04` into `Entity::target_userpoint_byte` —
through the placement matrix `@ 0x43b5d4..0x43b5f6`, else the collision-bbox center
entity+0x1FC `@ 0x43b619`, else the raw position `@ 0x43b54f`). Residuals: (a) the PERSON
leg (`pos + entity+0x6C`; the +0x6C writer stays unwalked) substitutes the posed muzzle; (b) the weapon-def userpoint (`+0x333 <- weaponDef+856`) and the
person `+0x4D8` cluster are not carried; (c) the AI fire pitch adds `inf.recoil_pitch`, which
the AI fire path was not seen to add (`@ 0x4b274b` copies entity Pitch); (d) the
`ItemDef+0x144` pre-evaluation hook (the BoneCallback pair from `BoneCallback_LookupByTag
@ 0x4e32b0`) is unwalked — our equivalent is the inline CTRL publish; (e) retail's vehicle
part pose reads the render clock for flag-2 spinners, ours the logic clock (the documented
PANM seam); (f) the SM bone-list filler (`Entity_InitVehicleAI @ 0x4603ba..0x460414`) rides
D-AI-2.

### 21.5 Witness map: the userpoint chain (grill 2026-08-26)

Record layout (48 B, `gpm+0xC0` table, count `gpm+0xBC`, 1-based index `i` ->
`table + 48*(i-1)` [`modelgpm_FindUserpointByName @ 0x5b21ef`, stricmp on `+32`]): `+0/+4/+8`
local position 16.16, `+12/+16/+20` local direction (the point's local Z axis), `+24`
subobject/bone row (-1 none), `+28` type, `+32` `char[16]` name — our `ThreediUserPoint`.

| Function | Address | Role |
|---|---|---|
| `Entity_GetAttachmentWorldPosition` | `0x4b2670` | `(out[6], entity, u8 index)`: parentSlot 3 on an attrib-0x20 parent -> `Entity_ComputeUserpointWorldTransform(out, NULL, mtx, parent, NULL, NULL)` `@ 0x4b26b6`; index 0 / no model / no table -> raw `{pos, Yaw, Pitch, Roll}` `@ 0x4b2767..0x4b278e`; else the whole skeleton (`Entity_BuildBoneTransformMatrices @ 0x4b1290`), `Math_FloatMatrixToFixedPoint22` of the record's bone row, `Math_FixedPointTransformPoint22` of the record position `@ 0x4b272e..0x4b2743`, `out[3..5]` = entity Yaw/Pitch/Roll `@ 0x4b274b` |
| `Entity_ComputeUserpointWorldTransform` | `0x545c60` | `(out[6], outDir, outMatrix, entity, slot, record)`: `ItemDef+0x144` hook `@ 0x545cae`; slot default `entity+0x2B4`; model/byte select `@ 0x545d06..0x545d55` (`WeaponDef+0x170` tpModel -> `+0x2D4`, else the graphic model + `Entity_GetWeaponSlotByte(entity, seatMask&3, field)` with field `(cur==2&&next==3)?1:(cur==3)?2:0` `@ 0x545d17..0x545d3f`); `slot+0x5D` caches the byte; byte 0 / no model -> raw copy `@ 0x545e1f`; placement `@ 0x545d91..0x545dec` (`+0x158` or def `+0x1B8` scale -> `@ 0x614210`, else `@ 0x613f40`; skipped under `Flags & 0x20000`) -> `Userpoint_ComputeWorldTransform` `@ 0x545e08` |
| `Userpoint_ComputeWorldTransform` | `0x56c420` | bone clamp `@ 0x56c471..0x56c489`; `Math_FixedPointToFloatMatrix4x4_Swizzled` of the entity matrix replicated `info+0x34` times; `Model_TransformBoneMatrices(info, slots, 0)` `@ 0x56c4dd` (PANM current pose); `Math_FloatMatrixToFixedPoint22(parts[bone])` `@ 0x56c4ed`; point `@ 0x56c4f2..0x56c513`; optional direction `@ 0x56c524..0x56c5ef` (rotate `+12`, yaw/pitch from the local direction, `Math_BuildFixedPointRotationMatrixYXZ`); euler `Math_FixedPointMatrixToEulerAngles` `@ 0x56c604`; `out[0..2] = pos` `@ 0x56c618` |
| `Entity_GetWeaponSlotByte` | `0x5459c0` | `(entity, slot, field)`: 0 unless `def+0x54 & 0x20`; `& 0x40` -> `entity+0x4D8+3*slot+field` `@ 0x5459e7`; else `entity+0x327+3*slot+field` `@ 0x5459ef..0x5459f9` |
| `Entity_InitBoneReferences` | `0x441470` | resolves the twelve def names (§21.2) on the entity model into `entity+0x327`: slot 0 <- def[3..5] (`weapr?up`), 1 <- def[0..2] (`weapl?up`), 2 <- def[9..11] (`weapr?up2`), 3 <- def[6..8] (`weapl?up2`) `@ 0x4414e0..0x4415aa`; `+0x333 <- weaponDef+856` `@ 0x4415cd`; CAMERA/USEGUN bytes `+0x318/+0x31A` `@ 0x4414b4/0x4414c7` |
| `Entity_ResolveBoneUserpoints` | `0x545940` | the zero-fill: `[3..5] <- [0..2]`, `[6..8] <- [0..2]`, `[9..11] <- [3..5]`, `[12] <- [6]` |
| `Entity_CalcWeaponFirePosition` | `0x4dc750` | `(out[6], entity, slot)`: `slot+0x5E & 1` -> the latched `+0x40..0x54` transform `@ 0x4dc762..0x4dc786`; parentSlot 3 -> `Entity_ComputeUserpointWorldTransform(out, NULL, mtx, parent, slot, NULL)` `@ 0x4dc7e6` (the carrier-routed eweap case `@ 0x4dc7bb..0x4dc7d9`); parentSlot 2 on attrib-0x20 -> `Entity_ComputeUserpointTransform @ 0x545a40`; else `pos + CameraOffset(+0x6C)`, `out[4] += pitchBlend(+0x380)` `@ 0x4dc847..0x4dc880` |
| `WeaponAction_Fire` | `0x542b10` | `Entity_CalcWeaponFirePosition` `@ 0x542bf7` -> `Entity_FireWeaponAndSendPacket` `@ 0x542c5e`; the fire command copies `out[0..2]` and `out[3]/out[4]` `@ 0x42be84..0x42bef2` — a mounted round leaves along the emplacement's posed bone euler |
| `Math_FixedPointMatrixToEulerAngles` | `0x613310` | yaw = atan2(M4, M0) `@ 0x61332c..0x61335d`; `A = (-M6*cy + M2*sy)>>22`, `B = (M2*cy + M6*sy)>>22`, `C = (M0*cy + M4*sy)>>22` `@ 0x61339a..0x613400`; pitch = atan2(M8, C) `@ 0x6133fa..0x613421`; `D = (M10*cp - B*sp)>>22`, roll = atan2(A, D) `@ 0x61344a..0x61347b`; 64-bit shifts without bias, `ftol` truncation — `collision_matrix_to_euler` |
| `Math_FloatMatrixToFixedPoint22` / `Math_FixedPointToFloatMatrix4x4_Swizzled` | `0x611140` / `0x611080` | the exact inverse pair (`d0=+s10*C, d1=-s2*C, d2=+s6*C, d3=+s14*65536; d4=-s8*C, d5=+s0*C, d6=-s4*C, d7=-s12*65536; d8=+s9*C, d9=-s1*C, d10=+s5*C, d11=+s13*65536`) — `collision_matrix_apply_render_pose` |

The per-frame audit: `@ 0x4dc750`'s callers are the camera builders, the local body, the
projectile preview, the crosshair, a sector-action message, and the fire tick;
`@ 0x545c60`'s other callers are effect-gated (`WeaponAction_ProcessFrame @ 0x540e60` reads it
only with a live action-effect tracker or an open heat window), the NVG laser, the joiner's
round-event effect, and debug lines. Nothing per frame writes a muzzle the simulation reads.

## 22. Appendix: the org2 player-body physics grill (grill-ida, 2026-07-16 session 8)

The dedicated player-physics grill D-INF-10/D-INF-12 deferred to. Scope: the
player (org2) heading/leg model, gravity, jump, the airborne/landing edges, and
the org1 leg block re-read at the same precision. Method: `py_eval` displacement
scans over `Entity_UpdateInfantryPlayerBody @ 0x4b40e0` (0x42d3 B) and
`Entity_UpdateInfantryAI @ 0x4b9910` for every instruction touching
+0x8C/+0x2D4/+0x2D8/+0x2E4/+0x2E8, then byte-reads of each hit cluster — the
"displacement scanning unavailable" tooling limit that cut the earlier attempt
is gone. Port: `engine/runtime/world/infantry.cpp` `tick_infantry` (heading/legs step
5, gravity/edges/jump step 9), `player_body_select` (airborne gate),
`InfantryState.jump_cooldown`; pinned by the rewritten
`test_player_body_chase_and_legs`, `test_player_body_chase_crosses_the_bam_seam`,
the gravity-cadence case, and the player-jump case in
`tests/world/infantry_test.cpp`.

### 22.1 The org2 heading model (closes D-INF-12; §3.3 carries the org1 half)

| Finding | Witness |
|---|---|
| Parachute (Flags 0x20): bodyHeading(+0x8C) sixteenth-steps toward the render yaw(+0x10) `(yaw−body+8)>>4`, both leg re-plant targets snap to the result | `[orig: @ 0x4b494d-0x4b496c]` |
| Any of Flags 0x100060 (parachute/carried/ladder): the yaw is clamped to ±0x55555500 (120°) of bodyHeading (mount seat-cfg 3 and an active-parent carried byte skip the clamp), the local player's camera-yaw mirror `g_LocalPlayerLookYaw` (ex `dword_B75FCC`, renamed 2026-08-15) moves with it, legs snap to the body. The LADDER leg is PORTED (§30 — the sim writes the view, the embedder's mouse accumulator inherits it through the post-tick write-back); the parachute/carried legs and their exemptions ride those slices | `[orig: @ 0x4b4ac6-0x4b4b6b]` |
| ON FOOT — movement state (flag-table bit 0): both leg targets = the yaw EVERY tick | `[orig: @ 0x4b4984 → @ 0x4b49dd/@ 0x4b49e3]` |
| ON FOOT — idle: per-leg re-plant, drift measured vs the CURRENT LEG YAW (org1 measures vs the target), 5°/30° hysteresis, 64-tick windows staggered 32 apart (L `(tick−32)&0x3F`, R `tick&0x3F` via ebp set at function head) | `[orig: L @ 0x4b4993/@ 0x4b49ad-0x4b49bc; R @ 0x4b499b/@ 0x4b49d0-0x4b49e3; ebp @ 0x4b4680]` |
| Leg chase: quarter-step `(Δ+2)>>2`, rate clamp ±0x3000000 (~4.2°/tick — 3/5 the org1 0x5000000), twist limit ±0x30000000 (67.5°) measured vs the YAW (org1: ±0x20000000 vs the body); no def+84&0x200 1/16 variant in the org2 block | `[orig: R @ 0x4b49e9-0x4b4a43; L @ 0x4b4a49-0x4b4aa9]` |
| **bodyHeading = legYawL + (legYawR − legYawL)/2** — the body follows the FEET; the §14 torso twist is (yaw − midpoint), so small aim moves twist the torso while the feet and body hold | `[orig: @ 0x4b4aa9-0x4b4abb]` |
| UseGun live root follow: slot 3 calls `Entity_AttachToBoneAndUpdateTransform @ 0x5463d0` from the player body at `0x4b63c7` and AI body at `0x4bec23`; the posed parent matrix transforms the authored UseGun point into the child Position. The port matches this root-position result only. | `[orig: @ 0x5463d0; callers @ 0x4b63c7 / @ 0x4bec23]` |
| Generic-seat basis follow: ordinary mounted seats write Position from the seat bone and +0x8C/both leg targets = bone yaw, bodyPitch/Roll = bone pitch/roll (rides D-INF-2). | `[orig: Entity_GetBoneTransformAndOrientation @ 0x4b0c50 → @ 0x4b654e-0x4b6575]` |
| Carrier rotation follow (RE-ROUTED 2026-08-15, ex "ladder yaw alignment"): the block sits in the PARENT-CARRY positioning region — the delta is the CARRIER entity's yaw change this tick, added to +0x8C, both leg yaws, both leg targets (+ the yaw and `g_LocalPlayerLookYaw` unless the parent def's +0x58 & 0x1000). A static ladder contributes zero; the leg rides the mount/carry slice (D-INF-2), not D-COL-5 | `[orig: @ 0x4b5690-0x4b56d9; the carry region upstream @ 0x4b562e-0x4b5683]` |

### 22.2 Gravity / jump / edges (closes D-INF-10's player leg)

| Finding | Witness |
|---|---|
| org2 gravity: `vel_z −= 208` EVERY tick, skipped while Flags 0x108000 (ladder/drowning); terminal clamp −32768; `pos.z += vel_z + root_dz` in ONE store (ours splits the two adds, same net) | `[orig: gate @ 0x4b7ac8; step @ 0x4b7acf; clamp @ 0x4b7c77; pos @ 0x4b7cef]` |
| org1 gravity re-pinned with the same gate shape: skip on 0x108000, `−416`, clamp, `pos.z += 2·vel_z` | `[orig: @ 0x4bf7b8-0x4bf7ee]` |
| The horizontal integrate is 1× (rotated root delta + vel) for BOTH motors in normal play; the org2 local-player 2× branch is gated on `g_localPlayerPoofMode` — see D-INF-21 | `[orig: org1 @ 0x4bf684-0x4bf6a2; org2 1× @ 0x4b7cbf-0x4b7cd9; 2× gate @ 0x4b7c8d]` |
| Jump cooldown lives in the REUSED +0x1A8 slot (org1's targetHeading): clamp [0,32], >1 counts down, parks at 1 while the jump key (MoveOrder bit 5) is held, key release → 0 — no auto-repeat on a held key | `[orig: @ 0x4b7de0-0x4b7e15; release edge @ 0x4b7e78-0x4b7e82]` |
| Jump gates: cooldown 0 + key held + not prone (the cached prone local, also the freelook-pitch-halving and lean-skip selector) + `!(Flags & 0x1A002)` (in-air/dead/the water pair) + not carried (0x40) | `[orig: @ 0x4b7e8c-0x4b7ebd]` |
| Jump impulse: `vel.xy += 3/4 · (this tick's ROTATED root step)` — running momentum — then `vel_z = 0x1600`, Flags |= 0x2000 (no 0x40 clear here — carried was gated out @0x4b7ebb), anim 30 jump_start NOW + 31 jump_loop PENDING (straight stamps, no availability check), cooldown = 32; an on-ladder jump additionally pushes 0.5u along −bodyHeading and unlatches (PORTED §30) | `[orig: @ 0x4b7ec3-0x4b7f0c]` |
| The ledge-fall edge, org2 (resolver return > 0xF000; gate `!(Flags & 0x10A002)` — the 0x2000 bit is the was-grounded test and DEAD skips the whole edge, carry included): carried 0x40 is force-CLEARED (not skipped), Flags |= 0x2000, the 3/4 momentum carry, pending cleared, then anim = 31 (+0x10 while parachuting) stamped STRAIGHT — no availability check. org1 (gate `!(Flags & 0x10A000)`, no dead bit): NO carry; 0x2000 sets and pending clears for live non-carried bodies (dead skips the stamp AND the pending-clear, carried skips the stamp only), and the 47→31 availability ladder (`animMap[id] != animMap[0]`) runs ONLY while parachuting — a plain NPC ledge fall keeps its current clip | `[orig: org2 @ 0x4b7e17-0x4b7e73; org1 @ 0x4bf8ae-0x4bf901, parachute gate @ 0x4bf8d8]` |
| The 4th-tick org2 body-anim SELECTION is skipped while airborne(0x2000)/dead(0x2)/carried(0x40) — the jump/fall edges own the in-air clip; selection resumes on landing | `[orig: @ 0x4b70b8-0x4b70d3]` |
| org1 landing: fall damage gates on WAS-airborne + authority + !0x4000000 + NOT DEAD (`test dl,2` — ours previously lacked the dead skip: a hard-landing corpse's health rounded back toward 0 through the clamp; fixed), damage `(threshold − vel_z) >> 4` clamped to health, a damaging landing STAGES the fall death-anim (+0x2C0 ← selector cause 4 → 174, equal to our generic-death fallback), landing sound = weapon slot 16 (15 when dead), then Flags &= ~0x2000 | `[orig: @ 0x4bf802-0x4bf89f]` |
| org2's OWN landing block (2026-07-17 validation read): authority + !0x4000000 gates, the same −1057·scale threshold and `(threshold − vel_z) >> 4` damage clamped to health, the cause-4 death-anim stage (+0x2C0), plus a local-player red-flash call (`Player_OnDamageReceived`) even off-authority — but NO dead gate (the org1 `test dl,2` has no org2 twin) and no explicit was-airborne test (vel_z zeroes every grounded tick, net-equivalent). The port's shared `health > 0` landing gate is org1-witnessed, org2-EXTENDED | `[orig: @ 0x4b7d0a-0x4b7d91]` |
| Fast-fall sound leg: `vel_z < −12288` plays weapon-slot 0x2C (falling wind/scream), gated on the once-per-64-ticks window (`var_10A8 == 0`) — sound slice | `[orig: @ 0x4b7c4c-0x4b7c74]` |
| Auto-parachute: authority + alive + `vel_z ≤ −14336` + aux `+0x2C & 0x10` → Flags |= 0x20 (D-INF-20) | `[orig: @ 0x4b7aef-0x4b7afa]` |
| The client no-authority selection leg rebases movement states to idle 43 (the flag-table bit-0 "client re-derives" semantic, at address precision) | `[orig: @ 0x4b4655-0x4b4670]` |

### 22.3 The "!Poof!" ghost mode (D-INF-21)

`g_localPlayerPoofMode @ 0xA82298` (renamed this session): set 1/0 by the
0x70-byte net-message handler at `[orig: @ 0x42d450]` (payload byte, debug-chat
`!Poof!` on enable; renamed `NetMsg_HandlePoofToggle` this session, ex the
`NetPacket_HandleWeaponSwitch` misnomer), serialized into the world-sync state
(`NetPacket_WriteWorldSyncState`), queried by dev-command senders
(msg 0x36 family `@ 0x42d340/@ 0x42d3e0`). Disable prints `!Unpoof!` and clears
the local player's Flags bit 0 `[orig: @ 0x42d4a5]`. Its one physics consumer is
the org2 2× local integrate (§22.2). Unported by decision — dev/admin feature.

### 22.4 Port deltas landed this session (engine/runtime/world)

- Mounted pose phase captures seat yaw/pitch/roll before the local look rewrite,
  synchronizes `body_heading`, both `leg_yaw`/`leg_target` chains, `body_pitch`, and roll,
  and keeps the remote seat heading separate from the local player's full-precision look.
  The non-cardinal yaw path retains the witnessed `(90 - yaw) * 11930464` integer convention.
- Player heading/legs rewritten to the §22.1 model (was the org1 approximation);
  NPC legs corrected to the midpoint re-plant value, staggered windows, and the
  walk half-snap path (§3.3).
- Player gravity per-tick −208 + `pos += vel` (was −416 every 2 ticks);
  NPC unchanged.
- Player jump: the 32-tick cooldown + release edge, the prone gate, the 3/4
  momentum carry, the 30→pending-31 stamp, ordered after the vertical resolve.
- The airborne edge is per motor (corrected by the 2026-07-17 validation grill —
  a first cut stamped both motors): the PLAYER stamps 31 straight with the 3/4
  momentum carry (the `has_clip` guard is a reimpl guard the original lacks); org1
  keeps its clip on a plain fall — its 47→31 ladder is parachute-gated
  (`@ 0x4bf8d8`) — and clears pending only. `player_body_select` lost its
  airborne stand-in branch in favor of the witnessed selection gate. Review
  follow-up: the org2 dead gate now suppresses the entire edge, including the
  airborne-bit write (`test 0x10A002 @ 0x4b7e22`); pinned by
  `test_dead_player_ledge_fall_edge_is_suppressed`.
- Fall damage now skips dead bodies (the org1 `test dl,2` leg).
- `InfantryState.jump_cooldown` added (the +0x1A8 reuse gets a dedicated field).
- (2026-07-17 follow-up, same branch) the resolver's idle-skip gravity undo
  restored to the witnessed per-motor split — x1 player / x2 NPC, the
  Flags&0x100 select (§15.3 item 1) — the x2-for-both adaptation depended on
  the 2-tick player cadence this section deleted, and the mismatch was the
  standing player's visible rise-and-snap bounce.

### 22.5 Open follow-ups

1. The org2 parachute physics half `@ 0x4b7b18-0x4b7c8d` (auto-deploy at
   vel −14336 + aux 0x10, chute sounds/flap words, the chute brake
   `vel += 0x29C` under −0x1C00) — read 2026-08-06, still unported; rides
   D-INF-20. The WATER half is §29.1 (witnessed; replica-ported AND, 2026-08-24,
   ported in the LOCAL motors — `AiSystem::infantry_water_block` /
   `player_water_block` in world/infantry_water.cpp — with the swim selection
   36-40; the wash 27/28, scope auto-untoggle and resolver swim-force damping
   remain on D-INF-3).
2. The mounted ±120° look clamp and true per-tick transform for generic non-UseGun seats remain;
   the host-fed seat frame synchronizes body/legs/pitch/roll while preserving the local look.
   UseGun root position now follows the live control-posed userpoint, but its full matrix basis is
   not claimed. The ladder exit legs landed with the D-COL-5 port (§30); the
   0x4b5690 block is the carrier rotation follow and rides D-INF-2 (§22.1).
3. `remote_player_body_anim` (the authority's wire-snapped peer selection) has
   no airborne gate — the wire does not carry the peer's in-air flag to the
   host today; rides the D-NET-159 anim-byte work.
4. Landing sounds (slots 15/16), the fast-fall slot 0x2C, and the jump/land
   effect legs — sound slice.
5. org2's tick counter identity (`var_10BC` vs the org1 `tick + 36·net_id`
   stagger) is unverified — only the window PHASE depends on it; ours uses the
   entity-salted key for both motors.
6. `GamePlayerEntity` +0x98/+0x9C/+0xA0 (the slide-velocity triplet the
   integrate/gravity legs write) — +0xA0 is still named `slideDecay` in the IDB,
   a now-visible vel_z misnomer; rename proposal rides the next IDB pass.
7. Fast-fall ground-probe tunneling (bare-rig observation, 2026-07-17): the
   witnessed settle probe (quantize Z up + 2u down-probe `@ 0x4b3d6e`) can miss
   the terrain once the entity is below the surface (the probe start quantizes
   under the ground and the ray looks only DOWN), so a crossing step larger
   than the ~6144 sub-surface window sails through — reproduced in a terrain-only
   test rig with a 2u free fall. Whether retail's
   `Entity_RaycastGroundHeightAndObject @ 0x414370` terrain leg has an
   unbounded-height fallback (no tunnel) needs its own witness; rides D-COL/D-INF-3.

### 22.6 IDB write-backs (2026-07-16 session 8, saved)

Renames: `g_localPlayerPoofMode @ 0xA82298` (ex `dword_A82298`);
`NetMsg_HandlePoofToggle @ 0x42d450` (ex the `NetPacket_HandleWeaponSwitch` misnomer);
`GamePlayerEntity` members `legChaseYawR/L` +0x2D4/+0x2D8 (ex `torsoYaw`/`torsoPitch`)
and `legReplantYawR/L` +0x2E4/+0x2E8 (ex `headLookYaw`/`headLookPitch`) — the §3.3
misnomer family, applied on maintainer OK. Truth comments at
`@ 0x42d450` (func), `@ 0x4b4945`, `@ 0x4b4984`, `@ 0x4b4ab5`, `@ 0x4be969`, `@ 0x4be9d4`,
`@ 0x4b7de0`, `@ 0x4b7ec3`, `@ 0x4b7c8d`, `@ 0x4bf8d4`, `@ 0x4b70b8`, `@ 0x4b7aef`,
`@ 0x4b7ac8` — each carrying the reimpl pointer.

## 23. Appendix: the vehicle pass — mount input chain, mount triggers, drive-authority legs, AI boarding (engine-research, 2026-07-16 session 9)

Question: why do vehicle-gated training missions (00TRa "Training: Basics /
Armory", 04TR "Training: Base Defense") not progress? Witnessed end to end:
the USE-ITEM mount chain, the BMS Player mount triggers, the three drive
classes inside the vehicle physics, the AI boarding machinery, and the player
deploy group stamp. Ported same session: `engine/runtime/world/vehicle_attach.cpp`
(the toggle + scan), the four trigger predicates (`world.cpp` +
`event_runtime.cpp`), the vehicle motor's parked/AI-driver staging
(`ai.cpp::vehicle_ai_drive` + `vehicle_motor.cpp`), `player_spawn.cpp`
(group 1). Evidence: ctest `vehicle_mount` (9 blocks: toggle/deck/swap/enemy-gate/predicates/
AI-drive/redirect+speed/drive-mirror/group-stamp), in-game
the `vehicle_ride_00tra` ctest (formerly `vehicle_ride_probe.gd`) on retail 00TRa — PASS: toggle-mount into the
truck, event 2 fires, the command-mounted instructor DRIVES the redirected truck
11 u+ along list 2 with the player carried at the seat.

### 23.1 The USE-ITEM mount input chain

One input action (id 0xB1 = 177 "useitem") serves armory, vehicle screen and
mount toggle, in that witnessed order [orig: Input_HandleActionBinding_0
@ 0x4e0420 case 0xB1]:

1. seated (`parentSlot != 0`) -> fall through to the toggle latch;
2. standing in a type-6 armory volume (`Flags & 0x400000`) -> weapon.mnu
   (previously grilled);
3. standing in a vehicle-loadout volume (`Flags & 0x800`) -> vehicle.mnu
   VEHICLE, team-gated on the volume's groundEntity team [orig: @ 0x4e0b8x];
4. otherwise -> the latch: `dword_24C18DC = 1` (+ `dword_24C18E4 = 0` when
   fresh). `Input_ProcessFrame @ 0x49d520` consumes it on key RELEASE
   (`!dword_24C18DC && dword_24C18E0`, suppressed by `dword_24C18E4`) ->
   `Entity_ToggleVehicleMount(g_local_player_entity)` [orig: @ 0x49d6dc].

`Entity_ToggleVehicleMount @ 0x436950`: gated on the equipped weapon action
(`!EquippedSlot || currentAction < 2 || currentAction == 5 ||
slot.nextAction == 11` — idle/emptyidle/dry-click/pending-overheat pass);
unmounted -> `Entity_TryEnterNearestVehicle @ 0x4368c0`; mounted -> a fresh
`Entity_FindNearestSeatOrArmory` hit re-enters (seat SWAP), else
`Entity_SendDetachPacket @ 0x435510` (wire 0x27; the authority applies through
the same server leg). NOTE the dismount mechanism: from INSIDE a multi-seat
vehicle the scan's other-seat candidates are killed by the vehicle's OWN hull
(the LOS leg walks pool-1 collision models) — that is why the use key EXITS in
retail rather than cycling seats. Our pool-1 vehicles carry no collision
instances yet, so the own-hull occlusion is modeled as a candidate skip of the
CURRENT mount vehicle — same observable (USE exits; a different vehicle in
reach still swaps); the ray-accurate form lands with the vehicle-collision
slice (D-AI-11 j). A WAC-settable global `dword_C6EADC` (reset by
`WacScript_FreeAll @ 0x4f637b`) blocks the local player's dismount when set.

`Entity_TryEnterNearestVehicle @ 0x4368c0`: standing ON a vehicle
(`Flags & 0x200`) -> `Entity_FindBestSeatSlot(player, groundEntity)`
[orig: @ 0x4351f0]; else the proximity scan. Either result feeds
`Entity_RequestVehicleAttach @ 0x4364A0` — which pre-snaps the requester yaw
from the seat bone (UseGun seats: `vehicle->Yaw - (HIWORD(SpawnOrigin.Z) << 16)`;
others: the bone euler yaw), then applies directly on the authority
(`Entity_ProcessVehicleAttach`) or queues C2S 0x26.

OpenNova now performs that snap before either in-process attach relationship is written. Because the
port splits retail's body/look state across records, `presnap_vehicle_attach_heading` synchronizes the
registry `Entity.yaw`, `AiEntity.heading`, and the local player's
`InfantryState.target_heading`. The Godot side had one more stale copy:
`player_input_.look_heading`; the next `apply_player_input_pre_tick` overwrote the otherwise-correct
core snap. `sync_local_mounted_input_heading` now copies the snapped target back immediately after a
successful local toggle and after local/host logic ticks. Both operations intentionally leave pitch
unchanged. `vehicle_mount::test_usegun_attach_presnaps_local_look` pins the core fields and first pose;
the asset-backed B50 GUT pins the immediate toggle-side binding latch through the first `sim.step()`.
Joiner attach/detach now uses C2S 0x26/0x27 and applies the requester-local relationship only after the
authoritative S2C 0x0A carrier/bone echo. The generic live seat-bone transform follow remains deferred
(§9.2.5). The separate live UseGun root-position path is matched in §23.5.

`Entity_FindNearestSeatOrArmory @ 0x435d50` (searchMode 0, the seat leg): walks
the player's proximity list; per candidate — dead skip, vehicles with a live
ENEMY occupant rejected [orig: Vehicle_HasEnemyOccupant @ 0x4359F0], emplaced
guns (attrib 0x20, not 0x40) LOS-resolve through their carrier; per FREE seat
bone (`sitex`->1, `ctrlx`->2, `drvrx`->5, `UseGun`->3 by name prefix; occupancy
`mountHandles[slot] == 0xFFFF`): the posed bone world position vs the player
eye (`Position + CameraOffset`, +0.1875 u bias) must sit within **4.0 u
horizontal** (0x40000) and the 3D cap (0x3FFFFFC0 unmounted / 0x38E38E0 = 910.2 u
while seat-swapping); LOS last
[orig: Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130]; score =
`horiz + (dist3d >> 9)`, lowest wins. The armory leg (attrib 0x80000, bone
prefix "armory", seatType 4) shares the math. Angle terms are computed and
capped but feed nothing measurable (vestigial).

`Entity_FindBestSeatSlot @ 0x4351f0` (deck/board pick): walks the vehicle +
carried children (child qualifies when it IS the vehicle or its groundEntity
is); per free-or-own seat, weight by type — **ctrl/drvr 0x2000 < UseGun/default
0x20000 < sitex-on-vehicle 0x200000 < sitex-on-child 0x2000000; LOWEST wins**
(the driver seat wins a deck board). AI boarding modes constrain it: aiComp
mode 123 takes only `sitex`, mode 124 refuses `ctrlx` (see 23.4).

Seat-position keys: actions 0xB6-0xBF -> `Entity_FindAvailableSeat(player,
0..9)` [orig: @ 0x436790] — seat N of the CURRENT mount's slot list (weapon
idle-gated; a seat occupied by an AI can be displaced by a player —
`occupant.Flags & 0x100` check). Unported (follow-up; the toggle covers the
missions).

Port notes (`vehicle_attach.cpp`): `player_toggle_vehicle_mount` +
`find_nearest_free_seat` + `attach_to_seat_index` over our seat model; the
witnessed constants verbatim; deviations ledgered as D-AI-11. The sim binding
is `Simulation::local_player_toggle_mount` (the weapon gate reads the
ported weapon FSM slot), the shell key is main_game.gd's USE-ITEM handler
(armory leg first, faithful order).

### 23.2 The BMS Player mount triggers (cat 7 subs 38-41)

`EventTrigger_EvaluateCondition @ 0x453620` cat-7 resolves param1 by SSN
(`EntityPool_FindByNetId`) and calls four predicates — all requiring a live
local player (`Flags & 2` clear), all walking ONE carrier link:

| sub | editor name | original (renamed this session) | true test |
|---|---|---|---|
| 38 | PLYRATTACHED | `Entity_IsLocalPlayerSeatedOnSsn @ 0x4f10d0` | seated: `parentEntity == E` or `parentEntity->groundEntity == E`, any seat |
| 39 | PLYRONSSN | `Entity_IsLocalPlayerStandingOnSsn @ 0x4f1260` | STANDING: `groundEntity == E` or `groundEntity->groundEntity == E` |
| 40 | PLYRDRIVING | `Entity_IsLocalPlayerDrivingSsn @ 0x4f1150` | sub-38 chain AND `parentSlot in {2, 5}` |
| 41 | PLYRONGUN | `Entity_IsLocalPlayerOnGunOfSsn @ 0x4f11e0` | sub-38 chain AND `parentSlot == 3` |

The shipped IDB names were PERMUTED misnomers (the "Driver" name sat on the
on-gun test); renamed + commented in the IDB this session. Ported as four
`EntityCommands::local_player_*` predicates consumed by
`BmsEventSystem::evaluate_trigger`. 00TRa's ride gate is exactly these: event 2
(PLYRATTACHED 11) kicks the ride off; events 46/47 mix GroupIsWithinArea with
PLYRONSSN.

### 23.3 The three drive classes in the vehicle physics

`Entity_UpdateVehiclePhysics @ 0x48af00` (all `move_function cveh` vehicles via
`Entity_DispatchPhysics_cveh @ 0x48efc0`, `physics != 0`): after the per-tick
occupant sweep (stale ctrl/drvr slots cleared; `occupantEntity` = the validated
ctrl/drvr occupant only — sitex NEVER claims it [orig: @ 0x48b8a1-0x48b944]),
the `attrib & 0x40` (PlayerControl) input staging splits three ways
[orig: @ 0x48b949]:

1. **No controller (or dead/locked vehicle)** [orig: @ 0x48b978 ->
   0x48c002-0x48c02d]: steer holds `Yaw`, command speed/ramp zero,
   `AI_CheckVehicleStuckState`, `Flags &= ~0x80`, **SM state forced 22** — a
   driverless PlayerControl vehicle CANNOT drive, whatever its waypoints say.
2. **A PLAYER controller above water** [orig: @ 0x48b993]: the occupant leg
   (ported 2026-07-04, net-re 5.13; SM state forced 22 too — the SM's mover
   never advances for player-driven vehicles).
3. **An AI controller** [orig: @ 0x48bc12-0x48c034]: state 22 hands back to 16
   [@ 0x48bc16]; `cmd = min(brain[128] outSpeed, playerSpeed)`
   [@ 0x48bc23-48]; the minAI crew clamp (crew count < def minai -> health
   capped at criticalHp) [@ 0x48bc4e-94]; when the per-leg turn budget
   `brain[32]` is spent and a waypoint is live, re-resolve
   (`AIWaypoint_UpdateTarget(brain+13)`) and recompute
   `budget = 32*|Yaw - bearing| / ((brain[35] >> 15) + 32)` [@ 0x48bc9a-cf];
   `delta = clamp(bearing - Yaw, +-budget)`; sharp legs on slow steerers damp
   speed x0.75 per ~30/60 deg residual (`turnRate2 << 6 < budget`, thresholds
   357913920/715827840) [@ 0x48bcfb-0x48bd51]; **steer = Yaw + delta +
   delta/8** [@ 0x48bd7f]; then the pool-1 collision-avoid damping (heading-
   aware ellipse, pseudo-random 0.25-0.75 factor off DcbId+tick)
   [@ 0x48bd8f-0x48bf26] and the wait-for-boarders stop (any pool-0 person
   with aiComp mode 125 targeting this vehicle's DcbId and not yet mounted ->
   hold) [@ 0x48bf6f-0x48bff9]. A handbrake latch (entity byte 973 = +0x3CD,
   def handBrake) and an aim-lock stop byte follow [@ 0x48c03a/0x48c086].

Port: legs 1 and 3 land as `AiSystem::vehicle_ai_drive` (the brain state
stamps + the steer/speed math; the motor consumes a `VehicleDriveCmd`) +
`tick_vehicle_motor`'s AI branch; leg 2's SM freeze lands in the staging block
(a player controller stamps state 22 like the parked leg — the mover never
advances under a human driver; our cur/pend SPLIT means both fields take every
stamp, the original has one state word).

The avoid BRAKE (witnessed in full + ported 2026-07-18 — the 00TRa
trucks-off-path fix): per pool-1 neighbor, reach = both bounds + 1.0 u with a
DOUBLED z term (`|2dz|`), carrier chains skipped both ways
[@ 0x48be1d-0x48be25]; bearing other->self = `fpatan(dy, dx) * 2^32/2pi` (dbl
@ 0x7C19D8); the trigger ellipse compares dist against the DIRECTIONAL
footprints `r/2 + (r/2)*|cos(yaw - ang)|` of both entities + 1.0 u (the
1024-entry cos table `off_849934`); a neighbor within ~30 deg of dead ahead
(`|Yaw - ang - 0x7FFFFF80| <= 357913920`) multiplies the command speed by
`((DcbId + (frame << 8)) & 0x7FFF) + 0x4000` >> 16 — a 0.25..0.75 stochastic
brake per tick, compounding per neighbor (the frame counter is
`dword_24C1948`; our net id + logic tick stand in). Without it, redirected
convoys drove full-speed into parked neighbors and the (ported) hull contact
DEFLECTED them off their routes — braking behind obstacles, not deflection, is
the retail path-follow behavior. ctest `vehicle_mount`
(`test_ai_drive_avoid_brake` — exact factor ahead, no damp behind).
The wait-for-boarders stop stays deferred WITH its witness: it only fires for
a live unmounted pool-0 entity whose brain runs the boarding think (mode
`f[37] == 125` keyed to this vehicle's DcbId in `f[38]`) — moot until the
boarding think (D-AI-11) lands. The handbrake byte-973 latch and the aim-lock
stop stay deferred (D-NET-161).

**Hull-vs-world collision (witnessed 2026-07-17, ported the same session).**
The physics tick runs `Entity_CheckCollisionState @ 0x462a30` (twice — the
second pass at averaged suspension heights) [orig: calls @ 0x47cb8c / 0x47d213
inside `Entity_ProcessTrackedVehiclePhysics @ 0x47c1c0`]: per wheel point it
4-tap samples terrain and, over the proximity candidates, runs the SAME contact
query the person resolver uses (`Entity_ComputeBoneCollisionForce @ 0x4ae150`
= our `collision_contact_force`). The response classifies by force VERTICALITY
(`|fz|<<22 / |force|` vs caller slope thresholds @ 0x462fc2-0x462fcb): a
wall-like (horizontal-dominant) push applies IN FULL at severity 3
[@ 0x46322d-0x463240]; vertical-dominant contacts take graded ¼/⅛ bands. The
caller decays speed by the def `torque` (+0x91C raw, parse @ 0x49dcca):
severity 1/3 → `speed -= speed >> (torque+2)`, severity 2 →
`>> (torque+1)` [@ 0x47cc13-0x47ccc1; `sar cl` masks the count mod 32];
severity 3 on the authority additionally runs a wreck-damage block
(29300-magnitude gates, unitType 3 leg zeroes health @ 0x47cd5d). Also
witnessed: a big-vs-small size-class crush leg (`itemDef->mass` vs 2× the
other model bound @ +2312 → flag 0x40 stamp, no force @ 0x462e94-0x462ec2).
The vehicle's contact mask is 8: a section with VC/type 7 starts at its specialized
vehicle run, while a section without VC/VK falls back to CB/default solids. It is 24
when the def attrib2 low byte has bit 7 set (adds VK/type-12 volumes)
[@ 0x462a91-0x462a9f].
The proximity walk SKIPS any candidate whose `groundEntity` CHAIN reaches the
resolving entity, three hops deep — `v33 = candidate->groundEntity; skip if
v33 == ent, v33->groundEntity == ent, or v33->groundEntity->groundEntity ==
ent` [@ 0x462e26-0x462e4f] — so a vehicle never collides with its own
mounted/carried children (the addeweap cannon whose 0x0D TARGET seeds
`groundEntity` = the hull). Witnessed exactly 2026-08-05: the first port
INVERTED this relation (it skipped `ent->ground_target == candidate` — MY
carrier) and a live joiner's Stryker ground against its own RCWS cannon's
collision volume every tick, walking the hull to a false equilibrium ~13 u
off its wire pose and re-snapping every subrate record — the reported
"vehicle jumping all around". Corrected to the witnessed candidate-side
chain; ctest `collision`
(`test_vehicle_hull_skips_mounted_child_ground_chain` — 1-hop and 2-hop
children produce no contact, an unrelated other hull's gun still pushes).
Ported: pool-1 candidate slices (+6.0 u [orig: @ 0x4b902f]) +
`CollisionWorld::resolve_vehicle_hull` (one mid-hull point, radius 1.5 u,
wall-class-only, mask 8, the witnessed child-chain skip) + the motor's
push/decay leg; ctest `vehicle_mount`
(`test_vehicle_hull_stops_at_building` — a driving truck grinds to a stop at
a wall square). Deferred (D-NET-161): the mask-24 attrib2 leg (attrib2 is not
fed to the sim), the per-wheel point array/radii, the
v84/v85 slope-threshold derivation (caller locals, unwitnessed), the graded
bands, the crush leg, the severity-3 damage block, the second averaged pass
with its pushable-other mass-ratio force split (`otherMass/(otherMass+mass)`
@ 0x463037-0x46324d / @ 0x47d24a). the SM's kinematic `apply_locomotion`
RETIRES for motor vehicles (`physics != 0`) — the SM stays the decision layer
(waypoints, visited bits, states), the motor is the only integrator, matching
the original split. Deferrals stay under D-NET-161 (updated in
[novaworld-net-re.md](../net/novaworld-net-re.md)).

The live motor now also supplies the two cveh control-register fields for
which OpenNova owns exact sources. `VEHICLE_STEERING` zero-extends the high
word of `steer_state`; `VEHICLE_SPEED` applies the original
`CDQ`/`XOR`/`SUB` absolute value and unsigned `0x10000` cap, so `INT_MIN`
publishes `0x10000` rather than entering signed-`abs` undefined behavior
`[orig: Entity_CacheVehicleHUDStats @ 0x4929B0; stores
@ 0x4929D7 / @ 0x4929F1]`. The projection is restricted to authoritative
pool-1 rows with resolved vehicle traits. Compact rows contain neither source,
so the joiner does not estimate them from position deltas. The other vehicle
CTRL families remain in the complete producer gap recorded as D-3DI-2.

Consequences pinned for the training missions: 00TRa's truck ride is
**instructor-driven** (the command-mounted instructor holds ctrlx from spawn,
so the ride runs leg 3 — the authored RedirectGroupTo + PatrolSpeed feed the
AI-driver leg while the player boards a sitex; probe-verified), the tour
dialogs are `Group 1 IsWithinArea X` triggers riding the PLAYER's group — the deploy leg stamps every (re)spawned
player `commandGroup = 1` [orig: @ 0x519fd0, the same block that writes team
+0x162 and clears the movement gate]. Ported into `spawn_player_entity`.
04TR's waves are infantry-led; its lone-vehicle groups need crews (leg 3) or
stay parked exactly as retail parks them.

2026-08-06 addendum — **00TRg's "move in to defend" waves** (the host-parity
slice; diagnosis: the `defense_00trg` ctest, formerly `00trg_defense_probe.gd`). The mission's
entire defense is event-driven: crews command-mount at spawn (waypoint_id 125,
authored on their vehicles), then delay-gated events RedirectGroupTo the boat
groups 5/6/7 (patrol boat SSN 60, Zodiacs 56/57) and truck group 3, and
GroupAtWaypoint arrivals debark defender groups 14/15. Two host-side defects
kept every vehicle parked:

1. **The nav-channel table was off by one** (a reimpl bug, fixed in
   `engine/runtime/mission/promote.cpp`): the .bms waypoint block is POSITIONAL — 128
   slots, slot index == authored list id, channel 0 = "no route" only because
   no mission authors list 0 [orig: `XML_ParseGroupAction @ 0x4cc450` writes
   the per-channel record; `AIWaypoint_UpdateTarget @ 0x457380` returns −1 on
   navMeshId 0]. The build inserted a synthetic slot-0 sentinel and appended,
   so an order for list N read slot N−1: 00TRg authors lists 1 and 3–16 (no
   list 2), so group 3's redirect read the empty slot 2 and dropped, while the
   boats took the NEIGHBORING lists' routes. 00TRa's probe-verified ride had
   masked this (its shifted slot still held a drivable route and the probe
   asserted displacement only). Pinned by the `mission_promote` ctest's
   gap-shape checks (list 3 above an unauthored list 2).
2. **Boats had no authority mover** — Watercraft rows fell through the ground
   core, sank 1–3 u below the waterline at load (gravity, no buoyancy, no
   wheel solve) and never integrated drive. The cbot authority half is now
   witnessed and ported (`tick_watercraft_motor` +
   `AiSystem::watercraft_ai_drive`, family-dispatched in the AiSystem
   authority pass) — see
   [vehicle-client-movers-re.md](vehicle-client-movers-re.md) §1.12.

Retail-faithful quirk left in place: 00TRg's event 21 redirects group 7
(Zodiac 57) but sets its PatrolSpeed on group 6 (a designer slip in the
shipped mission), so 57 sails at its PROFILE default speed. That default is
now witnessed and ported as a narrow D-AI-11 (h) sub-piece: the .aip parse
stores `patrol_speed` at profile+0xC0 and `combat_speed` at profile+0xC4,
both scaled ×1000 × 1/225000 × 65536 = ×65536/225 — the same constant as the
scripted PATROLSPEED command [orig: `AIProfile_ParseProperty @ 0x45de70`, the
patrol handler @ 0x45E6DF..0x45E717] — and the vehicle spawn init seeds
`brain[49] (kSpeedA) = profile+0xC4`, `brain[50] (kSpeedB) = profile+0xC0`,
with the profile resolved from the slot's ai_textfile (def-level fallback
itemDef+0x8B8, then `"helo1"`) [orig: `Entity_InitVehicleAIFromDef
@ 0x4686C0` — the seeds @ 0x4688C7/@ 0x4688D3]. The port:
`PromoteOptions::ai_profile_speeds` (raw values, keyed by ai_textfile;
scale applied at the brain seed in `init_brain`), resolved natively by
`mission::resolve_ai_profiles` (engine/runtime/mission/runtime_boot.cpp, driven by `boot_mission`) inside `run_mission_boot` (S9, ADR
0028; the former shell `AiProfileSpeeds.build` reader + the
`set_ai_profile_speeds` seam are deleted). d_zode's patrol 75 → 21845 (≈0.333 u/tick), pinned by the
`mission_promote` ctest. The REST of the profile parse stays D-AI-11 (h).

**The profile-NAME fallbacks, witnessed + ported 2026-08-25 (post-merge tidy
#553–#573).** A placed vehicle with no ai_textfile never runs profile-less in
retail: the AI class table `@0x813280` (24-byte rows: 4-char tag, state
machine, init) routes `chel`/`cpln` to `Entity_InitHelicopterAIFromDef
@0x4683C0` and `cveh`/`cbot`/`ctrn` to `Entity_InitVehicleAIFromDef @0x4686C0`
(the `cbot`/`cpln`/`ctrn` rows through the thunks `@0x474560`/`@0x474550`/
`@0x474570`), and each init resolves the name in arms — the helicopter
family: the record's +0x9C ai_textfile when non-empty (`sprintf("%s.aip")`
@0x4684ae), else the literal `"helo1.aip"` @0x4684c9 (the BMS-spawn twin
`Entity_InitHelicopterAI @0x461F00` has the same pair @0x461f35/@0x461f50);
the vehicle family: the ai_textfile @0x4687a3, else the def's `default_aip`
(+0x8B8) @0x4687c1, else `"helo1"` @0x4687d3, then
`Path_ReplaceOrAppendExtension(".aip")` @0x4687f4. The consequence that
exposed it: `Entity_UpdateHeloRotorSpin @0x48FA70` returns unless the brain's
profile is type HELO (`profile+0x10 == 1` @0x48fa98), so a bare-placed
Blackhawk's rotor turns in retail only because helo1.aip is type HELO — and
the port, resolving names from the ai_textfile alone, left that brain type 0
and the rotor still. Ported as `mission::ai_profile_name_for` (shared by the
boot resolver, which now loads the fallback files too, and the promote's brain
seed) with the class answer supplied per items.def type id by the embedder
(`PromoteOptions::ai_profile_defaults`; the Simulation reads `ai_function` +
`default_aip` from its ItemDatabase). Pinned by `runtime_boot::run_aip_fallback`,
`mission_promote::test_nameless_vehicle_takes_the_retail_default_profile`, and
the asset-gated GUT `vehicle_emplacement_alignment_test::
test_real_blackhawk_rotor_register_spins_while_crewed` (the register that
stayed 0 before).

2026-08-25 addendum (post-merge tidy of #567) — **the HELO (type 1) flight
set** of `AIProfile_ParseProperty @ 0x45de70` is parsed by
`engine/formats/aip` with retail's stored forms, re-witnessed at the type-1
arms `@ 0x45f684..0x45f9eb`: `patrol_speed` `@ 0x45f6cf` -> +200,
`combat_speed` `@ 0x45f79f` -> +212 and `min_speed` `@ 0x45f9a1` -> +236 are
`atof * 1000.0 (dbl_7C6BD0) * 4.444444444444444e-06 (dbl_7C6BC8) * 65536.0
(dbl_7C3CC0)` — km/h to 16.16 units per tick, i.e. x65536/225, chopped by
`_ftol2_sse`; `patrol_climb` `@ 0x45f687` -> +208 and `combat_climb`
`@ 0x45f757` -> +220 are `atof * 0.016 (dbl_7C6A80) * 65536.0`;
`patrol_altitude` `@ 0x45f71d` -> +204 and `combat_altitude` `@ 0x45f7ed` ->
+216 are `atol << 16`; `turn_rate` `@ 0x45f89a` -> +224 is `atol * 0xB60B60`
then the signed `/62` (the `0x84210843` magic, `sar 5`, sign fix);
`accel_time` `@ 0x45f8ec` -> +228 is `(v << 5 - v) * 2 = 62 v`;
`use_waypoint_z` `@ 0x45f92b` -> +56 is `atol`; `min_agl` `@ 0x45f95f` -> +232
is `atof * 65536`; `flight_skill`/`drive_skill` `@ 0x45f827/@ 0x45f83c` share
the +0x20 clamp 0..4 with `aim_skill`. The GROUND branch converts its speed
pair the same way `@ 0x45e6df/@ 0x45e72d` (+0xC0/+0xC4); the port keeps that
pair raw because its consumer (the brain seed) applies the identical scale.
No engine consumer reads the HELO rows yet — the host-side helicopter mover is
the open port (D-AI-11 (h)); the def layer is faithful ahead of it
(`runtime_boot` ctest pins every conversion).

### 23.4 The AI boarding chain (witnessed; the board walk + the 123..127 command legs PORTED 2026-08-25, #571)

aiComp (+0x68 component) fields: **+148 (dword 37) = the waypoint-list slot,
OVERLOADED as the boarding mode when 123/124/125**; +152 (dword 38) = the
target vehicle id (DcbId) in boarding modes (waypoint node index otherwise);
+144 (dword 36) = the resolved carrier pointer.

- Setters: WAC `ssn2ssn` [orig: WacCmd_SsnToSsn (ex sub_4F7330) @ 0x4f7330, reached via the
  command table @ 0x82dd4c — defined + witnessed this session: detach if
  mounted, mode 125 + target id + carrier ptr, move order reset] and
  `ssnrelease` [orig: WacCmd_SsnRelease @ 0x4f7420 — detach + mode clear];
  `HeliLift_SpawnPickup @ 0x452668` (mode 125). No shipped-mission BMS action
  sets 123/124 (register-form writers only).
- The think consumes them [orig: Entity_UpdateInfantryAI]: mode present ->
  resolve the target by DcbId across pools 0-3 [@ 0x4baeb5-0x4baf88], walk to
  the nearest `E8`->`E7`->`E6`->`E5` entry bone [@ 0x4baf9b-0x4bb026]; on
  arrival with `attrib & 0x60` -> `Entity_FindBestSeatSlot` +
  `Entity_RequestVehicleAttach` [@ 0x4bbda6-0x4bbe07]; every 64 ticks a seated
  boarder re-runs the pick and UPGRADES seats when a better one freed (ctrl
  over sitex) [@ 0x4ba9d8-0x4baa4e].
- `Entity_SetWaypointByTeam @ 0x43cdb4` (the RedirectGroupTo/SingleTo
  implementation, both pools, matched by commandGroup): **auto-detaches a
  mounted non-player** [orig: Entity_DetachFromVehicleIfServer @ 0x4359d0],
  writes mode 1 + list + node (nearest trigger when -1
  [orig: Entity_FindNearestTriggerByType @ 0x407ea0]), seeds the brain wp
  slots + the turn budget.
- Spawn-time crews: BMS attribute bit 1 ("Guarding") -> `Flags |= 0x40` at
  spawn [orig: Entity_SpawnFromBMSRecord @ 0x40e9f0] (neither training mission
  authors it); the MP deploy-into-vehicle leg latches `Flags |= 0x200` +
  the carrier into +364/+384 when the spawn target is a vehicle
  [orig: Server_PositionPlayerForSpawn @ 0x50d44d], consumed by the body
  update's 0x200-toggle [orig: Entity_UpdateInfantryPlayerBody @ 0x4b426a].
- **PORTED (#571, `world/infantry_board.cpp`; re-witnessed 2026-08-25 tidy)**:
  the 123/124/125 board orders resolve the target by DcbId across pools 0-3
  [@ 0x4bee93..0x4beec6 — the authority gate, the 123/124/125 compare
  `@ 0x4beea5..0x4beeaf`, aiComp+152 vs entity+0x7C `@ 0x4beeba`], walk to the
  filtered best seat's approach point (the 2.0u ring `@ 0x4bb32e`, widened to
  bound-radius + 1.0u while the push latch +0x369 is set `@ 0x4bb325..0x4bb34a`)
  with the final-approach one-shot raised for the shared gait select, and on
  arrival run `Entity_FindBestSeatSlot @ 0x4351f0` -> `Entity_RequestVehicleAttach
  @ 0x4364a0` behind the `!parentEntity && radius < 0x640000 && attrib & 0x60`
  gate [@ 0x4bbda6..0x4bbe07]; 126 parks the gait select at moveMode 3 with
  dist = radius = 0xA0000 [@ 0x4baabd..0x4baacf]; 127 follows the local player at
  0x40000, `max(aiComp[16], 0x40000)` when the combat focus IS the player
  [@ 0x4baad4..0x4bab83]. The per-command seat filter is
  `Entity_FindBestSeatSlot`'s aiComp+148 admit term `(cmd != 124 || type !=
  ctrlx) && (cmd != 123 || type == sitex)` [@ 0x4353e6], and its weights are
  `ctrlx`/`drvrx` 0x2000 < `UseGun`/default 0x20000 < `sitex` 0x200000 <
  child `sitex` 0x2000000 (lowest wins) [@ 0x4353ee..0x43540e]; the think's
  per-entity stagger key is `current_tick + 36 * entity+0x7C` [@ 0x4b9948..
  0x4b9953, strength-reduced `lea [eax+eax*8]` / `lea [tick+edx*4]`].
  Still open, on D-INF-2: the E1..E8 entry-bone claim/stagger
  [@ 0x4bb100..0x4bb17a], the 64-tick seated re-upgrade [@ 0x4ba9d8..0x4baa41:
  `(tickKey & 0x3F) == 0`, mounted, `Entity_FindBestSeatSlot`,
  `Entity_GetBoneSlotType != +0x168`, re-attach], the 11000/12000/12001 escort
  offsets, the can't-enter fallbacks, and `Entity_CanEnterVehicle @ 0x435480`'s
  internal gates.

Ported same session (the second wave, after the 00TRa probe forced them out):

- **Redirects reach brains**: `set_ssn_waypoint`/`group_to_waypoint` now run the
  witnessed per-entity order — mounted non-players auto-detach, the entity route
  fields update, and `AiSystem::apply_route_order` writes mode 1 + list +
  the authored node + the turn-budget seed into the brain. Only authored node
  `-1` resolves to the nearest trigger; BMS action `param3` now reaches this seam
  [orig: Entity_SetWaypointByTeam @ 0x43cdb4 / Entity_FindNearestTriggerByType
  @ 0x407ea0]. Before this the redirect actions only touched entity fields and a
  vehicle's SM never saw its new route.
- **Arrivals reach BMS conditions (2026-07-22)**: the infantry channel mover and
  the shared SM/vehicle route mover apply the retail SetBitB(group) then
  SetBitA(SSN) waypoint-visited pair to `World::relations`; the retained
  `relmat_calls` vector is now a diagnostic trace of applied side effects
  [orig: AI_UpdateWaypointMovement @ 0x457c6d..0x457c88].
- **The speed commands**: BMS ChangeGroup/SingleAI subs 29 COMBATSPEED /
  30 PATROLSPEED land in `ai_apply_command` as the kSpeedA/kSpeedB writes with
  the exact scale — km/h x 1000 x (1/225000) x 65536 = x65536/225 (~291.27; the
  items.def x293 is its integer approximation)
  [orig: Entity_ApplyCommand @ 0x43ab60 cases 0x1D/0x1E -> AIEvent types 10/11 ->
  AI_HandleCommand @ 0x465770 cases 0xA/0xB].
- **Vehicle brains**: promote attaches an AI brain to pool-1 items whose type
  authors a CONTROL seat (ctrlx/drvrx spec — the drivable class; the stand-in
  for the def AIData gate, D-AI-11), initialized into state 16 GROUND_FOLLOWWP
  (the shipped ground .aip `default_state`; the profile parse is unported).
  Pure-gunner parent items stay brainless: retail lets the attached organic gunner own
  perception and drive the parent's embedded weapon slot (§26), so this is no longer a
  no-fire condition. Item brains also
  spawn with the flags100-bit1 ACQUIRE SKIP [orig: the profile+100 & 2 gate
  @ 0x46775c]: the shipped transport .aip profiles author zero target
  priorities, and the D-AI-1 feed scans unconditionally where retail's class
  table rejects — without the skip, 13 per-tick pool scans + LOS raycasts
  spiraled the 00TRa load. Lifts with the .aip parse (D-AI-11 i). With the SM mover
  feeding `kOutSpeed` and the AI-driver leg consuming it, a crewed truck now
  drives its authored route: 00TRa's instructor (command-mounted into ctrlx at
  spawn via waypoint_id 123-125) drives the ride the moment event 2 redirects
  group 3.

### 23.5 The mounted seat carry (the ride)

The body update's mounted leg [orig: Entity_UpdateInfantryPlayerBody, the
parentEntity block]: every tick, flags scrubbed (`&= 0xFF8F57DF`, fire-bone
bytes cleared), then dispatches by seat class. Slot 3 (UseGun) calls
`Entity_AttachToBoneAndUpdateTransform @ 0x5463d0` from the player-body site
`0x4b63c7` and the AI-body site `0x4bec23`, then selects anim state 67 (+ def
config variants 68-75). That helper reads the parent UseGun slot byte at `+0x31A`,
selects the 48-byte record at `gpModel[48] + 48 * (slot - 1)`, primes the model state
through `HUD_CacheWeaponSlotInfo @ 0x440930` and
`Model_TransformBoneMatrices @ 0x58e390`, and uses the record's matrix index at
`+0x18`. `Math_FloatMatrixToFixedPoint22 @ 0x611140` converts the live matrix;
`Math_FixedPointTransformPoint22 @ 0x615810` transforms the record's authored XYZ
into child Position. Retail also derives orientation from that matrix through
`Math_FixedPointMatrixToEulerAngles @ 0x613310`, then restores the occupant's
independent look yaw/pitch at `0x546661` / `0x546664`.

Other seats call `Entity_GetBoneTransformAndOrientation @ 0x4b0c50` on the seat
bone -> **Position = bone world pos, bodyHeading/headLook = bone yaw, Roll,
bodyPitch** (the entity Yaw — the LOOK — stays player-owned), and select seated anim
`atol(bone-name digits) + 76` (a `sitex24` names anim 100; +-0x4444440 roll on
the carrier flips 109/110).

The port now matches the UseGun helper's root-position contract: after semantic EWEAP
controls pose the owning parent part, the authored UseGun point is transformed through
that live part and fed back to the occupant. It does **not** claim retail's full matrix
basis or the ordinary-seat `0x4b0c50` follow. The static
`seat_local` carry remains the generic/fallback path. `pose_if_mounted` captures that
seat frame before any local-look rewrite, synchronizes body/leg state, and a UseGun
organic restores its independent live look before chasing/clamping against the mount
base (§26.5).
The mounted branch is no longer an early bypass of the infantry combat/animation pass:
live mounted organics keep perception, damage reaction, target acquisition, aim, and
animation, then bypass only the ordinary locomotion tail after the mounted collision phase
(§26), matching the retail live-parent/health split.
The local body remains in that seat frame while the camera LOOK remains player-owned:
`AiEntity.heading`/`pitch` take full-precision `target_heading`/`look_pitch`; only registry
yaw is the rounded wire/motor mirror. Live move-order bits still mirror into the wire fields
the motor consumes, and a degree round-trip therefore cannot quantize yaw or freeze pitch
[orig: Input_HandleActionBinding_0 @ 0x4e1330 writes entity +0x10/+0x14 from input, mount or not].

### 23.6 Divergences

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-AI-11 | Mount-chain residuals after §26 closes emplaced fire, mounted collision suppression, death-detach animation, UseGun live root position, and the joiner C2S 0x26/0x27 + requester-local S2C 0x0A confirmation leg (`Entity_AttachToBoneAndUpdateTransform @ 0x5463d0`; player/AI callers `0x4b63c7` / `0x4bec23`): (a) the USE scan remains a registry sweep with a +0.9 u chest eye; (b) its emplaced-carrier LOS/reject leg (`attrib & 0x20` -> `groundEntity`) is unmodeled, while the armory leg is ported; (c) the WAC no-dismount global is unmodeled; (d) seat-position keys 0xB6-0xBF and the displace-AI rule are unported; (e) the AI board walk is ported (#571, 2026-08-25 — §23.4); the 64-tick seated seat-upgrade `@ 0x4ba9d8..0x4baa41` and the E1..E8 entry-bone stagger stay open under D-INF-2; (f) child-vehicle seat traversal is unmodeled; (g) sub-39 groundEntity persistence rides the generic carrier reference; (h) vehicle brains retain the acquire-skip stand-in pending `.aip` parse; (i) own-hull seated-scan occlusion remains a candidate skip until pool-1 hulls are built. Generic-seat follow and the full UseGun matrix basis remain D-INF-2. | §23.1/§23.4/§23.5 and §26; asset-gated 00TRc E50triB GUT | mount/ride/drive/emplaced-fire/death, UseGun root position, and joiner relationship confirmation are live; boarding, generic/full-basis, and scan residuals stay OPEN |

Correspondence adds: see the rows appended to the section-2 map this session
(the toggle chain, the four predicates, `vehicle_ai_drive`, the deploy stamp).

### 23.7 The part-animation registers — rotors and the wheel phase (witnessed + ported 2026-08-21)

The accumulators that drive a model's PANM tracks (`HELO_ROTOR` 46,
`HELO_TAILROTOR` 47, `VEHICLE_WHEELS` 60) — the full record is
vehicle-client-movers-re.md §7.4; the vehicle-pass facts:

- The rotor machine is SPLIT by the `.aip` profile type (`brain+4 →
  profile+16`): GROUND profiles (type 2) run `Entity_UpdatePartSpinAccumulator
  @0x4928b0` (decay 186413/tick), called unconditionally at the tail of all
  six movers (`@0x46f99e` infantry, `@0x4700f5` air, `@0x4869ea` light,
  `@0x4889f5` mounted infantry, `@0x48ae3d` tank, `@0x48d42b` ground vehicle =
  `Entity_UpdateVehiclePhysics @0x48af00`); the watercraft mover
  `Entity_UpdateWatercraftPhysics @0x48d480` calls NEITHER machine — its only
  part register is the wheel phase `@0x48e9f0`, so a boat never draws the
  rotor PRNG word; helicopters (type 1) run the twin `Entity_UpdateHeloRotorSpin @0x48fa70` from `Entity_UpdateAircraftPhysics
  @0x4905a6` (decay 46603/tick, plus the engine-start sound and the downwash
  overlay — presentation residuals). Both seed the rate the same way: a
  PlayerControl item (attrib 0x40) with an occupant → 186413; any other item →
  `PRNG_Next16() % 100` (> 66 → 139809, > 33 → 163110, else 186413), re-rolled
  EVERY unoccupied tick because the unoccupied branch resets the rate — one
  shared-stream draw per idle non-player-control vehicle per tick (D-WPN-35's
  open stream identity).
- Ordinals 46 and 47 are BOTH `HIWORD(+0x464)` `[orig: Entity_CacheVehicleHUDStats
  @0x492aca/@0x492ad7]` — one accumulator. The wheel phase is `+0x2B8 +=
  |+0x46C| + (+0x29C << 13)` per tick `[orig: @0x48c4c5..0x48c4f4]`; the slip
  term rides D-NET-161 (0 in the port). The BMS record's `attrib_flags &
  0x20000` ("engine running at spawn") seeds `Flags |= 0x80; +0x468 = 186413;
  +0x460 = 214748352; +0x29C = 0x10000` `[orig: Entity_SpawnFromBMSRecord
  @0x40ee70..0x40ee94]`.
- **Port**: `engine/runtime/world/vehicle_part_anim.{h,cpp}` (the two arms,
  called at every mover tail and — for the host's helicopters, whose mover is
  the unported HELO movement physics — from the authority pass at the missing
  tail site), `vehicle_ctrl_registers` publishing rotor/tail_rotor/wheels
  beside steering/speed, `PF_VEHICLE_ROTOR/_TAIL_ROTOR/_WHEELS` →
  `ObjectModel::set_ctrl_override` (the present applier's ctrl leg grew from
  18 to 21 fields, append-only). Pinned by ctest `vehicle_part_anim` and GUT
  `vehicle_emplacement_alignment_test` (a driven DBuggy's VEHICLE_WHEELS
  override advances; the Blackhawk rotor witness).

## 24. Appendix: item destruction — the explosion queue, the destructible death chain, husks, death pieces (engine-research, 2026-07-17)

Question: how does the original destroy ITEMS — damage application (bullet +
blast), the destroyed/husk model swap, and the explosion/pieces/sounds at
death. Witnessed on retail `Jointops.exe` (`Jointops.exe.kong.i64`, imagebase
0x400000). Ported subset: `engine/runtime/world/destruction.{h,cpp}` + the round_sim
item leg + the collision husk swap +
`godot/game/world/destruction_present_pass.gd`. The native collision,
damage, callback-routing, death-piece, and item-settle mechanics are pinned by
the `destruction` ctest and the def-parse additions in
`tests/def/def_parse_items_test.cpp` (ctest `def_parse_items`). The presenter
is not retail-identical; every remaining simulation and presentation gap is
bounded explicitly in §24.7.

### 24.1 The explosion queue

`WeaponEffect_QueueExplosion @ 0x4e8330` pushes 52-B entries into the 64-slot
authority queue `g_explosion_queue @ 0xB7C688` (count `@ 0xB7C680`; renamed
this session from the `positionRef` kong label): `{+0 pos xyz, +12 dir BAM,
+24 type = the AMMO kztype word (+44), +28 ammoDef ptr, +32 owner entity, +40
hit word, +44 radius-override float}`. A zero override means "the ammo's own
`kz_maxradius` (+56)". `Projectile_ProcessExplosionQueue @ 0x4ead80` drains it
once per frame and resets the count; the type dispatch `@ 0x4eadc6` routes
2/5/6/7 (Standard/C4/Bullets/Slash) to `Entity_ApplyWeaponDamage @ 0x4e6820`,
3 (Medic) to the heal handler, 1 to the vehicle-ram applicator, 4 (RadiusBlast)
to weapon damage with the direct-hit legs (no falloff `@ 0x4e695a`, no LOS
`@ 0x4eb0e0`).

The three pool sweeps ARE the O/M/D item classes: the ammo `flag` bits
NoOItems 0x80000 / NoMItems 0x100000 / NoDItems 0x200000 skip pool 0 / 1 / 2
respectively (`@ 0x4eaece / @ 0x4eb378 / @ 0x4eb5b8`). Per pool: organics get
a 2x-radius reaction band (flinch `Entity_OnDamageReceived @ 0x4eb05c`,
knockback `Entity_ApplyCollisionForce @ 0x4eb1d2`, the victim-attached hit
emitter into entity+0x1CC `@ 0x4eb292`, the hit sound), an LOS ray gates the
damage (`Entity_CheckLineOfSightTerrainAndEntities @ 0x4eb162`; pool 1 lifts
both endpoints +0x4000 `@ 0x4eb4ca`), the cone gate compares atan2(dy,dx) BAM
against the entry dir plus/minus the ammo `kz_pieslice` (+60) `@ 0x4eaffa`, and
pool-2 statics take an AABB-face distance refinement over the model bounds
(`@ 0x4eb700`) plus the GLASS1..GLASS4 user-point window-shatter spawns
(`@ 0x4eb814-0x4eb85d`). Kill credit resolves the owner up its own
lastAttacker (+0x178) chain while dead and not player-flagged (Flags & 0x100)
(`@ 0x4eae95`), and the sweep stamps an EMPTY victim +0x178 with the resolved
attacker (`@ 0x4eb319/@ 0x4eb593`). Destructible-class victims (deathCallback
== `Entity_HandleDestructibleDeathEvent @ 0x440210`) get the blast center
written into +0x80 — the debris launch origin (`@ 0x4eb553-0x4eb569`).

**Glass user-point port (2026-08-16, D-ITEM-17 CLOSED).** Collision asset
resolution retains the first exact case-insensitive user point selected by
retail's complete model/key table: `eurhr2/2a/2b` `GLASS1` → `GLASS`,
`eurhr1/1b/1c/3/3b/3c` `GLASS2` → `GLASS02`, and `atrm2a` `GLASS1` →
`GLASS1` (`GLASS3/4` have no shipped rows). The pool-2 explosion leg applies
the entity's full Euler pose, range-tests the point against the AMMO'S authored
`kz_maxradius` (not the queued radius override), and breaks each point once.
Every newly broken point consumes two destruction-PRNG draws for each of the
four ordered slots, then submits `Effect_BldGlassExp`, `Effect_BldPaperExp`,
`Effect_BldFireExp`, and `Effect_BldDustExp` when the second roll modulo 100
clears the effect's probability gate. The gate is data-driven
(decoded 2026-08-18): `Effect_RollSurfaceEffectProbability @ 0x5cc1f0`
(renamed from `sub_5CC1F0` that session) scans
`g_SurfaceEffectProbTable @ 0x8418b8` — 40-byte rows
`{name[32], prob_main f32, prob_alt f32}`, empty-name terminated: Glass
1.0/0.33, Paper 0.1/0.05, Fire 0.2/0.05, Dust 0.33/0.1 — burns one
`PRNG_Next16` into `srand`, then accepts when the second
`PRNG_Next16 % 100 <= ftol(prob * 100.0)` (the ×100 scale `@ 0x7c4654`).
The glass user-point path passes `useAltProbability = 1`
(`@ 0x5cf0b3/0x5cf0f4` in `Terrain_SpawnEffectsAtUserPoint`). Gate arithmetic
(re-grilled 2026-08-19 against the raw table bytes): the Glass alt cell is
`0x3EA8F5C3` — exactly `0.33f` — and `0.33f × 100` is `33.0000013` in every
intermediate precision, so the `ftol` yields `33` and the `<=` admits rolls
`0..33`, a 34-of-100 gate. (The 2026-08-18 decode's "truncates to 32" claim
was arithmetic error — the product sits just above 33, not below.) The reimpl
ports the float×100-truncate structurally with the bit-exact table floats and
a compile-time boundary pin (`destruction.h kGlassShatterAltProbability` +
`static_assert`). The presentation drain receives
those resolved position, direction, and effect rows verbatim. Native
`destruction` pins full-pose placement, authored-radius selection, effect
order, PRNG state, and break-once; GUT `simulation_test` pins the real
3DI name/axis resolve and `destruction_present_pass_test` pins the transient
presentation leg.

`Entity_ApplyWeaponDamage @ 0x4e6820` order: dead flag; in-session building
gate (`g_destroy_buildings @ 0x24d2164`); same-team immunity when the target
def authors attrib 0x8000 (`@ 0x4e688d`); indestructible Flags 0x4000000 /
blast-armor word 0xFFFF (`@ 0x4e68aa`); base = ammo `kz_damage` (+46,
authority else 0); linear falloff from `kz_minradius` (+52) to the blast
radius (`@ 0x4e695a-0x4e699c`); blast armor gate ammo `penetration_kz` (+200)
below def+0x192; dying gate (+0x124); occupant scale for vehicles
(`Entity_ApplyOccupantDamageScale @ 0x4e5a50`); NoDie (attrib 0x40000000)
clamps to health-1. Persons then run the death-anim pick at damage time (bone
hardcoded 1 `@ 0x4e6ac7`, quadrant from the blast direction, cause 2, or 3 by a
~25% PRNG roll `@ 0x4e6a84` drawn ONLY when the surface distance lies in
[4.0, 8.0) u `@ 0x4e6a61` (no draw outside the band), 4 when the source kz is Slash) + the tag-23
impact effect; non-persons run the breakable-section sweep (collision sections
with byte flag & 2 inside the blast OR into the entity sectionMask
`@ 0x4e6e48`) and the health drain + deathCallback(2) + kill scoring.

Queue producers witnessed: the round-class kz dispatch (grenades/rockets),
`Entity_SpawnExplosionEffects @ 0x4399c0` (kz_M406HE + SP shrapnel),
`Entity_QueueKzBlastAtUserPoints @ 0x4eabf0` (renamed from the
`Projectile_SpawnEffectAtBoneOrDefault` misnomer — it queues DAMAGE at each
named user point r=5.0, else at the entity with r = def `kz` (+0x198) else
boundRadius), the crane/water-tower special (`@ 0x43fc70`, "scrane"),
`DeathPiece_PhysicsUpdate @ 0x48f500`, `Entity_InitDeathState @ 0x48f7c0`,
`Entity_UpdateFallingDeathPhysics @ 0x493f70` (the landing blast), and the
water physics `@ 0x4a92e0`. The kz ammo entries are interned by
`WeaponDef_ResolveAllReferences @ 0x540270`: `g_ammo_kz_OrganicBlast
@ 0x24E7DBC`, `g_ammo_kz_MItemBlast @ 0x24E7DB8`, `g_ammo_kz_DebrisBlast
@ 0x24E7DB4`, `g_ammo_kz_M406HE @ 0x24E7DB0` (all renamed this session; the
store-lags-the-push interning order was verified instruction-level).

### 24.2 Bullet damage to items

`Projectile_ProcessDamageOnTarget @ 0x4e7fb0` (the net-re §5.60 chain) zeroes
on: the indestructible flag (`@ 0x4e7ff6`), the def impact-armor word +0x190
== 0xFFFF (`@ 0x4e8019`) or greater than the ammo `penetration_impact` (+196)
(`@ 0x4e802a`), the dying marker +0x124 (`@ 0x4e8032`); clamps to remaining
health (`@ 0x4e8064`) and NoDie pins at health-1 (`@ 0x4e8074`). The entity
damage callback (+0x1C8) fires with phase 1 on every processed bullet hit and
phase 4 on the player-flag (0x100) kill leg. items.def `armor A [B]` writes
+0x190 = A then B, +0x192 = A (`@ 0x4a00e7-0x4a0147`) — one value fills both
words.

### 24.3 The destructible death chain

`Entity_HandleDestructibleDeathEvent @ 0x440210` (the destructible-class
deathCallback): phase 0 = the ambient time-of-day shot leg
(`Entity_SpawnRegionalEffect @ 0x408290` — dawnShot/dayShot/duskShot/nightShot
+ the interval reschedule into the entity timer); on the authority, already
husked resends S2C 0x26, else health <= 0 sends 0x26
(`Server_SendEntityStatePacket @ 0x509d70`) and runs
`Entity_ProcessDestructibleDeath @ 0x43fbc0`; a non-authority client destroys
on phase 4. The destruction: per-collision-section
`Entity_SpawnSectionDebris @ 0x43f580`, the scar clear
(`Scar_ClearEntriesByEntity @ 0x5ccec0`, ex `sub_5CCEC0`), Flags |= 6 (dead
2 + husk 4), the death tick +0x1AC (first write wins), the 992-tick re-notify
timer, and a shrunk-bbox invalidation call.

`Entity_SpawnSectionDebris @ 0x43f580`: samples the model's collision faces at
8.8 stride `(totalFaceCount<<8)/150`, resets the accumulator for each section,
forms each triangle centroid with signed divide-by-three truncation, then
promotes the Q8 result to 16.16. The callback returns a matrix-array base once;
the retail loop passes that same first matrix to every section transform (it
does not add the section ordinal). It launches AWAY from the +0x80 blast center
using retail's planar-distance approximation (else radially at pitch ~63.3
deg), and spawns `g_fx_TreeFoliageExp` for material-17 triangles else
`g_fx_TreeWoodExp` (the built-in effect-name pair table @0x849150).

**Port status (2026-08-16, D-ITEM-16 CLOSED).** `CollisionWorld` performs that
walk against the intact collision view before the husk flag changes model
identity. `process_destructible_death` converts every sample directly into its
material-selected, positioned, directed transient effect row; the Godot pass
no longer invents a six-spawn radial burst. Native `destruction` pins the
150-face cadence, signed-centroid edge, transform, launch direction, and
foliage/wood split; `destruction_present_pass_test` pins verbatim delivery.
**Ownership (re-witnessed 2026-08-21):** the material-17 foliage effect is
spawned OWNED by the entity `@0x43f839` (it dies with it) while the wood effect
is spawned unowned `@0x43f84d` (it outlives it); `DestructionEffectEvent`
carries no owner tag, which is the one fact the live port lacks — D-ITEM-16
stays FIXED. The #549 `section_debris.h` restatement of this routine was a
value-for-value duplicate of `sample_section_debris` and was deleted in the
post-merge tidy.

**The client fold (witnessed + ported 2026-08-05, D-NET-208).** The "a
non-authority client destroys on phase 4" leg above is DRIVEN by two S2C
routes, both ending in the same class death callback with reason 4:
S2C `0x13` `[u16 handle][i16 killerSource]` — the health<=0 detection's
non-player broadcast (`Entity_CheckAndProcessDeath @ 0x51b550`, msg 19, mask
0x90) → `NapiNPClientMsg_EntityDeath @ 0x42eb50` (Health=0 @ 0x42ebd6,
`deathAnimStateId` @ 0x42ebdf — the wire's i16 killerSource doubles as the remote
death-anim selection for organics, the same store zeroes the +0x1BA word (its IDB
gloss "clear ammo/weapon field" remains wrong), and while the destructible fold
consumes the handle + cb(4) legs the organic death-anim consumption still rides MP
corpse parity — cb(entity, 4, 0) @ 0x42ebf5); and S2C `0x26`
`[u16 victimSlot][u16 attacker]` — the destructible callback's own authority
(re)send (`Server_SendEntityStatePacket @ 0x509d70`) →
`NapiNPClientMsg_0x026 @ 0x42ec30` → `Entity_KillBySlotId @ 0x42bce0`
(alive-gated on Flags&2, Health=0 @ 0x42bd33, cb(entity, 4, flags)
@ 0x42bd6a). The client then runs the WHOLE chain locally from its own
pools: `Entity_UpdateAllEntities @ 0x4c2100` drains `DeathPiece_TickAll`
@ 0x4c221c and `Projectile_ProcessExplosionQueue` @ 0x4c223f
UNCONDITIONALLY on every peer, so the death chain's kz blasts detonate and
the pieces fly on a pure client too. Ported: JoinerConnection surfaces both
tags, `ClientReplicaPipeline::apply_entity_death` folds them (row Health=0 +
a once-drained record), the joiner world bridge (`engine/net/npruntime/joiner_world_bridge.cpp`) runs
`destruction_notify_item_damage(world, twin, 4)` on the materialized world
row, `World::run_logic_tick` runs the explosion/dead-settle/piece drains
under the MP visual-client predicate, and `mission_presentation.gd` builds the
destruction present pass for joiners. Named deferrals: the pool-0 organic
0x13 leg (presentation stays on the compact dead bit), the 0x13
local-player camera-lerp/scope leg, vehicle (is_ai_capable) victims (their
death rides the rows-21/23 state machine, §19.6), and the 0x26 itemType-1
flags-bit0 strip (our fold passes no flags).

### 24.4 The unitType death dispatch + death pieces

`Entity_DispatchDeathCallback @ 0x493ef0` (from `Entity_UpdateDeathTransforms
@ 0x494660`: child cleanup, savedLivePose+euler snapshot, dispatch, then
`Entity_InitDeathSounds @ 0x4939b0`) scar-clears, matches def `unitType`
(+0x196, the items.def `unit_type` token) against the table `@ 0x815410`
(stride 16: {type, flagBits=6, arg, callback}), calls `callback(entity, arg,
phase)` and ORs Flags 6; no row = `Entity_SpawnDeathPieces @ 0x493400` +
`Flags = Flags & ~0x20006 | 6`. Rows: 1/2/10/12 (vehicle families) = pieces
with post-death update := `Entity_UpdateFallingDeathPhysics @ 0x493f70`; 3
(person) = pieces + `DeathPiece_PhysicsUpdate @ 0x48f500`; 5/6/7/8 (building
families) = `Entity_ProcessBuildingDeath @ 0x494420` (ex `sub_494420`); and 11
(bridge) = `Entity_SpawnDeathEffectsAtBones @ 0x4944c0` (pieces + KZ + an
`Effect_ShockWaterBrdg` at every "DEAD" user point at water height).

The building callback's loaded huskFinal/husk model-pointer gate wraps its
body, not the dispatcher's final Flags OR. With a live model it calls
SpawnDeathPieces, clamps
`slideDecay` to 0 only when it was positive, emits
`g_snd_EXPLO_SHIP_TINY`, and installs `Entity_UpdateStaticDeathPhysics` even
when SpawnDeathPieces rejects a fully submerged entity. With no husk model the
callback body is a no-op; the matched-row `Flags |= 6` still follows. The port
now matches that gate: `resolve_collision_instances` records successful live
huskFinal/husk `ObjectData` resolution separately from the authored name,
and the building callback reads that runtime bit. D-ITEM-20 closed 2026-07-22.
For unitType 3 it spawns the pieces and installs the explicit `PiecePhysics`
mode backed by `DeathPiece_PhysicsUpdate`, followed by `PiecePitchSettle` for
`DeathPiece_SettlePitch`; this specialized pair is ported under D-ITEM-18 rather than
substituted with generic falling. UnitType 11's
common pieces and per-`KZ` blast path run, and as of 2026-08-15 its first husk's
case-insensitive `DEAD` bank is transformed through the complete entity pose;
one unowned `Effect_ShockWaterBrdg` is submitted per point at raw water height,
including zero, with no origin fallback (D-ITEM-19 fixed). The installed retail
base/RevX02 catalog does not author that exact name, so its faithful output is
the invisible `stockeffect` clone rather than the different `Effect_ShockWater`.
The `KZ` positions were fixed separately under D-ITEM-5 on 2026-07-20.

`Entity_SpawnDeathPieces @ 0x493400`: gate = husk model present (huskFinal
+0x38 else husk +0x34), not already husked, not fully underwater. The
explosion glow light (`LightPool_SpawnGlowEffect @ 0x49351a`, 2x model radius,
non-decorations), then per husk section 1..N — N = the husk RENDER object's
own section count (`renderObj[8]+52`, read `@ 0x49361a`), NOT the items.def
`husk_sub_parts` token (most retail defs author none; section 0 — the hull —
never leaves): the def `huskSubPartTypes[i]` byte (+0x101, clamped at 16) indexes
the 80-B debris-type table `g_death_piece_types @ 0x8404f0` (13 rows: HULL,
WHEEL, CHUNK_S/M/L, ROCK_S/M/L, CHUNKNP_S/M/L, CACTUS_, CHUNKSF_M — the
`DeathPieceType` mirror in engine/runtime/world carries the full decoded constants and
resolved effect/sound names), rolls the row probability, allocates from the
256x180-B ring `g_death_piece_pool @ 0x26BAC58`
(`DeathPiece_AllocSlot @ 0x57b4f0`, ex `SoundEmitter_AllocSlot`), renders ONLY
its own section (mask piece[31] excludes every other), spawns AT the section's
center (the render section-row dwords 14..16 through the entity orientation
matrix `@ 0x4938bf-0x493900`), budgets `rand % lifetime + 1` bounces (floor
lifetime/8), attaches the row trail effect + looped sound, and stamps the
spawned-section mask into entity+0x138 (`1 << section`, an x86 `shl` that
wraps the count mod 32).

The launch build `@ 0x493718-0x49380e` is two-stage and HORIZONTAL: (1)
2D-normalize the +-0.5 random spread summed with the wreck fold — the fold
`@ 0x493589-0x4935ff` is `normalize2D(vel) * |vel| * 2.0` once the wreck moves
faster than 0.1 u/tick (flt 2.0 @ 0x7C3B90, eps @ 0x7C69F4), zero at rest; (2)
add the row `launch_add` ALONG the AI row's normalized 3D motion direction
(aiRuntime +0x64..0x6C — zero for brainless items, so launch_add contributes
nothing to a static barrel) and 2D-normalize again. The vertical is an
INDEPENDENT `rand[0,1) * 1.25 * velScale` (flt 1.25 @ 0x7C6F18) — the
horizontal launch speed is always EXACTLY the row velocity; only the direction
varies. Spins: `Death_RandomSpinRateBam @ 0x57b940` (ex `sub_57B940`) =
`max_deg * (rand%100)/100` floored at `min_deg`, in BAM32 per tick
(deg x 2^32/360 = 11930464.0 @ 0x7D76D0) — NOT uniform in [min, max]; a
quarter of WHEEL's 3.5..14 rolls land exactly on the floor. The vertical kick:
the def TYPE word (+0x5C) == 2 = DECORATION (the same field the glow-light
skip tests `@ 0x4934ee`) drops `slideDecay -= 16182`; everything else pops
`+= (rand>>4) + 4096` (`@ 0x493969` — NOT the unitType dispatch word).

`DeathPiece_TickAll @ 0x57b900` (ex `sub_57B900`) invokes each piece's
callback `Entity_ProcessDeathPiecePhysics @ 0x492dd0`:
`Entity_ApplyGravitySimple @ 0x492d80` (above water `velZ -= 334`/tick; BELOW
water `velXY >>= 1` per tick and `velZ` pinned at -4096, then `pos += vel`,
`Yaw += spinA`, `Pitch += spinB`), ground rest at `terrain + 1024`
(`@ 0x492e0d`), ground bounce (spin halves, velocity x0.95 @ 0x7C6FB8,
vertical negates through the row bounce factor, dust effect + the
speed>20480-gated sound), the water-surface splash (strict `preZ > water`
`@ 0x492e20`) + sink-to-free, and on exhaustion the final effect/sound then
persist-as-ground-debris (row flags bit 0: wheels and large chunks stay) or
free. The exhaustion leg first releases the trail, then tests debris-row
flags bit 1: every row except `CACTUS_` calls the standard permanent-terrain
scorch router with id 7 (`@ 0x492fdf..0x492fec`) before the final effect/sound.
`DeathPieceSim::tick` now preserves that gate and call order; the focused
`destruction` ctest pins scorch bounds/CRT texture selection, cactus
suppression, bounce silence, and underwater-free silence.

### 24.5 Death sounds + the wreck effect banks

`Entity_InitDeathSounds @ 0x4939b0` (phase bit 0 = the silent variant): plays
the def `soundDeath` (+0x6DB name resolved to +0x860; items.def token
`sounddeath`), releases the +0x1CC burn emitter, then fills three
bone-attached 4-slot banks: fully submerged = the def +0x446 water-death pair,
else the `particledeath` pair (+0x412 handle / +0x414 "Dead" bone mask);
always the fire family (+0x47A, the "Fire" bones) and the third family
(+0x4AE, "Other"); and queues a `g_ammo_kz_OrganicBlast` blast (r=5.0) at
every husk "KZ" user point via `Entity_QueueKzBlastAtUserPoints @ 0x4eabf0` —
THE death explosion.

Port note (2026-07-20): `resolve_collision_instances` now caches every exact,
case-insensitive `KZ` point from the authored first-stage `huskModel`; a
final-only definition does not borrow the `huskFinal` piece source and keeps
the no-point fallback. It converts IR
`(-sourceY, sourceZ, sourceX)` to destruction's mission-local
`(sourceX, sourceY, sourceZ)`, and stamps `ItemDeathTraits::kz_points`.
`emit_death_sounds_and_effects` already applies the full authored placement
Euler transform and queues radius 5.0 per point; an empty bank keeps the
witnessed entity-origin `kz ?: boundRadius` fallback. The Godot integration
test pins first-husk selection, final-only exclusion, multiplicity, and axes; the native
`destruction` test pins the downstream Euler transform and radius.

`Entity_UpdateDeadWreckEffects @ 0x493140` (renamed from
the `Entity_UpdateMuzzleFlashAndEffects` misnomer) follows the husk bones per
tick, rolls the fire crackle (PRNG < 16/65536 = `g_fx_BoatExpSec` +
`g_snd_EXPLO_SHIP_SM_b`), and steams a bone out when it dips underwater
(`g_fx_Boat01Steam`). Ground contact (`Entity_TransitionToGroundDeath
@ 0x493080`) spawns the item's authored `particlefinale` effect once at the
entity position (the +0x4E2 interned handle, name +0x4E4 — resolved by
`resolve_item_materials_and_spawn_bone_trails @ 0x5231cb`; the earlier
"+0x4E0 impact pair" gloss was wrong: only the one +0x4E2 word is read
`@ 0x493088`), calls `PeriodicSound_ClearByEntity @ 0x57b3e0`, backs the
grounded pose into savedLivePose (`@ 0x4930dd..0x493104`), and installs the
settle physics. The clear scans the 256x20-B periodic-sound pool at
`g_periodic_sound_pool @ 0x26B8050` — **witnessed a NO-OP in retail JO
(2026-08-12)**: the pool's allocator (`@ 0x57b380` — slot = {min_s, max_s,
countdown, entity, soundDef}, reload `(min + PRNG%(max−min))·62` ticks) and
its memset reset (`@ 0x57b360`) have ZERO xrefs, so nothing ever fills the
pool the per-frame `PeriodicSound_TickAll @ 0x57b450` walks, and the
entity-matched clear always clears nothing. It also does NOT touch the
256x180-B `g_death_piece_pool @ 0x26BAC58`: death-piece slots carry no owner
pointer, and `Entity_SpawnDeathPieces` writes none.

Port status: the reimpl creates at most one origin-anchored group for each
authored Dead/water, Fire, and Other family and performs one crackle roll per
wreck. The crackle roll itself moved into the SIM (S12b, 2026-08-07):
`destruction_tick_dead_items` rolls per logic tick per burning wreck
(husk + authored `particlefire`) on the world's rol-xor PRNG stand-in
stream — the same generator as `PRNG_Next16_C @ 0x6131b0`, one stream
standing in for the A/B/C instances — with retail's evaluation order (the
draw consumed BEFORE the water gate `@ 0x4932bf`), the effect emitted as a
transient event and the sound routed through the fire-sound distance-delay
queue at the ENTITY position (`Sound_PlayWithDistanceAttenuation @ 0x4932e2`
— the previous shell-side roll ran per render frame on a Godot RNG with an
immediate ungated sound). It does not populate or follow the four bone slots
independently. It
also does not sample fire-bone submersion or emit `g_fx_Boat01Steam`: the
effect world's kill plane merely culls particles at a plane, so it cannot
substitute for the retail steam spawn or per-bone bank release (D-ITEM-15).
The settle transition is ported, and D-ITEM-14 CLOSED 2026-08-12: the four
ported transition sites (`destruction.cpp transition_to_ground_death` — the
routed-falling contact `@ 0x494113`, both static-death legs
`@ 0x4942c6`/`@ 0x4943da`, and unitType-3 pitch equality `@0x48f0c7`) play the
authored `particlefinale` once at the
grounded pose and stamp savedLivePose; the periodic-sound clear closed as
faithful-nothing (the pool has no producer — above). DeathPiece slots remain
ownerless and untouched, matching retail. The debris-type trail column rides
the one native `kDeathPieceTypes` table onto the piece drain
(`death_piece_trail_effect`; the shell table died with S12b).

The main dead-wreck settle is a four-callback family, with unitType 3 adding a
second short-lived pitch-settle phase after its specialized contact. The shared
production-mode pass walks installed callbacks in pools 1/2, including
AI-capable entities. The falling pair share gravity (-334/tick above water;
below it `velXY >>= 1` per tick and the fall pins at -4096). Their ground line =
`Entity_RaycastGroundHeightAndObject @ 0x414320` (terrain AND objects, mask
0x200000) minus `|husk sec0 z min|` upright / plus `|sec0 z max|` inverted
(the section-row +84/+88 extents — the wreck rests its lowest geometry on the
ground):

- `Entity_UpdateFallingDeathPhysics @ 0x493f70` — the unitType-routed rows'
  falling leg: adds the water-crossing splash when the bound top passes below
  (`@ 0x49409f` — the effect slot @ 0x2C25C64 + the def water slot +156 else
  `g_snd_IMP_DEBLRG_WATER`). At `newZ <= ground` it temporarily writes the
  ground pose, transitions to Generic, plays the def landing slot +140 else
  `g_snd_IMP_VCL_DROP`, and queues the authority landing blast (r = def `kz`
  else boundRadius `@ 0x4941be`). Between the transition and sound it adds
  standard permanent-terrain scorch 7 only when the ground trace returned no
  entity (`@ 0x49414e..0x494179`), using the entity's current/pre-tail X/Y.
  The portable ground query is terrain-only (D-ITEM-9), so every reachable
  routed contact takes that null-entity scorch leg. The callback's
  unconditional tail then
  commits the computed X/Y/newZ — including a newZ below ground — while
  retaining XY and `slideDecay`. The next Generic tick handles contact.
- `Entity_ProcessFallingDeathPhysics @ 0x461d30` — the GENERIC falling leg
  (SpawnDeathPieces' default install, the post-landing state, and the AI-SM
  rows 21/23): at `newZ <= ground` it restores the complete pre-move pose,
  sets `slideDecay = 0`, and retains XY velocity (after any underwater
  halving); there is no landing sound or blast. After the routed tail above,
  this means the retained below-ground pose becomes the next tick's restored
  pre-move pose. The items.def `attrib2 & 0x100` `StaticDeath` bit is promoted
  into `ItemDeathTraits` and freezes Generic before it mutates pose or motion;
  routed Falling and building Static do not consult it.
- `DeathPiece_PhysicsUpdate @ 0x48f500` — unitType 3's specialized main-husk
  leg. Above water it subtracts 334 from `slideDecay`, consumes exactly one
  `PRNG_Next16_C` draw, and either integrates pitch/roll rates or adds four
  degrees to yaw. Below water it detects the strict pre-position crossing,
  clears both angular rates, submits `Effect_MedSplash` plus the authored +156
  sound or `EXPLO_HELO_WATER`, halves signed XY, and floors only a fall below
  -2048. After raw-Q16 integration, `Entity_CalcSlopeForces @ 0x4b0b00`
  samples forward/back at 5.5 units and left/right at 1.375 using the 1024-row
  Q22 direction table and four `(z+0.25) -> (z-1.75)` rays. It clamps the two
  deltas before the `<<10`/`<<12` BAM results and averages all four heights,
  minus `(240 * modelBoxZLow) >> 8` — where the model is picked by the husk
  flag: `test byte ptr [entity+0x24], 4` selects the HUSK model (entity+0x34)
  for a husked entity and the graphic model (entity+0x30) otherwise, a null
  pick keeps the correction at zero (no cross-fallback), and the floor is the
  picked model's collision block `+0xB0 -> +0x28` (the CMDL header bbox
  z-lo) `[orig: @ 0x4b0c0e..0x4b0c31]`. Every DeathPiece_PhysicsUpdate caller
  is husked, so the live path reads the husk shell's floor; the graphic
  branch serves only `AI_UpdateFallingPhysics @ 0x457ff9`. Port:
  `piece_physics_rest_floor_q16` (destruction.cpp) — husk floor =
  `CollisionWorld::husk_model_for(...)->min[2]`, graphic floor =
  `VehicleTraits::box_z_lo`; `destruction` ctest pins husk-over-graphic and
  the null-husk zero. A strict above-water `z < ground` contact
  writes `ground-0.5`, stores forward slope in the former pitch-rate register,
  emits `Effect_HeloGroundHit`, standard terrain scorch 7, authority-only
  `kz_OrganicBlast` then `kz_MItemBlast`, and the authored +140 sound or
  `EXPLO_VEHCL_LG`. The portable four-ray query has the exact terrain segment
  and bound correction; object hits stay in D-ITEM-9. `DeathPiece_SettlePitch @ 0x48f0b0`
  then steps pitch toward the stored slope by at most two degrees, snaps inside
  four, and only on the following equal-pitch tick transitions to Generic and
  runs `Entity_TransitionToGroundDeath`. D-ITEM-18 closed 2026-08-23; the
  focused `destruction` test pins PRNG count/branch, raw motion, ramp slope,
  landing/scorch/effect/sound order, both authority blasts, non-authority
  silence, water crossing, and the delayed transition.
- `Entity_UpdateStaticDeathPhysics @ 0x494230` — the building/static rows:
  each tick clears Flags 0x20000 and samples terrain plus the water plane
  (no-water sentinel normalized to 0). `water < ground` writes z=ground and
  transitions to Generic immediately, but the callback still runs its motion
  tail. Otherwise the landing line is ground-25 when `water-ground > 10`, and
  ground when the difference is at most 10. It then advances XYZ, float-truncates
  each XY component after multiplying by the exact `0.9700000286102295f`,
  zeroes a component only when `-8 < value < 8`, and subtracts 167 from
  `slideDecay` only after both XY components stop. The final contact test is
  strict (`position.z < landing_line`; equality keeps Static); transition
  writes z=ground and Generic but retains all motion fields. The following
  Generic contact clears vertical decay. The port matches this ordering for
  unitType 5/6/7/8, including AI-capable entities.

### 24.6 The husk swap (render + collision)

Flags bit 2 (4) is THE husk swap: the ray pick substitutes entity+52 huskModel
when set (`Entity_RaycastCollisionModel @ 0x413086`), the contact query picks
the same (`@ 0x4ae233`), the pool raycast walk substitutes +52
(`raycast_against_entity_pool @ 0x538720`), and a def with NO husk keeps its
graphic serving — the witnessed fallback. Ported: `CollisionWorld` husk
instances (assign_entity_husk; every query resolves through the one
`target_view` seam), the present-pass model swap
(`destruction_present_pass.gd`), and dead non-organics now RENDER instead of
hiding (the old D-AI-9 stand-in). items.def: `huskfinal` (+0x80),
`husk_sub_parts` (+0x100), `husk_sub_part_types NN_NAME` (split at the FIRST
underscore, slot NN-1 in 0..15, name matched by `DeathPieceType_FindByName
@ 0x57b310` — renamed from the `MinimapSlot_FindByEntityPtr` misnomer),
`husk_swap_at` (+0x19C) / `husk_swap_at_sec` (+0x1A0) with the witnessed
dual-unit parse (percent x0.01 while +0x1A0 is 0, else seconds x62; scales
dbl 62.0 @ 0x7c88c0 / flt 0.01 @ 0x7c56a8), `debris_scale` (+0x1BC), `kz`
(+0x198), `armor` (+0x190/+0x192) — all parsed in engine/formats/def and mirrored in
the FFI structs.

### 24.7 Divergences

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-ITEM-1 | The bullet item hit-test now runs the witnessed shape: bound-sphere broad phase over pools 1/2 (model-less entities excluded as the proximity-residency equivalence) + the collision-model CFAC FACE narrow phase (husk-aware; a sphere graze that misses every face lets the round fly on) with the face material feeding the impact tag (material + 4 — the port's extra "building material 1 → 23 flesh" remap in `RoundSim` REFUTED 2026-08-15 and DELETED: `Projectile_HandleEntityImpact` passes `ray[22] + 4` unconditionally `@0x4e982b` and `AmmoDef_ProcessImpactEffect` clamps only ≥ 28 `@0x40a1bf`; the remap is the knife presenter's PERSON leg, `Weapon_RaycastAndSpawnImpact @0x4e8880..0x4e8888`). The adjacent person-leg residual raised 2026-08-15 was GRILLED and FIXED 2026-08-22 — see D-ITEM-21. A dynamic item that survives broad phase without its required live collision model is a fatal binding invariant, not substitute geometry. Residuals: the `+533` refNum self-hit exclusion and the retail prox-slot tables (we scan the pools directly) are unmodeled; the blast pool-2 leg still uses the bound sphere, not the AABB-face refinement | `Projectile_RaycastProximitySlots @ 0x4e5340` → `Physics_RaycastAgainstBoneCollision @ 0x4e4cb0` (see §15.8); the AABB refinement `@ 0x4eb700`; material + 4 `@ 0x4e982b` / `@ 0x4e9b80` | shots beside a prop no longer stop midair on the invisible bound sphere, impact effects pick the surface material row (metal barrels spark as metal), and hit points land on real faces; ctest `collision` face-raycast set |
| D-ITEM-2 | `husk_swap_at`/`_sec` parsed for format fidelity only — the runtime consumer is unwitnessed (no +0x19C/+0x1A0 reader found this session) | fields written `@ 0x49f1ce-0x49f2c2` | no behavior port yet; find the reader (a progressive damage-stage swap is the hypothesis) |
| D-ITEM-3 | The mid-life breakable-section sweep (a blast marks collision sections with byte flag & 2 into sectionMask) is a cited stub — our CollisionModel carries no per-section flag byte | `@ 0x4e6c5e-0x4e6e6b` | partial visual damage (windows/panels before death) missing; needs the section-flag plumb in the collision build |
| D-ITEM-4 | Death pieces present only as their row's TRAIL effect following the sim piece: the single-section husk mesh, its render spin, and the explosion glow light are absent; one world-local PRNG stream stands in for the three retail streams | pieces render one husk section w/ spin `@ 0x493400`; `LightPool_SpawnGlowEffect @ 0x49351a`; PRNG_Next16/_B/_C | the debris trajectory is pinned, but the visible chunks do not match retail; mesh pieces need section-ordinal render instancing. `CollisionSection::parent_part_index` preserves COBJ hierarchy metadata and is not that selector |
| D-ITEM-5 | **FIXED 2026-07-20:** the active first-stage husk's exact case-insensitive "KZ" user points feed `ItemDeathTraits::kz_points`; each queues r=5.0 after full authored placement rotation, while a model with no match falls back once at the entity with r = def kz else boundRadius | `Entity_QueueKzBlastAtUserPoints @ 0x4eabf0` | `simulation_test` pins first-husk selection, final-only exclusion, all-match multiplicity, and IR→mission axes; `destruction` pins full-Euler placement and the radius-5 queue. Wreck-bank anchors remain separately D-ITEM-15 |
| D-ITEM-6 | Blast/damage stubs: organic knockback (`Entity_ApplyCollisionForce`), the victim-attached burn emitter + hit sound (the ammo +72/+76 pair — field source unwitnessed), medic (type 3) + vehicle-ram (type 1) queue legs, the occupant damage scale, `g_destroy_buildings` (an MP rules seam), and the S2C 0x26/0x2F/0x21 wire emits | `@ 0x4eb1d2 / @ 0x4eb292 / @ 0x4eadc6 / @ 0x4e5a50 / @ 0x4e6860`; net-re §5.60 | each cited at its port site; glass presentation closed under D-ITEM-17, while the wire legs stage with the npruntime death broadcasts |
| D-ITEM-7 | Which items take the destructible death path is routed by KIND (non-organic, non-AI-capable) + unit_type; retail routes via the def class resolve (`EntityDef_LoadModelsAndCallbacks @ 0x439f50` callback columns, unwitnessed per class) | deathCallback (+0x1C8) authored per def class | same observable for shipped JO data (destructibles author no ai/move function); witness the class-to-callback table to close |
| D-ITEM-8 | The crane/water-tower special death (the "scrane" pool walk + the double kz queue `@ 0x43fc70`) and `Entity_ProcessCraneDestruction @ 0x43eee0` are unported; the destructible 992-tick spawnPhase re-notify and the ambient phase-0 shot leg (`Entity_SpawnRegionalEffect @ 0x408290`) are unported | as cited | special-cased content (shipyard cranes, water towers); the ambient shot leg is a separate feature (items firing scheduled time-of-day sounds) |
| D-ITEM-9 | The Falling/Generic wreck callbacks and unitType-3's four short slope rays ground on TERRAIN only. Falling/Generic use sec0 z extents synthesized from LOD-0 primitive bounds (upright leg only); PiecePhysics uses the husk-flag pick — the husk collision shell's floor for a husked piece (the section-AABB union stands in for the CMDL header z-lo), `box_z_lo` otherwise. Static's separate terrain/water thresholds are ported as described in §24.5 | `Entity_RaycastGroundHeightAndObject @0x414320` (Falling/Generic, terrain + objects, mask 0x200000); `Entity_RaycastGroundHeight @0x4142c0` x4 from `Entity_CalcSlopeForces @0x4b0b00`; section-row +84/+88 extents `@0x461e23-0x461e4b` | a wreck dying on a roof can sink to terrain below; port the object-return leg for both query shapes and verify the generic runtime section-row fields against the render-model builder |
| D-ITEM-10 | `dword_2C25C64` is resolved and both routed/specialized water crossings now emit `Effect_MedSplash`; fallback sounds are ported (`IMP_DEBLRG_WATER` / `IMP_VCL_DROP`, and specialized `EXPLO_HELO_WATER` / `EXPLO_VEHCL_LG`). The def per-item landing (+140) and water (+156) sound slots remain unmodeled | `@0x4940c6-0x494100 / @0x49417c-0x4941af`; specialized twins `@0x48f547..0x48f588 / @0x48f726..0x48f759` | items authoring custom impact sounds still play the matching fallback; splash visuals now route through the ordinary destruction-effect presenter |
| D-ITEM-11 | The round exclusion set skips shooter + mount (Controller/Gunner/Driver seats only — a Passenger's rounds can hit their own vehicle) + the Gunner mount's standing-on carrier, PORTED 2026-07-18 (§15.8a); the FOURTH slot — `projectile+388` ← the fire request's dword +40 — is consumed by every prox walk but its fill is an uninitialized extra on the client fire path, provenance OPEN (the server path `Server_ClientFiredRound @ 0x50baa0` unwalked) | `ray[17..20] @ 0x4ea2a5-0x4ea2f8`; `RoundData_SpawnRound @ 0x4ec0d0` ([97] ← hitData+40); compares `@ 0x4e5572/@ 0x4e5782/@ 0x4e5983/@ 0x4e4c4e` | firing from Controller/Gunner/Driver seats no longer self-hits the hull; walk 0x50baa0's cmd[21]→spawn plumbing to close the +388 slot |
| D-ITEM-12 | Round BALLISTICS are absent: no gravity, drag, wind, water. Original: velZ −= 167/tick for non-thruster rounds without ammo flag 0x100 (`@ 0x4eaa5a`; the 0x100 class takes −167 inside the slow regime instead `@ 0x4e6329`); per-tick drag force = `g_ProjectileDragTable[62·speed>>16, clamp 1219]` scaled by ammo drag (+28) — the 4000-entry table is generated at init by a piecewise power-law over ~40 speed regimes (transonic bands 1025..1360 ft/s) — direction −vel normalized, WIND-relative (`@ 0x2C059E4..EC`), 25× underwater, a velocity-reversal zero clamp, and a one-shot random TUMBLE kick when the speed index first drops below ammo+176 (spread ammo+180, seeded by ownerConnectionId); water: hitType-4 splash at the plane + rounds continue submerged, killed when speed < 0x4000 below water (`@ 0x4ea13e`) | `Entity_ApplyDragAndBounceForce @ 0x4e5ec0`; `Projectile_InitDragTable @ 0x4e78d0`; `g_ProjectileDragTable @ 0xB7B300`; gravity `@ 0x4eaa5a`; water `@ 0x4ea4e0` | our rounds fly straight forever — no drop, no slowdown, crosshair-perfect at any range, no water interaction; port = extract the ~40 (exponent, scale) double pairs + the two scale constants off 0x4e78d0 and the wind source |
| D-ITEM-13 | Hit-resolution residuals: (a) the terrain leg sub-steps the bilinear column at 2-u intervals with a crossing refinement — the original raycasts the hi-res heightmap (`Terrain_RaycastHeightmapHiRes_Thunk @ 0x610890`) with a proportional end-below-ground fallback (`@ 0x4ea42b-0x4ea4af`), so thin crests can tunnel in ours (the strict-less tie-break itself was FIXED 2026-07-18); (b) the person effect point is FIXED 2026-07-18 (`ray[29] - 0x800`), but generic item/terrain effect backoff and retail's post-hit round parking at hit+0x800 (+victim boundRadius for persons) remain absent `@ 0x4ea603-0x4ea7d5`; (c) ~~the pool-0 person path used one body cylinder~~ FIXED 2026-07-18: `Physics_RaycastAgainstBoneSections @ 0x4e4670` now walks the current posed COBJ spheres with strict `COBJ[i]` ↔ `boneMatrix[i]` pairing (COBJ parent/offset/CXLT ignored), exact radius scaling/caps, section mask, split `ray[31]` reaction/death and `ray[32]` normal-infantry damage semantics, ammo bullet radius, and first-person-entity termination; the bounded torso sphere is only used when graphic resolution cannot supply a usable COBJ model; (d) ~~our sphere gate was segment-vs-sphere (a boundary-crossing requirement: a tick segment entirely INSIDE a big bound sphere skipped the entity — the in-play shoot-through-building-walls report)~~ FIXED 2026-07-18b: the item-leg gate is now the witnessed per-axis AABB + UNCLAMPED perpendicular line distance (`round_broad_phase`, round_sim.cpp), the face-less stand-in hits at t=0 from inside, and the ctest `collision` `test_round_inside_bound_sphere_hits_wall` pins both the inside-sphere wall stop and the past-the-edge fly-on | as cited; person path §15.8b; the gate `@ 0x4e53d4-0x4e554a` / `@ 0x4e5492`; the dispatch order `@ 0x4ea3b4-0x4ea5f2` | posed reaction/death bones and normal-infantry damage zones are live; remaining drift is thin terrain crests, generic effect/parking offsets, the optional FatBullets floor, and the attrib-0x200 seat x6 branch |
| D-ITEM-14 | CLOSED 2026-08-12: the four ported transition sites (`transition_to_ground_death` — routed-falling `@0x494113`, static legs `@0x4942c6`/`@0x4943da`, unitType-3 pitch equality `@0x48f0c7`) play the item's authored `particlefinale` once at the grounded pose and stamp savedLivePose; the periodic-sound clear closed as FAITHFUL-NOTHING — the pool has no producer in retail JO (allocator `@0x57b380` + reset `@0x57b360` have zero xrefs; `PeriodicSound_TickAll @0x57b450` walks an always-empty pool), so the entity-matched clear never clears anything and no pool is modeled. The old "+0x4E0 impact pair" gloss corrected: only the +0x4E2 `particlefinale` handle is read `@0x493088` | `Entity_TransitionToGroundDeath @0x493080`; intern site `resolve_item_materials_and_spawn_bone_trails @0x5231cb` (name +0x4E4 → handle +0x4E2); `PeriodicSound_ClearByEntity @0x57b3e0` over `g_periodic_sound_pool @0x26B8050`; the separate DeathPiece pool is 256x180 B at `0x26BAC58` | `destruction_test` pins routed and specialized finale/saved-pose transitions plus generic silence |
| D-ITEM-15 | Wreck effects are one origin-anchored group per authored family plus one fire-crackle roll per wreck; there are no four-slot Dead/water/Fire/Other bone banks, per-slot bone follow, or underwater `g_fx_Boat01Steam` transition. The effect kill plane is particle culling only and cannot substitute for spawning steam | `Entity_InitDeathSounds @ 0x4939b0`; `Entity_UpdateDeadWreckEffects @ 0x493140` | large/multi-bone wreck effects originate and roll at one point, and burning bones entering water neither steam nor retire like retail |
| D-ITEM-16 | **FIXED 2026-08-16.** The intact `CollisionWorld` view now samples every section at the total-face 8.8 stride, uses retail's signed centroid arithmetic and first callback matrix, derives the witnessed blast/radial direction, and emits material-17 foliage versus wood effects at each sampled triangle | `Entity_SpawnSectionDebris @ 0x43f580` | `destruction_test::test_section_debris_samples_collision_faces` pins 150 samples plus centroid/transform/direction/material; the present-pass test pins the resolved event row |
| D-ITEM-17 | **FIXED 2026-08-16.** Exact stock graphic→userpoint resolution feeds full-Euler glass points; the pool-2 blast leg uses authored `kz_maxradius`, four ordered probabilistic shatter families, exact PRNG consumption, and a persistent per-point broken bit | `Projectile_ProcessExplosionQueue @0x4eb814-0x4eb85d`; `Terrain_SpawnEffectsAtUserPoint @0x5cee20` | native `destruction` pins range/effects/RNG/break-once; GUT pins real-model name/axis resolution and verbatim presentation |
| D-ITEM-18 | **FIXED 2026-08-23.** UnitType 3's explicit `PiecePhysics` callback now ports raw-Q16 air/water motion, the one-draw angle branch, four Q22 slope probes and the husk-picked model-bottom correction, strict landing pose, `Effect_HeloGroundHit`, scorch 7, authority-only Organic/MItem dual blasts, exact fallback sounds, and a separate `PiecePitchSettle` state for `DeathPiece_SettlePitch`'s two-degree step/four-degree snap/delayed transition | `DeathPiece_PhysicsUpdate @0x48f500`; `Entity_CalcSlopeForces @0x4b0b00`; `DeathPiece_SettlePitch @0x48f0b0` | `destruction_test::test_specialized_piece_physics_callback` pins dry/wet/ramp/authority paths; object participation in the four rays is tracked once under D-ITEM-9 |
| D-ITEM-19 | **FIXED 2026-08-15.** Collision resolution retains exact case-insensitive `DEAD` points from the first husk only; UnitType 11 transforms each through full Euler and emits one family-0 `Effect_ShockWaterBrdg` at raw water height, first transition only, with no origin fallback | `Entity_SpawnDeathEffectsAtBones @ 0x4944c0` | `destruction_test::test_bridge_dead_points_emit_water_shocks` pins count, full-pose positions, zero water, first-transition gating, no fallback, and non-UnitType-11 silence; `simulation_test` pins real 3DI user-point axes |
| D-ITEM-20 | **FIXED 2026-07-22.** Building Static/collapse dispatch gates on `ItemDeathTraits::husk_model_loaded`, fed by successful live huskFinal/husk `ObjectData` resolution and kept separate from authored `has_husk` | `Entity_ProcessBuildingDeath @ 0x49442c`; the pointer gate wraps only the callback body | missing/corrupt husk assets receive the matched-row death flags, but no pieces, Static motion, or collapse sound; valid first-stage and final-only models both open the gate |
| D-ITEM-21 | **FIXED 2026-08-22 (grill).** The bullet person impact-effect leg. Correspondence first, because the dispatch is easy to misread: bullets reach a person ONLY through the bone-section pass. `Entity_BuildProximityLists_Pool01` fills `g_DynProx*` from pool 1 `@0x4b9389` and `g_PersonProx*` from pool 0 `@0x4b93eb`; `Projectile_RaycastProximitySlots` walks `g_StaticProx` for slotType 2 and `g_DynProx` for slotType 1, reaching `g_PersonProx` only on its DEFAULT leg `@0x4e57e7`, which the bullet dispatch never calls (it passes 2 `@0x4ea4f5` and 1 `@0x4ea535` only). Pool 0 is reached solely via `Physics_RaycastAgainstProximityList @0x4ea5bf` → `Physics_RaycastAgainstBoneSections @0x4e4670` (sole caller `@0x4e4c59`) → dispatch case 3 → `Projectile_HandleTerrainImpact_0 @0x4e98f0`, an auto-namer misnomer that is in fact the person-impact handler. Cases 1/2 therefore cannot produce a person hit, and the bone-bounds call inside `Projectile_HandleEntityImpact @0x4e9672` is a different thing (`Entity_ComputeBoneCollisionBounds`, gated `weaponType==15 && itemDef+92==5`). Ported from that handler: an already-dead victim (`hitEntity+36 & 2`) presents NOTHING; the LOCAL player takes tag 2 'player'; every other person takes tag 23 'flesh' but ONLY IF the victim's group differs from the local player's OR the victim is below half its items.def hp — a same-group victim at or above half health shows no impact effect at all (squad declutter). The two tag legs are mutually exclusive, not additive. Ours previously emitted tag 2 for every person collision | dead gate `@0x4e9920`/`@0x4e994f`; local compare `@0x4e9a55`, push 2 `@0x4e9aa1`; group WORDs +0x11C `@0x4e9aac`/`@0x4e9ab3`; healthMax WORD itemDef+0x17C halved by `sar dx,1` `@0x4e9abf`..`@0x4e9ac6`; signed Health WORD +0x11E compared with `jge` skipping the spawn `@0x4e9ac9`/`@0x4e9ad0`; push 17h `@0x4e9ad7` | shooting an enemy now plays the flesh row (`imp_bullet_flesh`) instead of the player row, and a healthy squad-mate no longer sprays. `projectile_combat::test_person_impact_tag_splits_on_identity_and_squad_health` pins all five branches; `npruntime_round_sim` and `simulation_test` pin the non-local sound row. RESIDUAL: the ADDITIVE body-armor leg `@0x4e99f8`..`@0x4e9a38` (tag 24 'bodyarmor' when the hit zone `hitContext+0x80` is 0..4 UNSIGNED and `victim+0x2C & 8`, the armor carry bit from weapon def `flags & 0x1000` — `WeaponSlotTable_LoadAllFromDefs @0x5415ac`) is UNPORTED: `WeaponInventory::carry_flags` exists but has no per-entity mirror, so the victim's bit is unreadable. Needs an `Entity` field plus a host/wire feed |

### 24.8 IDB write-backs (2026-07-17, saved)

2026-08-21 (the post-merge tidy; saved): `sub_5CC360` → `Scar_TextureForId`,
`Terrain_GetSurfaceFriction @0x5cc3b0` → `Scar_RadiusForId`, `sub_5CC4C0` →
`Scar_GetEntityCache`, `CTerrainRenderer_AdvanceFrameIndex @0x5cc1d0` →
`Scar_AdvanceRingCursor`, `Terrain_AddDecalToSector @0x5cc830` →
`Scar_AddEntry`, `terrain_render_sector_userpoints @0x5cd830` →
`Scar_RenderCache`, `sub_5CDF70` → `Scar_RenderAllCaches`,
`check_glass_impact_and_spawn_effects @0x5cf1b0` →
`Impact_SpawnGlassEffectsOrScar`, `dword_8417A8` → `g_scarTable`;
`CNapiTransport_DetachFromSession @0x5f75d0` → `Entity_ReleaseEffectEmitter`
(the `+0x1CC` emitter release — callers `Entity_Destroy`,
`Projectile_ReleaseEffects @0x4e82ba`, `Projectile_UpdatePhysics @0x4ea031`,
`WacScript_SpawnEffectAtSsnEntity`); `Entity_UpdateGravityAccumulator
@0x4928b0` → `Entity_UpdatePartSpinAccumulator` (the occupant-gated rotor /
part spin-up whose `HIWORD(+0x464)` the PANM feed reads); `Vehicle_ApplyBrakingForce
@0x45ceb0` / `sub_45CFB0` / `Entity_ApplyDamageOscillationFast @0x45d110` /
`Entity_ApplyDamageOscillation @0x45d240` → `Suspension_CompressWheelLinear` /
`Suspension_CompressWheelQuadratic` / `Suspension_OscillateWheelFast` /
`Suspension_OscillateWheel` (vehicle-client-movers-re.md §7). Entry comments +
`reimpl:` links on every one, on `Entity_SpawnSectionDebris @0x43f580`, the
two camera functions, `NetPacket_SerializePlayerState @0x4c09c0` (the read-side
goal stores), `Entity_UpdateInfantryPlayerBody @0x4b40e0` (the chase block
bounds), `Entity_UpdateVehiclePhysics @0x48af00` (#551's sites) and
`Entity_UpdateWatercraftPhysics @0x48d480` (the `+0x2B8` integrate that #551
mis-cited as the aircraft form).

2026-08-21 (the wire-up round's witness pass; saved): renames `sub_5CC2E0` →
`Scar_LoadTextures`, `sub_5CC3F0` → `Scar_InitDynamicBuffers`, `sub_5CC760` →
`Scar_ResetAllCaches`, `sub_5CC700` → `Scar_HasProjectedDecalAt`, `sub_5CC5E0`
→ `Scar_FreeProjectedDecals`, `sub_546680` → `Entity_GetMountSlotBoneIndex`,
`sub_5BBE90` → `HUD_GetChatBoxCoord`, `dword_28E4DF8` → `g_hudChatBoxCoords`;
entry comments (`[opennova 2026-08-21 …]`) on `Scar_AddEntry @0x5cc830` (the
by-pool ring selection, the section-local transform), `Scar_GetEntityCache
@0x5cc4c0`, `Impact_SpawnGlassEffectsOrScar @0x5cf1b0` (the `scar_type`
router), `Terrain_SpawnSurfaceEffectsAtUserPoints @0x5cea90` (the 24-row
table), `Scar_DrawBatches (ex Terrain_RenderFoliageBatches) @0x5ccd10` (the scar drawer),
`Entity_UpdatePartSpinAccumulator @0x4928b0` (the profile-type gate),
`Entity_UpdateHeloRotorSpin (ex entity_update_damage_accumulator_and_shadow) Entity_UpdateHeloRotorSpin @0x48fa70` (the HELO twin),
`Entity_CacheVehicleHUDStats @0x4929b0` (46 = 47), `Entity_UpdateVehiclePhysics
@0x48af00` (the wheel phase), `Entity_SpawnFromBMSRecord @0x40e9f0` (the
0x20000 seed), `ThirdPersonCamera_Update @0x437af0` / `Camera_ComputeThirdPersonView
@0x437d10` (the mounted legs), `Entity_ClearSuspensionState @0x4592b0`,
`Entity_ProcessWheeledVehicleSuspension @0x46b140` (the W2 latch correction)
and its tank/bike twins, `Entity_ProcessWheeledVehiclePhysics @0x475de0` /
`Entity_ProcessLightVehiclePhysics @0x479600` / `Entity_ProcessTrackedVehiclePhysics
@0x47c1c0` / `Entity_RespawnVehicle @0x45ff40` (the `+0x2ED` producers and the
spawn values), `ItemDef_ParsePhysicsProperty @0x49d870` (the four keys),
`Projectile_UpdatePhysics @0x4e9d70` (the pre-move water/life tests).
Proposed, not applied (curated names): `Entity_UpdateHeloRotorSpin
@0x48fa70` → `Entity_UpdateHeloRotorSpin`; `Scar_DrawBatches
@0x5ccd10` → `Scar_DrawBatches`; `dword_81518C` → `g_wheelSlipLockQ16`
(0xE8480000 = −6064.0). Both function renames re-confirmed by body reads
2026-08-25: `@0x5ccd10` walks the 32 scar batch lists at `0x2BDF848` that only
`Scar_ResetAllCaches @0x5cc797` and `Scar_RenderAllCaches @0x5cdf70` otherwise
touch (no foliage state anywhere in its body); `@0x48fa70` owns the +0x460 /
+0x464 / +0x468 rotor speed / angle / rate words, the 46603 decay and the
214748352 cap, reached only from `Entity_UpdateAircraftPhysics @0x4905a6`.

Renames: `DeathPiece_AllocSlot @ 0x57b4f0` (ex `SoundEmitter_AllocSlot`),
`PeriodicSound_ClearByEntity @ 0x57b3e0`, `DeathPiece_TickAll @ 0x57b900`,
`DeathPiece_GetTypeDef @ 0x57b350`, `DeathPieceType_FindByName @ 0x57b310` (ex
`MinimapSlot_FindByEntityPtr`), `PeriodicSound_TickAll @ 0x57b450`,
`Entity_ProcessBuildingDeath @ 0x494420` (+ the t6/t7/t8 thunks),
`Scar_ClearEntriesByEntity @ 0x5ccec0` (+ thunk),
`Entity_UpdateDeadWreckEffects @ 0x493140` (ex
`Entity_UpdateMuzzleFlashAndEffects`), `Entity_QueueKzBlastAtUserPoints
@ 0x4eabf0` (ex `Projectile_SpawnEffectAtBoneOrDefault`); data:
`g_explosion_queue @ 0xB7C688` (+count), `g_death_piece_pool @ 0x26BAC58`
(+cursor), `g_death_piece_types @ 0x8404f0`, the four `g_ammo_kz_*` slots, the
`g_fx_TreeWoodExp/TreeFoliageExp/Boat01Steam/BoatExpSec` and
`g_snd_EXPLO_SHIP_TINY/EXPLO_SHIP_SM_b/IMP_VCL_DROP/IMP_DEBLRG_WATER` slots.
Functions defined over unexplored code: `@ 0x494420`, `@ 0x494480/90/A0`.
Entry comments on the whole chain.

2026-07-18 (the piece/settle physics grill, saved): rename
`Death_RandomSpinRateBam @ 0x57b940` (ex `sub_57B940`); comments pinning the
spin distribution (`@ 0x57b940`), the decoration kick key (`@ 0x493969` — the
+0x5C TYPE word, correcting the earlier "helicopter" gloss), the
huskFinal-first piece model (`@ 0x4934af`), the two-stage launch build
(`@ 0x493718`), the piece gravity/water legs (`@ 0x492d80`), the three settle
callbacks (`@ 0x461d30 / @ 0x493f70 / @ 0x494230`), and the vehicle contact
mask (`@ 0x462a95`).

### 24.9 Impact scars — the `Scar_*` family (witnessed 2026-08-21; PORTED in the wire-up round)

The marks ordinary ammo leaves on what it hits are NOT projected-volume decals:
retail keeps a **256-slot ring per struck pool-1 item/vehicle plus one SHARED
ring for everything else** and draws each slot as an oriented two-triangle
square. Walked for #543, re-witnessed in the tidy, and completed by the
wire-up round's witness pass (the ring selection, the router's `scar_type`
kind, the batch base, the drawer and the glass leg); every function below was
IDB-renamed 2026-08-21 (the old names are in 24.8).

- **Router** `Impact_SpawnGlassEffectsOrScar @0x5cf1b0` (sole caller
  `AmmoDef_ProcessImpactEffect @0x40a170`, the call `@0x40a24e..0x40a264`
  passing `kind = the ammo's scar_type word +0x76` — the `scar_type` ammo.def
  token `[orig: AmmoDef_ParseProperty @0x40aeea..0x40af11]`, which `def_ammo`
  now parses; the knife leaf reaches the same processor through
  `Weapon_RaycastAndSpawnImpact @0x4e8460`): gates `kind != 0`, an entity, a
  def, `!(Flags & 1) && (def+92 == 1 || !(def+84 & 0x10000000))` `@0x5cf1f7`
  (a vehicle def always scars, any other def only without NoScar); tries the
  GLASS userpoint leg (`Terrain_SpawnSurfaceEffectsAtUserPoints @0x5cea90`,
  below); kind 2 skips the ring fallback `@0x5cf289`; else `Scar_AddEntry`
  with scar id **18** when the hit record's surface (`+0x58`) is 15 (the
  non-glass-group glass face), else id **1**, spin flag 1 `@0x5cf295..0x5cf2b2`.
- **Table** `g_scarTable @0x8417a8` (named 2026-08-21): rows
  `[id, minTex, maxTex, radiusQ16]`, `-1` terminated; id 1 = textures 0..3 at
  radius `0x2000`, id 18 = texture 27 only at `0x1000`. `Scar_TextureForId
  @0x5cc360` draws `PRNG_Next16 % (range + 1)` ONLY when `max > min` (the
  fallback never touches the shared stream) and returns
  `&aScorch1Tga[32·tex]` `@0x8413a8`; `Scar_RadiusForId @0x5cc3b0` returns
  row[3] (ex `Terrain_GetSurfaceFriction`).
- **Cache** `Scar_GetEntityCache @0x5cc4c0`: 128 caches × 0x4008 bytes at
  `unk_29DB3C0..dword_2BDB7C0` keyed by entity pointer (header = entity, 256 ×
  64-byte entries from +4, ring cursor at +0x4004); `alloc = 0` returns the
  terrain cache `dword_2BDB7C0`; when all 128 are taken it returns NULL — **no
  eviction**, the impact leaves no scar.
- **Add** `Scar_AddEntry @0x5cc830` (ex `Terrain_AddDecalToSector`; args
  `(entity, hitRec, arg2, hitPos, hitZ, scarId, spinFlag)`): gates `scarId
  != 0`, a model, `hitZ > Env_WaterHeightFixed` `@0x5cc865`, entity flag 4
  clear `@0x5cc894`, section/face indices in range, a face without flag
  `0x400` `@0x5cc92b`. **Ring selection is BY POOL** (`isEntityLocal`
  computed `@0x5cc873..0x5cc88c` = `Pool_GetIndexFromPtr(1, entity) >= 0 ||
  (def+84 byte0 & 0x80)`), but the ring is LEASED only AFTER the face gate
  (`@0x5cc96f..0x5cc983`) — a rejected hit never takes a ring. The face
  normal ×4 → Q16 `@0x5cc938..0x5cc960` is stored RAW (untransformed) for a
  pool-1 item/vehicle's per-entity ring (`Scar_GetEntityCache(entity, 1)`),
  whose slot keeps that model-local normal and the position transformed INTO
  section-local space through the transposed section matrix
  `@0x5ccc99..0x5ccca5` (bone byte = the section index, hitRecord+0x7C), so
  the scars follow a moving carrier; buildings (pool 2) and persons (pool 0)
  write the shared world ring `dword_2BDB7C0` (alloc 0) with the normal
  rotated by the live section matrix (the `@0x5cc96f` branch — the ONLY
  normal rotation in the function) and the position in world space. The
  tangent is a
  seven-case magnitude ladder `@0x5cc9da..0x5cca71` (b<a&&a>c&&b>c →
  (ny,−nx,0); b<a&&a>c&&b<=c → (nz,0,−nx); b<a&&a<=c → (−nz,0,nx); b==a →
  (0,−nz,ny); b>a&&b<=c → (0,−nz,ny); b>a&&b>c&&a>c → (−ny,nx,0);
  b>a&&b>c&&a<=c → (0,nz,−ny)) normalized `65536/len` `@0x5cca74..0x5ccad7`,
  `B = N × T` with T flipped when `N·(B × T) < 0` `@0x5ccada..0x5ccb37`;
  `spinFlag` draws `PRNG_Next16 << 16` as BAM32 `@0x5ccb50` FIRST (the texture
  word is drawn at the slot write) and rotates `T' = (T·c − B·s) >> 22`,
  `B' = (T·s + B·c) >> 22` `@0x5ccb54..0x5ccc65` — retail shifts EACH product
  `>> 22` separately before the add/sub, where the port sums the products and
  shifts once (a ±1 LSB difference, recorded, not fixed); the slot is
  `Scar_AdvanceRingCursor @0x5cc1d0` (compare-and-wrap at 256, ex
  `CTerrainRenderer_AdvanceFrameIndex`); the 64-byte entry = N@0, T@12, B@24,
  position@36, radius@48 (`Scar_RadiusForId`), texture index@52 (`(name −
  0x8413A8) >> 5`), owner@56, isBuilding@60 (`def+92 == 5`), bone@61.
- **Render** `Scar_RenderAllCaches @0x5cdf70` (from
  `Terrain_CollectVisibleEntities @0x5c91b7`) zeroes the 32 per-texture
  batches at **`0x2BDF848`** (32 CDynList24 × 12 B — the decompiler constant
  46004296; the earlier `0x2BDF8C8` reading was wrong) then walks the shared
  ring first and every live entity ring through `Scar_RenderCache @0x5cd830`
  (ex `terrain_render_sector_userpoints`): texture `< 0x20`, the fog-box
  cull, the `g_BuildingSectionVisMask` gate (D-OCC-5's reader; building
  owners by their own bit, others by their four containing-building handles
  +464..+476), `C = bone·pos` for entity rings / `pos` for the shared ring,
  `A = (r·T + 0x8000) >> 16`, `B` likewise, six verts `{x,y,z,argb,u,v}`
  in the order `C−A−B (0,0)`, `C+A−B (1,0)`, `C−A+B (0,1)`, `C+A−B (1,0)`,
  `C+A+B (1,1)`, `C−A+B (0,1)` in `Env_TerrainLightCombined | FF000000`,
  appended per texture via `Terrain_ParseSectorTypeCallback @0x5cf390`. The
  drawer `Scar_DrawBatches @0x5ccd10` (a misnomer; proposed
  `Scar_DrawBatches`; caller `Terrain_RenderSceneWithReflection @0x5c9658`
  after the lit sector entities): `CD3DDevice_SetFogAndBlendMode(dev, 0)` (the
  fog COLOUR select — scene fog colour; it never touches a blend state),
  identity world, vertex shader 0, FVF 0x142, the 1000-vertex dynamic VB
  `dword_2BDF9C8` (stride 24, `Scar_InitDynamicBuffers @0x5cc3f0`),
  `CGfxDevice_SetAlphaTestRef(128) @0x5ccdae` (ALPHAFUNC GREATER + ALPHAREF
  only — a latch), per texture `GfxShader_ApplyPassChecked(strip[k].effect,
  0x10000000)` (LIGHTING off) then D3DPT_TRIANGLELIST. **The drawer state
  lives in the strip's effect** (re-grilled 2026-08-21): `Scar_LoadTextures
  @0x5cc2e0` walks the 32-byte entries `@0x8413a8` {name[16], tex@16,
  effect@20, modeId@24, loadFlags@28} and `modeId` is a SELECTOR, not a state
  word — `0 -> 0x120651` `@0x5cc315`, `1 -> 0x460651` `@0x5cc321` (the
  decompiler shows the latter as a bogus `offset loc_46064F+2`), else 0 — fed
  to `GfxShader_Create1TexModeId(tex, modeWord) @0x679030`, then clamp wrap
  (`CGfxTexture_SetSamplerAddressing (ex sub_680720)(effect, 1, 0, 0, 0)`). Every strip but bhole1 (idx 27) carries
  modeId 0. Decoded through the mode-word layout
  ([render-material-re.md](../render/render-material-re.md) "The mode word"):
  **scorch 0x120651** = SRCALPHA/INVSRCALPHA blend, stage 0
  `MODULATE2X(TEXTURE, DIFFUSE)` colour / `MODULATE(TEXTURE, DIFFUSE)` alpha,
  FOGENABLE, z-write OFF, CULLMODE CCW, and **no ALPHATESTENABLE** — the 128
  latch is inert there; **bhole1 0x460651** = the same plus ALPHATESTENABLE
  (GREATER 128 live), z-write ON, CULLMODE NONE. The scorch TGAs are black RGB
  under an alpha falloff (peak ~0.75), so the mark IS the alpha blend over
  the lit wall; alpha-testing them paints an opaque black blob. With
  `Env_TerrainLightCombined` as DIFFUSE and the unconditional
  `GfxDevice_Modulate2XEnabled` (`CGfxDevice_CreateDevice @0x67eb5f`), the
  colour is `saturate(2 · tex · light)`. Winding: the slot writer's basis is
  LEFT-handed (`n . (t x b) < 0` — the bitangent is crossed before the
  tangent-only flip, `@0x5ccada`/`@0x5ccae6..0x5ccb37`) and the upload is the
  Y-negated reflection (`Math_FixedPointToFloat3_YNegated @0x611210`), which
  together put the quad's front on the struck face's side under the CCW cull.
- **The GLASS userpoint leg** `Terrain_SpawnSurfaceEffectsAtUserPoints
  @0x5cea90`: the 24-row surface-material table `@0x841980` ({surface[16]
  GLASS1..3, userpoint[16], model[16], fx[4][32] = Effect_BldGlassExp /
  PaperExp / FireExp / DustExp, scarId@176, radius@180 (0x8000..0x40000),
  useUserpointPos@184, projectedDecalPath@188 = 1 on every row}); a model
  userpoint matching the row's name within ±radius of the hit on all three
  axes takes the projected-decal path (`scar_project_decal_onto_entity
  @0x5ce4a0` — pool-2 only, above water, the clipped triangle mesh) plus the
  four effects rolled on the MAIN column of `g_SurfaceEffectProbTable
  @0x8418b8` (Glass 1.0, Paper 0.1, Fire 0.2, Dust 0.33), and the ring scar
  is SKIPPED.
- **Clear** `Scar_ClearEntriesByEntity @0x5ccec0`: zeroes the owner's
  shared-ring slots and memsets its entity ring, then
  `Scar_FreeProjectedDecals`; called from `Entity_Destroy @0x43e8e4` (24.3)
  AND `Entity_AttachToVehicle @0x43c155` (the boarding passenger's scars).

**Port status (2026-08-21, the wire-up round).** PORTED: `world::ScarCache`
(`engine/runtime/world/impact_scar.{h,cpp}` — 128 entity rings + the shared
world ring, handle + spawn-id lease, never evict, miss-when-full → no scar),
`scar_add_entry` at the round-sim impact site and the knife leaf (the
`scar_type` router + the def gate, the by-pool ring selection, the seven-case
tangent, spin-then-texture on `World::next_prng16`), `clear_entity` at both
death entries and on attach, `renderer::compile_scar_draws`
(`engine/runtime/renderer/scar_draw_list.*` — the witnessed vertex order/UVs,
per-strip batches, the fog-box cull, the owner-visibility gate), and the
device `ScarPresenter` + `scar_present_pass.gd` (entity-ring batches parented
under the owner model's section node, shared-ring batches as a world mesh; the
entity-ring batches are uploaded in the section-local frame the engine stores,
the shared ring in world space). The device state is per strip from the
witnessed mode words (`world::scar_texture_strip_mode_word`,
`renderer::decode_scar_strip_mode`, the draw list's `strip_mode_words`):
`scar_quad.gdshader` carries the scorch state (blend_mix, unshaded, fog,
depth_draw_never, cull_back, no alpha test, `saturate(2 · tex · COLOR)` in
gamma space) and `scar_quad_hole.gdshader` the bhole state (plus the
GREATER/128 discard, depth_draw_always, cull_disabled); Godot's front face is
clockwise like D3D's but in a right-handed frame, so the witnessed order is
already front on the struck side under the packer's world ROTATION fold and
the shared ring keeps it, while the entity-local REFLECTION fold flips it and
those triangles are re-wound — the scorch `cull_back` then culls what retail's
CCW cull culls (the probe's front/behind captures prove it). **Fixed
2026-08-21**: the first cut
drew both strips through one alpha-scissor-0.5 material, which in Godot means the
opaque pass with no blend — every scorch drew as an opaque black blob
(the "pitch black, no texture" report); the scissor was a misread of the
inert 128 latch.
Pinned by ctest `impact_scar`, `projectile_combat`, `renderer_scar_draw_list`,
`destruction`. **The one residual**: the GLASS userpoint leg — the 24-row
table is not witnessed in full, so a userpoint match cannot be detected and
such a hit takes the ring scar (D-ITEM-6 narrowed to the blast/damage tails +
this leg). The `scar_type` word was parsed into `DefAmmoDef` this round (no
Python mirror exists for ammo).

## 25. Appendix: the tracer visual system — the trail emitter pool, style tables, and the ribbon renderer (grill-ida, 2026-07-18)

What a tracer LOOKS like in flight: `RoundData_SpawnRound` allocates a channel in a
dedicated 256-slot trail pool, the projectile tick appends one point per tick, and a
camera-facing ribbon renderer draws each channel through a per-`tracer_type` style
block (color ramp + width curve). The same pool renders rocket/AT4/grenade smoke
trails and the NVG IR laser. All addresses retail `Jointops.exe` (imagebase
0x400000, IDB `Jointops.exe.kong.i64`). Port (same session): the point rings =
`engine/runtime/world` `tracer_trails.{h,cpp}` (`TracerTrailPool`, fed by `RoundSim`), the
styles + ribbons = `fire_present_pass.gd`; pinned by the `npruntime_round_sim`
ctest section 7 and the `fire_present_pass_test.gd` ribbon tests.

### 25.1 The pool — `g_TracerEmitterPool @ 0x2BF5270`

256 channels x 44 B + 256 alloc flags (+0x2C00), reset + style rebuild at every
mission start `[orig: CEffectEmitterPool_ResetAndBuildStyles @ 0x5db3a0, thiscall
from Game_StartMission @ 0x525df7]`. Channel fields (dword idx): [2] point buffer
(16 B verts `{x,y,z fixed, w float}`, alloc tag literally "Tracer"), [3] cap =
the style's table count, [4] count, [5] age, [6] style id, [7] style desc ptr,
[8]/[9] normal/distortion shader handles, [10] kill flag.

- **Alloc** `[orig: CEffectEmitterPool_AllocSlot @ 0x5db7a0]`: first-free scan;
  arg = the `tracer_type` id itself; NULL when all 256 busy. Channel init
  `[orig: CEffectChannel_Init @ 0x5db130]` binds the style block per id (case
  map 25.2) and the shader: stock device slot 6 for the tracer families
  `[orig: CD3DDevice_GetRenderStateByIndex(dev, 6) @ 0x5db1cf]`, pool+0x3004/8
  for smoke/NVG, pool+0x300C for the distortion pass (writers unwitnessed).
- **Append** `[orig: CEffectChannel_AppendPoint @ 0x5db290 — ex kong
  "CNetRateSampler_RecordSample", renamed]`: ring append (full -> drop oldest);
  `w = 1.0 + PRNG_Next16() * 1e-5` on jitter styles (desc+4: smoke/sniper/NVG),
  else exactly 1.0; every append resets the channel age.
- **Tick** `[orig: CEffectEmitterPool_Tick @ 0x5db830, once per 62 Hz frame from
  Game_ProcessMainFrame @ 0x526758]`: per active channel `age < cap -> age++`,
  else pop the oldest point; kill + empty frees the slot. Because appends re-arm
  the age, a live trail holds shape; after the round dies the streak survives a
  cap-length grace then evaporates one point per tick.
- **Round integration** `[orig: Projectile_UpdatePhysics @ 0x4e9d70]`: one append
  per tick at the PRE-move anchor (guided @ 0x4ea04f, ballistic @ 0x4ea97a) —
  the streak head trails the round by a tick; the anchor is
  `Projectile_GetTrailAnchorPos @ 0x4e64e0` (ex kong "Entity_GetRecoilOffset",
  renamed): position, or position + a rotated `{0, ampY sin, ampZ sin}` spiral
  offset when round+0x2AC carries the oscillation block (the rocket corkscrew;
  its writer is unwalked). Death `[orig: Projectile_ReleaseEffects @ 0x4e8280,
  ex sub_4E8280]`: append the final (still pre-move) anchor + kill-request —
  the streak ends a tick short of the wall and the impact flash covers the gap;
  the glow handle (+0x1B4) clears, the loop-sound emitter (+0x1CC) detaches.

### 25.2 The style blocks — 12 `tracer_type` ids, stride 0x830

Layout: +0 additive flag (fog family), +4 jitter/anim flag, +8/+0xC unwitnessed
words (0x10000000/0x8000000 and 0x400/0x4000/0x1000/0x100/0 — no consumer found
in the walked functions), +0x10 count (= ring cap = color-table length), +0x14
base ARGB (the oldest vertex pair), +0x18 ARGB[256] ramp, +0x418 size count,
+0x41C float size[256] (half-widths), +0x81C/+0x820/+0x824 wave params (the
B=1 anim), +0x828 distortion-pass flag. Static `.data` blocks: stdred
`@ 0x8437F0` (also the default for unknown ids `[orig: CEffectChannel_Init
default case @ 0x5db1c8]`), stdgreen `@ 0x844020`, rapidred `@ 0x844850`,
rapidgreen `@ 0x845080`, sniperred `@ 0x8458B0`, snipergreen `@ 0x8460E0`;
runtime-built in `CEffectEmitterPool_ResetAndBuildStyles @ 0x5db3a0`: rocket
`@ 0x2BF4A40`, at4 `@ 0x2BF4210`, grenade `@ 0x2BF39E0`, NVG laser (numeric id
8, no ammo name) `@ 0x2BF31B0`, df1red `@ 0x2BF2980`, df1green `@ 0x2BF2150`
(all renamed `g_TracerStyle_*` this session).

| id | style | count | colors (head -> tail, ARGB) | sizes | flags |
|---|---|---|---|---|---|
| 1 | stdred | 12 | 0, E08080, C08080, A04040, 802020, 601010, 200000, 100000 x3, 080000, 0 | [0.02] | additive |
| 2 | stdgreen | 12 | 0, 80A080, 808880, 407040, 205820, 104010, 001400, 000800 x3, 000400, 0 | [0.02] | additive |
| 3 | rocket | 112 | gray C0C0C0, alpha ((255-2i)^2)>>8 | 32: i*0.0625 | alpha smoke, jitter, distortion |
| 4 | at4 | 112 | same quadratic gray fade | 32: i*0.03125 | alpha smoke, jitter, distortion |
| 5 | grenade | 64 | gray, alpha (192*(255-3i)^3)>>24 | 32: i*0.0078125 | alpha smoke, jitter |
| 6 | rapidred | 6 | 0, E08080, A04040, 200000, 100000, 0 | [0.02] | additive |
| 7 | rapidgreen | 6 | 0, 80A080, 407040, 001400, 000800, 0 | [0.02] | additive |
| 8 | NVG laser | 32 | FF2020, alpha ramps UP 6*i (base C04040) | [0.01] | additive, jitter |
| 9 | sniperred | 20 | FF180000 x4 then dim-red high-alpha fade to 0 | 10: 0.04..0.1 | additive, jitter, distortion |
| 10 | snipergreen | 20 | green mirror of 9 | 10: 0.04..0.1 | additive, jitter, distortion |
| 11 | df1red | 32 | red ramp R=(32-i)*8>>1, G=B=(32-i)*8>>2, [0]=0 | [0.006] | additive |
| 12 | df1green | 32 | green mirror of 11 | [0.006] | additive |

### 25.3 The ribbon renderer — `CEffectChannel_RenderRibbon @ 0x5db8a0`

Main pass per channel `[orig: CEffectEmitterPool_RenderMainPass @ 0x5dcaf0]`,
distortion pass over +0x828-flagged channels only `[orig:
CEffectEmitterPool_RenderDistortionPass @ 0x5dcb40, gated by
CEffectEmitterPool_HasDistortionChannels @ 0x5db7f0 behind a backbuffer-capture
FrameFX leg]`. The normal-pass geometry:

- One +/- right vertex pair per point over points [0..count-2] — the newest
  point steers direction only (the visible head is the second-newest point);
  `right = normalize(cross(dir_to_next, camera - point))`.
- Half-width = `max(size[idx] * point.w, dist * ~1.83e-8 fixed / proj)` — the
  distance term is the minimum-screen-width clamp (`flt_7DC69C = 1.83e-8`
  against the 16.16 camera distance; the projection divisor operand is an open
  item — ported as 0.0012/u).
- Table index = `(count - i) + age - 1`, clamped per table — one expression
  makes the ramp BOTH the along-trail gradient and the post-death fade (the
  whole trail slides down the ramp as age grows). The oldest pair takes the
  style base color (+0x14). Colors are used RAW in the normal pass; the cubic
  alpha boost `255 - ((255-a)^3 >> 16)` belongs to the DISTORTION pass only.
- B=0 styles: 36-B FVF verts (pos + packed color; UV dwords left UNWRITTEN in
  the shared scratch `g_TrailStripVertexScratch @ 0x2BED830` — the stock trail
  shader cannot be sampling a texture), one D3DPT_TRIANGLESTRIP draw
  `[orig: Render_DrawDynamicPrimitive @ 0x56be90]`.
- B=1 styles (smoke/sniper/NVG): 4 verts per point (a 3-quad-wide ribbon),
  indexed triangle-list draw, `GetTickCount`-driven wave animation from the
  style +0x81C/+0x820/+0x824 params (x 0.3 / 0.2 / 4e-4 consts) and animated
  UVs; the distortion pass widens x1.2 (`flt_7D8FB4`).
- Fog: additive styles force the fog COLOR to black
  `[orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @ 0x677740 — a fog-color
  select, NOT a blend set]` so distance fog fades an additive streak out
  instead of tinting it; smoke styles keep scene fog (mode 0). Shader pass id
  0x10520000 via `CGfxShader_ApplyPass @ 0x683190`.
- `Render_DrawTrailOrBeamSegments @ 0x5dcb80` is the immediate-points sibling
  (caller-supplied point array, same style machinery) — the NVG laser draws
  through it with style 8 `[orig: Entity_RenderNVGLaserBeam @ 0x5c6090, ex kong
  "Entity_BuildProjectileTrailRay", renamed: gate = weapon def+8 flag
  0x40000000 + g_NVGActive + not the local player; aim ray clipped by the
  vehicle/infantry proximity raycasts, max 8.0 u, one sample per 0.25 u]`.

### 25.4 The round graphic + glow legs (witness completed)

- Item graphic: `frndlyTrcrID <type_id>` / `foeTrcrID <type_id>` resolve through
  `ItemList_FindIndexByTypeId` (fallback `ItemList_FindIndexByPrimaryName`,
  warnings "couldn't find ammodef frndlyTrcrID"/"... type_id") into ammo +16/+20
  `[orig: AmmoDef_ParseProperty @ 0x40a5f8-0x40a68d]`; the spawn picks the enemy
  id when round team != local team. `@0x4ec79b..0x4ec7b7` stores that id at
  round+0x1C and calls `Entity_InitFromItemDef @ 0x49e550`, binding the item
  def (including throwable motor/think) independently of cadence; a
  non-tracer shot then clears only the round+0x30 visible-model pointer
  `[orig: @ 0x4ec900]`. The tracer item models carry TRACER_SCALE /
  TRACER_WIDTH nodes — entries of the 0x20-stride .3di node-name procedural
  channel table `@ 0x83e428/0x83e448` (WEAP_GUNYAW/HELO_*/VEHICLE_* family);
  the channel evaluator (the 0x41bxxx region) is unwalked.
- Glow: `light_move <radius> <r> <g> <b>` -> ammo +120 (16.16) / +124 (packed
  RGB) `[orig: parse @ 0x40a2d0]`; per-round
  `LightPool_SpawnGlowEffect({x, y, z + radius/2}, radius, color, 1, -1)` ->
  WORD handle round+0x1B4 + render flag 1024 `[orig: @ 0x4ec8a9-0x4ec8f1]`,
  repositioned every tick `[orig: CEffectInstance_SetPositionAndBounds
  @ 0x4eaa9f]`, cleared on death `[orig: Projectile_ReleaseEffects @ 0x4e8308]`.
- Adjacent legs witnessed in the same walk: ammo dword+28 (+112) is a lazily
  spawned attached .ptl emitter (the rocket smoke .ptl beside the pool trail)
  `[orig: @ 0x4e9f58 / @ 0x4ea8ae]`; the whiz-by leg fires
  `Projectile_SpawnTracerScarEffect @ 0x4e5ac0` when the tick's path passes
  within ammo dword+35 (+140) of the listener on X AND Y `[orig: @ 0x4ea998]`.

### 25.5 Port map + verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Tracer decision (cadence/forcetracer/team) | MATCHING (per-slot cadence byte landed 2026-08-12 — D-AI-8a closed; NPC fire keeps the entity stand-in byte, one modeled weapon per NPC under D-AI-5) | `round_sim.cpp` spawn; `npruntime_round_sim` section 6 incl. the per-slot phase pin |
| Trail channel lifecycle (alloc/append/drain/free) | MATCHING | `TracerTrailPool` [orig cites inline]; `npruntime_round_sim` section 7 |
| Spawn-time friendly/enemy style select + NoTracers gate | MATCHING (the rules bit itself = the D-AI-8e net seam, sim field `no_tracers_rule`) | `round_sim.cpp` spawn at the 0x4ec740 cite; ctest section 7 |
| Per-tick pre-move append + death append | MATCHING | `RoundSim::tick`; ctest section 7 |
| Style tables (12 ids: colors/sizes/caps/base/flags) | MATCHING (data transcribed from the six static blocks + the builder) | `fire_present_pass.gd _build_styles`; caps in `tracer_trails.h` |
| Ribbon geometry (pairs, facing, widths, ramp index) | MATCHING (structural; min-width proj divisor approximated 0.0012/u) | `_append_channel_ribbon`; `fire_present_pass_test.gd` |
| Blend/fog (additive fog-black vs alpha smoke) | MATCHING (family-level; Godot `disable_fog` stands in for fog-to-black — D-AI-12c) | materials in `fire_present_pass.gd` |
| B=1 wave anim + 4-wide cross-section + anim UVs | divergent (single-ribbon stand-in; params recorded 25.3) | D-AI-12a |
| Distortion pass (+0x828 channels) | not ported (witnessed structurally) | D-AI-12b |
| Round item graphic + TRACER_SCALE/WIDTH channels | visible TrcrID item model ported (including friendly/enemy fallback and non-tracer suppression); procedural SCALE/WIDTH channels unported | `Simulation::get_throwable_visuals` + `throwable_present_pass.gd`; D-AI-12d |
| light_move glow | ported 2026-08-16 through the D-RLIT-4 light pool (`Simulation::get_round_glow_rows` → `EffectLightDirector.sync_round_glows`) | D-AI-12e (closed leg) |
| NVG laser beam | not ported (witnessed; needs NVG mode) | D-AI-12f |

### 25.6 Divergences

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-AI-12 | Tracer ribbon residuals: (a) jitter/anim styles (smoke 3/4/5, sniper 9/10, NVG 8) draw the same single camera-facing ribbon as the tracer styles — the witnessed 4-verts-per-point 3-quad cross-section, the GetTickCount wave (+0x81C/+0x820/+0x824 x 0.3/0.2/4e-4), and the animated UVs are unported (params recorded 25.3); (b) the distortion pass (+0x828 styles: rocket/at4/sniper — backbuffer-capture shimmer behind `CEffectEmitterPool_RenderDistortionPass @ 0x5dcb40`) is unported; (c) additive fog-to-black (`SetFogAndBlendMode(dev, 2) @ 0x677740`) approximated by `disable_fog` on the Godot material — an additive streak neither fades nor tints with distance until our fog model lands; (d) the visible round item model selected by `frndlyTrcrID`/`foeTrcrID` is ported through `Simulation::get_throwable_visuals` and `throwable_present_pass.gd`, including the retail non-tracer suppression, but its TRACER_SCALE/TRACER_WIDTH procedural node channels (table `@ 0x83e428`, evaluator in the 0x41bxxx region, unwalked) remain unported; (e) the `light_move` per-round glow (round+0x1B4) presented 2026-08-16 through the D-RLIT-4 light pool (mode 1, radius/2 spawn lift, per-tick follow at the raw position, despawn on drop); (f) the NVG laser beam (`Entity_RenderNVGLaserBeam @ 0x5c6090`, style 8) waits on an NVG mode; (g) the min-screen-width projection divisor (the `fdiv` operand feeding `flt_7DC69C = 1.83e-8`) is unresolved — ported as 0.0012 x distance; (h) the per-point W jitter uses a local LCG, not the shared effect PRNG (`PRNG_Next16_B @ 0x6130f0` stream unwitnessed) — presentation-only randomness; (i) the style blocks' +8/+0xC words have no witnessed consumer; (j) the POOL drain runs per logic tick in our sim — retail drains per FRAME (`Game_ProcessMainFrame`); identical at 62 Hz presentation, faster evaporation during catch-up bursts | 25.1-25.4 above | the visible model and core in-flight look are ported; the remaining procedural/dressing residuals each retain their witness |

### 25.7 IDB write-backs (2026-07-18 session, saved)

Renames (anchored): `CEffectChannel_AppendPoint @ 0x5db290` (ex kong
`CNetRateSampler_RecordSample` — provably wrong), `Projectile_GetTrailAnchorPos
@ 0x4e64e0` (ex `Entity_GetRecoilOffset`), `Projectile_ReleaseEffects @ 0x4e8280`
(ex `sub_4E8280`), `CEffectChannel_RequestKill @ 0x5db380` (ex `sub_5DB380`),
`CEffectEmitterPool_Tick @ 0x5db830` (ex `sub_5DB830`),
`CEffectEmitterPool_HasDistortionChannels @ 0x5db7f0` (ex `sub_5DB7F0`),
`CEffectEmitterPool_RenderDistortionPass @ 0x5dcb40` (ex `sub_5DCB40`),
`CEffectEmitterPool_ResetAndBuildStyles @ 0x5db3a0` (ex
`init_default_effect_channel_slots`), `CEffectEmitterPool_RenderMainPass
@ 0x5dcaf0` (ex `render_all_trail_strips`), `CEffectChannel_RenderRibbon
@ 0x5db8a0` (ex `render_trail_strip`), `Entity_RenderNVGLaserBeam @ 0x5c6090`
(ex kong `Entity_BuildProjectileTrailRay` — provably wrong). Data:
`g_TracerEmitterPool @ 0x2BF5270`, the twelve `g_TracerStyle_*` blocks
(25.2 addresses), `g_TracerPool_SmokeTrailShader/NVGLaserShader/
DistortionShader @ 0x2BF8274/78/7C`, `g_TrailStripVertexScratch @ 0x2BED830`.
Entry comments on `@ 0x5db290 / 0x4e64e0 / 0x4e8280 / 0x5db830 / 0x5db3a0 /
0x5db8a0 / 0x4ec740 / 0x4ec8a9 / 0x5c6090 / 0x83e428`. IDB saved.

### 25.8 Open follow-ups

1. The stock shader in device slot 6 (`this[41]`,
   `CD3DDevice_GetRenderStateByIndex(dev, 6)`) — its loader/technique (blend
   states) is unwalked; the pool+0x3004/8/C smoke/NVG/distortion shader writers
   likewise.
2. The `fdiv` projection operand in the min-width clamp (25.3) — resolve and
   replace the 0.0012 approximation.
3. The TRACER_SCALE/TRACER_WIDTH node-channel evaluator (the 0x41bxxx undefined
   region) — define + walk; the item graphic now renders without these
   procedural scale/width channels.
4. The round+0x2AC spiral-offset writer (the rocket corkscrew source).
5. The style blocks' +8/+0xC words — find the consumer (possibly the distortion
   or an unwalked LOD path).

### 25.9 The in-flight round effect's lifecycle — the `move`-row emitter (witnessed + ported 2026-08-21)

The ammo's effects-table tag 1 ("move") emitter the round carries at
`round+0x1CC` (ammo +0x70). The shell already spawned and followed it; what
was missing was WHEN it spawns and when the water plane releases it
`[orig: Projectile_UpdatePhysics @0x4e9d70]`:

- **The `useownmove` (0x2000) leg ONLY** `@0x4e9f50..0x4ea06a` carries the
  water gate and the release (its custom-motor call `@0x4e9f06` precedes the
  decrement): the life decrement (`+0x2AC`, only while > 0) `@0x4e9f41`
  PRECEDES the spawn test; spawn iff `ammo+0x70` authored `@0x4e9f58` `&&
  +0x1CC == 0` `@0x4e9f63` `&& life != 0` `@0x4e9f70` `&& !(z <=
  Env_WaterHeightFixed && ammoFlags & 0x20000000)` `@0x4e9f7d..0x4e9f8e` →
  `CEffectWorld_SpawnEmitterAtPosition` → `+0x1CC` `@0x4ea011`. With a handle
  `@0x4ea019..0x4ea03e`: `z > water` → `CEffect_UpdateEmitterTransform`
  (re-pose); else `ClipWaterFx` → `Entity_ReleaseEffectEmitter @0x5f75d0`
  (UNLATCHED — `+0x1CC = 0`, so a round that surfaces again spawns a fresh
  emitter); else re-pose. Both tests read the PRE-move position: the only
  `Position.Z` store in the function is `@0x4eaa45`, after them. The ammo
  flag table `@0x813500`: `ClipWaterFx = 0x20000000` `@0x8135b8` (`ClipWater
  = 0x1000000` `@0x8135b0`, `noage = 0x4000`).
- **The ballistic leg has NO water term**: its spawn test `@0x4ea8ae..
  0x4ea8d3` is `ammo+0x70 authored && +0x1CC == 0 && life != 0` with the
  life read BEFORE that leg's own decrement `@0x4eaa7f` (the opposite order
  from the `useownmove` leg), and its handle branch `@0x4ea963` only
  re-poses — a ballistic round under the water plane keeps its emitter until
  `Projectile_ReleaseEffects` lets it go. (The tidy's "the thrown leg runs the
  same tests" was the mislabeled `useownmove` block itself.)
- Death detaches rather than destroys `[orig: Projectile_ReleaseEffects
  @0x4e8280 -> Entity_ReleaseEffectEmitter @0x4e82b1..0x4e82bf]` — stop
  emission and let the live particles drain (the live
  `throwable_present_pass.gd` leg). "A zero max_age never times out" was
  REFUTED in the tidy: the head `@0x4e9da7..0x4e9dae` retires `+0x2AC <= 0` on
  the first tick; "never times out" is the `noage` flag.
- **Port**: `round_sim.cpp` evaluates each leg's own predicates each flight
  tick (the water term on the `useownmove` leg only) into
  `LiveRound::move_effect_live` (`round_move_effect.h` keeps the consumed
  predicates; `ammo_table.h` names `kAmmoFlagClipWater/ClipWaterFx`); the rows
  carry the bool and `throwable_present_pass.gd` spawns only when live with
  no handle, retires and forgets the handle when it drops, respawns fresh
  when it returns. Pinned by ctest `round_move_effect`, `projectile_combat`
  and GUT `throwable_present_pass_test`.

## 26. Appendix: allegiance, damage response, and mounted-weapon parity (grill-ida, 2026-07-20)

This pass resolves the mission-playability reports that ordinary AI attacked allies,
actual hits did not wake NPCs reliably, an organic attached to a UseGun emplacement never
fired, and a mounted corpse never entered its death animation. It also establishes a retail
boundary that is easy to misread: a projectile passing near an NPC has presentation/listener
side effects only; retail does not notify the AI. Implementing suppression for a near miss
would therefore diverge from JO:CA. All addresses below are retail `Jointops.exe`, imagebase
0x400000, IDB `Jointops.exe.kong.i64`.

### 26.1 Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| BMS team + Blind/Guarding/Berserk/Coward promotion | **MATCHING** (behavioral proof) | exact spawn stores in §26.2; `mission_promote` and `ai` ctests |
| ordinary same-team rejection + Berserk exception | **MATCHING** (behavioral proof) | `Entity_FindTargets` witness in §26.3; `ai` ctest |
| actual-hit alert/reaction and self-hit exclusion | **MATCHING** (behavioral proof) | both retail callbacks in §26.4; `ai` and `player_spawn` ctests |
| projectile near-miss behavior | **MATCHING** (read-only grill) | listener-only tail in §26.4; deliberately no AI notification |
| mounted aim, request gates, action-FSM fire, muzzle, and shooter ownership | **MATCHING** (behavioral proof) | §26.5-§26.6; `ai` and `npruntime_weapon_table` ctests |
| late-spawn player body-ADM binding + configured UseGun pose | **MATCHING** (behavioral proof) | §26.5b; asset-backed `simulation_test.gd` |
| live UseGun root position | **MATCHING** relationship/root-position core | §23.5/§26.5a; asset-gated 00TRc E50triB GUT; joiner authoritative relationship confirmation landed; generic seats and full matrix basis remain open |
| mounted collision cadence + model-force suppression | **MATCHING** core | §26.7; `collision` and `ai` ctests; D-COL-9 narrowed |
| mounted death detach + directional death animation | **MATCHING** (behavioral proof) | §26.7 and §19; `ai` ctest |
| automatic ADM-derived action duration in the runtime table builder | **MATCHING** | §26.8; D-WPN-26 fixed; `npruntime_weapon_table` |

### 26.2 Spawn allegiance and mission combat flags

`Entity_SpawnFromBMSRecord` copies BMS byte `+73` to entity team byte `+354`; this is
the authoritative team seed used by every later target filter
[orig: Entity_SpawnFromBMSRecord @ 0x40eba9-0x40ebad]. The same spawn path maps the
mission attributes that previously disappeared during promotion:

| BMS attribute | Retail destination | Witness |
|---|---|---|
| Blind `0x1` | `AiSlot[1] |= 0x1` | `[orig: Entity_SpawnFromBMSRecord @ 0x40ed92-0x40ed9b]` |
| Guarding `0x2` | entity `Flags |= 0x40` | `[orig: Entity_SpawnFromBMSRecord @ 0x40ed9f-0x40eda5]` |
| Berserk `0x800` | `AiSlot[1] |= 0x200` | `[orig: Entity_SpawnFromBMSRecord @ 0x40eddd-0x40edea]` |
| Coward `0x10000` | `AiSlot[1] |= 0x8` | `[orig: Entity_SpawnFromBMSRecord @ 0x40ee1a-0x40ee26]` |

The port now writes those exact bits. `Guarding` shares the runtime entity flag used by
the generic carried collision-force gate; UseGun itself deliberately does not set that
flag and instead takes the live-modeled-parent suppression leg in §26.7. Blind/Coward are
preserved in their retail slot for downstream AI legs (this pass does not claim a new
consumer), while Berserk is live in target selection.

### 26.3 Friendly filtering and the intentional Berserk exception

For an ordinary scanner, a candidate on the same nonzero team is rejected. The retail
exception is deliberate: a teamless or same-team candidate remains eligible when either
the scanner or the candidate carries `AiSlot[1] & 0x200`
[orig: Entity_FindTargets @ 0x53a7ea-0x53a824]. That bit is the Berserk spawn mapping in
§26.2, not an unrelated host-side `see_all` switch. The infantry feed now reads this slot
bit directly, so a normal allied pair never engages or fires while authored Berserk actors
retain retail's attack-anyone behavior.

### 26.4 Damage wakes AI; near misses do not

An actual hit runs the organic damage callback. For a non-player victim
(`!(Flags & 0x100)`), it writes AI move/alert byte `+136 = 2` and raises the command group
to red [orig: Entity_HandleDamageTrigger @ 0x407310; alert block @ 0x4073c8-0x4073ea].
The separate receive callback returns immediately for self-damage at `0x4af859`; otherwise
it sets `wasHit`, adds 10 to the reaction timer when the old value is below 25, and records
the attacker [orig: Entity_OnDamageReceived @ 0x4af800]. The port mirrors both layers:
player entities carry the `Flags & 0x100` classifier, but a non-self hit still records the
reaction source; self-damage does not.

By contrast, the close-pass branch in the projectile update only submits listener and
presentation work. It never mutates the candidate's AI slot, group alert, target, or last
attacker [orig: Projectile_UpdatePhysics @ 0x4e9d70, near-miss tail @ 0x4ea99a-0x4ea9f2].
The fidelity fix is therefore the actual-hit chain above, not an invented suppression
event for shots that miss.

### 26.5 Mounted live gate, slot ownership, and aim chase

The mounted live branch requires both a parent and positive health
[orig: Entity_UpdateInfantryAI @ 0x4b996f-0x4b9983]. Attaching to UseGun swaps the
gunner's equipped-slot pointer to the parent's embedded slot at `parent+0x2B4`, assigns the
gunner as its owner, and records parent slot 3
[orig: Entity_AttachToUseGunSlot @ 0x546b80; slot swap @ 0x546c42]. OpenNova represents
that same effective state as a parent-owned primary slot plus an explicit gunner owner.
The early null-slot rejection is narrower than a generic mount gate: it applies only to
an out-of-session player (`!is_in_session && Flags & 0x100 && !EquippedSlot` at
`0x546c07`). Forced/scripted attaches and NAPI session authority bypass it, while an
unarmed offline player remains free to enter ordinary `sitex`/`ctrlx`/`drvrx` seats.
Attach saves the prior equipped ADM immediately and equips the parent slot. UseGun clears
the transient `0xA000` pair but, unlike a generic vehicle-slot attach, does not set
`Flags & 0x40` [orig: Entity_AttachToUseGunSlot @ 0x546c56-0x546c7c;
Entity_AttachToVehicleSlot @ 0x494752]. Dismount and the death-detach edge clear parent
ownership; retail restores the saved slot for a player-class occupant, then clears the
equipped slot for a non-player NPC [orig: Entity_DetachFromVehicle @ 0x435671-0x4356aa].
The persistent parent slot continues to own its own ammo.

Retail first initializes the mount base with the request-time yaw snap described in §23.1. The port
previously updated only `Entity.yaw` from the posed seat on the following tick; its stale
`AiEntity.heading` and local `target_heading` then restored the pre-attach look, immediately feeding a
false EWEAP yaw phase and the configured emplaced-body counter-lean. The shared attach helper now
synchronizes all three core yaw representations before the relationship goes live. A focused
Simulation red test then exposed the Godot side's fourth representation: even with the core fixed,
`player_input_.look_heading` still reapplied the stale look at the top of the next step. The simulation
now mirrors the snapped `target_heading` into that input latch immediately on mount and again after
local/host logic ticks. The joiner path queues C2S 0x26/0x27, waits for the requester-local S2C 0x0A
carrier/bone relationship, then runs the same synchronization without local prediction. Pitch is not
mirrored because the retail request snap is yaw-only. This request-time snap and the matching live
UseGun root-position feedback are separate stages. Generic-seat follow and the full UseGun matrix
basis remain open (§9.2.5).

After that initialization, mounted look remains live rather than snapping directly to the target. Yaw chases by
`((desired-look)+2)>>2`, clamped to ±`0x02000000` per tick
[orig: Entity_UpdateInfantryAI @ 0x4bef57-0x4bef84]; pitch uses
`(delta+4)>>3` [orig: Entity_UpdateInfantryAI @ 0x4bef87-0x4bef97]. Look yaw is then
clamped to ±`0x40000000` from the mount base except for mount configurations
3/4/5/7 [orig: Entity_UpdateInfantryAI @ 0x4bef9a-0x4beff0]. The port applies and
captures the attachment base, restores the saved independent live look, then performs
this chase/clamp against that base.

### 26.5a EWEAP model articulation (grill-ida, 2026-07-21)

The attached organic owns live aim, but the parent owns the embedded weapon model.
Retail bridges those two records in Entity_UpdateTransformAndTurret @ 0x440ca0.
For a live occupant it publishes the wrapped high words of the parent-minus-occupant
BAM angles: occupant Yaw = parent Yaw - turretYaw
[@ 0x441251-0x441263] and occupant Pitch = parent Pitch - turretPitch
[@ 0x441298-0x4412b3]. The resulting uint16 values are written into the model
animation state as yaw slots 118/124 [@ 0x441007/0x44100d] and pitch slots
119/125 [@ 0x44101a/0x441020]. Negative angles therefore wrap through 65535;
they are not signed-degree values and must not be clamped.

The semantic CTRL names are EWEAP_GUNYAW @ 0x83e3c8 and
EWEAP_GUNPITCH @ 0x83e3e8. They are not PLAYPARTANIM channels. The global
32-byte name table begins at LOD_FRAC @ 0x83dce8, making these name ordinals
55/56, while each model has its own CTRL order. The checked-in B50Cal model
demonstrates the failure mode: HEAT_GLOW is first, followed by yaw and pitch.
Its PANM drives the upper mount and barrel from EWEAP_GUNYAW and the barrel
from EWEAP_GUNPITCH; the authored start/end angles own direction (B50Cal
authors pitch 360 to 0, while M1trret authors the forward mapping).

D-WPN-27 records the former divergence: OpenNova supplied only two
model-order PLAYPARTANIM phases, so B50Cal received HEAT_GLOW plus yaw at
zero and could never receive pitch. The fixed path derives one typed semantic
pair from the validated parent primary-weapon owner and applies the exact
names after generic channels to authoritative collision, placed and wire
presentation, and every first-person weapon part. Dismount/death clears only
those two owned names. The client path joins the decoded carrier, mount bone,
and current heading without extending the retail wire. A player compact already
carries live entity Pitch; because the port splits Entity from AiEntity, the
world-to-wire player lift restores that live AiEntity value before witnessed
compact-byte rounding. An infantry compact instead carries the desired aim-pitch
target. ClientReplicaPipeline reconstructs the mounted NPC's live Pitch once per decoded
frame with retail's wrapped one-eighth chase before deriving the semantic phase.

Those same EWEAP controls also pose the parent PANM consumed by the mounted carry.

**HEAT_GLOW (writer audit corrected 2026-07-29).** B50Cal's leading CTRL entry is live:
`HEAT_GLOW` is global ordinal **54** in the 96-entry table (`aLodFrac @ 0x83dce8`, resolved by
`CtrlName_ToOrdinal @ 0x57b290`, stored per model CtrlReg by the loader
`[orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640; ordinal store @ 0x5B46E6]`), directly ahead of
`EWEAP_GUNYAW` (55) and `EWEAP_GUNPITCH` (56). The checked-in `B50Cal.3di` CTRL chunk still carries
exactly those three in local order, but the loader remaps them to global ordinals.

Retail has dedicated heat writers outside the generic ACTION animator. The world writer is
`HUD_CacheWeaponSlotInfo @0x440930`, whose sole caller is the valid-bone branch of
`Entity_AttachToBoneAndUpdateTransform @0x546518`. It receives the parent carrier, validates the
live UseGun/Gunner child relationship, and publishes that child's weapon heat to the carrier model's
`HEAT_GLOW` slot at `0x440969` and `0x440991`; it is not a blanket world-render callback. The
first-person viewmodel path writes the same register at `0x4DEEC2..0x4DEEF5`
`[orig: Player_RenderFirstPersonViewModel @0x4DED60]`. It remains true that no shipped
`weapon.def` row authors `ctrlreg`, but that corpus fact says nothing about these hard-coded writers.

OpenNova's first-person writer exposes a separate `heat_glow` value clamped to `[0,0x10000]` and
publishes the current `HEAT_GLOW` value with a writer identity used only to reject stale teardown;
there is no value rollback stack. The
authority/SP/listen world writer reconstructs the witnessed attachment predicate, publishes cold
zero, and caps the hot leg at `0xFFFF`; presentation and collision consume the same scoped carrier
value. Wire-direct snapshots carry that result too. Compact joiner rows do not contain enough
attachment/heat state to reconstruct it, so remote joiner heat remains unavailable. The particle
emitter and compact-joiner reconstruction are the two remaining D-WPN-28 residuals.
After yaw/pitch update the port transforms the authored UseGun point through its owning
live part and feeds that world position to the occupant root, matching
`Entity_AttachToBoneAndUpdateTransform @ 0x5463d0` and its player/AI callers at
`0x4b63c7` / `0x4bec23`. Retail additionally consumes the resulting full matrix
orientation; this port status is deliberately narrower and claims root position only.
The asset-gated 00TRc E50triB regression supplies the articulated-part witness.

### 26.5b Late-spawn body-ADM registration (2026-07-21)

The mounted state is selected against the occupant's own body AnimMap. Retail creates both body
channels from that entity's graphic-definition ADM in `AnimMap_RegisterEntity @ 0x40bb60`;
`AnimMap_UpdateEntity @ 0x40b5f0` subsequently indexes that per-entity table. A US01 player on B50
`phrase_set 4` therefore selects state 71, `anim_emplaced_5`.

OpenNova had reduced ADM resolution to one mission-load sweep. Joiner-local and host-admitted remote
players appended afterward retained default `adm_id 0`, so the configured-state availability gate
selected state 67, generic `anim_emplaced`, even though the entity origin remained on the authored
UseGun point. D-INF-22 closes that ordering divergence: `Simulation` retains the resource root and
item database, resolves host admissions after connection spawning but before `Server_TickUpdate`, and
resolves direct local/joiner spawn paths before their first body update. Rebuilding the animation
registry rewinds the resolver high-water and repopulates all live ids; Play→Stop restore independently
rewinds the AI array and resolver mark before re-resolving the baseline, including the equal-count case.
This mounted-selector fallback happened before playback and is distinct from §14.8.1's
primary-body missing-key behavior, which now resolves to retail's RESET binding. The
secondary weapon channel's missing-key behavior remains separate.

### 26.6 Fire request and global action-FSM phase

Let `S = current_tick + 36*net_id`. A mounted gunner considers a fire request only when it
has a live parent, target, and equipped slot; `(S & 3) == 0`; the target passes
`((target.x-target.y+S) & 0x40) == 0`; `dz` is halved before distance; distance is strictly
below `AiSlot[15]`; bearing error is below `0x0AAAAAA0` (about 15 degrees); and the slot's
current action is IDLE. It writes only next action FIRE
[orig: Entity_UpdateInfantryAI @ 0x4bf4b3-0x4bf59e]. Existing next action, phase,
counter, heat, and ammo do not participate in this AI-side request gate.

The infantry update does not spawn a round itself. Retail finishes all entity updates, then
pumps every weapon action slot [orig: Entity_UpdateAllEntities @ 0x52674b;
WeaponAction_ProcessAllEntities @ 0x526786]. The FIRE action invokes the shared weapon path,
attributes the shooter to the organic owner, obtains origin from the mount/userpoint, and
consumes the parent's embedded ammo; a clip size of -1 remains infinite. OpenNova now keeps
that phase boundary: mounted requests queue during AI update, the parent slot is pumped once
after world systems and before round simulation, and the resulting authoritative round uses
the gunner net id with the mount's posed muzzle. An occupied pool-1 parent is not pumped a
second time.

### 26.7 Mounted collision and death

After mounted combat/animation work, the live branch runs the movement resolver when
`(S & 7) == 0` and exits without the ordinary mover tail
[orig: Entity_UpdateInfantryAI @ 0x4bf5a5-0x4bf5c6]. The resolver does not omit the
parent from its candidates. Instead, `savedPosY` suppresses accumulated model push for a
source with a live modeled parent or `Flags & 0x40`, while contact classification and
callbacks still run [orig: movement collision resolver @ 0x4b2be0-0x4b2d3f; force gates
@ 0x4b3045-0x4b30af and @ 0x4b3658-0x4b36b9]. This core is now ported; D-COL-9 retains
only its specialized step-up/auxiliary tails. In particular, a UseGun occupant reaches
this suppression through its live modeled parent, not through the generic `0x40` flag.

When health crosses to dead, the live mounted gate stops applying. The infantry death edge
detaches first, then runs the same generic death pipeline as an unmounted organic
[orig: Entity_UpdateInfantryAI @ 0x4b9c57-0x4b9d52]. The directional state staged by the
damage callback in `+0x2C0` is consumed into current animation `+0x2BC`, the mount link is
cleared, and the non-looping clip holds its final corpse pose. OpenNova now follows that
order, so a gunner visibly leaves the emplacement pose and plays its selected death clip.

### 26.8 Remaining divergence and regression map

D-WPN-6 is narrowed, not closed. Mounted parent slots now participate in the global
post-entity action phase, but other non-local pool-0 equipped slots and eligible unmounted
pool-1 weapons still need the general pump coverage described by the original row.

D-WPN-26 is closed (2026-08-15). The production builder now accepts the mounted
`ResourceIndex`, resolves each weapon ADM, and builds a definition-local `AdmClipIndex`
whose variant rings are consumed by the bake callbacks. Each authored automatic start/end
field therefore consumes one matching clip and resolves its 62.5 Hz duration, matching the
two independent retail reads in `Anim_InitActions`; assetless callers retain the explicit
zero-duration fallback. `npruntime_weapon_table_test` pins committed `soldier`
`anim_idle` data at 18 ticks and separately pins that fallback.

Regression coverage is split by contract: `mission_promote` pins all four BMS flag mappings;
`player_spawn` pins the player classifier; `ai` pins ordinary friendly exclusion, the
Berserk exception, actual/self/player damage reactions, exact mounted request/traverse
gates, parent-slot FSM fire with mount muzzle and gunner attribution, and mounted death;
`collision` pins mounted contact processing without push; `mission_mount` and
`vehicle_mount` pin attach/snapshot/detach slot lifecycle (player restore, NPC clear) and
the UseGun-versus-generic `0x40` split. `vehicle_mount` also starts a local occupant with a
deliberately mismatched look and pins request-time synchronization of `Entity.yaw`,
`AiEntity.heading`, and `target_heading`, unchanged pitch, and the first mounted pose tick;
`npruntime_weapon_table` pins the authored action-row bake. Every name is an always-on
CTest target.

GUT `test_late_spawn_player_resolves_own_adm_before_configured_usegun_pose` reproduces the
production load-before-spawn order and requires the US01/B50 combination to select
`anim_emplaced_5`; its rebuild/restore companions pin repopulation after registry invalidation.
The adjacent B50 UseGun assertions prove that fixture's static authored point does not translate
under yaw/pitch, and the host-session regression pins assignment before the first authoritative
remote-player update. The complementary asset-gated
`test_00trc_e50trib_mounted_avatar_root_follows_live_usegun_frame` loads 00TRc's E50triB,
whose UseGun point belongs to articulated part 1, applies nonzero EWEAP yaw, and requires the
sim occupant root to equal the live transformed userpoint within 0.001 and its body basis to match
this asset's live yaw-part basis within 0.51 degrees. It requires
`OPENNOVA_JO_DIR`, so it is production-asset evidence rather than an always-on CI test; it does
not generalize full-basis parity to seats with non-identity rest-part frames or authored yaw offsets.

The EWEAP articulation regression is asset-backed: simulation_test.gd
mounts a deliberately yaw-misaligned local player on fixture B50Cal, requires the attach to establish
the neutral UseGun base before look input, and proves both yaw and pitch then move
the authoritative collision triangles. Its red fixture starts at 160 degrees: before the binding-latch
mirror, the first `sim.step()` restored that stale look and produced a nonzero EWEAP yaw phase and
body-segment twist; green requires snapped yaw, EWEAP_GUNYAW, and maximum segment twist all neutral
before new look input. mission_present_pass_test.gd,
wire_present_pass_test.gd, local_player_presenter_test.gd, and
player_weapon_view_test.gd pin named-control delivery, precedence, and stale
clearing; two_peer_fanout_test pins live player pitch at the existing wire lift,
and loopback_identity_test pins the client-side mounted-infantry pitch chase.

### 26.9 IDB write-backs (2026-07-20, saved)

No functions were renamed. Repeatable comments were appended and dedupe-verified at the
allegiance/damage boundary (`@ 0x40eddd`, `@ 0x4073c8`, `@ 0x4af859`,
`@ 0x4ea99a`), the mounted live/aim boundary (`@ 0x4b996f`, `@ 0x4bef57`,
`@ 0x4bef87`), the fire-request gates (`@ 0x4bf4bb`, `@ 0x4bf4da`,
`@ 0x4bf4e3`, `@ 0x4bf515`, `@ 0x4bf598`, `@ 0x4bf59e`), the embedded-slot swap
and attach-flag split (`@ 0x546c42`, `@ 0x546c5c`, `@ 0x494752`), the conditional
detach slot restore/clear (`@ 0x435671`, `@ 0x43568d`), and the detach-first death
edge (`@ 0x4b9c57`). IDB
`Jointops.exe.kong.i64` was saved.

## 27. Appendix: throwables — grenades, satchels, claymores, mines, the detonator (engine-research, 2026-07-20)

Reimpl: `engine/runtime/world/throwables.{h,cpp}` (motors + placed devices + thinks),
the `RoundSim` spawn dispatches and witnessed throwable `useownmove` leg
(`engine/runtime/world/round_sim.{h,cpp}`),
the PowerThrow charge chain (`godot/src/simulation/simulation.cpp`), the
class/trait feed (`resolve_item_traits`), and `godot/game/world/throwable_present_pass.gd`.
Binary: retail `Jointops.exe` (kong IDB, imagebase 0x400000). ctest `throwables`
+ the claymore rows in `def_parse_ammo`.

### 27.1 Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| PowerThrow charge (press gate, release curve, speed scale, remote C2S byte) | MATCHING | §27.3; ctest `throwables` test_power_throw_charge / test_charge_scales_spawn_speed; `npruntime_client_fire_test` |
| PowerThrow HUDPOWERBAR | MATCHING (including 15 output-pixel text offset) | §27.3; D-THROW-5 |
| Grenade motor (drag/gravity/spin/bounce/water/fuse) | ported core; exact no-water sentinel, lifetime-head/fuse timing, sound-only first-five-bounces presentation, and pool-only item sweep are live; PRNG/parent-Euler residuals remain | §27.4; D-THROW-2/-4; test_grenade_bounce_and_fuse, test_motor_sweep_ignores_non_pool_domains, test_ballistic_expiry_is_silent |
| Satchel/claymore motors + rest conversion | ported core; face-normal stick predicate, full placed pose/item/health carry, and pool-only item sweep are live | §27.5; D-THROW-2/-4; test_satchel_places_device, test_motor_sweep_ignores_non_pool_domains |
| Placed-device think/detonate chain (satchel/claymore/AV mine) | ported core; exact pool-1 order/cadence, wrapped cone angle, and full terrain-plus-sector LOS are live; placed-model collision remains | §27.6; D-THROW-2/-8; device lifecycle/cone tests incl. test_claymore_sector_los_blocks_trigger |
| Owner-death cleanup | matching observable, with generation-checked sim-side owner poll standing in for the death hook | §27.6; test_owner_death_removes_devices |
| items.def class binding (ai_function/move_function) | MATCHING | §27.2; the resolve_item_traits feed |
| ammo.def `kz_pieslice` HALF-angle | MATCHING (fixed this slice — was full-angle) | def_parse_ammo claymore row |
| Host and remote flying item-model presentation | ported; decoded tag-2 events feed a visual-only client `RoundSim`; procedural TRACER_SCALE/WIDTH remains D-AI-12d | §25.4/§27.2; `throwable_present_pass.gd` |
| Landmine items (`lndm`) | witnessed, unported | D-THROW-6 |
| MP round/device presentation (tag 2 + S2C 0x59/0x12) | MATCHING: tag-2 client flight plus reliable placed-device spawn/update/removal are live | D-THROW-7 fixed; net-re §5.36; `npruntime_placed_device_relay` |

This is not a blanket MATCHING classification. The audit closes D-THROW-5 and
D-THROW-9 and fixes the fuse, dry-water, face-normal, cone, and lifecycle bugs
described below. The 2026-07-21 follow-up also restores the grenade descriptor's
sound/particle leg mask so a bounce cannot submit the detonation particle;
D-THROW-2 and D-THROW-6 remain explicit fidelity gaps. The
2026-07-22 follow-up closes D-THROW-3 by routing placed-device cone LOS through
the shared terrain-plus-sector collision query; a type-1 building wall now
blocks the trigger until removed. The 2026-08-12 follow-up closes D-THROW-1:
the shared projectile query gained explicit terrain/water/person domain gates,
and each throwable motor sweep disables all three before nearest-hit arbitration,
leaving exactly the witnessed pool-2 then pool-1 candidate passes.

### 27.2 The class architecture — items.def tags drive everything

A throwable's behavior binds through its **TrcrID item**, not the ammo:
`RoundData_SpawnRound @ 0x4ec0d0` picks `frndlyTrcrID`(+16)/`foeTrcrID`(+20) by
round team vs the local player's team, with a zero foe id falling back to the
friendly item `[orig: @ 0x4ec787..0x4ec79d]`. This selection is independent of
the per-shot tracer-rate decision; only the global NoTracers rule suppresses
it (unless ForceTracer is set) `[orig: @ 0x4ec79d..0x4ec7b7]`. The result is
stored as the round's ItemTypeIndex (+28) and passed to
`Entity_InitFromItemDef @ 0x49e550`, which
copies from the item def: the **per-tick motor** (`move_function` tag against
the physics table `@ 0x82abc8`) into round+452, the **event/think callback**
(`ai_function` tag against `g_EntityClassEventCallbackTable @ 0x813000`, rows
12–17: squib/nade/schl/clym/vmne/lndm at `@ 0x813120..0x8131A8`) into +456,
def Health/Armor, and the init callback (`Entity_InitThrowableSpin_* @ 0x4435A0/
C0/E0/610`) which seeds **1 deg/tick spin** on +164/+168/+172 (the constant
0xB60B60 = 11930464 BAM, not a pointer). JO data (ids = items.def id − 100000):
frag 1883 + flashbang 1875 `nade/nade`, satchel 1891 `schl/schl` (foe id 0,
therefore the friendly 1891 fallback), claymore 1895 `clym/clym`, AT mine 368
`vmne/schl` (mine think, satchel flight). Both teams therefore see the same
satchel model and receive the `schl` class bind. A missing friendly TrcrID or
global NoTracers suppression yields no model and **no motor** — the round
drifts and only the fuse leg still runs.

The sim keeps the selected item id on every bound round, but
`Simulation::get_throwable_visuals` exposes it to
`throwable_present_pass.gd` only when `round.tracer` is true. Thus class binding
remains independent of the per-shot tracer-rate decision while the flying
model follows the separate `@0x4ec900` model-pointer clear. (JO's grenade,
smoke, flashbang, satchel, and claymore ammo author `ForceTracer`, so their
thrown bodies remain visible.) The item's procedural TRACER_SCALE/TRACER_WIDTH
channels remain D-AI-12d.

The ammo `effects_table` **`move`** row is a separate, continuously attached
particle channel; it is neither a tracer ribbon nor an arm/detonation event.
The parser stages canonical tag 1 at `0xA2E974` (effect handle
`0xA2E978`) `[orig: AmmoDef_ParseProperty @ 0x40a46a]`, and
`AmmoDef_InitEffectsTable @ 0x409fc2` copies that handle to AmmoDef+0x70.
Both projectile tick paths load +0x70, lazily spawn one group into the round's
+0x1CC slot, and update that same group from the current round transform
`[orig: @ 0x4e9f58..0x4ea041 / @ 0x4ea8ae..0x4ea970;
CEffect_UpdateEmitterTransform @ 0x5f7410]` — but the `ClipWaterFx`
water-plane release is the `useownmove` leg's alone; the ballistic leg's
handle branch `@ 0x4ea963` only re-poses (the `move` emitter record above). `Projectile_ReleaseEffects
@ 0x4e8280` releases +0x1CC when the round dies. The port mirrors this through
`get_throwable_visuals().move_effect` and an owner-bound group in
`throwable_present_pass.gd`: one spawn, full-transform follow, then immediate
emission stop when the round disappears while emitted particles drain. In
particular, `grenadesm` authors `move Effect_SmokeToss`; that trail is present
before and after its five-second `arm_age`. The arm boundary only submits the
separate `obj` row sound, and neither stops the move group nor detonates the
round. A mounted JO+`revx02` production-data probe resolves its TrcrID item
101875 as `graphic Flsh_3rd`, `ai_function nade`, `move_function nade`, and
the ammo row as `velocity 20`, `max_age 40`; the 40-second fuse owns both
round and move-group retirement. Those two fields are an **expansion
override**: base JO's `ammo.def` authors `max_age 30` / `velocity 30`, and
`revx02`'s `AMMO.DEF` re-authors them to `40` / `20`, so which pair a session
sees is a property of the mount order (`TrcrID 1875` and the five-second
`arm_age` are identical in both). `fixtures/def/ammo.def` stays a byte-exact
copy of the base JO file and its regression pins the base pair; the expansion
pair is witnessed here rather than by editing the retail extract
(docs/adr/0003-no-raw-passthrough-create-from-scratch.md).

Ammo pairs intern by name in `WeaponDef_ResolveAllReferences @ 0x540270`:
`g_ammo_satchel/satchelboom/claymore/claymoreshrapnel/claymorekillzone/AV_Mine/
AV_Minekillzone @ 0x24E7DD8/D4/D0/CC/C8/C0/C4`.

### 27.3 The PowerThrow charge

- Press (fire binding, jumptable 0x4E048B): weapon def+8 Flags sign bit
  0x80000000 = `PowerThrow`; fireable (`WeaponSlot_CanFireInCurrentState`) with
  ammo → `g_fireChargeStartTick @ 0xB76800` = tick; **no fire on press**
  `[orig: @ 0x4e08fd]`.
- Release `[orig: @ 0x4e07e9]`: held < 31 ticks → charge 255 (a tap throws
  FULL power); else `clamp((held−31)/93, 0.1, 1.0) × 255` — 10% at 0.5 s
  rising to 100% at ~2.5 s (flt_7CD390 = 1/93, flt_7C69F4 = 0.1, flt_7CA29C
  = 255). → `WeaponSlot_RequestFire @ 0x53efa0` arg 3 → MountSlot+0x5C.
- `WeaponAction_Fire @ 0x542b10` passes slot+0x5C as
  `Entity_FireWeaponAndSendPacket @ 0x42bd80` arg 6 → fire descriptor +20 →
  the spawner scales launch speed ×charge/256 for charge ∈ [1..254]
  `[orig: @ 0x4ec5bb]`; 0 and 255 = unscaled. The byte rides the wire as the
  round event's `slot_byte` (ring+32, flags|0x80 leg, net-re §5.60);
  `NetPacket_DeserializeRoundEvent` restores it into slot+0x5C
  `[orig: @ 0x42f769/@ 0x42f935]`. The reimpl's C2S 0x06 authority dispatch
  now preserves that byte into `RoundSpawnParams::charge`; tag-2 visual flight
  is followed by the authoritative placed-device 0x59/0x12 lifecycle (D-THROW-7 fixed).
- The HUD windup meter is `HUD_DrawPowerThrowChargeBar @ 0x599830` (renamed
  2026-07-21, ex kong "HUD_DrawWeaponReloadBar" — a misnomer: the function only
  draws this bar). Gates: local player, HUD element enabled, equipped def+8
  sign bit, `g_fireChargeStartTick != 0`, `WeaponSlot_HasAmmoAvailable
  @ 0x541b30`. Fill: held < 31 ticks → **full** (the tap window mirrors the
  charge-255 tap), else `clamp((held−31)/93, 1.0)` `[orig: @ 0x5998ad,
  flt_7CD390 = 1/93]`. Geometry = the hudpos `HUDPOWERBAR` row read as
  **x,y,w,h** (`dword_27237EC..F8`, writer `HUD_ParseHudposToken @ 0x5a1549`;
  JOX authors `20,720,72,11` — unlike the corner-encoded HUDHEALTH/HUDHEAT
  rects), viewport-scaled: a wireframe outline `(x,y)-(x+w,y+h)`, the fill
  `(x+1,y+1)-(x+span, y+h−1)` with `span = (fill_fp16 × w + 0x8000) >> 16`
  `[orig: @ 0x599964]`, and `"%d%"` percent text at `(x, y−15)` via
  `HUD_DrawTextLeft_HalfBright @ 0x5804c0`. The 15-unit text lift is **15
  final output pixels**, not 15 design-space pixels; the port converts that
  delta through `HudLayout.pixel_delta_to_design` before the shared HUD scale.
  Everything uses the flat 0xFF800000 half-red `@ 0x840B1C`. Ported:
  the HUD power-bar element off the sim's windup state (now
`HudFrameCompiler::element_power`; originally `game_hud.gd _draw_power_bar`;
closes D-THROW-5).
- The mount zeroes the charge state: `Player_SwitchToWeaponByHandle @ 0x4e0170`
  clears `g_fireChargeStartTick` before mounting — a new mount can never carry
  a stale windup or charge byte (the port mirrors this in the sim's mount
  install; a release the FSM refuses also drops its charge).

### 27.4 The grenade motor — `Entity_UpdateGrenadePhysics @ 0x443F50` (defined this session)

An environment water height of zero means **no authored water**; the throwable
motor maps that binding-fed sentinel to `INT32_MIN` before all comparisons, rather
than treating every position at/below world z=0 as submerged.

Per tick, Q16 with +0x8000 rounding throughout: drag (+28) on velX/velY then
advance; gravity split −167 above water / −55 submerged (submerged also
disables the bounce this tick); spin integration yaw += +164, pitch += +168
(roll spin only ever decays); horizontal speed < 2048 → treated as 0. Water:
entry (prev above, now below, moving) = splash tag 11 + velXY ×0.25; breach
upward = ×0.45 on all three; submerged = spins ×0.9, velocity ×0.95. Terrain
(`Terrain_SampleHeightBilinear @ 0x6067b0`; skipped for nocollide 0x80):
clamp, spins ×0.8, velXY ×0.8; moving → **bounce**: velZ ×−0.2 (−13107),
bounce_count(+341)++, spin kicks +11930464×(PRNG16%10−5) yaw and
+11930464×(PRNG16%10)−59652320 pitch (`PRNG_Next16 @ 0x6130a0`). Bounce
presentation calls `AmmoDef_ProcessImpactEffect @ 0x40A170` with surface tag
material+4, but its descriptor deliberately selects individual legs: contacts
1–5 use flags `0x80000400` (sound only), then `0x00000400` (neither sound nor
particle) `[orig: @ 0x4447D3..0x444824]`. The helper plays sound from the sign
bit `[orig: @ 0x40A20D..0x40A216]` and submits a particle only for bit
`0x40000000` `[orig: @ 0x40A21E..0x40A240]`; therefore no grenade bounce
spawns a particle. This is materially visible because JO's dirt tag 5 and
detonation obj tag 4 both author `Effect_FragGrndDirt`, while their sounds are
`IMP_GREN_DIRT` and `EXPLO_FRAG_GREN` respectively. At rest → spins 0, roll
0x3FFFFFC0 (lies flat), pitch
0. Landed-on entity (+40) → parent-follow (`Entity_InterpolateFromParentDelta
@ 0x4a8d60`). The swept item raycast covers pools 2/1 ONLY (grenades pass
through persons); face hits reflect via `Physics_ComputeReflectionForce
@ 0x4e4310` — normal = the CFAC face normal (fallback −v̂), v' = tangential +
(1−|v·n|)·n̂, rescaled to **0.35 × |v_in|** (flt_7C6FA8). ARM: elapsed ==
arm_age(+12) → the ammo obj row (tag 4) fires once — the smoke-pour start.
FUSE timing follows the retail head exactly: `Projectile_UpdatePhysics`
tests armed/expired state **before** this tick's age increment and motor; the
ported motor therefore computes retail elapsed age as stored age−1. At exactly
2 ticks remaining, above water → runtime flag 0x1000, and the next lifetime
head queues `WeaponEffect_PushExplosionQueueEntry @ 0x4e83c0` + obj tag 4
**only under 0x1000** (an ordinary expired round vanishes silently);
submerged → immediate detonation with depth-keyed tags (>3 u: 27+25, else 26)
and zeroed age. The useownmove leg `[orig: @ 0x4e9f06]` runs ONLY the motor
(+ proximity list): no stock ray, gravity, drag, or collision; noage (0x4000)
skips aging.

### 27.5 The satchel/claymore motors — `Entity_UpdateSatchelPhysics @ 0x4482A0` / `Entity_UpdateClaymorePhysics @ 0x4472F0` (renamed this session, ex Entity_UpdateShellPhysics / Entity_ProcessFallingPhysics)

One skeleton: drag with |v| < 256 epsilon stop; **inverted gravity** −55 above
water (floaty toss) / −167 submerged (sinks fast); satchel angles −= spins,
claymore yaw −= spin with pitch pinned 0 (stands upright); water entry splash
tag 11, submerged damp (satchel spins ×0.98 vel ×0.94; claymore velXY ×0.5);
terrain = full stop (velocity zeroed, no bounce) + surface effect. An object
hit reflects; the independent retail stick predicate reads the **face normal**:
`nz > 0 && nz > 0.5 × hypot(nx, ny)`
`[orig: schl @ 0x448858..0x4488c4; clym @ 0x447802..0x4478a7]`. Accepted
faces take the `Entity_OrientToSurfaceNormal @ 0x445fa0` pose — BOTH keep
their own yaw (`entity+0x10` passed through); the satchel biases the PITCH
slot 0xC0000040 (~−90°, lies flat) and the claymore 0 (stands upright), roll
bias 0 on both — zero spins, and parent to the hit entity (satchel-on-vehicle
rides it). REST (height over ground ≤ 0xFF, not rising):
+4096 z lift when
unparented, then on the authority `Entity_CloneFromTemplateByType @ 0x4398a0`
(pool by items.def type: Person→0, Vehicle/object→1, Building/Decoration→2)
+ `Entity_ConvertRoundToPlacedEntity @ 0x5455B0` (memcpy of the round's first
0x2B4 bytes — pos/angles/team/owner/ammo/health/callbacks all carry; source
round expires next tick; placed motor +452 = parent-interp or null) + **S2C
0x59** (32 B, mask 0x90 — net-re §5.36) + the per-owner same-type device cap
(`Server_EnforcePlacedDeviceCapByOwner @ 0x5119E0`, ex `sub_5119E0` — walked
2026-08-15, NOT a stat op: the cap keys on the conversion MOTOR, not an item
class — the satchel motor calls with max 3 and the claymore motor with max 4,
each passing the converting round's own item def id, so AT mines (authored
`move_function schl`) ride the satchel motor's max-3 call; a surplus removes
the oldest ARMED device, most negative +684, via
`Server_RemoveEntityAndNotify` = S2C 0x12; ported 2026-08-15, D-THROW-10); a
non-authority round instead clears noage and self-expires in 248 ticks.

The audited host mirror now carries the complete placed pose, selected TrcrID
item, item-def health/armor, owner/team/ammo/think data, and parent link into
the pool-1 entity. Device, owner, and parent references retain registry
generation ids so a recycled handle cannot be mistaken for the original
object. Parent follow runs the exact full-Euler
`Entity_InterpolateFromParentDelta @ 0x4a8d60` port for both the stuck round
and the placed device (D-THROW-4 CLOSED 2026-08-12 — catalog entry below).

The 2026-08-15 network port closes the lifecycle around that mirror. Conversion emits an
exact S2C 0x59 row and removal emits S2C 0x12; the server reliably fans both to in-match
remote peers while excluding its loopback. The client selects base/friendly/enemy TrcrID
variants from the owner/local teams and mission MP attribute `0x8000`, materializes the
pool-1 row at the wire handle, preserves same-item updates as one lifetime, attaches its
structural parent, and retires it on 0x12. `npruntime_placed_device_relay`,
`npruntime_entity_lifecycle_net`, `npruntime_client_runtime`, and
`netsim_client_world_materializer` pin the production path.

### 27.6 The placed-device think chain

`Entity_UpdatePool1Slot @ 0x4b8dd0` (defined this session) per pool-1 entity
per tick: +0x144 removal countdown → `Server_RemoveEntityAndNotify @ 0x50a270`
at 0; the think gates on the PRE-decrement age (+684 read `@ 0x4b8e1b`): at
≤ 0 → `Entity_BuildProximityList` + **deathCallback(entity, 0, 0) every
tick**; the −1 decrement runs unconditionally AFTER the gate (`@ 0x4b8ea0` —
an armed device's age keeps wrapping negative). The clone kept the round's un-aged
+684 (noage), so **the leftover ammo max_age IS the arm delay** (satchel/
claymore 1 s). Ported devices are pool 1 and therefore decrement exactly once
per tick; the pool-2 every-8 / pool-3 every-64 cadence belongs to other item
pools (including the unported `lndm` scope), not these devices
`[orig: Entity_UpdateAllEntities @ 0x4c2100 loops @ 0x4c2288 / @ 0x4c2340]`.
The production world walks this pool-1 device pass before projectiles, so a
newly converted charge does not lose an arm-delay tick and shrapnel spawned by
a device think can fly later in the same frame. Per-tick throwable event rows
are cleared at the start of that authoritative pass.

The thinks (renamed this session):
- `Entity_SatchelThink @ 0x443670` (ex Entity_HandleInfantryDeath): authority,
  Health(+286) ≤ 0 → spawn a fire descriptor at the device position with
  `g_ammo_satchelboom` (owner +368 credited, +442 hit word) →
  the instantkillzone leg queues the blast; above water obj tag 4, submerged
  depth tags 27+25/26; `Server_SendEntityStatePacket(entity, 0)` +
  `Entity_Destroy @ 0x43e810`. Non-authority mirrors on eventType 4.
- `Entity_ClaymoreThink @ 0x4438C0` (ex Entity_HandleDeathExplosion): Health ≤
  0 OR `Entity_FindEnemyInCone @ 0x43cba0` → spawn `claymoreshrapnel` (flag
  0x20000 → the fan) + `claymorekillzone` (instantkillzone) → destroy.
- `Entity_AVMineThink @ 0x443BB0` (ex Entity_HandleVehicleDeathExplosion):
  Health ≤ 0 OR `Entity_FindEnemyVehicleInCone @ 0x43c9f0` → `AV_Minekillzone`.
- `Entity_LandmineThink @ 0x441A40` (ex check_bone_ground_contact): the mission
  minefield item — up to 14 per-mine bones (int16 triplets, spent mask +308);
  persons (pool 0, stance z-adjust: stand −1 u, crouch −0.5 u, prone none via
  MoveOrder +300 bits 0x100/0x200) within the item radius then 0.75 u per
  bone; ground-clamped moving vehicles by bounds; per-bone size byte[784+n]
  1/2 → def ammo +692 (`SMALLLANDMINE`) / 3/4 → +696 (`LARGELANDMINE`) via
  `Weapon_FireProcess @ 0x53f5b0`; all bones spent → destroy. **Unported**
  (D-THROW-6).

The cone tests: eye = device + 0.3 u; candidate gates active/alive/controller,
team ≠ device team OR the `TeamTriggerClaymore` host rule (dword_24D1E34 &
0x8000, admin set `@ 0x405f16`); production host creation copies
`HostConfig.config.mp_attributes & 0x8000` into that sim rule. 3D distance ≤
ammo kz_minradius(+52);
32-bit-wrapped `abs(bearing − Yaw)` ≤ kz_pieslice(+60), including the signed
absolute-value wrap at the BAM seam; LOS `Physics_RaycastSegment
@ 0x415550` result 1..2. The vehicle variant adds def kind(+92) == 1, skips
the device's own parent, and requires |speed(+0x29C)| ≥ 3276 (0.05 u/t —
parked vehicles never trigger). **`kz_pieslice` stores the HALF-angle**:
`AmmoDef_ParseProperty @ 0x40ad33` parses `(atol / 2) × 11930464` (signed
div-2 then the deg→BAM multiply) — our parser stored the full angle until
this slice (fixed; the §18.1/net-re gloss lacked the ÷2). Retail `AV_Mine`
authors NO kz_pieslice → +60 = 0 → the cone gate never passes: **AV-mine
proximity is data-dead in JO:CA** — damage/chain detonation only.

Other consumers: `Projectile_DamagePairEligible @ 0x4e74f0` (ex
Server_IsEntityValidForUpdate; called from `Projectile_ProcessDamageOnTarget`
+ 3× `Projectile_ProcessExplosionQueue`) refuses damage from a placed-device
round while its owner sits unseated; `EventTrigger_AnySatchelInArea @ 0x547160`
is a BMS/WAC event condition (satchel-ammo pool-1 entity inside a zone rec);
`SaveFile_WriteAmmoInstances @ 0x4aae60` persists satchel instances.

### 27.7 The detonator + owner cleanup

`RoundData_SpawnRound` dispatch order `[orig: @ 0x4ec1f3..0x4ec2a5]`:
instantkillzone (0x400) → queue at the spawn point, no round (the kztype 1
knife raycast leaf excepted); **Detonatesatchels (0x20)** →
`Entity_DetonateSatchelsByOwner @ 0x546ed0` (ex
Entity_InvalidateStreamingSlotsByNetId): every pool-1 satchel-ammo entity
owned (+368) by the firer gets **Health = −1** — the next think detonates;
DesignateTarget (0x2000000) → the tracker; claymore (0x20000) →
`Weapon_SpawnProjectileBurst @ 0x4eb900`: spread_count (≤32) plain ballistic
pellets, per pellet the rol4(s+rol11(s))^1 stream draws yaw ∈ base ±
kz_pieslice and pitch ∈ [base, base+kz_pieslice), no tracer/model/fire-event;
then shotgun (0x10000) and the ballistic default.

Owner death/leave: `Server_ProcessPlayerDeath @ 0x5178d8` (and the player-
removal recursion in `Server_RemoveEntityAndNotify @ 0x50a270`, S2C 0x12) →
`Entity_RemovePlacedDevicesByOwner @ 0x546e00`: the owner's satchels/
claymores/AV mines are **removed silently, never detonated**. Our port polls
the owner's health in `ThrowableSim::tick` (transport-free stand-in for the
death hook).

### 27.8 Divergence catalog (D-THROW)

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-THROW-1 | CLOSED 2026-08-12: `ProjectileTrace` carries terrain/water/person walk gates and `motor_item_sweep` disables all three, so excluded domains never enter nearest-hit arbitration | pools 2/1 broad+face walk only (`Projectile_RaycastProximitySlots(2/1) @0x444619..0x444667`) | `collision_test::test_projectile_trace_domain_switches_preserve_farther_pool_hit` walks water → terrain → person → pool-1 as gates close; `throwables::test_motor_sweep_ignores_non_pool_domains` pins the live motor reaching the farther item in all three masking cases |
| D-THROW-2 | world-local PRNG streams (the retail generator shape) | shared globals @ 0x31BFBB0/B8 | bounce kicks / fan angles distribution-faithful, not sequence-identical (the destruction-port precedent) |
| D-THROW-3 | CLOSED 2026-07-22: device LOS uses the shared full terrain-plus-sector query, excluding the device and candidate (`CollisionWorld::raycast_clear`) | `Physics_RaycastSegment @ 0x415550` (terrain + sectors) | `test_claymore_sector_los_blocks_trigger` pins a type-1 building wall blocking the cone and removal exposing the same target |
| D-THROW-4 | CLOSED 2026-08-12: `throwable_stick_pose` is the exact orient port — the normal rotated into the yaw-local frame by the transposed yaw-only Q22 matrix (+0x200000 rounding), pitch = bias + bam(atan2(z,y)) − 0x40000000, roll = bias + bam(atan2(z,x)) − 0x40000000, yaw KEPT (satchel pitch bias 0xC0000040, claymore 0 — the callers pass the entity's own +0x10); `parent_delta_follow` is the exact full-Euler follow shared by the round motors and the placed-device ride, reading the parent's `saved_live_*` channel (netsim `row_deck_ride` ports the inlined org twins of the same math) | `Entity_OrientToSurfaceNormal @ 0x445fa0` (via `Math_BuildFixedPointRotationMatrixYXZ @ 0x615400` yaw-only + `Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0` + `Math_TransformPointFixedPoint22 @ 0x412e90`, atan2 scale dbl_7C57B8 = −2^31/π); `Entity_InterpolateFromParentDelta @ 0x4a8d60` (code calls from all four round motors @ 0x44443b/0x444e8b/0x4475d1/0x44861f; installed as the placed motor +452 @ 0x5455fd) | `throwables::test_stick_pose_exact` pins the four pose legs; `test_parented_device_adopts_parent_pitch` pins the out-of-plane orbit + pitch adoption. Residue: a parent that never stamps `saved_live_*` (statics — faithful zero delta; joiner wire-materialized rows lack the stamp, tracked with device-on-decoded-vehicle under D-THROW-7/D-WPN-8). The double-trig port carries the established ±2 BAM LSB envelope |
| D-THROW-5 | CLOSED 2026-07-21: the charge bar is ported (now `HudFrameCompiler::element_power`, originally `game_hud.gd _draw_power_bar` — the hudpos HUDPOWERBAR x,y,w,h rect, witnessed fill curve + percent text at an exact 15-output-pixel lift + 0xFF800000 half-red) | `HUD_DrawPowerThrowChargeBar @ 0x599830` (ex "HUD_DrawWeaponReloadBar" misnomer, renamed) | §27.3 windup meter entry; throwable_repro_test windup-state pin |
| D-THROW-6 | landmine items (`lndm`) unported | `Entity_LandmineThink @ 0x441A40` witnessed in full | the def wiring for ammo slots +692/+696 (`SMALLLANDMINE`/`LARGELANDMINE` names) is unwitnessed; no lndm items found in JO:CA missions so far |
| D-THROW-7 | CLOSED 2026-08-15: authoritative conversion/removal events encode exact S2C 0x59/0x12 records and fan reliably to in-match remotes while skipping host loopback; joiners validate both, select the base/friendly/enemy item from owner/local teams plus MP attribute 0x8000, preserve same-type updates as one lifetime, attach the structural parent, materialize at the exact pool-1 wire handle, and retire it on 0x12. Recorded residual — the client-side REST GAP: retail keeps the non-authority resting round alive 248 ticks and its 0x59 handler copies the client's own resting round into the placed row, so the device never blinks out; ours removes the visual round at rest and shows nothing until the 0x59 arrives | retail re-simulates tag-2 rounds, then applies S2C 0x59 (net-re §5.36) + 0x12 removal | `nw_ingame_encode`, `netsim_client_world_materializer`, `npruntime_entity_lifecycle_net`, `npruntime_client_runtime`, `npruntime_placed_device_relay`, and `throwables` |
| D-THROW-8 | CLOSED 2026-08-12: the pool-1 projectile walk demand-resolves a late placed item's retained host collision asset before narrow phase; the model uses CFAC, while failure to bind the clone's required collision model raises a fatal invariant rather than substituting sphere geometry | the item clone is initialized from its model before the ordinary pool-1 CFAC walk `[orig: Entity_CloneFromTemplateByType @0x4398A0 -> Entity_InitFromModel @0x40DC30; Projectile_RaycastProximitySlots @0x4E53D4 -> Physics_RaycastAgainstBoneCollision @0x4E4CB0]` | `collision_test::test_late_pool1_item_resolves_cfac_or_raises` pins a sphere-only graze miss, face metadata, allocation-serial-safe packed-slot reuse, and the fatal unresolved-model branch |
| D-THROW-9 | CLOSED: ported devices are pool 1, run before projectiles, and decrement arm delay once per tick | pool-1 age −1/tick; the pool-2/3 −8/−64 cadence is outside the ported-device scope | no divergence for satchel/claymore/AV-mine devices; unported lndm cadence remains under D-THROW-6 |
| D-THROW-10 | CLOSED 2026-08-15 (refined post-#500): after the 0x59 conversion event, `place_from_round` counts live same-owner-generation + same-item devices and caps by the CONVERSION MOTOR — the satchel motor's call at 3, the claymore motor's at 4, each keyed on the converting round's own item def (AT mines author `move_function schl` and ride the satchel motor's max-3 call) — removing the armed row with the most-negative age through `remove_device`; nonnegative countdowns are ineligible and ties remain registry-stable. The armed think gates on the PRE-decrement age (`@ 0x4b8e1b`) with the unconditional wrap decrement after (`@ 0x4b8ea0`) | `Server_EnforcePlacedDeviceCapByOwner @ 0x5119E0` (ex `sub_5119E0`) after each motor's 0x59 send; removal through `Server_RemoveEntityAndNotify` (S2C 0x12) | `throwables` pins both limits, most-negative selection, stable ties, transient unarmed over-cap, age evolution, and owner/item isolation; `npruntime_placed_device_relay` pins one exact remote 0x59 + 0x12 pair and no loopback |

### 27.9 Open follow-ups

1. The +388 fourth exclusion slot on thrown rounds (D-ITEM-11 shares it).
2. The `squib` class pair (`Entity_InitArcMovement @ 0x449810` fn1 /
   `Entity_ProcessProjectileTravel @ 0x448D50` motor) — unwalked.
3. `Weapon_FireProcess @ 0x53f5b0` internals (the AI/scripted fire helper the
   landmine uses) — skimmed only.
4. The AI grenade-throw think (body anims 159–162 exist, §8/§14 tables) — the
   AI never throws in our port yet.

### 27.10 IDB write-backs (2026-07-20 session, saved)

Renames (kong misnomers corrected at anchored confidence + new definitions):
`Entity_UpdateGrenadePhysics @ 0x443F50` (defined; was an undefined blob
behind a stale byte), `Entity_UpdateSatchelPhysics @ 0x4482A0`,
`Entity_UpdateClaymorePhysics @ 0x4472F0`, `Entity_SatchelThink @ 0x443670`,
`Entity_ClaymoreThink @ 0x4438C0`, `Entity_AVMineThink @ 0x443BB0`,
`Entity_LandmineThink @ 0x441A40`, `Entity_FindEnemyVehicleInCone @ 0x43c9f0`,
`Entity_RemovePlacedDevicesByOwner @ 0x546e00`, `Entity_DetonateSatchelsByOwner
@ 0x546ed0`, `EventTrigger_AnySatchelInArea @ 0x547160`,
`Server_RemoveEntityAndNotify @ 0x50a270`, `Entity_UpdatePool1Slot @ 0x4b8dd0`,
`Entity_ConvertRoundToPlacedEntity @ 0x5455b0`,
`WeaponEffect_PushExplosionQueueEntry @ 0x4e83c0`,
`Projectile_DamagePairEligible @ 0x4e74f0`, `Entity_InitThrowableSpin_Nade/
Satchel/Claymore/AVMine @ 0x4435A0/C0/E0/610`, `Entity_OrientToSurfaceNormal
@ 0x445fa0`; data `g_ammo_satchel/satchelboom/claymore/claymoreshrapnel/
claymorekillzone/AV_Mine/AV_Minekillzone @ 0x24E7DD8..C0`. Entry comments on
each + inner-site comments at `@ 0x4e08fd` (press gate), `@ 0x4e07e9` (charge
curve), `@ 0x4ec5bb` (speed scale), `@ 0x4ec234/0x4ec288/0x4ec79b` (spawn
dispatches), `@ 0x4e9f06` (useownmove leg), `@ 0x4c2288` (pool-2 think loop).
IDB saved.

## 28. The entity Flags dword (consolidation, 2026-07-29)

One table for the retail entity Flags dword (`entity+36`; reimpl
`Entity::engine_flags` + the organic low-byte legacy `flags` mirror). This
section consolidates witnesses already scattered through this record (§3, §13,
§15, §16, §17, §23, §24) and backs the `kEntityFlag*` constants in
`engine/runtime/world/entity.h` — no new IDA work. Spawn composition:
BMS `Indestructible(1<<21) -> 0x4000000`, `Reflective(1<<23) -> 0x400`,
`NoShadow(1<<24) -> 0x1000000` `[orig: Entity_SpawnFromBMSRecord @ 0x40e9f0]`;
kind Building `-> 0x20000` `[orig: Entity_InitFromModel @ 0x40e105]`;
items.def hp==0 `-> 0x4000000` `[orig: @ 0x40dc8e]`.

| Bit | Constant | Meaning | Witness |
|---|---|---|---|
| 0x1 | `kEntityFlagCarried` | carried object hidden while attached to its carrier (the flag/carryable pickup family); distinct from 0x40, which marks the CARRIER/mounted body | `[orig: Entity_AttachToVehicle @ 0x43C130]`; the Match flag producer (§15.5 D-COL-8 flag leg) |
| 0x2 | `kEntityFlagDead` | dead (kill writes `Flags \|= 6`) | `[orig: @ 0x43fbf6]`; the SP dead gate reads `entity+36 & 2` (§20) |
| 0x4 | `kEntityFlagHusk` | items/buildings: husk swap (with 0x2 on kill) | `[orig: @ 0x43fbf6]`; §24 |
| 0x4 | `kEntityFlagNVGWorn` | organics: NVG worn — draw gate for the goggle model; same bit, kind-dependent read | `[orig: draw @ 0x4e3b54]`; §13.1 draw 3 |
| 0x8 | `kEntityFlagBinoculars` | binoculars raised (bits 2-4 refresh from the local `g_binocularsRaised` global; peers receive them over the wire) | `[orig: draw @ 0x4e3c04; refresh gate @ 0x4b5d77]`; §13.1 draw 4 |
| 0x10 | `kEntityFlagScopeRaised` | weapon scope raised (`g_weaponScopeActive` refresh) | `[orig: test @ 0x4b5deb]`; §13.2 |
| 0x20 | `kEntityFlagParachute` | parachute deployed (system unmodeled, D-INF-20) | `[orig: repulsion radius leg @ 0x4b3aac]`; §15.4 |
| 0x40 | `kEntityFlagMounted` | carried / vehicle-mounted; the AI guard family also reads it | `[orig: Entity_AttachToVehicleSlot @ 0x494752-0x494775]`; §1, §15, §17, D-COL-9 |
| 0x80 | `kEntityFlagAiClimb` | org1 ladder-CLIMB order mode (named 2026-08-15): the capped sixteenth-step Z chase to +0x304 replacing gravity (floor −16384) — PORTED §30 — plus the eighth-step x/y chase to +0x2FC/+0x300 gated on `attachParent == self` (the AI direct-move mover; rides the AI-order slice with the bit's WRITER) | `[orig: test @ 0x4bf6c1; z chase @ 0x4bf6d2-0x4bf6e5; x/y chase @ 0x4bf651-0x4bf664]`; §30 |
| 0x100 | `kEntityFlagPlayer` | player — the wire Player dispatch class; gates held-weapon draws and the death-event leg | §5.10b (net-re); §13.2; §16.2 |
| 0x400 | `kEntityFlagReflective` | BMS Reflective trait | `[orig: @ 0x40e9f0]` |
| 0x800 | `kEntityFlagVehicleLoadoutZone` | type-11 volume touch — gates vehicle.mnu | `[orig: @ 0x4aeb92, @ 0x49b858]`; §15.4. NOTE: the damage path also writes an entity `Flags \|= 0x800` critical-hit latch (`round_sim.cpp` seat/head branches) — same value, distinct unnamed meaning; that site stays raw |
| 0x2000 | `kEntityFlagInAir` | airborne / swimming | `[orig: grounded selector @ 0x4b78ab]`; §3, §15.3 |
| 0x4000 | `kEntityFlagPriorityTarget` | set on every fire; decays per perception scan (the §16.2 x6 scoring flag) | `[orig: set @ 0x4bf370; clear @ 0x4bbfa4]` |
| 0x8000 | `kEntityFlagDrowning` | deep-water FLOAT latch (the "drowning" family — the death-cause consumer maps it to 175): asymmetric-hysteresis submerge, zeroes the vertical root, gates gravity via 0x108000, and hands z to the per-motor float blocks | `[orig: latch @ 0x4b8363 / @ 0x4bfc48; entry @ 0x4b8020 / @ 0x4bfafe]`; §29.1 |
| 0x20000 | `kEntityFlagBuilding` | kind Building | `[orig: Entity_InitFromModel @ 0x40e105]` |
| 0x100000 | `kEntityFlagLadderContact` | CL/type-4 ladder touch; locks upper-body pose + skips gravity while aligned | `[orig: @ 0x4b3291]`; §14, §15.4 |
| 0x400000 | `kEntityFlagArmoryZone` | type-6 (CA) volume touch — gates weapon.mnu on action 218 | `[orig: @ 0x4aea45, @ 0x49b848]`; §15.4 |
| 0x200000 | (unnamed in reimpl; the netsim flags mirror carries it raw) | fully-submerged/dive latch — org2's water block sets it below the float line − 0x2000 (with the dive splash), clears at the surface clamp; both motors clear it with 0x8000 on the not-submerged exit (`~0x208000`) | `[orig: set @ 0x4b81ef; clear @ 0x4b8176; exits @ 0x4b8373 / @ 0x4bfc5c]`; §29.1 |
| 0x800000 | `kEntityFlagIndoors` | indoors (blink accum bit 2 -> Flags); render + AI retry gates | §4 (render-occlusion-re), §15.4, §17 |
| 0x1000000 | `kEntityFlagNoShadow` | BMS NoShadow trait | `[orig: @ 0x40e9f0]` |
| 0x4000000 | `kEntityFlagIndestructible` | BMS Indestructible / hp==0 item | `[orig: @ 0x40e9f0; @ 0x40dc8e]`; §15.3 force skip |

Known-but-unnamed bits (witnessed IN USE but the meaning is not pinned — the
code keeps raw hex at these sites; do not name without a new witness):

| Bit | Where it appears | Note |
|---|---|---|
| 0x1 | destruction sweeps skip `engine_flags & 0x1` targets; part of the `0x2000001`/`0x43` composites | reads as an "inactive/exempt" family; unpinned |
| 0x10000 | `!(Flags & 0x112002)` comment-only gate (§3) | unpinned |
| 0x2000000 | the `0x2000001` skip composite (collision/throwables/LOS) | unpinned |
| 0x8000000 | AI combat candidate skip; `0x8000001` composite | unpinned |
| 0x8 / 0x20 (vehicle context) | `vehicle_motor` writes on the vehicle's own Flags dword from MoveOrder | different, unwitnessed meanings on vehicles — distinct from the infantry binocular/parachute reads |

Composite masks the original uses as units (kept composed-with-static_assert or
raw per the partially-witnessed rule): dismount scrub `~0xA000`
(`Drowning|InAir`), mount scrub-and-set `& 0xFFFF5FBF | 0x40`
(`~(Drowning|InAir|Mounted) | Mounted`) `[orig: @ 0x546c56-0x546c7c;
@ 0x494752-0x494775]`; repulsion exemption `0x43` and skip composites
`0x2000001`/`0x8000001` stay raw (constituent bits unpinned).

## 29. The org water/float channel and the deck-ride (D-NET-196 replica tails, 2026-08-06)

The two mover blocks the replica-infantry tails needed, witnessed end to end
in both org movers. Ported for DECODED rows in
`engine/net/netsim/client_replica_pipeline.cpp` (`row_water_channel`,
`row_deck_ride`, the root suppressions and gravity gate in
`row_root_motion_tick`) with the resolver-side flags channel in
`engine/runtime/world/collision_resolve.cpp` (`resolve_replica` `entity_flags`).
The LOCAL infantry motor's water block remains unported (D-INF-3) — this
section is its witness when that slice runs.

### 29.1 The water/float channel (`Flags 0x8000` + the dive bit `0x200000`)

Entry, both motors, at the mover tail after the settle: submerged iff
`pos.z + (Flags & 0x8000 ? 0 : 0xA000) < Env_WaterHeightFixed`, and
`Flags & 0x100000` (CL/platform contact) exempts entirely — an asymmetric
hysteresis: enter head-under (0.625 u), leave only at the surface
`[orig: org2 @ 0x4b8020-0x4b804d; org1 @ 0x4bfae2-0x4bfb10]`. The
not-submerged exit clears BOTH bits: `Flags &= ~0x208000`
`[orig: org2 @ 0x4b8373; org1 @ 0x4bfc5c]`. Every submerged tick latches
`Flags = (Flags & ~0x2000) | 0x8000` — swimming overrides airborne — with a
splash effect + type-0x34 overlay broadcast on the not-yet-latched edge
(effect selected by the was-swim 0x2000 bit)
`[orig: org2 @ 0x4b8182/0x4b8363; org1 @ 0x4bfb87/0x4bfc48]`.

- **org2 (player body) — the buoyant-rise form** `[orig: @ 0x4b8053-0x4b8373]`:
  base = `min(capsule_bottom, 0) - 0x4C9`, plus a LOCAL-player-only surface
  bob `-sin(((y+x)>>12 + 4·tick)·(1/256)·3.1)·(-1224)` — a remote row gets the
  flat `-0x4C9` `[orig: the local/else split @ 0x4b8063-0x4b80a5]`. A
  look-pitch dive/rise term `clamp(((|base/2|+0x1000)·(Pitch>>14)+0x8000)>>16,
  ±0x800)` applies only for `MoveOrder bit3 && (local || authority)` — zero
  for a replica row `[orig: @ 0x4b80aa-0x4b8113]`. Per tick:
  `pos.z += |base>>4| + pitch_term + 0x70` (the buoyant rise), the velocity
  triplet drags `v -= (v+16)>>5` `[orig: @ 0x4b8124-0x4b8163]`, and the
  surface line `surf = water + base/2 - eyeHeight(+0x74)/2` clamps from
  above (clear `0x200000`, `pos.z = surf`); deeper than `surf - 0x2000` with
  `0x200000` clear sets the dive bit + the dive splash once
  `[orig: @ 0x4b8169-0x4b81f5]`. The local-player scope auto-toggles ride the
  same block (`@ 0x4b8304-0x4b8360`, presentation-only).
- **org1 (NPC) — the snap form** `[orig: @ 0x4bfb16-0x4bfc86]`: float target
  `water + bob - height(+0x74)/2 - 0x4C9 + min(capsule_bottom, 0)` with the
  bob for EVERY row (no local gate), then the tail rewrites
  `pos.z = z_saved + (target - z_saved + 2) >> 2` where `z_saved` is the
  POST-INTEGRATE z captured before gravity `[orig: the save @ 0x4bf6ba; the
  quarter-step tail through `+0xAC` @ 0x4bfc65-0x4bfc86]` — gravity's and the
  resolver's z contributions are DISCARDED while afloat. org1 never sets the
  dive bit (clears it on exit only).
- **Motion couplings**: `0x8000` suppresses the vertical root channel and
  `0x100000` the horizontal pair — BOTH motors store true ZEROS (CORRECTED
  2026-08-15: the earlier "org2 writes the literal 1" reading mistook the
  0x8000 TEST-MASK load `mov ebp, 8000h @ 0x4b7979` for the stored operand;
  the stores use xor-zeroed scratch — org2 `xor edi, edi @ 0x4b797e/@ 0x4b79b7`
  with stores `@ 0x4b7ab5/@ 0x4b7ac0-0x4b7ac4`, org1 `xor ebx, ebx @ 0x4bf600`
  with stores `@ 0x4bf671/@ 0x4bf67c-0x4bf680`; the netsim replica port's
  Player-row `1` was a misport of this and is fixed); gravity skips while
  `Flags & 0x108000` `[orig: org2 @ 0x4b7ac8; org1 @ 0x4bf7b8]` — the
  position-add itself is unconditional (org2 folds vel into the one root
  store `@ 0x4b7cef`).
- **Port notes**: the replica port carries the org2 REMOTE arm (flat base,
  no pitch term) and the org1 snap form verbatim; the full velocity-triplet
  drags run against the row velocity pair + `rm_vel_z`; and the `+0x74` eye
  vertical is derived per tick exactly as retail's non-local arm derives it —
  `min(capsule_top - capsule_bottom, 0xD000)`, floored at `0x2000` (the lean
  tilt is the lean channel's term, zero for an unleaning row)
  `[orig: @ 0x4b6984-0x4b6991; floor @ 0x4b68e7]`. The splash/overlay edges
  ride the cross-cutting sound/FX slice (open for local rows too — §22.5).
  `Env` source: the mission water plane (`World::EnvState.water_z`, 0 = no
  water = channel off with a self-healing `~0x208000` clear).
  **2026-08-24 — the LOCAL motors are ported too** (world/infantry_water.cpp):
  org1 verbatim; org2 with the LOCAL bob arm (`-1225 - sin·(-1224) + min(cb,0)`
  — the negated amplitude is its own global `dbl_7C9BC8`) and the look-pitch
  dive/rise term; the swim selection 36/37/38/39/40
  REPLACES the land commit as retail's overwrite semantics require of our
  crossfade channel (one commit per selection pass — a double stamp wedges the
  blend and lets the stale source's root motion self-propel the swimmer). The
  bob phase pair is the LITERAL `1/256 · 3.1` (exe bytes @0x7C6950/@0x7C9BD0),
  not pi/256 — corrected from the earlier port. Authored swim root velocity:
  C4SwimfG.bad = constant 0.096 u/frame (2.88 u/s); a played retail session
  measures ~2.30 u/s median forward — no swim scale exists at the root fold
  (`@0x4b7cef`, 1x; root zeroes only while airborne 0x2000), so the residual
  is measurement noise or an unwitnessed projection: OPEN, needs a controlled
  straight-line comparison.

### 29.2 The deck-ride (groundEntity pose-follow)

Both org movers make a row FOLLOW its `groundEntity (+0x28)` at mover top —
after the position chase, before root motion — so anything standing on a
moving entity rides it. There is no vehicle-type gate: a static carrier
simply contributes zero deltas.

- **Entry/drop** `[orig: org2 @ 0x4b5288-0x4b52ff; org1 @ 0x4ba45d-0x4ba479]`:
  null groundEntity skips. An UNMOUNTED rider beyond the carrier's
  boundRadius (`[carrier+0]`, 3D distance saturated at
  `flt_7C19E0 = 2147418112.0`) drops the ride AND zeroes groundEntity; a
  mounted (+0x16C) rider skips the radius check.
- **Translation** `[orig: org2 @ 0x4b530b-0x4b535d; org1 @ 0x4ba47f-0x4ba4bc]`:
  `rider.pos += carrier.Position - carrier.savedLivePose (+0x80..)`; the
  attitude deltas read `body* (+0x8C/+0x90/+0x94)` — the carrier's saved
  attitude triple, stamped at its own mover start.
- **Rotate-about-carrier** (only when an attitude delta exists)
  `[orig: org2 @ 0x4b537b-0x4b5653; the standalone twin
  Entity_InterpolateFromParentDelta @ 0x4a8dc0 (the physicsless-child move
  function — same math, no capsule bias)]`: `rel = ((rider - carrier) << 8)
  + 127` per axis, org2 biasing rel_z by `-capsule_bottom/2` (restored on
  writeback `@ 0x4b5649`); un-rotate by the SAVED attitude with
  2^-22-scaled NEGATED sines (`dbl_7C57B0 = -4194304.0` — the inverse
  rotation), re-rotate by the CURRENT attitude with positive sines
  (yaw→pitch→roll forward, roll→pitch→yaw back), Q22 products
  (`imul; shrd 22`), writeback `rider.pos = carrier.pos + rel' >> 8`.
- **Attitude adoption** `[orig: org2 @ 0x4b5656-0x4b5726; org1
  @ 0x4ba7a5-0x4ba88e]`: the pitch/roll deltas rotate by the carrier-vs-rider
  relative yaw (taken BEFORE the yaw add): `pitchΔ' = cos·pitchΔ - sin·rollΔ`,
  `rollΔ' = sin·pitchΔ + cos·rollΔ`. Both motors add yawΔ to Yaw + bodyHeading
  + the four torso/aim chase targets (+0x2D4/+0x2D8/+0x2E4/+0x2E8) and rollΔ'
  to Roll (+0x18); a rider mounted on a `def+0x58 & 0x1000` seat skips the
  Yaw add (bodyHeading still follows); the LOCAL player's camera-yaw
  accumulator (`dword_B75FCC`) follows too. org1 additionally drags its chase
  TARGET (+0x1A8) and the look Pitch (+0x14) `[orig: @ 0x4ba867/@ 0x4ba88e]`;
  org2 adds pitchΔ' to bodyPitch (+0x90) only.
- **Port notes (replica rows)**: the carrier keeps the REAL mover-entry
  savedLivePose — `Entity::saved_live_*`, stamped by `stamp_saved_live_pose`
  at the top of both world vehicle passes via the shared
  `carrier_pose_fixed` reader — and the provider serves live + saved
  together, the witnessed source pair (a never-stamped static reads zero
  delta). Our joiner frame runs vehicle prediction AFTER remote motion, so a
  rider consumes the PREVIOUS frame's carrier delta — retail's own
  rider-ticks-before-carrier ordering case (the mover order is entity-table
  order there). Rows adopt heading (+ the org1 chase target and RENDERED
  look pitch) and roll (rendered — the avatar euler consumes
  `pitch_bam`/`roll_bam`). Open pair, tracked on the ledger row: the org2
  bodyPitch/torso-aim adoption needs the replica body-conform channel (the
  same +0x90 consumer family as the unported replica slope pass), and the
  VEHICLE deck-carrier ride twins (`@ 0x48D6DA-0x48DACD` watercraft /
  `@ 0x4905BC-0x49095B` air — same math, no capsule bias, no radius drop)
  sit behind D-NET-161's vehicle-vs-vehicle contact, without which no ground
  link can form on our side. Pinned by
  `netsim_client_replica_pipeline_contact_resolver` (zero-delta parked
  carrier, translation follow, the 90° rotate-about, heading adoption, the
  radius drop) and the collision_test replica flags legs (the resolve-start
  clear, the InAir full-update discriminant).

### 29.2a The org planar-velocity channel (ported for replica rows 2026-08-06)

The momentum pair (`+0x98/+0x9C`) beside the anim root, decoded from both
movers and ported as `ClientEntityState::rm_vel_xy`:

- **Maintenance** (before the integrate): org2 splits on the airborne bit
  `[orig: @ 0x4b78a8-0x4b79dc]` — in air, an optional MoveOrder-bit3
  air-steer nudge (`angle = look-yaw hi16 · 2π/65536 + dirpad(MoveOrder&7) ·
  π/4` `[orig: dbl_7C9BC0/dbl_7C9BB0]`, force `ftol(cos/sin · −64.0)`
  `[orig: flt_7C9BD8]`, applied TWICE for a parachute straight fall at
  `vel_z <= −14336` with no pad `[orig: @ 0x4b7915-0x4b793d]`), then the
  63/64 damp and the ROOT PAIR ZEROED (airborne motion is momentum-owned)
  `[orig: @ 0x4b7943-0x4b7975]`; grounded — and org1 on every path
  `[orig: @ 0x4bf5cb-0x4bf61f]` — the `(7·v + 4) >> 3` decay with the
  `|v| <= 8` snap to zero.
- **Integrate**: `pos += vel + root`, one store per axis
  `[orig: org2 @ 0x4b7cbf-0x4b7cd2; org1 @ 0x4bf684-0x4bf6a2]`.
- **Feeds**: the org2 ledge edge banks `3·root/4` into the pair and stamps
  anim 31 straight (47 while parachuting; org1 keeps its clip on a plain
  fall) `[orig: carry @ 0x4b7e43-0x4b7e6d; stamp @ 0x4b7e3f-0x4b7e61]`; the
  water block drains the full triplet at `(v+16)>>5`
  `[orig: @ 0x4b8124-0x4b8163]`.
- The org2 slope-slide impulses (`@ 0x4b6f0e-0x4b6fa3`) and the jump-launch
  writes (`@ 0x4b7ecc-0x4b7ed5`) are the slope-pass/jump systems' feeds into
  the same pair — they ride those systems' own port units.

### 29.3 Follow-ups

1. The local infantry motor's water block stays D-INF-3 scope. The ladder
   half of this item LANDED 2026-08-15 (§30): the `Flags 0x80` Z-chase
   gravity variant is ported (its x/y `attachParent==self` leg rides the
   AI-order slice with the bit's writer), and the `@ 0x4b7fa8-0x4b8019` push
   is the grounded BOTTOM dismount (clearance ≤ 0, below the anchor), not a
   top exit — the earlier gloss read it backwards; the natural top-out is
   the resolver EXIT leg's +bodyHeading push.
2. The vehicle-side 0x8000 writers (`Entity_ProcessVehicleSuspension`/the
   family solves) are that family's in-water flag — same bit, vehicle
   context; already ported in the contact solves.
3. The runover/crush kill leg inside the resolver
   (`[orig: @ 0x4b37c2-0x4b39f7]` — def-type-1 vehicle, |v| thresholds
   0x27B0 both sides, team/FF gates, authority-only, death anim cause 2 by
   approach quadrant, killer = carrier occupant +0x170, groundEntity-riders
   exempt) is witnessed for the D-NET-161/00TRg authority arc — not a
   client-subset item.

## 30. The ladder climb state machine — the D-COL-5 port (engine-research + port, 2026-08-15)

The climb motor over the CL/type-4 frame the earlier sessions extracted. The
consumer census is closed: exactly four functions touch the
`g_LadderContact{Pitch,Yaw,X,Y,Z}` globals (`@ 0xB5AB70..80`) —
`Entity_ComputeBoneCollisionForce @ 0x4ae150` (writes them),
`Entity_MovementCollisionResolver @ 0x4b2bd0` (entry/chase/exit),
`Entity_UpdateInfantryPlayerBody @ 0x4b40e0` (org2 dismounts + bottom exit),
`Entity_UpdateInfantryAI @ 0x4b9910` (org1 press + 33/35 select). Reimpl:
`engine/runtime/world/collision_resolve.cpp` (`LadderResolveIO` + the CL block),
`engine/runtime/world/infantry_ladder.cpp` (the climb block + exits + the org1
legs + the view clamp; `infantry.cpp` defers to it),
`engine/runtime/simassets/pose_inputs.h` (the arms lock), and the view
write-back seam in `godot/src/simulation/simulation_player.cpp`
(`sync_local_mounted_input_heading`).

### 30.1 Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Resolver CL legs (entry gate, snap/bump, recontact, chase, exit + pitch restore) | MATCHING | 4 `collision` ctest cases (`test_ladder_entry_gate_snap_and_chase`, `test_ladder_recontact_inflated_and_relatch`, `test_ladder_exit_push_and_pitch_restore`, `test_ladder_class_bit_climber_remote_and_local`); 20+ inline citations. The resolver's player predicate is the class bit at every physics leg `@ 0x4b2cd9 / @ 0x4b2f7c / @ 0x4b3271 / @ 0x4b33aa / @ 0x4b3c78`; only the pitch restore `@ 0x4b3cdc / @ 0x4b3cfe` (and the `g_LocalPlayerLookYaw` / blink mirrors) test the local entity — the remote twin eases its heading and takes the exit push without arming the restore (2026-08-24) |
| org2 climb block (states 32–34, dismounts, jump-off, bottom exit, gravity/root gates, view clamp) | MATCHING | 2 `infantry` ctest cases (`test_player_ladder_climb_cycle`, `test_player_ladder_bottom_exit_and_jump_off`) |
| org1 on-ladder legs (congestion hold, press, 33/35 select, the `0x80` Z chase) | MATCHING (dormant in game until the AI-order writer lands) | `infantry` ctest `test_org1_ladder_hold_press_and_top_select` |
| The arms lock (`Flags 0x100000` → aim overlay bypass) | MATCHING (read-only grill) | `@ 0x4b1cf1` anchors the bit as the LADDER latch; wired via `pose_inputs.h` |

### 30.2 The witnessed machine

**Entry** (`Entity_MovementCollisionResolver @ 0x4b3245-0x4b334b`, per CL
contact in the candidate loop, after the force fold): dead never latches; a
previous latch re-latches unconditionally; a FRESH entry needs the player
class bit 0x100 or the AI order `aiRuntime[1] & 0x400`, and a player must
satisfy `(heightDiff < 0 || |Yaw − frameYaw| < 0x2AAAAAA0 (60°)) &&
(heightDiff > 0) == (Pitch > 0)` with `heightDiff = anchorZ −
savedLivePose.Z` (the tick-start pose captured `@ 0x4b4190`). A fresh entry
SNAPS X/Y to the anchor, bumps Z by stance (+20480 stand / +39936 crouch
MoveOrder 0x200 / +60416 prone 0x100 && !savedPosY) below the anchor — the
bump breaks the grounded state so the climb can start — or snaps to
`anchorZ − 4096` from above (over the lip onto the ladder), then re-stages
the capsule as the on-ladder pair at the anchor and ZEROES the accumulated
force (`@ 0x4b335a-0x4b3392`).

**While latched** — the per-tick alignment (`@ 0x4b33a4-0x4b3495`): player
yaw eases `(frameYaw − bodyHeading + 8) >> 4` into Yaw AND bodyHeading (the
local player's `g_LocalPlayerLookYaw` dragged along); AI hard-sets
aimHeading/targetHeading/Yaw/bodyHeading; bodyPitch = frame pitch; the
pitch-restore latch (+0x2C bit 4) disarms; the 0.0625u facing press
(full-precision sincos·2^22, `<<12>>22`) plus the `(anchor − pos + 32) >> 6`
X/Y chase. The recontact query arms mask bit 0x1 (the +0.5u inflated CL
test over the 2-point capsule, radii 25088 — the body-axis sincos chain in
the capsule build multiplies a constant-zero length and is dead in the
image, `@ 0x4b2e1c-0x4b2ed7`). Gravity skips on `0x108000`; the horizontal
root pair zeroes on `0x100000` (climb MOTION is the climb clip's vertical
root lane — the 32–35 rows in the capsule-bottom reset list exist exactly
so the dz lane rules); the airborne edge and its `0x2000` write are
suppressed (`0x10A002`/`0x10A000` masks); the view yaw clamps to ±120° of
bodyHeading (§22.1); lean is blocked (`0x100020 @ 0x4b7dad`); the pose
builder keeps the arms on the body matrix (`@ 0x4b1cf1` — hands on the
rungs); water entry is exempt (`@ 0x4b8042`); a CL latch also clears the
parachute bit in the org2 authority leg (`@ 0x4b7b10`, noted for D-INF-20).

**org2 state selection** (`@ 0x4b7484-0x4b76d8`, EVERY tick — the 4th-tick
stance selection's pick is overwritten; our port skips the selection instead,
since our transition machinery would restart the crossfade on the
intermediate stamp — the per-tick net state is identical): on-ladder && not
dead → clear `0x2000` + `slideDecay = 0`; idle → 32 `climb_idle`; the
forward fan (dir 0/1/7) → 33 `climb_up` / 34 `climb_down` by `sign(Pitch)`;
dir 2/6 side-dismount: 0.875u at `frameYaw ∓ 0x3FFFFFC0 (90°)`, +0.25u hop,
the 0.5u off-face push, unlatch; dir 3/4/5 back-dismount: the 0.5u push +
unlatch.

**Exits**: (a) natural top-out — climbing above the volume stops the
re-latch; the resolver exit leg (`@ 0x4b3c5c-0x4b3d69`) pushes 0.375u along
+bodyHeading (over the lip) and arms the local pitch restore (target 4096,
`g_LadderPitchRestorePrev` = Pitch), which then eases the view pitch back at
quarter-step, per-tick clamp ±0x1E00000, done at target+16, canceled by a
user pitch-up past prev (the chase runs in the same resolve, so the first
step lands with the arm); (b) the grounded BOTTOM dismount
(`@ 0x4b7fba-0x4b8019` — clearance ≤ 0, latched, `g_LadderContactZ > pos.z`):
0.5u along −bodyHeading + unlatch — how climbing down ends (the earlier
"ladder-top exit push" gloss read this leg backwards); (c) jump-off
(`@ 0x4b7f0c`): the jump commit adds the same 0.5u back-push + unlatch;
(d) the side/back dismounts above.

**org1 on-ladder** (`@ 0x4bf917-0x4bfad8`, post-resolve, not dead):
`slideDecay = 0`; the congestion probe — 1.25u along bodyHeading, a live
pool-0 person within 73728 (1.125u) per axis whose Z band overlaps
(`selfZ + bound/2 ≥ otherZ − otherBound/2 && selfZ ≤ otherZ`) holds the
climb at 32 (the record's old "water-edge climb-out" gloss misfiled this);
otherwise the 0.03125u facing press (gated on
`g_ResolverAppliedPush == 0` — "applied a nonzero net push", not pass-2
contact: slot re-zero `@ 0x4b3734`, set on a nonzero total `@ 0x4b3767`,
the repulsion store `@ 0x4b3a5c-0x4b3a62`; retail leaves the flag STALE
across skipped resolves — the early ret `@ 0x4b2cfe` — while the port
zeroes it at resolver entry, a skip-tick micro-residual), then 33
`climb_up` below `anchorZ − 49152 (0.75u)`
(or when the 35 clip is unavailable) else 35 `climb_top`. The
`Flags 0x80` = `kEntityFlagAiClimb` order mode replaces gravity with the
sixteenth-step Z chase to the AI move target `+0x304` (cap 0x4000, floor
−16384, `@ 0x4bf6c1-0x4bf6e5`); its eighth-step x/y chase to
`+0x2FC/+0x300` is gated on `attachParent == self` (`@ 0x4bf651-0x4bf664`)
— the AI direct-move mover that rides the order slice with the bit's
writer. NPC gait selection is suppressed while latched
(`@ 0x4bd18d`).

**The view write-back seam**: retail's mouse accumulators
(`g_LocalPlayerLookYaw` / `g_LadderPitchRestorePrev` beside entity
Yaw/Pitch) are our host input record — any sim-side view write (the chase,
the clamp, the pitch restore, the mount snap) mirrors back post-tick so the
next pre-tick input write cannot undo it
(`Simulation::sync_local_mounted_input_heading`).

### 30.3 Follow-ups

1. The AI move-order WRITER — what sets `aiRuntime[1] & 0x400`,
   `kEntityFlagAiClimb`, `attachParent = self`, and the `+0x2FC..+0x304`
   move target (AI pathing onto ladders/rooftops) — rides the AI-order
   slice; the org1 legs here are live in tests via the latch and dormant in
   game until it lands.
2. The authority now runs the shared collision tail for snapshot-owned remote
   players, including CL latching. `remote_player_body_anim` still lacks org2's
   every-tick climb-state override, so a remote climber can show gait states —
   an MP display residual on the D-NET-159 arc.
3. The parachute/carried halves of the shared gates (the 0x100060 clamp
   legs, 0x100020 lean legs, the chute-cancel `@ 0x4b7b10`) ride D-INF-20 /
   the mount slice.
4. Retail runs the org2 stance selection and lets the climb block overwrite
   the field in the same tick; our port skips the selection while latched and
   mirrors its one persistent side effect (the standing-idle counter's
   advance/reset) so the net state cannot drift. The increment is
   standing-only (`@ 0x4b727b`; the crouch `@ 0x4b724b` and prone
   `@ 0x4b722f` legs leave `+0x148` untouched), while the moving reset
   (`@ 0x4b719b`) is stance-independent.
5. The 3P held-weapon presentation follows the aim channels rather than the
   posed hand bones, so a climber's rifle floats beside the body instead of
   riding the rung-hand (retail's weapon attach consumes the arms-locked
   pose; `Entity_CanFireWeapon @ 0x4dcb10` carries no ladder gate, so the
   weapon stays drawn). Presentation-seam residual.

### 30.3a Adversarial review disposition (2026-08-15, three-lens pass)

- **The pitch-up movement-dismount cycle is AUTHENTIC**: pressing the back
  fan while looking up mid-ladder dismounts and the SAME tick's resolver
  passes the fresh-entry gate again (below the anchor + pitch-up + facing
  eased onto the frame), snapping back to the anchor with the stance Z bump
  each tick until the pose crosses the anchor — the witnessed arithmetic
  produces this cycle verbatim, and the port keeps it (decision: copy
  retail). Getting off mid-ladder is look-down + back, a side fan clear of
  the volume, or a jump — as in retail.
- **The one-sided pitch-restore arrival is AUTHENTIC**: the witnessed check
  is `next > target + 16` only, so a restore approaching from BELOW the 4096
  target lands in one resolve (`the else leg @ 0x4b3d45`), pinned by
  `test_ladder_pitch_restore_from_below_snaps`.
- **Fixed from the review**: the mounted 8th-tick resolve now carries the
  ladder IO (retail has ONE resolver — the latch-only channel there latched a
  carried body on any CL touch with no entry gate); `infantry_respawn_snap`
  clears the pitch-restore trio (the host-respawn twin of reset_for_spawn);
  the airborne-edge suppression carries the masks' 0x8000 half; the
  congestion probe reads the peer's ENTITY bound (the staged prox radius is
  pose-widened); the org1 stamps save/restore `anim_pending` (retail's raw
  stores leave the slot untouched); the netsim replica root-suppression
  literal (see §29.2a's correction); `debug_teleport_local_player` drops a
  live latch + restore and invalidates the prev-position gate.
- **Latent, single-writer today**: `last_ladder_frame` is one world-level
  slot read a tick later by the motor legs (dismount yaw, the bottom-exit
  anchor compare, the org1 press); two concurrent climbers would cross-read
  frames once a second writer exists (the AI order slice) — revisit with
  item 1.
- **Replica rows carrying wire 0x100000 now resolve with the on-ladder
  2-point capsule + recontact mask** (the `was_on_ladder` read includes the
  staged flags mirror) — more faithful than master's always-3-point, noted
  as a D-NET-196-adjacent behavior change.

### 30.4 IDB write-backs (2026-08-15 session, saved)

- Renames (anchored, ex auto-names): `dword_B57C8C` →
  `g_ResolverAppliedPush` (first written back as
  `g_ResolverSecondPassContact`; corrected 2026-08-15 — the flag records
  "applied a nonzero net push", and the pass-2 contact bool's only reader
  is the half-force gate `@ 0x4b36da`); `dword_B7900C` → `g_LadderPitchRestorePrev`;
  `dword_B75FCC` → `g_LocalPlayerLookYaw`.
- 13 comments: the climb-SM sites (`@ 0x4b7484`, `@ 0x4b32e5`, `@ 0x4b33af`,
  `@ 0x4b3c5c`, `@ 0x4bf917`, `@ 0x4bf6c1`, `@ 0x4b7fba`, `@ 0x4b7f0c`,
  `@ 0x4b1cf1`, `@ 0x4b2e1c`, `@ 0x4b4978`, `@ 0x4bf9ac`, `@ 0x4b3a5c`) with
  reimpl reverse links.

## IDB type-sync session (2026-07-30)

A net/entity type-sync pass over `Jointops.exe.kong.i64` confirmed the curated
IDB is already ~95% in sync with the reimpl (every witnessed `GamePlayerEntity`
offset already named). The AI-component structs were the main gap and were
declared + applied:

- **`AiBrain`** (812 B, `unk_AED380` = `entity+0x64`) declared with the ~55
  witnessed dwords from `world/ai.h` (waypoint sub-struct `+52..`,
  `cooldown_pair +208`, `part_anim* +436..`, the state-17 working set). Applied
  to `GamePlayerEntity+0x64` (member kept its existing name `renderInstance`,
  now typed `AiBrain *`), so the ~40 AI functions that read
  `entity->renderInstance` as the brain now decompile against named fields
  (`aiState->cooldown_pair`, `->fire_timer`, `->burst_window` …) instead of
  `aiState[52]`/`[9]`/`[181]`. Note: `docs/net/novaworld-net-re.md` labels
  `+0x64` `renderInstance` and the world docs / `ai.h` / the live decomp treat
  it as the 812-B brain — the member name was left as `renderInstance` so the
  net-side label is not overwritten; only the type was set.
- **`AiSlot`** (172 B, `unk_A34B90` = `entity+0x68`) declared (`ai_action +32`,
  `move_flag +136`) and applied to `GamePlayerEntity.aiRuntime` (`AiSlot *`).
- **`GamePlayerEntity+0x370`** `pad_370[4]` split into `prevHeldAdmIndex`
  (`+0x370`) / `armsDipTicks` (`+0x371`) / `reloadAnimTicks` (`+0x372`) + one
  pad byte, matching the reimpl `[orig]` names (`entity.h`). IDB saved.

## Ledger de-table transplants (2026-08-06)

Closed ledger rows whose full text previously lived only in the divergence
ledger, transplanted verbatim at the 2026-08-06 compaction (Standing rule 6).

- **D-AI-3** [FIXED 2026-07-16 (applied; matrix identity pinned by the ai ctest)] CLOSED 2026-07-16: the engagement relation ops APPLY — `apply_engage_relations` writes the sees+targeted quads into `world.relations` with the witnessed key orientation (group = `Entity::group_id` +0x11C, single = `net_id` +0x7C; GS transposed), at both the SM engage (`@ 0x4677b3..0x4678b2`) and the infantry scan-hit (`@ 0x4b0a6f..0x4b0ae2`) sites; `ai_set_target` ports `Entity_SetAITarget @ 0x45d760` incl. the target `+530` refcount. The `rel_ops` vector remains as a test-observability trace
- **D-INF-12** [FIXED 2026-07-16 (§22; mounted/parachute branches ride D-INF-2/-20)] Player (org2) chase sources — the org2 grill landed 2026-07-16 (world-wac-ai-re §22): the witnessed model has NO body chase — the legs chase the render yaw (¼-step, clamp ±0x3000000, twist ±0x30000000 vs the yaw, per-leg staggered 64-tick re-plant windows) and `bodyHeading` = the leg midpoint (`@0x4b4945-0x4b4ac1`); ported in `tick_infantry` (the org1 approximation deleted), and the org1 legs corrected the same session (midpoint re-plant value, staggered windows, walk half-snap `@0x4be969-0x4bea0b`)
- **D-INF-21** [PERMANENT (dev/admin feature, decision recorded)] The "!Poof!" ghost mode deliberately unported: `g_localPlayerPoofMode @0xA82298` (net-toggled `@0x42d450`, debug-chat `!Poof!`) doubles the local player's horizontal root-motion integrate while set (`@0x4b7c8d`); normal play takes the same 1× integrate as org1 (`@0x4b7cbf`), which is what the port implements (world-wac-ai-re §22.3)
- **D-THROW-3** [FIXED 2026-07-22 (`throwables` ctest `test_claymore_sector_los_blocks_trigger`)] Placed-device LOS now routes through the full terrain-plus-sector collision ray, excluding the device and candidate; claymores no longer see eligible targets through type-1 building solids (`Entity_FindEnemyInCone @0x43cba0 -> Physics_RaycastSegment @0x415550`; world-wac-ai-re §27.6/§27.8)
- **D-WPN-4** [FIXED 2026-07-22 (model + deny + HUD feed; residuals split to D-WPN-28/-29)] The heat model, witnessed and ported 2026-07-22: heat is not a stored accumulator but a DEADLINE — `WeaponSlot_CalcAccumulatedHeat @0x53f780` returns `def+880 × (slot+0x14 − current_tick)` gated on `def+876`, the recoil handler pushes the deadline out by `def+876/def+880 + 1` ticks a shot and clamps at 0x12000 `@0x542f8b..0x542fdc`, the pump denies a queued FIRE into EMPTY above 0xFFFF `@0x541046` and zeroes a lapsed/submerged window `@0x54125f`. The def keys are `heat_values <pct/shot>,<pct/s cool>` (parsed 16.16 then integer-divided by 100 and by 6200) and `heat_effect <fx>,<threshold>` `@0x543eb7`/`@0x543e36`. Ported to `engine/formats/def` + `engine/runtime/world/weapon_fsm` + the HUD feed; the glow emitter and the state-11 question move to D-WPN-28 (net-re §5.62)
- **D-WPN-10** [FIXED 2026-07-10 (`nocasecmp_to`)] Reimpl clip-key lookup was case-SENSITIVE (`SkeletalAnim::find_clip` exact ==) where the original resolves anim names with stricmp (`AnimMap_FindSlotByName @0x40cfa0`, name+5 `anim_` skip): JOTAC-era weapon.def rows author `ANIM_WPN_*` uppercase vs the .adm's lowercase clip keys, so every `auto` delaystart/delayend collapsed to 0 and no weapon-action clip played on JOTAC/RevX02 mounts (JOX's lowercase rows masked it) (net-re §5.62)
- **D-WPN-13** [FIXED 2026-07-12 (the `refire_queued` slot latch — the deferred-event queue ported)] Held auto fire ran as a per-tick `request_fire` re-request where the original sustains the volley through the recoil window's deferred re-queue of binding 149 (`Input_QueueDeferredEvent @0x542e9d`, gated `counter<=1 && local && ROUNDS && Flags&0x100 && burst<=0 && Input_IsBindingActive(149)` @0x542e7f; the mid-FIRE press re-queue @0x53effd): at an empty magazine the ungated re-request overwrote the recoil arbiter's queued RELOAD every tick — a held trigger never auto-reloaded and the FSM thrashed FIRE↔RECOIL at 31 Hz strobing the recoil row's soundset/particle (the REVVY M4 wedge) (net-re §5.62)
- **D-WPN-14** [FIXED (disproven premise removed; arrival-time behavior retained)] **FALSE READING RESOLVED 2026-07-14.** `Weapon_RaycastAndSpawnImpact @0x4e8460` is not the ballistic-impact path: `RoundData_SpawnRound` calls it only inside `AmmoDef.flags & 0x400` (instantkillzone) and only for `kztype == Knife`; its `AmmoDef+0x38` extent is `kz_maxradius`, not a weapon range. Ordinary bullets emit their ammo effect/sound row from the physical `Projectile_Handle*Impact -> Projectile_SpawnImpactEffect @0x4e9b80` path, so `world::RoundSim` resolving presentation at simulated collision time is correct. (net-re §5.60)
- **D-WPN-17** [FIXED 2026-07-15 (owner-bound spawn + the GameWorld live-anchor resolver polled by the owner-pose sync; underwater release rides the §8 water plumb)] The local muzzle flash spawned `BINDING_WORLD` at its spawn-time userpoint where retail re-anchors the live action-effect emitter (`MountSlot+0x18 actionEffectHandle` + anchor action `+0x28`) to the action's bone EVERY pump tick (`WeaponAction_ProcessFrame` tracker leg `@0x540edf` → `CEffectEmitter_UpdatePositionAndParams @0x5f6810`), releasing it underwater — a moving/turning shooter's flash trailed the muzzle (net-re §5.62)
- **D-WPN-33** [FIXED 2026-07-27 — ported: fire row, both paths; the recoil row remains] Fire particles spawned at the shooter's EYE instead of the muzzle, on two independent paths. REMOTE: the wire fire position genuinely IS the eye — retail sends `Position + CameraOffset` `[orig: Entity_CalcWeaponFirePosition @ 0x4dc750]` — but retail does not spawn an ammo-def effect there for an adm-indexed round. `NetPacket_DeserializeRoundEvent @ 0x42f270` has TWO mutually exclusive arms, bit 0 tested first `@ 0x42f521`: set -> the ammo-def arm (`ammoDef+64` sound `@ 0x42f5dc`, `ammoDef+68` effect `@ 0x42f6c2`) at the wire position; bit 0 clear with bit 1 set `@ 0x42f6ce` -> the adm arm, which spawns NO ammo-def leg and calls `ActionSlot_ExecuteAction` on the WIRE-ADDRESSED def (`AdmDef_GetEntryByIndex @ 0x42f6d9`, NOT the observed shooter's equipped def) at four sites in two sub-paths chosen by `entity+0x168`: `@ 0x42f777`/`@ 0x42f785` and `@ 0x42f98f`/`@ 0x42f9d0`; `RoundData_SpawnRound` still runs `@ 0x42fa6c`. We admitted `flags & 0x03` identically and ran the ammo legs on both. LOCAL 3P: `_action_particle_world_position` resolved against `_vm_parts`, the FIRST-person viewmodel — re-pinned to the camera every frame and merely HIDDEN in third person, never detached — with a deepest fallback that was literally the eye position; retail requires `g_camera_mode == 0` for the first-person leg `[orig: gate @ 0x540e8c..0x540eca]` and otherwise resolves the same authored userpoint name against gfx3, falling back to the ENTITY ORIGIN `[orig: loc_401867 @ 0x401867..0x401887]`. PORTED 2026-07-27: `FireEvent`/`RoundSpawnParams` carry the wire flags byte and the addressed ADM index; presentation exposes the arm plus the FIRE row's sound/effect/userpoint; `FirePresentPass` swaps in that row on the adm arm and re-anchors to `WirePresentPass.muzzle_world_for()`; the local 3P path resolves against the gfx3 world gun. No new parsing was needed — the row index is identity: retail's per-def action array is 12 pointer slots at `def+676` in the order of `g_weaponActionTable @ 0x830B90` `[orig: base/stride @ 0x54203d/@ 0x542231, bound @ 0x542239]`, so `def+684` IS slot 2 and our `weapon_action::kFire` is the same ordinal. Scoped so a ZERO flags byte (host/AI-originated fire, which retail presents inline at the shooter `[orig: WeaponSlot_FireAndSpawnEffects @ 0x53f440]`) keeps the ammo-def legs untouched. RESIDUAL: `actions[3]` (recoil) is NOT ported — retail runs it beside fire with its own action context and userpoint byte, but only `actions[2]` carries the muzzle flash (the sole row that can reach the muzzle-glow leg, gated on the context being 2 `[orig: @ 0x40205e/@ 0x402080, stamped @ 0x42f8a0]`) and the recoil row's presentation legs are not witnessed well enough to port without inventing them. MUZZLE-ANCHOR AUTHORITY DECIDED (2026-08-08, closing the S12a shadow seam): the rendered held-weapon node's own userpoint (`WirePresentPass.muzzle_world_for`) is the PERMANENT anchor — it is the direct analog of retail's spawn at its rendered model's userpoint, it covers entity-less wire shooters the sim cannot pose, and the weapon node's transform already comes from the native attach walk (one impl). The S12a sim-posed re-derivation (`resolve_held_weapon_muzzle`), the event's shadow `muzzle` field, and the `muzzle_ab` counters are deleted; the anchor fallback chain stays the provider's body origin (retail's entity-origin fallback `@ 0x401867..0x401887`).
- **D-WPN-34** [FIXED 2026-07-31 (`def_parse_ammo`, `def_parse_weapons`, `npruntime_weapon_table`, `npruntime_round_sim`, `infantry`, `netsim_client_replica_pipeline_recoil`, Godot simulation/HUD regressions)] Spawn-time weapon spread and physical recoil were absent/approximate: ERROR/theta and ammo recoil values lost their exact integer carriers; `Weapon_CalcRandomSpreadOffset @0x4e4120` was unported; ordinary rounds omitted the player/rules/UseSpreadTwo gates and the intentionally asymmetric `pitchBlend>>8` versus HUD `>>7`; the shotgun path reused the claymore rectangular fan instead of `Weapon_SpawnProjectileBurstWithSpread @0x4ebbb0`; the per-shot impulse and both infantry-body accumulators had no writer/decay path. The 2026-07-31 grill recovered the exact stance/vertical selectors, uint32 hash, x87-precision constants/distributions (including the shotgun radial fan), special-ammo ordering, post-spawn recoil impulse, movement-weight producer with the exact shared `Player_CanFireWeapon` gates, signed decay/drift, and local/remote/AI body-pass order `[orig: RoundData_SpawnRound @ 0x4ec0d0; Entity_UpdateInfantryPlayerBody @ 0x4b40e0]`. OpenNova now retains the exact DEF integers and applies that path to local authority/SP, C2S authority, own-fire prediction, and decoded tag-2 visual rounds; the ring remains pre-spread. The world-side below-water eye-height projection remains D-INF-18, and whole-process recoil-yaw PRNG call-history identity is isolated as D-WPN-35; neither changes spread/hash, accumulator, impulse, or HUD math.
- **D-WPN-32** [OPEN — row history de-tabled 2026-08-06; the ledger row now carries the open scope only] No entity in the port renders a THIRD-PERSON held weapon — not remote players, not AI, and not the local player's own body. The body `.adm` clip names encode gait+weapon so the pose reads roughly right, which is why this went unnoticed, but every soldier's hands are empty. Retail draws it as draw 5 of `BoneCallback_org0_World @ 0x4e3940`: model = the WeaponDef's `tpModel` (+0x170 = weapon.def `gfx3`) read `@ 0x4e3cd3`, drawn **RIGID** — one matrix `qmemcpy`'d into EVERY bone slot `@ 0x4e3d71` — so it carries no clip, no skeleton and no pose of its own. The placement matrix is bone slot 16's posed matrix applied to that bone's own PIVOT plus a fixed nudge: the model bone-def table is `*(modelDef+56)` with row stride 64 and the pivot float3 at row `+0x24`, so the original's `modelDef+0x424` IS row 16's pivot, and the sight/muzzle triples fall out of the same arithmetic (`0x3E4` = row 15, `0x3A4` = row 14), 1:1 with bone matrix slots `+1024`/`+960`/`+896` `[orig: base load @ 0x4b2186; row walk @ 0x4b201e..0x4b2036; stride @ 0x4b2145]`. Its ORIENTATION is neither bone 16's rotation nor any of the nine aim-overlay classes — the head class carries full-aim pitch and the arm class the 3/4-blended yaw, so reusing either aims the gun visibly off-axis; the original instead writes a triple onto the ENTITY and builds a transform from it (`Roll = savedRoll + lean` `@ 0x4b1bdc`, `Yaw = aimYaw` `@ 0x4b1bf2`, `Pitch = savedPitch + pitchKickAccum + 2*pitchBlend` `@ 0x4b1bf5`, then `Math_BuildFixedPointToFloatMatrix4x4` `@ 0x4b1bf8`), dropping the head-look term outside an aim state `@ 0x4b1dd9..0x4b1dfa`. Visibility is one predicate — the weapon is drawn iff the soldier may FIRE it `[orig: Entity_CanFireWeapon @ 0x4dcb10]`, whose LOCAL branch additionally requires the weapon to author a FIRST-person model, so a `gfx3`-without-`gfx1` weapon is visible to every observer and invisible on your own body (retail asymmetry, cited at the port site). **INCREMENTS 1-2 LANDED 2026-07-26/27 — the local player's own avatar, then every REMOTE player.** Increment 1: `anim::compute_held_weapon_attach_angles` (both on-foot branches, pinned by `aim_overlay_test::test_held_weapon_attach_basis` against the arm/head classes), `Simulation::local_held_weapon_visible` (the local gate incl. the dead, empty-pool, no-`gfx1` and seat legs), `PlayerViewmodelDef.gfx3` + `PlayerAimOverlay.weapon_attach_angles/weapon_visible`, and `LocalPlayerPresenter._update_held_weapon` building the model through `MissionObjectPlacer.build_model_from_graphic` as a SIBLING of the avatar (`ObjectModel.rebuild()` frees all children, so a child would vanish on every body rebuild). **Increment 2** extends it to every remote player, live-confirmed as the gap after increment 1 (the maintainer, joined to a stock retail host: "none of them are holding their weapon. Empty hands"). `world::WeaponTableEntry` gained `third_person_model` (`gfx3`) so a held weapon resolves through THE SAME table the wire's ADM index refers to — routing it through `WeaponDatabase` instead would have coupled two independent `weapon.def` parses whose index bases differ (0-based file order vs this table's 1-based null-row-0), an untested assumption behind a runtime index. Four snapshot fields (`PF_HELD_WEAPON_ADM` + the attach euler) are written by one shared helper from both the host and joiner legs, with the DRAW GATE FOLDED INTO THE ADM FIELD: a hidden or unarmed body reports 0, which is simultaneously our table's null row and the original's own `if (entity->equippedAdmIndex)` precondition, so the zero-filled default is correct by construction and there is no sentinel to invert. The REMOTE branch of the gate reduces to two terms — alive, and `MountMode::OnFoot` — because that mount mode is exactly the complement of retail's `{2,3,5}` hide set (a PASSENGER maps to OnFoot and keeps its weapon), and because the remote branch never consults an `EquippedSlot`, so a peer whose slot we do not model still passes as retail intends. `wire_present_pass` owns a weapon node per wire handle beside `_nodes`, and the placement math is now shared with the local path through `PresentHeldWeapon` rather than duplicated. RESIDUAL: AI/NPCs still render no held weapon — placed `.bms` soldiers never receive an equipped ADM index at all and its source in the original is UNWITNESSED, so increment 3 is blocked on that research rather than on effort. Also unported from the same callback: the NVG and BINOCULAR item-model draws (`Flags & 4` / `animStateFlags & 0x40 && Flags & 8`, draws 3 and 4), the death-pose weapon branch `@ 0x4b21b0`, the projected-size cull `@ 0x4b3d4b`, and the mount-branch correlation for the attach basis (the original copies the body or arm matrix per branch `@ 0x4b193e`/`0x4b19cf`/`0x4b1ab2`/`0x4b1b35`/`0x4b1b94`; the draw gate hides control/gunner/driver seats anyway, leaving only the passenger seat exposed). **INCREMENT 3 LANDED 2026-07-27 — the SECOND attach frame**, after the weapon was reported "rotated, often near-vertical". The entity-frame half was verified matching and needed no constant moved: (a) the out-matrix 3×3 is the attachment matrix verbatim — only the translation row is overwritten before the copy `@ 0x4b22cf..0x4b22f8` — and the draw site adds nothing `@ 0x4e3d71/0x4e3d99`; (b) the attach builder `Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200` is the same one that places an ordinary world object `@ 0x4e2912`, so a `gfx3` is posed exactly like a placed `.3di`; conjugating its Z→X→Y row-vector composition (quantized trig stores −sin θ) through the render↔Godot X↔Z relabel and the loader's X-negation gives `swap ∘ Mᵀ ∘ flipX`, which is `bms_to_godot_basis` term for term, trailing `Ry(+90°)` included — pinned by `present_held_weapon_test.gd::test_bms_basis_matches_the_original_placement_matrix` over five triples including two with pitch, yaw AND roll non-zero (the earlier live sample had roll = 0 and could not have caught an axis or ordering swap); (c) the authored model frame is muzzle `+Z` / up `+Y`, from `M4_3RD`'s `MFLASH01` `(+0.003, +0.089, +0.781)` and `scope` `(−0.002, +0.194, +0.141)` userpoints with identity node transforms throughout, closing the "which end is the muzzle" and "which way is up" questions the AABB could not. **The actual defect was a MISSING BRANCH.** Retail has TWO attach frames and picks on one bit: `g_animStateFlagsTable[entity+0x2C8] & 0x80` `[orig: gate @ 0x4b21b6, branch @ 0x4b220f]` selects `Ry_e · Rz_e · boneMatrix[16]` — the HAND frame — instead of the entity triple. A first re-measurement called that branch death-only; **that was wrong**, and the error was assuming `+0x2C8` holds a `wpn_*` id (rows 240–251, all flag `0x0`). Its writer `[orig: Entity_UpdateInfantryPlayerBody @ 0x4b5dad..0x4b5ea9]` stores `AdmDefs[+0x2B0].kind + 0x31`, a HOLD state in the 43–66 band, where 0x80 is set for 50 `knife`, 52 `grenade`, 54 `designator`, 62/63 the melee attacks, 64 `binoculars` and 65/66 BOTH reloads. The asset side corroborates exactly: of 41 distinct `gfx3`, 33 are `+Z`-long and the 8 `+Y`-long ones (`M9K_3rd`, `mach_3rd`, `Flsh_3RD`, `Frag_3RD`, `smok_3RD`, `medp_3rd`, `stch_3rd`, `det_3rd`) are precisely the kinds that stamp a 0x80 row — authored blade-up because retail poses them at the hand. Under the entity frame `elev(B·+Y) = 90 − pitch`, so they drew at +72.05° at the measured live triple and at exactly 90° when level: the reported symptom, reproduced numerically. PORTED: `PF_HELD_WEAPON_HAND_FRAME` written by the one shared helper from both legs (the joiner's hold state hoisted OUT of the channel-visibility gate — the pose is gated, the FRAME is not), `PlayerAimOverlay.weapon_hand_frame`, and `PresentHeldWeapon.hand_frame_basis`. Both constants are used with their authored signs, which is derived and not chosen: `Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0` is fed `sin = −sin θ` (making each block a rotation by −θ) and the conjugation through the loader's X-negation flips the sign back — two inversions. Pinned by `test_hand_frame_constants_match_the_original_composition`, which re-derives the calibration from the builder's ELEMENT PLACEMENT rather than from the constants' signs. Also fixed: the nudge was rotated by bone 16's own posed basis and added to a WORLD position, so at rest it picked up that bone's large rest rotation (`BN17 R Hand`'s rest basis is nowhere near identity); it now rides the bone's MODEL→WORLD rotation (`pose · rest⁻¹`, in world), matching `nudge · M16_rotation` `@ 0x4b2186..0x4b220b`. Recorded while here: ADM index 1 IS `WPN_KNIFE` — `AnimDef_InitAll @ 0x5435c0` consumes slot 0 with a reserved `"null"` def before any `weapon.def` row parses and `AdmDef_GetEntryByIndex @ 0x53fc80` applies no bias, so the wire index is 1-based over file order, validating `world::WeaponTable`'s null-row-0 layout and confirming `WeaponDatabase`'s 0-based enumeration (which also drops rows, exposing 72 of 94) must never back a runtime ADM index. STILL OPEN: the mounted-branch correlation, the NVG/binocular draws, AI bodies, and the death family's own use of the same 0x80 branch


## 31. The AI convoy movement chain, audited end to end (2026-08-24)

Written while chasing an open behavioural divergence on mission `00TRg`: a
scripted five-vehicle convoy where our followers fall progressively behind
retail's. The divergence is NOT resolved by this record. What the record
carries is the audit itself, because eleven separate checks against the
decompilation all came back matching, and knowing which links are already
witnessed is worth as much as the fix.

### 31.1 The measurement that started it

Path travelled between two mission events (channel-3 entry to the scripted
redirect), our host against a retail AI-probe capture of the same mission.
Path length is a geometric integral, so neither probe's sampling cadence
enters the number.

| convoy slot | bms | item | accel/decel | retail | ours | delta |
|---|---|---|---|---|---|---|
| 0 (lead) | 59 | 1302 | 80/160 | 726.1 | 726.1 | -0.00% |
| 1 | 58 | 1294 | 40/80 | 710.9 | 704.9 | -0.85% |
| 2 | 63 | 1294 | 40/80 | 707.0 | 687.9 | -2.71% |
| 3 | 1664 | 1420 | 40/80 | 713.5 | 686.3 | -3.81% |
| 4 (tail) | 62 | 1305 | 80/160 | 716.4 | 680.6 | -5.00% |

Both engines enter channel 3 at the same points, so the divergence accrues
during the drive. The error is monotonic with position in the convoy and the
lead is exact. Vehicle physics does NOT predict it: bms 62 carries the same
high acceleration as the lead and has the worst shortfall, while bms 58 and
63 are the SAME item with identical accel, decel and top speed in adjacent
slots and differ threefold. Retail's own shortfall is not monotonic (its 1664
and 62 travel further than its 63), so retail's convoy recovers where ours
degrades.

### 31.2 The chain, and the verdict on each link

The mechanism is the pool-1 avoid brake compounding down the chain: the
braking pairs are exactly the convoy order (62<-1664, 1664<-63, 63<-58,
58<-59) and **the lead brakes zero times**. Effective brakes per vehicle
(0/267/412/549/709) rise in the same order as the shortfall.

Every link from nav node to wheel was then witnessed:

| link | original | verdict |
|---|---|---|
| Waypoint target resolve | `AIWaypoint_UpdateTarget @0x457380` | matching (two deviations declared at the site) |
| Distance + bearing | same function | matching, including the `5*(...)>>16` term and the `atan2 * 683565275.5764316` bearing |
| Node advance + arrival halve | `AI_UpdateWaypointMovement @0x457BD0` | matching register for register, both halve branches, write order |
| Commanded-speed cap | `@0x48bc23-0x48bc48` | matching |
| Pool-1 avoid brake, factor | `@0x48bf17-0x48bf26` | matching; measured f in [0.251, 0.748], mean 0.4989 against the witnessed [0.25, 0.75] |
| Avoid brake, trigger geometry | `@0x48bdf2..0x48bf0f` | matching: reach, doubled z, directional footprints, the ~30 degree dead-ahead test |
| Bound-radius derivation | `Entity_InitFromModel @0x40dc30` | matching, including the `max` against the husk radius and the `+0x1000` pad |
| Speed approach + recovery | `@0x48AF00` interior | matching: `cos^2(Pitch)` target, `(target - speed + 16) >> 5`, the `deceleration >> 1` clamp, the `<48` stop snap and the zero-delta snap |
| itemDef accel/decel parse | `@0x49da84` | matching; verified at runtime that the parsed values reach the loaded traits |
| Heading fed to the brake | `entity+0x10` | matching; our mirror is that field and is seeded for ground vehicles |
| Per-entity tick order | `Entity_DispatchPhysics_cveh @0x48efc0 -> @0x48af00` | matching: one interleaved pass, brake before the speed approach in both |
| Pool-1 iteration direction | `Entity_UpdateAllEntities @0x4C2100` | matching: ascending slot on both sides |

### 31.3 One defect, found and closed

The brake's pool-walk gate skipped a neighbour whose bound radius was <= 0,
cited to `@0x48bdd9`. Retail's gate there reads **`entity+0x1C`, which is
`ItemTypeIndex`** (offset derived by walking `GamePlayerEntity` and validated
against four known anchors on the way: Yaw `0x10`, itemDef `0x20`, Flags
`0x24`, groundEntity `0x28`), stamped at spawn as `defIndex`
`[orig: Entity_SpawnFromBMSRecord @0x40E9F0]`. The occupancy half of retail's
walk is already covered by our registry returning null for an unused slot, so
the radius test was an additional gate with no counterpart in the original.

Measured before changing anything: over 400,000 gate evaluations on this
mission the radius test skipped nothing and never disagreed with the
item-type test, so the fix is a no-op here and the rig's output is
byte-identical across it. Ported regardless: an unwitnessed gate is a defect
even when it is inert, and the next mission need not be so forgiving.

### 31.4 One divergence, real but inert here

Retail assigns pool-1 slot `j` = the BMS record index directly
(`Pool_GetEntry(1, j)`) and `Entity_SpawnFromBMSRecord` can early-return on
its team filter **while still consuming the slot**, so retail's pool carries
holes and `used` counts records rather than live entities. Our registry
compacts to the first free slot.

This is inert for the movement chain: compaction is monotonic, so relative
update order is preserved, and the brake's per-neighbour multiply is
commutative within a tick. It is live for anything that keys on the slot
INDEX rather than on order.

2026-08-25 (post-merge tidy): the slot assignment is ported —
`promote_mission` spawns record i at pool slot i (`spawn_at`, overwriting a
live occupant the way the fixed pool entry would), for every pool loop
[`Mission_LoadBMSFile @ 0x40F4E0` — pool 1 `@ 0x40f9bb..0x40f9c6`, pool 2
`@ 0x40fa28..0x40fa34`, pool 3 `@ 0x40fa98..0x40faa4`, pool 0 `@ 0x40fb0d..
0x40fb19`; `promote_test`]. Note the pool-0 loop's `Pool_SetUsed` counts
SUCCESSFUL spawns (`@ 0x40fb21..0x40fb34`) where pools 1-3 store the record
count. What remains is the FILTER that makes the holes:
`Entity_SpawnFromBMSRecord @ 0x40e9f0` returns without spawning for the BMS
RemoveIfLessThan / RemoveIfMoreThan / MP-only / SP-only attribs against the
session player count `@ 0x40ea8e..0x40eac2`, for an itemDef attrib 0x10000 item
outside a 4-team TDM / TKOTH / FlagBall session `@ 0x40eade..0x40eb05`, and for a
team-3/4 record when `g_num_teams_config != 4` `@ 0x40eb23..0x40eb2e`. Our
promote spawns every record, so both the holes and the entities retail never
spawns are the open surface; that filter is a spawn port of its own (it needs
the session player count, the in-session bit and the team count at promote
time) and is recorded for sign-off in the 2026-08-25 tidy PR rather than
minted here.

### 31.5 What this leaves

Every consumer of the nav channel is witnessed faithful, so if the outcome
differs, what differs is what they consume. The remaining unaudited surface is
the `NavChannel`/`NavEntry` data itself: node positions, the per-channel count
and loop flag, and in particular the arrival threshold `node->f[0]`, which
becomes `aiState[23]` and gates BOTH the node advance and the arrival halve. A
wrong threshold has exactly this bug's signature, because it is compared
against a per-entity distance and therefore fires at a different moment for
each vehicle depending on where that vehicle is.

### 31.6 Method notes worth keeping

- **Check which FUNCTION you are reading, not just which field.** The speed
  approach exists in both `@0x46E100` and inside `@0x48AF00` with the same
  shape. An audit was once run against the wrong one; the verdict happened to
  survive re-checking, but only by luck, and the correct function carried a
  `deceleration >> 1` clamp the other did not.
- **A control is only a control if it exercises the subsystem under test.**
  The lead vehicle was used as a same-motor control before anyone checked its
  item id; it is a different vehicle with double the acceleration. The usable
  control is the two vehicles that share item 1294.
- **Put a positive control on a counter before believing a zero.** A bare
  "0 disagreements" is indistinguishable from "the probe never ran".
