shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Additive (1).
// Engine D3D state: SrcBlend = SRCALPHA, DestBlend = ONE.
// The additive blend state applies the authored alpha; pre-multiplying RGB in
// the shader would apply alpha a second time and dim translucent particles.
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;

uniform sampler2D albedo_tex : source_color, filter_linear, repeat_disable;
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
	ALBEDO = base.rgb;
	ALPHA = base.a;
}
