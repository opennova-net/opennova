shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_always, fog_disabled;

#include "res://shaders/foliage_fd_sampling.gdshaderinc"

// The distant MODEL tier is an unfogged, unlit, color-invisible depth mask.
// Retail emits black RGB under SRC=ONE/DEST=ONE additive blending: accepted
// texels leave the destination color untouched while retaining their Z write.
// Each draw is one MultiMesh of the slot's normalized instanced VB (x/z over
// [0,1], y halved); every instance carries its GridPlacementVS constant block
// c[12+4i]..c[15+4i]: the rows of the instance transform hold render x (the
// Godot Z) of the four corners, their ground heights and render z (Godot X),
// the custom data the fold. Alpha reference and the c9.x wind term are
// per-submission instance uniforms.
// [orig: Foliage_LoadDefAssets @ 0x6015b7;
//  GfxBlend_ApplyToDevice @ 0x681823;
//  Terrain_CreateFoliageVertexShaders @ 0x5ff6a5 (the g_FoliageGridPlacementVS @ 0x2c25e60 source copy);
//  Foliage_DrawModelTileSlot @ 0x601d90].
// Retail GridPlacementVS literal @ 0x7de3a0; the blocks from
// Foliage_UploadModelTileVSConstants @ 0x6010e6..0x601209.
uniform sampler2D u_fd_texture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool u_has_fd_texture = false;
instance uniform float u_wind_offset = 0.0;
instance uniform float u_alpha_ref = 0.0;

void vertex() {
	// Row r of the instance's 3x4 transform is column-major MODEL_MATRIX's
	// r-th component across the four columns (the node transform is identity).
	vec4 render_x = vec4(MODEL_MATRIX[0][0], MODEL_MATRIX[1][0], MODEL_MATRIX[2][0], MODEL_MATRIX[3][0]);
	vec4 heights = vec4(MODEL_MATRIX[0][1], MODEL_MATRIX[1][1], MODEL_MATRIX[2][1], MODEL_MATRIX[3][1]);
	vec4 render_z = vec4(MODEL_MATRIX[0][2], MODEL_MATRIX[1][2], MODEL_MATRIX[2][2], MODEL_MATRIX[3][2]);
	vec4 fold = INSTANCE_CUSTOM;
	float x = VERTEX.x;
	float z = VERTEX.z;
	vec4 weights = vec4((1.0 - x) * (1.0 - z), x * (1.0 - z), (1.0 - x) * z, x * z);
	float u = x * 2.0 - 1.0;
	float v = z * 2.0 - 1.0;
	vec4 fold_basis = vec4(1.0 - v * v, (1.0 - v * v) * u, 1.0 - u * u, (1.0 - u * u) * v);
	vec3 world_pos;
	world_pos.x = dot(weights, render_z);
	world_pos.y = dot(weights, heights) + dot(fold_basis, fold) + VERTEX.y;
	// `mad r10, v0.y, c9, r10`: render x (Godot +Z) gains the halved source
	// height times c9.x = sin(counter * 0.001) * 0.08.
	world_pos.z = dot(weights, render_x) + VERTEX.y * u_wind_offset;
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
