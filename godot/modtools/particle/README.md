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
- **Particles** — a practical per-particle form: core emission, motion, appearance
  flags as checkboxes, colors, curve assignments, and the per-layer texture
  editor. Edits refresh the preview live.
- **Tables** — draw curve tables directly (drag to paint, Ramp/Flat/Invert)
  and see each curve as a sparkline.

Saving validates first: empty names block the save and duplicate names warn.
References may intentionally resolve through another mounted `.ptl`, so the
editor preserves unresolved names instead of treating every cross-file link as
a local-document error.

## Formats

| Format | Backing library | Notes |
|---|---|---|
| `.ptl` | [`libs/particle`](../../../libs/particle) | text format: effects, particles, curve tables, editor handles; parser/writer round-trip tested against the retail corpus |

## How it is built

`particle_workspace.gd` is the `EditorWorkspace` adapter: it owns a
`ParticleEditor` document (`particle_editor.gd`, dirty state + selection +
CRUD), mounts `blueprint/particle_blueprint_screen.gd` (GraphEdit +
`particle_preview.gd`) in the viewport lane, and declares the three workflow
inspectors under `inspectors/`. The preview drives one `NovaEffectScene` and
one `NovaParticleRenderer`—the same 62.5 Hz value scene, exact shared atlas,
ordered packet compiler, and compositor backend the game runtime uses. No
per-emitter renderer Nodes or preview-only simulation path are involved
(witness record: [`docs/particles/ptl-format-re.md`](../../../docs/particles/ptl-format-re.md)).

Tests: `godot/tests/particle_editor_workstation_test.gd` (shell integration),
`godot/tests/particle_authoring_test.gd` (model + blueprint), and
`ctest -R particle` (parser/writer/simulator).
