shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Distort (7).
// Retail decodes the atlas texel as a normal and combines it with a rotating
// 1/51.2 perturbation and the particle's projective position before sampling
// the captured scene. SCREEN_UV is the Godot projective-position equivalent.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear, repeat_disable;
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
	vec3 normal = texel.rgb * 2.0 - 1.0;
	float theta = TIME * 4.0;
	float wave_x = sin(theta) * 0.01953125;
	float wave_y = cos(theta) * 0.01953125;
	float wave_z = -sin(theta) * 0.01953125;
	float alpha = COLOR.a;
	vec2 scene_uv;
	scene_uv.x = normal.x * (alpha * wave_x) + normal.y * (alpha * wave_y) + normal.z * SCREEN_UV.x;
	scene_uv.y = normal.x * (alpha * wave_y) + normal.y * (alpha * wave_z) + normal.z * SCREEN_UV.y;
	ALBEDO = texture(screen_tex, scene_uv).rgb;
	ALPHA = alpha * texel.a;
}
