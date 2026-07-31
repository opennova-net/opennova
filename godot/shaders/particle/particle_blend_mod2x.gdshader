shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Mod2x (5).
// Retail computes source = TEXTURE * DIFFUSE and blends it with
// SRC=DESTCOLOR, DST=SRCCOLOR, producing 2 * source * framebuffer. Godot has
// no separate-factor equivalent, so blend_mul with a doubled source preserves
// the same color equation (subject to the target framebuffer's normal clamp).
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
		// Source 0.5 is neutral after the 2x destination modulation.
		base.rgb = mix(vec3(0.5), base.rgb, mask);
	}
	ALBEDO = base.rgb * 2.0;
	ALPHA = 1.0;
}
