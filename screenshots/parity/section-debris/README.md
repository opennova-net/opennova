# Collision-triangle section-debris parity

Deterministic OpenNova presentation capture for D-ITEM-16.

![Material-aware collision-triangle centroid samples and launch vectors](capture.png)

The stage visualizes a representative 18-row subset through the production
`DestructionPresentPass` and a mounted retail particle catalog. Colored points
are diagnostic centroid overlays: green is material 17
`Effect_TreeFoliageExp`; amber is `Effect_TreeWoodExp`; vectors point away from
the stored blast center. The wall is a deterministic probe fixture, not a
retail mission capture.

Exact generation is pinned below the visual seam by native
`test_section_debris_samples_collision_faces`: its 150-face CFAC fixture
asserts the retail 8.8 stride, per-section accumulator reset, signed centroid,
first callback matrix, direction, and material selection. GUT
`test_resolved_debris_and_glass_effects_present_verbatim` proves those resolved
rows are not randomized or repositioned by Godot.

## Reproduce

```powershell
python scripts/mcp/game_mcp.py launch --windowed --resource-dir $env:OPENNOVA_JO_DIR
python scripts/mcp/game_mcp.py probe run retail_parity_visual '{"mode":"debris","output_dir":"screenshots/parity/section-debris"}' --wait
python scripts/mcp/game_mcp.py stop
```
