# Mission-load performance baseline (2026-06-11)

The recorded numbers behind the editor-depth roadmap's perf decisions. C1
(PR #97) instrumented both hosts with `PerfTimeline`; this is the first
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
  scene (`modtools/terrain/terrain_editor.tscn`), sets the resource root to
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

## Reproduction

```text
# /tmp/perf_capture.gd: SceneTree script — instantiate
# res://modtools/terrain/terrain_editor.tscn, _set_resource_root_dir(<assets>),
# open_in_workspace("mission", <path>) per mission, then dump
# PerfTimeline.latest().spans().
Godot_v4.6.1-stable_win64_console.exe --headless --path godot -s perf_capture.gd
```

The same data is visible interactively in the debug overlay's Perf tab
(C11) after any mission load, in either host.
