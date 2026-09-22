# Vehicle motors, state and presentation — reverse-engineering record

Authority simulation and client prediction for ground, tank, bike, boat,
amphibious and aircraft families, including selector-zero motors, mounted
animation, HUD state, sound, trails, rotor wash and wreck lifecycle.
The implementation lives in engine/runtime/world/vehicle_*.cpp, the shared
carrier/collision/AI/destruction modules, and the Godot presentation adapters.
Binary: retail Jointops.exe, imagebase 0x400000, IDB Jointops.exe.kong.i64.
Every address below is absolute in that image. The wire/chase architecture
is [net-re section 5.38e](../net/novaworld-net-re.md).

Sections 1 through 10 retain the dated instruction-level research and the boundaries
of the earlier client-only ports. Their historical deferred/unported labels
are superseded by sections 11 through 37 and the current table below. PR #640
ported the vehicle families end to end; its 2026-09-08 review re-grilled the
traction, lean, selector-zero, aircraft-brain, sound and dispatcher legs (the
corrections are listed under "PR #640 review corrections" below). D-ITEM-15 and
D-AI-11 are FIXED; D-NET-161 and D-SND-17 are OPEN but narrowed to the residuals
the review witnessed and did not port (the ledger rows carry them with
addresses). D-NET-196 retains its separate organic body-conform and
authority-smoothing scope. D-VEH-2 records the bounded water-ring retirement
correction — proposed in PR #640, pending maintainer ratification at merge.

## Verdicts

| Component | Verdict | Evidence |
| --- | --- | --- |
| Full and selector-zero motors; amphibious dispatch | Ported for authority and prediction (the selector-zero boat's PlayerControl block, its claimant edge and part-spin call, ported 2026-09-12; the former D-NET-161 (d)) | Sections 11, 16, 26, 30 through 31; vehicle_motor, aircraft_client_motor, watercraft_client_motor, vehicle_part_anim |
| Model contacts, springs, traction, chassis and carrier motion | Ported; #645 crash-height, bike axle and wheelie corrections included; the tank solve's crash, wreck, wall and stability machinery and the tank mover's impulse, crash-stop, trail, slope and per-family chase gates completed 2026-09-22 (D-VEH-4, the [tank record](tank-parity-re.md)) | §8, §10, sections 12 through 15, 19 through 20, 22 through 23, 27, 38, 40; collision, vehicle_suspension, vehicle_followups, vehicle_mount, vehicle_motor |
| Occupancy, AI, death and respawn state | Ported (the `entity+684` think countdown ported 2026-09-12, the former D-NET-161 (f); the AI slot seed for every AI-class record, the /62 respawn budget and the respawn class-init re-run 2026-09-22; open: the pool-3 deck-marker localization, the ground-height tap ray kinds, the emplacement brain dispatch: D-NET-161 (b), (e), (g); the gunner-attachment runtime of section 26.2 awaits its three data hooks) | Sections 17 through 18, 24 through 27; ai, destruction, vehicle_mount, mission_mount, mission_promote, vehicle_attachments |
| Wheel/track/turret/gear and mounted-body animation; HUD state | Ported through renderer-owned channels and existing HUD snapshot | Sections 11, 14, 21; vehicle_part_anim, netsim_present_rows, simulation and attachment GUT suites |
| Ground/boat/aircraft sound and contact edges | Ported (tank pivot cue/latch/fourth loop, the tread cue, the tank solve's tumble cues, the corrected detach stop and the last-tick gate completed 2026-09-22; D-SND-17 closed) | Sections 11 through 13, 29, 31 through 33, 40; vehicle_motor, vehicle_suspension, ambient_mixer, mission_audio |
| Wreck bone banks, W1 through W4 trails, rotor wash, foliage sway and water rings | Ported | Sections 28 through 29, 32; destruction, vehicle_part_anim, vehicle_trail_present_pass, shader_resource_contract |
| Full water-ring bank expiry | PERMANENT bounded-pool correction (proposed in PR #640, requires maintainer ratification at merge) | D-VEH-2; saturated 128-slot retirement regression; ADR 0022 register entry |

---

## §1 The watercraft mover — client-executed subset

Source: `Entity_UpdateWatercraftPhysics` @ 0x48D480 .. 0x48EF74 (retail Jointops.exe,
imagebase 0x400000). Decompilation cross-checked against a full instruction-level dump
(a full instruction-level disassembly dump, session artifact); every FPU-garbled block below was
reconstructed from the disassembly, not the decompile. Ground-family comparison (part F)
is from `Entity_UpdateVehiclePhysics` decompile only, as permitted.

Scope: originally the blocks a CLIENT executes for a REMOTE (non-local-driver,
non-authority) watercraft — everything OUTSIDE the drive-INPUT block gated at
0x48DF8C..0x48DFA2 (`attrib&0x40 && (is_authority || occupant ==
g_local_player_entity)`), from the interp block's register mirror to the position
integration. The per-record chase (interp) is already ported; it is recapped only
for ordering and for the stale-speed decay it owns. **2026-08-06: the AUTHORITY
half (the gated MoveOrder merge/capsize and the INPUT block's player/AI/parked
legs) is witnessed in §1.12 and ported — this record now covers the full
function; the subsequent closure work is recorded in sections 11 through 36.**

Conventions: positions/velocities are 16.16 fixed (1.0 = 0x10000 world unit); angles are
32-bit BAM; `>>` is the x86 `sar` (arithmetic); `(a*b + 0x8000) >> 16` denotes the exact
retail sequence `imul` (signed 32x32->64) ; `add eax,0x8000 ; adc edx,0 ; shrd eax,edx,16`
(round-half-up on the 16.16 product); `(a*b) >> 22` is `imul ; shrd eax,edx,22` with NO
rounding bias. `idiv` truncates toward zero. Tick = the 62 Hz world tick.

---

### 0. Pointer identities and offset map (part E)

`entityState` (decompile name) is **the vehicle's own AiBrain**, `AiBrain *` at
**entity+0x64** [orig: `mov ebp,[esi+64h]` @ 0x48D499; IDB field name `renderInstance`
is historical — the IDB *type* is `AiBrain *`]. It is NOT a separate vehicleData struct;
the drive-command registers live inside the AI component. (entity+0x68 is a different
pointer, `aiRuntime : AiSlot *` — used by the pool-0 boarding scan, not by motion.)

AiBrain dword index -> byte offset (index*4), with role in this function:

| idx | offset | role | witness |
|-----|--------|------|---------|
| [4]   | +0x10  | cur_state (22 = idle default) | 0x48D518, 0x48DB84 |
| [5]   | +0x14  | pend_state (21 = wreck on death) | 0x48D55A |
| [127] | +0x1FC | AI target ref (AI leg only) | 0x48E254 |
| [128] | +0x200 | AI out_speed (AI leg only) | 0x48E260 |
| [115..126] | +0x1CC..+0x1F8 | wake-anim lerp block (anim-only, see §7) | 0x48ECF5.. |
| [132] | +0x210 | **steer target (BAM heading the rudder chases)** | 0x48E8AB |
| [136] | +0x220 | **commanded speed, 16.16 u/tick** | 0x48E926 |
| [137] | +0x224 | key-steer ramp (input block only; cap 0x1FFFFFE0, step 0x16C16C0) | 0x48E13A |
| [177] | +0x2C4 | net-received speed (record apply writes it; decays when stale) | 0x48DDC0 |
| [179] | +0x2CC | net-received steer heading | 0x48DDE8 |
| byte +0x318 | (792) | sound-latch bits | 0x48EDCC |

GamePlayerEntity fields consumed by the client motion path (IDB names):

| offset | field | role |
|--------|-------|------|
| +0x04/08/0x0C | Position.X/Y/Z | 16.16 world position |
| +0x10/14/18   | Yaw/Pitch/Roll | BAM attitude (`&Position` doubles as the 6-dword euler block passed to matrix build) |
| +0x20 | itemDef | |
| +0x24 | Flags | bit 1 dead; bit 0x80 lights; **bit 0x2000 airborne** (set/cleared by platform solve @ 0x483B93 / 0x483D55); **bit 0x8000 afloat/in-water** (set/cleared @ 0x482CA5 / 0x482DB7); bit 0x20000 "matrix valid" set at tail |
| +0x28 | groundEntity | carrier entity (deck riding) |
| +0x64 | AiBrain* (see above) | |
| +0x80..0x88 | savedLivePose | position at tick start |
| +0x8C/90/94 | bodyHeading/bodyPitch/bodyRoll | attitude at tick start |
| +0x98 | velocityX | 16.16 u/tick |
| +0x9C | velocityY | 16.16 u/tick |
| +0xA0 | slideDecay | **vertical velocity** (Z u/tick, 16.16) |
| +0xA4 | modelPtr0 | **yaw rate** (BAM/tick) — misnomered `void*` in IDB |
| +0x170 | occupantEntity | driver |
| +0x234/240 | smoothTargetPos / smoothTargetHeading | interp deltas (ported chase) |
| +0x27C/27E | interpProgress / interpStepBucket | interp counters |
| +0x29C | currentSpeed | along-hull signed speed, recomputed every tick in §5 |
| +0x2B4 | aiState | **rudder deflection state** (low-pass filter register) |
| +0x2B8 | pendingAnimStateId | anim accumulator (note-only) |
| +0x364 | attachBone (uint8) | gates the afloat -8350 Z case (§6) |

ItemDef fields: +0x8E0 `acceleration`, +0x8EC `waterSpeed` (same field the ground family
reads as playerSpeed — it is the family's max drive speed), +0x924 `turnRate`,
+0x928 `turnRate2` (all 16.16-per-tick or BAM-per-tick as used below).

---

### 1. Per-tick block order for a remote watercraft on a client (part E)

All of this is ONE call of Entity_UpdateWatercraftPhysics per world tick:

1. savedLivePose <- Position; bodyHeading/Pitch/Roll <- Yaw/Pitch/Roll [0x48D4A7..0x48D4EF].
2. Brain state defaults (cur_state 22 if 0) [0x48D512]; every-8th-tick
   `Entity_RaycastGroundHeightAndObject(entity,0,0,0x10000,0x200000)` refreshes
   groundEntity [0x48D51F, gate `(phase & 7)==0` where phase = current_tick + 36*DcbId,
   the per-entity stagger key built at 0x48D496..0x48D4A4 — the decompile's
   `&current_tick[9*DcbId]` pointer read is this scalar]; death -> pend_state 21 [0x48D53D].
3. Fire/smoke FX + regen/drain (authority-gated where damaging; FX cosmetic) — no motion.
4. Carrier follow: if groundEntity != NULL, Position += carrier's tick delta and the full
   fixed-point parent-rotation transform re-seats Position/Yaw/Pitch/Roll
   [0x48D6DA..0x48DACD]. Runs on clients (boat on a moving deck).
5. attrib&0x40 occupant water enter/exit one-shot sounds [0x48DAD1..] — no motion.
6. `!is_authority` interp block [0x48DB6B..0x48DDD4]: chase target recompute when
   interpProgress==0 (snap threshold 0x60000, or 0x20000 when brain[177] < 293), yaw
   step while interpProgress < 20, position step while < interpStepBucket,
   interpProgress++ while < 128; **once >= 128 (stale records): brain[177] -=
   (brain[177] + 64) >> 7** [0x48DDC0..0x48DDCE]. (Ported chase; the [177] decay
   belongs to the prediction contract — the mirrored cmd coasts to zero.)
7. **Register mirror** [0x48DDD4..0x48DDF4]: if `occupantEntity != g_local_player_entity`:
   `brain[136] = brain[177]; brain[132] = brain[179];`
   (client-side, every tick, including unoccupied boats).
8. Dead check: `Flags & 2` -> skip everything to step 16 [0x48DDFA].
9. MoveOrder merge + capsize damage — both gated off for remote clients
   [0x48DE20 requires occupant==local or authority; 0x48DE84 requires authority].
   Authority behavior: §1.12.
10. Seat sweep (attrib&0x40): stale occupantEntity/mountHandles cleared [0x48DED4..0x48DF7D]. Runs on clients.
11. INPUT block — **skipped on remote clients** (the gate) [0x48DF8C..0x48DFA2 jumps
    remote clients straight to 0x48E82C]. The authority/local-driver interior: §1.12.
12. Steer/rudder integrator — §3 [0x48E82C..0x48E926].
13. Thrust (cmd != 0) or zero-cmd velocity snap — §4 [0x48E926..0x48EA12 / 0x48EAB1].
14. Drag/slip/keel block — §5 [0x48EA14..0x48EBB3]. Writes currentSpeed.
15. Ground/water contact drags — §6 [0x48EBB5..0x48ECA6].
16. Integration — §7 [0x48ECA8..0x48ECF5]:
    Position.X += velocityX; Position.Y += velocityY; Position.Z += slideDecay;
    modelPtr0 self-decay; `Entity_ProcessPlatformPhysics(entity, frameCount)`
    (buoyancy/attitude/flag producer, @ 0x481870 — ungated, runs on clients);
    **Yaw += modelPtr0** [0x48ECF2].
17. Wake anim lerp on brain[115..126] (anim-only) [0x48ECF5..0x48ED76].
18. Bone-trail FX + movement sounds (cosmetic) [0x48ED76..].
19. Tail: `Math_BuildFixedPointMatrixFromEulerAngles(&entity->Position,
    entity->orientationMatrix)`; `Flags |= 0x20000` [0x48EF50..0x48EF63].

---

### 2. Angle/trig conventions used by the reconstructed blocks

Constants (exact doubles read from the binary):

- `dbl_7C3608` = **1.4629627251502471e-09** — BAM -> radians. This is
  pi / 0x7FFF8000 (NOT pi/2^31): the engine's half-turn here is 0x7FFF8000.
- `dbl_7C19D8` = **683565275.5764316** — radians -> BAM = 2^31/pi. (The pair is not an
  exact inverse; the roundtrip inflates by ~3.05e-5. Port both constants verbatim.)
- `dbl_7C3600` = **4194304.0** = 2^22 — trig results are scaled to 22-bit fixed before ftol.
- `flt_7C19E0` = **2147418112.0** (= (float)0x7FFF8000) — ftol overflow clamp applied to
  every sqrt magnitude before conversion (`fcom st; keep min`).

So, in pseudo-C: `sin22(bam) = (int32)(sin(bam * 1.4629627251502471e-9) * 4194304.0)`,
same for cos22; `bam_of(atan2(y,x)) = (int32)(atan2(y, x) * 683565275.5764316)` where
atan2 is x87 `fpatan` (atan2(0,0) == 0). Magnitude:
`mag = (int32)min(sqrt((double)x*x + (double)y*y), 2147418112.0)`.

---

### 3. Steer/rudder integrator (part A) [orig: 0x48E82C..0x48E926]

```c
// itemDef reads
int32 turnRate  = itemDef->turnRate;            // +0x924   [0x48E82F]
int32 waterSpd  = itemDef->waterSpeed;          // +0x8EC   [0x48E835]
int32 minRate   = turnRate >> 2;                //          [0x48E83D]
if (itemDef->turnRate2 != 0) minRate = itemDef->turnRate2;  // +0x928 [0x48E872..0x48E880]

// speed fraction f in [0 .. open-ended], 16.16
int32 f;
if (waterSpd != 0) {
    // 64-bit (currentSpeed << 16) / waterSpd via edx:eax manual widen + idiv
    f = 0x10000 - (int32)(((int64)entity->currentSpeed << 16) / waterSpd);  // [0x48E844..0x48E869]
} else {
    f = 0;                                       // [0x48E86D]
}
if (f < 0) f = 0;                                // [0x48E882..0x48E886]
// NOTE: no upper clamp — currentSpeed < 0 (reversing) makes f > 0x10000 and the
// effective rate EXCEEDS turnRate. Witnessed absent; keep it absent.

// effective turn-rate clamp (BAM/tick "chase step" bound)
int32 eff = minRate + (int32)(((int64)(turnRate - minRate) * f + 0x8000) >> 16); // [0x48E88E..0x48E8A8]

// proportional chase toward the steer register, 1/64 of the error with +32 bias
int32 delta = (brain[132] - entity->Yaw + 32) >> 6;   // [0x48E8AB..0x48E8B7]
if (delta >  eff) delta =  eff;                        // [0x48E8BA..0x48E8BE]
if (delta < -eff) delta = -eff;                        // [0x48E8C0..0x48E8C6]

// rudder state low-pass: aiState chases (-32*delta) at 1/8 per tick, +4 bias
entity->aiState += (4 - 32*delta - entity->aiState) >> 3;   // [0x48E8C8..0x48E8E9]

// yaw rate recompute, gated:  !(Flags & 0x2000) || (Flags & 0x8000)
//   (i.e. recompute when not airborne, OR whenever afloat)
if ((entity->Flags & 0x2000) == 0 || (entity->Flags & 0x8000) != 0) {   // [0x48E8E3..0x48E8F7]
    entity->modelPtr0 = (int32)(((int64)(-entity->currentSpeed)
                                * (entity->aiState >> 2) + 0x8000) >> 16); // [0x48E8F9..0x48E920]
}
// modelPtr0 is APPLIED unconditionally every tick at integration time:
//   entity->Yaw += modelPtr0;   [orig: 0x48ECF2, AFTER Entity_ProcessPlatformPhysics]
// When the recompute gate is off the stale rate still turns the boat while the decay
// legs (§6, §7) bleed it.
```

`v116` in the decompile = `turnRate - minRate`; `v181` = `f` above. At standstill the
chase clamp is `turnRate`; at waterSpeed it is `minRate`. currentSpeed is signed, so a
reversing boat steers mirrored (negated product), exactly like the ground family.

---

### 4. Thrust block (part B) [orig: 0x48E926..0x48EA12]

```c
int32 cmd = brain[136];                          // [0x48E926]
if (cmd == 0) {
    // zero-command: snap tiny planar velocity to rest, then fall through to §5
    if (abs(entity->velocityX) < 384) entity->velocityX = 0;   // 0x180  [0x48EAB1..0x48EAC3]
    if (abs(entity->velocityY) < 384) entity->velocityY = 0;   //        [0x48EAC9..0x48EADF]
} else {
    // acceleration magnitude: def accel plus speed-proportional terms
    int32 a  = abs(cmd);
    int32 acc = itemDef->acceleration + (a >> 8) + (a >> 7);   // +0x8E0  [0x48E936..0x48E954]
    // clamp toward cmd (sign handling verified at 0x48E956..0x48E96E):
    //   cmd > 0:  acc =  min(acc, cmd)
    //   cmd < 0:  acc = -acc; if (acc < cmd) acc = cmd;   // == max(-accMag, cmd)
    int32 accel = (cmd >= 0) ? min(acc, cmd) : max(-acc, cmd);

    // Orientation from the entity's OWN live pose. The decompile's "(int*)v124" arg is
    // garbage: the real call site pushes  lea eax,[esi+4]  = &entity->Position —
    // the 6-dword block {Pos.X, Pos.Y, Pos.Z, Yaw, Pitch, Roll}. So thrust direction
    // includes the platform-solve pitch/roll, not just yaw.   [orig: 0x48E972..0x48E97B]
    fx22mat m;
    Math_BuildFixedPointMatrixFromEulerAngles(&entity->Position, m);
    int32 fwd[3]; Math_ExtractRow0FromFixedPoint22(m, fwd);  // forward, 16.16 [0x48E98A]
    int32 up[3];  Math_ExtractRow2FromFixedPoint22(m, up);   // up, 16.16      [0x48E999]

    int32 vertical_thrust = 0;   // "v193"/var_14, zero-initialized at fn entry [0x48D4DE]
    if (up[2] > 0) {             // capsize gate: hull upright only   [0x48E9A1]
        entity->velocityX += (int32)(((int64)accel * fwd[0] + 0x8000) >> 16);  // [0x48E9A8..0x48E9BC]
        entity->velocityY += (int32)(((int64)accel * fwd[1] + 0x8000) >> 16);  // [0x48E9C2..0x48E9D6]
        vertical_thrust    = (int32)(((int64)accel * fwd[2] + 0x8000) >> 16);  // [0x48E9DC..0x48E9FF]
        // NOT added to any velocity — only gates the beaching stop in §6.
        brain_pendingAnim: entity->pendingAnimStateId += cmd << 13;  // anim-only, 0x2000*cmd [0x48E9F0..0x48E9F9]
    }
}
```

---

### 5. Drag / slip / keel block (part C) — FPU-reconstructed [orig: 0x48EA14..0x48EBB3]

Runs unconditionally after §4 (both cmd paths join at 0x48EA14).

```c
// 1) hydrodynamic drag: shed 1/64 of planar velocity (BEFORE the decomposition)
entity->velocityX -= entity->velocityX >> 6;      // [0x48EA1A..0x48EA3E]
entity->velocityY -= entity->velocityY >> 6;      // [0x48EA14..0x48EA44]
int32 vx = entity->velocityX, vy = entity->velocityY;

// 2) slip angle: hull heading minus velocity heading
int32 velHeading = (int32)(atan2((double)vy, (double)vx) * 683565275.5764316); // fpatan [0x48EA32..0x48EA52]
int32 slip = entity->Yaw - velHeading;            // BAM  [0x48EA57..0x48EA5A]

// 3) decompose |v| (clamped) into along/lateral by sin/cos of slip
int32 s22 = sin22(slip);                          // sin(slip*K)*2^22  [0x48EA60..0x48EA72]
int32 c22 = cos22(slip);                          //                   [0x48EA77..0x48EA81]
int32 mag = (int32)min(sqrt((double)vx*vx + (double)vy*vy), 2147418112.0); // [0x48EA86..0x48EAEE]
if (mag > 0x10000) mag = 0x10000;                 // speed clamp 1.0 u/tick [0x48EAF3..0x48EAFE]
int32 lateral = (int32)(((int64)s22 * mag) >> 22);  // v128 — NO rounding bias [0x48EB06..0x48EB12]
int32 along   = (int32)(((int64)c22 * mag) >> 22);  // v129                    [0x48EB14..0x48EB20]
entity->currentSpeed = along;                     // [0x48EB32]  (signed; negative in reverse)

// 4) keel re-application: push 1/32 of the lateral speed along the beam axis
int32 beam = entity->Yaw + 0x3FFFFFC0;            // +90 deg minus 0x40 BAM   [0x48EB22..0x48EB2A]
int32 bs22 = sin22(beam), bc22 = cos22(beam);     // [0x48EB2E..0x48EB4D]
entity->velocityX += (int32)(((int64)(lateral >> 5) * bc22) >> 22);  // X pairs with cos [0x48EB52..0x48EB6B]
entity->velocityY += (int32)(((int64)(lateral >> 5) * bs22) >> 22);  // Y pairs with sin [0x48EB71..0x48EB81]
// Net effect: bleeds ~1/32 of the cross-track velocity back toward the hull axis per
// tick (velocity-heading is dragged toward Yaw); with the 1/64 shed above this is the
// water "grip".

// 5) overspeed drag: only when along-speed exceeds the commanded speed (signed compare)
if (along > brain[136]) {                          // [0x48EB87..0x48EB93]
    entity->velocityX -= entity->velocityX >> 6;   // extra 1/64 shed [0x48EB95..0x48EBA2]
    entity->velocityY -= entity->velocityY >> 6;   //                 [0x48EBA8..0x48EBAF]
}
```

Decompile corrections made here: the sin/cos assignments in the decompile are swapped
and misfolded (velocityX takes the **cos** term of `Yaw+0x3FFFFFC0`, velocityY the
**sin** term — verified against the register writes at 0x48EB6B / 0x48EB81); the drag
shed at step 1 happens before the atan2, so heading/magnitude use the post-drag
velocity; the `+1073741760` in the decompile is exactly **0x3FFFFFC0** (90 deg in BAM
minus 0x40 — with this engine's pi = 0x7FFF8000 convention that evaluates to ~90.003
deg; port the raw constant, do not "fix" it to 0x40000000).

---

### 6. Ground/water contact drags (part D) [orig: 0x48EBB5..0x48ECA6]

```c
if (entity->Flags & 0x8000) {                     // afloat  [0x48EBB5]
    if (entity->attachBone != 0)                  // byte +0x364      [0x48EBBE]
        entity->slideDecay -= 8350;               // 0xFFFFDF62       [0x48EBC7]
    // afloat with no attachBone: NO planar shed, NO slideDecay change here
    // (the platform solve owns buoyancy — see §7).
} else {                                          // not afloat (on land / airborne)
    entity->slideDecay -= 167;                    // 0xFFFFFF59 gravity [0x48EBD9]
    entity->velocityX  -= (entity->velocityX + 4) >> 3;   // 1/8 ground shed [0x48EBD3..0x48EBEB]
    entity->velocityY  -= (entity->velocityY + 4) >> 3;   //                 [0x48EBF1..0x48EBFF]
    entity->modelPtr0  -= (entity->modelPtr0 + 2) >> 2;   // 1/4 yaw-rate shed [0x48EC05..0x48EC13]
}

// look-ahead shore drag: sample terrain at the NEXT position
int32 ground = Terrain_SampleHeightBilinear(entity->Position.X + entity->velocityX,
                                            entity->Position.Y + entity->velocityY); // [0x48EC19..0x48EC2D]
if (ground >= Env_WaterHeightFixed) {             // ground at/above the water plane [0x48EC35]
    entity->velocityX -= (entity->velocityX + 4) >> 3;    // [0x48EC3D..]
    entity->velocityY -= (entity->velocityY + 4) >> 3;
    entity->modelPtr0 -= (entity->modelPtr0 + 2) >> 2;
    // beached full stop: only while actively thrusting "into" the shore this tick
    if (vertical_thrust > 0                       // §4's fwd[2] term  [0x48EC7B]
        && ground - Env_WaterHeightFixed > 30583  // 0x7777 ~ 0.467 u  [0x48EC84..0x48EC8A]
        && entity->groundEntity == NULL) {        //                   [0x48EC91]
        entity->velocityX = 0;
        entity->velocityY = 0;
        entity->modelPtr0 = 0;                    // [0x48EC96..0x48ECA2]
    }
}
```

**What Z does between records** (the rest of part D): inside this function, only
`Position.Z += slideDecay` (§7) and the slideDecay decrements above. There is **no
buoyancy block in the ~150-500 decompile region**: the only water-height uses there are
(a) the effect-anchor clamp `effectPos.Z = max(Position.Z, Env_WaterHeightFixed)` for
the fire/smoke emitters [orig: 0x48D5BD..0x48D5CB — cosmetic], and (b) the
occupant-head-above-water gates on the enter/exit sounds [0x48DB06, 0x48DB41]. The real
buoyancy/righting lives in **`Entity_ProcessPlatformPhysics` @ 0x481870** (10094 bytes),
called UNGATED at 0x48ECE7, i.e. it runs on clients every tick: it references
Env_WaterHeightFixed (0x482A9D/0x482BA5/0x482BEF/0x4834E6), writes Position.Z (5 sites,
e.g. 0x482A8C `add [esi+0Ch],eax`), rewrites slideDecay (0x4819BC/0x483C1D/0x483F25),
writes Yaw/Pitch/Roll (settle/capsize legs 0x483BEA..0x483F63), and is the producer of
Flags 0x8000 (set 0x482CA5 / clear 0x482DB7) and 0x2000 (set 0x483B93 / clear
0x483D55). It never touches modelPtr0 or velocityX/Y. So client-side Z is: slideDecay
integration in this function + the platform solve's water/ground settle — the remote
prediction leg must call the same platform solve (or its port) after integration.
Its internals are OUT OF SCOPE here (not traced; port separately).

---

### 7. Integration and yaw application (parts A/D tail) [orig: 0x48ECA8..0x48ECF5]

```c
entity->Position.X += entity->velocityX;          // [0x48ECA8]
entity->Position.Y += entity->velocityY;          // [0x48ECB7]
entity->Position.Z += entity->slideDecay;         // [0x48ECC0]

// yaw-rate self-decay, ~1/32 rounded toward zero:
int32 r = entity->modelPtr0;
entity->modelPtr0 = r - ((r + 16) >> 5) - (r >> 31);   // [0x48ECC9..0x48ECE1]
//   (r >> 31) is -1 for negative r, so the subtraction rounds the decay toward zero.

Entity_ProcessPlatformPhysics(entity, frameCount);     // [0x48ECE7] buoyancy/attitude/flags

entity->Yaw += entity->modelPtr0;                 // [0x48ECF2] EVERY tick, no gate
```

After this: the wake-anim lerp on brain[115..126] (current vec at +0x1CC..0x1E0 chases
target at +0x1E4..0x1F8; [118] steps by +/-0x2108421 toward [124], snap-copies when
close) [orig: 0x48ECF5..0x48ED76] — animation only, note-only. Then bone-trail FX
(masks 3/4 when afloat every 2nd tick, 1/2 otherwise every 4th tick) and the
movement-sound machine — cosmetic. **W3/W4 ported 2026-09-04:** the shared
authority/client core captures the post-solve pose and water plane on that
even-tick cadence, with W3 driven by brain[136] command speed and W4 by signed
current speed; the fixed-tick presenter updates persistent first-16 userpoint
groups and their two live particle controls. The dry W1/W2 leg remains open.
Tail rebuilds orientationMatrix from `&entity->Position` and sets Flags bit
0x20000 [0x48EF50..0x48EF63].

---

### 8. Interp-block facts the prediction leg depends on (recap, already-ported chase)

- Mirror site [orig: 0x48DDD4..0x48DDF4]: `if (occupantEntity != g_local_player_entity)
  { brain[136] = brain[177]; brain[132] = brain[179]; }` — runs for every watercraft on
  a non-authority machine whose occupant is not the local player, every tick, after the
  interp step and before the motion blocks (same-tick freshness).
- Stale-record coast-down [orig: 0x48DDC0..0x48DDCE]: once interpProgress reaches 128
  (no record applied for 128 ticks), `brain[177] -= (brain[177] + 64) >> 7` per tick —
  the mirrored command decays ~1/128/tick toward zero, so an abandoned boat predicts to
  a stop.
- Snap-threshold detail [orig: 0x48DB9B..0x48DBAC]: interp target recompute selects the
  0x60000 snap radius when brain[177] >= 293, else 0x20000.
- (Local-driver only, NOT this path: 0x48E22E averages `[136] = ([136]+[177])>>1`.)

---

### 9. Ground-family comparison (part F) — decompile-level

Question: does the client-executed subset of `Entity_UpdateVehiclePhysics` (ground)
structurally match `world::tick_vehicle_motor` (engine/runtime/world/vehicle_motor.cpp) minus
the input block — can ground prediction reuse VehicleSystem::tick_motor driven by the mirrored
cmd registers?

**Yes.** The ground client path after the mirror (`v51[136]=v51[177]; v51[132]=v51[179]`,
occupant != local, decomp l.616-622 — identical to the boat's) falls through the same
code the authority runs below the input gate, and that code is exactly what
VehicleSystem::tick_motor ports:

| ground client block (decomp) | VehicleSystem::tick_motor |
|---|---|
| turn blend `minRate + ((turnRate-minRate)*f + 0x8000)>>16`, f = clamp0(0x10000 - speed<<16/playerSpeed) (l.994-1016) | steering-chase block, identical constants |
| steer step `(reg132 - Yaw + 32)>>6` clamp +/-eff (l.1017-1022) | identical |
| `aiState += (4 - 32*delta - aiState)>>3` (l.1023) | m.steer_state, identical |
| `modelPtr0 = (-speed*(aiState>>2)+0x8000)>>16` (l.1029) | m.wheel_rate_bam, identical |
| airborne cmd-opposition (l.1034-1046) | ported (grounded gate) |
| slope factor cos^2(pitch) via table (l.1047-1049) | ported (computed cos22) |
| `rawAccel = (target-speed+16)>>5` + accel/decel clamp tree (l.1050-1153) | ported verbatim (skid legs deferred) |
| `speed += accel`; `abs<48 -> 0`; `accel==0 -> speed=target` (l.1155-1160) | identical |
| velocity from heading dir, `(speed*dir+0x8000)>>16` (l.1623-1656) | ported (simplified frame) |
| `slideDecay -= 324`; dead-vehicle `(v+2)>>2` sheds (l.1659-1665) | gravity ported; dead-shed deferred |
| `Position += vel`; tracked-physics call; `Yaw += modelPtr0` gated on ground contact (l.1681-1687) | ported (m.grounded gate) |

Client-only deltas to account for when reusing it as the prediction leg:

1. **Command source**: brain[136]/[132] come from the mirror every tick (not from
   VehicleSystem::resolve_controller / ai_cmd) — run VehicleSystem::tick_motor with the input block
   bypassed and `m.cmd_speed / m.steer_target_bam` loaded from the mirrored registers;
   include the [177] stale decay (§8) or the boat/vehicle never coasts to rest.
2. LABEL_208 byte-gates run on the client too (decomp l.972-993): the handbrake latch
   (entity[1] byte 973, needs `occupantEntity && (Flags&8) && itemDef->handBrake`) and
   the aim-lock byte (`LOBYTE(aimHeading)` -> `[136]=0`). On a remote client Flags bit
   8 / the latch byte are only maintained by locally-run code, so these are usually
   inert; our port already defers them (D-NET-161) — same deferral is acceptable for
   prediction, flag it in the record.
3. The tire-slip / surface-normal steering legs (huskModel fields, l.1064-1121,
   1244-1557) DO run on the retail client and feed the velocity direction; the
   VehicleSystem::tick_motor simplification is an existing ledgered divergence that prediction
   inherits.
4. speedAccel/currentSpeed/aiState/modelPtr0 evolve purely locally on the client — no
   net override besides the chase; drift is the chase's job (already ported).
5. Blocks NOT to run in prediction (they are inside the authority/local gate):
   AI waypoint drive, pool-1 avoid-brake, boarding-wait stop, stuck check.
6. Client-run cosmetic tails (anim accumulator, wake/dust FX, sound machine,
   `Entity_UpdatePartSpinAccumulator` at l.1846 — attrib&0x40 tail helper, untraced)
   are presentation, not motion.
7. UNVERIFIED (decomp-level only): the garbled euler arg of the ground
   `Math_BuildFixedPointMatrixFromEulerAngles((int*)speedAccel, ...)` (l.1241) is
   presumed `&entity->Position` by the boat's verified codegen pattern (0x48E972);
   confirm at disasm before citing in code.

Boat-vs-ground family deltas (why the boat needs its own motor, not VehicleSystem::tick_motor):
the boat has no speedAccel/target_speed pipeline (thrust adds directly to velocity, and
"speed" is re-derived from velocity each tick); drag is 1/64 exponential + keel lateral
bleed instead of direction-projection; gravity is 167 (vs 324); yaw applies ungated
every tick (vs ground-contact gated); Z rides slideDecay + the platform buoyancy solve
(vs tracked-vehicle wheel solve); the steer ramp cap differs (0x1FFFFFE0 vs ground
0x238E38C0 — input block only).

---

### 10. UNVERIFIED / out-of-scope list

- `Entity_ProcessPlatformPhysics` @ 0x481870 internals (buoyancy spring constants,
  righting torque, the 4-corner suspension): witnessed only as the producer of Flags
  0x2000/0x8000 and the Z/slideDecay/attitude writer (§6). Needs its own grill before
  the Z/attitude part of prediction can be called done.
- Semantics of `attachBone != 0` while afloat (the -8350 slideDecay leg §6): the
  witnessed condition is exact, but WHAT sets attachBone for a watercraft (carried/
  crane state?) was not traced.
- Flags bit 0x2000 = "airborne" and 0x8000 = "afloat" interpretations: inferred from
  the producer sites in the platform solve and the consumer patterns (bone-trail masks
  3/4 on 0x8000; land-drag on !0x8000). Producers witnessed; naming is interpretive.
- brain[177]/[179] writer (the per-record apply) is outside this function — cited as
  the already-ported chase's input, not re-witnessed here.
- Ground-family part F claims are decompile-level per task scope (§9 item 7).
- `Entity_UpdatePartSpinAccumulator` (ground tail) untraced.
- The 0x3FFFFFC0 beam-axis constant is exact; whether the -0x40 bias is intentional
  (compiler fold of `+90deg - 64`) or a source-constant quirk is unknowable — port the
  raw constant.

### 11. Field-write summary for the remote-client path (part E)

Reads: brain[132],[136],[137],[177],[179]; entity Position, Yaw/Pitch/Roll, Flags,
groundEntity, occupantEntity, attachBone, currentSpeed, aiState, velocityX/Y,
slideDecay, modelPtr0; itemDef acceleration/waterSpeed/turnRate/turnRate2;
Env_WaterHeightFixed; terrain height at the look-ahead point.

Writes (motion-relevant): brain[136]<-[177], brain[132]<-[179], brain[177] (stale
decay); entity velocityX/Y, slideDecay, currentSpeed, aiState, modelPtr0, Position
X/Y/Z, Yaw (+= modelPtr0, and the interp steps), Pitch/Roll (carrier follow + platform
solve only), interpProgress, pendingAnimStateId (anim), orientationMatrix, Flags
(0x20000 tail; 0x2000/0x8000 via the platform solve).

### 12. The authority half — input gate, occupant legs, capsize (witnessed 2026-08-06)

Disasm-witnessed (decompile too large to serve; every claim below is from the
instruction stream). Port: `tick_watercraft_motor` +
`AiSystem::watercraft_ai_drive` (`engine/runtime/world/vehicle_motor.cpp` /
`ai_waypoints.cpp`); blocks 12..19 are the client-ported sequence, extracted
verbatim into the shared `watercraft_motor_core`. Field identities pinned this
session from the curated IDB types: entity +0x11E = `Health` (i16), +0x12C =
`MoveOrder` (the input-flags word), +0x130/131/132 = analogX/Y/Z, +0x148 =
`moveTimer`, +0x74 = `CameraOffset.z`, +0x7C = `DcbId`; itemDef +0x8D8 =
`minAI`, +0x180 = `criticalHp`, +0x8E8 = `playerSpeed` DISTINCT from +0x8EC =
`waterSpeed`; AiBrain +0x1FC = `target_ref` [127], +0x200 = `out_speed` [128],
+0x34.. = the `wp_type/wp_channel/...` waypoint block [13..], +0x54 =
`wp_bearing` [21], +0x80 = the per-leg turn budget [32] (IDB name `anim_flag`
is stale here), +0x8C = the budget divisor param [35] (IDB `stored_key_time`).

- **MoveOrder merge** [orig: @ 0x48DE04..0x48DE7B; gate `attrib&0x40 &&
  occupant && occupant->Flags&0x100 && (occupant==local || is_authority)`]:
  if analogX+analogY+analogZ != 0, or MoveOrder bit3 with a nonzero low-3
  direction, set MoveOrder bit 0x10 on the OCCUPANT (the free-look/steer-mode
  latch the player leg reads). Deferred in the port — our input model rebuilds
  MoveOrder from the wire each tick.
- **Capsize drain, authority-only** [orig: @ 0x48DE84..0x48DECD]: when |Roll|
  or |Pitch| exceeds 0x471C7180 (~100°), `Health -= 200` per tick, floored at
  0; at the kill edge `overlayFlags` (+0x178) is zeroed (unmodeled slot).
  Ported in `tick_watercraft_motor`.
- **INPUT gate** [orig: @ 0x48DF7F..0x48DFA2]: no `attrib&0x40` → straight to
  the steer integrator @ 0x48E82C (the core runs on persisted registers).
  Gate pass = `is_authority || occupant == g_local_player_entity`.
- **Entry split** [orig: @ 0x48DFA8..0x48DFCD]: occupant NULL **or entity
  Flags&2** → parked leg @ 0x48E7EE. (A second Flags&2 test @ 0x48DDFA
  earlier jumps dead hulls to the matrix tail — the port early-returns there.)
  Occupied: `moveTimer = 0`; occupant WITHOUT Flags&0x100 (an AI body) → AI
  leg @ 0x48E247; a PLAYER whose head is underwater (`occupant->Position.z +
  CameraOffset.z <= Env_WaterHeightFixed` [orig: @ 0x48DFD3..0x48DFDF]) also
  routes to the AI leg (deferred in the port — player-leg refinement).
- **Player leg** [orig: @ 0x48DFE5..0x48E20F]: forces brain state 22
  [@ 0x48DFF5]; MoveOrder bit6 forces dir=1; bit7 → cmd = waterSpeed, dir=7;
  bit3 → cmd = waterSpeed; else analog: cmd = −(analogX×waterSpeed)>>7 and the
  dominant lateral analog (×0x2EFAA>>1) steers occupant Yaw when !bit4 (plus
  the local camera mirror); bit9 cmd>>=1, bit8 cmd>>=2, bit5 = lights (Flags
  0x80); analog-sum==0 → steer = occupant Yaw (bit4: entity Yaw); dir!=0 ramps
  [137] += 0x16C16C0 cap 0x1FFFFFE0 else [137]=0; the 8-way switch (1..7):
  ±ramp on steer, reverse cases cmd = −cmd>>1, dir 2/6 cmd=0. Ported as the
  shared `stage_player_vehicle_input` (already carried the boat ramp cap).
  Tail [orig: @ 0x48E20F..0x48E242]: the LOCAL driver on a NON-authority
  machine averages `[136] = ([177]+[136])>>1` — the client reconcile the
  D-NET-196 port already models.
- **AI-driver leg** [orig: @ 0x48E247..0x48E756], ported as
  `watercraft_ai_drive`:
  1. state 22 → 16 hand-back [@ 0x48E247..0x48E24D];
  2. `[135] ← target_ref[127]` (unmodeled slot), `[136] ← out_speed[128]`
     capped at waterSpeed [@ 0x48E254..0x48E279];
  3. the minAI crew clamp [@ 0x48E27F..0x48E2C7]: `minAI > 1` and no body near
     the entry bone (`Entity_IsBoneInProximity @ 0x434F90`, 8 u) and
     `Entity_CountMountedEntities @ 0x435970` < minAI → `Health =
     min(Health, criticalHp)` — undercrewed AI hulls bleed to critical
     (deferred, D-NET-161: def minai/criticalHp unparsed in traits);
  4. waypoint refresh when the budget [32] is spent and the waypoint block
     [13] is live: `AIWaypoint_UpdateTarget @ 0x457380` on `&brain[13]`, then
     budget = `(|Yaw − wp_bearing[21]| / ((brain[35]>>15) + 32)) << 4` —
     truncate-divide THEN shift, unlike the ground leg's `32*err/denom`
     [@ 0x48E2D5..0x48E31C];
  5. Δ = clamp(wp_bearing − Yaw, ±budget) [@ 0x48E322..0x48E33C]; when
     `turnRate2<<6 < budget`, cmd ×= 0.75 per tier |Δ| > 15°/30°/45°
     (0x0AAAAAA0/0x15555540/0x1FFFFFE0, round-half-up) [@ 0x48E33E..0x48E3EA];
  6. steer = Yaw + Δ — NO ground-style `Δ>>3` term [@ 0x48E3F0..0x48E3F5];
  7. slip counter-steer [@ 0x48E3FB..0x48E577]: motion = atan2(velY,velX)→BAM;
     corr = `(sin22(Yaw − motion) × min(|v|, 1.0)) >> 22` (sin at the 2^22
     scale, dbl 4194304.0 @ 0x7C3600; no rounding bias), `steer += corr << 14`;
     |corr| tiers 0x800/0x1000/0x2000/0x3000 damp cmd ×0xC000/0x8000/0x6000/
     0x4000 (each round-half-up, compounding);
  8. the pool-1 avoid brake [@ 0x48E577..0x48E756] — instruction-identical to
     the ground block @ 0x48bd8f (shared `vehicle_avoid_brake` in the port);
  9. the boarding-wait hold [@ 0x48E75B..0x48E7EC]: if
     `Entity_CanEnterVehicle @ 0x435480` and any live unmounted pool-0 AI has
     `aiRuntime[+0x94] == 125` with `[+0x98] == DcbId` → steer = Yaw, cmd = 0,
     ramp = 0, lights off (deferred with the boarding think, D-NET-161).
- **Parked leg** [orig: @ 0x48E7EE..0x48E81E]: steer = Yaw, cmd = 0, ramp = 0,
  `AI_CheckVehicleStuckState @ 0x465290` (deferred), lights off, state 22.
  Both legs fall into the steer integrator @ 0x48E82C (§3, the ported core).

Deferrals that stayed with D-NET-161 at the time: the fire-FX leg (health is
ported in section 11), the [135] mirror, and the wake-anim lerp. The
every-8th-tick groundEntity refresh [@ 0x48D51F] + deck-carrier follow are
PORTED (section 15, `vehicle_contact.cpp`, phase `(current_tick + 36*DcbId) & 7`;
the "deferred" label that stood here was stale at the 2026-09-08 review).
Ported 2026-09-01: the MoveOrder merge (the
occupant's own word, wire-visible in the echo), the submerged-driver cut
(`watercraft_driver_submerged` — a body with no derived eye height keeps the
wheel), the minAI clamp (`AiSystem::apply_min_ai_crew_clamp`), the boarding-wait
hold, and the stuck check (`AiSystem::check_vehicle_stuck`); ctest
`watercraft_client_motor` (`run_submerged_driver_hands_to_ai_leg`) and
`vehicle_mount`. The same submerged-driver cut sits in all four ground-template
movers (cveh `@ 0x48B9A0..0x48B9AC` -> `@ 0x48BC12`, ctan `@ 0x489579..0x489585`
-> `@ 0x4897DB`, cbik `@ 0x484AC6..0x484AD2` -> `@ 0x484DB8`, cbot
`@ 0x48DFD3..0x48DFDF` -> `@ 0x48E247`; catv and ctrn use the cveh mover); the
port applied it only to the boat until 2026-09-22
(`vehicle_mount::test_submerged_player_driver_takes_the_ai_leg`).

### 1.13 The AIR authority half (witnessed + ported 2026-09-01)

The `(is_authority || occupant == local)` gate's other arm of the aircraft mover
`@ 0x490310`, decompiled this session and ported as `AiSystem::chel_ai_drive`
(ai_waypoints.cpp) + the authority legs of `aircraft_client_tick`
(vehicle_motor_air.cpp). Block map, in retail order:

1. **Health machine** [@ 0x4903F0..0x490480, on the `(tick + 36*DcbId) & 0x3F`
   cadence — `lea eax,[eax+eax*8]` @0x490334 then `lea ecx,[ecx+eax*4]`
   @0x490340; the decompiler's `tick[9*DcbId]` was pointer scaling, not a
   nine-tick stagger (corrected 2026-09-08)]: above `criticalHp` (+0x180) the
   hull regens `nonCriticalRegen` (+0x184) while `Health < healthMax − regen`
   [@ 0x4903f9..0x49042d]; at or below it the hull BURNS `criticalDrain`
   (+0x182) per cadence [@ 0x490434..0x490480] and, airborne (Flags 0x2000)
   with `[524] − ground > 1 u`, `Yaw −= 2886390` every tick — the tail-rotor
   spiral [@ 0x49048e..0x4904be; the pilot's own Yaw follows unless
   free-looking — `@0x4904a6..0x4904d0`, PORTED in section 16 as
   `turn_pilot_view(world, *pilot, -2886390)`]. Smoke (`Health < healthMax/4`)
   and fire emitters + the every-64th-tick fire sound are presentation seams.
2. **Rotor gate** [@ 0x490592..0x4905a6]: `updated = Health > 0 && !(Flags & 1)
   ? Entity_UpdateHeloRotorSpin(...) : 0`, whose return is `!is_authority ||
   speed >= 0x0CCCCCC0`; at LABEL_328 [@ 0x491ca7..0x491cc2] a false `updated`
   parks every command (`[548] = 0, [524] = ground − 0x2000, [544] = [540] = 0,
   [528] = Yaw`) — a cold helicopter commands nothing through the ~18 s
   spool-up. Port: `m.part_spin.speed >= kRotorSpeedMax` read one tick late
   (our part-anim machine runs at the mover tail).
3. **The AI leg** (an occupant WITHOUT Flags 0x100, or a submerged pilot):
   state 14 → 7 [@ 0x491590]; `[540] = brain[127]; [544] = brain[128]`
   [@ 0x4915a3..0x4915a9 — brain[127] has no live SM writer, brain[128] is the
   SM mover's out-speed]; the minAI clamp [@ 0x4915b2..0x4915f2]; the AIR turn
   budget on the budget refresh `[32] = 8 * (|Yaw − brain[21]| /
   ((brain[35] >> 15) + 32))` — divide THEN ×8 [@ 0x49160d..0x491663]; then,
   only with a node (`brain[16]`, nulled when both `brain[14]`/`brain[15]` are
   zero [@ 0x491576..0x49159a]) and state 7 [@ 0x491671], the **flight block**
   [@ 0x491672..0x491998]: node Z floored at `ground − 0x4000`; planar
   distance/bearing (fpatan) and planar speed, zero lengths → 1
   [@ 0x491694..0x4916dd]; `v104 = speed * dz / dist` (64-bit), `[524] = Z +
   4*v104`, `slideDecay = (v104 + slideDecay) >> 1` [@ 0x49175c..0x491796];
   `[548] = [524] − ground − 0x4000`, negative → `[548] = 0, [524] = ground −
   0x2000` [@ 0x4917a5..0x4917c9]; beyond 6 u planar a ZERO `[540]`/`[544]`
   takes `132 * sin/cos(err) >> 22` [@ 0x4917f3..0x491834]; two ground samples
   (self @ 0x491845, the node @ 0x491855): en route (node > 6 u above its
   ground, or planar > 6 u) a target under `ground + bound/4` lifts to `+16 u`
   with `[544] ×= 1/8` [@ 0x491862..0x4918aa]; else, landing under that floor,
   `[544] ×= 1/8`, `X/Y += (node − pos) >> 6`, `[524] = ground − 0x2000`
   [@ 0x4918b0..0x4918fc]; `[528] = Yaw + clamp(err, ±[32])` [@ 0x491928..
   0x49195c]; `[544] ×= |cos err|` twice [@ 0x491970..0x49198a]. Then the
   pool-1 separation damp on `[544]` [@ 0x4919fc..0x491b67 — the ground brake's
   ellipse/cone/id-frame factor; the air walk gates on `entity+0x1C == 1`] and
   the boarders hold [@ 0x491b7a..0x491c01]. The parked block (no pilot / dead)
   [@ 0x491be6..0x491c6d] zeroes the registers, calls the stuck check
   [@ 0x491c5e] and clears Flags 0x80.
   The pilot/AI/parking block is entered only for PlayerControl (def+0x54 &
   0x40). A non-drivable aircraft skips it; on authority it seeds brain[131]
   to sampled ground + brain[137], then joins the rotor/engine gate. A client
   keeps the replicated target [orig: Entity_UpdateAircraftPhysics @ 0x490310,
   gate @ 0x490ef6, authority branch @ 0x491da7, store @ 0x491dbd].
   `chel_ai_drive` preserves the collective on that path and
   `aircraft_client_tick` applies the ground-relative target. Ctest
   `aircraft_client_motor` exercises the real vehicle pass with a gunner-only
   aircraft and contrasts the drivable, unpiloted parking path.
4. **Engine flag** [@ 0x491dfd..0x491e11]: `Flags 0x80 = [548] != 0` — port:
   `VehicleMotorState::net_climb` carries [548] on the authority (the client
   path keeps folding it into `net_alt_target`).
5. **Drains**: submerged (Flags 0x8000) `Health −= 100`/tick [@ 0x4924e2..
   0x492503]; `|Roll| or |Pitch| > 0x471C7180` → `Health −= 200`/tick
   [@ 0x492637..0x49266f]; both floor at 0 and zero `+0x178` at the kill edge
   (unmodeled slot).

Residuals: the flare scan over the weapon-slot list [@ 0x4911xx], the pilot's
analog collective, the pilot's Yaw follow of the burn spiral, the FX/sound
seams, and `brain[127]`'s savegame-only producer. ctest `vehicle_mount`
(`test_helo_ai_flight`: climbs and closes on the node, routeless hold,
cold-rotor park; `test_helo_authority_health`: regen, burn + spin, crash
drain).

---

## §2 The aircraft mover (CHel + cpln) — client-executed subset

Source: `Entity_UpdateAircraftPhysics` @ 0x490310 .. 0x492777 (retail Jointops.exe,
imagebase 0x400000). Decompilation cross-checked against a full instruction-level dump
(a session disassembly dump, 2476 instructions); every FPU-garbled block
below was reconstructed from the disassembly, not the decompile. Companion to
`watercraft_client_spec.md` — same conventions (16.16 fixed positions/velocities, 32-bit
BAM angles, `sar` shifts, `(a*b + 0x8000) >> 16` = imul/add 0x8000/adc/shrd rounded
product, `(a*b) >> 22` = imul/shrd with NO bias, idiv truncates, 62 Hz tick), same trig
constant set (§2 of the watercraft spec applies verbatim: dbl_7C3608 =
1.4629627251502471e-09 BAM→rad, dbl_7C19D8 = 683565275.5764316 rad→BAM, dbl_7C3600 =
2^22, flt_7C19E0 = 2147418112.0 ftol clamp; `sin22/cos22/bam_of/mag` as defined there).

Scope: the blocks a CLIENT executes for a REMOTE (non-local-driver, non-authority)
aircraft — everything OUTSIDE the drive-INPUT gate at 0x490EF3..0x490F14
(`attrib&0x40 && (is_authority || occupant == g_local_player_entity)`), from the interp
block's register mirror to position/attitude integration. The per-record chase (interp)
is recapped for ordering and for the register seeding it owns (which for aircraft
includes the ALTITUDE register — see §5/§9).

**Family finding (task 2): the plane family (`cpln`) has NO mover of its own.** The
class-physics table rows (tag + pad + callback, 12 bytes/row, table label `aArti` @
0x82AC7C):

- `CHel` row callback @ 0x82AC9C = 0x490310 `Entity_UpdateAircraftPhysics` (direct).
- `cpln` row callback @ 0x82ACE4 = **0x45D6F0**, which was undefined code in the
  align-16 gap after `Entity_UpdateAttachedChildren` (ends 0x45D6EE). Defined as a
  function during this session: it is a **5-byte thunk `jmp Entity_UpdateAircraftPhysics`**
  (IDA auto-named it `j_Entity_UpdateAircraftPhysics`; decompiles as a pure tail-call,
  attributes: thunk). It is NOT a dispatch stub with logic and NOT a separate mover.

So helicopters and planes run the SAME function, byte for byte. Everything in this spec
covers both families; per-family behavior comes only from ItemDef data (§0 field list —
acceleration/playerSpeed/turnRoll/speedPitch/weathervane/climbSpeed/turnRate) and the
`boundRadius >= 15.0` climb-gain branch (§5). Port ONE air mover, parameterized by the
def. (Contrast: `cveh`/`ctank`/`cbike`/`cbot`/`catv` rows go through
`Entity_DispatchPhysics_*` stubs @ 0x48EF90..0x48F060 — see §12.)
IDB writes this session: define_func @ 0x45D6F0 + an append_comment there; idb saved.
No renames, no type changes.

**2026-08-21 — #553 folded.** The `air_attitude.h` "attitude integrator" that
PR #553 staged was a duplicate of the live `aircraft_client_tick`
(`vehicle_motor.cpp` — the sideslip roll/pitch feedback `:2126-2152`, the
1/512 self-level, the asymmetric `>>4`/`>>3` rate damp with the `+ (v >> 31)`
negative-only term `:2204-2241`, the def rate ceiling ×192426, the 15°/tick
clamp 178956960); the header and its test were deleted and the two distinctive
pins retargeted at the live mover in `tests/world/aircraft_client_motor_test.cpp`.

---

### 0. Pointer identities and offset map

`brain` = the vehicle's own AiBrain, `AiBrain *` at entity+0x64 [orig: `mov ebx,[esi+64h]`
@ 0x49032C] — identical identity to the watercraft's (the decompile's `v4`). If NULL the
whole function skips to the tail matrix build [orig: jz @ 0x490371].

AiBrain dword index -> byte offset, role in this function:

| idx | offset | role | witness |
|-----|--------|------|---------|
| [4]   | +0x10  | cur_state (14 = idle default; **forced 7/14 on clients**, §3) | 0x49037D, 0x490987 |
| [5]   | +0x14  | pend_state (13 = falling-wreck on death, unless cur 15/13) | 0x4903A1 |
| [11]  | +0x2C  | Z probe offset subtracted before every ground sample (semantics untraced, §13) | 0x4903AD, 0x4909D0 |
| [32]  | +0x80  | AI turn cache (AI leg only) | 0x491788 (region) |
| [115..126] | +0x1CC..+0x1F8 | rotor/attitude anim lerp block (anim-only, §8 tail) | 0x492694.. |
| [131] | +0x20C | **target altitude, absolute Z 16.16 — the climb servo chases it** | 0x491D8B, 0x491E1D |
| [132] | +0x210 | **steer target (BAM heading the yaw servo chases)** | 0x491CCB |
| [135] | +0x21C | **commanded LATERAL speed, 16.16 u/tick (drives roll tilt)** | 0x491D39 |
| [136] | +0x220 | **commanded FORWARD speed, 16.16 u/tick (drives pitch tilt)** | 0x491D33 |
| [137] | +0x224 | hover height (target Z minus ground); authority's engine-flag source; on a client written but only read by the engine-off override | 0x490A36, 0x491DFD |
| [177] | +0x2C4 | net-received forward speed (record apply writes; decays when stale) | 0x490C76 |
| [178] | +0x2C8 | net-received lateral speed (ditto) | 0x490C8A |
| [179] | +0x2CC | net-received steer heading (never decayed) | 0x490CB8 |
| [118]/[121..126] | +0x1D8/+0x1E4.. | rotor-tilt anim chase regs (anim-only) | 0x492694 |
| byte +0x318 | (792) | sound/flare latch bits (input leg only) | decomp l.1390 |

GamePlayerEntity fields consumed by the client motion path (IDB names; same layout as
the watercraft spec except where noted):

| offset | field | role |
|--------|-------|------|
| +0x00 | boundRadius | 16.16; climb-gain select (>= 0xF0000 = 15.0), AI separation radius |
| +0x04/08/0x0C | Position.X/Y/Z | |
| +0x10/14/18 | Yaw/Pitch/Roll | BAM; `&Position` doubles as the 6-dword euler block |
| +0x20 | itemDef | |
| +0x24 | Flags | bit 1 dead; **bit 0x80 = engine-on/collective-active (producer: authority @ 0x491E05/0x491E11 from brain[137] != 0; consumers on client: §3 record-apply branch, §4a engine-off override, §8 rate shed)**; bit 0x100 (on occupant: "in control"); bit 0x2000 airborne (producer: the 0x47EF10 solve); bit 0x8000 in-water (ditto); bit 0x20000 matrix-valid (tail) |
| +0x28 | groundEntity | carrier (deck riding) |
| +0x64 | AiBrain* | |
| +0x7C | DcbId | stagger key = current_tick + 36*DcbId [orig: 0x490316..0x490340] |
| +0x80..0x94 | savedLivePose + bodyHeading/Pitch/Roll | full 6-dword pose at tick start [0x490326..0x49036B] |
| +0x98 | velocityX | 16.16 u/tick |
| +0x9C | velocityY | |
| +0xA0 | slideDecay | **vertical velocity (climb rate)** |
| +0xA4 | modelPtr0 | **yaw rate** (BAM/tick) |
| +0xA8 | modelPtr1 | **pitch rate** (BAM/tick) — aircraft-only use |
| +0xAC | modelPtr2 | **roll rate** (BAM/tick) — aircraft-only use |
| +0xB4 | orientationMatrix | rebuilt at tail |
| +0x11E | Health (int16) | |
| +0x170 | occupantEntity | pilot |
| +0x190 | mountHandles[10] | seat sweep |
| +0x234/238/23C | smoothTargetPos | record target -> becomes per-tick step after recompute |
| +0x240 | smoothTargetHeading | record target -> becomes per-tick yaw step (/20) |
| +0x27C/27E | interpProgress / interpStepBucket | |
| +0x2A4 | cached average ground height (refreshed every 8th stagger tick, §1.2) | |
| +0x45C | view-tilt register (cosmetic, §1 note) | |
| +0x470 (word) | rotor spool accumulator (anim) | |
| +0x472 (byte) | bit 0 = "in flight" (Z - 5.0 >= ground), drives spool | |

ItemDef fields read by this mover (verified exhaustive over the disasm — notably NO
turnRate2 (+0x928) and NO waterSpeed: **there is no speed-fraction turn blend in the air
family**):

| offset | field | role here |
|--------|-------|-----------|
| +0x8D8 | minAI | AI leg only (multi-crew wait) |
| +0x8E0 | acceleration | **tilt-command clamp** (§5, << 12) |
| +0x8E8 | playerSpeed | input-leg 8-dir command magnitude (gated; not client) |
| +0x90C | turnRoll | roll-rate cap, x 192426 (§6) |
| +0x910 | speedPitch | pitch-rate cap, x 192426 (§6) |
| +0x914 | weathervane | input-leg pedal-yaw rate (gated; not client) |
| +0x920 | climbSpeed | vertical velocity clamp: up = climbSpeed, down = 2x (§8) |
| +0x924 | turnRate | steer-step clamp (§4) |

---

### 1. Per-tick block order for a remote aircraft on a client

All of this is ONE call of Entity_UpdateAircraftPhysics per world tick:

1. savedLivePose/bodyHeading etc. <- full 6-dword pose [0x490326..0x49036B]; stagger
   key = current_tick + 36*DcbId [0x490316..0x490340].
2. Brain state defaults (cur_state 14 if 0 [0x49037D]; pend_state 13 on
   dead-or-health<=0 unless cur is 15/13 [0x490384..0x4903A1]). Every-8th stagger tick
   (`(key & 7) == 0`): Position.Z -= brain[11]; `Entity_CalcAverageGroundHeight(entity,0,0)`
   -> entity+0x2A4; Z restored [0x4903A8..0x4903D8]. This cached ground height ("ground"
   below) is the base of every altitude decision.
3. Damage band FX + regen/drain — authority-gated where damaging. One client-run
   motion-adjacent piece: while in the burning health band, if the OCCUPANT's MoveOrder
   bit 0x10 is clear and the aircraft is airborne and brain[131] - ground > 0x10000,
   `occupant->Yaw -= 2886390` per tick [0x4904A6..0x4904D7] — the burn spiral applied to
   the rider's view (the vehicle's own `Yaw -= 2886390` twin @ 0x49049F is
   authority + every-64-tick only). Cosmetic for remote prediction.
4. Carrier follow: if groundEntity != NULL, Position += carrier tick delta + full
   fixed-point parent-rotation re-seat of Position/Yaw/Pitch/Roll
   [0x4905BC..0x49095B]. Runs on clients (helicopter on a moving deck/vehicle).
5. `!is_authority` interp block [0x49095E..0x490C98] — §3. Includes the client state
   forcing, the record-apply register seeding (brain[131]/[137]!), the chase stepping,
   and the stale decay of brain[177] AND brain[178].
6. **Register mirror** [0x490C9E..0x490CCA]: if `occupantEntity != g_local_player_entity`:
   `brain[136] = brain[177]; brain[135] = brain[178]; brain[132] = brain[179];`
   (every tick, including unoccupied aircraft).
7. Dead check: `Flags & 2` -> skip to the tail matrix build [0x490CD0 -> 0x49274C].
8. Collective-jump MoveOrder merge — gated occupant==local || authority
   [0x490CDA..0x490DBD]; skipped remote.
9. Seat sweep (attrib&0x40): stale occupantEntity/mountHandles cleared
   [0x490DBD..0x490E64]. Runs on clients.
10. View-tilt register +0x45C update [0x490E66..0x490EF3]: with occupant, chases
    `occupant->Pitch - entity->Pitch` at 1/8 (+4 bias), step quantized (step > 1924260
    -> 3848520; < -1924260 -> -3848520), then clamped to [-298261600, 0]; without
    occupant the same quantized `(tilt+4)>>3` step is ADDED (no clamp — witnessed
    as-is, see §13). Not consumed by the motion state in this function; note-only.
11. INPUT/AI gate [0x490EF3..0x490F14] — **remote clients jump 0x490F0E -> 0x491C95**,
    skipping the whole occupant-input leg (8-dir thrust jumptable @ 0x490F80, analog,
    collective 0x40/0x80 bits, flare release), the unmanned-authority leg, and the AI
    leg (waypoint drive, speed governor by heading deviation, pool-1 separation with
    the tick-seeded jitter, wait-to-board) — all inside 0x490F16..0x491C87. Non-drivable
    (`!attrib&0x40`) aircraft: authority does brain[131] = ground + brain[137]
    [0x491DA7..0x491DC3]; a client goes straight to the same 0x491C95.
12. Client engine-off override (LABEL_305) [0x491C95..0x491CC2] — §4a.
13. Shared physics core (LABEL_307 = 0x491CC8, runs for authority AND client):
    steer/yaw servo §4 -> tilt command §5 -> climb servo §5 -> airborne aero block §6
    OR grounded shed block §7 -> common tail §8 (zero-cmd snap, climb clamps,
    engine-off shed, water shed, position integration, yaw-rate decay, 0x47EF10
    ground/water solve, attitude-rate clamps, attitude integration).
14. Flight flag +0x472 bit0 = (Position.Z - 0x50000 >= ground) [0x492676..0x492691];
    rotor-tilt anim chase on brain[115..126] (anim-only) [0x492694..0x492703]; rotor
    spool word +0x470 += / -= 0x63E toward 0xFFFF / 0 per the flag [0x492703..0x49274A].
15. Tail: `Math_BuildFixedPointMatrixFromEulerAngles(&entity->Position,
    entity->orientationMatrix)` (push esi+4; push esi+0B4h — same call shape as the
    watercraft tail; the decompile's `(entity+180, entity+180)` is garbage);
    `Flags |= 0x20000` [0x492752..0x492776].

---

### 2. Trig conventions

Identical constant set and idioms to watercraft spec §2 (same dbl_7C3608/dbl_7C19D8/
dbl_7C3600/flt_7C19E0 loads, fpatan for atan2, ftol2_sse, sqrt clamp). Not repeated.

---

### 3. Interp block + register mirror (the prediction contract's inputs)

[orig: 0x49095E..0x490CCA], non-authority only.

- **State forcing** [0x490971..0x490987]: if not dead, `cur_state = occupant ? 7 : 14`
  every tick (branchless neg/sbb/and 0xFFFFFFF9/add 0xE).
- **Record-apply target recompute** (when interpProgress == 0; the record apply outside
  this function resets interpProgress) [0x49098A..0x490C24]:
  - **Altitude register seeding** [0x490998..0x490B33]: if `Flags & 0x80` (engine on,
    as replicated): sample ground at the TARGET position (Position temporarily set to
    smoothTargetPos, Z minus brain[11], Entity_CalcAverageGroundHeight, restore), then
    `brain[137] = smoothTargetPos.Z - groundAtTarget; brain[131] = smoothTargetPos.Z;`
    [0x4909A2..0x490A45]. Engine off: `brain[131] = smoothTargetPos.Z - 0x4000;
    brain[137] = 0;` [0x490B16..0x490B33]. **This is how the client's altitude-hold
    servo (§5) gets its setpoint — the record's absolute Z.**
  - Snap radius 0xA0000 (655360), or 0x20000 when brain[177] < 293 AND brain[178] < 293
    [0x490A4D..0x490A67]. (Boat: 0x60000, [177] only.)
  - Distance is **3D**: sqrt(dx^2+dy^2+dz^2) over (smoothTarget - savedLivePose), ftol
    clamp [0x490A6C..0x490AC5].
  - dist > snap: hard snap Position.X/Y/Z AND Yaw from the targets, zero the four step
    registers and interpStepBucket [0x490ACE..0x490B11]. dist < 0x2AAA (10922): steps
    zeroed, bucket 0, yaw step = (smoothTargetHeading - Yaw + 10)/20 (idiv-by-20 via
    0x66666667 magic, truncating) [0x490B38..0x490B64, 0x490C13..0x490C24].
  - Else bucket ladder on dist [0x490B69..0x490BD6]: 8 (<0x4000), 10 (<0x5555), 15
    (<0x8000), 20 (<0x10000), 25 (<0x20000), else 32; steps =
    (delta + bucket/2)/bucket per axis (signed idiv) [0x490BD6..0x490C0D]; yaw step =
    (smoothTargetHeading - bodyHeading + 10)/20 [0x490C0A, 0x490C13..0x490C24]. The
    +0x234..+0x240 registers are REUSED: targets before recompute, per-tick steps after.
- **Application** [0x490C2A..0x490C61]: while interpProgress < 20: Yaw += yawStep(+0x240);
  while < interpStepBucket: Position.X/Y/Z += steps (Z included).
- **Stale coast-down** [0x490C64..0x490C98]: once interpProgress >= 128 (progress++
  stops there): `brain[177] -= (brain[177] + 64) >> 7` AND
  `brain[178] -= (brain[178] + 64) >> 7` per tick — BOTH mirrored commands decay
  ~1/128/tick; brain[179] (steer) and brain[131] (altitude) do NOT decay, so an
  abandoned helicopter predicts to a hover at the last record's altitude.
- **Mirror** [0x490C9E..0x490CCA]: `if (occupantEntity != g_local_player_entity)
  { brain[136] = brain[177]; brain[135] = brain[178]; brain[132] = brain[179]; }` —
  after the interp step, before the motion core (same-tick freshness), including
  occupant == NULL.
- (Local-driver only, NOT this path: 0x491546..0x491568 averages
  `[136] = ([136]+[177])>>1` and `[135] = ([135]+[178])>>1`; steer is not averaged.)

### 4a. Client engine-off override (LABEL_305) [orig: 0x491C95..0x491CC2]

Runs on every client tick just before the core, replacing the input block:

```c
if ((entity->Flags & 0x80) == 0) {         // engine off (replicated flag)
    brain[137] = 0;
    brain[131] = ground - 0x2000;          // ground = entity+0x2A4 cache
    brain[136] = 0;                        // overrides the mirror this tick
    brain[135] = 0;
    brain[132] = entity->Yaw;
}
```

So the mirrored commands only drive motion while the replicated engine flag is set;
with it clear the client predicts a powered-down settle: the climb servo (§5) chases
ground - 0x2000 (the 0x47EF10 solve stops it at contact), yaw holds, tilt commands are
zero. (The authority's counterpart producer `brain[137] ? Flags|=0x80 : Flags&=~0x80`
@ 0x491DFD..0x491E18 is authority-only; clients consume the replicated bit.)

---

### 4. Steer / yaw servo [orig: 0x491CC8..0x491D27]

Aircraft yaw is a SECOND-ORDER chase (rate integrates a clamped step), unlike the
boat/ground first-order rudder-filter model. No speed-fraction turn blend; flat clamp.

```c
int32 step = (brain[132] - entity->Yaw + 8) >> 4;        // 1/16 of error, +8 bias [0x491CCB..0x491CDF]
int32 tr = itemDef->turnRate;                            // +0x924  [0x491CD6]
if (step >  tr) step =  tr;                              // [0x491CE2..0x491CEC]
if (step < -tr) step = -tr;                              // [0x491CF0..0x491CF8]
// (step is also saved as the rotor-tilt anim chase rate, §8 tail  [0x491CE4])

entity->yawRate/*+0xA4*/ += (step + 4) >> 3;             // [0x491CFC..0x491D03]
if (yawRate >  abs(step)) yawRate =  abs(step);          // [0x491D09..0x491D17]
if (yawRate < -abs(step)) yawRate = -abs(step);          // [0x491D1D..0x491D27]
```

Yaw is additionally pushed by the airborne weathervane term (§6) and integrated at the
tail: `Yaw += yawRate` [0x492619..0x49261F] after the +/-178956960 clamp, every tick,
no ground gate. yawRate self-decays 1/32 round-toward-zero right before the ground
solve: `yawRate -= ((yawRate + 16) >> 5) + (yawRate >> 31 as -1 for neg)`
[0x49252B..0x492545] — identical idiom to the boat's modelPtr0 decay.

---

### 5. Thrust model: tilt commands + climb servo [orig: 0x491D2D..0x491E2F]

**There is no direct thrust-to-velocity add.** Commanded speeds tilt the airframe
(pitch/roll rates), and the airborne block (§6) converts tilt into planar acceleration.
The vertical axis is a first-order altitude-hold servo on brain[131].

```c
// tilt command: commanded speeds push the attitude RATES (note the minus signs:
// positive forward cmd = nose-down pitch rate)
int32 acc  = itemDef->acceleration;                      // +0x8E0  [0x491D2D]
int32 fwd  = brain[136] << 11;                           // [0x491D41]
int32 lat  = brain[135] << 11;                           // [0x491D47]
int32 cap  = acc << 12;                                  // [0x491D44]
if (fwd >  cap) fwd =  cap;   if (fwd < -cap) fwd = -cap;    // [0x491D4A..0x491D59]
if (lat >  cap) lat =  cap;   if (lat < -cap) lat = -cap;    // [0x491D5B..0x491D65]
entity->pitchRate/*+0xA8*/ -= fwd;                       // [0x491D67]
entity->rollRate /*+0xAC*/ -= lat;                       // [0x491D6D]

// climb servo — THE vertical mover. brain[131] is the absolute target altitude,
// seeded on the client by the record apply (§3) / engine-off override (§4a).
if (entity->boundRadius >= 0xF0000)                      // 15.0  [0x491D73..0x491D85]
    entity->slideDecay += (brain[131] - Position.Z + 0x100) >> 9;   // heavy: 1/512 [0x491E1D..0x491E2F]
else
    entity->slideDecay += (brain[131] - Position.Z + 0x80) >> 8;    // light: 1/256 [0x491D8B..0x491D9C]
```

There is NO gravity constant anywhere in the air mover: descent is the servo chasing a
lower brain[131] (engine off: ground - 0x2000). The servo output is damped x15/16 in
the airborne block, shed on ground/water, and clamped to [-2*climbSpeed, +climbSpeed]
(§8) before integrating `Position.Z += slideDecay`.

---

### 6. Airborne aerodynamic block (Flags & 0x2000) [orig: 0x491E35..0x4922BC] — FPU-reconstructed

```c
// 1) velocity heading, slip, decomposition (2D planar; NO 1.0 magnitude cap — only
//    the ftol overflow clamp; unlike the boat)
int32 velHeading = bam_of(atan2((double)velY, (double)velX));   // fpatan [0x491E52..0x491E66]
int32 slip = entity->Yaw - velHeading;                          // [0x491E6D..0x491E72]
int32 s22 = sin22(slip);                                        // [0x491E76..0x491E8D]
int32 c22 = cos22(slip);                                        // [0x491E8B..0x491E98]
int32 mag = (int32)min(sqrt((double)velX*velX + (double)velY*velY), 2147418112.0); // [0x491E9C..0x491ECE]
int32 lateral = (int32)(((int64)s22 * mag) >> 22);   // cross-track speed [0x491ED2..0x491EDE]
int32 along   = (int32)(((int64)c22 * mag) >> 22);   // along-track speed [0x491EE4..0x491EF0]

// 2) lateral governor -> roll rate: only on cross-track overspeed
if (abs(lateral) > abs(brain[135]))                             // [0x491EF7..0x491F10]
    entity->rollRate += 8*brain[135] - 8*lateral;               // [0x491F12..0x491F2A]

// 3) forward governor -> pitch rate AND direct pitch, only on along overspeed;
//    strong correction when it reduces |Pitch|, weak otherwise
if (abs(along) > abs(brain[136])) {                             // [0x491F30..0x491F4E]
    int32 e = along - brain[136];
    if (abs(entity->Pitch + 32*e) < abs(entity->Pitch)) {       // [0x491F54..0x491F74]
        entity->pitchRate += 16*e;  entity->Pitch += 32*e;      // [0x491F76..0x491F85, 0x491FA5]
    } else {
        entity->pitchRate += 4*e;   entity->Pitch += 4*e;       // [0x491F8A..0x491F9F, 0x491FA5]
    }
}

// 4) weathervane: yaw dragged toward the velocity heading by the cross-track speed
int32 wv = (int32)(((int64)abs(lateral >> 6) * (velHeading - entity->Yaw) + 0x8000) >> 16);
entity->Yaw += wv;                                              // [0x491FA8..0x491FD9]
if (occupant && (occupant->analogX + analogY + analogZ) != 0)   // bytes +0x130..0x132
    brain[132] += 6*wv;                                         // [0x491FDC..0x492000]

// 5) tilt -> acceleration, in the yaw frame; exact 22-bit coefficients:
int32 aFwd = -(int32)((1169LL * sin22(entity->Pitch)) >> 22);   // 0x491 [0x492006..0x492031]
int32 rollK = (abs(lateral) < abs(brain[135])) ? 501 : 334;     // 0x1F5 / 0x14E [0x49204F..0x49207F]
int32 aLat = (int32)(((int64)rollK * sin22(entity->Roll)) >> 22);

// 6) vertical loss from tilt (climbing costs, negative terms quartered first):
int32 zp = (int32)(((int64)along * sin22(entity->Pitch)) >> 22);
if (zp < 0) zp >>= 2;
entity->slideDecay += zp >> 2;                                  // [0x49207F..0x4920AE]
int32 zr = (int32)(((int64)lateral * sin22(entity->Roll)) >> 22);
if (zr < 0) zr >>= 2;
entity->slideDecay -= zr >> 3;                                  // [0x4920A8..0x4920DC]

// 7) rotate into world by Yaw and integrate into planar velocity, then damp:
int32 sy = sin22(entity->Yaw), cy = cos22(entity->Yaw);         // [0x4920D6..0x4920FE]
entity->velocityX += ((int64)aFwd*cy >> 22) + ((int64)aLat*sy >> 22);   // [0x492102..0x49211E]
int32 vy = entity->velocityY + ((int64)aFwd*sy >> 22) - ((int64)aLat*cy >> 22); // [0x492124..0x492152]
entity->velocityX = (1019*entity->velocityX + 512) >> 10;       // 0x3FB/0x400 [0x492146..0x49218C]
entity->velocityY = (1019*vy + 512) >> 10;                      // [0x49215E..0x492192]
entity->slideDecay = (240*entity->slideDecay + 128) >> 8;       // 15/16 [0x492152..0x492178]
if (occupant && !(occupant->Flags & 0x100)) {                   // pilot not in control [0x49217E..0x49219A]
    entity->velocityX = (1019*velocityX + 512) >> 10;           // second damping pass
    entity->velocityY = (1019*velocityY + 512) >> 10;           // [0x4921A3..0x4921C7]
}

// 8) attitude self-righting (1/512 per tick on BOTH the angle and its rate):
entity->pitchRate -= (Pitch + 0x100) >> 9;  entity->Pitch -= (Pitch + 0x100) >> 9;  // [0x4921CD..0x4921F6]
entity->rollRate  -= (Roll  + 0x100) >> 9;  entity->Roll  -= (Roll  + 0x100) >> 9;  // [0x4921F0..0x492210]
// 9) rate damping 1/16 round-toward-zero:
rollRate  -= ((rollRate  + 8) >> 4) + sign_correction;          // [0x492213..0x492227]
pitchRate -= ((pitchRate + 8) >> 4) + sign_correction;          // [0x492228..0x492246]
// 10) def caps (skipped when the def field is 0):
if (itemDef->speedPitch) clamp pitchRate to +/-(speedPitch * 192426);  // +0x910 [0x49224C..0x49227A]
if (itemDef->turnRoll)   clamp rollRate  to +/-(turnRoll   * 192426);  // +0x90C [0x492280..0x4922B6]
```

(`sign_correction` = the `- (x >> 31)` term — subtracting -1 for negative x rounds the
decay toward zero; same idiom throughout.)

Decompile corrections made here: the decompile swapped which ftol results were
sin22/cos22 of the slip (var_B0 IS sin22, var_A4 IS cos22 — 0x491E86 ftol happens
before the fcos at 0x491E8B), and left the yaw-frame products as garbled v240/v251;
the disasm pins velocityX pairing with (aFwd*cos + aLat*sin) and velocityY with
(aFwd*sin - aLat*cos).

---

### 7. Grounded block (else of §6) [orig: 0x4922C1..0x492378]

```c
velocityX -= ((velocityX + 2) >> 2) + sign_correction;   // shed 1/4 [0x4922C1..0x4922E5]
velocityY -= ((velocityY + 2) >> 2) + sign_correction;   // [0x4922DC..0x4922FD]
slideDecay -= ((slideDecay + 8) >> 4) + sign_correction; // shed 1/16 [0x492303..0x492312]
if (slideDecay < 0) slideDecay >>= 2;                    // downward vel quartered [0x492318..0x49231D]
yawRate  -= ((yawRate  + 4) >> 3) + sign_correction;     // shed 1/8 [0x492323..0x49235F]
rollRate -= ((rollRate + 4) >> 3) + sign_correction;     // 1/8 (uses the value captured
pitchRate-= ((pitchRate+ 4) >> 3) + sign_correction;     //  at 0x491D79/7F) [0x492338..0x492365]
brain[131] += (Position.Z - brain[131]) >> 2;            // target altitude re-seats onto
                                                         // actual Z, 1/4 [0x49235C..0x492378]
```

---

### 8. Common tail: snaps, clamps, integration [orig: 0x49237E..0x492776]

```c
// zero-command velocity snap (BOTH commands must be zero — cf. boat's single cmd):
if (brain[136] == 0 && brain[135] == 0) {                // [0x49237E..0x49238E]
    if (abs(velocityX) < 384) velocityX = 0;             // 0x180 [0x492390..0x4923A2]
    if (abs(velocityY) < 384) velocityY = 0;             // [0x4923A8..0x4923BA]
}
// vertical clamps:
if (slideDecay >  itemDef->climbSpeed)     slideDecay = climbSpeed;      // +0x920 [0x4923C0..0x4923D1]
if (slideDecay < -2*itemDef->climbSpeed)   slideDecay = -2*climbSpeed;   // [0x4923D7..0x4923E9]
// engine-off attitude-rate shed:
if (!(Flags & 0x80)) { rollRate, pitchRate -= ((x+8)>>4) + sign; }       // [0x4923EF..0x492426]
// in-water shed:
if (Flags & 0x8000) {                                    // [0x49242C]
    velocityX, velocityY, slideDecay -= ((x+4)>>3) + sign;   // 1/8 [0x492438..0x492489]
    yawRate, rollRate, pitchRate     -= ((x+8)>>4) + sign;   // 1/16 [0x492483..0x4924D4]
    // (authority-only: health -= 100/tick in water [0x4924DA..0x492503])
}
// INTEGRATION (planar + vertical BEFORE the solve, attitude AFTER):
Position.X += velocityX;                                 // [0x49250A..0x492510]
Position.Z += slideDecay;                                // [0x492513..0x49251F]
Position.Y += velocityY;                                 // [0x492519..0x492528]
yawRate -= ((yawRate + 16) >> 5) + sign_correction;      // 1/32 self-decay [0x49252B..0x492545]
was_airborne = (Flags >> 13) & 1;                        // captured pre-solve [0x49253D]
loc_47EF10(entity, 0);                                   // ground/water contact solve,
                                                         // UNGATED — runs on clients [0x49254E]
// (touchdown thud sound if was_airborne && !airborne && slideDecay < -3000 — cosmetic
//  [0x492556..0x4925A6])
clamp yawRate, pitchRate, rollRate to +/-178956960;      // 0xAAAAAA0 [0x4925A9..0x492601]
Pitch += pitchRate;                                      // [0x492607..0x49260D]
Roll  += rollRate;                                       // [0x492610..0x492616]
Yaw   += yawRate;                                        // [0x492619..0x49261F]
// (authority-only: inverted-flight damage when |Roll| or |Pitch| > 0x471C7180
//  [0x49262A..0x49266F])
flight flag +0x472 bit0 = (Position.Z - 0x50000 >= ground);   // 5.0 u [0x492676..0x492691]
// rotor-tilt anim chase brain[118] +/- steer-step toward brain[124], snap-copy
// [121..126]->[115..120] when close (0x2108421 threshold) — anim-only [0x492694..0x492703]
// rotor spool word +0x470 +/- 0x63E (1598) toward 0xFFFF/0 per the flag — anim/sound
// [0x492703..0x49274A]
Math_BuildFixedPointMatrixFromEulerAngles(&entity->Position, entity->orientationMatrix);
Flags |= 0x20000;                                        // [0x492752..0x492776]
```

---

### 9. What drives Z on a remote client between records (explicit answer)

Unlike the boat (whose in-function Z is gravity + the platform solve), the helicopter's
Z IS part of the mover, and the client runs all of it:

1. The interp chase steps Position.Z directly while interpProgress < interpStepBucket
   (§3) — the 3D distance/ladder includes Z.
2. **The climb servo** (§5): `slideDecay += (brain[131] - Z + bias) >> (8|9)` every
   tick, where brain[131] (absolute target altitude) was seeded by the record apply
   from smoothTargetPos.Z (engine on: exactly; engine off: -0x4000) and is NOT decayed
   when records go stale — an abandoned aircraft holds altitude.
3. The airborne tilt-loss terms and the x15/16 damping (§6), the grounded/water sheds
   (§7/§8), and the [-2*climbSpeed, +climbSpeed] clamp (§8).
4. `Position.Z += slideDecay` (§8), then the 0x47EF10 contact solve (ground clamp,
   Flags 0x2000/0x8000 production).
5. There is no gravity constant; descent is the servo chasing a lower setpoint
   (engine-off: ground - 0x2000 via §4a). There is no climb REGISTER mirrored from the
   net ([178] is LATERAL speed, not climb) — altitude is predicted purely from the
   record-seeded brain[131] plus the interp chase.

---

### 10. Blocks depending on unavailable infrastructure (residual scoping)

- **`loc_47EF10` (UNDEFINED function in the IDB, 0x47EF10..~0x481866, sitting between
  `Entity_ProcessTrackedVehiclePhysics` and `Entity_ProcessPlatformPhysics`)** — the
  air-family ground/water contact + suspension solve, called ungated at 0x49254E.
  Witnessed only as: consumer of Env_WaterHeightFixed (0x48015A/0x48019D/0x4801D3/
  0x480D05), producer of Flags 0x8000 (0x48030B/0x48032B) and 0x2000 (0x480ED5, tests
  at 0x4802EC/0x480CF9/0x481089/0x4817FA), and Position.Z writer (0x480150/0x480EC9/
  0x48110A/0x4814C4/0x481849). Internals untraced — port separately (it is the
  aircraft's counterpart of the boat's Entity_ProcessPlatformPhysics; a sibling IDB
  names the equivalent call site "Entity_ProcessVehicleSuspension"). Without it the
  port must stub: ground clamp at the sampled ground height, and the two flags.
- **Terrain**: `Entity_CalcAverageGroundHeight` (5-point weighted average, water-aware)
  feeds the every-8th-tick ground cache (entity+0x2A4) AND the record-apply
  brain[137] computation (§3) AND the flight flag. A client port needs a terrain height
  query; the record-apply ground sample can be skipped if brain[137] is unused (it is,
  on clients — but then the engine-flag semantics must still come from the replicated
  Flags 0x80).
- **Replicated Flags bits** 0x80 (engine), 0x2000 (airborne), 0x8000 (water): 0x80
  arrives only via replication (producer is authority-gated); 0x2000/0x8000 are
  REPRODUCED locally by the 0x47EF10 solve each tick — ported 2026-08-01: rows with
  collision boxes run `aircraft_contact_solve` and produce both bits locally; boxless
  rows keep the replication-fed derivation (which then lags the record cadence).
- Carrier follow (§1.4) needs the carrier entity's saved-pose deltas — available only
  if the carrier is itself simulated that tick (same ordering dependency as the boat).
- The AI leg, pool-1 separation, wait-to-board, stuck check, flare release, collective
  jump: all inside the (authority || local-driver) gate — NOT residuals, simply absent
  from the client subset (the authority half is witnessed + ported in §1.13,
  2026-09-01; the flare release and the collective jump stay deferred there).

---

### 11. cpln (plane) family — precise statement

The `cpln` class-table callback (0x82ACE4) is the newly defined thunk
`j_Entity_UpdateAircraftPhysics` @ 0x45D6F0 -> `jmp Entity_UpdateAircraftPhysics`
@ 0x490310. **The plane mover IS the helicopter mover** — one function, both rows;
there is no plane-specific code path inside it (no class-tag branch exists in the
function; the only data-driven splits are the ItemDef air params and the
boundRadius >= 15.0 climb-gain branch). Everything in §§0-10 applies to cpln verbatim.
Do NOT port a second mover: parameterize the one air mover by the def. (Fixed-wing
flight "feel" in JO comes from cpln defs' acceleration/climbSpeed/turnRate/speedPitch/
turnRoll values, not from code.)

---

### 12. Part-3 quick answers (decompile-level, as tasked)

**cbike — `Entity_UpdatePlayerInfantryMovement` @ 0x483FE0** (sole caller: the class
table's cbike row via `Entity_DispatchPhysics_cbike` @ 0x48EFF0, arg2=0). Despite the
IDB name, this is NOT an infantry motor: it is the GROUND-VEHICLE template. It has the
full vehicle scaffolding — interp block with the brain[177] stale decay (l.649), the
two-register mirror `brain[136]<-[177]; brain[132]<-[179]` gated occupant != local
(l.652-656), the (authority || local-driver) input gates — and below the gate the
ground family's exact steering chain: turn blend `minRate + ((turnRate-minRate)*f +
0x8000)>>16` with `f = clamp0(0x10000 - speed<<16/playerSpeed)` (l.1103-1129), steer
step `(brain[132]-Yaw+32)>>6` clamped, `aiState += (4 - 32*delta - aiState)>>3`,
`modelPtr0 = (-speed*(aiState>>2)+0x8000)>>16` (recomputed when !airborne), cos^2(pitch)
slope factor via the same table, and the ground speedAccel pipeline
`(target-speed+16)>>5` + accel/decel clamp tree feeding `currentSpeed += speedAccel`
with the |speed|<48 snap. Bike-specific deltas: wheelie/stoppie state bytes (the `+25`
accel leg), aim-brake full stop, velocity = currentSpeed x a normalized 3D direction
vector INCLUDING Z (slope-aligned, `slideDecay = speed*dirZ` — not the car's flat
heading frame), a hard speed clamp +/-24576, low-speed velocity halving + modelPtr
zeroing below |speed| 4096, the decompile shows a DOUBLED integration step
(`Position.X += 2*velX; Y += 2*velY; Z += slideDecay twice` l.2052-2055 — verify at
disasm before porting), `Entity_ProcessLightVehiclePhysics(entity, ..., 1)` as its
contact solve, and `Yaw += modelPtr0` full when grounded / `>>2` when airborne.

**Mounted movers 0x486A50 / 0x488AB0** (`Entity_UpdateMountedInfantryMovement` /`_0`).
Same verdict: both are the ground-vehicle template, not infantry-motor models — same
interp + [177] decay + `[136]<-[177]/[132]<-[179]` mirror (l.592-596), same 8-dir
playerSpeed command with walk>>1/crouch>>2 modifiers, the local-driver
`[136]=([136]+[177])>>1` averaging (l.760), identical turn blend/steer chase/aiState
low-pass (l.1026-1035), cos^2(pitch) slope factor, speedAccel pipeline, velocity =
speed x normalized 3D direction vector, `Entity_ProcessWheeledVehiclePhysics(entity,..,1)`
contact solve, and the same low-speed halving tail. They differ from each other mainly
in tuning (0x486A50 gravity `slideDecay -= 1250` vs 0x488AB0 `-= 250`, plus small brake
legs). Wiring: **0x488AB0 is the `ctank` row's actual mover** (called from
`Entity_DispatchPhysics_ctank` @ 0x48F000, arg2=0 — the "MountedInfantry" name is an
auto-analysis misnomer); **0x486A50 has zero code AND zero data xrefs** — an orphaned
near-twin (dead code; do not port).

**Is the GROUND motor core (`world::tick_vehicle_motor`) a reasonable interim
prediction stand-in for cbik?** Yes, with ledgered residuals. The command/steering core
is literally the same skeleton with the same constants (mirror registers, [177] decay,
turn blend, steer chase, aiState filter, modelPtr0 formula, speedAccel pipeline,
<48 snap), so a mirrored-register-driven VehicleSystem::tick_motor will track a remote bike's
speed and heading correctly between records — and the per-record chase corrects the
rest. What it will get wrong (bias, not divergence): the bike's velocity rides a
3D slope-aligned direction vector (so predicted hills behave like flat ground), the
apparent 2x integration step (verify!), the wheelie/stoppie and aim-brake legs, the
+/-24576 clamp and low-speed halving, and the light-vehicle contact solve. Do NOT
reuse it for the mounted movers' families without the same caveats; and note all three
use ItemDef playerSpeed (+0x8E8) as the command magnitude like the car, not waterSpeed.

---

### 13. UNVERIFIED / out-of-scope list

- `loc_47EF10` internals (§10) — only its outputs witnessed. Its exact extent is also
  unconfirmed (0x47EF10..~0x481866 contains retns at 0x47F182/0x48181B/0x481866; it may
  be more than one undefined function).
- brain[11] (+0x2C) semantics — the Z offset subtracted before every ground sample
  (both the every-8-tick cache and the record-apply target sample). Value producer
  untraced; port as an opaque def/brain offset.
- The view-tilt register +0x45C: witnessed exactly (§1.10) including the unoccupied
  branch's `tilt += (tilt+4)>>3` with NO clamp (a latent runaway for tilt < 0 — the
  occupied branch clamps to [-298261600, 0]); its consumer is outside this function
  (presumed the mounted-gun/present pass). Note-only.
- Flags 0x80 = "engine-on" naming is interpretive (producer: brain[137] != 0 on
  authority; consumers as in §4a/§8). Producers/consumers witnessed; name is ours.
- brain[177]/[178]/[179] and smoothTarget/interpProgress writers (the per-record apply)
  are outside this function — cited as the ported chase's inputs, not re-witnessed.
- Part-12 claims are decompile-level per task scope (esp. the cbike 2x integration
  step and the mounted movers' gravity constants).
- The occupant burn-spiral (§1.3) writes occupant->Yaw ungated — witnessed, but its
  interaction with the occupant's own pose replication was not traced (cosmetic).
- Whether any retail cpln def sets boundRadius >= 15.0 (selecting the 1/512 climb gain)
  was not checked — data question, not code.

### 14. Field-write summary for the remote-client path

Reads: brain[4],[11],[131],[132],[135],[136],[137],[177],[178],[179]; entity
boundRadius, Position, Yaw/Pitch/Roll, Flags, groundEntity, occupantEntity (+ its
Flags/MoveOrder/analog bytes/Pitch), DcbId, savedLivePose block, velocityX/Y,
slideDecay, modelPtr0/1/2 (yaw/pitch/roll rates), smoothTargetPos/Heading,
interpProgress/StepBucket, ground cache +0x2A4, +0x45C, +0x470/+0x472; itemDef
acceleration/turnRoll/speedPitch/climbSpeed/turnRate (+ controlBone for the seat
sweep); Env via Entity_CalcAverageGroundHeight; the 0x47EF10 solve's world.

Writes (motion-relevant): brain[136]<-[177], brain[135]<-[178], brain[132]<-[179],
brain[177]/[178] (stale decay), brain[131]/[137] (record apply seeding, §4a override,
grounded re-seat), brain[4] (state forcing); entity velocityX/Y, slideDecay,
modelPtr0/1/2, Position.X/Y/Z, Yaw (interp step + weathervane + rate), Pitch/Roll
(governors + self-right + rates + carrier follow), interpProgress, savedLivePose
block, mountHandles/occupant (seat sweep), +0x45C, +0x470/+0x472, orientationMatrix,
Flags (0x20000 tail; 0x2000/0x8000 via the 0x47EF10 solve; NOT 0x80 — client never
produces the engine flag).

---

## §3 The boat platform solve

Source: `Entity_ProcessPlatformPhysics` @ 0x481870 .. 0x483FCF (retail Jointops.exe,
imagebase 0x400000, 10094 bytes). Decompilation cross-checked against a full
instruction-level dump (a full instruction-level disassembly dump, session artifact); every FPU-garbled
block below was reconstructed from the disassembly. Helpers witnessed at decompile
level: `Entity_ComputeCollisionForces` @ 0x462150 (fully reconstructed),
`Entity_ApplyBreathingOscillation` @ 0x463BE0 (fully reconstructed),
`Entity_ComputeSuspensionOrientation` @ 0x46C8E0 and
`Entity_ProcessWheeledVehicleSuspension` @ 0x46B140 (contract + key paths),
`Entity_BuildOrientationFromVectors` @ 0x458DF0 / `Entity_RebuildOrientationMatrixFromAxes`
@ 0x4632E0 (contract).

This is the D-NET-196 stand-in target: our `watercraft_client_tick` keeps a level hull
and chase-only Z; retail derives Z/Pitch/Roll every tick from THIS solve, on clients too.

Conventions: positions 16.16 fixed (1.0 = 0x10000 u), angles 32-bit BAM with the
engine's half-turn = 0x7FFF8000, `>>` = x86 `sar`, `(a*b + 0x8000) >> 16` is the exact
`imul; add 0x8000; adc; shrd 16` round-half-up product, `idiv` truncates toward zero.
`cos22(bam) = (int32)(cos(bam * 1.4629627251502471e-9) * 4194304.0)` (dbl_7C3608 /
dbl_7C3600; ftol magnitudes clamped by flt_7C19E0 = 2147418112.0). Tick = the 62 Hz
world tick. "q" is defined in §3.

---

### 0. Caller, entry conditions, and the two dispatch modes

- ONE caller: `Entity_UpdateWatercraftPhysics` @ 0x48D480 calls it at 0x48ECE7, EVERY
  tick, unconditionally (afloat, beached, or airborne), AFTER `Position += velocity/
  slideDecay` and the modelPtr0 self-decay, BEFORE `Yaw += modelPtr0` [orig:
  Entity_UpdateWatercraftPhysics @ 0x48ECA8..0x48ECF2]. It runs on CLIENTS for remote
  boats (the call is not gated on authority or occupancy).
- arg2 `applyWaterPhysics` = the watercraft mover's own mode arg passed through
  [orig: `push [esp+slotEntry]` @ 0x48ECD8..0x48ECDF]: 1 from the `cbot` class-table
  dispatcher [orig: Entity_DispatchPhysics_cbot @ 0x48EFA3], 2 from the generic
  dispatcher (`catv` amphibians and anything routed by Flags 0x8000) [orig:
  Entity_DispatchPhysicsUpdate @ 0x48F02C]. Inside the solve it is used ONLY as a
  boolean gate on the water-submersion computation (§8) [orig: 0x482AA5/0x482ADA/
  0x482B0F/0x482B42]. Both real modes are nonzero.
- The generic dispatcher selects the watercraft mover BY Flags bit 0x8000 — i.e. this
  function's afloat flag chooses the family mover for amphibians next tick [orig:
  Entity_DispatchPhysicsUpdate @ 0x48F01E..0x48F02C].
- Return value: the second `Entity_ComputeCollisionForces` result (0 if no contact
  pass ran); the watercraft caller ignores it [orig: 0x483FB9 `mov eax,[esp+var_20C]`].

### 1. Field map (all offsets byte offsets on GamePlayerEntity, size 0x388)

| offset | role in this function | witness |
|--------|----------------------|---------|
| +0x00 | boundRadius | 0x482494 |
| +0x04/08/0C | Position X/Y/Z | throughout |
| +0x10/14/18 | Yaw/Pitch/Roll (BAM) | 0x483BE3..0x483BF6 |
| +0x20 | itemDef | 0x481880 |
| +0x24 | Flags (bits 0x10, 0x40, 0x2000, 0x8000, 0x4000000 — §11) | |
| +0x30 | graphicModel; +0xB0 in it = modelData (probe geometry source, §3) | 0x481A85, 0x481AC3 |
| +0x5C | busy/hold counter — sleep denied while > 0 | 0x481933 |
| +0x64 | AiBrain*; brain+0x220 = commanded speed (mirrored net register brain[136]); brain byte +0x318 = sound latches (bits 4,0x10 here) | 0x48188A, 0x483741, 0x482407 |
| +0x80/84/88 | savedLivePose (position at tick start, written by the caller) | 0x48193F.. |
| +0x98/9C | velocityX/Y — READ ONLY here | 0x4818B9 |
| +0xA0 | slideDecay = vertical velocity | 0x481919, 0x483C15, 0x483F25 |
| +0xA4 | modelPtr0 = yaw rate — read only (sleep gate) | 0x4818DD |
| +0xA8 | modelPtr1 — read only here (sleep gate); pitch-override angle consumed by the solver when byte+0x2EF set | 0x4818F5, [orig: Entity_ComputeSuspensionOrientation @ 0x46E0D7] |
| +0xAC | modelPtr2 = roll-override angle; written §9 (±0x2468AC legs, zeroed) | 0x483920, 0x483D65 |
| +0x11E | Health (int16) | 0x481883, 0x4823C7 |
| +0x158 | anim/scale ptr — selects scaled matrix build | 0x481BEE |
| +0x170 | occupantEntity (not read here; listed for context) | — |
| +0x178 | cleared with Health on hard-collision kill | 0x4823CE |
| +0x1BC/+0x1C0 | proximity list base / count (entity-entity collision) | [orig: 0x462170, 0x462561] |
| +0x1C4 | updateCallback — compared against 0x48F010 to pick the draft formula (§8) | 0x482B6E |
| +0x1CC | smoke emitter handle | 0x4818AA, 0x48352B |
| +0x29C | currentSpeed — collision sheds write it (§6) | 0x4821E7.. |
| +0x2C4/2C8/2CC/2D0 | per-corner drop accumulators (250/tick ramp state, §10) | 0x48369E.., 0x483F70 |
| +0x2D4/2D8/2DC/2E0 | per-corner probe Z spring offsets — READ only here (writer not in this fn; the suspension solvers own per-wheel state) | 0x481D06/0x481D8F/0x481DCE/0x481E35 |
| +0x2EC (byte) | yaw-from-fit enable: nonzero → the solve writes Yaw too; also read by the solver | 0x483BE3, 0x483D2F, 0x483F50 |
| +0x2EF (byte) | pitch/roll-override active (solver rebuilds matrix from +0xA8/+0xAC when set); zeroed in many legs here | 0x48390D, 0x483D5E |
| +0x2F0 (byte) | capsized latch (set when up.z < 0 in contact) | 0x48348B |
| +0x2F2 (byte) | "upright & unlocked" indicator (up.z > 0x1000 && byte+0x2EC==0) | 0x4819EF, 0x481A9F |
| +0x300 | bob amplitude = min(q/16, 352), recomputed every solve | 0x4821BF..0x4821D1 |
| +0x304.. | bob oscillation state block; +0x318 = float phase (radians) | 0x483A15, 0x483A07 |
| +0x364 (byte) | at-rest latch (arms the bob) | 0x483A00, 0x483A29 |
| +0x365 (byte) | porpoise latch (bow-dip cycle) | 0x48382B, 0x483864 |
| +0x3B8 | last-pushed-by-bigger-vehicle tick (with Flags 0x40) | 0x481A6B, 0x481A7F |
| +0x3D4 | airborne tick counter (++ while 0x2000, else 0) | 0x483FAF/0x483FC8 |
| +0x400 | burn emitter handle | 0x48356C, 0x4835D4 |
| +0x464 (dword), +0x470 (word) | anim state resets | 0x483980/0x483986 |
| +0x472 (byte) | bit 1 = planing/bow-up anim+physics bit | 0x483765, 0x48388D |

ItemDef fields:

| offset | role |
|--------|------|
| +0x182 | smoke/fire-off health threshold (Health <= it → emitters released) [0x481890, 0x483549] |
| +0x17C | healthMax; >>2 = burn-on threshold [0x483591] |
| +0x196 (byte) | unitType: 3 = dies outright on hard collision; 8/7 = large/med fire FX [0x4823BC, 0x4835A2] |
| +0x864 | sound bank; +0x68 = collision one-shot [0x482420..0x482437] |
| +0x8F4 | slope-soft angle (BAM) → threshold_soft = cos22 [0x481BCD..0x481BE7] |
| +0x8F8 | slope-hard angle (BAM) → threshold_hard = cos22 [0x481BAC..0x481BC8] |
| +0x908 | mass; also boat weight class: <= 1 = light [0x482105, 0x482476] |
| +0x91C | collision speed-shed shift base ("torque") [0x4821ED, 0x4822AC, 0x4822DA] |
| +0x92C..+0x948 | tuning params clamped in place every call: 0x92C∈[0,30], 0x930∈[0,20], 0x934∈[0,10], 0x938∈[0,10], 0x93C∈[0,10], 0x948∈[0,100] [0x481ACC..0x481BA3] |
| +0x934 | bow-lift pitch threshold scale (§9) |
| +0x938 | bow-lift amount scale (§9) |
| +0x93C | porpoise exit threshold scale (§9) |

Globals: `Env_WaterHeightFixed` @ 0x26C6454 — the FLAT water plane (single 16.16
scalar; there is no wave field — "waves" are the §10 heave bob).
Terrain: `Terrain_SampleHeightBilinear` @ 0x6067B0 (16.16 world XY → ground Z).

### 2. Sleep path (early-out) [orig: 0x4818B9..0x481A5C]

If ALL of: velocityX == 0, velocityY == 0, currentSpeed == 0, modelPtr0 == 0,
modelPtr2 == 0, modelPtr1 == 0 [0x4818B9..0x4818FB]; !(Flags & 0x2000) &&
!(Flags & 0x40) [0x481904..0x481913]; -500 < slideDecay < 0 [0x48191F..0x48192D];
holdCounter(+0x5C) <= 0; Position.X/Y unchanged from savedLivePose and
|Position.Z - saved.Z| < 500 [0x48193C..0x481972]:

- If the four corner accumulators (+0x2C4..+0x2D0) are all zero [0x481978..0x4819A6]:
  `Position.Z -= slideDecay; slideDecay >>= 1;` (undo this tick's Z integration and
  halve the residual — the boat converges to rest with no solve) [0x4819AC..0x4819BC];
  rebuild the pose matrix from `&Position`, extract up (row2);
  `byte+0x2F2 = (up.z > 0x1000 && byte+0x2EC == 0)` [0x4819C2..0x4819F8]; if authority:
  Flags bit 0x10 = byte+0x2F0 [0x4819FF..0x481A22]; return 0.
- Else (accumulators pending): clear Flags bit 0x40, rebuild matrix + up row, and FALL
  THROUGH to the full solve [0x481A33..0x481A5C].

Wake coupling: a larger vehicle colliding with a sleeping boat sets its Flags 0x40 +
tick at +0x3B8 (see §5 entity loop), which defeats this early-out; the latch
self-clears after 200 ticks [orig: 0x481A5E..0x481A7F].

If graphicModel == NULL: only the byte+0x2F2 / authority-0x10 updates run; return 0
[orig: 0x481A85..0x481ABE].

### 3. Probe geometry — 7 points from the model boxes [orig: 0x481CB4..0x48209D]

`modelData = *(graphicModel + 0xB0)`. Two boxes read (16.16 model space;
pairs are (lo,hi)):
box1: Z = ([0x28],[0x2C]), X = ([0x30],[0x34]), Y = ([0x38],[0x3C]);
box2 (footprint): X = ([0x40],[0x44]), Y = ([0x48],[0x4C]).

**Provenance WITNESSED 2026-08-12** — `modelData` IS the runtime collision
block (the same `gpm_model[44]` pointer `Entity_InitFromModel @ 0x40dc30`
reads as collision data), and the collision-model builder derives both boxes
at load time [orig: `Threedi_BuildCollisionModelFromChunks @ 0x5b3bf0`, tail
@ 0x5b4455..0x5b45db]:

- The box1 **Z pair ([0x28]/[0x2C]) is the CMDL header bbox Z pair verbatim**
  (runtime dwords [10]/[11], copied raw from the chunk @ 0x5b3d06/@ 0x5b3d18)
  — NOT the deepest collision vertex. Wheeled hulls author their origin at
  wheel contact with the CMDL floor at ~0 (DTruck1: `+0.01`, while its wheel
  COBJ volumes dip to `−0.334`), so a solve resting pads at `[0x28] + q`
  puts the ORIGIN on the terrain.
- The box1 **X/Y pairs fold the type-1 (solid) BVOL extents** of every volume
  whose min-Z lies below `CMDL minZ + zspan/2` (the lower HALF; type filter
  @ 0x5b44c4, thresholds @ 0x5b446e/@ 0x5b4477). Volumes walk through the
  per-COBJ runs only — unowned trailing BVOLs stay dead.
- The box2 **footprint X/Y pairs fold the same extents** over volumes whose
  min-Z lies below `CMDL minZ + zspan/8` (the bottom EIGHTH — the wheel/skid
  volumes), then clamp each side to at least `q + 0x2000` from the origin
  with `q = (box1 Y span) >> 2` (@ 0x5b4563..0x5b45b0). The half box stores
  unclamped; both folds start from ±0x40000000 sentinels that survive when
  nothing qualifies.

Port: `threedi_3di3_collision_probe_boxes` (engine/formats/threedi/
threedi_3di3.h), consumed by the mission collision resolve into
`VehicleTraits`; pinned by ctest `threedi_collision_3di`
(`test_collision_probe_boxes_follow_the_witnessed_folds`). The earlier
stand-in (union of per-COBJ AABBs on all axes) floated every wheeled hull by
its below-origin wheel depth — the user-visible SP parked-truck float on
00TRa (~0.33 u for DTruck1/2). Ledgered **D-VEH-1**, minted-and-closed
2026-08-12. A model the derivation rejects (no CMDL Z span / no lower-half
type-1 volume) keeps zeroed traits boxes and therefore the solves'
terrain-clamp stand-in — the same observable as retail's sentinel-boxed hull,
whose solve-active test also fails.

```c
q  = (m[0x3C] - m[0x38]) >> 2;              // beam/4 — corner probe radius [0x481CF4]
zb = m[0x28] + q;                           // keel Z + q                  [0x481D13]
// 4 corner probes (model space), k-th gets spring_k = dword at +0x2D4+4k:
p0 = { m[0x44]-q, m[0x4C]-q, zb + spring0 } // bow-A    [0x481D06..0x481D48]
p1 = { m[0x44]-q, m[0x48]+q, zb + spring1 } // bow-B    [0x481D8F..0x481DAB]
p2 = { m[0x40]+q, m[0x48]+q, zb + spring2 } // stern-B  [0x481DCE..0x481E12]
p3 = { m[0x40]+q, m[0x4C]-q, zb + spring3 } // stern-A  [0x481E35..0x481E79]
// midline probe radius:
rm = min( ((m[0x2C]-m[0x28])>>1) - 0x4000,  // halfheight - 0.25
          ((m[0x3C]-m[0x38])>>1) - 0x1000 );// halfbeam  - 0.0625   [0x481EA3..0x481EB9]
if (rm < 0x2000) rm = 0x2000;               // floor 0.125          [0x481EDB..0x481EE3]
// 3 midline probes along the length at mid-beam, just below the deck:
lx = m[0x34] - m[0x30];  ymid = m[0x38] + ((m[0x3C]-m[0x38])>>1);  zt = m[0x2C] - rm;
p4 = { m[0x30] +   lx>>2 , ymid, zt }       // [0x481EE8..0x481F3E]
p5 = { m[0x30] + 3*lx>>2 , ymid, zt }       // [0x481F56..0x481F99]
p6 = { m[0x30] +   lx>>1 , ymid, zt }       // [0x481FE7..0x482013]
```

All 7 are transformed to WORLD space (rotation + translation) by
`Math_FixedPointTransformPoint22` with the matrix built from `&Position` (the 6-dword
{Pos.XYZ, Yaw, Pitch, Roll} block; scaled variant if +0x158 or itemDef+0x1B8 set)
[orig: 0x481BEE..0x481C48, transforms at the cites above]. Radii array: r0..r3 = q,
r4..r6 = rm [0x481D80, 0x481E0E, 0x481E72, 0x481FA4, 0x482008, 0x482056].
Also saved: halfbeam hb = (m[0x4C]-q)-(m[0x48]+q), halflength hl =
(m[0x44]-q)-(m[0x40]+q) [0x48202B..0x48205D], and
`v210 = -(m[0x28] + q + spring0)` (the corner-0 model-Z offset, negated — used by the
afloat test §8) [0x481D1C..0x481D2B].

### 4. Terrain probe & force computation — Entity_ComputeCollisionForces @ 0x462150

Called twice: first with `&hit_entity_out`, then (after the position push) with NULL
[orig: 0x4820A4, 0x482818]. Full reconstruction of the terrain loop (boat passes
per_wheel_distances = NULL):

```c
// per probe i: P = (X,Y,Z) world, r = radius_i; forces[i] = {0,0,0} pre-zeroed
if (entity->Flags & 0x800000) skip;                    // terrain-collision opt-out [0x462214]
h_xm = Terrain(X-r, Y); h_xp = Terrain(X+r, Y);
h_ym = Terrain(X, Y-r); h_yp = Terrain(X, Y+r);        // 4 samples [0x462246..0x46227E]
rel  = Z - (h_yp>>2) - (h_ym>>2) - (h_xp>>2) - (h_xm>>2);  // height over avg [0x4622A3]
if (h_xm+r < Z && h_xp+r < Z && h_ym+r < Z && h_yp+r < Z) skip;  // clear by > r [0x4622D2]
gx = h_xp - h_xm;  gy = h_yp - h_ym;  gz = -2*r;       // gradient vector [0x4622DE..0x4622F4]
norm = (int32)min(sqrt((double)gx*gx + gy*gy + gz*gz), 2147418112.0);   // [0x4622F8..0x46232B]
fx = gx*(int64)r / norm;  fy = gy*(int64)r / norm;     // downhill push (applied negated)
pen = rel + gz*(int64)r / norm;                        // = rel - 2r^2/norm  [0x462360]
if (pen >= 0) {                                        // grazing case [0x4623AF]
    ax = gx ? gz*pen/gx : 0;  ay = gy ? gz*pen/gy : 0; // [0x4623BB..0x4623E3]
    hit = false;
    if (|ax| < |fx|) { fx += ax; hit = true; }         // [0x4623F9..0x4623FF]
    if (|ay| < |fy|) { fy += ay; hit = true; }         // [0x46241E..0x462424]
    if (!hit) skip;
    pen = 0;
}
mag  = (int32)sqrt(fx*fx + fy*fy + (pen - rel)*(pen - rel));   // [0x462443..0x46248F]
cosr = ((rel - pen) << 22) / mag;                      // contact-slope cos, 22-bit [0x4624A7]
force[i].Z -= pen;                                     // push-up = penetration [0x4624AD]
if      (cosr <  soft)              { force[i].X -= fx;    force[i].Y -= fy;    sev = 3; }        // [0x4624BB..0x4624CA]
else if (cosr < (hard+soft)>>1)     { force[i].X -= fx>>2; force[i].Y -= fy>>2; sev = max(sev,2);} // [0x4624E0..0x4624FC]
else if (cosr <  hard)              { force[i].X -= fx>>3; force[i].Y -= fy>>3; sev = max(sev,1);} // [0x462508..0x462528]
else                                { /* climbable: vertical only */ }
```

where `soft = cos22(itemDef+0x8F4)`, `hard = cos22(itemDef+0x8F8)` (computed by the
platform fn and passed by pointer) [orig: 0x481BAC..0x481BE7]. Return = max severity
0..3.

Entity-entity loop (proximity list at +0x1BC, count +0x1C0) [orig: 0x462561..0x462A24]:
skips self and entities standing on us; if OUR mass > 2× theirs or boundRadius > 2×
theirs → set THEIR Flags |= 0x40 (+ their +0x3B8 = current_tick when their
itemDef+0x5C == 1) and skip (the sleeping-boat wake path §2) [0x462604..0x46262D];
if THEY are < half our mass/radius AND our brain cmd register (brain+0x220) == 0 →
`pushable = 1` [0x46265C, gate built at 0x46219A]. Then per probe:
`Entity_ComputeBoneCollisionForce(...)` (mask 8, or 24 when itemDef attrib2 < 0
[0x4621B0..0x4621BE]) yields a 3-D force; classified by the same cos-ratio
`|fz|<<22/|f|` against soft/mid/hard into full / >>1 / >>2 lateral applications with
severity 3/2/1, ricochet Z-terms `(fz*fz + fxy*fxy)/fz` on steep hits, and
`hit_entity_out = neighbor` on severity-3 [0x4626B9..0x4629CD]. outFlags bit 0x800 →
`dword +0x2C |= 0x40` on us [0x4629D9..0x4629DB].

### 5. First call results and the bob/lift parameter block [orig: 0x4820A9..0x482293]

The 7 per-point Z forces of call #1 are saved (zf0..zf6) [0x4820B0..0x48211E]. Then,
with F = (double)q:

```c
amp = min( (int32)(F * 0.0625), 352 );  entity+0x300 = amp;   // bob amplitude [0x4821A8..0x4821D1]
if (itemDef->mass(+0x908) <= 1) {                      // LIGHT boat [0x482105]
    floatH   = (int32)(0.65 * F);                      // flt_7C6F94 [0x482130]
    liftHi   = itemDef[0x938] * 100;                   // flt_7C4654 [0x48213D..0x482155]
    liftLo   = 250;  planeSpd = 10000;                 // [0x48215C, 0x482163]
    pitchThr = (int32)(itemDef[0x934] * 0.1 * 4096.0); // = i934*409.6 [0x48216E..0x482189]
} else {                                               // HEAVY [0x48221B]
    floatH   = q;                                      // [0x482221]
    liftHi   = (int32)(itemDef[0x938] * F * 0.004);    // flt_7C6F8C [0x48221B..0x482239]
    liftLo   = liftHi >> 1 (via *0.5);  planeSpd = 20000;  // [0x482240..0x482267]
    if (|right.z| > 0x2000)                            // heavily rolled [0x482259..0x482272]
         { pitchThr = (int32)(itemDef[0x934] * 0.1 * 409.6); liftHi = 250; } // flt_7C6F88 [0x482278..0x482293]
    else   pitchThr = (int32)(itemDef[0x934] * 0.1 * 4096.0);
}
dipExit = (int32)(pitchThr * (1.0 - 0.1*itemDef[0x93C]));  // flt_7C486C fold [0x482190..0x4821AE]
```

(right = row1 of the pose matrix; right.z = the roll indicator used throughout.)

### 6. Collision response by severity (call #1 result) [orig: 0x4821D1..0x4826FC]

- sev 0: clear brain sound-latch bit 0x10; skip §7 entirely (no position push), go to
  §8 [0x482205..0x482210].
- sev 1: `currentSpeed -= currentSpeed >> (torque+2)`; clear latch bit 0x10; go §7
  [0x4821E7..0x482216].
- sev 2: `currentSpeed -= currentSpeed >> (torque+1)`; go §7 [0x4822A4..0x4822BB].
- sev 3 [0x4822C9..]: `currentSpeed -= currentSpeed >> (torque+2)`;
  `impact = |(velocityX, velocityY, slideDecay)|` [0x4822E2..0x48233D];
  - AUTHORITY-ONLY damage [gate 0x482336..0x48234C: is_authority && !(Flags &
    0x4000000)]: `moved = |Position - savedLivePose|`; if impact > 29300 (0x7274) AND
    moved > 29300: unitType==3 → Health=0, +0x178=0; else Health -= 100 (floor 0,
    zeroing +0x178 at 0) [0x482352..0x482402].
  - Collision one-shot sound when impact > 2344 (0x928), latched by brain byte
    +0x318 bit 0x10; sound id itemDef[+0x864]+0x68, fallback global [0x482404..0x48244D].
  - Momentum exchange with the hit entity (client-run too): if their itemDef attrib
    (+0x54) bit 0x40, their mass < 2×ours, their boundRadius < 2×ours:
    `t = dv * ourMass/(ourMass+theirMass)` per axis (dv = our vel − their vel, axes
    velocityX/Y + slideDecay); their velocity += (3*t)>>2; their POSITION += t>>2
    [0x482459..0x48253D].
  - Strongest-force pick: scan the 7 planar force magnitudes, take the max index
    [0x482546..0x4825DD]; if that probe is > 0x8000 (0.5 u) from Position in the
    plane [0x4825E3..0x48262D]: compute a BAM yaw-kick
    `delta = bam(atan2(dy,dx)) - bam(atan2(dy+fy, dx+fx))` (rad→BAM via dbl_7C57B8 =
    -683565275.5764316), mass-scaled if the hit entity is light — **computed and then
    DISCARDED (dead code; overwritten before any use)** [0x48263E..0x4826E9]; if there
    was NO hit entity: `currentSpeed = (int32)(currentSpeed * 0.25)` (flt_7C333C)
    [0x4826EB..0x4826FC].

### 7. Position push + second solve [orig: 0x482702..0x482A8C] (sev >= 1 only)

```c
dX = sum of the 7 force X; dY = sum of the 7 force Y;      // [0x482702..0x482770]
all 7 probe points += (dX, dY);                             // [0x482769..0x4827CB]
sev2 = Entity_ComputeCollisionForces(... hit_entity_out=NULL ...);   // [0x482818]
if (sev2) {          // still colliding after the push: average old+new
    zf0..zf6 = (zfNew + zfOld) >> 1;                        // [0x4828BD..0x482950]
    dX = (dXnew + dX) >> 1;  dY likewise;                   // [0x482931..0x482942]
}
if (hit entity is light)   // mass yield: we take only their share
    dX = dX*theirMass/(ourMass+theirMass); dY likewise; zf0..zf5 likewise; // [0x482957..0x482A7F]
Position.X += dX; Position.Y += dY; Position.Z += 0;        // Z-sum always 0 [0x482A86..0x482A8C]
```

### 8. Water leg — submersion, draft, Flags 0x8000 [orig: 0x482A8F..0x482DB7]

With W = Env_WaterHeightFixed and cz0..cz3 = the (pushed) world Z of corner probes
0..3 (saved v279..v282):

```c
sub_k  = W + q - cz_k;                       // per-corner submersion, k=0..3 [0x482AB2..0x482B4E]
lowest = argmin(cz_k);                       // [0x482AC8..0x482B3B]
avg    = (cz0+cz1+cz2+cz3) >> 2;             // [0x482AF4..0x482B5A]
if (Flags & 0x8000)                          // already afloat: deepen the reference
    draft = (updateCallback == 0x48F010) ? avg - (q>>1)           // amphibian [0x482B6E..0x482B80]
                                         : (int32)(avg - 0.9*F);  // boat, flt_7C459C [0x482B84..0x482B97]
else draft = avg;
if (draft + v210 >= W) {                     // v210 = -(m[0x28]+q+spring0), §3 [0x482B9C..0x482BA5]
    if (was afloat) release the two water-line emitter banks (itemDef particle words,
        per-bone handles at entity+0x408..) [0x482CB3..0x482DB5];
    Flags &= ~0x8000;                        // [0x482DB7]
} else {
    if (!(Flags & 0x8000)) {                 // splash on entry [0x482BB9..0x482C9D]
        spawn effect dword_2C25C84 (if airborne 0x2000) else dword_2C25BFC at the
        LOWEST corner with Z = W (descriptor flags 1, attenuation -0x8000);
        Server_SendOverlayActionToAlive(dword_24E09B0 / dword_24E09B4, pos)  // authority-gated inside
        Entity_UpdateBoneTrailEffects(entity, 1, ...); (entity, 2, ...);
    }
    Flags |= 0x8000;                         // [0x482CA5]
}
```

### 9. Lever corners, capsize latch, bow-lift machine [orig: 0x482DCA..0x48398C]

Rebuild the pose matrix from `&Position` (post-push), re-extract fwd = row0
{outVec, v179, v180}, right = row1 {rc, v182, v183}, up = row2 {vec0, vec1, v177}
[0x482DCA..0x482E2C]. Build 4 ORIENTATION (lever) corners around the CURRENT pose
(round-half-up 16.16 products) [0x482E31..0x48346D]:

```c
c0 = Pos + (-hb/2)*right + (+hl/2)*fwd      // pairs probe p0
c1 = Pos + (+hb/2)*right + (+hl/2)*fwd      // pairs probe p1
c2 = Pos + (-hb/2)*right + (-hl/2)*fwd      // pairs probe p2
c3 = Pos + (+hb/2)*right + (-hl/2)*fwd      // pairs probe p3
// (±0.5 factors via flt_7C59B0=-0.5 / flt_7C3B94=0.5; the lateral sign convention
//  follows the engine's row1; the k-k pairing is the load-bearing fact.)
```

cornerPositions[] = {c0.xyz, c1.xyz, c2.xyz, c3.xyz}; the SOLVE fits pitch/roll
through their Z entries ([2],[5],[8],[11]) after the adjustments below.

- Capsize latch: if !(Flags & 0x2000) && up.z < 0 && !byte+0x2F0 → byte+0x2F0 = 1
  [0x483474..0x48348B]. AUTHORITY: Flags bit 0x10 = byte+0x2F0; and if !(Flags &
  0x2000) && up.z > 0 → clear 0x10 [0x483494..0x4834C6].
- Capsized smoke/fire (cosmetic; also sets the effect position var {X, Y, max(Z, W)}
  used as the solver Z seed): runs when Health > 0 && byte+0x2F0; smoke at +0x1CC
  (g_FxHandleSmkSigB), fire at +0x400 when Health <= healthMax>>2 (size by unitType
  8/7), both released when Health <= itemDef+0x182 [0x4834C9..0x4835D4].
- Flip handling: if (Flags & 0x10) && up.z > 0 → `Entity_BuildOrientationFromVectors`
  (re-orthonormalize pose → euler, reset state, reinit sounds) + re-extract rows
  [0x4835DA..0x48362A]; if !(Flags & 0x10) && up.z < 0 →
  `Entity_RebuildOrientationMatrixFromAxes` (re-seat euler from orthonormalized axes,
  clears bytes +0x2F0/+0x2EF/+0x2EC, drops the smoke emitter) + re-extract
  [0x48362D..0x48367D]; the same call repeats at the tail if still upside-down
  [0x483F88..0x483F9E].
- Accumulator ramp: for k=0..3: if zf_k <= 0 && sub_k <= floatH → acc_k += 250
  (corner hanging above its waterline with no ground under it) [0x483680..0x4836E6].
- Bow lift / planing / porpoise (needs currentSpeed > 0, liftLo > 0, liftHi > 0;
  fwd row re-extracted) [0x4836EC..0x483891]:
  - afloat && brain cmd(+0x220) > 0: zero acc0/acc1; set planing bit (+0x472 |= 1);
    dist = |PosXY - savedXY| [0x483738..0x4837B8];
    - porpoise latch (+0x365) clear:
      - fwd.z > pitchThr: if dist/293 > (cmd/293)*0.5 → +0x365 = 1 (magic-number
        idiv by 293 both sides; 0.5 = flt_7C3B94) [0x4837C9..0x48382B];
      - fwd.z <= pitchThr: lift = (currentSpeed >= planeSpd) ? liftHi : liftLo;
        c0.z += lift; c1.z += lift (bow rises) [0x483834..0x483850];
    - latch set: if fwd.z < dipExit → clear latch; else c0.z -= liftHi;
      c1.z -= liftHi (bow slams down) [0x483859..0x48387F];
  - cmd <= 0 → clear planing bit [0x483881..0x48388D].
- Post-machine adjustments [0x483893..0x48398C]:
  - planing bit set: if afloat && currentSpeed > 0x2000 →
    `Vehicle_UpdateTurretRotation(rightVec, entity)` (@0x45AEA0, heading-accel
    cosmetic, untraced) + zero all 4 accs → §10 [0x4838A4..0x4838F1]. Else if
    |right.z| > 240 → byte+0x2EF = 0; modelPtr2 = (right.z >= 0) ? 0x2468AC :
    -2386239 (0xFFDB96C1) — the roll-override angle consumed by the solver while
    +0x2EF is set; else modelPtr2 = 0, byte+0x2EF = 0. Then +0x464 = 0, word
    +0x470 = 0 [0x4838F6..0x483986].
  - planing bit clear && byte+0x364 == 0 (not at rest): ALL 4 corner Z targets -=
    500; byte+0x2EF = 0; zero all 4 accs; +0x464/+0x470 = 0 [0x483937..0x483986].

### 10. The solve select — grounded / buoyant / settled [orig: 0x48398D..0x483F88]

Let solvedPos = the 3-dword {X, Y, Z} seeded by the §9 smoke block (Z = max(Pos.Z, W))
and WRITTEN by the solvers; corner target Zs = cornerPositions[2]/[5]/[8]/[11].

**A. Grounded branch** — ANY corner zf_k > 0 [0x48398D..0x48399D → 0x483D4E]:
`Flags &= ~0x2000` (airborne cleared HERE and only here); byte+0x2EF = 0;
modelPtr2 = 0 [0x483D55..0x483D65]. Per corner k (zc_k = call-#2 Z force):
```c
if (sub_k > floatH)      lift = (zf_k > sub_k) ? zc_k : |sub_k - floatH|, acc_k = 0;
else if (zf_k != 0)      lift = zc_k,                     acc_k = 0;
else                     lift = 250 - acc_k;              // no <=0 clamp here
corner_k.z += lift;                                       // [0x483D5C..0x483F04]
```
`slideDecay = 0` [0x483F25]; `Entity_ComputeSuspensionOrientation(corners, mtx,
&solvedPos, q, entity, 1, 0)` [0x483F2B]; euler out: Roll/Pitch always, Yaw only if
byte+0x2EC [0x483F3D..0x483F63]; `Position.Z = solvedPos.Z` [0x483F66..0x483F6D];
zero all 4 accs [0x483F70..0x483F82].

**B/C. No ground contact** (all zf_k <= 0) [0x4839A3..0x4839BF]:
- At-rest bob arm: if currentSpeed < 100 && afloat && |fwd.z| < 5 && |right.z| < 5:
  if !byte+0x364 → set it and seed the phase float +0x318 = 4.71f (3π/2 → factor
  ramps from 0) [0x4839C5..0x483A07]; then
  `Entity_ApplyBreathingOscillation(entity, +0x304, cornerPositions)` @ 0x463BE0:
  `phase += 0.03488888964056969f` (flt_7C6EB4, ≈ 2π/180 → ~2.9 s period);
  `f = min(1.0, (sin(phase) + 1.0) * 0.5)`; all four corner Z targets +=
  `(int32)(amp * f)`; returns that increment, and `floatH -= it` (deeper effective
  submersion while the hull rises — heave bob, no wave tilt) [0x483A0D..0x483A25].
  Else byte+0x364 = 0 [0x483A29].
- **B. Buoyancy push-up** — ANY sub_k > floatH [0x483A30..0x483A62 → 0x483C15]:
  `if (slideDecay < 0) slideDecay = 0` [0x483C15..0x483C1D]; per corner:
  ```c
  if (sub_k >= floatH) lift = |sub_k - floatH|, acc_k = 0;   // raise to waterline
  else                 lift = min(0, 250 - acc_k);           // ramped drop
  corner_k.z += lift;                                        // [0x483C23..0x483CD8]
  ```
  `Entity_ComputeSuspensionOrientation(...)` [0x483D00]; `Position.Z = solvedPos.Z`
  [0x483D05..0x483D19]; euler: Roll/Pitch (+Yaw if +0x2EC) [0x483D1C..0x483D46].
  Accs NOT zeroed here.
- **C. Settled / airborne** — all sub_k <= floatH [0x483A68..0x483C10]:
  - afloat: tournament-scan the two LOWEST corner Zs (sentinel 0x10000000)
    [0x483A76..0x483AF0]; if brain cmd <= 0: acc[lowest] = acc[second] = 0, and if
    fwd.z < 100 zero ALL accs (an idle boat's low corners fall freely → settles
    level) [0x483AF5..0x483B2B]; per corner: corner_k.z += min(0, 250 - acc_k)
    [0x483B31..0x483B8A].
  - NOT afloat: `Flags |= 0x2000` (airborne set HERE and only here) [0x483B93..0x483B98].
  - `Entity_ProcessWheeledVehicleSuspension(corners, mtx, &solvedPos, q, entity, 1,
    0)` @ 0x46B140 (the shared 4-wheel spring/orientation solver; spring constants
    8000 grounded / 800 free per its interior; handles the airborne free-fall fit)
    [0x483B9D..0x483BC3]; euler: Roll/Pitch (+Yaw if +0x2EC) [0x483BD0..0x483BF6];
    `if (afloat) Position.Z = solvedPos.Z` [0x483BF9..0x483C0D].

Tail [0x483F88..0x483FCF]: up.z < 0 && !(Flags & 0x10) → righting call (§9); airborne
counter +0x3D4 ++/reset; return sev2.

**Solver contract** (Entity_ComputeSuspensionOrientation @ 0x46C8E0, water path):
edge vectors from the 4 corner targets (c0−c3 and c3−c2 style pairs), each normalized
(scale flt_7C32BC = 65536.0), up = normalized cross → matrix rows set via
Math_SetRow0/1/2 [orig: 0x46DE2F..0x46DE45]; fitted-plane tilt check `|up.z| < 36864`
(0.5625) drives the flip/disable state machine (authority `Flags |= 0x10`)
[0x46CC89..0x46CCA4]; solvedPos.X/Y = corner midpoint + normal-projected offset
[0x46DE4E..0x46E08A]; **solvedPos.Z = (c0.z + c1.z + c2.z + c3.z) * 0.25**
(flt_7C333C) [0x46E099..0x46E0B3]; if byte+0x2EF: the matrix is REBUILT as
`Math_BuildFixedPointRotationMatrixYXZ(mtx, 0, modelPtr1(+0xA8), modelPtr2(+0xAC))`
— the capsize roll-override path [0x46E0B6..0x46E0D7]. Caller then converts with
`Math_FixedPointMatrixToEulerAngles` @ 0x613310 (rad→BAM 683565275.5764316).

So the boat's Pitch/Roll = plane fit through {lever corner Zs + water/ground lifts},
and Z = the average of those 4 target Zs. Wave source = flat plane + the §10 bob.

### 11. Flags produced/consumed (entity+0x24)

| bit | meaning | producer here | consumer |
|-----|---------|---------------|----------|
| 0x10 | capsized/disabled | AUTH-only: set/clear from byte+0x2F0, cleared when upright grounded [0x4834A5/0x4834AA/0x4834C6]; solver also sets on steep fit (auth) [0x46CCA4] | flip legs §9, sleep latch §2 |
| 0x40 | recently shoved by a bigger vehicle | cleared here after 200 ticks [0x481A79]; SET on the victim inside the collision helper [0x46261E/0x46262D] with tick @+0x3B8 | sleep denial §2 |
| 0x2000 | airborne | set §10-C [0x483B98], cleared §10-A [0x483D55] | watercraft §3 yaw-rate gate, §6 splash FX pick, +0x3D4 counter |
| 0x8000 | afloat | set [0x482CA5] / cleared [0x482DB7], §8 | watercraft §6 drag select; amphibian mover dispatch [0x48F01E]; every afloat gate here |
| 0x4000000 | collision-damage immunity | — | damage gate [0x482345] |
| 0x800000 | terrain-collision opt-out | — | probe skip [0x462214] |

NOT written here: velocityX/Y, modelPtr0 (yaw rate), brain registers. currentSpeed IS
written (collision sheds §6).

### 12. Client vs authority split (part 3)

Authority-gated INSIDE this function — everything else runs identically on a CLIENT
for a remote boat every tick:
1. Hard-collision Health damage/kill [0x482336..0x482402].
2. Flags bit 0x10 latch maintenance [0x483494..0x4834C6, 0x4819FF..0x481A22].
3. The splash overlay broadcast (Server_SendOverlayActionToAlive self-gates) [0x482C88].

Client-executed subset therefore includes: the full 7-probe terrain/entity solve, both
position pushes, the momentum exchange writes into OTHER entities, currentSpeed
collision sheds, ALL of Flags 0x8000/0x2000, byte latches, corner accumulators, bob
state, the complete Z/Pitch/Roll production, and the cosmetic FX/sounds. For
D-NET-196 this is the tracked gap: the remote-boat prediction leg must run this solve
(minus the three authority items) after integration, exactly where the retail caller
does [0x48ECE7], or remote hulls stay level and Z-chased.

### 13. Feed into the beach full-stop fwd[2] (part 4)

The watercraft thrust block builds its matrix from `&Position` AFTER this solve wrote
Pitch/Roll (+Yaw) the PREVIOUS tick, extracts fwd = row0, and computes
`vertical_thrust = (accel * fwd[2] + 0x8000) >> 16` at [orig: Entity_
UpdateWatercraftPhysics @ 0x48E975..0x48E99E / 0x48E9DC]. fwd[2] > 0 (bow pitched up)
comes exclusively from THIS function: the §9 bow-lift (`fwd.z` chased toward
pitchThr while cmd > 0) and, on a beach ramp, the §10-A grounded fit through the
terrain-forced corner Zs. The full stop then requires vertical_thrust > 0 AND
look-ahead ground − W > 0x7777 AND no carrier [orig: 0x48EC7B..0x48ECA2]. Port
consequence: without this solve the beach stop can never trigger on a client
(level hull → fwd[2] == 0), and the §3 yaw-rate gate `!(0x2000) || (0x8000)`
free-runs stale (both flags are produced only here).

### 14. Constants table (verbatim)

| value | meaning |
|-------|---------|
| dbl_7C3608 = 1.4629627251502471e-09 | BAM→rad (pi/0x7FFF8000) |
| dbl_7C3600 = 4194304.0 | 2^22 trig scale |
| dbl_7C57B8 = -683565275.5764316 | rad→BAM, negated (dead yaw-kick) |
| flt_7C19E0 = 2147418112.0 | ftol clamp (float)0x7FFF8000 |
| flt_7C32BC = 65536.0 | solver normalize scale |
| flt_7C333C = 0.25 | corner-Z average; sev-3 speed cut |
| flt_7C3B94 = 0.5 | porpoise trigger; heavy liftLo |
| flt_7C459C = 0.9 | boat draft factor |
| flt_7C4654 = 100.0 | light liftHi scale |
| flt_7C486C = 0.0625 | amp = q/16 (and the dipExit fold) |
| flt_7C59B0 = -0.5 | lever corner lateral factor |
| flt_7C69F4 = 0.1 | itemDef 0x934 scale |
| flt_7C6F84 = 4.71 | bob phase seed (≈3π/2) |
| flt_7C6F88 = 409.6 | pitchThr scale (heavy rolled) |
| flt_7C6F8C = 0.004 | heavy liftHi scale (=1/250) |
| flt_7C6F90 = 4096.0 | pitchThr scale (×0.1 → 409.6) |
| flt_7C6F94 = 0.65 | light float-height factor |
| flt_7C6EB4 = 0.03488888964056969 | bob phase step rad/tick |
| dbl_7C6A08 = 1.0, dbl_7C3618 = 0.5 | bob (sin+1)*0.5 |
| 250 (0xFA) | accumulator step / drop ramp unit |
| 500 (0x1F4) | sleep Z tolerance; idle sink amount |
| 293 | speed quantum in the porpoise trigger (magic idiv 0xDFAC1F75,>>8) |
| 352 (0x160) | bob amplitude cap |
| 100 (0x64) | at-rest currentSpeed bound; idle fwd.z bound |
| 5 | at-rest |fwd.z| / |right.z| bound |
| 240 (0xF0) | roll-override |right.z| threshold |
| 0x2468AC / -2386239 (0xFFDB96C1) | modelPtr2 roll-override values |
| 0x2000 | planing speed for the spray leg; midprobe radius floor; rolled |right.z| |
| 10000 / 20000 | planeSpd light/heavy |
| 29300 (0x7274) | collision damage threshold (speed AND displacement) |
| 100 HP | collision damage |
| 2344 (0x928) | collision sound threshold |
| 36864 (0x9000 = 0.5625) | solver flip-dot threshold |
| 0x1000 / 0x4000 / 0x2000 | midprobe radius folds (§3) |
| 0x1000 (4096) | sleep upright up.z threshold |
| -500..0 | sleep slideDecay window |
| 200 ticks | Flags 0x40 shove-latch expiry |
| 0x10000000 | corner-Z tournament sentinel |
| 8000 / 800 | wheeled-solver spring constants (interior, contract-level) |

### 15. UNVERIFIED / out-of-scope

- `Entity_ComputeSuspensionOrientation` / `Entity_ProcessWheeledVehicleSuspension`
  interiors beyond the §10 contract (state-machine bytes +0x2EC/+0x2EE writers, the
  per-wheel spring block at entity+0x368.., death/wreck transitions, the exact X/Y
  center adjustment): decompile-level; needs its own grill (shared with the ground
  family — grill once for both).
- The writer of the per-corner probe springs +0x2D4..+0x2E0 (read-only here).
- `Vehicle_UpdateTurretRotation` interior (cosmetic heading-accel at planing speed).
- The lateral sign of row1 vs model +Y (pairing k↔k is verified; the world-side
  handedness note in §9 is interpretive).
- The dead yaw-kick block (§6) — witnessed dead; do not port.
- byte+0x2F2 consumer (set here, read elsewhere).
- `Entity_ComputeBoneCollisionForce` interior (entity-entity probe).

---

## §4 The shared suspension-solver interiors + platform blockers

Companion to `boat_platform_solve_spec.md` §15. Source: retail Jointops.exe (imagebase
0x400000, IDB Jointops.exe.kong.i64). Both solver interiors were dumped to full
instruction listings (`susp_orient_disasm.txt`, `wheel_susp_disasm.txt` in this
session-artifact dir) and every math block below was reconstructed from the disassembly; the
Hex-Rays output (susp_orient.c / wheel_susp.c) was used only for control-flow skeleton.
Conventions as in the main spec (16.16 positions, BAM angles, `normalize(v)` =
`v * 65536.0 / sqrt(vx²+vy²+vz²)` computed in x87 double, each component ftol'd;
zero-length → (0,0,0) [orig: 0x46C9BA..0x46CA14 pattern, repeated]).

Function bounds: `Entity_ProcessWheeledVehicleSuspension` 0x46B140..0x46C8E0 (6048 b),
`Entity_ComputeSuspensionOrientation` 0x46C8E0..0x46E0FF (6175 b).

---

### 0. Call graph and argument contract (shared by both solvers)

```
cdecl Solver(int32 corners[12], int32 mtx22[16], int32 outPos[3],
             int32 q_unused, Entity* e, int unused6, int32* wheelContactFlags)
```
- `corners` = the caller's 4 lever-corner positions (c0 @+0/4/8, c1 @+0xC/10/14,
  c2 @+0x18/1C/20, c3 @+0x24/28/2C). `mtx22` = the 22.10 pose matrix (rows read/written
  via Math_ExtractRow*/Math_SetRow*FromVec3Scaled, >>6 / <<6 converting to/from 16.16).
- `q` (arg 3) is **never referenced** by either solver. Arg 5 (`1` at every platform
  call site) is likewise never referenced by 0x46B140; 0x46C8E0 reads it as its
  water-vs-ground path switch (`hasWaterContact`, [orig: 0x46CD6C]).
- `wheelContactFlags`: 4 ints, nonzero = wheel k grounded. NULL from the boat
  [orig: 0x483BA1/0x483BBE], NULL from aircraft [orig: 0x480EE3], NULL or a real array
  from tracked [orig: 0x47E5A2 vs 0x47E5C8]. When the tracked caller passes the array
  it passes arg6 = 0 [orig: 0x47E5A3].
- Callers of 0x46B140: Entity_ProcessTrackedVehiclePhysics @ 0x47E5E6 / 0x47EC4D,
  Entity_ProcessAircraftContactPhysics @ 0x480F01 / 0x481475,
  Entity_ProcessPlatformPhysics @ 0x483BBE (leg §10-C).
  Callers of 0x46C8E0: ONLY Entity_ProcessPlatformPhysics @ 0x483D00 (leg B) and
  0x483F2B (leg A). The name "wheeled vehicle suspension" is historical; 0x46B140 is
  the shared settled/airborne 4-corner solver for every vehicle family.

### 1. The shared 4-normal plane fit (identical code in both solvers)

Edge/normal sets, verbatim corner pairing (verified at instruction level in BOTH
functions — 0x46C92B../0x46D4C7../0x46D6D8../0x46D8E8 and 0x46B332../0x46BC76../
0x46BE7C../0x46C091):

| set | edge A (normalized) | edge B (normalized) | normal = norm(cross(A,B)) |
|-----|--------------------|---------------------|---------------------------|
| 1 | a = c0 − c3 | b = c3 − c2 | n1 [orig: 0x46CAB7 / 0x46BAF9] |
| 2 | e2 = c0 − c3 | f2 = c3 − c2 | n2 (**same pair as set 1, recomputed**) [orig: 0x46D63B / 0x46BDCD] |
| 3 | e3 = c1 − c2 | f3 = c3 − c2 | n3 [orig: 0x46D847 / 0x46BFE5] |
| 4 | e4 = c1 − c2 | f4 = c0 − c1 | n4 [orig: 0x46DA5E / 0x46C1FC] |

The set-1/set-2 duplication is real shipped code (a fourth distinct corner normal,
cross(c0−c3, c0−c1), is never computed). Aggregation [orig: 0x46DC1F..0x46DC69 /
0x46C3B1..0x46C3FB]:

```c
up_sum   = n1 + n2 + n3 + n4;                     // n1 pair counted twice
fwd_sum  = e4 + e3 + a + e2;                      // = 2*(c1-c2)u + 2*(c0-c3)u
side_sum = f4 + f3 + b + f2;                      // = (c0-c1)u + 3*(c3-c2)u
each *= 0.25 (flt_7C333C, float round-trip);      // [orig: 0x46DC23..0x46DCF0]
row0 = normalize(fwd_sum); row1 = normalize(side_sum); row2 = normalize(up_sum);
Math_SetRow2/0/1FromVec3Scaled(mtx, ...);         // [orig: 0x46DE2F..0x46DE45 / 0x46C5CD..0x46C5E3]
```

For a planar rectangle the odd weights cancel exactly (fwd_sum ∝ +forward,
side_sum ∝ +right — the lateral parts of the two long edges cancel, the (c0−c1)
term opposes the three (c3−c2) terms); the asymmetry only skews twisted quads.
Port literally.

Per-corner plane heights: only ONE is actually computed —
`hDot = c1·n1` (corners[3..5] dotted with n1, round-half-up 16.16)
[orig: 0x46CD0A..0x46CD74 / 0x46BC6A]. See §2.3 for how the other three "heights"
are read.

### 2. Blocker 1 — Entity_ComputeSuspensionOrientation @ 0x46C8E0, full interior

#### 2.1 Entry

```c
up_acc = {0,0,0};                                  // [orig: 0x46C8F9]
if (byte e+0x2EC && byte e+0x2EF)                  // disabled + override: grab old up
    up_old = ExtractRow2(mtx);                     // [orig: 0x46C907..0x46C91C]
zAxis = {0, 0, 0x10000};                           // [orig: 0x46C97E/0x46C984/0x46C98A]
compute set-1 a, b, n1 (§1);
upz  = n1·zAxis  (three 16.16 dot terms summed = n1.z)   // [orig: 0x46CBED..0x46CC44]
cross(zAxis, n1) → v112;                           // computed, NEVER read — dead
                                                   // [orig: 0x46CC4E; single xref 0x46CC49]
```

#### 2.2 Flip/disable state machine [orig: 0x46CC53..0x46CE4D]

Bytes (all on the entity): 0x2EC = disabled/wreck-fit latch ("yaw-from-fit"),
0x2ED = mover disable request (read by 0x46B140 only), 0x2EE = falling-arm,
0x2EF = pitch/roll override active, 0x2F0 = capsize/at-rest-wreck latch,
0x2FC = wreck settled.

```c
if (!byte0x2F0 && abs(upz) < 0x9000 /*36864*/      // steep fit         [0x46CC5C..0x46CC77]
    && !byte0x2EC && byte0x2EE) {                  // armed & not latched [0x46CC7D..0x46CC8F]
    byte0x2EF = 0;                                 // [0x46CC95]
    if (is_authority)          { Flags |= 0x10; byte0x2EC = 1; }   // [0x46CCA4/0x46CCB0]
    else if (Flags & 0x10)     { byte0x2EC = 1; }                  // [0x46CCAE/0x46CCB0]
    if (byte0x2EC) {                               // [0x46CCB7]
        byte0x2EE = 0;                             // [0x46CCC7]
        if (acc0..acc3 (+0x2C4..+0x2D0) all >= 0) {                // [0x46CCC0..0x46CCE9]
            Entity_ClearSuspensionState(e);        // @0x4592B0: identity mtx @+0x4F4,
                                                   // zero quat @+0x534.., byte+0x4E8,
                                                   // timers +0x4EC/+0x4F0, byte+0x3DC
            Entity_ComputeChassisOrientation(e, corners, 0);       // [0x46CCF5] mode 0 = averaged
            if (Flags & 0x2000) return;            // [0x46CCFD..0x46CD04]
        }
    }
    goto MAIN_FIT;                                 // 0x46CD0A
}
// else [0x46CDFE]:
al = byte0x2EC;
if (al && !byte0x2EF && (Flags & 0x2000))          // disabled, no override, airborne
    { Entity_ApplyBoneAttachmentTransform(e, mtx, 1, 0); return; } // [0x46CE1B..0x46CE1F → 0x46E0E3]
if (!byte0x2EF) goto MAIN_FIT;                     // [0x46CE2A..0x46CE2C]
if (al && byte0x2F0 && !(Flags & 0x2000)) goto MAIN_FIT;           // [0x46CE32..0x46CE3D]
if (!byte0x2EF || !al) goto MAIN_FIT;              // [0x46CE43..0x46CE4D]
// remaining: byte0x2EC && byte0x2EF && !(byte0x2F0 && grounded) → WRECK path §2.5
```

Boat-effective: byte+0x2EE is written only by the light/tracked/aircraft movers and
Entity_UpdateVehicleChassisOrientation (§6) — the watercraft family never sets it, so
for a pure `cbot` boat the steep-fit disable NEVER fires, byte+0x2EC stays 0, and the
wreck path §2.5 is unreachable. Boat capsize is handled entirely by the platform fn's
byte+0x2F0 machinery. (`catv` amphibians can enter the water with 0x2EC/0x2ED/0x2EE
latched from land.)

#### 2.3 MAIN_FIT, water path (`hasWaterContact != 0` — always, from the boat)

After `hDot = c1·n1` [0x46CD0A..0x46CD74] and the §1 aggregation/row writes:

```c
// solvedPos.X/Y [orig: 0x46DE4E..0x46E08A]
midX = (c0.x + c2.x) >> 1;  midY = (c0.y + c2.y) >> 1;   // c0–c2 diagonal midpoint
                                                          // [0x46DE6F/0x46DE82, 0x46DEB7/0x46DEB9]
edgeLen = (int32)min(sqrt(|c1 − c0|²), 2147418112.0);     // bow edge length, float
                                                          // [0x46DE7B..0x46DEE5]
sideOff.x = (row1.x * edgeLen + 0x8000) >> 16;            // [0x46DEE9..0x46DF21]
sideOff.y = (row1.y * edgeLen + 0x8000) >> 16;            // [0x46DF25..0x46DF5D]
(row1.z term computed and discarded)                      // [0x46DF61..0x46DF98]
avgH = (hDot + STACK[-0x0C] + STACK[-0x08] + STACK[-0x04]) >> 2;   // !!! see below
                                                          // [0x46DF9C..0x46DFC5]
upOff.x = (row2.x * avgH + 0x8000) >> 16;                 // [0x46DFC9..0x46E001]
upOff.y = (row2.y * avgH + 0x8000) >> 16;                 // [0x46E005..0x46E03D]
outPos[0] = midX + sideOff.x + upOff.x;                   // [0x46DFAA + 0x46E088]
outPos[1] = midY + sideOff.y + upOff.y;                   // [0x46DFAE + 0x46E08A]
// solvedPos.Z — the ONLY output the platform fn consumes:
outPos[2] = (int32)((c1.z + c3.z + c2.z + c0.z) * 0.25);  // flt_7C333C [0x46E08D..0x46E0B3]
```

**Uninitialized-stack witness (do NOT port the X/Y formula):** the three summands at
frame slots var_C/var_8/var_4 are READ at 0x46DF9C/0x46DFA3/0x46DFB4 but have **no
writer anywhere in the function** (verified by exhaustive scan of the listing; only
var_10 = hDot is written, at 0x46CD74). They are leftover caller-stack garbage; the
identical pattern exists in 0x46B140 (var_C/var_8/var_4 read at 0x46C731..0x46C749,
written nowhere; only var_10 = w1·n1 @ 0x46BC6A). The original quite clearly intended
the four per-corner c_k·n_k heights and shipped with three of them missing.
It is harmless in retail because **no caller reads outPos[0]/[1]**: the platform fn
uses only `Position.Z = solvedPos.Z` (spec §10), and pre-seeds solvedPos in §9 so the
wreck/early-return paths (which never write outPos at all) leave a sane value.
Port ruling: replicate outputs, not the garbage — write outPos.Z exactly as above and
leave outPos.X/Y = the caller's seed (i.e., never consume them), which is
byte-equivalent in every observable.

The fild/ftol pairs around each product ((v<<16)/0x10000 then int→float→int) are
verified identities [orig: 0x46DEE9..0x46DF1D] — fixed↔float round-trips from the
original source; no hidden scale.

Tail: `if (byte0x2EF) Math_BuildFixedPointRotationMatrixYXZ(mtx, 0, e+0xA8, e+0xAC)`
— override rebuild, yaw forced 0, pitch = modelPtr1, roll = modelPtr2
[orig: 0x46E0B6..0x46E0D7]; then `Entity_ApplyBoneAttachmentTransform(e, mtx, 0, 0)`
[orig: 0x46E0DF..0x46E0EC] and return.

#### 2.4 MAIN_FIT, ground path (`hasWaterContact == 0` — never from the boat)

Single-normal fit only: re-normalize a, n1, b; SetRow2(n1), SetRow0(a), SetRow1(b);
`outPos[2] = (c0.z + c2.z + c3.z + c1.z) * 0.25`; outPos[0]/[1] NOT written; return
(no override rebuild on this path) [orig: 0x46CD81..0x46CDF9 → 0x46D351..0x46D4C6].

#### 2.5 Wreck path [orig: 0x46CE53..0x46D340] (never reached by pure boats, §2.2)

```c
m = graphicModel(+0x30)->modelData(+0xB0);
beam = m[0x3C]-m[0x38];  length = m[0x34]-m[0x30];        // [0x46CE56..0x46CE65]
sideVec = ExtractRow1(mtx);       // via the misnamed CWnd_HitTest @ 0x6137D0:
                                  // reads mtx[1],[5],[9] >> 6 — it IS
                                  // Math_ExtractRow1FromFixedPoint22 [0x46CE75]
fwdVec  = ExtractRow0(mtx);  up = ExtractRow2(mtx);       // [0x46CE87/0x46CE99]
Entity_ComputeBoundingQuad(e, corners, beam, length, sideVec, fwdVec, e->Position, 0);
    // @0x45B6E0 — REBUILDS the caller's 4 corners around the current pose from the
    // model bounds (overwrites the platform lever corners)     [0x46CEF5]
if (flags && ((f0&&f2)||(f1&&f3))) Entity_ClearSuspensionState(e);   // [0x46CF18]
spd = e->currentSpeed(+0x29C);                              // [0x46CF26]
if (spd < 14080 /*0x3700*/) {                               // SLOW wreck [0x46CF2C]
    if (byte0x2FC) return spd;                              // settled [0x46CF37]
    if (up.z > 0) {            // upright: flag-based        [0x46CF44 jle inverse]
        ClearSuspensionState; if (!flags) goto APPLY;
        per wheel k with !flags[k]: rate(+0x374+0x14k) = 16000 (0x3E80),
            dir(+0x368+0x14k) = (0, 0, -0x10000);            // [0x46CF5B..0x46CFBB]
    } else {                   // upside-down: corner-based  [0x46CFC6]
        ClearSuspensionState;
        per corner k with corners[k].z > e->Position.Z: rate_k = 16000,
            dir_k = (0, 0, -0x10000);                        // [0x46CFDE..0x46D052]
    }
APPLY: Entity_ClearSuspensionForces(e, corners, 0);          // applies + zeroes, §5
    Entity_ApplyBoneAttachmentTransform(e, mtx, 0, 0); return spd;   // [0x46D058..0x46D07E]
}
// FAST wreck (spd >= 14080), only when !byte0x2F0            [0x46D08A]
per !flags[k]: rate_k = 80, dir_k = up;                       // [0x46D09F..0x46D128]
Entity_ClearSuspensionForces(e, corners, 0);                  // [0x46D131]
vel = normalize({velocityX(+0x98), velocityY(+0x9C), slideDecay(+0xA0)});  // [0x46D13C..0x46D1DB]
if (|vel| != 0)
    per !flags[k]: rate_k = 16*5*ftol(spd * flt_7C6F1C /*1/10240*/) = 80*trunc(spd/10240),
        dir_k = vel;                                          // [0x46D21E..0x46D279]
Entity_ClearSuspensionForces(e, corners, 0);                  // [0x46D282]
per !flags[k]: rate_k = 80, dir_k = (0, 0, -0x10000);         // [0x46D28C..0x46D305]
Entity_ApplyBoneAttachmentTransform(e, mtx, 0, 0); return spd;
```

(With flags == NULL — every boat/aircraft call — the flag-guarded loops are skipped
entirely; only the upside-down corner-based 16000 writes can run.)

### 3. Blocker 2 — Entity_ProcessWheeledVehicleSuspension @ 0x46B140, full interior

#### 3.1 Entry / disable machine [orig: 0x46B140..0x46B330]

```c
if (byte0x2EC && byte0x2EF) negUp = -ExtractRow2(mtx);      // [0x46B159..0x46B1A2]
if (byte0x2ED && !byte0x2EC) {                              // mover requested disable
                                                            // [0x46B1A6..0x46B1BF]
    deathMult = is_authority ? 1.25f /*flt_7C6F18*/ : 1.75f /*flt_7C6F14*/;  // [0x46B1C5..0x46B1DB]
    byte0x2EF = 0;                                          // [0x46B1DB]
    if (is_authority)      { Flags |= 0x10; byte0x2EC = 1; }        // [0x46B1ED/0x46B1F9]
    else if (Flags & 0x10) { byte0x2EC = 1; }                       // [0x46B1F3/0x46B1F9]
    if (byte0x2EC) {                                        // [0x46B200]
        byte0x2EE = 0; Entity_ClearSuspensionState(e);      // [0x46B20D/0x46B213]
        if (!(Flags & 0x2000) && flags) {                   // grounded, flags known
            per wheel k with !flags[k]:
                rate_k = ftol(acc_k(+0x2C4+4k) * deathMult); // !! rates seeded from the
                dir_k  = (0, 0, -0x10000);                   // per-corner accumulators
                                                             // [0x46B22E..0x46B27E]
        } else if (Flags & 0x2000) {                        // airborne [0x46B290]
            rate_k = acc_k (raw copy, all four); dir_k = (0,0,-0x10000);  // [0x46B294..0x46B30B]
        }
        Entity_ClearSuspensionForces(e, corners, 0);        // [0x46B314]
        Entity_ApplyBoneAttachmentTransform(e, mtx, 0, 0);  // [0x46B31D]
    }
    if (Flags & 0x2000) return;                             // [0x46B325..0x46B32C]
    goto MAIN_FIT;                                          // 0x46B332
}
// byte0x2ED == 0 or byte0x2EC already set:                  [0x46B3D4]
if (byte0x2EC && !byte0x2EF)
    { Entity_ApplyBoneAttachmentTransform(e, mtx, 1, 0); return; }   // (3rd arg 1!)
                                                            // [0x46B3DA..0x46B417, 0x46B3F0]
if (byte0x2FC) { Entity_ClearSuspensionState(e); return; }  // settled [0x46B419..0x46B434]
if (!byte0x2EF || !byte0x2EC) goto MAIN_FIT;                // [0x46B435..0x46B443]
// wreck path (byte0x2EC && byte0x2EF):                      [0x46B449..]
```

There is NO steepness test here — 0x46B140's disable trigger is purely the mover's
byte+0x2ED request. byte+0x2ED is set by the ground movers on: airborne fitted-normal
|up.z| < itemDef->flip(+0x948) * 0.01 (flt_7C56A8) in 16.16 [orig:
Entity_ProcessWheeledVehiclePhysics @ 0x477742..0x477776 — the `flip` consumer],
authority |slideDecay| > 0x7000, client airborne+Flags 0x10 [orig: 0x477784..0x4777BF],
and as a 10-tick pulse timed via +0x2F8 [orig: 0x478BA2..0x478BD6]; cleared at the
mover tail [orig: 0x4795DA] and on respawn [orig: 0x460018]. Never written by the
watercraft family.

#### 3.2 Wreck path [orig: 0x46B449..0x46B6A2 slow / 0x46B6A6..0x46BA0F fast]

Same shape as §2.5 with these differences:
- speed threshold **4096** (0x1000), not 14080 [orig: 0x46B4CE].
- slow + upright (up.z > 0): `ClearSuspensionState; byte0x2F0 = 1; byte0x2EC = 0;
  Entity_ClearVehicleState(e)` (@0x459160 — zeroes acc0..acc3 +0x2C4..+0x2D0) then
  falls into MAIN_FIT — the recovered-upright wreck re-fits [orig: 0x46B4E6..0x46B50A].
- slow + upside-down: ComputeBoundingQuad rebuild, then per corner above entity Z:
  rate 8000, dir (0,0,-0x10000); with flags, pair-grouped variants keyed on
  flags[4]/flags[6] (adjacent caller stack — tracked passes a larger block)
  [orig: 0x46B50F..0x46B66E].
- fast: ComputeBoundingQuad; per wheel: contact → {8000, dir = -up (negUp from
  entry)}, free → {800, dir = +up} [orig: 0x46B703..0x46B85D]; ClearSuspensionForces;
  planar velocity unit dir = normalize2D(velocityX, velocityY) with z = 0
  [orig: 0x46B86D..0x46B944 — verified: third component multiplies the zero slot];
  if |velXY| != 0: per CONTACT wheel {8000, dir = velUnit} [orig: 0x46B991..0x46BA0F];
  then ClearSuspensionForces, ApplyBoneAttachmentTransform, ClearSuspensionState,
  return [orig: 0x46B66E..0x46B69A].
- The famous 8000/800 constants are THESE wreck-transition spring rates — the live
  (non-wreck) fit writes no springs at all. (Corrects the main spec §10's
  "spring constants 8000 grounded / 800 free" gloss.)

#### 3.3 MAIN_FIT [orig: 0x46B332..0x46C8DF]

Identical §1 fit (same edge sets, same duplicated pair, same 0.25 fold, same
SetRow2/0/1), identical X/Y center computation with the SAME three uninitialized
height slots [orig: 0x46C5EC..0x46C81F, garbage reads 0x46C731..0x46C750]. Then the
solver's own Z — different from 0x46C8E0:

```c
sum = 0; n = 0;
for k in 0..3: if (corners[k].z > 0) { sum += corners[k].z; n++; }   // [0x46C822..0x46C867]
outPos[2] = ftol((double)sum / n);            // fild/fidiv — n==0 ⇒ x87 div-by-zero,
                                              // ftol(∞) = 0x80000000; retail relies on
                                              // some corner Z > 0     [0x46C86B..0x46C87F]
if (outPos[2] - e->Position.Z > 0x2000)       // rise clamp: max +0.125 u per tick
    outPos[2] = e->Position.Z + 0x2000;       // [0x46C882..0x46C894]
if (byte0x2EF) BuildYXZ(mtx, 0, e+0xA8, e+0xAC);   // [0x46C897..0x46C8B8]
Entity_ApplyBoneAttachmentTransform(e, mtx, 0, 0); // [0x46C8CD]
```

This IS the "airborne free-fall fit": corners fall with the §10-C `min(0, 250-acc)`
ramps, the fit follows them, and the +0x2000 clamp stops the hull popping up faster
than 0.125 u/tick when a corner spikes. The platform caller consumes
`Position.Z = solvedPos.Z` only when afloat (spec §10-C); euler extraction from mtx is
unconditional.

#### 3.4 Differences vs Entity_ComputeSuspensionOrientation (summary)

| aspect | 0x46C8E0 (water/orientation) | 0x46B140 (settled/airborne) |
|---|---|---|
| disable trigger | steep fit: !2F0 && |up.z|<0x9000 && !2EC && 2EE | mover request: 2ED && !2EC (no angle test) |
| solvedPos.Z | plain 4-corner average * 0.25 | average of corners with z>0, rise-clamped +0x2000 |
| solvedPos.X/Y | written (dead + garbage-fed) | written (dead + garbage-fed) |
| wreck speed threshold | 14080 | 4096 |
| wreck rates | 16000 slow / 80 & 80*spd/10240 fast | 8000/800 fast, 8000 slow, acc-seeded on request |
| ground (non-water) fit | single normal n1, rows a/b | n/a (always 4-normal fit) |
| upright-recovery | — | slow upright wreck: 2F0=1, 2EC=0, accs cleared, refit |

### 4. Per-wheel block entity+0x368 and the wreck-tumble pipeline

Layout: 4 slots, stride 0x14: slot k at +0x368+0x14k =
`{ dirX, dirY, dirZ (16.16 unit), +0xC rate, +0x10 scratch }`
(rate slots: +0x374, +0x388, +0x39C, +0x3B0) [orig: writes at 0x46B294..0x46B30B,
zeroing at 0x4689C7..0x468A39].

Consumers: `Entity_ClearSuspensionForces` @ 0x468980 — misnamed "clear": if ANY rate
is nonzero it first calls `Entity_ComputeChassisOrientation(e, corners, mode)`
@ 0x463940, then zeroes all 20 dwords. ComputeChassisOrientation runs
`Vehicle_ComputeOrientationFrom4Wheels` @ 0x459310 (mode 1) or
`Vehicle_ComputeAveragedOrientation` @ 0x459A50 (mode 0) — these read the per-wheel
dirs/rates, offset the wheel positions by the scaled normals, build a basis, and
multiply the DELTA into the persistent wreck matrix at e+0x4F4, extracting a float
quaternion into e+0x534.. with flag byte e+0x4E8 = 1 [orig: 0x463A12..0x463A64].
So the {dir, rate} block is a one-tick torque impulse feeding the wreck-tumble
integrator; the live solve never populates it. `Entity_ClearSuspensionState`
@ 0x4592B0 resets that integrator (identity matrix, zero quat/timers).
(Interior of the two 0x4593xx integrators left at contract level — tumble-anim only,
no effect on the live boat solve.)

### 5. State bytes — writer map and boat-effective values

| byte | writers (set) | writers (clear) | boat value |
|------|---------------|-----------------|-----------|
| +0x2EC | both solver machines [0x46CCB0/0x46B1F9]; Entity_ApplyWheelSuspensionForces 0x463642/0x463733; Entity_ApplyLightVehicleSuspensionForces 0x463832/0x463923; Entity_UpdateVehicleChassisOrientation 0x468B28; update_vehicle_suspension 0x46937B; Entity_ComputeSuspensionAndOrientation 0x469982; movers 0x476060/0x4785D8/0x478998/0x478C87/0x47BEB7/0x47E47B/0x47E89B/0x4810A0/0x48168D | Entity_BuildOrientationFromVectors 0x45904A; Entity_RebuildOrientationMatrixFromAxes 0x463532; respawn 0x45FFF1; 0x469C49; wheel-solver upright wreck 0x46B4FB; movers 0x479437/0x47C42C/0x47ED60/0x47EE3B/0x481544/0x48177C | **always 0** (no boat-family setter; platform §9 righting clears) — the platform's "write Yaw from fit" gate never opens for boats |
| +0x2ED | ground movers only (flip-threshold/slide/10-tick pulse, §3.1) | mover tails 0x4795DA/0x47C0B6/0x47E7E1/0x47EEEE; respawn 0x460018 | **always 0** |
| +0x2EE | movers when a corner force < -5000 (0xFFFFEC78) while slideDecay < 0 [0x478E10..0x478E20, 0x47884F..0x47885F], aircraft 0x480E90/0x481259, light 0x47B4CE/0x47B986, tracked 0x47E32C/0x47EA2C | solvers [0x46CCC7/0x46B20D]; Entity_UpdateVehicleChassisOrientation 0x468B35; respawn 0x45FFF7; movers 0x478E07/0x47BD55/0x47EA12/0x47ED3A; aircraft 0x48123F/0x48151E | **always 0** → 0x46C8E0's disable machine dead for boats |
| +0x2EF | Vehicle_UpdateTurretRotation 0x45AEF2 (§7); Entity_SmoothHeadingToTarget 0x45B363/0x45B547; movers (many) | platform fn 0x48390D/0x48392E/0x483962/0x483D5E; both solvers 0x46CC95/0x46B1DB; BuildOrientationFromVectors 0x459050; RebuildOrientationMatrixFromAxes 0x46352C; movers | toggles: set only while §9 planing-fast leg runs the lean machine |
| +0x2F0 | platform fn (capsize, spec §9); wheel-solver upright wreck 0x46B4F4 | RebuildOrientationMatrixFromAxes (spec §9); respawn | capsize latch as per main spec |
| +0x2F1 | respawn 0x46001E; movers 0x478BD6/0x478D07/0x47E184/0x47E7EE | movers 0x47777D/0x4777B8/0x47D76A/0x47D7A1 | unused by boats |
| +0x2FC | Entity_UpdateVehicleChassisOrientation 0x468BF3; movers 0x4785D1/0x47DC8F/0x480803 ("wreck settled" → solvers early-return) | respawn 0x46000C | **always 0** |

### 6. Blocker 3 — items.def tokens for itemDef +0x92C..+0x948

Parser: `ItemDef_ParsePhysicsProperty` @ 0x49D870 (token at ctx+4, value string at
ctx+8, `atol`, case-insensitive). All six store RAW atol values — **no parse-time
scale**; scaling happens at the consumer. The "+0x93C" token string is stored inline
at 0x7C7D78 (bytes 62 6F 62 00 = `"bob"`; Hex-Rays mis-renders it as off_7C7D78).

| offset | field | token | store [orig] | consumer & scale |
|--------|-------|-------|--------------|------------------|
| +0x92C | lean | `lean` | 0x49DDDE | roll-lean machines: max lean = lean * 0.1 (flt_7C69F4) * 8192.0 (flt_7C69F0), ftol [orig: 0x45AEF9..0x45AF0F / 0x45B311..0x45B32E] |
| +0x930 | leanVelocity | `lean_velocity` | 0x49DE1A | lean accel factor = lean_velocity * 0.01 (flt_7C56A8) [orig: 0x45AF14..0x45AF22 / 0x45B33B..0x45B34D] |
| +0x934 | pitch | `pitch` | 0x49DE56 | platform §5 pitchThr = pitch * 0.1 * 4096.0 (409.6 heavy-rolled) — bow-lift trigger |
| +0x938 | pitchVelocity | `pitch_velocity` | 0x49DE92 | platform §5 liftHi = pitch_velocity*100 (light) / *q*0.004 (heavy) — bow-lift amount |
| +0x93C | (unk591) | `bob` | 0x49DECE | platform §5 dipExit = pitchThr * (1 - 0.1*bob) — porpoise exit |
| +0x948 | flip | `flip` | 0x49DF82 | ground movers: airborne disable request when fitted |up.z| < flip * 0.01 * 65536 [orig: 0x477742..0x477762, flt_7C56A8 * flt_7C32BC]; clamped [0,100] by the platform fn and [0,55] by the tank contact solve's head [orig: 0x476166..0x476187] |

Clamps at 0x481ACC..0x481BA3 (platform fn, every call): lean [0,30],
lean_velocity [0,20], pitch [0,10], pitch_velocity [0,10], bob [0,10], flip [0,100].

Retail JOX ITEMS.DEF corpus authoring, per move_function:

| vehicle (mf) | lean | lean_velocity | pitch | pitch_velocity | bob | flip |
|---|---|---|---|---|---|---|
| Zodiac, Zodiac w/M60 (cbot) | 10 | 13 | 10 | 3 | 5 | — |
| Mark V, Indonesian Assault Boat (cbot) | 10 | 10 | 7 | 1 | 3 | — |
| civilian boats #1–#4 (cbot) | 10 | 10 | 10 | 2 | — (0) | — |
| Motorcycle (cbike) | 30 | 40 | — | — | — | — |
| M1A1 / T80 (ctank) | 0 | 0 | 0 | 0 | 0 | 15 |
| Taxi (cveh) | 0 | 0 | 0 | 0 | 0 | 45 |
| ATV (catv) | 0 | 0 | 0 | 0 | 0 | 45 |
| BTR-80 (catv) | 0 | 0 | 0 | 0 | 0 | 10 |
| Stryker (catv) | 0 | 0 | 0 | 0 | 0 | 35 |
| SUV/Attack Vehicle 50cal/Mk19 (cveh) | — | — | — | — | — | 35 |
| Fuel Truck (cveh) | — | — | — | — | — | 15 |
| Dune Buggy (cveh) | — | — | — | — | — | 30 |
| Transport Trucks (cveh) | — | — | — | — | — | 10 |

Boats never author `flip` (their capsize path ignores it); ground vehicles never
author the pitch/bob trio. Unauthored = 0 (clamped in range).

### 7. The §9 planing lean machine (byte+0x2EF producer for boats)

`Vehicle_UpdateTurretRotation` @ 0x45AEA0 is misnamed — it is the boat ROLL-LEAN
controller, called ONLY from the platform §9 planing leg as
`(entity, right.x, right.y, right.z)` (right row pushed by value)
[orig: 0x4838B9..0x4838CF, gated on planing bit && afloat && currentSpeed > 0x2000].
Interior (reconstruction):

- sets byte+0x2EF = 1 every call [orig: 0x45AEF2] — so while planing fast, the
  solver's YXZ override (yaw 0, pitch +0xA8, roll +0xAC) is ACTIVE each solve.
- error signal = right.z (arg 4). |right.z| <= 160 → modelPtr2(+0xAC) = 0 and the
  envelope state zeroed [orig: 0x45B25C..0x45B279 leg; state: word +0x470 = decel
  span, dword +0x464 = entry error, dword +0x468 = rate bookkeeping, dword +0x45C =
  prev +0x2B4 sample].
- otherwise +0xAC is driven toward countering right.z: snap legs write
  ±2386092 (0x2468AC; the negative leg -2386092 here — the platform §9 direct write
  uses -2386239) when |error| > maxLean, else ramps `+0xAC += ftol(f(error/decel) *
  ±11930465.0)` (flt_7C69E0/flt_7C69E4 = ∓deg→BAM, 0x7FFF8000/180) with the
  items.def lean/lean_velocity scales of §6 [orig: 0x45AF89..0x45B225].
- platform §9's `+0x464 = 0; word +0x470 = 0` resets are precisely this machine's
  envelope state.

The land sibling `Entity_SmoothHeadingToTarget` @ 0x45B2C0 (same math, speed gate
4096, called only by Entity_ProcessLightVehiclePhysics @ 0x47A7D3) is the catv/cbike
lean — not boat-relevant. Full interior of both left at this contract level (cosmetic
roll shaping; no Z/position effect).

### 8. Blocker 4 — writers of the per-corner probe springs entity+0x2D4..+0x2E0

Array of 4 ints (wheel k at +0x2D4+4k). Complete non-esp writer scan of .text:

| writer | when | value |
|--------|------|-------|
| Entity_RespawnVehicle @ 0x4600F6/FC/0x460102/08 | vehicle (re)spawn | 0 (also zeroes +0x2CC..+0x2E8) |
| Entity_InitDeathSounds block @ 0x493A1E/24/2A/30 | on death (wreck FX/sound reinit) | 0 (xor eax; also zeroes +0x2B4..+0x2C0 and accs +0x2C4..+0x2D0) [orig: 0x4939EA] |
| Entity_InitVehicleAI @ 0x4602C2 | AI vehicle init | writes +0x2D4 (with +0x2D0 = 0xFFFD0000) — AI-brain overlay usage of the same slots |
| Suspension_CompressWheelLinear @ 0x45CEB0 (sites 0x45CEFC) | per tick from Entity_ProcessWheeledVehiclePhysics @ 0x478816/0x478EC2/0x478EEB | `spring_k += deltaTime` ramp, capped at dword_815180 − dword_815184 = 65535 − 52428 = 13107 (0.2 u); side products scaled by itemDef->spring (+0x8FC) shed into the wheel state and entity+0x300 |
| Suspension_CompressWheelQuadratic (quadratic variant, dt²*2*spring) | per tick from light @ 0x47B431/0x47BAD0/0x47BAE4, tracked @ 0x47E2C1/0x47EB68/0x47EB7C, aircraft @ 0x480E48/0x48138A/0x48139E | same ramp/cap |
| Entity_ProcessWheeledVehiclePhysics direct @ 0x4790C7../0x47922B.. (all four, two blocks) | per tick | `spring_k -= step; clamp` decay legs |
| Entity_ProcessVehicleSuspension @ 0x463C60 (sites 0x464C23/64/6E/A7/AF/E4/EC, 0x464DAC/FF/0x464E4D/0x464EA0) | per tick from Entity_ProcessInfantryPhysics @ 0x46F679 and Entity_ProcessAirVehiclePhysics @ 0x47130B | paired write/clamp spring integration |
| PlayerClass_InitEntity @ 0x4B119A.. / Entity_ResetToSpawnState @ 0x4B96AA.. | infantry init/reset | 0 (infantry overlay: leg-chase state shares these offsets) |

**Neither suspension solver, nor Entity_ProcessPlatformPhysics, nor the watercraft
mover (Entity_UpdateWatercraftPhysics @ 0x48D480 and its dispatchers) ever writes
them.** For a pure `cbot` boat the four probe springs are 0 from spawn to death —
the §3 probe Zs reduce to `zb = m[0x28] + q` exactly, and `v210 = -(m[0x28]+q)`.
For `catv` amphibians the land movers' ramped values (up to 13107 = 0.2 u) carry into
the water solve until the next death/respawn zero. Port: initialize to 0, feed the
land-family ramps only when those movers are ported; the boat solve just reads.

(HeliLift_UpdateSlotState @ 0x451923/0x451D28 and the missile/net/UI hits at these
displacement bytes are different structs — excluded by context.)

### 9. New constants (verbatim)

| value | meaning |
|-------|---------|
| flt_7C6F14 = 1.75 | wheel-solver disable rate multiplier, non-authority |
| flt_7C6F18 = 1.25 | wheel-solver disable rate multiplier, authority |
| flt_7C6F1C = 9.765625145519152e-05 (1/10240) | fast-wreck rate = 80*trunc(speed/10240) |
| flt_7C56A8 = 0.01 | lean_velocity scale; flip-threshold scale |
| flt_7C69F0 = 8192.0 | max-lean scale (lean * 0.1 * 8192) |
| flt_7C69E0 / flt_7C69E4 = ∓11930465.0 | deg→BAM (0x7FFF8000/180), lean ramp |
| flt_7C69EC = 180.0, flt_7C3BA4 = 360.0, flt_7C3BA8 = 8.3819032e-08 (360/2^32) | lean machine angle folds |
| flt_7C69E8 = 0.001 | lean deadband factor |
| 0x9000 (36864) | solver steep-fit threshold (|up.z| < 0.5625) |
| 0x2000 | wheel-solver Z rise clamp (0.125 u/tick); wheel-solver planing gate for the lean call |
| 14080 (0x3700) / 4096 (0x1000) | wreck fast/slow speed thresholds (0x46C8E0 / 0x46B140) |
| 16000 (0x3E80) / 8000 / 800 / 80 | wreck spring rates |
| 13107 (= 65535 − 52428, dword_815180 − dword_815184) | ground spring-travel ramp cap (0.2 u) |
| ±2386092 (0x2468AC) | lean-machine roll snap value (platform §9 uses -2386239 on the negative leg) |
| 160 | lean-machine |right.z| deadband |
| 0xFFFFEC78 (-5000) | mover corner-force threshold arming byte+0x2EE |
| 0x7000 | authority |slideDecay| threshold for byte+0x2ED |
| 10 ticks | byte+0x2ED pulse length (via +0x2F8) |

### 10. Port rulings & remaining out-of-scope

1. **Boat effective solver behavior** (all the port needs for D-NET-196): with
   byte+0x2EC/0x2ED/0x2EE/0x2FC ≡ 0 and springs ≡ 0, BOTH solvers reduce for a pure
   boat to: 4-normal fit (§1) → rows 0/1/2 of the pose matrix; Z = plain corner
   average (legs A/B via 0x46C8E0) or positive-corner average with +0x2000 rise clamp
   (leg C via 0x46B140); plus the YXZ override rebuild (0, +0xA8, +0xAC) whenever
   byte+0x2EF is set (capsize legs §9, or the planing lean machine §7). Everything
   else (disable machines, wreck paths, spring blocks) is ground/amphibian shared
   state — port behind the same bytes, or defer with the bytes pinned 0.
2. Do not reproduce the solvedPos.X/Y computation (uninitialized-stack input, output
   never consumed) — keep the caller's seed. Do reproduce solvedPos.Z bit-exactly.
3. Not witnessed (accepted): interiors of Vehicle_ComputeOrientationFrom4Wheels
   @ 0x459310 / Vehicle_ComputeAveragedOrientation @ 0x459A50 (wreck tumble
   integrator), Entity_ComputeBoundingQuad @ 0x45B6E0 (wreck corner rebuild),
   Entity_ApplyBoneAttachmentTransform (mount transform), full lean-machine ramp
   envelope f(error/decel) in §7, Entity_ProcessVehicleSuspension @ 0x463C60 interior
   (air/infantry spring integrator — blocker 4 only needed its writer role), and the
   tracked caller's flags[4]/flags[6] extended block. None are on the boat path.
4. IDB renames worth landing: CWnd_HitTest @ 0x6137D0 →
   Math_ExtractRow1FromFixedPoint22; Vehicle_UpdateTurretRotation @ 0x45AEA0 →
   Vehicle_UpdateRollLean (or similar); Entity_ClearSuspensionForces @ 0x468980 →
   Entity_ApplyAndClearSuspensionImpulses; ItemDef +0x93C unk591 → bobExit ("bob").

---

## §5 The cbik (bike) mover — client subset and family deltas

Witness session 2026-07-31. Target: `Entity_UpdatePlayerInfantryMovement @ 0x483FE0`
(name is a known misnomer; this is the cbike-family mover — class row `cbik` in the
update-callback table `@ 0x82ABC0`, reached via dispatch stub `@ 0x48EFF0`; net-re
§5.38e row). Comparison base: the ground core `Entity_UpdateVehiclePhysics @ 0x48AF00`
(our `VehicleSystem::ground_client_tick` interim carrier for bikes,
`engine/runtime/world/vehicle_motor.cpp:725`).

Decompiler field-path glossary (IDB names, misnomer-tolerant — same struct both movers):
`entity->renderInstance` = the aiComp pointer ("moveMode"); aiComp slots `[132]@+528` =
steer target BAM, `[136]@+544` = commanded speed, `[137]@+548` = seat-steer ramp,
`[177]@+708` = received (wire) speed, `[179]@+716` = received steer, `[4]@+16` = AI
state, `+792` = sound flag byte, `[199]@+796` = engine timer, `[115..126]@+460..+504` =
wheel/handlebar visual block. `entity->aiState` = smoothed steer state,
`entity->modelPtr0` = yaw-rate accumulator, `entity->slideDecay` = vertical velocity
vZ, `entity->currentSpeed`/`speedAccel` = signed speed/accel,
`entity->pendingAnimStateId` = wheel-roll anim accumulator. Status bytes:
`LOBYTE(entity->aimHeading)` = crashed/laid-down, `BYTE2(entity->aiRef0)` = ground
contact (reimpl `m.grounded`), `LOBYTE(entity->aiRef0)` = settled latch. Extension
block (`entity[1].*`, shared layout with the ground core): `pad_040[5]` = brake-lock
latch, `pad_040[21]/[22]` = jump-edge/airborne latches, `pad_040[24/28/32]` = air
travel-direction vec, `huskModel/huskFinalModel/CharacterEntity` (as int32×3) =
tire-slip direction memory vec, `CameraOffset.Y` = slip-start tick stamp,
`pad_0d8[12]` = lean-state magnitude. Entity `Flags` bit `0x2000` = airborne/swimming
(repo `kEntityFlagInAir`), `0x8000` = submerged/drowning, `2` = dead/wreck.

### 1. Block map — what runs where

All machines (client incl., every tick): prologue — `savedLivePose` recapture from the
live pose + `bodyHeading/Pitch/Roll` capture `[orig: @ 0x484054..0x484080]`; euler
matrix build + forward/up extraction `[orig: @ 0x484010..0x484046]`; null-aiComp bail
to the epilogue `[orig: @ 0x484086]`; AI-state default 22 `[orig: @ 0x48408c]`; ground
raycast every 8th frame `(dword_24C1948 & 7) == 0` →
`Entity_RaycastGroundHeightAndObject(entity, 0, 0, 0x10000, 0x200000) @ 0x414320`
`[orig: @ 0x4840a0..0x4840b1]`; death → state 21 `[orig: @ 0x4840c7..0x4840d6]`;
damage presentation (smoke < healthMax/4 `[orig: @ 0x4841be..0x4841d6]`, fire +
critical loop sound every 32 ticks below criticalHp `[orig: @ 0x48414a..0x4841a1]`);
carrier/deck follow — full 22.10 trig re-anchor of position + yaw/pitch/roll onto a
moving `groundEntity` `[orig: @ 0x4841fc..0x484637]` (port-scoped to the row-level
seat-follow, §5.38e).

Authority only: health regen every 64 ticks `[orig: @ 0x4840e4..0x48411f]`; critical
drain `[orig: @ 0x48414a..0x484163]`; drowning damage −2 `[orig: @ 0x4865f9..0x48661c]`.

Client only (`!is_authority`): the chase block + register mirror `[orig:
@ 0x484648..0x484930]` — §2.

Authority ∥ local-driver only (`is_authority || occupantEntity == g_local_player_entity`):
the input block `[orig: @ 0x484a92..0x4851e0]` — occupant MoveOrder/analog decode, seat
gear cases, AI-drive leg, pool-1 stochastic avoid brake, no-driver settle. A REMOTE
bike on a client skips all of it.

All machines again (the prediction physics a remote bike runs between records, from
mirrored registers): §3 — brake/jump/crash latches, turn servo, speed servo,
traction/slip, gravity, integration, the light-vehicle contact solve, yaw apply, wheel
servo, sounds/effects. Epilogue: orientation matrix rebuild + `Flags |= 0x20000`
`[orig: @ 0x486a04..0x486a17]`.

### 2. The client chase — the vehicle template, ground/water constant set

Confirmed the SAME template and constant set as the ground core (line-for-line), with
two bike-specific yaw gates. On `!is_authority`:

- AI state forced 22 unless dead `[orig: @ 0x484652..0x484658]` (ground:
  `@ 0x48b556..0x48b55c`).
- Fresh record (`interpProgress == 0` `[orig: @ 0x48465f]`): snap threshold `0x60000`,
  demoted to `0x20000` when received speed `[177] < 293` (signed) `[orig:
  @ 0x48467d..0x484684]`; 3D distance staged-target − savedLivePose, float sqrt with
  the `flt_7C19E0 = 2147418112.0` overflow clamp `[orig: @ 0x48463a, 0x4846b9..0x4846e2]`.
  - **Snap arm** (dist > threshold): Position X/Y/Z ALL snapped unconditionally
    `[orig: @ 0x484704..0x48470a]`; Yaw snapped only if `!crashed && !settled &&
    !airborne-latch(pad_040[22]) && up.z >= 0` (upright — up vector from the live
    euler matrix) `[orig: @ 0x484725..0x48472d]`; staged cluster + bucket zeroed
    `[orig: @ 0x484730..0x484749]`.
    GROUND DIFF: the ground core gates Z-snap AND Yaw together on the two-flag gate
    (`!crashed && !settled`) and snaps only X/Y unconditionally `[orig:
    @ 0x48b602..0x48b622]`; the bike adds the airborne + upright conditions and moves
    Z to the unconditional side.
  - **Deadband arm** (dist < `0x2000`): staged position zeroed, bucket 0, heading step
    still computed `(Δ + 10) / 20` `[orig: @ 0x48476c..0x484794]`. Same as ground
    `[orig: @ 0x48b658..0x48b68d]`.
  - **Bucket ladder** (else): default 30; `< 0x2AAA`→6, `< 0x4000`→8, `< 0x5555`→10,
    `< 0x8000`→15, `< 0x10000`→20, `< 0x20000`→25 `[orig: @ 0x4847a5..0x484815]`.
    Steps `((N>>1) + d) / N` per axis re-stored into the staged slots `[orig:
    @ 0x484833..0x484851]`; heading step `(Δ + 10) / 20` `[orig: @ 0x484868]`.
    Identical to ground `[orig: @ 0x48b69e..0x48b761]`.
- Per-tick application: heading while `progress < 20`, gated `!crashed && !settled &&
  !airborne-latch && up.z >= 0` `[orig: @ 0x48489c..0x4848a4]` — GROUND DIFF: ground
  applies it UNGATED `[orig: @ 0x48b772..0x48b77a]`. Position X/Y while
  `progress < bucket`; the Z step only when `(Flags & 0x2000) && !settled && !crashed`
  `[orig: @ 0x4848ae..0x4848e3]` — identical gate in ground `[orig: @ 0x48b7aa..0x48b7b9]`
  (on contact, Z is contact-solve-owned).
- Starvation: `progress >= 128` → received speed decays `[177] -= ([177] + 64) >> 7`
  (full signed 32-bit), else `progress++` `[orig: @ 0x4848ee..0x48490a]`. Identical to
  ground `[orig: @ 0x48b7c0..0x48b7e4]`.
- **Register mirror** (the prediction feed): when `occupantEntity !=
  g_local_player_entity`: `[136] = [177]` (speed) and `[132] = [179]` (steer)
  `[orig: @ 0x48491c..0x484930]`. Identical slots + gate in ground `[orig:
  @ 0x48b7f6..0x48b80a]`.

Verdict for this section: SAME template, ground/water constant set — not the AIR set.
The only deltas are the extra yaw-suppression gates (airborne latch, upright) and the
unconditional Z-snap.

### 3. The between-records prediction physics (client-executed for a remote bike)

Runs below the input gate on every machine, driven by the mirrored `[136]/[132]`:

1. Brake-lock latch: `occupant && (Flags & 8) && !airborne-latch && lastAttacker > 0`
   → `pad_040[5] = 1` and `[136] = 0` `[orig: @ 0x485204..0x48522d]`. Jump latch:
   `(Flags & 0x20) && currentSpeed > 4096` → `pad_040[21] = pad_040[22] = 1`
   `[orig: @ 0x485243..0x48524c]`. Crash pair `HIBYTE && LOBYTE(aimHeading)` →
   `[136] = 0` `[orig: @ 0x48525c..0x485269]`. (Inert for a remote bike until the
   occupant/crash flags replicate — Flags bits 8/0x20 are set by the input block.)
2. Turn servo (identical template + constants to ground `[orig: ground
   @ 0x48c098..0x48c18f]`): `minRate = turnRate >> 2` (or `turnRate2` if nonzero);
   `f = 0x10000 − (speed << 16)/playerSpeed` clamped ≥ 0;
   `eff = ((turnRate − minRate)·f + 0x8000) >> 16 + minRate`; `Δ = (steerTarget −
   Yaw + 32) >> 6` clamped ±eff; `steerState += (4 − 32Δ − steerState) >> 3`
   `[orig: @ 0x48526f..0x485329]`. Yaw rate `modelPtr0 = (−speed·(steerState >> 2)
   + 0x8000) >> 16`, recomputed when `!(Flags & 0x2000)` `[orig: @ 0x48532f..0x485358]`
   — GROUND DIFF: ground additionally requires `!crashed && !settled` `[orig:
   @ 0x48c15f]`.
3. Speed servo: slope factor cos²(pitch) via the shared table
   `off_849934[(Pitch + 0x200000) >> 22]`, squared `>> 22`; `target = cos²·[136]`
   `[orig: @ 0x485377..0x4853a8]` (same as ground `[orig: @ 0x48c1c6..0x48c1fa]`).
   Then:
   - Jump-latched (`pad_040[22] && pad_040[21]`): `speed < 0x4000` →
     `speedAccel += 25` per tick (throttle ramp); else the normal servo
     `(target − speed + 16) >> 5` `[orig: @ 0x4853ac..0x4853eb]`.
     GROUND DIFF: ground has NO ramp; while off-contact it sign-flips the command
     into a coast brake `[orig: @ 0x48c1ab..0x48c1c0]` and clamps accel to
     `±(deceleration >> 1)` `[orig: @ 0x48c471..0x48c48e]`.
   - Crashed (`LOBYTE(aimHeading)`): `speedAccel = ∓itemDef+0x8E4` (a bike-only
     crash-brake rate field) `[orig: @ 0x485687..0x4856bf]`.
   - Traction clamps otherwise: same-direction drive `±acceleration`, zero target
     `±deceleration`, reversal keeps the raw 1/32 chase; tire-slip window active
     (slip stamp within `5 × tireSlip` ticks) uses `±(acceleration >> 4)`, expired
     slip forces `∓deceleration` `[orig: @ 0x485417..0x4855d7, window compare
     @ 0x4854ac/0x485756]` (same tree + constants as ground `[orig:
     @ 0x48c28a..0x48c466]`).
   - Integration: `currentSpeed += speedAccel`, deadband `|speed| < 48 → 0` when
     target = 0, `speedAccel == 0 → speed = target` — GATED on contact:
     `!crashed && !(Flags & 0x2000) && BYTE2(aiRef0)` `[orig: @ 0x485501..0x485534]`.
     GROUND DIFF: ground integrates UNGATED `[orig: @ 0x48c302..0x48c32a]`.
4. Lean/anim accumulators (occupant not braking): lean magnitude from `dword_81518C`
   (also read by the ground core `[orig: @ 0x48c49f]` — shared, not a bike
   differentiator), wheel-roll `pendingAnimStateId += 0x2000·speed + |leanState|`
   `[orig: @ 0x485803, 0x485827]`, lean fade factor `(1 − speed·flt_7C6F98)`,
   `flt_7C6F98 = 1/61440` `[orig: @ 0x485839..0x485862]`.
5. Velocity build + tire-slip direction memory (same template as ground `[orig:
   ground @ 0x48c6xx..0x48cfe3]`): velocity = `speed ×` the stored slip-direction
   vec while slipping, else the live forward vec, normalized ×`flt_7C32BC = 65536.0`;
   slip release when `dot(slipDir, fwd) >= 61166` (≈0.9333 in 16.16, ~cos 21°); slip
   direction relaxed ±`11930464` BAM (exactly 1.0°) per tick toward forward via
   `Math_BuildFixedPointRotationMatrixYXZ`; below `|speed| > 256` the slip vec is
   dropped; slip stamp `entity[1].CameraOffset.Y = current_tick`
   `[orig: @ 0x4858e5..0x486049; dot @ 0x485ceb, ±1° @ 0x485d6b/0x485d7a,
   256 @ 0x485ea9, stamp @ 0x485a67]`. Airborne (`pad_040[22]`): velocity = speed ×
   the normalized stored air-direction vec `pad_040[24/28/32]`
   `[orig: @ 0x486052..0x486145]`.
6. Crashed slide (`LOBYTE(aimHeading)`, no contact-dir): `|speed|` clamped to
   `±24576` `[orig: @ 0x486237..0x48624f]`; velocity = speed × normalized previous
   velocity; speed decays `∓deceleration` per tick `[orig: @ 0x4862e1..0x486307]`;
   `|speed| < 4096` → velocities and speed halved per tick, yaw-rate accumulators
   zeroed, settled latch set `[orig: @ 0x48638b..0x4863c0]` (ground has the same
   <4096 settle `[orig: @ 0x48cf4c..0x48cf83]` but no 24576 clamp / per-tick decel
   decay).
7. Vertical: up-cap `vZ = min(vZ, 0x4000)` when `(!airborne-latch || (Flags &
   0x2000))` `[orig: @ 0x48659b..0x48659d]` — NO ground equivalent. **Gravity
   `vZ -= 250` per tick `[orig: @ 0x4865a6]` — the ground core uses 324
   `[orig: @ 0x48d009]`.** Submerged (`Flags & 0x8000`): planar + vertical velocity
   damped `v -= (v + 2) >> 2` (×3/4 per tick) `[orig: @ 0x4865bb..0x4865ed]` (same
   as ground `[orig: @ 0x48d020..0x48d052]`).
8. Position integration: `X += 2·(vX·flt_7C3B94)`, `Y += 2·(vY·flt_7C3B94)`,
   `Z += 2·(vZ·flt_7C3B94)` with `flt_7C3B94 = 0.5` — i.e. exactly `pos += v`,
   identical to the ground core's direct add `[orig: @ 0x486623..0x48666e; ground
   @ 0x48d090..0x48d0a9]`.
9. Contact/attitude solve: `Entity_ProcessLightVehiclePhysics(entity, frame, 1)
   @ 0x479600` `[orig: call @ 0x486672]` — GROUND DIFF: ground calls
   `Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0` `[orig: call @ 0x48d0b1]`. The
   bike's terrain pose + lean/roll attitude live in the light solve (the B-facet
   is ported in §9; only the grounded lean smoother remains deferred).
10. Yaw application: ALWAYS applied — `Yaw += modelPtr0`, quartered (`>> 2`) when
    `Flags & 0x2000` `[orig: @ 0x486681..0x486697]`. GROUND DIFF: ground gates on
    `!(Flags & 0x2000) && BYTE2(aiRef0) && !crashed && !settled` and never quarters
    `[orig: @ 0x48d0d4..0x48d0e3]`.
11. Wheel/handlebar visual servo: aiComp `[118]@+472` stepped ±`0x2108421`
    (= 34636833 BAM ≈ 2.90°) per tick toward `[124]@+496`, else the 6-dword visual
    block latch `[115..120] ← [121..126]` `[orig: @ 0x48669a..0x4866ed]` — identical
    in ground `[orig: @ 0x48d104..0x48d150]`.
12. Client-visible effects/sounds: trail effects — submerged: every 2nd tick,
    channels 3/4 fed `[136]`-reg/`currentSpeed`; else every 4th tick, channels 1/2
    clamped ≥ 0 (`Entity_UpdateBoneTrailEffects @ 0x4589C0`)
    `[orig: @ 0x486177..0x4861e9]`; engine/skid/horn sound machine on the `+792`
    flag byte with itemDef sound handles `defaultResPlus64 +96/+100/+120/+124/
    +128/+132` `[orig: @ 0x486710..0x4869e0]`;
    `Entity_UpdatePartSpinAccumulator @ 0x4928B0` on rideables
    `[orig: @ 0x4869ea]`; engine timer `[199]++`, jump-edge latch `pad_040[21] = 0`
    `[orig: @ 0x4869f2..0x4869f9]`.

### 4. Input-block family deltas (authority/local-driver only — for the record)

Seat-direction template identical to ground (ramp step `0x16C16C0` = 2.0° BAM/tick,
cap `596523200` = 50°, analog steer scale `192426`, crouch/prone speed `>>1`/`>>2` on
MoveOrder `0x200`/`0x100`, free-look bit `0x10`, local-driver client blend
`[136] = ([136] + [177]) >> 1` `[orig: @ 0x484dad; ground @ 0x48bbef]`) EXCEPT:
reverse gear cases 3/4/5 command `−[136] >> 3` (1/8 speed) `[orig: @ 0x484d0f,
0x484d2b, 0x484d50]` vs the ground core's `−[136] >> 1` (1/2) `[orig: @ 0x48bb89]`.
The modifier-bit writes appear twice back-to-back in the bike (source-level
duplication, port once is NOT faithful — keep the duplicate writes' net effect; they
are idempotent) `[orig: @ 0x484bfb..0x484c3a]`. Bike-only no-driver leg: crew check
`Entity_CountMountedEntities < itemDef->minAI` clamps Health to criticalHp
`[orig: @ 0x484dfe..0x484e33]`.

### 5. Constants table (client subset)

| constant | value | site |
|---|---|---|
| snap threshold (fast) | `0x60000` | `[orig: @ 0x48467d]` |
| snap threshold (slow), speed reg < 293 | `0x20000` | `[orig: @ 0x484682..84]` |
| deadband | `0x2000` | `[orig: @ 0x484759]` |
| buckets | 6/8/10/15/20/25/30 at `0x2AAA/0x4000/0x5555/0x8000/0x10000/0x20000` | `[orig: @ 0x4847a5..0x484815]` |
| position step | `((N>>1)+d)/N` | `[orig: @ 0x484833..51]` |
| heading step / window | `(Δ+10)/20`, applied while progress < 20 | `[orig: @ 0x484868, 0x48489c]` |
| starvation | progress ≥ 128 → `[177] -= ([177]+64)>>7` | `[orig: @ 0x4848ee..0x48490a]` |
| mirror | `[136]←[177]`, `[132]←[179]`, occupant ≠ local | `[orig: @ 0x48491c..0x484930]` |
| float clamp | `flt_7C19E0 = 2147418112.0` | `[orig: @ 0x48463a]` |
| turn floor | `turnRate>>2` or `turnRate2` | `[orig: @ 0x485280, 0x4852c3]` |
| steer smoothing | `(4 − 32Δ − s)>>3` | `[orig: @ 0x485326]` |
| yaw rate | `(−speed·(s>>2)+0x8000)>>16` | `[orig: @ 0x485358]` |
| speed servo | `(target−speed+16)>>5` | `[orig: @ 0x4853d2, 0x4853eb]` |
| jump throttle ramp | `+25`/tick below speed `0x4000` | `[orig: @ 0x4853c2, 0x4853c0]` |
| jump latch arm | `Flags&0x20 && speed > 4096` | `[orig: @ 0x485243]` |
| speed zero deadband | 48 | `[orig: @ 0x485528]` |
| slip window | `5 × itemDef->tireSlip` ticks | `[orig: @ 0x4854ac, 0x485756, 0x485afb, 0x486548]` |
| slip accel clamp | `±(acceleration>>4)` | `[orig: @ 0x4854c7..0x4854e3]` |
| slip release dot | `61166` (16.16 ≈ 0.9333) | `[orig: @ 0x485ceb]` |
| slip relax rate | `±11930464` BAM = 1.0°/tick | `[orig: @ 0x485d6b, 0x485d7a]` |
| slip min speed | 256 | `[orig: @ 0x485ea9]` |
| crash speed clamp | `±24576` | `[orig: @ 0x48623d, 0x48624f]` |
| crash brake rate | `itemDef+0x8E4` | `[orig: @ 0x485692, 0x4856b4]` |
| settle threshold | `|speed| < 4096` → halve all | `[orig: @ 0x48638b..0x4863a2]` |
| vZ up-cap | `0x4000` | `[orig: @ 0x48659b]` |
| **gravity** | **250/tick** (ground: 324) | `[orig: @ 0x4865a6; ground @ 0x48d009]` |
| water damping | `v -= (v+2)>>2` | `[orig: @ 0x4865bb..0x4865ed]` |
| integration scale | `flt_7C3B94 = 0.5`, doubled → `pos += v` | `[orig: @ 0x486623..0x48666e]` |
| in-water yaw | `modelPtr0 >> 2` when `Flags&0x2000` | `[orig: @ 0x486697]` |
| wheel servo step | `0x2108421` (≈2.90°)/tick | `[orig: @ 0x4866b8..0x486704]` |
| lean fade | `flt_7C6F98 = 1/61440` | `[orig: @ 0x485839]` |
| wheel-roll accum | `+ 0x2000·speed + |lean|` | `[orig: @ 0x485803, 0x485827]` |
| raycast cadence | every 8th frame, args `(0,0,0x10000,0x200000)` | `[orig: @ 0x4840a0..b1]` |
| contact solve | `Entity_ProcessLightVehiclePhysics @ 0x479600`, mode 1 | `[orig: @ 0x486672]` |

### 6. Ground-core interim: divergence inventory

Identical (no visible divergence riding `ground_client_tick`): the whole chase
template + constants, register mirror, starvation decay, turn servo, cos² slope
factor, 1/32 speed servo + traction clamp tree, 48-deadband, integration scale,
wheel servo, water damping, submerged trail cadence.

Divergent (bike vs what the interim runs):
1. **Gravity 250 vs 324** — remote bikes fall ~30% too fast in the interim; jump and
   drop arcs land visibly short/hard between records.
2. **Airborne speed handling** — interim (ground) sign-flips the command into a coast
   brake + clamps to half-decel; the bike holds throttle (+25 ramp below `0x4000`)
   and skips speed integration entirely while off-contact. A remote jumping bike
   decelerates mid-air under the interim until the next record corrects.
3. **Airborne/water yaw** — interim gates yaw application on grounded; the bike
   always applies it (quartered in water). A mid-air remote bike stops yawing under
   the interim.
4. **vZ up-cap `0x4000`** absent in the interim (minor).
5. **Chase yaw gates** — the bike suppresses heading snap/steps while
   airborne-latched or upside-down and snaps Z unconditionally; inert until those
   states are simulated client-side (they derive from the input block + light
   solve).
6. **Contact/attitude solve** — light (`@ 0x479600`) vs tracked (`@ 0x47C1C0`):
   owns bike lean/roll. The dedicated light solve is PORTED (§9, 2026-08-06):
   a parked bike rests at wheel clearance and conforms pitch/roll, while a
   contacted crashed bike now enters the witnessed `+0x2FC` fall-over arm. The
   grounded lean smoother remains the open attitude witness.
7. Crash/brake-lock/slip legs differ in detail (24576 clamp, `+0x8E4` brake, 1/8
   reverse) — all input-side or crash-state-side, unreachable for a remote bike
   until occupant/crash replication lands.

---

## §6 The aircraft contact/suspension solve (ported 2026-08-01)

Source: the formerly-UNDEFINED region at **0x47EF10..0x481867** (retail Jointops.exe,
imagebase 0x400000, IDB Jointops.exe.kong.i64). Defined as a function this session
(2026-07-31): a misdecoded instruction run at 0x480051 (`db 8Bh,4Eh` + garbage) was
re-created as code (`mov ecx,[esi+20h] / mov ecx,[ecx+908h] / ...`), then
`add_func(0x47EF10)` succeeded → function 0x47EF10..0x481867 (0x2957 bytes, 2732
instructions). Named **`Entity_ProcessAircraftContactPhysics`**, prototype
`int __cdecl(GamePlayerEntity *entity, int check_water_depth)`, function comment set,
IDB saved. Decompiled clean (2072 lines); every FPU-garbled block below was
re-derived from the full instruction dump (a full instruction-level disassembly dump, session artifact).

This was the tracked D-NET-196 stand-in target: our `aircraft_client_tick`
substituted a terrain clamp for this call until the 2026-08-01 port
(`aircraft_contact_solve`). Companion to `aircraft_plane_client_spec.md`
(the mover) and `watercraft_client_spec.md` — same conventions: 16.16 fixed positions,
32-bit BAM angles, 22-bit trig (`cos22`), `dbl_7C3608` = 1.4629627251502471e-09 BAM→rad,
`dbl_7C3600` = 2^22, `flt_7C19E0` = 2147418112.0 ftol clamp, 62 Hz tick.

### 0. Identity and call contract

- **Sole xref in the whole image**: `call sub_47EF10` @ 0x49254E inside
  `Entity_UpdateAircraftPhysics` @ 0x490310 — `push 0; push esi(entity); call`
  [orig: 0x492543..0x49254E]. So `check_water_depth` (arg2) **== 0 always in retail
  JO**: every `if (a2)` block (per-point water-DEPTH accumulation into the contact
  slots at 0x480162/0x480199/0x4801CF/0x480203) is dead code. With a2=1 the pad
  "contact" depths would also include water-surface support (amphibious floats) —
  ignore for the port.
- Called UNGATED (authority AND client), every tick, for helicopters AND planes
  (cpln thunks to the same mover), AFTER `Position += velocity` /
  `Position.Z += slideDecay` and BEFORE the attitude-rate integration
  [orig: mover §8, 0x49250A..0x49254E .. 0x492607].
- Returns the collision **severity** (0..3) from the object/terrain force pass; the
  caller ignores it (`add esp,8` @ 0x492553) but reads back `Flags & 0x2000` for the
  touchdown thud (was_airborne && now-grounded && slideDecay < -3000)
  [orig: 0x492556..0x4925A6].
- Shape note: the function is the air-family instance of the per-family contact
  solves (`Entity_ProcessWheeledVehiclePhysics`, `Entity_ProcessLightVehiclePhysics`,
  `Entity_ProcessTrackedVehiclePhysics` @ 0x47C1C0, `Entity_ProcessPlatformPhysics`
  @ 0x481870 — it sits in the gap between the last two). Everything below says
  "wheel"/"pad" for the four gear contact points; for aircraft these are the landing
  gear footprint corners of the model's bound block.

### 1. Constants (verbatim)

| addr | value | role |
|------|-------|------|
| flt_7C333C | 0.25 | per-tick pad free-fall step factor when wreck-rest (+0x2F0) [orig: 0x47EF2A] |
| flt_7C3DC8 | 0.75 | same factor, normal state [orig: 0x47EF32] |
| flt_7C6F7C | 250.0 | pad free-fall step scale → step = ftol(factor*250) = **187** normal / **62** wreck [orig: 0x480E0F, 0x480736, 0x4811E4] |
| flt_7C56A8 | 0.01 | springComp normalization; also tail energy threshold [orig: 0x47F23C, 0x481790] |
| flt_7C3B94 | 0.5 | spring-force ½ factor (all k·d² terms), and the ½ in Suspension_CompressWheelQuadratic's energy drain [orig: 0x4812A9, 0x481338, 0x48053E] |
| flt_7C6F18 | 1.25 | spring impulse → energy(+0x300) gain factor [orig: 0x4812C0] |
| flt_7C6F6C | 0.025 | hard-landing damage factor, mass>3 (dead path, §7) [orig: 0x480527] |
| flt_7C6F70 | 0.005 | hard-landing damage factor, mass<=3 (dead path) [orig: 0x48051B] |
| flt_7C6EB8 | 2.5e-05 | severity-3 object-impact damage factor, mass>3 [orig: 0x47FACE] |
| flt_7C6F74 | 1.25e-05 | severity-3 object-impact damage factor, mass<=3 [orig: 0x47FABA] |
| flt_7C32BC | 65536.0 | 16.16 normalize scale for axis vectors [orig: 0x47F380 etc.] |
| flt_7C19E0 | 2147418112.0 | ftol overflow clamp [orig: throughout] |
| dbl_7C57B8 | -683565275.5764316 | -(rad→BAM) for the two fpatan headings (severity-3 deflection calc) [orig: 0x47FD76] |
| flt_7C69FC | 1.57 | oscillator phase init (Suspension_CompressWheelQuadratic writes block+0x14) [orig: 0x45CFC8] |
| flt_7C59B0 | -0.5 | energy(+0x300) drain factor in Suspension_CompressWheelQuadratic [orig: 0x45D059] |
| dword_815180 | 0xFFFF | global spring range constant (16.16 ≈ 1.0); `delta_time` local = 0xFFFF>>4 = **4095** [orig: 0x47F8EB..0x47F8FF] |
| dword_815184 | runtime | = ftol(0xFFFF·(100−springComp)·0.01), recomputed **every call** (shared global, per-entity value!) [orig: 0x47F228..0x47F247] |
| 0x1388 (5000) | | dead crush-gate sink threshold [orig: 0x480436] |
| 0x1B58 (7000) | | dead inverted-crush sink threshold [orig: 0x480788] |
| 0xBB8 (3000) | | dead landing-thud slideDecay threshold [orig: 0x4804A2] |
| 0x3A98 (15000) | | dead hard-landing damage slideDecay threshold [orig: 0x4804F7] |
| 0xFFFE7960 (−100000) | | dead inverted-crush slideDecay threshold [orig: 0x4807AD] |
| 0x7274 (29300) | | unitType-3 object-impact kill threshold (16.16 u/tick, both speed AND tick displacement) [orig: 0x47FA67/0x47FA7A] |
| 0x928 (2344) | | severity-3 scrape-sound speed threshold [orig: 0x47FB1A region] |
| 0x8000 (32768) | | severity-3 deflection: min planar distance from max-push point to center [orig: 0x47FD3C] |
| 0x186A0 (100000) | | min-depth scan init [orig: 0x480F55] |
| 0x6000 (24576) | | right.z bank limit for the settle flag [orig: 0x4803EB] |
| 0x1000 (4096) | | up.z uprightness threshold (16.16, 0.0625) [orig: 0x47F08B, 0x4803B3, 0x480413] |
| 0x2000 (8192) | | up.z park-exit threshold (§13) [orig: 0x4816B1 region] |
| 0x4000 / 0x1000 / 0x2000 | | spine-point radius derivation biases (§3) [orig: 0x47F6BB..0x47F6F0] |
| 0x1000000 | | per-wheel spring-force overflow clamp [orig: 0x4812B8] |
| 0xC8 (200) | | Flags 0x40 (recent-object-collision latch) expiry ticks vs +0x3B8 [orig: 0x47F19E] |
| 350 (0xFFFFFEA2 as −350) | | sleep window: −350 < slideDecay < 0 [orig: 0x47EFD5/0x47EFDB] |
| 32 (0x20) | | currentSpeed "at rest" threshold for wreck-rest latch [orig: 0x480890] |
| 8000 / 800 | | wheel contact-block spring k: pad touching / free (written by the sub-solve) [orig: 0x46B760/0x46B73A] |
| 10 | | park k written into contact blocks w0/w3 on park-enter [orig: 0x4815B4] |

Trig thresholds from the def, computed once per call [orig: 0x47F24C..0x47F287]:
`threshold_hard = ftol(2^22 · cos(itemDef->slipSlope · BAM2RAD))` (+0x8F8),
`threshold_soft = ftol(2^22 · cos(itemDef->maxSlope · BAM2RAD))` (+0x8F4).

### 2. Field map

ItemDef (IDB member names — these are the ITEMS.DEF keys):

| offset | name | use here |
|--------|------|----------|
| +0x54 | attrib | hit-entity check: byte & 0x40 (drivable) gates momentum exchange |
| +0x182 | criticalDrain (int16) | engine-dead health floor: smoke release at entry, burn-down target |
| +0x196 | unitType (int16, low byte) | **== 3 → object-impact = instant kill** at >29300 speed+displacement; else scaled damage |
| +0x1B8 | scale (float) | nonzero → scaled matrix build |
| +0x864 | defaultResPlus64 | sound-slot page: +0x68 = severity-3 scrape, +0x90 = hard-landing (dead path) |
| +0x8F4 | maxSlope | BAM angle → cos22 = threshold_soft |
| +0x8F8 | slipSlope | BAM angle → cos22 = threshold_hard |
| +0x8FC | spring | clamped to [0,10] **in the def, in place** [orig: 0x47F1DA..0x47F1F8]; spring constant |
| +0x900 | springComp | clamped to [0,100] in place [orig: 0x47F1FE..0x47F21F]; compression range % |
| +0x904 | shock | clamped [0,10] inside the free-decay oscillator (0x45D110) |
| +0x908 | mass | damage scaling, momentum exchange, settle-flag mass<=10 gate |
| +0x91C | torque | collision speed-shed shift: severity 1/3 → speed −= speed>>(torque+2); severity 2 → >>(torque+1) |

GamePlayerEntity — the vehicle overlay of the 0x388-byte entity (IDB names at these
offsets are infantry misnomers; roles below are the witnessed vehicle semantics):

| offset | role | witness |
|--------|------|---------|
| +0x04/08/0C | Position X/Y/Z | displacement 0x48014A..0x480150; Z writes 0x480EC9/0x48110A/0x4814C4/0x481849 |
| +0x10/14/18 | Yaw/Pitch/Roll | euler writes 0x480F33../0x48149D.. |
| +0x20 | itemDef | |
| +0x24 | Flags | 0x10 park (authority-produced), 0x40 recent-collision latch, 0x2000 airborne, 0x8000 in-water, 0x4000000 invulnerable |
| +0x30 | graphicModel; +0xB0 = bound block (see §3) | 0x47F1CA/0x47F1D1 |
| +0x5C | pending-impulse gate: > 0 blocks sleep | 0x47EFE7 |
| +0x60 | suspension-override gate: > 0 blocks corner-target adjust, freezes depths to max | 0x48120F, 0x4813F0 |
| +0x64 | AiBrain*; brain byte +0x318 bit 0x10 = scrape-sound latch | 0x47F93E/0x47FB09 |
| +0x98/9C/A0 | velocityX/Y, slideDecay (vertical vel) | sleep test, impact mag |
| +0xA4/A8/AC | yaw/pitch/roll rates | sleep test; +0xAC selects +0x364 vs +0x365 on park-freeze |
| +0x11E | Health (int16) | authority damage |
| +0x158 | animData: nonzero → scaled matrix build | 0x47F28E |
| +0x170 | occupantEntity | sleep gate |
| +0x178 | overlayFlags — zeroed with every health kill | |
| +0x1CC | smoke emitter handle (wreck) | 0x47EF54/0x480925 |
| +0x29C | currentSpeed | severity sheds |
| +0x2C4..+0x2D0 | per-pad free-fall "sink" accumulators (4 dwords). **Zeroed at every solve entry** [0x47F1B2..0x47F1C4]; += step(187) for free pads in the extend loop; zeroed per-pad on contact [0x4814CF..0x4814F9]; resolved values written back by the suspension sub-solve; read next tick only by the sleep gate | |
| +0x2D4..+0x2E0 | per-wheel spring compression (written by Suspension_CompressWheelQuadratic/0x45D110); **added to the pad body-frame Z** when building probe points | 0x47F514/0x47F56A/0x47F5E3/0x47F63F |
| +0x2EC | byte: parked latch | producer 0x48168D (park-enter), 0x46B1F9 (sub-solve); cleared 0x480831/0x481544/0x48177C |
| +0x2ED | byte (BYTE1): used by the sub-solve pathing | 0x46B1A6 |
| +0x2EE | byte: hard-sag latch (dead producers here in practice) | 0x480E90/0x481259/0x48123F/0x48151E |
| +0x2EF | byte: park-freeze latch (with +0x2EC → Z += maxdepth & grounded) | 0x480DED; cleared 0x48082B/0x48153D |
| +0x2F0 | byte: wreck-rest latch (0.25 fall factor, forced grounding, burn smoke) | producer 0x4808A1, 0x46B4F4; cleared 0x481783 |
| +0x2F2 | byte: settled/stable-contact flag (cosmetic consumer outside) | 0x47F09B/0x4803C8/0x48041B/0x480424 |
| +0x2FC | byte: suspension-reset marker | 0x480803; consumed 0x480882, sub-solve 0x46B419 |
| +0x300 | spring energy accumulator | 0x4812CB, drained in Suspension_CompressWheelQuadratic, zeroed in tail 0x4817C4 |
| +0x304 + 0x18·i (i=0..3) | per-wheel oscillator block: +0 amplitude, +4 remaining range (reset 0xFFFF each tick @ 0x4806F5), +8 spring force (clamp-to-0x1000000 on sign overflow), +0x14 phase (float, init 1.57, +0.262/tick in free decay) | |
| +0x364 / +0x365 | bytes: park-freeze cause (rollRate!=0 → +0x364 else +0x365) | 0x480DE6..0x480DFF |
| +0x368 + 0x14·i (i=0..3) | per-wheel CONTACT block: [nx,ny,nz,(pad),k] — k=8000 touching / 800 free (sub-solve), 10 on park-enter; = entity dwords 218..237, cleared by Entity_ClearSuspensionForces | 0x46B73A.., 0x4815B9.. |
| +0x3B8 | last object-collision tick (Flags 0x40 expiry, 200 ticks) | 0x47F192..0x47F1AC |
| +0x3BC/C0/C4 | cached contact-plane vector; lazily seeded from the right axis and normalized ×65536 when pads 2&3 touch and byte +0x3CD set | 0x480F76..0x481075 |
| +0x3CD | byte: enables the +0x3BC seeding | 0x480F76 |
| +0x3CE | byte: "solve ran" flag, = 1 every tail | 0x4817D3 |
| +0x3D4 | airborne tick counter: ++ when Flags&0x2000 else = 0 | 0x481801..0x481851 |
| +0x4E8/4EC/4F0, +0x534..+0x540, +0x364, entity[1] matrix | park-pose visual state zeroed on unpark | 0x480837..0x48087C |

Model bound block `B = *(graphicModel + 0xB0)` (dword indices) [orig: 0x47F4D0..0x47F503]:
`B[10]` pad plane Z (bottom), `B[11]` ceiling Z, `B[12]/B[13]` spine axis (X) min/max,
`B[14]/B[15]` the BEAM (Y) pair (r = (B[15]−B[14])>>2 — byte offsets 0x38/0x3C, the
same pair the boat solve reads as beam at 0x481CF4, and the spineY base below),
`B[16]/B[17]` pad X min/max, `B[18]/B[19]` pad Y min/max. (An earlier draft labeled
B[14]/B[15] "vertical span pair" — wrong, and it rode into the first port cut as a
Z-span pad radius; the 2026-08-01 review caught it. The arithmetic below is exact.)

### 3. Probe-point construction (7 points) [orig: 0x47F4D0..0x47F88F]

First the orientation matrix is built from `&Position` (translation + euler block):
scaled variant when `entity+0x158` or `itemDef->scale` nonzero, else plain
[orig: 0x47F28E..0x47F2E5]. Rows are extracted and normalized to 16.16 unit vectors
(×65536/len, zero-len → 0): row1 = forward (fwd, v187, v188), row0 = right
(vec, v180, v181), row2 = up (outVec, v183, **v184 = up.z**) [orig: 0x47F2E8..0x47F49A].
The normalized rows are written BACK into the matrix (SetRow0/1/2 ×2^6)
[orig: 0x47F4A7..0x47F4CB] — the matrix used everywhere below is orthonormalized.

With `r = (B[15]−B[14])>>2` (pad radius) and `sink_i` = the spring compression
+0x2D4+4i, body-frame points transformed by `Math_FixedPointTransformPoint22`
(M·p + translation, 10.22 rounding +0x200000):

```
pad0 = (B[17]−r, B[19]−r, B[10]+r+sink0)   radius r     [0x47F514..0x47F552]
pad1 = (B[17]−r, B[18]+r, B[10]+r+sink1)   radius r     [0x47F56A..0x47F5B2]
pad2 = (B[16]+r, B[18]+r, B[10]+r+sink2)   radius r     [0x47F5E3..0x47F619]
pad3 = (B[16]+r, B[19]−r, B[10]+r+sink3)   radius r     [0x47F63F..0x47F683]
// spine radius: rs = clamp(min(((B[11]−B[10])>>1)−0x4000, ((B[15]−B[14])>>1)−0x1000), 0x2000, +inf)
// spine Y = B[14] + (B[15]−B[14])>>1;  spine Z = B[11] − rs;  L = B[13]−B[12]
pt4 = (B[12] +   L>>2, spineY, spineZ)     radius rs    [0x47F6F5..0x47F73C]
pt5 = (B[12] + 3·L>>2, spineY, spineZ)     radius rs    [0x47F774..0x47F7A2]
pt6 = (B[12] +   L>>1, spineY, spineZ)     radius rs    [0x47F7FC..0x47F807]
```

Each world point is stored as 4 dwords (x,y,z,0); `output_matrix` local = −(pad0 body
z) = −(B[10]+r+sink0) is kept as the hull-bottom reference for the water test
[orig: 0x47F529..0x47F535]. Also `halfWidth = B[19]−r−(B[18]+r)` and
`halfHeight = (B[17]−r)−(B[16]+r)` (the pad rectangle dims) are saved for the
bounding-quad calls [orig: 0x47F81F..0x47F84E... 0x47F888].

### 4. Collision/terrain force passes + severity [orig: 0x47F855..0x480150]

`sev1 = Entity_ComputeCollisionForces(entity, points, radii, &cos_slip, &cos_max,
force_out, &hit_entity, 7, 0, 0, 0)` [orig: 0x47F88F]. Sub-contract (0x462150,
decompiled): per point, samples terrain (`Terrain_SampleHeightBilinear` @ 0x6067B0)
and the entity proximity list (bone collision volumes, `Entity_ComputeBoneCollisionForce`
@ 0x4AE150); output = 7 × [pushX, pushY, depth] (depth > 0 = penetration); planar push
scale by local slope cos22 vs the def thresholds: `>= cos(slipSlope)` no push,
in [(cos_slip+cos_max)/2, cos_slip) push>>3 & sev≥1, in [cos_max, mid) push>>2 & sev≥2,
`< cos(maxSlope)` full push & sev=3 [orig: 0x4624bb..0x462528]; entity hits classify
similarly; returns max severity 0..3, hit entity out.

Severity handling [orig: 0x47F905..0x47F99A]:
- **sev 1**: `currentSpeed −= currentSpeed >> (torque+2)`; clear brain+0x318 bit 0x10.
- **sev 2**: `currentSpeed −= currentSpeed >> (torque+1)`; skip everything else below.
- **sev 3**: `currentSpeed −= currentSpeed >> (torque+2)`, then the impact block:
  - 3D speed mag `m = |(velX, velY, slideDecay)|` [orig: 0x47F9A8..0x47F9F0].
  - AUTHORITY && !(Flags & 0x4000000): displacement mag `d = |Position − savedLivePose|`;
    if `LOBYTE(unitType) == 3`: **m > 29300 && d > 29300 → Health = 0** (aircraft
    hard-impact kill) [orig: 0x47FA5E..0x47FA94]. Else: damage =
    `|newSpeed| · mass · (mass>3 ? 2.5e-5 : 1.25e-5)` applied to Health (kill on
    exhaust) [orig: 0x47FA96..0x47FB07].
  - If brain+0x318 bit 0x10 clear && m > 2344: play scrape sound (def slot
    defaultResPlus64+0x68, else global soundDef), set the latch; **momentum
    exchange** with the hit entity if it is drivable (itemDef attrib byte&0x40),
    its mass < 2·ours, and its boundRadius < 2·ours:
    `t = ourMass·relVel/(mSum)` per axis; hitVel(+0x98/9C/A0) += 3t/4;
    hitPosition(+0x04..0x0C) += t/4 [orig: 0x47FB0B..0x47FC5D].
  - Deflection calc: farthest-push point (max |push| over 7); if its planar distance
    from center > 0x8000, heading delta between −push and the point bearing is
    computed (fpatan ×−(rad→BAM)) — stored to a local, **no consumer** (dead);
    if no qualifying hit entity: `currentSpeed = ftol(0.25·currentSpeed)`
    [orig: 0x47FC66..0x47FDF8].
- **sev 0**: clear brain latch; **skip the displacement entirely** (jump to §5 water).

Displacement (sev ≥ 1): sum planar pushes over 7 points → (Σx, Σy); all 7 point
records shifted by it; **second identical call**; if second sev != 0, average the
two passes' depths and sums (`(a+b)>>1` each) [orig: 0x47FDFE..0x48002B]. If the hit
entity qualifies (same drivable/mass/radius test): scale the displacement AND all
7 depths by `hisMass/(mSum)` (64-bit muls) [orig: 0x480034..0x48013C]. Then
`Position.X += Σx; Position.Y += Σy; Position.Z += Σz` (Σz = 0 unless the hit-entity
scale path produced one — terrain never does) [orig: 0x480143..0x480150].

The four PAD depths (points 0..3) land in locals `d0..d3`; spine depths `d4..d6`.

### 5. Water flag 0x8000 [orig: 0x480153..0x48033D]

```
zsum = padZ0+padZ1+padZ2+padZ3            // world Z of the 4 pad probe points
lowest = argmin(padZ_i); lowZ = min       // tracked during the scan
avg = zsum >> 2
if (Flags & 0x8000) avg -= r >> 1         // in-water hysteresis, r = pad radius
testZ = avg + (−(B[10]+r+sink0))          // hull-bottom reference ('output_matrix')
if (testZ >= Env_WaterHeightFixed) Flags &= ~0x8000;
else {
  if (!(Flags & 0x8000)) {                // water ENTRY only
    spawn splash emitter: 14-dword descriptor, flags=1, handle =
      (Flags&0x2000 ? dword_2C25C84 : dword_2C25BFC)   // airborne vs surface splash
      at (pad_x[lowest], pad_y[lowest], WaterZ), blend −32768 (dest[9])
      via CEffectWorld_SpawnEmitterAtPosition           // LOCAL, runs on clients
    Server_SendOverlayActionToAlive(Flags&0x2000 ? dword_24E09B0 : dword_24E09B4,
      &pos)                               // internally AUTHORITY-gated broadcast 0x34
  }
  Flags |= 0x8000;
}
```
[orig: 0x48021A..0x48033D; emitter select 0x48025C..0x48027F; entry-only test
0x480241..0x480243]. `Env_WaterHeightFixed` is the global water level (16.16).

### 6. Settle flag +0x2F2 [orig: 0x480340..0x48042B]

Matrix rebuilt from the (possibly displaced) euler block; up/right rows re-extracted.
`span_contact = (d0&&d3) || (d1&&d2) || (d0&&d2) || (d1&&d3)` (both length-ends
touching — pads 0/3 share one Y edge, 1/2 the other).
- If `up.z > 0x1000 && span_contact && !parked(+0x2EC)` → +0x2F2 = 1.
- Else if `(any pad depth) && right.z < 0x6000 && !parked && !wreck(+0x2F0) &&
  mass <= 10 && up.z > 0x1000` → +0x2F2 = 1, else +0x2F2 = 0.

### 7. Dead-in-practice damage loops (witnessed, port as no-ops)

Because the four sinks +0x2C4.. are zeroed at entry and only ever reach `step`(=187)
within a tick, every branch gated on `sink > 5000/7000` is unreachable in THIS
variant (their live twins exist in the ground-family solves):
- First-contact loop [orig: 0x48042B..0x4805AE]: for the first point with depth>0,
  gated `Flags&0x4000000 set → skip` then all four sinks > 5000: touchdown thud
  (slot +0x90 of defaultResPlus64) at |slideDecay| > 3000, authority damage
  `mass·|slideDecay|·(mass>3 ? 0.025 : 0.005)` at |slideDecay| > 15000, inverted
  (up.z<=0) → instant kill. **Dead** — the caller's thud at 0x492556 is the live
  aircraft touchdown sound.
- Inverted-crush [orig: 0x480759..0x4807C0]: spine contact && up.z<0 && authority
  && !invuln && sinks>7000 && slideDecay < −100000 → Health=0. **Dead**.
- The spring-loop "free-fall catch-up" branches keyed on `sink − step > 0` and the
  +0x2EE producers [orig: 0x480E62..0x480E95, 0x4811D2..0x48125F]. **Dead** (== 0).

### 8. Latched-state maintenance [orig: 0x4807C6..0x480BC7]

- Un-park on real contact: `parked(+0x2EC) && freeze(+0x2EF) && ((d0&&d2)||(d1&&d3)
  ||d6>0)` → +0x2FC=1 (reset marker), zero sinks, clear +0x2EF/+0x2EC, reset the
  park visual state (identity matrix, zero floats +0x534..+0x540, +0x4E8/4EC/4F0,
  +0x364, +0x3DC) [orig: 0x4807C6..0x48087C].
- Wreck-rest latch: `+0x2FC && currentSpeed < 32 && !+0x2F0` → **+0x2F0 = 1**
  [orig: 0x480882..0x4808A1].
- Parked upkeep: authority sets Flags|=0x10 while parked; parked && !wreck &&
  speed > 32 → zero sinks [orig: 0x4808A8..0x4808E7]. Authority clears 0x10 when
  neither latch [orig: 0x4808FE..0x480907].
- Wreck burning (Health > 0 && +0x2F0): smoke emitter (+0x1CC,
  `g_FxHandleSmkSigB`), sound slots init; AUTHORITY: Health −= 5/tick down to
  criticalDrain; below criticalDrain: release emitters; fire FX at
  Health <= 20·criticalDrain (`g_FxHandleVehicleFireMed`); authority re-asserts
  Flags 0x10; if 0x10 CLEAR (client not yet told): rebuild orientation from axes +
  bounding quad refresh [orig: 0x480918..0x480AF8].
- `!parked && up.z < 0 && !(Flags&0x10)`: rebuild orientation + quad (inverted
  free vehicle keeps its axes orthonormal) [orig: 0x480AFC..0x480BB8].
- Spring energy +0x300 clamped >= 0 [orig: 0x480BC7..0x480BC9].
- Max-depth scan #1 (`v185`): over all 7 depths, **only when parked || up.z<0 ||
  wreck**, else −1. Max-depth scan #2 (`maxAll`): always, all 7 [orig:
  0x480BD3..0x480CE5].
- Sink flush: `(wreck && up.z<0) || (Flags&0x2000 && Position.Z <
  Env_WaterHeightFixed)` → zero all four sinks [orig: 0x480CF2..0x480D25].

### 9. Bounding quad + pad extend loop [orig: 0x4805B4..0x480757]

`Entity_ComputeBoundingQuad(entity, dest, halfWidth, halfHeight, fwd3, right3,
Position3, 0)` — fills `dest` = 4 corner target positions (x,y,z per corner) of the
pad rectangle in world space from the orthonormal axes (sub-contract 0x45B6E0: pure
geometry, no terrain). `Entity_ClearSuspensionForces(entity, dest, 0)` applies any
pending wheel-contact-block forces via `Entity_ComputeChassisOrientation` then clears
the 4 blocks (entity dwords 218..237). The quad is then RE-computed (post-force
axes). [orig: 0x4805DF..0x4806D7]

Extend loop, per pad i [orig: 0x4806E2..0x480753]:
```
oscBlock_i.range(+0x308+0x18i) = 0xFFFF
skip = (!settled(+0x2F2) && all sinks==0 && up.z<0)   // inverted & clean: no extend
if (!skip && d_i == 0) sink_i += ftol(factor·250)     // 187 normal / 62 wreck
```

### 10. Branch select

`if (d0||d1||d2||d3)` → grounded branch (§12), else airborne branch (§11)
[orig: 0x480D2B..0x480D47].

### 11. AIRBORNE branch (no pad contact) [orig: 0x480D47..0x480F49]

```
maxDepth = −1
if (parked(+0x2EC) || up.z < 0) {
  maxDepth = max(−1, d0..d6)                       // spine contact counts!
  if (maxDepth > 0 && parked && !+0x2EF && !+0x2F0) {
    +0x2EF = 1                                     // park-freeze
    (rollRate != 0) ? (+0x364 = 1) : (+0x365 = 1)
  }
}
per pad: oscillator upkeep (force>0 → Suspension_CompressWheelQuadratic(block, 4095, i, entity);
         else amp!=0 → free decay 0x45D110)        // cosmetic spring settle
if ((parked && +0x2EF) || wreck(+0x2F0)) {
  Position.Z += maxDepth;  Flags &= ~0x2000;       // frozen/wreck: stay grounded
} else {
  Flags |= 0x2000;                                 // *** AIRBORNE SET HERE [0x480ED5] ***
}
Entity_ProcessWheeledVehicleSuspension(dest, matrix, &outPos, r, entity, 1, NULL)
euler = Math_FixedPointMatrixToEulerAngles(matrix)  // out[3]=yaw,[4]=pitch,[5]=roll
Roll = euler[5]; Pitch = euler[4];                  // ALWAYS overwritten
if (parked) Yaw = euler[3];
goto tail                                           // Z NOT touched from outPos here
```
For a flying aircraft the sub-solve round-trips the current attitude through the
corner quad (sinks contribute 0 net), so Pitch/Roll are effectively re-quantized,
then the mover adds pitchRate/rollRate after return.

### 12. GROUNDED branch (any pad depth) [orig: 0x480F4E..0x48154B]

- Contact-plane vector cache: if `d2 && d3 && byte +0x3CD` and the stored vector
  +0x3BC..C4 is zero-length: seed from the right axis, normalize ×65536
  [orig: 0x480F76..0x481075].
- Landed-while-parked relatch: `parked && !wreck && (Flags&0x2000)` → +0x2EF=1 (+
  park visual reset) [orig: 0x481077..0x4810E5].
- **`Flags &= ~0x2000` unconditionally** [orig: 0x4810EB] — grounded.
- Wreck shortcut: `wreck(+0x2F0) && up.z < 0`: if maxScan1 > 0 `Position.Z += it`;
  → tail [orig: 0x481100..0x48110D].
- minDepth = min(d0..d3, 100000-init), maxDepth4 = max(−1, d0..d3)
  [orig: 0x48111C..0x481183].
- **Spring loop** per wheel i (blocks: force F_i at +0x30C+0x18i, sink_i, depth d_i)
  [orig: 0x4811A2..0x481447]:
  - spring == 0 → skip (no suspension def'd).
  - (dead free-fall catch-up branch, §7.)
  - Impulse: `sink_i > (springComp<=10 ? 1000 : 5000) && d_i != 0` → **dead**
    (sink ≤ 187): would add `0.5·mass·(sink−thr)²` to F_i (overflow→0x1000000) and
    `1.25·F_i` to energy +0x300.
  - Settle (live, energy <= 0): `e = (d_i − minDepth) − amp_i(+0x304+0x18i…
    NOTE: the amp read is UNINDEXED `[esi+0x304]` = wheel 0's amp — witnessed
    as-is [orig: 0x4812EA])`; if e > 0:
    `F_i += ftol(0.5 · 2·spring · min(e, 4095)²)` [orig: 0x4812F4..0x481343].
  - If F_i > 0: `T = ftol(sqrt(2·F_i / (2·spring)))`; T<=0 → F_i=0;
    `Δ = Suspension_CompressWheelQuadratic(block_i, min(T,4095), i, entity)` (compression step: sink
    compression +0x2D4+4i += step, range/energy bookkeeping, returns Δcompression);
    `d_i −= Δ`. Else if amp_i != 0 (and sinks < 2000 — always true): free-decay
    oscillator (0x45D110: phase += 0.262, sinusoid × amp writes compression) and
    `d_i −= Δ` [orig: 0x481345..0x4813E5].
  - `entity+0x60 > 0` → d_i = maxDepth4 (override freeze) [orig: 0x4813F0..0x4813F6].
  - **`dest.corner_z[i] += d_i`** — corner targets lifted by resolved penetration
    [orig: 0x481425..0x481427].
- `Entity_ProcessWheeledVehicleSuspension(dest, matrix, &outPos, r, entity, 1, &d[0..3])`
  [orig: 0x481475] — the chassis conform (sub-contract §14).
- `euler = MatrixToEuler(matrix)`; **`Roll = euler[5]; Pitch = euler[4]`** (always)
  [orig: 0x48148A..0x4814AB]. Then Z/Yaw resolution:
```
if (parked(+0x2EC)) { Yaw = euler[3]; Position.Z += maxScan1; }        // [0x4814B4..0x4814C4]
else if (up.z > 0 && !wreck) {
    if (slideDecay > 0) slideDecay = 0;                                // [0x481834..0x48183C]
    Position.Z = outPos.z;                    // SOLVER CHASSIS Z      // [0x481842..0x481849]
} else Position.Z += maxScan1;                // inverted: lift by max depth [0x4814C0..0x4814C4]
```
- Pads with contact: sink_i = 0 [orig: 0x4814C7..0x4814F9].
- Stable-contact unlatch: `((d0&&d3)||(d1&&d2)) && up.z > 0 && !parked` →
  +0x2EE = 0, zero sinks, +0x2EF = 0, +0x2EC = 0 [orig: 0x4814FF..0x481544].

### 13. Park enter/exit (Flags 0x10, replicated to clients) [orig: 0x48154B..0x481789]

- 0x10 SET && !parked && !wreck → **park-enter**: extract axes, write contact
  blocks w0 & w3 = [up, k=10], full-extent bounding quad (B[13]−B[12], B[15]−B[14],
  isSquare=1), ClearSuspensionForces, `Entity_ComputeChassisOrientation(entity,
  dest, 0)`, `Entity_ApplyBoneAttachmentTransform`, **+0x2EC = 1**
  [orig: 0x48156F..0x48168D].
- 0x10 CLEAR → **park-exit**: `(parked && up.z < 0x2000) || wreck` → rebuild
  orientation from axes, quad (isSquare=1), clear +0x2EC and +0x2F0
  [orig: 0x48169E..0x481783].

### 14. Tail [orig: 0x48178A..0x481866]

```
thr = ftol(dword_815184 · 0.01)
if (all four osc amps (+0x304/+0x31C/+0x334/+0x34C) <= thr && energy(+0x300) != 0)
    energy = 0
byte +0x3CE = 1
if (maxAll > 0 && up.z < 0 && !(Flags&0x10)) Entity_RebuildOrientationMatrixFromAxes
if (Flags & 0x2000) ++counter(+0x3D4) else counter = 0
return severity
```

### 15. Sleep fast-path (entry) [orig: 0x47EF20..0x47F189]

Before anything: pick fall factor (0.75 / 0.25 by +0x2F0); release the smoke emitter
if Health <= criticalDrain. Then IF velocityX==velocityY==currentSpeed==0, all three
rates==0, !(Flags&0x2000), !(Flags&0x40), −350 < slideDecay < 0, +0x5C <= 0,
Position == savedLivePose (Transform_ComparePartial — its field scope is an open
witness, but it must exclude Z: the path's own undo proves Z already moved by
slideDecay before entry; ported as the planar XY compare), energy +0x300 == 0, no
occupant, and all four sinks == 0:
- `Position.Z −= slideDecay; slideDecay >>= 1` (undo the mover's gravity/servo dribble)
  [orig: 0x47F059..0x47F067];
- rebuild matrix, +0x2F2 = (up.z > 0x1000 && !parked);
- authority: Flags 0x10 ← (parked || wreck);
- if 0x10 set && up.z > 0: `Entity_BuildOrientationFromVectors` (+extract);
  if 0x10 clear && up.z < 0: `Entity_RebuildOrientationMatrixFromAxes` (+extract);
- **return 0** — the entire solve skipped. A remote engine-off aircraft settles into
  this state once the mover's sheds zero everything (its slideDecay dribble from the
  §4a/§5 servo keeps landing in (−350,0)).
Failing only the sink/pose sub-checks falls through after clearing Flags 0x40
[orig: 0x47F183..0x47F189].

Flags 0x40 upkeep at solve entry: if set and `current_tick − +0x3B8 > 200` → clear
both [orig: 0x47F18B..0x47F1AC]. (0x40 = "recently object-collided" latch; its
producer is outside this function.)

### 16. Client subset for a REMOTE aircraft (the D-NET-196 port contract)

Authority-only inside this function (skip on client): all Health writes (impact,
crush, burn drain), Flags 0x10 production, `Server_SendOverlayActionToAlive` (gated
inside the callee). EVERYTHING ELSE RUNS ON CLIENTS, notably:

1. **Flags 0x2000** (airborne): reproduced locally every tick — set iff no gear-pad
   penetration and not (parked-frozen || wreck-rest); cleared on any pad contact.
   Our current terrain clamp approximates this; exact parity needs the 4-pad probe
   (§3) + per-point terrain depth (§4).
2. **Flags 0x8000** (in-water): avg pad Z + hull-bottom offset vs Env_WaterHeightFixed
   with r/2 hysteresis (§5). Splash emitter spawn is client-local cosmetic; the
   overlay-action broadcast is authority-internal.
3. **Position.Z**: airborne → untouched; grounded upright non-parked →
   **Z = solver chassis Z** (+ slideDecay clamped ≤ 0); grounded inverted/wreck/parked
   → Z += max penetration. This replaces our clamp `Z = max(Z, ground)`.
4. **Pitch/Roll**: OVERWRITTEN both branches from the conform matrix (attitude on
   ground = terrain conform via the 4 corner targets; in air ≈ identity round-trip).
   Yaw only when parked. The mover then integrates the rates on top.
5. **Position.X/Y**: += averaged planar push when severity ≥ 1 (slope/wall/object
   separation) — this is what keeps a remote aircraft from clipping into hillsides
   between records.
6. **currentSpeed** sheds by `>>(torque+2)` / `>>(torque+1)` per severity, ×0.25 on
   severity-3 without a qualifying hit entity.
7. Momentum exchange writes INTO the hit entity (velocity 3/4, position 1/4 split) —
   runs on clients (local prediction of the shove).
8. Latches +0x2EC/+0x2EF/+0x2F0/+0x2F2/+0x2FC and the wheel spring state
   (+0x2C4/+0x2D4/osc blocks/energy): client-local; the parked flow additionally
   consumes replicated Flags 0x10.
9. The sleep fast-path (§15) — client aircraft at rest stop burning the solve.

Minimum-fidelity port for remote prediction (if the full 7-point pass is deferred):
pads = 4 corners of the def's gear rectangle at the entity yaw; depth_i =
terrain_height(pad) − pad_z (clamped ≥ 0); 0x2000 = all depths 0; grounded →
conform pitch/roll to the pad-plane fit, Z = chassis fit; 0x8000 per §5. Keep the
severity sheds off until Entity_ComputeCollisionForces is ported — they only matter
on slope/object contact.

### 17. Sub-contract index (port separately)

| callee | addr | contract |
|--------|------|----------|
| Entity_ComputeCollisionForces | 0x462150 | §4; terrain bilinear + proximity bone volumes; 7×[pushX,pushY,depth]; severity 0..3; hit entity |
| Entity_ProcessWheeledVehicleSuspension | 0x46B140 | 4-corner chassis conform: orientation from corner positions (cross products), writes matrix + outPos, contact blocks k=8000/800, honors/updates +0x2C4/+0x2EC/+0x2ED/+0x2EE latches, flip/underwater/death sub-paths; 1685-line decompile in `decomp_46b140.c` |
| Entity_ComputeBoundingQuad | 0x45B6E0 | pure geometry: 4 corner targets from axes + dims (isSquare variant uses model bounds center) |
| Entity_ClearSuspensionForces | 0x468980 | applies pending contact-block forces via ComputeChassisOrientation, then clears blocks (entity dwords 218..237) |
| Entity_ComputeChassisOrientation | 0x463940 | orientation from wheel data (Vehicle_ComputeOrientationFrom4Wheels @ 0x459310, quaternion path) |
| Suspension_CompressWheelQuadratic (ex `sub_45CFB0`, renamed 2026-08-21) | 0x45CFB0 | wheel-spring COMPRESSING step, quadratic (`dt²·2·spring`): phase `+0x14` := 1.57 `@0x45CFC8`; `travel = dword_815180 − dword_815184`; compression (`+0x2D4+4i`) > travel → energy 0, amp = travel, return 0 (bottomed `@0x45CFEE → @0x45D0A5`); else compression += dt, extension `+4` −= dt, energy `+8` += `dt²·2·k·(−0.5)`, impact sink `+0x300` −= `dt²·2·k·0.5` only while > 0 `@0x45D03A`, energy floor −1 `@0x45D077`, amp = min(compression, travel); returns the step. Linear twin `Suspension_CompressWheelLinear @0x45CEB0` (ex `Vehicle_ApplyBrakingForce` — nothing brakes). The kernel is `world/ground_conform.cpp` (ctest `ground_conform`); WIRED 2026-08-21 through `world/vehicle_suspension.cpp` (§7.3) |
| Suspension_OscillateWheelFast (ex `Entity_ApplyDamageOscillationFast`, renamed 2026-08-21) | 0x45D110 | free-decay wheel oscillator, FAST: phase += 0.2617 rad/tick (`flt_7C6A10`); env = clamp((sin+1)·0.5, ≤ 1); compression = env·amp `@0x45D167`; extension = travel − compression; itemDef shock `+0x904` clamped [0,10] IN PLACE `@0x45D18F..0x45D1A2` (the kernel now clamps the def's field itself, as retail does — the copy-clamp divergence closed 2026-08-21; there is NO caller-side shock clamp: the `cmp [+900h], 0Ah` sites `@0x47b9af/@0x47ea48/@0x481268` are a `spring_comp <= 10 ? 1000 : 5000` select); amp ×= 0.99 EVERY tick `@0x45D1D5`; only when compression == 0: amp ×= (11−shock)/11 `@0x45D1E5`, impact-sink drain `@0x45D1F0..0x45D219`, amp >>= 2 when `entity+0xA0 < −2000` `@0x45D21F..0x45D22D`. Slow twin `Suspension_OscillateWheel @0x45D240` (0.0872 rad/tick, ex `Entity_ApplyDamageOscillation`). WIRED 2026-08-21 as the release arm of `vehicle_suspension_step` (§7.3) |
| Entity_BuildOrientationFromVectors / Entity_RebuildOrientationMatrixFromAxes | 0x458DF0 / 0x4632E0 | orthonormal rebuilds (the latter's IDA comment is a known misname) |
| CWnd_HitTest | 0x6137D0 | MISNOMER: matrix row-1 extract (sibling of Row0 @ 0x613770 / Row2 @ 0x6137A0) — used as "extract forward axis" throughout |

### 18. UNVERIFIED / notes

- Bound-block axis labels (B[10..19], §2/§3) are inferred from usage; exact
  min/max-per-axis semantics need the model-format cross-check (3di collision
  block). The arithmetic is exact as written.
- `Entity_ComputeCollisionForces` internals: characterized from its decompile
  (slope classes, terrain sampler, per-point layout) but not instruction-verified;
  its own spec is the next port unit.
- `Entity_ProcessWheeledVehicleSuspension` internals beyond the contract row
  (decompile captured to `decomp_46b140.c`, not fully walked).
- The unindexed amp read `[esi+0x304]` in the settle branch (§12) — witnessed
  verbatim; looks like an original bug (always wheel 0's amplitude).
- The severity-3 heading-delta computation with no consumer (§4) — dead store,
  witnessed.
- dword_2C25C84/dword_2C25BFC (splash FX handles) and dword_24E09B4/dword_24E09B0
  (overlay-action ids): runtime-registered; producers not traced.
- entity +0x5C / +0x60 gate producers not traced (external impulse/override
  counters).
- +0x2ED semantics inside the sub-solve not walked; +0x2F2's consumer is outside
  this function.
- unitType==3 assumed to cover the air families (the kill rule reads as the
  aircraft crash rule); verify JO defs' unitType values against ITEMS.DEF before
  relying on the branch select.

---

## §7 The ground/tracked contact/suspension solve (ported 2026-08-05)

Source: `Entity_ProcessTrackedVehiclePhysics` @ 0x47C1C0 .. 0x47EF0E (0x2D4E
bytes; decompile + targeted disassembly of the mover call region). This is the
GROUND-family instance of the per-family contact solves — the function whose
ABSENCE was the live joiner symptom: parked ground vehicles presented SUNKEN by
exactly their per-model wheel clearance (Stryker −1.10 onto flat terrain
z=42.00, BTR-80 −0.99, buggies −0.25..−0.70) with their spawn-posed addeweap
gun children floating above, because the stand-in clamped the hull ORIGIN onto
the terrain while this solve rests the origin at `ground − box_z_lo`.

### 0. Identity and call contract

- **Sole xref**: `call @ 0x48d0b1` inside `Entity_UpdateVehiclePhysics`
  @ 0x48AF00 — AFTER `Position += velocity/slideDecay`
  [orig: 0x48d090..0x48d0a9], BEFORE the gated yaw apply
  [orig: `!(Flags & 0x2000) && byte+0x2F2 && !+0x2EC && !+0x2F0 → Yaw +=
  modelPtr0` @ 0x48d0b9..0x48d0e3]. Ungated on authority: a client runs it for
  every ground vehicle every tick.
- Signature `(entity, hasWaterLevel)`; `hasWaterLevel` is the MOVER's arg2, a
  dispatcher constant: **0** via the cveh/ctrn dispatchers [orig: `push 0`
  @ 0x48efce / @ 0x48f06e], **2** via the generic router `@ 0x48F010` — the
  catv (amphibian) path [orig: `push 2` @ 0x48f01b]. It gates ONLY the per-pad
  water-support forces (§7.4).
- Family routing (class table @ 0x82ABC0): cveh/ctrn/catv → the ground mover →
  THIS solve; **ctan** → its own mover `Entity_UpdateTankVehiclePhysics`
  @ 0x488AB0 → `Entity_ProcessWheeledVehiclePhysics` @ 0x475DE0 (call
  @ 0x48a9ef, water arg 0 via the ctank dispatcher @ 0x48f004); **cbik** →
  `Entity_UpdateLightVehiclePhysics` @ 0x483FE0 →
  `Entity_ProcessLightVehiclePhysics` @ 0x479600 [orig: call @ 0x486672,
  water arg 0 @ 0x48eff4]. Both variants are witnessed and ported (§8/§9);
  `VehicleFamily::Tank`/`Bike` route them (2026-08-06 — the tracked-solve
  interim is retired).
- Returns the collision severity; the mover ignores it (`add esp,8`
  @ 0x48d0b6).
- The mover's grounded velocity re-derive consumes the solve's stored contact
  direction (+0x3BC..+0x3C4, written @ 0x47E6E7..0x47E78F): `vel = speed × dir`
  with the Z component applied only when NEGATIVE (downhill)
  [orig: 0x48cf97..0x48d003]. NOT ported — our mover keeps the level-frame
  re-derive (D-NET-161).
- The mover's submerged drag rides the solve-owned Flags 0x8000:
  `v -= (v+2)>>2` on all three velocity components
  [orig: `test Flags,0x8000` @ 0x48d013; sheds 0x48d022..0x48d052] — PORTED;
  the authority drown-drain countdown (word +0x11E → overlay clear) is
  authority-gated [orig: 0x48d05a..0x48d083] — deferred.

### 1. Structure vs the air solve (§6)

The skeleton is the §6 solve with these deltas (everything not listed matches
§6 block-for-block — probe radii/shape, the two force passes, severity sheds
via the torque shifts, the strongest-probe distance-gated 0.25 cut, the planar
push, the in-water flag with the r/2 hysteresis, corner-quad conform, and the
0x46B140 positive-corner rise-clamped Z):

1. **Sleep fast-path** [orig: 0x47C244..0x47C44B]: same gate set as §6.15
   (velocities/speed/rates zero, not airborne, not carried, `slideDecay ∈
   (−350,−1]` as the unsigned compare `> 0xFFFFFEA2` @ 0x47C2D0, planar
   `Transform_ComparePartial`, four spring sinks +0x2C4..+0x2D0 zero, energy
   +0x300 zero, `occupantEntity` +0x170 null @ 0x47C302). Action: `Position.Z
   -= slideDecay; slideDecay >>= 1` [orig: 0x47C349/0x47C357], then the
   contact byte refresh `+0x2F2 = up.z(Q16) > 4096 && !+0x2EC`
   [orig: 0x47C35D..0x47C395]. Authority Flags 0x10 upkeep + the at-rest flip
   restore follow [orig: 0x47C3A3..0x47C443] — deferred (park/wreck machine).
   CONSEQUENCE, witnessed by the port bench: a ZERO-MOTION midair hull meets
   this gate and hovers — the classic retail floating-placement artifact.
2. **Model gate**: `graphicModel == NULL → return 0` before any Z logic
   [orig: 0x47C49F] — the boxless row does NOTHING in retail; our boxless/
   terrain-less rows keep the terrain-clamp stand-in (documented substitute,
   same policy as §6.16).
3. **Def clamps every call** [orig: 0x47C4B0..0x47C516]: spring 0..10,
   springComp 0..100, flip 0..55; `dword_815184` recomputed
   [orig: 0x47C52B..0x47C544]. All feed the deferred spring/latch machinery.
4. **Probe geometry** [orig: 0x47C7D0..0x47CB48]: identical pad/spine shape to
   §6.3 (`r = beam>>2`, pads at footprint corners inset r, `pad_z = box_z_lo +
   r`, spine radius `rs = min((height>>1)−0x4000, (beam>>1)−0x1000)` floored
   0x2000) with TWO deltas: each pad Z adds its per-wheel +0x2D4 spring offset
   (+0x2D4/+0x2D8/+0x2DC/+0x2E0 — written only by the wheeled-solve brake
   machinery `Suspension_CompressWheelLinear` @ 0x45CEB0 / @ 0x4790C7 decay legs, so
   ZERO for tracked rows and in our subset), and the spine slots land in the
   order 3L/4, L/2, L/4 [orig: 0x47CA6E → slot 4, 0x47CAFD → slot 5,
   0x47CA26 → slot 6] (order is behavior-neutral — spine probes share one
   radius and only feed severity + the max-penetration scan).
   `v211 = −(box_z_lo + r + spring0)` @ 0x47C832 is the hull-bottom reference.
5. **Force passes**: `Entity_CheckCollisionState(entity, probes)` twice
   [orig: 0x47CB8C, 0x47D213] — the §6.4 sub-contract (terrain leg =
   `Entity_ComputeCollisionForces`; entity leg deferred). Severity sheds:
   sev1/sev3 `speed -= speed >> (torque+2)` [orig: 0x47CC31/0x47CCA1], sev2
   `>> (torque+1)` [orig: 0x47CC71]; sev-3 authority damage (unitType-3 kill
   at 29300, mass-scaled drain) deferred [orig: 0x47CD00..0x47CDFB]; scrape
   sound + momentum exchange deferred [orig: 0x47CE17..0x47CF55]; the 0.25 cut
   with the strongest-probe > 0x8000 planar distance gate and the
   no-hit-entity condition [orig: scan 0x47CF5E..0x47D038, cut
   0x47D0DE..0x47D0EF; the deflection-heading pair 0x47D050..0x47D093 is the
   witnessed-dead yaw kick]. Pass 2 gated sev ≥ 1, averaged in when it still
   collides; push is X/Y only (the Z sum rides the deferred entity-mass
   scaling and is zero) [orig: 0x47D0F5..0x47D330; push 0x47D452..0x47D458].
6. **Water leg** [orig: 0x47D45B..0x47D629]: per-pad support forces
   `d_k = max(d_k, WaterZ − probeZ_k − v211)` — AMPHIBIAN DISPATCH ONLY
   (hasWaterLevel, §7.0) [orig: 0x47D489..0x47D512] — at level pose this
   floats the hull ORIGIN to the waterline. In-water flag: pad-average Z,
   `avg -= r>>1` hysteresis while set, `v211 + avg >= WaterZ` clears
   [orig: 0x47D516..0x47D53A; leaving-water emitter release 0x47D637..0x47D6BB
   and the splash FX/overlay send 0x47D542..0x47D629 = deferrals].
7. **Contact byte** +0x2F2 [orig: 0x47D7F4..0x47D8AF]: grounds on a SAME-SIDE
   or DIAGONAL pad pair — {0,3},{1,2},{0,2},{1,3}; the axle pairs {0,1}/{2,3}
   do NOT count — with `up.z(Q16) > 4096`, OR (light hulls, `mass <= 10`) any
   single pad while `fwd.z(Q16) < 24576`; both arms also require the
   crash/wreck/settle bytes clear (deferred latches). This is the byte the
   mover's yaw apply and velocity re-derive read.
8. **Spring free-fall / crush / wreck legs** [orig: 0x47DB59..0x47DD1F,
   0x47DBFF..0x47DC50, 0x47DC62..0x47DD04]: the +0x2C4 sinks grow 187/tick on
   no-contact wheels (`flt_7C3DC8 0.75 × flt_7C6F7C 250`; 0.25 when crashed),
   authority crush kills at sink > 5000/7000 — all deferred with the
   spring/oscillator machinery exactly as §6 defers them (state identically
   zero in the subset).
9. **Solve select** [orig: the all-zero pad test @ 0x47E1A9]:
   - NO pad contact, upright: the airborne arm — `Flags |= 0x2000`
     [orig: 0x47E57B], +0x2EF cleared, then the mode-1 NULL suspension call
     [orig: 0x47E5E6] whose fit of the UNLIFTED corners is an attitude
     identity round-trip (Z not consumed — the Z select is pad-path-only).
     The non-authority parked spring-apply leg
     [orig: 0x47E4D2..0x47E56C] rides replicated Flags 0x10 — deferred.
   - NO pad contact, INVERTED (`up.z ≤ 0`): max-penetration scan over all 7
     [orig: 0x47E1E9..0x47E23E] → `Position.Z += max` [orig: 0x47E358], the
     crash bytes latch + `Flags &= ~0x2000` [orig: 0x47E474..0x47E4C6] —
     ported as the physical subset (Z lift + un-airborne), crash latching
     deferred.
   - PAD contact: `Flags &= ~0x2000` unconditional [orig: 0x47E8EE]; the
     contact-direction store [orig: 0x47E65D..0x47E78F, deferred]; the
     non-authority 10-tick landing-grace timer on +0x2ED/+0x2F1
     [orig: 0x47E7A3..0x47E7EE, deferred — both bytes feed only the deferred
     park/wreck machine]; corner quad via `Entity_ComputeBoundingQuad`
     @ 0x45B6E0 (non-square arm) [orig: calls 0x47DAE2/0x47DB54] in the
     witnessed winding **c0=(+f,+s), c1=(+f,−s), c2=(−f,−s), c3=(−f,+s)** —
     UNLIKE the §6/boat corner tables, this winding is AXIS-ALIGNED with the
     pad probes, so the identity `corner_z[k] += d_k` pairing
     [orig: 0x47EC05, the spring loop's zero-state arm — `itemDef->spring ==
     0` short-circuits to it @ 0x47E973..0x47E998, and zero spring
     sinks/energy reduce the nonzero-spring arm to the same raw lift] is
     geometric here. Fit + Z via
     `Entity_ProcessWheeledVehicleSuspension(corners, mtx, &outPos, r, e, 1,
     &d[0..3])` [orig: 0x47EC4D] — the §4 MAIN_FIT with the positive-corner
     average, `fidiv` by the positive count (the §4 zero-count hazard), and
     the `+0x2000/tick` rise clamp [orig: 0x46C822..0x46C894]. Euler out:
     `Math_FixedPointMatrixToEulerAngles` [orig: 0x47EC62]; **Pitch/Roll
     always** [orig: 0x47EC83/0x47EC75], Yaw only when crashed
     [orig: 0x47EC8F, deferred].
10. **Z select** [orig: 0x47EC9E..0x47ECE2]: healthy upright (`!+0x2EC &&
    up.z > 0 && !+0x2F0`): `if (slideDecay > 0) slideDecay = 0; Position.Z =
    outPos.Z` [orig: 0x47ECAC..0x47ECBB] — **the rest-height mechanism**: at
    equilibrium every pad probe sits at `ground + r`, so the solved Z holds
    the origin at `ground − box_z_lo` (the wheel clearance). Crashed or
    inverted: `Position.Z += maxGroundHeight` (the max-penetration scan
    @ 0x47E09F..0x47E107) [orig: 0x47ECE2]; wreck-rest upright: the solved Z.
11. **Post-contact sink reset** [orig: 0x47ECE9..0x47ED60] (walked
    2026-08-21): every pad WITH contact zeroes its OWN sink [orig:
    0x47ECED..0x47ED17]; then a DIAGONAL pair `((d0 && d3) || (d1 && d2)) &&
    up.z > 0 && !crashed(+0x2EC)` → `+0x2EE = 0`, all four sinks = 0,
    `+0x2EF = 0`. The `up.z` term is `ebp = var_288` — the same up.z the
    pre-skip gate reads, NOT a penetration depth; the `+0x2EC = 0` store
    @ 0x47ED60 is redundant under the gate's own `!crashed` term.
12. **Crash recovery** [orig: 0x47ED67..0x47EE49; the client twin
    @ 0x47EEA0] (walked 2026-08-21): `!(Flags & 0x10) && up.z < 0` →
    `Entity_RebuildOrientationMatrixFromAxes` + `Entity_ComputeBoundingQuad`,
    then `+0x2EC = +0x2EF = +0x2F0 = 0` (crashed, the 2EF byte and the settle
    latch all clear). The righting itself is the named residual of §7.3.
13. **Tail** [orig: 0x47EE50..0x47EEF5]: spring-energy release, the airborne
    tick counter (entity[1] bookkeeping @ 0x47EEC7), the non-authority
    inverted flip-restore [orig: 0x47EEAC..0x47EEB7], wreck-rest slideDecay
    halving, `+0x2ED = 0` — all deferred except nothing our subset consumes.

### 2. The client subset (the port contract)

`ground_contact_solve` in `engine/runtime/world/vehicle_motor.cpp`, dispatched inside
`VehicleSystem::tick_motor` at the witnessed call position (after integration, before
the yaw apply) for the Ground and Bike families with resolved boxes on a
terrain-backed world; Watercraft (the authority stand-in path) and
boxless/terrain-less rows keep the 5-tap terrain clamp. Ported legs: the sleep
fast-path, the 7-probe geometry (+0x2D4 offsets zero), both force passes with
the severity sheds and the distance-gated 0.25 cut, the planar push, the
amphibian pad water-support forces (`VehicleTraits.amphibian` ⇐ move_function
catv), the in-water flag with hysteresis, the contact byte (→ `m.grounded`),
the corner quad in the witnessed winding with identity d_k lifts, the shared
4-normal fit (`plat_fit_corners`), the positive-corner rise-clamped rest Z,
the inverted max-penetration lift, and the airborne/in-water flag ownership.
The mover gained the submerged drag leg and reads the live solve attitude for
its cos²(pitch) slope factor [orig: entity Pitch @ 0x48ba47]; entity
pitch/roll int16-degree mirrors publish the conform to presentation and
mounted-pose consumers. Named deferrals (beyond the §6-shared seams): the
contact-direction slope-velocity feed, the +0x2ED/+0x2F1 landing-grace and
park/wreck/crash latch machine, spring sinks/oscillators (raw d_k lifts —
exact at rest, softened transients differ), authority damage/drown legs, FX
and sounds, and entity-entity collision. Bench: the
`run_ground_parked_rests_at_wheel_clearance` legs (exact rest at
`ground − box_z_lo` from the parked wire pose AND from a 1.5 u drop; red
against the clamp stand-in, which parks the origin at terrain — the live
symptom).

---

### 7.3 The spring leg (witnessed in full + WIRED 2026-08-21)

`engine/runtime/world/ground_conform.h/.cpp` is the compress/oscillate
KERNEL (the pair above, the travel derivation `@0x47C51F..0x47C544`, the
`>> 4` `@0x47CBE5`; its radius helpers were deleted as duplicates of the
solves' pad/spine radii); `engine/runtime/world/vehicle_suspension.{h,cpp}` is
the leg that calls it and owns the state bytes on `Entity::VehicleMotorState`
(`wheel_comp[4]` = +0x2D4..+0x2E0, `wheel_osc[4]` = +0x304 + 0x18·i,
`spring_energy` = +0x300, the latch bytes +0x2EC/+0x2ED/+0x2EE/+0x2EF). The
wire-up round's witness pass corrected two readings of the tidy:

- **`+0x2EC` is the CRASHED / TIPPED state, not "parked", and the latch gate
  is `+0x2ED != 0 && +0x2EC == 0`** — `+0x2ED` NONZERO. All three seeds read
  it that way: `Entity_ProcessWheeledVehicleSuspension @0x46B140` (`cmp
  [+2EDh],0 ; jz skip` `@0x46b1a6..0x46b1b3`, then `cmp [+2ECh],0 ; jnz skip`
  `@0x46b1b9..0x46b1bf`; callers tracked `@0x47e5e6/@0x47ec4d`, aircraft
  `@0x480f01/@0x481475`, platform `@0x483bbe`), `Entity_ComputeSuspensionAndOrientation
  @0x4698A0` (tank, identical `@0x469933..0x469947`) and
  `Entity_UpdateVehicleChassisOrientation @0x468A50` (bike, `@0x468b00..
  0x468b0e`; its arm also `Entity_EjectAllOccupants @0x468b3b`). The tank /
  tracked crash sites play the crash sound (`def+0x864 → +0x64` slot else
  `dword_24E0908`) before their own `+0x2EC = 1` stores (`@0x478998`,
  `@0x47e47b`).
- **`+0x2ED` is a per-tick crash REQUEST**, 0 at spawn (`Entity_RespawnVehicle
  @0x45FF40` zeroes `+0x2F0/+0x2EC/+0x2EE/+0x2F2/+0x44C/+0x2FC/+0x2F8/+0x2ED`
  and sets `+0x2F1 = 1` `@0x45ffeb..0x46001e`; the BMS spawn writes none —
  memset only), raised by the family physics during the tick and cleared
  UNCONDITIONALLY at every tick tail (tank `@0x4795da`, bike `@0x47c0b6`,
  tracked `@0x47eeee`) — so a fresh row can never latch. The 17 producers (all
  require `+0x2EC == 0`): tank `@0x477776` (crash test (a): `|up.z| <
  flip · flt_7C56A8 (0.01) · flt_7C32BC (65536.0, NOT 65535.0)` — the
  `cdq; xor; sub` ABSOLUTE value `@0x477748..0x477776`, so an inverted hull
  tip-tests too — `&& Flags & 0x2000` airborne), `@0x4777bf` (authority
  `|velocity.z| > 0x7000`, client `airborne && Flags & 0x10`), `@0x478bc0`
  (CLIENT-ONLY, the 10-tick airborne window over `+0x2F1/+0x2F0/+0x2F8`, with
  a `+0x2F0 == 0` settle term `@0x478b8a` that is TANK-ONLY); tracked
  `@0x47d763` (the same `|up.z|` test `@0x47d722..0x47d763` `|| Flags &
  0x10` — the `|| bit` alternative is TRACKED-ONLY, absent from the tank
  twin — `&& airborne`), `@0x47d7a8` (the same authority/client split),
  `@0x47e7d8` (the client window twin `@0x47e793..0x47e7a8`, WITHOUT the
  settle term); bike
  `@0x47b14c` (authority, driven `var_29C > 0x6702`, upside-down `up·z <
  −0.87`, then `front || rear` contact), `@0x47b375` (both wheels off `&&
  +0x3DE && an extra probe hit`), `@0x47b6a7` / `@0x47b6d1` / `@0x47b6fb`
  (`+0x3DE` / `|vz|` / `var_280` pitch tests) — `+0x3DE` = "has been driven",
  set by the bike mover `@0x48524c` when `Flags & 0x20 && speed > 0x1000`.
- **The pick and what it scales**: with the seed armed, `flt_7C6F18 = 1.25`
  (authority, `@0x46b1cd`) / `flt_7C6F14 = 1.75` (client, `@0x46b1d5`) by
  `g_napi_np_ctx.is_authority`; `+0x2EF = 0`; the authority raises `Flags |=
  0x10` `@0x46b1ed` while a client only TESTS the bit `@0x46b1f3` (the latch
  replicates through it — the vehicle rows' flags byte); `+0x2EC = 1`
  `@0x46b1f9`, `+0x2EE = 0`, `Entity_ClearSuspensionState @0x4592B0`. Then,
  for each wheel k with no contact, the force slot `+0x368 + 0x14·k = {0, 0,
  −1.0, magnitude = ftol(sink_k × pick)}` `@0x46b24b..0x46b269` (airborne /
  no flags: the sinks unscaled `@0x46b290..0x46b30b`) → `Entity_ClearSuspensionForces
  @0x46b314` → `Entity_ComputeChassisOrientation(…, 0)` =
  `Vehicle_ComputeAveragedOrientation` — a downward IMPULSE that TILTS the
  chassis (the delta folded into the chassis matrix + the quaternion at
  +0x534..+0x540, `+0x4E8 = 1` `@0x463a3b..0x463a64`); never a Z offset.
- **The spring dt** `@0x47c1de..0x47c222`: `+0x2EC == 0 ? 0.75 (flt_7C3DC8)
  : 3.0 (flt_7C6F80)` — 3.0 is the CRASHED step, 0.75 the normal one;
  aircraft `+0x2F0 ? flt_7C333C : 0.75` `@0x47ef20`; the tank and the bike
  have no select.
- **Sink growth (the non-crash path)**: tank `@0x478510..0x47852b` per probe
  with no contact `&& +0x2ED == 0 && +0x2EC == 0` → `sink_k += 250`; tracked
  `@0x47db70..0x47dbd1` the same gate, `+= ftol(dt × 250.0)` = 187
  (pre-skipped when `+0x2F2 == 0 && all sinks == 0 && var_288 < 0`); bike
  `@0x47ab36..0x47abdc` with NO latch terms — front/rear `+= 100` whenever
  that wheel has no contact. The tracked RESET (contact → own sink = 0, the
  diagonal-pair clear of `+0x2EE`/sinks/`+0x2EF`) is §7 step 11.
- **`Entity_ClearSuspensionState @0x4592B0`** is routine, not a latch effect:
  identity into the chassis matrix `+0x4F4`, the interp quaternion
  `+0x534..+0x540 = 0`, `+0x4EC/+0x4F0 = 0`, `+0x4E8 = 0`, `+0x3DC = 0` if
  set; it never touches the sinks, compressions, oscillators or force slots;
  the tank solve calls it EVERY tick when `+0x3CE == 0 && !(Flags & 0x40)`
  `@0x47606e..0x47607d`.
- **Def keys** (`ItemDef_ParsePhysicsProperty @0x49D870`, raw `atol`):
  `spring` +0x8FC `@0x49db86`, `top_heavy` +0x918 `@0x49dbc2`, `spring_comp`
  +0x900 `@0x49dbfe`, `shock` +0x904 `@0x49dc3a`; defaults spring 0,
  spring_comp 20 `@0x49e496`; per-family clamps spring [0, 10], spring_comp
  [0, 100], flip (+0x948) [0, 55]; `travel = (100 − spring_comp) · 0xFFFF`
  `@0x476190 / @0x47c51f`; `spring != 0` gates the spring-energy loop
  (`@0x478d73 / @0x47b933 / @0x47e973 / @0x4811a5`); `top_heavy` has NO
  runtime consumer (dead — parsed for parity, not fed).

**Port**: `vehicle_suspension_latch` (the crash-request shape: arms on
`request && !crashed`, the authority sets / the client reads Flags 0x10, the
sinks × pick dumped into the −Z tilt impulses), `vehicle_suspension_dt`,
`vehicle_suspension_step` (the compress arm over the solves' pad depths, the
release arm = the oscillator, the per-family sink growth), `vehicle_suspension_clear`;
the four `pad_z` sites of `vehicle_contact_solve.cpp` add `wheel_comp[k]` and
the corner-lift feedback consumes the stepped compression; the role is
`World::rules.logic_authority` stamped in `run_logic_tick`; the def keys reach
`VehicleTraits` (and both Python FFI mirrors + the native-stride pins, since
`DefItemDef` crosses the C ABI). Pinned by ctest `ground_conform`,
`vehicle_suspension` (a driven buggy settles ×0.99/tick with the
`(11 − shock)/11` damp at compression 0; the authority 1.25 vs client 1.75 pick
taken once per latch; the Flags-0x10 client path; the clear), and the
`watercraft_client_motor` bike-drop bench (exact again — the tidy-era literal
reading of the gate had parked every vehicle at spawn). **Named residuals**
(no D-row): the crash-request producers whose inputs are unported — the
bike's `+0x3DE` has-been-driven byte and the client 10-tick airborne window
(`+0x2F1/+0x2F0/+0x2F8`) — the contact-direction slope feed (D-NET-161), and
the crash-recovery righting (§7 step 12: the inverted-hull rebuild and its
three-byte clear, walked 2026-08-21, unported).

### 7.4 The part-animation registers — rotors and the wheel phase (witnessed + ported 2026-08-21)

The accumulators the model's PANM tracks read (`HELO_ROTOR` 46,
`HELO_TAILROTOR` 47, `VEHICLE_WHEELS` 60), published as the HIGH WORD of a
dword accumulator `[orig: Entity_CacheVehicleHUDStats @0x4929B0]` — the table
base `0x83FCE8 + 8n`: 60 = HIWORD(+0x2B8) `@0x4929b4`; 61 STEERING =
min(HIWORD(+0x2B4), 0x10000) `@0x4929c0`; 62 SPEED = min(|+0x29C|, 0x10000);
73..78 TIRE00..05 = clamp01 of the per-wheel compressions `@0x4929f6..
0x492ac5`; **46 and 47 are BOTH HIWORD(+0x464)** `@0x492aca/@0x492ad7` — one
accumulator, no separate tail-rotor state.

- **The rotor machine is split by `.aip` profile type** (`brain+4 →
  profile+16`): GROUND (2) = `Entity_UpdatePartSpinAccumulator @0x4928B0`
  (ex `Entity_UpdateGravityAccumulator`; the gate `@0x4928c9`), called
  unconditionally at the tail of all six movers after the splash sound
  (`@0x46f99e` inf, `@0x4700f5` air, `@0x4869ea` light, `@0x4889f5` mounted,
  `@0x48ae3d` tank, `@0x48d42b` ground vehicle = `Entity_UpdateVehiclePhysics
  @0x48AF00`). The watercraft mover `Entity_UpdateWatercraftPhysics @0x48D480`
  calls NEITHER rotor machine — its only part register is the wheel phase
  `+0x2B8 += cmd << 13` `@0x48E9F0`, so a boat never draws the rotor PRNG
  word. HELO (1) =
  `Entity_UpdateHeloRotorSpin (ex entity_update_damage_accumulator_and_shadow) Entity_UpdateHeloRotorSpin @0x48FA70` (a misnomer;
  proposed `Entity_UpdateHeloRotorSpin`), called from `Entity_UpdateAircraftPhysics
  @0x4905a6`. Rate seeding (both): `attrib & 0x40` (PlayerControl) with an
  occupant → 186413; a non-0x40 item → `PRNG_Next16() % 100` (> 66 → 139809,
  > 33 → 163110, else 186413 `@0x49290e..0x492935` / `@0x48fb1c..0x48fb2e`)
  — re-rolled EVERY unoccupied tick because the empty branch resets the
  rate (`@0x492972` / `@0x48fb7c`): one shared-stream draw per idle
  non-player-control vehicle per tick. Occupied: `speed(+0x460) += rate
  (+0x468)`, cap 214748352, `angle(+0x464) += speed`; empty: `speed −=
  186413` (ground) / `46603` (helo `@0x48fb67..0x48fb76`) floored at 0,
  `angle += speed` always `@0x48fba9`. The HELO twin additionally plays the
  engine-start sound (`def->defaultResPlus64+120` when `speed <= 0.05·cap`
  above water `@0x48faea..0x48fafb`), spawns `Entity_SpawnBoneTrailEffect`,
  lays the downwash terrain overlay (`terrain_overlay_alloc(handle, 786432,
  983040, ratio)` `@0x48fc7d`) and returns `!authority || speed >= cap`
  `@0x48fe4b` — presentation residuals.
- **The wheel phase** `[orig: Entity_UpdateVehiclePhysics @0x48c4c5..0x48c4d0
  and the twin @0x48c4e4..0x48c4f4]`: `+0x2B8 += |+0x46C| + (+0x29C << 13)`
  per tick. The slip term +0x46C is gated on an occupant and `+0x3CD == 0`
  `@0x48c330..0x48c343`; branch A (`ftol(sqrt(+0x3BC² + +0x3C0² + +0x3C4²)) !=
  0`) locks it to −6064.0 (`dword_81518C` = 0xE8480000) `@0x48c4a5..0x48c4b4`;
  branch B decays it `ftol(min(1, 1 − speed·1.6276e-5) · +0x46C)` +
  `Entity_SpawnBoneEffectsAtMask` `@0x48c506..0x48c535` or zeroes it. The
  +0x3BC contact-direction store is the D-NET-161 leg → slip = 0 in the port
  (the phase reduces to `speed << 13`). +0x460/+0x464/+0x468 is a union with
  the turret/heading smoothers (`Vehicle_UpdateTurretRotation @0x45b016`,
  `Entity_SmoothHeadingToTarget @0x45b4fc` write +0x464 for emplacements).
- **The BMS seed** `[orig: Entity_SpawnFromBMSRecord @0x40E9F0, edi = the
  record]`: `record+0xC (attrib_flags) & 0x20000` (`bms.h` Attribute17 —
  "engine running at spawn") → `Flags |= 0x80; +0x468 = 186413; +0x460 =
  214748352; +0x29C = 0x10000` `@0x40ee70..0x40ee94`.

**Port**: `engine/runtime/world/vehicle_part_anim.{h,cpp}` (`RotorState` =
`VehicleMotorState::PartSpin` +0x460/+0x464/+0x468, `wheel_phase` +0x2B8; the
two arms selected by the profile type, `rotor_rate_needs_roll` /
`rotor_rate_from_roll` on `World::next_prng16`, `rotor_tick`), called at the ground mover tails and the aircraft mover head before the
rotor-up gate (section 11); boats advance only their wheel phase;
`vehicle_ctrl_registers` publishes rotor/tail_rotor/wheels beside
steering/speed; `present_rows.h` `PF_VEHICLE_ROTOR/_TAIL_ROTOR/_WHEELS` →
`entity_presenter` (`CtrlNames` += HELO_ROTOR / HELO_TAILROTOR /
VEHICLE_WHEELS, the ctrl memoization table includes all published fields) →
`ObjectModel::set_ctrl_override`; the seed in `entity_spawn.cpp`. Pinned by
ctest `vehicle_part_anim` (the pure pins; a player-control buggy spins only
while mounted and decays after dismount; exactly one PRNG draw per unoccupied
tick on a non-0x40 vehicle; the cap; the HIWORD projection) and GUT
`vehicle_emplacement_alignment_test` (the driven DBuggy's VEHICLE_WHEELS
override advances end to end; the crewed Blackhawk rotor).

Observed, not changed: `vehicle_motor.cpp:455-476` cites the gear-mode switch
`@0x48bb46..0x48bbe0` for the speed ladder it ports, while that ladder (the
reversal-unclamped / drive-clamp `def+0x8E0` / brake-clamp `def+0x8E4` cases)
sits at `@0x48C3D2..0x48C46C` and also carries the carrier-flag-`0x100`
exception `@0x48C428..0x48C439` the port lacks.


## §8 The wheeled (ctan) contact/suspension solve (ported 2026-08-06)

Source: `Entity_ProcessWheeledVehiclePhysics` @ 0x475DE0 .. 0x4795FA (0x381A
bytes; decompile + targeted disassembly). The ctan/tank family's instance of
the per-family contact solves — the live caller is the tank mover (§10), call
@ 0x48a9ef, hasWaterLevel pinned 0 by the ctank dispatcher (`push 0`
@ 0x48f004: tanks never float). A second reference from
`Entity_UpdateMountedInfantryMovement` @ 0x486a50 (call @ 0x4885d0) has no
live dispatcher — table-resolved dead/AI-side code.

The skeleton is the tracked solve (§7) with these deltas (everything not
listed matches §7 block-for-block — def clamps, severity sheds via the torque
shifts, pass-2 averaging with the X/Y-only push, the in-water flag with the
r/2 hysteresis against `-(box_z_lo + r)`, and the §4 positive-corner solver
Z):

1. **THIRTEEN probes, every radius r = beam >> 2** [orig: geometry
   @ 0x4762B8..0x476559; the 13 radius stores all copy suspensionOffset]:
   the 4 wheel pads of §7 (footprint corners inset r, Z = box_z_lo + r + the
   per-wheel +0x2D4 sink), SIX belly stations
   along the two side rails at X = foot_x_lo + r + {3q, 3q, q, q, 2q, 2q}
   with q = ftol(0.75 × box_x_span) >> 2 (flt_7C3DC8 = 0.75) and the
   front/rear/averaged per-side sinks in Z, and THREE spine probes at
   X = box_x_lo + {3L/4, L/2, L/4}, Y = box ymid, Z = box_z_hi − w/+w/−w with
   w = (beam >> 3) − 0x4000 (the MIDDLE spine probe sits ABOVE the deck).
   There is no separate spine radius.
2. **The 7-slot contact model** [orig: the slot merge @ 0x4784CC..0x4784FF]:
   slot k = max(d_wheel_k, d_belly_k) for the four wheel/rail pairs, slots
   4..6 = the spine d's. The slots decide sink growth, grounded catch-up and
   the tail's sink clear (D-VEH-4, [tank record](tank-parity-re.md)). The
   sev-3 0.25-cut scans the strongest of the first SEVEN probe forces only
   [orig: init from probe 0 @ 0x476BC3]. The two depth views: the probe
   records hold the last pass and feed the slots, the maxes, the crash and
   wheel depths, the springs and the lifts; the averaged, mass-shared copy
   feeds only landing, crush and the wreck latch [orig: pass one @ 0x476A8E,
   copy @ 0x476A93..0x476B3A, pass two @ 0x477037, mass share @ 0x47720D].
3. **Wall bytes and the stability byte** [orig: `Entity_CheckCollisionState
   @ 0x462A30`; the walk @ 0x477940..0x477AF8; the byte
   @ 0x477CF9..0x477DA5]: only a probe that struck a steep model face carries
   a wall byte, OR-ed across passes; terrain never does. A flagged probe is a
   REVERSE hit when its normalized force opposes the contact direction past
   −0.75 (−49152, @ 0x477AD3). The stability byte +0x2F2 (`grounded`)
   requires `up.z(Q16) > 0x2000` (NOT §7's 4096), the LEADING axle for the
   current gear (front pads forward, rear pads in reverse) to be SYMMETRIC —
   both flagged or neither — with no reverse hit on it, and none of +0x2EC,
   +0x2F0, +0x2FC (@ 0x477D2F..0x477DA5). The direction is the +0x3BC
   contact-direction store, falling back to the basis forward row when empty
   [orig: @ 0x477A48..]. (Corrected 2026-09-22: the earlier text flagged any
   probe with a planar force and omitted the three latch gates.)
4. **The wall stop** [orig: any wall byte zeroes velocityX/Y and
   currentSpeed @ 0x477B7B..0x477B87; the head-on test @ 0x477B8D..0x477C09]:
   the summed flagged force, normalized, against the same direction — past
   −0.871 (−57070) the stop repeats. The authority head-on damage riding it
   is dead code in retail: its operand is `xor eax, eax` @ 0x477C90, the
   speed just zeroed.
5. **The Z select absorbs into slideDecay** [orig: `slideDecay += solvedZ −
   Position.Z; if (slideDecay > 0) slideDecay = 0; Position.Z = solvedZ`
   @ 0x47904A..0x4790A1 / @ 0x4791B8..0x479205]: no §7 +0x2000 rise clamp —
   the landing step cancels the fall velocity instead. The solver is
   `Entity_ComputeSuspensionAndOrientation` @ 0x4698A0 (the third §4-family
   instance, the same MAIN_FIT core as `plat_fit_corners`).
   Inverted: select the retained maximum over ALL 13 d's [orig: seed
   @ 0x478391, gate (+2EC || up.z < 0 || +2F0) @ 0x4783BA..0x4783D1, the
   13-record max (stride 0Ch, bound 9Ch) @ 0x4783D5..0x4783EC, consumer
   `Position.Z +=` @ 0x479311..0x479318; the four-WHEEL max the failed-fit
   arms add instead is seeded @ 0x478B69/@ 0x478B72, scanned
   @ 0x478D0E..0x478D4D and consumed @ 0x479075..0x479079 /
   @ 0x4791D9..0x4791DD]. The airborne branch lifts a crashed or inverted
   hull by the max over the SEVEN contact slots [orig: @ 0x478720..0x478735,
   `Position.Z +=` @ 0x478887].
6. **The corner quad lifts by the four WHEEL d's only** (belly/spine d's feed
   severity and the Z maxes). `Suspension_CompressWheelLinear @ 0x45CEB0` and
   `Suspension_OscillateWheel @ 0x45D240` are the live spring machinery (section
   14): sinks grow +250/tick on unsupported corners and each corner's drop
   adjustment feeds the fit [orig: the spring loop @ 0x4787EC..0x478879,
   adjustment @ 0x478834..0x47884D]. (Corrected 2026-09-22: the springs run
   live, not zero-state.)
7. **Ported since, with the §7-shared seams** (sections 12 through 14, 19
   through 20, 23 and 27, and the [tank record](tank-parity-re.md)'s
   2026-09-22 witness map): the sev-3 entity momentum exchange and the
   pass-two mass share, the crash, flip and wreck latches (`+0x2EF` writers
   @ 0x4785DF, @ 0x4787D3, @ 0x478991, @ 0x478C80; clear @ 0x478ABC) with the
   settle machine and the solve-head forcing, the grounded crashed block
   @ 0x478BDD..0x478CCC with `Entity_ApplyWheelSuspensionForces @ 0x463560`
   and its tumble cues, the client crash window at the grounded entry
   (@ 0x478B6C..0x478BD6), the common-tail righting of a penetrating inverted
   hull without Flags 0x10 (@ 0x47948A..0x4794A8, both branches), the
   contact-direction downhill tail [orig: @ 0x4794B0..0x4795B5], landing and
   crush damage, and the landing-pass force consume
   (`Entity_ClearSuspensionForces` call @ 0x477FB6).
8. **A crashed authority tank never self-rights** (the 07TR tank 34
   integration run, 2026-09-22; no contact divergence). The arming seed sets
   Flags 0x10 on the authority [orig: @ 0x469976] and only
   `Entity_RespawnVehicle` rewrites it, Flags = (Flags & 0x400) | 0x2000
   [orig: @ 0x45FFB2..0x460009]; nothing in the tank family clears it, and
   every righting path tests it: the grounded-tail recovery
   @ 0x479352..0x47943E, the common-tail rebuild @ 0x47948A..0x4794A8, the
   settle block's rebuild (`test Flags, 10h` @ 0x4781D4) and the rest path
   @ 0x475FA2..0x475FC6 (the client twin @ 0x4782CA is non-authority only).
   Such a tank settles through the mover's crashed arm (@ 0x48A684..0x48A81F;
   +0x2F0 at |speed| < 0x1000, @ 0x48A7E0..0x48A816), burns
   (@ 0x478125..0x478288; the 5/tick drain skips Flags 0x4000000), latches
   the wreck (@ 0x47853C..0x478643) and waits for respawn.
   `Entity_RebuildOrientationMatrixFromAxes @ 0x4632E0` keeps the forward
   axis, so it keeps pitch and clears only roll: it cannot right a
   nose-stand. The tank mover applies the groundEntity (+0x28) delta
   unconditionally [orig: @ 0x488CFC..0x489106] and refreshes the link every
   8 ticks [orig: @ 0x488B69..0x488B81]. The solve head's only [0,55] clamp
   is the tip threshold def+0x948 [orig: @ 0x476166..0x476187]; the tank fit
   has no pitch clamp.

Port: `wheeled_contact_solve` (engine/runtime/world/vehicle_contact_solve.cpp),
routed by `VehicleFamily::Tank`. Bench: the tank rest/drop legs (exact rest at
`ground − box_z_lo` through the 13-probe geometry and the slideDecay-absorb
select).

## §9 The light (cbik) contact/suspension solve (ported 2026-08-06)

Source: `Entity_ProcessLightVehiclePhysics` @ 0x479600 .. 0x47C1B7 (0x2BB7
bytes) + `Entity_UpdateVehicleChassisOrientation` @ 0x468A50 (0x83C — the
2-corner chassis fit) + `Entity_SmoothHeadingToTarget` @ 0x45B2C0 (0x317 —
the grounded heading/lean smoother, called @ 0x47a7d3). Sole caller the cbik
mover @ 0x483FE0 (call @ 0x486672, frameFlags 1, water arg 0 @ 0x48eff4).

Deltas from the §7/§8 skeleton:

1. **r is HEIGHT-derived**: r = ((box_z_hi − box_z_lo) >> 1) − 0x4000 — not
   beam >> 2. SIX probes on the hull centerline, every radius r [orig:
   geometry @ 0x479960..0x479C20; the plain fild/ftol radius round-trip
   @ 0x4799b7]: two WHEELS on the FOOT centerline (X = foot_x_hi − r /
   foot_x_lo + r, Y = (foot_y_lo + foot_y_hi) >> 1, Z = box_z_lo + r + sink),
   three spine probes on the BOX centerline at X = box_x_lo + {L/4, 3L/4,
   L/2}, Z = box_z_lo + 2r, and one mid-hull probe at L/2, Z = box_z_lo + 3r
   [orig: `box_z_lo − ftol(4r × −0.75)` via flt_7C6F78 = −0.75 @ 0x479BC1].
2. **Sleep window (−300, −1]** (the `> 0xFFFFFED4` unsigned compare — §7/§8
   use 350), no contact-byte refresh in the sleep arm, and the whole
   suspension tail is gated on the mover's frameFlags.
3. **The 0.25-cut scans probes 0..4** (the mid-hull probe excluded); pass 2's
   planar push sums probes 0..4 only.
4. **The in-water flag is the plain two-wheel average** against the waterline
   — NO hysteresis, NO hull-bottom offset [orig: `(z0 + z1) >> 1 >= WaterZ`
   @ 0x479F7C..0x479F90].
5. **The any-contact speed halver** [orig: @ 0x47A66E..0x47A683]: planar
   contact at |velocity| > 17580 sheds half the drive speed. There is NO tank
   wall stop — the bike's head-on detector (summed contacted force against
   the normalized VELOCITY past −0.871) feeds the authority damage/eject
   ladder instead (29300 kill / 21975 eject / 19045 −20, the belly-strike
   eject at speed > 26370) [orig: @ 0x47A343..] — deferred authority legs.
6. **The 2-corner AXLE fit** [orig: Entity_UpdateVehicleChassisOrientation
   @ 0x468A50, the grounded arm @ 0x468B03..0x468D3B]: corners from the
   isSquare quad — `c0 = pos + 0.5·len·fwd`, `c1 = pos − 0.5·len·fwd`
   [orig: Entity_ComputeBoundingQuad @ 0x45B6E0 isSquare arm] — lifted by the
   wheel d's; fwd = the normalized front-to-rear line, right =
   normalize(old_up × fwd), up = fwd × right (roll continuity), Euler out
   through the standard substitute pair. **Z = (c0.z + c1.z) × 0.5**
   (flt_7C3B94 = 0.5) — the mean wheel penetration, adopted with the
   slideDecay non-positive clamp. Airborne: the fit of the unlifted corners =
   attitude identity. The contacted-crash fall arm is LIVE: front, rear, and
   center-spine contact below `0x2000` speed latches `+0x2FC`; speed above
   `0x1000` seeds the shared `+0x460` accumulator to `0x016C16C1`, then
   `Math_BuildFixedPointRotationMatrixYXZ(frame, 0, +0x460, 0)` compounds the
   local-Y fall and subtracts 298261 BAM/tick [orig: @0x468B62..0x468D83].
   The later part-spin tick intentionally consumes the same `+0x460` word.
   Wheelie force queues (`Entity_QueueSuspensionForce`) and the flip 0..100
   def clamp remain in the deferred wreck machine.
7. **The contact byte needs a REAR-WHEEL RUN**: `up.z(Q16) > 4096`, the lean
   bound |right.z| < 40960, rear-wheel contact, and MORE THAN ONE consecutive
   rear-contact tick (`entity[1].pad_040[8]`, reset in the both-wheels-off
   branch) [orig: @ 0x47A4CC..0x47A52B] — a one-tick graze never grounds the
   bike. Both wheels off clears the contact byte as it sets Flags 0x2000; the
   mover therefore preserves its ballistic velocity instead of rebuilding it
   from the grounded forward row. Both-wheel landings absorb half the fall
   [orig: `slideDecay −= slideDecay >> 1` @ 0x47B0F1..0x47B103].
8. **The grounded heading/lean smoother** `Entity_SmoothHeadingToTarget`
   @ 0x45B2C0 (roll-rate producer into modelPtr2, the speed>4096 lean-latch
   arm vs the low-speed direct arm, ±itemDef+92C·0.1·scale clamps, the
   |delta| ≤ 256 deadband) is **FPU-garbled in decompile and stays a NAMED
   DEFERRAL** pinned to its disassembly — the lean is presentation-additive;
   pitch/roll pose comes from the axle fit. Sinks grow +100/tick (not 250)
   with `Suspension_CompressWheelQuadratic`/`Suspension_OscillateWheelFast` as the spring
   machinery — zero-state in the subset.

Port: `light_contact_solve` (vehicle_contact_solve.cpp), routed by
`VehicleFamily::Bike` (the tracked-solve interim retired). Bench: the bike
rest/drop legs and `run_bike_wreck_falls_under_gravity` (the crashed sleep
gate, `+0x2EF` contact transition, `+0x2FC` latch, and rotational fall).

## §10 The tank (ctan) mover deltas + the air local-driver input map (ported 2026-08-06)

Source: `Entity_UpdateTankVehiclePhysics` @ 0x488AB0 .. 0x48AEDB (0x242B;
renamed this session from the historic misnomer
`Entity_UpdateMountedInfantryMovement_0`). The ground-mover template
(@ 0x48AF00, §5.38e) with these witnessed deltas — everything not listed
(input dir cases with the 0x16C16C0 ramp / 0x238E38C0 cap, the >>1 reverse
command, the crouch/prone speed mods, the turn-rate blend, the
(target−yaw+32)>>6 servo, the aiState (4−32·delta) smoothing, the
(cmd+received)>>1 local-driver blend, the ±drag and submerged legs) matches
the ground core:

1. **Gravity 250** [orig: `slideDecay += −250` @ 0x48a82c] — the bike step,
   with NO vertical up-cap.
2. **Contact-gated speed integration** with the <48 stop snap [orig:
   @ 0x489f34..0x489f56] — an airborne tank never chases its command.
3. **The servo clamp tree** [orig: chase @ 0x489d79..0x489d84, clamps
   @ 0x489d89..0x489e79]: a direction REVERSAL clamps at ±2·deceleration
   (the ground core keeps the raw 1/32 chase); same-direction at
   ±acceleration (target ≠ 0) or ±deceleration (target 0). The sharp-steering
   caps (±decel >> 2 at |speed| ≤ 4096, the ∓2·decel hard arm) are ported with
   the traction legs [orig: @ 0x489DB1..0x489E97]. A zero-command tank whose
   +0x16C parent is an NPC keeps the raw servo [orig: @ 0x489EBA..0x489ECB].
4. **Full-basis drive velocity** [orig: the contact velocity-build stores
   @ 0x48a5ac..0x48a8b4]: velocity = speed × the normalized basis forward
   row, slideDecay REPLACED by speed × fwd.z — the tank drives along its
   conformed pitch. The low-speed contact-direction realign (the ±5°/tick
   BuildYXZ ±59652323 cross-product rotate toward forward) and the downhill
   creep-hold are ported with the traction legs (section 13). The recovery
   arm (target 0 without sharp steering, or |speed| <= 0x2000) stores
   slideDecay unconditionally [orig: @ 0x48A5D4]; only the straight arm keeps
   a crashed hull's vertical velocity [orig: @ 0x48A28D..0x48A2AE].
5. **Yaw applied unless crash-settled, quartered airborne** [orig:
   @ 0x48a9f7..0x48aa1d `if (!settleByte) Yaw += (Flags & 0x2000) ?
   modelPtr0 >> 2 : modelPtr0`] — the bike shape keyed on the solve-owned
   flag. The +0x2F0 byte is the post-crash settle latch (setters
   @ 0x475FAE, @ 0x476067, @ 0x4780EC, @ 0x469C42, @ 0x48A816, all crash
   paths), not retail's Flags 0x10 park bit.
6. **Joiner interp block, per family** (Corrected 2026-09-22: the earlier text
   called the tank's gates IDENTICAL to the ground chase; the shared ladder
   {6,8,10,15,20,25,30} at {0x2AAA, 0x4000, 0x5555, 0x8000, 0x10000,
   0x20000}, 0x2000 deadband, 0x60000/0x20000-at-reg<293 snap, (delta+10)/20
   heading and (v+64)>>7 drift decay are common, the gates are not; ported as
   `vehicle_client_chase` with `VehicleChaseFamily`,
   `vehicle_motor::test_client_chase_family_gates`):
   - Plain (cbot @ 0x48DB6B..0x48DDD4, selector-zero ground
     @ 0x46E62E..0x46E85A and boat @ 0x470129..0x470355): snap and step every
     axis; Z steps whether or not airborne.
   - Ground (cveh @ 0x48B563..0x48B7C5): the snap moves heading and Z only
     while neither crashed nor settled (@ 0x48B5FB..0x48B622); the per-tick
     heading step is ungated (@ 0x48B767..0x48B77A); Z steps only while
     airborne and unlatched (@ 0x48B798..0x48B7B9).
   - Bike (cbik @ 0x484666..0x4848F3): the snap always moves Z; heading snaps
     and steps only while unlatched, not wheeling (+0x3DE) and upright in the
     entry matrix (@ 0x4846EB..0x48472D, @ 0x48487D..0x4848A4); Z steps as
     ground (@ 0x4848AE..0x4848E3).
   - Tank (ctan @ 0x48912B..0x4893A6): a crashed, crash-settled or +0x2FC hull
     widens the snap radius to 0xC0000 (@ 0x48913B..0x48916E); heading snaps
     and steps only while unlatched (@ 0x4891D5..0x489208,
     @ 0x48933D..0x48935B); Z snaps always and steps as ground
     (@ 0x489379..0x48939A).
   Only dead-pose rows (state bit 0x04) and pool-1 deck rides leave
   prediction. Wire bit 0x01 is the organic mover-skip, which no vehicle mover
   tests (`Entity_UpdatePool1Slot` calls the mover with no Flags test,
   @ 0x4B8E41..0x4B8E53), so a bit-0x01 vehicle row keeps chasing (corrected
   2026-09-22, `netsim_remote_motion_smoothness`). Crashed and settled rows run
   the chase. A joiner mirrors each accepted record's 0xB9 Flags bits onto its
   twin before the client movers run (@ 0x460AEA..0x460AFC), so the client
   crash arm (@ 0x46997C..0x469982) sees the host's word
   (`netsim_joiner_vehicle_replica::run_record_tail_mirrors_flags_and_health`). The differential track-scroll
   accumulators (`track_phase_tick`, section 11) and the turret slew chase
   (`slew_turret`, section 11) are ported.
7. **Crash stop** [orig: @ 0x489C06..0x489C1C]: the tank stops a crashed hull
   only with +0x2EF as well as +0x2EC, like the bike; the ground core stops on
   +0x2EC alone [orig: @ 0x48C086..0x48C08F]. The contact solve owns the
   +0x2EF writes (§8 item 7).
8. **Recoil and heavy-hit impulse** [orig: @ 0x48A8C0..0x48A9C0]: while the
   +0x3EC direction is nonzero, dir × trunc(+0x3FC × 28.16f) (flt_7C6FA0)
   joins vel_x, vel_y and slideDecay every tick (Q16, round half up), after the
   crash-settle zero and before integration; the direction clears once +0x3DC
   drops. Only the tank mover reads the impulse (section 27).
9. **Slope factor** [orig: ctan @ 0x489CE6..0x489D01, cveh
   @ 0x48C1C6..0x48C1D7, cbik @ 0x485362..0x48537E]: the movers sample the
   `Math_BuildSinTable` cos table at (Pitch + 0x200000) >> 22; the port used
   a continuous cosine and wrongly called it the D-INF-4 equivalent until
   2026-09-22.
10. **Trails** sample only at the end of the contact arms; the airborne and
    off-contact arms jump past them [orig: ctan @ 0x48A60D..0x48A67F vs
    @ 0x48A684; cveh @ 0x48CD93..0x48CDFD vs @ 0x48CE02; cbik
    @ 0x486170..0x4861F1 vs @ 0x4861F6] (section 28).
11. **Dispatch**: the ctank and cbike class rows call their movers without
    testing the physics selector [orig: `Entity_DispatchPhysics_ctank
    @ 0x48F000..0x48F007`, `Entity_DispatchPhysics_cbike @ 0x48EFF0..0x48EFF7`];
    cveh, ctrn, catv and cbot test it.
12. **Role gate**: only the input block is role-gated [orig: ctan
    @ 0x489522..0x489545; cveh @ 0x48B949..0x48B96C]. The seat sweep and the
    PlayerControl tail (claimant fold gate @ 0x48AAC4..0x48AADA, engine
    start/stop and part spin @ 0x48AD94..0x48AE3D) run on predicting clients,
    and the handbrake latch reads the +0x170 claimant [orig: cveh @ 0x48C03C,
    cbik @ 0x4851E8].
13. **Entry pose**: every mover prologue copies the pose (+0x04..+0x18) to
    savedLivePose (+0x80..+0x94) before its first bail, on every role [orig:
    ctan @ 0x488B24..0x488B50, cveh @ 0x48AF74..0x48AFA0, cbik
    @ 0x484054..0x484080, cbot @ 0x48D4AC..0x48D4E9]; `tick_motor` and
    `ground_client_tick` stamp after their seed, and the world vehicle passes
    stamp the rows whose mover does not.
14. **Command registers**: the ground and tank templates keep steer target,
    command speed and ramp in brain+0x210/+0x220/+0x224 [orig: ctan
    @ 0x489952 / @ 0x489811; cveh @ 0x48BD89 / @ 0x48BC35]. The port holds them
    in `VehicleMotorState` and mirrors them back into brain[132]/[136]/[137]
    after the ground/boat motor.

Tests for items 3 through 14: `vehicle_motor` (`test_tank_npc_parent_coast_keeps_raw_servo`,
`test_tank_crashed_vertical_velocity_arms`, `test_client_chase_family_gates`,
`test_tank_crash_stop_needs_2ef`, `test_tank_impulse_pushes_velocity`,
`test_slope_factor_samples_quantized_table`, `test_trails_sample_only_in_contact_arms`,
`test_tank_and_bike_ignore_physics_selector`, `test_prediction_keeps_player_control_tail`,
`test_prediction_latches_handbrake_from_claimant`, `test_mover_prologue_stamps_saved_live_pose`)
and `vehicle_mount` (`test_mover_command_registers_are_the_brain_words`).

### 10.4 The aircraft local-driver input map

Source: the occupant input block of `Entity_UpdateAircraftPhysics`
@ 0x490310. Brain registers: [544] fwd cmd, [540] lateral cmd, [548]
climb-above-ground, [524] absolute altitude target, [528] steer heading.
Witnessed and ported (`stage_air_vehicle_input` + the register-mirror gate in
`aircraft_client_tick`):

- The 8-way key thrust table on the air speed slot (itemDef+0x8E8 —
  playerSpeed): fwd full; every diagonal/side/reverse component HALF.
- Analog cyclic: fwd = −(fs·x) >> 7, lateral = −(fs·y) >> 8 (half-scale
  lateral), steer −= (192426·z) >> 1. Crouch/walk mods >> 2 / >> 1 on both.
- The collective: the 0x40/0x80 MoveOrder pair steps ±0x4000/tick across the
  near-ground boundary (2 × boundRadius) between [548] and [524]; the 256 u
  climb ceiling (0x1000000); climb < 0 = LANDED → the engine-off reset
  (commands zero, steer = own yaw, target parks 0x4000 below ground). Our
  state stores the ABSOLUTE target only; the split is carried as
  climb = target − ground (the same quantity retail derives at every
  boundary).
- **The local-pilot blend on BOTH commands** [orig: `([2C4]+[220])>>1 →
  [220]; ([2C8]+[21C])>>1 → [21C]` @ 0x491546..0x491568]: fwd and lateral
  each reconcile with their received mirrors; steer stays owned by local
  LOOK. A remote-piloted row keeps the verbatim register mirror.
- Named deferrals: the analog COLLECTIVE channel (`analogThrottle << 7` — a
  fourth analog axis our C2S uplink does not carry), the occupant's own
  analog-yaw write (D-NET-161), the flare-release weapon scan, and the
  authority engine-flag upkeep.

---

## Section 11: health, turret state, animation ownership and sound edges (2026-09-07)

This pass uses retail `Jointops.exe.kong.i64`, imagebase `0x400000`.
Implementation: `vehicle_motor.cpp`, `vehicle_motor_air.cpp`,
`vehicle_part_anim.{h,cpp}`, `vehicle_sound.cpp`, the item-traits fold,
`inmatch/present_rows.cpp`, and Godot's `entity_presenter.cpp`.
The proof is an instruction/decompiler grill and regression tests, not
complete mission-level vehicle parity.

| Witness | Ported behavior |
|---|---|
| `[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]`, phase `@ 0x48AF24..0x48AF2D`, health `@ 0x48AFFD..0x48B083`; boat `@ 0x48D561..0x48D616`; aircraft `@ 0x4903F4..0x490480` | All families use `(tick + 36 * DcbId) & 63`, authority only. Above critical HP, regeneration requires `health < max - regen` strictly. At/below critical HP, criticalDrain floors at zero. The x9 LEA is scaled by four again: the decompiler's `tick[9 * id]` is not a nine-tick offset. Boat health runs before authority-only capsize damage. |
| `[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]`, drain `@ 0x48D05A..0x48D083` | Ground/bike/tank lose two HP per submerged authority tick. Boat flag 0x8000 means normal afloat operation and must not use this drain. The attacker-slot clear at death remains unmodeled. |
| `[orig: Entity_UpdateAircraftPhysics @ 0x490310]`, `@ 0x490459..0x4904A6`; pilot follow `@ 0x4904A6..0x4904D0` | The burning hull's -2886390 BAM yaw step shares the authority 64-tick cadence. The separate pilot-yaw follow (occupant +0x170, MoveOrder bit 0x10 freelook clear, Flags 0x2000, brain+0x20C − ground > 0x10000 → `add [eax+10h], 0FFD3F50Ah` every tick, no authority or cadence gate) is ported in `vehicle_motor_air.cpp` — section 16. |
| `[orig: Entity_UpdateHeloRotorSpin @ 0x48FA70]`, call `@ 0x4905A6` | Rotor integration runs before the lift gate; the exact full-speed crossing opens the gate that tick. A cold occupied player-control rotor plays slot 30 using the vehicle profile and pilot position only above water and at speed <= 10737417.6. Aircraft does not advance the ground wheel phase. Each caller admits only its own profile type (ground 2, helo 1); other air profiles bypass rotor run-up without running a different spin machine. |
| Ground `@ 0x48D0E3..0x48D15D`, bike `@ 0x48669A..0x486711`, tank `@ 0x48AA23..0x48AA9E`, aircraft `@ 0x492694..0x492703` | Movers slew active brain yaw (+0x1D8) toward staged yaw (+0x1F0). Below absolute wrapped delta 0x2108421, all six staged words commit; equality takes the yaw-only step. Ground/bike/tank step by signed 0x2108421. Aircraft uses retained signed yaw demand (`@ 0x491CE4..0x491CF8`), not the damped yaw velocity. Watercraft has no such slew block. |
| `[orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0]`, `@ 0x489F6E..0x489FA0` | Before velocity/contact solving, +0x2BC adds `((speed << 12) - yawRate) << 3`; +0x2C0 adds `((speed << 12) + yawRate) << 3`, with 32-bit wrap. Stationary steering counter-rotates the tracks. |
| `[orig: Entity_CacheVehicleHUDStats @ 0x4929B0]`, `@ 0x4929F6..0x492AC5` | TIRE00..05 are clamp(comp0), clamp(comp1), clamp((comp0+comp2)>>1), clamp((comp1+comp3)>>1), clamp(comp3), clamp(comp2). Average before clamping. |
| Renderer table `@ 0x82CFD0..0x82D00F`; `[orig: HUD_CacheEntityDebugStats @ 0x449C10]`; `[orig: HUD_CacheInfantryDisplayInfo @ 0x48F1A0]` (the IDB name; a misnomer — the chel render cache: rotor `+0x466`, gear `+0x470`, view `+0x45E`; sections 14 and 16 use this one name); `[orig: HUD_CacheVehicleDisplayInfo @ 0x48F140]` | `render_function` selects independently of `move_function`. Tank WHEELS00..03 alternate unsigned track high words. Gun yaw/pitch use signed active-brain high words (`@ 0x449ECF..0x449EE2`). Ground, tank, helo and plane own different channel subsets; absent channels are released. The curated tank/helo function names were retained despite their misleading descriptions. |
| `[orig: Entity_ProcessMovementSoundEffects @ 0x5294A0]`, `@ 0x5294A8..0x5294D8`, `@ 0x52953B..0x52959D`; ground caller `@ 0x48D196..0x48D1B8` | A submerged player claimant takes the stop branch. Zero speed substitutes saved-live-pose XYZ displacement, then sqrt/clamp/truncate and the 256 deadband. Ground's speed denominator falls back to brain speed A, then command speed. |
| `[orig: Entity_UpdateVehiclePhysics @ 0x48AF00]`, sound tail; `[orig: Entity_UpdateWatercraftPhysics @ 0x48D480]`, engine edge | Ground lights emit slot 24 on the rising edge. First claimant emits slot 30 when the occupant eye is above water. Claimant loss clears motion lanes and emits slot 31 subject to the hull water gate. Explicit detach (`Entity_DetachFromVehicle @ 0x4355F0`, `@ 0x4356EF..0x435759`) clears the motion lanes and plays slot 31 on the departing occupant while its eye clears the water (`@ 0x43571B..0x43573E`), releases the +0x1CC smoke emitter and writes no +0x318 bit, so the mover's leave edge plays a second stop on the hull the next tick (tank `@ 0x48ADE9..0x48AE33`; corrected 2026-09-22, the port had cleared the latch). Mover stop includes +0x18000 clearance. |
| Mounted panel gate `@ 0x5A5038` | A missing interface texture suppresses both silhouette and seats. A valid texture admits both. |
| Critical warning (slot 34) cadence: `Entity_UpdateVehiclePhysics` `test bl,1Fh @0x48B08C`; `Entity_UpdateLightVehiclePhysics @0x48416C`; `Entity_UpdateTankVehiclePhysics @0x488C44`; `Entity_UpdateWatercraftPhysics @0x48D61F`; `Entity_ProcessInfantryPhysics @0x46E250..0x46E266`; `Entity_ProcessAirVehiclePhysics @0x46FB79`; `Entity_UpdateAircraftPhysics` `test byte ptr [esp+var_A4],3Fh @0x4904D7..0x4904F0` | The slot-34 warning fires on the `& 0x1F` (32-tick) phase for every mover except the direct-air family, which uses `& 0x3F` (64 ticks); the `0x3F` tests in the other movers are the authority health regen/drain cadence, not the warning. (Corrected 2026-09-08; `vehicle_motor::test_warning_cadence_by_family`.) |
| Skid latch under the settle gate: cveh settle jump `@0x48D163..0x48D16A` lands ON the skid section `@0x48D264` (bit 8: and `@0x48D2C4`, set `@0x48D2EB`, clear `@0x48D345`); ctan's settle jump `@0x48AAB7..0x48AABE` lands at `0x48AD49` PAST its skid section `@0x48ABCB..0x48ACB5`; `flt_7C19E0` = 0x4EFFFE00 = 2147418112.0 | A settled cveh/cbik wreck still runs the skid latch/clear; only the tank skips it. The skid test is `ftol(min(sqrt(cx²+cy²+cz²), 2147418112.0)) != 0` with speed nonzero and `!(Flags & 0x2000)`. `+0x318` bit values: 1 claimant latch (`@0x48D3A5`), 2 reverse latch (`@0x48D1D7`), 4 lights latch (`@0x48D358`), 8 skid latch (`@0x48D2C4`), 0x20 a tank-only latch (`@0x48AAE0`). (Corrected 2026-09-08.) |
| Helicopter loops: `update_vehicle_effect_emissions @0x528F20` `@0x52919D..0x5291CE` / `@0x5291ED..0x52921D` / `@0x529235..0x529260`; lifetime `effect_params+16 = 15 @0x528F94` → `SoundEmitter_RegisterSetLayers` slot word 21 `@0x528471`; `ItemDef_ResolveAllResources @0x49E7F0` fills `ItemDef.soundLoopId[7] @0x82C` from `res[16+i]` | Lane 21 reads `soundLoopId[2]` (+2100 = Soundloop_3, the `*_DLP` loop), lane 11 `soundLoopId[1]` (+2096 = Soundloop_2, `*_ILP`), lane 1 `soundLoopId[0]` (+2092 = Soundloop_1); each registers with lifetime 15 (the ground fold's `SoundEmitter_Register @0x5292A6` packs 30). Retail sndprof.def helicopter profiles author only soundloop_2/3 (SP_Apache1: V_APACHE_ILP / V_APACHE_DLP .8 1.2), so lane 1 is normally silent and Soundloop_4..7 are never consulted. (Corrected 2026-09-08 — the PR row had slots 7/6/5; `vehicle_part_anim::test_helicopter_sound_curves_and_decay`.) |

The presentation mask carries ownership only. A joiner publishes its local
twin's motor controls (track phases, steering, speed, wheel phase, tire and
rotor channels) exactly as the authority collector does, because the render
callbacks run on every peer against that peer's own mover state [orig:
HUD_CacheEntityDebugStats @ 0x449C10, track words @ 0x449C3C..0x449C69;
Entity_CacheVehicleHUDStats @ 0x4929B0; the client mover's track phases
Entity_UpdateTankVehiclePhysics @ 0x489F98 / @ 0x489FA0] (2026-09-22,
`netsim_present_rows::test_joiner_vehicle_motion_controls_reach_present_rows`).
The hull's gun yaw/pitch words come from the turret child: its ewep class update
writes them into the parent brain [orig: Entity_UpdateTransformAndTurret Def gate
@ 0x440E8C..0x440EA0, profile type @ 0x440F65..0x440F6B, GROUND
@ 0x440F70..0x440F8A, HELO @ 0x440FA1..0x441020], where the tank render callback
reads them [orig: @ 0x449ECF..0x449EE2]. A joiner runs no brains, so its form of
the class update lands the words on the carrier's replica row
(`ClientEntityState::carried_gun_*`) and takes the profile type from the
ai_function brain class, else the motor family: a joiner-side structural
stand-in (D-NET-157 context), not a new row (2026-09-22,
`netsim_present_rows::test_joiner_hull_gun_words_follow_the_turret_child`).
Native regressions cover
cadence/authority, submersion, sound transitions, rotor timing, wrapped track
math, turret commit boundaries and snapshot transport. GUT covers channel
updates and ground-to-tank-to-helo ownership changes.

The subsequent sections complete the tank's fourteen tire channels, helo
gear, contact sound, all movement-loop families, rotor wash and the force,
contact and death-state machinery. Renderer ownership is retained: simulation
exports the selected control state, and presentation composes it with PANM.

## IDB changes made during these sessions

2026-08-21 (the post-merge tidy): `Vehicle_ApplyBrakingForce @0x45CEB0` →
`Suspension_CompressWheelLinear`, `sub_45CFB0` →
`Suspension_CompressWheelQuadratic`, `Entity_ApplyDamageOscillationFast
@0x45D110` → `Suspension_OscillateWheelFast`, `Entity_ApplyDamageOscillation
@0x45D240` → `Suspension_OscillateWheel` (bodies read: compression/extension/
energy/impact-sink bookkeeping and the free-decay envelope; no braking, no
damage), `Entity_UpdateGravityAccumulator @0x4928B0` →
`Entity_UpdatePartSpinAccumulator`; stale "braking"/"damage" comments replaced
with the witnessed contracts; the `+0x2B8` integrate `@0x48E9F0..0x48E9F9`
annotated on `Entity_UpdateWatercraftPhysics` (it is the watercraft mover, not
`Entity_UpdateAircraftPhysics @0x490310`). Saved.

2026-08-21 (the wire-up round's witness pass): `[opennova 2026-08-21 W2]`
latch-correction comment on `Entity_ProcessWheeledVehicleSuspension @0x46b140`
(the gate reads `+0x2ED` NONZERO; `+0x2EC` = crashed) and `[… W]` comments on
the tank/bike seeds `@0x4698a0`/`@0x468a50`, the `+0x2ED` producers
`@0x475de0`/`@0x479600`/`@0x47c1c0`, `Entity_RespawnVehicle @0x45ff40` (the
spawn values), `Entity_ClearSuspensionState @0x4592b0` (the field set),
`ItemDef_ParsePhysicsProperty @0x49d870` (the four keys), `Entity_UpdatePartSpinAccumulator
@0x4928b0` (the profile-type gate), the HELO twin `Entity_UpdateHeloRotorSpin @0x48fa70`,
`Entity_CacheVehicleHUDStats @0x4929b0` (46 = 47), `Entity_UpdateVehiclePhysics
@0x48af00` (the wheel phase). Proposed, not applied: `Entity_UpdateHeloRotorSpin @0x48fa70` →
`Entity_UpdateHeloRotorSpin`; `dword_81518C` → `g_wheelSlipLockQ16`. Saved.

`0x47EF10` defined + named `Entity_ProcessAircraftContactPhysics` (a misdecoded
instruction run at `0x480051` re-created as code); the cpln class-table callback
`0x45D6F0` defined as the 5-byte thunk `j_Entity_UpdateAircraftPhysics`; witness
comments on the mover/solve heads per the session notes in §5.38e. Saved.
2026-08-05: witness comment on `Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0`
(the §7 contract summary + the opennova port cross-reference). Saved.
2026-08-06: `0x488AB0` renamed `Entity_UpdateMountedInfantryMovement_0` →
`Entity_UpdateTankVehiclePhysics` (the ctan class-table mover) and `0x483FE0`
renamed `Entity_UpdatePlayerInfantryMovement` → `Entity_UpdateLightVehiclePhysics`
(the cbik mover) — both historic misnomers; witness comments on the two solves
(§8/§9 contract summaries + port cross-references). Saved.

PR #640 review corrections (2026-09-08; IDA read-only, nothing renamed): the
record's own names were corrected to the IDB's — `Entity_DispatchPhysics_catv`
→ `Entity_DispatchPhysicsUpdate @0x48F010`; `AI_UpdateHelicopterCombatMovement`
→ `AI_ProcessVehicleCombatState @0x461080`; `AI_UpdateAircraftCombat` →
`Entity_ProcessInfantryWeaponFire @0x471710`; `Entity_Respawn` →
`Entity_RespawnVehicle @0x45FF40` (no `Entity_InitFromItemDef` edge);
`HUD_CacheEntityDisplayInfo_Helicopter` → `HUD_CacheInfantryDisplayInfo
@0x48F1A0` (one name, misnomer noted); `Physics_ResolveEntityCollision` →
`Entity_MovementCollisionResolver @0x4B2BD0`; `Entity_ProcessAirPhysics` →
`Entity_ProcessAircraftContactPhysics @0x47EF10`; the `WaterWake_*` names →
`sub_5DDC60` / `CWeatherSlot_Init` / `sub_5DDDB0` / `sub_5DDE10`; the bike/boat
lean pairing (`Entity_SmoothHeadingToTarget @0x45B2C0` = bike,
`Vehicle_UpdateTurretRotation @0x45AEA0` = boat) restored from `xrefs_to`; the
§1.13 cadence rewritten to `36*DcbId` (the `lea` pair `@0x490334`/`@0x490340`);
the section-11 "pilot-yaw follow unported" line reconciled with section 16; the
section-29 render-callback labels, lane/slot mapping and focal-wind clear
moments corrected; section 31's boat-wave reading corrected; the three
section-13 "behavioral proof" rows replaced by instruction-level witnesses; the
D-VEH-2 wording changed from "Accepted" to "proposed, pending ratification".
Ported at the review (all cited in code): the vehicle-class brain machine + the
class key + the brain lifetime, the off-contact/crashed traction arms, the
steering blend default, the submerged-driver cut on both selector-zero movers,
the selector-zero sound tails, the helicopter lane/slot mapping and lifetime,
the aircraft warning cadence, the skid latch under the settle gate, the
rotor-wash surface-effect group protocol and spawn-time zone binding, the
grazing correction width, the tank catch-up gate, the bike lean gate order, the
part-spin call gating, the pilot flare descriptor bytes, the death spin-rate
order, the thinkCooldown seed and the crew clip/reserve zeroing. Witnessed and
NOT ported (ledger D-NET-161 (a)..(h), D-SND-17, D-INF-2): listed in those rows.

## Current coverage

The vehicle work in this record is implemented through section 37. Native
regressions pin authority/prediction gates and the fixed-point transitions;
GUT exercises snapshot transport, renderer ownership and retail attachments.
These checks establish the covered branches, not an exhaustive retail LAN
or pixel-by-pixel comparison. D-NET-196 remains open for organic body-conform
presentation and its authority smoothing arm, outside the vehicle consumers.
D-NET-161 remains OPEN, narrowed to the residuals the 2026-09-08 review
witnessed and did not port (the pool-3 deck-marker localization, the flare
off28 target, the ground-height tap ray kinds, the emplacement brain dispatch;
the bike launch vector, the selector-zero boat part-spin call, the
`entity+684` step mirror and the run-over player gates have since been ported),
each with its witness in the ledger row. D-SND-17 closed 2026-09-22 when the
tank fold's extra-effect argument (the yaw rate), the tread cue and the
last-tick gate were ported (section 40).

## Section 12: model probe contacts and impact response (2026-09-07)

`engine/runtime/world/collision_vehicle.cpp` replaces the single raised hull
sphere with the family solvers' actual wheel and spine arrays. The common fold
is in `vehicle_contact.cpp`; ground, tank, bike, aircraft and boat callers retain
their own probe counts, radii, second-pass depth buffers and torque decay.

| Component | Verdict | Evidence |
| --- | --- | --- |
| Model contact probes and graded lateral forces | MATCHING | `collision`: deck support, 0/1/2/3 slope grades, stale-candidate prefilter and mounted-child exclusion |
| Shared severe-impact health and momentum | MATCHING | `vehicle_motor`: exact authority damage, client health preservation, mass-weighted velocity/position transfer and sustained-contact edge |
| Family integration | MATCHING for the shared contact fold | `vehicle_mount` wall drive; `aircraft_client_motor`, `watercraft_client_motor` |

The witness is retail `Jointops.exe`, imagebase `0x400000`,
`Jointops.exe.kong.i64`, confirmed before the research. The model half of
[orig: Entity_CheckCollisionState @ 0x462A30], sites
`@ 0x462DFB..0x4632CC`, matches the boat twin
[orig: Entity_ComputeCollisionForces @ 0x462150], sites
`@ 0x462561..0x462A24`:

- The source's existing proximity slice supplies candidates. A candidate whose
  ground-link chain reaches the source within three hops is excluded. A heavy
  source wakes a much lighter/smaller player-control candidate with Flags 0x40
  and its type-1 contact timestamp; otherwise every probe queries the posed
  model through the existing bone-contact SAT implementation.
- The query mask is 8, or 24 when the source's ItemDefAttrib2 low byte has bit 7.
  Callback-only targets do not supply physical forces. Contact flag 0x800 feeds
  the source's auxiliary 0x40 flag. Indoors skips terrain and preserves models.
- Vertical force is always retained. The radial planar dot gate controls the
  lateral response. Absolute force verticality selects full, half or quarter
  lateral force at severity 3, 2 or 1; climbable contacts retain vertical
  support at severity 0. The same signed, low-dword ricochet corrections adjust
  vertical support. Severity 3 retains the last hit entity and wall-probe flag.
- Source mass and radius gates use strict comparisons. The optional pushable
  branch reads a present brain with commanded speed zero, not an AI disable
  byte: the decompiler's padded field name hid the actual `brain+0x220` read.

[orig: Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0], sites
`@ 0x47CD00..0x47CF55`, and the bike/air siblings apply authority-only damage
when the immunity flag is clear. Unit type 3 requires impact and saved-pose
movement both above 29300, then dies. Other ground/bike/air hulls lose truncated
absolute post-torque speed times mass times the retail float multiplier
(0.000025, halved for mass at most 3). The boat instead loses 100 HP above the
same impact/movement thresholds
[orig: Entity_ProcessPlatformPhysics @ 0x481870], sites
`@ 0x482336..0x482402`. The tank has no shared severity-three health branch;
its other impact tests belong to its family solve. Reaching zero clears the
last attacker.

The scrape edge requires impact above 2344 and clear brain sound bit 0x10.
Its SSAudio3 lookup and mass-weighted momentum exchange share that edge; the
exchange also runs on clients. The contacted vehicle gains three quarters of
`relative_velocity * our_mass / total_mass` in velocity and one quarter in
position. Severity 0/1 clears the edge; severity 2 preserves it. The second
force pass scales planar separation and the caller's saved depths by the other
vehicle's mass share. Boat retains its seventh unscaled depth. The terrain-only
quarter-speed cut now requires no hit entity, as the original does.

The ground movement-sound call's final argument is `entity+0x3D4 > 30`, an
airborne streak, rather than immediate model-contact severity
[orig: Entity_UpdateVehiclePhysics @ 0x48AF00], sites
`@ 0x48D181..0x48D1C7`. The caller now supplies that state. The tests' old
center-sphere assumptions were replaced with explicit probe fixtures; the wall
route includes model bounds, a valid road terrain layout and saved-pose stamps.

Grazing-correction width (2026-09-08): both terrain probes form `pen * (−2r)`
with the two-operand 32-bit `imul` before `cdq; idiv` —
`Entity_ComputeCollisionForces @0x4623BF` (Y twin `@0x4623D7`),
`Entity_CheckCollisionState @0x462C5F` (Y `@0x462C77`), `ebx = −2r` set
`@0x462B7A..0x462B7E`. Wheel probes (index < 4) skip the clearance gate
`@0x462C16..0x462C45`, so the wrap is reachable; `plat_terrain_probe` now uses
the wrapped low-dword product
(`vehicle_motor::test_plat_terrain_probe_grazing_uses_wrapped_product`).


## Section 13: traction, wheel slip and movement effects (2026-09-07)

`engine/runtime/world/vehicle_traction.cpp` implements the contact-direction
state consumed by the ground, tank and bike movers. All findings below are
anchored to retail Jointops.exe; the class table identifies the families even
where an older curated function name describes another family.

| Component | Verdict | Evidence |
| --- | --- | --- |
| Ground/bike handbrake slip and grip recovery | MATCHING (IDA-witnessed 2026-09-08; DIVERGENT in three off-contact legs before the review fixes below) | held direction under `+0x3CD` `@0x48C5AC..0x48C69B` (cbik `@0x4858F1..0x485964`); slip stamp `@0x48C702..0x48C71C`; inclusive hold `cmp edx,eax; jle @0x48C7AF..0x48C7B1` (accel twin `jle @0x48C2B3`: > 5·tireslip → accel>>4 `@0x48C2CE..0x48C2F0`, else deceleration `@0x48C397..0x48C3CB`); opposing arm speed > 0x100 `@0x48CB59`, speed ∓ acceleration `@0x48CBE8..0x48CC16`; aligned arm planar dot < 0xEEEE && speed > 0 `@0x48C98F..0x48C9A0` then the 1° step 0xB60B60 `@0x48CA13` (cbik `@0x485CDA`/`@0x485D5E`); clear arm zeroes `+0x3F8/+0x3BC/+0x3C0/+0x3C4` `@0x48CD56..0x48CD6A`; wheel carry lock `dword_81518C` `@0x48C49F..0x48C4B4`, reset on target 0 `@0x48C53F`, decay `min(1, 1 − speed·flt_7C6F98)` `@0x48C506..0x48C52F`; grounded store's Z rule `cmp +0x2EC,0 @0x48CD70..0x48CD8D`; `vehicle_motor` |
| Tank pivot and track phase ordering | MATCHING (IDA-witnessed 2026-09-08) | track phase `+0x2BC += ((speed<<12) − +0xA4) << 3; +0x2C0 += ((speed<<12) + +0xA4) << 3` `@0x489F6E..0x489FA0` runs before the basis build `@0x489FB3`, and the ctan steering block `@0x489C26..0x489CE0` writes only `+0x2B4` (no `+0xA4`), so the phase consumes the previous tick's yaw rate (normal-arm write `−speed·(+0x2B4>>2)` `@0x48A5DA..0x48A607`); pivot gates `cmp eax,0EFFFh; jge @0x48A38D` and `cmp +0x29C,500h; jle @0x48A398` → clear arm `@0x48A4BA..0x48A532` (zeroes `+0x3BC/+0x3C0/+0x3C4`), else the 5° step 0x38E38E3 `@0x48A418` signed by `var_F8` `@0x48A40D..0x48A416`; `vehicle_motor` |
| Skid sound, rev edge and rigid masked particles | MATCHING (the particle half IDA-witnessed 2026-09-08; the skid-sound/rev-edge half in `vehicle_sound.cpp` carries the PR #640 cites `@0x48D226..0x48D34E` and was re-grilled only for the settle-gate structure in section 11) | `Entity_SpawnBoneEffectsAtMask @0x458750`: gates `+0x44C == 0`, graphicModel, itemDef word[340] effect and word[342] mask `@0x45875E..0x458789`; camera distance vs `(tick&3)==0 ? 600<<16 : 300<<16` `@0x4587D0..0x458816`; surface type 3 → word[341] `@0x458831..0x458838`; matrix = entity orientation under Flags 0x20000 `@0x458843..0x458854`; per masked bone (48-B stride) position via `Math_FixedPointTransformPoint22 @0x615810`, direction via `Math_TransformPointFixedPoint22 @0x412e90` `@0x4588F0..0x458991`; call sites cveh `@0x48C285` (skid accel arm), `@0x48C535` (slip-carry decay), `@0x48CF8D` (crashed ≥ 0x1000); cbik `@0x48547A`/`@0x48571D`/`@0x485868`/`@0x4863CD`; ctan `@0x48A820`; `vehicle_motor` |

[orig: Entity_UpdateVehiclePhysics @ 0x48AF00] and
[orig: Entity_UpdateLightVehiclePhysics @ 0x483FE0] retain the Q16 contact
vector through a handbrake skid. Ground captures it with both rear pads in
contact; bike needs its rear wheel. Uphill Z clamps to zero before normalization.
Releasing the brake starts a hold of five times the authored `tireslip` ticks,
including the final boundary tick. Afterwards an opposing direction decelerates;
an aligned direction either clears or turns by one degree using the original
raw cross-product axis. Capturing, holding and releasing are distinct operations.
The wheel phase advances before contact and uses the occupant input gate (the
bike inhibits it on LeanRight). The large negative slip carry locks at the
retail constant and decays after grip recovery.

[orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0] uses the steering angle for
its four-degree pivot gate. A pivot can retain a previous contact direction;
slow realignment turns by five degrees. Track animation integrates the previous
stored yaw rate before the current velocity/yaw block replaces it. Ground, tank
and bike retain the solved-contact and airborne-streak fields through their
contact tails. Tank and bike bias an existing direction downhill after descent
[orig: @ 0x47951E; @ 0x47C0EB].

Off-contact and crashed velocity arms (re-grilled 2026-09-08; three
divergences fixed in `vehicle_traction_velocity`): the basis is built before the
gates; cveh's entry gates are `@0x48C587..0x48C5A6` and its off-contact arm
`@0x48CE02..0x48D003` (the `+0x3CE` gate `@0x48CE17`, the slip-stamp
refresh/clear `@0x48CE24..0x48CE61`, the not-crashed `jz loc_48CF97 @0x48CE6E` →
forward·speed into `+0x98/+0x9C`, `+0xA0` only when `fwd.z < 0`
`@0x48CFD9..0x48D003` — the PR held velocity here; crashed = 2-D normalize
`@0x48CE74..0x48CEEE`, NO speed step, X/Y stores `@0x48CF0E`/`@0x48CF46`). ctan
enters its arm on `+0x2F2 == 0` alone `@0x489FFF..0x48A005` (no `+0x3CD` test;
the PR gated it on the handbrake), arm `@0x48A684..0x48A81F` (not crashed → hold
`@0x48A6AD`; crashed 3-D normalize, ∓2·decel `@0x48A752`/`@0x48A75E`, three stores
`@0x48A78B`/`@0x48A7AF`/`@0x48A7D3`). cbik gates `@0x4858CC..0x4858EB`, arm
`@0x4861F6..0x4863D5` with NO `+0x3F8` slip handling (the PR refreshed/cleared it
for the bike; crashed: ±0x6000 clamp `@0x486232..0x48624F`, 3-D normalize,
∓1·decel `@0x4862EE`/`@0x486305`, stores `@0x48632B`/`@0x486355`/`@0x486379`). The
shared tail — speed ≥ 0x1000 → `Entity_SpawnBoneEffectsAtMask`, else halve
X/Y/speed, zero `+0xA4/+0xAC/+0xA8`, `+0x2F0 = 1` — is `@0x48CF41..0x48CF95` /
`@0x486386..0x4863D5` / `@0x48A7E0..0x48A81F`. OPEN (D-NET-161 (a)): the cbik
not-crashed off-contact arm `@0x4863DA..0x48657E` drives velocity from a stored
launch vector `+0x3E0..+0x3E8` (3-D normalized `@0x4863E3..0x486451`) under the
`+0x3DE` has-been-driven byte, else from a planar pair kept in frame slots
`outVec`/`var_E4` (`@0x48645D..0x4864A7`; writer not located this pass), stores
`@0x4864C1..0x48651F`, slip-stamp refresh/clear `@0x486523..0x48655E`, join
`@0x48657E`; the reimpl carries no `+0x3E0` state and holds the bike's velocity
there.

Steering blend (2026-09-08): the turn-rate blend fraction is 0 whenever
`playerSpeed == 0` in every ground family (`cmp eax,edx; jz → xor ecx,ecx`
`@0x48C0A9..0x48C0D8`; ctan `@0x489C3C..0x489C67`; cbik `@0x485283..0x4852B0`),
so a zero-`player_speed` row turns at `minRate` — the PR's `Tank ? 0 : 0x10000`
default was wrong for Ground/Bike
(`vehicle_motor::test_zero_player_speed_turns_at_min_rate`). The remaining cveh
drive-core sites, for the record: proportional step `@0x48C111..0x48C12C`, wheel
deflection `@0x48C12E..0x48C14E`, yaw rate + the `0x2000`/`+0x2EC`/`+0x2F0` gates
`@0x48C146..0x48C18F`, coast brake `@0x48C195..0x48C1C2`, cos² slope
`@0x48C1C6..0x48C1FA`, raw accel `@0x48C1FC..0x48C215`, the straight-drive clamp
tree `@0x48C3D0..0x48C46C` (parent `+0x16C` Flags 0x100 gate `@0x48C428`), the
airborne half-deceleration `+0x8E4 >> 1` `@0x48C471..0x48C492` reached by
`cmp +0x2F2,0; jz` / `cmp +0x2F0,0; jnz` `@0x48C20E..0x48C228`, integration
`@0x48C2FC..0x48C32A`, gravity −324 `add [esi+0A0h], 0FFFFFEBCh @0x48D009`.

Part-spin call gating (2026-09-08): `Entity_UpdatePartSpinAccumulator @0x4928B0`
is reached only inside the `itemDef->attrib & 0x40` block at all six callers —
cveh gate `@0x48D38B` / call `@0x48D42B`, ctan `@0x48AD97`/`@0x48AE3D`, cbik
`@0x486944`/`@0x4869EA`, selector-zero ground `@0x46F8FF`/`@0x46F99E`,
selector-zero boat `@0x47004B`/`@0x4700F5`, mounted infantry
`@0x488956`/`@0x4889F5` — so a non-PlayerControl item never draws the machine's
PRNG roll through any mover. `Entity_UpdateWatercraftPhysics` has no such call;
its wheel-phase add `mov edx,[ebp+220h]; shl edx,0Dh; add [esi+2B8h],edx`
`@0x48E9F0..0x48E9F9` sits inside the `cmd != 0` (`@0x48E92E`) and `up.z > 0`
(`jle loc_48EA06 @0x48E9A6`) gates of the thrust block. The full movers' tails
now call `part_anim_tick` only for `traits.player_control`, and the boat keeps
its phase step inline.

The movement sound pass uses the solved direction and airborne streak. Slot 25
starts on a new skid edge; slot 33 requires an occupant, an airborne streak above
30 and a rev counter above 124. The counter increments after the check. Missing
slot 25 falls back to `TIRE_SKID`; slots 26/36 fall back to `IMP_DEBMED_LAND`
(the global name table at `@ 0x82F590`). These sounds use the existing distance
delay queue. The contact scrape edge and shared impact response are in section 12.

[orig: Entity_SpawnBoneEffectsAtMask @ 0x458750] resolves the first sixteen
matching authored `particlefxs` userpoints, transforms their rigid position and
Q16 direction, and emits a transient effect for the next source tick. Camera XY
range is 600 units on every fourth tick, 300 on intervening ticks. Surface type 3
selects `particlefxs_snow` when authored. Entity `+0x44C` suppresses the emitter.
The native effect queue crosses a typed Simulation drain to the Godot particle
presenter; no physics or effect selection is implemented in the shell.

## Section 14: tank springs and renderer channels (2026-09-07)

The tank uses six compression channels. Before its probe solve it snapshots two
middle channels as the averages of wheel 0/3 and wheel 1/2
[orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0,
@ 0x476432..0x476473]. Belly probes follow the relevant compression too.
The collision function's optional output is positive terrain separation, written
before the terrain broad phase and replaced by the second contact pass. A gap
larger than the stored impulse arms half that gap while amplitude is below 500
[orig: @ 0x478643..0x478706]. A grounded penetrating wheel consumes this impulse
as mass times gap, caps its energy at 65536, and adds 1.25 times that energy to
the impact sink.

[orig: Suspension_CompressWheelLinear @ 0x45CEB0] drains the linear spring
energy by half the low-dword spring/step product. The tank caps each step at
1023. Grounded release uses
[orig: Suspension_OscillateWheel @ 0x45D240] with the slow float phase step
0.08722222596406937; airborne wheels do not run free oscillation. The upright,
uncrashed grounded tail releases compression into each positive terrain gap
[orig: @ 0x4790A1..0x4791B3 settled, @ 0x479205..0x47930F otherwise]; the
airborne branch (which jumps to `0x479445`, `@ 0x478B54` / `@ 0x478B64`) and the
crashed/inverted grounded path (`@ 0x479311..0x479318`) keep their compression.
(Corrected 2026-09-22: the earlier text had a common-tail release.)
`vehicle_suspension` exercises these numerical boundaries; `vehicle_motor` and
`vehicle_followups` (`tank_airborne_tail`) cover the integrated family path.

The grounded catch-up (2026-09-08): `mov eax,[eax]; sub eax,0FAh; test eax,eax;
jle loc_478E27` `@0x478DC0..0x478DC9` returns before the same-side pair-clear
tests `@0x478DDF..0x478E0E` whenever `sink − 250 <= 0`; the tracked twin clamps
(`jns`/`xor` `@0x47E9DC..0x47E9DE`) and still runs them. The tank loop's grounded
call now carries that flag
(`vehicle_suspension::test_tank_grounded_catch_up_skips_within_one_step`).

[orig: HUD_CacheEntityDebugStats @ 0x449C10] is the tank renderer cache despite
its curated name. It publishes fourteen tire words from the six channels:
repeated front/back values, pairwise means and two triplet means. Both triplets
multiply the retail float 0.33; the x87 stack retains the constant across the
first conversion, which Hex-Rays obscures. Nonnegative clamping follows each
mean. `vehicle_part_anim` pins all fourteen values and negative inputs. The
present-row schema, native appliers and wire memoization table carry all fourteen
channels under tank-specific ownership.

The helicopter's gear word is unsigned `entity+0x470`
[orig: HUD_CacheInfantryDisplayInfo @ 0x48F1A0 (the IDB name; a misnomer —
the chel render cache), load @ 0x48F1C8]. Its mover approaches 65535 by 1598 per tick when terrain
clearance is at least five units, otherwise approaches zero by the same step
[orig: @ 0x492676..0x492743]. `aircraft_client_motor` exercises the threshold
and both clamps; renderer ownership and projection are in `vehicle_part_anim`.

IDB checkpoint: entry comments were appended at `@ 0x462A30`, `@ 0x48AF00`,
`@ 0x483FE0`, `@ 0x488AB0`, `@ 0x458750`, `@ 0x475DE0`, `@ 0x449C10` and
`@ 0x48F1A0`, with implementation cross-links. Existing names and types were
preserved and the database was saved.


## 15. Moving supports and matrix extraction

Witness: `Entity_UpdateVehiclePhysics @0x48AF00`, bike `@0x483FE0`, tank
`@0x488AB0`, boat `@0x48D480`, aircraft `@0x490310`, and
`Math_FixedPointMatrixToEulerAngles @0x613310`.

Ground, bike and tank refresh `groundEntity` on the global entity-update
counter modulo eight (`@0x48AFB9`, `@0x484099`, `@0x488B69`). Boat and aircraft
use `(current_tick + 36*DcbId) & 7`. The zero-radius aircraft ground sample
subtracts brain[11] before the ray and adds the live/dead ground offset after
it. Brain[11] originates in the absolute CMDL lower Z bound in
`Entity_InitHelicopterAIFromDef @0x4683C0`.

A valid support carries the hull through the complete saved-to-current parent
pose before network interpolation. This is the same Q22 inverse-old/forward-new
rotation and biased point transform used by the throwable follower, without
the infantry capsule offset or radius detach rule. `carrier_motion.cpp` owns
that shared transform; `vehicle_contact.cpp` owns refresh and vehicle publication.
The native mover test covers deck acquisition, translation and rotation, then
release on the next failed ground ray.

Matrix-to-Euler extraction removes yaw, then pitch, using individually shifted
signed Q22 products before each `atan2`. It negates matrix elements 4 and 6;
yaw uses the negative `683565275.5764316` scale (`dbl_7C57B8` =
−683565275.5764316, `dbl_7C19D8` = +683565275.5764316, `dbl_7C3608` =
1.4629627251502471e-9, `dbl_7C3600` = 4194304.0; disasm `0x613316..0x61347B`;
`collision::test_fixed_matrix_euler_witnessed_vector` pins the Q22 matrix
{3413315, −807948, −2299715; 1970678, 3243611, 1785386; 1434536, −2533455,
3019254} → yaw 357913891, pitch 238609309, roll −477222480, hand-traced with
every ftol at least 0.12 from an integer). Subsequent sine/cosine calls
use `1.4629627251502471e-9` radians per BAM, which is 30.5 ppm above the
mathematical inverse. Thus compound rotations do not round-trip exactly.
The shared `collision_matrix_to_euler` now retains these individual truncations
and signed-zero quadrant selection. Platform plane fits and bike fall axes
consume it, including yaw publication for crashed hulls.

## 16. Aircraft controls and countermeasures

Witness: `Entity_UpdateAircraftPhysics @0x490310`,
`HUD_CacheInfantryDisplayInfo @0x48F1A0` (the IDB name; the chel render
cache, which calls `HUD_CacheEntityDisplayInfo` first),
`AIEntity_ReleaseFlareCountermeasures @0x455EF0`,
`Entity_InitVehicleAI @0x460200`, `Weapon_FireProcess @0x53F5B0`,
`Entity_FireWeaponAndSendPacket @0x42BD80` and
`NetPacket_WriteEntityPositionUpdate @0x42A610`.

The collective keeps brain[548] (climb above ground) distinct from brain[524]
(absolute altitude). Strictly below twice the bound radius, keyboard/analog
collective changes climb; at or above that boundary it changes absolute
altitude and derives climb. The following reconciliation uses an inclusive
boundary. The absolute ceiling is `0x1000000`; negative climb floors to zero.
Zero climb then parks altitude at ground minus `0x4000`, clears both cyclic
commands, and holds hull heading. Keyboard `0x40/0x80` takes precedence over
the signed fourth analog byte shifted by seven. That byte is local input;
retail's C2S 0x0C carries only the three cyclic/pedal bytes.

Analog pedals subtract `(192426*analogZ)>>1` from pilot yaw unless freelook
is held (`@0x491083..0x49108C`). Critical-burning aircraft separately subtract
2886390 from pilot yaw every airborne tick above one unit of target clearance;
the authority hull spiral still runs only on its health cadence. Pilot pitch
feeds the +0x45C relative-view servo: `(error+4)>>3`, steps beyond +/-1924260
become +/-3848520, occupied result clamps to [-298261600,0]. The unoccupied
arm adds its own signed value without that clamp. The helicopter renderer
reads +0x45E signed for its brainless GUNPITCH fallback (`@0x48F1D5`); brain
gun registers override it. Dead hulls finish carrier/interpolation and received
command mirroring, then skip live controls, attitude integration and gear.

Flare input scans weapon/seat-list slots 1 through 9, reading each associated
seat occupant's MoveOrder bit 0x20. The pilot entry is excluded. The 64-phase
boundary clears entity+0x318 bit 2; nonzero climb plus a pressed seat fires once
and sets it. AI fire-timer callers use the same dispenser directly. Verbatim
(2026-09-08): `Entity_BuildWeaponSlotList(entityPtrs[10], slotTypes[10], entity)`
`@0x4911a5..0x4911b5`; for entries 1..9 (`@0x4911c5..0x49145c`, unrolled) a
non-null entry reads `occ = word[entry + slotTypes[i]*2 + 0x190]` (the per-slot
seat occupant word), skips 0xFFFF, pool-resolves it through `g_pool_list`
(`handle >> 12` pool, `& 0xFFF` slot × stride) and ORs `(rider+0x12C >> 5) & 1`;
then `if ((var_A4 & 0x3F) == 0) entity+0x318 &= ~4` `@0x49145e..0x491465`; then
`if (entity+0x224 != 0 && pressed) { if (!(entity+0x318 & 4))
AIEntity_ReleaseFlareCountermeasures(entity) @0x455ef0; entity+0x318 |= 4; }`
`@0x49146c..0x49148b`. `VehicleSystem::tick_flare_input` builds the fixed
10-entry `VehiclePanelSlotList` (the port of retail's two frame arrays, tail
zeroed like `Entity_BuildWeaponSlotList @0x434db4..0x434dd3`) and early-outs on
`net_climb == 0` before the side-effect-free scan — observationally identical.

The flare's C2S 0x06 descriptor (2026-09-08): `Weapon_FireProcess @0x53F5B0`
takes the vehicle's occupant (+0x170, the pilot) as the shooter `@0x53f6d6` and
calls `Entity_FireWeaponAndSendPacket(aim, occupant, aiRuntime->f0_7[3], 1,
ammoDefIndex, 0)` `@0x53f70a`; the packet then carries off6 = 1 (the ammo-def
arm), off7 = the FLARE ammo-def index, off32 = the PILOT's `entity+352` (his
handheld ammo-def index, `@0x42c052` → `@0x42a7da`), off33 = `fire_flags` =
`Weapon_GetScopeZoomLevel(can_fire, 12) | (can_fire ? 0x80 : 0)`
`@0x42bdd6..0x42bdf9` where a seated pilot (parentSlot 2 or 5) fails
`Player_CanFireWeapon @0x5cf7a8..0x5cf7b6`, so off33 = 12 (`@0x422fd1`/`@0x422fd5`),
off34 = 0, and off28 = the VEHICLE's `aiRuntime[3]` pool-resolved (0xFFFF when
null, `@0x42a70d`). On a joiner the packet ships only when the pilot is the
local player (`@0x53f6f4`). `JoinerRole::tick_local_weapon`'s source-fire leg
now stamps the pilot's handheld ammo index and subtype 12
(`inmatch_joiner_role::run_flare_descriptor_carries_the_pilot_handheld`); off28
stays 0xFFFF — OPEN, D-NET-161 (c).

The dispenser requires a brain and chooses GROUND_FLARE for profile type 2,
FLARE otherwise. Model initialization retains the first 16 case-insensitive
FLARE-prefix userpoints (across the whole point table). With no points, fire
uses the hull pose. With points, position and direction use the full scaled
placement matrix; a zero local direction Z becomes 24576 before rotation.
Yaw is atan2(Y,X); pitch uses atan2(Z,truncated capped horizontal length).
The temporary equipped-ammo byte does not escape the call.

`vehicle_countermeasures.cpp` joins these inputs to `RoundSim::fire_source`.
Launch presentation belongs to the vehicle; the primary occupant owns the
round. A remote client presents only. Authority/offline fire uses the existing
round ring and spawn path; a local joiner queues the ammo-indexed C2S fire
through the same sequence/pose encoder as the player weapon. The minefield
source-fire caller shares this mechanism.


## 17. Wreck callback dispatch and shared falling physics

`Entity_UpdatePool1Slot @0x4B8DD0` independently invokes the think/death
callback at +0x1C8 when its age permits (`@0x4B8E2A..0x4B8E3C`), then the
installed update callback at +0x1C4 (`@0x4B8E41..0x4B8E53`). Consequently a
vehicle dying/dead AI tick can explicitly call generic falling physics as
well as running its installed death-motion callback that frame. Frame-based
deduplication would change that behavior. Once death installs an update
callback, the live family mover no longer runs.

`destruction_motion.cpp` ports `Entity_ProcessFallingDeathPhysics @0x461D30`
as the shared movement operation. It stamps the full saved pose before
checking StaticDeath. Otherwise gravity is -334 even when every velocity
was initially zero; fully submerged hulls halve planar velocity and use
vertical -4096. The ground ray includes model supports. The upright rest
line subtracts the absolute husk section-0 minimum Z; an inverted hull adds
the absolute maximum Z. A step at or below that line retains the previous
full pose and clears only vertical velocity. It neither snaps to ground nor
emits the specialized falling callback's landing blast. The dying state
uses this operation before its stopped/still test.


## 18. Vehicle spawn anchors and wreck respawn

`Game_StartMission @0x525F80..0x526071` grounds player-control vehicles,
executes their update once, then records the resulting six-word pose, support
and team. Attached spawn positions use the support-local transform from
`Entity_TransformWorldToParent @0x43BB50`; respawn resolves it with the live
support via `Entity_TransformParentToWorld @0x43BD00`. These helpers rotate
position through yaw/pitch/roll, but only subtract/add the parent's yaw from
the stored angles. A missing or dead support blocks respawn. The stuck check
uses the same live anchor instead of a fixed mission-world position.

`AI_TickVehicleDead @0x467EA0` advances the wreck timer and calls falling
physics before authority-only removal/respawn decisions. A player-controlled
wreck is removed when flag 0x1000 is set or vehicle respawn is disabled. After
15 ticks, a nonzero cooldown decrements; a transition from one to zero restores
one and returns, while other values continue. The overlay-wait arm moves the
wreck by 5000 units in X/Y and buries it 1000 below terrain
(`Terrain_SampleHeightBilinear @0x6067B0`, call `@0x467F3C`). Otherwise the saved
pose/team and the route from the AiSlot words +0x94/+0x98 (`@0x468005..0x46801D`)
are restored before `Entity_RespawnVehicle
@0x45FF40` (the IDB name; the PR's `Entity_Respawn` → `Entity_InitFromItemDef`
chain was wrong — its callees are `Entity_ClearSuspensionState`,
`EntityDef_LoadModelsAndCallbacks`, `Entity_BuildProximityListsFromPools`,
`Entity_RaycastGroundHeightAndObject`, `Math_BuildFixedPointMatrixFromEulerAngles`,
`EntityList_ClearParentRef` and `CEffectEmitter_ReleaseSafe` on `+0x1CC`
`@0x460199` / `+0x400` `@0x4601b2`; it zeroes the words `+0x138` `@0x4600ae` and
`+0x1AC` `@0x46006e` and pends `cur == 15 ? 14 : 22` `@0x460130`). Its first
act re-runs the class's ItemDef+0x148 init (`@0x45FF53..0x45FF68`, then
+0x468/+0x460 = 0 `@0x45FF6D..0x45FF73`): `Entity_InitVehicleAIFromDef
@0x4686C0` stamps the class, keeps an existing brain, copies the slot route raw,
seeds the weapon ammo gated on the profile's resolved ammo bytes
(`@0x468882..0x4688B7`), the speeds, the turret, step 16 with its stagger, the
current state's enter and the gunner setup; the helicopter twin is `@0x4683C0`.
It also zeroes the kill credit +0x178 (`@0x460074`). Ported 2026-09-22 as
`VehicleSystem::rerun_class_init` at the head of `VehicleSystem::respawn`.
The implemented reset restores health, clears seats and death state, resets
motion/springs/chassis and controls, sets flags to preserved 0x400 plus 0x22000,
sets vertical speed to -501, and resamples the ground. Not re-witnessed at the
2026-09-08 review: how those two retail emitter slots map onto the reimpl's
three event-driven bank releases plus `vehicle_release_damage_effects`
(section 28).

`Game_StartMission`'s pool-1 leg `@0x525F80..0x526071` is the anchor capture
above (`Entity_RaycastGroundHeightAndObject(entity,0,0,0x10000,0x100000)`, Z =
ground + brain[11], water clamp, Flags |= 0x40, `+0x2A4` = Z, `+0x165` = `+0x162`,
the update callback, `+0x264` = groundEntity, `Entity_TransformWorldToLocal` into
`+0x24C`); it then seeds `thinkCooldown` (`+0x128`) = `aiSlot[18] / 62`
`@0x526079..0x52608F` (`imul 0x84210843; add edx, ecx; sar 5`, the sign fix: the
dividend add-back makes it a signed divide by 62, not 31) for each hull with an AI
slot. The spawn seeds slot[18] = 62 times the authored spawn count (record +0x3E,
`Entity_SpawnFromBMSRecord @0x40EFE4..0x40EFF4`, def gate AI attrib 0x100000
`@0x40ED4E`), so the budget is the spawn count, and `AI_TickState_VehicleDead`
consumes one per respawn and holds at 1 `@0x467eff..0x467f1c`
(`vehicle_lifecycle.cpp initialize_mission_vehicles`, ported 2026-09-08; the /62
divide and the vehicle slot seed, `promote.cpp init_ai_slot`, corrected
2026-09-22: before that promote seeded organics only, so a vehicle's budget was 0,
which meant unlimited respawns). On jox01 07TR the enemy hulls author zero spawns
and no route, so their budget stays 0 there (unlimited respawns, as retail's
0 / 62), and their respawn route comes from the slot words the event redirects
wrote. The pool-3 spawn-MARKER leg that
precedes it is NOT ported — section 25.

`vehicle_lifecycle.cpp` owns these transitions. `destruction` tests cover the
15/16-tick boundary, cooldown one/two, team/health/suspension restoration,
moving/rotating supports, dead-support rejection and client/authority removal.

## 19. Resting contacts and orientation recovery

The five contact solvers test all three angular rates. Ground, tank and
helicopter also require zero wheel energy and free-fall sinks; bike retains
its separate occupied/crash gates. Boat rest additionally requires a saved
pose with identical X/Y and Z difference below 500. Rest undoes this tick's
vertical step and halves vertical speed. Collision wake flag 0x40 expires only
after more than 200 logic ticks, after the resting early exit.

The parked bit 0x10 is authority-owned. `Entity_BuildOrientationFromVectors
@0x458DF0` rebuilds an inverted resting attitude and disables movement effects;
`Entity_RebuildOrientationMatrixFromAxes @0x4632E0` rebuilds upright and clears
retained chassis rotation. Both normalize the forward axis and derive side/up
from the requested vertical axis, then use the shared retail Euler extractor.
The names alone do not describe which direction they choose. The common
resting logic is in `vehicle_contact.cpp`.

## 20. Retained chassis forces

`Entity_ComputeChassisOrientation @0x463940` consumes four five-word force
records. Nonzero rates normalize their direction, displace the corresponding
corner, fit the resulting frame, and append the old-to-new rotational delta
to the retained matrix. All force records clear after consumption.
`Entity_ClearSuspensionState @0x4592B0` resets that matrix, quaternion, blend
clock and active flags without clearing the independent wheel springs/sinks.

`Entity_ApplyBoneAttachmentTransform @0x45A750` applies the retained matrix
before advancing its timed identity blend. The matrix product sums all three
products before one Q22 rounding bias (`@0x613940`). Quaternion conversion and
slerp preserve the retail axis swizzle, 0.01 linear threshold, and unnormalized
interpolated quaternion (`@0x615C70`, `@0x615E20`, `@0x615A70`).

`Entity_ProcessWheeledVehicleSuspension @0x46B140` distinguishes a newly armed
crash, continued fall, wreck, inverted slow/fast recovery and an ordinary
four-corner fit. Its slow inverted arm reads all seven contact slots, including
spine 4 and 6, with unequal 8000/800 forces. Fast recovery also applies planar
velocity-directed forces. `vehicle_chassis.cpp` implements this state machine
for ground and aircraft contacts. The motor, aircraft, suspension and
destruction regression groups pass with this integration.

## 21. Mounted driver body animation

`Entity_UpdateInfantryPlayerBody @0x4B40E0` and `Entity_UpdateInfantryAI
@0x4B9910` both derive sit_N from the seat name. State 100 (sit_24) changes to
110 below hull roll -71582784, 109 above +71582784, then 108 for negative
speed, then 107 for absolute speed below 200. These are ordered assignments
with strict boundaries and no clip-presence gate. It is hull roll, not steering
input. The shared mounted-state resolver implements the same rule for both
body paths; other seat states are unaffected.


## 22. Bike and planing-boat lean

The lean error is the side axis's Z component (Q22 matrix element 9 shifted
by six), not the up axis's Y component. The public motor tests cover both
signs. A sleeping bike does not run the lean servo; a newly contacted hull
carrying flag 0x40 does.

Bike `Entity_SmoothHeadingToTarget @0x45B2C0` (the IDB name, a misnomer; sole
caller `Entity_ProcessLightVehiclePhysics @0x479600` at `@0x47A7D3` — the PR's
text had the two routines swapped, corrected 2026-09-08 from `xrefs_to`) runs
only when not crashed, not settled and not airborne, in the caller's order
`cmp +0x2EC @0x47A78B` (crashed returns), `mov byte ptr [esi+2EFh],0 @0x47A79F`
(the override byte clears), THEN `+0x2F0 @0x47A798/@0x47A7A6` and
`test Flags,0x2000 @0x47A7A8` return. Above speed 4096 it enables the lean override;
steer magnitude above two degrees applies authored lean velocity unless the
signed slope exceeds the authored lean limit. Otherwise error above 256
drives roll back toward zero. The high-speed deadband retains the old roll
rate; the low-speed deadband clears it. The +45C previous steering angle,
+468 steering delta, +464 envelope origin and +470 interval retain their
separate update gates. The caller supplies turnRate=0 and scaleFactor=1.

Boat `Vehicle_UpdateTurretRotation @0x45AEA0` is a hull-lean routine despite
its curated name (sole caller `Entity_ProcessPlatformPhysics @0x481870` at
`@0x4838CF`). It runs for planing, afloat boats above speed 8192. It uses
a two-degree steering deadband, slope deadband 160, authored lean limit,
and retained acceleration/deceleration interval. The interval stores only
16 bits; its progress clamps at one. The negative and positive arms retain
their different wrap/reversal gates and signed clamps. The small-steer
corrections are -2386239/+2386092 BAM. Outside the active planing branch,
the caller uses deadband 240, clears the lean override, and resets the
interval. Normal platform fits postmultiply the retained pitch/roll lean
and chassis transform; the settled/airborne branch runs the shared
suspension state machine. Crashed boats also adopt fitted yaw.

## 23. Crash effects, contact recovery and visual restoration

The live damage smoke (+1CC) and fire (+400) are independent emitter slots.
The motor health bands use Effect_smkSigB and the family-specific
vehicleFireSmall/Med/Large effects; critical health plays slot 34 every
32 staggered ticks. Tank smoke changes to large fire below criticalHp/8.
Contact settling starts smoke and stops movement effects. The authority
drains five health while above the critical-drain threshold; the original
family-specific fire thresholds and parked-bit rules remain separate.

Ground and tank contact tails recover their orientation after the parked
bit clears. Ground requires an inverted up axis; tank accepts a crashed
up axis below 8192 or a settled hull. Aircraft clear each contacted sink
and use the opposite contact-diagonal indexing, then perform their own
crashed/settled recovery. The tank suspension wrapper also retains its
tilt-dependent slow-recovery force and 8000/8000 spine-pair forces.

Respawn releases damage/wreck effects and publishes an intact-model
restoration event. The presentation pass restores original child visibility
or the static instance, clears the terrain shadow replacement, retires the
husk, and releases owned emitter bindings. The destruction presentation
test exercises this through real model/effect/anchor objects: 17 tests,
174 assertions pass. Destroying a ground vehicle also kills its authored
child emplacements, guarded by the parent's registry generation.

## 24. Live death entry and aircraft death states

All live movers check zero health before the regeneration/burn cadence.
Unless already flagged dead or in the corresponding dying/dead state,
ground/bike/tank/boat request state 21; aircraft request state 13. A missing
current state defaults to 22 or 14 respectively. These requests were absent
from the motor core even though the ground death handlers existed.

The original aircraft dispatch rows are:

| State | Enter | Tick | Event |
|---|---|---|---|
| 13 | AI_TransitionToDeath_Vehicle @0x4668A0 | AI_CheckLethalDamage @0x457910 | AI_HandleAlertEvent @0x457A30 |
| 14 | AI_ResetToPatrol @0x457A70 | AI_QueueDeathSoundEvent @0x457AA0 | AI_HandleEvent_VehicleDestroyOnly @0x466BE0 |
| 15 | Entity_ProcessVehicleDestruction @0x466A80 | Entity_UpdateVehicleAIMovement @0x461200 | consume all @0x457A60 |
| 22 | AI_FullResetToPatrol @0x458090 | AI_CheckAliveOrDead @0x4580C0 | AI_HandleEvent_HelicopterDestroyOnly @0x468040 |

Aircraft death probes lift one unit and search down 48, then add the
intact/husk height offset and clamp to water with an occupant. State 13
starts the immediate death initializer above ground except for the 20mm
ammo case, sets climb command 1336, and queues event 4 when below the
threshold. Its tick queues that event at/below the threshold or when
stationary; otherwise climb command increases by 1336 and the work heading
turns by INT32_MAX. State 15 resets the respawn timer, clears the team and
target, and installs death state. Its tick leaves movement to the separate
death callback and begins respawn checks strictly after 30 ticks, compared
with 15 for ground.

`Entity_InitDeathState @0x48F7C0` queues the authority's kz_MItemBlast,
clears scars, spawns pieces and death sounds/effects, optionally installs
piece physics, sets dead/husk flags and seeds the two hull spin rates.
The immediate grounded impact uses Effect_HeloGroundHit/EXPLO_HELO_LAND;
water uses Effect_MedSplash/EXPLO_HELO_WATER. It also releases the live
vehicle fire slot.

The two hull spin rates come from `Death_RandomSpinRateBam @0x57B940`
(2026-09-08): floor = ftol(min·11930464.0f) first, `r = PRNG % 100`, `fild r ×
0.01f` kept extended, max = ftol(max·11930464.0f), `fimul`, ftol, then max with
the floor (`flt_7D76D0` = 0x4B360B60 = 11930464.0f, `flt_7C56A8` = 0x3C23D70A =
0.01f); the callers are `@0x48F98A` (+172 roll, 0.45..0.65) then `@0x48F9A4`
(+168 pitch = −(0.15..0.35)) — roll draws first. Ported as
`death_random_spin_rate_bam`
(`destruction::test_aircraft_death_spin_rates_roll_in_retail_order`: seed 0x200
→ r 93/13 → roll 7211964, pitch −1789569). `Entity_InitDeathState @0x48F7C0`
also clears the focal-wind slot owner (`Terrain_ClearShadowTileSlot @0x5CB0D0`
at `@0x48F9F8`; the only other caller is `Entity_Destroy @0x43E984`).

The idle state-22 subtype test reads AI profile +20: BOAT is one. The AIP
parser accepts STD=0, GROUND BOAT=1/TRAIN=3, and HELO PLANE=2. An alive
idle boat copies its pose into the work transform and sets work Z to water.
A dead boat queues event 3; other dead idle ground rows and state 14 queue
event 5.

**The kill record (2026-09-22, the tank fix round).** After the death
transforms, an in-session authority sends the S2C 0x26 kill record (section 0)
from the ground vehicle's dying and destroyed enters and the aircraft's dying
and dead enters, each through `Server_SendEntityStatePacket @0x509D70`:
`AI_TransitionToDeath_GroundVehicle` (Flags&4 test `@0x467B32`, send
`@0x467B43..0x467B58`), `AI_TransitionToDestroyed_Vehicle`
`@0x467E40..0x467E55`, `AI_TransitionToDeath_Vehicle` `@0x46694E..0x466963`
and `Entity_ProcessVehicleDestruction` `@0x466B2C..0x466B41`. Vehicles never
send S2C 0x13: the router `Entity_CheckAndProcessDeath @0x51B550` is reached
only from the organic bodies (`@0x4B4CEA`, `@0x4B9D4D`, the console kill
`@0x4D29EC`). Ported in `h_enter_vehicle_dying`, `h_enter_vehicle_dead`,
`h_enter_aircraft_dying` and `h_enter_aircraft_dead`; `route_round_deaths`
now runs the organic transaction and the 0x13 send only for Organic victims
(`ai::test_vehicle_death_states_send_kill_record`, `npruntime_round_sim`,
`npruntime_round_end`). The death dispatch also sets Flags 6 on the Flags word
the compact serializes (`Entity_DispatchDeathCallback @0x493F63`), so the
wreck streams the dead-pose form (net-re §5.13).

### Respawn marker research

The authoritative selection entry is `assign_overlay_spawn_points
@0x529E60`; 0x52A110 is an interior address. Server_TickUpdate calls it
every 32 logic ticks. Game_StartMission resets its five lists, registers
pool-1 type-1 vehicles by their saved spawn team, then invokes
`build_spawn_marker_budget_list @0x529B40`. That builder requires numbered
ChangeTeam+SpawnPoint zones at both ends of the chain, collects pool-3
attrib2-bit-4 markers, and associates each with the nearest same-number
zone in pools 1/2. Marker priority is its zone number when the minimum zone
belongs to team one, or maximum-minus-marker-number when it belongs to team
two. The selector's occupancy, group-mask and saved-pose writes are a
distinct producer of the overlay-wait bit consumed by section 18.

IDB comments for this continuation are attached to the lean, aircraft
death/idle and respawn-marker entries above. The IDB carries the current function comments and corrected rotor-loop signature.

## 25. Respawn-marker assignment

The selector lives in `vehicle_spawn_markers.cpp`. Its five team lists use
the complete registry spawn identity. Each numbered ChangeTeam/SpawnPoint
zone supplies marker priority; the selector respects secure-team ownership,
nearby occupancy, the saved group mask, and the vehicle's saved spawn team.
A successful assignment writes the complete six-word spawn pose and clears
the overlay wait. There is no retry against a lower-priority occupied choice.
The shared lifecycle restores that pose, team, health, children, controls and
effect ownership. [orig: assign_overlay_spawn_points @ 0x529E60;
build_spawn_marker_budget_list @ 0x529B40]

NOT ported (witnessed 2026-09-08, D-NET-161 (b)): `Game_StartMission @0x524360`
localizes deck-carried spawn markers before its pool-1 vehicle leg —
`@0x525E6D..0x525F58` walks pool 3 for the marker item types 0x1771..0x1774 /
0x17CE..0x17D1 (6001..6004 / 6094..6097, matched through
`ItemList_FindIndexByTypeId @0x49E100`), calls
`sub_414380(entity, 0, 0, 0x10000, 0x80000)` `@0x525F25..0x525F34` and, when the
marker's `groundEntity` (+0x28) is then set, rewrites its Position in place
through `Entity_TransformWorldToLocal @0x43BB50` against the carrier
`@0x525F47..0x525F50`. `vehicle_spawn_markers.cpp` has no counterpart: the
markers always carry an empty `spawn_support`, so a marker authored on a deck
never follows its carrier.

The parked/player input fold writes the current state only. The pending
state remains available to the state callback: zero-health requests cannot
be overwritten by the same tick's idle stamp. `vehicle_mount` exercises
occupied and unoccupied ground/boat death through the real AI tick.

## 26. Aircraft AI combat and flight controls

`ai_aircraft.cpp` implements the aircraft combat and evade states, formation
offsets, flight phases, terrain clearance, target selection and the primary/
secondary weapon requests. The control modes 0/1/2/3 and 10000/10005 retain
their separate heading, altitude and speed budgets. The AI profile parser
carries the authored fixed-point values to these consumers; it does not
rescale them again in the motor.

Weapon cones and muzzle cycling use the posed model and the first sixteen
authored user points. Mounted emplacements retain their own relative aim
frame. Flares use the authored countermeasure weapon scan and authority/
prediction role gates. `ai`, `vehicle_mount` and
`aircraft_client_motor` pin the live decisions and movement.
The witness map in `ai_aircraft.cpp` records each original state callback.
[orig: AI_EnterState_AircraftCombat @ 0x466330;
AI_TransitionToDeath_Infantry @ 0x465F60;
AI_ProcessVehicleCombatState @ 0x461080;
Entity_ProcessInfantryWeaponFire @ 0x471710 (the IDB name; the aircraft
brain's weapon-fire leg, wired as the state-8 tick)]
The PR's `AI_UpdateHelicopterCombatMovement` / `AI_UpdateAircraftCombat` names
did not exist in the IDB (corrected 2026-09-08). Two more misnomers to read
past: `AI_CalcGroundVehicleTarget @0x4613A0` is the HELICOPTER mover (controller
0x10000, table `0x8153E0[1]`) and `AI_CalcHelicopterTarget @0x461870` the PLANE
mover (0x10005, `0x815408[1]`).

Mover facts re-grilled 2026-09-08 (ported in `ai_aircraft.cpp`, pinned by
`ai::test_aircraft_combat_mover_pins` / `test_aircraft_fire_arc_gate`): the helo
mover clears `[127]` at its head `@0x4613ca` and writes `[127] = [45] << 14` at
LABEL_35 `@0x4616c7` from both within-max_chase arms (only `[40] = 0` is gated on
`AI_GetSuspensionFirePoint` `@0x4616b1..0x4616bd`; the > min_chase / angle > 0x40
arm skips the fire check); its reverse bearing is a second truncated
atan2(self − target) `@0x461453..0x46149e` folded against the target's Yaw; helo
ceiling test ground + 819200 `@0x46172c`, plane ground + 3276800 `@0x461b5c`,
replacement ground + [51] + 3276800 in both (`@0x46173b`/`@0x461b6b`); speed
clamps `@0x46174c..0x46176c` / `@0x461b71..0x461b8b`; the plane's no-target idle
`@0x4619a4..0x461a07` falls through to the common tail `@0x461b3f` (the helo
returns `@0x4615a1`); the plane mover never writes `[127]`. Fire leg
(`@0x471710`): arc limit = `(movsx byte [profile+67] | 1) | 2` `@0x471736..0x47173a`,
`sar 1`, unsigned `cmp/ja` `@0x472477..0x472481` straight to the epilogue
`@0x472df5` (bone/last_weapon untouched); the unprocessed no-weapon leg tests
block flags & 1 (TURRET) at profile+136 `@0x471c18` / +168 `@0x471d18`, primary
first then return. `Entity_CalcAverageGroundHeight @0x457230` with radius 0 is
one centre ray (`@0x457254`/`@0x45735d`); its per-tap ray KIND is not ported
(north/south/west `Entity_RaycastGroundHeight @0x4142c0`
`@0x45725d`/`@0x457281`/`@0x4572c1`, east/centre
`Entity_RaycastGroundHeightAndObject @0x414320` `@0x4572a1`/`@0x4572e0`; the
reimpl uses one ray kind — D-NET-161 (e)).
`Vehicle_CleanupTeamEntitiesOnDestruction` zeroes the gun words (+804/+802 are
+0x324 pitch and +0x322 yaw, not clip/reserve) unconditionally
`@0x5470f9..0x547100` for every matched non-vehicle peer before the optional ammo
split `@0x547107..0x54710e`; both death legs lead with the cleanup for a refNum
carrier (`Entity_SpawnDeathPieces @0x493409..0x49344d`,
`Entity_UpdateDeathTransforms @0x494669..0x494673`) (ported 2026-09-08; the words
corrected 2026-09-22, `emplaced_gun_channel::test_carrier_destruction_resets_child_words`).

### 26.1 The vehicle-class brain machine, its class key and the brain lifetime

`EntityAI_ProcessVehicleStateMachine @0x4583C0` (cveh/cbot/ctrn) and
`EntityAI_ProcessInfantryStateMachine @0x4581B0` (CHel/cpln) share one body and
differ in exactly four gates: the alert edge pends 18 unless `cur == 22`
`@0x458442..0x458448` (vs 10 unless 14 `@0x458239..0x45823b`); the client tick
gate runs the tick table only when authority or `cur` is 21/23
`@0x458545..0x45855c` (vs 13/15 `@0x458340..0x458348`); the client commit gate
accepts `pend` 16 or 21..23 `@0x458576..0x458586` (vs 7 or 13..15
`@0x458375..0x458382`); the spawn AIEvent channel word is 0 (`xor ebx,ebx
@0x4583cd`, store `@0x45851a`) vs 9 (`@0x458312`). The death event (4) writes
channel 9 in both (`@0x4582c8`/`@0x4584d4`). Both mirror the brain step into
`entity+684` after the tick `@0x458568` and zero it on a committed transition
`@0x4585b4` (the air twin `@0x458363` / `@0x4583b0`). That word is the pool-1
visit's think countdown: `Entity_UpdatePool1Slot @0x4B8DD0` runs the class
event callback (the machine) only when the PRE-decrement word is <= 0
(`cmp [esi+2ACh],0; jg` `@0x4B8E1B..0x4B8E22`), after
`Entity_BuildProximityList @0x4B3DC0` (`@0x4B8E25`), then the +0x1C4 motor
every visit (`@0x4B8E53`), and subtracts one at its tail (`@0x4B8EA0`); the
class init seeds brain[7] = 16 (`@0x468915`) and `+684 = dword_B21F80++ mod 16`
(`@0x46891C..0x468945`, helo twin `@0x468645..0x468669`). A brain therefore
thinks every brain[7] visits (16 with step 16), on the visit after any
transition, with a 0..15 spawn stagger. `AI_BeginUpdate @ 0x457B40` is a
separate movement-controller row, never a callback admission gate; its per-entity
phase ownership and the removed global gate are documented in
[world section 34](world-wac-ai-re.md#34-ai-callback-ownership-and-movement-controllers-2026-09-18).
Ported 2026-09-12 (the former D-NET-161
(f)): the gate, re-arm, zero and decrement in `AiSystem::tick` /
`apply_transition` over `Entity::spawn_phase` (the same +0x2AC dword the org2
body think reads; the item callbacks read it as `class_think_ticks`), seeds in
`promote.cpp` / `teammate_spawn.cpp`; `ai::test_vehicle_brain_think_countdown`.
The per-think `Entity_BuildProximityList` call is the port's
`CollisionWorld::refresh_blink` (the entity's own blink/indoors refresh, run on
the think visit ahead of the callback; the per-source candidate slices are the
separate 17-tick `Entity_BuildProximityListsFromPools @0x4B8EB0` rebuild); the earlier
`vehicle_mount::test_helo_ai_flight` exact-orbit pin (414.44) retired to a
band with the every-tick think. Ported 2026-09-08 as
`AiSystem::process_vehicle_state_machine` / `process_infantry_state_machine`
over one `StateMachineGates` table (`ai::test_vehicle_class_state_machine_gates`).

The class key: `g_EntityClassEventCallbackTable @0x813000`, 41 rows (count
`@0x8133D8`) of 24 bytes = name[8] + 4 fn ptrs, consumed ONLY by
`Entity_LookupRenderCallbacks @0x407DC0` (stricmp over the names) from
`EntityDef_InitAllCallbacks @0x4A5AAE`, keyed by items.def `ai_function` (else
"Null"). fn1 is the brain machine: CHel `@0x8132a0` → 0x4581B0; cpln `@0x8133a8`
→ thunk `0x462120` → 0x4581B0; cveh `@0x813378` → 0x4583C0; cbot `@0x813390` →
thunk `0x462130` → 0x4583C0; ctrn `@0x8133c0` → thunk `0x462140` → 0x4583C0. fn2
= `Entity_InitHelicopterAIFromDef @0x4683C0` / `Entity_InitVehicleAIFromDef
@0x4686C0` (cbot/cpln/ctrn thunks `0x474560`/`0x474550`/`0x474570`), fn3 =
0x45D700, fn4 = `Entity_SerializeVehicleState @0x460560`. ewep `@0x8130a8` fn1 =
`Entity_UpdateChildAttachment @0x4409A0` (not a state machine); the null row's
fn1 = `sub_406FF0`, a 15-byte stub. `AI_DispatchStateMachineByProfileClass
@0x4680A0` has NO callers and must not be cited as the dispatcher. JOX ITEMS.DEF
`ai_function` census: cveh 15, chel 14, cbot 10, ewep 3; no cpln/ctrn/ctank rows.
Reimpl: `VehicleTraits::brain_class` (item_traits.cpp; chel/cpln → Air,
cveh/cbot/ctrn → Ground, else Unset → by mover family; a brain with no traits
row keeps the air machine — where retail ticks an emplacement brain's machine is
unwitnessed, D-NET-161 (g)).

Brain lifetime: `AI_TickState_VehicleDead @0x467ede` →
`Server_RemoveEntityAndNotify @0x50A270` (S2C 0x12 mask 0x90,
`Entity_RemovePlacedDevicesByOwner @0x546e00` for players, then `Entity_Destroy
@0x43E810`); `Entity_Destroy` memsets the 812-byte brain `@0x43e995`, nulls
`entity+100` `@0x43e99d` and memsets the AiSlot `@0x43e9ae`; `Entity_InitVehicleAI
@0x460200` allocates the lowest slot with `[0] == 0` `@0x460204..0x460222` and
memsets it `@0x460246`; the org1 corpse path calls `Entity_Destroy` `@0x4b9f93`
then jumps to the epilogue `@0x4b9f9b`. Reimpl (2026-09-08): `AiSystem::release`
+ `attach` slot reuse; release sites `vehicle_lifecycle.cpp tick_dead`,
`infantry.cpp` corpse despawn, `entity_commands.cpp remove_ssn/remove_group`,
`world.cpp` orphan children, `napi_np_protocol.cpp` player leave
(`ai::test_dead_vehicle_despawn_frees_brain`). Sites deliberately without a
release (no brain by construction): placed devices, minefields, flags,
`promote.cpp` (attach frees a stale brain itself).

### 26.2 Gunner attachments: the same-refNum peer list on the 'agun' points

Both class initializers test the def byte `ItemDef+0x548` after the enter
handler (`cmp byte ptr [edx+548h],0` `@0x46895A`, call `@0x468964`; the helo
twin `@0x468688..0x468692`) and call `Entity_SetupGunnerAttachments @0x468100`.
The byte is the items.def attrib token `Parent` (`ItemDef_ParseProperty
@0x49EB00` attrib arm; jo-c kong.c 193609..193611 `stricmp(arg,"Parent")` ->
`particleEffects[720] = 1`, 0x278 + 720 = 0x548). The setup walks
`g_pool_list[1]` in slot order (`@0x468130..0x468173`): every OTHER entity
whose refNum byte `+533` equals this entity's nonzero refNum lands in
`brain+580+8*i` (+4 = the entity, sixteen at most `@0x468173`); with any peer
it saves the current `+0x1C4` update callback into `brain[143]` (`@0x468183`),
installs `Entity_UpdateAttachedChildren @0x45D550` as `+0x1C4` (`@0x468189`)
and writes the count into `brain+576` (`@0x468193`; the word the machine's
no-target idle gate reads, so an attachment carrier never idles); it then
collects up to sixteen model userpoints whose name starts `agun` (`strnicmp`
4, `@0x4681BB..0x4681D9`, 48-byte rows, name +32), transforms each by the
entity's affine fixed-point matrix (the scale variant when anim data or an
item scale is present `@0x468200`, else `Math_BuildFixedPointMatrixFromEulerAngles`
`@0x46823E`; `Math_FixedPointTransformPoint22` `@0x468288`) and assigns them
greedily in child slot order (`@0x4682C1..0x4683AB`): per child the nearest
unconsumed point by `sqrt(dz^2 + dy^2 + dx^2)` in double, clamped at
2147418100.0, truncated and compared unsigned against -1 (`@0x468365`), the
point pointer stored `@0x468389` and zeroed `@0x46838B`.
`Entity_UpdateAttachedChildren` runs the saved callback first (`@0x45D573`),
gates on the count (`@0x45D578`), rebuilds the parent matrix (`@0x45D599` /
`@0x45D5BE` / `@0x45D5D1`) and, per child with a point, writes `+4..+24` = the
parent position and eulers (`@0x45D5FF..0x45D61D`) then `+4/+8/+12` = the
point's local position through the matrix (`@0x45D641..0x45D663`, translation
included) and the six velocity dwords `+152..+172` = the parent's velocityX /
velocityY / slideDecay / modelPtr0..2 unless `+286 <= 0` or `Flags & 6`, then
zero (`@0x45D65B..0x45D6CA`). The vehicle DYING enter kills that list under the
same `+1352` byte (`@0x467B6E..0x467BBB`: `+286 > 0` -> health 0, hit-record
attacker cleared, `deathCallback(child, 1, 0)`). `Entity_ApplyCommand` case
0x28 (AINODEPATH) overwrites `+0x1C4` and so drops the follow, which is retail.

Reimpl (2026-09-12): the runtime half is `VehicleSystem::setup_gunner_attachments`
/ `update_attached_children` (vehicle_lifecycle.cpp; the brain block
`AiBrain::kAttachCount` = +576 / `kAttachSlots` = +580, the point stored as
index+1 into `VehicleTraits::agun_points`, the child as handle+1), run from
`initialize_mission_vehicles` (every pool-1 record resident, before the first
mover tick) and after the mover in `tick_motors` (both air and ground/water
branches; `motor_suspended` and the installed death callback skip the mover
and the follow alike); `h_enter_vehicle_dying` kills the list under
`VehicleTraits::attrib_parent`. Regressions: `vehicle_attachments` (peer walk,
16 cap, greedy assignment, follow, velocity zeroing, brain mirrors, the motor
chain, the dying kill, the `Parent` gate). Fed by retail data since 2026-09-12:
the items.def parser's attrib arm maps the `Parent` token onto
`DefItemDef::attrib_parent` (`def_items.cpp`, `@0x4a0cd6..0x4a0ce2`), the
items.def traits sweep (`mission/item_traits.cpp`) copies it into
`VehicleTraits::attrib_parent`, and the collision resolve
(`mission/collision_resolve.cpp`, beside the `flare_points` fill) fills
`agun_points` from the model's first sixteen `agun*` userpoints (`strnicmp` 4;
`def_parse_item_attrib` + `mission_item_traits` ctests). The addeweap emplacement
children keep the earlier stand-in kill in `h_enter_vehicle_dying` (a
different list; retail kills only the refNum peers) because
`destruction_test::test_vehicle_death_kills_authored_children` pins it; the
`world.cpp` orphan cascade already retires those children on carrier death.

## 27. Recoil, impact, landing and crew death

`vehicle_impulse.cpp` carries mounted-weapon Action values from DEF parsing
to the chassis. A gunner's mounted tank receives the authored recoil at its
posed bounds. Heavy projectile hits use incoming projectile velocity,
the signed wrapped weight threshold and the original two-stage force
assignment. The contact solver preserves that impulse across its first
solve. [orig: Entity_InitVehicleSuspensionGeometry @ 0x474580;
Entity_SetupInfantryForcePoints @ 0x475220] The tank translates under it:
while the +0x3EC direction is nonzero, dir × trunc(+0x3FC × 28.16f)
(`flt_7C6FA0`) joins vel_x, vel_y and slideDecay every tick (Q16, round half
up), after the crash-settle zero and before integration, and the direction
clears once +0x3DC drops; only the tank mover reads the impulse
[orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, @ 0x48A8C0..0x48A9C0]
(ported 2026-09-22, `vehicle_motor::test_tank_impulse_pushes_velocity`).

The bike's head-on test considers wall normals, normalizes the complete
velocity, and uses the -57070 dot threshold. Its severity bands preserve
the original hull damage and crew-kill distinctions. The routine formerly
named Entity_ResetOccupantAnimSlots kills the nine occupant slots by setting
death animation, health and attacker; it does not detach them.
[orig: Entity_ProcessLightVehiclePhysics @ 0x479600;
Entity_ResetOccupantAnimSlots @ 0x463B20]

Landing and crush damage consume the spring depths before contact-state
updates. Aircraft also retain the parked tail's opposite-corner force pair.
The chassis, contact direction, flip request and recovery state are shared
by live movement and wreck settling. `vehicle_suspension`,
`watercraft_client_motor` and `vehicle_motor` exercise these paths.

## 28. Wreck banks and shared movement trails

`death_effects.cpp` owns four retained slots for each Dead/water, Fire and
Other bank. Masks use the original x86 shift behavior; extra matching
user points spawn transient effects after the first four retained handles.
The loaded husk supplies the points. Only the Dead bank falls back to the
origin when no point matches. Full placement rotation and scale position
the effects. Fire slots roll before the water test, steam and retire on
submersion, and restart when they emerge. Respawn releases every bank.
[orig: Entity_InitDeathSounds @ 0x4939B0;
Entity_UpdateDeadWreckEffects @ 0x493140;
Entity_SpawnMaskedEffectBank @ 0x5F7620]
The respawn release's correspondence to `Entity_RespawnVehicle @0x45FF40`'s two
emitter slots (`+0x1CC`/`+0x400`) and two zeroed words is not re-witnessed
(section 18).

The W1/W2/W3/W4 movement effects share a sixteen-point bank, implemented
in `vehicle_trails.cpp`. The first allocation owns an overlapping point's
definition; later lanes update its controls. Zero intensity releases a
masked slot. A retained slot whose control becomes zero is distinct from a
missing point. Camera admission, the quarter-rate/even-tick family cadence,
wet/dry release edges, full placement transforms and the wrapped magnitude
calculation are preserved. The Godot VehicleTrailPresenter follows these
sampled points and retires groups by registry lifetime.
[orig: Entity_UpdateBoneTrailEffects @ 0x4589C0] The ground-family movers
sample their trails only at the end of the contact arms; the airborne and
off-contact arms jump past them [orig: ctan @ 0x48A60D..0x48A67F vs
@ 0x48A684; cveh @ 0x48CD93..0x48CDFD vs @ 0x48CE02; cbik
@ 0x486170..0x4861F1 vs @ 0x4861F6] (ported 2026-09-22,
`vehicle_motor::test_trails_sample_only_in_contact_arms`).

Proof: `destruction`, `watercraft_client_motor`, GUT
`destruction_present_pass_test.gd` and
`vehicle_trail_present_pass_test.gd`.

## 29. Rotor sound, downwash, foliage and surface rings

The historical name `update_vehicle_effect_emissions @ 0x528F20` is misleading:
the function registers helicopter sound loops. Lane 21 reads itemDef
`soundLoopId[2]` (+2100 = Soundloop_3, the `*_DLP` loop) `@0x52919D..0x5291CE`,
lane 11 `soundLoopId[1]` (+2096 = Soundloop_2, `*_ILP`) `@0x5291ED..0x52921D`,
lane 1 `soundLoopId[0]` (+2092 = Soundloop_1) `@0x529235..0x529260`
(`ItemDef.soundLoopId` is `uint32_t[7] @0x82C`, filled from `res[16+i]` by
`ItemDef_ResolveAllResources @0x49E7F0`); each registers with lifetime 15
(`effect_params+16 @0x528F94` → `SoundEmitter_RegisterSetLayers` slot word 21
`@0x528471`; the ground fold's `SoundEmitter_Register @0x5292A6` packs 30).
Retail sndprof.def helicopter profiles author only soundloop_2/soundloop_3
(SP_Apache1: V_APACHE_ILP / V_APACHE_DLP .8 1.2), so lane 1 is normally silent
and Soundloop_4..7 are never consulted. (The PR's "slot 7 / 6 / 5" mapping was
wrong — corrected 2026-09-08.) Medium and cruise fade/pitch curves use the twelve
SndProf words; the lateral loop uses climb intensity and rotor pitch. The
medium scratch value is reused by a degenerate cruise interval. The float
reciprocal and ftol chop preserve the one-unit endpoint loss; negative lateral
volume wraps through the unsigned word. Empty helicopters refresh these loops
while their rotors spin down. [orig: update_vehicle_effect_emissions @ 0x528F20;
interpolate_value_in_range @ 0x527EA0;
Entity_UpdateHeloRotorSpin @ 0x48FA70]

`rotor_wash.cpp` owns the bounded focal-wind pool. Rotor-axis rays select
the authored RwDust/Grass/Snow/Sand/Water particle family. Stationary dry
wash starts without dust; movement establishes the intensity that later
decays. Particle focal-wind and repulsion flags read the same field at the
original phase/index cadence. The C random stream (`PRNG_Next16_C @0x6131B0`)
also supplies the occlusion wind sampler; it is a PRESENTATION-ONLY stream —
every draw sits behind the listener / 0x2200000 camera-distance gate
`@0x5CB1CD` or a render gate — so `World::prng16_c_state` is never
authoritative and must not enter any peer comparison.
[orig: terrain_overlay_alloc @ 0x5CAF40;
sub_5CB020 @ 0x5CB020; WeatherParticle_UpdateAllEmitters @ 0x5CB100
(the function spans 0x5CB100..0x5CB5B0; the PR's 0x5CB220 was mid-body);
WindZone_ApplyVortexForce @ 0x5CB8A0;
Terrain_CalcSectorRepulsionForce @ 0x5CBF50;
Terrain_FindNearestAmbientSoundZone @ 0x5CBCD0]

Surface-effect groups (re-grilled and fixed 2026-09-08): each focal-wind slot
keeps ONE surface-effect group — slot dwords +3 = the group instance, +4 = the
effect id `@0x5CB407..0x5CB46E`. The group is released (`sub_5F6C70 @0x5F6C70` →
`sub_5E5ED0` notifies each top-level child dead) and re-created
(`CEffectWorld_SpawnEmitterAtPosition @0x5F6DF0`, dest {type 4, id, hit}) only
when the effect id changes; EVERY hit then calls `sub_5F6C10 @0x5F6C10` →
`CEffectWorld_SpawnAllActiveChildren @0x5E5E70`, which invokes vtable slot 6 =
`CParticleEmitter_SpawnNewParticle @0x5F35B0` (position = hit, direction
(0,1,0), 0.0, 0, parent −1, 0) on every child whose nested-child word +264 is
zero; `dword_29D6BB0` (the spawn window) is set `@0x5CB3A6` and cleared
`@0x5CB4A0` around the create and the trigger. The slot never releases its group
when the zone clears (`Terrain_ClearShadowTileSlot @0x5CB0D0` only writes the
owner tag, and it is called from `Entity_InitDeathState @0x48F9F8` and
`Entity_Destroy @0x43E984` only — `Entity_RespawnVehicle @0x45FF40` does not
call it); the group dies with its last child (`CEffectWorld_UpdateAndReapGroups
@0x5EC920` → `CEffectGroup_AdvanceChildrenAndReap @0x5E59A0` →
`CEffectWorld_RemoveAndFreeGroup @0x5E9200`, which memsets and returns the
120-byte slot to the 1024-entry pool `@0x2C077C8`), after which the slot's stale
instance pointer makes the trigger a no-op until the surface effect changes.
Reimpl: `rotor_wash.cpp` emits `EnsureZoneGroup` / `TriggerZoneGroup` /
`ReleaseZoneGroup` `VehicleEffectEvent` kinds (the PR's per-hit spawn is gone),
`vehicle_trail_presenter.cpp` executes them and
`particle::EffectScene::trigger_group_children` is the re-trigger; the reimpl
releases at death init (`destruction.cpp entity_init_aircraft_death`), at the
rotor tick's dead/gone-owner sweep, and still on respawn. Deliberately NOT
ported: retail's stale group pointer aliasing a REUSED pool slot (a later hit
would re-trigger whatever group now occupies the slot;
`allocate_effect_emitter_slot @0x5E46B0` reuses round-robin) — a lifetime
hazard, not behavior; the reimpl treats a reaped group as the freed-slot no-op.

Every spawned particle binds its force zone at spawn:
`CParticleEmitter_SpawnNewParticle @0x5F37C6..0x5F37D8` stores
`Terrain_FindNearestAmbientSoundZone(particle+24)` into particle+12; that
function answers `dword_29D6BB0` when the window is open (`@0x5CBCD3`), else the
nearest containing zone. Reimpl: `emitter.cpp emit_one_internal` (window, else
`forces->zone_at`), `RotorWashSystem::zone_at`.

Sway renderers stay individually placed. Their five position-seeded waves
bend the second part about its authored pivot and add the radial wind offset.
The device composes that delta with a clean part pose each frame, preserving
PANM and avoiding accumulated transforms. [orig: find_nearest_force_zone
@ 0x5CB5B0; BoneCallback_Sway_World @ 0x4E2B10]

Every eighth tick above a nonzero water plane, downwash can allocate a
two-unit-grid surface ring. The 128-slot bank fades in over twelve ticks,
fades out through tick 31 and retires at 32. The portable renderer compiles
nine radial rows and nineteen angular columns, the camera-distance height
bias and both UV sets. The device uses the authored wake5/wakegrad textures.
The ring opacity divides by the slot's EXTENT: `fidiv dword ptr [esi-4]`
`@0x5CB532` (bytes DA 76 FC) is a 4-BYTE displacement from `_ESI` = &slot
dword 31, i.e. slot dword 30, which `terrain_overlay_alloc @0x5CAFC4` stamps
with the extent; the ray radius's `*(_ESI - 4)` `@0x5CB265` is the DWORD index
−4 = slot dword 27 = the inner radius (the review's "divide by inner" claim was
refuted 2026-09-08). Bank name map: `sub_5DDC60 @0x5DDC60` allocation,
`CWeatherSlot_Init @0x5DDD80` (IDB misnomer: one surface-ring row init),
`sub_5DDDB0 @0x5DDDB0` row removal, `sub_5DDE10 @0x5DDE10` per-tick fade.
[orig: sub_6108E0 @ 0x6108E0; sub_56BD20 @ 0x56BD20;
sub_5DDC60 @ 0x5DDC60; CWeatherSlot_Init @ 0x5DDD80; sub_5DDDB0 @ 0x5DDDB0;
sub_5DDE10 @ 0x5DDE10; create_water_surface_mesh @ 0x5DDEF0;
render_water_surface_decal @ 0x5DE0F0; sub_5DDC90 @ 0x5DDC90]

`vehicle_part_anim` pins rotor sounds, focal forces, lifetime ownership,
material selection, foliage waves and ring geometry. Device coverage lives
in `vehicle_trail_present_pass_test.gd`.

The render side (corrected 2026-09-08): `create_water_surface_mesh @0x5DDEF0`
builds the 19×9 ring geometry; `render_water_surface_decal @0x5DE0F0` owns the
draw state — the 0.4 ambient material `@0x5DE202..0x5DE217`, `SetMaterial`
`@0x5DE232`, `D3DRS_AMBIENT` (139) white `@0x5DE1EC`, `GfxShader_ApplyPassChecked`
pass 0x100000 `@0x5DE245`, and the first-UV scroll `(dword_24C1948 & 0x1FF) / 512`
and `(dword_24C1948 & 0x3FF) × −0.01171875` `@0x5DE277..0x5DE2AD`; `sub_5DDC90
@0x5DDC90` only loads wake5.tga / wakegrad.tga and sets sampler addressing. The
Godot shader implements that two-texture, unshaded draw through the compiled
mesh (`provenance.json` cites `render_water_surface_decal` / `sub_5DDC90`).
Documented stand-in, no ledger row: the UV scroll counter `dword_24C1948` is the
RENDER frame counter, but the portable compile runs once per fixed tick from
`Simulation::fill_water_wake_frame`, so the logic tick stands in
(`water_wake_frame.h/.cpp`).

## 30. Amphibious mover selection

The catv dispatcher reads the previous tick's afloat flag. An afloat vehicle
runs the water mover, including its input, sound and trail branches; a beached
vehicle returns to the ground mover. The platform draft uses half the pad
radius for catv instead of the boat's 0.9-unit adjustment. Authority and
prediction use the same dispatch. `watercraft_client_motor` pins both role
paths and the absence of ground drowning while afloat.
[orig: Entity_DispatchPhysicsUpdate @ 0x48F010 (the IDB name — the PR's
`Entity_DispatchPhysics_catv` alias does not exist in the IDB; Flags 0x8000
selects the watercraft mover);
Entity_ProcessPlatformPhysics @ 0x481870]

## 31. Selector-zero craft motors and suspension

The installed item audit finds two drivable selector-zero craft: the LCAC
(catv) and Indonesian landing craft (cbot). Zero selects a different motor.
The class thunks route to Entity_ProcessInfantryPhysics @0x46E100 on land
and Entity_ProcessAirVehiclePhysics @0x46FA00 on water; the curated IDB names
are misleading. Both use Entity_ProcessVehicleSuspension @0x463C60.
The direct helicopter and plane callbacks ignore this selector.

vehicle_simple_motor.cpp preserves the two cores: ground cos-squared
slope speed, unclamped opposite-sign reversal, the duplicated-X slip-length
test, wheel phase, ground attitude impulses and gravity -167; boat
horizontal thrust, signed longitudinal speed, 1/32 keel damping, three
authored water waves, size-dependent bob attenuation and post-contact lean.
Ground capsizing drains 20 HP per authority tick; the boat drains 200.
The simple ground health leg emits its warning without the physical
ground mover's smoke/fire bank. Boat damage effects use the water-clamped
emitter position. Both retain input reconciliation, carrier follow, staged
turret commit, light/direction sound edges and their separate trail cadences.

The slip-length duplicate is confirmed in instructions 46F399..46F3B5:
EAX is stored in both x87 input locals. Water wave constants are read
directly from 7C6950 and 7C6F20..58. The wave (re-traced from the x87 sequence
2026-09-08, `Entity_ProcessAirVehiclePhysics @0x46FA00 @0x47115D..0x4711D9`,
radius shifts `@0x4711DD..0x4711FC`): P = fild(((x+y)>>10) + tick·4) ×
`flt_7C6950` (1/256); pitch = ftol(cos(1.4P)·327680 + cos(P)·655360);
roll = ftol(sin(0.8P)·524288 + sin(1.2P)·655360) with NO 1.6 factor — the
`fmul dbl_7C6F28` `@0x4711BD` belongs to the HEIGHT phase: height = −1024 −
ftol(sin(1.6P) × −1024.0); constants `dbl_7C6F20..7C6F58` = −1024.0, 1.6, 1.2,
524288.0, 0.8, 655360.0, 327680.0, 1.4. (The PR's "1.6 multiplies the roll sum"
reading was wrong; `vehicle_motor` pins P = 0.5 → 825755 / 574211 / −145 and
P = 0.375 → 893366 / 439996 / −223.) A reverse boat does not enter the
positive-only lean arm. The submerged-driver cut (`CameraOffset.z + Z <=
Env_WaterHeightFixed` → the AI leg) exists on the ground selector-zero mover too
(`Entity_ProcessInfantryPhysics @0x46E100 @0x46EA2E..0x46EA3A`) and on cveh
(`@0x48B9A0..0x48B9AC`), not only the boat (`@0x48DFD3..0x48DFDF`) — the
`!boat ||` guard was dropped 2026-09-08. The selector-zero boat's PlayerControl
block sits at the HEAD of `@0x46FA00`, before the authority split `@0x470109`:
`test byte [itemDef+54h],40h` `@0x47004B`, the claimant start/stop edge
(`+0x170` claimant `@0x470055`, the `brain+0x318` bit-0 latch
`@0x470063..0x47006F`, slot 30 on the claimant eye above `Env_WaterHeightFixed`
`@0x470075..0x4700EB`, the all-zero movement fold plus slot 31 on the hull
`+0x18000` when the claimant leaves `@0x4700AB..0x4700EB`), then
`Entity_UpdatePartSpinAccumulator @0x4700F5` — the GROUND machine, selected by
the brain's profile type (JOX `Drivable LCAC` 101296 and `Drivable Indo Landing
Craft` 101200: cbot rows with no physics key, PlayerControl, `d_lcac.aip`
`type GROUND`; DLCAC1.3di consumes HELO_TAILROTOR, the fan). The full
watercraft mover `@0x48D480` has no such call (`xrefs_to 0x4928B0`: `@0x46F99E`,
`@0x4700F5`, `@0x4869EA`, `@0x4889F5`, `@0x48AE3D`, `@0x48D42B`). Ported
2026-09-12 (the former D-NET-161 (d)): `tick_simple_motor`'s boat form runs
`update_claimant_engine_sound` + `rotor_machine_tick` after its controller
resolve for the authority and the prediction call; the machine exclusion keys
on the physics-keyed full mover, not the family; the lights leg stays in the
tail (`vehicle_part_anim::test_selector_zero_boat_runs_the_ground_machine`,
`vehicle_motor::test_selector_zero_sound_tails`).

Sound tails (witnessed and ported 2026-09-08;
`vehicle_motor::test_selector_zero_sound_tails`): the ground twin's tail
`@0x46F7C8..0x46F9A6` is cveh's — gate `dword_24E0E80` `@0x46F7C8`, the fold
(max playerSpeed +0x8E8 → brain +0xC4 → command +0x220, isColliding =
`+0x3D4 > 30`) `@0x46F7D4..0x46F825`, direction latch `@0x46F828..0x46F888`,
high-rev (slot 33) `@0x46F88B..0x46F8C0`, lights (slot 24, bit 4)
`@0x46F8C3..0x46F8FC`, claimant edge `@0x46F8FC..0x46F99C` (bit 1; slot 30 when
occupant `+0xC+0x74` > water; all-zero fold then slot 31 when hull
`+0xC+0x18000` > water), part spin `@0x46F99E`, timer `@0x46F9A6` — ported
through `update_ground_sound` / `update_engine_sound`. The boat twin's tail
`@0x4714EA..0x47166F` is different: lights (slot 24) BEFORE the fold
`@0x4714EA..0x471523`, gate `@0x471523`, fold gate (`!player_control ||
occupant`) `@0x471533..0x471546`, the speed substitute
`ftol(min(sqrt(vx²+vy²+vz²), flt_7C19E0))` when `+0x29C == 0`
`@0x47154C..0x4715A5`, maximum waterSpeed +0x8EC → brain +0xC4 → command +0x220
with NO fold when all zero `@0x4715AA..0x4715C6`, fold (isColliding 0)
`@0x4715D2`, direction latch `@0x4715DA..0x471667` — NO high-rev, NO slot 30/31
claimant edge, NO part spin, NO timer. The reverse-shift (slot 32) `PlaySound`
sites are `@0x46F883` (ground) and `@0x471621` (boat) — the old D-SND-17 tail's
"aircraft slot-30 start" reading of those two addresses was wrong. The
`dword_24E0E80` gate is the outer loop's last-tick-of-batch flag
(`Game_MainLoop` `@0x52BA24..0x52BA3A`), carried as
`World::rules.last_tick_of_batch` and applied at both twins' folds and the
ground twin's inline high-rev (`@0x46F7C8..0x46F7CE`) since 2026-09-22
(D-SND-17 closed).

vehicle_simple_contact.cpp preserves the crossed four-pad order plus
three upper probes, the second pass's old-depth-plus-half-old/new sum,
mass sharing, strongest-probe yaw impulse, half-radius water hysteresis,
four shock/spring slots and independent roll/pitch lift fits. The pitch
fit's lesser-front arm multiplies front depth itself, rather than the
front/back difference. Its sleep window is -500 < slide < 0 and has no
occupant or spring-energy gate. The contact yaw impulse uses the negative
atan constant at 7C57B8.

Native regressions cover selector admission, numeric acceleration and
buoyancy, the X-only slip edge, authority/client agreement, amphibian wet
dispatch, sleep/contact wake and the water-supported spring bank. The
existing client-dispatch control now expects selector-zero ground motion.

## 32. Local water-entry effects

All vehicle contact families now publish the local emitter as well as the
positioned sound event. Resource-table rows 849188/849298 bind
Effect_sboatwake and Effect_SmlSplash; the vehicle's airborne flag
selects the latter. The emitter uses the lowest pad at the water plane
with direction (0, 0, -0.5). The corresponding sound names are BODYWATER1
and SURFACE_WTR. The transition gate prevents repeated entry effects while
the hull remains submerged.

Witnesses: Entity_ProcessVehicleSuspension @0x463C60,
Entity_ProcessTrackedVehiclePhysics @0x47C1C0,
Entity_ProcessPlatformPhysics @0x481870, and
Server_SendOverlayActionToAlive @0x50A1B0. The portable
vehicle_water_entry producer feeds the existing vehicle effect presenter
and the network water-crossing queue.

## 33. Falling-wreck impact support and sound

Entity_UpdateFallingDeathPhysics @0x493F70 queries terrain and posed model
support through Entity_RaycastGroundHeightAndObject before advancing the
wreck. The final hull bounds offset that height according to its up vector.
Landing on another entity suppresses the terrain scorch; the routed callback
still commits its integrated pose after the ground-death transition.

Water crossing uses sound-profile slot 39 (+156) at the old entity pose when
available, otherwise IMP_DEBLRG_WATER at the crossing point. Landing uses
slot 35 (+140), otherwise IMP_VCL_DROP. The fixed-height fallback remains for
embedders without a collision world. The destruction regression verifies the
authored water-sound override separately from the positioned splash effect.

## 34. Definition replacement and derived vehicle traits

MissionKernel invalidates the derived vehicle table when its item definitions
are installed, then refreshes model-derived probes and trail anchors from the
retained model cache after resolution. This keeps selector, motor tuning,
sound and effects consistent with the current table, including editor
revisions. Admitting selector-zero rows exposed the old insert-only cache:
a subsequent full-physics table could leave a craft on the earlier simple
motor. The real-UDP coop_two_sim regression catches that stale setting while
also checking the authoritative pose and mounted prediction order.

## Divergence catalog

| ID | Ours | Original | Why / consequence |
|---|---|---|---|
| D-VEH-2 | Clear the vacated tail when compacting the water-ring bank | `sub_5DDDB0 @ 0x5DDDB0` zeroes the removed slot and shifts the suffix (`@0x5DDDCB..0x5DDDFC`) without ever clearing slot 127; `sub_5DDE10 @ 0x5DDE10` steps its cursor back onto the removed index (`@0x5DDEAD..0x5DDEC0`) | PERMANENT — proposed in PR #640 (2026-09-07), requires maintainer ratification at merge (no sign-off recorded yet; [ADR 0022](../adr/0022-divergence-burn-down.md#permanent-register) original-bug class): a completely full bank otherwise re-copies and re-expires the duplicated final row forever. The saturation regression fills all 128 slots and proves retirement terminates. Normal non-full ring behavior is unchanged. |
| D-VEH-3 | Player steering reads the full BAM heading; analog input clears inactive digital direction; prior brake state and the retained +0x3C8 direction select the original ground/bike branches. | Ground @ 0x48B847..0x48C095 and bike @ 0x48496D..0x48526F; section 39. | FIXED 2026-09-18 in PR #652. 640 original-instruction command-state vectors cover heading precision, analog cancellation, stale directions and brake transitions. |
| D-VEH-4 | Pointer row: owned by the [tank record](tank-parity-re.md#divergence-catalog). Tank corner support now uses merged wheel/belly contact, clears the sinks on a supported diagonal and keeps each corner's drop in the airborne fit. | `Entity_ProcessWheeledVehiclePhysics @ 0x475DE0`: merge @ 0x4784CC, growth @ 0x478510, catch-up @ 0x478DA4, diagonal @ 0x478FF2..0x479040, tail @ 0x47931B..0x47934C, fit call @ 0x478B1C | Minted-and-closed 2026-09-22 (FIXED) in PR #671; `vehicle_followups`. |
| D-VEH-5 | Pointer row: owned by the [tank record](tank-parity-re.md#divergence-catalog). The Godot gameplay and Inset cameras stamp a basis built from the composed angles instead of `look_at(eye + forward)`. | Retail builds the view from the euler triple: `Viewport_BuildProjectionMatrix @ 0x410FB0`, rotations @ 0x4112A4..0x4112EB | Minted-and-closed 2026-09-22 (FIXED) in PR #671; a presentation precision fix for every far-from-origin view, filed here where it was found. GUT `local_player_presenter_test.gd`, `game_hud_presenter_declutter_test.gd`. |

## 35. Ground and boat pedal view turn

The non-keyboard input arm chooses the larger-magnitude Y/Z steering axis,
subtracts its scaled BAM delta from the steering target, and, while freelook
is clear, applies the same delta to the driver and the local look alias
(`Entity_UpdateVehiclePhysics @0x48B7AA..0x48B7D9`; boat
`Entity_UpdateWatercraftPhysics @0x48E08D..0x48E0BC`). The earlier freelook
merge tests the signed sum of all three axes, so opposing axes can cancel
without eliminating this view turn. Ground, boat and aircraft share the
full-precision driver-view write; the subsequent target reset reads the
updated heading. Ground yaw-rate updates also retain the crash/settle gates
(`@0x48BA17..0x48BA60`), whose bytes are now modeled.

## 36. Boarding, USE scans and authored AI state

PR #640 completes the mount-chain pass against `Entity_FindNearestSeatOrArmory
@ 0x435D50`, `Vehicle_HasEnemyOccupant @ 0x4359F0`,
`Entity_ToggleVehicleMount @ 0x436950`, `Entity_CanEnterVehicle @ 0x435480`
and `Entity_UpdateInfantryAI @ 0x4B9910`.

USE scores from the live three-axis eye offset and casts from Position to
seat + 12288 Z. Its sixth LOS argument admits every collision type; sound
queries retain the building filter. Carried EWeap occupancy is checked at
the root vehicle, and a non-PlayerControl EWeap ray uses its vehicle parent
as the endpoint. The walker never tests the endpoint's own hull: its exclusion
set is the query entity, the endpoint and both parent slots
(`raycast_find_collision_entity @0x539ab8..0x539b10` -> ctx[17..20], skipped
in `raycast_against_entity_pool @0x538832..0x538859`), and the 0x8000000 skip
applies to sound rays only (the walker's includeFlagged is the caller's
allTypes, `@0x539ba4 -> @0x5387ac`). What keeps a mounted USE from cycling
seats is the scan's second gate, not any hull: the two caps `maxDistance =
0x3FFFFFC0` / mounted `0x38E38E0` (`@0x435d90` / `@0x435d9a`) are BAM32 aim
radii, not distances. Per candidate point the block ftol's the horizontal and
3D reaches from the eye (`@0x436047..0x43608a`, through the `flt_7C19E0 =
0x4EFFFE00` clamp), takes `atan2(dy, dx) * 2^32/2pi - Yaw` and `atan2(dz,
horiz) * 2^32/2pi - Pitch` (`@0x43608f..0x4360c6`, `dbl_7C19D8`), clamps each
delta to +100 deg (`0x471C7180`, positive side only, `@0x4360c9..0x4360de`),
and forms the aim magnitude `sqrt(yaw_d^2 + pitch_d^2)` (`@0x4360e2..0x436103`).
Score is `dist3d + (aim >> 9)` (`@0x43610c..0x436111`); the gates are
`dist3d <= 0x40000` (4.0 u, `@0x436113`) and `aim <= maxDistance`
(`@0x43611f..0x436123`): just under 90 deg for a standing player, 5.0 deg for
a seated one. A rider therefore swaps only onto a free seat he is looking at,
and USE from a Drivable Dune Buggy driver looking ahead finds nothing and
exits (`Entity_ToggleVehicleMount @0x4369ac..0x4369c7`); the earlier port read
both caps as 3D distances (16384 u / 910 u) and the horizontal reach as the
4.0 u gate, which is why it needed an own-hull stand-in to exit. Corrected
2026-09-08: `vehicle_attach.cpp` (the view frame is the body's BAM32
heading/pitch, the mission-degree mirror for a bare entity), `collision_los.cpp`;
ctests `vehicle_mount`, `buggy_01tr`; GUT `vehicle_emplacement_alignment_test`
(the teleport looks down at the ctrlx point). Label rays retain their separate
sector-query endpoint exclusions; the label list's own 4.0 u radius has no
cone, only its nearest-scan bracket does.

`Entity_TryEnterNearestVehicle @0x4368C0` has exactly two arms, keyed on
`Flags & 0x200` alone (`@0x4368CF`): with the bit set,
`Entity_FindBestSeatSlot(player, groundEntity)` (`@0x4368E0`) and, with no
target, `return 0` (`@0x436903`) — no scan; with it clear,
`Entity_FindNearestSeatOrArmory` (`@0x43691A`). Either hit feeds
`Entity_RequestVehicleAttach` (`@0x4368EF`, `return 1` `@0x436907`). The bit is
the queued Co-op spawn-marker mount, not a deck latch: its one writer is
`Server_PositionPlayerForSpawn @0x50D44D` (the no-pick team-2 marker arm
`@0x50D442`, together with `+364`/`+384` = the marker's parent
`@0x50D454`/`@0x50D45A`), its one consumer `Entity_UpdateInfantryPlayerBody
@0x4B424A..0x4B4272` (restore `+0x28` from `+0x180` when null, toggle with the
bit still set, then clear it). Standing on a truck bed therefore boards the
looked-at seat through the scan, never the priority seat. Corrected 2026-09-12
(`vehicle_attach.cpp` `player_toggle_mount` / `find_mount_toggle_candidate`,
`kEntityFlagQueuedMount`; `vehicle_mount::test_toggle_deck_best_seat`): the
earlier port keyed the FindBestSeatSlot arm on "the groundEntity has seats" and
fell through to the scan, which handed a deck stander the driver seat and let a
queued mount on a full carrier board a neighbour.

The writable WAC `seatbelt` value at `0xC6EADC` blocks mounted local-player
USE in both local and joiner request paths. Forced script detach and numbered
seat commands preserve their separate paths. `event_runtime_bms` compiles a
mixed-case assignment, reads it back, verifies the lock and forced detach,
then releases it through WAC.

Board admission preserves the spawn-anchor and rider-on-deck arms, then the
not-airborne / stationary check: X and Y may differ from saved live pose by
at most **16 raw Q16 counts**, not 16 world units. A dead target leaves a
walker at spawn waiting; a walker more than eight units from spawn inherits
the target's attacker and dies. The 64-tick command-125 upgrade compares seat
types and selects through carried children.

On target resolution, the E1 through E8 claim scan assigns the lowest unused entry
index. PlayerControl targets approach the live selected seat. Other entry
targets use UseGun or the authored E/G/S/H sequence, with the original radii,
heading alignment, one-eighth entry chase and hull detour. The collision
resolver's signed push and facing gates (`@ 0x4B377A..0x4B37BB`) widen the
boarding radius to bound + one unit; terrain-gradient pressure retains its
separate latch producer. This replaces the earlier stalled-walk heuristic.
`vehicle_mount`, `collision`, `mission_promote`, `defense_00trg` and
`mission_ai_path_conformance` exercise these paths. The buggy regression
waits for its selected gun's equip action to permit USE, then checks
hull-based exit and the gun following its driven carrier.

Promotion leaves current, pending and fallback state at 0 for every vehicle
brain (`Entity_InitVehicleAI @ 0x460200`: the memset's zero is read back
`@ 0x46024b` and stored `@ 0x46028b..0x460291`; `profile+0x18` has no reader),
and the first mover tick promotes 0 → PRETTY (22 / 14). The native profile
regression authors `default_state HELO_LAND` to prove the profile state is NOT
consumed (corrected 2026-09-10; the earlier "consumes the parsed profile's
initial state" reading was wrong). The same promotion seeds the @ 0x460200
constant block through `initialize_vehicle_brain`, shared with the dynamic
teammate spawn. ORDER MATTERS: retail promotes 0 -> 22/14 at the mover HEAD
(`@ 0x48afac..0x48afb2`, `@ 0x490377..0x49037d`) BEFORE the occupant/AI-driver
block (`@ 0x48b949`) whose 22 -> 16 hand-back (`@ 0x48bc16`) lets the state
machine run row 16 and zero the out-speed; our authority pass stages the AI
drive ahead of the motor, so `VehicleSystem` hoists the promotion to the pass
head (the in-motor stamp stays for direct motor callers). The old profile seed
(row 17 GROUND_COMBAT for every GROUND_FOLLOWWP profile, because
`AIState_LookupByName` returns 17 for that name) had hidden the inversion: with a
faithful 0 seed and the promotion inside the motor, a boarded AI driver drove
at combat speed (00TRa's DTruck2 1714 drove off its spot). The earlier claim that a boarding pulse was faithful is corrected: the pool
callback never seeds `[128] = [49]` through `AI_BeginUpdate`. Row 22 retains the
existing working speed, so a fresh brain's zero stays zero through the mover's
hand-back. `vehicle_motor` now requires the fresh routeless hull to stay exactly
still from its first tick. [orig: Entity_UpdatePool1Slot @ 0x4B8DD0
(callback @ 0x4B8E3C, motor @ 0x4B8E53); D-AI-14 in the world record.]
The former
`AiEntity::arrival_prox` residual is resolved (2026-09-22): the patrol arrival
proximity is the def's +0x924 turn-rate word (`VehicleTraits::turn_rate`), read
live against brain[132] [orig: AI_UpdatePatrolBehavior, `@0x457DCB..0x457DD4`],
which the ground/tank mover now mirrors back each motor pass (§10 item 14), and
`h_vehicle_dying_tick` zeroes the mover's command speed with brain[136]
[orig: AI_TickState_VehicleDying, `mov [esi+220h], ebx` @0x467D83]. Residual
noted, not
ledgered: `AiEntity::has_physics`
(the entity+368 stand-in) defaults true for promoted vehicles while retail's +368
is null until a driver boards.

D-INF-2 scope after the 2026-09-08 review: the `+0x369` path-state byte is
modeled as nonzero-ness only (`board_blocked`) — producers `@0x4BA94E` and
`@0x4B37BB` and the ring reader `@0x4BB325` are ported, the arrival writes a
frame local (`var_1169 @0x4BBD8F`), not `+0x369`, and the 1/2/3 progression's
search → 2 `@0x4afea8` / clear-within-1 u `@0x4aff06` live in the unported
cover/path consumer `ai_find_cover_position @0x4afab0` / `CAIPath_FindPath
@0x409580`; the byte's other clears are in `Entity_UpdateInfantryAI` itself — the
combat aim solution `@0x4BCFDB` (ported 2026-09-09, `infantry_combat.cpp`),
`@0x4BD2E9`, `@0x4BD349`, `@0x4BD956`. The S-point leg `@0x4BB7B2..0x4BB88F`: the guard clip is gated
on `[[entity+0x188]+0x48][0x8C] != [0]` (= `has_clip(kGuard)`) — ported; its side
writes `attachParent (+0x184) = self @0x4BB840` and the S position into
`+0x2FC..+0x304` `@0x4BB846..0x4BB852` (then `+0x184 = 0 @0x4BB885`) are modeled
(2026-09-09): the reader is the self-attachment chase `@0x4BF625..0x4BF664` (gate
`+0x184 == self @0x4BF625..0x4BF62D`; eighth-step X/Y from `+0x2FC/+0x300`
`@0x4BF636/0x4BF63C`; Z = max(Z, `+0x304`) `@0x4BF653..0x4BF664`), ported in
`infantry_attachment_move`. Field split: `+0x2FC/+0x300` has one writer (the S
stamp) and one reader (the chase) → `InfantryState::self_attach_point`;
`+0x304` is the goal Z written by the stamp `@0x4BB852` and every moving
selection `@0x4BD3F7` (seeded `@0x4BFE07`, player `@0x4B709D`) →
`move_target[2]`; retail's goal X/Y are per-think frame locals. The live can't-enter arm (goal = self, radius
0x7D0000 `@0x4BB2CE..0x4BB2EC`; arrival stage++ `@0x4BBD9E`, attach skipped by
the 0x640000 gate `@0x4BBDAF`) is ported, and the arrival no longer clears
`board_blocked` (retail cleared the frame local). Driver lean 107..110 is
ported in `ai_detail.h mounted_anim_state_for_seat` (writes `@0x4bee46` 110,
`@0x4bee59` 109, `@0x4bee6b` 108 = carrier `+0x29C < 0`, `@0x4bee87` 107 =
abs(`+0x29C`) < 0xC8); the roll compare constants for 109/110 (section 21's
±71582784) were not re-witnessed this pass.

### 37. Retained contact geometry across shell asset binding

The windowed 02TR soak caught the selector-zero LCAC falling through the map:
rebinding the same items table for ADM/collision had cleared its motor traits.
Trait invalidation now belongs to the explicit definition sweep, which restores
model-derived probes afterward; read-side source binding preserves them. Boot
also arms the retained collision sweep through the same kernel entry. The
`watercraft_02tr` regression repeats the shell binding order with authored Zodiac,
LCAC and Mark V hulls, then checks ten seconds of idle support without terrain.
The simple suspension's dispatcher-enabled water support also accepts a plane
at zero, matching `Entity_ProcessVehicleSuspension @0x463C60`'s event gate.
The runtime drive probe records water height and horizontal travel, and rejects
large unsupported vertical movement for ground/water families.

The corrected windowed LCAC run travels 47.14 horizontal units, turns and brakes;
its height remains near the authored 13.5-unit water plane and rising shore.


### 38. Crash height, bike axle and wheelie follow-ups from #645

The contact solvers retain a maximum penetration **before** the spring loop,
initialized to -1 and populated while crashed, inverted or settling. A
crashed hull uses that depth even when its up axis is positive. The ground
family still accepts fitted Z while upright and settling, without the
ordinary upward-velocity clamp; aircraft and bikes use penetration in their
settled arms. Tanks distinguish a successful orientation fit from a retained
chassis frame: the latter uses maximum wheel penetration. Airborne aircraft
and bikes also retain their crashed/settled body-contact Z adjustments.
[orig: Entity_ProcessTrackedVehiclePhysics @ 0x47C1C0;
Entity_ProcessWheeledVehiclePhysics @ 0x475DE0;
Entity_ProcessAircraftContactPhysics @ 0x47EF10;
Entity_ProcessLightVehiclePhysics @ 0x479600]

The bike axle solver normalizes the front-minus-rear corner vector, retains
the old **side** axis, derives up by forward × side, and rebuilds forward by
side × up. The square bounding quad uses the height-derived wheel radius to
inset its length. The old implementation confused the retained side axis with
up and could rotate an otherwise unchanged frame.
[orig: Entity_UpdateVehicleChassisOrientation @ 0x468A50;
Entity_ComputeBoundingQuad @ 0x45B6E0; Math_FixedPointCrossProduct @ 0x6134A0]

Bike +0x3DD is the per-tick wheelie request; +0x3DE retains the active mode.
Flags 0x20 and speed >4096 set both. While requested and active below speed
16384, acceleration grows by 25 instead of using the ordinary servo.
Airborne/requested/active bikes skip the eight-way key-heading adjustment.
The brake requires contact timer +0x2F4 >0 and an inactive wheelie, independent
of the definition's hand-brake field. The upward cap 16384 applies when
inactive or airborne, before gravity 250; it does not cap a supported wheelie.
[orig: Entity_UpdateLightVehiclePhysics @ 0x483FE0]

The request applies upward rate-50 force records to square-quad corners 0 and
3 and suppresses the two wheel catch-up lifts while preserving landing
bookkeeping. Releasing the request clears active mode once both wheels have
contact and the retained contact timer reaches ten. The fallen-frame helper
and the accepted brake-velocity arm also clear active mode. Tail processing
clears only the request.
[orig: Entity_ProcessLightVehiclePhysics @ 0x479600;
Entity_UpdateVehicleChassisOrientation @ 0x468A50;
Entity_UpdateLightVehiclePhysics @ 0x4859E0]

The +0x3E0 launch vector normalizes displacement from **saved live pose**,
replaces X/Y with the pre-solve forward axis, then normalizes again. Active
supported travel consumes all three components; the not-crashed off-contact
arm writes X/Y and retains vertical velocity. This closes D-NET-161 (a).
[orig: Entity_ProcessLightVehiclePhysics @ 0x47BF00..0x47C03B;
Entity_UpdateLightVehiclePhysics @ 0x486052..0x48657E]

Validation: native `vehicle_followups`, `vehicle_motor`, and
`vehicle_suspension` pass. Regressions cover request/active lifetime,
acceleration, brake eligibility, heading gates, vertical cap, catch-up,
release at ten contacts, banked axle fitting, saved-pose launch capture, and
upright crashed penetration across all four contact families. No IDB changes
were made.


Witness-address corrections from the PR #645 follow-up review (2026-09-11):
0x478D06 is inside the conditional jump beginning at 0x478D05, not a Z-add
instruction; the retained-depth comparison begins at 0x478D15. Likewise,
0x468CE1 is inside the contact comparison at 0x468CDF, and the old
0x468BC1..0x468CE1 “cross/normalize” label actually covers fallen-bike
contact and suspension handling. The 0x468BC1 witness now sits at that contact
gate. The 0x48524C store arms wheelie-active (+0x3DE), alongside the request
byte at 0x485245. The citation census retires the two mid-instruction addresses
0x478D06 and 0x468CE1; no translated behavior was deleted.

Follow-up (2026-09-12, PR #646 review): the wheeled crash-depth pairing above
was itself wrong. 0x477FF4..0x478014 is argument marshalling for the
`Entity_ComputeBoundingQuad` call @ 0x47801C (both endpoints mid-instruction)
and 0x478D15 is the `cmp eax, -1` of the FOUR-wheel-record max
(@ 0x478D0E..0x478D4D, seeded -1 @ 0x478B69/@ 0x478B72) that only the
failed-fit arms add to Position.Z (@ 0x479075..0x479079 /
@ 0x4791D9..0x4791DD). The gated 13-record crash max is seeded @ 0x478391,
gated on `+2EC || up.z < 0 || +2F0` @ 0x4783BA..0x4783D1, scanned
@ 0x4783D5..0x4783EC (stride 0Ch, bound 9Ch) and consumed @ 0x479311..0x479318.
The 7-slot scan @ 0x478726..0x4787AF is the +2EF/+364/+365 latch arm, not a
depth. The census retires 0x477FF4, 0x478014 and 0x478D15 as crash-depth
witnesses; the translated `crash_depth` / `wheel_depth` split was already
correct. [orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0]

## 39. Player motor command precision and brake transitions (2026-09-18)

The shared player-input stage in `engine/runtime/world/vehicle_motor.cpp` was
compared with jo-c and the pinned retail executable in `Jointops.exe.kong.i64`.
The oracle executes the original input blocks and brake tails, stopping before
steering physics/contact; it does not mock engine calls. This pass made no IDB
changes. [Reproduction and limits](../jo-c-validation-2026-09-18.md).

| Component | Verdict | Evidence |
|---|---|---|
| Ground and bike input command state | MATCHING (behavioral proof) | `movement_brain_parity`: 640 vectors across both families, 16 MoveOrders, five analog triples, both prior brake states and two retained-direction values; public motor compared on eight output fields. |
| Full driver heading | MATCHING (read-only grill and command-state vectors) | Original occupant +0x10 feeds steering; `AiEntity::heading` preserves the full BAM word while `Entity::yaw` remains a rounded display mirror. |

The full ground mover reads the occupant's 32-bit yaw at @ 0x48BB0B. Analog
input clears the local digital direction when the old brake latch is zero
(@ 0x48BA03..0x48BA0C); otherwise held movement still takes the analog branch
(@ 0x48B9D2..0x48B9E5). Ground retains that analog throttle until the later
current-input brake test (@ 0x48C03A..0x48C095), while the bike zeroes it under
the old latch too (@ 0x484BA7..0x484BAB). Thus release has a family-specific
one-tick effect. The translation retains wraparound multiplication/subtraction
and arithmetic right shifts.
[orig: Entity_UpdateVehiclePhysics @ 0x48AF00;
Entity_UpdateLightVehiclePhysics @ 0x483FE0]

The earlier freelook merge uses retained entity+0x3C8 while +0x3CD is set,
otherwise the current direction; it also tests the signed sum of all three
analog axes. A nonzero steering axis can cancel out of that sum, allowing a
view turn. The command switch still uses the current direction. The port keeps
these two direction sources distinct. The live ground/bike motors do not write
+0x3C8; the mover-specific writer is in the unreferenced @ 0x486A50 motor
(@ 0x48748B), already classified as orphaned in the earlier mover census.
[orig: ground merge @ 0x48B855..0x48B897; bike merge @ 0x48497B..0x4849BD]

The shared heading/analog-direction correction also reaches the existing
simple-ground and watercraft callers. The watercraft analog clear and full-yaw
read are independently witnessed at @ 0x48E063 / @ 0x48E122; its existing tests
remain the family integration coverage. The new executable vectors prove the
bounded ground/bike command stage, not full contact or multiplayer playthrough
parity. [orig: Entity_UpdateWatercraftPhysics @ 0x48D480]


## 40. Tank contact, camera and pivot-sound follow-up (2026-09-22)

The [tank record](tank-parity-re.md) owns D-VEH-4/5, D-CTRL-5 and D-HUD-29:
merged wheel/belly support and airborne corner-fit corrections, camera basis
precision, wheel signs and HUD snapshot consumption. It also closes D-SND-17:
the fourth loop uses yaw rate (`entity+0xA4`), not `slide_z` (`+0xA0`) as the
older review stated, and the pivot-start cue, latch transitions and lane-40
loop, the slot-45 tread cue, the corrected claimant detach stop and the shared
last-tick gate are ported. The PR #671 review fix round, recorded in that
record's witness map, also ported the tank solve's crash, wreck, wall and
stability machinery (§8), the tank mover's impulse, crash stop, trails, slope
table, NPC-parent coast, dispatch and per-family client chase gates (§10), the
AI boarder hold, route writers, turn budget and submerged-driver cut (world
record §23.3 and §23.4), the vehicle AI slot seed and respawn class init
(section 18), and the ground-family mover-head savedLivePose stamp (§10 item
13). Its joiner legs are ported too: the vehicle death states' 0x26 kill
record (section 24), the dead-pose form and full-width wire attitude, the
reader's Flags/health/kill tail and the wreck-only freeze (§10 item 6),
remote claimants, joiner motion controls and hull gun words (section 11), the
0x0D parent/target split and the first-frame tick. The weapons legs landed with
them: the ewep CTRL publication with no occupant test (an emptied turret holds
its traverse), the claimant's vehicle-slot cut on detach, the mounted fire pose
(gfx3 launch point, barrel, view-tilt fold, point direction), the unconditional
turret window and the live seat points (tank record). Installed stock and JOTAC assisted 07TR course
runs pass; complete live-retail/normal-input acceptance remains unverified
there.
