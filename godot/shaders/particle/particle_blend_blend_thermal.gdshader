shader_type spatial;
// BlendMode::Blend (0) in a thermal-view frame: the texture's secondary
// material, colour MODULATE(1 - TEXTURE, 1 - DIFFUSE), alpha
// MODULATE(TEXTURE, DIFFUSE), SRCALPHA/INVSRCALPHA (retail
// CParticleTexture_InitTextureAndChannels @ 0x5E8584..0x5E85DD, bound by
// CParticleBatch_FlushAndBindMaterial @ 0x5E42BF while the effect world's
// thermal word is set; renderer::particle_thermal_material).
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

#include "res://shaders/viewmodel_pass.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

#include "res://shaders/particle/particle_atlas.gdshaderinc"
uniform bool has_texture = false;

void fragment() {
	vec4 texel = vec4(1.0);
	if (has_texture) {
		texel = particle_atlas_texel(UV);
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		texel.a = clamp(1.0 - r, 0.0, 1.0);
	}
	ALBEDO = scene_output((1.0 - texel.rgb) * (1.0 - COLOR.rgb));
	ALPHA = texel.a * COLOR.a;
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
