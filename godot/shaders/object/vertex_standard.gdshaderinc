// The sky-pass placement of a celestial body (Celestial stamps these onto
// its ObjectModels' materials; every other object keeps the defaults).
// Retail submits the sun/moon at THIS pass camera + direction * 64
// (Render_CelestialBodies @ 0x5acaa0, re-entered by the water mirror) and
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

// A model-space direction in world axes, unit length, or zero where a
// collapsed palette entry left none (shared.gdshaderinc obj_unit_or_zero).
vec3 obj_world_direction(mat4 model_matrix, vec3 direction) {
	return obj_unit_or_zero((model_matrix * vec4(direction, 0.0)).xyz);
}

void vertex() {
	#if defined(OBJ_SKIN_NORMAL_BLEND) || defined(OBJ_SKIN_NORMAL_FIRST_BONE)
	// A skinned effect's vertex program (skin.gdshaderinc): the bind-space
	// vertex through the model's bone palette, written back so every stage
	// below, and the depth and shadow passes, sees the posed vertex.
	ObjSkinVertex skin = obj_skin_vertex(VERTEX, NORMAL, TANGENT, BINORMAL,
			BONE_INDICES, BONE_WEIGHTS, CUSTOM0);
	VERTEX = skin.position;
	NORMAL = skin.normal;
	TANGENT = skin.tangent;
	BINORMAL = skin.binormal;
	#endif
	v_water_mirror_pass = obj_water_mirror_pass_flag(CAMERA_VISIBLE_LAYERS, IN_SHADOW_PASS);
	v_static_point_light_row = INSTANCE_CUSTOM.x;
	v_water_mirror_pass *= 1.0 + obj_water_clip_armed_flag(MODEL_MATRIX[3].xyz);
	v_raw_uv = UV;
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (u_sky_body) {
		v_world_pos += INV_VIEW_MATRIX[3].xyz - u_sky_anchor_camera;
	}
	v_fog_view_depth = -(VIEW_MATRIX * vec4(v_world_pos, 1.0)).z;
	v_world_normal = obj_world_direction(MODEL_MATRIX, NORMAL);
	v_geom_normal = v_world_normal;
	#ifdef OBJ_SKIN_NORMAL_FIRST_BONE
	// A lit skinned effect's Gouraud hemisphere reads CalcSkinWorldPosAndNormal's
	// normal, the first entry's own, apart from the light entry's frame.
	v_geom_normal = obj_world_direction(MODEL_MATRIX, skin.geometric_normal);
	#endif
	// CalcSelfShadowTerm dots In.Norm with the light in the light entry's frame.
	vec3 light_normal = v_world_normal;
	v_world_tangent = obj_world_direction(MODEL_MATRIX, TANGENT);
	v_world_binormal = obj_world_direction(MODEL_MATRIX, BINORMAL);
	// Point lights are measured from the vertex; a lit skinned effect
	// measures them from the vertex its light entry carries rigidly
	// (SkinModelLightArray[indexvector.x] - In.Pos, the entry's model space).
	vec3 point_light_origin = v_world_pos;
	#ifdef OBJ_SKIN_NORMAL_FIRST_BONE
	point_light_origin = (MODEL_MATRIX * vec4(skin.light_origin, 1.0)).xyz;
	v_skin_light_origin = point_light_origin;
	#endif
	#ifdef OBJ_NORMAL_FIRST_BONE_AXES
	// An object-space map resolves against the light entry's axes
	// (normal/object_uv1.gdshaderinc under OBJ_NORMAL_FIRST_BONE_AXES).
	mat3 light_axes = mat3(MODEL_MATRIX) * skin.light_frame;
	v_world_tangent = light_axes[0];
	v_world_binormal = light_axes[1];
	v_world_normal = light_axes[2];
	#endif
	// Each retail vertex program publishes exactly one EffectWorld point-light
	// product, and the wrapper's OBJ_VERTEX_POINT_LIGHTS_* contract names the
	// one its technique reads. The other varyings stay assigned (zero) so every
	// wrapper compiles, but their <=4-light loops (texelFetch rows on static
	// batches) never run for a technique that cannot observe them.
	#ifdef OBJ_VERTEX_POINT_LIGHTS_FFP_DIFFUSE
	// Vertex-rate like retail's D3D vertex lights (shared.gdshaderinc note).
	vec3 point_light_diffuse = obj_point_light_sum(v_world_pos, v_world_normal);
	#else
	vec3 point_light_diffuse = vec3(0.0);
	#endif
	#ifdef OBJ_VERTEX_FF_DIFFUSE
	// Fixed-function lighting is per vertex (shared.gdshaderinc
	// obj_ff_vertex_diffuse); the CLIP sum enables no point lights.
	v_ff_diffuse = obj_ff_vertex_diffuse(v_world_normal, point_light_diffuse);
	v_ff_clip_diffuse = obj_ff_vertex_diffuse(v_world_normal, vec3(0.0));
	#else
	v_ff_diffuse = vec3(0.0);
	v_ff_clip_diffuse = vec3(0.0);
	#endif
	// Pixel-shader NORMAL passes interpolate these oD0-equivalent factors;
	// their normal-map N.L/N.H terms remain fragment-rate.
	v_dir_self_shadow = obj_self_shadow(light_normal, -opennova_light_block_dir);
	#ifdef OBJ_VERTEX_POINT_LIGHTS_SELF_SHADOWED_ATTENUATION
	v_pixel_point_factor = obj_pixel_point_vertex_factors(
			point_light_origin, light_normal);
	#else
	v_pixel_point_factor = vec4(0.0);
	#endif
	#if defined(OBJ_VERTEX_POINT_LIGHTS_ATTENUATION) || defined(OBJ_CLIP_DOT3)
	v_pixel_point_attenuation = obj_pixel_point_vertex_attenuations(point_light_origin);
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
