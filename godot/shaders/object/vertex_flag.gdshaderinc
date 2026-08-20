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
	v_world_tangent = normalize((MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz);
	v_world_binormal = normalize((MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz);
	// Vertex-rate like retail's D3D vertex lights (shared.gdshaderinc note).
	v_point_light_diffuse = obj_point_light_sum(v_world_pos, v_world_normal);
}
