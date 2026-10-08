// Shared NovaLogic terrain lighting + fog math, included by both terrain.gdshader
// (runtime, baked-CPT mesh) and terrain_editor.gdshader (editor live-sculpt
// heightmap mesh). Both render the SAME surface; only their geometry source and
// overlays differ, so the surface-shading and fog math live here once.
//
// The surface chain is the witnessed TOP-TIER ps.1.4 splat shader
// [orig: g_PolyTrnPS14SplatNormalMap source text @ 0x7dece0, assembled by
// Terrain_CompilePixelShaders @ 0x605260]:
//   splat = c1*blend.r + c2*blend.g + c3*blend.b        (3-way detail splat)
//   dp3   = dot(detail2(t3), blendmap)  — only in the SplatNormalMap variant,
//           selected when polytrn_detailmap2 exists (PS14Splat otherwise)
//   light = (tile.a*c1 + c0) / 2                        (c1 = LIGHT, c0 = SKY)
//   out   = light *2 cm *2 dp3 *4 splat
// EXACTLY ONE normal term: the dual-normal product belonged to the separate
// ps.1.1 non-splat tier (g_PolyTrnPSDualNormalMap) and was wrongly STACKED on
// the splat here for years (the gobj-era "v23 dual-normal" chimera) — under
// the gamma-faithful pipeline (D-RMAT-7) that squared bump contrast and
// clipped whole regions to white. Corrected 2026-07-06 (D-TRN row in the
// ledger). Fog: [orig: Render_SetFogState @ 0x58a950 ->
// CD3DDevice_SetFogParameters @ 0x677960]. Keep byte-for-byte authoritative.

// --- Shared uniforms: terrain surface inputs + lighting + fog -----------------
// Textures sample RAW (no source_color): retail never enables sRGB sampling -
// texel bytes enter the ps.1.4 math as-is. The gamma-space convention
// (D-RMAT-7) lives in color.gdshaderinc; consumers encode the final
// color once at their ALBEDO write.
// The device's texfilter mode applies to every stage whose texture carries
// creation flag 0x8 (retail CGfxDevice_ApplyRenderStates per-stage loop
// @ 0x67e3b5..0x67e50e; CGfxShader_ApplyTextureStages @ 0x680856..0x680870),
// and the detail family is the one created with it (PolyTrn_InitTextures
// @ 0x60aba0): the detail inputs follow game.cfg's texfilter_level through
// opennova_texfilter_device and the viewport's anisotropy
// (texture_filter.gdshaderinc; engine renderer/texture_filter.h,
// TextureStage::TerrainDetail). The DBlendmap quadrants are box-mipped and
// created with flags 0x100001, no 0x8 (retail PolyTrn_InitTextures
// @ 0x60b515..0x60b51c), so they sample bilinear with point mips at any level
// (TextureStage::TerrainBlendMap).
#include "res://shaders/texture_filter.gdshaderinc"
uniform sampler2D u_detail_c1 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_detail_c2 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_detail_c3 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_blendmap : filter_linear_mipmap, repeat_disable;
// The ps.1.4 splat's stage-3 dp3 input: the authored SECOND detail pair at
// its own density, dp3'd with the normalized blend — a blend-weighted
// scalar modulation x2. It is NOT a normal map; the generated coefficient
// map belongs to the ps.1.1 tiers at stage 7.
// [orig: stage bind @ 0x6043ff (dword_319F998); texcoord transform
// density2/density @ 0x609810 (dword_31A1814 / dword_31A1810);
// g_PolyTrnPS14SplatNormalMap "dp3 r3, r3, r2; mul_x2" @ 0x605260]
uniform sampler2D u_detail2 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_detail2 = false;
uniform float u_detail2_density = 8.0;
// D-TERRAIN-8: camera-below-water swaps the LIVE stage-3 input from the
// authored second detail to the per-frame regenerated water noise color
// texture, and the stage-3 texcoord matrix becomes 8/density. Retail UV1 is
// source*detaildensity/512, so the result is source*8/512 — atlas_uv*16 for
// our normalized 1024 terrain atlas. The animation rides the texture
// CONTENT (the 128x128 pair regenerates every frame), not a UV scroll. The
// selector keeps PS14SplatNormalMap on full-splat+detail2 maps (the noise
// rides its dp3), and detail2-less splat maps faithfully render NO underwater
// modulation; the PSShadowBasic/NormalMap light-scale pair serves only the
// partially-authored stage sets our top-tier chain does not model
// (docs/terrain/terrain-re.md, underwater section).
// [orig: below-water flag @ 0x60FEE0 -> dword_319FB3C @ 0x60915F; live t3
//  slot swap @ 0x6043f2 (dword_3266E98 <- g_WaterNoiseColorTexture, authored
//  dword_319F998 otherwise); shader select @ 0x6044b1..0x604556; 8/density
//  texcoord @ 0x609786..0x6097D6]
uniform sampler2D u_water_noise : filter_linear, repeat_enable;
uniform bool u_below_water = false;
// Un-enved defaults = the retail noon register (full_00.env tod 1200:
// sun_rgb 170,170,167; sky_rgb 84,88,89 — bytes /255), one register with the
// object-model/global defaults.
uniform vec3 u_sun_light = vec3(0.666667, 0.666667, 0.654902);
uniform vec3 u_sky_ambient = vec3(0.329412, 0.345098, 0.349020);
uniform float u_detail_density = 128.0;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;
// The water mirror pass (the one non-shadow camera whose mask omits the water
// layer, render/visual_layers.h): the reflected scene fogs with the dry
// weather block (EnvironmentState::build_water_mirror_fog), keeps the
// authored stage 3 (the mirror context's below-water word is 0 — retail
// Render_MainScene @ 0x5c153d), and cuts the terrain at wh - 0.05: the GSysClip
// texgen u = y + 0.45 - (wh - 0.1) under AlphaRef 0x80 (retail
// Terrain_RenderSectorBatch @ 0x6092c6..0x60935b, plane wh - 0.1 from
// Render_MainScene @ 0x5c1561..0x5c1578, armed while wh != 0).
global uniform bool opennova_water_active;
global uniform float opennova_water_height;
global uniform vec3 opennova_water_mirror_fog_color;
// (start, end, type)
global uniform vec3 opennova_water_mirror_fog_range;
const float TERRAIN_WATER_MIRROR_CLIP_OFFSET = 0.05;

float terrain_water_mirror_pass_flag(uint camera_layers, bool in_shadow_pass) {
	return (!in_shadow_pass && (camera_layers & 1024u) == 0u) ? 1.0 : 0.0;
}

// --- The terrain leg of the EffectWorld light pool --------------------------
// Retail re-draws each terrain batch once more PER pool light (the first
// opaquely, later ones additively; terrain.gdshader composites the pool with
// the page and the fogged ordinary pass). The reference adapter (caps bit
// 0x100, four stages) draws it with the ps.1.1 program
//   mov r0, t0; dp3_sat r0, r0_bx2, t1_bx2; mul r0, r0, t2; mul r0, r0, t3;
//   mul r0, r0, c0
// over t0 = the cube-normalize map along the light vector, t1 = the batch's
// detail coefficient map at the detail coordinate, t2 = the 64x64
// "texlight2d" disc projected on the ground plane around the light, t3 = the
// same texture at the height above/below it, and c0 = the light colour
// (retail: the per-light arm of Terrain_RenderSectorBatch @0x6098a5 ->
//  Light_SetupTerrainProjectedPassPS @0x5aab30, the source @0x7d9fa0; the
//  collect, the 16-per-batch cap, the authored terrain-disable gate and c0
//  are portable in renderer::LightScene::collect_terrain_pass_rows, the
//  textures in renderer::falloff_texture_light2d_argb /
//  cube_normalize_texel_argb and terrain::build_detail_coefficient_map, the
//  projections in terrain_light_uv_disc / _height / terrain_light_cube_vector
//  — renderer/light_terrain_pass.h carries the stage map).
// Device fold: Godot instance uniforms take neither arrays nor samplers, so
// the <= 16 rows per patch ride one RGBAF texture row per pool slot (two
// texels per light: position.xyz + inv_scale, then c0 + the patch's count)
// and each patch instance carries its slot. The disc texture is created
// clamped, bilinear and without mips (flags 0x40001), the cube map as one
// bilinear level; the coefficient map wraps with the detail textures' device
// filter. An unbound coefficient map samples black like retail's empty stage.
uniform sampler2D u_terrain_light_rows : filter_nearest, repeat_disable, hint_default_black;
uniform sampler2D u_terrain_light_disc : filter_linear, repeat_disable, hint_default_black;
uniform samplerCube u_terrain_light_cube : filter_linear, hint_default_black;
uniform sampler2D u_terrain_light_normal : filter_linear_mipmap_anisotropic, repeat_enable, hint_default_black;
uniform bool u_terrain_light_enabled = false;
const int TERRAIN_LIGHT_ROWS_PER_PATCH = 16;

// The pool lights the patch re-draws with (0 = the batch draws only its
// ordinary pass).
int terrain_point_light_count(float slot) {
	if (!u_terrain_light_enabled) {
		return 0;
	}
	return int(texelFetch(u_terrain_light_rows, ivec2(1, int(slot + 0.5)), 0).a + 0.5);
}

vec4 sample_retail_detail_mips(sampler2D source, vec2 uv) {
	// The device mode (texfilter_level + 1) gives a 0x8 stage MIN LINEAR /
	// MIP POINT (mode 1), trilinear (mode 2), or MIN ANISOTROPIC with
	// MAXANISOTROPY 2 (mode 3) or the device's cap (mode 4)
	// [orig: Terrain_RenderSkyboxPass @ 0x610be8..0x610c1d; per-stage filter
	// select @ 0x67e37d..0x67e45b]. Bilinear on the nearest level is the
	// shader's (opennova_texfilter_device 0); the hardware sampler does the
	// rest, its anisotropy the viewport's. The explicit longest-gradient scale
	// is a conservative terminal guard: it keeps Godot's synthetic 2x2/1x1
	// tail unselectable, but can bias a grazing anisotropic footprint sharper
	// than the reference device's minor-axis/maximum-anisotropy selection.
	vec2 dimensions = vec2(textureSize(source, 0));
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float footprint = max(length(dx * dimensions), length(dy * dimensions));
	float terminal_lod = max(floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0, 0.0);
	if (opennova_texfilter_device == 0) {
		return textureLod(source, uv,
				retail_point_mip_level(log2(max(footprint, 1.0e-8)), terminal_lod));
	}
	float requested_lod = max(log2(max(footprint, 1.0)), 0.0);
	float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));
	return textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale);
}

vec3 terrain_point_light_pool(vec3 world_pos, vec2 detail_uv, float slot) {
	vec3 sum = vec3(0.0);
	if (!u_terrain_light_enabled) {
		return sum;
	}
	int row = int(slot + 0.5);
	int count = int(texelFetch(u_terrain_light_rows, ivec2(1, row), 0).a + 0.5);
	// t1_bx2: the coefficient texel is the batch's, shared by every light. The
	// coefficient map carries the detail family's flag 0x8
	// [orig: PolyTrn_InitTextures @ 0x60b14d -> Texture_GenerateNormalMap
	// @ 0x58c427], so it samples under the device mode like the details.
	vec3 normal = sample_retail_detail_mips(u_terrain_light_normal, detail_uv).rgb * 2.0 - 1.0;
	for (int k = 0; k < TERRAIN_LIGHT_ROWS_PER_PATCH; k++) {
		if (k >= count) {
			break;
		}
		vec4 posr = texelFetch(u_terrain_light_rows, ivec2(2 * k, row), 0);
		vec4 c0 = texelFetch(u_terrain_light_rows, ivec2(2 * k + 1, row), 0);
		vec3 d = world_pos - posr.xyz;
		// The mission-space contract (d3d.x = -mission.y, d3d.y = mission.z,
		// d3d.z = mission.x): disc u = (light.y - p.y) * inv + 0.5,
		// v = (p.x - light.x) * inv + 0.5; height u = (p.z - light.z) * inv +
		// 0.5, v = 0.5; the cube lookup (light - p) . (x, -y, z) * inv. In the
		// Godot frame (x, y-up, z = -mission.y) that is disc
		// (p.z - light.z, p.x - light.x), height (p.y - light.y) and the cube
		// lookup (light - p).xzy.
		vec2 disc_uv = vec2(d.z, d.x) * posr.w + 0.5;
		vec2 height_uv = vec2(d.y * posr.w + 0.5, 0.5);
		vec3 light_vec = texture(u_terrain_light_cube, -d.xzy * posr.w).rgb * 2.0 - 1.0;
		// dp3_sat r0, t0_bx2, t1_bx2, then x t2 x t3 x c0 (no doubling
		// anywhere in the program).
		float n_dot_l = clamp(dot(light_vec, normal), 0.0, 1.0);
		vec3 disc = texture(u_terrain_light_disc, disc_uv).rgb;
		vec3 height = texture(u_terrain_light_disc, height_uv).rgb;
		sum += n_dot_l * disc * height * c0.rgb;
	}
	return sum;
}

vec2 retail_detail_uv_from_atlas(vec2 atlas_uv, float density) {
	// Retail writes UV1 as source_position * (polytrn_detaildensity / 512).
	// Runtime and ONED instead carry normalized UVs for the full 1024 terrain
	// atlas, so multiply by two before applying either authored density.
	// [orig: configData+0x1738 @ 0x31BE270; density/512 global write
	// @ 0x6029a0..0x6029aa; UV1 stores @ 0x602db5..0x602dbe]
	return atlas_uv * (density * 2.0);
}

// The DBlendmap quadrant level the device samples: bilinear on the nearest
// level of the quadrant's own chain, which stops at 4x4 (flags 0x100001, no
// 0x8 [orig: PolyTrn_InitTextures @ 0x60b2c9; GTexture_CreateFromPixelData_0
// @ 0x6877bc..0x6877d8]). The atlas holds the four quadrants side by side, so
// its level of detail is a quadrant's and its last retail level is one above
// the atlas's own 4x4. The level comes from the unclamped coordinate, so the
// quadrant-edge clamp never pulls a border texel to level 0.
vec4 sample_retail_blendmap(vec2 uv, vec2 lod_uv) {
	vec2 dimensions = vec2(textureSize(u_blendmap, 0));
	float terminal_lod = max(floor(log2(max(min(dimensions.x, dimensions.y), 8.0))) - 3.0, 0.0);
	float lod = retail_isotropic_lod(lod_uv, dimensions);
	return textureLod(u_blendmap, uv, retail_point_mip_level(lod, terminal_lod));
}

vec2 clamp_retail_quadrant_uv(vec2 uv, vec2 quadrant, ivec2 dimensions) {
	vec2 texel = 1.0 / vec2(dimensions);
	vec2 minimum_uv = quadrant * 0.5 + texel * 0.5;
	vec2 maximum_uv = (quadrant + 1.0) * 0.5 - texel * 0.5;
	return clamp(uv, minimum_uv, maximum_uv);
}

// NovaLogic terrain surface shading — the witnessed ps.1.4 splat chain
// [orig: g_PolyTrnPS14SplatNormalMap source text @ 0x7dece0]. No fog/overlays — callers
// layer those on top. Colormap/detail UVs come from the caller's geometry
// (runtime: baked patch mesh; editor: sculpted heightmap mesh).
vec3 terrain_surface_color_from_colormap(vec4 cm, vec2 colormap_uv,
		vec2 detail_uv, vec2 blendmap_uv, vec2 quadrant, bool below_water) {
	// Stage 2 has its own transform: flat sectors collapse only this input,
	// while stages 1 and 3 keep their independent detail/noise coordinates.
	// [orig: Terrain_RenderSectorBatch @ 0x6092A0, transform 8 @ 0x60973A..0x6097A0;
	// PolyTrn_InitTextures @ 0x60AAA0, stage 2/3 binds @ 0x60C400..0x60C411]
	vec2 blend_uv = clamp_retail_quadrant_uv(
		blendmap_uv, quadrant, textureSize(u_blendmap, 0));
	vec3 blend = sample_retail_blendmap(blend_uv, blendmap_uv).rgb;
	vec3 c1 = sample_retail_detail_mips(u_detail_c1, detail_uv).rgb;
	vec3 c2 = sample_retail_detail_mips(u_detail_c2, detail_uv).rgb;
	vec3 c3 = sample_retail_detail_mips(u_detail_c3, detail_uv).rgb;

	// mul/mad: splat = c1*R + c2*G + c3*B [orig: @ 0x7dece0 rows 1-3]
	vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

	// The witnessed stage-3 term: the second detail pair sampled at its own
	// authored density, dp3'd with the normalized blend and doubled. Maps
	// without a second detail run the PS14Splat variant, which has no such
	// stage — the factor is exactly 1.
	// [orig: g_PolyTrnPS14SplatNormalMap vs PS14Splat @ 0x605260; selection on
	// detailmap2 presence @ 0x604544/0x6044e8; stage-3 bind @ 0x6043ff]
	float normal_factor = 1.0;
	if (u_has_detail2) {
		// Below water the live stage-3 bind is the water noise at the swapped
		// 8/density coord (see the u_water_noise uniform block); the dp3
		// modulation then IS the underwater caustic term, neutral at mid-gray.
		// The noise pair is a per-frame mipless dynamic texture — plain
		// bilinear, no retail mip chain to emulate.
		vec3 modulator = below_water
			? texture(u_water_noise,
				retail_detail_uv_from_atlas(colormap_uv, 8.0)).rgb
			: sample_retail_detail_mips(
				u_detail2, retail_detail_uv_from_atlas(
					colormap_uv, u_detail2_density)).rgb;
		normal_factor = dot(modulator, blend) * 2.0;
	}

	// mad_d2: light = (tile.a * c1 + c0) / 2 with the WITNESSED constants.
	// tile.a is the page's cleared alpha plus the additive, byte-quantized
	// heightfield-normal DOT3 bake on canonical cache pages (the .til and scorch
	// overlays write RGB only); raw authored colormap alpha is cleared by the
	// base pass and never reaches t0.
	// [orig: PolyTrn_RenderTile @ 0x60da70; Terrain_GenerateNormalMap
	// @ 0x603210; Terrain_SetupLightingAndShader @ 0x604420 pushes
	//  c0 <- g_PolyTrnPSConstC0SkyColorR @ 0x31a183c, c1 <- PolyTrn_PSConstC1_Light
	//  @ 0x31a182c; values from Terrain_InitLightingColorRamps @ 0x604ee0
	//  <- Render_TerrainScene @ 0x610c80 (g_EnvLightBlock / g_EnvSkyBlock)]
	vec3 ambient = (cm.a * u_sun_light + u_sky_ambient) * 0.5;

	// mul_x2 twice, then mul_x4 [orig: @ 0x7dece0 tail]:
	// out = light *2 colormap *2 dp3 *4 splat.
	vec3 lit = ambient * cm.rgb * 2.0;
	lit *= normal_factor;
	return lit * detail * 4.0;
}

// The pass start is already Render_SetFogState's per-type start, overcast
// fold included (type 2 = (1 - density) * end * 0.5, type 3 = (1 - density) *
// end * 0.25) [orig: Render_SetFogState @ 0x58a9c5..0x58a9fe].
float jointops_fog_factor(float dist, float fog_start, float fog_end, int fog_type) {
	float safe_end = max(fog_end, 1.0);
	if (fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}
	float fog_range = max(safe_end - fog_start, 1.0);
	return clamp((safe_end - dist) / fog_range, 0.0, 1.0);
}

// Type 0 consumes eye-space depth/Z. Linear modes consume radial camera
// distance. [orig: Render_SetFogState @ 0x58a950 ->
// CD3DDevice_SetFogParameters @ 0x677960]
vec3 apply_terrain_fog(vec3 color, vec3 world_pos, vec3 camera_pos, float eye_depth) {
	float radial_distance = length(world_pos - camera_pos);
	float dist = u_fog_type == 0 ? max(eye_depth, 0.0) : radial_distance;
	float fog_factor = jointops_fog_factor(dist, u_fog_start, u_fog_end, u_fog_type);
	return mix(u_fog_color, color, fog_factor);
}

// The terrain batch's fog in the pass drawing it: the water mirror pass fogs
// with its own (dry) block, every other pass with the scene block.
vec3 apply_terrain_pass_fog(vec3 color, vec3 world_pos, vec3 camera_pos, float eye_depth,
		bool water_mirror) {
	if (!water_mirror) {
		return apply_terrain_fog(color, world_pos, camera_pos, eye_depth);
	}
	int fog_type = int(opennova_water_mirror_fog_range.z);
	float dist = fog_type == 0 ? max(eye_depth, 0.0) : length(world_pos - camera_pos);
	float fog_factor = jointops_fog_factor(dist, opennova_water_mirror_fog_range.x,
		opennova_water_mirror_fog_range.y, fog_type);
	return mix(opennova_water_mirror_fog_color, color, fog_factor);
}
