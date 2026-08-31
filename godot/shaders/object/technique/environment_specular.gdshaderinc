// EnvPhongT.fx TSegTanEnvPhong: the pow-8 Diffuse1.a lobe plus an
// additive reflection -- a glossy part-metal under the lit pipeline.
ObjSurface obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	ObjSurface s = obj_surface_defaults(base, normal);
	s.specular = clamp(base.a, 0.0, 1.0) * 0.5;
	s.roughness = 0.3;
	s.metallic = 0.4;
	return s;
}
