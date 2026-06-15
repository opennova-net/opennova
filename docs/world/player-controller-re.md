# Local player controller — reverse-engineering record

Binary: retail **Jointops.exe** (Joint Operations: Combined Arms), IDB
`Jointops.exe.kong.i64`, imagebase `0x400000`. All addresses below are that
binary's. Sessions: **2026-06-14 (initial read-only grill)** and
**2026-06-14 (MAJOR motor-identity correction grill — re-witnessed the class
table, the org2 motor, the camera, the action handler, and the gravity/ground
paths)**.

> **Correction notice (2026-06-14).** The first pass misidentified the player
> motor as `cbike` / `Entity_UpdatePlayerInfantryMovement @ 0x483fe0`. A deeper
> grill of `g_EntityClassPhysicsTable @ 0x82abc8` proved that **`0x483fe0` is the
> bike / light-vehicle motor**, and the on-foot local player runs the **`org2`
> class motor `Entity_UpdateInfantryPhysics @ 0x4b40e0`** — the direct sibling of
> the AI infantry motor `org1 @ 0x4b9910`. Sections §1, §3, §4–§5 (entity
> offsets), the witness map, the verdict table, and the divergence ledger have
> been rewritten accordingly; the cbike material is retained only as brief
> out-of-scope context.

Scope: the **local human player on foot** — input capture and packing, the
per-tick player motor (locomotion / heading / gravity / collision), the
first-person camera, the stance state machine, weapons/aim/fire/recoil data,
the shared animation-name table, the class-keyed dispatch that reaches the
player motor, and local-player creation/loadout. **Out of scope** (separate
slices, flagged below where touched): mounted/vehicle seat physics, the
controllable-vehicle motors (incl. the `cbike` bike motor `0x483fe0`), the
swim/climb/ladder edge models, and NovaWorld replication beyond the local seam.

**Relationship to the AI motor.** The on-foot player motor `org2 /
Entity_UpdateInfantryPhysics @ 0x4b40e0` is the **direct sibling** of the AI
infantry motor `org1 / Entity_UpdateInfantryAI @ 0x4b9910`
([world-wac-ai-re.md §3](world-wac-ai-re.md)) — adjacent rows in
`g_EntityClassPhysicsTable`. They share the `GamePlayerEntity` struct and its
field layout, the 62 Hz tick, the BAM heading convention (`heading = 90 − yaw`),
the fixed-point matrix helpers, the shared animation-name table `off_8135F0`,
**and the same ground/collision/fall-damage helper
`Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0`, the same terminal
velocity, and the same root-motion locomotion mechanism**. The differences are
(a) gravity cadence — `−208`/tick `×1` for the player vs `−416`/2-ticks `×2` for
the AI (net-equal); (b) command source — input vs waypoints, and **neither motor
contains the command→velocity translation** (both consume pre-resolved fields
written upstream); and (c) the player **superstructure** org1 lacks (look,
pitch clamp, sway, ADS, footstep state, fall damage for the local player, swim,
recoil, the FP camera). The `org2` motor's local-player test at `0x4b43f3`
(pointer identity with `g_local_player_entity`) skips network position
prediction for the locally owned entity. See §3.6 for the full similarity
verdict and the port conclusion.

## Verdict table

This is a research record (no reimplementation to verify yet). Verdicts grade
how well the original behavior is **witnessed**, per the project confidence
scale (anchored > probable > guessed).

| Subsystem | Status | Evidence |
| --- | --- | --- |
| Dispatch / class routing (R1) | **witnessed** | `g_EntityClassPhysicsTable @ 0x82abc8` fully decoded (34 rows, 12-byte stride): the on-foot player is **`org2 → Entity_UpdateInfantryPhysics @ 0x4b40e0`** (sibling of AI `org1 @ 0x4b9910`); `cbike → 0x483fe0` is the **bike** motor, not the player (R1 **refuted-as-stated**, resolution corrected). Per-tick dispatch is `Entity_UpdateAllEntities @ 0x4c2100` → `(*(entity+0x1C4))(entity)` |
| Player motor — `Entity_UpdateInfantryPhysics @ 0x4b40e0` | **witnessed** | org2 core + the local-player superstructure read directly (continuation `0x4b434f`); locomotion = **root motion** (no scalar speed), gravity `−208`/×1, ground via shared `0x4b2bd0` |
| Input packing — `Player_PackInputStateToEntity` | **witnessed** | full straight-line decompile; entity layout `+0x12C` / `+0x130..0x133` pinned (corrects the inverted pre-grill framing) |
| Heading / mouse-look — `Input_HandleActionBinding_0` | **witnessed** | look applied in the action dispatcher; mouse scaling math pinned; **view-pitch clamp ±80° standing / ±40° crouched**, no on-foot yaw turn-rate clamp (D-PLR-5 resolved) |
| Gravity (R3/D-PLR-4) | **witnessed** | inline in org2: `vel_z(+0xA0) += −208`/tick, gated on platform/water, terminal `−32768`, buoyancy term — the net-equal single-rate form of the AI's `−416`/2-ticks (NOT `0x4928b0`, NOT exactly the AI cadence) |
| Ground / collision (R2/D-PLR-2/3) | **witnessed** | org2 uses the **same** `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0` as the AI motor (3 call sites), same settle + airborne flags + fall-damage block; no AI-style `+0x50000` stand offset |
| First-person camera — `Player_UpdateFirstPersonCamera` | **witnessed** | full decompile; weapon-bone offset, velocity lead, prone drop, ADS override, mount-bias residual all pinned (R8 FP-camera portion) |
| Stance state machine | **partial** | bit layout + three writers witnessed; the `A78394→A78398` ratio globals are vestigial (no writer; both static 0 → prone-drop branch effectively constant, D-PLR-7 resolved) |
| Weapons / aim / fire / recoil | **partial** | view-angle/recoil **data model** pinned; per-shot recoil **application** site not reached (IDA instance dropped) |
| Anim-name table `off_8135F0` (R9) | **witnessed** | R9 **confirmed**: 252-entry shared name table, two independent count witnesses |
| Local-player creation / loadout (R5) | **witnessed** | R5 **completed**: all three stores to `g_local_player_entity` enumerated (offline `Player_InitLocalPlayer`, save-load `SaveFile_ReadPlayerAvatarBlock`, in-session `player_initPlayer` via a PLAYER-flag+session-id pool scan); class tag is a def-level attribute (no per-spawn write); `EquippedSlot` null until `Player_SelectWeaponSlot` |
| Input layer (bindings / mouse / action dispatch) | **partial** | pipeline + mouse-gain math witnessed; the action-id → `dword_B3B728` bit map (`Input_HandleActionBinding_0`) movement-bit detail still partial |
| `cbike` bike motor `0x483fe0` (out of scope) | context only | the 8-way switch, forward accumulator, heading ease/snap, gravity `0x4928b0`, ground `0x479600` documented earlier all belong to the **bike**, not the player — retained as out-of-scope context |

## Witness map

Every behavioral claim cites `[orig: Name @ 0xADDR]`. No IDA renames were made
this session (read-only grill); proposed renames live in the open-questions
section.

### Entity (`GamePlayerEntity`) field layout

Witnessed via `Camera_ComputeThirdPersonView @ 0x437d10` (copies the entity's
position+euler into the view globals) and the org2 motor disasm. **The earlier
record's "euler at +0x4/+0x8/+0xC" was a misread — those offsets are POSITION.**

| Offset | Field |
| --- | --- |
| +0x04 / +0x08 / +0x0C | position X / Y / Z (16.16 world) |
| +0x10 / +0x14 / +0x18 | euler YAW / PITCH / ROLL (BAM) |
| +0x24 | flag word: `0x10` = ADS firing pose, `0x100` = PLAYER bit, `0x2000` = airborne |
| +0x80 / +0x84 / +0x88 | previous position X / Y / Z |
| +0xA0 | vel_z |
| +0xB0 | lean / heading accumulator |
| +0x11E | health |
| +0x12C | packed move-flags DWORD (incl. crouch `0x100` / prone `0x200`) |
| +0x130..0x133 | signed analog axis bytes |
| +0x170 | mount / vehicle pointer |

The camera bumps `g_view_pos_z += 0x10000` (eye height 1.0) for a foot soldier
[orig: `Camera_ComputeThirdPersonView @ 0x437e8f`].

### Input layer (binding tables → discrete intent)

| Anchor | Addr | Role |
| --- | --- | --- |
| `Input_ProcessFrame` | 0x49d520 | per-frame input pipeline driver |
| `Input_ProcessPlayerFrame` | 0x49d4c0 | keyboard/mouse/analog/toggle dispatch |
| `Input_ProcessKeyboardEvents` | 0x49d1f0 | release pass then press/dequeue pass |
| `Input_ProcessMouseAxisBindings` | 0x499680 | mouse delta → scaled look/turn axes |
| `Input_ProcessToggleBindings` | 0x499480 | joystick/hold continuous bindings |
| `Input_ProcessAnalogBindings` | 0x4995f0 | keyboard/button HOLD movement/fire |
| `Input_PumpAndCenterCursor` | 0x7616d0 | Win32 mouse delta source; recenters to (320,240) |
| `Input_DrainMouseMessages` | 0x4169a0 | zeroes the per-frame mouse deltas after dispatch |
| `Input_DispatchMouseEvent` | 0x761470 | absolute cursor + 120-step wheel |
| `Input_HandleActionBinding` | 0x49ad40 | command jumptable (case 17 = mouse-scale adjust) |
| `Input_HandleActionBinding_0` | 0x4e0420 | look application (yaw/pitch + `±80°`/`±40°` pitch clamp, §4.0) **witnessed**; the action-id → `dword_B3B728` movement-bit map still partial |
| `g_inputFlags` (`dword_B3B728`) | 0xb3b728 | per-frame discrete action bitfield |
| `g_inputFlags_prev` (`dword_B3B72C`) | 0xb3b72c | previous-frame snapshot |

### Input packing (discrete intent → entity fields)

| Anchor | Addr | Role |
| --- | --- | --- |
| `Player_PackInputStateToEntity` | 0x4df450 | folds `dword_B3B728` + analog axes into the entity input fields each tick |
| `Entity_CheckWeaponSeatFlags` | 0x540d00 | weapon-seat `0x20000` suppress test |
| `Player_ToggleWeaponScope` | 0x4df0c0 | scope toggle on movement with a scopeable weapon |
| raw analog accumulators | 0xb3b750 / 0xb3b752 / 0xb3b75a / 0xb3b75c | X / Y / Z / throttle (`>>3` before packing) |

### Player motor (org2)

| Anchor | Addr | Role |
| --- | --- | --- |
| `Entity_UpdateInfantryPhysics` | 0x4b40e0 | the **on-foot local-player / organic-soldier per-tick motor** (org2 class row); sibling of AI `org1 @ 0x4b9910` |
| `Entity_UpdateInfantryPhysics_Continuation2` | 0x4b434f | body continuation of the org2 motor |
| local-player skip-prediction test | 0x4b43f3 | `jz`-skips net position prediction when the entity == `g_local_player_entity` |
| `Math_FloatTranslationToFixedPoint16` | 0x4ad480 | extracts the clip's root-motion translation (mat[14]→X, −mat[12]→Y, mat[13]→Z, 16.16) — the per-tick body displacement |
| `AnimMap_UpdateDualChannels` | 0x40b8c0 | advances the locomotion clip (shared with AI; root-motion source) |
| inline gravity | 0x4b7ac8 / 0x4b7c77 / 0x4b7ce0 | `vel_z(+0xA0) += −208`, platform/water gate `0x108000`, terminal `−32768`, integrate |
| water buoyancy / swim clamp | 0x4b7bec / 0x4b7a80 | `+668` if `vel_z < −7168`; swim clamp `−167` |
| `Entity_ProcessCollisionAndPlatformPhysics` | 0x4b2bd0 | shared ground/water/platform resolver + settle + airborne flags + fall damage (3 call sites in org2: 0x4b7cf4 / 0x4b8bdf / 0x4b9755) |

### cbike bike motor (out of scope — not the player)

| Anchor | Addr | Role |
| --- | --- | --- |
| `Entity_UpdatePlayerInfantryMovement` | 0x483fe0 | **the controllable BIKE / light-vehicle motor** (cbike trampoline target); NOT the player. 8-way switch @0x484ccf, forward accumulator (+0x16C16C0/tick, cap 0x238E38C0), heading ease/snap toward `+0x234`, gravity `0x4928b0`, ground `0x479600` |
| `Entity_DispatchPhysics_cbike` | 0x48eff0 | `cbike` class trampoline → 0x483fe0 (sole xref) |
| `Entity_UpdateGravityAccumulator` | 0x4928b0 | the **bike's** gravity accumulator (NOT the player's) |
| `Entity_ProcessLightVehiclePhysics` | 0x479600 | the **bike's** ground/water/airborne processor (NOT the player's) |

### First-person camera

| Anchor | Addr | Role |
| --- | --- | --- |
| `Player_UpdateFirstPersonCamera` | 0x4dd380 | composes the FP eye transform `g_view_matrix` |
| `Entity_GetCameraTransform` | 0x4b8c00 | per-entity camera query; pulls the FP cam fn |
| `Player_ResetCameraAndMovementState` | 0x4de1f0 | clears the camera/movement smoothing block |
| `Camera_ComputeThirdPersonView` | 0x437d10 | seeds the view globals from the tracked entity (mode 0 = FP) |
| `ThirdPersonCamera_Update` | 0x437af0 | maintains the velocity-lead IIR |
| `CNetPlayer_InterpolateTransformStep` | 0x4ddd20 | writes the mount-settle view biases |
| `Player_MountWeaponSlot` | 0x4dfa40 | zeroes the view biases on every mount; reads `EquippedSlot` |
| `Math_BuildFixedPointRotationMatrixYXZ` | 0x615400 | YXZ matrix from view+bone rot |
| `Math_FixedPointTransformPoint22` | 0x615810 | rotate cam offset by view matrix |
| `g_view_euler_translation_out` | 0xb764c8 | output eye euler + translation |

### Stance

| Anchor | Addr | Role |
| --- | --- | --- |
| `NapiNPServerMsg_HandleStanceChange` | 0x501c60 | authority stance message (codes 169/170/172) → entity `+0x12C` bits 8/9 |
| `LocalPlayer_NetStateApply` (stance block) | 0x430556 | re-masks `+0x12C` bits 8/9 from the server snapshot |
| `dword_B76484` / `dword_B76480` | 0xb76484 / 0xb76480 | local crouch / prone mirror globals (drive the camera) |
| `dword_A78394` / `dword_A78398` | 0xa78394 / 0xa78398 | camera stance-height ratio: current / target |
| prone-drop gate `3a≤4b` | 0x4dd571 | lowers the eye `0x500` when into stance |
| `Game_InitNewRound` | 0x422740 | zeroes the stance mirrors at round start |

### Weapons / aim / fire / recoil

| Anchor | Addr | Role |
| --- | --- | --- |
| on-foot view euler | entity +0x10 / +0x14 | yaw / pitch; written by the look handler (§4.0), pitch-clamped `±80°`/`±40°` there, smoothed by org2 (`+0x2E0`, ÷32) |
| `AmmoDef_ParseProperty` (`recoil`) | 0x40a2d0 (match 0x40a9b4) | parses 3 recoil bytes → def `+0xE3/+0xE4/+0xE5` |
| `g_MpNoWeaponRecoil` | 0x2550b48 | `mp_NoWeaponRecoil` runtime cvar |
| `CharAttr_LoadFromDef` (`RECOIL_MUTE`) | 0x412140 | weapon attribute flag |
| view-angle block `0x485780` (`+0x2A0`/`+0x2B8`, `±def[0x8E4]`, `dword_81518C` turn cap) | 0x485780 | **bike-motor** aim/clamp model — NOT the on-foot player (re-attributed; out of scope) |

### Anim-name table / dispatch

| Anchor | Addr | Role |
| --- | --- | --- |
| `off_8135F0` | 0x8135f0 | 252-entry anim clip-name table (`+ "EOF"` sentinel `+` NULL) |
| `AnimMap_FindSlotByName` | 0x40cfa0 | name → slot, linear stricmp, bound `0xFC` |
| `Script_ForceAnimation` | 0x4f2610 | slot → name `(&off_8135F0)[idx]` |
| `Entity_ComputeAnimSlotIndex` | 0x43a690 | the per-tick **death** selector (only runtime arithmetic index) |
| `AnimMap_UpdateDualChannels` | 0x40b8c0 | per-tick anim channel update |
| `AnimMap_UpdateEntity` | 0x40b5f0 | channel → clip + root-motion (shared with AI; see [world-wac-ai-re.md §3.4](world-wac-ai-re.md)) |

### Dispatch / class routing

| Anchor | Addr | Role |
| --- | --- | --- |
| `g_EntityClassPhysicsTable` | 0x82abc8 | class-name → physics callback; 34 rows, 12-byte stride `{char name[8]; void* cb}` |
| `g_EntityClassPhysicsCount` (`dword_82AD60`) | 0x82ad60 | row count = `0x22` (34) |
| `EntityDef_LookupPhysicsCallback` | 0x4a9240 | stricmp class tag; stores cb at `ItemDef+344` |
| `org2` row → `Entity_UpdateInfantryPhysics` | 0x4b40e0 | **the on-foot player / organic-soldier motor** (the player path) |
| `org1` row → `Entity_UpdateInfantryAI` | 0x4b9910 | the AI infantry motor (sibling) |
| `org0` row → nullsub | 0x4aff60 | static organic (null stub) |
| `cbike` row → `Entity_DispatchPhysics_cbike` | 0x48eff0 | controllable-bike trampoline → bike motor `0x483fe0` (NOT the player) |
| controllable-vehicle block | 0x48efc0–0x490310 | `cveh→0x48efc0`, `ctank→0x48f0f0`, `cbike→0x48eff0`, `catv→0x48f010`, `cpln→0x45d6f0`, `ctrn→0x48f060`, `chld→0x45d540`, `CHel→0x490310` |
| `Entity_UpdateAllEntities` | 0x4c2100 | per-frame driver; calls `(*(entity+0x1C4))(entity)` once per pool-0 entity; gated on `g_local_player_entity != 0` |
| `g_local_player_entity` | 0xb75fc8 | local-player pointer; designation = PLAYER flag (`+0x24 & 0x100`) + pointer identity, not a class re-tag |

### Local-player creation

| Anchor | Addr | Role |
| --- | --- | --- |
| `g_local_player_entity` | 0xb75fc8 | the local human player's `GamePlayerEntity*` (exactly 3 store sites) |
| `player_initPlayer` | 0x4e15f0 | **in-session designation**: store @0x4e17eb from the pool scan; zeroes the 0x3160-byte local block, wires weapon slots, difficulty-scales move speed |
| `Player_FindLocalPlayerEntity` | 0x4e0090 | the in-session pool scan: pool 0, match `Flags(+0x24) & 0x100` (PLAYER) AND `+0x78 == session_id` |
| `SaveFile_ReadPlayerAvatarBlock` | 0x4ac780 | **save-load designation**: store @0x4ac7b1 from a saved pool handle; restores `EquippedSlot +0x118` |
| `Player_InitLocalPlayer` | 0x4b1060 | offline designation: store @0x4b1124 gated `!is_in_session`; default-weapon/anim seed (dispatched via table slot @0x813054) |
| `EntityDef_LookupPhysicsCallback` | 0x4a9240 | resolves the class tag at **def load** → stores cb at `ItemDef+344` (def-level, no per-spawn write) |
| `NapiNPClientMsg_0x00F` | 0x42e200 | spawn Position/Yaw/Pitch/Roll seed (msg 0x0F); reads the already-set global |
| `NapiNPClientMsg_HandleWeaponLoadoutSync` | 0x4290e0 | avatar class `+0x294` + loadout seed (msg 0x5A); reads the already-set global |
| `Player_SelectWeaponSlot` | 0x4dd680 | writes `EquippedSlot @+0x118` (active) from cached slot `pad9[108]`; scans `weaponSlotArrayBase` for a valid slot |
| `Entity_BuildSpawnPointList` | 0x42de40 | spawn-marker selection by def type id + team |

## 1. Dispatch — how the player reaches its motor (R1)

The engine resolves a per-entity-class physics callback by 8-byte class tag
(the items.def class name) from `g_EntityClassPhysicsTable @ 0x82abc8` —
**34 rows** (`dword_82AD60 = 0x22`), **12-byte stride** `{char name[8]; void* cb}`
(name first, callback at `name+8`). `EntityDef_LookupPhysicsCallback @ 0x4a9240`
`stricmp`s each ItemDef's physics-type-name against the table and stores the
match at `ItemDef+344` (default = row 0 = a null stub).

The table decoded in full [orig: `g_EntityClassPhysicsTable @ 0x82abc8`]:

- **Organic-soldier block** — `org0 → 0x4aff60` (null stub, static organics),
  `org1 → Entity_UpdateInfantryAI @ 0x4b9910` (the AI motor), **`org2 →
  Entity_UpdateInfantryPhysics @ 0x4b40e0` (the on-foot player / organic-soldier
  motor)**.
- **Controllable-vehicle block** — `cveh → 0x48efc0`, `ctank → 0x48f0f0`,
  **`cbike → 0x48eff0`** (a controllable BIKE / light vehicle, trampoline →
  `Entity_UpdatePlayerInfantryMovement @ 0x483fe0`), `catv → 0x48f010`,
  `cpln → 0x45d6f0`, `ctrn → 0x48f060`, `chld → 0x45d540`, `CHel → 0x490310`.

**The on-foot local player runs the `org2` motor `Entity_UpdateInfantryPhysics
@ 0x4b40e0`** (continuation `Entity_UpdateInfantryPhysics_Continuation2 @
0x4b434f`), the direct **sibling** of the AI infantry motor `org1 @ 0x4b9910`.
Per-tick dispatch is `Entity_UpdateAllEntities @ 0x4c2100`, which calls one
callback per pool-0 entity via `(*(entity+0x1C4))(entity)`; for the player that
callback is `org2 / 0x4b40e0`.

**`cbike / 0x483fe0` is the bike / light-vehicle motor, NOT the player.** Every
behavior the earlier record attributed to "the player motor 0x483fe0" — the
8-way switch @0x484ccf, the forward-speed accumulator (`+0x16C16C0`/tick, cap
`0x238E38C0`), the heading ease/snap toward `entity+0x234`, gravity via
`Entity_UpdateGravityAccumulator @ 0x4928b0`, ground via
`Entity_ProcessLightVehiclePhysics @ 0x479600` — is the **bike's** behavior, kept
only as out-of-scope context (witness map). `Entity_DispatchPhysics_cbike @
0x48eff0` is the bike trampoline (sole xref to `0x483fe0`).

The local player is designated by the **PLAYER flag** (`entity+0x24 & 0x100`) plus
pointer identity with `g_local_player_entity @ 0xb75fc8` — **not** by a class
re-tag (this matches §8; a local human soldier is an `org2`-class entity flagged
PLAYER and pointed-to by the global). "Is this the local player" is everywhere a
pointer-equality test against the global, e.g. inside the org2 motor @0x4b43f3,
which `jz`-skips net position prediction for the locally owned entity.

**R1 verdict: refuted-as-stated.** The original pre-grill hypothesis (and the
first correction to it) routed the player through a vehicle slot. The first
correction's resolution — "player = `cbike` → `0x483fe0`" — was **itself the
error**: `0x483fe0` is the bike motor. The corrected resolution: the on-foot
player motor is **`org2 / Entity_UpdateInfantryPhysics @ 0x4b40e0`**, reached via
the per-tick `(*(entity+0x1C4))` callback, the sibling of the AI motor.

**Confirmed in the game data (third, binary-independent line).** The retail
`ITEMS.DEF` (JO_ASSETS_t) declares the player soldier (`graphic US01`, `hp 150`)
with `move_function org2` / `ai_function plyr` / `input_function troop` /
`disk_function PLAYER`; the AI enemy soldier declares `move_function org1`; and
`cbike` only ever appears as a *vehicle* `move_function` (bikes/ATVs/jeeps, with
`HasTurret` / `turn_rate`). The per-entity class is this 4-field taxonomy —
`move_function` (the motor, the `g_EntityClassPhysicsTable` tag), `ai_function`
(brain: `plyr` vs `org1`), `input_function` (`troop`), `disk_function`. So the
player motor = `org2` is settled by code (class table + pool-0 PLAYER-flag scan
`Player_FindLocalPlayerEntity @ 0x4e0090`) and by data.

## 2. Input packing — the `PlayerInputCommand` model

`Player_PackInputStateToEntity @ 0x4df450` runs once per tick, returns early if
`g_local_player_entity` is null, and translates the accumulated discrete input
`dword_B3B728` plus the four raw analog accumulators into the entity's
network-input fields. It is straight-line (one switch, several if-ladders, no
loops). **This corrects the inverted pre-grill framing**: the move-flags live in
a DWORD at `entity+0x12C`, and `+0x130..0x133` are four signed analog axis
bytes — not the other way around.

### 2.1 Packed move-flags DWORD `entity+0x12C` (300)

Built by ORing into a fresh DWORD:
- **bits 0–2** = 8-way move-direction index `0..7`, **bit 3 (`0x8`)** = is_moving
  [orig: `@ 0x4df6a1`]. The index comes from a switch over a 4-bit
  `direction_bits` value assembled from input bits `0x2/0x4/0x8/0x10` (the four
  movement keys → dir bits 0/1/2/3 [orig: `@ 0x4df48e`]); opposing pairs cancel
  to `is_moving=0`.
- Feature OR-bits (each from a `dword_B3B728` test):
  `0x8000→0x10`, `0x1000→0x20`, `0x2000→0x40` (lean), `0x4000→0x80` (lean),
  `0x100→0x1000`, `0x200→0x2000`, `0x20→0x4000`, `0x40→0x8000`
  [orig: `@ 0x4df6e8 .. 0x4df790`].
- **`0x100` = stance A / crouch** when `dword_B76484` set; **`0x200` = stance B /
  prone** when `dword_B76480` set [orig: `@ 0x4df6b5 / 0x4df6cd`]. These are the
  stance mirrors (§5), not packed by the analog logic.

On-foot, the org2 motor reads the stance bits (`0x100`/`0x200`) to switch the
**locomotion animation** (stand/crouch/prone) — there is **no** scalar
quarter/half-speed multiplier; the slower crouch/prone gait is the slower
animation's root motion (§3.2). (The bit8/bit9 "quarter/half lateral speed"
behavior documented in the first pass was the **bike** motor's, not the
player's.)

### 2.2 Analog axes `entity+0x130..0x133` (304–307)

Four **signed bytes**: X = `word_B3B750 >> 3` → `+0x130`, Y = `word_B3B752 >> 3`
→ `+0x131`, Z = `word_B3B75A >> 3` → `+0x132`, throttle = `dword_B3B75C >> 3`
→ `+0x133` [orig: `ANALOG_SHIFT = 3 @ 0x4df7ac`]. Throttle has a **deadzone
`0x14` (20)**: `|throttle| < 20` is zeroed [orig: `@ 0x4df7d3`]. Each axis is
re-transmitted only when it changed by **> `0x20` (32)** from the cached prev
sample (caches `byte_B79445/444/443/441`, change flags `byte_B79442` for X/Y/Z,
`byte_B79440` for throttle) [orig: `AXIS_CHANGE_THRESHOLD = 0x20 @ 0x4df7e8`];
otherwise the entity byte is written 0. `is_moving` forces **both** change flags
to 0 (movement suppresses analog look transmit); the lean pair `0x6000` forces
the throttle flag to 0 [orig: `LEAN_MASK = 0x6000 @ 0x4df855`].

### 2.3 Seat lockout and bookkeeping

If the equipped weapon-seat has flag `0x20000` set
[orig: `Entity_CheckWeaponSeatFlags @ 0x540d00`, tested via `EquippedSlot`
`+0x118`], `input_flags` is masked to `& 0xE1` and `dword_B3B728 &= 0xFFFF9FE1`
(clears movement/lean/stance bits `0x601E`) — a passenger/turret lockout
[orig: `@ 0x4df47d / 0x4df482`]. On movement with a scopeable weapon
(`weaponDef+8 & 1`) it calls `Player_ToggleWeaponScope @ 0x4df0c0`.

Finally: `dword_B3B72C = dword_B3B728` (save prev), then `dword_B3B728 = 0`
(clear the accumulator for the next frame).

## 3. Player motor — `Entity_UpdateInfantryPhysics @ 0x4b40e0` (org2)

Per-tick (62 Hz) physics/locomotion for the on-foot local player (and
net-replicated organic soldiers). It is the **sibling of the AI infantry motor**
`org1 / Entity_UpdateInfantryAI @ 0x4b9910` and shares its ground/collision
helper, terminal velocity, entity layout, and root-motion locomotion mechanism;
it adds the player superstructure (look smoothing, sway, ADS, footstep/landing
state, fall damage for the local player, swim, recoil). The motor **does not
integrate mouse look itself** — body heading and view pitch arrive already
resolved from the action dispatcher (§4); this motor only **smooths** view pitch
(`+0x2E0`, ÷32 via `sar 5`) and applies idle sway. Continuation:
`Entity_UpdateInfantryPhysics_Continuation2 @ 0x4b434f`.

### 3.1 Top-level shape

For the locally owned entity the motor **skips network position prediction**
(the `+0x234../+0x240..` interp), branched at `0x4b43f3` on pointer identity with
`g_local_player_entity`; a mirror of the local pitch is kept in `dword_B75FCC`.
Remote-replicated soldiers run the same motor with prediction enabled. The mount
pointer is `entity+0x170` (0 for an un-mounted foot soldier).

### 3.2 Locomotion is root-motion driven — no scalar speed

The 8-way move index (`entity+0x12C` bits 0–2) selects a **directional
locomotion animation**; the per-tick body displacement is **that clip's root
motion translation**, extracted by `Math_FloatTranslationToFixedPoint16 @
0x4ad480` (mat[14]→X, −mat[12]→Y, mat[13]→Z, fixed 16.16) and advanced by
`AnimMap_UpdateDualChannels @ 0x40b8c0`. Walk-vs-run is an **animation choice**;
there are **no** walk/run/strafe scalar def-speed fields (a string sweep
confirms). This is the **same** root-motion mechanism as the AI motor and as our
existing `libs/world InfantryRootMotion`
([world-wac-ai-re.md §3.4](world-wac-ai-re.md)).

8-way index → direction (clockwise from facing): `0` = forward, `1` = fwd-left,
`2` = strafe-left, `3` = back-left, `4` = back, `5` = back-right, `6` =
strafe-right, `7` = fwd-right.

On-foot crouch (`0x100`) / prone (`0x200`) switch to **crouch / prone locomotion
animations** (different root motion) — there is **no** on-foot scalar stance
multiplier. **Only SWIM** has scalar stance factors (`1.5` crouch / `0.667`
prone / `1/3` standing) on the equipped-def `+0x150/+0x154`.

### 3.3 Heading / view pitch (consumed, not produced here)

Body heading and view pitch arrive already resolved from the action dispatcher
(§4); the motor only **smooths** view pitch (`+0x2E0`, ÷32 via `sar 5`) and adds
idle sway. The motor contains **no** body-turn rate clamp and **no** view-pitch
clamp — the pitch clamp (`±80°` standing / `±40°` crouched) lives in the action
handler `Input_HandleActionBinding_0 @ 0x4e0420` (§4), not here. The lean /
heading accumulator is `entity+0xB0`.

### 3.4 Gravity — `−208`/tick `×1` (the net-equal single-rate form, D-PLR-4)

On-foot player (org2) gravity is **inline**:
`vel_z(+0xA0) += −208 (0xFFFFFF30)` per tick, **gated OFF** when on a platform
(`+0x24 & 0x100000`) or in water (`+0x24 & 0x8000`) via `test [+0x24],
0x108000` [orig: `@ 0x4b7ac8`]; terminal clamp `−32768` [orig: `@ 0x4b7c77`];
`pos_z(+0xC) += vel_z × 1` (plus a platform term) [orig: `@ 0x4b7ce0`]. Water
buoyancy: if `vel_z < −7168` add `+668` [orig: `@ 0x4b7bec`]; swim clamp `−167`
[orig: `@ 0x4b7a80`].

This is the **net-equal single-rate form** of the AI motor's `−416`/2-ticks `×2`
(the AI is byte-witnessed inline at `0x4bf7b8–0x4bf7ee`: `test eax, 0x108000`
cadence gate, `add [esi+0xA0], 0xFFFFFE60`, clamp `−32768`, `pos_z += vel_z·2`).
The player is **neither** `Entity_UpdateGravityAccumulator @ 0x4928b0` (that was
the **bike**) **nor** exactly the AI `−416`/×2 — it is `−208`/×1, net-equal.

### 3.5 Ground / collision, integration, effects (D-PLR-2/3)

org2 (the player) uses the **same** `Entity_ProcessCollisionAndPlatformPhysics @
0x4b2bd0` as the AI motor — **3 call sites** in org2 (`0x4b7cf4`, `0x4b8bdf`,
`0x4b9755`) — with the same settle (`sub [+0xC], heightDelta; vel_z(+0xA0) = 0`),
the same airborne-flag transitions (`+0x24` `0x2000`/`0x40`), and the same
**fall-damage** block (threshold `dword_C6EAE4 × 0xFFFFFBDF`; damage =
`(−vel_z − C6EAE4×0x421) >> 4` against health `+0x11E`; `Player_OnDamageReceived`
for the local player) — structurally identical to the AI motor's `0x4bf7f2`
block. There is **no** AI-style fixed `+0x50000` stand offset inside org2; the
settle is penetration/force-based via the shared helper.

**Our port grounds feet-on-terrain (no `+0x50000`) for BOTH the player and the AI
infantry motor.** Our soldier `.3di` models import feet-origin (the witness:
statically-placed soldiers render correct), so the original's `+0x50000` mover
stand clearance — which it pairs with origin-above-feet models — floated our
soldiers ~1 body when applied to the rendered entity Z. Our infantry grounding now
omits it (`tick_infantry`, `floor_z = inf.ground_cache`; see world-wac-ai-re.md
**D-INF-6**), matching this player path. OPEN (IDA follow-up): whether
`brain[131] = ground + 0x50000` is the entity's actual `pos[2]` or only the mover's
look-ahead target Z (→ mis-port correction rather than a divergence).

**Retraction (V-F3).** The wave-2 V-F3 note claimed our `libs/world/infantry.cpp`
"mis-cites `0x4b2bd0` as the player path" and that the player uses `0x479600` —
that conclusion came from the cbike misID; V-F3 examined the **bike**. The real
on-foot player (org2) uses `0x4b2bd0`, exactly as `infantry.cpp` cites. So
`infantry.cpp`'s ground citation is **correct for both AI and player**.
(`infantry.cpp`'s gravity uses the AI `−416`/×2 form; the player is `−208`/×1,
net-equal — a minor tracked divergence, not a mis-cite.)

The final matrix is rebuilt from the updated euler and the matrix-dirty bit is
set; footstep/cloth events come from the shared `.bad` per-frame trigger array
(`dword_A2ED08`, surfaced by `AnimMap_UpdateEntity`), the same path the AI motor
and `Entity_UpdateInfantryPhysics_Continuation2 @ 0x4b434f` reference.

### 3.6 Similarity verdict — org2 (player) ≈ org1 (AI) family, and the port conclusion

The on-foot player motor `org2 @ 0x4b40e0` is the same family as the AI motor
`org1 @ 0x4b9910`:

- **Shared:** the same ground/collision/fall-damage helper `0x4b2bd0`; the same
  terminal velocity (`−32768`); the same `GamePlayerEntity` field layout; the
  same **root-motion locomotion mechanism** + animation selection.
- **Differences:** (a) gravity cadence `−208`/×1 (player) vs `−416`/2-ticks ×2
  (AI), **net-equal**; (b) command source — input vs waypoints — and **neither
  motor contains the command→velocity translation** (both consume pre-resolved
  fields written upstream); (c) the player superstructure org1 lacks:
  network-prediction skip for the local player (the `+0x234../+0x240..` interp,
  skipped at `0x4b43f3`), the local-pitch mirror `dword_B75FCC`,
  look-smoothing / sway, ADS / weapon sway, the footstep / landing-sound state
  machine, fall damage for the local player, swim physics, the full bone-matrix
  build, and recoil (§6).

**Port conclusion.** Our existing `libs/world/infantry.cpp` (the `org1` port) is
the **right structural basis for the player motor** — reuse its
gravity / ground / integrate / root-motion / anim core, change gravity to
`−208`/×1, drive it from input (`PlayerInputCommand`) instead of waypoints, and
add the player superstructure (look + the `±80°`/`±40°` pitch clamp, sway, recoil,
FP camera).

## 4. Heading / mouse-look and the first-person camera

### 4.0 Look application — `Input_HandleActionBinding_0 @ 0x4e0420` (D-PLR-5)

Mouse look is applied **in the action dispatcher**, not in a separate drain pass
and not in the motor. The action handler consumes the scaled mouse delta as the
bound action's `analogValue`:

- **Scaling** (`Input_ProcessMouseAxisBindings @ 0x499680`):
  `scaled = (delta · (dword_24D207C << 11) + 0x8000) >> 16`; Y is inverted unless
  `dword_24D2078`; ADS divides sensitivity by the weapon zoom factor.
- **act166 TurnLeft**: `Yaw(+0x10) −= scaled << 16`; **act167 TurnRight**:
  `Yaw += scaled << 16` — **no per-tick yaw turn-rate clamp on-foot** (the AI's
  `±69273360` clamp does **not** apply to the player).
- **act164 LookUp**: `Pitch(+0x14) += scaled << 16`; **act165 LookDown**: `−=`;
  **act161**: `Pitch = 0` recenter; snap-turn `±0x1259E68` (`±1.613°`).
- **View-pitch clamp**: `±0x38E38E00 = ±80.0°` standing, reduced to
  `±0x1C71C700 = ±40.0°` when crouched (`entity+0x12C & 0x100`)
  [orig: `@ 0x4e0d44` max / `@ 0x4e0d56` min / `@ 0x4e0ffe` crouch-limit].
  Turret / mounted seats use per-vehicle limits
  (`Entity_GetWeaponTurretLimits @ 0x540d70`).

The org2 motor itself only **smooths** pitch (`+0x2E0`, ÷32 via `sar 5`) and adds
idle sway — it does not clamp. **D-PLR-5 is resolved**: the pitch clamp is
`±80°` / `±40°` in the action handler; there is no on-foot yaw clamp.

### 4.1 First-person camera — `Player_UpdateFirstPersonCamera @ 0x4dd380`

The FP eye transform is built in two stages.

**Stage 1** (`Camera_ComputeThirdPersonView @ 0x437d10`, mode-0 path) seeds the
shared view globals from the camera-tracked entity (`dword_A890CC` = the local
player): `g_view_pos_*` ← `entity[+4/+8/+0xC]`, `g_view_rot_*` ←
`entity[+0x10/+0x14/+0x18]`. For a foot soldier it bumps `g_view_pos_z += 0x10000`
(eye height 1.0) [orig: `@ 0x437e8f`]. `ThirdPersonCamera_Update @ 0x437af0`
keeps a velocity-lead via a 1/32 IIR: delta `= (pos − prev) << 8`,
`velocity += (oldDelta − velocity − newDelta + 16) >> 5` [orig: `@ 0x437bbf`].

**Stage 2** (`Player_UpdateFirstPersonCamera @ 0x4dd380`) consumes them, running
only if `EquippedSlot (+0x118)` and its `Def` are non-null:
- Build a YXZ matrix from `(g_view_rot_*)` then the weapon **bone rot**
  (`Def+0x100/+0x104/+0x108`) [orig: `Math_BuildFixedPointRotationMatrixYXZ @ 0x615400`].
- Bone **pos** offset `Def+0xF4/+0xF8/+0xFC` (`ftol` → int `cam_offset`).
- **View biases** `g_view_rot_bias_*` / `g_view_pos_bias_*` are added to the bone
  rot/pos **unless** in a turret seat (`Flags & 0x2000`) or the
  `Type==4 && (Def.Flags & 0x2000000)==0` branch. These biases are **not
  constants**: they default 0, are zeroed on every mount by
  `Player_MountWeaponSlot @ 0x4dfa40`, and are written each interp step by
  `CNetPlayer_InterpolateTransformStep @ 0x4ddd20` as a mount-settle residual
  that decays to 0. A steady-state on-foot player sees them as 0.
- **Velocity lead** = `g_view_velocity >> 7`, clamped `±0x400` on X/Y and `±0x1000`
  on Z [orig: `@ 0x4dd4f2 .. 0x4dd51b`], added to `cam_offset`.
- **Prone drop**: if `3·dword_A78394 ≤ 4·dword_A78398`, `cam_offset_z −= 0x500`
  (1280) [orig: `@ 0x4dd571`]. The `dword_A78394`/`dword_A78398` stance-height
  ratio globals are **vestigial / dead in retail** (no writer; both static 0), so
  this branch is **effectively unconditional** (§5, D-PLR-7).
- **ADS/scope override**: if `(Flags & 0x2)` or `dword_24C1970`, `cam_offset` is
  **replaced** entirely by `WeaponDef.AltCamOffset` (`Def+0x10C/+0x110/+0x114`),
  discarding bone offset, bias, lead, and prone drop [orig: `@ 0x4dd582`].
- Rotate `cam_offset` by the view matrix, add `g_view_pos_*`, write the euler +
  translation to `g_view_euler_translation_out @ 0xb764c8`.

`Entity_GetCameraTransform @ 0x4b8c00` is the renderer's per-entity query: for a
passenger seat it delegates to `Entity_ComputeUserpointWorldTransform @ 0x545c60`;
for a bone-driven entity it **calls** `Player_UpdateFirstPersonCamera` and returns
the refreshed view matrix. The camera consumes the stored body heading; it does
**not** itself apply 90-yaw (that lives upstream, same as for the AI).
`Player_ResetCameraAndMovementState @ 0x4de1f0` clears the smoothing block and
sets `dword_26C6848 = 0x500000` (80.0).

## 5. Stance state machine (stand / crouch / prone) — R8

Stance is a **2-bit mutually-exclusive field** in the move-flags DWORD at
`entity+0x12C`: **bit 8 (`0x100`) = crouch**, **bit 9 (`0x200`) = prone**,
neither = stand. Set paths always clear (`& 0xFFFFFCFF`) before OR, enforcing
the enum. Three writers converge on `+0x12C`:
1. **Local pack** (`Player_PackInputStateToEntity @ 0x4df450`): `dword_B76484` →
   `0x100`, `dword_B76480` → `0x200` (§2.1).
2. **Net snapshot apply** (`@ 0x430556`): extracts stance bits from the packed
   server value, `and [+0x12C], 0xFFFFFCFF`, then `or` the masked `0x300` bits —
   keeping the mirrors in sync.
3. **Authority stance message** (`NapiNPServerMsg_HandleStanceChange @ 0x501c60`):
   decodes codes **169/170/172** → entity bits, then if the entity is the local
   player mirrors bit8→`dword_B76484`, bit9→`dword_B76480`.

The local mirrors `dword_B76484` (crouch) / `dword_B76480` (prone) carry stance
to the camera. The camera reads two ratio globals `dword_A78394` (current) /
`dword_A78398` (target) via the prone-drop gate `3·cur ≤ 4·tgt → eye_z −= 0x500`
[orig: `@ 0x4dd571`]. **These globals are vestigial / dead in retail**: a
whole-binary search finds **no writer** and both are statically `0`, so the gate
`3·0 ≤ 4·0` is **always true** and the `−0x500` drop is effectively
**unconditional**. There is no dynamic stance-height ramp in retail. **D-PLR-7
is resolved**: do **not** implement a dynamic prone-cam lerp — replicate the
retail (effectively constant) behavior.

## 6. Weapons / aim / fire / recoil

**Aim source.** The on-foot firing direction is the player's view **pitch
`entity+0x14`** + **yaw `entity+0x10`** (BAM euler; §"Entity field layout"),
written by the look action handler `Input_HandleActionBinding_0 @ 0x4e0420` and
clamped there to `±80°` standing / `±40°` crouched (§4.0), then **smoothed** by
the org2 motor (`+0x2E0`, ÷32). **Correction (re-attributed):** the earlier
"matrix-rebuild block @0x485780" aim/clamp paragraph (pitch `+0x2A0`, yaw
`+0x2B8`, per-def clamp `±def[0x8E4]`, turn-rate accumulator `+0x46C` vs
`dword_81518C`) describes the **bike / light-vehicle motor `0x483fe0`** (a turret
/ vehicle-aim model), **not** the on-foot player; it is out of scope here.

**Recoil data model (witnessed).** The weapon/ammo `recoil` key parses **3 bytes**
into def `+0xE3/+0xE4/+0xE5` [orig: `AmmoDef_ParseProperty @ 0x40a2d0`, stores
`@ 0x40a9d5/0x40a9ea/0x40aa05`]. `RECOIL_MUTE` is a weapon attribute
[orig: `CharAttr_LoadFromDef @ 0x412140`]; `mp_NoWeaponRecoil` is a runtime cvar
`g_MpNoWeaponRecoil @ 0x2550b48` (parsed in `Config_ParseSettingsLine`, applied
in `apply_session_settings_to_globals @ 0x551500`). **The per-shot recoil
APPLICATION site** (which reads `+0xE3/+0xE4/+0xE5` and kicks the view euler
`+0x10/+0x14`, plus the recovery decay) **was not reached** — the IDA instance
dropped. The per-byte meaning (pitch-kick / spread / recovery) is also
unconfirmed. (The `+0x2A0/+0x2B8` view fields named in the first pass are the
**bike-motor** view fields, not the on-foot player's.)

## 7. Animation-name table `off_8135F0` — R9

**R9 verdict: confirmed.** `off_8135F0 @ 0x8135F0` is a **single** global array of
**252** `char*` clip-key strings (indices 0..251), then a 253rd pointer to
`"EOF"` (sentinel, physically present but unreachable), then a NULL terminator at
`0x8139E4`. Two independent count witnesses: the search loop bound
`cmp esi, 0xFC` in `AnimMap_FindSlotByName @ 0x40cfa0` (`@ 0x40cfc8`), and a byte
dump — index 0 = `"reset"`, index 251 = `"wpn_scopedown"`, index 252 = `"EOF"`.
There is **no** separate player vs AI table and **no** 200-entry variant; the
player and AI share the same table and the same `0..251` slot namespace.

`AnimMap_FindSlotByName` is name→slot (linear `stricmp`, skips a 5-char prefix
via `name+5`); `Script_ForceAnimation @ 0x4f2610` is slot→name. Living-state
anims are resolved **by name at load/WAC/anim-init time**
(`AnimMap_ParseConfigLine @ 0x40cb60`, `WacScript_ResolveParameter @ 0x4f2920`,
`Anim_InitActions @ 0x541fa0`) then played by numeric slot — **not** by
`(weapon, stance, action, dir)` arithmetic. The **only** runtime arithmetic index
into the table is the **death** selector `Entity_ComputeAnimSlotIndex @ 0x43a690`:
infantry `slot = 180 + 4·v7[bone] + variant` into the `death_bullet_*` block
(180..239), with fixed slots for other entity types and a `0x8000`-flag override
to slot 175 (`death_drown`). Both the AI motor (site 0x4b9cd6) and the player
path (`Entity_UpdateInfantryPhysics_Continuation2 @ 0x4b434f`, site 0x4b4c7f)
call it identically. The table is a load-time/script/debug name utility, **not** a
per-tick motor table. Per-tick selection is `AnimMap_UpdateDualChannels @ 0x40b8c0`
→ `AnimMap_UpdateEntity @ 0x40b5f0` (the shared root-motion path; see
[world-wac-ai-re.md §3.4](world-wac-ai-re.md)).

## 8. Local-player creation + loadout (R5)

`g_local_player_entity @ 0xb75fc8` has **exactly three store sites** (whole-binary
byte scan of every `mov g_local_player_entity, reg` form; only `eax`/`esi` forms
exist — `0x4b1124`, `0x4ac7b1`, `0x4e17eb`):
- **Offline / non-session**: `Player_InitLocalPlayer @ 0x4b1060` (dispatched via
  the per-class init function-pointer table slot @0x813054) assigns the global
  **only when `!g_napi_np_ctx.is_in_session`** [orig: store `@ 0x4b1124`,
  `mov g_local_player_entity, esi`]. The same fn seeds the default weapon
  `"WPN_M4AUTO"` (-> `AvatarDef_FindIndexByName @ 0x53fd80` -> `entity+0x2B0`), the
  movement word `entity+0x160 = 3`, anim-channel defaults 43, and runs a physics
  warmup loop. Its `if (g_local_player_entity == entity)` check at 0x4b10d6 is a
  **read** that fires only when a *prior* caller already designated this entity.
- **Save-game load**: `SaveFile_ReadPlayerAvatarBlock @ 0x4ac780` resolves a saved
  pool handle to an entity pointer and stores it [orig: `@ 0x4ac7b1`,
  `mov g_local_player_entity, eax`], then immediately **restores `EquippedSlot
  @+0x118`** = `AdmDef_FindByHandle(AdmDef_GetEntryByIndex(entity[+0x2B0]))`, falling
  back to the prior `+0x118` value if the lookup is null [orig: `@ 0x4ac7da`].
- **In session (the witnessed MP path)**: `player_initPlayer @ 0x4e15f0` is the
  connect/handoff player-init. It `memset`s the 0x3160-byte local-player global
  block (0x4e1602), then calls `Player_FindLocalPlayerEntity @ 0x4e0090` and stores
  its result into the global [orig: `@ 0x4e17eb`, `mov g_local_player_entity, eax`].
  **`Player_FindLocalPlayerEntity` scans pool 0 and returns the first entity whose
  `Flags (+0x24) & 0x100` (PLAYER bit) is set AND whose `+0x78` equals the NapiNP
  session id** (`sub_4C6D40(&g_napi_np_ctx)` reads connection `+920 -> +24`). If
  none matches, `Player_BuildNetIdLookupOrFatalError @ 0x4dff60` shows the fatal
  "Could not find player dcb in list of dcbs sent from the server!" box. So the
  in-session designation is **by PLAYER-flag + session-id pool match, not by class
  tag and not by the wire-side msg handler**; `player_initPlayer` then wires weapon
  slots (`WeaponSlotTable_LoadAllFromDefs`, `Player_SelectWeaponSlot`,
  `Player_SwitchToWeaponByHandle`) and difficulty-scales the move-speed globals
  `dword_B76490 = 524288.0 * diff`, `dword_B7648C = * 65536.0` [orig: `@ 0x4e1754`].

The **class tag** is never written per spawn. `EntityDef_LookupPhysicsCallback
@ 0x4a9240` `stricmp`s the items.def physics-type-name against
`g_EntityClassPhysicsTable @ 0x82abc8` **at def load** and stores the resolved
callback at `ItemDef+344` (the DEF, shared by all entities of that type), defaulting
to row 0 (null stub) on no match. A local human soldier reaches the player motor
purely because its ItemDef's class tag is `org2` ->
`Entity_UpdateInfantryPhysics @ 0x4b40e0` (§1). The per-entity "is the local
player" distinction is the **PLAYER flag (`+0x24 & 0x100`) + the
`g_local_player_entity` pointer identity**, not a class-tag rewrite.

Every net handler treats the global as already-set and only seeds fields (msg 0x0F
explicitly guards `if (!g_local_player_entity)` at 0x42e3af; msg 0x5A guards
`if (g_local_player_entity)` at 0x4293f2). Spawn pose arrives via **msg 0x0F**
  (`NapiNPClientMsg_0x00F @ 0x42e200`): writes Position `+0x04/+0x08/+0x0C`, Yaw
  `+0x10`, Pitch `+0x14`, Roll `+0x18` (`raw << 16` fixed), caches yaw to
  `dword_B75FCC`, builds the spawn list. The full loadout + avatar class arrives
  via **msg 0x5A** (`NapiNPClientMsg_HandleWeaponLoadoutSync @ 0x4290e0`): byte0
  = avatar class → `entity+0x294`, then a `0xFF`-terminated list of
  `(typeId, primAmmo, secAmmo, altAmmo)` records → `weaponSlotArrayBase @ 0xb75fd4`
  via `WeaponSlotTable_LoadAllFromDefs @ 0x5414e0`, then
  `Player_SelectWeaponSlot @ 0x4dd680` + `Player_MountWeaponSlot @ 0x4dfa40`.

`EquippedSlot @ entity+0x118` (typed `MountSlot*`, struct-confirmed) is the
**active** weapon-slot pointer into the 780-entry × 100-byte `weaponSlotArrayBase
@ 0xb75fd4` table. `Player_SelectWeaponSlot @ 0x4dd680` writes it: it scans the
table for a valid slot (`slot[+8] != 0 && (*(slot+8)+12) & 1`) — first the exact
requested group/slot, then any valid slot in the same group of 65, then any valid
slot globally — caching the pointer at `pad9[108]` and, unless the player is in
mount-state 2/3, copying it to the active `pad6_pre[20]`/`+0x118`.
**EquippedSlot is null for an un-mounted infantry player until both (a) the loadout
populates `weaponSlotArrayBase` (msg 0x5A `WeaponSlotTable_LoadAllFromDefs` /
save-load / offline default) and (b) `Player_SelectWeaponSlot` runs**: the zero
`memset` in `player_initPlayer` (0x4e1602) clears it, the scan needs a non-empty
table, and `Player_MountWeaponSlot @ 0x4dfa40` early-returns when `+0x118` is null.
So a freshly designated local player has `EquippedSlot == NULL` until the loadout
+ select sequence completes (which is why `Player_UpdateFirstPersonCamera` and the
input-pack seat-lockout test both null-guard `+0x118`). Spawn markers are matched by
`Entity_BuildSpawnPointList @ 0x42de40` by def type ids (items 4091/4093/4095,
buildings 4098/4100/4102/4103) + flag `0x8000`, keyed to the player's Team
`+0x162`.

## 9. Divergence / deferral ledger (D-PLR-n)

Stable ids; never renumber. These are the deliberate or pending divergences a
faithful port will carry. Phrased "to be confirmed at port time" where the grill
was incomplete.

| ID | Ours (planned) | Original (Jointops.exe) | Why / consequence |
| --- | --- | --- | --- |
| D-PLR-1 | client-side position interpolation deferred (local-authority integration only) | non-authority clients converge to the server snapshot over a step count, or teleport beyond a max distance; for the **local** player the org2 motor **skips** the prediction block (`0x4b43f3` pointer-identity test) | single-player + the locally owned entity skip the block, so the local-play port is faithful without it; the interp/teleport model is a net-replication concern, ported when the client seam needs it (the earlier `6..30`-step / `0x60000`/`0x20000` / `0x484337` figures were the **bike** motor's interp — re-grill the org2 prediction block when porting the net seam) |
| D-PLR-2 | ground/collision: reuse the shared `Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0` path (as `infantry.cpp` already cites) | org2 (player) uses the **same** `0x4b2bd0` as the AI motor (3 call sites: `0x4b7cf4`/`0x4b8bdf`/`0x4b9755`); force-based settle (`sub [+0xC], heightDelta; vel_z=0`), shared airborne flags + fall damage; **no** AI-style fixed `+0x50000` stand offset | **corrected** — our `infantry.cpp` ground citation (`0x4b2bd0`) is **right for both AI and player**; the prior "`0x479600` foot path" was the **bike** (V-F3 retracted, §3.5). Port the shared resolver, force-based settle |
| D-PLR-3 | parent-transform inheritance deferred (un-parented foot player) | a parent-transform inheritance block exists for parented entities; foot players are normally un-parented (block skipped) | needed only for riding moving platforms/vehicles — rides the mount slice (the `0x4848bc`/22.10-trig figures cited earlier were the **bike** motor; re-grill the org2 parent path if/when a foot player rides a platform) |
| D-PLR-4 | gravity = `vel_z(+0xA0) += −208`/tick `×1` (inline), platform/water gated, terminal `−32768`, buoyancy `+668` / swim clamp `−167` | org2 inline gravity: `−208`/tick gated on `+0x24 & 0x108000`, terminal `−32768`, `pos_z += vel_z×1`; this is the **net-equal single-rate form** of the AI's `−416`/2-ticks `×2` [orig: `0x4b7ac8`/`0x4b7c77`/`0x4b7ce0`/`0x4b7bec`/`0x4b7a80`] | **corrected** — gravity is `−208`/×1, **not** `Entity_UpdateGravityAccumulator @ 0x4928b0` (that was the bike) and **not** exactly the AI `−416`/×2; net-equal to the AI, a minor cadence divergence vs `infantry.cpp` |
| D-PLR-5 | view-pitch clamp `±80°` standing / `±40°` crouched, applied in the look action handler; no on-foot yaw turn-rate clamp | look applied in `Input_HandleActionBinding_0 @ 0x4e0420`: pitch clamp `±0x38E38E00` (±80°), reduced to `±0x1C71C700` (±40°) when crouched [orig: `0x4e0d44`/`0x4e0d56`/`0x4e0ffe`]; yaw has **no** per-tick clamp on-foot; the org2 motor only smooths pitch (`+0x2E0`, ÷32) | **RESOLVED** — port the `±80°`/`±40°` pitch clamp in the look handler; do not add an on-foot yaw rate clamp (the AI's `±69273360` does not apply) |
| D-PLR-6 | recoil = data model only until the application site is witnessed | 3 recoil bytes parse to def `+0xE3/+0xE4/+0xE5`; the per-shot kick (to the on-foot view euler `+0x10/+0x14`) + recovery decay was not reached | to be confirmed at port time — needs a follow-up grill (§10); do not invent a recoil curve (the `+0x2A0/+0x2B8` view fields named earlier are the **bike** motor's) |
| D-PLR-7 | prone-cam drop is effectively constant — no dynamic stance-height lerp | `dword_A78394`/`dword_A78398` are **vestigial / dead** (no writer; both static 0), so the prone-drop gate `3·cur ≤ 4·tgt → −0x500` is always true | **RESOLVED** — do **not** implement a dynamic prone-cam lerp; replicate the retail (effectively constant) `−0x500` behavior |
| D-PLR-8 | local-player designated explicitly at spawn (set the PLAYER flag + own the pointer); default `"WPN_M4AUTO"` loadout; `EquippedSlot` null until a slot is selected | three stores: offline `Player_InitLocalPlayer @0x4b1124` (gated `!is_in_session`), save-load `SaveFile_ReadPlayerAvatarBlock @0x4ac7b1`, in-session `player_initPlayer @0x4e17eb` via `Player_FindLocalPlayerEntity` (pool-0 scan: `Flags & 0x100` PLAYER + `+0x78` == session id). Class tag = def-level (`ItemDef+344`, no per-spawn write). `EquippedSlot @+0x118` null until `Player_SelectWeaponSlot` runs on a populated `weaponSlotArrayBase` | **R5 witnessed** — the faithful port: designate by setting the PLAYER flag and pointing `g_local_player_entity` at the chosen pool entity; do NOT rewrite a class tag (the **`org2`** physics callback is resolved from the def); seed `EquippedSlot` only after the loadout table is filled |
| D-PLR-9 | anim selection by name→slot at load, dual-channel update at runtime | shared 252-entry `off_8135F0`; living anims resolved by name, death by arithmetic (R9) | no divergence in the table itself; the living-state **selection tree** (gait/stance/dir → slot) was not traced and is a separate grill target |
| D-PLR-10 | locomotion is **root-motion driven** (reuse the existing `InfantryRootMotion`); no scalar walk/run/strafe speed | the 8-way move index selects a directional locomotion **animation**; per-tick body displacement is that clip's root motion via `Math_FloatTranslationToFixedPoint16 @ 0x4ad480` + `AnimMap_UpdateDualChannels @ 0x40b8c0` — the **same** mechanism as the AI motor; only SWIM has scalar stance factors (1.5/0.667/1/3 on def `+0x150/+0x154`) | no divergence — reuse `libs/world InfantryRootMotion`; crouch/prone are different **animations** (different root motion), not a speed multiplier (the bike's `0x16C16C0`-accumulator scalar speed is **not** the player's) |

## 10. Open questions / unwitnessed gaps

Listed by the dependent milestone they block. Confidence is < anchored for all of
these — each warrants a focused follow-up grill before its milestone lands.

> **Closed by the 2026-06-14 correction grill** (no longer open): the
> cbike-vs-org2 motor identity (§1), the gravity model (`−208`/×1, §3.4 /
> D-PLR-4), the ground/collision path (shared `0x4b2bd0`, §3.5 / D-PLR-2), the
> view-pitch clamp (`±80°`/`±40°` in the action handler, §4.0 / D-PLR-5), and the
> "locomotion speed source" (there is none — it is root-motion, §3.2 / D-PLR-10).

**Input map (blocks the input → intent port):**
- The exact **action-id → `dword_B3B728` bit name** map inside
  `Input_HandleActionBinding_0 @ 0x4e0420` (which key/binding sets `0x2/0x4/0x8/0x10`
  movement vs `0x20/0x40/0x100/0x200/0x1000/0x2000/0x4000/0x8000` action bits, by
  name: forward/back/strafe-L/strafe-R/jump/crouch/prone/lean/fire/reload). The
  movement-bit detail is still partial; cross the `or g_inputFlags, imm` sites
  with the binding action-id table.
- `byte_A860EC` (alternate-binding-set / shift-state selector) — what sets it.

**Player motor (blocks the motor port):**
- The **`+0xB0` lean / heading accumulator clamp bound** (the lean limit) — read
  the clamp constant.
- The precise **root-motion fixed-point shift** (`dbl_7C3600` / `dbl_7C3608`) used
  when rotating the clip's root translation into world space (the AI motor uses
  the same constants; pin the shift for byte-exact playback).
- The exact **swim stance semantics** on the equipped-def `+0x150/+0x154` (the
  `1.5` crouch / `0.667` prone / `1/3` standing factors) and the def parser that
  fills them.

**Stance:**
- The exact net stance-code → bit pairing in `NapiNPServerMsg_HandleStanceChange`
  (the 169/170/172 ladder pairing looked suspicious; needs one clean re-read).
- The **local keypress** writer of `dword_B76484/dword_B76480` (only resets and
  net-driven writes witnessed).

**Weapons (blocks D-PLR-6):**
- The **per-shot recoil application** site (reads def `+0xE3/+0xE4/+0xE5`, kicks
  the on-foot view euler `+0x10/+0x14`, recovery decay) — likely the shared
  weapon-fire handler gated by `g_MpNoWeaponRecoil`.
- The per-byte recoil semantics (pitch-kick / spread / recovery).
- The **ADS/scope state flag** bit and the turn/move-rate slowdown it applies.

**Creation (D-PLR-8 — now witnessed; residual leads):**
- ~~The in-session assignment site~~ **FOUND**: `player_initPlayer @ 0x4e15f0`
  stores `g_local_player_entity` at 0x4e17eb from `Player_FindLocalPlayerEntity
  @ 0x4e0090` (pool-0 scan: `Flags & 0x100` PLAYER + `+0x78` == NapiNP session id).
- ~~The class tag the spawn path assigns~~ **RESOLVED**: no per-spawn class-tag
  write; `EntityDef_LookupPhysicsCallback @ 0x4a9240` resolves the def's class tag
  to `ItemDef+344` at def load. The **`org2`** routing is a def attribute, not a
  spawn act.
- ~~Whether `EquippedSlot @+0x118` is non-null pre-select~~ **RESOLVED**: NULL until
  `Player_SelectWeaponSlot @ 0x4dd680` runs on a populated `weaponSlotArrayBase`.
- Residual: who *calls* `player_initPlayer @ 0x4e15f0` (no direct xref → called
  via a function pointer / dispatch; trace the player-list / "dcb from server"
  handshake that fires it). Also confirm `+0x78` (session id) vs `+0x7C` (`DcbId`)
  — `Player_FindLocalPlayerEntity` keys on `+0x78`, while triggers/actions key on
  the `DcbId @+0x7C`; both are distinct per-entity ids.

**Anim (blocks D-PLR-9):**
- The living-state selection tree (gait/stance/dir → slot) inside
  `AnimMap_UpdateEntity @ 0x40b5f0` / its callers.

---

*Read-only grill; no IDA write-backs or source changes this session. Companion:
[world-wac-ai-re.md](world-wac-ai-re.md) (the AI infantry-motor sibling),
[runtime-architecture.md](../runtime-architecture.md) (the tick), and the engine
heading/fixed-point conventions in [engine-primer.md](../engine-primer.md).*
