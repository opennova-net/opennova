# ADR 0038: Native runtime assets now; GLB at the future editor seam

- **Status**: accepted (2026-08-26; hard cut)
- **Owners**: native formats, future editor
- **Supersedes/updates**: retires the Python importer, Qt application, Blender
  add-ons, ASE/OED/TDP authoring pipeline, and the flat shared-library model in
  ADR 0024. Updates ADRs 0027, 0029, 0034, and 0037 where they describe those
  retired surfaces. Their runtime and layering decisions remain in force.

## Context

The repository carried several overlapping object-authoring routes: a Qt
importer, Python FFI packages, Blender add-ons, and a native ASE/3DP/OED
pipeline. They duplicated scene knowledge, made release and CI depend on a
second language toolchain, and exposed editing state through the Godot runtime
adapter. None of those modules is required to run retail game data.

The editor will eventually use GLB/GLTF as its scene interchange format. That
work is deliberately separate from this cleanup and is not needed by the
current game runtime.

## Decision

1. The repository has no Python, Qt importer, Blender add-on, or DCC-specific
   package. The cut is literal: no compatibility modules or deprecated entry
   points remain.
2. Native ASE, TDP/3DP, and OED authoring modules are removed. Godot
   `ObjectData` loads and evaluates immutable 3DI runtime documents only.
3. ADM and BAD remain runtime input formats. Their readers remain; repository
   writer/export interfaces are removed. Native 3DI reading and writing remain
   format capabilities, but Godot exposes no object-export command yet.
4. A future editor will accept GLB/GLTF scenes and produce 3DI, and will read
   3DI and produce GLB. Neither direction is implemented by this decision.
5. The future exchange seam is the ordinary scene plus the stable naming
   convention in `docs/threedi/scene-naming-contract.md`. Conversion must not
   depend on another converter, importer-private state, Blender custom
   properties, application metadata, or an import having happened first.
6. Exact GLTF representation for LODs, collision data, animation events, and
   other Nova-specific fields is future editor design work. This decision does
   not reserve private extras or carry the deleted Blender schema forward.

## Consequences

- The runtime module has one small interface: load 3DI and expose the parsed
  document for rendering, collision, animation, and inspection.
- Build, test, and release no longer install Python or package DCC tooling.
- Checked-in native WAC tables and Godot shader sources are authoritative
  sources rather than outputs whose missing Python generators are implied.
- Historical reverse-engineering records may continue to discuss ASE, OED, or
  3DP as provenance. They must not advertise those paths as current products.

## Verification

- The tracked tree contains no Python, ASE, 3DP, or Blender scene files.
- Native and Godot builds succeed with no authoring-format or shared-FFI target.
- Runtime tests exercise ADM/BAD readers and immutable 3DI fixtures.
- Active product and release documentation names only the native runtime and
  the future GLB/GLTF editor seam.
