#include <runtime/renderer/frame_fx_effects.h>

#include <algorithm>
#include <cmath>

namespace opennova::renderer {

namespace {

// The DrawPass constants the rows share.
// [orig: FrameFX_CaptureBackBufferAndSubmitDecal @0x583f71 / @0x583f82
//  (flt_7D83BC / flt_7D68F4, the capture downsample); sub_5841D0 @0x58424b
//  (flt_7D83A4, every 256-square pass's base) / @0x58425b (flt_7C6950, the
//  1/256 blur radius unit)]
constexpr float kDownsampleBase = 1.0f / 2048.0f;
constexpr float kDownsampleRadius = 1.0f / 1024.0f;
constexpr int kDownsampleAngle = 30;
constexpr float kWorkBase = 1.0f / 512.0f;
constexpr float kWorkTexel = 1.0f / 256.0f;

// The capture downsample into 256A: LumaAverage, four taps at 30 degrees,
// the 256-square rect. [orig: FrameFX_CaptureBackBufferAndSubmitDecal
// @0x583f64..0x583fcb; FrameFX_CaptureRenderTarget @0x584165..0x5841c0]
FrameFxPass capture_downsample() {
	FrameFxPass pass;
	pass.source = FrameFxBuffer::Capture;
	pass.target = FrameFxBuffer::WorkA;
	pass.stage = FrameFxStage::LumaAverage;
	pass.taps = FrameFxTaps::Rotated;
	pass.base = kDownsampleBase;
	pass.radius = kDownsampleRadius;
	pass.angle_degrees = kDownsampleAngle;
	return pass;
}

FrameFxStep pass_step(const FrameFxPass &pass) {
	FrameFxStep step;
	step.kind = FrameFxStepKind::Pass;
	step.pass = pass;
	return step;
}

FrameFxStep capture_step() {
	FrameFxStep step;
	step.kind = FrameFxStepKind::CaptureFrame;
	return step;
}

FrameFxStep distortion_step(FrameFxDistortionSet set, FrameFxBuffer screen) {
	FrameFxStep step;
	step.kind = FrameFxStepKind::Distortion;
	step.set = set;
	step.screen_texture = screen;
	return step;
}

// The backbuffer capture plus its 256A downsample: FrameFX_CaptureBackBuffer-
// AndSubmitDecal(1) [orig: Render_DispatchShadowByType @0x584487].
void append_capture_and_downsample(std::vector<FrameFxStep> &steps) {
	steps.push_back(capture_step());
	steps.push_back(pass_step(capture_downsample()));
}

// Type 0, the distortion pass: the jittered 256A -> 256B pass, then the two
// distortion sets with slot 2 bound. The jitter is (GetTickCount() >> 1) +
// (rand() & 15) degrees.
// [orig: render_projected_shadow @0x583748..0x5837ea (the pass), @0x5838f3
//  (slot 2 = [fx+4]) / @0x583922 (slot 2 = [fx+8])]
void append_distortion(std::vector<FrameFxStep> &steps, std::uint32_t clock_ms,
		const FrameFxRand &rand) {
	append_capture_and_downsample(steps);
	FrameFxPass jitter;
	jitter.source = FrameFxBuffer::WorkA;
	jitter.target = FrameFxBuffer::WorkB;
	jitter.stage = FrameFxStage::LumaAverage;
	jitter.taps = FrameFxTaps::Rotated;
	jitter.reduced_u = true;
	jitter.base = kWorkBase;
	jitter.radius = 0.0027617188f; // flt_7D83B8 [orig: render_projected_shadow @0x583764]
	const std::int32_t tick_half = static_cast<std::int32_t>(clock_ms >> 1);
	jitter.angle_degrees = tick_half + static_cast<std::int32_t>(rand() & 15u);
	jitter.constant_alpha = 0.99f; // flt_7C6A00 @0x583792 (c0.a, unread by the stage)
	steps.push_back(pass_step(jitter));
	steps.push_back(distortion_step(FrameFxDistortionSet::Particles, FrameFxBuffer::WorkA));
	steps.push_back(distortion_step(FrameFxDistortionSet::TracerRibbons, FrameFxBuffer::WorkB));
}

// Type 1, the damage blur at p = red / 120: from p = 0.5 the frame is
// replaced by 256A blurred at radius min(p - 0.5, 2) / 256 through 60-degree
// taps; below it 256A cross-fades over the frame at alpha 2p.
// [orig: Scar_SubmitShadowDecal @0x5830f0 (p >= 0.5 @0x58312d..0x5831c0;
//  p < 0.5 @0x5831ce..0x58323e); p = word x flt_7DC1A4 @0x5caa5c..0x5caa62]
void append_damage_blur(std::vector<FrameFxStep> &steps, std::int32_t red_word) {
	append_capture_and_downsample(steps);
	const float p = static_cast<float>(red_word) * 0.008333334f; // flt_7DC1A4
	FrameFxPass pass;
	pass.source = FrameFxBuffer::WorkA;
	pass.target = FrameFxBuffer::Frame;
	pass.taps = FrameFxTaps::Rotated;
	pass.viewport_rect = true;
	pass.reduced_u = true;
	pass.base = kWorkBase;
	if (p >= 0.5f) {
		const float over = std::min(p - 0.5f, 2.0f); // flt_7C3B90 @0x58312f
		pass.stage = FrameFxStage::LumaAverage;
		pass.radius = over * kWorkTexel;
		pass.angle_degrees = 60; // @0x5831ab
		pass.constant_alpha = 0.99f; // flt_7C6A00 @0x583186 (unread by the stage)
	} else {
		pass.stage = FrameFxStage::AverageBlend;
		pass.radius = 0.0f;
		pass.constant_alpha = p + p; // fadd st, st @0x583211
	}
	steps.push_back(pass_step(pass));
}

// Type 4, the death blur at d = clamp(tick - stamp - 30, 0, 200) / 15:
// radial-fan passes that keep the rect centre and blur toward its border,
// one pass below d = 2, two (256A -> 256B at d/4) below 8, three (d/16 into
// 256B, d/4 back into 256A) from 8, the last blending into the frame.
// [orig: Scar_SubmitCascadeShadowPasses @0x5833a0 (d < 2 @0x5833df..0x583421,
//  2 <= d < 8 @0x58345f..0x5834a3 then @0x583548..0x583584, d >= 8
//  @0x5834a8..0x583584); d @0x5ca9fb..0x5caa34 (flt_7DC1A8)]
void append_death_blur(std::vector<FrameFxStep> &steps, std::int32_t elapsed_ticks) {
	append_capture_and_downsample(steps);
	std::int32_t ticks = elapsed_ticks - 30;
	if (ticks <= 0)
		ticks = 0;
	if (ticks >= 200)
		ticks = 200;
	const float d = static_cast<float>(ticks) * 0.06666667f; // flt_7DC1A8
	FrameFxPass fan;
	fan.taps = FrameFxTaps::RadialFan;
	fan.reduced_u = true;
	fan.base = kWorkBase;
	auto intermediate = [&](FrameFxBuffer source, FrameFxBuffer target, float scale) {
		FrameFxPass pass = fan;
		pass.source = source;
		pass.target = target;
		pass.stage = FrameFxStage::LumaAverage;
		pass.radius = d * scale * kWorkTexel;
		steps.push_back(pass_step(pass));
	};
	FrameFxBuffer last = FrameFxBuffer::WorkA;
	if (d >= 2.0f) {
		if (d >= 8.0f) {
			intermediate(FrameFxBuffer::WorkA, FrameFxBuffer::WorkB, 0.0625f); // flt_7C486C
			intermediate(FrameFxBuffer::WorkB, FrameFxBuffer::WorkA, 0.25f);   // flt_7C333C
			last = FrameFxBuffer::WorkA;
		} else {
			intermediate(FrameFxBuffer::WorkA, FrameFxBuffer::WorkB, 0.25f);
			last = FrameFxBuffer::WorkB;
		}
	}
	FrameFxPass final_pass = fan;
	final_pass.source = last;
	final_pass.target = FrameFxBuffer::Frame;
	final_pass.stage = FrameFxStage::FanAverage;
	final_pass.viewport_rect = true;
	final_pass.radius = d * kWorkTexel;
	steps.push_back(pass_step(final_pass));
}

// The Tiled scanline pass over the frame: "ffscan" at base 1/128, two rand()
// draws per pass (the first gives the V offset, the second the U offset).
// [orig: sub_583A60 @0x583a60; render_scar_decal_batch @0x582cc2..0x582cd8]
FrameFxPass scanline_pass(const FrameFxRand &rand) {
	FrameFxPass pass;
	pass.source = FrameFxBuffer::Scanlines;
	pass.target = FrameFxBuffer::Frame;
	pass.stage = FrameFxStage::Scanline;
	pass.taps = FrameFxTaps::Tiled;
	pass.viewport_rect = true;
	pass.base = 0.0078125f; // flt_7C3DD4 @0x583a76
	pass.vertex_color = kFrameFxScanlineDiffuse; // @0x583ac7
	pass.tile_offset_y = static_cast<std::int32_t>(rand() & 0x3Eu);
	pass.tile_offset_x = static_cast<std::int32_t>(rand() & 0xFFu);
	return pass;
}

// Type 8, the thermal view: the backbuffer capture without its downsample,
// the inverted-luma green stage over the frame, then the scanlines.
// [orig: sub_5845B0 @0x5845f5 (FrameFX_CaptureBackBufferAndSubmitDecal(0));
//  sub_584390 @0x584390 (@0x5843b0..0x58441a, then sub_583A60 @0x584433)]
void append_thermal(std::vector<FrameFxStep> &steps, const FrameFxRand &rand) {
	steps.push_back(capture_step());
	FrameFxPass pass;
	pass.source = FrameFxBuffer::Capture;
	pass.target = FrameFxBuffer::Frame;
	pass.stage = FrameFxStage::Thermal;
	pass.taps = FrameFxTaps::Rotated;
	pass.viewport_rect = true;
	pass.base_from_capture = true;
	pass.radius = kDownsampleRadius; // flt_7D68F4 @0x584410
	pass.angle_degrees = kDownsampleAngle; // @0x5843f8
	steps.push_back(pass_step(pass));
	steps.push_back(pass_step(scanline_pass(rand)));
}

} // namespace

int frame_fx_capture_side(int backbuffer_side) {
	unsigned value = static_cast<unsigned>(backbuffer_side - 1);
	unsigned last = value;
	while (value != 0) {
		last = value;
		value &= value - 1;
	}
	return static_cast<int>(last);
}

FrameFxFramePlan plan_frame_fx(const FrameFxFrameInputs &in, FrameFxPlannerState &state,
		const FrameFxRand &rand) {
	FrameFxFramePlan plan;
	const FrameFxViewInputs &view = in.view;
	// The NVG scene runs on g_NVGActive under the death screen, else only in
	// camera mode 0; the composite (and the jump past every FrameFX dispatch)
	// needs camera mode 0 either way. [orig: @0x5ca516..0x5ca554, @0x5ca6ab..0x5ca6bf]
	plan.nvg.scene = view.nvg_active &&
			(view.death_screen_active || view.camera_mode == 0);
	plan.nvg.composite = view.nvg_active && view.camera_mode == 0;
	plan.nvg.clear_glow = plan.nvg.scene && view.nvg_active != state.nvg_active_last;
	state.nvg_active_last = view.nvg_active; // dword_29D6BA4 @0x5ca5cd
	if (plan.nvg.composite)
		return plan;

	// Render_DispatchShadowByType returns at once below FBEFFECTS 1.
	// [orig: @0x584440..0x58444a]
	const bool dispatcher = in.frame_effects_level > 0;
	const bool red_flash = view.red_word != 0 && view.camera_mode != 3;
	// Type 0: not while dead in a session, not under the red flash outside
	// camera mode 3, FBEFFECTS 2 and something distorting.
	// [orig: @0x5ca8f6..0x5ca92e; the dispatcher's gate @0x584463..0x584481]
	if (dispatcher && !(view.in_session && view.local_dead) && !red_flash &&
			in.frame_effects_level >= 2 && in.distortion_present)
		append_distortion(plan.before_bloom, in.clock_ms, rand);
	// Type 4 while dead, else type 1 under the red flash. [orig: @0x5ca9f5..0x5caa71]
	if (dispatcher) {
		if (view.local_dead)
			append_death_blur(plan.before_bloom, view.death_elapsed_ticks);
		else if (red_flash)
			append_damage_blur(plan.before_bloom, view.red_word);
	}
	// Type 2 at FBEFFECTS 3. [orig: FrameFX_QualityAtLeast3 @0x5caa7b; @0x5844c4..0x5844eb]
	plan.bloom = dispatcher && in.frame_effects_level >= 3;
	// Types 8 and 9 on the frame's CanFire latches; sub_5845B0 has no level
	// gate. [orig: @0x5caa9c..0x5caad5]
	if (view.thermal_view)
		append_thermal(plan.after_bloom, rand);
	if (view.monitor_view)
		plan.after_bloom.push_back(pass_step(scanline_pass(rand)));
	return plan;
}

std::vector<FrameFxPass> frame_fx_bloom_passes() {
	std::vector<FrameFxPass> passes;
	passes.push_back(capture_downsample());
	// The two weighted pairs: 90 + 270 degrees from 256A into 256B with the
	// U taps reduced, then 0 + 180 degrees back into 256A.
	// [orig: sub_5841D0 @0x584248..0x5842a3, @0x5842a8..0x5842f8]
	FrameFxPass pair;
	pair.stage = FrameFxStage::WeightedAdd;
	pair.taps = FrameFxTaps::Weighted;
	pair.base = kWorkBase;
	pair.radius = kWorkTexel;
	pair.angle_step_degrees = 180;
	pair.count = 2;
	FrameFxPass first = pair;
	first.source = FrameFxBuffer::WorkA;
	first.target = FrameFxBuffer::WorkB;
	first.angle_degrees = 90;
	first.reduced_u = true;
	passes.push_back(first);
	FrameFxPass second = pair;
	second.source = FrameFxBuffer::WorkB;
	second.target = FrameFxBuffer::WorkA;
	second.angle_degrees = 0;
	passes.push_back(second);
	// The composite: 256A added over the frame at c0.a = 0.5 through 45-degree
	// taps. [orig: sub_5841D0 @0x5842fd..0x58437a (flt_7D844C, flt_7C3B94)]
	FrameFxPass composite;
	composite.source = FrameFxBuffer::WorkA;
	composite.target = FrameFxBuffer::Frame;
	composite.stage = FrameFxStage::AverageAdd;
	composite.taps = FrameFxTaps::Rotated;
	composite.viewport_rect = true;
	composite.base = kWorkBase;
	composite.radius = 0.0027621093f;
	composite.angle_degrees = 45;
	composite.constant_alpha = 0.5f;
	passes.push_back(composite);
	return passes;
}

FrameFxRgb frame_fx_thermal_color(const FrameFxRgb &average) {
	const float luma = (1.0f - average[0]) * kFrameFxThermalLuma[0] +
			(1.0f - average[1]) * kFrameFxThermalLuma[1] +
			(1.0f - average[2]) * kFrameFxThermalLuma[2];
	// add_sat r1 = sat(L + (0, 1, 0)); mad r0 = L r1 + (0, 0.05, 0); the
	// target saturates the write.
	const float red_blue = std::clamp(luma, 0.0f, 1.0f);
	const float green = std::clamp(luma + 1.0f, 0.0f, 1.0f);
	auto saturate = [](float value) { return std::clamp(value, 0.0f, 1.0f); };
	return {saturate(luma * red_blue), saturate(luma * green + kFrameFxThermalGreenBias),
			saturate(luma * red_blue)};
}

float frame_fx_fan_alpha(float s, float t) {
	return 2.0f * std::max(std::fabs(s - 0.5f), std::fabs(t - 0.5f));
}

std::array<float, 2> frame_fx_fan_tap(float s, float t, int k, float radius, float base,
		bool reduced_u) {
	const float m = frame_fx_fan_alpha(s, t);
	const float u_scale = reduced_u ? kFrameFxReducedU : 1.0f;
	const float step = kFrameFxFanTapSteps[static_cast<std::size_t>(k)] * radius;
	return {s + m * base + step * 2.0f * u_scale * (0.5f - s),
			t + m * base + step * 2.0f * (0.5f - t)};
}

std::vector<std::uint8_t> frame_fx_scanline_texels(const FrameFxRand &rand) {
	std::vector<std::uint8_t> texels(
			static_cast<std::size_t>(kFrameFxScanlineSide * kFrameFxScanlineSide));
	std::size_t index = 0;
	for (int row = 0; row < kFrameFxScanlineSide; ++row) {
		for (int column = 0; column < kFrameFxScanlineSide; ++column) {
			int value = 0x60;
			if ((row & 1) != 0)
				value = 0x80 + ((static_cast<int>(rand()) >> 2) % 24);
			texels[index++] = static_cast<std::uint8_t>(value);
		}
	}
	return texels;
}

FrameFxRgb nvg_tint_color(const FrameFxRgb &scene) {
	const float d = scene[0] * kNvgTintDot[0] + scene[1] * kNvgTintDot[1] +
			scene[2] * kNvgTintDot[2];
	FrameFxRgb out;
	for (std::size_t c = 0; c < 3; ++c)
		out[c] = std::clamp(d * kNvgTintScale[c] + kNvgTintBias[c], 0.0f, 1.0f);
	return out;
}

FrameFxRgb nvg_glow_source(const std::array<FrameFxRgb, 4> &taps) {
	FrameFxRgb sum = {0.0f, 0.0f, 0.0f};
	for (const FrameFxRgb &tap : taps) {
		for (std::size_t c = 0; c < 3; ++c)
			sum[c] += tap[c] * tap[c];
	}
	const float luma = sum[0] * kNvgGlowLuma[0] + sum[1] * kNvgGlowLuma[1] +
			sum[2] * kNvgGlowLuma[2];
	const float g = std::clamp(luma - kNvgGlowThreshold, 0.0f, 1.0f);
	return {g * kNvgGlowScale[0], g * kNvgGlowScale[1], g * kNvgGlowScale[2]};
}

} // namespace opennova::renderer
