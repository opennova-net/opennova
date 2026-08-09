# 0028 — engine/ is the engine; godot/adapter/ is the shell adapter

Status: accepted 2026-08-06. §3's target-name clause (per-lib
`opennova_<domain>` targets, families as the only link conveniences) was
superseded by [ADR 0029](0029-engine-group-targets.md) on 2026-08-08: the
groups are now also the CMake build targets. Decision 5's standing rule
("GDScript in `godot/adapter/` wires, adapts, and presents") is superseded
for the GAME runtime by [ADR 0033](0033-engine-owned-loops-device-shells.md)
(2026-08-09): the engine owns the frame; GDScript remains ONED's authoring
language. Directories, the `opennova` namespace, and include paths stand as
written.

## Context

ADR 0016 fixed the layering in words: the portable, Godot-free C++ core "is the
engine", and the Godot-side binding layer is "the shell adapter". The directory tree
contradicted both halves: the engine was named `libs/`, and the adapter was named
`godot/engine/` — its own docs called it "the engine layer". Every conversation and
document paid a translation tax, and the word "engine" meant three things (NovaLogic's
original engine, our portable core, and the adapter directory). The repo has resolved
word collisions before (NovaWorld → GameWorld; "menu host" retired 2026-07); this ADR
applies the same fix to "engine".

At the same time, a gameplay push-down program (this ADR's §Standing rule) needs the
core organized as an engine — a flat list of fifty sibling libraries did not read as
one.

## Decision

1. **`libs/` is renamed `engine/`, with four groups:**
   - `engine/base/` — shared substrate and repo plumbing: io, vfs, resource_index,
     gameprofile, pcapio, oned_edit, refs.
   - `engine/formats/` — one library per NovaLogic format (ADR 0024 unchanged): pff,
     scr, bfc1, pcx, fnt, rtxt, cbin, threedi, tdp, ase, bad, def, avatars, trn, tpj,
     cpt, til, foliage, env, mnu, mns, sbf, lwf, dbf, mus, playersav, oed.
   - `engine/runtime/` — the in-match systems: world, wac, mission, anim, audio,
     particle, renderer, controls, terrain, terrain_query.
   - `engine/net/` — the wire/protocol stack (ADRs 0009–0012, 0019; Model-B-only,
     outside the C ABI): novacrypto, napi, npwire, novaworld, netsim, npruntime.
   - `engine/families.cmake` carries the family link groups (ADR 0024).

2. **Placement of the dual-natured libraries** (by primary identity):
   - env → formats: the `.env` codec plus its keyframe model; runtime consumption
     lives with its consumers.
   - mus → formats: the MUS script format together with its compiler/VM; playback
     selection policy is `runtime/audio`.
   - particle → runtime: the simulator dominates; `.ptl` parsing rides along.
   - oed → formats: the 3DI export session is a format producer beside ase/tdp/threedi.
   - terrain → runtime: LOD/build/query runtime; its on-disk legs (trn, cpt, til, tpj)
     are their own formats libraries.
   - foliage → formats: the per-cell foliage-map raster; scatter/dispatch is
     consumer-side.
   - novacrypto → net: protocol crypto, never part of the C ABI.

3. **Groups are directories only.** Not link groups (families remain the only link
   conveniences, ADR 0024), not namespaces (`opennova` unchanged), not include-path
   segments (`<domain>/...` unchanged), not target-name segments
   (`opennova_<domain>` unchanged). One-lib-per-format stands; no physical merges.

4. **`godot/engine/` is renamed `godot/adapter/`** — ADR 0016's own term. The
   `Nova*` class names, the `opennova` GDExtension target, and `godot/bin/libopennova.*`
   are unchanged. "Engine" no longer names any Godot-side directory.

5. **Standing rule (extends ADR 0016): new engine logic starts in the engine, and
   gameplay loops live in the engine.** ADR 0016 already banned witnessed engine math
   from GDScript; this ADR extends the same rule to gameplay orchestration — tick and
   catch-up arithmetic, mission boot ordering, session/admission state machines, the
   player weapon/loadout cluster, sim-consumed asset resolution, and witnessed
   presentation timing. GDScript in `godot/adapter/` wires, adapts, and presents; the
   shells own nodes, devices, and scene lifetime. Presenting means writing Godot
   resources from engine-decided state, never re-deriving the state.

## Consequences

- Both CMake roots' `add_subdirectory` paths, the lint tier's scan roots/scopes, the
  CI image-workflow path filters, and the `res://` tree were rewritten in the same
  change; ratchet keys renamed `libs_*` → `engine_*` with values carried verbatim.
- The renderer's private view of oed headers is expressed as a target-property
  include (`$<TARGET_PROPERTY:opennova_oed,INTERFACE_INCLUDE_DIRECTORIES>`) instead
  of a sibling-relative path.
- CONTEXT.md gains Layers entries: Engine, Adapter, Shell, Simulation.

## What we deliberately did not do

- Rewrite ADRs 0001–0027: historical path citations stay as written (the ADR 0019
  precedent). A path in an old ADR describes the tree at decision time.
- Rename targets or namespaces, or alter family topology (ADR 0024).
- Introduce any new intermediate layer as part of the reorganization (ADR 0027).
