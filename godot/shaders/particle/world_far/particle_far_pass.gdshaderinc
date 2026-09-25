// The World particle pass on the water's far side (retail pass A) drawn as
// transparent render-list geometry: retail submits it after the far-side
// tracers and before the far-side foliage and the water surface
// (Terrain_RenderSceneWithReflection: CEffectEmitterPool_RenderMainPass
// @ 0x5c95ac -> EffectWorld_RenderParticlePass(0) @ 0x5c95b5 ->
// Foliage_RenderFarPatchesPass(0) @ 0x5c95c5 -> Terrain_RenderWaterPass
// @ 0x5c95dc), so its runs sit at kRungParticleFarSide, ordered among
// themselves by the instances' sorting offsets (ParticleFarPass). The stage
// program per type is retail's (CParticleTexture_InitTextureAndChannels):
// MODULATE(TEXTURE, DIFFUSE) on colour and alpha for Blend/Additive/Premult/
// Mod/Mod2x (@ 0x5e8347, @ 0x5e8380, @ 0x5e843b, @ 0x5e8487), DOT3(TEXTURE,
// DIFFUSE) with TEXTURE.a x DIFFUSE.a for Bump/Bumpadd (@ 0x5e83d6,
// @ 0x5e84c0). Each type's intrinsic pass word (CGfxShader_SetRenderStateDesc
// @ 0x683664, committed by CGfxShader_ApplyPass @ 0x68324f / @ 0x683271)
// enables FOGENABLE (0x20000, @ 0x5e8324) everywhere but on Bump/Bumpadd,
// whose 0x10000 (@ 0x5e8424 / @ 0x5e850a) enables SPECULARENABLE instead: the
// SPECULAR vertex colour adds after the stages and no fog applies. The fog
// colour per type is CParticleBatch_FlushAndBindMaterial's (@ 0x5e4297:
// black for Additive/Premult/Bumpadd, white for Mod, mid-grey for Mod2x,
// the scene colour otherwise). Distort stays on the effects' distortion
// path. Each wrapper selects its type with one PARTICLE_FAR_* define and its
// framebuffer blend with render_mode. PARTICLE_FAR_THERMAL selects the
// texture's secondary material, bound while the main scene's thermal byte is
// set (CParticleBatch_FlushAndBindMaterial @ 0x5e42bf): Blend's
// MODULATE(1 - TEXTURE, 1 - DIFFUSE) colour (@ 0x5e8584..0x5e85dd) and
// Additive/Premult's ZERO/INVSRCCOLOR darkening (@ 0x5e8390..0x5e83a5).

#include "res://shaders/color.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear, repeat_disable;
uniform bool has_texture = false;
// The view this run was compiled for (0 the main view, 1 the second scene
// view): its camera-facing quads belong to that eye alone. The main view is
// the camera carrying the viewmodel layer (2048), the second scene view the
// one that draws the water layer (1024) without it; the mirror, which draws
// no water layer, runs its own passes.
instance uniform int u_far_view = 0;
// The scene pass fog the World compositor fogs with (the same block the
// object family reads).
global uniform vec3 opennova_fog_color;
global uniform float opennova_fog_start;
global uniform float opennova_fog_end;
global uniform int opennova_fog_type;

varying vec3 v_world_position;
varying vec4 v_secondary;

void vertex() {
	v_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_secondary = CUSTOM0;
	POSITION = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(VERTEX, 1.0));
	bool main_eye = (CAMERA_VISIBLE_LAYERS & 2048u) != 0u;
	bool second_eye = !main_eye && (CAMERA_VISIBLE_LAYERS & 1024u) != 0u;
	if (IN_SHADOW_PASS || (u_far_view == 0 ? !main_eye : !second_eye)) {
		// Outside every clip plane: another eye draws nothing of this run.
		POSITION = vec4(2.0, 2.0, 2.0, 1.0);
	}
}

// The World compositor's fog visibility: type 0 exponential over the view
// depth, every other type linear over the radial distance from the
// environment-resolved start.
float particle_far_fog_visibility(vec3 camera_position, vec3 camera_forward) {
	if (opennova_fog_start == opennova_fog_end || opennova_fog_end <= 0.0) {
		return 1.0;
	}
	if (opennova_fog_type == 0) {
		float view_depth = max(dot(v_world_position - camera_position, camera_forward), 0.0);
		return clamp(exp(-view_depth * (4.1588830833596715 / opennova_fog_end)), 0.0, 1.0);
	}
	float distance_to_eye = length(v_world_position - camera_position);
	return clamp((opennova_fog_end - distance_to_eye) /
			(opennova_fog_end - opennova_fog_start), 0.0, 1.0);
}

vec4 particle_far_texel(vec2 uv) {
	if (has_texture) {
		return texture(albedo_tex, uv);
	}
	// The procedural fallback for a missing atlas page (the first-person
	// batch's shape).
	vec2 centered = uv - vec2(0.5);
	return vec4(1.0, 1.0, 1.0, clamp(1.0 - length(centered) * 2.0, 0.0, 1.0));
}

void fragment() {
	vec4 texel = particle_far_texel(UV);
	vec4 color = texel * COLOR;
	float visibility = particle_far_fog_visibility(CAMERA_POSITION_WORLD,
			CAMERA_DIRECTION_WORLD);
#if defined(PARTICLE_FAR_BUMP)
	// DOT3 of the bump texel against the lit DIFFUSE, then the SPECULAR vertex
	// colour added by SPECULARENABLE; no fog (the type's pass word is 0x10000).
	float dot3 = clamp(dot(texel.rgb * 2.0 - 1.0, COLOR.rgb * 2.0 - 1.0), 0.0, 1.0);
	color = vec4(min(vec3(dot3) + v_secondary.rgb, vec3(1.0)), texel.a * COLOR.a);
#elif defined(PARTICLE_FAR_BLEND)
#if defined(PARTICLE_FAR_THERMAL)
	color.rgb = (vec3(1.0) - texel.rgb) * (vec3(1.0) - COLOR.rgb);
#endif
	color.rgb = mix(opennova_fog_color, color.rgb, visibility);
#elif defined(PARTICLE_FAR_ADDITIVE)
	color.rgb = mix(vec3(0.0), color.rgb, visibility);
#elif defined(PARTICLE_FAR_MOD)
	color.rgb = mix(vec3(1.0), color.rgb, visibility);
#elif defined(PARTICLE_FAR_MOD2X)
	color.rgb = mix(vec3(127.0 / 255.0), color.rgb, visibility);
#endif
#if defined(PARTICLE_FAR_MOD2X)
	// DESTCOLOR/SRCCOLOR = 2 * src * dst: the multiply blend with the doubled
	// (unclamped) source on the floating-point scene target.
	ALBEDO = clamp(color.rgb, vec3(0.0), vec3(1.0)) * 2.0;
	ALPHA = 1.0;
#elif defined(PARTICLE_FAR_MOD)
	ALBEDO = clamp(color.rgb, vec3(0.0), vec3(1.0));
	ALPHA = 1.0;
#elif defined(PARTICLE_FAR_ADDITIVE) && defined(PARTICLE_FAR_THERMAL)
	ALBEDO = vec3(1.0) - scene_output(color.rgb);
	ALPHA = 1.0;
#else
	ALBEDO = scene_output(color.rgb);
	ALPHA = clamp(color.a, 0.0, 1.0);
#endif
}
