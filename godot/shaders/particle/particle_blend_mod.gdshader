shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Mod (4).
// Engine D3D state: SrcBlend = ZERO, DestBlend = SRCCOLOR — multiplicative
// modulation. Unauthored / transparent pixels must read as `vec3(1.0)` so
// they don't darken the framebuffer where the soft-circle fallback is empty.
// We blend toward white via `mix(white, base, alpha)` before the multiply.
render_mode unshaded, cull_disabled, blend_mul, depth_draw_never;

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
	ALBEDO = mix(vec3(1.0), base.rgb, base.a);
	ALPHA = 1.0;
}
