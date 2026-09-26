# 03TR frame costs and presentation fixes

Measured 2026-09-21 against base `4dcd178fa`, in the isolated
`perf/03tr-presentation` worktree. Three fixes reduce repeated work at native
presentation boundaries. Across all three clean comparison runs per build,
frame time averages **18.76 ms -> 15.34 ms (18.3% lower)**. The changes do not
establish a locked 60 FPS: fixed-run p95 values range from 16.26 to 20.78 ms.

## Measured result

[Machine-readable evidence](03tr-frame-costs.json) retains all six runs,
sample counts, per-run percentiles, environment, topology hashes and the final
baseline/fixed DLL hashes. Means below are weighted by each metric's sample
count: 1,601 baseline frames and 1,960 fixed frames. Nested timing spans are
not additive.

| CPU/frame metric | Baseline mean, ms | Fixed mean, ms | Reduction |
|---|---:|---:|---:|
| Whole frame | 18.763 | 15.337 | 18.3% |
| Placed-model presentation (`mission_rows`) | 2.497 | 0.883 | 64.7% |
| Core of placed presentation, including destruction | 1.711 | 0.536 | 68.7% |
| Regular control publication within placed presentation | 0.508 | 0.118 | 76.7% |
| Presentation snapshot preparation | 1.629 | 0.935 | 42.6% |
| HUD device emission, including maps | 1.219 | 0.258 | 78.8% |

| Run | Baseline frames | Fixed frames | Baseline frame p95, ms | Fixed frame p95, ms |
|---|---:|---:|---:|---:|
| a | 545 | 684 | 20.337 | 17.020 |
| b | 548 | 691 | 20.011 | 16.261 |
| c, final DLL / fresh launch of each build | 508 | 585 | 22.443 | 20.778 |

Runs a/b averaged 18.32 -> 14.57 ms. The final fresh-launch pair was slower
for both builds (19.71 -> 17.13 ms); it remains included rather than discarded.
Its hotspot reductions agree with the earlier runs. The final DLL includes
an additional safety refinement: each linked-model traversal copies the
current link before a recursive call, protecting against vector reallocation.
These are short repeated samples of one camera pose, not a mission-wide
frame-time guarantee or a GPU benchmark.

## Why these paths were expensive

**Control publication.** The prior native presenter still passed Godot
Strings into `ObjectModel` for every write/clear. Each call normalized a
register name, converted it to UTF-8, looked up an ordinal, and built a
temporary list of linked parts. Six destruction channels were visited even
for rows later rejected from drawing. The initial instrumented diagnosis
attributed roughly 1.4 ms/frame to that bridge; simply moving it after
visibility rejection would change publication order.

The fixed path passes catalog ordinals and native owner tags directly to
`ModelControls`, traverses links without temporary vectors, and resolves
part-local register masks once when linked. ObjectIDs are validated on use.
Value **and owner** still determine whether a store changes the model; an
unchanged numeric value must reclaim an intervening owner's write and reach
linked parts. Destruction zeroes retain owner-scoped release semantics.

Retail resolves names while loading model CTRL records and publishes into
numeric slots: `[orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640]` calls
`[orig: CtrlName_ToOrdinal @ 0x57B290]`. Its six destruction stores are in
`[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]`, called before the later subpixel
rejection by `[orig: render_sector_entity @ 0x5C4190]`. See the
[CTRL witness and jo-c provenance](../threedi/3di-gp-format-re.md#native-ctrl-publication-2026-09-21).
Retail's global persistent bus differs from our per-model owner lifetime;
**D-3DI-2 remains open**.

**Snapshot marshalling.** The native builder produced a float vector, then
allocated/copied a Godot packed array which another native consumer read.
This scene produced 2,098 rows at 140 floats each: 1,174,880 bytes copied per
frame. A fresh identity vector and packed door side table added work.

`MissionRoot` now holds one immutable native snapshot lease across both
placed and wire walks. Rows, doors and revision remain together; normal
frames reuse storage and compare exact row identities in place. Nested
snapshot requests use fresh storage while an earlier lease is alive. Script
and tooling callers still receive independent packed arrays at their actual
boundary. Retail's native pool traversal is witnessed at
`[orig: collect_visible_entities_for_terrain @ 0x5C8C60]`; the lease itself
is host code, not a retail snapshot object. See the
[ownership contract](../runtime-architecture.md#godot-adapter-and-presentation).

**HUD submission.** The main overlay submitted one polygon per glyph or
triangle, repeatedly allocating packed arrays and crossing the CanvasItem
API. A typical measured mission frame had 99 glyphs, 14 triangles and 10
quads. The compiler was much cheaper than device emission.

The device now combines consecutive equal-texture triangle runs and
consecutive font-page runs into indexed triangle arrays. Geometry, colors,
UVs, kind order and underline layering remain unchanged. Maps already had
batching. Retail likewise batches same-page glyph vertices before dynamic
vertex-buffer submission: `[orig: CGameFont_DrawText @ 0x6752C0]` and
`[orig: GDynamicVB_DrawPrimitive @ 0x6788E0]`. Our existing kind-separated
HUD list does not reproduce every retail underline capacity-flush ordering;
this optimization preserves that existing boundary. See the
[font witness and jo-c provenance](../fonts/fnt-re.md#retail-font-submission-batches-2026-09-21).

## Capture conditions and reproduction

Windows, AMD Ryzen 7 7735HS / integrated AMD Radeon Graphics, Godot 4.6.1,
Forward+ / D3D12, 1600 x 900, vsync off, F3 closed. Both DLLs were built with
`bash scripts/build_godot.sh --jobs 4`: **Dev / RelWithDebInfo**, not `/Od`.
Comparison runs waited for the separate native build to finish; no compiler
or linker was active during the clean captures. No tests/builds ran alongside
the final pair.

Mission `03TR.bms`, expansion `revx02`, standing at the initial hangar spawn
without input. Mounted BMS size: 396,293 bytes. All six runs retained the same
topology SHA-256:
`dcd2dc749456f7e5ce88c7b160e89217996cb260510085c93de8eb08b2967396`.
There were 224 ObjectModels, 192 hidden; the presentation counters were about
185 planned rows / 12 submitted, 92 awake / 30 renderable models and 30 live
wire rows. No reduced population or diagnostic skip was used for these
before/after runs.

From the checkout being measured, with `OPENNOVA_JO_DIR` and `GODOT_BIN`
configured, launch through the existing game MCP:

```powershell
python scripts/mcp/game_mcp.py launch --project godot --windowed --resolution 1600x900 --exp revx02 --mission 03TR.bms --port 8976 --pid-file .scratch/perf-03tr/game.pid
python scripts/mcp/game_mcp.py probe --port 8976 run perf_mission_rows --args-file .scratch/perf-03tr/probe.json --wait
python scripts/mcp/game_mcp.py stop --port 8976 --pid-file .scratch/perf-03tr/game.pid
```

The probe argument file (create the scratch directory first):

```json
{
  "warmup_seconds": 4,
  "window_seconds": 5,
  "windows": 2,
  "label": "comparison",
  "output": "res://../.scratch/perf-03tr/capture.json"
}
```

Repeat for each build with identical settings and inspect the recorded
fingerprint. The probe captures CPU timings with its own instrumentation;
its nested spans must not be summed. Existing root GPU counters did not
provide a reliable attribution during the diagnosis, so no GPU-bottleneck
or wrong-viewport conclusion is drawn from them.

## Validation and remaining limits

- Optimized native extension build passed. Actual repository test sources
  `renderer_model_controls`, `netsim_present_rows`, and `hud_frame_compiler`
  passed against the worktree's optimized engine libraries (3/3). The scoped
  CMake harness stayed in ignored scratch storage.
- Seven targeted GUT files passed 204 tests with 28 asset-gated tests pending:
  `mission_present_pass_test.gd`, `mission_root_test.gd`,
  `player_visual_resolver_test.gd`, `object_model_part_anim_test.gd`,
  `destruction_present_pass_test.gd`, `hud_overlay_test.gd`, and
  `simulation_test.gd`.
- Supplying unchanged local retail `weapon.def`, `ammo.def`, `hudpos.def`
  and `BINOC.bad` in the expected fixture layout enabled additional coverage:
  wire presentation 41/41, mission root 25/25, simulation 97/97, HUD 25/26.
  The single HUD failure also occurs with the baseline DLL, in isolation:
  `test_overlay_draws_health_from_fixture_layout` expects
  `HUDDECLUT_DMGBAR 0 1 1 0`, while the available reference file authors
  `1 1 1 0`. No asset or test expectation was altered to suppress it.
- New regressions cover hidden-row destruction publication, the 65536
  endpoint, equal-value ownership reclamation, owner-scoped zero release,
  composed-model propagation / part-local camo / freed linked children,
  and a real visibility callback that requests another snapshot while the
  outer MissionRoot walk still needs its original rows.
- Deterministic rendered HUD comparison, using real `revx02` fonts and
  colored messages: 1024 x 768, 122 glyphs, 14 reticle triangles, 4 quads,
  **zero differing pixels**. A normal 03TR capture was also visually checked.
  Whole live-scene screenshots are not a pixel oracle because weather and
  simulation time differ between launches.
- Include graph, citation census, architecture ratchets and whitespace checks
  passed. No new divergence IDs were introduced or existing gaps closed.

The owning records contain the live IDA and jo-c citations. IDA received
four anchored entry comments (`0x5B4640`, `0x5C3F40`, `0x5C8C60`, `0x6752C0`) and was
saved; no curated names/types or retail bytes were changed. This slice covers
presentation overhead. Further worst-frame work needs a longer capture with
representative movement/combat and reliable GPU attribution.

## Frame hitches after the rendering parity pass (PR #679)

After the 2026-09-24 rendering parity pass the 03TR route still hitched:
~30 ms `WORLD_SLOT_SHADOW` spikes, 3 to 5 catch-up ticks after a long frame,
and 15 to 25 ms terrain page composes. Three fixes remove them without
changing what is drawn. The route numbers below come from a local route probe
(spawn to the Mk 19), not a tracked instrument.

**The shared terrain lane pool.** The CPU page rasters (the tile composer and
the static-shadow alpha pass) cut their rows into 8-row stripes dealt
round-robin to lanes (`engine/runtime/terrain/row_stripes.h`); each pixel sees
its writes in serial order, so the bytes are identical for any thread count.
The page workers used to spawn eight threads per raster call, up to 64 on the
cores when several pages composed at once. Every lane now runs on one shared
pool (hardware threads minus one) that the page workers lease and the caller
also claims lanes from; the pool lives beside its one owner,
`terrain_tile_composition_worker.cpp`. The static-shadow planner lays a page's
triangles out in serial draw order, then projects runs of that order on the
pool (a single page's triangle build 12.1 -> 4.0 ms). The tile cache device
reports the main thread's compose wait, upload and shadow-build time per frame.
Route: frames over 50 ms 18 -> 3, the worst frame 67.9 -> 53.5 ms, p99
36.6 -> 30.8 ms (with the two fixes below). Tests: `row_stripes`,
`terrain_static_shadow_planner` (a 40k-triangle page pinned to the serial bytes
under eight lanes).

**RetainedArrayMesh.** The render-slot ground shadows and the FrameFX Q3 pass
pack each caster surface on the CPU the first time they see it. They read the
arrays through `Mesh::surface_get_arrays`, which under the RenderingDevice
drivers reads the vertex, attribute, skin and index buffers back from the GPU
and stalls on every in-flight frame, once per node of a shared model.
`ObjectData` now builds its shared LOD meshes as `RetainedArrayMesh`
(`godot/src/render/retained_array_mesh.h`), an `ArrayMesh` that keeps each
surface's CPU arrays (tangents dropped), and both packers read those; only a
mesh from another producer still reads back. The Stats window's
`RENDER_SLOT_READBACKS` value counts those, and a stable frame reads 0.
Route: slot read-backs 5 frames -> 0, `WORLD_SLOT_SHADOW` max 11.0 -> under
4.5 ms.

**The retail tick bank.** The game banked wall-clock whole, so a 40 to 60 ms
frame's backlog ran as 3 to 5 catch-up ticks in the next frame and doubled the
hitch. It now banks through `world::TickAccumulator`, the port of retail's
bank (`[orig: Game_MainLoop @0x52B630]`: 1/16 ms units, a 7/8 EMA before the
4 ms drain, a 500 ms clamp; see `runtime-architecture.md`), which repays a
backlog over the following frames; it is the only bank. The bank needs retail's
mission-start re-base, ported into `inmatch::Session`: the Game Loop mode runs
`Game_StartMission` as its initialize and reads its clock afterwards, so the
load is never banked, and the start arms three frames whose render time is not
banked either, the loop re-reading its clock after each one's render
`[orig: Game Loop mode @0x82F340; Game_MainLoop clock read @0x52B75C;
Game_StartMission @0x525e1f; Render_ProcessMainSceneFrame @0x5caeff..0x5caf0e;
Game_MainLoop @0x52bac8..0x52bad2]`. `GameWorld` stamps each render from
`RenderingServer.frame_post_draw` and the game shell feeds the time since it
into the frame input. Route: frames running 3 or more ticks 51 -> 0 (at most
2). Test: `inmatch_session` pins the re-base (the load and three render frames
unbanked, then the full frame time).

Still open after #679: frames that compose several LOD pages wait 13 to 35 ms
on CPU raster work retail did on the GPU (the next step is a GPU page raster or
a byte-exact prefetch), and one-off 20 to 28 ms `FRAME_DRAW` frames, likely
pipeline compiles.
