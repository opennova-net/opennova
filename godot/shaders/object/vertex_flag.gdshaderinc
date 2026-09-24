void vertex() {
	v_static_point_light_row = INSTANCE_CUSTOM.x;
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	vec3 local_pos = VERTEX;
	float angle = TIME * 8.0 + VERTEX.x * 3.0 + VERTEX.y * 0.8;
	float sa = sin(angle);
	float ca = cos(angle);
	local_pos.z += sa * min(VERTEX.x, 1.0) * 0.15;
	VERTEX = local_pos;
	v_world_pos = (MODEL_MATRIX * vec4(local_pos, 1.0)).xyz;
	vec3 animated_local_normal = normalize(vec3(ca * 0.5 + 0.5, -sa * 0.1, abs(sa)));
	vec3 animated_world_normal = normalize(
			(MODEL_MATRIX * vec4(animated_local_normal, 0.0)).xyz);
	vec3 original_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	if (dot(CAMERA_POSITION_WORLD - v_world_pos, original_world_normal) < 0.0) {
		animated_world_normal = -animated_world_normal;
	}
	v_world_normal = animated_world_normal;
	v_geom_normal = animated_world_normal;
	v_world_tangent = normalize((MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz);
	v_world_binormal = normalize((MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz);
	// Flag.fx has only its hemisphere + directional pass; EffectWorld point
	// lights do not participate in this effect.
	v_point_light_diffuse = vec3(0.0);
	// vsFlag's Out.Diff: hemisphere + directional at vertex rate, saturated
	// by the colour output. Flag.fx has no CLIP technique.
	v_ff_diffuse = obj_ff_vertex_diffuse(v_world_normal, vec3(0.0));
	v_ff_clip_diffuse = vec3(0.0);
	v_dir_self_shadow = 1.0;
	v_pixel_point_factor = vec4(0.0);
	v_pixel_point_attenuation = vec4(0.0);
	// The first-person viewmodel takes retail's renderfov projection + depth
	// band (viewmodel_pass.gdshaderinc); everything else takes the plain
	// projection. Writing POSITION anywhere makes Godot override the clip
	// position for the whole shader, so EVERY path writes it (an unwritten
	// path would leave it zero and the object would vanish).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
