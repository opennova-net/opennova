# Shader validation contract

The checked-in shader set under `godot/shaders/` is a closed inventory of
`.gdshader` entry points and `.gdshaderinc` implementation files: a shader
lands or leaves with the change that owns its consumer. The rendering parity
pass (2026-09-24) deleted the celestial, light-corona, precipitation and
CanvasItem NVG post shaders and added the far-side particle wrappers and
their include, the thermal particle wrappers, the three tracer ribbon
wrappers, `sky_clouds.gdshader`, and the `effect_fog`, `foliage_mask` and
`sky` includes. `godot/tests/shader_resource_validation_test.gd` loads every
resource through Godot, which catches the parser, import and include failures
a textual walk cannot. Nothing else validates the set and no machine-readable
twin of this page exists: the family citations are the table under "Live
citation audit" below. The textual pins over the shader and C++ sources (the
former `shader_resource_contract_test.gd`, `shader_provenance_pins_test.gd`,
`foliage_shader_contract_test.gd`, `terrain_shader_contract_test.gd` and the
transitive-source hash golden) were retired on 2026-09-21: a test asserts
behavior through a public seam, never the text of the code.

The 132 object shaders (the 128 object wrappers under `object/` and four
declared auxiliary EnvironmentMirrorTextured P3 postmultiply passes) are
hand-maintained: each wrapper states its own selection defines (pass, fog,
blend, clip and coverage axes) ahead of its shared includes and inherits its
citations through its include closure. `object/pipeline_manifest.json`
describes the reachable techniques and their static render policies for the
`render_swatch` probes and the shader-cache tests; it neither generates nor
selects a wrapper, and it has no field for the newer per-wrapper axes (the
vertex-shader fog, the fixed-function vertex diffuse, the blended pass and the
two CLIP variants), so it is not a complete description of one.

The validation target is deliberately singular: the retail fixture catalog's
`retail_reference_highest_retail_selectable_v2` profile, including
`shader_usage_level = 2`. OpenNova's authored retail video controls are kept
visible but locked to that profile by `retail_video_quality_policy.gd`.
Lower shader-usage/fallback surface techniques are not a port target. The
2026-08-23 audit, run against the then-vendored `third_party/modsuperoed`
corpus (removed by ADR 0038), decoded all 44 SCR `.fx` files and inventoried all 59
technique declarations (57 typed across all six retail classes plus two
untyped fixed-function fallbacks) and all 138 pass declarations in
`object/retail_effect_inventory.json`; the tests pin that inventory's counts and
dispositions, the decode is not re-run. For `TECHNIQUE_NORMAL`, 19 declarations
are selected by the highest-quality profile, ten are lower-quality fallbacks,
and `leaves.FX` is explicitly excluded because its tag is absent from the
shipped OED/runtime registry. Those 19 declarations plus the runtime-only
`VS_TRACER` witness project exactly onto the 24 checked-in runtime techniques.

`object/technique_validation.json` then audits those 24 NORMAL techniques
individually: RGB/alpha/coverage channel ownership (including specular or
PhongMap data in Diffuse1 alpha), mapped/geometric normal space, directional,
hemisphere, ambient, and point-light response, state/fog policy, and decoded
retail symbols. `object/auxiliary_technique_validation.json` closes the loop
over all six typed pass classes: CLIP, PROJSHAD, GLOW, and MATCHTERRAIN have
explicit per-technique contracts and D3D12 raster probes; DEPTHMASK's two
declarations are source-audited but intentionally have no runtime shader
because their only spot-projector producer is caller-less in the shipped
executable. The two untyped fixed-function fallbacks remain excluded by the
locked shader-usage level. Tests fail if any inventory count, declaration,
pass, citation, selected technique, wrapper topology, pass-class disposition,
or response contract drifts (the decoded-source symbol checks retired with the
corpus).

## What “parity” means here

A shader family either matches or matches with named, bounded residuals; an
open divergence is never converted into a pass.
No ledger exception is retained for the shader families: D-TERRAIN-7 and
D-FOLIAGE-7 closed 2026-09-26 (D-TERRAIN-9 retired with the ONED terrain preview
on 2026-08-24; D-FOLIAGE-9/-10 and D-RORD-7 closed 2026-09-24); the water
surface's mirror keeps two measurements due (env-tod-re.md, "Open after the
2026-09-24 pass").
Their scopes and current evidence remain authoritative in the linked RE records
and divergence ledger.

Lighting validation is layered:

1. `renderer_light_runtime` pins the recovered CPU equations and edge cases.
2. GUT device-seam tests pin environment uniform delivery, per-model point
   light isolation, and the terrain projected-light row path.
3. `render_swatch` mode `lighting` (`game_probe`) rasterizes the actual object
   shader matrix under opposing directional/hemisphere states and point-light
   states. It checks each direction/hemisphere/point response independently,
   so both a missing authored response and an invented one fail. In pixel-
   shader effects the mapped-normal N.L/N.H remains fragment-rate while retail
   attenuation and geometric self-shadow are vertex-interpolated. BDiffT2's
   fixed-function point stage and the skinned DOT3 effects instead use the
   separately pinned attenuation-only varying required by their source. The
   PhongMap probe obtains the generated 256x256 LUT through the production
   `ObjectShaderCache.configure_material_for_key` binding path rather than a
   probe-only substitute. Its final state switches every cell to an identical
   production `MultiMesh` draw, fetches that draw's RGBAF light row through
   `INSTANCE_CUSTOM.x`, and requires byte-identical RGBA8 output against the
   live per-instance-uniform route. Native and GUT layers separately pin atlas
   selection, immutable row identity, owner/interior isolation, and clearing.
4. `render_swatch` mode `channels` rasterizes the same 24
   techniques through eight paired states. It proves that `RgbGen` affects
   only `_FFP` self-lit RGB, `AlphaGenValue` affects only `_FFP` fixed alpha,
   each technique cuts out from its declared diffuse/normal/vertex/reflect/
   zero coverage channel, and only the five cited Phong techniques react to
   raw `Diffuse1.a` as specular data. These probe frames are diagnostic
   swatches, never catalog screenshots.
5. Four pass-class modes complete the auxiliary matrix: `clip` proves the
   above-water reflection plane plus NORMAL fallback/skinned skip behavior;
   `projshadow` proves the render-slot RenderingDevice pass
   (`SlotCaptureAdapter`): retail-black output over the white slot clear,
   the Diffuse1/Diffuse2/AlphaGen coverage contracts at the alpha-test
   discard boundary and through the alpha-blend variant's gray, the
   additive-LUM and glass no-pass surfaces, and the beauty frame keeping
   NORMAL shading where the capture is black (the synthetic 3DI fixtures
   stand in for the technique matrix, which ctest
   `renderer_material_classify` pins per technique); `matchterrain` proves stance, page residency,
   coverage, and the exact `2 * tile.rgb * (HemiSky + tile.a * DirLight)`
   combine; `glow` proves LUM NORMAL-copy, Glass's rotated two-lobe sun glint,
   and every no-pass contract. The GLOW mode also asserts the typed focused-Q3
   compiler/RenderingDevice target, resolved beauty depth, exact capture, four weighted downsample taps, four
   cardinal blur draws, 45-degree final average, and `SRCALPHA/ONE` composite.
   All run against Forward+ over D3D12.
6. `render_swatch` mode `calibrate` proves the production terminal
   transfer and live framebuffer blend domain: all 256 gamma bytes round-trip
   exactly, SRCALPHA/INVSRCALPHA resolves to byte 128 for the pinned operands,
   and the selected ONE/ONE additive convention resolves to byte 96.
7. The registered retail fixture catalog remains the scene-level authority;
   descriptive image metrics are not pass thresholds.

## Live citation audit (2026-08-22)

Each shader family's retail anchors were resolved in the active `Jointops.exe`
IDA database (image base `0x400000`). These are containing-function checks;
the detailed semantic derivations stay in the cited RE documents.

| Family | Address | IDA containing function |
| --- | ---: | --- |
| Object effects | `0x5af3fe` | `HLSLEffect_LoadFromFile` (`0x5ae690`) |
| Entity light delivery | `0x5d98a0` | `Render_SetupEntityLightingAndShaderConstants` |
| Hemisphere D3D lights | `0x5d8cb0` | `Lighting_SetHemisphereD3DLights` |
| Terrain shader programs | `0x605260` | `Terrain_CompilePixelShaders` |
| Terrain light constants | `0x604420` | `Terrain_SetupLightingAndShader` |
| Terrain temporary-blue composite | `0x60e0c6` | `PolyTrn_RenderTile` (`0x60da70`) |
| Terrain projected lights | `0x5aa830` | `Light_SetupTerrainProjectedPass` |
| Foliage instances/lightmap/assets | `0x5ffdd0`, `0x5ff7a0`, `0x6015b7` | `Foliage_GenerateInstances_0`, `Foliage_CreateLightmapBlendPS`, `Foliage_LoadDefAssets` (`0x601260`) |
| Max-quality foliage c7/c8 projection | `0x60a220`, `0x6006f0` | `Foliage_RenderDetailPatches` (`0x609de0`), `Foliage_SetupVertexShaderConstants` (`0x600450`) |
| Water programs | `0x5c19b0` | `Water_InitSurfaceShaders` |
| Sky/celestial | `0x579080`, `0x5acaa0` | `Render_Skybox`, `Render_CelestialBodies` |
| Light coronas | `0x5aaf40` | `EffectWorld_RenderLightCoronas` |
| Scars | `0x5cc315` | `Scar_LoadTextures` (`0x5cc2e0`) |
| Ground-shadow drapes | `0x5d6e20` | `RenderSlot_DrawAllDrapes` |
| Sun veil | `0x5ad8b0` | `Environment_ApplySunVeilAndExposureStopdown` |
| Particle blend programs | `0x5e29f0`, `0x5e8380` | `CParticleDefEntry_ParseBlendMode`, `CParticleTexture_InitTextureAndChannels` (`0x5e8210`) |
| Gamma/display path | `0x679c1b`, `0x677be0` | `CD3DDevice_InitializeDisplay` (`0x679890`), `GLib_SetGammaRamp` |
| NVG lighting constants | `0x5c8090` | `CTerrainRenderer_BuildLightingShaderConstants` |

The audit is dated. Since the 2026-09-24 rendering parity pass the "Light
coronas" row names no resource (the corona billboards draw through the
post-particle overlay stage, `renderer::append_corona_overlay`; the
precipitation streak and light-corona shaders were deleted), the celestial
bodies draw through their authored object materials rather than
`celestial*.gdshader`, and the NVG post is FrameFx's render-to-texture chain
rather than `nvg_view.gdshader` ([render-order-re.md](render-order-re.md)
§2026-09-24 rendering parity pass). The ONED terrain preview's
`terrain_editor.gdshader` left with that preview, and the particle family
includes the far-side wrappers and `particle_far_pass.gdshaderinc`.

Re-run the GUT contract tests, focused native renderer tests, full GUT suite, and
windowed probe whenever a shader, shader owner, lighting producer, renderer,
or Godot version changes.
