shader_type spatial;
// Retail's dome passes write no depth (sky.gdshaderinc carries the pass-flag
// witness); the far-plane pin keeps every world surface in front. The dome
// reaches the Godot world through the render-basis x/z swap, a mirror, so
// its inside faces the eye as front faces.
render_mode unshaded, cull_back, depth_draw_never;
// Retail's sky pass draws before the terrain and its slot drapes, so a drape
// edge that overhangs the terrain silhouette multiplies the sky, or the clear
// colour when the bracket is skipped (renderer::kRungSlotDrape carries the
// witness). The dome passes depth only where nothing opaque drew and marks
// those pixels for slot_shadow_drape.gdshader, as the terrain does.
stencil_mode write, compare_always, 1;

// Dome pass 1 (the sky gradient) and the advanced_clouds=0 flat dome; the
// shared vertex stage, the witness map and the pass gates live in
// sky.gdshaderinc, the cloud pass in sky_clouds.gdshader.
#define SKY_GATED_PASS_KEEPS_STENCIL
#include "res://shaders/sky.gdshaderinc"

uniform bool u_flat_pass = false; // advanced_clouds == 0
uniform vec3 u_flat_color = vec3(0.5, 0.5, 0.5); // cloud_rgb, flat path only


void fragment() {
	// advanced_clouds == 0: flat cloud-color dome, no texture
	// (retail Render_Skybox @ 0x579b42); otherwise the pass-1 gradient.
	vec3 col = u_flat_pass ? u_flat_color : v_pass1;
	// Gamma-space output (D-RMAT-7): the gradient/fog combine ran on raw byte
	// values; encode once so the displayed dome bytes equal the computed ones.
	ALBEDO = scene_output(sky_apply_fog(col));
	// A closed sky bracket leaves the clear colour (alpha 0 under the mix
	// blend) and keeps only the stencil mark; an open one replaces it.
	ALPHA = v_sky_pass_drawn;
}
