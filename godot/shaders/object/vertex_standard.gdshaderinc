// The sky-pass placement of a celestial body (Celestial stamps these onto
// its ObjectModels' materials; every other object keeps the defaults).
// Retail submits the sun/moon at THIS pass camera + direction * 64
// (render_celestial_bodies @ 0x5acaa0, re-entered by the water mirror) and
// the glow/glint at the main camera; the discs ride the sky bracket before
// any world geometry, so they pin to the far plane and every nearer surface
// overdraws them, and each scene pass gates its own bracket
// (renderer/scene_pass_gates.h). The water layer tells the passes apart:
// the capture cameras cull to it alone, the mirror culls it out.
uniform bool u_sky_body = false;
uniform vec3 u_sky_anchor_camera = vec3(0.0);
uniform bool u_sky_far_pin = false;
uniform bool u_sky_beauty_drawn = true;
uniform bool u_sky_mirror_drawn = true;

bool obj_sky_pass_drawn(uint camera_layers) {
	const uint water_layer = 1024u; // visual_layers::WATER (0x00000400)
	if (camera_layers == water_layer) {
		return true;
	}
	return (camera_layers & water_layer) != 0u ? u_sky_beauty_drawn
			: u_sky_mirror_drawn;
}

void vertex() {
	v_water_mirror_pass = obj_water_mirror_pass_flag(CAMERA_VISIBLE_LAYERS, IN_SHADOW_PASS);
	v_static_point_light_row = INSTANCE_CUSTOM.x;
	v_raw_uv = UV;
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (u_sky_body) {
		v_world_pos += INV_VIEW_MATRIX[3].xyz - u_sky_anchor_camera;
	}
	v_fog_view_depth = -(VIEW_MATRIX * vec4(v_world_pos, 1.0)).z;
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
	#ifdef OBJ_VERTEX_FF_DIFFUSE
	// Fixed-function lighting is per vertex (shared.gdshaderinc
	// obj_ff_vertex_diffuse); the CLIP sum enables no point lights.
	v_ff_diffuse = obj_ff_vertex_diffuse(v_world_normal, v_point_light_diffuse);
	v_ff_clip_diffuse = obj_ff_vertex_diffuse(v_world_normal, vec3(0.0));
	#else
	v_ff_diffuse = vec3(0.0);
	v_ff_clip_diffuse = vec3(0.0);
	#endif
	// Pixel-shader NORMAL passes interpolate these oD0-equivalent factors;
	// their normal-map N.L/N.H terms remain fragment-rate.
	v_dir_self_shadow = obj_self_shadow(v_geom_normal, -opennova_light_block_dir);
	#ifdef OBJ_VERTEX_POINT_LIGHTS_SELF_SHADOWED_ATTENUATION
	v_pixel_point_factor = obj_pixel_point_vertex_factors(
			v_world_pos, v_geom_normal);
	#else
	v_pixel_point_factor = vec4(0.0);
	#endif
	#if defined(OBJ_VERTEX_POINT_LIGHTS_ATTENUATION) || defined(OBJ_CLIP_DOT3)
	v_pixel_point_attenuation = obj_pixel_point_vertex_attenuations(v_world_pos);
	#else
	v_pixel_point_attenuation = vec4(0.0);
	#endif
	// The first-person viewmodel takes retail's renderfov projection + depth
	// band (viewmodel_pass.gdshaderinc); everything else takes the plain
	// projection. Writing POSITION anywhere makes Godot override the clip
	// position for the whole shader, so EVERY path writes it (an unwritten
	// path would leave it zero and the object would vanish).
	if (u_sky_body) {
		POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(v_world_pos, 1.0);
		if (u_sky_far_pin) {
			// Exact far plane (reverse-Z 0): an epsilon fails because distant
			// terrain's depth near/dist drops below any epsilon.
			POSITION.z = 0.0;
		}
		if (!IN_SHADOW_PASS && !obj_sky_pass_drawn(CAMERA_VISIBLE_LAYERS)) {
			POSITION = vec4(2.0, 2.0, 2.0, 1.0);
		}
	} else {
		POSITION = viewmodel_position(u_viewmodel_pass && !IN_SHADOW_PASS,
				MODELVIEW_MATRIX * vec4(VERTEX, 1.0), PROJECTION_MATRIX);
	}
}
