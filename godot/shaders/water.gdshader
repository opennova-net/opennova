shader_type spatial;
// blend_premul_alpha with ALPHA = 1 - a expresses the witnessed framebuffer
// blend EXACTLY: retail's above-water surface draws with SrcBlend=ONE,
// DestBlend=SRCALPHA (blend mode 11 in decode_blend_mode_to_d3d_states
// @ 0x680f00) - out = src + dst * a - and Godot's premul pass computes
// out = src + dst * (1 - ALPHA). [orig: Water_InitSurfaceShaders @ 0x5c19b0
// builds Water_ShaderBlend with SrcBlend 2/DestBlend 5/enable 1]
// depth_draw_always = the witnessed water z-write: pass flags 0x30000/0x20000
// leave bit 0x100000 (zwrite-off) clear [orig: CGfxShader_ApplyPass @ 0x683232;
// RS14 commit @ 0x67e265], so retail water writes depth and later passes test
// against it.
render_mode unshaded, blend_premul_alpha, cull_disabled, depth_draw_always;

// Water surface: structural port of the witnessed render_water_surface pass
// [orig: @ 0x5c32c0, docs/env/env-tod-re.md "Water surface"]. The look is the
// HIGH-DETAIL strip model [orig: render_water_strip_detailed @ 0x5c27d0]:
// flat geometry, the animated 128x128 noise ridge texture + its DuDv map
// (regenerated per frame in engine/formats/env - Water_GenerateNoiseTextures
// @ 0x5c0360), per-vertex diffuse/alpha from the row color chain, and the
// shared scene fog. The screen-marched strip tessellation is LIVE (env #29):
// Water rebuilds the mesh per frame from WaterCore.strip_build, so
// COLOR carries the witnessed row diffuse (brightness x 0x10101, murk/
// distance alpha in A), CUSTOM1 the v1 specular register, CUSTOM0
// (depth, rhw, screen U, screen V) - the projective depth pair + the
// texm3x2 screen coordinates - and CUSTOM2 the texm3x2 perturbation basis
// (t1.xy, t2.xy). The reflection RTT consumer is LIVE too (env #30):
// Water owns the mirrored-scene SubViewport and the fragment below
// runs the witnessed texm3x2 lookup against it (the fog term rides the
// per-row spec-alpha factor — see v_fog). The LOW tier (render_water_strip
// @ 0x5c1d60) is unreachable at the shipped water detail.
//
// The water material set was decoded at REN-4 [orig: Water_InitSurfaceShaders
// @ 0x5c19b0; selection in render_water_surface @ 0x5c33e6..0x5c34ea]:
// - camera ABOVE water: the BLENDED material (ONE + dst*SRCALPHA, ported via
//   the premul trick above); camera BELOW (underwater view of the surface):
//   the OPAQUE material - ported as the u_underwater_view branch below
//   (blend-off = ALPHA 1.0 under the premul pass);
// - the FrameFX nightvision redraw (above water only) draws Water's
//   nightvision strip through the typed Q3 WaterNightVision technique;
// - all water passes are two-sided (pass flags 0x400000 -> cull NONE
//   [orig: GfxShader_ApplyPassChecked(shader, 0x400000) @ 0x5c340d;
//   CGfxShader_ApplyPass @ 0x683190]) = cull_disabled, and NO alpha test is
//   enabled: ALPHATESTENABLE rides pass-flag bit 0x40000, which every water
//   pass leaves CLEAR, so the state commit forces the test OFF
//   [orig: CGfxShader_ApplyPass @ 0x68326b -> RS15 commit in
//   CGfxDevice_ApplyRenderStates @ 0x67e2fc];
//   CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419 is a device latch
//   (ALPHAFUNC/ALPHAREF only) inert for water;
// - PS path (detail >= 2): the embedded ps.1.1 assembled in
//   Water_InitSurfaceShaders @ 0x5c19b0 —
//     tex t0 / texm3x2pad t1, t0_bx2 / texm3x2tex t2, t0_bx2 / tex t3
//     mov r0, t2 / mul_x2 r0.rgb, r0, v0 / mul_x4 r0.rgb, r0, t3
//     +mul_x2 r0.a, t3.a, v0.a / add r0.rgb, r0, v1
//   (t2 = the bump-perturbed reflection sample; the fragment below runs the
//   chain against Water's mirror-scene RTT, u_water_color standing in
//   only on reflection-less owners). FF fallback: s0 MOD2X(reflection,
//   diffuse), s1 MOD2X(noise, current), alpha MOD(noise.a, diffuse.a).

// Gamma-domain scene-target contract.
#include "res://shaders/color.gdshaderinc"
// Env_WaterColorLit — since env #30 this is only the reflection-LESS
// stand-in for the t2 sample (headless owners/tests, u_has_reflection false).
uniform vec3 u_water_color = vec3(0.408, 0.314, 0.224);
// The reflection RTT (env #30): Water's mirror-camera SubViewport
// texture, the chain's t2 [orig: texm3x2pad/texm3x2tex t2 —
// Water_InitSurfaceShaders @ 0x5c19b0; the mirror scene RTT
// render_main_scene @ 0x5c1240]. Sampled WITHOUT source_color on purpose:
// under the gamma-space convention (D-RMAT-7) the RTT's stored bytes are
// the display-encoded scene, consumed raw exactly like retail's t2 texels.
uniform sampler2D u_reflection : filter_linear;
uniform bool u_has_reflection = false;
// The witnessed texm3x2 rows are normalized against the MAIN viewport, while
// Godot derives the mirror camera's vertical projection from its square RTT.
// Water preserves horizontal FOV, so X is already registered and Y needs
// the source-to-square focal-scale ratio (source height / source width).
uniform vec2 u_reflection_uv_scale = vec2(1.0);
// water_murk [orig: SetWaterMurk @ 0x57d4f0] no longer has a uniform: the
// witnessed angle-weighted murk x distance chain arrives per vertex in
// COLOR.a now that the strip march is live (env #29) — the former flat
// (1 - u_water_murk) knob is retired (Water still sets the parameter for
// probe compatibility; a ShaderMaterial ignores unknown names).
uniform sampler2D u_noise_color : filter_linear, repeat_enable;
uniform sampler2D u_noise_normal : filter_linear, repeat_enable;
uniform vec3 u_fog_color = vec3(0.5, 0.6, 0.8);
// EnvironmentCubeCapture aliases water's otherwise exclusive visual layer so
// the six cameras can admit only sky + sun/moon without consuming a 21st Godot
// layer. Reject those six pass eyes by their exact camera mask — the capture
// cameras cull to the aliased layer ALONE (EnvironmentCubeCapture
// set_cull_mask), while the beauty/mirror masks are wider. IN_SHADOW_PASS guards the read:
// shadow passes leave CAMERA_VISIBLE_LAYERS unset.
const uint NOVA_ENVIRONMENT_CAPTURE_CAMERA_MASK = 1024u; // 0x00000400
// The underwater pass swaps to the OPAQUE material — blend OFF, flags 0x20000
// [orig: Water_ShaderOpaque @ 0x28ee8c8; selection @ 0x5c33e6..0x5c34ea]. Under
// the premul pass, ALPHA = 1.0 expresses blend-off exactly (out = src + dst*0);
// the ps.1.1 color chain is IDENTICAL on both materials (same 4-tex desc), so one
// shader serves both sides. The ladder position does not change by side (REN-3).
uniform bool u_underwater_view = false;
// The retail scene projection's near/far the strip depth is tested against
// (Water::_march_strip): near 0.2, far = the fog distance's integer word + 1.
uniform vec2 u_scene_depth_range = vec2(0.2, 1025.0);
// The scene viewport MaxZ, the strip depth's upper clamp (flt_7C4658 =
// 0x3F7FFE00 = 1 - 2^-15; engine kWaterStripDepthMax).
const float WATER_VIEWPORT_MAX_Z = 0.999969482421875;

varying vec3 v_world_pos;
// The ps.1.1 v1 specular register: WaterColorLit RGB under the row's
// distance alpha, raw bytes/255 like COLOR (no color-space transform),
// carried on the mesh as CUSTOM1 [orig: rows @ 0x5c2eb5..0x5c2ef4; consumed
// by add r0.rgb, r0, v1 in the detail>=2 pixel shader assembled in
// Water_InitSurfaceShaders @ 0x5c19b0].
varying vec3 v_specular;
// The texm3x2 row registers, reassembled per vertex from CUSTOM2 (the
// perturbation basis t1.xy / t2.xy — camera right/forward xz under the
// witnessed rhw scales) and CUSTOM0.zw (screen U / V):
// u-row = (t1.x, t1.y, source-screen U),
// v-row = (t2.x, t2.y, source-screen V)
// [orig: rows @ 0x5c2f83..0x5c3067; engine/formats/env WaterStripRows]. The underwater
// V-flip rides the rows themselves [orig: @ 0x5c306f..0x5c3085].
varying vec3 v_refl_row_u;
varying vec3 v_refl_row_v;
varying float v_fog;

void vertex() {
	v_world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_specular = CUSTOM1.rgb;
	v_refl_row_u = vec3(CUSTOM2.xy, CUSTOM0.z);
	v_refl_row_v = vec3(CUSTOM2.zw, CUSTOM0.w);
	// The witnessed water fog factor: D3D8 fogs pre-transformed (XYZRHW) verts by
	// the SPECULAR ALPHA, which the rows fill with dist_alpha (255 near -> 0 far)
	// [orig: RS28 on via pass bit 0x20000 @ 0x68324f; spec.a = dist_alpha
	// @ 0x5c2eb5..0x5c2ef4]. NOT the terrain fog table — the old
	// jointops_fog_factor curve here was a reimpl invention.
	v_fog = CUSTOM1.a;
	// The witnessed depth model: the pre-transformed strip carries its own
	// depth, an arithmetic replica of the scene projection with far = the fog
	// distance's INTEGER word w, z = clamp(scale - bias/t, 4e-5, 1 - 2^-15)
	// (CUSTOM0.x) [orig: depth chain @ 0x5c2c0c..0x5c2c4a; scale build
	// @ 0x5c3348; clamps flt_7DBF7C / flt_7C4658; near 0.2 @ 0x58ac0a, mission far
	// 1024 @ 0x524721, viewport depth 0.99996948 = 1 - 2^-15 @ 0x58ac32], against a
	// scene drawn at far = w + 1 (retail Render_ProcessMainSceneFrame
	// @ 0x5CA4BA..0x5CA4D0). The depth test compares those two curves, so the
	// water is written at the view depth whose retail scene-curve depth equals
	// the replica: slightly BEHIND its true depth (w < w + 1), and at the far
	// plane itself beyond t = w, where the clamp holds it — pre-transformed
	// rows are never far-clipped, so fully fogged water reaches the horizon.
	// That view depth then takes the drawing camera's own clip depth (view
	// z = -t, clip w = t: z / w = -P[2][2] + P[3][2] / t), so the test holds
	// whatever near plane the Godot camera carries. Perspective only; the
	// constant-W orthographic extension keeps its plain projection.
	vec3 view_pos = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	POSITION = PROJECTION_MATRIX * vec4(view_pos, 1.0);
	float scene_near = u_scene_depth_range.x;
	float scene_far = u_scene_depth_range.y;
	if (PROJECTION_MATRIX[2][3] != 0.0 && scene_far > scene_near) {
		float scene_z = CUSTOM0.x / WATER_VIEWPORT_MAX_Z;
		float inv_view_depth =
				(1.0 - scene_z * (scene_far - scene_near) / scene_far) / scene_near;
		float clip_depth = -PROJECTION_MATRIX[2][2] +
				PROJECTION_MATRIX[3][2] * inv_view_depth;
		POSITION.z = max(clip_depth, 0.0) * POSITION.w;
	}
}

void fragment() {
	if (!IN_SHADOW_PASS &&
			CAMERA_VISIBLE_LAYERS == NOVA_ENVIRONMENT_CAPTURE_CAMERA_MASK) {
		discard;
	}
	// The witnessed texcoords: t0 is the unprojected ABSOLUTE render-basis
	// world x/32, z/32 of each strip vertex (Godot world z/32, x/32), and t3
	// duplicates it (retail render_water_strip_detailed @ 0x5c2aec..0x5c2b00,
	// the t3 copy @ 0x5c3095..0x5c30bf). The pre-transformed XYZRHW strip
	// takes no texture transform, so both noise textures sample UV verbatim:
	// fixed in the world, animated only through the per-frame regenerated
	// noise. (flt_8412B0/B4 feed only the depth chain, and the cloud-scroll
	// "offsets" render_water_surface stores are never read back.)
	vec2 uv = UV;

	// Ridge intensity + its alpha mask (A = 255 - i*i/512, more opaque where
	// dark) [orig: Water_GenerateNoiseTextures pass 2 @ 0x5c04b7..0x5c0552].
	// u_noise_normal (the DuDv map, pass 3) is the chain's t0 — sampled at
	// the SAME coordinates: retail's texcoord 3 (color noise) duplicates
	// texcoord 0 (DuDv) [orig: @ 0x5c3095..0x5c30bf].
	vec4 noise = texture(u_noise_color, uv);

	// t2 = the reflection RTT, bump-perturbed by the witnessed texm3x2 pair
	// over the DuDv sample (t0): u = dot(t1row, t0_bx2),
	// v = dot(t2row, t0_bx2), _bx2 = c*2 - 1 [orig: tex t0 / texm3x2pad t1,
	// t0_bx2 / texm3x2tex t2, t0_bx2 — Water_InitSurfaceShaders @ 0x5c19b0;
	// the mirror scene RTT render_main_scene @ 0x5c1240]. The DuDv map's
	// blue byte is the constant 0xFF (engine/formats/env water_noise_normal_pixels),
	// so _bx2 passes the rows' screen component through at weight 1 while
	// R/G perturb it. The dots remain the witnessed lookup verbatim. Their
	// result is then mapped from the main viewport's normalized projection to
	// Godot's square mirror projection; without that sim/present-boundary conversion,
	// the reflected image swims vertically as the view rotates on a widescreen
	// display. The clamp is the RTT edge address mode.
	// Reflection-less owners (headless/tests) keep u_water_color as the
	// tracked t2 stand-in.
	vec3 t2_sample = u_water_color;
	if (u_has_reflection) {
		vec3 dudv_bx2 = texture(u_noise_normal, uv).rgb * 2.0 - 1.0;
		vec2 refl_uv = vec2(dot(v_refl_row_u, dudv_bx2), dot(v_refl_row_v, dudv_bx2));
		refl_uv = vec2(0.5) + (refl_uv - vec2(0.5)) * u_reflection_uv_scale;
		t2_sample = texture(u_reflection, clamp(refl_uv, vec2(0.0), vec2(1.0))).rgb;
	}

	// The witnessed detail>=2 ps.1.1 color chain [orig: assembled in
	// Water_InitSurfaceShaders @ 0x5c19b0]:
	//   mov r0, t2            ; t2 = the REFLECTION texture, bump-perturbed
	//   mul_x2 r0.rgb, r0, v0 ; x2 diffuse (COLOR.rgb = brightness x 0x10101,
	//                         ; 192*k near-grazing -> 38.4*k straight-down)
	//   mul_x4 r0.rgb, r0, t3 ; x4 color-noise
	//   add r0.rgb, r0, v1    ; + specular
	// staged below with per-write saturation like ps.1.1 register writes
	// (every input here is [0,1], so each write clamps at 1).
	vec3 color = clamp(t2_sample * COLOR.rgb * 2.0, 0.0, 1.0);
	color = clamp(color * noise.rgb * 4.0, 0.0, 1.0);
	color = clamp(color + v_specular, 0.0, 1.0);

	// alpha = t3.a x v0.a x2, saturated [orig: +mul_x2 r0.a, t3.a, v0.a —
	// Water_InitSurfaceShaders @ 0x5c19b0 ps.1.1]. COLOR.a carries the
	// angle-weighted murk term and the 255 - a^2/255 distance falloff,
	// computed per row in engine/formats/env [orig: alpha chain @ 0x5c2e22..0x5c2e9d;
	// written @ 0x5c2f0a..0x5c2f2b] — the shader's former dist^2 stand-in
	// and flat (1 - u_water_murk) knob both retired to it.
	float alpha_dist = clamp(noise.a * COLOR.a * 2.0, 0.0, 1.0);

	// NO alpha test: the REN-4 "ref-32 far-fade cutoff" reading was a misread.
	// D3DRS_ALPHATESTENABLE rides pass-flag bit 0x40000 [orig: CGfxShader_ApplyPass
	// @ 0x68326b -> RS15 commit in CGfxDevice_ApplyRenderStates @ 0x67e2fc], and the
	// water passes carry 0x30000 (blend) / 0x20000 (opaque) + 0x400000 — bit 0x40000
	// CLEAR, so the state commit forces the test OFF for every water draw.
	// CGfxDevice_SetAlphaTestRef(0x20) @ 0x5c3419/@ 0x5c3484 only latches
	// ALPHAFUNC/ALPHAREF on the device [orig: @ 0x6770a0 — no ALPHATESTENABLE write];
	// the far water renders to the full march extent, converging to the fog color.
	float alpha = alpha_dist;

	// Scene fog toward u_fog_color by the witnessed per-row factor (see v_fog).
	color = mix(u_fog_color, color, v_fog);

		// The witnessed blend is out = src + dst * a (ONE + dst*SRCALPHA): under
		// Godot's premul pass (out = src.rgb + dst * (1 - ALPHA)) that is
		// src.rgb = color * 1 with ALPHA = 1 - a. The source term itself is NOT
		// multiplied by a — retail's SrcBlend is ONE.
		// Both source and destination remain gamma-domain numeric values until the
		// terminal compositor, so this blend matches the retail framebuffer.
	ALBEDO = scene_output(color);
	ALPHA = u_underwater_view ? 1.0 : (1.0 - alpha);
}
