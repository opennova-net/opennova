vec3 obj_apply_policy_fog(vec3 lit, vec3 camera_position_world) {
	#ifdef OBJ_CLIP_FOG_GRAY
	// TBoringFFPClip's BLEND_ALPHA pass declares FOGMODE_NORMALSET, whose
	// device fog colour is 0x7F7F7F. retail CD3DDevice_SetFogAndBlendMode
	// @ 0x6778E1..0x6778E6 (modes 1/5/9) -> D3DRS_FOGCOLOR @ 0x677928.
	if (obj_water_reflection_clip_view(camera_position_world)) {
		if (!opennova_fog_enabled) {
			return lit;
		}
		return mix(vec3(127.0 / 255.0), lit,
				obj_scene_fog_visibility(camera_position_world));
	}
	#endif
	#ifdef OBJ_CLIP_DOT3
	// The DOT3 CLIP body fogs each of its passes itself.
	if (obj_water_reflection_clip_view(camera_position_world)) {
		return lit;
	}
	#endif
	return obj_apply_fog(lit, camera_position_world);
}
