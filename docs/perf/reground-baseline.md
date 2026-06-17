# Bulk re-ground performance baseline (2026-06-12)

The recorded numbers behind the re-ground perf slices. The activate-time
"terrain changed under N objects" flow (drift scan -> prompt -> bulk apply)
was reported as very slow; this captures where the time actually goes, with
the cycle instrumented by `PerfTimeline` (same idiom as the mission-load
spans, see mission-load-baseline.md).

## Capture setup

- **Date / branch:** 2026-06-12, `terrain-workspace-polish` (after the
  request-cache slice — see the arithmetic note below for the pre-slice
  shape).
- **Machine:** Windows 11 Pro, local NVMe, OS-warm second run.
- **Godot:** 4.6.1-stable console build, `--headless`.
- **GDExtension:** Debug (`template_debug`).
- **Method:** `godot/tests/mission_reground_perf_probe.gd` — opens each
  retail mission through `open_in_workspace("mission", path)`, raises the
  entire editable heightmap 5 world units through the editor's own
  height-commit seam (`_set_heightmap_image`, bumping the height revision),
  then drives `reconcile_with_terrain()` (the activate-time scan) and
  `reground_drifted()` (the prompt-confirm apply). Run with `JO_ASSETS_DIR`
  pointing at a retail JO loose-asset directory.
- **Missions:** the same three as the load baseline (CP15, ASH_I1gA, 03TR);
  the whole-surface raise drifts every groundable entity (~2,000-2,100 rows).
- **Headless caveat:** the `rebake` span measures only the CPU side of
  re-placing every object. In the interactive editor the same re-bake also
  tears down and recreates ~2,000 RenderingServer MultiMesh instances and
  PhysicsServer pick bodies — the visible frame hitch is larger than the
  number below. (Same caveat as the load baseline.)

## Results (per-span milliseconds)

| Span | CP15 (2,096 moved) | ASH_I1gA (2,015) | 03TR (2,049) |
|---|---:|---:|---:|
| **drift scan total** | **24** | **26** | **34** |
| &nbsp;&nbsp;requests (cache miss: marshal + sample) | 22 | 24 | 31 |
| &nbsp;&nbsp;count (engine dry-run) | 2 | 2 | 2 |
| **re-ground total** | **343** | **262** | **312** |
| &nbsp;&nbsp;requests (cache hit) | 2 | 1 | 2 |
| &nbsp;&nbsp;apply (engine bake + write) | 2 | 2 | 2 |
| &nbsp;&nbsp;baseline (re-record from cached rows) | 1 | 1 | 1 |
| &nbsp;&nbsp;**rebake (full world re-place)** | **257** | **141** | **230** |

## Pre-slice arithmetic

Before the request cache, the cycle built the request set three times
(activate-time count, apply, baseline re-record), each paying the full
marshal + per-entity sampling (~22-31 ms here): the `requests` and
`baseline` rows above were each another ~25 ms. The cache collapses the
cycle to one build; the remaining cache-miss build (the drift-scan row) is
the per-cycle floor.

## Verdict

1. **The re-bake is the apply's budget** (75-85% headless, more
   interactively per the caveat). A bulk re-ground changes no membership and
   only entity Z, so the next slice replaces the full re-place with an
   in-place transform update of the moved entities' MultiMesh slots /
   animated nodes / pick bodies, falling back to the re-bake only when a
   record is unmappable.
2. The remaining cache-miss build (~25 ms) is dominated by the
   `get_all_entities` Dictionary marshal plus ~6 GDScript->C++ boundary
   crossings per entity for height sampling; a batch sampler over the live
   heightmap collapses the sampling to one call. Worth taking while the
   seams are open; the deeper C++ request-build pushdown sketched in the
   plan is **not** justified by these numbers.

## After the targeted apply (same protocol, same session)

The in-place update replaced the re-bake; `update` covers the slot/node/pick
writes for every moved entity plus the marker-overlay refresh (markers are
mesh-less, their gizmos live in the overlay — ~29 ms of the span):

| Span | CP15 | ASH_I1gA | 03TR |
|---|---:|---:|---:|
| **re-ground total** | **46** (was 343) | **48** (was 262) | **44** (was 312) |
| &nbsp;&nbsp;requests (cache hit) | 2 | 3 | 2 |
| &nbsp;&nbsp;apply (engine bake + moved-row report) | 4 | 5 | 4 |
| &nbsp;&nbsp;baseline | 1 | 1 | 1 |
| &nbsp;&nbsp;**update (in-place, no re-bake)** | **38** | **38** | **36** |

Interactively the gap is wider than 6-7x: the old re-bake also tore down and
recreated ~2,000 RenderingServer MultiMesh instances and PhysicsServer pick
bodies per apply; the targeted path touches only existing objects.

## After the batch height sampler (same protocol, same session)

`NovaTerrainData.sample_heights_world_live` collapses the ~6 boundary
crossings per entity to one call per build (parity with the scalar sampler
pinned by terrain_height_revision_test). The honest result: the cache-miss
build stays ~31-34 ms — it is **marshal-bound** (`get_all_entities`'s
per-entity Dictionary walk), and the sampling it removed was only a few ms
at this entity count. The batch call still pays off as headroom (debug
builds, larger missions) and as the one C++ sampler future per-point loops
(foliage/tile previews) can adopt — but it does not move these numbers,
which is exactly why the deeper C++ request-build pushdown stays shelved:
the scan runs once per height-edit-then-activate and already fits in two
frames.

| | CP15 | ASH_I1gA | 03TR |
|---|---:|---:|---:|
| drift scan total | 36 | 36 | 34 |
| re-ground total | 38 | 44 | 37 |

End to end, the user-facing flow on a ~2,100-entity retail mission went
from ~370-460 ms of CPU (3x build + count + apply + full re-bake) to
~70-80 ms (scan 34-36 + apply 37-44), with the interactive hitch from
instance/body recreation gone entirely.
