shader_type spatial;
render_mode unshaded, cull_disabled, depth_prepass_alpha;

// RE anchors (corrected at REN-4 — the old "0x005BF064" pixel-shader anchor
// was bogus: that address is inside draw_death_screen_overlay, and
// "sub_5C1790" is not a function in the retail image; both came from a stale
// note. The witnessed foliage shader set, docs/terrain/terrain-re.md):
// - Terrain_GetModulatedColorAtPos @ 0x5C5FE0: base.rgb * light.rgb >> 7.
// - Foliage_CreateLightmapBlendPS @ 0x5ff7a0 — the hw fragment combine for
//   foliage/sector models: rgb = t0 x (t1 x (t1.a*c1 + c0)) x v0 x 8,
//   a = t0.a x v0.a, where t1 is the terrain lightmap sampled by a planar
//   world-position projection (VS oT1 = dp4(world, c7/c8)) and c0/c1 are the
//   lighting constants (values pinned at REN-5).
// - Terrain_CreateFoliageVertexShaders @ 0x5ff630 — the wind-sway VS
//   (polynomial sine of world-X phase, weight = vertex RED, Z displacement)
//   and the per-patch grid-placement VS.
// - Terrain_SetupSectorModelDraw @ 0x6007c0 — sector-model draw setup:
//   ALPHA-TEST REF 180 (high quality) / 8 (low); our u_alpha_cutoff 0.33 is a
//   tracked stand-in until the foliage draw paths are split by quality.
// - Terrain_InitSectors @ 0x601260 — the FF fallback stage tables (3-stage
//   TEMP-routed MODULATE4X / 2-stage MODULATE2X) + the 9-tap alpha smoothing
//   and gray-mip fill preprocessing of foliage textures.

uniform sampler2D u_albedo_texture : source_color, filter_linear_mipmap, repeat_enable;
uniform bool u_has_albedo_texture = false;
uniform sampler2D u_colormap : source_color, filter_linear, repeat_enable;
uniform float u_alpha_cutoff = 0.33;

global uniform vec3 opennova_fill_light;
global uniform vec3 opennova_sun_light;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;
global uniform float opennova_wind_sway_amount;
global uniform float opennova_wind_sway_phase;

varying vec4 v_ground_color;
varying vec3 v_world_pos;

vec3 terrain_light_modulation() {
	vec3 ambient = max(opennova_fill_light, vec3(0.0));
	vec3 diffuse = max(opennova_sun_light, vec3(0.0));
	vec3 combined = max(ambient * 0.70700002 + diffuse, vec3(1.0 / 255.0));
	return clamp((diffuse / combined) * (255.0 / 128.0), vec3(0.0), vec3(2.0));
}

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
	float height_control = clamp(VERTEX.y / 8.0, 0.0, 1.0);
	vec3 world_before_sway = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float phase = opennova_wind_sway_phase + world_before_sway.x * 0.11 + world_before_sway.z * 0.07;
	float sway = sin(phase) * opennova_wind_sway_amount * 0.04 * height_control;
	VERTEX.x += sway;

	v_ground_color = COLOR;
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec4 leaf = u_has_albedo_texture ? texture(u_albedo_texture, UV) : vec4(1.0);
	float alpha = leaf.a * v_ground_color.a;
	if (alpha < u_alpha_cutoff) {
		discard;
	}

	vec3 ground = clamp(v_ground_color.rgb, vec3(0.0), vec3(1.0));
	vec3 lit_ground = clamp(ground * terrain_light_modulation(), vec3(0.0), vec3(1.0));
	vec3 result = leaf.rgb * lit_ground;

	float fog_factor = fog_factor_for_distance(length(v_world_pos - CAMERA_POSITION_WORLD));
	result = mix(opennova_fog_color, result, fog_factor);

	ALBEDO = clamp(result, vec3(0.0), vec3(1.0));
	ALPHA = alpha;
}
