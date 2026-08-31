#include "res://shaders/foliage_fd_sampling.gdshaderinc"

// Expanded detail foliage. Retail emits the source model once per accepted
// candidate and bends every transformed vertex onto the terrain before this
// shader runs [orig: generate_foliage_instances_0 @ 0x5ffdd0].
// u_fd_texture stays RAW: its rgb rides the witnessed x8 fold whose
// mid-gray emitter constant (c6) is byte-neutral by construction.
uniform sampler2D u_fd_texture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_fd_texture = false;
uniform sampler2D u_colormap : source_color, filter_linear, repeat_disable;
uniform bool u_has_colormap = false;
uniform sampler2D u_heightfield_normal : filter_linear, repeat_disable;
uniform bool u_has_heightfield_normal = false;
uniform sampler2D u_tile_overlay : source_color, filter_linear, repeat_enable;
uniform bool u_has_tile_overlay = false;
uniform vec3 u_tile_overlay_tint : source_color = vec3(1.0);
uniform sampler2DArray u_tile_cache : source_color, filter_linear, repeat_disable;
uniform bool u_has_tile_cache = false;
uniform vec3 u_emitter_color = vec3(0.5019608);
instance uniform float u_wind_phase = 0.0;
instance uniform float u_fade = 1.0;
instance uniform float u_alpha_ref = 0.0;
instance uniform bool u_instance_tile_cache_ready = false;
instance uniform float u_instance_tile_cache_layer = 0.0;
// Exact max-quality retail c7/c8 reduction: world origin X/Z, inverse world
// span, and span. Foliage_WindSwayVS projects the original pre-wind vertex.
instance uniform vec4 u_instance_tile_cache_projection = vec4(0.0);
// Nonzero only on the near LOW resubmission: retail selects strict
// D3DCMP_LESS there, so identical geometry only lands where the preceding
// HIGH draw rejected alpha and left no depth. Discarding texels the HIGH
// pass accepted reproduces that selection without a compare-op toggle.
// [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912;
// CGfxDevice ZFUNC wrapper @ 0x67cac0..0x67caea]
instance uniform float u_high_pass_cutoff = 0.0;

global uniform vec3 opennova_sky_ambient;
global uniform vec3 opennova_sun_direction;
// Do not add EffectWorld point-light uniforms here. Retail does call
// Light_SelectAndEnableForDraw for each patch @ 0x60a5dc, but the selected
// highest-quality path binds Foliage_WindSwayVS @ 0x60087a..0x600883. Its
// complete vs_1_1 source declares no normal/light input and writes oD0 = c6;
// the blend PS therefore receives only the emitter/fade vertex color below.
// D3D SetLight/LightEnable state can affect only the failed-VS fixed-function
// fallback, which is outside the locked highest-quality profile.
// [orig: Terrain_CreateFoliageVertexShaders @ 0x5ff630;
// Foliage_RenderFarPatches @ 0x609de0]

varying vec2 v_tile_uv;
varying vec2 v_overlay_uv;
varying vec2 v_tile_cache_uv;
varying vec3 v_heightfield_normal;

vec2 foliage_quadrant_clamped_uv(vec2 uv, ivec2 dimensions) {
	vec2 quadrant = floor(clamp(uv * 2.0, vec2(0.0), vec2(1.999999)));
	vec2 texel = 1.0 / vec2(dimensions);
	return clamp(
		uv,
		quadrant * 0.5 + texel * 0.5,
		(quadrant + 1.0) * 0.5 - texel * 0.5);
}

void vertex() {
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 pre_wind_world_pos = world_pos;
	// v5.x is the source-height bend byte. The retail VS displaces render-Z
	// by sin(position.x + phase) * bend * 0.03
	// [orig: Foliage_WindSwayVS source @ 0x5ff691].
	world_pos.z += sin(world_pos.x + u_wind_phase) * COLOR.r * 0.03;

	// CPU-expanded vertices carry the retail c7/c8 planar terrain projection
	// in UV2. This preserves TRN sector-origin/quadrant mapping; deriving it
	// from raw world XZ would only be correct for an origin-zero atlas.
	v_tile_uv = UV2;
	// Retail projects t1 independently from v0, while only oPos consumes the
	// wind-displaced r1. The hosted 1024 .til bake uses the same world X/-Z
	// wrap, so preserve the pre-wind position here. Wrap in the FRAGMENT
	// stage: a per-vertex fract() would interpolate ~0.999 -> ~0.001 across a
	// 1024-unit wrap seam and smear the whole overlay atlas over that
	// triangle (u_tile_overlay is repeat_enable, so unwrapped UV is exact).
	v_overlay_uv =
		vec2(pre_wind_world_pos.x, -pre_wind_world_pos.z) * (1.0 / 1024.0);
	v_tile_cache_uv =
		(pre_wind_world_pos.xz - u_instance_tile_cache_projection.xy) *
		u_instance_tile_cache_projection.z;
	v_heightfield_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}

void fragment() {
	vec4 fd = u_has_fd_texture
		? sample_retail_foliage_fd(u_fd_texture, UV)
		: vec4(0.5, 0.5, 0.5, 1.0);
	// Retail alpha = t0.a * v0.a feeds both the GREATER alpha test and the
	// SRCALPHA/INVSRCALPHA framebuffer blend enabled in the detail technique
	// block. [orig: Foliage_LoadDefAssets @ 0x60141f..0x601427;
	// Foliage_CreateLightmapBlendPS @ 0x5ff7a0 "+mul r0.a, t0.a, v0.a"]
	float retail_alpha = fd.a * u_fade;
	if (retail_alpha <= u_alpha_ref) {
		discard;
	}
	if (u_high_pass_cutoff > 0.0 && retail_alpha > u_high_pass_cutoff) {
		discard;
	}
	vec4 tile = vec4(0.5, 0.5, 0.5, 1.0);
	if (u_has_tile_cache && u_instance_tile_cache_ready) {
		vec2 page_uv = v_tile_cache_uv;
		vec2 half_texel = vec2(0.5 / 256.0);
		page_uv = clamp(page_uv, half_texel, vec2(1.0) - half_texel);
		tile = texture(u_tile_cache, vec3(
			page_uv, u_instance_tile_cache_layer));
	} else if (u_has_colormap) {
		vec2 colormap_uv = foliage_quadrant_clamped_uv(
			v_tile_uv, textureSize(u_colormap, 0));
		vec3 raw_colormap = texture(u_colormap, colormap_uv).rgb;
		vec3 normal_byte;
		if (u_has_heightfield_normal) {
			vec2 normal_uv = foliage_quadrant_clamped_uv(
				v_tile_uv, textureSize(u_heightfield_normal, 0));
			normal_byte = texture(u_heightfield_normal, normal_uv).rgb;
		} else {
			vec3 texture_basis_normal = vec3(
				v_heightfield_normal.x,
				v_heightfield_normal.z,
				v_heightfield_normal.y);
			normal_byte = floor(clamp(
				(normalize(texture_basis_normal) + 1.0) * 127.5,
				vec3(0.0), vec3(255.0))) / 255.0;
		}
		vec3 texture_basis_light = vec3(
			opennova_sun_direction.z,
			opennova_sun_direction.x,
			opennova_sun_direction.y);
		vec3 light_byte = floor(clamp(
			(texture_basis_light + 1.0) * 127.5,
			vec3(0.0), vec3(255.0))) / 255.0;
		vec3 composed_rgb =
			clamp(raw_colormap * (256.0 / 255.0), vec3(0.0), vec3(1.0));
		if (u_has_tile_overlay) {
			vec4 overlay = texture(u_tile_overlay, v_overlay_uv);
			composed_rgb = mix(
				composed_rgb, overlay.rgb * u_tile_overlay_tint, overlay.a);
		}
		tile = vec4(
			composed_rgb,
			clamp(4.0 * dot(normal_byte - 0.5, light_byte - 0.5), 0.0, 1.0));
	}

	// Exact ps.1.4 combine: t0 * (t1 * (t1.a*c1 + c0)) * v0 * 8, re-expressed
	// through the lit pipeline so the sun leg receives the scene CSM
	// (ATTENUATION in light()) exactly like the terrain beneath it — the
	// unshaded form painted full-sun foliage over every dynamic shadow near
	// the camera (retail had no dynamic ground shadows to receive; ADR 0043's
	// sun does). The analytic bare-ground sample reproduces the byte-quantized
	// heightfield DOT3 stored in composed tile alpha; that DOT3 rides tile.a
	// into the sun leg as before. The sky leg stays the flat unconditional add
	// (EMISSION, ambient_light_disabled on the wrappers); the retail sum-clamp
	// becomes per-leg clamps, the terrain fold's precedent.
	// [orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0].
	vec3 fold_rgb = fd.rgb * u_emitter_color * 8.0;
	vec3 lit = tile.rgb;
	ALBEDO = clamp(fold_rgb * lit * tile.a, vec3(0.0), vec3(1.0));
	EMISSION = clamp(fold_rgb * lit, vec3(0.0), vec3(1.0)) * opennova_sky_ambient;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	// Environment depth fog (ADR 0043, MissionEnvironment::apply_scene_fog
	// carries the witnessed .env law) folds into the source color before the
	// blend, like D3D's fold, so the blended weight stays the retail SRCALPHA
	// term.
	ALPHA = clamp(retail_alpha, 0.0, 1.0);
}

void light() {
	// The sun leg of the retail combine through the scene light: LIGHT_COLOR
	// arrives color * energy(2) * PI, so 0.5/PI restores sun_light, and
	// ATTENUATION lands the CSM mask on foliage exactly as on the terrain
	// under it. The hemisphere delta pair excludes this layer via
	// light_cull_mask (SunShadow::_make_hemi_light) and every omni must
	// contribute nothing (the LIGHT_IS_DIRECTIONAL gate below) — the
	// witnessed highest-quality path binds Foliage_WindSwayVS, whose complete
	// vs_1_1 source declares no light inputs and writes oD0 = c6, so the
	// blend PS sees only the emitter/fade vertex color; D3D SetLight state
	// could reach only the failed-VS fixed-function fallback
	// [orig: Foliage_RenderFarPatches @ 0x60a5dc;
	// Terrain_CreateFoliageVertexShaders @ 0x5ff630].
	if (LIGHT_IS_DIRECTIONAL) {
		DIFFUSE_LIGHT += LIGHT_COLOR * (0.5 / PI) * ATTENUATION * ALBEDO;
	}
}
