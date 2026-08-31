void vertex() {
	v_raw_uv = UV;
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_geom_normal = v_world_normal;
	v_world_tangent = normalize((MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz);
	v_world_binormal = normalize((MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz);
	// The first-person viewmodel takes retail's renderfov projection + depth
	// band (viewmodel_pass.gdshaderinc); everything else takes the plain
	// projection. Writing POSITION anywhere makes Godot override the clip
	// position for the whole shader, so EVERY path writes it (an unwritten
	// path would leave it zero and the object would vanish).
	POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
			MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
}
