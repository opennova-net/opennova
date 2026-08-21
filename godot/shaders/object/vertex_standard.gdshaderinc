void vertex() {
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_geom_normal = v_world_normal;
	vec3 world_tangent = (MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz;
	vec3 world_binormal = (MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz;
	v_world_tangent = dot(world_tangent, world_tangent) < 0.000001
			? vec3(0.0) : normalize(world_tangent);
	v_world_binormal = dot(world_binormal, world_binormal) < 0.000001
			? vec3(0.0) : normalize(world_binormal);
}
