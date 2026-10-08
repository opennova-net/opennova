shader_type spatial;
// BlendMode::Additive (1) / Premult (2) in a thermal-view frame: the
// texture's secondary material, colour MODULATE(TEXTURE, DIFFUSE) under
// ZERO/INVSRCCOLOR, so the source darkens what is drawn (retail
// CParticleTexture_InitTextureAndChannels @ 0x5E8390..0x5E83A5, bound by
// CParticleBatch_FlushAndBindMaterial @ 0x5E42BF while the effect world's
// thermal word is set; renderer::particle_thermal_material). Godot's
// multiply blend (DST_COLOR/ZERO) over the complemented source is exactly
// dst x (1 - src).
render_mode unshaded, cull_disabled, blend_mul, depth_draw_never;

#include "res://shaders/viewmodel_pass.gdshaderinc"
#include "res://shaders/color.gdshaderinc"

#include "res://shaders/particle/particle_atlas.gdshaderinc"
uniform bool has_texture = false;

void fragment() {
	vec3 rgb = COLOR.rgb;
	if (has_texture) {
		rgb *= particle_atlas_texel(UV).rgb;
	} else {
		vec2 centered = UV - vec2(0.5);
		float r = length(centered) * 2.0;
		rgb *= clamp(1.0 - r, 0.0, 1.0);
	}
	ALBEDO = vec3(1.0) - scene_output(rgb);
}

// The first-person particle batch rides the viewmodel's renderfov projection
// + depth band inside the beauty pass (viewmodel_pass.gdshaderinc).
void vertex() {
	// Every path writes POSITION (see viewmodel_pass.gdshaderinc).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
