// Glass.fx TGlassFFP: TexCubeEnvironment x ReflectColor x gain, additive.
// Under ADR 0043 glass is a lit metal on an additive blend: the scene
// reflection source stands in for the retired sky-capture cube.
ObjSurface obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	ObjSurface s = obj_surface_defaults(base, normal);
	s.albedo = u_reflect_color.rgb *
			min(opennova_light_block_gain, vec3(1.0)) * 2.0;
	s.metallic = 1.0;
	s.roughness = 0.05;
	s.specular = 1.0;
	return s;
}
