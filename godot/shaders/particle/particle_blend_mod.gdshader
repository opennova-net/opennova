shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Mod (4).
// Retail computes source = TEXTURE * DIFFUSE and blends it with
// SRC=DESTCOLOR, DST=ZERO, producing source * framebuffer.
render_mode unshaded, cull_disabled, blend_mul, depth_draw_never, fog_disabled;

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
		// White is the neutral value for destination multiplication.
		base.rgb = mix(vec3(1.0), base.rgb, mask);
	}
	ALBEDO = scene_output(base.rgb);
	ALPHA = 1.0;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
