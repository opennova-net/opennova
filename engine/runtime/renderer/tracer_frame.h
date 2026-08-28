#pragma once

// Portable tracer-ribbon compilation — the styled half of the tracer/trail
// pool split (the SIM owns the point rings in world/tracer_trails.h; this
// module owns the witnessed style tables and the camera-facing ribbon build).
// No Godot or simulator dependencies: channels arrive as raw point runs, the
// result is interleaved vertex data an embedding renderer uploads verbatim.
//
// [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0 — per point a +/- right pair,
// right = normalize(cross(segment dir, camera - point)), half-width =
// max(style size curve x point jitter, distance x min-screen-width), vertex
// color from the style ARGB ramp indexed by (count - i) + age - 1 (the same
// ramp is the along-trail gradient AND the post-death fade), the oldest pair
// takes the style base color, triangle strip, untextured vertex color (the
// B=0 path writes no UVs). Styles: the 0x830-B blocks — static .data
// g_TracerStyle_Std*/Rapid*/Sniper* @ 0x8437F0..0x8460E0, runtime-built
// DF1/rocket/at4/grenade/NVG in CEffectEmitterPool_ResetAndBuildStyles
// @ 0x5DB3A0. Additive styles draw fog-to-black additive (SetFogAndBlendMode
// mode 2 — ONE:ONE, alpha unused); smoke styles (rocket/at4/grenade) draw
// alpha-blended with scene fog (mode 0).]

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::renderer {

// One style row: the ARGB color ramp (+0x18; head index 0 = the ramp start),
// the float half-width curve (+0x41C, count +0x418), the base color (+0x14,
// the oldest vertex pair), and the additive flag (+0 — fog-black additive vs
// alpha smoke).
struct TracerStyleView {
	const std::uint32_t *colors = nullptr;
	int color_count = 0;
	const float *sizes = nullptr;
	int size_count = 0;
	std::uint32_t base_argb = 0;
	bool additive = false;
};

// Style ids 1..12 are the ammo.def `tracer_type` ids (stdred 1, stdgreen 2,
// rocket 3, at4 4, grenade 5, rapidred 6, rapidgreen 7, 8 = the NVG laser,
// sniperred 9, snipergreen 10, df1red 11, df1green 12); any other id takes
// the stdred block [orig: the CEffectChannel_Init default case @ 0x5db1c8].
const TracerStyleView &tracer_style(int style_id);

// Min half-width per unit of camera distance — the witnessed screen-size clamp
// [orig: |camera - point| (16.16) x 1.83e-8 / proj scale
// @ CEffectChannel_RenderRibbon; ~0.0012/u at the shipped projection — the
// proj divisor is D-AI-12g].
inline constexpr float kTracerMinWidthPerDist = 0.0012f;

struct TracerVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// One live trail channel: `points` is count x 4 floats {x, y, z, w} where w is
// the per-point width multiplier stamped at append time (world/tracer_trails.h).
struct TracerChannelInput {
	int style_id = 0;
	int age = 0;
	int count = 0;
	const float *points = nullptr;
};

// One blend family's run: interleaved {x, y, z, r, g, b, a} per vertex
// (stride 7, components 0..1). Channels of a family join into one triangle
// strip via degenerate pairs. Additive vertices force a = 1 (retail renders
// ONE:ONE with the alpha byte unused — the std ramps author alpha 0); smoke
// vertices carry their authored alpha fade.
struct TracerRibbonFrame {
	std::vector<float> additive;
	std::vector<float> alpha;
	int channels = 0;

	void clear() {
		additive.clear();
		alpha.clear();
		channels = 0;
	}
};

// The witnessed ribbon build over points [0 .. count-2] (the newest point
// steers direction only — the visible head is the second-newest point, one
// tick behind the pre-move append). Channels with fewer than two points emit
// nothing but still count toward `channels`.
void compile_tracer_ribbons(const TracerChannelInput *channels, std::size_t count,
		const TracerVec3 &camera, TracerRibbonFrame &out);

}  // namespace opennova::renderer