# Editor–runtime parity: the shared-node patterns

How the OpenNova Editor (ONED) and the game runtime share one implementation per
visual/simulation surface instead of maintaining two. These patterns were
confirmed by an architecture survey of the editor/engine split (2026-06-10) and
are the template for every new surface: **instantiate the runtime node, flip a
flag or swap a sampler for editing — never reimplement the surface for the
editor.**

## The patterns, by surface

### Menus — `edit_mode` runtime-node reuse (the canonical exemplar)

The Menus workspace previews `.mnu` screens by instantiating the *runtime*
`NovaMnuMenu` node and setting `edit_mode = true`, which makes the live menu
inert and click-through so the WYSIWYG canvas can overlay selection and drag
gestures. The game's `NovaMenuHost` uses the same node with `edit_mode` off.
"Interactive preview" re-arms navigation while sandboxing external Commands
(launch/quit/URL) to no-ops. See CONTEXT.md ("Edit mode", "Interactive
preview").

Use this shape when the editor needs WYSIWYG fidelity: one runtime node, one
flag, editor gestures supplied from outside the node.

### Environment — direct runtime-node reuse

The Terrain editor instantiates the runtime `NovaEnvironment` / `NovaSky` /
`NovaWater` nodes and drives them through the same `environment_data` +
`environment_changed` path `GameWorld` uses at runtime. The Environment
workspace edits apply to the same nodes the game renders with; water is fully
unified (parameterized, not forked).

Use this shape when the surface has no edit-time interaction at all: share the
node outright.

### Foliage — shared dispatcher, abstracted geometry source

Editor and runtime both configure `NovaFoliageDispatcher` identically
(cell-grid algorithm, radius, `VegAssets.resolve_slot_meshes`). They differ
only in where geometry comes from: the runtime feeds `NovaTerrainData` (the C++
fast path); the editor binds `Callable` samplers onto the live-sculpt mesh so
scatter follows unsaved terrain edits.

Use this shape when the editor must preview *unsaved* state: keep the engine
system shared and abstract only the data source behind a sampler seam.

### Mission simulation — one runtime driver, two transports

`godot/engine/world/mission_runtime.gd` is THE driver both hosts go through:
the game (`GameWorld`) drives it with explicit `tick()` calls ordered against
its other passes; the Mission workspace self-ticks it via `_process`. Both run
the one `NovaSimulation.step()` cadence with the sim's default `loco_scale`, so
the editor preview is the game's pacing — the sim exposes no tick-mode knob to
diverge on. While simulating, editing is locked out (the present pass is
the sole transform authority); Stop rewinds the world and restores authored
transforms.

Use this shape for live behavior: one driver, host-chosen transport, mutual
exclusion between the simulation's writes and the editor's.

### Local player & play-in-editor — one host, two boot shapes

`godot/engine/world/local_player_host.gd` is THE local-player host both hosts
instance: the game shell (`main_game.gd`) and the Mission workspace's play
controller (`mission_play_controller.gd`) each create one, call `setup(world,
camera)`, and drive the same `before_world_tick` / `after_world_tick` pair
around `GameWorld.tick()`. The gameplay keys (F4 camera, C/Z stance), the
mouse-look, the FP viewmodel (and its dedicated render pass), and the debug
overlay's View toggles all live on the shared host — the shells only route
input and decide when the player is live. F3 summons the same
`NovaDebugOverlay` in both hosts (the game shell binds the key directly; the
play controller forwards it to the workspace's overlay).

Two lessons this surface carries:

- **The hosts enter the tree differently.** The game shell wires the host from
  its scene `_ready`, while the player camera's viewport is still making its
  children ready — a direct `add_child` into that viewport is rejected
  ("parent busy"), so anything the host mounts into `camera.get_viewport()`
  must mount deferred. ONED's play controller enters an already-running tree
  and never sees the rejection; only the runtime boot does. Symptom when
  violated: a feature that "works in ONED but not in the game" with identical
  shared code.
- **Freeing the mouse is a host decision.** The game shell frees the captured
  mouse through its pause menu (Esc); play-in-editor has no pause state
  (Esc stops), so the workspace suspends the play session's per-tick capture
  while the debug overlay is up and resumes it on close. Same shared overlay,
  host-appropriate mouse ownership.

### Terrain shaders — intentional, contained divergence

Terrain is the one deliberate split: the editor renders with a live-sculpt
shader (height edits without rebake), the runtime with the baked shader. The
surface-shading math is shared via an include so the two cannot drift in look.
Divergence is acceptable only like this: tracked, justified by an editing need
the runtime path cannot serve, and sharing the fidelity-bearing core.

## Rules of thumb for new surfaces

1. Start from the runtime node/system. If the editor needs it inert, add an
   `edit_mode` flag to the node rather than a parallel preview implementation.
2. If the editor must show unsaved state, add a sampler/data seam, not a fork.
3. Editor-only interaction (gizmos, hover, pick bodies) lives in modtools and
   attaches *around* the shared node; it never leaks into the runtime path.
4. While a simulation owns nodes, editing is locked out — two writers over one
   node is a race, not a feature.
5. A genuine divergence (terrain shaders) is a tracked decision: document why,
   and share the fidelity-bearing math.
