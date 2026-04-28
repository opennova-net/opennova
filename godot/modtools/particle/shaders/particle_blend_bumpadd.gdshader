shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Bumpadd (6).
// Engine quirk: chained strstr matches `bump` before `bumpadd` so
// `bumpadd` resolves to BlendMode::Bump (3) at parse-time per
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0. We keep this shader for
// editor-authoring round-trips. Vertex format same as Bump (DIFFUSE +
// SPECULAR), but we use additive blend on the framebuffer (the engine's
// combiner mode for Bumpadd is unwitnessed; this shader approximates with
// `texture × COLOR + CUSTOM0`).
render_mode unshaded, cull_disabled, blend_add, depth_draw_never, depth_test_disabled;

uniform sampler2D albedo_tex : source_color, filter_linear, repeat_disable;
uniform bool has_texture = false;

varying vec4 lit_color;

void vertex() {
	lit_color = CUSTOM0;
}

void fragment() {
	vec4 base = COLOR;
	if (has_texture) {
		base *= texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		base.a *= clamp(1.0 - r, 0.0, 1.0);
	}
	// texture × COLOR + lit (additive combine on top of the additive
	// framebuffer blend = brighter overall vs Bump).
	ALBEDO = (base.rgb * base.a) + lit_color.rgb * 0.5;
	ALPHA = base.a;
}
