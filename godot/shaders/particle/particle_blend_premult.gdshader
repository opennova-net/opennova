shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Premult (2).
// Engine D3D state: SrcBlend = ONE, DestBlend = INVSRCALPHA — pre-multiplied
// alpha. Texture pixels are expected to be authored with RGB already
// multiplied by alpha; `blend_premul_alpha` matches this.
render_mode unshaded, cull_disabled, blend_premul_alpha, depth_draw_never;

uniform sampler2D albedo_tex : source_color, filter_linear, repeat_disable;
uniform bool has_texture = false;

void fragment() {
	vec4 base = COLOR;
	// Premult textures already carry texture alpha in RGB, but particle/curve
	// opacity lives in vertex COLOR.a and must also scale RGB because the blend
	// source factor is ONE.
	float opacity = COLOR.a;
	if (has_texture) {
		base *= texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		float radial = clamp(1.0 - r, 0.0, 1.0);
		base.a *= radial;
		opacity *= radial;
	}
	ALBEDO = base.rgb * opacity;
	ALPHA = base.a;
}
