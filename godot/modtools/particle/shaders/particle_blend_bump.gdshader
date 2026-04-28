shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → BlendMode::Bump (3).
// Engine vertex format (FVF 450) carries DIFFUSE = lit color and SPECULAR =
// raw modulated. The fixed-function texture stage combiner produces
// `texture × DIFFUSE × SPECULAR` (exact combiner mode unwitnessed; this
// shader assumes MODULATE-then-MODULATE = full multiplicative combine).
// Engine ref: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 second-color
// branch when particle.flags & 0x80 (LitColor).
//
// Our `CUSTOM0` carries the lit color (RGBA8 unorm) computed Godot-side
// from `def.bump_scale × (1/√3, 1/√3, 1/√3)` (engine's hardcoded light
// direction) encoded via `(value + 1) * 0.5`. Bounded deviation: we skip
// the engine's per-particle rotation transform, so the lit color is a
// uniform tint rather than direction-dependent — see
// notes/ida_particle_witness.md for the full RE.
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never, depth_test_disabled;

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
	// texture × COLOR (modulated) × CUSTOM0 (lit)
	ALBEDO = base.rgb * lit_color.rgb;
	ALPHA = base.a;
}
