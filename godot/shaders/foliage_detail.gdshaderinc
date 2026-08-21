#include "res://shaders/foliage_fd_sampling.gdshaderinc"

// Expanded detail foliage. Retail emits the source model once per accepted
// candidate and bends every transformed vertex onto the terrain before this
// shader runs [orig: generate_foliage_instances_0 @ 0x5ffdd0].
uniform sampler2D u_fd_texture : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_fd_texture = false;
uniform sampler2D u_colormap : source_color, filter_linear, repeat_disable;
uniform bool u_has_colormap = false;
uniform sampler2D u_heightfield_normal : filter_linear, repeat_disable;
uniform bool u_has_heightfield_normal = false;
uniform sampler2D u_tile_overlay : source_color, filter_linear, repeat_enable;
uniform bool u_has_tile_overlay = false;
uniform vec3 u_tile_overlay_tint = vec3(1.0);
uniform sampler2DArray u_tile_cache : source_color, filter_linear, repeat_disable;
uniform bool u_has_tile_cache = false;
uniform vec3 u_emitter_color = vec3(0.5019608);
instance uniform float u_wind_phase = 0.0;
instance uniform float u_fade = 1.0;
instance uniform float u_alpha_ref = 0.0;
instance uniform bool u_instance_tile_cache_ready = false;
instance uniform float u_instance_tile_cache_layer = 0.0;
// XY is the page's world origin, Z is reciprocal world span, W is span.
instance uniform vec4 u_instance_tile_cache_origin_span = vec4(0.0);
// Nonzero only on the near LOW resubmission: retail selects strict
// D3DCMP_LESS there, so identical geometry only lands where the preceding
// HIGH draw rejected alpha and left no depth. Discarding texels the HIGH
// pass accepted reproduces that selection without a compare-op toggle.
// [orig: Foliage_SetupFarSlotDraw @ 0x6008fc..0x600912;
// CGfxDevice ZFUNC wrapper @ 0x67cac0..0x67caea]
instance uniform float u_high_pass_cutoff = 0.0;

varying vec2 v_tile_uv;
varying vec2 v_overlay_uv;
varying vec2 v_tile_cache_uv;

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
		(pre_wind_world_pos.xz - u_instance_tile_cache_origin_span.xy) *
		u_instance_tile_cache_origin_span.z;
	// Draw meshes are world-space and their pooled nodes are top-level identity.
	// Move VERTEX itself so native lighting and directional shadow passes see
	// the same wind-deformed surface as the color pass.
	VERTEX = world_pos;
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
		vec3 composed_rgb =
			clamp(raw_colormap * (256.0 / 255.0), vec3(0.0), vec3(1.0));
		if (u_has_tile_overlay) {
			vec4 overlay = texture(u_tile_overlay, v_overlay_uv);
			composed_rgb = mix(
				composed_rgb, overlay.rgb * u_tile_overlay_tint, overlay.a);
		}
		tile = vec4(composed_rgb, 1.0);
	}

	// Preserve the :fd, terrain-color, emitter-color, and fade semantics while
	// discarding the composed page's legacy lighting alpha. Native environment,
	// sun, local lights, fog, and shadows now realize the final appearance.
	vec3 color = clamp(fd.rgb * tile.rgb * u_emitter_color * 8.0,
		vec3(0.0), vec3(1.0));
	ALBEDO = color;
	METALLIC = 0.0;
	ROUGHNESS = 0.8;
	SPECULAR = 0.2;
	ALPHA = clamp(retail_alpha, 0.0, 1.0);
}
