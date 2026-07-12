# Particles workspace

Author NovaLogic `.ptl` particle effects: explosions, smoke, muzzle flashes,
dust, and water spray. Part of the [OpenNova Editor (ONED)](../README.md).

## What you do here

A `.ptl` file holds effects (named groups of particles fired together),
particles (one emitter template each: emission shape and rate, motion, colors,
size, up to four texture layers), and curve tables (hand-drawn value-over-life
curves that particles reference for size, opacity, color, and emit rate).

The viewport hosts a blueprint graph of the whole file — effects, particles,
and curve tables as nodes, with edges for membership (effect → particle) and
spawn chains (particle → child) — side by side with a live 3D preview running
the same simulation the game uses. Selecting a node switches the inspector to
that entry. The preview has full playback control: play/pause, frame stepping,
a deterministic timeline scrubber, time scale, and background brightness.

The three inspector workflows:

- **Effects** — compose effects from particle entries.
- **Particles** — the full per-particle form: emission, motion, appearance
  flags as checkboxes, colors, curve assignments, and the per-layer texture
  editor. Edits refresh the preview live.
- **Tables** — draw curve tables directly (drag to paint, Ramp/Flat/Invert)
  and see each curve as a sparkline.

Saving validates first: empty or duplicate names and broken references block
the save with a message instead of writing a file the game would reject.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.ptl` | [`libs/particle`](../../../libs/particle) | text format: effects, particles, curve tables, editor handles; parser/writer round-trip tested against the retail corpus |

## How it is built

`particle_workspace.gd` is the `EditorWorkspace` adapter: it owns a
`ParticleEditor` document (`particle_editor.gd`, dirty state + selection +
CRUD), mounts `blueprint/particle_blueprint_screen.gd` (GraphEdit +
`particle_preview.gd`) in the viewport lane, and declares the three workflow
inspectors under `inspectors/`. The preview drives `NovaParticleEmitter`
(GDExtension) — the same portable simulator in
[`libs/particle`](../../../libs/particle) the game runtime uses, with the
per-blend-mode shaders in `shaders/` mirroring the engine's render states
(witness record: [`docs/particles/ptl-format-re.md`](../../../docs/particles/ptl-format-re.md)).

Tests: `godot/tests/particle_editor_workstation_test.gd` (shell integration),
`godot/tests/particle_authoring_test.gd` (model + blueprint), and
`ctest -R particle` (parser/writer/simulator).
