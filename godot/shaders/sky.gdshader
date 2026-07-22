shader_type spatial;
render_mode unshaded, cull_front;

// Sky-dome shader: structural port of the original two-pass dome combine
// [orig: render_skybox @ 0x579080], folded into one Godot pass:
//   pass 1 - textureless sky gradient, always sun-driven
//            [orig: SkyVS_GradientPassSource @ 0x7d7338]
//   pass 2 - both cloud layers in one multi-stage pass, driven by the ACTIVE
//            light (sun by day, moon at night), alpha-blended over pass 1
//            [orig: SkyVS_CloudPassSource @ 0x7d6fc0 + the 3-stage TSS table
//            decoded against RenderState_ApplyToDevice @ 0x681920]
// advanced_clouds=0 instead draws a flat cloud-color dome with the pass-1
// (NULL-texture) effect: material ambient = cloud_rgb against
// D3DRS_AMBIENT 0xFFFFFF [orig: render_skybox @ 0x579b42..0x579bb6].
// Full spec: docs/env/env-tod-re.md "Sky dome".

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

uniform vec3 u_sky_base = vec3(0.3, 0.4, 0.7); // c11 skybase
uniform vec3 u_sky_bright = vec3(0.5, 0.6, 0.9); // c11 + c12
uniform vec3 u_sky_highlight = vec3(1.0, 0.95, 0.8); // c15
uniform vec3 u_cloud_base = vec3(0.7, 0.75, 0.8); // c24
uniform vec3 u_cloud_highlight = vec3(0.6, 0.65, 0.7); // c27
uniform vec3 u_cloud_edge = vec3(0.3, 0.35, 0.4); // c26
uniform vec3 u_sun_dir = vec3(0.0, 1.0, 0.0); // pass-1 c13/c14 direction
uniform vec3 u_light_dir = vec3(0.0, 1.0, 0.0); // pass-2 c13/c14 direction
uniform vec3 u_fog_color = vec3(0.5, 0.7, 0.9); // sky-pass D3DRS_FOGCOLOR (skyfog)
uniform float u_fog_end = 1000.0; // world-unit fog end distance
uniform bool u_flat_pass = false; // advanced_clouds == 0
uniform vec3 u_flat_color = vec3(0.5, 0.5, 0.5); // cloud_rgb, flat path only

// The reference device applies anisotropic minification to active texture
// stages; this matters most at the dome's grazing-angle horizon.
uniform sampler2D u_cloud_tex1 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_cloud_tex2 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_clouds = false;
uniform vec2 u_scroll_offset1 = vec2(0.0);
uniform vec2 u_scroll_offset2 = vec2(0.0);
uniform float u_sky_height = 175.0;

varying vec3 v_pass1; // pass-1 oD0: gradient + sun-proximity highlight
varying vec3 v_cloud_ramp; // pass-2 oD0
varying vec3 v_sky_behind; // pass-2 oD1
varying float v_fog; // oFog = 1 - dist/(0.9*fogEnd)
varying vec2 v_uv1;
varying vec2 v_uv2;


// Godot's reverse-Z depth is the complement of D3D9's forward [0, 1] depth.
// Convert only the proximity vectors; POSITION must remain native Godot clip.
vec3 d3d_clip_xyz(vec4 clip) {
	return vec3(clip.xy, clip.w - clip.z);
}


// Clip-space proximity of this vertex to a direction: the original transforms
// (eye + dir*2000) through view-proj and dp3's the normalized D3D xyz of both
// [orig: SkyVS dp3 r5, r4, c14; c14 upload @ 0x57960e..0x579641].
float prox_to(vec4 clip, vec3 eye, vec3 dir, mat4 view_proj) {
	vec4 ref_clip = view_proj * vec4(eye + dir * 2000.0, 1.0);
	return max(dot(normalize(d3d_clip_xyz(clip)),
			normalize(d3d_clip_xyz(ref_clip))), 0.0);
}


void vertex() {
	// Y-only dome scale: the mesh bakes the 175.69-unit reference dome and only
	// the height stretches with sky_height; x/z stay at the 1024-unit rim
	// [orig: build_sky_dome_mesh @ 0x578ed4 - v14 = skyHeight / (3072 - sqrt(8388608))].
	// (epsilon floor: a hand-authored sky_height 0 must not NaN-poison the dome)
	float height_scale = max(u_sky_height, 0.01) / 175.69;
	vec3 scaled = vec3(VERTEX.x, VERTEX.y * height_scale, VERTEX.z);

	// Dome normal: the builder's anisotropic normal, NOT the vertex direction -
	// normalize(x, y_unscaled/scale, z) [orig: build_sky_dome_mesh @ 0x578fbb].
	// The dome never rotates, so the world normal equals the local normal.
	vec3 dome_normal = normalize(vec3(VERTEX.x, VERTEX.y / height_scale, VERTEX.z));

	vec3 eye = INV_VIEW_MATRIX[3].xyz;
	// Anchor from THIS render pass: retail re-enters render_skybox for the
	// mirrored water camera, so the reflection cannot reuse the main-camera
	// dome transform [orig: @ 0x5790d0; reflection pass @ 0x5c2780].
	vec3 world_pos = vec3(eye.x, eye.y * 0.5, eye.z) + scaled;
	mat4 view_proj = PROJECTION_MATRIX * VIEW_MATRIX;
	vec4 clip = view_proj * vec4(world_pos, 1.0);

	// Gradient driver t = 0.5*dot(N_world, dir) + 0.5; pass 1 always uses the
	// sun, pass 2 the active light [orig: SkyVS m3x3 v3,c4 / dp3 ..,c13 / mad].
	float t_sun = 0.5 * dot(dome_normal, u_sun_dir) + 0.5;
	float t_light = 0.5 * dot(dome_normal, u_light_dir) + 0.5;
	vec3 sky_sun = u_sky_base + (u_sky_bright - u_sky_base) * t_sun;
	vec3 sky_light = u_sky_base + (u_sky_bright - u_sky_base) * t_light;

	float prox_sun = prox_to(clip, eye, u_sun_dir, view_proj);
	float prox_light = prox_to(clip, eye, u_light_dir, view_proj);
	float p4_sun = prox_sun * prox_sun * prox_sun * prox_sun;
	float p8_sun = p4_sun * p4_sun;
	float p4_light = prox_light * prox_light * prox_light * prox_light;
	float p8_light = p4_light * p4_light;

	// vs_1_1 oD0/oD1 are color-register outputs: constants may exceed 1 after
	// the witnessed 2/255 upload, but the interpolated color inputs saturate.
	v_pass1 = clamp(mix(sky_sun, u_sky_highlight, p8_sun), vec3(0.0), vec3(1.0));
	v_sky_behind = clamp(mix(sky_light, u_cloud_edge, p4_light), vec3(0.0), vec3(1.0));
	v_cloud_ramp = clamp(mix(mix(u_cloud_base, u_cloud_edge, p4_light), u_cloud_highlight, p8_light), vec3(0.0), vec3(1.0));

	// Per-vertex dome fog [orig: SkyVS oFog = 1 - dist(world, eye)/(0.9*fogDist),
	// c9 upload @ 0x5792c2].
	// (divisor floor keeps the varying defined when fog is off; the fragment
	// guard never reads it in that case)
	v_fog = 1.0 - distance(world_pos, eye) / max(0.9 * u_fog_end, 0.001);

	v_uv1 = UV + u_scroll_offset1;
	v_uv2 = UV2 + u_scroll_offset2;
	POSITION = clip;
	VERTEX = scaled;
}


void fragment() {
	vec3 col;
	if (u_flat_pass) {
		// advanced_clouds == 0: flat cloud-color dome, no texture
		// [orig: render_skybox @ 0x579b42].
		col = u_flat_color;
	} else {
		col = v_pass1;
		if (u_has_clouds) {
			// 3-stage TSS cloud combine [orig: effect build @ 0x5789e0]:
			//   s1 color = tex0*tex1*2 (per-channel density, saturated)
			//   s2 color = LERP(density, cloudRamp, skyBehind)
			//   s1 alpha = tex0.a*tex1.a*2, s2 alpha = current^2*2 (each saturated)
			// blended over pass 1 with srcAlpha/invSrcAlpha.
			vec4 c0 = texture(u_cloud_tex1, v_uv1);
			vec4 c1 = texture(u_cloud_tex2, v_uv2);
			vec3 density = min(c0.rgb * c1.rgb * 2.0, vec3(1.0));
			vec3 cloud_rgb = density * v_cloud_ramp + (1.0 - density) * v_sky_behind;
			float a1 = min(c0.a * c1.a * 2.0, 1.0);
			float a2 = min(a1 * a1 * 2.0, 1.0);
			col = mix(col, cloud_rgb, a2);
		}
	}

	// The sky wrapper temporarily selects the post-horizon-blend, doubled
	// skyfog block, matching the clear below the open dome rim; world passes
	// restore ordinary fog afterward [orig: sky fog wrapper @ 0x579cb0].
	if (u_fog_end > 0.0) {
		col = mix(u_fog_color, col, clamp(v_fog, 0.0, 1.0));
	}
	// Gamma-space output (D-RMAT-7): the gradient/cloud/fog combine above ran
	// on raw byte values (colors and cloud texels alike); encode once so the
	// displayed dome bytes equal the computed ones.
	ALBEDO = nova_gamma_to_linear(col);
}
