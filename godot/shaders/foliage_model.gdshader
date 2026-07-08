shader_type spatial;
render_mode unshaded, cull_disabled;

// The foliage MODEL tier - full 3DI geometry stamped around sector entities,
// terrain-fitted per vertex by the witnessed grid-placement VS
// (docs/foliage/foliage-re.md §The model tier):
// - Foliage_GridPlacementVS (assembled inline in
//   Terrain_CreateFoliageVertexShaders @ 0x5ff630): normalized model XZ over
//   the bound square -> bilinear weights over the 4 rotated footprint
//   corners + the biquadratic sag fold (E_A, T_A, E_B, T_B) - exact at the
//   4 corners AND the 4 edge midpoints; final height += y_model * 0.5;
//   height-weighted wind on the world-Z axis only.
// - Foliage_UploadModelTileVSConstants @ 0x600f00: c9.x = sin(ctr*0.001)*0.08
//   (the wind term; the host advances the phase once per FRAME instead of
//   once per tile draw - cadence host mapping, see the dispatcher).
// - Terrain_RenderSectorEntitiesBySide @ 0x5c7d50: the alpha-test ref curve
//   clamp(int(4096/(dist+1)), 8, 128) from the anchor distance; host form:
//   the same curve evaluated in-shader from the per-INSTANCE center distance.
// - Foliage_DrawModelTileSlot @ 0x601d90: binds the def's ":fd" texture
//   (flat-0x808080 RGB, smoothed alpha [orig: Foliage_LoadDefAssets
//   @ 0x601260]) and alpha-tests (pass bit 0x40000, scissor).
//
// Host data layout (NovaFoliageDispatcher packs; derivation at
// _model_instance_transform): the instance TRANSFORM carries the placement
// (center + hbase + yaw + the witnessed 0.75-XZ / 0.5-height scale), COLOR
// carries the corner height DELTAS (shader corner order), INSTANCE_CUSTOM
// carries the sag fold with the mesh-axis T_A sign flip baked in.
//
// Fragment combine: the SAME witnessed foliage family as foliage.gdshader
// (rgb = t0 x (t1 x (t1.a*c1 + c0)) x v0 x 8 [orig:
// Foliage_CreateLightmapBlendPS @ 0x5ff7a0; constants
// terrain_setup_lighting_and_shader @ 0x604420]). The retail MODEL pass's
// own color chain (its pass object + oD0 = black constant) is NOT yet
// witnessed - D-FOLIAGE-6 (needs-RE); until then v0 is DERIVED IN-SHADER as
// the quad-emitter half-plus-bias form of the tinted colormap sample
// (0x40/255 + tinted_cm/2), the recorded host mapping.

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

uniform sampler2D u_fd_texture : filter_linear_mipmap, repeat_enable;
uniform bool u_has_fd_texture = false;
uniform sampler2D u_colormap : filter_linear, repeat_enable;
uniform bool u_has_colormap = false;
// The env terrain_rgb tint (FULL form ~ x255/128 saturating)
// [orig: sample_terrain_colormap_tinted @ 0x606030].
uniform vec3 u_terrain_tint = vec3(1.0);
// The bound square the VB normalization spans, in PRE-scale Godot mesh space
// [orig: Foliage_FillInstancedModelBuffers @ 0x5ffa20:
// pos.xz = (xz - center) / (2 * radius) + 0.5].
uniform vec2 u_bound_center = vec2(0.0);
uniform float u_bound_radius = 1.0;
// counter * 0.001 [orig: Foliage_ModelWindPhaseCounter @ 0x3162170].
uniform float u_model_wind_phase = 0.0;

global uniform vec3 opennova_sky_ambient;
global uniform vec3 opennova_sun_light;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;

varying vec3 v_world_pos;
varying float v_center_dist;

float fog_factor_for_distance(float dist) {
	float safe_end = max(opennova_fog_end, 1.0);
	if (opennova_fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}

	float start = opennova_fog_start;
	if (opennova_fog_type == 2) {
		start = safe_end * 0.5;
	} else if (opennova_fog_type == 3) {
		start = safe_end * 0.25;
	}
	float fog_range = max(safe_end - start, 1.0);
	return clamp((safe_end - dist) / fog_range, 0.0, 1.0);
}

void vertex() {
	// Normalized coords over the bound square, from the PRE-scale model
	// vertex - the exact VB form [orig: Foliage_FillInstancedModelBuffers
	// @ 0x5ffa20]. x_n/z_n here are the SHADER (Godot-mesh) axes; the host
	// packed COLOR/CUSTOM in this basis (corner permutation + T_A flip).
	float inv_span = 1.0 / (2.0 * max(u_bound_radius, 1e-6));
	float x_n = (VERTEX.x - u_bound_center.x) * inv_span + 0.5;
	float z_n = (VERTEX.z - u_bound_center.y) * inv_span + 0.5;

	// Bilinear corner weights + the biquadratic sag
	// [orig: Foliage_GridPlacementVS]. COLOR = corner height deltas
	// (h - hbase); INSTANCE_CUSTOM = (E_A, T_A, E_B, T_B) in shader axes.
	vec4 w = vec4((1.0 - x_n) * (1.0 - z_n), x_n * (1.0 - z_n),
	              (1.0 - x_n) * z_n, x_n * z_n);
	float ground_delta = dot(w, COLOR);
	float u = 2.0 * x_n - 1.0;
	float v = 2.0 * z_n - 1.0;
	ground_delta += (1.0 - v * v) * (INSTANCE_CUSTOM.x + u * INSTANCE_CUSTOM.y)
	              + (1.0 - u * u) * (INSTANCE_CUSTOM.z + v * INSTANCE_CUSTOM.w);

	// World placement: the instance transform carries hbase and the 0.75/0.5
	// scale; the fit contributes only the delta. Mirrors the retail VS shape
	// (compute world, displace, project).
	vec3 world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_pos.y += ground_delta;

	// Height-weighted wind on the WORLD-Z axis only [orig: renderZ (-worldZ)
	// += y_half * sin(ctr*0.001) * 0.08 in Foliage_GridPlacementVS via c9].
	float y_half = VERTEX.y * 0.5;
	world_pos.z -= y_half * sin(u_model_wind_phase) * 0.08;

	// The witnessed alpha-ref argument is the ANCHOR distance; host form:
	// the instance center distance (transform origin) - per-instance, not
	// per-fragment [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50].
	v_center_dist = length(MODEL_MATRIX[3].xyz - CAMERA_POSITION_WORLD);
	v_world_pos = world_pos;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_pos, 1.0);
}

void fragment() {
	vec4 fd = u_has_fd_texture ? texture(u_fd_texture, UV) : vec4(0.5, 0.5, 0.5, 1.0);

	// Alpha test (scissor): ref = clamp(int(4096 / (dist_units + 1)), 8, 128),
	// dist_units = integer world units; threshold = ref/255. Near instances
	// get a TIGHT silhouette (128), far ones a fat one (8, compensating mip
	// alpha erosion) [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50;
	// CGfxDevice_SetAlphaTestRef in Foliage_DrawModelTileSlot @ 0x601d90].
	float dist_units = floor(v_center_dist);
	float alpha_ref = clamp(floor(4096.0 / (dist_units + 1.0)), 8.0, 128.0);
	if (fd.a * 255.0 < alpha_ref) {
		discard;
	}

	// The witnessed foliage combine family (foliage.gdshader carries the full
	// anchor commentary): rgb = t0 x (t1 x (t1.a*c1 + c0)) x v0 x 8 - t1 =
	// the terrain colormap planar-projected at the fragment's world position
	// (uv = (x, -z)/texsize, wrap), c0 = the SKY block, c1 = the LIGHT block
	// [orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0; constants
	// terrain_setup_lighting_and_shader @ 0x604420]. No-terrain hosts get the
	// neutral mid-gray stand-in - retail never draws foliage without a
	// colormap.
	vec4 cm = vec4(0.5);
	if (u_has_colormap) {
		vec2 cm_uv = vec2(v_world_pos.x, -v_world_pos.z) / vec2(textureSize(u_colormap, 0));
		cm = texture(u_colormap, cm_uv);
	}
	vec3 lit = cm.rgb * (cm.a * opennova_sun_light + opennova_sky_ambient);

	// v0, derived in-shader: the retail model pass sets oD0 = black and its
	// pass-object color chain is unwitnessed (D-FOLIAGE-6, needs-RE); the
	// host applies the QUAD-emitter half-plus-bias form over the tinted
	// colormap sample - tinted = min(cm x tint x 255/128, 1) (the FULL-tint
	// sampler [orig: sample_terrain_colormap_tinted @ 0x606030]), v0 =
	// 0x40/255 + tinted/2, opaque [orig: the quad emitter's 0xFF000000 |
	// (0x404040 + (avg >> 1)) in generate_foliage_instances_0 @ 0x600197].
	vec3 tinted = min(cm.rgb * u_terrain_tint * (255.0 / 128.0), vec3(1.0));
	vec3 v0 = vec3(64.0 / 255.0) + tinted * 0.5;
	vec3 result = clamp(fd.rgb * lit * v0 * 8.0, vec3(0.0), vec3(1.0));

	float fog_factor = fog_factor_for_distance(length(v_world_pos - CAMERA_POSITION_WORLD));
	result = mix(opennova_fog_color, result, fog_factor);

	// Gamma-space output (D-RMAT-7): the combine ran on raw bytes like the
	// witnessed FF stages; encode once at the write. Alpha test passed ->
	// opaque write (D3D alpha-test semantics; no blend witnessed).
	ALBEDO = nova_gamma_to_linear(result);
}
