#include "renderer/object_shader_template.h"

namespace renderer {

namespace {

uint32_t encode_family(ObjectShaderFamily family) {
	return static_cast<uint32_t>(family) << OSCAP_FAMILY_SHIFT;
}

bool has_flag(ObjectShaderKey key, uint32_t bit) {
	return (key & bit) != 0;
}

std::string compose_render_mode(ObjectShaderKey key) {
	const bool alpha_test = has_flag(key, OSCAP_ALPHA_TEST);
	const uint32_t blend = static_cast<uint32_t>(decode_object_shader_blend(key));

	// Ported from the pre-repo prototype nova_shader_cache.cpp:99-127.
	// Always emits a blend mode (default `blend_mix` for Opaque + AlphaBlend);
	// without one Godot may push the surface into a different render pass
	// from the one our depth-draw / cull settings expect.
	std::string rm = "shader_type spatial;\nrender_mode unshaded, ";
	switch (blend) {
		case 2: rm += "blend_add"; break;
		case 3: rm += "blend_mul"; break;
		case 1:
		default: rm += "blend_mix"; break;
	}

	if (alpha_test) {
		rm += ", depth_prepass_alpha, depth_draw_opaque";
	} else if (blend != 0) {
		rm += ", depth_draw_never";
	} else {
		rm += ", depth_draw_opaque";
	}
	rm += has_flag(key, OSCAP_TWO_SIDED) ? ", cull_disabled" : ", cull_back";
	rm += ";\n\n";
	return rm;
}

std::string compose_uniforms(ObjectShaderKey key) {
	std::string u;
	// Textures sample RAW (no source_color): the retail pipeline never enables
	// D3DSAMP_SRGBTEXTURE - texel bytes enter the TSS math as-is (the gamma-
	// space convention, D-RMAT-7; witness map in render-material-re.md and
	// godot/shaders/nova_color.gdshaderinc).
	u += "uniform sampler2D u_diffuse : filter_linear_mipmap, repeat_enable;\n";
	if (has_flag(key, OSCAP_DETAIL))
		u += "uniform sampler2D u_detail : filter_linear_mipmap, repeat_enable;\n";
	if (has_flag(key, OSCAP_NORMAL_MAP))
		u += "uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;\n";

	u += "uniform vec3 u_uv_transform_u = vec3(1.0, 0.0, 0.0);\n";
	u += "uniform vec3 u_uv_transform_v = vec3(0.0, 1.0, 0.0);\n";
	u += "uniform vec3 u_rgb_mod = vec3(1.0);\n";
	u += "uniform float u_alpha_mod = 1.0;\n";

	if (has_flag(key, OSCAP_ALPHA_TEST)) {
		u += "uniform float u_alpha_test_threshold = 0.5;\n";
		u += "uniform float u_alpha_test_invert = 0.0;\n";
	}
	if (has_flag(key, OSCAP_GLASS)) {
		u += "uniform vec4 u_reflect_color = vec4(0.7, 0.8, 0.9, 0.35);\n";
	}
	u += "uniform float u_emissive = 0.0;\n";

	// The witnessed lighting uniform surface (.fx parameter slots 227-230 +
	// 232, resolved at load [orig: HLSLEffect_LoadFromFile @ 0x5af417..
	// 0x5af49e]): DirLightVector/DirLightColor/HemiGroundColor/HemiSkyColor
	// (AmbientColor derives as their average) and ColorSrcGlobalGain. Engine-
	// fed values are the env blocks' post-modulator colors (docs/render/
	// render-lighting-re.md); the defaults below (un-enved swatch/editor
	// scenes) are the RETAIL NOON register — the shipped full_00.env tod 1200
	// block bytes /255 (sun_rgb 170,170,167; sky_rgb 84,88,89; ground_rgb
	// 49,55,46) — so un-enved previews light like a JO noon world. Mirrored
	// by nova_object_model.gd's DEFAULT_* constants.
	u += "uniform vec3 u_hemi_sky_color = vec3(0.32941, 0.34510, 0.34902);\n";
	u += "uniform vec3 u_hemi_ground_color = vec3(0.19216, 0.21569, 0.18039);\n";
	u += "uniform vec3 u_dir_light_dir = vec3(-0.4082, -0.8165, -0.4082);\n";
	u += "uniform vec3 u_dir_light_color = vec3(0.66667, 0.66667, 0.65490);\n";
	u += "uniform vec3 u_color_src_global_gain = vec3(1.0);\n";
	u += "uniform bool u_fog_enabled = false;\n";
	u += "uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);\n";
	u += "uniform float u_fog_start = 0.0;\n";
	u += "uniform float u_fog_end = 1024.0;\n";
	u += "uniform int u_fog_type = 0;\n";
	u += "uniform float u_wind_amount = 0.5;\n";
	u += "uniform float u_wind_phase = 0.0;\n";

	// LGHT (per-model authored lights), retained for the explicit object-editor
	// preview only. Retail parses/stores this chunk but its gameplay renderer
	// has no post-load read of the model light field; runtime models therefore
	// keep u_local_light_count=0. The opt-in preview picks one dominant entry
	// and supplies its world-space position, colour and approximate attenuation.
	u += "uniform int u_local_light_count = 0;\n";
	u += "uniform vec3 u_local_light_position = vec3(0.0);\n";
	u += "uniform vec3 u_local_light_color = vec3(1.0);\n";
	u += "uniform float u_local_light_intensity = 1.0;\n";
	u += "uniform float u_local_light_atten_start = 0.0;\n";
	u += "uniform float u_local_light_atten_end = 5.0;\n";

	u += "varying vec2 v_uv;\n";
	u += "varying vec2 v_uv2;\n";
	u += "varying vec3 v_world_pos;\n";
	u += "varying vec3 v_world_normal;\n";
	u += "varying vec3 v_geom_normal;\n";
	if (has_flag(key, OSCAP_NORMAL_MAP)) {
		u += "varying vec3 v_world_tangent;\n";
		u += "varying vec3 v_world_binormal;\n";
	}

	// Full D3D COUNT2 affine transform. The U and V coefficient vectors carry
	// {m00,m10,m20} and {m01,m11,m21}, preserving SET and shear as well as
	// scroll/scale/center rotation. [orig: compute_uv_transform_matrix @ 0x5B1990]
	u += "vec2 obj_transform_uv(vec2 uv) {\n";
	u += "\tvec3 uv1 = vec3(uv, 1.0);\n";
	u += "\treturn vec2(dot(uv1, u_uv_transform_u), dot(uv1, u_uv_transform_v));\n";
	u += "}\n\n";

	// The FF hemisphere: AmbientColor = (sky + ground)/2 applied as the
	// material emissive, plus two opposing directional lights carrying the
	// DELTA colors (sky - ambient downward, ground - ambient upward) — which
	// is exactly mix(ground, sky, n.y * 0.5 + 0.5)
	// [orig: RenderBatchCtx_StoreLightingConstants @ 0x5d89e0 (the averages);
	//  Lighting_SetHemisphereD3DLights @ 0x5d8cb0 (the delta lights)].
	u += "vec3 obj_hemi(vec3 n) {\n";
	u += "\tfloat up = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);\n";
	u += "\treturn mix(u_hemi_ground_color, u_hemi_sky_color, up);\n";
	u += "}\n\n";

	// The fixed-function lit combine: the D3D vertex diffuse saturates
	// (hemi + directional clamped to 1), then the output stage is
	// MODULATE2X(Texture, Diffuse) — texture x diffuse x 2
	// [orig: _FFP.fx TBoringFFP TSSColor(0, Modulate2x, Texture, Diffuse);
	//  D3D light 0 setup @ 0x5d9ce2..0x5d9d76].
	u += "vec3 obj_ff_lighting(vec3 base_rgb, vec3 normal_ws) {\n";
	u += "\tvec3 N = normalize(normal_ws);\n";
	u += "\tvec3 L = normalize(-u_dir_light_dir);\n";
	u += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
	u += "\treturn base_rgb * min(obj_hemi(N) + u_dir_light_color * ndotl, vec3(1.0)) * 2.0;\n";
	u += "}\n\n";

	// Editor-preview LGHT contribution: returns additive RGB from
	// u_local_light_*.
	// Returns zero when no local light is bound (count==0) or the surface is
	// outside the attenuation range. Linear falloff between atten_start and
	// atten_end is an authoring-preview approximation, not a gameplay path.
	u += "vec3 obj_local_light_contrib(vec3 base_rgb, vec3 normal_ws, vec3 world_pos) {\n";
	u += "\tif (u_local_light_count <= 0) return vec3(0.0);\n";
	u += "\tvec3 to_light = u_local_light_position - world_pos;\n";
	u += "\tfloat dist = length(to_light);\n";
	u += "\tif (dist >= u_local_light_atten_end) return vec3(0.0);\n";
	u += "\tfloat range = max(u_local_light_atten_end - u_local_light_atten_start, 0.001);\n";
	u += "\tfloat atten = 1.0 - clamp((dist - u_local_light_atten_start) / range, 0.0, 1.0);\n";
	u += "\tvec3 N = normalize(normal_ws);\n";
	u += "\tvec3 L = to_light / max(dist, 0.001);\n";
	u += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
	u += "\treturn base_rgb * u_local_light_color * (u_local_light_intensity * ndotl * atten);\n";
	u += "}\n\n";

	// The witnessed device fog table (one text with terrain_lighting.gdshaderinc
	// / water.gdshader): type 0 = exponential density ln(64)/end; types 1/2/3 =
	// linear with start = caller 0.5 / (1-density)*end*0.5 / (1-density)*end*0.25
	// (density = overcast, 0 in the ported scope - the ends fold to end/2, end/4)
	// [orig: Render_SetFogState @ 0x58a950 -> CD3DDevice_SetFogParameters
	// @ 0x677960; env-tod-re.md "Fog policy"]. Replaces the pre-witness linear
	// ramp + smoothstep (D-RMAT-9).
	u += "float obj_fog_visibility(float dist, float fog_start, float fog_end, int fog_type) {\n";
	u += "\tfloat safe_end = max(fog_end, 1.0);\n";
	u += "\tif (fog_type == 0) {\n";
	u += "\t\treturn clamp(exp(-max(dist, 0.0) * (4.1588830833596715 / safe_end)), 0.0, 1.0);\n";
	u += "\t}\n";
	u += "\tfloat start = fog_start;\n";
	u += "\tif (fog_type == 2) {\n";
	u += "\t\tstart = safe_end * 0.5;\n";
	u += "\t} else if (fog_type == 3) {\n";
	u += "\t\tstart = safe_end * 0.25;\n";
	u += "\t}\n";
	u += "\tfloat fog_range = max(safe_end - start, 1.0);\n";
	u += "\treturn clamp((safe_end - dist) / fog_range, 0.0, 1.0);\n";
	u += "}\n\n";

	// The gamma-space output convention (D-RMAT-7): the witnessed math above
	// runs on raw gamma-encoded values like the original device; this exact
	// inverse of the reimpl's sRGB-encoding blit makes the displayed byte equal
	// the computed gamma-space byte. Body identical to
	// godot/shaders/nova_color.gdshaderinc (the witness lives there and in
	// render-material-re.md; the composer embeds it so generated shaders stay
	// self-contained). Retail saturates at the byte framebuffer, so the clamp
	// to [0,1] is itself witnessed behavior.
	u += "vec3 nova_gamma_to_linear(vec3 gamma_rgb) {\n";
	u += "\tvec3 c = clamp(gamma_rgb, vec3(0.0), vec3(1.0));\n";
	u += "\treturn mix(pow((c + vec3(0.055)) * (1.0 / 1.055), vec3(2.4)),\n";
	u += "\t\t\tc * (1.0 / 12.92), lessThan(c, vec3(0.04045)));\n";
	u += "}\n\n";

	return u;
}

std::string compose_vertex(ObjectShaderKey key) {
	const ObjectShaderFamily family = decode_object_shader_family(key);
	std::string v;
	v += "void vertex() {\n";
	v += "\tv_uv = obj_transform_uv(UV);\n";
	v += "\tv_uv2 = obj_transform_uv(UV2);\n";

	if (family == ObjectShaderFamily::Flag) {
		v += "\tvec3 local_pos = VERTEX;\n";
		v += "\tfloat angle = TIME * 8.0 + u_wind_phase + VERTEX.x * 3.0 + VERTEX.y * 0.8;\n";
		v += "\tfloat sa = sin(angle);\n";
		v += "\tlocal_pos.z += sa * min(VERTEX.x, 1.0) * (0.15 * u_wind_amount);\n";
		v += "\tVERTEX = local_pos;\n";
		v += "\tv_world_pos = (MODEL_MATRIX * vec4(local_pos, 1.0)).xyz;\n";
	} else {
		v += "\tv_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;\n";
	}

	v += "\tv_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);\n";
	v += "\tv_geom_normal = v_world_normal;\n";
	if (has_flag(key, OSCAP_NORMAL_MAP)) {
		v += "\tv_world_tangent = normalize((MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz);\n";
		v += "\tv_world_binormal = normalize((MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz);\n";
	}
	v += "}\n\n";
	return v;
}

std::string compose_fragment(ObjectShaderKey key) {
	const ObjectShaderFamily family = decode_object_shader_family(key);
	const bool emissive = has_flag(key, OSCAP_EMISSIVE);
	const bool luminance = has_flag(key, OSCAP_LUMINANCE);
	const uint32_t blend = static_cast<uint32_t>(decode_object_shader_blend(key));
	// ALPHA is only written for AlphaBlend (blend == 1).  Mirrors canonical at
	// nova_shader_cache.cpp:338 (`needs_alpha = blend == 1`).  Touching ALPHA
	// for any non-AlphaBlend material can cause Godot to auto-classify the
	// shader as transparent and push it into the wrong render pass.
	const bool needs_alpha = blend == 1;

	std::string f;
	const char *normal_uv = has_flag(key, OSCAP_NORMAL_UV2) ? "v_uv2" : "v_uv";
	f += "void fragment() {\n";
	f += "\tvec4 base = texture(u_diffuse, v_uv);\n";
	if (has_flag(key, OSCAP_DETAIL)) {
		// The _MT second texture stage: Modulate2x on color (an avg-128 gray
		// detail map is neutral through the x2), plain Modulate on alpha —
		// gamma-space bytes per D-RMAT-7, saturation at the output clamp.
		// Samples the second UV set: the .3di vertex carries two, and MT
		// models author a distinct uv1 for the detail map (v_uv2 <- UV2).
		// [orig: _FFP.fx TECHNIQUE_NORMAL _MT stage 1 -
		//  TSSColor(1, Modulate2x, Texture, Current),
		//  TSSAlpha(1, Modulate, Texture, Current);
		//  render-material-re.md §FF technique tables]
		f += "\tvec4 detail = texture(u_detail, v_uv2);\n";
		f += "\tbase.rgb *= detail.rgb * 2.0;\n";
		f += "\tbase.a *= detail.a;\n";
	}
	f += "\tbase.rgb *= u_rgb_mod;\n";
	f += "\tbase.a *= u_alpha_mod;\n";

	if (has_flag(key, OSCAP_ALPHA_TEST)) {
		// The original keeps a > ref (D3DCMP_GREATER); the invert flag flips
		// the COMPARE to a <= ref, not the sampled value.
		// [orig: CRenderBatchQueue_FlushBatches @ 0x5da3a9..0x5da401 ->
		//  CGfxDevice_SetAlphaTestRef @ 0x6770a0]
		f += "\tif (u_alpha_test_invert > 0.5) {\n";
		f += "\t\tif (base.a > u_alpha_test_threshold) discard;\n";
		f += "\t} else {\n";
		f += "\t\tif (base.a <= u_alpha_test_threshold) discard;\n";
		f += "\t}\n";
	}

	f += "\tvec3 view_dir = normalize(CAMERA_POSITION_WORLD - v_world_pos);\n";
	f += "\tvec3 surface_normal = normalize(v_world_normal);\n";
	f += "\tvec3 geom_normal = normalize(v_geom_normal);\n";

	if (has_flag(key, OSCAP_NORMAL_MAP)) {
		if (has_flag(key, OSCAP_OBJECT_SPACE)) {
			f += "\tvec3 ns = texture(u_normal_map, ";
			f += normal_uv;
			f += ").xyz * 2.0 - 1.0;\n";
			f += "\tns = normalize(vec3(-ns.x, ns.y, ns.z));\n";
			f += "\tsurface_normal = normalize((MODEL_MATRIX * vec4(ns, 0.0)).xyz);\n";
		} else {
			f += "\tvec3 ns = texture(u_normal_map, ";
			f += normal_uv;
			f += ").xyz * 2.0 - 1.0;\n";
			f += "\tsurface_normal = normalize(\n";
			f += "\t\tns.x * normalize(v_world_tangent) +\n";
			f += "\t\tns.y * normalize(v_world_binormal) +\n";
			f += "\t\tns.z * geom_normal);\n";
		}
	}

	f += "\tvec3 lit = base.rgb;\n";
	f += "\tfloat alpha = base.a;\n";

	if (has_flag(key, OSCAP_VIEW_FADE)) {
		// vsTracer: unlit, color x |dot(eye, normal)|^2 — the facing-angle
		// falloff that fades a well-tessellated tube toward its silhouette
		// edges ("soft edges"); no lighting includes, Spec = 0. Evaluated
		// per-fragment here (the original computes lum per-vertex in vs_1_1
		// and interpolates — same formula). Tracer modulates 1x, not 2x
		// [orig: Tracer.fx vsTracer; TSSColor MODULATE(Texture, Diffuse)].
		f += "\tfloat vf = abs(dot(geom_normal, view_dir));\n";
		f += "\tlit = base.rgb * (vf * vf);\n";
	} else if (emissive || luminance) {
		// SELFLUM: the material emissive becomes SelfLumColor x
		// ColorSrcGlobalGain (the modulator /64 — the iris exposure reaching
		// self-lit surfaces) with black diffuse/ambient, still under the
		// MODULATE2X output stage. The vertex color saturates before the x2
		// [orig: _FFP.fx SELFLUM variant; gain bind @ 0x58e05d ->
		//  Render_LightScaleRGB @ 0x8409f4].
		f += "\tlit = base.rgb * min(u_color_src_global_gain, vec3(1.0)) * 2.0;\n";
	} else if (family == ObjectShaderFamily::Flag) {
		// Two-sided cloth: light the camera-facing side (the reimpl form of the
		// cull-none FF draw); the lighting model is the standard FF combine.
		f += "\tvec3 nfacing = surface_normal;\n";
		f += "\tif (dot(view_dir, geom_normal) < 0.0) nfacing = -nfacing;\n";
		f += "\tlit = obj_ff_lighting(base.rgb, nfacing);\n";
	} else if (family == ObjectShaderFamily::Glass) {
		// Glass NORMAL technique: the FF lit base plus the environment-cube
		// reflection scaled by ReflectColor. The live scene cube
		// (Render_CubeEnvironmentTexture, re-rendered every 128 frames
		// [orig: update_environment_cubemap @ 0x6106a0]) is not hosted; its
		// dominant content — sky above, ground below — stands in via the
		// hemisphere sampled along the reflected view (tracked, D-RLIT-5).
		f += "\tvec3 N = surface_normal;\n";
		f += "\tvec3 ff_lit = obj_ff_lighting(base.rgb, N);\n";
		f += "\tvec3 refl_dir = reflect(-view_dir, N);\n";
		f += "\tvec3 env = obj_hemi(refl_dir) * 2.0;\n";
		f += "\tfloat fresnel = pow(1.0 - max(dot(N, view_dir), 0.0), 2.0);\n";
		f += "\tlit = mix(ff_lit, env * u_reflect_color.rgb, clamp(u_reflect_color.a + fresnel * (1.0 - u_reflect_color.a), 0.0, 1.0));\n";
		f += "\talpha = clamp(max(base.a, 0.35), 0.0, 1.0);\n";
	} else if (family == ObjectShaderFamily::Phong ||
	           family == ObjectShaderFamily::Environment) {
		f += "\tvec3 N = surface_normal;\n";
		f += "\tvec3 L = normalize(-u_dir_light_dir);\n";
		f += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
		f += "\tlit = obj_ff_lighting(base.rgb, N);\n";
		if (has_flag(key, OSCAP_SPECULAR)) {
			// The VS_PHONG* specular is a PhongMap texture lookup along the
			// reflection vector; the lobe content is unwitnessed — a pow-16
			// half-vector lobe in the witnessed light color stands in
			// (tracked, D-RLIT-5).
			f += "\tif (ndotl > 0.0) {\n";
			f += "\t\tvec3 H = normalize(L + view_dir);\n";
			f += "\t\tfloat spec = pow(max(dot(N, H), 0.0), 16.0);\n";
			f += "\t\tlit += u_dir_light_color * spec;\n";
			f += "\t}\n";
		}
		if (family == ObjectShaderFamily::Environment) {
			// Env-mapped surfaces sample the scene cube; the hemisphere along
			// the reflected view stands in for the unhosted cube (D-RLIT-5).
			f += "\tfloat fresnel = pow(1.0 - max(dot(N, view_dir), 0.0), 3.0);\n";
			f += "\tlit = mix(lit, obj_hemi(reflect(-view_dir, N)) * 2.0, fresnel * 0.35);\n";
		}
	} else if (family == ObjectShaderFamily::Dot3) {
		f += "\tlit = obj_ff_lighting(base.rgb, surface_normal);\n";
	} else {
		f += "\tlit = obj_ff_lighting(base.rgb, surface_normal);\n";
	}

	// Layer in the opt-in editor LGHT preview before the emissive override
	// (LUM / emissive variants intentionally bypass lighting). Gameplay leaves
	// u_local_light_count at zero.
	f += "\tif (u_emissive < 0.5) lit += obj_local_light_contrib(base.rgb, surface_normal, v_world_pos);\n";
	f += "\tif (u_emissive > 0.5) lit = base.rgb * min(u_color_src_global_gain, vec3(1.0)) * 2.0;\n";
	f += "\tif (u_fog_enabled) {\n";
	f += "\t\tfloat fog_visibility = obj_fog_visibility(distance(CAMERA_POSITION_WORLD, v_world_pos), u_fog_start, u_fog_end, u_fog_type);\n";
	f += "\t\tlit = mix(u_fog_color, lit, fog_visibility);\n";
	f += "\t}\n";
	// Gamma-space output (D-RMAT-7): `lit` is the witnessed gamma-space result;
	// encode it so the reimpl blit displays exactly those bytes.
	f += "\tALBEDO = nova_gamma_to_linear(lit);\n";
	if (needs_alpha) {
		f += "\tALPHA = clamp(alpha, 0.0, 1.0);\n";
	}
	// Otherwise: do not touch ALPHA; Godot's default ALPHA = 1.0 keeps
	// the surface in the opaque pass.
	f += "}\n";
	return f;
}

} // namespace

ObjectShaderKey build_object_shader_key(const ObjectMaterialClassification &cls) {
	ObjectShaderKey key = 0;
	switch (cls.blend) {
		case ObjectBlendMode::Opaque: key |= 0; break;
		case ObjectBlendMode::AlphaBlend: key |= 1; break;
		case ObjectBlendMode::Additive: key |= 2; break;
		case ObjectBlendMode::Multiplicative: key |= 3; break;
	}
	key |= encode_family(cls.family);
	if (cls.is_two_sided) key |= OSCAP_TWO_SIDED;
	if (cls.alpha_test) key |= OSCAP_ALPHA_TEST;
	if (cls.alpha_test_invert) key |= OSCAP_ALPHA_INVERT;
	if (cls.is_emissive) key |= OSCAP_EMISSIVE;
	if (cls.is_luminance) key |= OSCAP_LUMINANCE;
	if (cls.needs_normal_map) key |= OSCAP_NORMAL_MAP;
	if (cls.normal_uses_uv2) key |= OSCAP_NORMAL_UV2;
	if (cls.normal_space == ObjectNormalSpace::Object) key |= OSCAP_OBJECT_SPACE;
	if (cls.has_detail) key |= OSCAP_DETAIL;
	if (cls.uses_specular) key |= OSCAP_SPECULAR;
	if (cls.is_glass) key |= OSCAP_GLASS;
	if (cls.view_angle_fade) key |= OSCAP_VIEW_FADE;
	return key;
}

ObjectShaderFamily decode_object_shader_family(ObjectShaderKey key) {
	return static_cast<ObjectShaderFamily>(
		(key & OSCAP_FAMILY_MASK) >> OSCAP_FAMILY_SHIFT);
}

ObjectBlendMode decode_object_shader_blend(ObjectShaderKey key) {
	return static_cast<ObjectBlendMode>(key & OSCAP_BLEND_MASK);
}

std::string compose_object_shader_glsl(ObjectShaderKey key) {
	std::string code;
	code += compose_render_mode(key);
	code += compose_uniforms(key);
	code += compose_vertex(key);
	code += compose_fragment(key);
	return code;
}

} // namespace renderer
