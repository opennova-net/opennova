// Values and witnessed math shared by object techniques. Execution topology
// (coverage, texture stages, normal source, self-lighting, family) lives in
// statically selected includes; this file contains no capability switches.

#include "res://shaders/color.gdshaderinc"
#include "res://shaders/viewmodel_pass.gdshaderinc"

uniform vec3 u_uv_transform_u = vec3(1.0, 0.0, 0.0);
uniform vec3 u_uv_transform_v = vec3(0.0, 1.0, 0.0);

// Water reflection CLIP class. The reflected scene draws through the one
// camera whose mask omits the water layer (the mirror never draws the water
// surface, render/visual_layers.h), on both sides of the plane, and retail
// arms the clip there whatever side the eye is on: g_WaterMirrorActive = 1
// with the GSysClip texgen row u = y - wh + 0.5 under AlphaRef 0x80, i.e. keep
// y >= wh (retail Water_RenderReflectedWorldScene @ 0x5c8540..0x5c856a,
// Render_CreateSystemTextures @ 0x58acc0). It applies only to effects with an
// explicit CLIP block; missing blocks fall back to NORMAL and skinned
// submissions are skipped.
// [orig: Water_RenderReflectedWorldScene @ 0x5c8510;
// HLSLEffect CLIP fallback @ 0x5aebc8; bone skip @ 0x5d94bc;
// _FFP.fx TBoringFFPClip / Glass.fx TGlassFFP_clip].
global uniform bool opennova_water_active;
global uniform float opennova_water_height;
// The mirror pass's fog block (EnvironmentState::build_water_mirror_fog): the
// reflected scene applies the dry weather fog whatever the main pass selects.
global uniform vec3 opennova_water_mirror_fog_color;
// (start, end, type), the viewmodel block's packing.
global uniform vec3 opennova_water_mirror_fog_range;

// Highest-quality TexCubeEnvironment host. The captured texture contains the
// six gamma-byte sky + sun/moon faces after retail's 0x60 framebuffer multiply;
// the callback's final CubeRotSpecular sphere is analytic below so it follows
// the active light without baking an approximation into the resource.
global uniform samplerCube opennova_environment_cube;
global uniform bool opennova_environment_cube_ready;
// The raw light block the specular sphere is drawn with (never the terrain
// ramps' thermal pair); the sphere's alignment axis is the Godot-world
// opennova_light_block_dir below.
global uniform vec3 opennova_env_light_block;

// The PROJSHAD pass (the render-slot black silhouette whose alpha is
// Diffuse1.a, plus _FFP's optional Diffuse2.a and AlphaGenValue) is not a
// branch of these wrappers: SlotShadow's RenderingDevice pass
// (godot/src/render/slot_capture_adapter.cpp) draws it from the engine's
// per-technique coverage and blend tables, so every camera that renders an
// object wrapper renders its NORMAL technique.

// The pass-global world lighting block. Retail builds ONE 32-float constant
// block per scene pass from the five environment color blocks and stores it
// on the batch context; every drawn strip then reads that block at flush,
// scaled by the two per-entity factors below. Godot's global shader
// parameters are that per-pass constant block: MissionEnvironment writes
// them once per env change and no material is ever touched. Defaults
// (project.godot) are the shipped full_00.env noon register. The direction
// is -normalize(light dir) in Godot world axes (ctx+132), the negation of
// the diffuse "to light" vector.
// [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090 ->
// RenderBatchCtx_StoreLightingConstants @ 0x5d89e0 (ctx+116 light,
// +144 sky, +160 ground, +176 floor, +192 ceiling); the gain is
// Render_LightScaleRGB bound per draw by Material_ApplyShaderParameters @ 0x58db80]
global uniform bool opennova_thermal_view;
uniform bool u_thermal_wave_draw = true;
global uniform vec3 opennova_light_block_dir;
global uniform vec3 opennova_light_block_dir_color;
global uniform vec3 opennova_light_block_hemi_sky;
global uniform vec3 opennova_light_block_hemi_ground;
global uniform vec3 opennova_light_block_ceiling;
global uniform vec3 opennova_light_block_floor;
global uniform vec3 opennova_light_block_gain;
// The device fog state of the scene pass, shared with terrain, foliage and
// water: one Render_SetFogState per pass fogs every draw of that pass.
// [orig: Environment_ApplyFogAndAmbient @ 0x57e440 -> Render_SetFogState
// @ 0x58a950]
global uniform bool opennova_fog_enabled;
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;
// The per-entity lighting factors retail pushes per batch entry on top of the
// pass block: x = effectScale (entry[9], the sun-visibility factor),
// y = the interior-parented flag (entry flag bit 1), z = the parent
// interior's daylight openness t (entry[10]). Instance state stamped by the
// presenters on change; a static MultiMesh row reads the same four floats
// from its atlas row's last texel instead (obj_entity_light below; pool-2
// statics have no proximity slice, so effectScale 1).
// [orig: Render_SetupEntityLightingAndShaderConstants @ 0x5d98a0 (lerp branch
// @ 0x5d9a6a.., direct branch @ 0x5d9db5..)]
instance uniform vec4 u_entity_light = vec4(1.0, 0.0, 1.0, 0.0);
// ReflectColor keeps _BaseInc.fx's default unless a material routes a colour to it.
uniform vec4 u_reflect_color = vec4(0.75, 0.75, 0.75, 0.75);
// Runtime-generated byte-for-byte from retail's gsys_phong constructor.
// X=N.L, Y=N.H; RGBA=pow4,pow16,pow64,N.L with linear clamp sampling.
uniform sampler2D u_phong_map : filter_linear, repeat_disable;
// EffectWorld dynamic point lights: the <= 3 nearest group-passing pool
// instances for THIS model's draw context (the witness map lives on
// engine/runtime/renderer/light_scene.h — spawn/query/select and the D3D FF
// participation these uniforms stand in for). atten2 = 15/range^2; the rgb is
// the Light_GetPointLightParams colour (record x ambient x gen) the shader
// passes receive through PointLightColor / PointLightColorArray; the
// fixed-function helper alone adds the D3D fill's 1.5; range is the
// D3DLIGHT9.Range hard cutoff.
// Per-instance: each rendered model carries its own draw context's selected
// lights (retail selects per draw: Light_SelectAndEnableForDraw @0x5ab9d0,
// the batch collectors' group gate @0x5d91f8 / @0x5d96b8 and 3-cap, the flush
// arrays @0x5da6a8 and per-light params @0x5da8ae;
// docs/render/render-lighting-re.md). Count rides as float (instance uniforms
// exclude int); posr_N = xyz world + atten2, color_N = rgb + range.
// Static MultiMeshes cannot vary instance uniforms. INSTANCE_CUSTOM.x carries
// atlasRow+1 and selects the equivalent RGBAF payload; zero remains the live
// ObjectModel route. Both paths feed every technique helper below.
global uniform sampler2D opennova_static_point_light_rows;
instance uniform float u_point_light_count = 0.0;
instance uniform vec4 u_point_light_posr_0 = vec4(0.0);
instance uniform vec4 u_point_light_posr_1 = vec4(0.0);
instance uniform vec4 u_point_light_posr_2 = vec4(0.0);
instance uniform vec4 u_point_light_posr_3 = vec4(0.0);
instance uniform vec4 u_point_light_color_0 = vec4(0.0);
instance uniform vec4 u_point_light_color_1 = vec4(0.0);
instance uniform vec4 u_point_light_color_2 = vec4(0.0);
instance uniform vec4 u_point_light_color_3 = vec4(0.0);

// MATCHTERRAIN is a skinned crouch/prone pre-pass in retail. The reimpl folds
// that pass into the object's NORMAL draw: only pixels rejected by NORMAL's
// alpha test retain the terrain color, so accepted pixels are byte-equivalent
// to the retail NORMAL overwrite. The composed terrain page is the same cache
// sampled by Terrain and foliage. Per-model material ownership makes the array
// binding safe; per-instance state carries the live stance and resident page.
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50, entity+300 & 0x300;
// _tSkFolg.fx Simple_MatchTerrain_PS; _vsSkFolg.fx vscSkMatchTerrain;
// TerrainTile_CacheLookup @ 0x604140].
uniform sampler2DArray u_match_terrain_cache : filter_linear, repeat_disable;
uniform bool u_has_match_terrain_cache = false;
instance uniform bool u_match_terrain_enabled = false;
instance uniform bool u_match_terrain_page_ready = false;
instance uniform float u_match_terrain_page_layer = 0.0;
// xy = page world origin, z = reciprocal world span, w = world span.
instance uniform vec4 u_match_terrain_page_projection = vec4(0.0);
// LGHT preview is explicit editor-only state; gameplay binds count zero. The
// GAMEPLAY consumption of model lights is the EffectWorld pool above (spawned
// per placed entity at mission start), not these per-material uniforms.
uniform int u_local_light_count = 0;
uniform vec3 u_local_light_position = vec3(0.0);
uniform vec3 u_local_light_color = vec3(1.0);
uniform float u_local_light_intensity = 1.0;
uniform float u_local_light_atten_start = 0.0;
uniform float u_local_light_atten_end = 5.0;

varying vec2 v_uv;
varying vec2 v_uv2;
// Raw TEXCOORD0 is distinct: several non-#UV retail vertex programs bypass
// CalcAnimatedUV even when another pass of the same material uses Diffuse1.
varying vec2 v_raw_uv;
varying vec3 v_world_pos;
// The pass camera's view depth of this vertex (clip w): the planar fog
// distance (the vertex stage writes it; obj_scene_fog_visibility reads it).
varying float v_fog_view_depth;
varying vec3 v_world_normal;
varying vec3 v_geom_normal;
varying vec3 v_world_tangent;
varying vec3 v_world_binormal;
// The saturated fixed-function vertex diffuse (hemisphere + directional +
// the enabled point lights) Gouraud-interpolated into MODULATE2X, and the
// same sum without point lights for the CLIP technique, which enables none.
// The point lights are EffectWorld's, evaluated at VERTEX rate: the entry's
// <= 3 re-enabled pool lights join the D3D vertex diffuse (summed in the
// vertex stage, vertex_standard.gdshaderinc) and Gouraud-interpolate across
// the triangle. Pixel-shader DOT3/Phong effects instead consume the same pool
// below in their authored mapped-normal point passes.
// [orig: the per-frame SetLight upload @ 0x5ca6a1 -> Light_FillD3DPointLight
// @ 0x5aa450; the flush's disable-all @ 0x5da5de and Light_ApplyAsD3DLight
// @ 0x5da61a; Dot3DiffT/PhongT NORMAL P1 PASSRULE_ONCE_PER_POINTLIGHT].
varying vec3 v_ff_diffuse;
varying vec3 v_ff_clip_diffuse;
// Pixel-shader effect programs put their geometric self-shadow and point
// attenuation in oD0 in the vertex shader, then interpolate it into the
// mapped-normal pixel pass. Keep those factors separate from the FFP sum.
varying float v_dir_self_shadow;
varying vec4 v_pixel_point_factor;
varying vec4 v_pixel_point_attenuation;
// Identical for every vertex of a MultiMesh instance, so ordinary varying
// interpolation remains constant across each primitive.
varying float v_static_point_light_row;
// The water mirror pass: 0 in every other view, 1 in the mirror (the one
// non-shadow camera whose mask omits the water layer, render/visual_layers.h),
// 2 in the mirror for a draw whose CLIP technique the reflected pass arms
// (retail's per-draw test, runtime/environment/water_mirror.h
// water_mirror_clip_armed: a sector model whose bottom sits below wh - 0.25,
// a first-wave entity whose z - boundRadius sits below wh, never a BySide
// person). A static row carries the arming test as an origin offset in its
// lighting lane's w; every other model the armed bit (2) its ObjectModel
// stamps into u_entity_light.w. Every object vertex() writes it through
// obj_water_mirror_pass_flag and obj_water_clip_armed_flag (one varying: the
// object stages sit at the device's varying limit); built-ins stay out of
// helpers.
varying flat float v_water_mirror_pass;

// A direction at unit length, or zero where it has none. A lit skinned
// effect's vertex whose first palette entry collapsed (the right hand's
// zero-scale row, world §14.1.5) keeps a zero geometric normal, which the
// Gouraud hemisphere reads as level (skin.gdshaderinc); normalize would
// hand the lighting a NaN instead.
vec3 obj_unit_or_zero(vec3 v) {
	float length_squared = dot(v, v);
	return length_squared > 0.0 ? v * inversesqrt(length_squared) : v;
}

// Where the point-light vectors start. The vertex, for every unskinned effect
// and for SkBasic, whose vsSkinBasic measures PointLightCoordArray from the
// blended position; the lit skinned effects measure theirs from the vertex
// their light entry (skin.gdshaderinc: the first palette entry, or the one
// whose inverse the fill kept) carries rigidly: SkinModelLightArray
// [indexvector.x] is the light in that entry's model space (the inverse of
// its matrix applied to PointLightCoord), and the vertex programs subtract
// the undeformed In.Pos.
// [orig: CRenderBatchQueue_FlushBatches @ 0x5DA950..0x5DA9A1 (the entry
// fill), _vsSkDfT.fx vsTanSkinDot3PointPS, _vsSkPhT.fx vsTanSkinPhongPoint,
// _vsSkDfO.fx vsObjSkinDot3PointPS, _vsSkPhO.fx vsObjSkinPhongPoint]
#ifdef OBJ_SKIN_NORMAL_FIRST_BONE
varying vec3 v_skin_light_origin;

vec3 obj_point_light_origin() {
	return v_skin_light_origin;
}
#else
vec3 obj_point_light_origin() {
	return v_world_pos;
}
#endif

float obj_water_mirror_pass_flag(uint camera_layers, bool in_shadow_pass) {
	return (!in_shadow_pass && (camera_layers & 1024u) == 0u) ? 1.0 : 0.0;
}

// The CLIP-class discard must stay in the one material Godot gives a mesh
// (no per-camera material variant exists), so the depth prepass of every
// explicit-CLIP wrapper keeps this discard; the mirror-pass flag keeps every
// other camera unclipped. Nothing samples a texture before this runs.
// Whether this draw is the armed reflection view: the view whose explicit
// CLIP blocks replace NORMAL. Retail arms it in the reflected pass on both
// sides of the plane whenever the water is live, for the draws its per-draw
// test selects (v_water_mirror_pass 2).
bool obj_water_reflection_clip_view(vec3 camera_position_world) {
	return v_water_mirror_pass > 1.5 && opennova_water_active;
}

bool obj_water_reflection_clips(vec3 camera_position_world) {
	return obj_water_reflection_clip_view(camera_position_world) &&
			v_world_pos.y < opennova_water_height;
}

void obj_apply_water_reflection_clip(vec3 camera_position_world) {
	#ifdef OBJ_CLIP_EXPLICIT
	if (obj_water_reflection_clips(camera_position_world)) {
		discard;
	}
	#endif
}

// Full D3D COUNT2 affine transform. Coefficient rows preserve SET, shear,
// scroll, scale, and center rotation. [orig: Material_ComputeUVTransformMatrix @ 0x5B1990]
vec2 obj_transform_uv(vec2 uv) {
	vec3 uv1 = vec3(uv, 1.0);
	return vec2(dot(uv1, u_uv_transform_u), dot(uv1, u_uv_transform_v));
}

// sampLinearWrap2D at retail's highest texture-filter tier: every object
// effect compiles with ANISO, a MaxAnisotropy 2 ANISOTROPIC minifier with
// LINEAR mip and magnification filters. The project's anisotropy level is
// global and set for terrain, so the object sampler applies its own 2x
// footprint: the level of detail follows the pixel footprint's major axis
// shortened by the anisotropy ratio (capped at 2), and two trilinear taps
// cover the major axis. retail Render_InitAllSubsystems @ 0x586413..0x586427
// (filter tier -> mode 2), HLSLEffect_LoadFromFile @ 0x5AE6C5..0x5AE6D8
// ("ANISO" define), _BaseInc.fx sampLinearWrap2D. max_lod is the texture's
// last retail mip level (texture_path_resolver material_texture_max_lod):
// pixel-built textures stop at the 4x4 level, D3D clamps there.
vec4 obj_sample_aniso2(sampler2D tex, vec2 uv, float max_lod) {
	vec2 size = vec2(textureSize(tex, 0));
	vec2 du = dFdx(uv);
	vec2 dv = dFdy(uv);
	float length_x = length(du * size);
	float length_y = length(dv * size);
	float major = max(length_x, length_y);
	float minor = min(length_x, length_y);
	float ratio = clamp(major / max(minor, 1.0e-8), 1.0, 2.0);
	float lod = min(log2(max(major / ratio, 1.0e-8)), max_lod);
	vec2 major_axis = length_x >= length_y ? du : dv;
	vec2 offset = major_axis * (0.5 - 0.5 / ratio);
	return 0.5 * (textureLod(tex, uv - offset, lod) +
			textureLod(tex, uv + offset, lod));
}

// The per-entry constants: the pass block scaled by effectScale, and under
// the interior flag additionally by t with the hemisphere pair lerped from
// the indoor (floor/ceiling) to the outdoor (ground/sky) block by t. This is
// renderer::compute_entity_lighting evaluated at draw time.
// [orig: Render_SetupEntityLightingAndShaderConstants @ 0x5d98a0]
bool obj_uses_static_point_light_row() {
	return v_static_point_light_row > 0.5;
}

int obj_static_point_light_row() {
	return max(int(floor(v_static_point_light_row + 0.5)) - 1, 0);
}

// The per-entry lighting state: the instance lane, or a static MultiMesh
// row's atlas lane (texel 9) -- the only per-instance channel a batch has.
vec4 obj_entity_light() {
	if (obj_uses_static_point_light_row()) {
		return texelFetch(opennova_static_point_light_rows,
				ivec2(9, obj_static_point_light_row()), 0);
	}
	return u_entity_light;
}

bool obj_entity_interior() {
	return obj_entity_light().y > 0.5;
}

// The BySide person wave: bit 1 of u_entity_light.w (bit 2 is the mirror CLIP
// arming, v_water_mirror_pass 2). A static row is never a person; its lane's w is
// the mirror clip offset instead.
bool obj_thermal_wave() {
	return opennova_thermal_view && !obj_uses_static_point_light_row() &&
			mod(u_entity_light.w, 2.0) > 0.5 && u_thermal_wave_draw;
}

float obj_water_clip_armed_flag(vec3 model_origin) {
	if (obj_uses_static_point_light_row()) {
		return model_origin.y + obj_entity_light().w < opennova_water_height ? 1.0 : 0.0;
	}
	return u_entity_light.w >= 1.5 ? 1.0 : 0.0;
}

vec3 obj_dir_light_color() {
	if (obj_thermal_wave()) return vec3(0.0);
	vec4 entity = obj_entity_light();
	vec3 scaled = opennova_light_block_dir_color * entity.x;
	return obj_entity_interior() ? scaled * entity.z : scaled;
}

vec3 obj_hemi_sky_color() {
	if (obj_thermal_wave()) return vec3(0.25);
	return obj_entity_interior()
			? mix(opennova_light_block_ceiling, opennova_light_block_hemi_sky,
					obj_entity_light().z)
			: opennova_light_block_hemi_sky;
}

vec3 obj_hemi_ground_color() {
	if (obj_thermal_wave()) return vec3(0.25);
	return obj_entity_interior()
			? mix(opennova_light_block_floor, opennova_light_block_hemi_ground,
					obj_entity_light().z)
			: opennova_light_block_hemi_ground;
}

// Ambient average plus the opposing sky/ground delta lights reduces to this
// hemisphere mix. [orig: RenderBatchCtx_StoreLightingConstants @ 0x5D89E0;
// Lighting_SetHemisphereD3DLights @ 0x5D8CB0]
vec3 obj_hemi(vec3 normal_ws) {
	float up = clamp(normal_ws.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(obj_hemi_ground_color(), obj_hemi_sky_color(), up);
}

float obj_point_light_count() {
	if (obj_uses_static_point_light_row()) {
		return texelFetch(opennova_static_point_light_rows,
				ivec2(0, obj_static_point_light_row()), 0).x;
	}
	return u_point_light_count;
}

vec4 obj_point_light_posr(int light_index) {
	if (obj_uses_static_point_light_row()) {
		return texelFetch(opennova_static_point_light_rows,
				ivec2(1 + light_index * 2, obj_static_point_light_row()), 0);
	}
	if (light_index == 0) {
		return u_point_light_posr_0;
	}
	if (light_index == 1) {
		return u_point_light_posr_1;
	}
	if (light_index == 2) {
		return u_point_light_posr_2;
	}
	return u_point_light_posr_3;
}

vec4 obj_point_light_color(int light_index) {
	if (obj_uses_static_point_light_row()) {
		return texelFetch(opennova_static_point_light_rows,
				ivec2(2 + light_index * 2, obj_static_point_light_row()), 0);
	}
	if (light_index == 0) {
		return u_point_light_color_0;
	}
	if (light_index == 1) {
		return u_point_light_color_1;
	}
	if (light_index == 2) {
		return u_point_light_color_2;
	}
	return u_point_light_color_3;
}

// The fixed-function contribution: per selected light, the D3D light's
// diffuse (the delivered colour x 1.5, the fill's boost) * ndotl /
// (1 + atten2 * d^2) inside the range cutoff. Only this D3D-light path takes
// the boost; every shader pass reads the unboosted colour.
// vertex_standard.gdshaderinc sums it into v_ff_diffuse for the Fixed
// families; the pixel-shader helpers below bypass it and evaluate their
// NORMAL-pass mapped N.L/N.H explicitly.
// [orig: re-enable @ Light_ApplyAsD3DLight 0x5da61a; params @
// Light_GetPointLightParams 0x5a9180 / Light_FillD3DPointLight 0x5aa4b2
// (flt_7D4B80 = 1.5)]
vec3 obj_point_light_one(vec3 world_pos, vec3 normal_ws, vec4 posr, vec4 col) {
	vec3 to_light = posr.xyz - world_pos;
	float dist = length(to_light);
	if (dist >= col.w) {
		return vec3(0.0);
	}
	float ndotl = max(dot(normal_ws, to_light / max(dist, 0.001)), 0.0);
	return col.rgb * 1.5 * (ndotl / (1.0 + posr.w * dist * dist));
}

vec3 obj_point_light_sum(vec3 world_pos, vec3 normal_ws) {
	vec3 sum = vec3(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				obj_point_light_posr(0), obj_point_light_color(0));
	}
	if (count > 1.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				obj_point_light_posr(1), obj_point_light_color(1));
	}
	if (count > 2.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				obj_point_light_posr(2), obj_point_light_color(2));
	}
	if (count > 3.5) {
		sum += obj_point_light_one(world_pos, normal_ws,
				obj_point_light_posr(3), obj_point_light_color(3));
	}
	return sum;
}

// Pixel-shader object effects keep the normal-map N.L/N.H evaluation in the
// pixel stage. Their vertex shaders multiply each directional/point pass by
// this sharp geometric-normal horizon term. SelfShadowMult=10 and
// SelfShadowAdd=.5 are literal retail constants. [orig: _BaseInc.fx
// CalcSelfShadowTerm; _vsDiffT/O, _vsPhongT/O, _vsSkDfT/O, _vsSkPhT/O].
float obj_self_shadow(vec3 geom_normal_ws, vec3 to_light_ws) {
	return clamp(dot(obj_unit_or_zero(geom_normal_ws), normalize(to_light_ws)) * 10.0 + 0.5,
			0.0, 1.0);
}

float obj_pixel_point_vertex_factor_one(vec3 world_pos, vec3 geom_normal_ws,
		vec4 posr) {
	vec3 to_light = posr.xyz - world_pos;
	float distance_to_light = length(to_light);
	float attenuation = 1.0 /
			(1.0 + posr.w * distance_to_light * distance_to_light);
	return attenuation * obj_self_shadow(geom_normal_ws, to_light);
}

float obj_pixel_point_vertex_attenuation_one(vec3 world_pos, vec4 posr) {
	vec3 to_light = posr.xyz - world_pos;
	float distance_to_light = length(to_light);
	return 1.0 / (1.0 + posr.w * distance_to_light * distance_to_light);
}

vec4 obj_pixel_point_vertex_factors(vec3 world_pos, vec3 geom_normal_ws) {
	vec4 factors = vec4(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		factors.x = obj_pixel_point_vertex_factor_one(
				world_pos, geom_normal_ws, obj_point_light_posr(0));
	}
	if (count > 1.5) {
		factors.y = obj_pixel_point_vertex_factor_one(
				world_pos, geom_normal_ws, obj_point_light_posr(1));
	}
	if (count > 2.5) {
		factors.z = obj_pixel_point_vertex_factor_one(
				world_pos, geom_normal_ws, obj_point_light_posr(2));
	}
	if (count > 3.5) {
		factors.w = obj_pixel_point_vertex_factor_one(
				world_pos, geom_normal_ws, obj_point_light_posr(3));
	}
	return factors;
}

vec4 obj_pixel_point_vertex_attenuations(vec3 world_pos) {
	vec4 factors = vec4(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		factors.x = obj_pixel_point_vertex_attenuation_one(
				world_pos, obj_point_light_posr(0));
	}
	if (count > 1.5) {
		factors.y = obj_pixel_point_vertex_attenuation_one(
				world_pos, obj_point_light_posr(1));
	}
	if (count > 2.5) {
		factors.z = obj_pixel_point_vertex_attenuation_one(
				world_pos, obj_point_light_posr(2));
	}
	if (count > 3.5) {
		factors.w = obj_pixel_point_vertex_attenuation_one(
				world_pos, obj_point_light_posr(3));
	}
	return factors;
}

vec3 obj_bump_diffuse_point_one(vec3 base_rgb, vec3 normal_ws,
		vec4 posr, vec4 col, float vertex_factor) {
	if (vertex_factor <= 0.0) {
		return vec3(0.0);
	}
	vec3 to_light = posr.xyz - obj_point_light_origin();
	float dist = length(to_light);
	vec3 light_dir = to_light / max(dist, 0.001);
	float ndotl = max(dot(normalize(normal_ws), light_dir), 0.0);
	return base_rgb * col.rgb * (2.0 * ndotl * vertex_factor);
}

vec3 obj_bump_diffuse_points_with_factors(vec3 base_rgb, vec3 normal_ws,
		vec4 point_factors) {
	vec3 sum = vec3(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		sum += obj_bump_diffuse_point_one(base_rgb, normal_ws,
				obj_point_light_posr(0), obj_point_light_color(0), point_factors.x);
	}
	if (count > 1.5) {
		sum += obj_bump_diffuse_point_one(base_rgb, normal_ws,
				obj_point_light_posr(1), obj_point_light_color(1), point_factors.y);
	}
	if (count > 2.5) {
		sum += obj_bump_diffuse_point_one(base_rgb, normal_ws,
				obj_point_light_posr(2), obj_point_light_color(2), point_factors.z);
	}
	if (count > 3.5) {
		sum += obj_bump_diffuse_point_one(base_rgb, normal_ws,
				obj_point_light_posr(3), obj_point_light_color(3), point_factors.w);
	}
	return sum;
}

vec3 obj_bump_diffuse_points(vec3 base_rgb, vec3 normal_ws) {
	return obj_bump_diffuse_points_with_factors(
			base_rgb, normal_ws, v_pixel_point_factor);
}

vec3 obj_dot3_points(vec3 base_rgb, vec3 normal_ws) {
	return obj_bump_diffuse_points_with_factors(
			base_rgb, normal_ws, v_pixel_point_attenuation);
}

vec3 obj_bump_diffuse_lighting(vec3 base_rgb, vec3 normal_ws,
		vec3 geom_normal_ws) {
	vec3 normal = normalize(normal_ws);
	vec3 to_light = normalize(-opennova_light_block_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	vec3 lit = base_rgb * (obj_hemi(normal) +
			obj_dir_light_color() * (ndotl * v_dir_self_shadow)) * 2.0;
	return lit + obj_bump_diffuse_points(base_rgb, normal);
}

// BDiffT2's shader-usage-2 path keeps mapped hemisphere lighting and the
// directional self-shadow in its pixel pass. Its point-light pass is the
// fixed-function encoded-vector path, so attenuation is interpolated without
// a geometric self-shadow multiplier.
vec3 obj_dot3_lighting(vec3 base_rgb, vec3 normal_ws) {
	vec3 normal = normalize(normal_ws);
	vec3 to_light = normalize(-opennova_light_block_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	vec3 lit = base_rgb * (obj_hemi(normal) +
			obj_dir_light_color() * (ndotl * v_dir_self_shadow)) * 2.0;
	return lit + obj_dot3_points(base_rgb, normal);
}

// SkBDiffT2/SkBDiffO2 have no pixel-shader NORMAL alternative: hemisphere
// is Gouraud geometric-normal light, and their encoded directional/point
// vectors carry no CalcSelfShadowTerm multiplier.
vec3 obj_dot3_skinned_lighting(vec3 base_rgb, vec3 normal_ws,
		vec3 geom_normal_ws) {
	vec3 normal = normalize(normal_ws);
	vec3 to_light = normalize(-opennova_light_block_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	vec3 lit = base_rgb * (obj_hemi(obj_unit_or_zero(geom_normal_ws)) +
			obj_dir_light_color() * ndotl) * 2.0;
	return lit + obj_dot3_points(base_rgb, normal);
}

float obj_pow8(float value) {
	float p2 = value * value;
	float p4 = p2 * p2;
	return p4 * p4;
}

float obj_pow16(float value) {
	float p8 = obj_pow8(value);
	return p8 * p8;
}

float obj_pow64(float value) {
	float p16 = obj_pow16(value);
	float p32 = p16 * p16;
	return p32 * p32;
}

vec3 obj_phong_point_one(vec3 base_rgb, float specular_brightness,
		vec3 normal_ws, vec3 view_dir, vec4 posr, vec4 col, float vertex_factor) {
	if (vertex_factor <= 0.0) {
		return vec3(0.0);
	}
	vec3 to_light = posr.xyz - obj_point_light_origin();
	float dist = length(to_light);
	vec3 light_dir = to_light / max(dist, 0.001);
	float ndotl = max(dot(normal_ws, light_dir), 0.0);
	float specular = 0.0;
	if (ndotl > 0.0) {
		specular = obj_pow8(max(dot(normal_ws,
				normalize(light_dir + view_dir)), 0.0)) * specular_brightness;
	}
	return (base_rgb * ndotl + vec3(specular)) * col.rgb *
			(2.0 * vertex_factor);
}

vec3 obj_phong_points(vec3 base_rgb, float specular_brightness,
		vec3 normal_ws, vec3 view_dir) {
	vec3 sum = vec3(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		sum += obj_phong_point_one(base_rgb, specular_brightness, normal_ws,
				view_dir, obj_point_light_posr(0), obj_point_light_color(0),
				v_pixel_point_factor.x);
	}
	if (count > 1.5) {
		sum += obj_phong_point_one(base_rgb, specular_brightness, normal_ws,
				view_dir, obj_point_light_posr(1), obj_point_light_color(1),
				v_pixel_point_factor.y);
	}
	if (count > 2.5) {
		sum += obj_phong_point_one(base_rgb, specular_brightness, normal_ws,
				view_dir, obj_point_light_posr(2), obj_point_light_color(2),
				v_pixel_point_factor.z);
	}
	if (count > 3.5) {
		sum += obj_phong_point_one(base_rgb, specular_brightness, normal_ws,
				view_dir, obj_point_light_posr(3), obj_point_light_color(3),
				v_pixel_point_factor.w);
	}
	return sum;
}

vec3 obj_phong_lighting(vec3 base_rgb, float specular_brightness,
		vec3 normal_ws, vec3 geom_normal_ws, vec3 view_dir,
		float directional_self_shadow) {
	vec3 normal = normalize(normal_ws);
	vec3 to_light = normalize(-opennova_light_block_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	float specular = 0.0;
	if (ndotl > 0.0) {
		specular = obj_pow8(max(dot(normal, normalize(to_light + view_dir)), 0.0)) *
				specular_brightness;
	}
	float shadow = mix(1.0, v_dir_self_shadow,
			directional_self_shadow);
	// ps11Phong's hemisphere value comes from the geometric normal in oD1;
	// Diffuse1.a scales only the pow-8 specular lobe.
	vec3 lit = (base_rgb * obj_hemi(geom_normal_ws) +
			(base_rgb * ndotl + vec3(specular)) * obj_dir_light_color() * shadow) * 2.0;
	return lit + obj_phong_points(base_rgb, specular_brightness,
			normal, view_dir);
}

vec4 obj_phong_map_lookup(float ndotl, float ndoth) {
	return texture(u_phong_map, clamp(vec2(ndotl, ndoth), vec2(0.0), vec2(1.0)));
}

float obj_phong_map_specular(vec4 lookup, float control) {
	float alpha = clamp(control, 0.0, 1.0);
	return alpha * mix(lookup.r, lookup.b, alpha);
}

vec3 obj_phong_map_point_one(vec3 base_rgb, float control, vec3 normal_ws,
		vec3 view_dir, vec4 posr, vec4 col, float vertex_factor) {
	if (vertex_factor <= 0.0) {
		return vec3(0.0);
	}
	vec3 to_light = posr.xyz - obj_point_light_origin();
	float dist = length(to_light);
	vec3 light_dir = to_light / max(dist, 0.001);
	float ndotl = max(dot(normal_ws, light_dir), 0.0);
	float ndoth = max(dot(normal_ws, normalize(light_dir + view_dir)), 0.0);
	vec4 lookup = obj_phong_map_lookup(ndotl, ndoth);
	float specular = ndotl > 0.0 ? obj_phong_map_specular(lookup, control) : 0.0;
	return (base_rgb * lookup.a + vec3(specular)) * col.rgb *
			(2.0 * vertex_factor);
}

vec3 obj_phong_map_points(vec3 base_rgb, float control, vec3 normal_ws,
		vec3 view_dir) {
	vec3 sum = vec3(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		sum += obj_phong_map_point_one(base_rgb, control, normal_ws,
				view_dir, obj_point_light_posr(0), obj_point_light_color(0),
				v_pixel_point_factor.x);
	}
	if (count > 1.5) {
		sum += obj_phong_map_point_one(base_rgb, control, normal_ws,
				view_dir, obj_point_light_posr(1), obj_point_light_color(1),
				v_pixel_point_factor.y);
	}
	if (count > 2.5) {
		sum += obj_phong_map_point_one(base_rgb, control, normal_ws,
				view_dir, obj_point_light_posr(2), obj_point_light_color(2),
				v_pixel_point_factor.z);
	}
	if (count > 3.5) {
		sum += obj_phong_map_point_one(base_rgb, control, normal_ws,
				view_dir, obj_point_light_posr(3), obj_point_light_color(3),
				v_pixel_point_factor.w);
	}
	return sum;
}

vec3 obj_environment_cube_approx(vec3 reflection_ws) {
	vec3 direction = normalize(reflection_ws);
	if (!opennova_environment_cube_ready) {
		// Dummy/headless renderers cannot produce the six GPU faces. The live
		// Forward+ path publishes synchronously before its visible main draw;
		// this only keeps editor/resource-validation contexts defined.
		return obj_hemi(direction);
	}
	vec3 dimmed_sky = texture(opennova_environment_cube, direction).rgb;
	// Environment_GetLightDirectionFloat is the incoming-light tuple;
	// MatRotSpecular aligns the static cube's -Z lobe to its negation. The
	// reflection is a Godot-world vector, so the axis is the light block's
	// Godot-world -normalize(light dir), never the raw render-float tuple
	// (whose x/z are swapped against Godot's): retail
	// Material_ApplyShaderParameters @ 0x58e146..0x58e1cb ->
	// Math_BuildDirectionLookAtMatrix(Environment_GetLightDirectionFloat).
	float aligned = max(dot(direction, normalize(opennova_light_block_dir)), 0.0);
	vec3 static_lobe = clamp(
			vec3(1.4) * pow(aligned, 800.0) +
			vec3(1.0, 248.0 / 255.0, 240.0 / 255.0) *
					pow(aligned, 40.0),
			vec3(0.0), vec3(1.0));
	// Render_SkyMesh binds g_EnvLightBlock and the fixed-function MODULATE2X
	// descriptor, then submits the rotated CubeRotSpecular sphere additively.
	// [orig: Render_SkyMesh @ 0x5ac680; sub_5AC600 (the sky-mesh descriptor)].
	return clamp(dimmed_sky + static_lobe * opennova_env_light_block * 2.0,
			vec3(0.0), vec3(1.0));
}

vec3 obj_mirror_point_one(vec3 normal_ws, vec3 view_dir,
		vec4 posr, vec4 col, float vertex_factor) {
	if (vertex_factor <= 0.0) {
		return vec3(0.0);
	}
	vec3 to_light = posr.xyz - obj_point_light_origin();
	float dist = length(to_light);
	vec3 light_dir = to_light / max(dist, 0.001);
	float ndoth = max(dot(normal_ws, normalize(light_dir + view_dir)), 0.0);
	return col.rgb * (2.0 * obj_pow16(ndoth) * vertex_factor);
}

vec3 obj_mirror_points(vec3 normal_ws, vec3 view_dir) {
	vec3 sum = vec3(0.0);
	float count = obj_point_light_count();
	if (count > 0.5) {
		sum += obj_mirror_point_one(normal_ws, view_dir,
				obj_point_light_posr(0), obj_point_light_color(0), v_pixel_point_factor.x);
	}
	if (count > 1.5) {
		sum += obj_mirror_point_one(normal_ws, view_dir,
				obj_point_light_posr(1), obj_point_light_color(1), v_pixel_point_factor.y);
	}
	if (count > 2.5) {
		sum += obj_mirror_point_one(normal_ws, view_dir,
				obj_point_light_posr(2), obj_point_light_color(2), v_pixel_point_factor.z);
	}
	if (count > 3.5) {
		sum += obj_mirror_point_one(normal_ws, view_dir,
				obj_point_light_posr(3), obj_point_light_color(3), v_pixel_point_factor.w);
	}
	return sum;
}

// D3D vertex diffuse saturates hemi + directional + the enabled point
// lights at each vertex; the colour then Gouraud-interpolates and MODULATE2X
// combines texture and diffuse. Flag.fx and SkBasic.fx compute the same
// Out.Diff per vertex. [orig: _FFP.fx TBoringFFP; D3D light 0 setup
// @ 0x5D9CE2..0x5D9D76]
vec3 obj_ff_vertex_diffuse(vec3 normal_ws, vec3 point_diffuse) {
	vec3 normal = normalize(normal_ws);
	vec3 to_light = normalize(-opennova_light_block_dir);
	float ndotl = max(dot(normal, to_light), 0.0);
	return clamp(obj_hemi(normal) + obj_dir_light_color() * ndotl + point_diffuse,
			vec3(0.0), vec3(1.0));
}

vec3 obj_ff_lighting(vec3 base_rgb) {
	return base_rgb * v_ff_diffuse * 2.0;
}

// The SELFLUM emissive is SelfLumColor x ColorSrcGlobalGain; fixed-function
// lighting saturates it before MODULATE2X. retail _FFP.fx TBoringFFP SELFLUM
// block, Material_ApplyShaderParameters @ 0x58E050..0x58E06A (ColorSrcGlobalGain).
vec3 obj_self_lit(vec3 base_rgb, vec3 self_lum) {
	return base_rgb * clamp(self_lum * opennova_light_block_gain,
			vec3(0.0), vec3(1.0)) * 2.0;
}

vec3 obj_add_local_light(vec3 lit, vec3 base_rgb, vec3 normal_ws) {
	if (u_local_light_count <= 0) {
		return lit;
	}
	vec3 to_light = u_local_light_position - v_world_pos;
	float dist = length(to_light);
	if (dist >= u_local_light_atten_end) {
		return lit;
	}
	float range = max(u_local_light_atten_end - u_local_light_atten_start, 0.001);
	float atten = 1.0 - clamp((dist - u_local_light_atten_start) / range, 0.0, 1.0);
	vec3 light_dir = to_light / max(dist, 0.001);
	float ndotl = max(dot(normalize(normal_ws), light_dir), 0.0);
	return lit + base_rgb * u_local_light_color *
			(u_local_light_intensity * ndotl * atten);
}

// Witnessed device fog policy shared with terrain and water. The pass start
// is already Render_SetFogState's per-type start (the overcast fold rides
// it: type 2 = (1 - density) * end * 0.5, type 3 = (1 - density) * end * 0.25
// [orig: Render_SetFogState @ 0x58a9c5..0x58a9fe]).
// [orig: Render_SetFogState @ 0x58A950; CD3DDevice_SetFogParameters @ 0x677960]
float obj_fog_visibility(float dist, float fog_start, float fog_end, int fog_type) {
	float safe_end = max(fog_end, 1.0);
	if (fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}
	return clamp((safe_end - dist) / max(safe_end - fog_start, 1.0), 0.0, 1.0);
}

// The pass fog visibility at this fragment; 1.0 while the pass has no fog.
// The distance follows the fog path of the wrapper's retail NORMAL P0:
// - OBJ_FOG_VERTEX_SHADER: a FOGMODE_SHADER* pass leaves the device's table
//   and vertex fog OFF (CD3DDevice_SetFogAndBlendMode mode 8..11 @ 0x677783),
//   so the fog factor is the VS oFog = 1 - (z - FogStart) * FogRangeRecip
//   over clip z (_BaseInc.fx CalcFogSegmented / CalcFogSkinned) with
//   FogStart / FogRangeRecip the start / end pushed through the projection
//   (Material_ApplyShaderParameters @ 0x58e20d..0x58e26f): linear in view depth for
//   EVERY fog type, from the pass start (0.5 for types 0/1, the caller's
//   constant, Environment_ApplyFogAndAmbient @ 0x57e4d2).
// - OBJ_FOG_VERTEX_SHADER_DEVICE: a vertex-shader pass on FOGMODE_NORMAL
//   (SkBasic.fx): the linear types keep that VS oFog, type 0 is the device's
//   EXP fog (table or non-range vertex: view depth).
// - otherwise (a fixed-function pass, VertexShader = NULL): the linear types
//   are vertex RANGE fog over the radial eye distance (FOGVERTEXMODE LINEAR,
//   RANGEFOGENABLE 1 @ 0x677a50..0x677a69), type 0 the device EXP on view depth
//   (@ 0x6779e4..0x677a48).
// The viewmodel fogs under the dry pass (viewmodel_pass.gdshaderinc) and the
// water mirror pass under its own dry block (EnvironmentState::
// build_water_mirror_fog).
vec3 obj_scene_fog_range() {
	if (u_viewmodel_pass) {
		return opennova_viewmodel_fog_range;
	}
	if (v_water_mirror_pass > 0.5) {
		return opennova_water_mirror_fog_range;
	}
	return vec3(opennova_fog_start, opennova_fog_end, float(opennova_fog_type));
}

float obj_scene_fog_visibility(vec3 camera_position_world) {
	if (!opennova_fog_enabled) {
		return 1.0;
	}
	vec3 range = obj_scene_fog_range();
	int fog_type = int(range.z);
	float view_depth = max(v_fog_view_depth, 0.0);
	#if defined(OBJ_FOG_VERTEX_SHADER)
	float safe_end = max(range.y, 1.0);
	return clamp((safe_end - view_depth) / max(safe_end - range.x, 1.0), 0.0, 1.0);
	#elif defined(OBJ_FOG_VERTEX_SHADER_DEVICE)
	return obj_fog_visibility(view_depth, range.x, range.y, fog_type);
	#else
	float dist = fog_type == 0 ? view_depth
			: distance(camera_position_world, v_world_pos);
	return obj_fog_visibility(dist, range.x, range.y, fog_type);
	#endif
}

vec3 obj_scene_fog_color() {
	if (u_viewmodel_pass) {
		return opennova_viewmodel_fog_color;
	}
	return v_water_mirror_pass > 0.5 ? opennova_water_mirror_fog_color
			: opennova_fog_color;
}

vec3 obj_apply_fog(vec3 lit, vec3 camera_position_world) {
	if (!opennova_fog_enabled) {
		return lit;
	}
	return mix(obj_scene_fog_color(), lit,
			obj_scene_fog_visibility(camera_position_world));
}

vec3 obj_apply_additive_fog(vec3 lit, vec3 camera_position_world) {
	return lit * obj_scene_fog_visibility(camera_position_world);
}
