shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Bumpadd (6).
// Retail uses the same DOT3 + alpha-restore stages as Bump, then blends with
// SRCALPHA/ONE. The parser's substring order makes authored `bumpadd` resolve
// to Bump, but this exact program remains reachable through numeric type 6.
render_mode unshaded, cull_disabled, blend_add, depth_draw_never;

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
	float dotv = clamp(dot(texel.rgb * 2.0 - 1.0, COLOR.rgb * 2.0 - 1.0), 0.0, 1.0);
	ALBEDO = vec3(dotv);
	ALPHA = texel.a * COLOR.a;
}
