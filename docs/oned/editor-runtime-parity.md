# Editor–runtime parity: the shared-node patterns

How the OpenNova Editor (ONED) reuses game rendering and data systems for
authoring previews without becoming a second gameplay runtime. These patterns were
confirmed by an architecture survey of the editor/engine split (2026-06-10) and
updated when embedded mission play was removed (2026-07-29). The template for
new authoring surfaces is: **reuse the runtime node, public engine function, or
sampler behind an editor-owned preview seam; launch the standalone game for
live behavior.**

## The patterns, by surface

### Menus — `edit_mode` runtime-node reuse (the canonical exemplar)

The Menus workspace previews `.mnu` screens by instantiating the *runtime*
`NovaMnuMenu` node and setting `edit_mode = true`, which makes the live menu
inert and click-through so the WYSIWYG canvas can overlay selection and drag
gestures. The game's `NovaMenuShell` uses the same node with `edit_mode` off.
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

### Mission execution — one standalone runtime

`godot/engine/world/mission_runtime.gd` has one live owner: `GameWorld`, entered
through `MainGame`. ONED does not self-tick a mission, create a local-player
presenter, or embed the F3 debug overlay. This removes the editor-specific transport
and lifecycle state that could make an apparently shared simulation behave
differently from the shipped game.

The editor instead launches one managed standalone child over its mounted
resource directory:

- F5 starts the normal game from the saved loose assets.
- F6 starts the Mission workspace's current saved, top-level loose `.bms`.
- F8 stops the child. Another F5 or F6 restarts that same managed child.

Run never saves, exports, copies, or stages data. Every dirty workspace is
reported, but the child always observes disk state. This is the parity
boundary: the thing used for mission validation literally is the game runtime.
See [ADR 0025](../adr/0025-standalone-game-is-the-only-live-mission-runtime.md).

Use this shape for live behavior: save the loose asset and launch the product,
rather than adding an editor-owned transport around the simulation.

### Local player and debug UI — game-owned surfaces

`godot/engine/world/local_player_presenter.gd` is instantiated only by the game
shell. Gameplay input, mouse ownership, the viewmodel render pass, HUD feeds,
and F3 therefore have one boot path and one lifecycle. Editor automation may
control or inspect the managed child through the runtime debug/MCP seam, but
that seam calls the same public runtime controls as F3; it does not recreate a
player or world inside ONED.

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
4. Live mission behavior belongs to the standalone game. The explicit
   save-to-loose-assets boundary keeps editor state from becoming a second
   runtime state model.
5. A genuine preview divergence (terrain shaders) is a tracked decision:
   document why, and share the fidelity-bearing math.
