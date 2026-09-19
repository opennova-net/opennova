# ADR 0044: shared native assets

- **Status**: accepted (2026-09-18; PR #653)
- **Supersedes/updates**: amends [ADR 0043](0043-canonical-cpp-and-godot-hard-cut.md) d4 —
  the NET-AGNOSTIC runtime tree list loses `simassets` and gains `assets`; the
  `engine/runtime/simassets` mentions in [ADR 0029](0029-engine-group-targets.md) and
  [ADR 0031](0031-adapter-composition-contract.md) are historical (the directory is gone).

## Context

The simulation and the Godot presentation each parsed and cached the same assets: the
simulation through its private model cache, Godot through independent model and skeletal
loading. One 3DI model, ADM map, BAD animation or compiled skeletal rig could be read and
held twice per process, and a refresh of one side did not reach the other.

## Decision

Simulation and Godot presentation share one native `AssetStore` per mounted resource
source. It retains immutable parsed 3DI models, ADM maps, BAD animations and compiled
skeletal rigs; consumers retain owning handles. This replaces the simulation's private
model cache and Godot's independent model and skeletal loading, preserving headless
execution and the existing format representations.

The resource index supplies a content revision. Remounts, failed scans and decode policy
changes invalidate subsequent asset lookups; explicit invalidation also supports edited
loose files. The existing process-wide cache epoch now lives with the native resource
index so a refresh reaches simulation lookups immediately. Previously issued handles
remain valid snapshots until consumers release them. There is no global cache keyed by
raw index pointers. The source owns the store and outlives its lookups; asset handles
can outlive both.

Animation playheads, channel rotation, entity traits and collision instances stay with
their runtime owners. Meshes, materials, skeleton nodes and presentation caches stay in
Godot. Shared skeletal loading and evaluation live in `runtime/anim`; mission population
lives in `runtime/mission`; world collision and attachment poses live in `runtime/world`;
first-person presentation rules live in `runtime/renderer`. The former `runtime/simassets`
grouping and duplicate loaders are removed without compatibility forwarding headers.
Existing retained definition rows remain shared inputs to entity initialization.

## Consequences

Model registration for the retail network challenge still records presentation loads
only. Sharing a parsed model does not make a headless lookup count as a presentation
load or change authored clip variant order, mount precedence, animation timing, or
collision behavior.

## Verification

The `asset_store` ctest (`tests/assets/asset_store_test.cpp`) pins the store's retention,
revision invalidation and handle-snapshot rules; `godot/tests/shared_native_assets_test.gd`
pins the Godot side sharing the same store. The live wiring is described in
[runtime-architecture.md](../runtime-architecture.md) ("Shared native assets");
`runtime/assets/asset_store.cpp` sits in the citation allowlist as infrastructure.
