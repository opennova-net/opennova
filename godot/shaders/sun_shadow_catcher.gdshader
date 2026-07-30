shader_type spatial;
render_mode blend_mix, depth_draw_never, cull_back, specular_disabled,
		ambient_light_disabled, fog_disabled;

// A second material pass over the hand-lit fixed-function surface. Godot's
// DirectionalLight3D contributes only its shadow attenuation to this black
// overlay; it never replaces or re-lights the base color.
uniform vec4 u_shadow_color : source_color = vec4(0.0, 0.0, 0.0, 0.58);

void fragment() {
	ALBEDO = u_shadow_color.rgb;
	ALPHA = 0.0;
	ROUGHNESS = 1.0;
}

void light() {
	if (LIGHT_IS_DIRECTIONAL) {
		DIFFUSE_LIGHT += vec3(0.0);
		ALPHA = max(ALPHA, (1.0 - ATTENUATION) * u_shadow_color.a);
	}
}
