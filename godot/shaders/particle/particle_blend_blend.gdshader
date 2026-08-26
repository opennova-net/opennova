shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Blend (0).
// Engine D3D state: SrcBlend = SRCALPHA, DestBlend = INVSRCALPHA — standard
// alpha blending.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

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
		base.a *= clamp(1.0 - r, 0.0, 1.0);
	}
	ALBEDO = nova_scene_output(base.rgb);
	ALPHA = base.a;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (nova_viewmodel_pass.gdshaderinc).
void vertex() {
	if (u_viewmodel_pass && !IN_SHADOW_PASS) {
		POSITION = nova_viewmodel_clip(MODELVIEW_MATRIX * vec4(VERTEX, 1.0),
				PROJECTION_MATRIX);
	}
}
