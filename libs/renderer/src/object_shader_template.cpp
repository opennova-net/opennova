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
	u += "uniform sampler2D u_diffuse : source_color, filter_linear_mipmap, repeat_enable;\n";
	if (has_flag(key, OSCAP_DETAIL))
		u += "uniform sampler2D u_detail : source_color, filter_linear_mipmap, repeat_enable;\n";
	if (has_flag(key, OSCAP_NORMAL_MAP))
		u += "uniform sampler2D u_normal_map : hint_normal, filter_linear_mipmap, repeat_enable;\n";

	u += "uniform vec2 u_uv_offset = vec2(0.0);\n";
	u += "uniform vec2 u_uv_scale = vec2(1.0);\n";
	u += "uniform float u_uv_rotation = 0.0;\n";
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

	u += "uniform vec3 u_ambient_color = vec3(0.35, 0.36, 0.40);\n";
	u += "uniform vec3 u_dir_light_dir = vec3(-0.4082, -0.8165, -0.4082);\n";
	u += "uniform vec3 u_dir_light_color = vec3(0.85, 0.82, 0.75);\n";
	u += "uniform vec3 u_fill_light_color = vec3(0.18, 0.20, 0.25);\n";
	u += "uniform bool u_fog_enabled = false;\n";
	u += "uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);\n";
	u += "uniform float u_fog_start = 0.0;\n";
	u += "uniform float u_fog_end = 1024.0;\n";
	u += "uniform int u_fog_type = 0;\n";
	u += "uniform float u_wind_amount = 0.5;\n";
	u += "uniform float u_wind_phase = 0.0;\n";

	// LGHT (per-model local lights).  Single-light prototype: the editor
	// picks the most-intense LGHT entry and pushes its world-space position
	// + colour + attenuation range every frame.  N4.1 will widen this to
	// uniform vec3 arrays for multi-light support.  Set u_local_light_count=0
	// to disable (default).
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

	u += "vec2 obj_transform_uv(vec2 uv) {\n";
	u += "\tvec2 out_uv = (uv - vec2(0.5)) * u_uv_scale;\n";
	u += "\tfloat c = cos(u_uv_rotation);\n";
	u += "\tfloat s = sin(u_uv_rotation);\n";
	u += "\tout_uv = vec2(c * out_uv.x - s * out_uv.y, s * out_uv.x + c * out_uv.y);\n";
	u += "\treturn out_uv + vec2(0.5) + u_uv_offset;\n";
	u += "}\n\n";

	u += "vec3 obj_hemi_fill(vec3 n) {\n";
	u += "\tfloat up = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);\n";
	u += "\treturn mix(u_fill_light_color, u_ambient_color, up);\n";
	u += "}\n\n";

	u += "vec3 obj_ff_lighting(vec3 base_rgb, vec3 normal_ws) {\n";
	u += "\tvec3 N = normalize(normal_ws);\n";
	u += "\tvec3 L = normalize(-u_dir_light_dir);\n";
	u += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
	u += "\treturn base_rgb * (obj_hemi_fill(N) + u_dir_light_color * ndotl);\n";
	u += "}\n\n";

	// Local-light contribution: returns the additive RGB from u_local_light_*.
	// Returns zero when no local light is bound (count==0) or the surface is
	// outside the attenuation range.  Linear falloff between atten_start and
	// atten_end mirrors the original tool's NEAR/FAR attenuation pair.
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

	u += "float obj_fog_visibility(float dist, float fog_start, float fog_end, int fog_type) {\n";
	u += "\tfloat span = max(fog_end - fog_start, 0.001);\n";
	u += "\tfloat t = clamp((dist - fog_start) / span, 0.0, 1.0);\n";
	u += "\tif (fog_type == 3) t = smoothstep(0.0, 1.0, t);\n";
	u += "\treturn 1.0 - t;\n";
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
		f += "\tvec4 detail = texture(u_detail, v_uv2);\n";
		f += "\tbase.rgb *= detail.rgb;\n";
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

	if (emissive || luminance) {
		f += "\tlit = base.rgb;\n";
	} else if (family == ObjectShaderFamily::Flag) {
		f += "\tvec3 nfacing = surface_normal;\n";
		f += "\tif (dot(view_dir, geom_normal) < 0.0) nfacing = -nfacing;\n";
		f += "\tvec3 N = normalize(nfacing);\n";
		f += "\tvec3 L = normalize(-u_dir_light_dir);\n";
		f += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
		f += "\tlit = base.rgb * (obj_hemi_fill(N) + u_dir_light_color * ndotl) * 1.5;\n";
	} else if (family == ObjectShaderFamily::Glass) {
		f += "\tvec3 N = surface_normal;\n";
		f += "\tvec3 L = normalize(-u_dir_light_dir);\n";
		f += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
		f += "\tvec3 ff_lit = base.rgb * (max(obj_hemi_fill(N), vec3(0.2)) + u_dir_light_color * ndotl);\n";
		f += "\tfloat fresnel = pow(1.0 - max(dot(N, view_dir), 0.0), 2.0);\n";
		f += "\tvec3 env = mix(u_ambient_color, u_dir_light_color, 0.4);\n";
		f += "\tlit = mix(ff_lit, env * u_reflect_color.rgb, clamp(0.4 + fresnel * 0.6, 0.0, 1.0));\n";
		f += "\talpha = clamp(max(base.a, 0.35), 0.0, 1.0);\n";
	} else if (family == ObjectShaderFamily::Phong ||
	           family == ObjectShaderFamily::Environment) {
		f += "\tvec3 N = surface_normal;\n";
		f += "\tvec3 L = normalize(-u_dir_light_dir);\n";
		f += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
		f += "\tlit = base.rgb * obj_hemi_fill(N);\n";
		f += "\tlit += base.rgb * u_dir_light_color * ndotl * 1.6;\n";
		if (has_flag(key, OSCAP_SPECULAR)) {
			f += "\tif (ndotl > 0.0) {\n";
			f += "\t\tvec3 H = normalize(L + view_dir);\n";
			f += "\t\tfloat spec = pow(max(dot(N, H), 0.0), 16.0);\n";
			f += "\t\tlit += u_dir_light_color * spec * 0.8;\n";
			f += "\t}\n";
		}
		if (family == ObjectShaderFamily::Environment) {
			f += "\tfloat fresnel = pow(1.0 - max(dot(N, view_dir), 0.0), 3.0);\n";
			f += "\tlit = mix(lit, u_dir_light_color * 1.2, fresnel * 0.35);\n";
		}
	} else if (family == ObjectShaderFamily::Dot3) {
		f += "\tvec3 N = surface_normal;\n";
		f += "\tvec3 L = normalize(-u_dir_light_dir);\n";
		f += "\tfloat ndotl = max(dot(N, L), 0.0);\n";
		f += "\tlit = base.rgb * (obj_hemi_fill(N) + u_dir_light_color * ndotl);\n";
	} else {
		f += "\tlit = obj_ff_lighting(base.rgb, surface_normal);\n";
	}

	// Layer in any LGHT contribution before the emissive override (LUM /
	// emissive variants intentionally bypass lighting and shouldn't be
	// brightened by local point lights).
	f += "\tif (u_emissive < 0.5) lit += obj_local_light_contrib(base.rgb, surface_normal, v_world_pos);\n";
	f += "\tif (u_emissive > 0.5) lit = base.rgb;\n";
	f += "\tif (u_fog_enabled) {\n";
	f += "\t\tfloat fog_visibility = obj_fog_visibility(distance(CAMERA_POSITION_WORLD, v_world_pos), u_fog_start, u_fog_end, u_fog_type);\n";
	f += "\t\tlit = mix(u_fog_color, lit, fog_visibility);\n";
	f += "\t}\n";
	f += "\tALBEDO = max(lit, vec3(0.0));\n";
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
