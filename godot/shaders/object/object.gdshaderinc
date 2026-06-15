// Object-material macro über-shader body. The cache (NovaObjectShaderCache)
// prepends `shader_type`/`render_mode` + a block of `#define OBJ_*` per material
// key, then includes this file. The OBJ_FAMILY_* / OBJ_* defines select the
// per-family combine — the same macro-permutation model the original effects use
// (one `_BaseInc.fx` + per-technique bodies). See docs/renderer/renderer-re.md.
//
// Faithful so far: FixedFunction / Dot3 / Phong (bumped diffuse + hemisphere +
// gloss-gated specular, all ×2), Flag (witnessed wind), self-illum bypass.
// Approximate, pending the param-feed grill (task in renderer-re.md open
// questions): Glass / Environment cube reflection (D-RENDER-10), the ≤4-light
// array (D-RENDER-9), the 3x2 UV matrix (D-RENDER-8).

#include "res://shaders/object/_object_baseinc.gdshaderinc"

varying vec2 v_uv;
varying vec2 v_uv2;
varying vec3 v_world_pos;
varying vec3 v_world_normal;
varying vec3 v_geom_normal;
varying float v_view_z;
#ifdef OBJ_NORMAL_MAP
varying vec3 v_world_tangent;
varying vec3 v_world_binormal;
#endif

void vertex() {
	v_uv = obj_transform_uv(UV);
	v_uv2 = obj_transform_uv(UV2);

#ifdef OBJ_FAMILY_FLAG
	// Cloth wave [orig: vsFlag in Flag.fx]: angle = FloatTicks*8 + x*3 + y*0.8,
	// z += sin(angle)*min(x,1)*0.15. u_wind_amount scales the displacement.
	vec3 local_pos = VERTEX;
	float angle = TIME * 8.0 + u_wind_phase + VERTEX.x * 3.0 + VERTEX.y * 0.8;
	local_pos.z += sin(angle) * min(VERTEX.x, 1.0) * (0.15 * u_wind_amount);
	VERTEX = local_pos;
	v_world_pos = (MODEL_MATRIX * vec4(local_pos, 1.0)).xyz;
#else
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
#endif

	v_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	v_geom_normal = v_world_normal;
	v_view_z = -(MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).z;

#ifdef OBJ_NORMAL_MAP
	v_world_tangent = normalize((MODEL_MATRIX * vec4(TANGENT, 0.0)).xyz);
	v_world_binormal = normalize((MODEL_MATRIX * vec4(BINORMAL, 0.0)).xyz);
#endif
}

void fragment() {
	vec4 base = texture(u_diffuse, v_uv);
#ifdef OBJ_DETAIL
	base.rgb *= texture(u_detail, v_uv2).rgb;
#endif
	base.rgb *= u_rgb_mod;
	base.a *= u_alpha_mod;

#ifdef OBJ_ALPHA_TEST
	float av = (u_alpha_test_invert > 0.5) ? (1.0 - base.a) : base.a;
	if (av < u_alpha_test_threshold) {
		discard;
	}
#endif

	vec3 view_dir = normalize(CAMERA_POSITION_WORLD - v_world_pos);
	vec3 geom_n = normalize(v_geom_normal);
	vec3 N = normalize(v_world_normal);

#ifdef OBJ_NORMAL_MAP
	#ifdef OBJ_NORMAL_UV2
	vec2 nuv = v_uv2;
	#else
	vec2 nuv = v_uv;
	#endif
	vec3 ns = texture(u_normal_map, nuv).xyz * 2.0 - 1.0;
	#ifdef OBJ_OBJECT_SPACE
	ns = normalize(vec3(-ns.x, ns.y, ns.z));
	N = normalize((MODEL_MATRIX * vec4(ns, 0.0)).xyz);
	#else
	N = normalize(ns.x * normalize(v_world_tangent)
		+ ns.y * normalize(v_world_binormal)
		+ ns.z * geom_n);
	#endif
#endif

	vec3 lit;

#if defined(OBJ_LUMINANCE) || defined(OBJ_EMISSIVE)
	// Self-illum (_LUM / emissive): MaterialEmissive path, still doubled.
	// [orig: SELFLUM branch in _FFP.fx] (D-RENDER-11)
	lit = base.rgb * OBJ_OVERBRIGHT;
#elif defined(OBJ_FAMILY_GLASS)
	// Flat cube-environment glass. Real reflection needs the environment cube
	// (D-RENDER-10); approximated here as reflect-tint + fresnel, additive ×2.
	// [orig: TGlassFFP in Glass.fx]
	float fr = pow(1.0 - max(dot(N, view_dir), 0.0), 2.0);
	lit = base.rgb * u_reflect_color.rgb * (0.5 + 0.5 * fr) * OBJ_OVERBRIGHT;
#else
	vec3 L = normalize(-u_dir_light_dir);
	float ndotl = max(dot(N, L), 0.0);

	// Directional diffuse with self-shadow wrap (bumped families only — the FFP
	// path uses plain D3D vertex N.L). [orig: ps11Diffuse / CalcSelfShadowTerm]
	#ifdef OBJ_NORMAL_MAP
	float ss = obj_self_shadow(dot(geom_n, L));
	#else
	float ss = 1.0;
	#endif
	vec3 dir_term = u_dir_light_color * ss * ndotl;
	vec3 hemi = obj_hemisphere(N);

	// out = 2 * tex * (dir + hemi). [orig: ps11Diffuse in _psDiff.fx]
	lit = base.rgb * (dir_term + hemi) * OBJ_OVERBRIGHT;

	#ifdef OBJ_SPECULAR
	// (N.H)^8 gloss gated by diffuse alpha, added ×2. [orig: ps11Phong in _psPhong.fx]
	if (ndotl > 0.0) {
		vec3 H = normalize(L + view_dir);
		float sp = max(dot(N, H), 0.0);
		sp = sp * sp;
		sp = sp * sp;
		sp = sp * sp;
		lit += u_dir_light_color * (sp * base.a) * OBJ_OVERBRIGHT;
	}
	#endif

	#ifdef OBJ_FAMILY_ENVIRONMENT
	// Extra reflection lobe (approx; cube env pending, D-RENDER-10).
	// [orig: TSegTanEnvPhong P2 in EnvPhongT.fx]
	float efr = pow(1.0 - max(dot(N, view_dir), 0.0), 3.0);
	lit = mix(lit, u_reflect_color.rgb * OBJ_OVERBRIGHT, efr * 0.35);
	#endif

	// Additive point light, ×2 like the per-light pass. [orig: ps11DiffuseSingle]
	lit += obj_point_light(base.rgb, N, v_world_pos) * OBJ_OVERBRIGHT;

	// Runtime emissive override (per-material toggle separate from the _LUM tag).
	if (u_emissive > 0.5) {
		lit = base.rgb * OBJ_OVERBRIGHT;
	}
#endif

	lit = obj_apply_fog(lit, v_view_z);
	ALBEDO = max(lit, vec3(0.0));

#ifdef OBJ_BLEND_ALPHABLEND
	// Only the AlphaBlend family writes opacity; leaving ALPHA at 1.0 keeps the
	// opaque/additive/mul families in their intended pass.
	ALPHA = clamp(base.a, 0.0, 1.0);
#endif
}
