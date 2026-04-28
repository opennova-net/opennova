shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Bump (3).
// Engine D3D state: a multi-stage texture combiner with a normal-map sample
// modulated against a directional light. We don't have a normal map in the
// portable preview, so we fall back to standard alpha blending — visually a
// soft Phong-less puff. Bounded Deviation: documented in
// notes/particle_visual_parity.md until the engine combiner is witnessed.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, depth_test_disabled;

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
