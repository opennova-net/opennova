#pragma once

#include <runtime/renderer/light_scene.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennova::renderer {

// THE TERRAIN LEG OF THE DYNAMIC LIGHT POOL — the projected-texture pass that
// puts a light's pool of illumination on the GROUND, as distinct from the
// object shader's per-vertex term that `light_scene` already carries.
//
// Retail re-draws each terrain batch once more PER LIGHT, additively, through
// a two-stage projected-texture technique
// [orig: Terrain_RenderSectorBatch @0x6092A0 — the batch collects the pool
//  by its own AABB (sector origin ints + the batch record's float corners
//  +0x18..+0x2C through Math_FloatToFixedPoint3_YNegated @0x609641/@0x609653)
//  with Light_CollectNearbyZonesByAABB(min, max, &count, 16) @0x60966f — a cap
//  of SIXTEEN, not the object pass's 64 — then CLEARS both light groups
//  (Lighting_SetInteriorLightGroup(0,0) @0x60967c,
//  Lighting_SetOwnerLightGroup(0,0) @0x609685), so only UNOWNED lights ever
//  reach the terrain; a counting pass @0x609690..0x6096c9 keeps the light
//  leg only when at least one handle passes Light_PassesActiveGroups
//  @0x60969f AND LightInstance_IsAliveAndLightsTerrain @0x6096b3 (a live
//  handle whose flags lack 0x400); the per-light loop @0x60984c..0x609953
//  then re-draws the batch once per passing handle with NO four-light cap —
//  the adapter caps word g_TerrainAdapterCapsStorage+0x34 (ex dword_319FBD4)
//  bit 0x100 @0x609890 selects Light_SetupTerrainProjectedPassPS @0x6098a5
//  (the ps.1.1 pass at 0.4/r) or the fixed-function
//  Light_SetupTerrainProjectedPass @0x6098b4 (ex `render_foliage_instance`:
//  its argument is a g_LightInstanceTable slot index @0x5AA857, not a foliage
//  instance); both take (handle, first-light flag) @0x60989a..0x6098b4. The
//  whole leg is skipped when g_PolyTrnUsePixelShaderPath == 0 @0x6095e4 or
//  dword_319FB84 != 0 @0x60983f.]
//
// The engine half: LightScene::collect_terrain_pass_rows runs that collect +
// gate per patch and publishes one row per passing light — position,
// projection scale and the pixel constant — for the terrain shader. The
// device half (the projected samples, the pass combine and the additive
// re-draw) is the presenter's.
//
// THE SERVED PATH is the ps.1.1 pass: the reference adapter carries caps bit
// 0x100 and at least four texture stages, the same test that creates the pass
// [orig: Lighting_InitTextures @0x5a99c4 (device_flags & 0x100, caps[5] >= 4)
// -> D3DXAssembleShader @0x5a9a21 over the source @0x7d9fa0, the first-light
// pass g_LightTerrainPassPSFirst @0x5a9a98 and the ONE/ONE pass g_LightTerrainPassPSAdd @0x5a9b04].
// The program is
//   tex t0..t3; mov r0, t0; dp3_sat r0, r0_bx2, t1_bx2; mul r0, r0, t2;
//   mul r0, r0, t3; mul r0, r0, c0
// over these stages (texture slot index and stage mode per stage: the pass is
// created with slots (16, 8, 23, 23) @0x5a9a5f..0x5a9a6f, every stage mode 1
// (TEXCOORDINDEX = stage, no transform) by CGfxResource_ResetStageModes @0x6796cf, then stages
// 0/2/3 re-moded @0x5a9aad/@0x5a9abd/@0x5a9ace; the modes decoded by
// CGfxShader_SetTextureStageState @0x680950):
//   t0 slot 16 = g_RenderCubeNormalizeTexture (@0x5aaeb7), mode 13 = the
//      camera-space position, COUNT3, through transform 16 (@0x5aacac): the
//      light-relative vector (light - point) * scale in D3D (z, x, y) order,
//      the cube-normalize lookup;
//   t1 slot 8 = the terrain batch's generated detail coefficient map
//      (PolyTrn_BindStageTextures @0x6043ad = dword_319F994 =
//      Texture_GenerateNormalMap(polytrn_detailmap, B channel, 1/32)
//      @0x60b164 — terrain::build_detail_coefficient_map), mode 1 = the
//      vertex's second UV set untransformed, the detail coordinate the
//      ps.1.4 surface samples its t1 with;
//   t2 slot 23 = g_LightTexLight2D (@0x5aaeb1), mode 12 (COUNT2) through
//      transform 17 (@0x5aad51): the ground-plane disc;
//   t3 slot 23 = g_LightTexLight2D again, mode 12 through transform 18
//      (@0x5aad99): the height coordinate (u, 0.5). Slot 22 holds
//      g_LightTexSpot1D (@0x5aaeab), but no terrain light pass reads it — the
//      fixed-function pair is created with slots (23, 23) too
//      [orig: sub_679250(23, 23, 0x600) @0x5a98e0].
// c0 = the colour below, a = 1 [orig: SetPixelShaderConstantF(0) @0x5aae8c].

// The flag that keeps a light off the terrain pass (light_scene.h's "terrain
// disabled (flag 1024)") [orig: LightInstance_IsAliveAndLightsTerrain @0x609880].
inline constexpr uint32_t kLightFlagNoTerrain = 0x400u;

// The per-batch collect cap [orig: the `push 10h` @0x609658 into
// Light_CollectNearbyZonesByAABB @0x60966f].
inline constexpr size_t kTerrainLightQueryLimit = 16;

// THE PS PASS's COLOUR (the served path): c0.rgb = record rgb (+44/+48/+52)
// * blend (+56) * g_EffectWorldAmbientScale{R,G,B} * flt_2732DA{C,8,4}, then
// * RgbGen_EvaluateColor when the record carries a gen block, c0.a = 1
// [orig: Light_SetupTerrainProjectedPassPS @0x5aad93..0x5aadd3, the gen
// multiply @0x5aade5..0x5aae4c, SetPixelShaderConstantF(0) @0x5aae8c]. It
// carries NEITHER the fixed-function pass's 0.66 NOR its 0.5 (below): the
// ps.1.1 program multiplies c0 in once, with no MODULATE2X to compensate.
inline float terrain_light_ps_constant(float channel, float blend,
		float ambient_scale, float per_channel_factor) {
	return channel * blend * ambient_scale * per_channel_factor;
}

// A ps_1_x constant register holds [-1, 1]: the device clamps c0 as the
// program reads it (the D3D9 ps_1_1 register range). The colour above can
// exceed one (a bright record under an ambient scale or recip factor above
// unity); the pass sees the clamped value.
inline constexpr float kPs11ConstantMax = 1.0f;

inline float terrain_light_ps_constant_register(float value) {
	return value < -kPs11ConstantMax ? -kPs11ConstantMax
			: (value > kPs11ConstantMax ? kPs11ConstantMax : value);
}

// THE FIXED-FUNCTION PASS (caps bit 0x100 clear; not served — the reference
// adapter takes the ps.1.1 pass above). Its pair dword_2732DC8 / dword_2732DC4
// (modes 0x600 / 0x602) is created with FFP lighting on and the material
// sources on MATERIAL [orig: GfxShader_SetFfpLightingSources(this, 1, 0, 0, 0)
// @0x5a98eb..0x5a98f4]; the ps.1.1 pair clears that byte instead
// [orig: CEffect_SetTextureParam(pass, nullptr) @0x5a9a9d].
// The terrain pass's own literal [orig: flt_7D3E68 = 0.66000003 @0x5AA9C8 /
// @0x5AA9E9 / @0x5AAA01] and the 0.5 that accompanies it on the Ambient
// constants [orig: dbl_7C3618 (0.5) @0x5AAA9B..0x5AAAB3].
inline constexpr float kTerrainLightFactor = 0.66000003f;
inline constexpr float kTerrainAmbientHalf = 0.5f;

// WHY THE 0.5 EXISTS, which is the part that makes this look wrong if it is
// copied without its reason: the pass's blend doubles stage 0 (MODULATE2X —
// the stage state is set by the shader the pass applies, GfxShader_ApplyPassChecked
// @0x5AAAFE/@0x5AAB16, not by this function). The witnessed Ambient term
// carries 0.66 * 0.5 precisely BECAUSE that stage-0 2x brings it back:
// 0.66 * 0.5 * 2 == 0.66. Stage 1's 2x is left uncompensated, and the shader
// side is what applies it.
//
// So the value PUBLISHED to the shader is rgb * blend * 0.66, and the
// on-screen contribution is 2 * g * that. Dropping either the 0.5 or the
// stage-0 2x alone changes the result by a factor of two.
inline float terrain_light_published(float channel, float blend) {
	return channel * blend * kTerrainLightFactor;
}

// The Ambient term as the original composes it
// [orig: Light_SetupTerrainProjectedPass @0x5AA9C8..0x5AAA01 — record float
//  +44/+48/+52 (rgb) * float +56 (blend) * g_EffectWorldAmbientScale{R,G,B}
//  @0x840b24.. * flt_2732DA{C,8,4} * 0.66, then * RgbGen_EvaluateColor when
//  the record carries a gen block @0x5AAA05..0x5AAA5F, then * 0.5 into
//  constants 4..6 @0x5AAA9B..0x5AAAB3].
inline float terrain_light_ambient(float channel, float blend,
		float ambient_scale, float per_channel_factor) {
	return channel * blend * ambient_scale * per_channel_factor *
			kTerrainLightFactor * kTerrainAmbientHalf;
}

// THE PER-CHANNEL FACTOR is the environment's packed terrain colour
// reciprocal `g_EnvTerrainColorRecip` unpacked to floats ONCE per tick: red is
// byte 2 into flt_2732DAC, green byte 1 into flt_2732DA8, blue byte 0 into
// flt_2732DA4, each times flt_7C3DD4 = 1/128
// [orig: EffectWorld_TickInstancesAndLightScale @0x5AA21D..0x5AA23F]. It is
// not the ambient scale (that is the modulator's 1/64 unpack,
// EffectWorld_UnpackModulatorToAmbientScale @0x5AAF1D), and the pass reads the
// three floats rather than re-unpacking. The environment default 0x808080
// [orig: Environment_InitDefaults @0x57C050..0x57C065 — the same immediate
// seeds g_EnvCloudColorTarget and g_EnvLightningColor] divides to exactly
// (1, 1, 1); if it did not, every terrain light would be tinted by default.
// It is a factor, not a clamp: a white recip byte exceeds unity. The recip
// itself is env::terrain_color_recip_packed (TimeOfDay_ParseProperty
// @0x57CA60..0x57CAE3) — EnvironmentState::terrain_color_recip_packed serves
// the loaded value.
inline constexpr uint32_t kTerrainFactorDefaultPacked = 0x808080u;
inline constexpr float kTerrainFactorDivisor = 128.0f;

inline std::array<float, 3> terrain_per_channel_factor(uint32_t packed_rgb) {
	return {
		static_cast<float>((packed_rgb >> 16) & 0xFFu) / kTerrainFactorDivisor,
		static_cast<float>((packed_rgb >> 8) & 0xFFu) / kTerrainFactorDivisor,
		static_cast<float>(packed_rgb & 0xFFu) / kTerrainFactorDivisor,
	};
}

// THE PROJECTED TEXTURE's SCALE. The fixed-function pass projects with
// `32768.0 / record[+0x28]` [orig: Light_SetupTerrainProjectedPass
//  @0x5AA864..0x5AA873, flt_7C32B4 = 32768], and +0x28 is the record's Q16
// range — the same dword Light_GetPointLightParams divides into pos.w =
// 65536 / range @0x5A91C8..0x5A91D4 — so the scale is 0.5 / radius: the
// texture's 0..1 span covers exactly one diameter, centred by the +0.5
// translation @0x5AA900/@0x5AA915. The ps.1.1 pass (caps bit 0x100) scales
// by 26214.4 / range instead, i.e. 0.4 / radius, so its falloff textures
// reach 1.25 radii [orig: Light_SetupTerrainProjectedPassPS @0x5aab76]. A
// bigger light casts a WIDER pool, not a brighter one.
inline constexpr float kTerrainProjectScale = 0.5f;      // 32768 / 65536
inline constexpr float kTerrainProjectScalePs = 0.4f;    // 26214.4 / 65536

inline float terrain_project_scale(float radius, bool ps_pass = false) {
	if (radius <= 0.0f) return 0.0f;
	return (ps_pass ? kTerrainProjectScalePs : kTerrainProjectScale) / radius;
}

// ---------------------------------------------------------------------------
// THE PROJECTION, in mission space.
//
// Retail builds two 4x4 texture matrices per light
// [orig: Light_SetupTerrainProjectedPass @0x5AA885..0x5AA996]: the "xz"
// matrix takes columns 0 and 2 of the camera matrix flt_27219C0 (its rows
// [0..2] at +0x00/+0x10/+0x20 and +0x08/+0x18/+0x28, scaled by inv) with the
// translation row 3 (flt_27219F0 - light.x, flt_27219F8 - light.z) * inv +
// 0.5, and the "y" matrix takes column 1 (+0x04/+0x14/+0x24) with
// (flt_27219F4 - light.y) * inv + 0.5 and a constant v of 0.5. flt_27219C0
// is the per-frame camera->world matrix (written by
// Render_SetViewAndProjectionMatrices @0x58D942, read by every world-space
// transform helper), so a texture matrix built from its columns plus its
// translation row re-projects a CAMERA-SPACE vertex position back to WORLD
// space — the texgen source the pass samplers select — and the camera terms
// cancel: each stage reduces to the light-relative world offset times inv,
// plus 0.5.
//
// The D3D world frame the matrices live in is the Y-negated fold of mission
// space [orig: Math_FixedPointToFloat3_YNegated @0x611210 — d3d.x = -m.y,
//  d3d.y = m.z, d3d.z = m.x], so in MISSION terms:
//   the disc (the 64x64 g_LightTexLight2D on the ground plane):
//     u = (light.y - point.y) * inv + 0.5,  v = (point.x - light.x) * inv + 0.5
//   the height coordinate (above/below the light, sampled from the same
//   g_LightTexLight2D: both passes bind slot 23 on this stage too):
//     u = (point.z - light.z) * inv + 0.5,  v = 0.5
// A point at the light samples (0.5, 0.5) — the disc's centre. The ps.1.1
// pass builds the same two matrices at its 0.4 / r scale for its stages 2
// and 3 [orig: Light_SetupTerrainProjectedPassPS @0x5aacc0..0x5aad7d].
struct TerrainLightUv {
	float u = 0.0f;
	float v = 0.0f;
};

TerrainLightUv terrain_light_uv_disc(const std::array<float, 3> &point_mission,
		const std::array<float, 3> &light_mission, float inv_scale);
TerrainLightUv terrain_light_uv_height(const std::array<float, 3> &point_mission,
		const std::array<float, 3> &light_mission, float inv_scale);

// The ps.1.1 pass's stage-0 lookup: its third matrix is the NEGATED camera
// matrix columns and translation, so stage 0 samples the cube-normalize map
// along (light - point) * inv in D3D (z, x, y) order
// [orig: Light_SetupTerrainProjectedPassPS @0x5aab99..0x5aaca5]. In MISSION
// terms that is (light - point) . (x, -y, z) — the cube face basis
// cube_normalize_texel_argb fills. The scale never matters to a cube lookup.
std::array<float, 3> terrain_light_cube_vector(const std::array<float, 3> &point_mission,
		const std::array<float, 3> &light_mission, float inv_scale);

// THE CUBE-NORMALIZE MAP the ps.1.1 pass's stage 0 samples
// [orig: GTexture_GenerateNormalMapCubeMap @0x685570 (ex
//  generate_normalmap_cubemap), filled once by Render_FillStaticCubemaps
//  @0x58f2b2]. Per face the generator loads a face axis C and a row-0 axis U
//  (+X: C = (1,0,0), U = (0,1,0); -X: (-1,0,0), (0,1,0); +Y: (0,1,0),
//  (0,0,-1); -Y: (0,-1,0), (0,0,1); +Z: (0,0,1), (0,1,0); -Z: (0,0,-1),
//  (0,1,0) @0x685644..0x6858b2), takes X = C x U @0x6858ba..0x68590b, and per
//  texel (col, row) with half = size / 2:
//    rt = (half - row) / half, ct = (half - col) / half   @0x6859e8..0x685a5a
//    n  = normalize(C + U * rt + X * ct)                  @0x685a66..0x685aaf
//    R, G, B = trunc(n.xyz * 127.5 + 128.0)               @0x685ab4..0x685b48
// (dbl_7DC6F8 = 127.5, dbl_7E3FA8 = 128.0, the x87 truncating control word
// 0xC00). Those are the D3D cube face orientations, so the faces load as-is
// in the +X, -X, +Y, -Y, +Z, -Z layer order. The alpha byte is left from the
// stack and never read (dp3 takes rgb only); it is written opaque here.
// The side follows the object texture detail through dword_8409E4
// [orig: Game_StartMission @0x52466d..0x524719]: 64 / 128 / 256
// [orig: Render_InitTextures @0x58f794..0x58f814]; the served top setting is
// 256.
inline constexpr int kCubeNormalizeFaces = 6;
inline constexpr int kCubeNormalizeSize = 256;

uint32_t cube_normalize_texel_argb(int face, int col, int row, int size);

// ---------------------------------------------------------------------------
// THE PROCEDURAL TEXTURES
// [orig: Lighting_InitTextures @0x5A94F0 — "texlight2d" 64x64 from
//  Texture_GenerateProceduralFalloffTexture(buf, 64, mode 2, rgb) @0x5A9553
//  (create 64x64, flags 0x40001 = clamp, bilinear, no mips
//  @0x5A9558..0x5A956E) — the texture BOTH falloff stages of the terrain
//  light passes sample (slot 23);
//  "texlightspot2d" 64x64 from mode 1 @0x5A9581 (flags 0x140001) — unused by
//  the terrain pass (the spot legs are dead code in JO, light_scene.h);
//  "texlightspot1d" 64x8 built inline @0x5A95A6..0x5A9612 (create 64 x 8
//  @0x5A95FC/@0x5A95FE) — bound to slot 22, which no terrain light pass
//  reads].
//
// Texture_GenerateProceduralFalloffTexture @0x5A92C0: for a 64-wide buffer
// the border texels (column or row 0 / 63) are 0x00000000; each interior
// texel maps its column and row to x, y in (-1, 1) as
//   x = ((col - 1) * 2 + 1) / 62 - 1       [orig: flt_7C3B90 = 2.0, inner
//   y = ((row - 1) * 2 + 1) / 62 - 1        size width - 2 @0x5A92CC]
// mode 2 (the disc): i = trunc(255 * exp(-4 x^2) * exp(-4 y^2))
//   [orig: flt_7C44B8 = 4.0, dbl_7D9F98 = 255, the two x87 2^(k log2 e)
//    expansions @0x5A934B..0x5A93A6, ftol @0x5A93A6, < 0 -> black
//    0xFF000000 @0x5A93B7, > 255 -> 255 @0x5A93C7, texel = 0x010101 * i |
//    0xFF000000 @0x5A93D2]
// mode 1 (the spot disc): i = trunc(255 * (1 - (x^2 + y^2))), < 0 -> 0,
//   texel = 0x01010101 * i (the ALPHA byte is i too — no 0xFF000000)
//   [orig: @0x5A944F..0x5A94A0]
// The 1D strip: row r in 0..7, column c in 0..63; c == 0 or 63 -> 0xFFFFFFFF,
//   else texel = 0x010101 * ((c * (r + 1)) >> 1) | 0xFF000000
//   [orig: @0x5A95B1..0x5A95E4 — the `imul esi, eax; sar esi, 1; imul esi,
//    10101h; or esi, 0FF000000h` run].
inline constexpr int kFalloffTextureSize = 64;
inline constexpr int kFalloffSpot1DRows = 8;

uint32_t falloff_texture_light2d_argb(int x, int y);
uint32_t falloff_texture_spot2d_argb(int x, int y);
uint32_t falloff_texture_spot1d_argb(int x, int row);

// ---------------------------------------------------------------------------
// THE COMPOSITE: how a batch that draws pool lights reaches the target. Such a
// batch never draws its ordinary lit pass on its own; per channel:
//   1. each drawn light writes its pass output unfogged, the first drawn light
//      opaquely (blending off) and every later one ONE/ONE, so the 8-bit target
//      holds the saturated sum `pool`;
//   2. one pass multiplies the target by the cached page colour: stage 0 is
//      MODULATE2X(page texel, the lit white diffuse) = saturate(2 * page),
//      blended DESTCOLOR/SRCCOLOR = 2 * stage * target, unfogged;
//   3. the ordinary lit terrain pass (the ps.1.4 surface, fog enabled) adds
//      ONE/ONE on top.
// So the pixel is saturate(saturate(2 * saturate(2 * page) * pool) + fogged
// lit). `page` is the page RGB (t0 before the detail splat) and `fogged_lit`
// the saturated ps.1.4 output after the fog blend.
// [orig: Terrain_RenderSectorBatch @0x6092A0 — the per-light loop
//  @0x60984C..0x609953 (the first-drawn flag esi cleared @0x609951 after a
//  light draws), the page multiply GfxShader_ApplyPassChecked(dword_319F934)
//  + its draw @0x609960..0x609A19 (the pass PolyTrn_InitTextures builds as
//  sub_6791A0(1, 0x1000628) @0x60C499..0x60C4AD), the lit pass
//  Terrain_SetupLightingAndShader(2) -> dword_319F930 (mode 0x1020002)
//  and its draw @0x609A49..0x609AB6; the first/later light passes
//  g_LightTerrainPassPSFirst / g_LightTerrainPassPSAdd (blending off / ONE-ONE,
//  Lighting_InitTextures @0x5A9A56..0x5A9B04), dword_2732DC8 / dword_2732DC4
//  (modes 0x600 / 0x602, @0x5A98D2..0x5A9933) on the fixed-function path]
inline float terrain_light_pool_composite(float page, float pool,
		float fogged_lit) {
	const auto saturate = [](float value) {
		return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
	};
	const float lit_by_pool = saturate(
			2.0f * saturate(2.0f * page) * saturate(pool));
	return saturate(lit_by_pool + saturate(fogged_lit));
}

// ---------------------------------------------------------------------------
// THE PER-PATCH ROWS.

// One terrain patch's collect volume, mission 16.16 — the same AABB the
// object pass hands LightDrawContext. The helper folds the terrain frame's
// float corners (the shell's render frame: x, y-up, z, with the patch mesh's
// sector-local AABB offset by its 512-unit sector origin — the
// traverse_quadtree world AABB) into mission fixed: mission.x = x,
// mission.y = -z, mission.z = y, per-axis min/max re-ordered after the fold.
struct TerrainLightPatchBounds {
	std::array<int32_t, 3> aabb_min_fixed{};
	std::array<int32_t, 3> aabb_max_fixed{};
};

TerrainLightPatchBounds terrain_patch_light_bounds(const float aabb_min[3],
		const float aabb_max[3], float sector_ox, float sector_oz);

// One passing light for one patch — what the shader needs to re-draw the
// patch once more: the light's mission-space position, the projection scale
// (0.4/r on the served ps.1.1 pass, 0.5/r on the fixed-function one) and the
// colour the pass uploads: the ps.1.1 pass's c0.rgb (terrain_light_ps_constant,
// clamped to the ps_1_x constant range) or the fixed-function pass's c4..c6
// (folded by 0.5 for its stage-0 MODULATE2X).
struct TerrainLightRow {
	std::array<float, 3> position{};   // mission-space float world units
	float inv_scale = 0.0f;
	std::array<float, 3> pixel_rgb{};  // c0 @0x5aae8c, or c4..c6 @0x5AAA9B..0x5AAAB3
	LightHandle handle{};
};

struct TerrainLightPatchRows {
	std::array<TerrainLightRow, kTerrainLightQueryLimit> rows{};
	size_t count = 0;
};

struct TerrainLightPassInputs {
	// g_EffectWorldAmbientScale{R,G,B} — the modulator unpack the presenter
	// already feeds the object pass (the env light-state gain).
	std::array<float, 3> ambient_scale{1.0f, 1.0f, 1.0f};
	// flt_2732DA{C,8,4} — terrain_per_channel_factor(the loaded recip).
	std::array<float, 3> terrain_factor{1.0f, 1.0f, 1.0f};
	LightFlickerInputs flicker{};
	// Adapter caps bit 0x100: the ps.1.1 pass (0.4/r, the unfolded c0)
	// instead of the fixed-function one [orig: @0x609890].
	bool ps_light_pass = false;
	// g_PolyTrnUsePixelShaderPath == 0 drops the light leg [orig: @0x6095e4].
	bool pixel_shader_path = true;
	// dword_319FB84 != 0 skips the per-light loop [orig: @0x60983f].
	bool light_pass_disabled = false;
};

// The per-patch collect + gate + constant build is
// renderer::LightScene::collect_terrain_pass_rows (light_scene.h), defined in
// light_terrain_pass.cpp so it reads the same pool slots the object pass does.

} // namespace opennova::renderer
