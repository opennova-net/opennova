// The per-frame celestial body state — the engine half of the shell's
// sun/moon/glare/glint applier. The witnessed fixed-point math (body alphas,
// glare window + hysteresis, kCelestialBodyDistance) is env_celestial.h's
// (engine/formats/env); this header owns the per-frame selection the applier
// writes into the bodies: placement and the UPL_INTENSITY submit value.
// Engine equivalents (docs/env/env-tod-re.md "Celestial bodies"):
// - [orig: EffectWorld_LoadCelestialModels @ 0x5adc50] resolves
//   sun_3di/moon_3di/star_3di/glare_3di; glare, star and upl load after
//   Model_SetNextLoadPassFlags(0x300000, 0), z-write off + ZFUNC ALWAYS (not
//   a blend mode). The star model is never drawn (env #33).
// - [orig: Render_CelestialBodies @ 0x5acaa0] places sun/moon at
//   camera + direction * 64 (full camera height, identity rotation) with the
//   witnessed overcast/SunDim (sun) and fog-distance (moon @ 0x5acc40)
//   alphas; the world overdraws them, so depth-tested materials are the
//   structural equivalent.
// - Every stock celestial model's parts pose VIEW-ALIGNED (PANM rotation
//   type 3: msun.3di 0x300, mglare.3di and Fmoon4.3di 0x301): the submit's
//   collect copies the pass's inverse view rotation into each such part and
//   scales it [orig: Model_TransformBoneMatrices @ 0x58eb48 (flag 0x100),
//   @ 0x58ec09 copies flt_27219C0, the inverse view that
//   Render_SetViewAndProjectionMatrices stores @ 0x58d947, then @ 0x58ec82
//   times diag(scale)], so each quad faces that pass's camera, the water
//   mirror's included [orig: Render_SceneWithWaterReflection @ 0x5d8124 sets
//   its own view]; the identity world rotation only places the model. The
//   embedder turns a body whose every part is type 3 by its main camera. The
//   mirror redraw keeps that turn (its own is not carried yet), and other
//   models' type 3/4 parts take none (divergence D-COL-10).
// - [orig: Render_SkyboxSunGlow @ 0x5acd00] drives the glare: a terrain gate
//   ray, then two jittered entity rays per frame into an 8-sample window +
//   hysteresis brightness, glow alpha = dot_view^4/2 x brightness x folds
//   (env #14; runtime/environment/glare_occlusion.h).
// - Every body renders through its AUTHORED material (the stock models are
//   FF_ST_AD_LUM): the submit alpha is not a blend factor but the value of
//   the global CTRL register UPL_INTENSITY (ordinal 32; the IDB's
//   g_CtrlGlobalUplIntensity @ 0x83fde8 = the register table dword_83FCE8 +
//   32 * 8), which the material's RgbGen style 113 reads into SelfLumColor
//   when the batch FLUSHES [orig: RgbGen_EvaluateColor @ 0x5b2453, called by
//   Material_ApplyShaderParameters @ 0x58ddfb from CRenderBatchQueue_FlushBatches].
#pragma once

#include <runtime/environment/environment_state.h>
#include <base/io/fixed.h>

#include <formats/env/env_celestial.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include <cmath>

namespace opennova::env {

// One call of the water-glint leg [orig: Environment_UpdateSunGlare @ 0x5ad130, once
// per scene pass from Terrain_RenderWorldScene @ 0x5c96c0: the main scene's,
// then the weapon Inset pass's at its own camera and view, on the same
// WaterGlintState (runtime/renderer/scene_overlay.h kInsetOverlayOrder)]:
// one sample per call, the frame index stepped first [orig:
// Environment_UpdateSunGlare @ 0x5ad1f5 (++dword_27E5694), masked
// @ 0x5ad200..0x5ad204, re-read @ 0x5ad25e]:
// - the reflected-sun point on the water, with the 0.25 * (frame & 3)
//   reflected-height jitter [orig: Water_ComputeReflectedSunPoint @ 0x5ac040
//   (the call @ 0x5ad21a; viewZ = (idx & 3) << 14 @ 0x5ad200..0x5ad20e)];
// - visible when that point sees BOTH the sun (point -> camera + sun * 2048)
//   and the camera over TERRAIN [orig: Terrain_RaycastLoResNoNormal @ 0x610860
//   (the calls @ 0x5ad234 / @ 0x5ad24e)];
// - then the point jittered +-2 in x and z [orig: Environment_UpdateSunGlare
//   @ 0x5ad25e..0x5ad27e] and the ENTITY ray from the camera to it [orig:
//   Physics_InitRayContext @ 0x5385e0 (the call @ 0x5ad296);
//   Physics_RaycastAgainstPoolByIndex @ 0x539530 (the calls @ 0x5ad2cb /
//   @ 0x5ad2e3 / @ 0x5ad2fb), pools 2, 1, 0];
// then the +-16 chase toward popcount * 64 and the mirrored glare submit at
// camera + sun * 128 with the height term negated.
// Mission axes throughout (z = height); `terrain_clear(a, b)` and
// `entities_clear(a, b)` are the embedder's two lines of sight between two
// mission points. The embedder checks for water first (no water = no glint)
// and owns the model writes.
struct WaterGlintFrame {
	bool visible = false;   // the accumulator saw the glint this frame
	Vec3 point_m;           // the sampled water point (jittered once its terrain rays pass)
	Vec3 mirrored_sun;      // the sun with its height term negated
	Vec3 position;          // the glint model's place: camera + mirrored_sun * 128
	int32_t upl = 0;        // the UPL_INTENSITY submit value, 16.16
	// The glint submits whenever its brightness accumulator is non-zero,
	// its alpha 0 included [orig: Environment_UpdateSunGlare @ 0x5ad35c..0x5ad36b ->
	// the submit @ 0x5ad470].
	bool drawn = false;
};

template <typename TerrainClear, typename EntitiesClear>
inline WaterGlintFrame advance_water_glint(const EnvironmentState &state,
		const Vec3 &cam_m, const Vec3 &sun_m, const Vec3 &forward_m,
		WaterGlintState &glint, TerrainClear &&terrain_clear,
		EntitiesClear &&entities_clear) {
	WaterGlintFrame out;
	// This call's frame index, stepped before any read (water_glint_tick
	// stores it).
	const uint32_t frame = glint.frame_index + 1u;
	const float view_z_jitter = 0.25f * static_cast<float>(frame & 3u);
	bool visible = water_glint_point(cam_m, sun_m, state.water_height(),
			view_z_jitter, out.point_m);
	if (visible) {
		const Vec3 sun_far{cam_m.x + sun_m.x * 2048.0f, cam_m.y + sun_m.y * 2048.0f,
				cam_m.z + sun_m.z * 2048.0f};
		visible = terrain_clear(out.point_m, sun_far) && terrain_clear(out.point_m, cam_m);
	}
	if (visible) {
		// The +-2 unit point jitter [orig: @ 0x5ad26a..0x5ad27e] — mission x
		// and mission z (height).
		out.point_m.x += (frame & 1u) ? 2.0f : -2.0f;
		out.point_m.z += (frame & 2u) ? 2.0f : -2.0f;
		visible = entities_clear(cam_m, out.point_m);
	}
	water_glint_tick(glint, visible);
	out.visible = visible;
	// Placement: camera + sun * 128 with the HEIGHT term negated (the mirrored
	// glint below the eye [orig: @ 0x5ad1ba..0x5ad213 — the float matrix stores
	// (-(camY + sunY*128), camZ - sunZ*128, camX + sunX*128), the mission ->
	// render-float map of exactly that mirrored point]).
	out.mirrored_sun = Vec3{sun_m.x, sun_m.y, -sun_m.z};
	// The offset is scaled, then added, as two steps (the embedder's Vector3
	// operators did the same).
	const Vec3 offset{out.mirrored_sun.x * 128.0f, out.mirrored_sun.y * 128.0f,
			out.mirrored_sun.z * 128.0f};
	out.position = Vec3{cam_m.x + offset.x, cam_m.y + offset.y, cam_m.z + offset.z};
	// Alpha: the view dot of the MIRRORED sun direction [orig: @ 0x5ad384
	// negates the height term before the view transform] through the witnessed
	// (dot^4 - 28672/65536) x brightness chain. Summed in the embedder's
	// (x, height, y) term order so the float rounding is the one it always had.
	const float view_dot = forward_m.x * out.mirrored_sun.x +
			forward_m.z * out.mirrored_sun.z + forward_m.y * out.mirrored_sun.y;
	const int dot_fixed = static_cast<int>(view_dot * 65536.0f);
	out.upl = water_glint_alpha_fixed(dot_fixed, glint.brightness,
			io::float_to_fp16_16(state.sun_dim_pct()));
	out.drawn = glint.brightness != 0;
	return out;
}

// The frame's sun veil: the screen's white quad alpha and the exposure
// stop-down [orig: Environment_ApplySunVeilAndExposureStopdown @ 0x5ad8b0, once
// per main scene frame after the occlusion tick, from Render_ProcessMainSceneFrame
// @ 0x5cac4b]:
// - nothing without the sun model (`sun_model`, the gate @ 0x5ad8b3..0x5ad8ba);
// - the sky term, Environment_ComputeSunGlareAndFogBlend(0) (the call @ 0x5ad8cd):
//   the view dot of the sun at the occlusion brightness (sun_veil_from_dot);
// - when the water plane is set (@ 0x5ad8d5..0x5ad8dc), plus the water-reflected
//   term, Environment_ComputeSunGlareAndFogBlend(1) (the call @ 0x5ad8ea), both
//   sums clamped 192 (sun_veil_combine). Its direction is the camera's to the
//   UNJITTERED glint point [orig: Environment_ComputeSunGlareAndFogBlend @ 0x5ad610
//   — Water_ComputeReflectedSunPoint at view z 0 (the call @ 0x5ad628), the
//   camera's offset to it normalized @ 0x5ad62d..0x5ad698] at the water glint's
//   brightness >> 2 [@ 0x5ad6a7].
// Mission axes (z = height). The two dots and the normalization sum their terms
// in the embedder's (x, height, y) order, each square a step of its own, so the
// float rounding is the one it always had; retail computes the secondary's
// direction in 16.16 (the float port is the standing one).
inline SunVeil sun_veil_frame(const EnvironmentState &state, bool sun_model,
		const Vec3 &cam_m, const Vec3 &sun_m, const Vec3 &forward_m,
		int occlusion_brightness, int glint_brightness) {
	if (!sun_model) return SunVeil{};
	const int sun_dim = io::float_to_fp16_16(state.sun_dim_pct());
	const int overcast = io::float_to_fp16_16(state.overcast_blend());
	const float view_dot = forward_m.x * sun_m.x + forward_m.z * sun_m.z + forward_m.y * sun_m.y;
	SunVeil veil = sun_veil_from_dot(io::float_to_fp16_16(view_dot), occlusion_brightness,
			sun_dim, overcast);
	if (!state.has_water_height() || state.water_height() == 0.0f) return veil;
	Vec3 point_m;
	// A rejected reflection (camera at or below the plane) adds no secondary
	// term; retail's reads the reflected point's unwritten stack there (env #X3,
	// class D) [orig: Environment_ComputeSunGlareAndFogBlend @0x5AD622..0x5AD632,
	// Water_ComputeReflectedSunPoint's reject @0x5AC094].
	if (!water_glint_point(cam_m, sun_m, state.water_height(), 0.0f, point_m)) return veil;
	const Vec3 offset{point_m.x - cam_m.x, point_m.y - cam_m.y, point_m.z - cam_m.z};
	const float x2 = offset.x * offset.x;
	const float height2 = offset.z * offset.z;
	const float y2 = offset.y * offset.y;
	const float length_squared = x2 + height2 + y2;
	Vec3 to_glint{0.0f, 0.0f, 0.0f};
	if (length_squared != 0.0f) {
		const float length = std::sqrt(length_squared);
		to_glint = Vec3{offset.x / length, offset.y / length, offset.z / length};
	}
	const float glint_dot =
			forward_m.x * to_glint.x + forward_m.z * to_glint.z + forward_m.y * to_glint.y;
	return sun_veil_combine(veil, sun_veil_from_dot(io::float_to_fp16_16(glint_dot),
			glint_brightness >> 2, sun_dim, overcast));
}

// The CTRL register every celestial submit writes (UPL_INTENSITY).
inline constexpr int kCelestialUplRegister = threedi::THREEDI_CTRL_UPL_INTENSITY;

inline Vec3 celestial_body_position(const Vec3 &cam_pos, const Vec3 &dir) {
	return Vec3{cam_pos.x + dir.x * kCelestialBodyDistance,
			cam_pos.y + dir.y * kCelestialBodyDistance,
			cam_pos.z + dir.z * kCelestialBodyDistance};
}

// The sun and moon discs: placed at camera + direction * 64 (full camera
// height, identity rotation), each submitted with its own alpha in the
// UPL_INTENSITY register [orig: Render_CelestialBodies @ 0x5acaa0: the sun
// alpha @ 0x5acbfa and its submit @ 0x5acc1c, the moon alpha @ 0x5accc1 and
// its submit @ 0x5accdd, one CRenderBatchQueue_SortAndFlush @ 0x5acce9].
// Sharing the flush does not share the value: the submit snapshots the
// registers each strip's material lists into its batch entry
// [orig: Render_CollectRenderObjectsForBatch @ 0x5d91c0..0x5d91de] and the
// flush writes them back into the register table before that batch's
// RgbGen reads it [orig: CRenderBatchQueue_FlushBatches @ 0x5da1d6..0x5da1fd;
// RgbGen_EvaluateColor @ 0x5b2453]. Overcast and SunDim are live end-to-end
// (env #27; spring-smoothed in the weather core, target 0 in stock data).
struct CelestialDiscsFrame {
	Vec3 sun_position{};
	Vec3 moon_position{};
	// Each disc's register value in the beauty (and mirror) pass, 16.16.
	int32_t sun_upl = 0;
	int32_t moon_upl = 0;
	// The same for the bloom pass's redraw Render_CelestialBodies(1), the
	// fog-shader path [orig: FrameFX_RenderGlowSource @ 0x582a77]: the moon
	// alpha leg is fogDistInt x 0.0002 x (1 - overcast) instead of the
	// (fogDistInt - 400) / 600 ramp [orig: Render_CelestialBodies
	// @ 0x5acc37..0x5acc61]; the sun has no fog-shader variant.
	int32_t sun_q3_upl = 0;
	int32_t moon_q3_upl = 0;
};

inline CelestialDiscsFrame build_celestial_discs_frame(const EnvironmentState &env,
		const Vec3 &cam_pos) {
	CelestialDiscsFrame frame;
	frame.sun_position = celestial_body_position(cam_pos, env.sun_direction());
	frame.moon_position = celestial_body_position(cam_pos, env.moon_direction());
	const int overcast = io::float_to_fp16_16(env.overcast_blend());
	frame.sun_upl = celestial_sun_alpha_fixed(overcast,
			io::float_to_fp16_16(env.sun_dim_pct()));
	frame.sun_q3_upl = frame.sun_upl;
	frame.moon_upl = celestial_moon_alpha_fixed(env.fog_level(), overcast, false);
	frame.moon_q3_upl = celestial_moon_alpha_fixed(env.fog_level(), overcast, true);
	return frame;
}

// The sun glow: the glare model at camera + sun * 64, flushed alone right
// after its submit [orig: Render_SkyboxSunGlow @ 0x5acd00, flush
// @ 0x5ad118]. The beauty draw takes the occlusion brightness, the bloom
// pass's redraw Render_SkyboxSunGlow(0, 0) the fog-based one; both fold
// dot^4/2 of the MAIN camera's view dot [orig: @ 0x5acfa0..0x5acff7]. A
// non-positive alpha submits nothing [orig: @ 0x5ad0ae]. Each submit poses
// the model's parts at the register value it has just written: the PANM
// runs inside the submit's collect [orig: Render_SubmitEntity @ 0x5dad80 ->
// Render_CollectRenderObjectsForBatch @ 0x5d8f3b -> Model_TransformBoneMatrices
// @ 0x58e390 -> PANM_SampleTrack @ 0x5b2270, style 113], after the register
// store [orig: @ 0x5ad09d / @ 0x5ad0a9]. The stock glare's part scales
// follow UPL_INTENSITY (mglare.3di: part 0 5.0..6.0, part 1 0.5..2.5), so the
// bloom redraw's parts take q3_upl and the beauty's take upl.
struct GlareFrame {
	Vec3 position{};
	int32_t upl = 0;
	bool drawn = false;
	int32_t q3_upl = 0;
	bool q3_drawn = false;
};

inline GlareFrame build_glare_frame(const EnvironmentState &env,
		const Vec3 &cam_pos, int view_dot_fixed, int occlusion_brightness) {
	GlareFrame frame;
	frame.position = celestial_body_position(cam_pos, env.sun_direction());
	const int overcast = io::float_to_fp16_16(env.overcast_blend());
	const int sun_dim = io::float_to_fp16_16(env.sun_dim_pct());
	// The locked reimpl profile is FBEFFECTS 3, so both draws carry the
	// witnessed quarter [orig: FrameFX_QualityAtLeast3 @ 0x581f60; >>= 2
	// @ 0x5ad033..0x5ad03c].
	frame.upl = glare_glow_alpha_fixed(view_dot_fixed, occlusion_brightness,
			overcast, sun_dim, true);
	frame.drawn = frame.upl > 0;
	frame.q3_upl = glare_q3_alpha_fixed(view_dot_fixed, env.fog_level(),
			overcast, sun_dim, true);
	frame.q3_drawn = frame.q3_upl > 0;
	return frame;
}

// The water mirror's post-dim glow, Render_SkyboxSunGlow(0, 0) from
// Render_MainScene [orig: Render_MainScene @ 0x5c1904]: no occlusion test,
// so the fog-based brightness of the bloom pass's call at the MIRROR view's
// dot, the same quarter and fold; a non-positive value submits nothing
// [orig: Render_SkyboxSunGlow @ 0x5ad0b0].
inline int32_t mirror_glare_upl(const EnvironmentState &env, int mirror_view_dot_fixed) {
	return glare_q3_alpha_fixed(mirror_view_dot_fixed, env.fog_level(),
			io::float_to_fp16_16(env.overcast_blend()),
			io::float_to_fp16_16(env.sun_dim_pct()), true);
}

} // namespace opennova::env
