# Tanks — reverse-engineering record

Tank audit of the portable engine and its Godot presentation:
`engine/runtime/world` (the tank contact solve, the tank mover, vehicle sound,
the mounted camera, AI drivers and gunners, damage, run-over and seats),
`engine/runtime/controls` (action signs, the wheel remainder),
`engine/runtime/hud` (optical text, the vehicle panel), `engine/runtime/mission`
(seat extraction, AI slot seeding), `godot/src/player/local_player_presenter.cpp`,
`godot/src/player/player_input_router.cpp`, `godot/src/hud/hud_inset_scope.cpp`,
`godot/src/util/axes.h` and `godot/game/world/game_hud_presenter.gd`. Binary:
retail **Jointops.exe**, IDB `Jointops.exe.kong.i64`; all addresses below belong
to that binary, and every code address lands on an instruction head (`0x816A1C`
and `0x816A88` are action-catalog rows). The 2026-09-22 PR #671 pass and
its same-day review fix round used read-only disassembly; no IDB changes were
made.

The review fix round's findings were ported in the same PR. They are recorded
below as witnessed and ported behavior, not as divergence rows.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Wheel/action/variable optic route | **MATCHING (behavioral proof)** for the action signs, whole wheel notches, authored clamps and each slot's zoom seed | `player_actions` (`test_wheel_remainder_dispatches_whole_notches`), `local_player_view`, `weapon_inventory` (`test_fill_seeds_the_slot_zoom`), `vehicle_mount` (`test_prepare_vehicle_weapon_slot_seeds_the_zoom`), `player_loadout`, `mission_kernel`, `npruntime_weapon_table`; GUT `tank_parity_test.gd` (installed M1A1/T80 driver, cannon and roof seats, default and remapped physical wheel), `local_player_presenter_test.gd` (`test_wheel_factor_accumulates_whole_notches`) |
| Contact solve: support, sinks, crash, wreck and wall machinery | **MATCHING (behavioral proof)** for the branches in the witness map | `vehicle_followups` (seventeen tank cases, including `tank_crash_landing_rebounds_and_latches`, `tank_wreck_latch`, `tank_wall_contact_stops_hull`, `tank_inverted_hull_rights_itself`), `vehicle_suspension` (`test_tank_wheel_suspension_forces`, `test_fit_seed_skips_latched_arms`, `test_crash_arm_quad_uses_the_box`), `ground_conform` |
| Tank mover: impulse, crash stop, traction arms, trails, slope, chase, dispatch | **MATCHING (behavioral proof)** | `vehicle_motor` (`test_tank_impulse_pushes_velocity`, `test_tank_crash_stop_needs_2ef`, `test_tank_crashed_vertical_velocity_arms`, `test_trails_sample_only_in_contact_arms`, `test_slope_factor_samples_quantized_table`, `test_tank_npc_parent_coast_keeps_raw_servo`, `test_tank_and_bike_ignore_physics_selector`, `test_client_chase_family_gates`, `test_prediction_keeps_player_control_tail`, `test_mover_prologue_stamps_saved_live_pose`); track phases rechecked `@ 0x489F6E` |
| Tank sound: pivot, tread, tumble, detach stop and the last-tick gate | **MATCHING (behavioral proof)**; D-SND-17 is closed | `vehicle_motor` (`test_tank_pivot_sound_latch_and_loop`, `test_tank_tread_sound_accumulates_even_ticks`, `test_catch_up_ticks_skip_the_movement_fold`, `test_crash_settled_tank_keeps_lane_anchor`, `test_claimant_detach_clears_motion_lanes_and_plays_stop`), `vehicle_suspension`, `vehicle_followups` |
| Mounted camera composition | **MATCHING (behavioral proof)** for the compose legs, the seat's full-width roll, the chase look-ahead, the target nudge, the march gate and the ground/person legs | `local_player_view` (`test_chase_lookahead_follows_the_hull_on_every_compose`, `test_seat_bone_pose_rider_legs`, `test_unadmitted_seat_takes_the_ground_or_person_leg`, `test_shake_turns_the_bam_heading`), `player_view` (`test_mounted_lookahead_eases_on_every_compose`, `test_compose_camera_mounted`), `local_player_targeting` (`test_camera_leg_aims_the_fire_pose_from_the_composed_eye`), `mission_kernel`; GUT `simulation_test.gd` |
| Godot camera stamp and HUD snapshot | host code / not grillable; **behavioral proof** that both cameras stamp the composed angles and every reader observes one composed view | GUT `local_player_presenter_test.gd` (`test_camera_direction_keeps_precision_far_from_origin`, `test_camera_keeps_its_heading_looking_straight_up_or_down`), `game_hud_presenter_declutter_test.gd` (`test_hud_uses_the_presented_camera_frame`, `test_inset_camera_direction_keeps_precision_far_from_origin`) |
| HUD text and the vehicle panel | **MATCHING (behavioral proof)** for per-call CRT sprintf, the panel's seat-slot gate, emplacement labels, the silhouette window and integer screen mapping | D-HUD-30 in the [HUD record](../interface/hud-re.md); `hud_frame_compiler`, `hud_game_text`, `hud_combat` (`integer_screen_mapping`), `vehicle_panel_feed`, `hud_vehicle_panel` |
| Seats, cannon/alternate weapon, roof gun and panel routing | **MATCHING (behavioral proof)** for tested routes and the load-time seat walk | `mission_seat_spec_extract`, `vehicle_mount`, `vehicle_panel_feed`, `hud_vehicle_panel`, `special_weapon_parity`, `host_role`; installed `tank_parity_test.gd` and `mounted_weapon_switch_test.gd` |
| AI drivers, gunners and vehicle AI slots | **MATCHING (behavioral proof)** for the legs in the witness map | `vehicle_mount` (`test_ai_drive_budget_divides_first`, `test_zero_health_hull_keeps_its_driver_leg`, `test_submerged_player_driver_takes_the_ai_leg`, `test_mover_command_registers_are_the_brain_words`), `vehicle_motor`, `infantry` (`test_aim_lead_uses_the_target_saved_live_pose`, `test_body_tick_stamps_saved_live_pose`, `test_mounted_gunner_aims_in_the_parent_frame`), `mission_promote` (`test_vehicle_records_seed_the_ai_slot`), `mission_item_traits`, `event_runtime_bms`, `ai`, `destruction` |
| Damage: kill zones, blast legs, knife, run-over and occupants | **MATCHING (behavioral proof)** | `projectile_combat` (`test_jox_tank_round_bullets_class_splashes`, `test_impact_producers_apply_their_own_gates`, `test_armed_expiry_detonates_only_a_kill_zone_class`), `destruction` (`test_blast_on_a_crewed_vehicle_scales_by_occupants`, `test_blast_respects_the_damage_disabled_word`, `test_zero_damage_blast_still_runs_the_item_leg`, `test_person_blast_quadrant_faces_the_blast`, `test_knife_kill_zone`), `collision` (`test_run_over_spares_a_protected_player`, `test_run_over_kills_an_enemy_and_plays_the_bump`), `vehicle_collision_damage` |
| Authored 07TR progression | Passing assisted integration, **not normal-input completion proof** | `tank_training_test.gd`: real mount, rounds, damage, BMS destruction/victory; debug positioning/aim and final APC positioning remain in the fixture |
| Multiplayer carrier model | Passing portable regressions and OpenNova host/joiner admission; mounted live retail matrix **unverified** | `netsim_vehicle_carrier_prediction`, `netsim_vehicle_compact_carrier`, `netsim_client_replica_pipeline_target_carrier_follow`, `host_role`; see acceptance limits below |

## Witness map

### Input and optics (D-CTRL-5)

The default catalog labels action 212 "Cycle Weapon Prev" (`cycleweaponP`, row
`0x816A1C`: VK 0xDB `[`, mouse mask 0x400 = wheel up) and action 214 "Cycle
Weapon Next" (`cycleweaponN`, row `0x816A88`: VK 0xDD `]`, mask 0x800). Action
212 passes +1 to `Player_CycleWeaponSlot @ 0x4DFE70` (`push ebx`, ebx = 1
`@ 0x4E0446`; call `@ 0x4E1341`) or +2 to `Player_AdjustWeaponElevation
@ 0x4DBDF0` (push `@ 0x4E13D5`, call `@ 0x4E13D7`). Action 214 passes -2 (push
`@ 0x4E1394`, call `@ 0x4E1396`) or -1 (call `@ 0x4E13A4`). The elevation leg is
the variable optic: it is taken only when the equipped def's +0x98 and +0x90
differ and `Player_CanFireWeapon` passes, and the slot word it steps is clamped
to [`scope_min_mag`, `scope_max_mag`] with a click on change. Binoculars and a
fire charge refuse both actions. Our action signs were inverted; the fix sits in
action resolution, so remapping and keyboard use keep working.
[orig: Input_HandleActionBinding_0 @ 0x4E0420]

The same signs drive infantry weapon cycling. Wheel-up / `[` (212, +1) steps to
the next-higher weapon slot, as retail does, even though the catalog calls it
"Prev". [orig: Player_CycleWeaponSlot @ 0x4DFE70, `add edi, ebp` @ 0x4DFEF6,
wrap @ 0x4DFEF8..0x4DFF08]

Wheel notches accumulate. Each `WM_MOUSEWHEEL` adds its signed HIWORD delta
(`@ 0x7614AB`) to a persistent remainder (`@ 0x7614AE`). Every whole +120
dispatches event 0x100 (call `@ 0x76158F`) and every whole -120 event 0x200
(call `@ 0x7615C0`). The remainder survives across messages and is cleared only
at process start (`Input_ResetMouseState @ 0x761260`, `@ 0x76127B`). The in-game
callback maps 0x100 to binding mask 0x400 and 0x200 to 0x800
(`try_dispatch_binding_by_weapon_type @ 0x4992FC..0x499327`).
`controls::WheelRemainder` feeds `lround(factor * 120)` per Godot wheel event and
dispatches one binding per whole notch. [orig: Input_DispatchMouseEvent
@ 0x761470, `@ 0x761575..0x7615DB`]

Every weapon slot starts at its def's seed. The optic word (+0xC) takes the
second `scope_max_mag` value (Def+0x94, `@ 0x53EF2D`, store `@ 0x53EF35`); below
the floor Def+0x98 it takes the floor without a max check (`@ 0x53EF38`),
otherwise it caps at Def+0x90 (`@ 0x53EF3A..0x53EF44`). An owned sniper slot
without the scope-zoom permission locks the floor to the max
(`@ 0x53EEF9..0x53EF27`). Emplacement and turret slots are built with no owner
(`push 0` `@ 0x546706`, call `@ 0x54670A`), so they skip the lock (`@ 0x53EEF7`).
The HUD magnification text reads the raw word (`@ 0x59E99E`). The port seeds the
inventory fill, a fresh install and `VehicleSystem::prepare_weapon_slot`,
restores the word at the mount and writes each zoom step back. JOTAC cannon
optics therefore start at 2x and its RCWS roof optics at 1x; stock
`WPN_M1TURRET` / `WPN_T80TURRET` (`scope_max_mag 10 2`) start at 2x. Before the
fix the vehicle slot was zeroed and the lazy getter (`@ 0x4DC6DB..0x4DC6E1`)
seeded the maximum. [orig: WeaponSlot_InitFromDef @ 0x53EE70;
WeaponSlot_InitFromEntityDef @ 0x5466C0]

A follow-up checked the Windows mouse-input path as well as injected Godot
events: downward wheel input widened the displayed vertical FOV from 6.87 to
9.64 to 16.19 degrees; upward input narrowed it back. The user also confirmed
the physical wheel direction in the restarted build. The native regression
resolves the default binding through the action catalog into the optical route,
including both bounds and the click on change. The installed regression reads
each weapon's own minimum and maximum; stock roof guns keep their fixed optics.

### Contact solve (D-VEH-4)

The tank contact solver has four wheel probes, six belly stations and three
spine probes. Its seven support values are max(wheel, corresponding belly) for
entries 0-3 and the spine values for 4-6. Those merged values decide whether a
corner's free-fall sink grows, whether grounded catch-up runs, and whether the
sink clears at the tail. Raw wheel depths still feed compression and spring
energy. Using raw wheel contact for both purposes let a belly-supported corner
accumulate a false fall. [orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0,
merge @ 0x4784CC, growth @ 0x478510, catch-up @ 0x478DA4, tail @ 0x47931B]

The diagonal support check clears all four sinks after the grounded fit. Its
threshold is zero for the crash-settle latch (+0x2F0) and 250 otherwise. Each
corner's drop adjustment feeds both fits, so an airborne hull tilts toward the
falling corner instead of holding its attitude until contact. The airborne fit
is `Entity_ComputeSuspensionAndOrientation`'s call `@ 0x478B1C`: the lift path
passes the slot records (`@ 0x478ACA..0x478AF5`), the no-lift path passes NULL
(`@ 0x478AF7..0x478B1B`). [orig: Entity_ProcessWheeledVehiclePhysics
@ 0x475DE0, diagonal reset @ 0x478FF2..0x479040, adjusted corners
@ 0x478834..0x47884D; Entity_ComputeSuspensionAndOrientation @ 0x4698A0]

The review fix round ported the rest of the solve's crash, wreck and wall
machinery:

- **Two depth views.** Pass one writes the thirteen probe records and a depth
  copy; pass two overwrites the records and averages into the copy only, and
  the mass share reads the pass-two hit. The records feed the slot merge, the
  maxes, the crash and wheel depths, the springs and the lifts; the averaged
  copy feeds only landing, crush and the wreck latch. [orig: @ 0x476A8E,
  copy @ 0x476A93..0x476B3A, pass two @ 0x477037, averages @ 0x4770BB, mass
  share @ 0x47720D]
- **Wall bytes.** Only a probe that struck a steep model face carries a wall
  byte; terrain never does. Any wall byte zeroes velX/velY/speed, a reverse hit
  lies past -0.75, and the head-on test past -0.871 repeats the stop. The
  authority head-on damage is dead in retail: its operand is the just-zeroed
  speed. [orig: Entity_CheckCollisionState @ 0x462A30; walk
  @ 0x477940..0x477AF8, reverse @ 0x477AD3, stop @ 0x477B7B..0x477B87,
  head-on @ 0x477B8D..0x477C09, `xor eax, eax` @ 0x477C90]
- **The stability byte +0x2F2** (`VehicleMotorState::grounded`) needs
  `up.z > 0x2000`, a symmetric leading axle with no reverse hit, and none of
  +0x2EC, +0x2F0, +0x2FC. A crashed upright tank therefore loses it and the
  mover's crashed off-contact arm settles the hull. [orig: @ 0x477CF9..0x477D2F,
  gates @ 0x477D2F..0x477DA5; consumers Entity_UpdateTankVehiclePhysics
  @ 0x489F2B, @ 0x489FFF, @ 0x48A68C]
- **Order.** Landing, crush and the crash state all run before the sink
  growth, and crash tests (a)/(b) run before the wall walk. [orig: landing
  @ 0x477DAC..0x477EEE, crush @ 0x478024..0x47809D, crash state
  @ 0x4780CD..0x478381, tests @ 0x477760..0x4777BF]
- **The wreck latch.** An upright crash-settled hull, or an inverted hull with
  +0x2EC and +0x2EF, whose averaged diagonal pair or middle spine touches sets
  +0x2FC, +0x2EC and +0x2EF, zeroes the sinks and clears the suspension state.
  The solve head then forces +0x2EC = +0x2F0 = 1 every tick while +0x2FC holds.
  [orig: @ 0x47853C..0x478643, spine byte @ 0x4784C0..0x4784CA; head
  @ 0x47604E..0x476067]
- **The airborne branch.** A crashed or inverted hull lifts by the largest
  slot depth (`@ 0x478720..0x478735`); a crashed lift arms +0x2EF
  (`@ 0x4787B3..0x4787E5`). A lifted crashed hull in the air rebounds
  (slideDecay = min(|slideDecay >> 1|, 0xA000), `@ 0x4788A4..0x478924`) or stops
  its fall (`@ 0x478929`); otherwise it plays the skid cue and enters the crash
  state (`@ 0x478958..0x4789DD`). Without a lift, a client-side hull strikes
  the ground through `Entity_ApplyWheelSuspensionForces` (call `@ 0x478A9F`),
  then the in-air tail clears brain+0x318 bits 6-7 (`@ 0x478AAE`), sets Flags
  0x2000 (`@ 0x478AB5`) and clears +0x2EF (`@ 0x478ABC`).
- **The grounded crashed block.** On the grounded entry a crashed hull that
  was in the air and still falls fast (|slideDecay| > 0x1000, not
  crash-settled) rebounds, applies the wheel suspension forces (call
  `@ 0x478C4D`) and clears the retained forces (call `@ 0x478C5C`); any other
  crashed hull re-enters the crash state (+0x2EF = +0x2EC = 1 with a cleared
  suspension state, `@ 0x478C80..0x478CCC`). The in-air flag then clears
  (`@ 0x478CD3`), and a supported averaged diagonal without +0x2FC sets
  +0x2F1 (`@ 0x478CDA..0x478D07`). The client crash window runs only on this
  entry, after the growth (`@ 0x478B6C..0x478BD6`, signed age compare
  `@ 0x478BB5..0x478BBE`). [orig: @ 0x478BDD..0x478CCC]
- **`Entity_ApplyWheelSuspensionForces`.** A missing input only enters the
  crash state. Otherwise the first strike in each speed band plays one tumble
  cue: |speed| > 0x4000 slot 47 with latch 0x40, > 0x3000 slot 48 with latch
  0x80, else slot 49 unless latch 0x80 is set. Each wheel then queues one
  force: a supported wheel presses along -up at 8000 - trunc(tilt * -3000), a
  clear wheel lifts along +up at 800 - trunc(tilt * -1000), tilt = |forward.z|.
  [orig: Entity_ApplyWheelSuspensionForces @ 0x463560, `@ 0x4635A4..0x463634`,
  `@ 0x463659..0x46369A`; Entity_QueueSuspensionForce @ 0x45C0B0]
- **Tails.** The airborne branch jumps to `0x479445` (`@ 0x478B54` /
  `@ 0x478B64`), and the crashed or inverted grounded path takes
  `@ 0x479311..0x479318`; both skip the compression release loops, so only
  the upright uncrashed grounded tail releases compression into each positive
  terrain gap (`@ 0x4790A1..0x4791B3` settled, `@ 0x479205..0x47930F`
  otherwise). The common tail rights a penetrating inverted hull without
  Flags 0x10 through `Entity_RebuildOrientationMatrixFromAxes @ 0x4632E0`,
  on both branches. [orig: righting @ 0x47948A..0x4794A8; 13-record max
  @ 0x47849B..0x4784B7]
- **Sink stores.** The only tank sink writes are the crashed low-speed clear
  (`@ 0x47810D`), the wreck latch (`@ 0x4785E6`), the diagonal reset
  (`@ 0x47902E`) and the tail (`@ 0x47931F`).
- **The sleep gate and the solve head.** The sleep gate compares X, Y, Yaw,
  Pitch and Roll against savedLivePose (`Transform_ComparePartial @ 0x459180`,
  call `@ 0x475EFB`). Before it, a hull at or below the def's critical drain
  releases its +0x1CC smoke emitter (`@ 0x475E25..0x475E65`). After the landing
  loop the solve builds the quad (call `@ 0x477FA6`) and consumes queued wheel
  forces through `Entity_ClearSuspensionForces` (call `@ 0x477FB6`).
- **The shared fit.** A pending crash request with the client missing Flags
  0x10 skips every latched arm and fits only when grounded (tank
  `@ 0x469933..0x469989`, tracked `@ 0x46B1A6..0x46B200`). The crash arm's
  quad uses the collision box spans, halved by the quad builder (tank
  `@ 0x469B93..0x469BA5`, tracked `@ 0x46B449..0x46B462`,
  `Entity_ComputeBoundingQuad @ 0x45B8E4..0x45B9A3`).

A 5,000-tick JOTAC training capture using the same assisted boarding setup
measured maximum consecutive camera pitch change of 1.42027° before and
0.46922° after the first-pass contact correction; roll fell from 2.10243° to
0.87620°. The changed physics also changes the route, so these are not
matched-position samples or retail traces.

### Tank mover

- **Recoil and heavy-hit impulse.** While the +0x3EC direction is nonzero,
  dir * trunc(+0x3FC * 28.16f) joins vel_x, vel_y and slideDecay every tick
  (Q16, round half up), after the crash-settle zero and before integration;
  the direction clears once +0x3DC drops. Only the tank mover reads it.
  [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, `@ 0x48A8C0..0x48A9C0`]
- **Crash stop and traction arms.** The crash stop needs +0x2EF as well as
  +0x2EC (`@ 0x489C06..0x489C1C`); the ground core stops on +0x2EC alone
  (`@ 0x48C086..0x48C08F`). The recovery arm stores slideDecay unconditionally
  (`@ 0x48A5D4`); only the straight arm keeps a crashed hull's vertical
  velocity (`@ 0x48A28D..0x48A2AE`). The sharp-steering caps are
  `@ 0x489DB1..0x489E97`.
- **Trails** sample only at the end of the contact arms; the airborne and
  off-contact arms jump past them (`@ 0x48A60D..0x48A67F` vs `@ 0x48A684`).
- **Slope factor.** The mover samples the cos table at
  (Pitch + 0x200000) >> 22 (`@ 0x489CE6..0x489D01`), not a continuous cosine.
- **NPC-parent coast.** A zero-command tank whose +0x16C parent is an NPC keeps
  the raw servo (`@ 0x489EBA..0x489ECB`).
- **Dispatch.** The ctank class row calls its mover without testing the
  physics selector (`Entity_DispatchPhysics_ctank @ 0x48F000..0x48F007`).
- **Client chase.** A crashed, crash-settled or +0x2FC hull widens the snap
  radius to 0xC0000 (`@ 0x48913B..0x48916E`); heading snaps and steps only
  while unlatched (`@ 0x4891D5..0x489208`, `@ 0x48933D..0x48935B`); Z snaps
  always and steps only while airborne and unlatched (`@ 0x489379..0x48939A`).
  The other families have their own gates (vehicle record section 10).
- **Role gate.** Only the input block is role-gated (`@ 0x489522..0x489545`);
  the seat sweep and the PlayerControl tail (claimant fold gate
  `@ 0x48AAC4..0x48AADA`, engine start/stop and part spin
  `@ 0x48AD94..0x48AE3D`) run on predicting clients.
- **Entry pose.** The prologue copies the pose to savedLivePose before its
  first bail on every role (`@ 0x488B24..0x488B50`); `tick_motor` and
  `ground_client_tick` stamp after their seed.
- **Track phases** add `((speed << 12) - yawRate) << 3` and
  `((speed << 12) + yawRate) << 3` before the velocity solve
  (`@ 0x489F6E`).

### Tank sound (D-SND-17 closed)

The fold's seventh argument (`extraEffectId`) is the absolute **yaw rate** at
`entity+0xA4` while brain `+0x318` bit `0x20` is set; vertical velocity is the
separate `entity+0xA0`. The older D-SND-17 text called it `slide_z`. The track
phase calculation independently identifies `+0xA4` as yaw rate.
[orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, argument @ 0x48AAE0 and
track phases @ 0x489F6E]

After the movement loop, an occupied tank with zero speed, nonzero yaw rate and
a heading error strictly greater than `0x071C71C0` arms the latch and plays
profile slot 46 (`swivel_shift`). The latch clears when the tank moves (any
nonzero speed), its yaw rate becomes zero, or the rate's sign changes from the
previous tick. Crash-settled tanks preserve the latch and still store the
previous rate. This order means the first continuous registration is on the
next tick. [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, @ 0x48ACB5
through @ 0x48AD53]

The fold registers authored `Soundloop_4` on lane 40, unity pitch, a 30-tick
lifetime and signed-clamped low-word volume. It reaches this branch only after
registering idle; collision's full idle reaches it, while a stop, zero
reference speed or full-speed idle rejection does not. [orig:
Entity_ProcessMovementSoundEffects @ 0x5294A0, @ 0x5297C8 through @ 0x5298C6]

Every even tick the tank adds its speed and the previous even tick's speed into
brain+0x320, dropping bit 31 of the doubled sum. At or past +/-0x80000 it plays
profile slot 45 (`drive_repeat`) on the hull and restarts. The tread cue is
independent of occupancy, the crash-settle latch and the last-tick gate.
[orig: Entity_UpdateTankVehiclePhysics @ 0x48AE45..0x48AEA1]

The `dword_24E0E80` gate is the outer loop's last-tick-of-batch flag: 1 after
the final quantum of a frame's catch-up batch, 0 on earlier quanta and in the
cinematic fixed-step mode, which the port does not have. The port carries it as
`World::rules.last_tick_of_batch` and applies it at every fold site; the tank
gate skips the fold, the reverse, high-rev, skid and pivot sections and the
+0x328 prior-rate store, while the rev timer and the tread cue stay ungated.
[orig: Game_MainLoop, flag @ 0x52BA24..0x52BA3A; tank `jz` @ 0x48AAA5; folds
@ 0x46F7C8, @ 0x471523, @ 0x48670A, @ 0x48AA9E, @ 0x48D156, @ 0x48EDFF,
@ 0x48FDD3]

A crash-settled tank skips the fold (`@ 0x48AABE`) but still refreshes the
source-only anchor while a registered lane lives. A claimant detach clears the
motion lanes and plays slot 31 on the departing occupant while its eye clears
the water plane; it leaves the +0x318 latch untouched, so the mover's leave
edge plays a second slot 31 on the hull (+0x18000) the next tick. The detach
also releases the +0x1CC smoke emitter. [orig: Entity_DetachFromVehicle
@ 0x4355F0, `@ 0x4356EF..0x435759`, occupant stop @ 0x43571B..0x43573E; tank
leave edge @ 0x48ADE9..0x48AE33]

The crash-entry skid cue (slot 25, `@ 0x478958..0x478987`), the airborne
rebound cue (slot 49 unless latch 0x80, `@ 0x478915`) and the banded tumble
cues of `Entity_ApplyWheelSuspensionForces` are the tank solve's sounds; the
in-air tail clears their latches (`@ 0x478AAE`).

### Camera and HUD ownership (D-VEH-5, D-HUD-29)

Mounted view position and orientation come from the posed camera/userpoint
frame, including hull and gun transforms. [orig: Camera_ComputeThirdPersonView
@ 0x437D10; Entity_GetBoneWorldPosition @ 0x545E60]

- **Full-width roll.** First-person mode copies the rider's own triple; the
  roll is the rider's BAM32 word (`mov ecx, [esi+18h]` `@ 0x437D86`). The seat
  carry writes it at full precision and restores only the look yaw/pitch
  (`Entity_AttachToBoneAndUpdateTransform @ 0x5463D0`, euler `@ 0x54656F`,
  restores `@ 0x546661` / `@ 0x546664`). The tank callback writes only the
  position (`@ 0x44A264..0x44A292`); the troop callback copies the carrier's
  three words (`@ 0x4DC732..0x4DC741`); the no-skeleton EWEAP copy adds the
  rider's +0x94 word to the pitch (`@ 0x545F48..0x545F4E`).
- **Compose legs.** Every compose advances the first-person shake filters and
  the mounted chase look-ahead. Retail composes once per logic quantum
  (`@ 0x526781`), once per rendered scene frame (`@ 0x5CA34D`) and once more
  for the Inset scene (`@ 0x5C9841`); the port composes on exactly those legs
  (`LocalPlayer::tick_view`, `LocalPlayer::present_view_frame`), and every
  other reader observes the last composed view. The binocular latch is owned
  by the rendered frame (`@ 0x5CA3E1..0x5CA3F3`). [orig: shake filter
  @ 0x43803C]
- **Look-ahead.** Each non-first-person compose with a parent in seat 2 or 5
  eases `lookahead += (target - lookahead + 16) >> 5` toward the carrier's full
  rotation matrix times (0x60000, 0, 0), so a pitched hull tilts it.
  [orig: gate @ 0x438811..0x43882B, target @ 0x438855, ease
  @ 0x43885A..0x4388AF]
- **Chase target and march.** The pivot, the anchor matrix's image of
  (0x2000, 0x2000, 0x2000), is the look-at target (`@ 0x43817E..0x4381D9`); the
  eye is the image of (-dist, 0, 0) (`@ 0x4383E0..0x4383FB`). The collision
  march runs with proximity candidates (`@ 0x4381CB`) below 8.0 units
  (`@ 0x4381E3`) for both arms, and the final yaw/pitch look from the eye at
  the target (`@ 0x4387DF..0x43892D`).
- **Ground and person legs.** A person whose seat neither carrier leg admits
  takes the ground-entity leg (`@ 0x437EB5..0x437F97`), then the person leg,
  which clears the ground lift (`@ 0x437F9C`); a non-person def jumps to the
  shake under its own triple (`@ 0x437E99`).
- **Shake heading.** Both shake legs add their yaw delta to the BAM heading
  (`@ 0x4380D9`, `@ 0x43898B`), so the mission yaw takes the negated delta.
- **Aim acquisition.** The third-person/no-weapon camera leg aims from the last
  composed eye toward the fire pose's far point and composes nothing (gate
  `@ 0x4B4E8F..0x4B4EA1`, leg `@ 0x4B4F90`; `Entity_BuildCameraView
  @ 0x4B0E30`).
- **Godot stamp (D-VEH-5).** The stamp formed `eye + forward` and recovered the
  direction through `look_at`; single-precision subtraction at large world
  coordinates quantized it. Both the gameplay and Inset cameras now build their
  basis from the composed yaw, pitch and roll (`mission_view_transform`,
  `godot/src/util/axes.h`), which keeps the heading when looking straight up or
  down. Retail builds the view from the euler triple with no look-at pole
  (`Viewport_BuildProjectionMatrix @ 0x410FB0`, rotations
  `@ 0x4112A4..0x4112EB`). No interpolation or extra smoothing was introduced.
- **HUD snapshot (D-HUD-29).** `LocalPlayerPresenter.presented_view()` exposes
  the composed frame; the live HUD and the tank probe consume it, and
  standalone HUD fixtures observe the last composed view. Weapon-event
  placement, the camera-mode refresh and the aim acquisition compose nothing.

### HUD text and the vehicle panel (D-HUD-30)

Every localized HUD template goes through CRT sprintf semantics with its own
call's argument list. The installed `Overlays/STROVER_DIST` is
`Distance: %ldm`; the over-1km, auto and none labels are sprintf'd with no
argument, so `%%` collapses. [orig: HUD_DrawScopeOverlayDetails @ 0x59E420,
sprintf @ 0x59E530 (1 m floor push @ 0x59E4E7), 1 km label @ 0x59E4D5;
HUD_RenderAllOverlays @ 0x5A8070, sprintf @ 0x5A8972]

The panel draws only on the slot-1, slot-2/5 and slot-3-with-weapon-group arms
(`HUD_RenderOverlays @ 0x5A7CAE..0x5A7D23`). The emplacement digit redraws once
per following list entry, the silhouette takes the bordered texture window
(`draw_textured_quad_with_border @ 0x590C40`), and projected cues map to the
1024x768 design space with integer rounding (`Viewport_ScreenToVirtual
@ 0x5D2C70`). The [HUD record](../interface/hud-re.md) owns the witnesses.

### AI drivers and gunners

- **Boarder hold.** The drive leg's wait-for-boarders stop reads the walker's
  AiSlot (+0x68) words +0x94/+0x98, never the brain. [orig: ctan
  @ 0x489B8D..0x489BA6; cveh @ 0x48BFC1..0x48BFDA; writer WacCmd_SsnToSsn
  @ 0x4F73E4..0x4F73F7]
- **Turn budget.** `(|Yaw - bearing| / ((brain[35] >> 15) + 32)) << 5`, a
  signed 32-bit divide before the shift. [orig: ctan @ 0x48988E..0x4898A2]
- **Route writers.** `Entity_SetWaypointByTeam @ 0x43CD20` (group redirect;
  pool 0 detaches mounted non-players through the authority-only detach and
  resets +0x128/slot+0x90, pool 1 writes only the slot, Flags and brain copy),
  `Entity_SetWaypointForTeam @ 0x43DD00` (single redirect: the first pool-0
  match without a budget, else every pool-1 match with one) and
  `WacCmd_SsnToWp @ 0x4F1CE0` (nearest node, no detach or resets) are three
  writers, ported as `group_to_waypoint`, `redirect_ssn_to_waypoint` and
  `set_ssn_waypoint`.
- **Submerged player driver.** A player driver whose eye is below the water
  hands every ground-template family to the AI leg, not only the boat. [orig:
  ctan @ 0x489579..0x489585]
- **Parked split.** The drive leg parks on no occupant (+0x170) or Flags
  0x10000002, never on zero health alone. [orig: ctan @ 0x48954B..0x489560]
- **Mover command registers.** The tank mover keeps steer target and command
  speed in brain+0x210 and +0x220 (`@ 0x489952`, `@ 0x489811`); the port mirrors
  its motor state back into brain[132]/[136]/[137] after the motor. Patrol
  arrival reads the def +0x924 turn-rate word live (`AI_UpdatePatrolBehavior`,
  `@ 0x457DCB..0x457DD4`).
- **Gunner lead.** Every body stamps savedLivePose at mover entry, and the lead
  is `target + lead * (target - target.savedLivePose)` with the led point stored
  at +0x30C; a never-stamped target leads by zero. [orig: block 1
  @ 0x4BC6FB..0x4BC798; block 2 @ 0x4BCAFE..0x4BCB69; stamps @ 0x4B9A53..0x4B9A6E,
  @ 0x4B4187..0x4B419C]
- **Mounted aim arm.** A parentSlot-3 gunner rotates the aim delta into the
  parent's Yaw/Pitch/Roll frame, solves bearing and elevation there and adds
  the parent Yaw/Pitch back, so a turret on a tilted hull aims true.
  [orig: @ 0x4BCD1C..0x4BCF24]
- **Fire gate.** The mounted fire request tests only for a null target.
  [orig: @ 0x4BF4CF..0x4BF4D4]
- **AI slot seed.** Every record whose def carries AI attrib 0x100000 gets an
  AiSlot, vehicles included: slot+0x48 = 62 times the authored spawn count
  (record +0x3E), and slot+0x8C/+0x94/+0x98 hold the route. The respawn budget
  `thinkCooldown` = slot+0x48 / 62 (a signed divide) is therefore the spawn
  count, and a dead hull's respawn route comes from the slot words. [orig:
  Entity_SpawnFromBMSRecord gate @ 0x40ED4E, fills @ 0x40EFE4..0x40EFF4 and
  @ 0x40F02F..0x40F054; Game_StartMission @ 0x526079..0x52608F;
  AI_TickState_VehicleDead @ 0x468005..0x46801D]
- **Respawn class init.** `Entity_RespawnVehicle` re-runs the class's ItemDef
  +0x148 init (`Entity_InitVehicleAIFromDef @ 0x4686C0`: route, ammo, speeds,
  turret, stagger, state enter, gunner setup) and zeroes the kill credit
  +0x178. [orig: Entity_RespawnVehicle @ 0x45FF40, `@ 0x45FF53..0x45FF68`,
  `@ 0x460074`]

### Damage, run-over and seats

- **Kill-zone producers.** `WeaponEffect_PushExplosionQueueEntry @ 0x4E83C0` is
  an ungated push; each producer gates its own call (armed expiry kztype test
  `@ 0x4E9DDC..0x4E9DE1`; terrain `@ 0x4E928C` with the arm age; item
  `@ 0x4E986E`; person `@ 0x4E9AE2` with the authority and kz_damage tests;
  water `@ 0x4E9C3C..0x4E9C62`). The drain's 2/5/6/7 arm applies weapon damage
  at kz_maxradius (`@ 0x4EADCD`), so the stock tank rounds (kztype 6,
  `rounds_kz_Bullets`) splash; a zero radius drops the entry (`@ 0x4EAE84`).
- **Blast legs.** A crewed vehicle takes the occupant damage scale
  (`@ 0x4E69C7..0x4E69D8`); the damage-disabled word +0x124 zeroes the damage
  (`@ 0x4E69B8..0x4E69C3`); zero damage still runs the person pick and the item
  tail (`@ 0x4E69FD`). [orig: Entity_ApplyWeaponDamage @ 0x4E6820]
- **Knife.** Drain type 1 is the knife kill zone (`Entity_ApplyVehicleCollisionDamage
  @ 0x4E6620`, an IDB misnomer), at kz_maxradius plus 1.0 with KnifeBonus
  (`@ 0x4EAE0C..0x4EAE34`).
- **Occupant count.** Pool-0 rows with an ItemDef and no Flags 2 count when
  their groundEntity, or its groundEntity, is the vehicle; there is no mount
  test. [orig: Entity_CountMountedEntities @ 0x435970, @ 0x4359B4,
  @ 0x4359BB..0x4359C2]
- **Run-over.** A player victim with a set +0x124 word or the spectator slot
  byte is spared (`@ 0x4B3918..0x4B3946`); the quadrant uses the victim's
  displacement minus the pusher's (`@ 0x4B395D..0x4B3987`); and both planar
  displacements above 0x3F8 play slot 38 on the victim with a 31-tick hold
  (`@ 0x4B39FD..0x4B3A4E`). [orig: Entity_MovementCollisionResolver
  @ 0x4B2BD0]
- **Seats.** The load-time walk matches `sitex`, `ctrlx` and `drvrx` as
  five-character prefixes and `UseGun` as a whole name, last match winning for
  the control and gun bones; a ninth `sitex` ends the scan. [orig:
  EntityDef_LoadModelsAndCallbacks @ 0x439F50, walk @ 0x43A47B..0x43A5CD]
- **Seat scan and bury.** The USE scan's LOS test (`@ 0x436183`) follows the
  reach and cone gates and precedes the best-score compare (`@ 0x43618F`)
  [orig: Entity_FindNearestSeatOrArmory @ 0x435D50]. The wreck bury arm samples
  `Terrain_SampleHeightBilinear @ 0x6067B0` (call `@ 0x467F3C`).

## Divergence catalog

| ID | Status | Summary | Witness |
| --- | --- | --- | --- |
| D-CTRL-5 | Minted-and-closed 2026-09-22 (FIXED) | `cycleweaponP/N` signs reversed the authored wheel and bracket actions, the tank optic direction and infantry weapon cycling; corrected in action resolution, with catalog-to-optic and remapped-device regressions. Pointer row in the [menu record](../mnu/menu-re.md). | `Input_HandleActionBinding_0 @ 0x4E0420`: 212 +1 `@ 0x4E1341` / +2 `@ 0x4E13D7`; 214 -2 `@ 0x4E1396` / -1 `@ 0x4E13A4`; catalog rows `0x816A1C` / `0x816A88` |
| D-VEH-4 | Minted-and-closed 2026-09-22 (FIXED) | Tank corner support used raw wheel contact instead of merged wheel/belly support, missed the diagonal sink clear and discarded the airborne corner correction. Pointer row in the [vehicle record](vehicle-client-movers-re.md). | `Entity_ProcessWheeledVehiclePhysics @ 0x475DE0`: merge `@ 0x4784CC`, growth `@ 0x478510`, catch-up `@ 0x478DA4`, diagonal `@ 0x478FF2..0x479040`, tail `@ 0x47931B..0x47934C`, corners `@ 0x478834..0x47884D`, fit call `@ 0x478B1C` |
| D-VEH-5 | Minted-and-closed 2026-09-22 (FIXED) | Godot camera orientation lost precision through `eye + forward` followed by world-space subtraction; both the gameplay and Inset cameras now stamp a basis built from the composed angles, including the up/down pole. A presentation precision fix for every far-from-origin view, filed under the vehicle catalog where it was found. | Retail builds the view from the euler triple: `Viewport_BuildProjectionMatrix @ 0x410FB0`, rotations `@ 0x4112A4..0x4112EB` |
| D-HUD-29 | Minted-and-closed 2026-09-22 (FIXED) | Live HUD observation recomposed the camera and advanced shake/drift state; the HUD consumes the displayed snapshot, and every non-rendering reader (weapon-event placement, mode refresh, aim acquisition, `Simulation.get_local_player_view`) observes the last composed view. Pointer row in the [HUD record](../interface/hud-re.md). | `Camera_ComputeThirdPersonView @ 0x437D10` callers `@ 0x526781` / `@ 0x5CA34D` / `@ 0x5C9841`; shake filter `@ 0x43803C` |

D-HUD-30 (the Distance text) is owned by the [HUD record](../interface/hud-re.md).
D-SND-17 is closed in [the audio record](../audio/lwf-dbf-sound-re.md): the
pivot cue, latch and fourth loop, the slot-45 tread cue and the last-tick gate
are implemented. Wider vehicle residuals remain in D-NET-161 and the
[vehicle record](vehicle-client-movers-re.md).

## Validation and acceptance limits

- The review fix round's native and GUT regressions are named per mechanism in
  the verdict table. At the PR's first pass all 23 targeted native suites
  passed: motor, support/suspension, attachments, mount/seat selection,
  emplaced guns, view/recoil, controls, panel math, host weapon switching, and
  replicated carrier prediction/compact/follow state.
- On stock `jox01`, the course/seat/mounted-switch GUT run passed four tests /
  449 assertions at the first pass. On the configured `revx02`, it passed four
  tests / 801 assertions. Both reach the authored training victory through the
  assisted course fixture. This is not proof of a human or device-only
  playthrough.
- Two windowed `tank_parity` zoom captures contain 758 and 284 frames, with
  cannon mount and posed userpoint data in every frame. The second starts at
  2x: wheel-up reaches 4x, 6x, 7x and clamps; wheel-down reaches 5x, 3x, 2x
  and clamps. The HUD screenshots show the cannon sight and hull panel. The
  probe verdict means capture succeeded; it does not mean retail parity. The
  probe also offers observe/drive/fire scenarios, optional numbered seat
  selection and explicitly reported assisted boarding. It writes JSONL and
  before/after screenshots under its probe artifact directory.
- OpenNova host/joiner admission passes on stock 07TR/COOP: the joiner reaches
  admission-complete, phase 4, with a local player and no lost session; the
  host reports one connected peer. The mounted joiner probe failed during
  assisted boarding setup before its input exercise. This proves admission,
  not mounted driving, firing or camera parity.
- Retail RR was attempted twice against isolated stock copies on 07TR/COOP,
  including a retry with the complete proxy shader deployment. The host
  reached the mission transition and bridge startup but never reported ready
  before timeout. RR is blocked, and RO has no accepted retail baseline. The
  installed bridge exposes no `onhook_join_lan`, blocking the supported OR
  path. No successful mixed-retail result is claimed.
- Exact retail-versus-OpenNova camera traces, a driver/gunner live
  four-topology comparison, normal-input victory, and side-by-side HUD
  pixel/audio comparison remain unverified.

Use `godot/probes/runtime/tank_parity_probe.gd` through `game_probe`, not a GUT
script as a live harness. On a local/authority world, its entity-card trace
includes unrounded motor BAM angles, all six wheel channels, four sink
accumulators and spring state. AI-free joiner cards expose the replica detail
without these local motor arrays. `vehicle_handle` selects a replica by its
actual packed handle when no authored mission net ID is available. Observe
these alongside camera and posed gun points to distinguish suspension,
carrier/attachment, weapon recoil and presentation errors.
