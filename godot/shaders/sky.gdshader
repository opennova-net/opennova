shader_type spatial;
// Retail's dome passes write no depth (sky.gdshaderinc carries the pass-flag
// witness); the far-plane pin keeps every world surface in front.
render_mode unshaded, cull_front, depth_draw_never;

// Dome pass 1 (the sky gradient) and the advanced_clouds=0 flat dome; the
// shared vertex stage, the witness map and the pass gates live in
// sky.gdshaderinc, the cloud pass in sky_clouds.gdshader.
#include "res://shaders/sky.gdshaderinc"

uniform bool u_flat_pass = false; // advanced_clouds == 0
uniform vec3 u_flat_color = vec3(0.5, 0.5, 0.5); // cloud_rgb, flat path only


void fragment() {
	// advanced_clouds == 0: flat cloud-color dome, no texture
	// (retail render_skybox @ 0x579b42); otherwise the pass-1 gradient.
	vec3 col = u_flat_pass ? u_flat_color : v_pass1;
	// Gamma-space output (D-RMAT-7): the gradient/fog combine ran on raw byte
	// values; encode once so the displayed dome bytes equal the computed ones.
	ALBEDO = scene_output(sky_apply_fog(col));
}
