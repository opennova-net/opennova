shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Mod2x (5).
// Retail computes source = TEXTURE * DIFFUSE and blends it with
// SRC=DESTCOLOR, DST=SRCCOLOR, producing 2 * source * framebuffer. Godot has
// no separate-factor equivalent, so blend_mul with a doubled source preserves
// the same color equation (subject to the target framebuffer's normal clamp).
render_mode unshaded, cull_disabled, blend_mul, depth_draw_never;

#include "res://shaders/viewmodel_pass.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform bool has_texture = false;

void fragment() {
	vec4 base = COLOR;
	if (has_texture) {
		base *= texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		float mask = clamp(1.0 - r, 0.0, 1.0);
		// Source 0.5 is neutral after the 2x destination modulation.
		base.rgb = mix(vec3(0.5), base.rgb, mask);
	}
	ALBEDO = scene_output(base.rgb * 2.0);
	ALPHA = 1.0;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
