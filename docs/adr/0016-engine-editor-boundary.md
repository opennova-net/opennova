# The editor is a detachable layer over public engine APIs; engine behavior does not live in GDScript

ONED is a view and data editor over the engine. If the editor shows anything
the game would not show — a height the engine didn't sample, a color the
engine didn't derive, a layout the engine wouldn't produce — the editor is
lying, and the lie is invisible until an artist ships data tuned against it.
The 2026-07 boundary audit found real instances: a GDScript bilinear height
sampler coexisting with the C++ one (held equal only by a parity test), a
hand-rolled terrain ray-march the mission picker depends on, duplicated
sector/atlas constants, FNT format facts re-expressed in an editor
rasterizer, and ~1,167 lines of `[orig]`-cited time-of-day/celestial/weather
math living in `godot/engine/environment/*.gd` instead of `libs/env`.

## Decisions worth recording

- **Layering.** `libs/` (portable C++, Godot-free) is the engine.
  `godot/engine/` is the shell adapter: bindings and thin wrappers, plus the
  shell-neutral GDScript glue that exists only to wire Godot nodes to engine
  facts. `godot/modtools/` (ONED) and `godot/game/` are applications over
  public engine APIs. Dependencies point one way: applications → adapter →
  engine. **Nothing in `libs/` or `godot/engine/` may depend on
  `godot/modtools/`** — this is what keeps the editor detachable (and keeps
  the door open for GOALS.md's export-a-game future without blocking on it).
- **One implementation per engine fact.** An engine fact — a sample, a
  transform, a format constant, a derived color, an intersection — has
  exactly one implementation, in the engine, exposed through a public API.
  The editor calls it; it never re-derives it. Where a parity test today
  pins an editor re-derivation to the engine result, the fix is deleting the
  re-derivation, after which the test pins the single path instead.
- **Engine behavior does not live in GDScript.** Witnessed engine math
  (`[orig]`-cited behavior) belongs in `libs/` as a structural translation.
  GDScript in `godot/engine/` may embed, wire, and adapt — it may not carry
  the algorithm. Existing violations are worked off through the maturity
  program's conformance checklist (the environment port is the largest);
  new ones are rejected at review.
- **The bypass sweep is a standing instrument.** The audit list is a seed,
  not a boundary. A documented sweep (per-domain review of modtools/engine
  GDScript for math and constants that exist in `libs/`, plus grep
  heuristics over known engine constants) runs at every program wave
  boundary; each finding is closed or carries a tracked exception. The
  done-right patterns to copy are catalogued in
  [docs/oned/editor-runtime-parity.md](../oned/editor-runtime-parity.md).
- **Exceptions are tracked or they are bugs.** Anything that must diverge
  (editor-only affordances like live-edited-surface sampling before a bake)
  gets a written rationale at the call site and an entry on the checklist —
  the same rule the faithful-port convention already applies to engine
  divergences.
