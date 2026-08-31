// SkGlass.fx TSkGlass: as glass, without the ColorSrcGlobalGain fold.
ObjSurface obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	ObjSurface s = obj_surface_defaults(base, normal);
	s.albedo = u_reflect_color.rgb * 2.0;
	s.metallic = 1.0;
	s.roughness = 0.05;
	s.specular = 1.0;
	return s;
}
