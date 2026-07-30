# docs/render/ — the REN track's RE-record home

The render visual-parity track ([maturity-program.md](../maturity-program.md)
REN; standing rules [ADR 0023](../adr/0023-render-visual-parity.md)). Records
land here as the grill slices convert the three `UNAUDITED` render systems
([divergence-ledger.md](../divergence-ledger.md) audit track):

| Record | Lands at | Catalog | Covers |
|---|---|---|---|
| [`render-material-re.md`](render-material-re.md) | **landed at REN-2** | D-RMAT | the runtime flag/tag→state path down to the device boundary (registry @ 0x5af790/0x5ae690, resolution @ 0x5b03c0, state application @ 0x5d9f50; the planning anchor "Entity_UpdateRenderState @ 0x5d6a30" resolved at REN-5 to the render-slot light updater, renamed `RenderSlot_UpdateEntityLight`) |
| [`render-order-re.md`](render-order-re.md) | **landed at REN-3** | D-RORD | batching, sort keys, technique-class selection, the render-state stack, and the frame pass sequence (Render_SubmitEntity @ 0x5dad80, CRenderBatchQueue_SortAndFlush @ 0x5dae40, Terrain_RenderSceneWithReflection @ 0x5c93a0) |
| [`render-lighting-re.md`](render-lighting-re.md) | **landed at REN-5** | D-RLIT | the iris/modulator chain (env #17), the world lighting block + per-entity uniforms and hemisphere D3D lights, dynamic point lights + group culling, terrain/foliage c0/c1, lighting textures, the cubemap sources (CubeRotSpecular = D-RORD-5's answer), the render-slot shadow lighting |
| [`render-occlusion-re.md`](render-occlusion-re.md) | **landed 2026-07-16** (outside the original three REN slices) | D-OCC | blink-box visibility: section masks, portal traversal, occluder culling, indoor frame gates, the GPM `OVRT`/`OPLN`/`OFAC`/`OOBJ` occlusion chunks, and the sound-occlusion witness (which closed D-SND-7). Sound occlusion (2026-07-16, `CollisionWorld` + `libs/terrain_query`), the indoor frame gates (2026-07-16, `OcclusionFramePass`), and the section-mask/portal engine (init, mask build, traversal, occluder culling — 2026-07-17, `libs/world/src/occlusion.cpp`) are all ported; residuals ride the D-OCC rows |

Terrain TSS findings grow [terrain/terrain-re.md](../terrain/terrain-re.md);
sky/water shader gaps grow [env/env-tod-re.md](../env/env-tod-re.md) — in
place, never forked.

## The parity instrument (REN-1)

Three tiers; tolerances never widen — a divergence triggers a re-grill
(ADR 0023 §4).

**T1 — render-state vectors (the CI gate).**
`tests/renderer/state_vectors_test.cpp` (ctest `renderer_state_vectors`)
walks the full material input matrix through
`classify_object_material` → `build_object_shader_key` →
`compose_object_shader_glsl` against the committed golden
`tests/renderer/render_state_vectors.golden` (2112 input rows + 602 composed
shader hashes). Dump mode (`OPENNOVA_RENDER_VECTORS_DUMP=1`) rewrites the
golden and deliberately fails. Re-dumps carry witness citations in the same
commit. A thin GUT leg (`godot/tests/render_shader_cache_handoff_test.gd`)
pins the GDScript→native binding.

**T2 — swatch A/B (local, mandatory per REN slice).**
`godot/tests/render_swatch_probe.gd` renders one cell per unique object-shader
key (sphere + quad, code-generated textures, orthogonal camera — asset-free,
deterministic) and diffs captures exactly (`compare` mode,
`Image.compute_image_metrics`, max-delta 0). World-level baselines ride the
asset-gated `godot/tests/env_visual_baseline_probe.gd` (ENG-2's driver).
The `composite` mode (REN-3) renders the draw-order scenes — overlapping
translucent layers with depths arranged AGAINST the witnessed order, so only
the ported priority ladder composes them correctly (the water bracket and the
sky ladder; [render-order-re.md](render-order-re.md)).

```
# capture (windowed, never --headless):
"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- capture .scratch/golden/render/<label>
"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- composite .scratch/golden/render/<label>
"$GODOT_BIN" --path godot -s res://tests/env_visual_baseline_probe.gd -- <JOX_dir> .scratch/golden/render/<label> world
# compare two captures (swatch or composite):
"$GODOT_BIN" --path godot -s res://tests/render_swatch_probe.gd -- compare a_grid.png b_grid.png
```

Baselines live under `.scratch/golden/render/` (machine-local, never
committed — [asset-gated-tests.md](../asset-gated-tests.md) policy). The
pre-change baseline set is captured before the first REN behavior change and
each slice's PR attests its A/B.

**T3 — retail side-by-side (the headline gate, attested at REN-7).**
The named scene list, each captured in retail JO and in our runtime from the
same viewpoint/TOD, judged by eye and attested scene-by-scene in the trunk PR:

1. Water horizon at TOD 550 / 1200 / 1845 / 2200 (sun-facing).
2. Alpha-tested foliage/fence close-up (scissor edges, two-sided).
3. Glass / env-mapped vehicle (reflection, fresnel, specular).
4. Transparents composite: smoke or glass over water over sky.
5. Night terrain with lightmapped emplacements (lightmap + modulator scale).
6. First-person viewmodel over the world (draw-order: viewmodel pass).

Retail captures come from the retail JO:CA install (the runtime launch recipe
is `docs/asset-gated-tests.md` + the `oned-run` skill); our side from the
same worldspace via the runtime. Neither side's pixels are committed.

REN-7 state (2026-07-07): the scene-by-scene attestation table lives in the
trunk PR (#207) — our side captured via `env_visual_baseline_probe` (the
water-horizon TOD set, scene 1) and `mission_visual_probe` (CP15 ground POV,
scenes 2/5); scenes 3/4/6 are enumerated there against their tracked rows
(D-RLIT-5, D-RMAT-8, D-RORD-4). The by-eye retail pass is the maintainer's
attestation.
