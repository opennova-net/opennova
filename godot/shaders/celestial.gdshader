shader_type spatial;
// Sun/moon/star/glare bodies attached to the sky dome.
// [orig: render_celestial_bodies @ 0x5acaa0] places these at camera +
// direction * 64; the engine draws them after the dome and before the world,
// so the world overdraws them - DEPTH-TESTED (no write) is the structural
// equivalent here. We keep the 3DI diffuse texture and modulate by the TOD
// sun/moon color. See docs/env/env-tod-re.md.
render_mode unshaded, depth_draw_never, cull_disabled, blend_mix;

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
// Which scene passes draw the sun/moon this frame: they ride the sky
// bracket, drawn by the main frame only while blink letter 0x4 is clear and
// the eye is strictly above the water, by the water mirror while letter 0x2
// is clear (renderer/scene_pass_gates.h carries the witnesses). The glare and
// glint keep the defaults: they are drawn outside the bracket.
uniform bool u_beauty_pass_drawn = true;
uniform bool u_mirror_pass_drawn = true;
const uint SKY_WATER_LAYER = 1024u; // visual_layers::WATER (0x00000400)

bool sky_pass_drawn(uint camera_layers) {
	if (camera_layers == SKY_WATER_LAYER) {
		return true; // the environment-capture cameras
	}
	return (camera_layers & SKY_WATER_LAYER) != 0u ? u_beauty_pass_drawn
			: u_mirror_pass_drawn;
}

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
	// Focused Q3 uses its typed source transform and resolved beauty depth;
	// this shader serves beauty only.
	POSITION.z = 0.0;
	if (!IN_SHADOW_PASS && !sky_pass_drawn(CAMERA_VISIBLE_LAYERS)) {
		POSITION = vec4(2.0, 2.0, 2.0, 1.0);
	}
}

void fragment() {
	// Sky bodies draw in the beauty transparent list at the sky rung.
	vec4 tex = texture(u_diffuse, UV);
	// Gamma-space modulate; the terminal compositor runs only after this
	// alpha blend has completed in the retail gamma domain.
	ALBEDO = scene_output(tex.rgb * u_tint);
	ALPHA = tex.a * u_opacity;
}
