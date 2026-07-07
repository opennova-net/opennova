# Foliage placement — reverse-engineering record

Structure-mapping record for the original engine's **procedural foliage
placement** (the deterministic per-cell grass/bush instancing) and its color
sampling. The reimplementation surface is `libs/foliage` (`placement.cpp` /
`dispatcher.cpp`) and the Godot host `NovaFoliageDispatcher`
(`godot/engine/terrain`). Binary: retail **Jointops.exe** (IDB
`Jointops.exe.kong.i64`). All addresses below are that binary's. This file is
the committed home for the `D-FOLIAGE-…` divergence catalog. Produced by a
read-only IDA audit (PAR-R2, 2026-07-05); no IDB renames were made. It converts
the **Foliage** system from `UNAUDITED` to tracked (divergence-ledger.md).

Note on binaries: `libs/foliage/placement.cpp` was originally ported from
`jodemo.exe` (it cites `sub_5C0240`/`sub_5C6450`/`sub_5C65E0`); this audit
**re-confirms the placement byte-for-byte against the RETAIL function**
`generate_foliage_instances_0 @ 0x600197` — identical seed, PRNG, and gates —
so the port is faithful to the shipped product, not just the demo.

## Verdict table

| Component | Verdict | Evidence |
| --- | --- | --- |
| Per-cell placement (seed + PRNG + gates) | **MATCHING** | `libs/foliage/placement.cpp` = retail `generate_foliage_instances_0 @ 0x600197` field-for-field (below) |
| Foliage instance color | **DIVERGENT (approximation, narrowed 2026-07-07)** | D-FOLIAGE-1 — the half-plus-bias emitter form is applied; the per-vertex gradient is the residual |
| Fragment combine (the blend PS) | **MATCHING (ported 2026-07-07)** | D-FOLIAGE-2 FIXED — `foliage.gdshader` runs the witnessed `t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8` chain (t1 = planar-projected colormap; c0/c1 = the SKY/LIGHT blocks) `[orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0]`; witness text in [terrain/terrain-re.md](../terrain/terrain-re.md) §Foliage / sector models |
| Wind-sway vertex path | **DIVERGENT (stand-in)** | D-FOLIAGE-3 — axis/weight/waveform diverge from `Foliage_WindSwayVS`; the sway state globals are live |

## The witnessed placement (`generate_foliage_instances_0 @ 0x600197`)

- **Cell key**: `key & 0x3FF` (x), `HIWORD(key) & 0x3FF` (z) — 10-bit tile
  coords; the sign bit (`0x80000000`) → empty cell (0 instances).
- **PRNG seed**: `state = (key & 0x1FF01FF) + ROL32(0xA55B1EED, key & 0x1F)`
  (`-1520754963` = **0xA55B1EED**; our `PRNG_SEED_CONST`, `int_convert`-verified).
- **PRNG step**: `state = ROL32(state + ROL32(state, 11), 4)`, value `= (u16)state ^ 1`
  (our `placement.cpp` `rol32(state + rol32(state, 11), 4) ^ 1`).
- **Candidates per cell**: **36** (`random_angle < 36`; our `FOLIAGE_CANDIDATES_PER_CELL = 36`,
  a 6×6 grid).
- **Surface gate**: an instance is placed only where `(1 << slot) &
  Terrain_GetSurfaceTypeAtFixedPoint(x, -z) @ 0x6066d0` is set (our port's
  `(1 << slot) & sub_5C65E0(...)` surface-mask gate).
- **Proximity/spacing**: a `0x20000` (2.0 world units, 16.16) path/spacing
  reject via `sub_606490` (our `samplers.path_blocked(x, -z, 0x20000)`), skipped
  when the foliage-type flag `byte_2C2608C & 1` is set.
- **Quad geometry**: each accepted instance emits a quad sampled at its 4 corners
  (`sub_606000` at `±0x10000`) for tangent/normal; our port mirrors the corner
  geometry.
- **Color**: `sample_terrain_colormap_tinted @ 0x606030` at the instance ±0x8000 on both
  axes (4 samples), 2×2 SWAR average of RGB + alpha (the tint ported for env #19,
  D-ENV #19); the emitter's per-vertex color is `0xFF000000 | (0x404040 +
  (avg>>1))` under a 2× draw — the half-plus-bias form is applied by the host
  dispatcher since 2026-07-07 (alpha forced opaque; per-channel `0x40 + avg/2`
  cannot carry).

## The witnessed fragment combine (ported 2026-07-07)

The blend PS `Foliage_LightmapBlendPS @ 0x5ff7a0` (witness text in
[terrain/terrain-re.md](../terrain/terrain-re.md) §Foliage / sector models):
`rgb = t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8`, `a = t0.a × v0.a` — t1 is the
terrain colormap sampled by a planar world-position projection (VS oT1 =
dp4(world, c7/c8); host: uv = `(x, −z) / texsize`, wrap — identical to the CPU
sampler `NovaTerrainData::get_colormap_color_world`), c0 = the SKY block and
c1 = the LIGHT block — the same post-modulator constants the terrain surface
serves (hosted as the `opennova_sky_ambient` / `opennova_sun_light` globals).
`foliage.gdshader` previously ran an unwitnessed ratio stand-in
(`(sun/(ground·0.707+sun))×255/128`, no colormap sample, no SKY term) — the
gobj-era family the gamma-faithful pipeline (D-RMAT-7) exposed; D-FOLIAGE-2.

## D-FOLIAGE divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FOLIAGE-1 | A | OPEN (approximation — narrowed 2026-07-07) | Foliage instance color: the host bakes ONE color per `MultiMesh` instance where the engine emits a per-VERTEX quad color (four corner samples) and reads an alpha-premultiplied colormap. The half-plus-bias emitter form (`0xFF000000 | (0x404040 + (avg>>1))`) IS applied since the combine slice; the per-vertex gradient + the premultiplied read are the residual. Rides the foliage render-emitter parity. |
| D-FOLIAGE-2 | A | **FIXED (2026-07-07, REN-6 rider)** | The fragment combine was an unwitnessed ratio stand-in (`(sun/(ground·0.707+sun))×255/128` — no terrain-colormap sample, no SKY term; the shader header already carried the correct witness) vs the witnessed `rgb = t0 × (t1 × (t1.a·c1 + c0)) × v0 × 8, a = t0.a × v0.a` `[orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0; constants terrain_setup_lighting_and_shader @ 0x604420]`. Ported 1:1: planar uv `(x,−z)/texsize` wrap ≡ the CPU sampler; the dispatcher binds the colormap from the CPU-color source chain (runtime terrain → editor colormap source) with a neutral no-terrain fallback (retail never draws foliage without a colormap). |
| D-FOLIAGE-3 | A | OPEN (stand-in — minted 2026-07-07) | Wind sway: the host displaces X weighted by height (`VERTEX.y/8`, `sin(phase + 0.11x + 0.07z)`) where the witnessed VS displaces Z weighted by vertex RED via a polynomial sine of `world.x·c24.y + time` `[orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630; Foliage_WindSwayVS @ 0x2c25e5c]`. The sway amount/phase state is live (NovaWeather globals); the axis/weight/waveform ride the foliage render-emitter parity. |

Placement itself carries **no divergence** — the seed, PRNG, candidate count,
surface gate, and proximity spacing are byte-exact against retail.

## Cross-references

- Reimpl: `libs/foliage` (`placement.cpp`/`dispatcher.cpp`),
  `godot/engine/terrain/nova_foliage_dispatcher.cpp` (the `MultiMesh` host).
- The terrain tint the color path consumes is [env/env-tod-re.md](../env/env-tod-re.md)
  #19 (`env::foliage_lightmap_tint`).
- Tiles (`libs/til`, PAR-R3) is jodemo-cited in code but retail-auditable
  (`PolyTrn_RenderTile @ 0x60df0d`, `serialize_terrain_tiles @ 0x6080F0`), just
  multi-part; terrain (PAR-R1) is the larger renderer/mesh pipeline. See the
  divergence-ledger.md UNAUDITED table for the per-audit binary scoping.
