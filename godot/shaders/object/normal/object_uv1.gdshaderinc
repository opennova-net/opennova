uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;
uniform float u_normal_max_lod = 1000.0;

float obj_normal_alpha() {
	return obj_sample_effect_stage(u_normal_map, v_uv, u_normal_max_lod).a;
}

#ifdef OBJ_NORMAL_FIRST_BONE_AXES
// The skinned object-space bump effects (SkBDiffO, SkBDiffO2, SkBPhongO): the
// map holds bind-space normals, and the vertex program takes the light (and
// the half vector) into the model space of the vertex's light entry (its
// first palette entry, or the one whose inverse the fill kept;
// skin.gdshaderinc), times ObjSpaceFixup, before the pixel stage dots it
// with the sample (_vsSkDfO.fx vsObjSkinDot3DirPS / vsObjSkinDot3Dir,
// _vsSkPhO.fx vsObjSkinPhongDir: lightvec_model =
// SkinModelLightArray[indexvector.x], Out.Tex1 = lightvec_model *
// ObjSpaceFixup). In world space that is the entry's rotation of the
// fixed-up sample; the entry's axes arrive interpolated in the tangent-frame
// varyings (vertex_standard.gdshaderinc), as retail interpolates the
// entry-space light vector across the triangle. The skinned wrappers define
// OBJ_NORMAL_FIRST_BONE_AXES ahead of their includes.
vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = obj_sample_effect_stage(u_normal_map, v_uv, u_normal_max_lod).xyz * 2.0 - 1.0;
	sampled = normalize(vec3(-sampled.x, sampled.y, sampled.z));
	return normalize(sampled.x * v_world_tangent + sampled.y * v_world_binormal +
			sampled.z * v_world_normal);
}
#else
// Rigid: the fixed-up sample in the model's own space, carried to world
// space by the model matrix.
vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = obj_sample_effect_stage(u_normal_map, v_uv, u_normal_max_lod).xyz * 2.0 - 1.0;
	sampled = normalize(vec3(-sampled.x, sampled.y, sampled.z));
	return normalize((model_matrix * vec4(sampled, 0.0)).xyz);
}
#endif
