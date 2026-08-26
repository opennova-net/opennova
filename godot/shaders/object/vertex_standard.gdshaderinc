void vertex() {
	v_static_point_light_row = INSTANCE_CUSTOM.x;
	v_raw_uv = UV;
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_geom_normal = v_world_normal;
	v_world_tangent = normalize((MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz);
	v_world_binormal = normalize((MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz);
	// Each retail vertex program publishes exactly one EffectWorld point-light
	// product, and the wrapper's OBJ_VERTEX_POINT_LIGHTS_* contract names the
	// one its technique reads. The other varyings stay assigned (zero) so every
	// wrapper compiles, but their <=4-light loops (texelFetch rows on static
	// batches) never run for a technique that cannot observe them.
	#ifdef OBJ_VERTEX_POINT_LIGHTS_FFP_DIFFUSE
	// Vertex-rate like retail's D3D vertex lights (shared.gdshaderinc note).
	v_point_light_diffuse = obj_point_light_sum(v_world_pos, v_world_normal);
	#else
	v_point_light_diffuse = vec3(0.0);
	#endif
	// Pixel-shader NORMAL passes interpolate these oD0-equivalent factors;
	// their normal-map N.L/N.H terms remain fragment-rate.
	v_dir_self_shadow = obj_self_shadow(v_geom_normal, -u_dir_light_dir);
	#ifdef OBJ_VERTEX_POINT_LIGHTS_SELF_SHADOWED_ATTENUATION
	v_pixel_point_factor = obj_pixel_point_vertex_factors(
			v_world_pos, v_geom_normal);
	#else
	v_pixel_point_factor = vec4(0.0);
	#endif
	#ifdef OBJ_VERTEX_POINT_LIGHTS_ATTENUATION
	v_pixel_point_attenuation = obj_pixel_point_vertex_attenuations(v_world_pos);
	#else
	v_pixel_point_attenuation = vec4(0.0);
	#endif
	// The first-person viewmodel: retail's renderfov projection + depth band
	// (nova_viewmodel_pass.gdshaderinc). Never inside a shadow pass.
	if (u_viewmodel_pass && !IN_SHADOW_PASS) {
		POSITION = nova_viewmodel_clip(MODELVIEW_MATRIX * vec4(VERTEX, 1.0),
				PROJECTION_MATRIX);
	}
}
