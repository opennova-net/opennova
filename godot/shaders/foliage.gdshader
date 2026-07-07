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
//   lighting constants — PINNED at REN-5: c0 = the SKY block, c1 = the LIGHT
//   block (both [0] render colors /255), inherited from the terrain surface
//   push (no foliage-side write) [orig: terrain_setup_lighting_and_shader
//   @ 0x604420; init_terrain_lighting_color_ramps @ 0x604ee0 <-
//   Render_TerrainScene @ 0x610c80]. The lightmap-tile pass draws the sector
//   models back-to-front with a pure-ambient D3D light 4 (ambient =
//   detail-avg x (light*0.707 + sky) x 2, diffuse 0)
//   [orig: render_terrain_lightmaps @ 0x609de0].
// - Terrain_CreateFoliageVertexShaders @ 0x5ff630 — the wind-sway VS
//   (polynomial sine of world-X phase, weight = vertex RED, Z displacement)
//   and the per-patch grid-placement VS.
// - Terrain_SetupSectorModelDraw @ 0x6007c0 — sector-model draw setup:
//   ALPHA-TEST REF 180 (high quality) / 8 (low); our u_alpha_cutoff 0.33 is a
//   tracked stand-in until the foliage draw paths are split by quality.
// - Terrain_InitSectors @ 0x601260 — the FF fallback stage tables (3-stage
//   TEMP-routed MODULATE4X / 2-stage MODULATE2X) + the 9-tap alpha smoothing
//   and gray-mip fill preprocessing of foliage textures.

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

// Raw sampling per the gamma-space convention (nova_color.gdshaderinc).
uniform sampler2D u_albedo_texture : filter_linear_mipmap, repeat_enable;
uniform bool u_has_albedo_texture = false;
uniform sampler2D u_colormap : filter_linear, repeat_enable;
uniform bool u_has_colormap = false;
uniform float u_alpha_cutoff = 0.33;

global uniform vec3 opennova_sky_ambient;
global uniform vec3 opennova_sun_light;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;
global uniform float opennova_wind_sway_amount;
global uniform float opennova_wind_sway_phase;

varying vec4 v_ground_color;
varying vec3 v_world_pos;

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

	// The witnessed blend PS: rgb = t0 x (t1 x (t1.a*c1 + c0)) x v0 x 8 —
	// t1 = the terrain colormap planar-projected at the blade's world position
	// (1 world unit per texel, texel = (x, -z), wrap; the host CPU sampler
	// NovaTerrainData::get_colormap_color_world indexes identically), c0 = the
	// SKY block, c1 = the LIGHT block — the same constants the terrain surface
	// serves [orig: Foliage_CreateLightmapBlendPS @ 0x5ff7a0; constants
	// terrain_setup_lighting_and_shader @ 0x604420]. v0 carries the emitter's
	// half-plus-bias instance color (the dispatcher applies 0x404040 + avg>>1).
	// No-terrain hosts (u_has_colormap false) get a neutral mid-gray stand-in —
	// retail never draws foliage without a terrain colormap.
	vec4 cm = vec4(0.5);
	if (u_has_colormap) {
		vec2 cm_uv = vec2(v_world_pos.x, -v_world_pos.z) / vec2(textureSize(u_colormap, 0));
		cm = texture(u_colormap, cm_uv);
	}
	vec3 lit = cm.rgb * (cm.a * opennova_sun_light + opennova_sky_ambient);
	vec3 result = clamp(leaf.rgb * lit * v_ground_color.rgb * 8.0, vec3(0.0), vec3(1.0));

	float fog_factor = fog_factor_for_distance(length(v_world_pos - CAMERA_POSITION_WORLD));
	result = mix(opennova_fog_color, result, fog_factor);

	// Gamma-space output (D-RMAT-7): the combine above ran on raw bytes like
	// the witnessed FF stages; encode once at the write.
	ALBEDO = nova_gamma_to_linear(result);
	ALPHA = alpha;
}
