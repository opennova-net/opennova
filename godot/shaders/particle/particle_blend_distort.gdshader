shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Distort (7).
// Engine D3D state: samples scene texture with a UV offset driven by the
// distortion source (typically a heat-haze / refraction normal map). We
// approximate by sampling SCREEN_TEXTURE with a UV jitter from the per-quad
// UV channel; falls back to standard alpha when the screen texture is not
// available. Bounded Deviation: heat-haze magnitude differs from engine
// until the scene-texture distortion factor is witnessed.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

uniform sampler2D albedo_tex : source_color, filter_linear, repeat_disable;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear, repeat_disable;
uniform bool has_texture = false;
uniform float distort_strength : hint_range(0.0, 0.5) = 0.05;

void fragment() {
	vec4 base = COLOR;
	if (has_texture) {
		base *= texture(albedo_tex, UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		base.a *= clamp(1.0 - r, 0.0, 1.0);
	}
	vec2 offset = (UV - vec2(0.5)) * distort_strength * base.a;
	vec3 scene = texture(screen_tex, SCREEN_UV + offset).rgb;
	ALBEDO = mix(scene, base.rgb, base.a * 0.25);
	ALPHA = base.a;
}
