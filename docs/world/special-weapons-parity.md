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
| Mortar elevation | AbsorbPitch weapons keep a separate elevation offset. Equipping starts it at `max - min` (the authored maximum) and clears body pitch. The mouse-Y binding SUBTRACTS its scaled value from the offset where the ordinary arm adds the same value to Pitch, then clamps it to `0..max-min`; the look-up/down keys are refused. Every one of these consumers asks `Entity_CheckWeaponSeatFlags`, which answers nothing for an OnlyScoped weapon in the local player's hands until the scope is promoted, so the stock mortar (AbsorbPitch + OnlyScoped + OnlyFireScoped) looks around normally while carried unscoped; scope-up levels the body pitch. Reload and a late same-weapon viewmodel rebake preserve the offset. | `Player_MountWeaponSlot @0x4DFA40` (raw flag `@0x4DFA86`, stamp `@0x4DFAB7`); mouse-Y action 164 `@0x4E0F91..0x4E0FE8` (`sub dword_B79008` `@0x4E0FE2`, ordinary `add` `@0x4E0D39`, clamp `@0x4E0F1A..0x4E0F3A`); key guards `@0x4E0E8C` / `@0x4E0F5A`; `Entity_CheckWeaponSeatFlags @0x540D00`; scope-up `Player_ToggleWeaponScope @0x4DF2A2..0x4DF31D`; fire add and clamp `@0x4DC8A3..0x4DC8EF`. |
| Authored pitch limits | Convert targetpitchmax with truncated BAM-per-degree `11930464`; negate targetpitchmin as the retail parser does. Add minimum plus elevation offset to the firing pitch, then apply the banded limit clamp (`0x1FFFF` admission band). | `WeaponDefs_ParseLineCallback @0x5443D1..0x544440`; `Entity_GetWeaponTurretLimits @0x540D70`; `Math_ClampAngleToBounds @0x540CC0`; `Entity_CalcWeaponFirePosition @0x4DC750`. |
| Mounted tank firing | A gunner fires from the posed userpoint of the gun it sits on; a G-attached gun whose slot is routed to its parent fires from the HULL's userpoint table instead. A controller seat on an EWEAP carrier uses the second helper, which folds the carrier's view tilt into Pitch around the matrix build. The magazine count before ammo consumption selects the barrel with its low two bits. Missing points fall back to the carrier pose. A gunner's aim ray STARTS at the seat-bone pose (the posed CAMERA userpoint), and only its far point rides the weapon view. | `Entity_CalcWeaponFirePosition` gunner branch `@0x4DC7A0..0x4DC802` (G-redirect arm `@0x4DC7A9..0x4DC7D9` -> `Entity_ComputeUserpointTransform @0x545A40` on `parent->groundEntity`), controller branch `@0x4DC803..0x4DC846` (view tilt `@0x545BB9..0x545BBF`); `Entity_ComputeUserpointWorldTransform @0x545C60`, fallback `@0x545E1F`; slot selection `@0x5459C0`; aim-ray start `@0x4B4F1C..0x4B4F35`, far point `Entity_BuildCameraFromWeaponView @0x4B4EE0`. |
| Shared fire origin | On-foot firing uses Position plus CameraOffset and undoubled recoil. DesignateTarget ammo substitutes the measured aim point only during the Fire action. | `@0x4DC847..0x4DC880`; ammo flag `@0x4DC90D`, action guard `@0x4DC916`, copy `@0x4DC91C..0x4DC937`. |
| Guided target sampling | Every IN-FLIGHT refresh samples the target's aim origin: a person's phased CameraOffset point, a model's `TARGET` userpoint, else its bounding-box centre. Raw Position is copied only by the Javelin launch seed (overwritten by phase 0 on the next tick), the owner's class get-target callback and the flare decoy. A muzzle or animated weapon userpoint never displaces the tracked location. | `Entity_ComputeWeaponFireOrigin @0x43B4B0` called from Stinger `@0x4463A3`, Hellfire `@0x446772`, Javelin `@0x446C2D` / `@0x446D82` / `@0x446DF6` / `@0x446E8E` / `@0x446F74`; raw copies: Javelin launch `@0x445E40..0x445E97`, `sub_4B0DD0 @0x4B0DF5..0x4B0E12`, flare decoy `@0x446232..0x44626B`. |
| Guided fire over the network | The selected `aiRuntime[3]` target accompanies the local fire event into C2S 0x06 +28. A delayed self replica cannot overwrite the local predicted missile's current launch lock. | `WeaponAction_Fire @0x542B10`, target read `@0x542C00..0x542C15`; `NetPacket_WriteEntityPositionUpdate` target store `@0x42A759`. Replica precedence is the port's implementation of the current-local-input boundary, not a witnessed rule. |
| Mounted first-person camera | A seated rider's mode-0 view belongs to its carrier. A carrier whose def authors `virtualdisplay <model> <userpoint>` calls its input class's camera callback: the `tank` class places the eye at the carrier matrix times that userpoint of the virtual-display model and pulls it 0x3000 back along the rider's view rotation, and the `tank` render class draws that model INSTEAD of the hull for the local claimant in mode 0 (nothing when the def authors none). Otherwise an EWEAP that is not PlayerControl poses its own `CAMERA` userpoint: view position AND rotation come from the posed gun part, so a turret gunner's view follows the lagging gun. Stock data: the M1A1 and T80 author `tankdrvr camera` / `t80_drvr camera` with `input_function tank` and `render_function tank`, and every emplaced gun model carries a `camera` userpoint. | `ItemDef_ParseProperty` `virtualdisplay @0x49F4E0` (model `@0x49F506`, userpoint `@0x49F521`), `input_function @0x49F650`; `EntityDef_LoadModelsAndCallbacks @0x43A5D3..0x43A644` (byte def+0x1C0); input rows `@0x829DA8` (`tank @0x829DC8` -> camera `0x44A190`, null/troop -> `0x4DC710`); `Camera_ComputeThirdPersonView @0x437D10` callback leg `@0x437DAC..0x437E77`, EWEAP leg `@0x437E7C..0x437EAD`; `Entity_InitBoneReferences @0x4414A9..0x4414B4` (CAMERA -> entity+0x318); `Entity_GetBoneWorldPosition @0x545E60` (byte read `@0x545F5B`); render row `tank @0x82CFF0` -> `0x449EF0`, gate `@0x449F12..0x449F27`, swap `@0x449F29..0x449F45`. |
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
  1200-unit forward query (`push 4B00000h @0x445DD1`) is also not covered by this pass; the
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

## Tank training right-click follow-up

The single-player host queued action 6's C2S `0x16`, but
`HostRole::drain_host_client_gameplay_requests` admitted only reloads. Its
movement-only successor then discarded the selector. The host now dispatches
both gameplay requests through the existing authority handler before that drain.
This fixes repeated cannon/alternate-gun switching without replacing the mount,
sharing ammunition, or changing the configured scope binding.

The original checks the carried gun's G bit at entity `+0x326`, then queues a
two-byte selector: zero for the child slot, nonzero for the vehicle's slot
[orig: Input_HandleActionBinding_0 @ 0x4E0420, action 6 @ 0x4E0492..0x4E0526].
The authority writes the route bit and equipped-slot pointer after validating
the sender's gun and vehicle [orig: NapiNPServerMsg_HandleWeaponToggle @ 0x511A70,
selection @ 0x511AF9..0x511B38]. Both were rechecked through IDA MCP and jo-c.

`host_role` now catches the previously discarded request through the real
listen-host queue, including repeated return switches and distinct depleted
cannon/coax ammunition. The prior preservation check now uses a movement packet,
which actually belongs to the later decoder. It failed before the fix.

`godot/tests/mounted_weapon_switch_test.gd` loads the installed `07TR.bms`,
places each candidate carrier that authors a designated-G child (`addeweapG`)
unoccupied beside the authored M1A1, boards the seat that borrows that child
and drives three action-6 switches through the real simulation and
`LocalPlayerPresenter`. It checks the selected weapon, independent ammo, HUD
definition and authored sight card after each switch, reading both weapon
names from the installed definitions. Stock JO:CA/Escalation authors the
attachment on the Apache and the Ka-52 (the chin gun against the helicopter's
own rockets), so the CI data runs the whole scenario (76 assertions, headless
and D3D12); JOTAC adds its M1A1 and T80 (the cannon against the coaxial gun,
151 assertions). The D3D12 run injects real right-button press/release events
through the default binding and input router; the headless run uses the
action-6 request seam, since headless Godot cannot capture the mouse. An
install that authors no such carrier fails the test rather than pending; only
a missing `OPENNOVA_JO_DIR` pends. The carriers sit 40 units north of the
authored tank: the lake twelve units east of it drowns a placed helicopter
the moment it is claimed. No retail assets are stored in the repository.

The native `host_role` regression always runs. The original scope fallback and
the data's ordinary roof-gun seats are unchanged.

## Tank course and attachment follow-up (2026-09-21)

The authored 07TR course exposed failures that the extra unoccupied carriers
in the earlier switching test did not reach:

- GameWorld published the water plane after mission-start vehicle grounding.
  Three offshore LCACs settled on the seabed; their crews could not board and
  the route gate releasing the tank convoy never fired. Publish the plane
  before the mission-start boundary, matching the water clamp already used by
  `Game_StartMission @0x525F80..0x526071`.
- Mounted organics retained a pre-boarding ground reference. Both retail
  organic movers refresh `groundEntity` from `parentEntity` before their
  mounted branch (`0x4B40E0`, `0x4B9910`). Restore that publication so
  `PLYRONSSN @0x4F1260` can follow the gun-to-hull chain for the boarding gate.
- USE and attach labels addressed rest-pose seats. Resolve their live bone
  position through the mounted-pose provider; the original USE scorer builds
  the attachment matrix at `0x435FE7`. The authored M1A1 now boards its cannon
  when aimed at that cannon seat, rather than scoring a different hatch.
- Native userpoint PANM evaluation omitted the vehicle CTRL channels that
  rendering already received. The redirected coax muzzle therefore stayed in
  the turret's rest direction: an actual 07TR shot was about 62 degrees away
  from its sight (forward dot 0.471). Share the motor's vehicle control
  projection with native userpoint/collision and mounted-seat poses. The same
  shot now agrees with the sight (dot approximately 1.0). Retail runs the
  entity's pre-callback before resolving the weapon userpoint
  (`0x545A89..0x545A94`); the tank callback publishes turret words at
  `0x449ECF..0x449EE2`.

The installed JOTAC cannon and coax sights have the same authored zoom. Its
base definitions share `M1IRN.TGA`; `revx02` adds different hint textures
(`Coaxhnt1.tga` / `Coaxhnt2.tga`). Action 6 switches those weapon slots; it does
not transfer the player to the separate roof gun.
The existing windowed Vulkan `mounted_weapon_switch_test.gd` again passed all
151 assertions using real right-button events, including independent ammunition
and the HUD sight card on the M1A1, T80, Apache and Ka-52.

`vehicle_mount` adds regressions for a live seat outside its rest-pose USE
reach and the mounted ground relationship on both local and NPC bodies.
The 03TR mounted-view suite additionally follows NPC 1750 through 80 seconds
of takeoff, banking and flight, checking both the simulation seat and rendered
body distance from the objective helicopter. All five mounted-view tests
passed (91 assertions). The reported NPC detachment was not reproduced before
these changes; that coverage records the tested flight rather than attributing
an unobserved failure to one fix.

`godot/tests/tank_training_test.gd` loads the authored course through GameWorld,
boards the live cannon seat with USE, switches/fires both guns, follows the
instructor and landing-craft convoy, and destroys the enemy vehicles with
normal cannon input. The assertions reach BMS events 92 and 93 and team-1
victory. The final windowed Vulkan run passed all 34 assertions; an earlier
headless run also reached victory. Only player positioning and aim use the
existing debug seam; no
health, deaths or objective completions are injected. It selects installed
`revx02` when available (the configured JOTAC game and reference course),
otherwise base JO. The separate switching suite continues to discover all
four authored carriers across the installation's mounts.

An additional JOTAC **base-mount** run exposed a separate convoy failure:
allied tank 34 fell into the water near the landing point and never released
the APC wave (event 48). The mission and carrier models match `revx02`, but
the base LCAC authors water speed 130 and mass 160 versus 74 and 87 in
`revx02`. This data variant remains an open comparison against the original
runtime; the complete-course victory witness here is for `revx02`.
