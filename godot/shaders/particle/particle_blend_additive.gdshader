shader_type spatial;
// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 -> BlendMode::Additive (1).
// Witnessed state [orig: CParticleTexture_InitTextureAndChannels @ 0x5e8380 +
// @ 0x5e85a9]: SrcBlend = ONE, DestBlend = INVSRCALPHA, fragment =
// MODULATE(TEXTURE, DIFFUSE) on color AND alpha — and type-1 atlas pages ship
// alpha cleared to 0 [orig: BuildTextureAtlases @ 0x5e9116], so the source
// adds at full strength and DIFFUSE alpha never affects an additive layer.
// blend_premul_alpha is Godot's ONE/INVSRCALPHA pair; ALPHA = 0 reproduces the
// cleared-page fragment against this loose (un-cleared) texture.
render_mode unshaded, cull_disabled, blend_premul_alpha, depth_draw_never;

uniform sampler2D albedo_tex : source_color, filter_linear, repeat_disable;
uniform bool has_texture = false;

void fragment() {
	vec3 rgb = COLOR.rgb;
	if (has_texture) {
		rgb *= texture(albedo_tex, UV).rgb;
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		float mask = clamp(1.0 - r, 0.0, 1.0);
		// The diagnostic disc rides RGB; additive alpha is witnessed-dead.
		rgb *= mask;
	}
	ALBEDO = rgb;
	ALPHA = 0.0;
}
