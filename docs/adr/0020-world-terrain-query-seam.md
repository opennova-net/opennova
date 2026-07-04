# ADR 0020: the world→terrain seam — libs/terrain_query

- **Status**: accepted (2026-07-04, rides the LIBS-1 seam PR)
- **Owners**: maturity program LIBS track
- **Supersedes/updates**: executes the LIBS-1 slice of the maturity
  umbrella (docs/maturity-program.md); sequenced after ADR 0019's npwire
  extraction so link topology churned once.

## Context

`libs/world` (the runtime world: AI grounding, the round sim's terrain
stop) consumes exactly one terrain capability — height at a world
position — through three free functions over a POD: `TerrainHeightField`
plus the `height_field_*` samplers, sitting on the world→source
coordinate kernel (`coords.h`). But it linked ALL of `libs/terrain` (the
format/bake stack: builder, quadtree, lighting, CDEP, mesh), which
PUBLIC-links cpt/pcx/tpj/trn (and trn → foliage). Every lib above world —
wac, mission, netsim, npruntime, and the net stack through them — dragged
the whole terrain-format family through its link closure while including
none of it (verified pre-move: zero terrain-family includes outside
`libs/world` itself).

## Decision

1. **The seam is a leaf lib, `libs/terrain_query`** (CMake target
   `opennova_terrain_query`, ZERO link dependencies): `height_field.h`,
   `coords.h`, and `height_field.cpp` move there from `libs/terrain` via
   `git mv`. The de-facto interface IS the existing surface — the
   `TerrainHeightField` POD + the `height_field_*` free functions — not a
   new abstraction invented for the cut.
2. **Include shape and namespace are preserved**: headers stay at
   `<terrain/height_field.h>` / `<terrain/coords.h>` in namespace
   `opennova::terrain`, so the move is invisible to every call site.
3. **Direction**: `world → terrain_query`; `terrain → terrain_query`
   (PUBLIC, so terrain's own consumers keep compiling unchanged).
   wac/mission/netsim/npruntime/novaworld now reach terrain data only
   through the query leaf, never the format stack.
4. **The provider stays `godot/engine`** (unchanged by this slice):
   `NovaTerrainData` builds the POD from its own cpt/trn (the
   `height_field_from` helper backing its `get_height*` methods), and
   `NovaSimulation::set_terrain_height_field` wires it to
   `world_->terrain` / `ai_->terrain`. The null path — no terrain wired —
   stays supported, so headless/tests run terrain-free.
5. **terrain_query is the growth point for ENG-3**: when engine-side
   terrain raycast/collision lands, the world-owned
   IHeightSampler/ITerrainCollision interfaces grow here (world keeps
   consuming the seam; providers implement it), rather than world ever
   re-linking the format stack.
6. **The forbidden-edge check is permanent**:
   `scripts/lint/link_graph_check.py` asserts the TRANSITIVE link closure
   (graphviz dump of the configured tree, with a CMake-file walk fallback
   for pre-build contexts) against `scripts/lint/forbidden_edges.json` —
   wac/mission/net libs never reach
   terrain/cpt/til/trn/tpj/foliage, and npwire never reaches sqlite
   (ADR 0019). Soft in Wave 1, hard-fail forever after, per the umbrella's
   enforcement table.

## Consequences

- The wac/mission/netsim/npruntime/novaworld link closures no longer
  contain the terrain-format libs; `ground_height_test` (links
  `opennova_world` alone yet constructs a `TerrainHeightField`) is the
  compile-time canary that the seam surface stays reachable through world.
- `opennova_terrain_query` is registered in both CMake roots (the
  top-level build and `godot/engine`'s standalone tree); the GDExtension
  links it explicitly since `nova_terrain_data`/`nova_simulation` include
  its headers directly (the ADR 0019 no-free-riding precedent).
- terrain_query is NOT added to `OPENNOVA_CORE_TARGETS`: it has no C ABI
  exports (C++ namespaced functions only), so the FFI DLL's export
  surface is unchanged; consumers resolve it transitively.
- The moved TUs keep the no-FMA-contraction FP flags they compiled under
  inside `opennova_terrain`, so sampler codegen (and the hand-math test
  expectations) are unchanged on every toolchain.
- PUBLIC vs PRIVATE text is deliberately NOT what the check reads: a
  STATIC lib's PRIVATE deps still propagate as `$<LINK_ONLY>` onto every
  downstream link line, so the check walks actual edges transitively.
