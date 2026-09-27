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
// - [orig: Render_SkyboxSunGlow @ 0x5acd00] drives the glare: two jittered
//   terrain rays per frame into an 8-sample window + hysteresis brightness,
//   glow alpha = dot_view^4/2 x brightness x folds (env #14, closed).
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

namespace opennova::env {

// One call of the water-glint leg [orig: Environment_UpdateSunGlare @ 0x5ad130, once
// per scene pass from Terrain_RenderWorldScene @ 0x5c96c0: the main scene's,
// then the weapon Inset pass's at its own camera and view, on the same
// WaterGlintState (runtime/renderer/scene_overlay.h kInsetOverlayOrder)]:
// one sample per call — the reflected-sun point on the water (with the
// 0.25 * (frame & 3) reflected-height jitter and the +-2 point x/z jitter),
// visible when the point sees BOTH the sun (point -> camera + sun * 2048) and
// the camera over terrain, then the +-16 chase toward popcount * 64 and the
// mirrored glare submit at camera + sun * 128 with the height term negated.
// Mission axes throughout (z = height); `segment_clear(a, b)` is the
// embedder's terrain line-of-sight between two mission points. The embedder
// checks for water first (no water = no glint) and owns the model writes.
struct WaterGlintFrame {
	bool visible = false;   // the accumulator saw the glint this frame
	Vec3 point_m;           // the sampled (jittered) water point
	Vec3 mirrored_sun;      // the sun with its height term negated
	int32_t upl = 0;        // the UPL_INTENSITY submit value, 16.16
	// The glint submits whenever its brightness accumulator is non-zero,
	// its alpha 0 included [orig: Environment_UpdateSunGlare @ 0x5ad35c..0x5ad36b ->
	// the submit @ 0x5ad470].
	bool drawn = false;
};

template <typename SegmentClear>
inline WaterGlintFrame advance_water_glint(const EnvironmentState &state,
		const Vec3 &cam_m, const Vec3 &sun_m, const Vec3 &forward_m,
		WaterGlintState &glint, SegmentClear &&segment_clear) {
	WaterGlintFrame out;
	const float view_z_jitter = 0.25f * static_cast<float>(glint.frame_index & 3u);
	bool visible = water_glint_point(cam_m, sun_m, state.water_height(),
			view_z_jitter, out.point_m);
	if (visible) {
		// The +-2 unit point jitter [orig: @ 0x5ad26a..0x5ad27e] — mission x
		// and mission z (height).
		out.point_m.x += (glint.frame_index & 1u) ? 2.0f : -2.0f;
		out.point_m.z += (glint.frame_index & 2u) ? 2.0f : -2.0f;
		const Vec3 sun_far{cam_m.x + sun_m.x * 2048.0f, cam_m.y + sun_m.y * 2048.0f,
				cam_m.z + sun_m.z * 2048.0f};
		visible = segment_clear(out.point_m, sun_far) && segment_clear(out.point_m, cam_m);
	}
	water_glint_tick(glint, visible);
	out.visible = visible;
	// Placement: camera + sun * 128 with the HEIGHT term negated (the mirrored
	// glint below the eye [orig: @ 0x5ad1ba..0x5ad213 — the float matrix stores
	// (-(camY + sunY*128), camZ - sunZ*128, camX + sunX*128), the mission ->
	// render-float map of exactly that mirrored point]).
	out.mirrored_sun = Vec3{sun_m.x, sun_m.y, -sun_m.z};
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
