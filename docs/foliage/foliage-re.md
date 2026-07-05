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
| Foliage instance color | **DIVERGENT (approximation)** | D-FOLIAGE-1 |

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
- **Color**: `sample_terrain_lightmap @ 0x606030` at the instance ±0x8000 on both
  axes (4 samples), 2×2 SWAR average of RGB + alpha (the tint ported for env #19,
  D-ENV #19); the emitter's per-vertex color is `0xFF000000 | (0x404040 +
  (avg>>1))` under a 2× draw.

## D-FOLIAGE divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-FOLIAGE-1 | A | OPEN (approximation) | Foliage instance color: the host bakes ONE color per `MultiMesh` instance (the 2×2 lightmap average `[orig: sample_terrain_lightmap @ 0x606030]`), where the engine emits a per-VERTEX quad color `0xFF000000 | (0x404040 + (avg>>1))` under a 2× draw (half-plus-bias) and reads an alpha-premultiplied colormap. Visually close (same average tone); the per-vertex gradient + the exact 2× half-plus-bias are the residual. Rides the foliage render-emitter parity. |

Placement itself carries **no divergence** — the seed, PRNG, candidate count,
surface gate, and proximity spacing are byte-exact against retail.

## Cross-references

- Reimpl: `libs/foliage` (`placement.cpp`/`dispatcher.cpp`),
  `godot/engine/terrain/nova_foliage_dispatcher.cpp` (the `MultiMesh` host).
- The terrain tint the color path consumes is [env/env-tod-re.md](../env/env-tod-re.md)
  #19 (`env::foliage_lightmap_tint`).
- Tiles (`libs/til`) and terrain rendering remain jodemo-cited; their audits
  (PAR-R3/R1) want the jodemo IDB (divergence-ledger.md UNAUDITED table).
