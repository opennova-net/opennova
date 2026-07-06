shader_type spatial;
// blend_premul_alpha with ALPHA = 1 - a expresses the witnessed framebuffer
// blend EXACTLY: retail's above-water surface draws with SrcBlend=ONE,
// DestBlend=SRCALPHA (blend mode 11 in decode_blend_mode_to_d3d_states
// @ 0x680f00) - out = src + dst * a - and Godot's premul pass computes
// out = src + dst * (1 - ALPHA). [orig: Water_InitSurfaceShaders @ 0x5c19b0
// builds Water_ShaderBlend with SrcBlend 2/DestBlend 5/enable 1]
render_mode unshaded, blend_premul_alpha, cull_disabled;

// Water surface: structural port of the witnessed render_water_surface pass
// [orig: @ 0x5c32c0, docs/env/env-tod-re.md "Water surface"]. The look is the
// HIGH-DETAIL strip model [orig: render_water_strip_detailed @ 0x5c27d0]:
// flat geometry, the animated 128x128 noise ridge texture + its DuDv map
// (regenerated per frame in libs/env - Water_GenerateNoiseTextures
// @ 0x5c0360), per-vertex Env_WaterColorLit modulation, distance-based alpha
// falloff, and the shared scene fog. The screen-marched strip TESSELLATION
// itself is a tracked divergence (env #29 - this plane is the host stand-in).
//
// The water material set was decoded at REN-4 [orig: Water_InitSurfaceShaders
// @ 0x5c19b0; selection in render_water_surface @ 0x5c33e6..0x5c34ea]:
// - camera ABOVE water: the BLENDED material (ONE + dst*SRCALPHA, ported via
//   the premul trick above); camera BELOW (underwater view of the surface):
//   the OPAQUE material - that side swap rides the underwater slice with
//   D-RORD-3's residual;
// - nightvision post-redraw picks NV variants (FrameFX; out of scope here);
// - all water passes are two-sided (pass flags 0x400000 -> cull NONE
//   [orig: GfxShader_ApplyPassChecked(shader, 0x400000) @ 0x5c340d;
//   CGfxShader_ApplyPass @ 0x683190]) = cull_disabled, and alpha-test ref 32
//   GREATER [orig: CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419] = the discard
//   below (retail's far cutoff: fragments past the fog fade fail the test and
//   the skyfog clear color shows instead);
// - PS path (detail >= 2): color = reflection(perturbed by DuDv) x2 x diffuse
//   x4 x noise + specular, alpha = noise.a x diffuse.a; FF fallback: s0
//   MOD2X(reflection, diffuse), s1 MOD2X(noise, current), alpha
//   MOD(noise.a, diffuse.a). The reflection RTT consumer is env #30 (REN-6);
//   until it lands the noise MOD2X chain below is the reflection-free subset
//   (flat-gray reflection folds MOD2X(refl, diffuse) to diffuse).

// Gamma-space output convention (D-RMAT-7): nova_gamma_to_linear().
#include "res://shaders/nova_color.gdshaderinc"

uniform vec3 u_water_color = vec3(0.408, 0.314, 0.224); // Env_WaterColorLit
// water_murk [orig: SetWaterMurk @ 0x57d4f0]. Under the witnessed blend
// (out = src + dst * a) the alpha term is the BACKGROUND share, and retail's
// murk SUBTRACTS from the per-vertex alpha [orig: render_water_strip murk
// angle term @ 0x5c1d60 - "- Env_WaterMurk"], so murk UP = background DOWN.
// The exact angle-weighted murk chain rides env #29; (1 - murk) is the
// interim host knob in the witnessed direction.
uniform float u_water_murk = 0.6;
// (scale, bias, offset_u, offset_v) [orig: @ 0x5c3348..0x5c33db]: scale/bias
// from the fog-distance INT part, offsets = cam/128 + cloud accumulators.
uniform vec4 u_water_uv = vec4(1.0, 0.2, 0.0, 0.0);
uniform sampler2D u_noise_color : filter_linear, repeat_enable;
uniform sampler2D u_noise_normal : filter_linear, repeat_enable;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
uniform float u_fog_start = 300.0;
uniform float u_fog_end = 1000.0;
uniform int u_fog_type = 2;

varying vec3 v_world_pos;

float jointops_fog_factor(float dist, float fog_start, float fog_end, int fog_type) {
	float safe_end = max(fog_end, 1.0);
	if (fog_type == 0) {
		return clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);
	}

	float start = fog_start;
	if (fog_type == 2) {
		start = safe_end * 0.5;
	} else if (fog_type == 3) {
		start = safe_end * 0.25;
	}
	float fog_range = max(safe_end - start, 1.0);
	return clamp((safe_end - dist) / fog_range, 0.0, 1.0);
}

void vertex() {
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	// The witnessed UV model: strip vertices are camera-relative, so the base
	// term is (world - camera)/128 through the fog-derived scale - bias, and
	// the offset carries camera/128 + the scroll accumulators
	// [orig: per-vertex u = t * flt_8412B0 - flt_8412B4 in both strips;
	//  offsets @ 0x5c33b9..0x5c33db]. u rides world z, v rides world x.
	vec2 rel = vec2(v_world_pos.z - CAMERA_POSITION_WORLD.z,
			v_world_pos.x - CAMERA_POSITION_WORLD.x) / 128.0;
	vec2 uv = rel * u_water_uv.x - vec2(u_water_uv.y) + vec2(u_water_uv.z, u_water_uv.w);

	// Ridge intensity + its alpha mask (A = 255 - i*i/512, more opaque where
	// dark) [orig: Water_GenerateNoiseTextures pass 2 @ 0x5c04b7..0x5c0552].
	// u_noise_normal (the DuDv map, pass 3) stays bound for the env #30
	// reflection consumer; its stage combine is undecoded, so it is not
	// sampled here.
	vec4 noise = texture(u_noise_color, uv);

	// Diffuse = lit water color modulated by the noise ridge (texture x
	// diffuse; the effect's stage table is undecoded — MODULATE2X is used as
	// the interim reading since the ridge averages ~128/255, making 2x
	// average-identity over the per-vertex color chain
	// [orig: (v * Env_WaterColorLit bytes) >> 8 @ 0x5c2430..0x5c2436]).
	vec3 color = u_water_color * noise.rgb * 2.0;

	// Distance alpha: the witnessed falloff direction — opaque near, fading
	// with distance^2 into the fog (v40 = 255 - fog_alpha^2/255
	// [orig: @ 0x5c2465 / the detailed-path v40 chain]), times the texture
	// alpha. u_water_murk stays as the host murk knob — the
	// witnessed murk term sits in the brightness/angle chain whose constants
	// are recorded (docs/env/env-tod-re.md Water surface) pending the exact
	// per-vertex port (env #29).
	float dist = length(v_world_pos - CAMERA_POSITION_WORLD);
	float dist_a = clamp(dist / max(u_fog_end, 1.0), 0.0, 1.0);
	// The witnessed TSS alpha = noise.a x per-vertex distance alpha — this is
	// what retail's alpha test sees; murk is applied after (a separate
	// per-vertex term, env #29).
	float alpha_dist = clamp(1.0 - dist_a * dist_a, 0.0, 1.0) * noise.a;

	// Alpha test ref 32, GREATER — texels at or below 32/255 are discarded
	// (kills the fully-faded far edge; retail's skyfog clear covers the
	// region) [orig: CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419 / @ 0x5c3484;
	// ref >= 0 -> D3DCMP_GREATER @ 0x6770a0].
	if (alpha_dist <= 32.0 / 255.0) {
		discard;
	}

	float alpha = alpha_dist * (1.0 - u_water_murk);

	float fog_factor = jointops_fog_factor(dist, u_fog_start, u_fog_end, u_fog_type);
	color = mix(u_fog_color, color, fog_factor);

	// The witnessed blend is out = src + dst * a (ONE + dst*SRCALPHA): under
	// Godot's premul pass (out = src.rgb + dst * (1 - ALPHA)) that is
	// src.rgb = color * 1 with ALPHA = 1 - a. The source term itself is NOT
	// multiplied by a — retail's SrcBlend is ONE.
	// Gamma-space output (D-RMAT-7): the src term encodes so the surface-
	// dominant (near) result displays the computed bytes; the dst*a term still
	// blends in the linear buffer — that residual is D-RMAT-8.
	ALBEDO = nova_gamma_to_linear(color);
	ALPHA = 1.0 - alpha;
}
