shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Bumpadd (6).
// Retail uses the same DOT3 + alpha-restore stages as Bump, then blends with
// SRCALPHA/ONE. The parser's substring order makes authored `bumpadd` resolve
// to Bump, but this exact program remains reachable through numeric type 6.
// Its intrinsic pass word is 0x10000 (retail
// CParticleTexture_InitTextureAndChannels @ 0x5E850A): SPECULARENABLE adds the
// SPECULAR colour (CUSTOM0) after the stages, and there is no FOGENABLE.
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;

#include "res://shaders/viewmodel_pass.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform bool has_texture = false;

// The SPECULAR dword, readable in the fragment stage only through a varying.
varying vec3 v_specular;

void fragment() {
	vec4 texel = vec4(0.5, 0.5, 1.0, 1.0);
	if (has_texture) {
		texel = texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		texel.a = clamp(1.0 - r, 0.0, 1.0);
	}
	float dotv = clamp(dot(texel.rgb * 2.0 - 1.0, COLOR.rgb * 2.0 - 1.0), 0.0, 1.0);
	ALBEDO = scene_output(vec3(dotv) + v_specular);
	ALPHA = texel.a * COLOR.a;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	v_specular = CUSTOM0.rgb;
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
