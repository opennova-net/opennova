uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;
uniform float u_normal_max_lod = 1000.0;

float obj_normal_alpha() {
	return obj_sample_aniso2(u_normal_map, v_raw_uv, u_normal_max_lod).a;
}

vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	// vsBumpEnvReflect feeds raw In.Tex0 into the normal sampler and scales
	// the basis Z vector by ReflectBumpDepth=4 before texm3x3vspec. Cubemap
	// direction lookup supplies the final normalization.
	// [orig: _vsBmEnv.fx; _BaseInc.fx ReflectBumpDepth].
	vec3 sampled = obj_sample_aniso2(u_normal_map, v_raw_uv, u_normal_max_lod).xyz * 2.0 - 1.0;
	return normalize(
		sampled.x * normalize(v_world_tangent) +
		sampled.y * normalize(v_world_binormal) +
		(sampled.z * 4.0) * geom_normal
	);
}
