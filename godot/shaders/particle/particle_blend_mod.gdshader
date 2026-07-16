shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Mod (4).
// Retail computes source = TEXTURE * DIFFUSE and blends it with
// SRC=DESTCOLOR, DST=ZERO, producing source * framebuffer.
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
		float mask = clamp(1.0 - r, 0.0, 1.0);
		// White is the neutral value for destination multiplication.
		base.rgb = mix(vec3(1.0), base.rgb, mask);
	}
	ALBEDO = base.rgb;
	ALPHA = 1.0;
}
