// [orig: Tracer.fx vsTracer] unlit texture x |dot(eye, normal)|².
void obj_evaluate_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	vec4 base; vec3 view_dir; vec3 normal; vec3 geom_normal;
	obj_prepare_surface(camera_position_world, model_matrix,
			base, view_dir, normal, geom_normal);
	float facing = abs(dot(geom_normal, view_dir));
	vec3 lit = base.rgb * (facing * facing);
	lit = obj_add_local_light(lit, base.rgb, normal);
	output_lit = obj_apply_fog(lit, camera_position_world);
	output_alpha = base.a;
}
