// Shared NovaLogic terrain lighting + fog math, included by both terrain.gdshader
// (runtime, baked-CPT mesh) and terrain_editor.gdshader (editor live-sculpt
// heightmap mesh). Both render the SAME surface; only their geometry source and
// overlays differ, so the surface-shading and fog math live here once.
//
// The surface chain is the witnessed TOP-TIER ps.1.4 splat shader
// [orig: PolyTrn_PS14SplatNormalMap source text @ 0x7dece0, assembled by
// compile_terrain_pixel_shaders @ 0x605260]:
//   splat = c1*blend.r + c2*blend.g + c3*blend.b        (3-way detail splat)
//   dp3   = dot(normalmap(t3), blendmap)
//   light = (tile.a*c1 + c0) / 2                        (c1 = LIGHT, c0 = SKY)
//   out   = light *2 cm *2 dp3 *4 splat
// EXACTLY ONE normal term: the dual-normal product belonged to the separate
// ps.1.1 non-splat tier (PolyTrn_PSDualNormalMap) and was wrongly STACKED on
// the splat here for years (the gobj-era "v23 dual-normal" chimera) — under
// the gamma-faithful pipeline (D-RMAT-7) that squared bump contrast and
// clipped whole regions to white. Corrected 2026-07-06 (D-TRN row in the
// ledger). Fog: [orig: Render_SetFogState @ 0x58a950 ->
// CD3DDevice_SetFogParameters @ 0x677960]. Keep byte-for-byte authoritative.

// --- Shared uniforms: terrain surface inputs + lighting + fog -----------------
// Textures sample RAW (no source_color): retail never enables sRGB sampling -
// texel bytes enter the ps.1.4 math as-is. The gamma-space convention
// (D-RMAT-7) lives in nova_color.gdshaderinc; consumers encode the final
// color once at their ALBEDO write.
uniform sampler2D u_colormap : filter_linear, repeat_disable;
uniform sampler2D u_detail_c1 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c2 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_detail_c3 : filter_linear_mipmap, repeat_enable;
uniform sampler2D u_blendmap : filter_linear, repeat_disable;
uniform sampler2D u_detail_coefficient : filter_linear_mipmap, repeat_disable;
uniform bool u_has_detail_coefficient = false;
// Retail creates a mip chain but the TileBakeDot3 pass explicitly uses
// base-level bilinear sampling with quadrant CLAMP.
uniform sampler2D u_heightfield_normal : filter_linear, repeat_disable;
uniform bool u_has_heightfield_normal = false;
// PolyTrn_RenderTile composes the mission .til base layer into the terrain
// tile before the same ps.1.4 lighting chain used by runtime and ONED.
uniform sampler2D u_tile_overlay : filter_linear, repeat_enable;
uniform bool u_has_tile_overlay = false;
uniform vec3 u_tile_overlay_tint = vec3(1.0, 1.0, 1.0);
// ONED live-edit fallback only: runtime binds the byte-faithful preprocessed
// coefficient map above. Keeping this authored source sampler lets the shared
// editor shader preserve retail semantics without rebaking on every paint.
uniform sampler2D u_detailmap : filter_nearest_mipmap, repeat_disable;
// Un-enved defaults = the retail noon register (full_00.env tod 1200:
// sun_rgb 170,170,167; sky_rgb 84,88,89 — bytes /255), one register with the
// object-model/global defaults.
uniform vec3 u_sun_light = vec3(0.666667, 0.666667, 0.654902);
uniform vec3 u_sky_ambient = vec3(0.329412, 0.345098, 0.349020);
uniform vec3 u_sun_direction = vec3(0.0, 0.70710678, 0.70710678);
uniform float u_detail_density = 128.0;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 0.0;
uniform float u_fog_end = 1024.0;
uniform int u_fog_type = 0;

vec4 sample_retail_detail_mips(sampler2D source, vec2 uv) {
	// The reference configuration (texfilter_level 0) sets MAG/MIN LINEAR
	// with MIPFILTER POINT and zero bias: retail holds each paired mip sharp
	// and snaps between levels instead of trilinearly pre-blending toward the
	// far texture. Select the nearest retail level explicitly, and clamp the
	// selection at the 4x4 terminal level (the 2x2/1x1 tail exists only for
	// Godot Image completeness).
	// [orig: device filter setup @ 0x679c28..0x679c9c, 0x677f9e..0x677ff9;
	// per-stage MIPFILTER select @ 0x67e3f7..0x67e4a7]
	vec2 dimensions = vec2(textureSize(source, 0));
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float footprint = max(length(dx * dimensions), length(dy * dimensions));
	float requested_lod = max(log2(max(footprint, 1.0)), 0.0);
	float terminal_lod = max(floor(log2(max(min(dimensions.x, dimensions.y), 4.0))) - 2.0, 0.0);
	float selected_lod = clamp(floor(requested_lod + 0.5), 0.0, terminal_lod);
	return textureLod(source, uv, selected_lod);
}

vec2 clamp_retail_quadrant_uv(vec2 uv, vec2 quadrant, ivec2 dimensions) {
	vec2 texel = 1.0 / vec2(dimensions);
	vec2 minimum_uv = quadrant * 0.5 + texel * 0.5;
	vec2 maximum_uv = (quadrant + 1.0) * 0.5 - texel * 0.5;
	return clamp(uv, minimum_uv, maximum_uv);
}

vec3 sample_detail_coefficient(vec2 coefficient_uv, vec2 quadrant) {
	if (u_has_detail_coefficient) {
		coefficient_uv = clamp_retail_quadrant_uv(
			coefficient_uv, quadrant, textureSize(u_detail_coefficient, 0));
		return texture(u_detail_coefficient, coefficient_uv).rgb;
	}

	// Live-editor fallback for an authored B-channel detailmap. Runtime never
	// takes this path. Retail assets are power-of-two, so dimension-1 masks
	// reproduce Texture_GenerateNormalMap's wrapped neighbor taps.
	ivec2 dimensions = textureSize(u_detailmap, 0);
	if (dimensions.x <= 0 || dimensions.y <= 0) {
		return vec3(0.5, 0.5, 1.0);
	}
	coefficient_uv = clamp_retail_quadrant_uv(
		coefficient_uv, quadrant, dimensions);
	// The coefficient texture itself clamps at terrain-atlas edges. Only the
	// generator's four neighbor taps wrap inside its power-of-two source.
	ivec2 center = clamp(
		ivec2(floor(clamp(coefficient_uv, vec2(0.0), vec2(1.0)) * vec2(dimensions))),
		ivec2(0), dimensions - ivec2(1));
	int x_mask = dimensions.x - 1;
	int y_mask = dimensions.y - 1;
	float left = texelFetch(u_detailmap, ivec2((center.x - 1) & x_mask, center.y), 0).b;
	float right = texelFetch(u_detailmap, ivec2((center.x + 1) & x_mask, center.y), 0).b;
	float previous = texelFetch(u_detailmap, ivec2(center.x, (center.y - 1) & y_mask), 0).b;
	float next = texelFetch(u_detailmap, ivec2(center.x, (center.y + 1) & y_mask), 0).b;
	vec3 coefficient = normalize(vec3(
		(left - right) * (255.0 / 32.0),
		(previous - next) * (255.0 / 32.0),
		1.0));
	// Texture_GenerateNormalMap truncates each encoded component to RGBA8
	// before the ps.1.4 sample. Keep the live editor fallback on that byte grid.
	ivec3 encoded = ivec3(clamp((coefficient + 1.0) * 127.5, vec3(0.0), vec3(255.0)));
	return vec3(encoded) / 255.0;
}

vec3 quantize_retail_signed_vector(vec3 value) {
	// Both Terrain_GenerateNormalMap and PolyTrn_RenderTile truncate
	// (component+1)*127.5 to bytes before D3DTA_TEXTURE/D3DTA_DIFFUSE DOT3.
	ivec3 encoded = ivec3(clamp((value + 1.0) * 127.5,
		vec3(0.0), vec3(255.0)));
	return vec3(encoded) / 255.0;
}

float retail_tile_light_alpha(vec3 heightfield_normal, vec2 terrain_uv,
		vec2 quadrant) {
	vec3 normal_byte = u_has_heightfield_normal
		? texture(u_heightfield_normal, clamp_retail_quadrant_uv(
			terrain_uv, quadrant, textureSize(u_heightfield_normal, 0))).rgb
		: quantize_retail_signed_vector(normalize(vec3(
			heightfield_normal.x, heightfield_normal.z, heightfield_normal.y)));
	// Env exposes the direct retail getter tuple. PolyTrn packs that tuple into
	// D3DCOLOR RGB as (Z, X, Y) before the heightfield-normal DOT3.
	// [orig: Environment_GetLightDirectionFloat @ 0x57d870;
	//  Terrain_GenerateNormalMap @ 0x603210;
	//  PolyTrn_TileBakeDot3LightPass @ 0x60e385..0x60e39e].
	vec3 texture_basis_light = vec3(u_sun_direction.z, u_sun_direction.x, u_sun_direction.y);
	vec3 light_byte = quantize_retail_signed_vector(texture_basis_light);
	return clamp(4.0 * dot(normal_byte - 0.5, light_byte - 0.5), 0.0, 1.0);
}

vec4 retail_bare_terrain_tile(vec3 raw_colormap_rgb, vec3 heightfield_normal,
		vec2 terrain_uv, vec2 quadrant) {
	// The base tile bake uses diffuse 0x00808080: RGB MODULATE2X gives
	// raw*256/255 while its zero diffuse alpha discards authored colormap A.
	// The following additive DOT3 pass writes the heightfield light term to A.
	return vec4(
		clamp(raw_colormap_rgb * (256.0 / 255.0), vec3(0.0), vec3(1.0)),
		retail_tile_light_alpha(heightfield_normal, terrain_uv, quadrant));
}

vec4 compose_retail_tile_overlay(vec4 colormap, vec3 world_pos) {
	if (u_has_tile_overlay) {
		vec2 tile_uv = fract(vec2(world_pos.x, -world_pos.z) * (1.0 / 1024.0));
		vec4 tile = texture(u_tile_overlay, tile_uv);
		// Preserve the bare-ground DOT3 alpha while replacing only base RGB,
		// matching the pre-lighting PolyTrn_RenderTile composite.
		colormap.rgb = mix(colormap.rgb, tile.rgb * u_tile_overlay_tint, tile.a);
	}
	return colormap;
}

// NovaLogic terrain surface shading — the witnessed ps.1.4 splat chain
// [orig: PolyTrn_PS14SplatNormalMap @ 0x7dece0]. No fog/overlays — callers
// layer those on top. Colormap/detail UVs come from the caller's geometry
// (runtime: baked patch mesh; editor: sculpted heightmap mesh).
vec3 terrain_surface_color_from_colormap(vec4 cm, vec2 colormap_uv,
		vec2 detail_uv, vec2 quadrant) {
	vec2 blend_uv = clamp_retail_quadrant_uv(
		colormap_uv, quadrant, textureSize(u_blendmap, 0));
	vec3 blend = texture(u_blendmap, blend_uv).rgb;
	vec3 c1 = sample_retail_detail_mips(u_detail_c1, detail_uv).rgb;
	vec3 c2 = sample_retail_detail_mips(u_detail_c2, detail_uv).rgb;
	vec3 c3 = sample_retail_detail_mips(u_detail_c3, detail_uv).rgb;

	// mul/mad: splat = c1*R + c2*G + c3*B [orig: @ 0x7dece0 rows 1-3]
	vec3 detail = c1 * blend.r + c2 * blend.g + c3 * blend.b;

	// The single witnessed coefficient term: t3 is generated from the authored
	// detailmap B channel at scale 1/32, then dp3'd with DBlend.
	// [orig: Texture_GenerateNormalMap @ 0x58c070;
	// PolyTrn_PS14SplatNormalMap @ 0x7dece0]
	vec3 nmap = sample_detail_coefficient(colormap_uv, quadrant);
	float normal_factor = dot(nmap, blend) * 2.0;

	// mad_d2: light = (tile.a * c1 + c0) / 2 with the WITNESSED constants.
	// tile.a is the additive, byte-quantized heightfield-normal DOT3 bake; raw
	// authored colormap alpha is cleared by the base pass and never reaches t0.
	// [orig: PolyTrn_RenderTile @ 0x60da70; Terrain_GenerateNormalMap
	// @ 0x603210; terrain_setup_lighting_and_shader @ 0x604420 pushes
	//  c0 <- PolyTrn_PSConstC0_Sky @ 0x31a183c, c1 <- PolyTrn_PSConstC1_Light
	//  @ 0x31a182c; values from init_terrain_lighting_color_ramps @ 0x604ee0
	//  <- Render_TerrainScene @ 0x610c80 (Env_LightBlock / Env_SkyBlock)]
	vec3 ambient = (cm.a * u_sun_light + u_sky_ambient) * 0.5;

	// mul_x2 twice, then mul_x4 [orig: @ 0x7dece0 tail]:
	// out = light *2 colormap *2 dp3 *4 splat.
	vec3 lit = ambient * cm.rgb * 2.0;
	lit *= normal_factor;
	return lit * detail * 4.0;
}

vec3 terrain_surface_color(vec2 colormap_uv, vec2 detail_uv,
		vec3 heightfield_normal, vec2 quadrant) {
	vec2 sampled_colormap_uv = clamp_retail_quadrant_uv(
		colormap_uv, quadrant, textureSize(u_colormap, 0));
	return terrain_surface_color_from_colormap(
		retail_bare_terrain_tile(texture(u_colormap, sampled_colormap_uv).rgb,
			heightfield_normal, colormap_uv, quadrant),
		colormap_uv, detail_uv, quadrant);
}

float jointops_fog_factor(float dist, float fog_start, float fog_end, int fog_type) {
	float safe_end = max(fog_end, 1.0);
	if (fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}

	float start = fog_start;
	if (fog_type == 2) {
		start = safe_end * 0.5;
	} else if (fog_type == 3) {
		start = safe_end * 0.25;
	}
	float fog_range = max(safe_end - start, 1.0);
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
