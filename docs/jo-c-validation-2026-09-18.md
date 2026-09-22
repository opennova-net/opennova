# JO-C validation and fixes - 2026-09-18

> **Dated snapshot.** This report records the 2026-09-18 validation. The
> [divergence ledger](divergence-ledger.md) and owning RE records track later
> status changes.

The concrete differences found in this pass are corrected: guided-round birth,
lifetime and motion; shared loaded-ammo storage; blast section marking; indoor
projectile terrain admission; parachute descent; writable WAC night state;
reverb selection/preset behavior; dedicated texture producers; AI callback and
controller ownership; and player steering/handbrake command state. Mission
save/resume is excluded at the user's request.

This is a bounded comparison with the original executable and `../jo-c`, not a
claim of whole-game equivalence. Existing unrelated ledger entries remain open.

## Compared versions

- Port baseline: `d1cfa4cec11321f6b55cd190c95d21a3c7698512` (PR #651).
- Reference source: sibling `../jo-c`, HEAD
  `19b9ce42d9f74f4d6b511f2f82217371869be498` at the audit checkpoint.
  Its original instruction harnesses supply synthetic expected results;
  no reference source was modified.
- Original executable SHA256:
  `b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac`.
  IDB: `Jointops.exe.kong.i64`, image base `0x400000`, identity checked in IDA.
- The initial reference/reference check passed 395 jo-c guided cases against
  its linked reconstruction. That result is distinct from the **308 port/original
  cases** committed here; it is not counted as port validation.

## Corrections and evidence

| Area | Before -> after | Evidence |
|---|---|---|
| Guided lifecycle (D-NET-64) | Guidance created or revived a separate replica row -> only an existing live fired-round slot accepts updates; normal lifetime/collision retires it. Fire records land after same-frame entities; guidance is retained until each recipient's spawn-frame boundary. | `npruntime_client_runtime`, `projectile_combat`, `npruntime_server_tick_maintenance`; dispatch @ 0x4D6960. |
| Guided motion (D-NET-64) | Stored XYZ, velocity 300 and approximate world-frame pursuit -> live target pose, actual ammo limits and original Stinger/Hellfire/Javelin launch/motor math in the ordinary round pool. Launch velocity exists before the age-31 booster. | **308 original-instruction cases**, including local-frame pursuit, axis limits, launch roles, target loss, phases, range guards and gravity; `guided_missile_flight`. Roots @ 0x445CC0, @ 0x445DB0, @ 0x445EF0, @ 0x446060, @ 0x446690, @ 0x446BA0, @ 0x546B30. |
| Shared loaded ammo (D-WPN-2/20) | Every weapon owned its loaded clip -> def+0xDC selects a separate DWORD bucket shared by related weapons. Recalc/refund, weight, selection, fire and reload use the same owner on player and authority paths. | `weapon_inventory`, `weapon_fsm`, `npruntime_server_tick_maintenance`; get/set @ 0x5405F0 / @ 0x540670, reload @ 0x541720, totals @ 0x5425F0. |
| Blast section state (D-ITEM-3) | Health damage skipped breakable sections -> COBJ flags feed the inclusive explosion-box sweep, section mask and one-time glass sound. | `destruction` tests translation, box corner outside the sphere, flag filtering and repeat hits; @ 0x4E6C5E..0x4E6E6B. |
| Indoor rounds (D-COL-11) | Terrain could stop rounds inside building BB volumes -> pre-move BB state suppresses terrain until the round leaves. | `projectile_combat` crosses the same heightfield inside and outside a BB; @ 0x4EA2F0. |
| Parachutes (D-INF-20) | Transported deployment flag lacked descent behavior -> authority admission/cancellation, canopy state, carry consumption, braking, sounds, animation, steering and view limits are wired into local, remote-authority and replica paths. | `infantry`, `npruntime_client_runtime`; @ 0x4B7AD9..0x4B7C8D. |
| WAC night (D-WAC-4) | Writes were discarded -> raw DWORD writes/arithmetic read back immediately, survive snapshots and feed light selection; the authored TOD update owns the later overwrite. | `wac_state`, `weather_state`, `environment_state`; Env @ 0x26C645C, TOD store @ 0x57DEAE. |
| Reverb (D-SND-18) | Mission id drove an invented Godot room size -> original live mission/building/region selector; invented effect removed. The earlier assumption of preset-dependent retail DSP was disproved at the observed mixer boundary. | `reverb`; **40 original mixer executions**, all 20 stock and all 20 altered preset rows, identical nonzero 1024-frame output. Selector @ 0x4B5F9E..0x4B633F, mixer copy @ 0x7BDD12. |
| Specialized textures (D-RMAT-12) | Types 6/7/16/17/18 raw-loaded -> 16-slice horizon generation, white AO, NQ8B normals and HRZ8/AOC8 alpha data, preserving volume dimensions through Godot. | **14 original horizon pixel cases**, chunk layout/channel/nesting/truncation cases and Godot resource/cache tests; @ 0x58A220, @ 0x58CB90, @ 0x58F350 / @ 0x58F470 / @ 0x58F590. |

## Reproduction

Normal native tests need no original executable or Python oracle packages. The
synthetic original outputs are committed in
`tests/world/fixtures/guided_missile_vectors.inc` and
`tests/renderer/material_horizon_vectors.inc`.

To verify or regenerate them with a local copy of the pinned game and jo-c:

```text
python scripts/oracles/jo_c_parity.py --jo-c ../jo-c --retail-exe <Jointops.exe>
# Add --write to regenerate the committed C++ vectors.
python scripts/oracles/reverb_preset.py --retail-exe <Jointops.exe> --output <report.json>
```

The scripts require `pefile`, `capstone` and `unicorn`. Guided cases execute the
original geometry/math with x87 PC53 and bounded world/aim/network service inputs.
They compare state/output words, not a full running match or unmodeled seeker
side effects. Horizon cases compare every used output byte. Reverb supplies
controlled MMX bus input at @ 0x7BDD90, leaving original selection, filters,
stereo packing and pacing intact. Every preset produced SHA256
`c51d1e7766544e5394f9ca3e415440f722392b575934f22bf2dc1c2f3fdcca03`.

## Validation and remaining limits

- Initial projectile/material pass: full native Release build succeeded. CTest: **482 passed, 1 skipped, 0 failed**
  out of 483; `motorcycle_gravity_06tr` could not open its required retail 06TR
  mission in the supplied reference install. Both retail asset roots were set.
- Initial projectile/material pass: Godot RelWithDebInfo extension build succeeded. The new producer test and
  existing resource-root contract suite: **23 tests, 237 assertions passed**.
- Original-executable comparisons: **308 guided cases, 14 horizon images,
  40 mixer preset cases passed**. No audible or live visual equivalence claim.
- jo-c's recorded corpus scan found zero specialized texture types among 2717
  3DI3 models / 15590 texture rows. That scan was not rerun here; synthetic
  producer/resource evidence does not establish live stock-scene reachability.
- D-NET-64 retains the flare projectile-candidate ring, proximity AI/warning
  consumers and C2S command-map overload. D-INF-20 retains fresh marker authoring
  and live canopy/leg presentation validation. D-ITEM-3 retains the separate
  lawr/fgrenade continue-through-glass report; D-COL-2 retains its wider
  animated/destroyed-section collision table. These were not the newly
  reproduced lifecycle/descent/blast-marking defects.
- Mission save/resume remains D-SAVE-1 and is deliberately untouched. No new
  mixed retail/OpenNova multiplayer playthrough was performed.

Durable domain details: [network](net/novaworld-net-re.md),
[world](world/world-wac-ai-re.md), [audio](audio/lwf-dbf-sound-re.md),
[materials](render/render-material-re.md). IDB comments for the new reverb and
texture findings were appended at @ 0x7BDD12, @ 0x58A220 and @ 0x58CB90 and saved;
no original executable bytes or reference source files were edited.

## AI and player movement follow-through

This pass compares PR #652 head `0fb204f13ea1303123896f8c38375c323a5b5350`
with jo-c `f2cd385955ad8b889aa7e820973a849707aa51a4` and the same SHA256-pinned
original executable above. The shared native engine owns these corrections:

- D-AI-14: remove the invented global brain quota and unconditional movement
  register seed. Each pool-1 brain uses its own think countdown. Aircraft
  movement-controller row 4 now runs with the owning aircraft's phase; default
  class events retain the original authority/client transition guard.
- D-VEH-3: retain full driver heading precision, replace inactive digital
  directions in the analog arm, and use prior brake state and its retained
  direction in the ground/bike command branches. A freshly boarded routeless
  AI vehicle now stays still from the first tick.

`movement_brain_parity` consumes **916 original-instruction vectors**: 640
player ground/bike command cases, 252 air/ground default-event cases and 24
aircraft recovery-phase cases. The original dispatcher callbacks execute
unchanged; motor slices stop before steering/contact. No engine calls are
mocked. Public-tick coverage also runs 80 brains on each role and checks that
later vehicles think without unrelated register rewrites. `vehicle_motor`
pins first-tick and sustained zero movement for the fresh routeless driver case.
The initial 436-case subset plus crowded-world checks reproduced 1095 failed
assertions before the implementation changes; the final vectors broaden the
motor scope to bikes and retained brake directions.

```text
python scripts/oracles/movement_brain_parity.py --retail-exe <Jointops.exe>
# Add --write to regenerate tests/world/fixtures/*_vectors.inc for this oracle.
ctest --test-dir build -C Release -R "^(movement_brain_parity|ai|vehicle_motor)$" --output-on-failure
```

The oracle needs `pefile` and `unicorn`; native tests use only the committed
synthetic vectors. Domain evidence and addresses are in
[world section 34](world/world-wac-ai-re.md#34-ai-callback-ownership-and-movement-controllers-2026-09-18)
and [vehicle section 39](world/vehicle-client-movers-re.md#39-player-motor-command-precision-and-brake-transitions-2026-09-18).
No original/reference files or IDB state were changed in this movement pass.
No new live mission or mixed retail/OpenNova multiplayer playthrough is claimed.

Final movement-pass validation: full native Release build succeeded; **483 tests
passed, 1 skipped, 0 failed** out of 484 with both retail asset roots set and Git
LFS fixtures materialized. The skip is the same missing `06TR.bms` mission.
All 916 oracle vectors verified against the pinned executable. Repository
ratchets, maturity, include/link graphs, orphan headers, witness census (zero
lost), fixture provenance/materialization, environment/convention checks,
ledger scoreboard, retail-gate documentation and diff whitespace passed.
The GDExtension/GUT results above belong to the earlier projectile/material pass;
they were not rerun for this native movement follow-through.


## MC-5 helicopter floor follow-up

The latest PR build (`590e4a6badf813e6e6a75565825bee9bc5fbb822`) reproduced
an unseated player falling behind the helicopter cabin at tick 1565 of the
09TR takeoff regression. NPCs also left the cabin: their authored boarding
and stop orders were discarded by a route lookup that reserved commands do
not require. This follow-up corrects both causes.

- D-INF-25: local and authority organic bodies consume the carrier's current
  translation and capsule-biased Q22 rotation. The vehicle pass now precedes
  infantry, matching the original pool order. Org2 retains its radius release
  and gradual pitch follow; NPC world aim stays independent for off-carrier
  combat targets. Mounted bodies retain their absolute seat poses.
- D-AI-15: route-order writers preserve reserved commands and their operands.
  09TR events 25/26 can now send command 123 with carrier SSN 4572, and event
  27 can stop the standing instructor with command 0. Only node -1 asks for
  the nearest route node.
- Local input packing remains before the vehicle pass. Current steering,
  throttle and key release reach the same motor tick; the new `vehicle_motor`
  regression failed all three assertions before that timing correction.

`infantry_terrain` covers local, NPC and authority remote-player carrier
transport, radius release, capsule rotation, world-aim adoption and pitch lag.
`event_runtime_bms` reproduced ten failed assertions for discarded reserved
orders. `parachute_09tr` boots the shipped mission, ADM motion and collision
assets, lets the opening events run, and stages the player above the floor
without setting a support link or mount. It runs 50 seconds of authored
flight and checks the unseated player and instructor plus six seated NPCs.
The helicopter travels approximately 248 horizontal units and climbs 18;
translation, turning and all three boarding/stop events are required.

```text
ctest --test-dir build -C Release -R "^(infantry_terrain|event_runtime_bms|vehicle_motor|local_player_view|parachute_09tr)$" --output-on-failure
```

The asset root for `parachute_09tr` is `OPENNOVA_JO_ASSETS`; see the
[asset-gated test matrix](asset-gated-tests.md). The original witnesses and
bounded verdicts are in [world section 35](world/world-wac-ai-re.md#35-unseated-helicopter-riders-and-reserved-route-orders-2026-09-18).
Reference source, executable and IDB pins are unchanged; no IDB state was edited.

Final follow-up validation:

- Full native Release and Godot RelWithDebInfo extension builds passed.
- Full CTest with both retail asset roots: **484 passed, 1 skipped, 0 failed**
  out of 485, including the existing 916 movement/brain vectors. The remaining
  skip is `motorcycle_gravity_06tr`, whose required `06TR.bms` is absent from
  the supplied install. No new original-instruction vectors are claimed here.
- The restarted Godot game ran the real 09TR mission. A 62-second floor-ride
  capture stayed in the cabin through takeoff with NPCs aboard; the final
  rebuilt extension also completed a still/strafe/stopped capture and retained
  an alive, airborne local player. This is not a complete mission playthrough.
- Repository ratchets, witness census, maturity, include/link graphs, orphan
  headers, fixture provenance, environment/convention checks, ledger consistency,
  retail-gate documentation and diff whitespace passed. The earlier GUT counts
  above are not a new GUT run for this native movement fix.

The separate report of faint trails while the local player moves is **not yet
reproduced or fixed**. The registered `player_motion_capture` probe records 18
viewport frames plus player/camera positions at rest, during a strafe and after
stopping; `move=false` with `interval_ms=3000` records a stationary floor ride.
The captured frames did not establish the reported trail. The user subsequently also reported the effect in spectator view, worse
when moving opposite a watched actor. The supplied clip was described as stock
JOX, so it is not a confirmed OpenNova reproduction. Captures stay in local `user://probe-runs/`, not
in the repository. Replica carrier scheduling retains the separate section
29.2 limitation; no new mixed retail/OpenNova multiplayer playthrough is claimed.


## Animation and relative-motion alignment

The jo-c follow-up closes D-INF-26's request/playback ordering mismatch. Both
channels advance at the motor head; requests, playing ids and root/pose samples
have separate lifetimes. Player root rotation reads the heading before leg chase.
The local eye is resolved from the current simulated head and re-anchored after
movement; old render feedback is removed. Held-weapon attachment follows body
posing. Spectator translation precedes world camera-dependent preparation, with
the user's 4.6875 camera speed retained. See [world section 36](world/world-wac-ai-re.md#36-animation-and-relative-motion-timing-audit-2026-09-18)
for witnesses, regression boundaries and the remaining scope limits.

New motor, current-head and spectator-order regressions were demonstrated red
before their fixes. Final validation with both retail asset roots:

- Native Release and Godot RelWithDebInfo extension builds passed.
- Full CTest: **484 passed, 1 skipped, 0 failed** out of 485. The remaining
  `motorcycle_gravity_06tr` skip requires the absent `06TR.bms`.
- Focused GUT (`fly_camera_spectator_test.gd`, `local_player_presenter_test.gd`,
  `skeletal_anim_test.gd`, `mission_present_pass_test.gd`): **92 tests and
  3,114 assertions passed**. One existing orphan warning; no script errors.
- Repository ratchets, witness census (zero lost addresses), maturity,
  include/link graphs, orphan headers, fixtures, environment/conventions,
  ledger consistency, retail-gate documentation and diff whitespace passed.

The visual-trail cause remains unproven; the rebuilt 09TR mission is for the
user's player/spectator comparison. No new mixed retail/OpenNova multiplayer
playthrough or complete closure of the broader motor/camera residuals is claimed.
