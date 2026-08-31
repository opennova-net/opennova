shader_type spatial;
// Sun/moon/star/glare bodies attached to the sky dome.
// [orig: render_celestial_bodies @ 0x5acaa0] places these at camera +
// direction * 64; the engine draws them after the dome and before the world,
// so the world overdraws them - DEPTH-TESTED (no write) is the structural
// equivalent here. We keep the 3DI diffuse texture and modulate by the TOD
// sun/moon color. See docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_mix, fog_disabled;

// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"

// Raw sampling per the gamma-space convention (color.gdshaderinc).
uniform sampler2D u_diffuse : filter_linear_mipmap;
uniform vec3 u_tint = vec3(1.0, 1.0, 1.0);
uniform float u_opacity = 1.0;
// CPU placement is anchored to the main camera. Each viewport supplies its
// own eye through INV_VIEW_MATRIX; the delta reproduces retail re-submission
// for the mirrored water pass without duplicating scene nodes.
uniform vec3 u_anchor_camera_world = vec3(0.0);

void vertex() {
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 pass_eye = INV_VIEW_MATRIX[3].xyz;
	world_pos += pass_eye - u_anchor_camera_world;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
	// Retail draws these in the sky bracket BEFORE the world, so ANY
	// terrain/world geometry overdraws the disc regardless of the 64 u
	// anchor distance [orig: render_celestial_bodies @ 0x5acaa0]. Godot's
	// transparent queue depth-tests at the anchor distance instead (a
	// mountain past 64 u would lose) - pin the quad to the exact far plane
	// (reverse-Z depth 0). An epsilon is NOT safe here: distant terrain
	// writes depth near/dist (a 560 u ridge is ~1e-4 at near 0.05), and any
	// epsilon puts the disc IN FRONT of everything past near/epsilon.
	POSITION.z = 0.0;
}

void fragment() {
	// Sky bodies draw in the beauty transparent list at the sky rung.
	vec4 tex = texture(u_diffuse, UV);
	// Gamma-space modulate; the terminal compositor runs only after this
	// alpha blend has completed in the retail gamma domain.
	ALBEDO = scene_output(tex.rgb * u_tint);
	ALPHA = tex.a * u_opacity;
}
