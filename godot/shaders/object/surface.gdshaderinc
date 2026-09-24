float obj_surface_coverage(vec4 base) {
	float coverage_alpha = base.a;
	#ifdef OBJ_COVERAGE_VERTEX_DIFFUSE_ALPHA
	coverage_alpha = v_dir_self_shadow;
	#endif
	#ifdef OBJ_COVERAGE_NORMAL_ALPHA
	coverage_alpha = obj_normal_alpha();
	#endif
	#ifdef OBJ_COVERAGE_ZERO
	coverage_alpha = 0.0;
	#endif
	#ifdef OBJ_COVERAGE_REFLECT_ALPHA
	coverage_alpha = u_reflect_color.a;
	#endif
	return coverage_alpha;
}

void obj_prepare_surface(
	vec3 camera_position_world,
	mat4 model_matrix,
	out vec4 base,
	out vec3 view_dir,
	out vec3 surface_normal,
	out vec3 geom_normal
) {
	obj_apply_water_reflection_clip(camera_position_world);
	base = obj_sample_base();
	float coverage_alpha = obj_surface_coverage(base);
	obj_apply_coverage(coverage_alpha);
	view_dir = normalize(camera_position_world - v_world_pos);
	geom_normal = normalize(v_geom_normal);
	surface_normal = obj_resolve_surface_normal(model_matrix, geom_normal);
}

// _FFP.fx TBoringFFPClip, drawn instead of NORMAL in the armed reflection
// view: stage 0 is Diffuse1 alone (the clip texture takes stage 1, so the
// _MT detail stage is gone) under MODULATE2X of the fixed-function diffuse
// without point lights (the technique carries no useffplights annotation)
// and without a SELFLUM block; alpha is Diffuse1.a x AlphaGenValue x the clip
// stage, tested by AMODE_CLIP's GREATER 128 in place of the material's own
// test. obj_apply_water_reflection_clip is the clip stage.
// retail CRenderBatchQueue_FlushBatches @ 0x5DA37F..0x5DA399 (AMODE_CLIP),
// @ 0x5DA5D4..0x5DA5DC (FF point lights only for useffplights techniques).
void obj_evaluate_ffp_clip_surface(vec3 camera_position_world,
		out vec3 output_lit, out float output_alpha) {
	obj_apply_water_reflection_clip(camera_position_world);
	vec4 base = obj_sample_aniso2(u_diffuse, v_uv, u_diffuse_max_lod);
	float alpha = base.a * u_alpha_mod;
	if (!(alpha > 128.0 / 255.0)) {
		discard;
	}
	output_lit = base.rgb * v_ff_clip_diffuse * 2.0;
	output_alpha = alpha;
}

#ifdef OBJ_CLIP_DOT3
// The fixed-function CLIP technique Dot3DiffT/Dot3DiffO/BDiffT2/PhongT share
// (TSegTanDiff_Clip / TSegObjDiff_clip; "clip for this shader will not
// support phong"), three passes:
//  P0  DOTPRODUCT3(normal map, vertex light vector) x TFACTOR DirLightColor,
//      plus the Gouraud hemisphere as the specular add, fogged to 0x7F7F7F
//      (FOGMODE_SHADERSET);
//  P1  per point light, DOTPRODUCT3(normal map, attenuated vector) x
//      PointLightColor, ONE/ONE, fogged to black (FOGMODE_SHADERADD);
//  P2  DESTCOLOR/SRCCOLOR post-multiply by Diffuse1 (MODULATE2X of the 0.5
//      vertex diffuse: 2 x Diffuse1 x framebuffer), fogged to the scene
//      colour (FOGMODE_SHADER).
// Only the clip stage feeds the alpha test; the material's own test is off.
// The fixed-function stages and the framebuffer clamp each pass to [0, 1].
// One P1 pass: its own framebuffer add, so each light clamps on its own.
vec3 obj_dot3_clip_point(vec3 normal, int light, float attenuation) {
	vec4 posr = obj_point_light_posr(light);
	float ndotl = clamp(dot(normal, normalize(posr.xyz - v_world_pos)) * attenuation,
			0.0, 1.0);
	return ndotl * clamp(obj_point_light_color(light).rgb, vec3(0.0), vec3(1.0));
}

void obj_evaluate_dot3_clip_surface(vec3 camera_position_world, mat4 model_matrix,
		out vec3 output_lit, out float output_alpha) {
	obj_apply_water_reflection_clip(camera_position_world);
	vec3 geom_normal = normalize(v_geom_normal);
	vec3 normal = obj_resolve_surface_normal(model_matrix, geom_normal);
	float visibility = obj_scene_fog_visibility(camera_position_world);
	// The unnormalised interpolated normal keeps the hemisphere exactly the
	// Gouraud value of the per-vertex HemicolorFromVectorY.
	vec3 hemisphere = clamp(obj_hemi(v_geom_normal), vec3(0.0), vec3(1.0));
	float ndotl = clamp(dot(normal, normalize(-opennova_light_block_dir)), 0.0, 1.0);
	vec3 pass0 = clamp(clamp(obj_dir_light_color(), vec3(0.0), vec3(1.0)) * ndotl +
			hemisphere, vec3(0.0), vec3(1.0));
	vec3 target = mix(vec3(127.0 / 255.0), pass0, visibility);
	vec3 points = vec3(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		points += obj_dot3_clip_point(normal, 0, v_pixel_point_attenuation.x);
	}
	if (count > 1.5) {
		points += obj_dot3_clip_point(normal, 1, v_pixel_point_attenuation.y);
	}
	if (count > 2.5) {
		points += obj_dot3_clip_point(normal, 2, v_pixel_point_attenuation.z);
	}
	if (count > 3.5) {
		points += obj_dot3_clip_point(normal, 3, v_pixel_point_attenuation.w);
	}
	target = clamp(target + points * visibility, vec3(0.0), vec3(1.0));
	vec3 diffuse = obj_sample_aniso2(u_diffuse, v_uv, u_diffuse_max_lod).rgb;
	vec3 source = opennova_fog_enabled
			? mix(opennova_fog_color, diffuse, visibility)
			: diffuse;
	output_lit = clamp(2.0 * source * target, vec3(0.0), vec3(1.0));
	output_alpha = 1.0;
}
#endif
