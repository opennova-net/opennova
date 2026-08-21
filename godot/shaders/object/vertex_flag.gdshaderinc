uniform float u_wind_amount = 0.5;
uniform float u_wind_phase = 0.0;

void vertex() {
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	vec3 local_pos = VERTEX;
	float angle = TIME * 8.0 + u_wind_phase + VERTEX.x * 3.0 + VERTEX.y * 0.8;
	local_pos.z += sin(angle) * min(VERTEX.x, 1.0) * (0.15 * u_wind_amount);
	VERTEX = local_pos;
	v_world_pos = (MODEL_MATRIX * vec4(local_pos, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_geom_normal = v_world_normal;
	vec3 world_tangent = (MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz;
	vec3 world_binormal = (MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz;
	v_world_tangent = dot(world_tangent, world_tangent) < 0.000001
			? vec3(0.0) : normalize(world_tangent);
	v_world_binormal = dot(world_binormal, world_binormal) < 0.000001
			? vec3(0.0) : normalize(world_binormal);
}
