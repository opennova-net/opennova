// FrameFX's screen-effect planner and the CPU references of its pixel stages
// (runtime/renderer/frame_fx_effects.h), pinned against the retail witnesses.

#include <runtime/renderer/frame_fx_effects.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

using namespace opennova::renderer;

bool near(float a, float b, float tolerance = 1.0e-5f) {
	return std::fabs(a - b) <= tolerance;
}

// A scripted rand() stream that records how many draws the planner made.
struct ScriptedRand {
	std::vector<std::uint16_t> values;
	std::size_t next = 0;
	FrameFxRand fn() {
		return [this]() -> std::uint16_t {
			const std::uint16_t value = next < values.size() ? values[next] : 0;
			++next;
			return value;
		};
	}
};

const FrameFxPass *nth_pass(const std::vector<FrameFxStep> &steps, std::size_t n) {
	std::size_t seen = 0;
	for (const FrameFxStep &step : steps) {
		if (step.kind != FrameFxStepKind::Pass)
			continue;
		if (seen++ == n)
			return &step.pass;
	}
	return nullptr;
}

std::size_t count_kind(const std::vector<FrameFxStep> &steps, FrameFxStepKind kind) {
	std::size_t count = 0;
	for (const FrameFxStep &step : steps)
		count += step.kind == kind ? 1 : 0;
	return count;
}

// [orig: FrameFX_CreateRenderTargets @0x583c7f..0x583c97]
void capture_side_is_the_power_of_two_floor_of_one_less() {
	CHECK(frame_fx_capture_side(1024) == 512);
	CHECK(frame_fx_capture_side(1025) == 1024);
	CHECK(frame_fx_capture_side(1280) == 1024);
	CHECK(frame_fx_capture_side(1920) == 1024);
	CHECK(frame_fx_capture_side(1080) == 1024);
	CHECK(frame_fx_capture_side(768) == 512);
	CHECK(frame_fx_capture_side(900) == 512);
	CHECK(frame_fx_capture_side(2000) == 1024);
	CHECK(frame_fx_capture_side(1200) == 1024);
	CHECK(frame_fx_capture_side(257) == 256);
	CHECK(frame_fx_capture_side(256) == 128);
}

// The default frame at FBEFFECTS 3 runs the bloom alone.
void quiet_frame_plans_only_the_bloom() {
	FrameFxPlannerState state;
	ScriptedRand rand;
	const FrameFxFramePlan plan = plan_frame_fx(FrameFxFrameInputs{}, state, rand.fn());
	CHECK(plan.bloom);
	CHECK(plan.before_bloom.empty());
	CHECK(plan.after_bloom.empty());
	CHECK(!plan.nvg.scene && !plan.nvg.composite);
	CHECK(rand.next == 0);
	FrameFxFrameInputs low;
	low.frame_effects_level = 2;
	CHECK(!plan_frame_fx(low, state, rand.fn()).bloom);
}

// [orig: FrameFX_BloomKernel @0x584248..0x58437a; FrameFX_CaptureAltBuffer @0x584165..0x5841c0]
void bloom_kernel_reduces_u_on_the_first_weighted_pair() {
	const std::vector<FrameFxPass> passes = frame_fx_bloom_passes();
	CHECK(passes.size() == 4);
	const FrameFxPass &down = passes[0];
	CHECK(down.source == FrameFxBuffer::Capture && down.target == FrameFxBuffer::WorkA);
	CHECK(down.stage == FrameFxStage::LumaAverage && down.angle_degrees == 30);
	CHECK(near(down.base, 1.0f / 2048.0f) && near(down.radius, 1.0f / 1024.0f));
	CHECK(!down.reduced_u && !down.viewport_rect);
	const FrameFxPass &first = passes[1];
	CHECK(first.source == FrameFxBuffer::WorkA && first.target == FrameFxBuffer::WorkB);
	CHECK(first.stage == FrameFxStage::WeightedAdd && first.taps == FrameFxTaps::Weighted);
	CHECK(first.angle_degrees == 90 && first.angle_step_degrees == 180 && first.count == 2);
	CHECK(first.reduced_u);
	const FrameFxPass &second = passes[2];
	CHECK(second.source == FrameFxBuffer::WorkB && second.target == FrameFxBuffer::WorkA);
	CHECK(second.angle_degrees == 0 && second.angle_step_degrees == 180 && second.count == 2);
	CHECK(!second.reduced_u);
	const FrameFxPass &composite = passes[3];
	CHECK(composite.target == FrameFxBuffer::Frame && composite.viewport_rect);
	CHECK(composite.stage == FrameFxStage::AverageAdd && composite.angle_degrees == 45);
	CHECK(near(composite.radius, 0.0027621093f) && near(composite.constant_alpha, 0.5f));
}

// Type 1 [orig: FrameFX_DamageBlur @0x5830f0; the dispatch @0x5caa41..0x5caa71]
void damage_blur_replaces_then_cross_fades() {
	FrameFxPlannerState state;
	ScriptedRand rand;
	FrameFxFrameInputs in;
	in.view.red_word = 120; // one hit: p = 1
	FrameFxFramePlan plan = plan_frame_fx(in, state, rand.fn());
	CHECK(count_kind(plan.before_bloom, FrameFxStepKind::CaptureFrame) == 1);
	const FrameFxPass *blur = nth_pass(plan.before_bloom, 1);
	CHECK(blur != nullptr);
	if (blur != nullptr) {
		CHECK(blur->source == FrameFxBuffer::WorkA && blur->target == FrameFxBuffer::Frame);
		CHECK(blur->stage == FrameFxStage::LumaAverage);
		CHECK(blur->angle_degrees == 60 && blur->reduced_u && blur->viewport_rect);
		CHECK(near(blur->radius, 0.5f / 256.0f, 1.0e-6f));
		CHECK(near(blur->base, 1.0f / 512.0f));
	}
	in.view.red_word = 40; // p = 1/3: a 2/3 cross-fade
	plan = plan_frame_fx(in, state, rand.fn());
	blur = nth_pass(plan.before_bloom, 1);
	CHECK(blur != nullptr);
	if (blur != nullptr) {
		CHECK(blur->stage == FrameFxStage::AverageBlend);
		CHECK(blur->radius == 0.0f && blur->angle_degrees == 0);
		CHECK(near(blur->constant_alpha, 2.0f / 3.0f, 1.0e-5f));
	}
	in.view.red_word = 255; // p - 0.5 = 1.625 stays under the 2.0 clamp
	blur = nth_pass(plan_frame_fx(in, state, rand.fn()).before_bloom, 1);
	CHECK(blur != nullptr && near(blur->radius, 1.625f / 256.0f, 1.0e-5f));
	in.view.camera_mode = 3; // the free camera never blurs
	CHECK(plan_frame_fx(in, state, rand.fn()).before_bloom.empty());
	CHECK(rand.next == 0);
}

// Type 4 [orig: FrameFX_DeathBlur @0x5833a0; d @0x5ca9fb..0x5caa34]
void death_blur_grows_through_one_two_and_three_fan_passes() {
	FrameFxPlannerState state;
	ScriptedRand rand;
	FrameFxFrameInputs in;
	in.view.local_dead = true;
	in.view.red_word = 120; // the death blur wins over the damage blur
	in.view.death_elapsed_ticks = 30; // d = 0
	FrameFxFramePlan plan = plan_frame_fx(in, state, rand.fn());
	const FrameFxPass *last = nth_pass(plan.before_bloom, 1);
	CHECK(last != nullptr && last->stage == FrameFxStage::FanAverage);
	CHECK(last != nullptr && last->radius == 0.0f && last->taps == FrameFxTaps::RadialFan);
	CHECK(last != nullptr && last->source == FrameFxBuffer::WorkA);
	CHECK(nth_pass(plan.before_bloom, 2) == nullptr);

	in.view.death_elapsed_ticks = 30 + 60; // d = 4: two passes
	plan = plan_frame_fx(in, state, rand.fn());
	const FrameFxPass *mid = nth_pass(plan.before_bloom, 1);
	last = nth_pass(plan.before_bloom, 2);
	CHECK(mid != nullptr && mid->stage == FrameFxStage::LumaAverage);
	CHECK(mid != nullptr && mid->target == FrameFxBuffer::WorkB && !mid->viewport_rect);
	CHECK(mid != nullptr && near(mid->radius, 4.0f * 0.25f / 256.0f, 1.0e-5f));
	CHECK(last != nullptr && last->source == FrameFxBuffer::WorkB);
	CHECK(last != nullptr && near(last->radius, 4.0f / 256.0f, 1.0e-5f));

	in.view.death_elapsed_ticks = 30 + 500; // clamps at 200 ticks: d = 13.33
	plan = plan_frame_fx(in, state, rand.fn());
	const float d = 200.0f * 0.06666667f;
	const FrameFxPass *a = nth_pass(plan.before_bloom, 1);
	const FrameFxPass *b = nth_pass(plan.before_bloom, 2);
	last = nth_pass(plan.before_bloom, 3);
	CHECK(a != nullptr && a->target == FrameFxBuffer::WorkB &&
			near(a->radius, d * 0.0625f / 256.0f, 1.0e-5f));
	CHECK(b != nullptr && b->source == FrameFxBuffer::WorkB && b->target == FrameFxBuffer::WorkA &&
			near(b->radius, d * 0.25f / 256.0f, 1.0e-5f));
	CHECK(last != nullptr && last->source == FrameFxBuffer::WorkA &&
			last->target == FrameFxBuffer::Frame && near(last->radius, d / 256.0f, 1.0e-5f));
	CHECK(last != nullptr && last->reduced_u && last->viewport_rect);

	in.view.death_elapsed_ticks = 10; // before the 30-tick hold: d = 0
	last = nth_pass(plan_frame_fx(in, state, rand.fn()).before_bloom, 1);
	CHECK(last != nullptr && last->radius == 0.0f);
}

// Type 0 [orig: @0x5ca8f6..0x5ca92e; FrameFX_DistortionPass @0x583720]
void distortion_row_jitters_and_binds_both_work_targets() {
	FrameFxPlannerState state;
	ScriptedRand rand;
	rand.values = {0x1234};
	FrameFxFrameInputs in;
	in.distortion_present = true;
	in.clock_ms = 1000;
	FrameFxFramePlan plan = plan_frame_fx(in, state, rand.fn());
	CHECK(rand.next == 1);
	const std::vector<FrameFxStep> &steps = plan.before_bloom;
	CHECK(steps.size() == 5);
	if (steps.size() == 5) {
		CHECK(steps[0].kind == FrameFxStepKind::CaptureFrame);
		CHECK(steps[2].kind == FrameFxStepKind::Pass);
		CHECK(steps[2].pass.source == FrameFxBuffer::WorkA &&
				steps[2].pass.target == FrameFxBuffer::WorkB);
		CHECK(steps[2].pass.angle_degrees == 500 + (0x1234 & 15));
		CHECK(steps[2].pass.reduced_u && near(steps[2].pass.radius, 0.0027617188f));
		CHECK(steps[3].kind == FrameFxStepKind::Distortion &&
				steps[3].set == FrameFxDistortionSet::Particles &&
				steps[3].screen_texture == FrameFxBuffer::WorkA);
		CHECK(steps[4].kind == FrameFxStepKind::Distortion &&
				steps[4].set == FrameFxDistortionSet::TracerRibbons &&
				steps[4].screen_texture == FrameFxBuffer::WorkB);
	}
	// Nothing distorting, FBEFFECTS 1, dead in a session, or the red flash:
	// no row and no draw.
	rand.next = 0;
	in.distortion_present = false;
	CHECK(plan_frame_fx(in, state, rand.fn()).before_bloom.empty());
	in.distortion_present = true;
	in.frame_effects_level = 1;
	CHECK(plan_frame_fx(in, state, rand.fn()).before_bloom.empty());
	in.frame_effects_level = kLockedFrameEffectsLevel;
	in.view.in_session = true;
	in.view.local_dead = true;
	CHECK(count_kind(plan_frame_fx(in, state, rand.fn()).before_bloom,
			FrameFxStepKind::Distortion) == 0);
	in.view.local_dead = false;
	in.view.red_word = 10;
	CHECK(count_kind(plan_frame_fx(in, state, rand.fn()).before_bloom,
			FrameFxStepKind::Distortion) == 0);
	in.view.camera_mode = 3; // the red flash does not gate the free camera
	CHECK(count_kind(plan_frame_fx(in, state, rand.fn()).before_bloom,
			FrameFxStepKind::Distortion) == 2);
}

// Types 8 / 9 [orig: @0x5caa9c..0x5caad5; FrameFX_ThermalView @0x584390; FrameFX_ScanlineOverlay @0x583a60]
void thermal_and_monitor_follow_the_bloom() {
	FrameFxPlannerState state;
	ScriptedRand rand;
	rand.values = {0x7FFF, 0x0101, 0x00C2, 0x0033};
	FrameFxFrameInputs in;
	in.view.thermal_view = true;
	in.view.monitor_view = true;
	FrameFxFramePlan plan = plan_frame_fx(in, state, rand.fn());
	CHECK(plan.bloom);
	const std::vector<FrameFxStep> &after = plan.after_bloom;
	CHECK(after.size() == 4);
	if (after.size() == 4) {
		CHECK(after[0].kind == FrameFxStepKind::CaptureFrame);
		CHECK(after[1].pass.stage == FrameFxStage::Thermal);
		CHECK(after[1].pass.source == FrameFxBuffer::Capture && after[1].pass.base_from_capture);
		CHECK(after[1].pass.angle_degrees == 30 && near(after[1].pass.radius, 1.0f / 1024.0f));
		CHECK(after[2].pass.stage == FrameFxStage::Scanline &&
				after[2].pass.source == FrameFxBuffer::Scanlines);
		// The first draw is the V offset (& 0x3E), the second the U (& 0xFF).
		CHECK(after[2].pass.tile_offset_y == (0x7FFF & 0x3E));
		CHECK(after[2].pass.tile_offset_x == (0x0101 & 0xFF));
		CHECK(near(after[2].pass.base, 1.0f / 128.0f));
		CHECK(after[2].pass.vertex_color == 0x808080u);
		CHECK(after[3].pass.stage == FrameFxStage::Scanline);
		CHECK(after[3].pass.tile_offset_y == (0x00C2 & 0x3E));
		CHECK(after[3].pass.tile_offset_x == (0x0033 & 0xFF));
	}
	CHECK(rand.next == 4);
	// The latches ignore FBEFFECTS: FrameFX_ApplyWeaponViewEffect has no level gate.
	in.frame_effects_level = 0;
	CHECK(plan_frame_fx(in, state, rand.fn()).after_bloom.size() == 4);
}

// The NVG view [orig: @0x5ca516..0x5ca5cd; @0x5ca6ab..0x5ca73a]
void nvg_replaces_the_chain_and_clears_its_glow_on_the_toggle_frame() {
	FrameFxPlannerState state;
	ScriptedRand rand;
	FrameFxFrameInputs in;
	in.view.nvg_active = true;
	in.view.red_word = 120;
	in.view.thermal_view = true;
	in.distortion_present = true;
	FrameFxFramePlan plan = plan_frame_fx(in, state, rand.fn());
	CHECK(plan.nvg.scene && plan.nvg.composite && plan.nvg.clear_glow);
	CHECK(!plan.bloom && plan.before_bloom.empty() && plan.after_bloom.empty());
	CHECK(rand.next == 0);
	plan = plan_frame_fx(in, state, rand.fn());
	CHECK(plan.nvg.composite && !plan.nvg.clear_glow);
	// Outside first person the chain runs and no NVG scene renders; the latch
	// still follows g_NVGActive.
	in.view.camera_mode = 1;
	plan = plan_frame_fx(in, state, rand.fn());
	CHECK(!plan.nvg.scene && !plan.nvg.composite && plan.bloom);
	// Under the death screen the NVG scene still renders.
	in.view.death_screen_active = true;
	plan = plan_frame_fx(in, state, rand.fn());
	CHECK(plan.nvg.scene && !plan.nvg.composite && !plan.nvg.clear_glow);
	// Toggling off then on in first person clears again.
	in.view.death_screen_active = false;
	in.view.camera_mode = 0;
	in.view.nvg_active = false;
	CHECK(!plan_frame_fx(in, state, rand.fn()).nvg.scene);
	in.view.nvg_active = true;
	CHECK(plan_frame_fx(in, state, rand.fn()).nvg.clear_glow);
}

// The Scoped arm's lens and the NVG.tga mask gate [orig: @0x5ca549..0x5ca575;
// @0x5ca6f5..0x5ca71a; NVG_Composite @0x5d1077..0x5d1080]
void nvg_scoped_arm_draws_the_lens_and_drops_the_mask() {
	FrameFxViewInputs view;
	view.nvg_active = true;
	view.scoped_selector = true;
	FrameFxNvgPlan nvg = frame_fx_nvg_view(view);
	CHECK(nvg.scene && nvg.composite && nvg.lens);
	CHECK(!frame_fx_nvg_mask_visible(view));
	// The planner carries the arm with its latch.
	FrameFxPlannerState state;
	ScriptedRand rand;
	FrameFxFrameInputs in;
	in.view = view;
	const FrameFxFramePlan plan = plan_frame_fx(in, state, rand.fn());
	CHECK(plan.nvg.lens && plan.nvg.clear_glow && !plan.bloom);
	// The binocular arm comes first: the full-screen composite and its mask.
	view.binoculars_view_active = true;
	nvg = frame_fx_nvg_view(view);
	CHECK(nvg.composite && !nvg.lens);
	CHECK(frame_fx_nvg_mask_visible(view));
	view.binoculars_view_active = false;
	// The death screen composites without the lens and without the mask.
	view.death_screen_active = true;
	nvg = frame_fx_nvg_view(view);
	CHECK(nvg.composite && !nvg.lens);
	CHECK(!frame_fx_nvg_mask_visible(view));
	view.death_screen_active = false;
	// Unscoped first person: the composite and the mask.
	view.scoped_selector = false;
	CHECK(!frame_fx_nvg_view(view).lens && frame_fx_nvg_mask_visible(view));
	// Out of first person there is no composite, so neither.
	view.scoped_selector = true;
	view.camera_mode = 1;
	CHECK(!frame_fx_nvg_view(view).lens && !frame_fx_nvg_mask_visible(view));
	// NVG off: nothing.
	view.camera_mode = 0;
	view.nvg_active = false;
	nvg = frame_fx_nvg_view(view);
	CHECK(!nvg.scene && !nvg.composite && !nvg.lens && !frame_fx_nvg_mask_visible(view));
}

// The Sighted arm: after the Scoped byte, the card goes into the scene and the
// full-screen composite (with its mask) draws. [orig: @0x5ca57f..0x5ca591;
// @0x5ca724..0x5ca72b]
void nvg_sighted_arm_takes_the_card_into_the_scene() {
	FrameFxViewInputs view;
	view.nvg_active = true;
	view.sighted_selector = true;
	FrameFxNvgPlan nvg = frame_fx_nvg_view(view);
	CHECK(nvg.composite && nvg.sighted && !nvg.lens);
	CHECK(frame_fx_nvg_mask_visible(view));
	// A def with both bytes takes the Scoped arm.
	view.scoped_selector = true;
	nvg = frame_fx_nvg_view(view);
	CHECK(nvg.lens && !nvg.sighted);
	view.scoped_selector = false;
	view.binoculars_view_active = true;
	CHECK(!frame_fx_nvg_view(view).sighted);
	view.binoculars_view_active = false;
	view.death_screen_active = true;
	CHECK(!frame_fx_nvg_view(view).sighted);
	view.death_screen_active = false;
	view.camera_mode = 1;
	CHECK(!frame_fx_nvg_view(view).sighted);
}

// [orig: ps @0x7D7C48]
void thermal_stage_inverts_the_luma_into_green() {
	const FrameFxRgb grey = frame_fx_thermal_color({0.5f, 0.5f, 0.5f});
	CHECK(near(grey[0], 0.25f) && near(grey[1], 0.55f) && near(grey[2], 0.25f));
	const FrameFxRgb black = frame_fx_thermal_color({0.0f, 0.0f, 0.0f});
	CHECK(near(black[0], 1.0f) && near(black[1], 1.0f) && near(black[2], 1.0f));
	const FrameFxRgb white = frame_fx_thermal_color({1.0f, 1.0f, 1.0f});
	CHECK(near(white[0], 0.0f) && near(white[1], 0.05f) && near(white[2], 0.0f));
}

// [orig: FrameFX_BuildRadialTapFan @0x581d20]
void fan_keeps_the_centre_and_blurs_toward_the_border() {
	CHECK(near(frame_fx_fan_alpha(0.5f, 0.5f), 0.0f));
	CHECK(near(frame_fx_fan_alpha(0.0f, 0.5f), 1.0f));
	CHECK(near(frame_fx_fan_alpha(0.75f, 0.9f), 0.8f));
	const auto centre = frame_fx_fan_tap(0.5f, 0.5f, 3, 0.05f, 1.0f / 512.0f, true);
	CHECK(near(centre[0], 0.5f) && near(centre[1], 0.5f));
	// The top-left corner: (base + 0.75 r k, base + r k), the corner vertex's taps.
	const float r = 0.04f;
	const float base = 1.0f / 512.0f;
	const auto corner = frame_fx_fan_tap(0.0f, 0.0f, 2, r, base, true);
	CHECK(near(corner[0], base + 0.75f * r * 2.5f) && near(corner[1], base + r * 2.5f));
	const auto right = frame_fx_fan_tap(1.0f, 0.5f, 0, r, base, false);
	CHECK(near(right[0], 1.0f + base - r * 0.5f) && near(right[1], 0.5f + base));
}

// [orig: CFrameFX_CreatePixelShaders @0x58289f..0x582912]
void scanline_texture_alternates_a_dim_row_and_a_noisy_row() {
	ScriptedRand rand;
	for (int i = 0; i < 2048; ++i)
		rand.values.push_back(static_cast<std::uint16_t>((i * 97) & 0x7FFF));
	const std::vector<std::uint8_t> texels = frame_fx_scanline_texels(rand.fn());
	CHECK(texels.size() == 64u * 64u);
	CHECK(rand.next == 2048);
	CHECK(texels[0] == 0x60 && texels[63] == 0x60);
	CHECK(texels[64] == 0x80 + ((0 >> 2) % 24));
	CHECK(texels[65] == 0x80 + (((97) >> 2) % 24));
	CHECK(texels[2 * 64] == 0x60);
	bool bright_rows_in_range = true;
	for (int row = 1; row < 64; row += 2)
		for (int column = 0; column < 64; ++column) {
			const int value = texels[static_cast<std::size_t>(row * 64 + column)];
			bright_rows_in_range = bright_rows_in_range && value >= 0x80 && value <= 0x97;
		}
	CHECK(bright_rows_in_range);
}

// [orig: ps @0x7DC3D8; ps @0x7DC228]
void nvg_tint_and_glow_follow_their_pixel_shaders() {
	const FrameFxRgb tint = nvg_tint_color({0.3f, 0.3f, 0.3f});
	CHECK(near(tint[0], 0.12f) && near(tint[1], 0.74f) && near(tint[2], 0.12f));
	// Green saturates from a 44% grey (1.8 L + 0.2 = 1).
	CHECK(near(nvg_tint_color({0.45f, 0.45f, 0.45f})[1], 1.0f));
	const FrameFxRgb glow = nvg_glow_source({{{0.3f, 0.3f, 0.3f}, {0.3f, 0.3f, 0.3f},
			{0.3f, 0.3f, 0.3f}, {0.3f, 0.3f, 0.3f}}});
	CHECK(near(glow[0], 0.042f) && near(glow[1], 0.126f) && near(glow[2], 0.042f));
	const FrameFxRgb dark = nvg_glow_source({{{0.1f, 0.1f, 0.1f}, {0.1f, 0.1f, 0.1f},
			{0.1f, 0.1f, 0.1f}, {0.1f, 0.1f, 0.1f}}});
	CHECK(dark[0] == 0.0f && dark[1] == 0.0f && dark[2] == 0.0f);
}

} // namespace

int main() {
	capture_side_is_the_power_of_two_floor_of_one_less();
	quiet_frame_plans_only_the_bloom();
	bloom_kernel_reduces_u_on_the_first_weighted_pair();
	damage_blur_replaces_then_cross_fades();
	death_blur_grows_through_one_two_and_three_fan_passes();
	distortion_row_jitters_and_binds_both_work_targets();
	thermal_and_monitor_follow_the_bloom();
	nvg_replaces_the_chain_and_clears_its_glow_on_the_toggle_frame();
	nvg_scoped_arm_draws_the_lens_and_drops_the_mask();
	nvg_sighted_arm_takes_the_card_into_the_scene();
	thermal_stage_inverts_the_luma_into_green();
	fan_keeps_the_centre_and_blurs_toward_the_border();
	scanline_texture_alternates_a_dim_row_and_a_noisy_row();
	nvg_tint_and_glow_follow_their_pixel_shaders();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("frame_fx_effects: ok\n");
	return 0;
}
