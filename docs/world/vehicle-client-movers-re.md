# Vehicle client-side movers & contact solves — reverse-engineering record

The client-executed subsets of the per-family vehicle movers (the D-NET-196
prediction legs), the boat platform solve, the shared suspension solvers, the
cbik (bike) mover, and the aircraft contact solve. Implementing code:
`engine/runtime/world/src/vehicle_motor.cpp` (the family `*_client_tick` movers +
`watercraft_platform_solve`), `engine/net/netsim/src/client_replica_pipeline.cpp` (the
per-class row chases), with the joiner wiring in
`godot/engine/simulation/nova_simulation_net.cpp`. Binary: retail
`Jointops.exe`, imagebase `0x400000`, IDB `Jointops.exe.kong.i64` — every
address below is absolute in that image. The between-update mover architecture
(stage-only reads, the class table, the chase templates) is
[`../net/novaworld-net-re.md`](../net/novaworld-net-re.md) §5.38e; divergences
are tracked in the ledger's **D-NET-196** row (and D-NET-161 for the family
deferrals) — this record hosts the witnesses, the ledger owns the catalog.

## Verdicts

| Component | Verdict | Evidence |
|---|---|---|
| Watercraft mover client subset (`@ 0x48D480`) | MATCHING — ported (`watercraft_client_tick`) | §1 spec; `watercraft_client_motor` ctest (glide/coast/steer + the witnessed −167/−8350 vertical legs) |
| Watercraft mover AUTHORITY half (`@ 0x48D480`, gate `@ 0x48DF8C`) | MATCHING — ported (`tick_watercraft_motor` + `AiSystem::watercraft_ai_drive`; shared `watercraft_motor_core`) | §1.12 spec (witnessed 2026-08-06); `watercraft_client_motor` ctest authority legs; `00trg_defense_probe.gd` (host boats drive their event routes) |
| Aircraft mover client subset (`@ 0x490310`; cpln thunk `@ 0x45D6F0`) | MATCHING — ported (`aircraft_client_tick`) | §2 spec; air glide + altitude-hold + abandoned-hover ctest legs |
| Boat platform solve (`@ 0x481870`) | MATCHING — ported, client subset (`watercraft_platform_solve`) | §3 spec + §4 solver interiors; settle/level/roll-stability/gravity bench legs |
| Shared suspension solvers (`@ 0x46C8E0` / `@ 0x46B140`) | witnessed — the Z/fit paths ported inside the platform, air, and ground solves; the wreck-tumble machinery unported | §4 |
| cbik mover client subset (`@ 0x483FE0`) | MATCHING — the four family deltas ported into the shared ground core | §5 spec; the bike bench leg (gravity 250 / vZ cap / airborne yaw vs Ground) |
| Aircraft contact solve (`@ 0x47EF10`, defined 2026-07-31) | MATCHING — ported, client subset (`aircraft_contact_solve`, 2026-08-01) | §6 spec; landing/ramp-conform/water-hysteresis/sleep bench legs in `watercraft_client_motor` |
| Ground/tracked contact solve (`@ 0x47C1C0`) | MATCHING — ported, client subset (`ground_contact_solve`, 2026-08-05) | §7 spec; the wheel-clearance rest + drop-landing bench legs in `watercraft_client_motor` |
| Wheeled (ctan) contact solve (`@ 0x475DE0`) | MATCHING — ported, client subset (`wheeled_contact_solve`, 2026-08-06) | §8 spec; the tank rest/drop bench legs |
| Light (cbik) contact solve (`@ 0x479600`) + 2-corner chassis fit (`@ 0x468A50`) | MATCHING — ported, client subset (`light_contact_solve`, 2026-08-06); the lean smoother `@ 0x45B2C0` is a named FPU deferral | §9 spec; the bike rest/drop bench legs |
| Tank (ctan) mover deltas (`@ 0x488AB0`) | MATCHING — the witnessed family deltas ported into the shared ground core (2026-08-06) | §10 spec; the tank family-delta bench leg |
| Aircraft local-driver input map (`@ 0x490310` occupant block) | MATCHING — ported (`stage_air_vehicle_input` + the both-command blend, 2026-08-06); analog collective = named deferral | §10.4; the air local-pilot gate bench leg |

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
function minus the cited deferrals.**

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
(masks 3/4 when afloat every 2nd tick, 1/2 otherwise every 4th tick, intensity =
brain[136] / currentSpeed) and the movement-sound machine — cosmetic. Tail rebuilds
orientationMatrix from `&entity->Position` and sets Flags bit 0x20000 [0x48EF50..0x48EF63].

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
structurally match `world::tick_vehicle_motor` (engine/runtime/world/src/vehicle_motor.cpp) minus
the input block — can ground prediction reuse tick_vehicle_motor driven by the mirrored
cmd registers?

**Yes.** The ground client path after the mirror (`v51[136]=v51[177]; v51[132]=v51[179]`,
occupant != local, decomp l.616-622 — identical to the boat's) falls through the same
code the authority runs below the input gate, and that code is exactly what
tick_vehicle_motor ports:

| ground client block (decomp) | tick_vehicle_motor |
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
   resolve_vehicle_controller / ai_cmd) — run tick_vehicle_motor with the input block
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
   tick_vehicle_motor simplification is an existing ledgered divergence that prediction
   inherits.
4. speedAccel/currentSpeed/aiState/modelPtr0 evolve purely locally on the client — no
   net override besides the chase; drift is the chase's job (already ported).
5. Blocks NOT to run in prediction (they are inside the authority/local gate):
   AI waypoint drive, pool-1 avoid-brake, boarding-wait stop, stuck check.
6. Client-run cosmetic tails (anim accumulator, wake/dust FX, sound machine,
   `Entity_UpdateGravityAccumulator` at l.1846 — attrib&0x40 tail helper, untraced)
   are presentation, not motion.
7. UNVERIFIED (decomp-level only): the garbled euler arg of the ground
   `Math_BuildFixedPointMatrixFromEulerAngles((int*)speedAccel, ...)` (l.1241) is
   presumed `&entity->Position` by the boat's verified codegen pattern (0x48E972);
   confirm at disasm before citing in code.

Boat-vs-ground family deltas (why the boat needs its own motor, not tick_vehicle_motor):
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
- `Entity_UpdateGravityAccumulator` (ground tail) untraced.
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
`AiSystem::watercraft_ai_drive` (`engine/runtime/world/src/vehicle_motor.cpp` /
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

Deferrals staying with D-NET-161: the every-8th-tick groundEntity refresh
[@ 0x48D51F] + deck-carrier follow, the fire-FX/regen-drain leg, the MoveOrder
merge, the submerged-driver cut, the minAI clamp, the [135] mirror, the
boarding-wait hold, the stuck check, and the wake-anim lerp.

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
  from the client subset.

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
<48 snap), so a mirrored-register-driven tick_vehicle_motor will track a remote bike's
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

`modelData = *(graphicModel + 0xB0)`. Two boxes read (16.16 model space; axis
interpretation inferred from use — pairs are (lo,hi)):
box1: Z = ([0x28],[0x2C]), X = ([0x30],[0x34]), Y = ([0x38],[0x3C]);
box2 (footprint): X = ([0x40],[0x44]), Y = ([0x48],[0x4C]).

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
- modelData axis naming ([0x28..0x4C] pair assignment) is inferred from use; the
  box1-vs-box2 provenance (render vs collision box) untraced.
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
| +0x948 | flip | `flip` | 0x49DF82 | ground movers: airborne disable request when fitted |up.z| < flip * 0.01 * 65536 [orig: 0x477742..0x477762, flt_7C56A8 * flt_7C32BC]; clamped [0,100] by the platform fn |

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
| Vehicle_ApplyBrakingForce @ 0x45CEB0 (sites 0x45CEFC) | per tick from Entity_ProcessWheeledVehiclePhysics @ 0x478816/0x478EC2/0x478EEB | `spring_k += deltaTime` ramp, capped at dword_815180 − dword_815184 = 65535 − 52428 = 13107 (0.2 u); side products scaled by itemDef->spring (+0x8FC) shed into the wheel state and entity+0x300 |
| sub_45CFB0 (quadratic variant, dt²*2*spring) | per tick from light @ 0x47B431/0x47BAD0/0x47BAE4, tracked @ 0x47E2C1/0x47EB68/0x47EB7C, aircraft @ 0x480E48/0x48138A/0x48139E | same ramp/cap |
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
(our `world::ground_client_tick` interim carrier for bikes,
`engine/runtime/world/src/vehicle_motor.cpp:725`).

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
   bike's terrain pose + lean/roll attitude live in the light solve (B-facet,
   unported for every family).
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
    `Entity_UpdateGravityAccumulator @ 0x4928B0` on rideables
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
   owns bike lean/roll. The tracked solve is PORTED (§7, 2026-08-05) and the
   interim runs it for bikes too, so a parked bike rests at wheel clearance and
   conforms pitch/roll; the light solve's own deltas (bike lean machine, its
   probe shape) remain the open witness.
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
| flt_7C3B94 | 0.5 | spring-force ½ factor (all k·d² terms), and the ½ in sub_45CFB0's energy drain [orig: 0x4812A9, 0x481338, 0x48053E] |
| flt_7C6F18 | 1.25 | spring impulse → energy(+0x300) gain factor [orig: 0x4812C0] |
| flt_7C6F6C | 0.025 | hard-landing damage factor, mass>3 (dead path, §7) [orig: 0x480527] |
| flt_7C6F70 | 0.005 | hard-landing damage factor, mass<=3 (dead path) [orig: 0x48051B] |
| flt_7C6EB8 | 2.5e-05 | severity-3 object-impact damage factor, mass>3 [orig: 0x47FACE] |
| flt_7C6F74 | 1.25e-05 | severity-3 object-impact damage factor, mass<=3 [orig: 0x47FABA] |
| flt_7C32BC | 65536.0 | 16.16 normalize scale for axis vectors [orig: 0x47F380 etc.] |
| flt_7C19E0 | 2147418112.0 | ftol overflow clamp [orig: throughout] |
| dbl_7C57B8 | -683565275.5764316 | -(rad→BAM) for the two fpatan headings (severity-3 deflection calc) [orig: 0x47FD76] |
| flt_7C69FC | 1.57 | oscillator phase init (sub_45CFB0 writes block+0x14) [orig: 0x45CFC8] |
| flt_7C59B0 | -0.5 | energy(+0x300) drain factor in sub_45CFB0 [orig: 0x45D059] |
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
| +0x2D4..+0x2E0 | per-wheel spring compression (written by sub_45CFB0/0x45D110); **added to the pad body-frame Z** when building probe points | 0x47F514/0x47F56A/0x47F5E3/0x47F63F |
| +0x2EC | byte: parked latch | producer 0x48168D (park-enter), 0x46B1F9 (sub-solve); cleared 0x480831/0x481544/0x48177C |
| +0x2ED | byte (BYTE1): used by the sub-solve pathing | 0x46B1A6 |
| +0x2EE | byte: hard-sag latch (dead producers here in practice) | 0x480E90/0x481259/0x48123F/0x48151E |
| +0x2EF | byte: park-freeze latch (with +0x2EC → Z += maxdepth & grounded) | 0x480DED; cleared 0x48082B/0x48153D |
| +0x2F0 | byte: wreck-rest latch (0.25 fall factor, forced grounding, burn smoke) | producer 0x4808A1, 0x46B4F4; cleared 0x481783 |
| +0x2F2 | byte: settled/stable-contact flag (cosmetic consumer outside) | 0x47F09B/0x4803C8/0x48041B/0x480424 |
| +0x2FC | byte: suspension-reset marker | 0x480803; consumed 0x480882, sub-solve 0x46B419 |
| +0x300 | spring energy accumulator | 0x4812CB, drained in sub_45CFB0, zeroed in tail 0x4817C4 |
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
per pad: oscillator upkeep (force>0 → sub_45CFB0(block, 4095, i, entity);
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
    `Δ = sub_45CFB0(block_i, min(T,4095), i, entity)` (compression step: sink
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
| sub_45CFB0 | 0x45CFB0 | spring compression step: comp(+0x2D4+4i) += dt vs range 0xFFFF−dword_815184; force −= dt²·2·spring·½; energy −= dt²·2·spring·0.5; phase init 1.57 |
| Entity_ApplyDamageOscillationFast | 0x45D110 | free bounce: phase += 0.262 rad/tick, sinusoid×amp → compression; clamps itemDef->shock [0,10] |
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
   machinery `Vehicle_ApplyBrakingForce` @ 0x45CEB0 / @ 0x4790C7 decay legs, so
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
11. **Tail** [orig: 0x47EE50..0x47EEF5]: spring-energy release, the airborne
    tick counter (entity[1] bookkeeping @ 0x47EEC7), the non-authority
    inverted flip-restore [orig: 0x47EEAC..0x47EEB7], wreck-rest slideDecay
    halving, `+0x2ED = 0` — all deferred except nothing our subset consumes.

### 2. The client subset (the port contract)

`ground_contact_solve` in `engine/runtime/world/src/vehicle_motor.cpp`, dispatched inside
`tick_vehicle_motor` at the witnessed call position (after integration, before
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
   per-wheel +0x2D4 sink — zero in the client subset), SIX belly stations
   along the two side rails at X = foot_x_lo + r + {3q, 3q, q, q, 2q, 2q}
   with q = ftol(0.75 × box_x_span) >> 2 (flt_7C3DC8 = 0.75) and the
   front/rear/averaged per-side sinks in Z, and THREE spine probes at
   X = box_x_lo + {3L/4, L/2, L/4}, Y = box ymid, Z = box_z_hi − w/+w/−w with
   w = (beam >> 3) − 0x4000 (the MIDDLE spine probe sits ABOVE the deck).
   There is no separate spine radius.
2. **The 7-slot contact model** [orig: the pair maxes @ 0x4780E5..0x478131]:
   slot k = max(d_wheel_k, d_belly_k) for the four wheel/rail pairs, slots
   4..6 = the spine d's. The sev-3 0.25-cut scans the strongest of the first
   SEVEN probe forces only [orig: init from probe 0 @ 0x476BC3].
3. **Per-probe planar-contact + reverse flags → the stability contact byte**
   [orig: the per-probe walk @ 0x477BF4..0x477D0A; the byte
   @ 0x477F31..0x477FB4]: a probe "contacts" when it produced a planar force;
   a contacted probe is a REVERSE hit when its normalized force opposes the
   contact direction past −0.75 (−49152). The contact byte requires
   `up.z(Q16) > 0x2000` (NOT §7's 4096) and the LEADING axle for the current
   gear (front pads forward, rear pads in reverse) to be SYMMETRIC — both
   contacted or neither — with no reverse hit on it. The direction is the
   +0x3BC contact-direction store, falling back to the basis forward row when
   empty [orig: @ 0x477A48..] — our subset defers the store (D-NET-161), so
   the fallback IS the direction.
4. **The head-on wall stop** [orig: @ 0x477D3E..0x477E30]: the summed
   contacted force, normalized, against the same direction — past −0.871
   (−57070) the drive state zeroes (velocityX/Y and currentSpeed). The
   authority park-move damage riding it is deferred.
5. **The Z select absorbs into slideDecay** [orig: `slideDecay += solvedZ −
   Position.Z; if (slideDecay > 0) slideDecay = 0; Position.Z = solvedZ`
   @ 0x478BE3..0x478C06]: no §7 +0x2000 rise clamp — the landing step cancels
   the fall velocity instead. The solver is
   `Entity_ComputeSuspensionAndOrientation` @ 0x4698A0 (the third §4-family
   instance; positive-only corner average + fidiv by the positive count
   @ 0x46AF54..0x46AFE1, the same MAIN_FIT core as `plat_fit_corners`).
   Inverted: `Position.Z += max` over ALL 13 d's [orig: @ 0x478D06 with the
   gated scan @ 0x477FF4..0x478014]; the upside no-wheel-contact arm lifts by
   the max over the SEVEN contact slots [orig: @ 0x47843E..0x478540].
6. **The corner quad lifts by the four WHEEL d's only** (belly/spine d's feed
   severity and the Z maxes) [orig: the zero-state spring loop
   `dest[corner].z += d_k` @ 0x478A16..; `Vehicle_ApplyBrakingForce`
   @ 0x45CEB0 and `Entity_ApplyDamageOscillation` @ 0x45D240 are the live
   spring machinery — sinks grow +250/tick on airborne wheels, decay through
   the brake curve — all zero-state in the subset, so pads probe at
   box_z_lo + r and the lifts are raw, exactly like the zero-state original].
7. **Live-in-retail legs deferred with their §7-shared seams**: the sev-3
   entity momentum exchange and the entity-mass delta scaling (mass-gated
   transfer at `hit mass < 2×own` [orig: @ 0x476E19-region]), the crash/flip
   latches (the |up·z| < cos(flip) capsize byte, the falling-crash client arm
   keyed airborne+parked, the settle machine + its Yaw adoption,
   `Entity_ApplyWheelSuspensionForces` @ 0x463560's parked spring apply), the
   contact-direction downhill store (renormalized with fixed Z = −28672 while
   descending [orig: the tail @ 0x479518-region]), authority damage
   (spine-impact, underside-crush, park-move, burn), and every sound/FX/
   overlay send.

Port: `wheeled_contact_solve` (engine/runtime/world/src/vehicle_contact_solve.cpp),
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
   attitude identity. The crash tumble (BuildYXZ with the 298261 BAM/tick
   fall-over decay — a parked bike TIPS OVER), the wheelie force queues
   (`Entity_QueueSuspensionForce`), and the flip 0..100 def clamp ride the
   deferred wreck machine.
7. **The contact byte needs a REAR-WHEEL RUN**: `up.z(Q16) > 4096`, the lean
   bound |right.z| < 40960, rear-wheel contact, and MORE THAN ONE consecutive
   rear-contact tick (`entity[1].pad_040[8]`, reset in the both-wheels-off
   branch) [orig: @ 0x47A4CC..0x47A52B] — a one-tick graze never grounds the
   bike. Both-wheel landings absorb half the fall
   [orig: `slideDecay −= slideDecay >> 1` @ 0x47B0F1..0x47B103].
8. **The grounded heading/lean smoother** `Entity_SmoothHeadingToTarget`
   @ 0x45B2C0 (roll-rate producer into modelPtr2, the speed>4096 lean-latch
   arm vs the low-speed direct arm, ±itemDef+92C·0.1·scale clamps, the
   |delta| ≤ 256 deadband) is **FPU-garbled in decompile and stays a NAMED
   DEFERRAL** pinned to its disassembly — the lean is presentation-additive;
   pitch/roll pose comes from the axle fit. Sinks grow +100/tick (not 250)
   with `sub_45CFB0`/`Entity_ApplyDamageOscillationFast` as the spring
   machinery — zero-state in the subset.

Port: `light_contact_solve` (vehicle_contact_solve.cpp), routed by
`VehicleFamily::Bike` (the tracked-solve interim retired). Bench: the bike
rest/drop legs.

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
   ±acceleration (target ≠ 0) or ±deceleration (target 0). The slope
   anti-creep legs (±decel >> 2 at |speed| ≤ 4096, the ∓2·decel hard arm)
   ride the deferred contact-direction store — with the store empty retail
   takes exactly the plain caps.
4. **Full-basis drive velocity** [orig: the contact velocity-build stores
   @ 0x48a5ac..0x48a8b4]: velocity = speed × the normalized basis forward
   row, slideDecay REPLACED by speed × fwd.z — the tank drives along its
   conformed pitch. The low-speed contact-direction realign (the ±5°/tick
   BuildYXZ ±59652323 cross-product rotate toward forward) and the downhill
   creep-hold ride the deferred D-NET-161 store; the empty-store arm is
   exactly velocity = speed × fwd.
5. **Yaw applied unless PARKED, quartered airborne** [orig:
   @ 0x48a9f7..0x48aa1d `if (!parkedByte) Yaw += (Flags & 0x2000) ?
   modelPtr0 >> 2 : modelPtr0`] — the bike shape keyed on the solve-owned
   flag; the park byte rides the deferred latch machine.
6. **Named deferrals**: the differential track-scroll accumulators
   (animStateId/deathAnimStateId — presentation, no consumer in our runtime
   yet), the turret slew chase (brain 460..504, the ±0x2108421 step), the
   carrier-follow rotation composition (our carrier recompose owns carried
   rows), the AI drive/avoidance legs, and the joiner interp block — whose
   ladder {6,8,10,15,20,25,30} at {0x2AAA, 0x4000, 0x5555, 0x8000, 0x10000,
   0x20000}, 0x2000 deadband, 0x60000/0x20000-at-reg<293 snap (0xC0000
   crashed), (delta+10)/20 heading, and (v+64)>>7 drift decay are IDENTICAL
   to the already-ported ground/vehicle chase — no netsim change needed.

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

## IDB changes made during these sessions

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

## Open items

The unresolved residuals live as explicit entries in the D-NET-196 ledger row
and the §5.38e disposition (the wheeled/light solves, the tank mover deltas
and the aircraft local-driver input map CLOSED 2026-08-06 — §8/§9/§10): the
ground-family shared deferrals (the contact-direction slope-velocity feed,
the park/wreck/crash latch machine, spring sinks/oscillators — D-NET-161),
the bike lean smoother (`Entity_SmoothHeadingToTarget @ 0x45B2C0` —
FPU-garbled, disasm-pinned), the analog collective channel, the tank
track-scroll/turret-slew presentation, the modelData box-pair provenance
(the platform probe boxes — §3's tracked unknown), the
`Math_FixedPointMatrixToEulerAngles` interior
(`@ 0x613310`), the `Transform_ComparePartial` field scope (§6.15 — ported as
planar XY by structural argument), and the HOST-side platform scope (authority
boats still ride the generic motor; retail's authority runs the solve — the
ungated call `@ 0x48ECE7`).
