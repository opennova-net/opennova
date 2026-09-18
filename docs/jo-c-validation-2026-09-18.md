# JO-C validation and fixes - 2026-09-18

The concrete differences found in this pass are corrected: guided-round birth,
lifetime and motion; shared loaded-ammo storage; blast section marking; indoor
projectile terrain admission; parachute descent; writable WAC night state;
reverb selection/preset behavior; and dedicated texture producers. Mission
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

- Full native Release build succeeded. CTest: **482 passed, 1 skipped, 0 failed**
  out of 483; `motorcycle_gravity_06tr` could not open its required retail 06TR
  mission in the supplied reference install. Both retail asset roots were set.
- Godot RelWithDebInfo extension build succeeded. The new producer test and
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
