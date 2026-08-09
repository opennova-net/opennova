# ADR 0033: the engine owns the loops; shells are devices

- **Status**: accepted (2026-08-09; the rearchitecture plan, maintainer-approved)
- **Owners**: runtime architecture
- **Supersedes/updates**: **replaces ADR 0031 and ADR 0032 in full as the
  standing architecture contract — there is ONE seam (the device boundary)
  and one rule.** ADR 0031's five-band contract and its census stay valid as
  the historical record that motivated this decision, but the bands stop
  being a live rule; ADR 0032's operative rules survive by RESTATEMENT inside
  this ADR (decision 3 below), not as a second contract. Also reverses ADR
  0023's "the queue itself (a device-era artifact) is not reproduced" at
  stage R3 (the queues carry the pass ORDER, and the order is the parity);
  supersedes ADR 0028 decision 5's standing rule ("GDScript in
  `godot/adapter/` wires, adapts, and presents") for the GAME runtime at
  stage R1 — GDScript remains the authoring language of ONED; narrows the
  Control-tree menu presentation (the PR #68 lineage) to authoring-only at
  stage R2. ADR 0016's principle ("engine behavior does not live in
  GDScript") is not superseded — it is *completed*: the runtime's
  orchestration follows the editor's math out of GDScript.

## Context

The original engine is ONE program. `Game_MainLoop @ 0x52b630` owns time (the
1/16-ms bank, the 4-ms drain, the ~31-tick clamp, render-once-per-iteration);
`Game_ProcessMainFrame @ 0x5263f0` owns the tick (net drain → WAC → BMS → AI →
weapons, in that order); `Render_ProcessMainSceneFrame @ 0x5ca0f0` owns the
frame (seven witnessed passes with explicit flush points,
docs/render/render-order-re.md §the-frame); and D3D sits underneath as a
device (`RenderState_ApplyToDevice @ 0x681920` is the boundary).

The port inverted that embedding: Godot owns the main loop and the scene
graph. The consequences, measured across the 2026-08-08 censuses and the
frame-flow trace:

- The engine's faithful tick (`World::run_logic_tick`, `Server_TickUpdate`)
  is driven by a GDScript catch-up loop (`mission_runtime.gd`) that
  hand-mirrors `Game_MainLoop` around the engine's own `TickAccumulator`.
- There is NO counterpart to `Render_ProcessMainSceneFrame` anywhere. The
  witnessed pass order survives only as `render_priority` integers;
  `renderer::opaque_sort_key`/`transparent_sort_key` have zero callers
  outside tests, because Godot owns the sort. Occlusion is applied AFTER the
  present pass — the inverse of retail — to survive scene-graph ownership.
- Three self-driven `_process` loops run outside the frame entirely (terrain
  LOD walk + submission, particle compile/composite, per-model material
  eval), plus a second 62 Hz weather clock beside the runtime's.
- Presentation is split across a native applier, direct node writes, and
  ~16k lines of "shell-neutral" runtime GDScript — and the GDExtension tax,
  dict marshalling, dual test suites, rebuild/restart cycle, and the
  band/ratchet bureaucracy of ADR 0031/0032 all exist to police a SEMANTIC
  seam the original never had.

The maintainer's directive, verbatim intent: fewer layers, less code
friction, better parity — and `godot/` adds nothing but Godot (ADR 0032).
The conclusion: stop policing the seam and move it DOWN to where the original
drew it.

## Decision

### 1. The boundary is a device interface

The engine owns the three loops. A shell implements devices:

- **RenderBackend** — executes an ordered command stream / uploads packet
  buffers. Until stage R3 it is "apply typed packets to Godot"; at R3 it is
  "execute the engine's ordered draw stream".
- **InputSource** — sampled device state in (one InputPacket per frame).
- **AudioSink** — bank/PCM playback out.
- The VFS already lives engine-side and stays there.

`godot/game` becomes a device host measured in hundreds of lines, not
thousands. ONED remains a full Godot application — authoring is where Godot
earns its keep — consuming engine documents exactly as today.

### 2. The ladder (each stage lands alone; later stages are gated)

- **R1 — the engine owns the frame.** `engine/runtime/frame` ports the
  orchestration: the bank/catch-up/present-once loop, the tick body's retail
  sequencing (listener, net drive, weather — ONE clock, occlusion in its
  retail position, iris, audio), and the input leg as an InputPacket. The
  three self-driven `_process` loops become frame phases whose outputs are
  packets; only GPU uploads remain outside. The bound surface trends toward
  `frame(input, camera, dt) → present handle`. Deletes `mission_runtime.gd`,
  most of `game_world.gd`, and the per-system present-pass GDScript.
  `docs/runtime-architecture.md` is rewritten to match (it currently inverts
  the retired NetSystem ownership and omits the render half).
- **R2 — presentation is packets, everywhere.** Generalize the two proven
  seams (`world/present_rows.h`, `renderer::ParticleFrameCompiler` — snapshot
  + camera in, typed packets out) to terrain patches, foliage batches, HUD
  elements, and GAME menu draws (retail `CUIElement` semantics over fnt text
  + quads; the mnu Control tree stays for ONED authoring/preview). One
  applier per domain; no Dictionary/TypedArray in any hot path; the
  duck-typed `NovaEntityVisual` dispatch dies.
- **R3 — the render frame (spike-gated).** Implement the seven witnessed
  passes as engine code emitting one ordered command stream — viewmodel-first
  depth, sky, the two-sided world pass with its flush(1)/flush(3)/flush(2)
  points, decals/water/foliage/weather, HUD, bloom — executed by one
  RenderingDevice/CompositorEffect backend. GATE: a spike renders one mission
  scene through the stream and screenshot-diffs against retail BEFORE the
  port is committed. This is where the ADR 0023 reversal takes effect and the
  sort keys get their caller.
- **R4 — a second backend (indefinitely optional).** SDL + a GL/D3D9-class
  backend proving the device seam. Godot remains ONED's home.

### 3. The absorbed rules (formerly ADR 0032; restated so this ADR is the one contract)

- **Nova formats never integrate with Godot's resource system.** No
  `ResourceFormatLoader`/`ResourceFormatSaver`/`EditorImportPlugin` for
  NovaLogic formats; documents read and write themselves (`load_from_path`/
  `load_from_bytes`/`save_to_path`, generic payload decode inside the load
  leg). `EditorResourceDocument._save_resource` stays a required override.
- **A shell line exists only because of the shell.** The old phrasing was
  "godot/ adds nothing but Godot"; under the device boundary it reads: shell
  code either implements a device (RenderBackend/InputSource/AudioSink leg)
  or is ONED authoring surface. Anything else — format semantics, witnessed
  math, gameplay rules, orchestration — is engine code, full stop.

### 4. What survives untouched

`engine/formats/*`, `engine/net/*` (wire parity is orthogonal and already
held), `engine/base/*`, the flat C ABI + Python/DCC pipeline, ONED's
thirteen workspaces and documents, the GUT suite for authoring surfaces. The
ctest suite GROWS: the frame becomes headless-testable end to end (golden
input scripts in → golden packet/command streams out — the pcap-golden method
extended to the whole frame).

## Consequences

- R1 and R2 are committed and staffed as the next slices; R3 waits on its
  spike; R4 waits on R3's parity results.
- During transition a domain not yet cut over keeps its node path; the packet
  applier makes old-vs-new diffable, which is itself the parity harness.
- Verification per stage: R1 — full ctest + frame-golden traces + the
  retail-LAN join recipes re-run to prove wire behavior unchanged; R2 —
  per-domain packet-diff + .mnu byte goldens + screenshot diffs; R3 — the
  spike's screenshot parity + a pass-order trace matched against
  docs/render/render-order-re.md before commitment.
- There are not two regimes. The band contract is retired NOW as a standing
  rule; the `adapter_cpp_orig_cites` ratchet survives only as the transition
  gauge (a number that must reach zero-or-seam-free as R1/R2 land) and is
  deleted with the seam it measured. New work is judged against this ADR
  alone: "is this a device leg, an ONED authoring surface, or engine code?"
