# Tanks — reverse-engineering record

Tank audit of `engine/runtime/world` (motor, contact, mounted camera, weapons,
vehicle panel and sound), `engine/runtime/controls/player_actions.cpp`, and
`godot/src/player/local_player_presenter.cpp`. Binary: retail **Jointops.exe**,
IDB `Jointops.exe.kong.i64`; all addresses below belong to that binary.
The 2026-09-22 pass uses read-only disassembly; no IDA renames were made.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Wheel/action/variable optic route | **MATCHING (behavioral proof)** for the witnessed action signs and authored clamps | Four action call sites; `player_actions`, `local_player_view`; installed M1A1/T80 driver/cannon/roof seats, default and remapped physical wheel in `tank_parity_test.gd` |
| Tank support and free-fall fit | **MATCHING (behavioral proof)** for merged support, sink resets and airborne corner correction | Six instruction ranges below; red/green `vehicle_followups` cases on authority and client state; existing `vehicle_suspension`, `ground_conform` |
| Motor, traction and track channels | **MATCHING (read-only grill)** within the existing motor record's scope | `vehicle_motor`, `vehicle_part_anim`; [family motor and traction witnesses](vehicle-client-movers-re.md); this pass also rechecked track phase before the velocity solve at `@ 0x489F6E` |
| Mounted camera and Godot transform | host code / not grillable at the Godot boundary; native pose uses witnessed camera rules | `local_player_view`, `player_view`, `vehicle_attachments`, `emplaced_gun_channel`; `local_player_presenter_test.gd` 19 tests / 1,668 assertions, including six far-origin orientations |
| Optical Distance text | **MATCHING (behavioral proof)** for installed Win32 integer formats | D-HUD-30 in the [HUD record](../interface/hud-re.md); emitted-glyph regressions for scope and mortar, including `%ld`, plus the 1000m boundary |
| HUD camera snapshot | host code / not grillable; **behavioral proof** of one shared displayed context | `game_hud_presenter_declutter_test.gd` 8 tests / 164 assertions; the new test fails before the change and passes afterward |
| Seats, cannon/alternate weapon, roof gun, optics and panel | **MATCHING (behavioral proof)** for tested routes | `vehicle_mount`, `vehicle_panel_feed`, `hud_vehicle_panel`, `special_weapon_parity`, `host_role`; installed `tank_parity_test.gd` and `mounted_weapon_switch_test.gd` |
| Stationary tank pivot sound | **MATCHING (behavioral proof)** for the latch, cue and fourth loop | `vehicle_motor::test_tank_pivot_sound_latch_and_loop`; caller `@ 0x48AAE0`, latch `@ 0x48ACB5`, consumer `@ 0x529887`; D-SND-17 retains the shared sound-ready gate |
| Authored 07TR progression | Passing assisted integration, **not normal-input completion proof** | `tank_training_test.gd`: real mount, rounds, damage, BMS destruction/victory; debug positioning/aim and final APC positioning remain in the fixture |
| Multiplayer carrier model | Passing portable regressions and OpenNova host/joiner admission; mounted live retail matrix **unverified** | `netsim_vehicle_carrier_prediction`, `netsim_vehicle_compact_carrier`, `netsim_client_replica_pipeline_target_carrier_follow`, `host_role`; see acceptance limits below |

## Witness map

### Input and optics

The default catalog gives `cycleweaponP` the up-wheel mask `0x400` and `[` key
(row `0x816A1C`), and `cycleweaponN` the down-wheel mask `0x800` and `]` key
(row `0x816A88`). Action 212 passes +1 to weapon cycling or +2 to the variable
optic adjustment. Action 214 passes -1 or -2. Our action signs were inverted;
the existing optical route already interpreted positive as increasing
magnification. The fix belongs in action resolution, preserving remapping and
keyboard behavior. [orig: Input_HandleActionBinding_0 @ 0x4E0420, calls at
@ 0x4E1341, @ 0x4E13D5, @ 0x4E13A4 and @ 0x4E1396]

A follow-up checked the Windows mouse-input path as well as injected Godot
events: downward wheel input widened the displayed vertical FOV from 6.87 to
9.64 to 16.19 degrees; upward input narrowed it back. The user also confirmed
correct physical wheel direction in the restarted build. No additional input
mapping change was needed in that follow-up.

The native regression resolves the actual default binding through the action
catalog into the optical route, including both bounds and click-on-change.
The installed regression reads the weapon's own minimum and maximum: JOTAC
cannon optics start at 2× and roof optics at 1×; stock roof guns use their fixed
optics. These data differences must survive the shared input fix.

### Contact, springs and camera bounce

The tank contact solver has four wheel probes, six belly stations and three
spine probes. Its seven support values are max(wheel, corresponding belly) for
entries 0-3 and the spine values for 4-6. Those merged values decide whether a
corner's free-fall sink grows, whether grounded catch-up runs, and whether the
sink clears at the tail. Raw wheel depths still feed compression and spring
energy. Using raw wheel contact for both purposes let a belly-supported corner
accumulate a false fall. [orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0,
merge @ 0x4784CC, growth @ 0x478510, catch-up @ 0x478DA4, tail @ 0x47931B]

The diagonal support check clears all four sinks after the grounded fit. Its
threshold is zero for the parked latch and 250 otherwise. The airborne fit
includes each corner's computed drop adjustment; discarding that adjustment
held the old attitude until ground contact returned. Both omissions are fixed.
[orig: Entity_ProcessWheeledVehiclePhysics @ 0x475DE0, diagonal reset
@ 0x478FF2, adjusted corners @ 0x478834, airborne fit call @ 0x478AA7]

A 5,000-tick JOTAC training capture using the same assisted boarding setup
measured maximum consecutive camera pitch change of 1.42027° before and
0.46922° after the contact correction; roll fell from 2.10243° to 0.87620°.
This demonstrates less discontinuity in OpenNova. The changed physics also
changes the route, so these are not matched-position samples or retail traces.

### Camera and HUD ownership

Mounted view position and orientation come from the posed camera/userpoint
frame, including hull and gun transforms. Native pose and entity update order
remain owned by the portable runtime. [orig: Camera_ComputeThirdPersonView
@ 0x437D10; Entity_GetBoneWorldPosition @ 0x545E70]

The Godot stamp previously formed `eye + forward`, then recovered direction
through `look_at`. Single-precision subtraction at large world coordinates
quantized that direction. It now constructs the basis directly from `forward`
and writes position and basis together. Roll still follows the composed native
view. The regression checks three offsets at both 800 and 8,192 world units.
No interpolation or extra smoothing was introduced.

Camera composition is stateful: first-person shake advances during each logic
quantum and during scene presentation. The native implementation preserves
those two legs. [orig: Camera_ComputeThirdPersonView @ 0x437D10,
logic/render call sites @ 0x526781 / @ 0x5CA34D, filter block @ 0x43803C]

`LocalPlayerPresenter.presented_view()` exposes the already composed frame.
The live HUD and tank probe now consume that snapshot rather than calling the
stateful simulation getter again. Standalone HUD fixtures without a camera
presenter compose their own context. This closes the HUD's extra sample; it
does not certify every diagnostic caller or weapon-event composition path.

### Pivot sound: corrected field identification

The tank's fourth movement-loop argument is the absolute **yaw rate** at
`entity+0xA4`, when brain `+0x318` bit `0x20` is set. Vertical velocity is the
separate `entity+0xA0`. The older D-SND-17 description incorrectly called the
argument `slide_z`. The track phase calculation independently identifies
`+0xA4` as yaw rate. [orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0,
argument @ 0x48AAE0 and track phases @ 0x489F6E]

After the movement loop, an occupied tank with zero forward speed, nonzero yaw
rate and a heading error strictly greater than `0x071C71C0` arms the latch and
plays profile slot 46 (`swivel_shift`). The latch clears when forward motion
starts, yaw rate becomes zero, or its sign changes from the previous tick.
Parked tanks preserve the latch and still store the previous rate. This order
means the first continuous registration is on the next tick.
[orig: Entity_UpdateTankVehiclePhysics @ 0x488AB0, @ 0x48ACB5 through @ 0x48AD53]

The fold registers authored `Soundloop_4` on lane 40, unity pitch, a 30-tick
lifetime and signed-clamped low-word volume. It reaches this branch only after
registering idle; collision's full idle reaches it, while a stop, zero
reference speed or full-speed idle rejection does not. [orig:
Entity_ProcessMovementSoundEffects @ 0x5294A0, @ 0x5297C8 through @ 0x5298C6]

## Divergence catalog

| ID | Status | Summary |
| --- | --- | --- |
| D-CTRL-5 | FIXED 2026-09-22 | `cycleweaponP/N` signs reversed the authored wheel and bracket actions; corrected in action resolution, with catalog-to-optic and remapped-device regressions. |
| D-VEH-4 | FIXED 2026-09-22 | Tank corner support used raw wheel contact instead of merged wheel/belly support, missed diagonal sink clearing and discarded airborne corner correction. |
| D-VEH-5 | FIXED 2026-09-22 | Godot camera orientation lost precision through `eye + forward` followed by world-space subtraction; stamp a directly constructed basis. Host precision fix, not a retail smoothing policy. |
| D-HUD-29 | FIXED 2026-09-22 | Live HUD observation recomposed the camera and advanced shake/drift state; consume the displayed camera snapshot. |

D-SND-17 remains owned by [the audio record](../audio/lwf-dbf-sound-re.md): its
tank pivot cue, latch and fourth loop are implemented; the cross-family
sound-system-ready gate remains open. Wider vehicle residuals remain in
D-NET-161 and the [vehicle record](vehicle-client-movers-re.md).

## Validation and acceptance limits

- All 23 targeted native suites pass. They cover motor, support/suspension, attachments, mount/seat
  selection, emplaced guns, view/recoil, controls, panel math, host weapon
  switching, and replicated carrier prediction/compact/follow state.
- On stock `jox01`, the course/seat/mounted-switch GUT run passes four tests /
  449 assertions. On the configured `revx02`, it passes four tests / 801
  assertions. Both reach the authored training victory through the assisted
  course fixture. This is not proof of a human or device-only playthrough.
- Two windowed `tank_parity` zoom captures contain 758 and 284 frames, with
  cannon mount and posed userpoint data in every frame. The second starts at
  2×: wheel-up reaches 4×, 6×, 7× and clamps; wheel-down reaches 5×,
  3×, 2× and clamps. The HUD screenshots show the cannon sight and hull
  panel. The probe verdict means capture succeeded; it does not mean retail parity.
  The probe also offers observe/drive/fire scenarios, optional numbered seat
  selection and explicitly reported assisted boarding. It writes JSONL and
  before/after screenshots under its probe artifact directory.
- OpenNova host/joiner admission passes on stock 07TR/COOP: the joiner
  reaches admission-complete, phase 4, with a local player and no lost session;
  the host reports one connected peer. The mounted joiner probe failed during
  assisted boarding setup before its input exercise. This proves admission,
  not mounted driving, firing or camera parity.
- Retail RR was attempted twice against isolated stock copies on 07TR/COOP,
  including a retry with the complete proxy shader deployment. The host
  reached the mission transition and bridge startup but never
  reported ready before timeout. RR is blocked, and RO has no accepted retail
  baseline. The installed bridge exposes no `onhook_join_lan`, blocking the
  supported OR path. No successful mixed-retail result is claimed.
- Exact retail-versus-OpenNova camera traces, a driver/gunner live four-topology
  comparison, normal-input victory, and side-by-side HUD pixel/audio comparison
  remain unverified. Additional stateful view queries during weapon-event
  presentation need a separate call-count witness before claiming complete
  shake cadence parity.

Use `godot/probes/runtime/tank_parity_probe.gd` through `game_probe`, not a GUT
script as a live harness. On a local/authority world, its entity-card trace
includes unrounded motor BAM angles, all six wheel channels, four sink
accumulators and spring state. AI-free joiner cards expose the replica detail
without these local motor arrays. `vehicle_handle` selects a replica by its
actual packed handle when no authored mission net ID is available. Observe
these alongside camera and posed gun points to distinguish suspension,
carrier/attachment, weapon recoil and presentation errors.
