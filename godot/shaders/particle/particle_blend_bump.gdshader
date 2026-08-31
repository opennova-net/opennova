shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Bump (3).
// Retail stage 0 is DOTPRODUCT3(TEXTURE, DIFFUSE); stage 1 keeps that RGB and
// restores alpha as TEXTURE.a * DIFFUSE.a. The renderer puts the encoded bump
// light in COLOR/DIFFUSE and the original modulated color in CUSTOM0/SPECULAR;
// SPECULAR is intentionally not consumed by this fixed-function program.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, fog_disabled;

#include "res://shaders/viewmodel_pass.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform bool has_texture = false;

void fragment() {
	vec4 texel = vec4(0.5, 0.5, 1.0, 1.0);
	if (has_texture) {
		texel = texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		texel.a = clamp(1.0 - r, 0.0, 1.0);
	}
	// Stage 0 DOT3(TEXTURE, DIFFUSE); stage 1 restores alpha as T.a*D.a.
	// CUSTOM0 carries the retail SPECULAR dword but this combiner ignores it.
	float dotv = clamp(dot(texel.rgb * 2.0 - 1.0, COLOR.rgb * 2.0 - 1.0), 0.0, 1.0);
	ALBEDO = scene_output(vec3(dotv));
	ALPHA = texel.a * COLOR.a;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
