# Mission-load performance baseline (2026-06-11)

The recorded numbers behind the editor-depth roadmap's perf decisions. C1
(PR #97) instrumented both products with `PerfTimeline`; this is the first
captured baseline, taken to decide which push-down the next perf slice
should be (the roadmap's C3 placement-plan / C4 pick-collider candidates
were *contingent on these numbers*).

## Capture setup

- **Date / commit:** 2026-06-11, master `c6046438`.
- **Machine:** Windows 11 Pro (DESKTOP-AKR2K8V), local NVMe, files OS-warm
  (second run of the session; the very first cold-cache run read ~10% higher
  with the same shape).
- **Godot:** 4.6.1-stable console build, `--headless`.
- **GDExtension:** Debug (`template_debug`) — C++ stages (model/texture
  decode) read slower than a release build in absolute terms; the *ratios*
  are what this doc decides on.
- **Method:** a headless `SceneTree` probe instantiates the real editor main
  scene (`modtools/editor/editor_main.tscn`), sets the resource root to
  a retail JO loose-asset dir, and opens each mission through
  `open_in_workspace("mission", path)` — the exact interactive path, so the
  `mission_controller.open_mission` timeline prints unmodified.
- **Missions:** the three largest retail `.bms` by file size on distinct
  terrains (CP15 398 KB, ASH_I1gA 398 KB, 03TR 396 KB).
- **Headless caveat:** no GPU upload / shader compilation / first-presented
  frame in these numbers. Every recorded stage is CPU-side (parse, decode,
  node building) — the costs the roadmap's push-down slices target.

## Results (per-span milliseconds, cold open)

| Span | CP15 | ASH_I1gA | 03TR |
|---|---:|---:|---:|
| **total** | **50,624** | **9,782** | **21,245** |
| parse | 2 | 2 | 2 |
| terrain | 239 | 210 | 196 |
| &nbsp;&nbsp;trn_data | 171 | 158 | 145 |
| &nbsp;&nbsp;heightmap | 10 | 11 | 11 |
| &nbsp;&nbsp;textures | 22 | 25 | 25 |
| &nbsp;&nbsp;sectors | 8 | 2 | 1 |
| environment | 26 | 14 | 12 |
| **objects** | **50,279** | **9,479** | **20,959** |
| &nbsp;&nbsp;bucket_entities | 23 | 23 | 25 |
| &nbsp;&nbsp;static_batches | 6,452 | 9,186 | 4,467 |
| &nbsp;&nbsp;pick_colliders | 160 | 234 | 227 |
| &nbsp;&nbsp;**animated_models** | **43,636** | 0 | **16,192** |
| overlays | 29 | 29 | 28 |

Reopening the already-open mission is a ~2 ms no-op (the shell's
skip-reopen guard); same-terrain *different*-mission opens additionally skip
the terrain remount (C2, PR #100), but terrain is no longer a meaningful
cost either way.

## Verdict: what the next perf slice should be

1. **`animated_models` is the budget.** 86% of CP15's load, 76% of 03TR's.
   Each animated entity builds its own `NovaObjectModel` — a full `.3di`
   decode plus texture decode *per instance*, with no template sharing,
   unlike the static path which builds one template per graphic and
   instances it. The evidence-based slice is **a shared model/texture
   template cache for animated entities** (decode each graphic once, clone
   per instance) — the "swap" the roadmap reserved for exactly this case.
2. **`static_batches` is the secondary target** (4.5–9.2 s; it is ASH's
   entire cost). Same family: per-graphic template build = `.3di` + texture
   decode; a decode cache across graphics sharing textures would cut it too.
3. **C3 as specced (portable bucketing push-down) is refuted:**
   `bucket_entities` costs 23–25 ms. Bucketing is free; the cost is decode.
4. **C4 as specced (pick-collider RID rework) is refuted as a LOAD win:**
   `pick_colliders` costs 160–234 ms. Its remaining value is memory /
   lifecycle hygiene, not load time — schedule on those merits or not at all.
5. Terrain, environment, parse, overlays: all under 250 ms — leave alone.

## After the wave-6 perf slice (2026-06-11)

Same machine, same protocol (second, OS-warm run; Debug GDExtension),
captured after the perf slice landed three changes:

1. **`NovaResourceRoot.resolve_file` memo** — the dominant cost. Every call
   walked the *entire* root directory (`DirAccess` listing, ~10k entries for
   a retail extract), and `NovaObjectData.get_materials()` resolves every
   texture of every material through it on every model rebuild. CP15 spent
   ~23 s of its load in these walks. Now one walk per cache epoch feeds a
   name→path memo (case-variant duplicates poison their key, preserving the
   duplicate-name error). A companion decoded-texture cache backs
   `load_texture`'s packed-PFF fallback, which re-extracted + re-decoded per
   call on runtime mounts.
2. **`NovaObjectData` submesh cache** — `build_lod_submeshes` results are
   memoized per `(lod, skeletal, bone_count)`; cache hits hand out the same
   `ArrayMesh` refs (entry dictionaries deep-copied, so callers can't taint
   the cache). The mission placer shares one `NovaObjectData` per graphic,
   so N animated soldiers now share meshes instead of paying N mesh builds.
   Materials stay per-instance (runtime shader params are per-entity).
3. **Placer build-order fix** — `_apply_skeletal_anim` now runs *before*
   `set_object_data`, so each animated entity does one skeletal-keyed build
   instead of building a static-keyed mesh set first and throwing it away.

| Span | CP15 before | CP15 after | ASH before | ASH after | 03TR before | 03TR after |
|---|---:|---:|---:|---:|---:|---:|
| **total** | 50,624 | **5,134** | 9,782 | **1,274** | 21,245 | **3,764** |
| objects | 50,279 | 4,779 | 9,479 | 1,024 | 20,959 | 3,525 |
| &nbsp;&nbsp;static_batches | 6,452 | 520 | 9,186 | 743 | 4,467 | 391 |
| &nbsp;&nbsp;pick_colliders | 160 | 159 | 234 | 219 | 227 | 208 |
| &nbsp;&nbsp;animated_models | 43,636 | **4,068** | 0 | 0 | 16,192 | **2,859** |

CP15 `animated_models` 43.6 s → 4.1 s (10.7x) against the slice's <9 s
acceptance gate; whole-mission opens are 5.6–9.9x faster.

**Where the remaining animated cost lives:** ~3.3 s of CP15's 4.1 s is 15
distinct `.adm` body-animation sets loading at ~220 ms each
(`NovaSkeletalAnim.load_from_resource_root`, cached per `.adm` by the
placer — the cost is intrinsic first-load parsing, once per distinct
animation set per mission open). If a future slice wants it, the lead is
sharing parsed clip/skeleton data across `.adm` sets, not more caching at
the placer layer.

## Reproduction

```text
# Committed probe (the capture above): instantiates the real editor main
# scene, mounts JO_ASSETS_DIR, opens each mission through
# open_in_workspace("mission", path), dumps PerfTimeline spans.
JO_ASSETS_DIR=<retail loose extract> \
Godot_v4.6.1-stable_win64_console.exe --headless --path godot \
    --script res://tests/mission_load_perf_probe.gd
```

Record the second run of a session (OS-warm); override the mission list
with JO_PROBE_MISSIONS=a.bms,b.bms when needed. The same data is visible
interactively in the debug overlay's Perf tab (C11) after any mission load,
in either host.
