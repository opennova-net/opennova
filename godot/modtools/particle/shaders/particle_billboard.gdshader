shader_type spatial;
// Camera-aligned billboard shader for the particle preview viewport.
// MVP uses additive blending — the IDA-faithful 8-mode shader (additive,
// blend, premult, bump, mod, mod2x, bumpadd, distort from
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0) lands in a follow-up plan.
render_mode unshaded, cull_disabled, blend_add, particle_trails;

uniform sampler2D albedo_tex : source_color, filter_linear, repeat_disable;
uniform bool has_texture = false;
uniform vec4 fallback_tint : source_color = vec4(1.0, 1.0, 1.0, 1.0);

void vertex() {
	// Camera-facing billboard. Using MODELVIEW_MATRIX rotation reset keeps the
	// quad parallel to the camera plane regardless of world orientation.
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(
			INV_VIEW_MATRIX[0],
			INV_VIEW_MATRIX[1],
			INV_VIEW_MATRIX[2],
			MODEL_MATRIX[3]);
	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);
}

void fragment() {
	// COLOR is the per-instance MultiMesh color (R,G,B,A 0..1 floats).
	// COLOR.a carries our particle alpha.
	vec4 base = COLOR;
	if (has_texture) {
		base *= texture(albedo_tex, UV);
	} else {
		// Soft radial falloff so MVP particles look like dots, not squares.
		vec2 centered = UV - vec2(0.5, 0.5);
		float r = length(centered) * 2.0;
		float falloff = clamp(1.0 - r, 0.0, 1.0);
		base.rgb *= fallback_tint.rgb;
		base.a *= falloff * fallback_tint.a;
	}
	ALBEDO = base.rgb;
	ALPHA = base.a;
}
