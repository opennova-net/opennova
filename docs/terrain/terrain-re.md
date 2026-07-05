# Terrain — reverse-engineering record (PARTIAL)

Structure-mapping record for the original engine's **terrain** pipeline — the
heightmap/mesh build, the quadtree LOD, CDEP, lighting/modulation, mesh
simplification, and byte packing. The reimplementation surface is `libs/terrain`
(+ `libs/terrain_query`, the world→height seam, ADR 0020) and the Godot terrain
host. Binaries: **both** `jodemo.exe` (the accessible LOD/quadtree/mip renderer)
and retail **Jointops.exe** (lighting/modulation/fog). This file is the committed
home for the `D-TERRAIN-…` catalog. Produced 2026-07-05 (PAR-R1).

**Status: PARTIAL.** Terrain is the largest system and the last of the seven
`UNAUDITED` systems; this record establishes the tracked surface — the module
map, the mixed-binary witness basis, and the one known deliberate divergence —
and honestly scopes the deep byte-parity grill (mesh simplification + the CDEP/LOD
bitstream) as the remaining work, the way [mission/mis-format-re.md](../mission/mis-format-re.md)
is a partial. It converts terrain from `UNAUDITED` to *tracked (partial)*.

## Module map (`libs/terrain`) and witness basis

| Module | Role | Witness |
|---|---|---|
| `builder` | heightmap → terrain mesh (the build pipeline entry) | the TrnGen byte-identical data path (canonical reference) |
| `quadtree` / `build_quadtree` / `lod` | quadtree LOD traversal, frustum culling, height mipchain | **jodemo** `Terrain_TraverseQuadTreeNode @ 0x5C89C0`, `Terrain_CollectVisibleSectors @ 0x5C9120`, `Terrain_BuildHeightMipChain @ 0x5C5310` |
| `cdep_constraint` | quantized [min,max] of the 256 pixels of a block (CDEP depth constraint) | documented in-code; full CDEP bitstream grill pending |
| `lighting` | terrain lighting colors + per-position modulation | **retail** `Terrain_SetLightingColors @ 0x5C4B10`, `Terrain_GetModulatedColorAtPos @ 0x5C5FE0`; fog via `Render_SetFogState @ 0x58a950` → `CD3DDevice_SetFogParameters @ 0x677960` |
| `mesh_simp` | mesh simplification (edge-collapse), targeting **byte-identical** output vs the canonical path | in-code "byte-identical output" + divergence-detection scaffolding — the parity is actively pinned, not yet witnessed clean end-to-end |
| `packing` | word→byte packing | **retail** `pack_words_to_bytes @ 0x403CD0` (low byte of each u16, 3 bytes/group) |
| `depthmap` | depth/height map storage | in-code |

The tile overlay and foliage that render over the terrain surface have their own
now-landed records: [tiles/til-re.md](../tiles/til-re.md) (PAR-R3),
[foliage/foliage-re.md](../foliage/foliage-re.md) (PAR-R2). The `terrain_rgb`
tint stack the surface modulates through is [env/env-tod-re.md](../env/env-tod-re.md) #19.

## D-TERRAIN divergence catalog

| ID | Class | Disposition | One-liner |
|---|---|---|---|
| D-TERRAIN-1 | C | PERMANENT (candidate) | **Terrain-shader edit/runtime split** (the one deliberate divergence): the editor renders terrain with a live-sculpt shader (height edits without rebake), the runtime with the baked shader — the *surface-shading math is shared via an include* so the two cannot drift in look. Tracked, justified by an editing need the runtime path cannot serve, and sharing the fidelity-bearing core ([oned/editor-runtime-parity.md](../oned/editor-runtime-parity.md) §Terrain shaders). Ratify under ADR 0022 to move from candidate to `PERMANENT`. |

No other terrain divergence is confirmed yet — the data path is the byte-identical
TrnGen port. The pending grill (below) may surface facets in mesh_simp / CDEP.

## Pending (the deep grill, to complete R1)

- **Mesh simplification byte-parity** — the `mesh_simp` divergence-detection
  scaffolding implies the edge-collapse ordering is being pinned to the canonical
  output; witness the collapse cost/order against the source and record any
  residual as `D-TERRAIN-n`.
- **CDEP / LOD bitstream** — `cdep_constraint` + the quadtree mip chain: witness
  the on-disk CDEP block encoding and the LOD selection thresholds.
- **Binary split** — the LOD/quadtree/mip witnesses are **jodemo**; confirm the
  retail equivalents (retail renders terrain too) so the record is retail-anchored
  where it ships, like the foliage (R2) re-confirmation.

## Cross-references

- Reimpl: `libs/terrain`, `libs/terrain_query` (ADR 0020, the world→height seam).
- The reference data path: the TrnGen byte-identical terrain generator (canonical).
- Surface consumers with their own records: tiles (R3), foliage (R2), env tint (#19).
