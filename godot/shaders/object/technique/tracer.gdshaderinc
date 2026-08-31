// [orig: Tracer.fx vsTracer] unlit texture x |dot(eye, normal)|^2 -- an
// emissive island on the additive output.
ObjSurface obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	ObjSurface s = obj_surface_defaults(base, normal);
	float facing = abs(dot(geom_normal, view_dir));
	s.emission = base.rgb * (facing * facing);
	s.albedo = vec3(0.0);
	return s;
}
