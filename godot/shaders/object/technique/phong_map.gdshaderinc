// _psPhong2.fx ps11Phong2: Diffuse1.a controlled both highlight
// brightness and the pow-4/pow-64 lobe blend of the retail PhongMap;
// the normal pass is opaque (Diffuse1.a is material data, not coverage).
ObjSurface obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	ObjSurface s = obj_surface_defaults(base, normal);
	float control = clamp(base.a, 0.0, 1.0);
	s.specular = control * 0.5;
	// The PhongMap lerped pow-4 -> pow-64 with control; map that exponent
	// ramp onto roughness.
	s.roughness = mix(0.55, 0.2, control);
	s.alpha = 1.0;
	return s;
}
