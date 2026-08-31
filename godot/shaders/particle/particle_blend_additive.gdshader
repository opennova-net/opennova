shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Additive (1).
// Witnessed state [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380 +
// @ 0x5e85a9]: SrcBlend = ONE, DestBlend = INVSRCALPHA, fragment =
// MODULATE(TEXTURE, DIFFUSE) on color AND alpha — and type-1 atlas pages ship
// alpha cleared to 0 [orig: BuildTextureAtlases @ 0x5e9116], so the source
// adds at full strength and DIFFUSE alpha never affects an additive layer.
// blend_premul_alpha is Godot's ONE/INVSRCALPHA pair; ALPHA = 0 reproduces the
// cleared-page fragment against this loose (un-cleared) texture.
render_mode unshaded, cull_disabled, blend_premul_alpha, depth_draw_never, fog_disabled;

#include "res://shaders/viewmodel_pass.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform bool has_texture = false;

void fragment() {
	vec3 rgb = COLOR.rgb;
	if (has_texture) {
		rgb *= texture(albedo_tex, UV).rgb;
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		float mask = clamp(1.0 - r, 0.0, 1.0);
		// The diagnostic disc rides RGB; additive alpha is witnessed-dead.
		rgb *= mask;
	}
	ALBEDO = scene_output(rgb);
	ALPHA = 0.0;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
