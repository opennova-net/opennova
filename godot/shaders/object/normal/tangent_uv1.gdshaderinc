uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;

vec3 obj_resolve_surface_normal(mat4 model_matrix, vec3 geom_normal) {
	vec3 sampled = texture(u_normal_map, v_uv).xyz * 2.0 - 1.0;
	vec3 tangent = v_world_tangent;
	vec3 binormal = v_world_binormal;
	if (dot(tangent, tangent) < 0.000001 ||
			dot(binormal, binormal) < 0.000001) {
		return geom_normal;
	}
	tangent = tangent - geom_normal * dot(tangent, geom_normal);
	if (dot(tangent, tangent) < 0.000001) {
		return geom_normal;
	}
	tangent = normalize(tangent);
	vec3 derived_binormal = normalize(cross(geom_normal, tangent));
	if (dot(derived_binormal, binormal) < 0.0) {
		derived_binormal = -derived_binormal;
	}
	return normalize(
		sampled.x * tangent +
		sampled.y * derived_binormal +
		sampled.z * geom_normal
	);
}
