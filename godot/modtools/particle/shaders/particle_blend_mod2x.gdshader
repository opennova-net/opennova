shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Mod2x (5).
// Engine D3D state: SrcBlend = DESTCOLOR, DestBlend = SRCCOLOR — modulate2x;
// Godot has no built-in render_mode for this, so we approximate by using
// blend_mul with the source pre-multiplied by 2.0. Result over-brightens by
// design (the engine effect is "darken-or-brighten depending on src around
// 0.5"); documented as a Bounded Deviation in particle_visual_parity.md.
render_mode unshaded, cull_disabled, blend_mul, depth_draw_never, depth_test_disabled, fog_disabled;

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
	vec3 doubled = clamp(base.rgb * 2.0, vec3(0.0), vec3(2.0));
	ALBEDO = mix(vec3(1.0), doubled, base.a);
	ALPHA = 1.0;
}
