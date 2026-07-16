shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_always, fog_disabled;

#include "res://shaders/foliage_fd_sampling.gdshaderinc"

// The distant MODEL tier is an unfogged, unlit, color-invisible depth mask.
// Retail emits black RGB under SRC=ONE/DEST=ONE additive blending: accepted
// texels leave the destination color untouched while retaining their Z write.
// UV2.x carries the already-halved source height used by the grid-placement
// wind term. Alpha reference and wind phase are per-submission instance
// uniforms because resident geometry is reused across anchors and frames.
// [orig: Foliage_LoadDefAssets @ 0x6015b7;
//  GfxBlend_ApplyToDevice @ 0x681823;
//  Foliage_GridPlacementVS @ 0x5ff6a5;
//  Foliage_DrawModelTileSlot @ 0x601d90].
uniform sampler2D u_fd_texture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_fd_texture = false;
instance uniform float u_wind_phase = 0.0;
instance uniform float u_alpha_ref = 0.0;

void vertex() {
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_pos.z -= UV2.x * sin(u_wind_phase) * 0.08;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}

void fragment() {
	vec4 fd = u_has_fd_texture
		? sample_retail_foliage_fd(u_fd_texture, UV)
		: vec4(0.5, 0.5, 0.5, 1.0);
	if (fd.a <= u_alpha_ref) {
		discard;
	}
	ALBEDO = vec3(0.0);
}
