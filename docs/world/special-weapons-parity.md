# Mortar, Javelin, Stinger and tank parity - 2026-09-19

This pass corrects the reproduced input, firing-pose and target handoff gaps.
It does not establish complete weapon or vehicle equivalence.

## Reference

- OpenNova baseline: `46ce87f78e7f38f4fb81cc088470e10ee5840543`.
- Sibling `../jo-c`: `1dfaae1a2aaf9e7879cfae60d7aa8d1461867486`.
  Read `Jointops.exe.kong.c`, `docs/ground-family-reconstruction.md`, and the
  guided-missile and ground-family original-instruction harnesses.
- IDA MCP: `Jointops.exe.kong.i64`, input `Jointops.exe`, image base `0x400000`.
  The addresses below were checked against the original disassembly/decompiler.
  No original executable, IDB or jo-c source was changed.

## Corrections

| Area | Corrected behavior | Original evidence |
| --- | --- | --- |
| Mortar elevation | AbsorbPitch weapons retain a separate elevation offset. Equipping starts at the authored maximum and clears body pitch; mouse Y adjusts only the offset within the authored limits. Look-up/down keys leave it alone. Reload and a late same-weapon viewmodel rebake preserve the offset. | `Player_MountWeaponSlot @0x4DFA40`; input axes `@0x4E0EC3..0x4E0FE8`, key guards `@0x4E0E88/@0x4E0F67`; fire clamp `@0x4DC8A3..0x4DC8EF`. |
| Authored pitch limits | Convert targetpitchmax with truncated BAM-per-degree `11930464`; negate targetpitchmin as the retail parser does. Add minimum plus elevation offset to the firing pitch before clamping. | `WeaponDefs_ParseLineCallback @0x5443B8..0x54444D`; `Entity_GetWeaponTurretLimits @0x540D70`; `Entity_CalcWeaponFirePosition @0x4DC750`. |
| Mounted tank firing | Local gunner firing and aim acquisition use the posed carrier userpoint. The magazine count before ammo consumption selects the barrel with its low two bits. Missing points fall back to the carrier pose. | Gunner/controller branches `@0x4DC789/@0x4DC7E5`; `Entity_ComputeUserpointWorldTransform @0x545C60`, fallback `@0x545E1F`; slot selection `@0x5459C0`. |
| Shared fire origin | On-foot firing uses Position plus CameraOffset and undoubled recoil. DesignateTarget ammo substitutes the measured aim point only during the Fire action. | `@0x4DC847..0x4DC880`; designate action guard `@0x4DC91A`, copy `@0x4DC92C`. |
| Javelin/Stinger target position | Launch and steering sample the target's live Position; a muzzle or animated weapon userpoint no longer displaces the tracked location. | Javelin launch copy `@0x445E40`; Stinger `@0x446370`; Javelin `@0x446D39`. |
| Guided fire over the network | The selected `aiRuntime[3]` target accompanies the local fire event into C2S 0x06 +28. A delayed self replica cannot overwrite the local predicted missile's current launch lock. | `WeaponAction_Fire @0x542B10`, target `@0x542C0D`; writer `@0x42A7B6`. Replica precedence is the port's implementation of the current-local-input boundary. |
| Tank controls | Tank input updates sprint bit 0x80 while preserving bits 0x20/0x8; infantry lean keys no longer write the ground/bike-only flags. Steering and throttle still use the shared witnessed command path. | `Entity_UpdateTankVehiclePhysics @0x488AB0`, input tail `@0x489675..0x4896A7`. |

The shared fire-pose helper lives in
[`player_weapon_pose.cpp`](../../engine/runtime/world/player_weapon_pose.cpp).
The Godot installation path uses the same `weapon_install_data_from_def` builder
as the native kernel.

## Validation

- New `special_weapon_parity` regressions exercise the actual local weapon pump,
  mortar input/reload/rebake, action-gated designation, two pre-consumption barrel
  indices, moving guided targets with deliberately displaced muzzle poses, and
  tank input flags. The first implementation-independent checks failed before
  the corresponding runtime fixes.
- `inmatch_joiner_role` sends and decodes a real C2S 0x06 and checks the live
  predicted Javelin with an older, conflicting self-replica target.
- All **14 focused native suites passed**: `special_weapon_parity`,
  `inmatch_joiner_role`, `local_player_targeting`, `local_player_view`,
  `player_look`, `projectile_combat`, `vehicle_motor`, `guided_missile_flight`,
  `weapon_fsm`, `fire_sound`, `emplaced_gun_channel`, `throwables`,
  `weapon_inventory`, and `npruntime_client_fire`.
- `guided_missile_flight` includes **308 existing original-instruction
  vectors** generated from jo-c. This pass reused those fixtures; it did not
  regenerate them or count a reference/reference run as port validation.
- Godot RelWithDebInfo extension build passed. The headless
  `simulation_test.gd::test_aim_overlay_exports_the_retail_authored_pitch_sign`
  passed (one test, five assertions) with isolated user settings.
- Include/link graph checks, orphan-header check, conventions lint, ratchet
  counts and citation census passed. No ratchet or citation baseline changed.

The synthetic mortar range and barrel/target poses in the new tests exercise
mechanics; they are not claimed as retail asset measurements.

## Remaining boundaries

- The later [HUD follow-up](../interface/weapon-vehicle-hud-validation.md)
  implements the mortar impact HUD and the distinct Scoped + Inset scene
  (D-HUD-26); the original attribution of that scene to mortars was wrong.
- D-NET-64 remains open for the flare projectile-candidate ring, proximity
  AI/warning consumers and C2S command-map 0x44 overload. The AI Javelin no-lock
  750-unit forward query (`@0x445DCF`) is also not covered by this pass; the
  adapter still supplies the retained owner aim point on that fallback path.
- Tank sound's extra-effect argument remains D-SND-17. These corrections do not
  claim complete tracked-vehicle dynamics or every mounted rig's alternate
  userpoint/model transform behavior.
- The initial gameplay pass used synthetic fixtures. The later HUD follow-up
  mounts the installed JOTAC data for launcher/mortar/vehicle tests. Mixed
  original/OpenNova live play is still a separate validation boundary.

## HUD follow-up

The [2026-09-19 HUD validation](../interface/weapon-vehicle-hud-validation.md)
checks weapon and seat transitions against the original HUD routines. It fixes
seat-specific group visibility, mounted stance, Inset reticle selection, and
launcher reload flashing, then adds launcher targeting, mortar impact/map
transitions, the separate Inset scene, and vehicle instruments.
