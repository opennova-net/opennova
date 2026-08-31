// Shared NovaLogic terrain SURFACE math, included by terrain.gdshader.
//
// ADR 0043 (modern presentation): the terrain shader is lit by Godot — the
// DirectionalLight3D sun, the hemisphere-sky ambient, clustered omni lights,
// Environment fog and (stage 4) CSM shadows replaced the fixed-function light
// fold `(cm.a*sun + sky)*0.5 * 2 cm * 2 dp3 * 4 splat`, the projected
// light-pool pass and the hand fog that lived here. What remains witnessed is
// the authored GROUND LOOK — the ps.1.4 splat chain's texture composition
// [orig: PolyTrn_PS14SplatNormalMap source text @ 0x7dece0, assembled by
// compile_terrain_pixel_shaders @ 0x605260]:
//   splat = c1*blend.r + c2*blend.g + c3*blend.b        (3-way detail splat)
//   dp3   = dot(detail2(t3), blendmap)  — only in the SplatNormalMap variant,
//           selected when polytrn_detailmap2 exists (PS14Splat otherwise)
// composed here as the lit material's ALBEDO. The x8 of retail's combined
// modulate ladder splits into ALBEDO x4 and the MODULATE2X light energy the
// sun/ambient carry (SunShadow / MissionEnvironment).

// --- Shared uniforms: terrain surface inputs ---------------------------------
// Color art decodes via source_color (the scene is linear — ADR 0043
// linear-scene amendment); the witnessed splat arithmetic keeps its text and
// constants over the decoded inputs. Data textures (blend weights, the dp3
// modulator, water noise) stay raw — they are weights, not color.
// The device-global texfilter mode applies to EVERY active stage
// [orig: CGfxDevice_ApplyRenderStates per-stage loop @ 0x67e3b5..0x67e50e],
// and the reference machine runs the anisotropic mode, so every mipped
// terrain input samples anisotropically. The colormap here is the box-mipped
// surrogate for the per-LOD tile-cache RT (D-TERRAIN-7); the DBlendmap
// quadrants are box-mipped in retail (created with flags 0x100001
// [orig: PolyTrn_InitTextures @ 0x60b2c9]).
uniform sampler2D u_colormap : source_color, filter_linear_mipmap_anisotropic, repeat_disable;
uniform sampler2D u_detail_c1 : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_detail_c2 : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_detail_c3 : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_blendmap : filter_linear_mipmap_anisotropic, repeat_disable;
// The ps.1.4 splat's stage-3 dp3 input: the authored SECOND detail pair at
// its own density, dp3'd with the normalized blend — a blend-weighted
// scalar modulation x2. It is NOT a normal map; the generated coefficient
// map belongs to the ps.1.1 tiers at stage 7.
// [orig: stage bind @ 0x6043ff (dword_319F998); texcoord transform
// density2/density @ 0x609810 (dword_31A1814 / dword_31A1810);
// PolyTrn_PS14SplatNormalMap "dp3 r3, r3, r2; mul_x2" @ 0x605260]
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
// modulation (docs/terrain/terrain-re.md, underwater section).
// [orig: below-water flag @ 0x60FEE0 -> dword_319FB3C @ 0x60915F; live t3
//  slot swap @ 0x6043f2 (dword_3266E98 <- Water_NoiseColorTexture, authored
//  dword_319F998 otherwise); shader select @ 0x6044b1..0x604556; 8/density
//  texcoord @ 0x609786..0x6097D6]
uniform sampler2D u_water_noise : filter_linear, repeat_enable;
uniform bool u_below_water = false;
// PolyTrn_RenderTile composes the mission .til base layer into the terrain
// tile before the shared surface chain used by runtime and ONED.
uniform sampler2D u_tile_overlay : source_color, filter_linear, repeat_enable;
uniform bool u_has_tile_overlay = false;
uniform vec3 u_tile_overlay_tint : source_color = vec3(1.0, 1.0, 1.0);
// Declared for the ONED authored-slot binding only; the top-tier chain does
// not consume it (the generated coefficient map belongs to the unported
// ps.1.1 tiers at stage 7).
uniform sampler2D u_detailmap : filter_nearest_mipmap, repeat_disable;
uniform float u_detail_density = 128.0;

// Retail's combined modulate ladder is x8 total; the light legs carry the
// MODULATE2X x2 as light energy, leaving x4 with the texture product.
const float TERRAIN_ALBEDO_SCALE = 4.0;
// The linear-gain conversion (ADR 0043 linear-scene amendment): a witnessed
// BYTE-domain constant gain k acts on the linear scene as ~k^2.2
// (decode(k*x) ~= k^2.2 * decode(x), the pure-power sRGB approximation) —
// without it the decoded-texel splat product collapses and noon terrain
// renders near-black. The witnessed combine text and its constants stay;
// the exponent is the one seam conversion.
const float LINEAR_GAIN_EXPONENT = 2.2;

vec2 retail_detail_uv_from_atlas(vec2 atlas_uv, float density) {
	// Retail writes UV1 as source_position * (polytrn_detaildensity / 512).
	// Runtime and ONED instead carry normalized UVs for the full 1024 terrain
	// atlas, so multiply by two before applying either authored density.
	// [orig: configData+0x1738 @ 0x31BE270; density/512 global write
	// @ 0x6029a0..0x6029aa; UV1 stores @ 0x602db5..0x602dbe]
	return atlas_uv * (density * 2.0);
}

vec4 sample_retail_detail_mips(sampler2D source, vec2 uv) {
	// The witnessed device supports texfilter modes 0-4; modes 3/4 select
	// MINFILTER=ANISOTROPIC with MAXANISOTROPY [orig: per-stage filter select
	// @ 0x67e38a..0x67e45b; device init @ 0x679c28..0x679c9c]. The retail
	// reference captures show the grain/textured-slope result of anisotropic
	// ground sampling, which pins the sampler mode but not an exact host LOD
	// reconstruction. The explicit longest-gradient scale below is a
	// conservative terminal guard: it keeps Godot's synthetic 2x2/1x1 tail
	// unselectable, but can bias a grazing anisotropic footprint sharper than
	// the reference device's minor-axis/maximum-anisotropy selection.
	vec2 dimensions = vec2(textureSize(source, 0));
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float footprint = max(length(dx * dimensions), length(dy * dimensions));
	float requested_lod = max(log2(max(footprint, 1.0)), 0.0);
	float terminal_lod = max(floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0, 0.0);
	float gradient_scale = exp2(min(terminal_lod - requested_lod, 0.0));
	return textureGrad(source, uv, dx * gradient_scale, dy * gradient_scale);
}

vec2 clamp_retail_quadrant_uv(vec2 uv, vec2 quadrant, ivec2 dimensions) {
	vec2 texel = 1.0 / vec2(dimensions);
	vec2 minimum_uv = quadrant * 0.5 + texel * 0.5;
	vec2 maximum_uv = (quadrant + 1.0) * 0.5 - texel * 0.5;
	return clamp(uv, minimum_uv, maximum_uv);
}

vec3 retail_bare_terrain_tile(vec3 raw_colormap_rgb) {
	// The base tile bake uses diffuse 0x00808080: RGB MODULATE2X gives
	// raw*256/255 [orig: PolyTrn_RenderTile @ 0x60da70]. The DOT3 sun-mask
	// alpha the retail bake added is retired (ADR 0043): Godot's per-pixel
	// N.L and CSM carry the terrain light shape.
	return clamp(raw_colormap_rgb * (256.0 / 255.0), vec3(0.0), vec3(1.0));
}

vec3 compose_retail_tile_overlay(vec3 colormap_rgb, vec3 world_pos) {
	if (u_has_tile_overlay) {
		vec2 tile_uv = fract(vec2(world_pos.x, -world_pos.z) * (1.0 / 1024.0));
		vec4 tile = texture(u_tile_overlay, tile_uv);
		// Analytic/editor fallback: the legacy full-atlas surrogate retains
		// standard coverage but not retail's ordered render-target alpha. The
		// canonical runtime page cache composes exact overlay RGBA; this cold
		// path therefore applies the overlay to RGB only.
		colormap_rgb = mix(colormap_rgb, tile.rgb * u_tile_overlay_tint, tile.a);
	}
	return colormap_rgb;
}

// The authored ground texture composition — the witnessed ps.1.4 splat chain's
// texture half [orig: PolyTrn_PS14SplatNormalMap source text @ 0x7dece0],
// serving as the lit material's ALBEDO. Colormap/detail UVs come from the
// caller's geometry.
vec3 terrain_surface_albedo(vec3 cm_rgb, vec2 colormap_uv,
		vec2 detail_uv, vec2 quadrant) {
	vec2 blend_uv = clamp_retail_quadrant_uv(
		colormap_uv, quadrant, textureSize(u_blendmap, 0));
	vec3 blend = texture(u_blendmap, blend_uv).rgb;
	vec3 c1 = sample_retail_detail_mips(u_detail_c1, detail_uv).rgb;
	vec3 c2 = sample_retail_detail_mips(u_detail_c2, detail_uv).rgb;
	vec3 c3 = sample_retail_detail_mips(u_detail_c3, detail_uv).rgb;

	// mul/mad: splat = c1*R + c2*G + c3*B [orig: @ 0x7dece0 rows 1-3]
	vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

	// The witnessed stage-3 term: the second detail pair sampled at its own
	// authored density, dp3'd with the normalized blend and doubled. Maps
	// without a second detail run the PS14Splat variant, which has no such
	// stage — the factor is exactly 1.
	// [orig: PolyTrn_PS14SplatNormalMap vs PS14Splat @ 0x605260; selection on
	// detailmap2 presence @ 0x604544/0x6044e8; stage-3 bind @ 0x6043ff]
	float normal_factor = 1.0;
	if (u_has_detail2) {
		// Below water the live stage-3 bind is the water noise at the swapped
		// 8/density coord (see the u_water_noise uniform block); the dp3
		// modulation then IS the underwater caustic term, neutral at mid-gray.
		// The noise pair is a per-frame mipless dynamic texture — plain
		// bilinear, no retail mip chain to emulate.
		vec3 modulator = u_below_water
			? texture(u_water_noise,
				retail_detail_uv_from_atlas(colormap_uv, 8.0)).rgb
			: sample_retail_detail_mips(
				u_detail2, retail_detail_uv_from_atlas(
					colormap_uv, u_detail2_density)).rgb;
		normal_factor = dot(modulator, blend) * 2.0;
	}

	// The x4 byte-domain gain converts to its linear equivalent (see
	// LINEAR_GAIN_EXPONENT); the dp3 modulator term stays as authored — its
	// raw mid-gray x2 is neutral at 1 in either domain.
	return cm_rgb * detail * normal_factor *
		pow(TERRAIN_ALBEDO_SCALE, LINEAR_GAIN_EXPONENT);
}
