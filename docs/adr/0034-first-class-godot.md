# ADR 0034: Godot is the first-class shell

- **Status**: accepted (2026-08-09; maintainer directive)
- **Amended**: ADR 0035 replaces the transitional frame-hook registration
  described below with a typed native session and direct Godot frame pipeline.
  [ADR 0043](0043-canonical-cpp-and-godot-hard-cut.md) (2026-09-02) executes
  decision 6's C++ rewrite queue (the world, the present passes, effects,
  audio, lights, the local-player presenter) and DELETES the practice that
  grew under decision 1 of invoking overridable names as `_world.<name>()`
  so GUT subclass-doubles keep binding: a test never subclasses a production
  Node. Decision 6's list of what stays GDScript (MCP tooling, the fly
  camera, shell UI) stands.
- **Updated**: [ADR 0037](0037-oned-runs-game-data.md) narrows ONED from an
  authoring front-end to run controls; Godot remains first-class.
  [ADR 0045](0045-cli-game-data-runtime-only.md) later removes ONED entirely.
  [ADR 0040](0040-the-engine-is-one-namespace.md) finishes decision 5 (the
  files, identifiers and macros drop the prefix too; bindings include
  root-relative) and restates §2's consumer list honestly.
- **Owners**: runtime architecture
- **Supersedes/updates**: amends ADR 0028 and ADR 0033's POSTURE — the
  "adapter" framing and the shell-neutral pretense — without touching their
  substance. ADR 0033's one-line test (engine owns the loops; a `godot/`
  line earns its place as a device leg or authoring surface) stands
  unchanged; what dies is the ceremony that pretended some other shell might
  one day plug into the same seams.

## Context

Every architecture round since ADR 0016 has framed `godot/src/` as "the
shell adapter" — glue between the portable engine and a nominally
replaceable presentation front-end. The vocabulary (CONTEXT.md "Shells"),
the directory name, and plumbing like the frame-hook Callable bundles
(`set_frame_shell_hooks` with seven Callables, `set_frame_world_hooks` with
eight, every install site rebuilding the same method-name list) all carried
the implication that Godot is one of N possible embedders.

It is not, and it never will be. The project ships ONE presentation layer:
the Godot project — the game shell and ONED are both inside it. The things
that genuinely run without Godot (the importer's Python FFI, the DCC
plugins, `apps/novaworld_server` and the dev harnesses, the entire ctest
suite, the wire tooling) consume the engine directly and never wanted a
"shell seam" — they are exactly why `engine/` stays portable C++. The
indirection bought nothing for them and cost the Godot side real
awkwardness: bundle-of-Callables registration, "adapter" naming that
describes no adaptation, and duck-typed seams excused as shell neutrality.

The 2026-08-09 trunk round (the ADR 0033 R2 draw list cutover + the native
ObjectModel port + the no-duck-typing sweep) made the cost visible:
once every runtime seam is a typed class, the "neutral" indirection is the
only untyped thing left.

## Decision

1. **Godot is the first-class, sole shell.** Godot-side code is written AS
   Godot code: typed `class_name` classes, direct typed calls between them,
   Godot idioms throughout. No seam exists to keep a hypothetical second
   shell plausible; no API is shaped around shell neutrality. "Shell" in
   CONTEXT.md now means the two front-ends INSIDE the Godot project (game,
   ONED), not an embedding abstraction.

2. **The engine stays portable — for its real consumers.** `engine/` remains
   Godot-free C++ (ADR 0028's namespace, group targets, and the C ABI all
   stand) because the importer, DCC plugins, headless servers, net tooling,
   and ctest consume it without Godot. Portability is a property those
   consumers pay for and use, not a bet on a second renderer. ADR 0033's
   boundary rule is unchanged: witnessed behavior and orchestration live in
   `engine/`, and the Godot side is device legs + authoring surfaces.

3. **Registration replaces hook bundles.** Where the engine frame needs
   Godot-side legs, the owning NODE registers itself once
   (`set_frame_world(self)` / `set_frame_shell(self, listener)`)
   and the binding wires the documented leg contract, rejecting the install
   loudly if a leg is missing. Bundles of loose Callables at call sites are
   retired everywhere a typed owner exists.

4. **The directory is renamed to match reality.** `godot/adapter/` becomes
   `godot/src/` — the Godot project's source (GDExtension C++ bindings +
   the shared GDScript runtime), named like what it is. `res://src/...`
   paths, the build scripts, lints, and docs follow mechanically. The
   `adapter_cpp_orig_cites` ratchet survives under the new path with the
   same meaning: a `[orig:]` cite in Godot-side C++ is either a documented
   seam contract or code that belongs engine-side.

5. **The `Nova` class prefix retires.** Registered classes and GDScript
   `class_name`s are the engine's first-class concepts — `Simulation`,
   `ObjectModel`, `Terrain`, `EntityIndex` — not branded wrappers.
   Exceptions, each for a reason: `NovaWorld*` keeps its name because
   NovaWorld is the SERVICE's proper noun (the matchmaking system we speak
   the wire protocol of), and three classes whose bare names Godot's global
   ClassDB already owns take honest specific names instead
   (`MissionEnvironment`, `SkyDome`, `WindowState`). File names keep their
   `nova_` prefix for now — directories already namespace them, and the
   directive is about the concepts, not the files.

6. **`godot/src/` is pure C++ — GDScript is game-level.** The bindings
   directory is part of the core engine; every `.gd` moved to `godot/game/`
   (subpaths preserved; ONED preloads from `res://game/...`). GDScript's
   place is quick-changing game-level code. The standing TRIAGE rule: any
   script now under `game/` that is really engine behavior is the C++
   rewrite queue — current queue, in priority order: the environment/
   weather cluster (witnessed math → `engine/runtime` + thin bindings),
   `mission_object_placer`, the `game_world`/`mission_runtime` composition,
   the present-pass facades + the wire cold path, avatar composition.
   Genuinely game-level scripts (MCP tooling, the fly camera, shell UI)
   stay GDScript. *(Amended by ADR 0039: the F3 debug pages moved into the
   engine as Dear ImGui dev-tool windows; ONED previews went with ADR 0037.)*

## Consequences

- The no-duck-typing doctrine loses its last excuse: with no hypothetical
  shell to stay compatible with, every Godot-side seam is a typed class or
  a registered contract, and `has_method`/`is`-dispatch has nowhere left to
  hide. The `has_method_guards` ratchet keeps riding down.
- "Shell-neutral" stops appearing in new code and docs as a design
  constraint. Shared-by-game-and-ONED remains a real property of the
  runtime layer (both front-ends compose the same classes) — it is now
  described as exactly that, not as embedder independence.
- ADR 0033's R3 spike (the engine taking the render frame) is unaffected:
  first-class Godot means the DEVICE is Godot's renderer, addressed
  directly; it does not reopen who owns the loops.
