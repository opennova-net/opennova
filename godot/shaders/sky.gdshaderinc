// Sky-dome pass shared by the two retail dome passes over the same 441-vertex
// dome (retail render_skybox @ 0x579080):
//   pass 1 - textureless sky gradient, always sun-driven
//            (retail SkyVS_GradientPassSource @ 0x7d7338) -> sky.gdshader
//   pass 2 - both cloud layers in one multi-stage pass, driven by the ACTIVE
//            light (sun by day, moon at night), alpha-blended over pass 1
//            (retail SkyVS_CloudPassSource @ 0x7d6fc0 + the 3-stage TSS table
//            decoded against RenderState_ApplyToDevice @ 0x681920)
//            -> sky_clouds.gdshader
// Retail draws the sun/moon discs BETWEEN the two passes
// (render_celestial_bodies(0) @ 0x5798e0, flushed @ 0x5acce9, before the
// cloud-pass constants @ 0x5798f1), so the clouds veil the discs: the cloud
// pass is a separate transparent draw on the sky-cloud rung, after the
// sky-body rung (renderer/render_order).
// advanced_clouds=0 instead draws a flat cloud-color dome with the pass-1
// (NULL-texture) effect and no cloud pass: material ambient = cloud_rgb
// against D3DRS_AMBIENT 0xFFFFFF (retail render_skybox @ 0x579b42..0x579bb6).
// Full spec: docs/env/env-tod-re.md "Sky dome".

// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"

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
uniform vec2 u_scroll_offset1 = vec2(0.0);
uniform vec2 u_scroll_offset2 = vec2(0.0);
uniform float u_sky_height = 175.0;
// Which scene passes draw the dome this frame. The main frame draws its sky
// bracket only while blink letter 0x4 is clear and the eye is strictly above
// the water (retail Render_ProcessMainSceneFrame @ 0x5ca1a3..0x5ca1bd ->
// @ 0x5ca7c4); the water mirror draws its own while blink letter 0x2 is clear
// (retail render_main_scene @ 0x5c1342..0x5c1353 -> @ 0x5c166b). The
// environment-cube capture cameras always draw it.
uniform bool u_beauty_pass_drawn = true;
uniform bool u_mirror_pass_drawn = true;

varying vec3 v_pass1; // pass-1 oD0: gradient + sun-proximity highlight
varying vec3 v_cloud_ramp; // pass-2 oD0
varying vec3 v_sky_behind; // pass-2 oD1
varying float v_fog; // oFog = 1 - dist/(0.9*fogEnd)
varying vec2 v_uv1;
varying vec2 v_uv2;
#ifdef SKY_GATED_PASS_KEEPS_STENCIL
// 1 while this pass draws its sky bracket, 0 while the bracket is closed and
// the dome only marks the slot-drape stencil (sky.gdshader).
varying flat float v_sky_pass_drawn;
#endif

// The capture cameras cull to the environment-capture layer ALONE (the water
// layer's alias, render/visual_layers.h); the water mirror culls the water
// layer out; every beauty camera (and the weapon inset) admits it.
const uint SKY_WATER_LAYER = 1024u; // visual_layers::WATER (0x00000400)

bool sky_pass_drawn(uint camera_layers) {
	if (camera_layers == SKY_WATER_LAYER) {
		return true;
	}
	return (camera_layers & SKY_WATER_LAYER) != 0u ? u_beauty_pass_drawn
			: u_mirror_pass_drawn;
}


// Godot's reverse-Z depth is the complement of D3D9's forward [0, 1] depth.
// Convert only the proximity vectors; POSITION must remain native Godot clip.
vec3 d3d_clip_xyz(vec4 clip) {
	return vec3(clip.xy, clip.w - clip.z);
}


// Clip-space proximity of this vertex to a direction: the original transforms
// (eye + dir*2000) through view-proj and dp3's the normalized D3D xyz of both
// (retail SkyVS dp3 r5, r4, c14; c14 upload @ 0x57960e..0x579641).
float prox_to(vec4 clip, vec3 eye, vec3 dir, mat4 view_proj) {
	vec4 ref_clip = view_proj * vec4(eye + dir * 2000.0, 1.0);
	return max(dot(normalize(d3d_clip_xyz(clip)),
			normalize(d3d_clip_xyz(ref_clip))), 0.0);
}


void vertex() {
	// Y-only dome scale: the mesh bakes the 175.69-unit reference dome and only
	// the height stretches with sky_height; x/z stay at the 1024-unit rim
	// (retail build_sky_dome_mesh @ 0x578ed4 - v14 = skyHeight / (3072 - sqrt(8388608))).
	// (epsilon floor: a hand-authored sky_height 0 must not NaN-poison the dome)
	float height_scale = max(u_sky_height, 0.01) / 175.69;
	vec3 scaled = vec3(VERTEX.x, VERTEX.y * height_scale, VERTEX.z);

	// Dome normal: the builder's anisotropic normal, NOT the vertex direction -
	// normalize(x, y_unscaled/scale, z) (retail build_sky_dome_mesh @ 0x578fbb).
	// The dome never rotates, so the world normal equals the local normal.
	vec3 dome_normal = normalize(vec3(VERTEX.x, VERTEX.y / height_scale, VERTEX.z));

	vec3 eye = INV_VIEW_MATRIX[3].xyz;
	// Anchor from THIS render pass: retail re-enters render_skybox for the
	// mirrored water camera, so the reflection cannot reuse the main-camera
	// dome transform (retail @ 0x5790d0; reflection pass @ 0x5c2780).
	vec3 world_pos = vec3(eye.x, eye.y * 0.5, eye.z) + scaled;
	mat4 view_proj = PROJECTION_MATRIX * VIEW_MATRIX;
	vec4 clip = view_proj * vec4(world_pos, 1.0);

	// Gradient driver t = 0.5*dot(N_world, dir) + 0.5; pass 1 always uses the
	// sun, pass 2 the active light (retail SkyVS m3x3 v3,c4 / dp3 ..,c13 / mad).
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

	// Per-vertex dome fog (retail SkyVS oFog = 1 - dist(world, eye)/(0.9*fogDist),
	// c9 upload @ 0x5792c2). vs_1_1 oFog, like oD0/oD1, is saturated per
	// vertex before the rasterizer interpolates it (the D3D9 output-register
	// contract), so a triangle that straddles the fog end blends from 0, not
	// from a negative factor clamped per pixel.
	// (divisor floor keeps the varying defined when fog is off; the fragment
	// guard never reads it in that case)
	v_fog = clamp(1.0 - distance(world_pos, eye) / max(0.9 * u_fog_end, 0.001), 0.0, 1.0);

	v_uv1 = UV + u_scroll_offset1;
	v_uv2 = UV2 + u_scroll_offset2;
	POSITION = clip;
	// Both passes draw with z-write OFF and ZFUNC ALWAYS (pass flags
	// 0x300000 from every beauty/mirror caller, retail render_skybox
	// @ 0x579883 / @ 0x579ac1 / @ 0x579c3e, the bits decoded by
	// CGfxShader_ApplyPass @ 0x68322b..0x68324f) before any world geometry,
	// so the whole world overdraws the dome. Pin the dome to the exact far
	// plane (reverse-Z 0) and never write depth: every nearer surface wins
	// the depth test whatever the draw order, while the cleared sky passes.
	POSITION.z = 0.0;
#ifdef SKY_GATED_PASS_KEEPS_STENCIL
	v_sky_pass_drawn = 1.0;
#endif
	if (!IN_SHADOW_PASS && !sky_pass_drawn(CAMERA_VISIBLE_LAYERS)) {
#ifdef SKY_GATED_PASS_KEEPS_STENCIL
		// The gradient pass keeps rasterizing at alpha 0: the clear colour
		// stays, but its pixels still carry the slot-drape stencil mark, as
		// retail's drape multiplies whatever the frame holds there
		// (sky.gdshader).
		v_sky_pass_drawn = 0.0;
#else
		// This pass skips its sky bracket: park every vertex outside the
		// clip volume so no triangle rasterizes.
		POSITION = vec4(2.0, 2.0, 2.0, 1.0);
#endif
	}
	VERTEX = scaled;
}


// The sky wrapper temporarily selects the post-horizon-blend, doubled
// skyfog block, matching the clear below the open dome rim; world passes
// restore ordinary fog afterward (retail sky fog wrapper @ 0x579cb0).
vec3 sky_apply_fog(vec3 col) {
	if (u_fog_end > 0.0) {
		// v_fog interpolates per-vertex factors already in [0, 1].
		return mix(u_fog_color, col, v_fog);
	}
	return col;
}
