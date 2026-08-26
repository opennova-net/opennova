shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Premult (2).
// Witnessed state [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380 +
// @ 0x5e85a9]: SrcBlend = ONE, DestBlend = INVSRCALPHA, fragment =
// MODULATE(TEXTURE, DIFFUSE) on color AND alpha. DIFFUSE alpha lands only in
// the fragment alpha (the destination attenuation) — retail does NOT scale the
// source RGB by it. `blend_premul_alpha` is Godot's ONE/INVSRCALPHA pair.
render_mode unshaded, cull_disabled, blend_premul_alpha, depth_draw_never;

#include "res://shaders/nova_viewmodel_pass.gdshaderinc"
#include "res://shaders/nova_color.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform bool has_texture = false;

void fragment() {
	vec4 base = COLOR;
	if (has_texture) {
		base *= texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		float radial = clamp(1.0 - r, 0.0, 1.0);
		// The diagnostic disc rides both channels of the premultiplied source.
		base.rgb *= radial;
		base.a *= radial;
	}
	ALBEDO = nova_scene_output(base.rgb);
	ALPHA = base.a;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (nova_viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see nova_viewmodel_pass.gdshaderinc).
	POSITION = nova_viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
