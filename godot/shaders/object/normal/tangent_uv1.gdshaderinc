uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;
uniform float u_normal_max_lod = 1000.0;

float obj_normal_alpha() {
	return obj_sample_effect_stage(u_normal_map, v_uv, u_normal_max_lod).a;
}

// The frame is the one the light is taken into: a lit skinned effect's
// light entry, whose normal can differ from the geometric normal its Gouraud
// hemisphere reads (a collapsed first palette entry, skin.gdshaderinc).
vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = obj_sample_effect_stage(u_normal_map, v_uv, u_normal_max_lod).xyz * 2.0 - 1.0;
	return normalize(
		sampled.x * normalize(v_world_tangent) +
		sampled.y * normalize(v_world_binormal) +
		sampled.z * normalize(v_world_normal)
	);
}
