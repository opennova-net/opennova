# 0044 - Shared native assets

Accepted 2026-09-18. Simulation and Godot presentation share one native
`AssetStore` per mounted resource source. It retains immutable parsed 3DI
models, ADM maps, BAD animations and compiled skeletal rigs; consumers retain
owning handles. This replaces the simulation's private model cache and Godot's
independent model and skeletal loading, preserving headless execution and the
existing format representations.

The resource index supplies a content revision. Remounts, failed scans and decode
policy changes invalidate subsequent asset lookups; explicit invalidation also
supports edited loose files. The existing process-wide cache epoch now lives
with the native resource index so a refresh reaches simulation lookups
immediately. Previously issued handles remain valid snapshots until consumers
release them. There is no global cache keyed by raw index pointers.
The source owns the store and outlives its lookups; asset handles can outlive both.

Animation playheads, channel rotation, entity traits and collision instances stay
with their runtime owners. Meshes, materials, skeleton nodes and presentation
caches stay in Godot. Shared skeletal loading and evaluation live in
`runtime/anim`; mission population lives in `runtime/mission`; world collision
and attachment poses live in `runtime/world`; first-person presentation rules
live in `runtime/renderer`. The former `runtime/simassets` grouping and duplicate
loaders are removed without compatibility forwarding headers. Existing retained
definition rows remain shared inputs to entity initialization.

Model registration for the retail network challenge still records presentation
loads only. Sharing a parsed model does not make a headless lookup count as a
presentation load or change authored clip variant order, mount precedence,
animation timing, or collision behavior.
